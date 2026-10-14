// physics_heat.hlsli — 物理で失った力学的エネルギーを世界の熱へ返す(T-0542・ADR-0242。19 §11・08 §4)。
// CPU の世界(engine/src/sim/physics_world.cpp)と GPU が同じ関数を呼ぶ(整数。和は順番に依存しない)。
//
// データの流れ:
//   小刻みごと(最後の本反復の λ が決まった後・速度を更新する前): 接触点ごとに「仕事の重み」= Σ 行 |λ| × |行の向きの相対の速さ|
//     (PxContactPointWork。2^-16 W)と、点の位置(A と B の点の真ん中。PxContactPointPosition)を熱の入口(PxHeatEntry)に積む
//   刻みの終わり: 返す元 = Σ 物の dissipatedEnergy − 返した量(PX_HEAT_UNITS_PER_MILLIJOULE × 返した mJ の累計)。
//     正なら mJ にして(PxHeatAvailableMilliJoules)、入口の重みの割合で分ける(PxHeatShare。切り捨て)→ 入口の milliJoules
//   世界(仮の世界 ProbeSim): 入口の位置のセルのエネルギーへ milliJoules を足す(probe_sim.hlsli の ProbeCellOfPhysicsPoint)。
// 帳簿(毎刻みビット単位): Σ dissipatedEnergy = PX_HEAT_UNITS_PER_MILLIJOULE × 返した mJ の累計 + 残り。
//   残り = mJ の端数と、返す元が負の時の「借り」(解き方が生んだエネルギー。世界を冷やさず、次の正の散逸から差し引く)。
// 返す元は世界全体で 1 つ(仮・ユーザー未確認。ADR-0242): どこへ返すかは接触点の仕事の重みが決め、全体の量だけを 1 つの帳簿で持つ。
//   島(接して動く物の組)ごとに分けないので、ある組の解き方の誤差(1 刻み 0.1 J 以下)が別の組の熱から差し引かれることがある。
// 物はまだ温度を持たない(08 §1)ので、接触の両側の分とも接触点のセルへ返す(物に温度ができたら半分を物へ。帳簿の式は変わらない)。
#ifndef BICAMERAL_PHYSICS_HEAT_HLSLI
#define BICAMERAL_PHYSICS_HEAT_HLSLI

#include "physics_step.hlsli"

PX_NAMESPACE_BEGIN

FX_CONST int64_t PX_HEAT_UNITS_PER_MILLIJOULE = 1024000;  // 1 mJ = 1000 µJ × 2^10(PX_ENERGY_FRACTION_BITS)
// 1 刻みに返す上限(2^31 mJ ≈ 2.1 MJ)。超えた分は残りに入り次の刻みに返す(帳簿の式は変わらない)
FX_CONST int64_t PX_HEAT_MAX_MILLIJOULES_PER_TICK = (int64_t)1 << 31;
FX_CONST uint32_t PX_HEAT_WEIGHT_SHIFT = 20;               // |λ|(2^-16 N)× |v|(2^-20 m/s)/ 2^20 = 2^-16 W
FX_CONST uint64_t PX_HEAT_WEIGHT_MAX = (uint64_t)1 << 44;  // 1 点の上限(2^28 W。2^20 個の入口の和が 64bit に収まる)
FX_CONST uint32_t PX_HEAT_DIVISOR_BITS = 31;               // 割る時の重みの桁(返す mJ × 重み < 2^62)

// 熱の入口(1 小刻みの 1 接触点)。C++ と HLSL で同じ並び(40 バイト)
struct PxHeatEntry {
    PxVec3 position;      // 2^-20 m(A と B の接触点の真ん中)
    uint64_t weight;      // 仕事の重み(2^-16 W。0 = 空き)
    int64_t milliJoules;  // 刻みの終わりに分けた熱(mJ)
};

// 熱の帳簿(世界に 1 つ)
struct PxHeatLedger {
    int64_t returnedMilliJoules;  // 世界へ返した熱の累計(mJ)
    int64_t remainder;            // 直近の刻みの終わりの残り(2^-10 µJ。端数と借り。負 = 借り)
    uint64_t weightSum;           // 直近の刻みの入口の重みの和
    int64_t tickMilliJoules;      // 直近の刻みに返した熱(mJ)
};

// --- 小刻みごと: 接触点の仕事の重みと位置 ---

