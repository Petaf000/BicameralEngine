// gas_flow.hlsli — 気体の陽解法の流れの芯(導く値・面の力積と重さ・質量流束・MUSCL と乱数の丸め・pdV・音速の頭打ち。T-0719・土台の段 3 ①)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束。heat_conduction.hlsli・liquid_flow.hlsli と同じ形)。CPU リファレンス
// (engine/src/sim/gas_reference.cpp の 1 レベルの箱・engine/src/sim/probe_gas.cpp の仮の世界の glue)と GPU(shaders/sim/gas_step.hlsl・
// shaders/sim/probe_gas_tick.hlsl。GPU の欄の並びに合わせる薄い包みは shaders/common/gas_cell.hlsli)が同じ関数を呼ぶので、式の二重書きは無い。
// 違うのは回し方(CPU は面を順に散らす・GPU はセルが自分の 6 面を集める)と、値の置き場(CPU の配列・GPU のバッファ)だけ。
// 模型の形と約束は engine/src/sim/gas_reference.h(07 §2・ADR-0043)。
//
// 入力は 3 つに分ける(後で多重解像度の 1 段の気体〔T-0720〕がそのまま呼べるように。docs/design/20-transport-substrate.md §2.2・§2.3):
//   - セルの見立て GfTotals: セルの気体の成分から作る和(物質量・熱容量・化学のエネルギー・質量)と、そこから作る導く値 GfDerived
//     (質量・逆数・温度・圧力・p̃)。永続しない。土台のセルの見立て(MakeCellView。T-0713)ができたら、その気体の分をここへ渡す
//   - セルの欄: 持ち越す状態(エネルギー・運動量 ×3)。関数は値で受けて値で返す
//   - 見立ての値 GfParams: 環境・レベル・模型の値(係数・α・頭打ちの温度・重力・MUSCL・乱数の丸め・断熱)。係数はレベルの関数にできる
// 周りの決まり(箱・面の列挙・壁と開いた境界・帳簿・区画・出ていく上限の集め方)は呼ぶ側に残す(段 1 ④ の面の帳簿と当てる段へ移すのは後)。
// 出ていく上限の換算(GfEffectiveOutflow・GfLimitFraction・GfLimitMovedAmount)は式だけここに置く。欄が物質に付く式は液体と共通の
// follow_matter.hlsli の FmMomentumAfterMassChange(T-0718)。
// D-508 の力積の上限はまだ無い(入ったらここに足す)。
//
// 単位(レベル 0。04 §2): 物質量 µmol・質量 mg・エネルギー mJ(熱は µJ)・運動量 mg·2^-20 m/s・圧力 µPa・温度 mK・割合 Q32・空き Q16。
// 浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_GAS_FLOW_HLSLI
#define BICAMERAL_GAS_FLOW_HLSLI

#include "common/fixed.hlsli"
#include "common/follow_matter.hlsli"
#include "common/units.hlsli"

#ifdef __cplusplus
#define GF_NAMESPACE_BEGIN     \
    namespace bicameral::gas { \
        using namespace ::bicameral::fx;
#define GF_NAMESPACE_END }
#else
#define GF_NAMESPACE_BEGIN
#define GF_NAMESPACE_END
#endif

GF_NAMESPACE_BEGIN

// --- 物理の定数 ---

// 理想気体: p(µPa)= n(µmol)× T(mK)× R ÷ V × 1e-3 = n × T × 0.0665157(R = 8.314462618 J/(mol·K)・V = 0.125 m³)。Q32
FX_CONST uint64_t GF_PRESSURE_PER_AMOUNT_TEMPERATURE_Q32 = 285682760;
FX_CONST uint64_t GF_MICRO = 1000000;
FX_CONST uint64_t GF_ONE_Q32 = FX_U64(1u, 0u);
FX_CONST uint64_t GF_LOW_32_BITS = FX_U64(0u, 0xFFFFFFFFu);
FX_CONST uint64_t GF_MAX_U64 = FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu);

// 運動量の 2 乗を運動エネルギーへ: ½ m v² = P² × 2^-41 ÷ m(µJ。P は mg·2^-20 m/s、m は mg)。
// 2 乗を 41 ずらした 3 軸の和が 64bit に収まる運動量の上限 2^52(150 g で約 3 万 m/s)
FX_CONST uint32_t GF_KINETIC_SHIFT = 41;
FX_CONST uint64_t GF_MAX_MOMENTUM = FX_U64(0x100000u, 0u);

// 丸めの乱数の用途(FxHash64。R6)
FX_CONST uint32_t GF_ROUNDING_PURPOSE = 0x47610001u;

// 空き(Q16)の 1 と、圧力を読む空きの下限(1/256。塞がったセルに気体ができても圧力が 64bit を溢れない。T-0593)
FX_CONST uint32_t GF_OPEN_FULL = 65536;
FX_CONST uint32_t GF_OPEN_PRESSURE_FLOOR = 256;

