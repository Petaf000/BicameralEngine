// physics_step.hlsli — 整数の AVBD(08 §2。T-0090)の 1 刻みの部品のうち、物ごと・組ごと・接触点ごとの手順と、物と接触の構造体。
// CPU の世界(engine/src/sim/physics_world.cpp)と GPU の Compute(shaders/sim/physics_step.hlsl)が同じ関数を同じ順で呼ぶので、
// ビット単位で同じ結果になる(tests/gpu_physics_test.cpp が確かめる)。呼ぶ順番と組の置き場所(CPU は std::map、GPU は持ち主の物の枠)は呼ぶ側が決める。
// 順番に依存しないことの根拠: 物ごとの 6×6 への足し込み(PxAddRow)は整数の和、β の「触れている一番重い質量」は最大値、彩色は 1 回ごとに前の回の色だけを読む。
//
// 構造体は C++ と HLSL の構造化バッファで同じ並びにする: 64bit の値は 8 の倍数の位置に置き、大きさも 8 の倍数にする
// (4 バイト詰めでも 8 バイト揃えでも同じ並びになる)。bool は使わない(HLSL は 4 バイト)。大きさは physics_world.h の static_assert。
#ifndef BICAMERAL_PHYSICS_STEP_HLSLI
#define BICAMERAL_PHYSICS_STEP_HLSLI

#include "physics_collision.hlsli"
#include "physics_solver.hlsli"

PX_NAMESPACE_BEGIN

FX_CONST uint32_t PX_MANIFOLD_POINTS = 8;  // 接触の生成で 4 点まで + 途中の探し直しで 4 点まで
FX_CONST int64_t PX_ALPHA_ONE_Q16 = 65536;
FX_CONST int64_t PX_NORMAL_SIMILARITY = 1020054733;  // 0.95(Q1.30)
FX_CONST int32_t PX_NO_COLOR = -1;
FX_CONST int32_t PX_GPU_MAX_COLORS = 16;  // GPU は色ごとに 1 回ずつ解くパスを固定の数だけ投げる(T-0090)
// GPU の持ち主の枠の数の上限(Work Graph の広域の選別が出すレコードの数はコンパイル時に決める。T-0092)
FX_CONST uint32_t PX_GPU_MAX_SLOTS_PER_BODY = 16;

// --- 物 ---------------------------------------------------------------------------------------
struct PxBody {
    // --- 形と質量(0 = 動かない)---
    PxVec3 halfExtent;  // 2^-20 m
    uint64_t massMilligrams;
    int64_t massOverH2;      // M/h²(2^-8 N/m)
    PxVec3 inertiaOverH2;    // 局所の主慣性モーメント / h²(2^-8 N·m/rad)
    int64_t boundingRadius;  // 外接球の半径(2^-20 m)

    // --- 現れる・消える刻み ---
    uint64_t spawnTick;
    uint64_t removeTick;

    // --- 状態 ---
    PxVec3 position;         // 2^-20 m
    PxQuat rotation;         // Q1.30
    PxVec3 velocity;         // 2^-20 m/s
    PxVec3 angularVelocity;  // 2^-20 rad/s
    PxVec3 previousVelocity;

    // --- 刻みの中(2^-32 m・2^-32 rad)---
    PxVec3 startPosition;
    PxQuat startRotation;
    PxMat3 inertiaWorld;  // 回転の I/h²(世界の向き)
    PxVec3 inertialLinear;
    PxVec3 inertialAngular;
    PxVec3 deltaLinear;
    PxVec3 deltaAngular;

    // --- 32bit ---
    int32_t color;    // PX_NO_COLOR = 塗っていない
    uint32_t active;  // 1 = 世界にいる(spawnTick ≤ 刻み < removeTick)
};

// --- 接触 -------------------------------------------------------------------------------------
struct PxContactPoint {
    uint32_t feature;
    uint32_t stick;  // 1 = 静止摩擦(次の刻みも同じ接触点を使う)
    PxVec3 normal;   // A → B(Q1.30。点ごと。途中で探し直した点は、その姿勢の法線)
    PxVec3 localA;   // A の局所座標での接触点(2^-20 m)
    PxVec3 localB;
    PxRow rows[3];  // 法線・接線 2 本
};