// 行の向きの相対の速さ(2^-20 m/s。B − A。∂C/∂θ は 2^-30 m、ω は 2^-20 rad/s)。速度は小刻みの初めの値
FX_FN int64_t PxRowSpeed(PxRow row, PxBody a, PxBody b) {
    const int64_t translation = PxDot(row.direction, PxSub(b.velocity, a.velocity), PX_UNIT_SHIFT);
    const int64_t angular = PxDot(row.angularA, a.angularVelocity, PX_UNIT_SHIFT) +
                            PxDot(row.angularB, b.angularVelocity, PX_UNIT_SHIFT);
    return translation + angular;
}

// 接触点の仕事の重み Σ |λ| × |相対の速さ|(2^-16 W。上限 PX_HEAT_WEIGHT_MAX)。λ は最後の本反復の値。
// 当たった瞬間は近づく速さ × 法線の力、滑る間は滑る速さ × 摩擦の力、静止は 0 に近い
FX_FN uint64_t PxContactPointWork(PxContactPoint contactPoint, PxBody a, PxBody b) {
    uint64_t work = 0;
    for (uint32_t r = 0; r < 3; ++r) {
        const PxRow row = contactPoint.rows[r];
        const uint64_t force = FxAbsU64(row.lambda);
        const uint64_t speed = FxAbsU64(PxRowSpeed(row, a, b));
        const uint64_t power = FxShiftRightU128(FxMulU64Full(force, speed), PX_HEAT_WEIGHT_SHIFT);
        work += power < PX_HEAT_WEIGHT_MAX ? power : PX_HEAT_WEIGHT_MAX;
    }

    return work < PX_HEAT_WEIGHT_MAX ? work : PX_HEAT_WEIGHT_MAX;
}

// 接触点の位置(2^-20 m): 小刻みの初めの姿勢の A の点と B の点の真ん中
FX_FN PxVec3 PxContactPointPosition(PxContactPoint contactPoint, PxBody a, PxBody b) {
    const PxVec3 armA = PxMulMat(PxRotationMatrix(a.startRotation), contactPoint.localA, PX_UNIT_SHIFT);
    const PxVec3 armB = PxMulMat(PxRotationMatrix(b.startRotation), contactPoint.localB, PX_UNIT_SHIFT);
    const PxVec3 sum = PxAdd(PxAdd(a.startPosition, armA), PxAdd(b.startPosition, armB));
    return PxMakeVec3(sum.x >> 1, sum.y >> 1, sum.z >> 1);
}

FX_FN PxHeatEntry PxMakeHeatEntry(PxContactPoint contactPoint, PxBody a, PxBody b) {
    PxHeatEntry entry = PX_ZERO(PxHeatEntry);
    entry.position = PxContactPointPosition(contactPoint, a, b);
    entry.weight = PxContactPointWork(contactPoint, a, b);
    return entry;
}

// --- 刻みの終わり: 返す元を入口へ分ける ---

// 返す元(2^-10 µJ)= Σ dissipatedEnergy − 返した量
FX_FN int64_t PxHeatSource(int64_t dissipatedSum, int64_t returnedMilliJoules) {
    return dissipatedSum - returnedMilliJoules * PX_HEAT_UNITS_PER_MILLIJOULE;
}

// この刻みに返せる熱(mJ)。返す元が負(借り)なら 0
FX_FN int64_t PxHeatAvailableMilliJoules(int64_t source) {
    if (source <= 0)
        return 0;

    const int64_t milliJoules = source / PX_HEAT_UNITS_PER_MILLIJOULE;
    return milliJoules < PX_HEAT_MAX_MILLIJOULES_PER_TICK ? milliJoules : PX_HEAT_MAX_MILLIJOULES_PER_TICK;
}

// 重みを割る前に右へずらす桁(和を PX_HEAT_DIVISOR_BITS 桁に収める)
FX_FN uint32_t PxHeatWeightShift(uint64_t weightSum) {
    if (weightSum == 0)
        return 0;

    const uint32_t bits = FxMsbU64(weightSum) + 1;
    return bits > PX_HEAT_DIVISOR_BITS ? bits - PX_HEAT_DIVISOR_BITS : 0;
}

// 1 つの入口の分(mJ)= 返せる熱 × (重み >> s) ÷ (和 >> s)(切り捨て)。
// Σ (重み >> s) ≤ (和 >> s) なので、分の和は返せる熱を超えない(残りは帳簿の残りへ)
FX_FN int64_t PxHeatShare(int64_t available, uint64_t weight, uint64_t weightSum) {
    const uint32_t shift = PxHeatWeightShift(weightSum);
    const uint64_t divisor = weightSum >> shift;
    if (available <= 0 || divisor == 0)
        return 0;

    return (int64_t)(((uint64_t)available * (weight >> shift)) / divisor);
}

PX_NAMESPACE_END

#endif  // BICAMERAL_PHYSICS_HEAT_HLSLI
