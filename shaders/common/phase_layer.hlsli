// phase_layer.hlsli — セルより小さい層: 凝縮相と気体が混ざるセルの 2 つのエネルギー(D-665・ADR-0469 の 3-2・20 §10.6。T-1160・ADR-0486)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(tests/phase_layer_test.cpp の列。世界の木へは T-1211)と
// GPU(shaders/sim/phase_layer_cells.hlsl。T-1211)が同じ関数を呼ぶ。
//
// 芯: 0.5 m のセルの中の薄い凝縮相(氷・水たまり・露・煤)は、温度が気体と 1 つだとセルの真ん中を温度の点として数えられ、
//   相が変わる層(溶ける・沸く)の熱が約 30 倍遅く、薄い層の道を短くするとセルの空気まで石とつながる(T-1110 の打ち切り)。
//   凝縮相と気体が混ざるセルでだけ、凝縮相のエネルギー Ec を別の真実として持つ(気体のエネルギー = セルのエネルギー − Ec)。
//   気体だけ・凝縮相だけのセルは今の 1 つのまま(Ec を持たない = 疎)。保存はセルのエネルギー(和)の今の列のまま。
//
// データの流れ:
//   セル(RxCell)+ Ec → 層の見立て(PlLayerOf: 体積・高さ)→ 2 つの部分の熱(PlPartsOf)
//   → 面ごとの側(PlFaceSide: 下の面 = 層・上の面 = 気体・横の面 = 高さで分けた 2 つ)→ heat_conduction.hlsli の HcFaceFlow
//   → 層の上の面の対の面(PlInnerFlow: 層 ⇄ 気体)
//   → 反応は 2 回(PlCondensedView → 凝縮相の項を持つ規則 / 気体だけのセル → 気体だけの規則)。気体の項の出入りは PlGasEnergyAfter で気体の側へ
//   → セルの段の形(T-1211): PlInnerFlowOf → PlBeginReact・反応の核・PlEndReact → PlRemixCondensedEnergy(1 セルだけの刻みは PlStepLoneCell)
//
// 層の形(D-664 の置かれ方・ADR-0469 の 3-1 (a)): 凝縮相は重さの向き(−y)に積もる。厚さ = 凝縮相の体積 ÷ 底の面積(体積の割合 × セルの高さ)。
//   下の面は全部が層、上の面は全部が気体、横の面は下から層の高さまでが層。
// 面の係数は heat_conduction.hlsli と同じ「両側の小さい方」の形(片側の値 = 伝導率 × 面積 ÷ (2 × 中心から面までの距離))。
//   層の中心から下の面までは厚さの半分なので、片側の値 = 伝導率 × 2^15 ÷ 高さ(Q16)(mW/K。満ちたセルでは今の 伝導率 × 0.5 と同じ)。
// 安定(heat_conduction.hlsli の 1/8): 部分ごとに、6 面(横の面は面積の割合で分けた上限)+ 層の上の面の重みの和が 7/8 以下。
//
// 浮動小数点は使わない(04 §4)。64bit の引き算と同じ値どうしの比べを並べない(CLAUDE.md §4。高さは 32bit で数える)。
#ifndef BICAMERAL_PHASE_LAYER_HLSLI
#define BICAMERAL_PHASE_LAYER_HLSLI

#include "heat_conduction.hlsli"

RX_NAMESPACE_BEGIN

// --- 定数 --------------------------------------------------------------------------------------
FX_CONST uint32_t PL_FULL_Q16 = 65536;
// 層の高さの下限(Q16。0.5 m ÷ 65536 ≈ 7.6 µm)。場所を取らない凝縮相(煤など)だけの層もこの厚さとして数える
FX_CONST uint32_t PL_MIN_HEIGHT_Q16 = 1;
// 片側の値 = 伝導率(mW/(m·K))× 2^15 ÷ 高さ(Q16)
FX_CONST uint32_t PL_HALF_SHIFT = 15;

