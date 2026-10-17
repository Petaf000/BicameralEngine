// cell_view.hlsli — セルの見立て(運ぶ計算の共通の土台の段 1 ③。T-0713・ADR-0333 Proposed。runner の設計 substrate-decided §2.2)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU(engine/src/sim/cell_view.h の表)と GPU が同じ関数 MakeCellView を呼ぶ。
//
// 一つの芯: 刻みごとに 1 か所で、セル(成分 + エネルギー)と相の表 1 つから「流れが読む値」を全部作る。永続しない(保存点・ハッシュに入らない。
// 段の外でセルを変えても作り直しの後始末が要らない)。作る値:
//   - 相ごとの物質量・質量・体積・熱容量(気体 / 液体 / 固体。相は表の phase 1 本。ベイクの speciesPhase〔T-0710〕を写す)
//   - 体積の分け合い: セル = 固体 + 液体 + 残り(気体)。液体の空き・気体の空き・液体の入れる体積・液体の満ち具合
//     (sim/cell_volume の ShareCellVolume・LiquidOpenQ16・GasOpenQ16 とビット単位で同じ。T-0713 の試験)
//   - セル全体の熱(熱・熱容量・温度)と伝導の係数(multires_conduction.hlsli の MrCellThermal・MrFaceLimit と同じ値。
//     レベル 0 の面の係数は heat_conduction.hlsli の熱のキャッシュ HcThermalCache の faceLimit と同じ)
//   - 気体の圧力 p(セル全体の温度 × 気体の物質量 ÷ 気体の空きの体積)と基準の大気との差 p − p_基準
//   - 液体の質量(mg)と、空きが液体で満ちた時のセル全部の液体に直した質量(液体の柱の圧力の元。sim/probe_liquid の ReadState と同じ)
//
// レベルの単位(17 §1・土台 §3): レベル k の物質量は 8^-k µmol なので、物質量 × モル体積(pL)もレベル k の単位(8^-k pL)で数えると
// セルの体積はどのレベルでも同じ数 CV_CELL_VOLUME_LEVEL_UNITS(= レベル 0 の 0.125 m³)になる。空き・満ち具合・圧力はレベルによらない。
// レベルが効くのは面の係数(コンダクタンス × 4^k)と、体積を本当の pL で要る時(CvCellVolumePicoliters)だけ。
//
// 作る部分(T-0767): 種類が読む部分だけを作る(parts。CV_VIEW_*)。セル全体の熱と伝導の係数はいつも作る(相の表を読まない 1 回の和)。
// 相ごとの和・体積の分け合い・気体の圧力・液体の質量は要る種類だけ(割り算 5 つと相の表の読みが要らない種類は払わない)。
// 作らない欄は 0(読まない約束)。作った欄の値は parts によらず同じ(全部作った MakeCellView とビット一致。cell_view_test)。
// GPU では parts を定数で渡し、インラインの後で作らない部分が消える(レジスタも増えない)。
//
// まだ入れないもの(後のチケット): 運動のエネルギー(気体の運動量は登録の欄。T-0711)・下の支え・孔の詰まり・吸い上げ(隣と物質の欄が要る。段 9)。
// 浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_CELL_VIEW_HLSLI
#define BICAMERAL_CELL_VIEW_HLSLI

#include "multires_conduction.hlsli"

MR_NAMESPACE_BEGIN

// --- 相の表(物質ごと。添字 = 物質 ID)----------------------------------------------------------

FX_CONST uint32_t CV_PHASE_GAS = 0;     // 体積を取らない(圧力で決まる)
FX_CONST uint32_t CV_PHASE_LIQUID = 1;  // 流れる凝縮相
FX_CONST uint32_t CV_PHASE_SOLID = 2;   // 流れない凝縮相

// 物質 1 つの相の値。Table の約束に Phase(uint32_t id) を足す(Species と並ぶ。C++ は sim::CellViewTable)
struct CvSpeciesPhase {
    uint64_t molarVolume;  // mm³/mol(= モル質量 ÷ 密度。密度を書いていなければ 0 = 場所を取らない)
    uint32_t molarMass;    // mg/mol
    uint32_t phase;        // CV_PHASE_*
};