// 移す塊の成分の割合を面へ延ばす方法(sim::GasReconstruction・GAS_GPU_RECONSTRUCTION_* と同じ値)
FX_CONST uint32_t GF_RECONSTRUCTION_UPWIND = 0;
FX_CONST uint32_t GF_RECONSTRUCTION_MINMOD = 1;
FX_CONST uint32_t GF_RECONSTRUCTION_MC = 2;

// --- 見立ての値(環境・レベル・模型。刻みの中は変わらない。sim::GasCoefficients と GasConfig の一部)---
struct GfParams {
    uint64_t centralFlow;         // (P_左 + P_右)× これ >> 52 = 質量の流れ(mg/小刻み)
    uint64_t pressureFlow;        // (p̃_左 − p̃_右)× これ >> 40 = 質量の流れ(mg/小刻み)
    uint64_t pressureImpulse;     // (p̃_左 + p̃_右)× これ >> 16 = 面の力積
    uint64_t gravityImpulse;      // (m − m_基準)× これ >> 16 = 重さの力積
    uint64_t hydrostatic;         // 質量(mg)× これ ÷ 10^4 = 1 セルの高さの層の重さの圧力(µPa)
    uint64_t gravityWork;         // 質量(mg)× これ ÷ 10^6 = 1 セル上がる重さの仕事(µJ)
    uint64_t soundScale;          // α の Q32
    uint32_t soundTemperature;    // 頭打ちの温度 T_頭(mK)
    uint32_t gravity;             // 0 / 1
    uint32_t reconstruction;      // GF_RECONSTRUCTION_*
    uint32_t stochasticRounding;  // 0 / 1
    uint32_t adiabatic;           // 1 = 断熱(T-0234)・0 = 等温
    uint32_t openness;            // 1 = セルの空きを読む箱(T-0593)
};

// --- セルの見立て(成分の和。足す順に依らない。04 R2)---
struct GfTotals {
    uint64_t amount;    // µmol
    uint64_t capacity;  // 物質量 × 比熱(nJ/K)
    int64_t chemical;   // 物質量 × 生成のエネルギー(µJ)
    uint64_t mass;      // mg
};

// セルから導く値(小刻みの初めに作る。GPU の GasGpuDerived と同じ並び)
struct GfDerived {
    uint64_t mass;           // mg
    uint64_t inverseMass;    // floor((2^64 − 1) ÷ mass)
    uint64_t inverseAmount;  // floor((2^64 − 1) ÷ 全物質量)
    uint32_t temperature;    // mK
    uint32_t unused;
    int64_t pressure;        // µPa(本物の値)
    int64_t scaledPressure;  // p̃ = α(p − p_基準)
};

// 縦の面の重さ(左右のセルの運動量 z から引く力積)
struct GfWeights {
    int64_t left;
    int64_t right;
};

// --- 整数の道具 ---

FX_FN uint64_t GfMulShiftU64(uint64_t a, uint64_t b, uint32_t shift) {
    return FxShiftRightU128(FxMulU64Full(a, b), shift);
}

// value × numerator ÷ denominator(0 の向きへ切り捨て)
FX_FN int64_t GfScaleTowardZero(int64_t value, uint64_t numerator, uint64_t denominator) {
    return FxApplySign(FxDivU128By64(FxMulU64Full(FxAbsU64(value), numerator), denominator).quotient, value < 0);
}

// a ÷ b を最も近い整数に(ちょうど半分は 0 から遠い側へ)。b > 0
FX_FN int64_t GfDivideNearest(int64_t a, uint64_t b) {
    FxU128 product;
    product.hi = 0;
    product.lo = FxAbsU64(a);
    const FxDivResult result = FxDivU128By64(product, b);
    const uint64_t quotient = result.quotient + (result.remainder * 2 >= b ? 1 : 0);

    return FxApplySign(quotient, a < 0);
}

// 床で割る(負の数も下へ)。divisor > 0
FX_FN int64_t GfDivideDown(int64_t value, uint64_t divisor) {
    FxU128 magnitude;
    magnitude.hi = 0;
    magnitude.lo = FxAbsU64(value);
    const FxDivResult result = FxDivU128By64(magnitude, divisor);
    if (value >= 0)
        return (int64_t)result.quotient;

    return -(int64_t)(result.quotient + (result.remainder != 0 ? 1 : 0));
}

FX_FN int64_t GfMinS64(int64_t a, int64_t b) {
    return a < b ? a : b;
}

// --- セルの見立て: 成分の和 ---

FX_FN GfTotals GfZeroTotals() {
    GfTotals totals;
    totals.amount = 0;
    totals.capacity = 0;
    totals.chemical = 0;
    totals.mass = 0;

    return totals;
}

// 成分 1 つを足す(比熱 mJ/(mol·K)・生成のエネルギー J/mol・モル質量 mg/µmol の Q32)
FX_FN GfTotals GfAddSpecies(GfTotals totals, uint64_t amount, uint64_t heatCapacity, int64_t formationEnergy,
                            uint64_t massPerAmountQ32) {
    GfTotals sum = totals;
    sum.amount += amount;
    sum.capacity += amount * heatCapacity;
    sum.chemical += (int64_t)amount * formationEnergy;
    sum.mass += GfMulShiftU64(amount, massPerAmountQ32, 32);

    return sum;
}

