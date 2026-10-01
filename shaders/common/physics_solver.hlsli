// physics_solver.hlsli — 剛体の AVBD(08 §2 の 4。T-0016 研究 R-PHYS-1)の整数の部品: 拘束の行の値と力・物ごとの 6×6 の組み立て・
// λ と硬さの更新・速度と位置の仕上げ。手順は試作(tools/physics_lab/avbd_solver.cpp)と同じで、値は 04 §2 の物理の単位。
// HLSL と C++ の両方でコンパイルする。呼ぶ順番(接触の生成 → 引き継ぎ → 彩色 → 反復)は CPU では engine/src/sim/physics_world.cpp。
//
// 1 刻みの中は「刻みの初めで線形化した接触」で解く: C = C0′ + J·Δx(Δx = 刻みの初めからの変位と回転ベクトル)。
// C0′ は、法線の行で離れている(C0 > 0、先読みの接触)ときは C0 のまま、それ以外は C0 × (1 − α)。
// ただし本反復(α = 1)では gapSlop 以下の隙間は触れているとみなす(C0′ = 0。T-0091)。
#ifndef BICAMERAL_PHYSICS_SOLVER_HLSLI
#define BICAMERAL_PHYSICS_SOLVER_HLSLI

#include "physics_math.hlsli"
#include "units.hlsli"

PX_NAMESPACE_BEGIN

// --- 単位の換算 -------------------------------------------------------------------------------
FX_CONST uint32_t PX_DELTA_EXTRA_BITS = 12;          // 変位 Δp(2^-32 m)= 位置(2^-20 m)× 2^12
FX_CONST int32_t PX_SOLVE_GAIN_SHIFT = 24;           // H(2^-8)X(2^-32)= g(2^-16)× 2^24
FX_CONST uint32_t PX_FORCE_FROM_PENALTY_SHIFT = 24;  // k(2^-8 N/m)× C(2^-32 m)/ 2^24 = 力(2^-16 N)
FX_CONST uint32_t PX_ARM_TO_JACOBIAN_SHIFT = 20;     // 腕(2^-20 m)× 方向(Q30)/ 2^20 = r × b(2^-30 m)
FX_CONST uint32_t PX_GAP_TO_CONSTRAINT_SHIFT = 18;   // 隙間(2^-20 m)× 方向(Q30)/ 2^18 = C(2^-32 m)
FX_CONST int64_t PX_GRAVITY = 10283018;              // 9.80665 m/s²(2^-20 m/s²)

// --- パラメータ(試作で決めた値。T-0016 のチケット)--------------------------------------------
struct PxParameters {
    uint32_t iterations;
    uint32_t substeps;   // 1 刻みを何回に分けるか(分けるたびに接触を作り直す)
    int64_t gammaQ16;    // 刻みをまたいだ硬さの引き継ぎの割合
    int64_t penaltyMin;  // 硬さの下限(2^-8 N/m)
    int64_t penaltyMax;  // 硬さの上限(2^-8 N/m)
    uint32_t
        penaltyRatioShift;  // 硬さの上限その 2: 組の軽い方(動く物)の M/h² × 2^これ。6×6 の条件数を抑え、整数の解の桁を保証する
    int64_t betaPerKilogram;  // 硬さの増え方 β(N/m² を 1 kg あたり。組の両方に触れている物の中で一番重い質量を掛ける)
    int64_t startPenaltyQ16;  // 組の硬さの下限 = これ × 組の質量 / h²
    int64_t collisionMargin;  // この距離まで離れていても接触を作る(2^-20 m。これに相対速度 × h を足す)
    int64_t stickThreshold;   // 静止摩擦で接触点を保つ、接線のずれの上限(2^-32 m)