// --- 作る部分(ビットの和。CV_VIEW_THERMAL = セル全体の熱と伝導の係数だけ)---------------------------------

FX_CONST uint32_t CV_VIEW_THERMAL = 0;       // heat・heatCapacity・thermal・faceLimit(いつも作る)
FX_CONST uint32_t CV_VIEW_PHASES = 1;        // 相ごとの和・液体と気体の空き・液体の入れる体積・満ち具合
FX_CONST uint32_t CV_VIEW_GAS_PRESSURE = 2;  // 気体の圧力と基準との差(相ごとの和を含む)
FX_CONST uint32_t CV_VIEW_LIQUID_MASS = 4;   // 液体の質量(mg)と空きで直した質量(相ごとの和を含む)
FX_CONST uint32_t CV_VIEW_ALL = 7;

// 圧力・液体の質量は相ごとの和から作るので、どちらかを読むなら相ごとの和も作る
FX_FN uint32_t CvViewPartsClosure(uint32_t parts) {
    return parts != CV_VIEW_THERMAL ? (parts | CV_VIEW_PHASES) : parts;
}

// --- 体積の定数 ----------------------------------------------------------------------------------

FX_CONST uint32_t CV_OPEN_FULL = 65536;  // 割合の 1(Q16。sim::CELL_OPEN_FULL)

// セルの体積(レベル k の単位の pL。どのレベルでも同じ数)= レベル 0 の 0.5 m 角 = 0.125 m³ = 1.25e14 pL(sim::PROBE_CELL_VOLUME_PICOLITERS)
FX_CONST uint64_t CV_CELL_VOLUME_LEVEL_UNITS = FX_U64(0x71AFu, 0xD498D000u);

// 占める体積の頭打ち(pL。どのセルの体積よりずっと大きく、固体 + 液体の和が 64bit を溢れない。sim/cell_volume の VOLUME_CAP)
FX_CONST uint64_t CV_VOLUME_CAP = FX_U64(0x40000000u, 0u);

// 圧力を読む体積の下限(Q16。gas_reference.cpp の GAS_OPEN_PRESSURE_FLOOR)
FX_CONST uint32_t CV_OPEN_PRESSURE_FLOOR = 256;

// 理想気体: p(µPa)= n × T × R ÷ V。V はセルの体積(レベルの単位で同じ数)。Q32(gas_internal.h の PRESSURE_PER_AMOUNT_TEMPERATURE_Q32)
FX_CONST uint64_t CV_PRESSURE_PER_AMOUNT_TEMPERATURE_Q32 = 285682760;

FX_CONST uint64_t CV_NANOGRAMS_PER_MILLIGRAM = 1000000;

// レベル k のセルの本当の体積(pL)= 0.125 m³ × 8^-k(細かすぎて 1 pL 未満は 0・粗すぎて溢れる所は最大で止める)
FX_FN uint64_t CvCellVolumePicoliters(int32_t level) {
    if (level < 0)
        return MrSaturatingShiftLeft(CV_CELL_VOLUME_LEVEL_UNITS, MR_LEVEL_SHIFT * (uint32_t)(-level));

    const uint32_t shift = MR_LEVEL_SHIFT * (uint32_t)level;

    return shift >= 64 ? 0 : CV_CELL_VOLUME_LEVEL_UNITS >> shift;
}

// --- 構造体 --------------------------------------------------------------------------------------

// 1 つの相の和
struct CvPhaseSums {
    uint64_t amount;        // µmol(レベルの単位)
    uint64_t mass;          // ng(= µmol × mg/mol)
    uint64_t volume;        // pL(レベルの単位。気体は 0。CV_VOLUME_CAP で頭打ち)
    uint64_t heatCapacity;  // nJ/K
};

// 1 成分ずつ足す和(GPU の溢れを持つセルが、上限の無い形のセルを作らずに足す形。MrThermalSums と同じ考え)
struct CvViewSums {
    MrThermalSums thermal;  // 化学のエネルギー・熱容量・伝導率の重み(セル全体)
    CvPhaseSums gas;
    CvPhaseSums liquid;
    CvPhaseSums solid;
};

