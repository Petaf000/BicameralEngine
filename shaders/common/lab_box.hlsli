// lab_box.hlsli — 実験室の箱(14 §2「実験室」・T-0142・ADR-0037)のコマンドの形と、コマンドをセルに当てる関数。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(engine/src/sim/lab_box.cpp)と
// GPU(shaders/sim/lab_box.hlsl)が同じ関数を呼ぶので、当てた結果はビット単位で同じになる。
//
// データの流れ:
//   エディタの実験室のパネル(CPU の Controller)→ sim::Command(64 バイト。type = LAB_COMMAND_TYPE_*)
//   → GPU: コマンドの列をバッファへ写し、刻みの前に ApplyLabCommands が箱のセルに当てる → 多重解像度の刻み(反応 + 伝導)
//   → CPU: 同じ列を同じ順で LabApplyCommand に通す → StepNest(CPU リファレンス)。刻みごとに両方を比べる。
// 箱は多重解像度の木の本物の根 1 つ(8³ セル、レベル 0 = 0.5 m 角。いつも頁を持つ)。コマンドが変えるのはセル 1 つか、
// 範囲のコマンド(T-0220)なら直方体のセル全部(同じコマンドを x → y → z の順に 1 セルずつ当てる)。
// セルを置き換える・温度を決めるのは、エディタの明示的な湧き出し(保存則の外。つつきと同じ扱い。D-206)。
#ifndef BICAMERAL_LAB_BOX_HLSLI
#define BICAMERAL_LAB_BOX_HLSLI

#include "multires.hlsli"