struct PxManifold {
    uint32_t bodyA;  // bodyA < bodyB
    uint32_t bodyB;
    uint32_t count;  // 0 = 組が無い(GPU の空いた枠)
    uint32_t unused;
    PxVec3 normal;  // A → B(Q1.30)
    int64_t frictionQ16;
    int64_t beta;        // 硬さの増え方(N/m²。組の両方に触れている物の中で一番重い質量 × β)
    int64_t minPenalty;  // 組の硬さの下限(2^-8 N/m)
    int64_t maxPenalty;  // 組の硬さの上限(2^-8 N/m)
    PxContactPoint points[PX_MANIFOLD_POINTS];
};

// --- 小さな部品 -------------------------------------------------------------------------------
FX_FN bool PxIsDynamic(PxBody body) {
    return body.massMilligrams > 0;
}

FX_FN uint32_t PxIsActiveAt(PxBody body, uint64_t tick) {
    return tick >= body.spawnTick && tick < body.removeTick ? 1u : 0u;
}

FX_FN uint64_t PxMaxU64(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

FX_FN uint64_t PxMinU64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

FX_FN PxBox PxShapeOf(PxBody body) {
    PxBox box = PX_ZERO(PxBox);
    box.center = body.position;
    box.rotation = PxRotationMatrix(body.rotation);
    box.halfExtent = body.halfExtent;

    return box;
}

// 刻みの初め + 今の推定の Δ の姿勢(反復の途中の探し直しに使う)
FX_FN PxBox PxEstimatedShape(PxBody body) {
    if (!PxIsDynamic(body))
        return PxShapeOf(body);

    const int64_t divisor = (int64_t)1 << PX_DELTA_EXTRA_BITS;
    const PxVec3 step = PxMakeVec3(body.deltaLinear.x / divisor, body.deltaLinear.y / divisor,
                                   body.deltaLinear.z / divisor);
    PxBox box = PX_ZERO(PxBox);
    box.center = PxAdd(body.startPosition, step);
    box.rotation = PxRotationMatrix(PxIntegrateRotation(body.startRotation, body.deltaAngular));
    box.halfExtent = body.halfExtent;

    return box;
}

// 1 刻みの今の動き(2^-32 m): |Δ| + |Δθ| × 外接球の半径
FX_FN int64_t PxStepMotion(PxBody body) {
    if (!PxIsDynamic(body))
        return 0;

    const int64_t turn = FxMulShiftS64((int64_t)PxLength(body.deltaAngular), body.boundingRadius, 20);
    return (int64_t)PxLength(body.deltaLinear) + turn;
}

// 法線に直交する 1 本目(法線の一番小さい成分の軸との外積から。試作と同じ選び方)。2 本目は 法線 × 1 本目
FX_FN PxVec3 PxFirstTangent(PxVec3 normal) {
    const int64_t ax = PxAbs(normal.x);
    const int64_t ay = PxAbs(normal.y);
    const int64_t az = PxAbs(normal.z);
    PxVec3 helper = PxMakeVec3(0, 0, PX_UNIT_ONE);
    if (ax <= ay && ax <= az)
        helper = PxMakeVec3(PX_UNIT_ONE, 0, 0);
    else if (ay <= az)
        helper = PxMakeVec3(0, PX_UNIT_ONE, 0);

    return PxNormalize(PxCross(normal, helper, PX_UNIT_SHIFT));
}

// 回転の I/h² を世界の向きに: (R diag(I) Rᵀ)_ij = Σ_k R_ik I_k R_jk
FX_FN int64_t PxWorldInertiaEntry(PxMat3 rotation, PxVec3 inertia, uint32_t i, uint32_t j) {
    int64_t value = 0;
    for (uint32_t k = 0; k < 3; ++k) {
        const int64_t scaled = FxMulShiftS64(PxGet(PxMatRow(rotation, i), k), PxGet(inertia, k), PX_UNIT_SHIFT);
        value += FxMulShiftS64(scaled, PxGet(PxMatRow(rotation, j), k), PX_UNIT_SHIFT);
    }

    return value;
}

FX_FN PxVec3 PxWorldInertiaRow(PxMat3 rotation, PxVec3 inertia, uint32_t i) {
    return PxMakeVec3(PxWorldInertiaEntry(rotation, inertia, i, 0), PxWorldInertiaEntry(rotation, inertia, i, 1),
                      PxWorldInertiaEntry(rotation, inertia, i, 2));
}

// --- 刻みの初め: 物の初期化 -----------------------------------------------------------------------
// 慣性の目標と初めの推定。刻みの初めの姿勢を取り、Δ を 0 に、色を消す
FX_FN PxBody PxInitializeBody(PxBody body, int64_t rate) {
    // 重力の 1 小刻みの変位 g h²(2^-32 m)= g(2^-20 m/s²)× 2^12 / rate²
    const int64_t gravityStep = (PX_GRAVITY << PX_DELTA_EXTRA_BITS) / (rate * rate);

    body.startPosition = body.position;
    body.startRotation = body.rotation;
    body.deltaLinear = PxMakeVec3(0, 0, 0);
    body.deltaAngular = PxMakeVec3(0, 0, 0);
    body.color = PX_NO_COLOR;
    if (!PxIsDynamic(body) || body.active == 0)
        return body;

    const PxMat3 rotation = PxRotationMatrix(body.rotation);
    body.inertiaWorld.row0 = PxWorldInertiaRow(rotation, body.inertiaOverH2, 0);
    body.inertiaWorld.row1 = PxWorldInertiaRow(rotation, body.inertiaOverH2, 1);
    body.inertiaWorld.row2 = PxWorldInertiaRow(rotation, body.inertiaOverH2, 2);

    // v h(2^-32 m)= v(2^-20 m/s)× 2^12 / rate
    const PxVec3 velocityStep = PxMakeVec3((body.velocity.x << PX_DELTA_EXTRA_BITS) / rate,
                                           (body.velocity.y << PX_DELTA_EXTRA_BITS) / rate,
                                           (body.velocity.z << PX_DELTA_EXTRA_BITS) / rate);
    body.inertialLinear = PxMakeVec3(velocityStep.x, velocityStep.y - gravityStep, velocityStep.z);
    body.inertialAngular = PxMakeVec3((body.angularVelocity.x << PX_DELTA_EXTRA_BITS) / rate,
                                      (body.angularVelocity.y << PX_DELTA_EXTRA_BITS) / rate,
                                      (body.angularVelocity.z << PX_DELTA_EXTRA_BITS) / rate);

    // 初めの推定: 前の刻みで重力の向きに加速していた割合(Q16)だけ重力を入れる(適応的なウォームスタート)
    const int64_t accelerationY = (body.velocity.y - body.previousVelocity.y) * rate;  // 2^-20 m/s²
    const int64_t weightQ16 = PxClamp(FxDivShiftS64(-accelerationY, PX_GRAVITY, 16), 0, 65536);
    body.deltaLinear = PxMakeVec3(velocityStep.x, velocityStep.y - FxMulShiftS64(gravityStep, weightQ16, 16),
                                  velocityStep.z);
    body.deltaAngular = body.inertialAngular;

    return body;
}

// --- 広域の選別と接触の生成 -------------------------------------------------------------------------
// 組の余裕(先読みの接触: 速さぶん広げる。2^-20 m)。組にしないなら -1。
// 角速度(2^-20 rad/s)× 半径(2^-20 m)/ 2^20 = 2^-20 m/s
FX_FN int64_t PxPairMargin(PxBody a, PxBody b, PxParameters p, int64_t rate) {
    if (a.active == 0 || b.active == 0 || (!PxIsDynamic(a) && !PxIsDynamic(b)))
        return -1;

    const int64_t spinA = FxMulShiftS64((int64_t)PxLength(a.angularVelocity), a.boundingRadius, 20);
    const int64_t spinB = FxMulShiftS64((int64_t)PxLength(b.angularVelocity), b.boundingRadius, 20);
    const int64_t relativeSpeed = (int64_t)PxLength(PxSub(a.velocity, b.velocity)) + spinA + spinB;
    const int64_t margin = p.collisionMargin + relativeSpeed / rate;
    if ((int64_t)PxLength(PxSub(a.position, b.position)) > a.boundingRadius + b.boundingRadius + margin)
        return -1;

    return margin;
}

// 組の見出し(点は無し): 硬さの下限 = 組の重い方の M/h² × startPenalty、
// 上限 = 組の軽い方(動く物)の M/h² × 2^penaltyRatioShift(その物の 6×6 の条件数がこれで抑えられる)
struct PxPenaltyRange {
    int64_t low;
    int64_t high;
};

FX_FN PxPenaltyRange PxPairPenaltyRange(PxBody bodyA, PxBody bodyB, PxParameters p, int64_t rate) {
    const int64_t pairMassOverH2 = PxMassOverStepSquared(PxMaxU64(bodyA.massMilligrams, bodyB.massMilligrams), rate);
    PxPenaltyRange range;
    range.low = PxMax(p.penaltyMin, FxMulShiftS64(pairMassOverH2, p.startPenaltyQ16, 16));

    uint64_t lighterMass = PxMinU64(bodyA.massMilligrams, bodyB.massMilligrams);
    if (!PxIsDynamic(bodyA))
        lighterMass = bodyB.massMilligrams;
    else if (!PxIsDynamic(bodyB))
        lighterMass = bodyA.massMilligrams;

    const int64_t ratioCap = PxMassOverStepSquared(lighterMass, rate) << p.penaltyRatioShift;
    range.high = PxMax(range.low, PxMin(p.penaltyMax, ratioCap));

    return range;
}

FX_FN PxManifold PxBeginManifold(uint32_t a, uint32_t b, PxBody bodyA, PxBody bodyB, PxVec3 normal, int64_t frictionQ16,
                                 PxParameters p, int64_t rate) {
    PxManifold manifold = PX_ZERO(PxManifold);
    manifold.bodyA = a;
    manifold.bodyB = b;
    manifold.normal = normal;
    manifold.frictionQ16 = frictionQ16;

    const PxPenaltyRange range = PxPairPenaltyRange(bodyA, bodyB, p, rate);
    manifold.minPenalty = range.low;
    manifold.maxPenalty = range.high;

    return manifold;
}

// 接触の生成の 1 点を、両方の物の局所座標の点にする(λ は 0、硬さは組の下限)
FX_FN PxContactPoint PxMakeContactPoint(PxContactPointGeometry source, PxVec3 normal, PxBox shapeA, PxBox shapeB,
                                        int64_t minPenalty) {
    PxContactPoint contactPoint = PX_ZERO(PxContactPoint);
    contactPoint.feature = source.feature;
    contactPoint.normal = normal;
    contactPoint.localA = PxMulMatTransposed(shapeA.rotation, PxSub(source.pointA, shapeA.center), PX_UNIT_SHIFT);
    contactPoint.localB = PxMulMatTransposed(shapeB.rotation, PxSub(source.pointB, shapeB.center), PX_UNIT_SHIFT);
    contactPoint.rows[0].penalty = minPenalty;
    contactPoint.rows[1].penalty = minPenalty;
    contactPoint.rows[2].penalty = minPenalty;

    return contactPoint;
}

// --- 前の刻みの組の読み方 --------------------------------------------------------------------------
// 引き継ぎ(PxBuildManifold・PxInheritFromPrevious・PxMatchByProximity)は前の組を Previous 型で読む: Count() と Point(j)。
// CPU は局所に持つ組(PxPreviousManifold)、GPU はバッファから点を 1 つずつ読む(physics_bindings.hlsli の GpuPreviousManifold)。
// GPU で組(3 KB)を 2 つ局所に持つと、Work Graph のノードの中で GPU が固まった(局所の変数が約 5〜6 KB を超えると。T-0092)
struct PxPreviousManifold {
    PxManifold manifold;  // count = 0 なら前の組は無い

    uint32_t Count() { return manifold.count; }

    PxContactPoint Point(uint32_t j) { return manifold.points[j]; }
};

// 前の刻みの同じ特徴の点から λ・硬さ・静止摩擦の接触点を引き継ぐ
template <typename Previous>
FX_FN PxContactPoint PxInheritFromPrevious(PxContactPoint contactPoint, Previous old) {
    for (uint32_t j = 0; j < old.Count(); ++j) {
        const PxContactPoint oldPoint = old.Point(j);
        if (oldPoint.feature != contactPoint.feature)
            continue;

        for (uint32_t r = 0; r < 3; ++r)
            contactPoint.rows[r] = oldPoint.rows[r];

        contactPoint.stick = oldPoint.stick;
        if (contactPoint.stick == 0)
            continue;

        contactPoint.localA = oldPoint.localA;
        contactPoint.localB = oldPoint.localB;
    }

    return contactPoint;
}

// 特徴の番号が合わなかった点(切り抜きの境界にかかった頂点・4 点の選び方が変わった)は、
// 特徴で使われなかった前の点のうち一番近いもの(proximity 以内・法線が近い)から λ と硬さを引き継ぐ(1 対 1)。
// 静止している山で、荷重のかかった点の λ が刻みごとに 0 に戻って揺れ続けるのを防ぐ(T-0091)
template <typename Previous>
FX_FN PxManifold PxMatchByProximity(PxQuat rotationA, Previous old, PxManifold manifold, int64_t proximity) {
    // 使った前の点・合った新しい点の印(ビット j / k。点は PX_MANIFOLD_POINTS まで)
    uint32_t usedOld = 0;
    uint32_t matchedNew = 0;
    for (uint32_t k = 0; k < manifold.count; ++k) {
        for (uint32_t j = 0; j < old.Count(); ++j) {
            if (old.Point(j).feature != manifold.points[k].feature)
                continue;

            usedOld |= 1u << j;
            matchedNew |= 1u << k;
        }
    }

    const PxMat3 rotation = PxRotationMatrix(rotationA);
    for (uint32_t k = 0; k < manifold.count; ++k) {
        if ((matchedNew & (1u << k)) != 0)
            continue;

        const PxVec3 here = PxMulMat(rotation, manifold.points[k].localA, PX_UNIT_SHIFT);
        int32_t nearestIndex = -1;
        int64_t nearest = proximity;
        for (uint32_t j = 0; j < old.Count(); ++j) {
            const PxContactPoint oldPoint = old.Point(j);
            const PxVec3 there = PxMulMat(rotation, oldPoint.localA, PX_UNIT_SHIFT);
            const int64_t distance = (int64_t)PxLength(PxSub(here, there));
            const bool similar = PxDot(oldPoint.normal, manifold.points[k].normal, PX_UNIT_SHIFT) >=
                                 PX_NORMAL_SIMILARITY;
            if ((usedOld & (1u << j)) != 0 || distance >= nearest || !similar)
                continue;

            nearest = distance;
            nearestIndex = (int32_t)j;
        }

        if (nearestIndex < 0)
            continue;

        usedOld |= 1u << (uint32_t)nearestIndex;
        const PxContactPoint nearestPoint = old.Point((uint32_t)nearestIndex);
        for (uint32_t r = 0; r < 3; ++r) {
            manifold.points[k].rows[r].lambda = nearestPoint.rows[r].lambda;
            manifold.points[k].rows[r].penalty = nearestPoint.rows[r].penalty;
        }
    }

    return manifold;
}

// 接触の生成の結果から組を作り、前の刻みの同じ組(無ければ Count() = 0 のもの)から引き継ぐ。点が無ければ count = 0
template <typename Previous>
FX_FN PxManifold PxBuildManifold(uint32_t a, uint32_t b, PxBody bodyA, PxBody bodyB, PxContactGeometry geometry,
                                 int64_t frictionQ16, Previous previous, PxParameters p, int64_t rate) {
    PxManifold manifold = PxBeginManifold(a, b, bodyA, bodyB, geometry.normal, frictionQ16, p, rate);
    const PxBox shapeA = PxShapeOf(bodyA);
    const PxBox shapeB = PxShapeOf(bodyB);
    for (uint32_t k = 0; k < geometry.count; ++k) {
        const PxContactPoint contactPoint = PxMakeContactPoint(geometry.points[k], geometry.normal, shapeA, shapeB,
                                                               manifold.minPenalty);
        manifold.points[manifold.count] = PxInheritFromPrevious(contactPoint, previous);
        manifold.count += 1;
    }

    return PxMatchByProximity(bodyA.rotation, previous, manifold, p.proximityMatch);
}

// 接触の生成の最大の食い込み(2^-20 m。食い込んでいなければ 0)
FX_FN int64_t PxMaxPenetration(PxContactGeometry geometry) {
    int64_t deepest = 0;
    for (uint32_t k = 0; k < geometry.count; ++k)
        deepest = PxMax(deepest, -geometry.points[k].separation);

    return deepest;
}

// --- 硬さの増え方と、刻みをまたいだ引き継ぎ --------------------------------------------------------
// β の「触れている質量」: 動く物は自分の質量から始め、組の相手(動く物)の質量の最大を取る
FX_FN uint64_t PxTouchMass(PxBody body) {
    return PxIsDynamic(body) ? body.massMilligrams : 0;
}

// β = βkg × 組の両方に触れている物の中で一番重い質量(mg)。
// 軽い物どうし・軽い物と地面の組も、上に重い物が載れば速く硬くなる(重い物に押されて潰れるのを少ない反復で止める。T-0091)
FX_FN int64_t PxBetaOf(uint64_t massMilligrams, PxParameters p) {
    return (int64_t)(massMilligrams / 1000000) * p.betaPerKilogram +
           (int64_t)(massMilligrams % 1000000) * p.betaPerKilogram / 1000000;
}

// 1 行の硬さを γ 倍して low〜high に収める
FX_FN PxRow PxDecayPenalty(PxRow row, int64_t gammaQ16, int64_t low, int64_t high) {
    row.penalty = PxClamp(FxMulShiftS64(row.penalty, gammaQ16, 16), low, high);

    return row;
}

// λ はそのまま、硬さは γ 倍(組の下限〜上限に収める)
FX_FN PxManifold PxWarmStartManifold(PxManifold manifold, int64_t gammaQ16) {
    const int64_t low = manifold.minPenalty;
    const int64_t high = manifold.maxPenalty;
    for (uint32_t k = 0; k < manifold.count; ++k) {
        manifold.points[k].rows[0] = PxDecayPenalty(manifold.points[k].rows[0], gammaQ16, low, high);
        manifold.points[k].rows[1] = PxDecayPenalty(manifold.points[k].rows[1], gammaQ16, low, high);
        manifold.points[k].rows[2] = PxDecayPenalty(manifold.points[k].rows[2], gammaQ16, low, high);
    }

    return manifold;
}

// --- 線形化と、反復の途中の探し直し ---------------------------------------------------------------
// 刻みの初めの姿勢で 1 点を線形化する(法線は点ごと)
FX_FN PxContactPoint PxLinearizePoint(PxContactPoint contactPoint, PxBody a, PxBody b) {
    const PxVec3 first = PxFirstTangent(contactPoint.normal);
    const PxVec3 second = PxCross(contactPoint.normal, first, PX_UNIT_SHIFT);
    const PxVec3 armA = PxMulMat(PxRotationMatrix(a.startRotation), contactPoint.localA, PX_UNIT_SHIFT);
    const PxVec3 armB = PxMulMat(PxRotationMatrix(b.startRotation), contactPoint.localB, PX_UNIT_SHIFT);
    const PxVec3 gap = PxSub(PxAdd(b.startPosition, armB), PxAdd(a.startPosition, armA));
    contactPoint.rows[0] = PxLinearizeRow(contactPoint.rows[0], contactPoint.normal, armA, armB, gap);
    contactPoint.rows[1] = PxLinearizeRow(contactPoint.rows[1], first, armA, armB, gap);
    contactPoint.rows[2] = PxLinearizeRow(contactPoint.rows[2], second, armA, armB, gap);

    return contactPoint;
}

FX_FN PxManifold PxLinearizeManifold(PxManifold manifold, PxBody a, PxBody b) {
    for (uint32_t k = 0; k < manifold.count; ++k)
        manifold.points[k] = PxLinearizePoint(manifold.points[k], a, b);

    return manifold;
}

// 反復の途中で、今の推定の姿勢(刻みの初め + Δ)で接触を探し直し、まだ無い特徴の点を足す(回って当たる角・跳ね返りで当たる面)。
// 刻みの初めの姿勢で線形化する(ほかの点と同じ式)。組は増やさない(彩色が変わるため)。
// 止まっている組(1 刻みの動きが recollideMinMotion 以下)は探さない(静止の釣り合いを崩さない)
FX_FN PxManifold PxRecollideManifold(PxManifold manifold, PxBody a, PxBody b, PxParameters p) {
    if (PxStepMotion(a) + PxStepMotion(b) <= p.recollideMinMotion)
        return manifold;

    const PxBox shapeA = PxEstimatedShape(a);
    const PxBox shapeB = PxEstimatedShape(b);
    const PxContactGeometry geometry = PxCollideBoxes(shapeA, shapeB, p.collisionMargin);
    for (uint32_t k = 0; k < geometry.count && manifold.count < PX_MANIFOLD_POINTS; ++k) {
        bool known = false;
        for (uint32_t j = 0; j < manifold.count; ++j)
            known = known || manifold.points[j].feature == geometry.points[k].feature;

        if (known)
            continue;

        const PxContactPoint contactPoint = PxMakeContactPoint(geometry.points[k], geometry.normal, shapeA, shapeB,
                                                               manifold.minPenalty);
        manifold.points[manifold.count] = PxLinearizePoint(contactPoint, a, b);
        manifold.count += 1;
    }

    return manifold;
}

// --- 反復: 物ごとの 6×6 と、λ と硬さの更新 ----------------------------------------------------------
// 摩擦の上限 μ|λn|(2^-16 N)
FX_FN int64_t PxFrictionBound(PxContactPoint contactPoint, int64_t frictionQ16) {
    return FxMulShiftS64(frictionQ16, PxAbs(contactPoint.rows[0].lambda), 16);
}

// 1 つの接触点の 3 行を、物の 6×6 に足す(物が A なら法線の向きを逆に)
FX_FN PxBodySystem PxAddPointRows(PxBodySystem system, PxContactPoint contactPoint, int64_t frictionQ16, bool isA,
                                  int64_t alphaQ16, int64_t gapSlop, PxVec3 deltaLinearA, PxVec3 deltaAngularA,
                                  PxVec3 deltaLinearB, PxVec3 deltaAngularB) {
    const int64_t frictionBound = PxFrictionBound(contactPoint, frictionQ16);
    for (uint32_t r = 0; r < 3; ++r) {
        const PxRow row = contactPoint.rows[r];
        const int64_t lower = r == 0 ? PX_MOST_NEGATIVE : -frictionBound;
        const int64_t upper = r == 0 ? 0 : frictionBound;
        const int64_t constraint = PxConstraintValue(row, r == 0, alphaQ16, gapSlop, deltaLinearA, deltaAngularA,
                                                     deltaLinearB, deltaAngularB);
        const int64_t force = PxRowForce(row, constraint, lower, upper);
        const PxVec3 direction = PxSelect(isA, PxNegate(row.direction), row.direction);
        system = PxAddRow(system, direction, PxSelect(isA, row.angularA, row.angularB), row.penalty, force);
    }

    return system;
}

// 慣性の項から始める(行は PxAddPointRows で足す)
FX_FN PxBodySystem PxBeginSystemOf(PxBody body) {
    return PxBeginBodySystem(body.massOverH2, body.inertiaWorld, PxSub(body.deltaLinear, body.inertialLinear),
                             PxSub(body.deltaAngular, body.inertialAngular));
}

// 解いた 6×6 の解を Δ から引く
FX_FN PxBody PxApplySolution(PxBody body, PxVec6 solution) {
    body.deltaLinear = PxSub(body.deltaLinear, PxMakeVec3(solution.v[0], solution.v[1], solution.v[2]));
    body.deltaAngular = PxSub(body.deltaAngular, PxMakeVec3(solution.v[3], solution.v[4], solution.v[5]));

    return body;
}

// λ = clamp(k C + λ)。上下限に張り付いていなければ硬さを β|C| だけ上げる。静止摩擦の判定も
FX_FN PxContactPoint PxUpdatePointDuals(PxContactPoint contactPoint, int64_t frictionQ16, int64_t beta,
                                        int64_t maxPenalty, int64_t alphaQ16, PxParameters p, PxVec3 deltaLinearA,
                                        PxVec3 deltaAngularA, PxVec3 deltaLinearB, PxVec3 deltaAngularB) {
    const int64_t frictionBound = PxFrictionBound(contactPoint, frictionQ16);
    for (uint32_t r = 0; r < 3; ++r) {
        PxRow row = contactPoint.rows[r];
        const int64_t lower = r == 0 ? PX_MOST_NEGATIVE : -frictionBound;
        const int64_t upper = r == 0 ? 0 : frictionBound;
        const int64_t constraint = PxConstraintValue(row, r == 0, alphaQ16, p.gapSlop, deltaLinearA, deltaAngularA,
                                                     deltaLinearB, deltaAngularB);
        row.lambda = PxRowForce(row, constraint, lower, upper);
        if (row.lambda > lower && row.lambda < upper) {
            const int64_t grown = row.penalty + FxMulShiftS64(beta, PxAbs(constraint), PX_FORCE_FROM_PENALTY_SHIFT);
            row.penalty = PxMin(grown, maxPenalty);
        }

        contactPoint.rows[r] = row;
    }

    const int64_t drift = (int64_t)PxLength(PxMakeVec3(contactPoint.rows[1].c0, contactPoint.rows[2].c0, 0));
    const bool held = PxAbs(contactPoint.rows[1].lambda) < frictionBound &&
                      PxAbs(contactPoint.rows[2].lambda) < frictionBound;
    contactPoint.stick = held && drift < p.stickThreshold ? 1u : 0u;

    return contactPoint;
}

// --- 刻みの終わり -----------------------------------------------------------------------------
// 速度 = Δ / h(2^-20 m/s = 2^-32 m × rate / 2^12)
FX_FN PxBody PxUpdateVelocity(PxBody body, int64_t rate) {
    if (!PxIsDynamic(body) || body.active == 0)
        return body;

    const int64_t divisor = (int64_t)1 << PX_DELTA_EXTRA_BITS;
    body.previousVelocity = body.velocity;
    body.velocity = PxMakeVec3(body.deltaLinear.x * rate / divisor, body.deltaLinear.y * rate / divisor,
                               body.deltaLinear.z * rate / divisor);
    body.angularVelocity = PxMakeVec3(body.deltaAngular.x * rate / divisor, body.deltaAngular.y * rate / divisor,
                                      body.deltaAngular.z * rate / divisor);

    return body;
}

// 位置と向きを刻みの初め + Δ に
FX_FN PxBody PxFinishBody(PxBody body) {
    if (!PxIsDynamic(body) || body.active == 0)
        return body;

    const int64_t divisor = (int64_t)1 << PX_DELTA_EXTRA_BITS;
    const PxVec3 step = PxMakeVec3(body.deltaLinear.x / divisor, body.deltaLinear.y / divisor,
                                   body.deltaLinear.z / divisor);
    body.position = PxAdd(body.startPosition, step);
    body.rotation = PxIntegrateRotation(body.startRotation, body.deltaAngular);

    return body;
}

// --- 彩色(Jones-Plassmann。番号のハッシュを優先度に)-----------------------------------------------
// j の優先度が i より低いか(ハッシュ、同じなら番号)
FX_FN bool PxColorPriorityBelow(uint32_t j, uint32_t i) {
    const uint64_t priorityJ = FxMix64((uint64_t)j + 1);
    const uint64_t priorityI = FxMix64((uint64_t)i + 1);
    return priorityJ < priorityI || (priorityJ == priorityI && j < i);
}

// 状態のハッシュに 1 つの物を足す(位置・向き・速度)
FX_FN uint64_t PxHashBody(uint64_t hash, PxBody body) {
    hash = FxHashCombine(hash, (uint64_t)body.position.x);
    hash = FxHashCombine(hash, (uint64_t)body.position.y);
    hash = FxHashCombine(hash, (uint64_t)body.position.z);
    hash = FxHashCombine(hash, (uint64_t)body.rotation.x);
    hash = FxHashCombine(hash, (uint64_t)body.rotation.y);
    hash = FxHashCombine(hash, (uint64_t)body.rotation.z);
    hash = FxHashCombine(hash, (uint64_t)body.rotation.w);
    hash = FxHashCombine(hash, (uint64_t)body.velocity.x);
    hash = FxHashCombine(hash, (uint64_t)body.velocity.y);
    hash = FxHashCombine(hash, (uint64_t)body.velocity.z);
    hash = FxHashCombine(hash, (uint64_t)body.angularVelocity.x);
    hash = FxHashCombine(hash, (uint64_t)body.angularVelocity.y);
    hash = FxHashCombine(hash, (uint64_t)body.angularVelocity.z);

    return hash;
}

PX_NAMESPACE_END

#endif  // BICAMERAL_PHYSICS_STEP_HLSLI