// --- 層の見立て -----------------------------------------------------------------------------------
struct PlLayer {
    uint64_t condensedVolume;  // Σ 凝縮相の体積(pL = µmol × mm³/mol)
    uint64_t condensedAmount;  // Σ 凝縮相(気体でない物質)の量(µmol。場所を取らない物質も)
    uint64_t gasAmount;        // Σ 気体の量(µmol)
    uint32_t heightQ16;        // 層の高さ ÷ セルの高さ(Q16。PL_MIN_HEIGHT_Q16〜PL_FULL_Q16)
    uint32_t mixed;            // 1 = 凝縮相と気体が混ざるセル(Ec を持つ)。満ちたセル(孔の気体)は 0
};

// 床の割り算(b > 0。0 方向の切り捨ての FxDivS64 と違い、負の数も小さい方へ)。負は |a| + b − 1 の足し算で(64bit の引き算と比べを並べない)
FX_FN int64_t PlFloorDivS64(int64_t a, uint64_t b) {
    const uint64_t magnitude = FxAbsU64(a);
    if (a >= 0)
        return (int64_t)(magnitude / b);

    return -(int64_t)((magnitude + b - 1) / b);
}

FX_FN bool PlIsGas(RxSpecies entry) {
    return (entry.flags & RX_SPECIES_GAS) != 0;
}

// 体積(pL)→ 高さ(Q16)。セルの体積 = HC_CELL_VOLUME_MEGA_PL × HC_MEGA(32bit の定数の積・割り算は 2 回。T-1078)
FX_FN uint32_t PlHeightQ16(uint64_t condensedVolume) {
    const uint64_t megaPl = condensedVolume / HC_MEGA;
    if (megaPl >= HC_CELL_VOLUME_MEGA_PL)
        return PL_FULL_Q16;

    const uint32_t height = (uint32_t)((megaPl * (uint64_t)PL_FULL_Q16) / HC_CELL_VOLUME_MEGA_PL);

    return height > PL_MIN_HEIGHT_Q16 ? height : PL_MIN_HEIGHT_Q16;
}

template <typename Table, typename Cell>
FX_FN PlLayer PlLayerOf(Table table, Cell cell) {
    PlLayer layer;
    layer.condensedVolume = 0;
    layer.condensedAmount = 0;
    layer.gasAmount = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        const RxSpecies entry = table.Species(RX_SPECIES_AT(cell, i));
        const uint64_t amount = RX_AMOUNT_AT(cell, i);
        if (PlIsGas(entry)) {
            layer.gasAmount += amount;
            continue;
        }

        layer.condensedAmount += amount;
        layer.condensedVolume += amount * (uint64_t)entry.molarVolume;
    }

    layer.heightQ16 = PlHeightQ16(layer.condensedVolume);
    const bool both = layer.condensedAmount != 0 && layer.gasAmount != 0;
    layer.mixed = both && layer.heightQ16 < PL_FULL_Q16 ? 1u : 0u;

    return layer;
}

// --- 2 つの部分の熱 ---------------------------------------------------------------------------------
struct PlParts {
    RxThermal condensed;             // 層(Ec・凝縮相の物質)
    RxThermal gas;                   // 気体(E − Ec・気体の物質)
    uint64_t condensedConductivity;  // 層の物質そのものの伝導率(mW/(m·K)。体積の重み。体積が無ければ量の平均)
    uint64_t gasConductivity;        // 気体の伝導率(量の平均 × √(T ÷ 室温)。D-639)
    int64_t condensedChemical;       // Σ 凝縮相の量 × H0(µJ)
    int64_t gasChemical;
};