// --- セルから導く値(gas_reference.h の GasDerived)---

// 運動エネルギー(µJ)= Σ (P² >> 41)÷ m(軸ごとに切り捨ててから足す)。等温は 0(エネルギーに数えない)
FX_FN int64_t GfKineticEnergy(GfParams params, int64_t momentumX, int64_t momentumY, int64_t momentumZ, uint64_t mass) {
    if (params.adiabatic == 0 || mass == 0)
        return 0;

    const uint64_t magnitudeX = FxAbsU64(momentumX);
    const uint64_t magnitudeY = FxAbsU64(momentumY);
    const uint64_t magnitudeZ = FxAbsU64(momentumZ);
    FX_ASSERT(magnitudeX < GF_MAX_MOMENTUM && magnitudeY < GF_MAX_MOMENTUM && magnitudeZ < GF_MAX_MOMENTUM);
    uint64_t scaledSquare = FxShiftRightU128(FxMulU64Full(magnitudeX, magnitudeX), GF_KINETIC_SHIFT);
    scaledSquare += FxShiftRightU128(FxMulU64Full(magnitudeY, magnitudeY), GF_KINETIC_SHIFT);
    scaledSquare += FxShiftRightU128(FxMulU64Full(magnitudeZ, magnitudeZ), GF_KINETIC_SHIFT);

    return (int64_t)(scaledSquare / mass);
}

// 運動エネルギー µJ → mJ(最も近い整数。負にならない。仮の世界のセルへ書き戻すぶん)
FX_FN int64_t GfKineticMilliJoule(int64_t kineticMicroJoule) {
    return (int64_t)(((uint64_t)kineticMicroJoule + 500) / 1000);
}

// 温度(mK)= 熱(µJ)× 1e6 ÷ 熱容量(nJ/K)。熱 = エネルギー − 化学 − 運動(µJ)。熱が 0 以下なら 0
FX_FN uint32_t GfCellTemperature(int64_t energy, GfTotals totals, int64_t kineticMicroJoule) {
    const int64_t heat = energy * 1000 - totals.chemical - kineticMicroJoule;
    if (heat <= 0 || totals.capacity == 0)
        return 0;

    const uint64_t temperature = FxDivU128By64(FxMulU64Full((uint64_t)heat, GF_MICRO), totals.capacity).quotient;
    FX_ASSERT(temperature <= 0xFFFFFFFFu);

    return (uint32_t)temperature;
}

// 本物の圧力(µPa)= n × T × R ÷ V
FX_FN int64_t GfIdealPressure(uint64_t amount, uint32_t temperature) {
    const FxU128 amountTemperature = FxMulU64Full(amount, temperature);
    FX_ASSERT(amountTemperature.hi == 0);

    return (int64_t)GfMulShiftU64(amountTemperature.lo, GF_PRESSURE_PER_AMOUNT_TEMPERATURE_Q32, 32);
}

// p̃ = α(p − p_基準)。頭打ちの温度 T_頭 より熱いセルは α × T_頭 ÷ T(実効の音速を安定の上限に抑える。c_iso² ∝ T。T-0269)
FX_FN int64_t GfScaledPressure(GfParams params, int64_t pressure, int64_t referencePressure, uint32_t temperature) {
    int64_t scaled = FxMulShiftS64(pressure - referencePressure, (int64_t)params.soundScale, 32);
    if (temperature > params.soundTemperature)
        scaled = FxMulDivS64(scaled, params.soundTemperature, temperature);  // 積は 128bit(T-0676)

    return scaled;
}

// 導く値のうち基準の要らない分: 質量・逆数・温度・本物の圧力(p̃ は 0)。空き open(Q16)が一部のセルは本物の圧力を空きの体積で読む
// (n R T ÷ (セルの体積 × 空き)。空きは下限 GF_OPEN_PRESSURE_FLOOR で頭打ち。T-0593)
FX_FN GfDerived GfDeriveRaw(GfParams params, int64_t energy, int64_t momentumX, int64_t momentumY, int64_t momentumZ,
                            GfTotals totals, uint32_t open) {
    FX_ASSERT(totals.amount < CELL_GAS_AMOUNT_LIMIT_MICROMOLES);

    GfDerived derived;
    derived.mass = totals.mass;
    derived.inverseMass = totals.mass == 0 ? 0 : GF_MAX_U64 / totals.mass;
    derived.inverseAmount = totals.amount == 0 ? 0 : GF_MAX_U64 / totals.amount;
    derived.temperature = GfCellTemperature(energy, totals,
                                            GfKineticEnergy(params, momentumX, momentumY, momentumZ, totals.mass));
    derived.unused = 0;
    derived.pressure = GfIdealPressure(totals.amount, derived.temperature);
    if (open != GF_OPEN_FULL) {
        const uint32_t volume = open > GF_OPEN_PRESSURE_FLOOR ? open : GF_OPEN_PRESSURE_FLOOR;
        derived.pressure = GfScaleTowardZero(derived.pressure, GF_OPEN_FULL, volume);
    }

    derived.scaledPressure = 0;

    return derived;
}

