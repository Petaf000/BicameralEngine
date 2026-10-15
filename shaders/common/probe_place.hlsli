// probe_place.hlsli — 世界に物を置く筆(T-0222・D-449)のコマンドの形と、1 セルに当てる関数。
// HLSL と C++ で共通(fixed.hlsli の約束)。GPU(shaders/sim/probe_tick.hlsl の ApplyPlace)と CPU リファレンス
// (engine/src/sim/probe_sim.cpp の ProbeReference)が同じ関数を呼ぶので、置いた結果はビット単位で同じになる。
//
// データの流れ:
//   エディタの筆(editor/brush_panel。CPU の Controller)→ sim::Command(type = PROBE_COMMAND_TYPE_PLACE。sim::MakePlaceCommand)
//   → 再生ファイルに記録 → GPU のキュー → 刻み t の適用の単位が、球の中のセルを (z, y, x) の順に置き換える・足す
//   → 触ったブロックを「刻みの直前に変わった」にして起こす(待ちの丸め。ADR-0018)→ 同じ刻みの伝導と反応
// 置く・足すのはエディタの明示的な湧き出し(保存則の外。つつきと同じ扱い。D-206)。変わったエネルギーは湧き出しの欄に数える。
// 置き換え(実験室の「置く」と同じ。D-439)は、セルの中身を選んだ材料にして温度を決める。
// 足すは、今の中身に材料を足し、足した材料が持ち込むエネルギー(化学 + その温度の熱)を足す(混ざった温度になる)。
// 形は球・直方体・円柱(T-0245・ADR-0089)。形の欄は T-0222 の記録では 0(= 球)なので、古い記録は同じ意味のまま読める。
#ifndef BICAMERAL_PROBE_PLACE_HLSLI
#define BICAMERAL_PROBE_PLACE_HLSLI

#include "probe_world.hlsli"