template <typename Table, typename Cell>
FX_FN PlParts PlPartsOf(Table table, Cell cell, int64_t condensedEnergy) {
    int64_t chemical[2];
    uint64_t capacity[2];
    chemical[0] = 0;
    chemical[1] = 0;
    capacity[0] = 0;
    capacity[1] = 0;
    HcConductivitySums sums = HcMakeConductivitySums();
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        const RxSpecies entry = table.Species(RX_SPECIES_AT(cell, i));
        const uint64_t amount = RX_AMOUNT_AT(cell, i);
        const uint32_t part = PlIsGas(entry) ? 1u : 0u;
        chemical[part] += RxMulS64Checked((int64_t)amount, entry.h0);
        capacity[part] += amount * (uint64_t)entry.heatCapacity;
        sums = HcAddConductivity(sums, entry, amount);
    }

    PlParts parts;
    parts.condensedChemical = chemical[0];
    parts.gasChemical = chemical[1];
    parts.condensed = RxThermalFromSums(condensedEnergy, chemical[0], capacity[0]);
    parts.gas = RxThermalFromSums(cell.energy - condensedEnergy, chemical[1], capacity[1]);

    // --- 層の伝導率: 体積を持つ凝縮相の体積の重み(場所を取らない物質だけなら量の平均)---
    parts.condensedConductivity = 0;
    if (sums.condensedVolume != 0)
        parts.condensedConductivity = FxDivU128By64(sums.condensedWeighted, sums.condensedVolume).quotient;
    else if (sums.looseAmount != 0)
        parts.condensedConductivity = FxDivU128By64(sums.looseWeighted, sums.looseAmount).quotient;

    // --- 気体の伝導率(気体の温度で)---
    parts.gasConductivity = 0;
    if (sums.gasAmount != 0) {
        const HcGasScale scale = HcGasScaleAt(HcCacheTemperature(parts.gas));
        parts.gasConductivity = FxDivU128By64(HcMulU128ByQ16(sums.gasWeighted, scale.conductionQ16), sums.gasAmount)
                                    .quotient;
    }

    return parts;
}

// 混ざったセルになった時の Ec: 2 つの部分の温度が揃う配り(熱容量の重み。D-674 と同じ考え)。
// Ec = 凝縮相の化学のエネルギー + 熱 × C_凝縮 ÷ C(mJ に切り捨て)。気体の熱も負にならない
template <typename Table, typename Cell>
FX_FN int64_t PlEvenCondensedEnergy(Table table, Cell cell) {
    int64_t chemical = 0;
    uint64_t condensedCapacity = 0;
    uint64_t capacity = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        const RxSpecies entry = table.Species(RX_SPECIES_AT(cell, i));
        const uint64_t amount = RX_AMOUNT_AT(cell, i);
        capacity += amount * (uint64_t)entry.heatCapacity;
        if (PlIsGas(entry))
            continue;

        chemical += RxMulS64Checked((int64_t)amount, entry.h0);
        condensedCapacity += amount * (uint64_t)entry.heatCapacity;
    }

    const RxThermal whole = RxComputeThermal(table, cell);
    const uint64_t heatShare = capacity == 0 || whole.heat <= 0
                                   ? 0
                                   : (uint64_t)FxMulDivS64(whole.heat, condensedCapacity, capacity);

    // 層の熱(Ec × 1000 − 化学)が 0〜heatShare に入るように: Ec = ceil(化学 ÷ 1000) + floor((heatShare − 余り) ÷ 1000)。
    // 余り(0〜999)は 32bit で比べ、64bit の差を 0 で止めない(CLAUDE.md §4)
    const int64_t chemicalMj = -PlFloorDivS64(-chemical, RX_MICROJOULES_PER_MILLIJOULE);
    const uint32_t slack = (uint32_t)(chemicalMj * (int64_t)RX_MICROJOULES_PER_MILLIJOULE - chemical);
    const uint64_t shareMj = heatShare / RX_MICROJOULES_PER_MILLIJOULE;
    const uint32_t shareRest = (uint32_t)(heatShare % RX_MICROJOULES_PER_MILLIJOULE);
    const uint64_t borrow = shareRest < slack ? 1u : 0u;
    const uint64_t extra = shareMj == 0 ? 0 : shareMj - borrow;

    return chemicalMj + (int64_t)extra;
}

// --- 面の側 ---------------------------------------------------------------------------------------
// 面の向き(heat_conduction の 6 面と同じ並び: −x・+x・−y・+y・−z・+z)。重さの向きは −y
FX_CONST uint32_t PL_FACE_BOTTOM = 2;
FX_CONST uint32_t PL_FACE_TOP = 3;

FX_FN uint32_t PlClampU32(uint64_t value) {
    return value < HC_UINT32_MAX ? (uint32_t)value : (uint32_t)HC_UINT32_MAX;
}