// 導く値(GfDeriveRaw + p̃)。referencePressure = 層の基準の本物の圧力
FX_FN GfDerived GfDerive(GfParams params, int64_t energy, int64_t momentumX, int64_t momentumY, int64_t momentumZ,
                         GfTotals totals, int64_t referencePressure, uint32_t open) {
    GfDerived derived = GfDeriveRaw(params, energy, momentumX, momentumY, momentumZ, totals, open);
    derived.scaledPressure = GfScaledPressure(params, derived.pressure, referencePressure, derived.temperature);

    return derived;
}

// --- 面の力(圧力の力積と重さ。小刻みの初めの導く値だけから)---

// 基準との質量の差(mg)。空きを読む箱では基準 × 空き と比べ(固体の多いセルの静かな大気に浮力を作らない)、気体の無いセルは 0。
// 箱のセルの側だけ(開いた境界の外・壁は呼ぶ側で 0)
FX_FN int64_t GfExcess(GfParams params, uint64_t mass, uint64_t referenceMass, uint32_t open) {
    if (params.openness == 0)
        return (int64_t)mass - (int64_t)referenceMass;

    if (mass == 0)
        return 0;

    return (int64_t)mass - GfScaleTowardZero((int64_t)referenceMass, open, GF_OPEN_FULL);
}

// 質量の差 Δm(mg)が 1 セルの高さで作る静水圧の差の半分(µPa)= Δm × 層の重さの係数 ÷ 2
FX_FN int64_t GfHalfHydrostatic(GfParams params, int64_t excess) {
    return excess * (int64_t)params.hydrostatic / 20000;
}

// 圧力の拡散を起こす圧力の差: p̃_左 − p̃_右。縦の面では、両側の質量の差の平均が作る静水圧の差を引く
// (釣り合った重い層・軽い層を拡散が押し上げ続けると、重力と引き合って偽の循環が回る。07 §2.1)
FX_FN int64_t GfDrivingPressure(GfParams params, uint32_t axis, int64_t scaledLeft, int64_t scaledRight,
                                int64_t excessLeft, int64_t excessRight) {
    const int64_t difference = scaledLeft - scaledRight;
    if (axis != 2 || params.gravity == 0)
        return difference;

    return difference - GfHalfHydrostatic(params, excessLeft + excessRight);
}

// 縦の面の重さ: 面の両側の質量の差の平均 × g を、両側のセルが 1/4 ずつ受ける(壁の面は手前のセルだけ 1/2。07 §2.1)
FX_FN GfWeights GfFaceWeight(GfParams params, uint32_t axis, bool leftWall, bool rightWall, int64_t excessLeft,
                             int64_t excessRight) {
    GfWeights weights;
    weights.left = 0;
    weights.right = 0;
    if (axis != 2 || params.gravity == 0)
        return weights;

    const int64_t coefficient = (int64_t)params.gravityImpulse;
    if (leftWall) {
        weights.right = FxMulShiftS64(excessRight, coefficient, 17);
        return weights;
    }

    if (rightWall) {
        weights.left = FxMulShiftS64(excessLeft, coefficient, 17);
        return weights;
    }

    const int64_t quarter = FxMulShiftS64(excessLeft + excessRight, coefficient, 18);
    weights.left = quarter;
    weights.right = quarter;

    return weights;
}

// 塞がった所(壁・一部だけ開いた面の閉じた所)が 1 つの側のセルから受ける力積: そのセルの p̃ を鏡に映す。縦の面は
// 半セル分の静水圧で面まで延ばす(above = その側が面の上〔+ 側〕。床は高く・天井は低く。重さの釣り合いとそろえる)
FX_FN int64_t GfMirrorImpulse(GfParams params, uint32_t axis, int64_t scaledPressure, int64_t excess, bool above) {
    int64_t pressure = scaledPressure;
    if (axis == 2 && params.gravity != 0) {
        const int64_t hydrostatic = GfHalfHydrostatic(params, excess);
        pressure += above ? hydrostatic : -hydrostatic;
    }

    return FxMulShiftS64(pressure * 2, (int64_t)params.pressureImpulse, 16);
}

// 全面が開いた面の力積(両側の p̃ の平均)
FX_FN int64_t GfSharedImpulse(GfParams params, int64_t scaledLeft, int64_t scaledRight) {
    return FxMulShiftS64(scaledLeft + scaledRight, (int64_t)params.pressureImpulse, 16);
}

