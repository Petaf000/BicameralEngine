// physics_push.hlsli — 光線で物を押す(T-0098。窓のクリック → 押すコマンド → GPU の適用の単位)。
// CPU の世界(engine/src/sim/physics_world.cpp の PhysicsWorld::Push)と GPU(shaders/sim/probe_tick.hlsl の押すコマンド)が
// 同じ関数を同じ順で呼ぶので、ビット単位で同じ結果になる。
//
// 押し方: 光線が最初に入る物(番号の順に全部の物を調べ、入る所までの距離が一番短い物。同じなら番号の小さい物)を探し、
//   それが動く物なら、入った点に光線の向きの力積を加える(速度と角速度を変える。位置は変えない)。
//   動かない物(地面・壁)が手前にあれば、それが光線を止める(何も押さない)。
//   CPU は物の場所を知らない(D-107)ので、どの物に当たるかは GPU(とリファレンス)が決める。
// 値の幅: 光線の原点と物の距離は 2^27(128 m)まで、力積は 2^33(8000 N·s 程度)まで、物の大きさは数 m を想定(64bit に収まる)。
#ifndef BICAMERAL_PHYSICS_PUSH_HLSLI
#define BICAMERAL_PHYSICS_PUSH_HLSLI

#include "physics_step.hlsli"

PX_NAMESPACE_BEGIN

FX_CONST int64_t PX_RAY_MISS = -1;
// 光線の向き(Q1.30)の成分がこれより小さい軸は、その軸に平行とみなす(割り算の商が大きくなりすぎないように)
FX_CONST int64_t PX_RAY_PARALLEL = 256;

// 光線(原点 2^-20 m、向き = 長さ 1 の Q1.30)と、物の直方体の交わり。当たれば入る所までの距離(2^-20 m、原点が中なら 0)、
// 外れれば PX_RAY_MISS。物の局所座標で、軸ごとの板(−h ≤ x ≤ h)との交わりを取る
FX_FN int64_t PxRayEntryDistance(PxBody body, PxVec3 origin, PxVec3 direction) {
    if (body.active == 0)
        return PX_RAY_MISS;

    const PxMat3 rotation = PxRotationMatrix(body.rotation);
    const PxVec3 localOrigin = PxMulMatTransposed(rotation, PxSub(origin, body.position), PX_UNIT_SHIFT);
    const PxVec3 localDirection = PxMulMatTransposed(rotation, direction, PX_UNIT_SHIFT);
    int64_t enter = 0;  // 原点より後ろには戻らない
    int64_t exit = (int64_t)1 << 62;

    for (uint32_t axis = 0; axis < 3; ++axis) {
        const int64_t start = PxGet(localOrigin, axis);
        const int64_t along = PxGet(localDirection, axis);
        const int64_t halfWidth = PxGet(body.halfExtent, axis);
        if (PxAbs(along) < PX_RAY_PARALLEL) {
            if (start < -halfWidth || start > halfWidth)
                return PX_RAY_MISS;

            continue;
        }

        // 板の 2 つの面までの距離(向きの成分で割る。小さい方が入る面)
        const int64_t toLow = FxDivShiftS64(-halfWidth - start, along, PX_UNIT_SHIFT);
        const int64_t toHigh = FxDivShiftS64(halfWidth - start, along, PX_UNIT_SHIFT);
        enter = PxMax(enter, PxMin(toLow, toHigh));
        exit = PxMin(exit, PxMax(toLow, toHigh));
    }

    return enter <= exit ? enter : PX_RAY_MISS;
}

// 動く物の、重心から offset(2^-20 m、世界の向き)の点に力積 impulse(2^-20 N·s、世界の向き)を加える。
// Δv = J / M、Δω = I⁻¹ (r × J)(I は局所の主慣性。物の向きで局所へ回して割り、世界へ戻す)。
// rate は物を作った時の 1 秒あたりの小刻みの数(inertiaOverH2 = I × rate² × 2^8 なので、I⁻¹ L = L × rate² × 2^8 / inertiaOverH2)
FX_FN PxBody PxApplyImpulse(PxBody body, PxVec3 offset, PxVec3 impulse, int64_t rate) {
    if (!PxIsDynamic(body) || body.active == 0)
        return body;

    // 速度(2^-20 m/s)= J(2^-20 N·s)× 10^6 / 質量(mg)
    const int64_t mass = (int64_t)body.massMilligrams;
    const PxVec3 deltaVelocity = PxMakeVec3(FxDivS64(impulse.x * 1000000, mass), FxDivS64(impulse.y * 1000000, mass),
                                            FxDivS64(impulse.z * 1000000, mass));
    body.velocity = PxAdd(body.velocity, deltaVelocity);

    // 角運動量(2^-20 N·m·s)を局所の向きへ回し、主慣性で割って戻す
    const PxMat3 rotation = PxRotationMatrix(body.rotation);
    const PxVec3 momentum = PxCross(offset, impulse, POSITION_FRACTION_BITS);
    const PxVec3 localMomentum = PxMulMatTransposed(rotation, momentum, PX_UNIT_SHIFT);
    const int64_t rateSquared = rate * rate;
    PxVec3 localDelta = PxMakeVec3(0, 0, 0);

    for (uint32_t axis = 0; axis < 3; ++axis) {
        const int64_t inertia = PxGet(body.inertiaOverH2, axis);
        if (inertia > 0)
            localDelta = PxSet(localDelta, axis, FxDivShiftS64(PxGet(localMomentum, axis) * rateSquared, inertia, 8));
    }

    body.angularVelocity = PxAdd(body.angularVelocity, PxMulMat(rotation, localDelta, PX_UNIT_SHIFT));

    return body;
}

// 光線の上の、原点から distance(2^-20 m)の点
FX_FN PxVec3 PxRayPoint(PxVec3 origin, PxVec3 direction, int64_t distance) {
    return PxAdd(origin, PxScale(direction, distance, PX_UNIT_SHIFT));
}

// 押す物を選ぶ途中の比べ方: 距離 distance の物が、今の一番(best。まだ無ければ PX_RAY_MISS)より手前か
// (番号の順に調べるので、同じ距離なら先の = 番号の小さい物が残る)
FX_FN bool PxRayHitCloser(int64_t distance, int64_t best) {
    return distance != PX_RAY_MISS && (best == PX_RAY_MISS || distance < best);
}

// 光線の向き(Q1.30)と力積の大きさ(mN·s)から、力積のベクトル(2^-20 N·s)
FX_FN PxVec3 PxPushImpulse(PxVec3 direction, uint32_t impulseMillinewtonSeconds) {
    const int64_t magnitude = FxDivS64((int64_t)impulseMillinewtonSeconds << POSITION_FRACTION_BITS, 1000);
    return PxScale(direction, magnitude, PX_UNIT_SHIFT);
}

PX_NAMESPACE_END

#endif  // BICAMERAL_PHYSICS_PUSH_HLSLI
