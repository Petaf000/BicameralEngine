// probe_world.hlsli — 仮の世界(common/probe_sim.hlsli)のセルの規則: 伝導 + 反応の 1 セルの 1 刻み・つつき・ハッシュ(T-0089)。
// HLSL と C++ で共通(fixed.hlsli の約束)。GPU(shaders/sim/probe_conduct.hlsl・probe_tick.hlsl)と CPU リファレンス(sim/probe_sim.cpp)が同じ関数を呼ぶ。
//
// データの流れ(1 セル・刻み t):
//   S(t) の自分のセルと、自分と 6 面の隣の熱のキャッシュ → 面の流れの和をエネルギーから引く(伝導。heat_conduction.hlsli)
//   → そのセルで反応を評価(reaction.hlsli の RxStepCellWait。待ちの丸め。ADR-0018・T-0122)
//   → S(t + 1) のセルとキャッシュ、変わったか、次に評価の要る刻みの印(wakeTick)
// 刻みと「ブロックが最後に変わった刻み」(tc)は印で渡す(印 = 刻み + 1。0 = 初めの状態 = 刻み 0 の前に変わった。multires.hlsli の MrChangeMark と同じ)。
// キャッシュ(温度・コンダクタンス・面の係数の上限)はセルから決まる値。コンダクタンスは成分が変わった刻み(反応の候補があった刻み)だけ作り直す。
#ifndef BICAMERAL_PROBE_WORLD_HLSLI
#define BICAMERAL_PROBE_WORLD_HLSLI

#include "heat_conduction.hlsli"
#include "probe_sim.hlsli"

#ifdef __cplusplus
#define PROBE_WORLD_NAMESPACE_BEGIN      \
    namespace bicameral::sim {           \
        using namespace ::bicameral::fx; \
        using namespace ::bicameral::reaction;
#define PROBE_WORLD_NAMESPACE_END }
#else
#define PROBE_WORLD_NAMESPACE_BEGIN
#define PROBE_WORLD_NAMESPACE_END
#endif

PROBE_WORLD_NAMESPACE_BEGIN

// 1 セルの 1 刻みの結果
struct ProbeCellStep {
    RxCell cell;
    HcThermalCache cache;
    uint32_t changed;  // S(t + 1) のセルが S(t) と違えば 1
    uint64_t
        wakeTick;  // 変わらなければ、次に評価の要る刻みの印(進める規則があれば次の刻みの印。進めないなら RX_WAIT_NEVER)
};

// 反応の乱数の世界のシード
FX_FN uint64_t ProbeWorldSeed() {
    return FX_U64(PROBE_WORLD_SEED_HIGH, PROBE_WORLD_SEED_LOW);
}

// 2 つのセルが同じか(成分は物質 ID の昇順で、0 は消してあるので、中身が同じなら並びも同じ)
FX_FN bool ProbeSameCell(RxCell a, RxCell b) {
    if (a.energy != b.energy || a.speciesCount != b.speciesCount)
        return false;

    for (uint32_t i = 0; i < a.speciesCount; ++i) {
        if (a.species[i] != b.species[i] || a.amounts[i] != b.amounts[i])
            return false;
    }

    return true;
}

// セルの成分に、反応物として索引に載っている物質があるか(無ければ反応の候補は決して無い)
template <typename Table>
FX_FN bool ProbeHasRules(Table table, RxCell cell) {
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        if (table.Species(cell.species[i]).ruleCount != 0)
            return true;
    }

    return false;
}

// セルのキャッシュを一から作る(初めの状態・つつきの後)
template <typename Table>
FX_FN HcThermalCache ProbeMakeCache(Table table, RxCell cell) {
    return HcMakeCache(RxComputeThermal(table, cell), HcCellConductance(table, cell));
}

// 刻み tick の印(ブロックが最後に変わった刻み・次に評価の要る刻みに使う。64bit なので一周しない)
FX_FN uint64_t ProbeChangeMark(uint64_t tick) {
    return tick + 1;
}