// 一部だけ開いた面(開き open)の 1 つの側が受ける力積: 開いた所は両側の平均(fullImpulse = 全面の時の力積)、塞がった所は
// その側の鏡(GfMirrorImpulse)。(HLSL では shared が予約語なので fullImpulse)
FX_FN int64_t GfPartialImpulse(GfParams params, uint32_t axis, int64_t scaledPressure, int64_t excess, bool above,
                               int64_t fullImpulse, uint32_t open) {
    const int64_t wall = GfMirrorImpulse(params, axis, scaledPressure, excess, above);

    return GfScaleTowardZero(fullImpulse, open, GF_OPEN_FULL) +
           GfScaleTowardZero(wall, GF_OPEN_FULL - open, GF_OPEN_FULL);
}

// --- 質量流束(力を当てた後の運動量 + 小刻みの初めの圧力。前進・後退の順)---

// 両側が箱の中か開いた境界の外の面の質量の流れ(mg/小刻み。左 → 右が正)。壁の面は呼ばない(0)。
// 中心の流れ(両側の運動量の和)+ 圧力の拡散(Rusanov の散逸を圧力の差にかける: 接触面を散らさず、静止大気では 0)。
// 一部だけ開いた面(T-0593)は、各側の運動量を「面の開き ÷ その側の空き」倍(孔の中の速さ × 開いた面積)、圧力の拡散を開きの倍。開き 0 は流さない
FX_FN int64_t GfFaceFlow(GfParams params, int64_t momentumLeft, int64_t momentumRight, uint32_t openLeft,
                         uint32_t openRight, int64_t driving) {
    const uint32_t open = openLeft < openRight ? openLeft : openRight;
    if (open == 0)
        return 0;

    int64_t momentumSum = momentumLeft + momentumRight;
    int64_t scaledDriving = driving;
    if (open != GF_OPEN_FULL) {
        momentumSum = GfScaleTowardZero(momentumLeft, open, openLeft) +
                      GfScaleTowardZero(momentumRight, open, openRight);
        scaledDriving = GfScaleTowardZero(driving, open, GF_OPEN_FULL);
    }

    const int64_t central = FxMulShiftS64(momentumSum, (int64_t)params.centralFlow, 52);
    const int64_t diffusion = FxMulShiftS64(scaledDriving, (int64_t)params.pressureFlow, 40);

    return central + diffusion;
}

// 風上のセルの中身のうち移す割合(Q32。抑える前)= |F| ÷ 風上の質量
FX_FN uint64_t GfRawFraction(int64_t flow, uint64_t donorInverseMass) {
    if (flow == 0)
        return 0;

    return GfMulShiftU64(FxAbsU64(flow), donorInverseMass, 32);
}

// --- 出ていく割合の上限(式だけ。集め方は呼ぶ側。当てる段ができたらそちらへ)---

// 出ていく割合の合計の、引く側の熱を負にしない換算(T-0646・T-0647・ADR-0286): 出ていく塊は熱の分け前に加えて押し出す仕事 n R T を
// 持って出るので、引く側の熱が残るのは 割合の合計 × (C + n R) ≤ C の時。合計 × (C + n R) ÷ C を返す。等温はそのまま。
// totals = 風上の小刻みの初めの成分の和
FX_FN uint64_t GfEffectiveOutflow(GfParams params, GfTotals totals, uint64_t outflow) {
    if (outflow == 0 || params.adiabatic == 0)
        return outflow;

    const uint64_t capacity = totals.capacity;
    if (capacity == 0)
        return outflow;

    const FxDivResult push = FxDivU128By64(FxMulU64Full(totals.amount, GAS_CONSTANT_NANOJOULES_PER_MOLE_KELVIN),
                                           GF_MICRO);
    const uint64_t pushCapacity = push.quotient + (push.remainder != 0 ? 1 : 0);  // n R(nJ/K。切り上げ)

    return FxDivU128By64(FxMulU64Full(outflow, capacity + pushCapacity), capacity).quotient;
}

// 換算した合計 outflow が 1 を超える風上の面の割合を比例して縮める
FX_FN uint64_t GfLimitFraction(uint64_t fraction, uint64_t outflow) {
    if (fraction == 0 || outflow <= GF_ONE_Q32)
        return fraction;

    return FxDivU128By64(FxMulU64Full(fraction, GF_ONE_Q32), outflow).quotient;
}

// 風上のセルの成分ごとの移す量の合計 total が中身 content を超えたら、面の移す量を比例して縮める(切り捨て)
FX_FN uint64_t GfLimitMovedAmount(uint64_t moved, uint64_t content, uint64_t total) {
    if (total <= content)
        return moved;

    return FxDivU128By64(FxMulU64Full(moved, content), total).quotient;
}

// --- 移す成分の物質量(MUSCL と乱数の丸め。G2。07 §2.2)---

