// lab_box.hlsli — 実験室の箱(14 §2「実験室」・T-0142・ADR-0037)のコマンドの形と、コマンドをセルに当てる関数。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(engine/src/sim/lab_box.cpp)と
// GPU(shaders/sim/lab_box.hlsl)が同じ関数を呼ぶので、当てた結果はビット単位で同じになる。
//
// データの流れ:
//   エディタの実験室のパネル(CPU の Controller)→ sim::Command(64 バイト。type = LAB_COMMAND_TYPE_*)
//   → GPU: コマンドの列をバッファへ写し、刻みの前に ApplyLabCommands が箱のセルに当てる → 多重解像度の刻み(反応 + 伝導)
//   → CPU: 同じ列を同じ順で LabApplyCommand に通す → StepNest(CPU リファレンス)。刻みごとに両方を比べる。
// 箱は多重解像度の木の本物の根(8³ セル、レベル 0 = 0.5 m 角。いつも頁を持つ)を x・y・z に並べたもの(T-0173。初めは根 1 つ)。
// コマンドが変えるのはセル 1 つか、範囲のコマンド(T-0220)なら直方体のセル全部(同じコマンドを x → y → z の順に 1 セルずつ当てる)。
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
// 魔法のパッチを足す(T-0024・ADR-0095)。payload は common/magic_patch.hlsli の MP_COMMAND_*(範囲はレベル 0 = 箱のセルの座標)。
// 始まりはコマンドの刻み。セルは変えず、足せて箱に重なれば箱をつつく(速さが変わる。ADR-0018)
FX_CONST uint32_t LAB_COMMAND_TYPE_PATCH = 0x4C06;

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

// --- 箱の形(T-0173): 根(8³ のブロック)を x・y・z に並べた数 ------------------------------------
// 根の枠は x → y → z の順に 0 から(sim::MakeLabBoxNest が空きのスタックから順に取る)。根の原点 = 根の座標 × 8(レベル 0 のセル)。
// 箱のセルの番号: 下位 9 ビット = 根の中のセル(MrCellIndex)、その上に根の x・y・z を 2 ビットずつ。
// 根 1 つの箱ではセルの番号 = MrCellIndex のまま(T-0142 からの記録・コマンドはそのまま読める)。
FX_CONST uint32_t LAB_MAX_ROOTS_PER_AXIS = 4;  // 範囲の語の座標が 5 ビット(32 セル = 根 4 つ)
FX_CONST uint32_t LAB_MAX_ROOTS = 64;
FX_CONST uint32_t LAB_MAX_BOX_EDGE = 32;  // MR_BLOCK_EDGE × LAB_MAX_ROOTS_PER_AXIS
FX_CONST uint32_t LAB_ROOT_COORD_BITS = 2;
FX_CONST uint32_t LAB_ROOT_COORD_MASK = 3u;
FX_CONST uint32_t LAB_CELL_ROOT_SHIFT = 9;          // MR_BLOCK_CELLS = 2^9
FX_CONST uint32_t LAB_CELL_INDEX_LIMIT = 1u << 15;  // 9 + 2 × 3 ビット
FX_CONST uint32_t LAB_SHAPE_FIELD_BITS = 8;         // LabPackShape の 1 軸のビット数

struct LabBoxShape {
    uint32_t rootsX;
    uint32_t rootsY;
    uint32_t rootsZ;
};

FX_FN LabBoxShape LabMakeShape(uint32_t rootsX, uint32_t rootsY, uint32_t rootsZ) {
    LabBoxShape shape;
    shape.rootsX = rootsX;
    shape.rootsY = rootsY;
    shape.rootsZ = rootsZ;

    return shape;
}

// どの軸も 1〜LAB_MAX_ROOTS_PER_AXIS
FX_FN bool LabShapeValid(LabBoxShape shape) {
    return shape.rootsX >= 1 && shape.rootsX <= LAB_MAX_ROOTS_PER_AXIS && shape.rootsY >= 1 &&
           shape.rootsY <= LAB_MAX_ROOTS_PER_AXIS && shape.rootsZ >= 1 && shape.rootsZ <= LAB_MAX_ROOTS_PER_AXIS;
}

// GPU のルート定数 1 語に詰める(shaders/sim/lab_box.hlsl の g_external1)
FX_FN uint32_t LabPackShape(LabBoxShape shape) {
    return shape.rootsX | (shape.rootsY << LAB_SHAPE_FIELD_BITS) | (shape.rootsZ << (2 * LAB_SHAPE_FIELD_BITS));
}