// 伝導と反応(06 §2 の段 3・4)。neighbors は −x, +x, −y, +y, −z, +z の順のキャッシュ(格子の外は自分 = 断熱)。
// changedMark = セルのブロックが最後に変わった刻みの印(刻み tick の印より小さい。刻みの初めに読んだ値)
template <typename Table>
FX_FN ProbeCellStep ProbeStepCell(Table table, RxCell cell, HcThermalCache self, HcThermalCache minusX,
                                  HcThermalCache plusX, HcThermalCache minusY, HcThermalCache plusY,
                                  HcThermalCache minusZ, HcThermalCache plusZ, uint64_t tick, uint64_t changedMark,
                                  uint32_t cellIndex) {
    // --- 伝導: 面の流れの和(自分 → 隣が正)を引く ---
    const int64_t outflow = HcFaceFlow(self, minusX) + HcFaceFlow(self, plusX) + HcFaceFlow(self, minusY) +
                            HcFaceFlow(self, plusY) + HcFaceFlow(self, minusZ) + HcFaceFlow(self, plusZ);
    RxCell conducted = cell;
    conducted.energy = cell.energy - outflow;

    ProbeCellStep result;

    // --- 近道: 熱の出入りが無く、反応の候補も無いセル(静かな空気)は何も変わらない ---
    // 下の本道でも、セルはそのまま・キャッシュは同じセルから作り直した値(= 今のキャッシュ。キャッシュはセルから決まる)になるので、
    // 結果はビット単位で同じ。割り算(温度)を省く
    if (outflow == 0 && !ProbeHasRules(table, cell)) {
        result.cell = cell;
        result.cache = self;
        result.changed = 0;
        result.wakeTick = RX_WAIT_NEVER;

        return result;
    }
    // --- 反応(セルの中で閉じる)---
    const RxWaitStep step = RxStepCellWait(table, conducted, ProbeWorldSeed(), ProbeChangeMark(tick), changedMark,
                                           cellIndex);
    result.cell = step.cell;
    result.wakeTick = step.wakeTick;
    result.changed = ProbeSameCell(step.cell, cell) ? 0 : 1;

    // 反応で成分が変わったときだけコンダクタンスを作り直す(割り算を減らす。HLSL の ?: は両辺を評価しうるので if で)
    uint32_t conductance = self.conductance;
    if (!ProbeSameCell(step.cell, conducted))
        conductance = HcCellConductance(table, step.cell);

    result.cache = HcMakeCache(step.thermal, conductance);

    return result;
}

// つつき: そのセルを約 2700 K 温めるエネルギー(mJ。熱容量 nJ/K から。probe_sim.hlsli)
template <typename Table>
FX_FN int64_t ProbePokeEnergy(Table table, RxCell cell) {
    const uint64_t heatCapacity = RxHeatCapacity(table, cell);
    const FxU128 product = FxMulU64Full(heatCapacity, PROBE_POKE_ENERGY_MULTIPLIER);
    FX_ASSERT((product.hi >> PROBE_POKE_ENERGY_SHIFT) == 0);

    return (int64_t)((product.hi << (64 - PROBE_POKE_ENERGY_SHIFT)) | (product.lo >> PROBE_POKE_ENERGY_SHIFT));
}

// 1 セルの寄与。状態のハッシュ = 全セルの寄与の和(mod 2^64)。和は足す順番に依存しないので、
// GPU が並列に(wave の和 + 64bit の atomic)足しても CPU が順に足しても同じ値になる(04 R2)。キャッシュはセルから決まるので入れない
FX_FN uint64_t ProbeCellHash(uint32_t cellIndex, RxCell cell) {
    uint64_t hash = FxHashCombine((uint64_t)cellIndex, (uint64_t)cell.energy);
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        hash = FxHashCombine(hash, (uint64_t)cell.species[i]);
        hash = FxHashCombine(hash, cell.amounts[i]);
    }

    return ProbeMix64(hash);
}

// 抽出に写す物質量(µmol、uint32 で飽和。無ければ 0)
FX_FN uint32_t ProbeViewAmount(RxCell cell, uint32_t speciesId) {
    const uint32_t slot = RxFindSlot(cell, speciesId);
    if (slot == RX_NO_SLOT)
        return 0;

    const uint64_t amount = cell.amounts[slot];

    return amount < (uint64_t)0xFFFFFFFFu ? (uint32_t)amount : 0xFFFFFFFFu;
}

PROBE_WORLD_NAMESPACE_END

#endif  // BICAMERAL_PROBE_WORLD_HLSLI