// 制限した勾配(a = 風上 − その外側、b = 風下 − 風上)。符号が違うか 0 なら 0。minmod は絶対値の小さい方、
// MC(monotonized central)は minmod(2a, 2b, (a + b) ÷ 2)。どちらも TVD
FX_FN int64_t GfLimitedSlope(GfParams params, int64_t a, int64_t b) {
    if (a == 0 || b == 0 || (a > 0) != (b > 0))
        return 0;

    const int64_t sign = a > 0 ? 1 : -1;
    const int64_t smaller = GfMinS64(a * sign, b * sign);
    if (params.reconstruction == GF_RECONSTRUCTION_MINMOD)
        return smaller * sign;

    const int64_t central = ((a * sign) + (b * sign)) / 2;
    return GfMinS64(2 * smaller, central) * sign;
}

// 成分 s の割合(Q32)= 物質量 × 1 ÷ 全物質量(割り算は導く値の逆数 1 つ。ADR-0010)
FX_FN int64_t GfSpeciesRatio(uint64_t amount, uint64_t inverseAmount) {
    return (int64_t)GfMulShiftU64(amount, inverseAmount, 32);
}

// 風上のセルの割合から面へ延ばす差 ½(1 − ν)σ(Q32)。σ = 制限した勾配、ν = 出ていく割合(van Leer の 1 段の MUSCL)。
// 使えない並び(風上が箱の外・外側か風下が壁・1 次の風上)は呼ぶ側で 0
FX_FN int64_t GfHalfSlope(GfParams params, int64_t outerRatio, int64_t donorRatio, int64_t receiverRatio,
                          uint64_t fraction) {
    const int64_t slope = GfLimitedSlope(params, donorRatio - outerRatio, receiverRatio - donorRatio);

    return FxMulShiftS64(slope, (int64_t)(GF_ONE_Q32 - fraction), 33);
}

// 面で移す成分の物質量(Q32 の µmol)= 割合 × 風上の物質量 + 延ばした差 × 割合 × 風上の全物質量。負になる分は 0
// (64bit の「0 で止める引き算」を書かない: NVIDIA のドライバが誤って下ろした。T-0185。moved・correction < 2^63 なので符号付きの差で同じ値)
FX_FN uint64_t GfMovedAmountQ32(GfParams params, uint64_t sourceAmount, uint64_t sourceTotal, uint64_t fraction,
                                int64_t halfSlope) {
    const uint64_t moved = sourceAmount * fraction;
    if (params.reconstruction == GF_RECONSTRUCTION_UPWIND)
        return moved;

    const uint64_t correction = GfMulShiftU64(sourceTotal * fraction, FxAbsU64(halfSlope), 32);
    if (halfSlope >= 0)
        return moved + correction;

    const int64_t remaining = (int64_t)moved - (int64_t)correction;
    if (remaining <= 0)
        return 0;

    return (uint64_t)remaining;
}

// 面の丸めの乱数の頭(世界のシード, 小刻みの通し番号, 面の番号, 用途。R6。並べた順に依存しない)。成分は FxHashCombine(頭, 鍵)
FX_FN uint64_t GfFaceHash(uint64_t seed, uint64_t roundingTick, uint64_t faceId) {
    return FxHash64(seed, roundingTick, faceId, GF_ROUNDING_PURPOSE);
}

// Q32 の物質量を整数へ。端数は決定的な乱数と比べて丸める(期待値が端数どおり。切り捨てだと微量の成分が永久に動かない〔D-428〕)。
// 乱数の丸めを切った設定なら切り捨て(G1)
FX_FN uint64_t GfRoundAmount(GfParams params, uint64_t amountQ32, uint64_t hash) {
    const uint64_t whole = amountQ32 >> 32;
    if (params.stochasticRounding == 0)
        return whole;

    return whole + ((hash >> 32) < (amountQ32 & GF_LOW_32_BITS) ? 1 : 0);
}

// --- 移す塊のエネルギーと運動量(pdV・重さの仕事)---

// 移す塊の運動量(1 軸)= 風上の運動量 × 割合
FX_FN int64_t GfMovedMomentum(int64_t sourceMomentum, uint64_t fraction) {
    return FxMulShiftS64(sourceMomentum, (int64_t)fraction, 32);
}