// 部分の面の片側(heat_conduction の HcThermalCache と同じ形)。areaQ16 = 面のうちこの部分が受け持つ割合
// (上限は熱容量の 1/8 × 割合。部分の面の上限の和が 7/8 以下になる)
FX_FN HcThermalCache PlSide(RxThermal part, uint64_t conductance, uint32_t areaQ16) {
    HcThermalCache side;
    side.temperature = HcCacheTemperature(part);
    side.conductance = PlClampU32(conductance);
    const uint64_t byConductance = (uint64_t)side.conductance * HC_LIMIT_PER_CONDUCTANCE;
    const uint64_t byCapacity = (((part.heatCapacity >> HC_CAPACITY_SHIFT) * HC_LIMIT_PER_CAPACITY) >> 16) *
                                (uint64_t)areaQ16;
    side.faceLimit = byConductance < byCapacity ? byConductance : byCapacity;

    return side;
}

// 層の下の面の片側(層 ⇄ 接する固体。D-665): 伝導率 × 2^15 ÷ 道の高さ。道の高さ = 層の中心から下の面までの 2 倍(既定は層の高さ)。
// 相が変わっている層(溶ける氷)は温度が一様で、熱は接する面で吸われるので本当の道はもっと短い(T-1218 の研究。試験は PL_MIN_HEIGHT_Q16 で上の端を測る)
FX_FN HcThermalCache PlBottomSide(PlParts parts, uint32_t pathHeightQ16) {
    const uint64_t conductance = (parts.condensedConductivity << PL_HALF_SHIFT) / (uint64_t)pathHeightQ16;

    return PlSide(parts.condensed, conductance, PL_FULL_Q16);
}

// 上の面の片側(気体 ⇄ 上のセル): 伝導率 × 2^15 ÷ (1 − 高さ)
FX_FN HcThermalCache PlTopSide(PlParts parts, PlLayer layer) {
    const uint32_t gasHeight = PL_FULL_Q16 - layer.heightQ16;
    const uint64_t conductance = (parts.gasConductivity << PL_HALF_SHIFT) / (uint64_t)gasHeight;

    return PlSide(parts.gas, conductance, PL_FULL_Q16);
}

// 横の面のうち部分が受け持つ割合 areaQ16 の片側(面の中心までは 0.25 m のまま): 伝導率 × 0.5 × 割合
FX_FN HcThermalCache PlLateralSide(RxThermal part, uint64_t conductivity, uint32_t areaQ16) {
    const uint64_t conductance = (conductivity * (uint64_t)areaQ16) >> (PL_HALF_SHIFT + 2);

    return PlSide(part, conductance, areaQ16);
}

// --- 層の上の面(層 ⇄ 気体。セルの中の対の面)---------------------------------------------------------
// 2 つの半分の道の直列: G_層 = k_層 ÷ 高さ・G_気体 = k_気体 ÷ (1 − 高さ)(どちらも 伝導率 × 2^16 ÷ 高さ(Q16)。面積 0.25 m²)。
// 流れ(層 → 気体が正、mJ)は HcFaceFlow(両側の上限は自分の熱容量の 1/8)
FX_FN int64_t PlInnerFlow(PlParts parts, PlLayer layer) {
    const uint64_t condensedHalf = (parts.condensedConductivity << 16) / (uint64_t)layer.heightQ16;
    const uint64_t gasHalf = (parts.gasConductivity << 16) / (uint64_t)(PL_FULL_Q16 - layer.heightQ16);
    const uint64_t halves = condensedHalf + gasHalf;
    const uint64_t series = halves == 0 ? 0 : FxDivU128By64(FxMulU64Full(condensedHalf, gasHalf), halves).quotient;

    return HcFaceFlow(PlSide(parts.condensed, series, PL_FULL_Q16), PlSide(parts.gas, series, PL_FULL_Q16));
}

