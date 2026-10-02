// physics_step.hlsl — 整数の AVBD の 1 刻み(08 §2。T-0090)を GPU の Compute で走らせるパスの列。入口ごとに 1 つの .cso(shaders/CMakeLists.txt)。
// 手順は shaders/common/physics_step.hlsli(CPU の engine/src/sim/physics_world.cpp と共通)、呼ぶ順番と結び付けは engine/src/sim/gpu_physics.cpp。
//
// 組の置き場所: 組は「持ち主」の物の枠(1 物 slotsPerBody 個)に、相手の番号の昇順で入る(持ち主 = 片方が動かない物なら動く方、それ以外は小さい番号)。
// 枠は 2 組あり(currentHalf が今の小刻み、もう片方が前の小刻み)、前の小刻みの同じ組から引き継ぐ。点が 0 の枠は「組が無い」。
// 動く物は、自分が入っている組の枠の一覧(incident)を持つ。並びは原子的な加算の順で決まらないが、読む側(6×6 の和・彩色の隣)は順番に依存しない。
// 枠・一覧・色が足りなかったら統計の overflow に印を付ける(テストが落とす。M1 の原理の確認なので数は固定。上限なしは T-0045)。
#include "common/physics_step.hlsli"

// 組(PxManifold、3016 バイト)は構造化バッファの要素の上限(2048 バイト)を超えるので、見出しと点に分けて置く
struct ManifoldHeader {
    uint32_t bodyA;
    uint32_t bodyB;
    uint32_t count;
    uint32_t unused;
    PxVec3 normal;
    int64_t frictionQ16;
    int64_t beta;
    int64_t minPenalty;
    int64_t maxPenalty;
};

// --- 結び付け(gpu_physics.cpp の GpuPhysicsBinding と同じ順)---
// u1 組の見出し [2][bodyCount × slotsPerBody] / u2 持ち主の枠の使っている数 [2][bodyCount] /
// u3 枠の相手の番号(今の小刻み)[bodyCount × slotsPerBody] / u4 物の組の一覧(枠の番号 | 物が A なら INCIDENT_IS_A)[bodyCount × incidentPerBody] /
// u5 一覧の数 [bodyCount] / u6 β の「触れている質量」(mg)[bodyCount] / u7 彩色の回ごとの色(交互に読み書き)[2][bodyCount] /
// u8 統計(GpuPhysicsStats)/ u9 組の点 [2][bodyCount × slotsPerBody][PX_MANIFOLD_POINTS] / t0 パラメータ 1 個 / t1 場面から作った初めの物
RWStructuredBuffer<PxBody> g_bodies : register(u0);
RWStructuredBuffer<ManifoldHeader> g_manifolds : register(u1);
RWStructuredBuffer<uint32_t> g_slotCounts : register(u2);
RWStructuredBuffer<uint32_t> g_candidates : register(u3);
RWStructuredBuffer<uint32_t> g_incident : register(u4);
RWStructuredBuffer<uint32_t> g_incidentCounts : register(u5);
RWStructuredBuffer<uint64_t> g_touchMass : register(u6);
RWStructuredBuffer<int32_t> g_colors : register(u7);
RWByteAddressBuffer g_stats : register(u8);
RWStructuredBuffer<PxContactPoint> g_points : register(u9);
StructuredBuffer<PxParameters> g_parameters : register(t0);
StructuredBuffer<PxBody> g_initialBodies : register(t1);

cbuffer RootConstants : register(b0) {
    uint32_t g_bodyCount;
    uint32_t g_slotsPerBody;
    uint32_t g_incidentPerBody;
    uint32_t g_currentHalf;  // 今の小刻みの組の枠(0 / 1)
    uint32_t g_tickLow;
    uint32_t g_tickHigh;
    uint32_t g_frictionQ16;
    int32_t g_color;        // SolveColor が解く色
    uint32_t g_alphaQ16;    // 本反復は 65536、最後の 1 回は 0
    uint32_t g_colorIn;     // ColorRound が読む色の組(0 / 1)。FinishColoring は最後の回の結果の組
    uint32_t g_resetStats;  // 1 なら BeginSubstep が統計を 0 にする(刻みの最初の小刻み)
};

// --- 統計の並び(GpuPhysicsStats)---
static const uint32_t STATS_MAX_PENETRATION = 0;  // uint64
static const uint32_t STATS_CONTACT_COUNT = 8;
static const uint32_t STATS_COLOR_COUNT = 12;
static const uint32_t STATS_SOLVE_FAILURES = 16;
static const uint32_t STATS_OVERFLOW = 20;