#ifdef __cplusplus
#define PROBE_PLACE_NAMESPACE_BEGIN      \
    namespace bicameral::sim {           \
        using namespace ::bicameral::fx; \
        using namespace ::bicameral::reaction;
#define PROBE_PLACE_NAMESPACE_END }
#else
#define PROBE_PLACE_NAMESPACE_BEGIN
#define PROBE_PLACE_NAMESPACE_END
#endif

PROBE_PLACE_NAMESPACE_BEGIN

// --- payload の形(sim/command.h の 48 バイト = 12 語)---
// [0] 中心のセル x | y << 8 | z << 16 | 大きさ 2 << 24(4 ビット)| 大きさ 3 << 28(4 ビット)
// [1] 大きさ 1(セル。0 = 1 セル)| 置き方 << 8 | 物質の数 << 16 | 形 << 24
//     球:     大きさ 1 = 半径(大きさ 2・3 は 0)
//     直方体: 大きさ 1・2・3 = x・y・z の半幅(中心から端のセルまで。0 = 1 セルの厚さ)
//     円柱:   大きさ 1 = 半径・大きさ 2 = 軸の向きの半幅・大きさ 3 = 軸(0 = x・1 = y・2 = z)
// [2] 温度(mK)
// [3..11] [物質 ID][量の下位][量の上位](µmol。1 セルあたり)× 数
FX_CONST uint32_t PROBE_PLACE_PAYLOAD_WORDS = 12;
FX_CONST uint32_t PROBE_PLACE_WORD_CENTER = 0;
FX_CONST uint32_t PROBE_PLACE_WORD_SHAPE = 1;
FX_CONST uint32_t PROBE_PLACE_WORD_TEMPERATURE = 2;
FX_CONST uint32_t PROBE_PLACE_WORD_ENTRIES = 3;
FX_CONST uint32_t PROBE_PLACE_WORDS_PER_ENTRY = 3;

FX_CONST uint32_t PROBE_PLACE_MAX_SPECIES = 3;
FX_CONST uint32_t PROBE_PLACE_MAX_RADIUS = 8;  // 1 つのコマンドで 17³ セルまで(重さの上限。適用は 1 グループ。ADR-0125)
FX_CONST uint32_t PROBE_PLACE_SHAPE_SPHERE = 0;
FX_CONST uint32_t PROBE_PLACE_SHAPE_BOX = 1;
FX_CONST uint32_t PROBE_PLACE_SHAPE_CYLINDER = 2;
FX_CONST uint32_t PROBE_PLACE_MODE_REPLACE = 0;
FX_CONST uint32_t PROBE_PLACE_MODE_ADD = 1;
// 1 セルの量の合計の上限(セルの器 CELL_AMOUNT_LIMIT = 2^36 µmol ≈ 6.9 万 mol。units.hlsli・ADR-0260)。
// 材料の合計も、足した後のセルの合計もこれまで(T-0558 の前は 1 物質ごとに 2^36)
FX_CONST uint64_t PROBE_PLACE_MAX_AMOUNT = CELL_AMOUNT_LIMIT_MICROMOLES;
// 温度の上限(実験室と同じ。反応の表の温度の範囲)
FX_CONST uint32_t PROBE_PLACE_MAX_TEMPERATURE_MILLIKELVIN = 6000000;

struct ProbePlacePayload {
    uint32_t words[PROBE_PLACE_PAYLOAD_WORDS];
};

// 読み解いたコマンド。valid = 0 なら GPU も CPU も何もしない(知らない置き方・格子の外・物質の誤りなど)
struct ProbePlace {
    uint32_t valid;
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t radius;  // 大きさ 1(球と円柱の半径・直方体の x の半幅)
    uint32_t mode;
    uint32_t count;
    uint32_t temperature;
    uint32_t species[PROBE_PLACE_MAX_SPECIES];
    uint64_t amounts[PROBE_PLACE_MAX_SPECIES];

    // --- 形(T-0245)---
    uint32_t shape;
    uint32_t axis;     // 円柱の軸(0 = x・1 = y・2 = z)
    uint32_t extentX;  // 中心から各軸の端のセルまで(形を囲む箱の半幅)
    uint32_t extentY;
    uint32_t extentZ;
};

// 1 セルに当てた結果(applied = 0 なら cell は元のまま。足すと成分がインラインの数を超えるセルは飛ばす)
struct ProbePlaced {
    RxCell cell;
    uint32_t applied;
};

// --- 読み解く ---

// 物質 i が使えるか(表の中・量が 0 でない・上限以下・前の物質と重ならない)
FX_FN bool ProbePlaceEntryValid(ProbePlace place, uint32_t i, uint32_t speciesCount) {
    if (place.species[i] == 0 || place.species[i] >= speciesCount)
        return false;

    if (place.amounts[i] == 0 || place.amounts[i] > PROBE_PLACE_MAX_AMOUNT)
        return false;

    for (uint32_t j = 0; j < i; ++j) {
        if (place.species[j] == place.species[i])
            return false;
    }

    return true;
}

// speciesCount = その刻みの表の物質の数(ID 0 は使わない)
FX_FN ProbePlace ProbeDecodePlace(ProbePlacePayload payload, uint32_t speciesCount) {
    ProbePlace place;
    const uint32_t center = payload.words[PROBE_PLACE_WORD_CENTER];
    const uint32_t shape = payload.words[PROBE_PLACE_WORD_SHAPE];
    place.x = center & 0xFFu;
    place.y = (center >> 8) & 0xFFu;
    place.z = (center >> 16) & 0xFFu;
    place.radius = shape & 0xFFu;
    place.mode = (shape >> 8) & 0xFFu;
    place.count = (shape >> 16) & 0xFFu;
    place.temperature = payload.words[PROBE_PLACE_WORD_TEMPERATURE];
    place.shape = shape >> 24;

    // --- 形の大きさ(大きさ 2・3 は中心の語の上の 8 ビット)---
    const uint32_t size2 = (center >> 24) & 0xFu;
    const uint32_t size3 = center >> 28;
    place.axis = 0;
    place.extentX = place.radius;
    place.extentY = place.radius;
    place.extentZ = place.radius;
    bool sizesValid = size2 == 0 && size3 == 0;  // 球
    if (place.shape == PROBE_PLACE_SHAPE_BOX) {
        place.extentY = size2;
        place.extentZ = size3;
        sizesValid = size2 <= PROBE_PLACE_MAX_RADIUS && size3 <= PROBE_PLACE_MAX_RADIUS;
    } else if (place.shape == PROBE_PLACE_SHAPE_CYLINDER) {
        place.axis = size3;
        place.extentX = size3 == 0 ? size2 : place.radius;
        place.extentY = size3 == 1 ? size2 : place.radius;
        place.extentZ = size3 == 2 ? size2 : place.radius;
        sizesValid = size2 <= PROBE_PLACE_MAX_RADIUS && size3 <= 2;
    } else if (place.shape != PROBE_PLACE_SHAPE_SPHERE) {
        sizesValid = false;
    }

    // --- 物質(数が範囲の外なら読まない)---
    const bool countValid = place.count != 0 && place.count <= PROBE_PLACE_MAX_SPECIES;
    bool entriesValid = countValid;
    uint64_t total = 0;  // 材料の合計(物質ごとに上限以下なので、合計の比べ方は溢れない)
    for (uint32_t i = 0; i < PROBE_PLACE_MAX_SPECIES; ++i) {
        const uint32_t word = PROBE_PLACE_WORD_ENTRIES + (PROBE_PLACE_WORDS_PER_ENTRY * i);
        const bool used = countValid && i < place.count;
        place.species[i] = used ? payload.words[word] : 0;
        place.amounts[i] = used ? FX_U64(payload.words[word + 2], payload.words[word + 1]) : 0;
        if (used && !ProbePlaceEntryValid(place, i, speciesCount))
            entriesValid = false;

        if (entriesValid && place.amounts[i] > PROBE_PLACE_MAX_AMOUNT - total)
            entriesValid = false;

        total += entriesValid ? place.amounts[i] : 0;
    }

    // --- 形 ---
    const bool inside = place.x < PROBE_GRID_SIZE && place.y < PROBE_GRID_SIZE && place.z < PROBE_GRID_SIZE;
    const bool shapeValid = place.radius <= PROBE_PLACE_MAX_RADIUS && sizesValid &&
                            place.mode <= PROBE_PLACE_MODE_ADD &&
                            place.temperature <= PROBE_PLACE_MAX_TEMPERATURE_MILLIKELVIN;
    place.valid = inside && shapeValid && entriesValid ? 1u : 0u;

    return place;
}

// --- 形(球・直方体・円柱。大きさ 0 は中心の 1 セルの厚さ)---

// 軸ごとの範囲 [begin, end)(中心 ± extent を格子の中に切る。extent は ProbePlace の extentX・Y・Z)
FX_FN uint32_t ProbePlaceBegin(uint32_t center, uint32_t extent) {
    return center > extent ? center - extent : 0u;
}

FX_FN uint32_t ProbePlaceEnd(uint32_t center, uint32_t extent) {
    const uint32_t last = center + extent;

    return last < PROBE_GRID_SIZE ? last + 1 : PROBE_GRID_SIZE;
}

// セル (x, y, z) が形の中か。球: 中心からの距離の 2 乗 ≤ 半径の 2 乗。直方体: 各軸で半幅の中。
// 円柱: 軸に垂直な 2 方向の距離の 2 乗 ≤ 半径の 2 乗 かつ 軸の向きで半幅の中
FX_FN bool ProbePlaceCovers(ProbePlace place, uint32_t x, uint32_t y, uint32_t z) {
    const int32_t dx = (int32_t)x - (int32_t)place.x;
    const int32_t dy = (int32_t)y - (int32_t)place.y;
    const int32_t dz = (int32_t)z - (int32_t)place.z;
    const bool inBox = (uint32_t)(dx * dx) <= place.extentX * place.extentX &&
                       (uint32_t)(dy * dy) <= place.extentY * place.extentY &&
                       (uint32_t)(dz * dz) <= place.extentZ * place.extentZ;
    if (place.shape == PROBE_PLACE_SHAPE_BOX)
        return inBox;

    if (place.shape == PROBE_PLACE_SHAPE_CYLINDER) {
        const int32_t across1 = place.axis == 0 ? dy : dx;
        const int32_t across2 = place.axis == 2 ? dy : dz;

        return inBox && (uint32_t)((across1 * across1) + (across2 * across2)) <= place.radius * place.radius;
    }

    return (uint32_t)((dx * dx) + (dy * dy) + (dz * dz)) <= place.radius * place.radius;
}

// --- セルに当てる ---

// 材料(成分だけのセル)が温度 temperature(mK)で持つエネルギー(mJ)= 化学 + 熱容量 × 温度 を mJ に切り上げる
// (sim::MakeReactionCell・実験室の LabSetTemperature と同じ値)
template <typename Table>
FX_FN int64_t ProbeMaterialEnergy(Table table, RxCell material, uint32_t temperatureMilliKelvin) {
    const int64_t chemical = RxChemicalEnergy(table, material);
    const uint64_t heatCapacity = RxHeatCapacity(table, material);
    const FxU128 product = FxMulU64Full(heatCapacity, (uint64_t)temperatureMilliKelvin);
    const int64_t heat = (int64_t)FxDivU128By64(product, RX_MILLIKELVIN_NUMERATOR).quotient;

    // --- µJ → mJ。正は切り上げ、負は 0 の方向へ切る(どちらも熱が負にならない向き)---
    const int64_t total = chemical + heat;
    const uint64_t magnitude = total >= 0 ? (uint64_t)total + (RX_MICROJOULES_PER_MILLIJOULE - 1) : (uint64_t)(-total);
    const int64_t millijoules = (int64_t)FxDivU128By64(FxMulU64Full(magnitude, 1), RX_MICROJOULES_PER_MILLIJOULE)
                                    .quotient;

    return total >= 0 ? millijoules : -millijoules;
}

// セルの量の合計(上限で止める。決まった数の回しで読む。T-0397)
FX_FN uint64_t ProbeCellTotalAmount(RxCell cell) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        const uint64_t amount = i < cell.speciesCount ? cell.amounts[i] : (uint64_t)0;
        total = amount > PROBE_PLACE_MAX_AMOUNT - total ? PROBE_PLACE_MAX_AMOUNT + 1 : total + amount;
    }

    return total;
}