// --- 反応: 凝縮相の項を持つ規則は層の温度で(接する面の反応。溶ける・沸く・露・炭が燃える)--------------------------
// 見るセル = セルのすべての物質 + エネルギー Ec + V(気体の物質を層の温度にした時のエネルギー)。気体の量は濃さとして読む。
// 反応の後、気体の項の出入りを気体の側へ: 生まれた気体は層の温度(後)で、消えた気体は気体の温度で持ち出す(PlGasEnergyAfter)。
// 気体だけの規則(気体の燃焼)は気体だけのセル(E − Ec)で回す(規則の振り分けは T-1218。今は呼ぶ側が場面で分ける)
template <typename Table, typename Cell>
FX_FN int64_t PlGasEnergyAt(Table table, Cell cell, uint32_t temperatureMk) {
    int64_t energy = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        const RxSpecies entry = table.Species(RX_SPECIES_AT(cell, i));
        if (!PlIsGas(entry))
            continue;

        const uint64_t amount = RX_AMOUNT_AT(cell, i);
        const int64_t sensible = FxMulDivS64((int64_t)(amount * (uint64_t)entry.heatCapacity), temperatureMk,
                                             RX_MILLIKELVIN_NUMERATOR);
        energy += RxMulS64Checked((int64_t)amount, entry.h0) + sensible;
    }

    return energy;  // µJ
}

// 見るセルのエネルギー(mJ)= Ec + floor(V ÷ 1000)
template <typename Table, typename Cell>
FX_FN int64_t PlCondensedViewEnergy(Table table, Cell cell, int64_t condensedEnergy, uint32_t condensedTemperatureMk) {
    return condensedEnergy +
           PlFloorDivS64(PlGasEnergyAt(table, cell, condensedTemperatureMk), RX_MICROJOULES_PER_MILLIJOULE);
}

// 反応の後の気体のエネルギー(mJ): 前の気体のエネルギー + Σ 生まれた気体 × h(層の温度・後) − Σ 消えた気体 × h(気体の温度)。
// Ec(後)= セルのエネルギー − これ(和はビット単位で保つ)
template <typename Table, typename Cell>
FX_FN int64_t PlGasEnergyAfter(Table table, Cell before, Cell after, int64_t gasEnergyBefore, uint32_t condensedAfterMk,
                               uint32_t gasTemperatureMk) {
    int64_t change = 0;  // µJ
    for (uint32_t i = 0; i < after.speciesCount; ++i) {
        const uint32_t id = RX_SPECIES_AT(after, i);
        const RxSpecies entry = table.Species(id);
        if (!PlIsGas(entry))
            continue;

        const uint32_t slot = RxFindSlot(before, id);
        const uint64_t previous = slot < before.speciesCount ? RX_AMOUNT_AT(before, slot) : 0;
        const uint64_t now = RX_AMOUNT_AT(after, i);
        if (now == previous)
            continue;

        const bool grew = now > previous;
        const uint64_t delta = grew ? now - previous : previous - now;
        const uint32_t temperature = grew ? condensedAfterMk : gasTemperatureMk;
        const int64_t sensible = FxMulDivS64((int64_t)(delta * (uint64_t)entry.heatCapacity), temperature,
                                             RX_MILLIKELVIN_NUMERATOR);
        const int64_t enthalpy = RxMulS64Checked((int64_t)delta, entry.h0) + sensible;
        change += grew ? enthalpy : -enthalpy;
    }

    // 消えた気体(後のセルに無い物質)
    for (uint32_t i = 0; i < before.speciesCount; ++i) {
        const uint32_t id = RX_SPECIES_AT(before, i);
        const RxSpecies entry = table.Species(id);
        if (!PlIsGas(entry) || RxFindSlot(after, id) < after.speciesCount)
            continue;

        const uint64_t delta = RX_AMOUNT_AT(before, i);
        const int64_t sensible = FxMulDivS64((int64_t)(delta * (uint64_t)entry.heatCapacity), gasTemperatureMk,
                                             RX_MILLIKELVIN_NUMERATOR);
        change -= RxMulS64Checked((int64_t)delta, entry.h0) + sensible;
    }

    return gasEnergyBefore + PlFloorDivS64(change, RX_MICROJOULES_PER_MILLIJOULE);
}

// --- セルの段(世界の熱・相変化の段が呼ぶ形。T-1211)--------------------------------------------------------------
// 世界のセルが持つのは、セル(和)と Ec(混ざったセルの時だけ意味がある。混ざらないセルは 0 = 欄の頁が無いのと同じ)。
// 混ざっているかはセルの中身から決まる(PlLayerOf の mixed。面の流れは量を変えないので刻みの中で変わらない)ので、別の印は持たない。
// 量が変わる段(反応・運ぶ・細かく/粗くする)の後は PlRemixCondensedEnergy で混ざり方の変わり目を直す(ADR-0486 の 6)。
// 1 刻みの順(phase_layer_test の列・PlAdvanceLoneCell・世界の段で同じ): 面の流れと層の上の面(刻みの初めの値から)→ 反応 → 混ざり方。

