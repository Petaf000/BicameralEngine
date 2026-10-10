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
// [0] 中心のセル x | y << 8 | z << 16
// [1] 半径(セル。0 = 1 セル)| 置き方 << 8 | 物質の数 << 16
// [2] 温度(mK)
// [3..11] [物質 ID][量の下位][量の上位](µmol。1 セルあたり)× 数
FX_CONST uint32_t PROBE_PLACE_PAYLOAD_WORDS = 12;
FX_CONST uint32_t PROBE_PLACE_WORD_CENTER = 0;
FX_CONST uint32_t PROBE_PLACE_WORD_SHAPE = 1;
FX_CONST uint32_t PROBE_PLACE_WORD_TEMPERATURE = 2;
FX_CONST uint32_t PROBE_PLACE_WORD_ENTRIES = 3;
FX_CONST uint32_t PROBE_PLACE_WORDS_PER_ENTRY = 3;

FX_CONST uint32_t PROBE_PLACE_MAX_SPECIES = 3;
FX_CONST uint32_t PROBE_PLACE_MAX_RADIUS = 8;  // 1 つのコマンドで 17³ セルまで(適用は 1 スレッドなので重さの上限)
FX_CONST uint32_t PROBE_PLACE_MODE_REPLACE = 0;
FX_CONST uint32_t PROBE_PLACE_MODE_ADD = 1;
// 1 セルの 1 物質の量の上限(2^36 µmol ≈ 6.9 万 mol。0.5 m 角の木は約 4 億 µmol)。足し続けて桁が溢れないように
FX_CONST uint64_t PROBE_PLACE_MAX_AMOUNT = FX_U64(16u, 0u);
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
    uint32_t radius;
    uint32_t mode;
    uint32_t count;
    uint32_t temperature;
    uint32_t species[PROBE_PLACE_MAX_SPECIES];
    uint64_t amounts[PROBE_PLACE_MAX_SPECIES];
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

    // --- 物質(数が範囲の外なら読まない)---
    const bool countValid = place.count != 0 && place.count <= PROBE_PLACE_MAX_SPECIES;
    bool entriesValid = countValid;
    for (uint32_t i = 0; i < PROBE_PLACE_MAX_SPECIES; ++i) {
        const uint32_t word = PROBE_PLACE_WORD_ENTRIES + (PROBE_PLACE_WORDS_PER_ENTRY * i);
        const bool used = countValid && i < place.count;
        place.species[i] = used ? payload.words[word] : 0;
        place.amounts[i] = used ? FX_U64(payload.words[word + 2], payload.words[word + 1]) : 0;
        if (used && !ProbePlaceEntryValid(place, i, speciesCount))
            entriesValid = false;
    }

    // --- 形 ---
    const bool inside = place.x < PROBE_GRID_SIZE && place.y < PROBE_GRID_SIZE && place.z < PROBE_GRID_SIZE &&
                        (center >> 24) == 0;
    const bool shapeValid = place.radius <= PROBE_PLACE_MAX_RADIUS && place.mode <= PROBE_PLACE_MODE_ADD &&
                            place.temperature <= PROBE_PLACE_MAX_TEMPERATURE_MILLIKELVIN;
    place.valid = inside && shapeValid && entriesValid ? 1u : 0u;

    return place;
}

// --- 形(球。半径 0 は中心の 1 セル)---

// 軸ごとの範囲 [begin, end)(格子の中に切る)
FX_FN uint32_t ProbePlaceBegin(uint32_t center, uint32_t radius) {
    return center > radius ? center - radius : 0u;
}

FX_FN uint32_t ProbePlaceEnd(uint32_t center, uint32_t radius) {
    const uint32_t last = center + radius;

    return last < PROBE_GRID_SIZE ? last + 1 : PROBE_GRID_SIZE;
}

// セル (x, y, z) が球の中か(中心からの距離の 2 乗 ≤ 半径の 2 乗)
FX_FN bool ProbePlaceCovers(ProbePlace place, uint32_t x, uint32_t y, uint32_t z) {
    const int32_t dx = (int32_t)x - (int32_t)place.x;
    const int32_t dy = (int32_t)y - (int32_t)place.y;
    const int32_t dz = (int32_t)z - (int32_t)place.z;

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

    // --- 足す: 量の上限を超える・成分が入りきらないセルは飛ばす ---
    RxAdded added;
    added.cell = cell;
    added.overflowed = 0;
    for (uint32_t i = 0; i < place.count; ++i) {
        const uint32_t slot = RxFindSlot(added.cell, place.species[i]);
        if (slot != RX_NO_SLOT && added.cell.amounts[slot] > PROBE_PLACE_MAX_AMOUNT - place.amounts[i]) {
            added.overflowed = 1;
            break;
        }

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