    // --- T-0091 ---
    int64_t gapSlop;              // 本反復で、これ以下の隙間は触れているとみなす(2^-32 m)
    int64_t proximityMatch;       // 特徴の番号が合わない点は、これ以内の前の点から λ を引き継ぐ(2^-20 m)
    uint32_t recollideIteration;  // この反復の前に、今の推定の姿勢で接触を探し直す(PX_NO_RECOLLIDE = しない)
    int64_t recollideMinMotion;   // 組の 1 刻みの動きがこれを超える時だけ探し直す(2^-32 m)
};

FX_CONST uint32_t PX_NO_RECOLLIDE = 0xFFFFFFFFu;

FX_FN PxParameters PxDefaultParameters() {
    PxParameters p;
    p.iterations = 10;
    p.substeps = 1;
    p.gammaQ16 = 64881;                               // 0.99
    p.penaltyMin = 256;                               // 1 N/m
    p.penaltyMax = FX_U64(0x0000E8D4u, 0xA5100000u);  // 1e12 N/m × 256
    p.penaltyRatioShift = 20;
    p.betaPerKilogram = 100000;
    p.startPenaltyQ16 = 65536;
    p.collisionMargin = 31457;    // 3 cm
    p.stickThreshold = 42949673;  // 1 cm
    p.gapSlop = 4294967;          // 1 mm
    p.proximityMatch = 20972;     // 2 cm
    p.recollideIteration = 5;
    p.recollideMinMotion = 21474836;  // 5 mm
    return p;
}

// 1 秒あたりの小刻みの数
FX_FN int64_t PxStepRate(PxParameters p) {
    return (int64_t)TICKS_PER_SECOND * (int64_t)p.substeps;
}

// --- 物の性質(場面から 1 回作る)----------------------------------------------------------------
// M/h²(2^-8 N/m)= mg × 10^-6 × rate² × 2^8
FX_FN int64_t PxMassOverStepSquared(uint64_t massMilligrams, int64_t rate) {
    return (int64_t)FxShiftRightU128(FxMulU64Full(massMilligrams, (uint64_t)(rate * rate * 256)), 0) / 1000000;
}

// 主慣性モーメント / h²(2^-8 N·m/rad)。直方体: I_x = m/3 (h_y² + h_z²)。
// mg × 10^-6 × (h² × 2^-40) / 3 × rate² × 2^8 = (mg × h² / 2^32) × rate² / (3 × 10^6)
FX_FN int64_t PxBoxInertiaOverStepSquared(uint64_t massMilligrams, int64_t halfA, int64_t halfB, int64_t rate) {
    const uint64_t squares = (uint64_t)(halfA * halfA + halfB * halfB);
    const uint64_t scaled = FxShiftRightU128(FxMulU64Full(massMilligrams, squares), 32);
    return (int64_t)FxShiftRightU128(FxMulU64Full(scaled, (uint64_t)(rate * rate)), 0) / 3000000;
}

// --- 拘束の行(接触点 1 つにつき法線・接線 2 本の 3 行)--------------------------------------------
struct PxRow {
    PxVec3 direction;  // Q1.30(A → B の向き)
    PxVec3 angularA;   // ∂C/∂θA(2^-30 m)
    PxVec3 angularB;   // ∂C/∂θB
    int64_t c0;        // 刻みの初めの C(2^-32 m)
    int64_t lambda;    // 2^-16 N
    int64_t penalty;   // 2^-8 N/m
};

// 刻みの初めの線形化: 腕(世界の向き、2^-20 m)と、2 点の隙間(B の点 − A の点、2^-20 m)から
FX_FN PxRow PxLinearizeRow(PxRow row, PxVec3 direction, PxVec3 armA, PxVec3 armB, PxVec3 gap) {
    row.direction = direction;
    row.c0 = PxDot(direction, gap, 0) >> PX_GAP_TO_CONSTRAINT_SHIFT;
    row.angularA = PxNegate(PxCross(armA, direction, PX_ARM_TO_JACOBIAN_SHIFT));
    row.angularB = PxCross(armB, direction, PX_ARM_TO_JACOBIAN_SHIFT);

    return row;
}