// 層の上の面の流れ(層 → 気体が正、mJ。混ざらないセルは 0)。刻みの初めの値から(面の流れと同じ Jacobi)
template <typename Table, typename Cell>
FX_FN int64_t PlInnerFlowOf(Table table, Cell cell, int64_t condensedEnergy) {
    const PlLayer layer = PlLayerOf(table, cell);
    if (layer.mixed == 0)
        return 0;

    return PlInnerFlow(PlPartsOf(table, cell, condensedEnergy), layer);
}

// 反応の前の見るセル: 混ざったセルは層の温度で(ADR-0486 の 5。PlCondensedViewEnergy)、混ざらないセルはセルそのもの
template <typename Cell>
struct PlReactViewOf {
    Cell view;                  // 反応の核(RxStepCellWait など)に渡すセル
    int64_t gasEnergyBefore;    // mJ(E − Ec。混ざらないセルは 0)
    uint32_t gasTemperatureMk;  // 気体の部分の温度(消えた気体の h)
    uint32_t mixed;
};

template <typename Table, typename Cell>
FX_FN PlReactViewOf<Cell> PlBeginReact(Table table, Cell cell, int64_t condensedEnergy) {
    PlReactViewOf<Cell> begin;
    begin.view = cell;
    begin.gasEnergyBefore = 0;
    begin.gasTemperatureMk = 0;
    begin.mixed = PlLayerOf(table, cell).mixed;
    if (begin.mixed == 0)
        return begin;

    const PlParts parts = PlPartsOf(table, cell, condensedEnergy);
    const uint32_t condensedMk = HcCacheTemperature(parts.condensed);
    begin.view.energy = PlCondensedViewEnergy(table, cell, condensedEnergy, condensedMk);
    begin.gasEnergyBefore = cell.energy - condensedEnergy;
    begin.gasTemperatureMk = HcCacheTemperature(parts.gas);

    return begin;
}

// Σ 凝縮相の量 × H0(µJ)
template <typename Table, typename Cell>
FX_FN int64_t PlCondensedChemical(Table table, Cell cell) {
    int64_t chemical = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        const RxSpecies entry = table.Species(RX_SPECIES_AT(cell, i));
        if (!PlIsGas(entry))
            chemical += RxMulS64Checked((int64_t)RX_AMOUNT_AT(cell, i), entry.h0);
    }

    return chemical;
}

// value と lowest(mJ)の大きい方(差 < 2^63)。64bit の引き算と同じ値どうしの比べ・選ぶを並べない(CLAUDE.md §4)ので、
// 差を 32bit の 2 つに分けて借りを手で回し、差の一番上のビットで負を見分けて lowest + max(差, 0) を返す(state_field の SfOpenVolume と同じ形)
FX_FN int64_t PlAtLeastS64(int64_t value, int64_t lowest) {
    const uint64_t a = (uint64_t)value;
    const uint64_t b = (uint64_t)lowest;
    const uint32_t aLow = (uint32_t)(a & 0xFFFFFFFFu);
    const uint32_t bLow = (uint32_t)(b & 0xFFFFFFFFu);
    const uint32_t borrow = aLow < bLow ? 1u : 0u;
    const uint32_t low = aLow - bLow;
    const uint32_t high = (uint32_t)(a >> 32) - (uint32_t)(b >> 32) - borrow;
    if ((high & 0x80000000u) != 0u)
        return lowest;

    return lowest + (int64_t)(((uint64_t)high << 32) | (uint64_t)low);
}