// 移す塊のエネルギー(µJ。丸める前): 熱は風上の熱を熱容量の比で分け(塊の温度 = 風上の温度。偽の温度の縞を作らない)、
// 化学のエネルギーは移す物質量から。断熱(T-0234): + 押し出す仕事 pV = n R T(風上は膨張で冷え、風下は圧縮で温まる。07 §2.4)
// + 運動エネルギーの分け前(運動量と同じ割合)。sourceKinetic・sourceTemperature は風上の導く値から
FX_FN int64_t GfMovedEnergy(GfParams params, int64_t sourceEnergy, GfTotals sourceTotals, int64_t sourceKinetic,
                            uint32_t sourceTemperature, GfTotals movedTotals, uint64_t fraction) {
    const uint64_t sourceCapacity = sourceTotals.capacity;
    const int64_t movedChemical = movedTotals.chemical;
    if (sourceCapacity == 0)
        return movedChemical;

    const int64_t sourceHeat = sourceEnergy * 1000 - sourceTotals.chemical - sourceKinetic;
    const FxU128 numerator = FxMulU64Full(FxAbsU64(sourceHeat), movedTotals.capacity);
    const FxDivResult share = FxDivU128By64(numerator, sourceCapacity);
    const uint64_t heat = share.quotient + (share.remainder * 2 >= sourceCapacity ? 1 : 0);
    int64_t energy = FxApplySign(heat, sourceHeat < 0) + movedChemical;
    if (params.adiabatic == 0)
        return energy;

    // --- 押し出す仕事 n R T(µJ)= n × T × (圧力の係数)÷ 8(V = 0.125 m³)と運動エネルギーの分け前 ---
    const FxU128 amountTemperature = FxMulU64Full(movedTotals.amount, sourceTemperature);
    FX_ASSERT(amountTemperature.hi == 0);
    energy += (int64_t)GfMulShiftU64(amountTemperature.lo, GF_PRESSURE_PER_AMOUNT_TEMPERATURE_Q32, 35);
    energy += (int64_t)GfMulShiftU64((uint64_t)sourceKinetic, fraction, 32);

    return energy;
}

// 重さの仕事(µJ): 縦の面を上へ動く塊は m g Δx を失い、下へ動く塊は得る(位置のエネルギーは数えないので帳簿へ。断熱のときだけ)
FX_FN int64_t GfGravityWork(GfParams params, uint32_t axis, int64_t flow, uint64_t movedMass) {
    if (axis != 2 || params.gravity == 0 || params.adiabatic == 0)
        return 0;

    const int64_t work = (int64_t)(movedMass * params.gravityWork / GF_MICRO);

    return flow > 0 ? work : -work;
}

// --- 当てる段の上限の熱の列(陽解法の安全網。D-508・T-0787・ADR-0355)---
// 風上のセルは、自分の熱より多くの熱を塊で持ち出せない。出ていく面の「塊の熱のぶん」の和が「持ち出せる熱」を超えるセルは、
// 出ていく面の割合と成分の量を同じ倍率で縮める(当てる段の TxMakeLimit・TxFaceScale・TxScaleAmount。入る側は縮めない)。
// 熱が負にならないことを式の形で保証する(0 K に着くことはある。1 小刻みの強い膨張の離散の冷え過ぎ。T-0787 の測り)。断熱のときだけ

// 余裕(µJ): 面ごとの丸め(最も近い mJ)× 6 面
FX_CONST int64_t GF_HEAT_LIMIT_MARGIN_MICROJOULES = 6000;

// 塊の熱のぶん(µJ)= 塊のエネルギー(GfMovedEnergy)− 化学 − 運動の分け前(= 熱の分け前 + 押し出す仕事 n R T)。
// 0 以下の塊も 1 と数える(力で熱が一時的に負になったセルは負の熱を下流へ持ち出さない = 出ていく面を 0 に縮める)
FX_FN uint64_t GfMovedHeatPart(int64_t movedEnergy, int64_t movedChemical, int64_t sourceKinetic, uint64_t fraction) {
    const uint64_t kinetic = sourceKinetic > 0 ? (uint64_t)sourceKinetic : 0;
    const int64_t heat = movedEnergy - movedChemical - (int64_t)GfMulShiftU64(kinetic, fraction, 32);

    return heat > 0 ? (uint64_t)heat : 1;
}

// 風上が塊で持ち出せる熱(µJ)= 熱(力を当てた後)− 余裕。0 以下は 0
FX_FN uint64_t GfHeatRoom(int64_t heat) {
    const int64_t room = heat - GF_HEAT_LIMIT_MARGIN_MICROJOULES;

    return room > 0 ? (uint64_t)room : 0;
}

// --- 仮の世界のセルとの出し入れ(式だけ。区画と書き戻しは呼ぶ側)---

// セルのエネルギーのうち気体のぶん(mJ)= 熱 × 気体の熱容量 ÷ セルの熱容量 + 気体の化学(セルの温度を気体と凝縮相で同じと置く。
// 局所の熱の釣り合い)。セルが気体だけなら全部・気体が無ければ 0。chemical・capacity はセルの全部の成分、gas… は気体の成分の和
FX_FN int64_t GfGasEnergyPart(int64_t cellEnergy, int64_t chemical, uint64_t capacity, int64_t gasChemical,
                              uint64_t gasCapacity, bool hasGas, bool hasCondensed) {
    if (!hasGas)
        return 0;

    if (!hasCondensed || capacity == 0)
        return cellEnergy;

    const int64_t heat = (cellEnergy * 1000) - chemical;  // µJ
    const int64_t gasHeat = GfScaleTowardZero(heat, gasCapacity, capacity);

    return GfDivideDown(gasHeat + gasChemical, 1000);
}