// セルの見立て(刻みごとに作り、永続しない)
struct CvCellView {
    // --- 相ごと ---
    CvPhaseSums gas;
    CvPhaseSums liquid;
    CvPhaseSums solid;

    // --- 体積の分け合い(固体 + 液体 + 残り = 気体)---
    uint32_t liquidOpen;  // 液体の空き: 固体が占めない割合(Q16。0 = 固体で満ちて液体は入れない)
    uint32_t gasOpen;     // 気体の空き: 固体と液体が占めない割合(Q16)
    uint64_t liquidRoom;  // 液体が入れる体積(pL。レベルの単位。セル − 固体)
    uint32_t liquidFill;  // 液体の満ち具合: min(液体, 入れる体積) ÷ 入れる体積(Q16。入れる体積が 0 なら 0)
    uint32_t padding;

    // --- セル全体の熱 ---
    int64_t heat;           // µJ(= エネルギー × 1000 − 化学)
    uint64_t heatCapacity;  // nJ/K
    MrThermal thermal;      // 温度・コンダクタンス・熱容量の上限(MrCellThermal と同じ)
    uint64_t faceLimit;     // このレベルの面の係数の上限(MrFaceLimit。レベル 0 は HcThermalCache::faceLimit と同じ)

    // --- 気体 ---
    int64_t gasPressure;           // µPa(気体の空きの体積で読む。空きは下限 CV_OPEN_PRESSURE_FLOOR)
    int64_t gasPressureDeviation;  // µPa(p − 基準の大気の圧力)

    // --- 液体 ---
    uint64_t liquidMass;            // mg
    uint64_t liquidEquivalentMass;  // mg(空きが液体で満ちた時の質量をセル全部に直したもの = 質量 ÷ 空き)
};

// --- 小さな関数 ----------------------------------------------------------------------------------

FX_FN CvPhaseSums CvMakePhaseSums() {
    CvPhaseSums sums;
    sums.amount = 0;
    sums.mass = 0;
    sums.volume = 0;
    sums.heatCapacity = 0;

    return sums;
}

// 1 成分を相の和に足す。体積は凝縮相だけ(気体は圧力で決まる)。頭打ちは sim/cell_volume の ShareCellVolume と同じ
FX_FN CvPhaseSums CvAddPhaseSpecies(CvPhaseSums sums, CvSpeciesPhase phase, uint32_t heatCapacity, uint64_t amount) {
    sums.amount += amount;
    sums.mass += amount * (uint64_t)phase.molarMass;
    sums.heatCapacity += amount * (uint64_t)heatCapacity;
    if (phase.phase == CV_PHASE_GAS)
        return sums;

    // µmol × mm³/mol(セルの器 2^36 µmol × モル体積 2^27 mm³/mol までは 64bit に収まる。超える分は頭打ち = セルより大きい)
    const FxU128 product = FxMulU64Full(amount, phase.molarVolume);
    const uint64_t volume = product.hi != 0 ? CV_VOLUME_CAP : MrMin64(product.lo, CV_VOLUME_CAP);
    sums.volume = MrMin64(sums.volume + volume, CV_VOLUME_CAP);

    return sums;
}

// 占める体積を除いた残りの割合(Q16。切り捨て = 空きを小さめに。占める分がセル以上なら 0)
FX_FN uint32_t CvRemainingQ16(uint64_t occupied, uint64_t cellVolume) {
    if (occupied >= cellVolume)
        return 0;

    const FxU128 product = FxMulU64Full(cellVolume - occupied, (uint64_t)CV_OPEN_FULL);

    return (uint32_t)FxDivU128By64(product, cellVolume).quotient;
}