// --- overflow の印 ---
static const uint32_t OVERFLOW_SLOTS = 1;      // 持ち主の枠が足りない
static const uint32_t OVERFLOW_INCIDENT = 2;   // 物の組の一覧が足りない
static const uint32_t OVERFLOW_UNCOLORED = 4;  // 彩色の回数が足りない(塗れなかった物がある)
static const uint32_t OVERFLOW_COLORS = 8;     // 色の数が PX_GPU_MAX_COLORS を超えた

static const uint32_t INCIDENT_IS_A = 0x80000000u;

// --- 小さな部品 -------------------------------------------------------------------------------
uint32_t ManifoldIndex(uint32_t manifoldHalf, uint32_t slot) {
    return manifoldHalf * g_bodyCount * g_slotsPerBody + slot;
}

uint64_t CurrentTick() {
    return ((uint64_t)g_tickHigh << 32) | (uint64_t)g_tickLow;
}

void Flag(uint32_t bit) {
    g_stats.InterlockedOr(STATS_OVERFLOW, bit);
}

// 今の小刻みの、点がある組の枠か
bool IsLiveSlot(uint32_t slot) {
    if (slot >= g_bodyCount * g_slotsPerBody)
        return false;

    const uint32_t owner = slot / g_slotsPerBody;
    const uint32_t k = slot % g_slotsPerBody;
    if (k >= g_slotCounts[g_currentHalf * g_bodyCount + owner])
        return false;

    return g_manifolds[ManifoldIndex(g_currentHalf, slot)].count > 0;
}

uint32_t PointIndex(uint32_t manifoldIndex, uint32_t k) {
    return manifoldIndex * PX_MANIFOLD_POINTS + k;
}

PxManifold LoadManifold(uint32_t index) {
    const ManifoldHeader header = g_manifolds[index];
    PxManifold manifold = (PxManifold)0;
    manifold.bodyA = header.bodyA;
    manifold.bodyB = header.bodyB;
    manifold.count = header.count;
    manifold.normal = header.normal;
    manifold.frictionQ16 = header.frictionQ16;
    manifold.beta = header.beta;
    manifold.minPenalty = header.minPenalty;
    manifold.maxPenalty = header.maxPenalty;
    for (uint32_t k = 0; k < header.count; ++k)
        manifold.points[k] = g_points[PointIndex(index, k)];

    return manifold;
}

void StoreManifold(uint32_t index, PxManifold manifold) {
    ManifoldHeader header;
    header.bodyA = manifold.bodyA;
    header.bodyB = manifold.bodyB;
    header.count = manifold.count;
    header.unused = 0;
    header.normal = manifold.normal;
    header.frictionQ16 = manifold.frictionQ16;
    header.beta = manifold.beta;
    header.minPenalty = manifold.minPenalty;
    header.maxPenalty = manifold.maxPenalty;
    g_manifolds[index] = header;
    for (uint32_t k = 0; k < manifold.count; ++k)
        g_points[PointIndex(index, k)] = manifold.points[k];
}

// 前の小刻みの同じ組(持ち主の枠を探す。無ければ count = 0)
PxManifold FindPrevious(uint32_t owner, uint32_t a, uint32_t b) {
    const uint32_t previousHalf = 1 - g_currentHalf;
    const uint32_t count = g_slotCounts[previousHalf * g_bodyCount + owner];
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t index = ManifoldIndex(previousHalf, owner * g_slotsPerBody + k);
        if (g_manifolds[index].count > 0 && g_manifolds[index].bodyA == a && g_manifolds[index].bodyB == b)
            return LoadManifold(index);
    }

    return (PxManifold)0;
}

void AddIncident(uint32_t body, uint32_t entry) {
    uint32_t position = 0;
    InterlockedAdd(g_incidentCounts[body], 1, position);
    if (position >= g_incidentPerBody) {
        Flag(OVERFLOW_INCIDENT);
        return;
    }

    g_incident[body * g_incidentPerBody + position] = entry;
}

uint32_t IncidentCount(uint32_t body) {
    return min(g_incidentCounts[body], g_incidentPerBody);
}

// 組の一覧の 1 つの相手(動く物どうしの組だけが彩色の隣)
uint32_t PartnerOf(uint32_t entry) {
    const uint32_t index = ManifoldIndex(g_currentHalf, entry & ~INCIDENT_IS_A);
    return (entry & INCIDENT_IS_A) != 0 ? g_manifolds[index].bodyB : g_manifolds[index].bodyA;
}