// C = C0′ + J·Δx(2^-32 m)。alphaQ16 = α × 2^16(postStabilize の本反復は 1、最後の 1 回は 0)。
// 本反復は gapSlop 以下の隙間を触れているとみなす: 仕上げ(α = 0)が少し押し出し過ぎて作った隙間に次の刻みで落ちて
// 速度が出る(毎刻み続いて滑り・崩れになる)のを防ぐ(T-0091)
FX_FN int64_t PxConstraintValue(PxRow row, bool isNormal, int64_t alphaQ16, int64_t gapSlop, PxVec3 deltaLinearA,
                                PxVec3 deltaAngularA, PxVec3 deltaLinearB, PxVec3 deltaAngularB) {
    const int64_t gapThreshold = alphaQ16 == 65536 ? gapSlop : 0;
    const bool isGap = isNormal && row.c0 > gapThreshold;
    const int64_t initial = isGap ? row.c0 : row.c0 - FxMulShiftS64(row.c0, alphaQ16, 16);
    const int64_t linear = PxDot(row.direction, PxSub(deltaLinearB, deltaLinearA), PX_UNIT_SHIFT);
    const int64_t angular = PxDot(row.angularA, deltaAngularA, PX_UNIT_SHIFT) +
                            PxDot(row.angularB, deltaAngularB, PX_UNIT_SHIFT);

    return initial + linear + angular;
}

// 力 f = clamp(k C + λ, 下限, 上限)(2^-16 N)
FX_FN int64_t PxRowForce(PxRow row, int64_t constraint, int64_t lower, int64_t upper) {
    return PxClamp(FxMulShiftS64(row.penalty, constraint, PX_FORCE_FROM_PENALTY_SHIFT) + row.lambda, lower, upper);
}

// --- 物ごとの 6×6 ----------------------------------------------------------------------------
struct PxBodySystem {
    PxMat6 lhs;  // 2^-8
    PxVec6 rhs;  // 2^-16
};

// 慣性の項: M/h² と M/h² (Δ − 慣性の目標)。inertiaWorld は回転の I/h²(世界の向き、2^-8)
FX_FN PxBodySystem PxBeginBodySystem(int64_t massOverH2, PxMat3 inertiaWorld, PxVec3 linearError, PxVec3 angularError) {
    PxBodySystem system = PX_ZERO(PxBodySystem);

    const PxVec3 angularForce = PxMulMat(inertiaWorld, angularError, PX_SOLVE_GAIN_SHIFT);
    for (uint32_t i = 0; i < 3; ++i) {
        system.lhs.m[i * 6 + i] = massOverH2;
        system.rhs.v[i] = FxMulShiftS64(massOverH2, PxGet(linearError, i), PX_SOLVE_GAIN_SHIFT);
        system.rhs.v[i + 3] = PxGet(angularForce, i);
        for (uint32_t j = 0; j < 3; ++j)
            system.lhs.m[(i + 3) * 6 + j + 3] = PxGet(PxMatRow(inertiaWorld, i), j);
    }

    return system;
}

// 拘束の行 1 つ: rhs += J f、lhs += k J Jᵀ。J = (向き(Q30), 角(2^-30 m))
FX_FN PxBodySystem PxAddRow(PxBodySystem system, PxVec3 linear, PxVec3 angular, int64_t penalty, int64_t force) {
    int64_t jacobian[6] = {linear.x, linear.y, linear.z, angular.x, angular.y, angular.z};
    for (uint32_t i = 0; i < 6; ++i) {
        system.rhs.v[i] += FxMulShiftS64(jacobian[i], force, PX_UNIT_SHIFT);
        const int64_t scaled = FxMulShiftS64(penalty, jacobian[i], PX_UNIT_SHIFT);
        for (uint32_t j = 0; j < 6; ++j)
            system.lhs.m[i * 6 + j] += FxMulShiftS64(scaled, jacobian[j], PX_UNIT_SHIFT);
    }

    return system;
}

PX_NAMESPACE_END

#endif  // BICAMERAL_PHYSICS_SOLVER_HLSLI