FX_FN LabBoxShape LabUnpackShape(uint32_t word) {
    const uint32_t mask = (1u << LAB_SHAPE_FIELD_BITS) - 1;
    return LabMakeShape(word & mask, (word >> LAB_SHAPE_FIELD_BITS) & mask,
                        (word >> (2 * LAB_SHAPE_FIELD_BITS)) & mask);
}

FX_FN uint32_t LabRootCount(LabBoxShape shape) {
    return shape.rootsX * shape.rootsY * shape.rootsZ;
}

FX_FN uint32_t LabRootSlot(LabBoxShape shape, uint32_t rootX, uint32_t rootY, uint32_t rootZ) {
    return rootX + (shape.rootsX * (rootY + (shape.rootsY * rootZ)));
}

// 箱の座標(0〜LAB_MAX_BOX_EDGE − 1)からセルの番号
FX_FN uint32_t LabCellIndex(uint32_t x, uint32_t y, uint32_t z) {
    const uint32_t local = MrCellIndex(x % MR_BLOCK_EDGE, y % MR_BLOCK_EDGE, z % MR_BLOCK_EDGE);
    const uint32_t roots = (x / MR_BLOCK_EDGE) | ((y / MR_BLOCK_EDGE) << LAB_ROOT_COORD_BITS) |
                           ((z / MR_BLOCK_EDGE) << (2 * LAB_ROOT_COORD_BITS));

    return local | (roots << LAB_CELL_ROOT_SHIFT);
}

FX_FN uint32_t LabCellRootX(uint32_t index) {
    return (index >> LAB_CELL_ROOT_SHIFT) & LAB_ROOT_COORD_MASK;
}

FX_FN uint32_t LabCellRootY(uint32_t index) {
    return (index >> (LAB_CELL_ROOT_SHIFT + LAB_ROOT_COORD_BITS)) & LAB_ROOT_COORD_MASK;
}

FX_FN uint32_t LabCellRootZ(uint32_t index) {
    return (index >> (LAB_CELL_ROOT_SHIFT + (2 * LAB_ROOT_COORD_BITS))) & LAB_ROOT_COORD_MASK;
}

FX_FN uint32_t LabCellX(uint32_t index) {
    return MrCellX(index & (MR_BLOCK_CELLS - 1)) + (MR_BLOCK_EDGE * LabCellRootX(index));
}

FX_FN uint32_t LabCellY(uint32_t index) {
    return MrCellY(index & (MR_BLOCK_CELLS - 1)) + (MR_BLOCK_EDGE * LabCellRootY(index));
}

FX_FN uint32_t LabCellZ(uint32_t index) {
    return MrCellZ(index & (MR_BLOCK_CELLS - 1)) + (MR_BLOCK_EDGE * LabCellRootZ(index));
}

// 根の中のセル(MrCellIndex)
FX_FN uint32_t LabCellLocal(uint32_t index) {
    return index & (MR_BLOCK_CELLS - 1);
}

FX_FN bool LabCellInBox(LabBoxShape shape, uint32_t index) {
    return index < LAB_CELL_INDEX_LIMIT && LabCellRootX(index) < shape.rootsX && LabCellRootY(index) < shape.rootsY &&
           LabCellRootZ(index) < shape.rootsZ;
}

// セルを含む根の枠
FX_FN uint32_t LabCellSlot(LabBoxShape shape, uint32_t index) {
    return LabRootSlot(shape, LabCellRootX(index), LabCellRootY(index), LabCellRootZ(index));
}

// つついた根の印(枠 slot のビット。根は LAB_MAX_ROOTS まで)
FX_FN uint64_t LabRootBit(uint32_t slot) {
    return ((uint64_t)1) << slot;
}

FX_FN uint64_t LabAllRoots(LabBoxShape shape) {
    const uint32_t count = LabRootCount(shape);
    return count >= LAB_MAX_ROOTS ? ~((uint64_t)0) : LabRootBit(count) - 1;
}

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