// --- 初め: 場面の物を写し、組の枠を空にする ---------------------------------------------------------
[numthreads(64, 1, 1)] void Initialize(uint3 id : SV_DispatchThreadID) {
    const uint32_t i = id.x;
    if (i >= g_bodyCount)
        return;

    g_bodies[i] = g_initialBodies[i];
    g_slotCounts[i] = 0;
    g_slotCounts[g_bodyCount + i] = 0;
}

    // --- 小刻みの初め: 活性・物の初期化・β と一覧と色の初め ------------------------------------------------
    [numthreads(64, 1, 1)] void BeginSubstep(uint3 id : SV_DispatchThreadID) {
    const uint32_t i = id.x;
    if (i == 0 && g_resetStats != 0) {
        g_stats.Store<uint64_t>(STATS_MAX_PENETRATION, 0);
        g_stats.Store3(STATS_CONTACT_COUNT, uint3(0, 0, 0));  // overflow は残す(テストが最後に見る)
    }

    if (i >= g_bodyCount)
        return;

    PxBody body = g_bodies[i];
    body.active = PxIsActiveAt(body, CurrentTick());
    body = PxInitializeBody(body, PxStepRate(g_parameters[0]));
    g_bodies[i] = body;
    g_touchMass[i] = PxTouchMass(body);
    g_incidentCounts[i] = 0;
    g_colors[i] = PX_NO_COLOR;  // 彩色の最初の回は 0 の組から読む
}

// --- 広域の選別: 持ち主の物ごとに、近い相手を番号の昇順に枠へ ------------------------------------------
[numthreads(64, 1, 1)] void Broadphase(uint3 id : SV_DispatchThreadID) {
    const uint32_t i = id.x;
    if (i >= g_bodyCount)
        return;

    const PxParameters p = g_parameters[0];
    const int64_t rate = PxStepRate(p);
    const PxBody self = g_bodies[i];
    uint32_t count = 0;
    for (uint32_t j = 0; j < g_bodyCount && PxIsDynamic(self) && self.active != 0; ++j) {
        if (j == i)
            continue;

        const PxBody other = g_bodies[j];
        if (PxIsDynamic(other) && j < i)  // この組の持ち主は j
            continue;

        int64_t margin = 0;
        if (i < j)
            margin = PxPairMargin(self, other, p, rate);
        else
            margin = PxPairMargin(other, self, p, rate);

        if (margin < 0)
            continue;

        if (count == g_slotsPerBody) {
            Flag(OVERFLOW_SLOTS);
            continue;
        }

        g_candidates[i * g_slotsPerBody + count] = j;
        count += 1;
    }

    g_slotCounts[g_currentHalf * g_bodyCount + i] = count;
}

    // --- 接触の生成: 枠ごとに接触を作り、前の小刻みの同じ組から引き継ぐ。β の質量と物の一覧も -----------------------
    [numthreads(64, 1, 1)] void Narrowphase(uint3 id : SV_DispatchThreadID) {
    const uint32_t slot = id.x;
    if (slot >= g_bodyCount * g_slotsPerBody)
        return;

    const uint32_t owner = slot / g_slotsPerBody;
    const uint32_t index = ManifoldIndex(g_currentHalf, slot);
    if (slot % g_slotsPerBody >= g_slotCounts[g_currentHalf * g_bodyCount + owner]) {
        g_manifolds[index].count = 0;
        return;
    }

    const uint32_t other = g_candidates[slot];
    const uint32_t a = min(owner, other);
    const uint32_t b = max(owner, other);
    const PxBody bodyA = g_bodies[a];
    const PxBody bodyB = g_bodies[b];
    const PxParameters p = g_parameters[0];
    const int64_t rate = PxStepRate(p);
    const int64_t margin = PxPairMargin(bodyA, bodyB, p, rate);
    const PxContactGeometry geometry = PxCollideBoxes(PxShapeOf(bodyA), PxShapeOf(bodyB), margin);
    if (geometry.count == 0) {
        g_manifolds[index].count = 0;
        return;
    }

    const PxManifold manifold = PxBuildManifold(a, b, bodyA, bodyB, geometry, (int64_t)g_frictionQ16,
                                                FindPrevious(owner, a, b), p, rate);
    StoreManifold(index, manifold);
    g_stats.InterlockedMax64(STATS_MAX_PENETRATION, (uint64_t)PxMaxPenetration(geometry));
    g_stats.InterlockedAdd(STATS_CONTACT_COUNT, manifold.count);
    InterlockedMax(g_touchMass[a], PxTouchMass(bodyB));
    InterlockedMax(g_touchMass[b], PxTouchMass(bodyA));
    if (PxIsDynamic(bodyA))
        AddIncident(a, slot | INCIDENT_IS_A);

    if (PxIsDynamic(bodyB))
        AddIncident(b, slot);
}