// コマンドの材料だけのセル(成分は 3 つまでなので必ず入る)
FX_FN RxCell ProbePlaceMaterial(ProbePlace place) {
    RxCell material = RxMakeEmptyCell(0);
    for (uint32_t i = 0; i < place.count; ++i)
        material = RxAddSpecies(material, place.species[i], place.amounts[i]);

    return material;
}

// 1 セルに当てる(place.valid のコマンドだけ)
template <typename Table>
FX_FN ProbePlaced ProbePlaceCell(Table table, RxCell cell, ProbePlace place) {
    const RxCell material = ProbePlaceMaterial(place);
    const int64_t materialEnergy = ProbeMaterialEnergy(table, material, place.temperature);

    ProbePlaced result;
    result.cell = cell;
    result.applied = 0;

    // --- 置き換え ---
    if (place.mode == PROBE_PLACE_MODE_REPLACE) {
        result.cell = material;
        result.cell.energy = materialEnergy;
        result.applied = 1;
        return result;
    }

    // --- 足す: 足した後の合計がセルの器を超える・成分が入りきらないセルは飛ばす ---
    const uint64_t cellTotal = ProbeCellTotalAmount(cell);
    const uint64_t materialTotal = ProbeCellTotalAmount(material);
    if (cellTotal > PROBE_PLACE_MAX_AMOUNT || materialTotal > PROBE_PLACE_MAX_AMOUNT - cellTotal)
        return result;

    RxAdded added;
    added.cell = cell;
    added.overflowed = 0;
    for (uint32_t i = 0; i < place.count; ++i) {
        const RxAdded next = RxTryAddSpecies(added.cell, place.species[i], place.amounts[i]);
        added.cell = next.cell;
        added.overflowed |= next.overflowed;
    }

    if (added.overflowed != 0)
        return result;

    result.cell = added.cell;
    result.cell.energy = cell.energy + materialEnergy;
    result.applied = 1;

    return result;
}

PROBE_PLACE_NAMESPACE_END

#endif  // BICAMERAL_PROBE_PLACE_HLSLI