#ifdef __cplusplus
#define LAB_NAMESPACE_BEGIN                    \
    namespace bicameral::lab {                 \
        using namespace ::bicameral::fx;       \
        using namespace ::bicameral::reaction; \
        using namespace ::bicameral::multires;
#define LAB_NAMESPACE_END }
#else
#define LAB_NAMESPACE_BEGIN
#define LAB_NAMESPACE_END
#endif

LAB_NAMESPACE_BEGIN

// --- コマンドの形(sim/command.h の 64 バイトを uint32 × 16 で読む)-----------------------------
// [0..1] targetTick・[2] sequence・[3] type(下位 16bit)| size(上位 16bit)・[4..15] payload
FX_CONST uint32_t LAB_COMMAND_WORDS = 16;
FX_CONST uint32_t LAB_COMMAND_TYPE_WORD = 3;
FX_CONST uint32_t LAB_PAYLOAD_BEGIN = 4;

// 種類(仮の世界の PROBE_COMMAND_TYPE_* と重ならない番号)
FX_CONST uint32_t LAB_COMMAND_TYPE_FILL = 0x4C01;         // セルの中身を物質(3 つまで)と温度で置き換える
FX_CONST uint32_t LAB_COMMAND_TYPE_TEMPERATURE = 0x4C02;  // セルの成分はそのまま、温度を決める(熱を足す・引く)
// 反応表を替えた印(T-0218・ADR-0055)。セルは変えない。CPU はこの刻みのコマンドを当てる前に表を替え、印は箱を「つつく」
// (待ちの予定を新しい表で求め直す。ADR-0018 の「表を変えるものは『変わった』にしてから評価する」)。payload は表の版
FX_CONST uint32_t LAB_COMMAND_TYPE_TABLE = 0x4C03;
// 範囲のコマンド(T-0220): FILL・TEMPERATURE と同じ中身を直方体のセル全部に当てる。payload の LAB_PAYLOAD_CELL の語が範囲
FX_CONST uint32_t LAB_COMMAND_TYPE_FILL_REGION = 0x4C04;
FX_CONST uint32_t LAB_COMMAND_TYPE_TEMPERATURE_REGION = 0x4C05;

// payload の語(FILL と TEMPERATURE で共通の先頭 2 語)
FX_CONST uint32_t LAB_PAYLOAD_CELL = 0;         // セルの番号(MrCellIndex(x, y, z))
FX_CONST uint32_t LAB_PAYLOAD_TEMPERATURE = 1;  // 温度(mK)
FX_CONST uint32_t LAB_PAYLOAD_COUNT = 2;        // FILL だけ: 物質の数(0〜LAB_MAX_FILL_SPECIES)
FX_CONST uint32_t LAB_PAYLOAD_ENTRIES = 3;      // FILL だけ: [物質 ID][量の下位][量の上位](µmol。箱のレベルの単位)× 数
FX_CONST uint32_t LAB_PAYLOAD_TABLE_VERSION = 0;  // TABLE だけ: 表の版の下位・上位(2 語)
FX_CONST uint32_t LAB_MAX_FILL_SPECIES = 3;
FX_CONST uint32_t LAB_NO_CELL = 0xFFFFFFFFu;

// 範囲の語: x, y, z を 5 ビットずつ。下位 15 ビット = 小さい角、その上の 15 ビット = 大きい角(両端を含む)
FX_CONST uint32_t LAB_REGION_COORD_BITS = 5;
FX_CONST uint32_t LAB_REGION_COORD_MASK = 0x1Fu;
FX_CONST uint32_t LAB_REGION_CORNER_BITS = 15;

// 温度の上限(反応の表の温度の範囲に合わせる。これより上のコマンドは当てない)
FX_CONST uint32_t LAB_MAX_TEMPERATURE_MILLIKELVIN = 6000000;

struct LabCommand {
    uint32_t words[LAB_COMMAND_WORDS];
};

// 直方体の範囲(両端を含む)
struct LabRegion {
    uint32_t lowX;
    uint32_t lowY;
    uint32_t lowZ;
    uint32_t highX;
    uint32_t highY;
    uint32_t highZ;
};

FX_FN uint32_t LabCommandType(LabCommand command) {
    return command.words[LAB_COMMAND_TYPE_WORD] & 0xFFFFu;
}

FX_FN uint32_t LabPayload(LabCommand command, uint32_t word) {
    return command.words[LAB_PAYLOAD_BEGIN + word];
}

// 表を替えた印か(セルには当たらないが、箱をつつく)
FX_FN bool LabCommandMarksTable(LabCommand command) {
    return LabCommandType(command) == LAB_COMMAND_TYPE_TABLE;
}

// 温度を決めるだけの種類か(1 セル・範囲)
FX_FN bool LabSetsTemperatureOnly(LabCommand command) {
    const uint32_t type = LabCommandType(command);
    return type == LAB_COMMAND_TYPE_TEMPERATURE || type == LAB_COMMAND_TYPE_TEMPERATURE_REGION;
}

// 温度と中身(FILL なら物質の並び)が当てられるか。speciesCount = 表の物質の数(ID 0 は使わない)
FX_FN bool LabContentsValid(LabCommand command, uint32_t speciesCount) {
    if (LabPayload(command, LAB_PAYLOAD_TEMPERATURE) > LAB_MAX_TEMPERATURE_MILLIKELVIN)
        return false;

    if (LabSetsTemperatureOnly(command))
        return true;

    // --- FILL: 物質は表の中・重ならない・量は 0 でない(並びは問わない。RxAddSpecies が ID の順に入れる)---
    const uint32_t count = LabPayload(command, LAB_PAYLOAD_COUNT);
    if (count > LAB_MAX_FILL_SPECIES)
        return false;

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t species = LabPayload(command, LAB_PAYLOAD_ENTRIES + (3 * i));
        const uint64_t amount = FX_U64(LabPayload(command, LAB_PAYLOAD_ENTRIES + (3 * i) + 2),
                                       LabPayload(command, LAB_PAYLOAD_ENTRIES + (3 * i) + 1));
        if (species == 0 || species >= speciesCount || amount == 0)
            return false;

        for (uint32_t j = 0; j < i; ++j) {
            if (LabPayload(command, LAB_PAYLOAD_ENTRIES + (3 * j)) == species)
                return false;
        }
    }

    return true;
}

// コマンドが当たるセルの番号。当てられないコマンド(1 セルの種類でない・箱の外・温度が範囲外・物質の誤り)なら LAB_NO_CELL
FX_FN uint32_t LabCommandCell(LabCommand command, uint32_t speciesCount) {
    const uint32_t type = LabCommandType(command);
    if (type != LAB_COMMAND_TYPE_FILL && type != LAB_COMMAND_TYPE_TEMPERATURE)
        return LAB_NO_CELL;

    const uint32_t cell = LabPayload(command, LAB_PAYLOAD_CELL);
    if (cell >= MR_BLOCK_CELLS || !LabContentsValid(command, speciesCount))
        return LAB_NO_CELL;

    return cell;
}