// 反応の後の Ec(混ざり方を直す前。混ざらないセルは 0)。reacted = 見るセルを反応の核で進めたセル・reactedThermal = その熱。
// 呼ぶ側は reacted のエネルギーをセルのエネルギー cellEnergy に戻す(反応は量だけを変え、和は変えない)。
// 層の熱は負にしない(ADR-0497 の 5): 見るセルは気体の物質を層の温度で数えるので、吸熱の規則が使える熱(reaction.hlsli の
// RX_ENDOTHERMIC_HEAT_SHIFT)は気体の熱容量の分まで大きい。薄い層(炭・木の層の熱分解・Boudouard)ではそれが層の熱を超えるので、
// 足りない分は気体の部分が出す(Ec = 層の化学のエネルギーの切り上げ = 層の熱 0〜1 mJ。和は変わらない)
template <typename Table, typename Cell>
FX_FN int64_t PlEndReact(Table table, PlReactViewOf<Cell> begin, Cell reacted, RxThermal reactedThermal,
                         int64_t cellEnergy) {
    if (begin.mixed == 0)
        return 0;

    const int64_t gasAfter = PlGasEnergyAfter(table, begin.view, reacted, begin.gasEnergyBefore,
                                              HcCacheTemperature(reactedThermal), begin.gasTemperatureMk);
    const int64_t lowest = -PlFloorDivS64(-PlCondensedChemical(table, reacted), RX_MICROJOULES_PER_MILLIJOULE);

    return PlAtLeastS64(cellEnergy - gasAfter, lowest);
}

// 混ざり方の変わり目(ADR-0486 の 6): 混ざったら 2 つの温度が揃う配り・混ざらなくなったら Ec を捨てる(0。和は変わらない)。
// before = 量が変わる前のセル・after = 後のセル(エネルギーは和)
template <typename Table, typename Cell>
FX_FN int64_t PlRemixCondensedEnergy(Table table, Cell before, Cell after, int64_t condensedEnergy) {
    if (PlLayerOf(table, after).mixed == 0)
        return 0;

    if (PlLayerOf(table, before).mixed == 0)
        return PlEvenCondensedEnergy(table, after);

    return condensedEnergy;
}

// --- 1 セルだけの刻み(GPU と CPU のビット一致の試験。shaders/sim/phase_layer_cells.hlsl・tests/gpu_phase_layer_test.cpp)---
// 層の上の面 → 反応(待ちの丸め。RxStepCellWait)→ 混ざり方、を 1 刻みずつ。面の流れは無い(セルの外と熱をやり取りしない)。
// セルか Ec が変わった刻みを changedTick にする(層の温度が変われば待ちを引き直す。世界ではブロックが変わったのと同じ)
struct PlLoneCell {
    RxCell cell;
    int64_t condensedEnergy;
    uint64_t changedTick;
};

FX_FN PlLoneCell PlMakeLoneCell(RxCell cell, int64_t condensedEnergy, uint64_t changedTick) {
    PlLoneCell lone;
    lone.cell = cell;
    lone.condensedEnergy = condensedEnergy;
    lone.changedTick = changedTick;

    return lone;
}

template <typename Table>
FX_FN PlLoneCell PlStepLoneCell(Table table, PlLoneCell lone, uint64_t worldSeed, uint64_t tick, uint64_t cellId) {
    const int64_t condensedEnergy = lone.condensedEnergy - PlInnerFlowOf(table, lone.cell, lone.condensedEnergy);
    const PlReactViewOf<RxCell> begin = PlBeginReact(table, lone.cell, condensedEnergy);
    const RxWaitStep step = RxStepCellWait(table, begin.view, worldSeed, tick, lone.changedTick, cellId);
    RxCell after = step.cell;
    after.energy = lone.cell.energy;
    const int64_t reacted = PlEndReact(table, begin, step.cell, step.thermal, lone.cell.energy);
    const int64_t remixed = PlRemixCondensedEnergy(table, lone.cell, after, reacted);

    PlLoneCell next = lone;
    next.cell = after;
    next.condensedEnergy = remixed;
    if (!RxSameCell(after, lone.cell) || remixed != lone.condensedEnergy)
        next.changedTick = tick;

    return next;
}

template <typename Table>
FX_FN PlLoneCell PlAdvanceLoneCell(Table table, PlLoneCell lone, uint64_t worldSeed, uint64_t tickBegin,
                                   uint32_t tickCount, uint64_t cellId) {
    for (uint32_t i = 0; i < tickCount; ++i)
        lone = PlStepLoneCell(table, lone, worldSeed, tickBegin + i, cellId);

    return lone;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_PHASE_LAYER_HLSLI