// 気体の圧力(µPa)= n × T × R ÷ (V × 空き)。gas_reference.cpp の DeriveRaw・DeriveOpenCell と同じ式(T は渡した温度)
FX_FN int64_t CvGasPressure(uint64_t amount, uint32_t temperature, uint32_t open) {
    const FxU128 amountTemperature = FxMulU64Full(amount, (uint64_t)temperature);
    FX_ASSERT(amountTemperature.hi == 0);
    const uint64_t full = FxShiftRightU128(FxMulU64Full(amountTemperature.lo, CV_PRESSURE_PER_AMOUNT_TEMPERATURE_Q32),
                                           32);
    if (open >= CV_OPEN_FULL)
        return (int64_t)full;

    const uint32_t volume = open > CV_OPEN_PRESSURE_FLOOR ? open : CV_OPEN_PRESSURE_FLOOR;
    const FxU128 scaled = FxMulU64Full(full, (uint64_t)CV_OPEN_FULL);
    FX_ASSERT(scaled.hi < (uint64_t)volume);

    return (int64_t)FxDivU128By64(scaled, (uint64_t)volume).quotient;
}

// --- 見立てを作る --------------------------------------------------------------------------------

FX_FN CvViewSums CvMakeViewSums() {
    CvViewSums sums;
    sums.thermal = MrMakeThermalSums();
    sums.gas = CvMakePhaseSums();
    sums.liquid = CvMakePhaseSums();
    sums.solid = CvMakePhaseSums();

    return sums;
}

// 1 成分を足す(足す順に依らない。整数の和)。相ごとの和は parts が CV_VIEW_PHASES を含む時だけ(相の表を読まない)
template <typename Table>
FX_FN CvViewSums CvAddSpeciesParts(Table table, CvViewSums sums, uint32_t species, uint64_t amount, uint32_t parts) {
    sums.thermal = MrAddThermalSpecies(table, sums.thermal, species, amount);
    if ((CvViewPartsClosure(parts) & CV_VIEW_PHASES) == 0)
        return sums;

    const CvSpeciesPhase phase = table.Phase(species);
    const uint32_t heatCapacity = table.Species(species).heatCapacity;
    if (phase.phase == CV_PHASE_LIQUID)
        sums.liquid = CvAddPhaseSpecies(sums.liquid, phase, heatCapacity, amount);
    else if (phase.phase == CV_PHASE_SOLID)
        sums.solid = CvAddPhaseSpecies(sums.solid, phase, heatCapacity, amount);
    else
        sums.gas = CvAddPhaseSpecies(sums.gas, phase, heatCapacity, amount);

    return sums;
}

template <typename Table>
FX_FN CvViewSums CvAddSpecies(Table table, CvViewSums sums, uint32_t species, uint64_t amount) {
    return CvAddSpeciesParts(table, sums, species, amount, CV_VIEW_ALL);
}

// 液体の満ち具合(Q16)= min(液体, 入れる体積) ÷ 入れる体積(入れる体積が 0 なら 0)
FX_FN uint32_t CvLiquidFillQ16(uint64_t liquidVolume, uint64_t room) {
    if (room == 0)
        return 0;

    const uint64_t filled = MrMin64(liquidVolume, room);

    return (uint32_t)FxDivU128By64(FxMulU64Full(filled, (uint64_t)CV_OPEN_FULL), room).quotient;
}