// --- β・硬さの引き継ぎ・線形化(組ごと)-------------------------------------------------------------
[numthreads(64, 1, 1)] void PrepareManifolds(uint3 id : SV_DispatchThreadID) {
    const uint32_t slot = id.x;
    if (!IsLiveSlot(slot))
        return;

    const PxParameters p = g_parameters[0];
    const uint32_t index = ManifoldIndex(g_currentHalf, slot);
    PxManifold manifold = LoadManifold(index);
    manifold.beta = PxBetaOf(PxMaxU64(g_touchMass[manifold.bodyA], g_touchMass[manifold.bodyB]), p);
    manifold = PxWarmStartManifold(manifold, p.gammaQ16);
    manifold = PxLinearizeManifold(manifold, g_bodies[manifold.bodyA], g_bodies[manifold.bodyB]);
    StoreManifold(index, manifold);
}

// --- 彩色の 1 回(Jones-Plassmann): 前の回の色だけを読み、次の組に書く --------------------------------------
// まだ色の無い動く物のうち、色の無い隣のどれよりも優先度が高い物に、隣が使っていない一番小さい色を付ける
bool IsLocalMaximum(uint32_t i, uint32_t inBase) {
    const uint32_t count = IncidentCount(i);
    for (uint32_t e = 0; e < count; ++e) {
        const uint32_t j = PartnerOf(g_incident[i * g_incidentPerBody + e]);
        if (g_bodies[j].massMilligrams == 0)
            continue;

        if (g_colors[inBase + j] < 0 && !PxColorPriorityBelow(j, i))
            return false;
    }

    return true;
}

bool IsColorUsedByNeighbor(uint32_t i, uint32_t inBase, int32_t color) {
    const uint32_t count = IncidentCount(i);
    for (uint32_t e = 0; e < count; ++e) {
        const uint32_t j = PartnerOf(g_incident[i * g_incidentPerBody + e]);
        if (g_bodies[j].massMilligrams != 0 && g_colors[inBase + j] == color)
            return true;
    }

    return false;
}

[numthreads(64, 1, 1)] void ColorRound(uint3 id : SV_DispatchThreadID) {
    const uint32_t i = id.x;
    if (i >= g_bodyCount)
        return;

    const uint32_t inBase = g_colorIn * g_bodyCount;
    const uint32_t outBase = (1 - g_colorIn) * g_bodyCount;
    int32_t color = g_colors[inBase + i];
    const PxBody body = g_bodies[i];
    if (color < 0 && PxIsDynamic(body) && body.active != 0 && IsLocalMaximum(i, inBase)) {
        color = 0;
        while (IsColorUsedByNeighbor(i, inBase, color))
            color += 1;
    }

    g_colors[outBase + i] = color;
}

    // 最後の回の色を物に移す。塗れなかった物・色が多すぎるのは印
    [numthreads(64, 1, 1)] void FinishColoring(uint3 id : SV_DispatchThreadID) {
    const uint32_t i = id.x;
    if (i >= g_bodyCount)
        return;

    const int32_t color = g_colors[g_colorIn * g_bodyCount + i];
    g_bodies[i].color = color;
    if (color < 0) {
        if (g_bodies[i].massMilligrams != 0 && g_bodies[i].active != 0)
            Flag(OVERFLOW_UNCOLORED);

        return;
    }

    g_stats.InterlockedMax(STATS_COLOR_COUNT, (uint32_t)(color + 1));
    if (color >= PX_GPU_MAX_COLORS)
        Flag(OVERFLOW_COLORS);
}