FX_FN LabRegion LabCommandRegion(LabCommand command) {
    const uint32_t word = LabPayload(command, LAB_PAYLOAD_CELL);
    const uint32_t high = word >> LAB_REGION_CORNER_BITS;
    LabRegion region;
    region.lowX = word & LAB_REGION_COORD_MASK;
    region.lowY = (word >> LAB_REGION_COORD_BITS) & LAB_REGION_COORD_MASK;
    region.lowZ = (word >> (2 * LAB_REGION_COORD_BITS)) & LAB_REGION_COORD_MASK;
    region.highX = high & LAB_REGION_COORD_MASK;
    region.highY = (high >> LAB_REGION_COORD_BITS) & LAB_REGION_COORD_MASK;
    region.highZ = (high >> (2 * LAB_REGION_COORD_BITS)) & LAB_REGION_COORD_MASK;

    return region;
}

// 範囲のコマンドで、当てられるか(範囲が箱の中で小さい角 ≤ 大きい角・温度と中身が当てられる)
FX_FN bool LabRegionCommandValid(LabCommand command, uint32_t speciesCount) {
    const uint32_t type = LabCommandType(command);
    if (type != LAB_COMMAND_TYPE_FILL_REGION && type != LAB_COMMAND_TYPE_TEMPERATURE_REGION)
        return false;

    const LabRegion region = LabCommandRegion(command);
    const bool inside = region.highX < MR_BLOCK_EDGE && region.highY < MR_BLOCK_EDGE && region.highZ < MR_BLOCK_EDGE;
    const bool ordered = region.lowX <= region.highX && region.lowY <= region.highY && region.lowZ <= region.highZ;

    return inside && ordered && LabContentsValid(command, speciesCount);
}

// --- セルを作る --------------------------------------------------------------------------------

// 成分(cell の energy は見ない)と温度(mK)から、エネルギー = 化学 + 熱容量 × 温度 を mJ に切り上げたセル
// (sim::MakeReactionCell と同じ値。lab_box_test が確かめる)
template <typename Table>
FX_FN RxCell LabSetTemperature(Table table, RxCell cell, uint32_t temperatureMilliKelvin) {
    const int64_t chemical = RxChemicalEnergy(table, cell);
    const uint64_t heatCapacity = RxHeatCapacity(table, cell);
    const FxU128 product = FxMulU64Full(heatCapacity, (uint64_t)temperatureMilliKelvin);
    const int64_t heat = (int64_t)FxDivU128By64(product, RX_MILLIKELVIN_NUMERATOR).quotient;

    // --- µJ → mJ。切り上げ(熱が負にならないように)。負なら 0 方向に切る = 切り上げ ---
    const int64_t total = chemical + heat;
    const uint64_t magnitude = total >= 0 ? (uint64_t)total + (RX_MICROJOULES_PER_MILLIJOULE - 1) : (uint64_t)(-total);
    const int64_t millijoules = (int64_t)FxDivU128By64(FxMulU64Full(magnitude, 1), RX_MICROJOULES_PER_MILLIJOULE)
                                    .quotient;
    cell.energy = total >= 0 ? millijoules : -millijoules;

    return cell;
}

// コマンドをセルに当てる(LabCommandCell が LAB_NO_CELL でないか、LabRegionCommandValid のコマンドだけ。範囲なら範囲のセルごとに)
template <typename Table>
FX_FN RxCell LabApplyCommand(Table table, RxCell cell, LabCommand command) {
    const uint32_t temperature = LabPayload(command, LAB_PAYLOAD_TEMPERATURE);
    if (LabSetsTemperatureOnly(command))
        return LabSetTemperature(table, cell, temperature);

    RxCell filled = RxMakeEmptyCell(0);
    const uint32_t count = LabPayload(command, LAB_PAYLOAD_COUNT);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t species = LabPayload(command, LAB_PAYLOAD_ENTRIES + (3 * i));
        const uint64_t amount = FX_U64(LabPayload(command, LAB_PAYLOAD_ENTRIES + (3 * i) + 2),
                                       LabPayload(command, LAB_PAYLOAD_ENTRIES + (3 * i) + 1));
        filled = RxAddSpecies(filled, species, amount);
    }

    return LabSetTemperature(table, filled, temperature);
}

LAB_NAMESPACE_END

#endif  // BICAMERAL_LAB_BOX_HLSLI