// 和から見立て(parts の部分だけ。作らない欄は 0)。energy = セルのエネルギー(mJ。レベルの単位)・level = セルのレベル・
// referencePressure = このセルの基準の大気の圧力(µPa)
FX_FN CvCellView CvViewFromSumsParts(int64_t energy, FX_IN(CvViewSums) sums, int32_t level, int64_t referencePressure,
                                     uint32_t parts) {
    const uint32_t built = CvViewPartsClosure(parts);

    CvCellView view;
    view.gas = sums.gas;  // 相ごとの和を作らない時は CvMakeViewSums の 0 のまま
    view.liquid = sums.liquid;
    view.solid = sums.solid;
    view.liquidOpen = 0;
    view.gasOpen = 0;
    view.liquidRoom = 0;
    view.liquidFill = 0;
    view.padding = 0;
    view.gasPressure = 0;
    view.gasPressureDeviation = 0;
    view.liquidMass = 0;
    view.liquidEquivalentMass = 0;

    // --- セル全体の熱と伝導の係数(MrCellThermal・RxComputeThermal と同じ式。いつも作る)---
    view.heat = RxMulS64Checked(energy, (int64_t)RX_MICROJOULES_PER_MILLIJOULE) - sums.thermal.chemical;
    view.heatCapacity = sums.thermal.heatCapacity;
    view.thermal = MrThermalFromSums(energy, sums.thermal);
    view.faceLimit = MrFaceLimit(view.thermal, level);
    if ((built & CV_VIEW_PHASES) == 0)
        return view;

    // --- 体積の分け合い(セルの体積はレベルの単位で同じ数)---
    const uint64_t cellVolume = CV_CELL_VOLUME_LEVEL_UNITS;
    view.liquidOpen = CvRemainingQ16(view.solid.volume, cellVolume);
    view.gasOpen = CvRemainingQ16(view.solid.volume + view.liquid.volume, cellVolume);
    view.liquidRoom = cellVolume - MrMin64(view.solid.volume, cellVolume);
    view.liquidFill = CvLiquidFillQ16(view.liquid.volume, view.liquidRoom);

    // --- 気体の圧力 ---
    if ((built & CV_VIEW_GAS_PRESSURE) != 0) {
        view.gasPressure = CvGasPressure(view.gas.amount, view.thermal.temperature, view.gasOpen);
        view.gasPressureDeviation = view.gasPressure - referencePressure;
    }

    if ((built & CV_VIEW_LIQUID_MASS) == 0)
        return view;

    // --- 液体(空きで割って、セル全部が同じ割合で満ちた時の質量に直す。sim/probe_liquid の ReadState)---
    view.liquidMass = view.liquid.mass / CV_NANOGRAMS_PER_MILLIGRAM;
    const bool undivided = view.liquidOpen == CV_OPEN_FULL || view.liquidOpen == 0;
    view.liquidEquivalentMass = undivided ? view.liquidMass
                                          : FxDivU128By64(FxMulU64Full(view.liquidMass, (uint64_t)CV_OPEN_FULL),
                                                          view.liquidOpen)
                                                .quotient;

    return view;
}

FX_FN CvCellView CvViewFromSums(int64_t energy, CvViewSums sums, int32_t level, int64_t referencePressure) {
    return CvViewFromSumsParts(energy, sums, level, referencePressure, CV_VIEW_ALL);
}

// セルの見立ての parts の部分だけ(種類が読む部分。transport_apply.hlsli の世界が種類の ViewParts で呼ぶ)
template <typename Table, typename Cell>
FX_FN CvCellView MakeCellViewParts(Table table, FX_IN(Cell) cell, int32_t level, int64_t referencePressure,
                                   uint32_t parts) {
    CvViewSums sums = CvMakeViewSums();
    if ((CvViewPartsClosure(parts) & CV_VIEW_PHASES) == 0) {
        // 熱だけ: 熱の和の 1 回の回し(相の表を読まない。MrCellThermal と同じ手間)
        for (uint32_t i = 0; i < cell.speciesCount; ++i)
            sums.thermal = MrAddThermalSpecies(table, sums.thermal, cell.species[i], cell.amounts[i]);

        return CvViewFromSumsParts(cell.energy, sums, level, referencePressure, parts);
    }

    for (uint32_t i = 0; i < cell.speciesCount; ++i)
        sums = CvAddSpeciesParts(table, sums, cell.species[i], cell.amounts[i], parts);

    return CvViewFromSumsParts(cell.energy, sums, level, referencePressure, parts);
}

// セルの見立て(全部)。Table は Species(id)(RxSpecies)と Phase(id)(CvSpeciesPhase)を持つ。Cell は RxCell・C++ の RxWideCell
template <typename Table, typename Cell>
FX_FN CvCellView MakeCellView(Table table, FX_IN(Cell) cell, int32_t level, int64_t referencePressure) {
    return MakeCellViewParts(table, cell, level, referencePressure, CV_VIEW_ALL);
}

MR_NAMESPACE_END

#endif  // BICAMERAL_CELL_VIEW_HLSLI