// --- 粘り(静まる性質。T-0758・D-516)--------------------------------------------------------------------
// 気体の運動量を隣のセルと分け合って速さの差を減らす(運動量の拡散 = 粘り)。面ごとに 1 つの ΔP を片方から引き他方へ足すので運動量はビット単位で保存し、
// 一様な流れ(風)は減らない。減った運動のエネルギーは熱になる(面を通して運ぶ仕事 ΔP × (u_A + u_B) ÷ 2 を同じ向きへ動かすと、両側の熱が
// ΔP(u_A − u_B)÷ 2 ずつ増える = どちらも負にならない)。
// 動粘性 ν(mm²/s)= 世界の値「格子で解けない乱れの粘り」(WorldPhysics::gasEddyViscosityMm2PerS)+ 物質の粘り(viscosity_upa_s)÷ 密度。
// 面の係数 κ = ν Δt ÷ Δx²(Δt = 1/60 s・Δx = 0.5 m のセル)。安定の条件として 1/8 で頭打ち(6 面の陽の拡散。伝導の C/8 と同じ形)。

FX_CONST uint64_t GF_VISCOSITY_DIVISOR = 15000000;                       // 60 × 500²(mm²)
FX_CONST uint64_t GF_VISCOSITY_LIMIT_Q32 = FX_U64(0u, 0x20000000u);      // 1/8
FX_CONST uint64_t GF_VISCOSITY_LIMIT_MM2_PER_S = 1875000;                // κ が 1/8 になる ν(世界の値の上限)
FX_CONST uint64_t GF_CELL_VOLUME_MILLILITERS = 125000;                   // 0.125 m³(物質の粘りの密度を読む体積)
FX_CONST uint64_t GF_SPECIES_VISCOSITY_LIMIT = FX_U64(0u, 0x40000000u);  // 物質の粘りの上限(µPa·s。和が 64bit に収まる)

// 面の係数 κ(Q32)。ν は 2^31 mm²/s 未満(呼ぶ側が頭打ち)
FX_FN uint64_t GfViscosityQ32(uint64_t viscosityMm2PerS) {
    const uint64_t capped = viscosityMm2PerS < GF_VISCOSITY_LIMIT_MM2_PER_S ? viscosityMm2PerS
                                                                            : GF_VISCOSITY_LIMIT_MM2_PER_S;

    return (capped << 32) / GF_VISCOSITY_DIVISOR;
}

// セルの物質の粘りの動粘性(mm²/s)= μ(µPa·s。成分の物質量で重みを付けた平均)× 空きの体積 ÷ 質量。
// μ × 0.125 m³ × 空き ÷ (質量 mg × 10^-6)= μ × 125000 × 空き(Q16)÷ (質量 << 16)。質量 0 は 0
FX_FN uint64_t GfMolecularViscosity(uint64_t viscosityAmountSum, uint64_t amount, uint64_t mass, uint32_t open) {
    if (amount == 0 || mass == 0)
        return 0;

    const uint64_t mixture = viscosityAmountSum / amount;  // µPa·s(物質の値は GF_SPECIES_VISCOSITY_LIMIT まで)
    const FxU128 numerator = FxMulU64Full(mixture * GF_CELL_VOLUME_MILLILITERS, (uint64_t)open);
    const uint64_t viscosity = FxDivU128By64(numerator, mass << 16).quotient;

    return viscosity < GF_VISCOSITY_LIMIT_MM2_PER_S ? viscosity : GF_VISCOSITY_LIMIT_MM2_PER_S;
}

// 面の 1 軸の運動量の受け渡し(A → B が正。mg·2^-20 m/s)= κ × 換算質量 × (u_A − u_B)。速さが同じなら 0
FX_FN int64_t GfViscousMomentum(int64_t momentumA, uint64_t massA, int64_t momentumB, uint64_t massB,
                                uint64_t coefficientQ32) {
    if (massA == 0 || massB == 0 || coefficientQ32 == 0)
        return 0;

    const int64_t speedDifference = FxDivS64(momentumA, (int64_t)massA) - FxDivS64(momentumB, (int64_t)massB);
    const int64_t scaled = FxMulShiftS64(speedDifference, (int64_t)coefficientQ32, 32);
    const uint64_t reduced = FxDivU128By64(FxMulU64Full(massA, massB), massA + massB).quotient;

    return FxMulDivS64(scaled, reduced, 1);
}

// 面を通して運ぶ運動のエネルギー(A → B が正。mJ。断熱のときだけ)= ΔP × (u_A + u_B)÷ 2(µJ = P × u × 2^-40)を mJ へ(0 の向きへ切り捨て)
FX_FN int64_t GfViscousEnergy(GfParams params, int64_t moved, int64_t momentumA, uint64_t massA, int64_t momentumB,
                              uint64_t massB) {
    if (params.adiabatic == 0u || moved == 0)
        return 0;

    const int64_t speedSum = FxDivS64(momentumA, (int64_t)massA) + FxDivS64(momentumB, (int64_t)massB);
    const int64_t microJoules = FxMulShiftS64(moved, speedSum, GF_KINETIC_SHIFT);

    return microJoules / 1000;
}

GF_NAMESPACE_END

#endif  // BICAMERAL_GAS_FLOW_HLSLI