// コマンドが当たるセルの番号(LabCellIndex)。当てられないコマンド(1 セルの種類でない・箱の外・温度が範囲外・物質の誤り)なら LAB_NO_CELL
FX_FN uint32_t LabCommandCell(LabCommand command, uint32_t speciesCount, LabBoxShape shape) {
    const uint32_t type = LabCommandType(command);
    if (type != LAB_COMMAND_TYPE_FILL && type != LAB_COMMAND_TYPE_TEMPERATURE)
        return LAB_NO_CELL;

    const uint32_t cell = LabPayload(command, LAB_PAYLOAD_CELL);
    if (!LabCellInBox(shape, cell) || !LabContentsValid(command, speciesCount))
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
FX_FN bool LabRegionCommandValid(LabCommand command, uint32_t speciesCount, LabBoxShape shape) {
    const uint32_t type = LabCommandType(command);
    if (type != LAB_COMMAND_TYPE_FILL_REGION && type != LAB_COMMAND_TYPE_TEMPERATURE_REGION)
        return false;

    const LabRegion region = LabCommandRegion(command);
    const bool inside = region.highX < MR_BLOCK_EDGE * shape.rootsX && region.highY < MR_BLOCK_EDGE * shape.rootsY &&
                        region.highZ < MR_BLOCK_EDGE * shape.rootsZ;
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

// --- 魔法のパッチ(T-0024・ADR-0095。common/magic_patch.hlsli)-----------------------------------

FX_FN bool LabCommandIsPatch(LabCommand command) {
    return LabCommandType(command) == LAB_COMMAND_TYPE_PATCH;
}

// コマンドの刻み(words[0..1] = targetTick)
FX_FN uint64_t LabCommandTick(LabCommand command) {
    return FX_U64(command.words[1], command.words[0]);
}

FX_FN MpPatch LabCommandPatch(LabCommand command) {
    return MpPatchFromCommand(
        LabPayload(command, MP_COMMAND_RULE), LabPayload(command, MP_COMMAND_EFFECT),
        LabPayload(command, MP_COMMAND_DURATION), (int32_t)LabPayload(command, MP_COMMAND_LOW),
        (int32_t)LabPayload(command, MP_COMMAND_LOW + 1), (int32_t)LabPayload(command, MP_COMMAND_LOW + 2),
        (int32_t)LabPayload(command, MP_COMMAND_HIGH), (int32_t)LabPayload(command, MP_COMMAND_HIGH + 1),
        (int32_t)LabPayload(command, MP_COMMAND_HIGH + 2), LabCommandTick(command));
}

// パッチの範囲がブロック(8³ セル)に重なるか
FX_FN bool LabPatchTouchesBlock(MpPatch patch, MrBlock block) {
    const int64_t last = (int64_t)MR_BLOCK_EDGE - 1;

    return MpOverlaps(patch, block.level, block.originX, block.originY, block.originZ, block.originX + last,
                      block.originY + last, block.originZ + last);
}

// 箱の根のうち、パッチの範囲が重なるもの(LabRootBit。T-0173 の大きい箱でも、範囲は箱の座標 = レベル 0 の座標)。
// Blocks は Block(slot) で根の MrBlock を返す型(C++ は木の枠の span、HLSL は g_blocks)
template <typename Blocks>
FX_FN uint64_t LabPatchRoots(Blocks blocks, LabBoxShape shape, MpPatch patch) {
    uint64_t roots = 0;
    const uint32_t count = LabRootCount(shape);
    for (uint32_t slot = 0; slot < count; ++slot) {
        if (LabPatchTouchesBlock(patch, blocks.Block(slot)))
            roots |= LabRootBit(slot);
    }

    return roots;
}

// パッチのコマンドを当てる: 列に足し、足せたら範囲が重なる根(LabRootBit。呼ぶ側がつつく)。足せなければ 0。
// ruleCount = 表の規則の数
template <typename List, typename Blocks>
FX_FN uint64_t LabApplyPatchCommand(List list, Blocks blocks, LabBoxShape shape, LabCommand command,
                                    uint32_t ruleCount) {
    const MpPatch patch = LabCommandPatch(command);
    if (!MpAddPatch(list, patch, LabCommandTick(command), ruleCount))
        return 0;

    return LabPatchRoots(blocks, shape, patch);
}

// 刻み tick の初め(コマンドを当てる前): 速さが変わる(始まり・終わり)パッチの範囲が重なる根(LabRootBit。呼ぶ側がつつく)。
// その後で終わったパッチを列から除く
template <typename List, typename Blocks>
FX_FN uint64_t LabPatchBoundaries(List list, Blocks blocks, LabBoxShape shape, uint64_t tick) {
    const uint32_t count = MpPatchCount(list);
    uint64_t roots = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const MpPatch patch = MpLoadPatch(list, i);
        if (MpBoundaryAt(patch, tick))
            roots |= LabPatchRoots(blocks, shape, patch);
    }

    MpRetireExpired(list, tick);

    return roots;
}

LAB_NAMESPACE_END

#endif  // BICAMERAL_LAB_BOX_HLSLI