// --- 反復の途中の探し直し(組ごと)----------------------------------------------------------------
[numthreads(64, 1, 1)] void Recollide(uint3 id : SV_DispatchThreadID) {
    const uint32_t slot = id.x;
    if (!IsLiveSlot(slot))
        return;

    const uint32_t index = ManifoldIndex(g_currentHalf, slot);
    PxManifold manifold = LoadManifold(index);
    const uint32_t before = manifold.count;
    manifold = PxRecollideManifold(manifold, g_bodies[manifold.bodyA], g_bodies[manifold.bodyB], g_parameters[0]);
    if (manifold.count == before)
        return;

    StoreManifold(index, manifold);
    g_stats.InterlockedAdd(STATS_CONTACT_COUNT, manifold.count - before);
}

    // --- 1 つの色の物を解く(同じ色の物は拘束を共有しないので並列に解ける)----------------------------------------
    [numthreads(64, 1, 1)] void SolveColor(uint3 id : SV_DispatchThreadID) {
    const uint32_t i = id.x;
    if (i >= g_bodyCount || g_bodies[i].color != g_color)
        return;

    const PxParameters p = g_parameters[0];
    const int64_t alphaQ16 = (int64_t)g_alphaQ16;
    PxBody body = g_bodies[i];
    PxBodySystem system = PxBeginSystemOf(body);
    const uint32_t count = IncidentCount(i);
    for (uint32_t e = 0; e < count; ++e) {
        const uint32_t entry = g_incident[i * g_incidentPerBody + e];
        const bool isA = (entry & INCIDENT_IS_A) != 0;
        const uint32_t index = ManifoldIndex(g_currentHalf, entry & ~INCIDENT_IS_A);
        const uint32_t partner = isA ? g_manifolds[index].bodyB : g_manifolds[index].bodyA;
        const PxVec3 partnerLinear = g_bodies[partner].deltaLinear;
        const PxVec3 partnerAngular = g_bodies[partner].deltaAngular;
        const PxVec3 linearA = PxSelect(isA, body.deltaLinear, partnerLinear);
        const PxVec3 angularA = PxSelect(isA, body.deltaAngular, partnerAngular);
        const PxVec3 linearB = PxSelect(isA, partnerLinear, body.deltaLinear);
        const PxVec3 angularB = PxSelect(isA, partnerAngular, body.deltaAngular);
        const int64_t frictionQ16 = g_manifolds[index].frictionQ16;
        const uint32_t pointCount = g_manifolds[index].count;
        for (uint32_t k = 0; k < pointCount; ++k) {
            system = PxAddPointRows(system, g_points[PointIndex(index, k)], frictionQ16, isA, alphaQ16, p.gapSlop,
                                    linearA, angularA, linearB, angularB);
        }
    }

    const PxSolveResult solved = PxSolveSymmetric6(system.lhs, system.rhs, PX_SOLVE_GAIN_SHIFT);
    if (!solved.ok)
        g_stats.InterlockedAdd(STATS_SOLVE_FAILURES, 1);

    body = PxApplySolution(body, solved.x);
    g_bodies[i].deltaLinear = body.deltaLinear;
    g_bodies[i].deltaAngular = body.deltaAngular;
}

// --- λ と硬さの更新(組ごと)-------------------------------------------------------------------------
[numthreads(64, 1, 1)] void UpdateDuals(uint3 id : SV_DispatchThreadID) {
    const uint32_t slot = id.x;
    if (!IsLiveSlot(slot))
        return;

    const PxParameters p = g_parameters[0];
    const int64_t alphaQ16 = (int64_t)g_alphaQ16;
    const uint32_t index = ManifoldIndex(g_currentHalf, slot);
    const uint32_t a = g_manifolds[index].bodyA;
    const uint32_t b = g_manifolds[index].bodyB;
    const PxVec3 linearA = g_bodies[a].deltaLinear;
    const PxVec3 angularA = g_bodies[a].deltaAngular;
    const PxVec3 linearB = g_bodies[b].deltaLinear;
    const PxVec3 angularB = g_bodies[b].deltaAngular;
    const int64_t frictionQ16 = g_manifolds[index].frictionQ16;
    const int64_t beta = g_manifolds[index].beta;
    const int64_t maxPenalty = g_manifolds[index].maxPenalty;
    const uint32_t count = g_manifolds[index].count;
    for (uint32_t k = 0; k < count; ++k) {
        g_points[PointIndex(index, k)] = PxUpdatePointDuals(g_points[PointIndex(index, k)], frictionQ16, beta,
                                                            maxPenalty, alphaQ16, p, linearA, angularA, linearB,
                                                            angularB);
    }
}

    // --- 刻みの終わり(物ごと)--------------------------------------------------------------------------
    [numthreads(64, 1, 1)] void UpdateVelocity(uint3 id : SV_DispatchThreadID) {
    const uint32_t i = id.x;
    if (i >= g_bodyCount)
        return;

    g_bodies[i] = PxUpdateVelocity(g_bodies[i], PxStepRate(g_parameters[0]));
}

[numthreads(64, 1, 1)] void Finish(uint3 id : SV_DispatchThreadID) {
    const uint32_t i = id.x;
    if (i >= g_bodyCount)
        return;

    g_bodies[i] = PxFinishBody(g_bodies[i]);
}
