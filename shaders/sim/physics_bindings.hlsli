// physics_bindings.hlsli — 物理の 1 刻みのシェーダー(physics_step.hlsl の Compute と physics_graph.hlsl の Work Graph)が共有する
// バッファの結び方・ルート定数・組の置き場所の部品。ルート署名は engine/src/sim/gpu_physics.cpp の ROOT_LAYOUT と同じ順。
// Work Graph のノードも同じグローバルのルート署名で動く(ノードごとのローカルのルート署名は使わない)。
//
// 組の置き場所: 組は「持ち主」の物の枠(1 物 slotsPerBody 個)に、相手の番号の昇順で入る(持ち主 = 片方が動かない物なら動く方、それ以外は小さい番号)。
// 枠は 2 組あり(currentHalf が今の小刻み、もう片方が前の小刻み)、前の小刻みの同じ組から引き継ぐ。点が 0 の枠は「組が無い」。
// 動く物は、自分が入っている組の枠の一覧(incident)を持つ。並びは原子的な加算の順で決まらないが、読む側(6×6 の和・彩色の隣)は順番に依存しない。
// 枠・一覧・色が足りなかったら統計の overflow に印を付ける(テストが落とす。M1 の原理の確認なので数は固定。上限なしは T-0045)。
#ifndef BICAMERAL_PHYSICS_BINDINGS_HLSLI
#define BICAMERAL_PHYSICS_BINDINGS_HLSLI

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
// u8 統計(GpuPhysicsStats)/ u9 組の点 [2][bodyCount × slotsPerBody][PX_MANIFOLD_POINTS] /
// u10 枠の接触の幾何(Work Graph の NarrowphaseNode → Compute の BuildManifolds。T-0092)[bodyCount × slotsPerBody] /
// u11 色ごとの Work Graph の GPU の入力(D3D12_NODE_GPU_INPUT × PX_GPU_MAX_COLORS)/ u12 色ごとの物の一覧 [PX_GPU_MAX_COLORS][bodyCount] /
// u13 島(physics_islands.hlsli の ISLAND_*。T-0094)/ u14 大きな島がある(uint64。全体の方式のパスの述語)/
// u15 大きな島の物が使う色(uint64 × PX_GPU_MAX_COLORS。色ごとの解の述語)/
// t0 パラメータ 1 個 / t1 場面から作った初めの物
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
RWStructuredBuffer<PxContactGeometry> g_geometry : register(u10);
RWByteAddressBuffer g_colorInputs : register(u11);
RWStructuredBuffer<uint32_t> g_colorBodies : register(u12);
RWStructuredBuffer<uint32_t> g_islands : register(u13);
RWByteAddressBuffer g_islandPredicate : register(u14);
RWByteAddressBuffer g_colorPredicates : register(u15);
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
    uint32_t g_solveEntry;  // physics_graph.hlsl の SolveBodyNode の入口の番号(Initialize が色ごとの GPU の入力に書く)
    uint32_t g_colorBodiesLow;  // u12 の GPU の仮想アドレス
    uint32_t g_colorBodiesHigh;
    uint32_t g_islandMode;       // 0 = 全部を全体の方式で / 1 = 全体の方式のパスは大きな島の物だけ(T-0094)
    uint32_t g_islandBodyLimit;  // これより物の多い島は全体の方式(大きな島)
};

// --- 色ごとの GPU の入力(D3D12_NODE_GPU_INPUT: 入口・レコードの数・レコードのアドレスと間隔、24 バイト)---
static const uint32_t COLOR_INPUT_BYTES = 24;
static const uint32_t COLOR_RECORD_STRIDE_BYTES = 4;  // レコード = 物の番号 1 つ

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

// --- 島(u13。T-0094)の並び: 見出し 4 語 + 物の数ずつの区画 ---
// 見出し [0] グループで解く島の数 [1] 大きな島の物の数 [2] 島の物の並びの次の空き [3] 予備。
// 区画(どれも物の数の長さ): 物ごとの「つながりの印」(島の一番小さい物の番号。島に入らない物は ISLAND_NONE。CPU の PhysicsWorld::IslandLabels と同じ)/
// 物ごとの島の番号(グループで解く島の番号・ISLAND_LARGE・ISLAND_NONE)/ 印ごとの物の数 / 島ごとの物の並びの始まり / 島ごとの物の数 /
// 島ごとの物の並び / 島ごとの色の順の物の並び(島の始まりから)
static const uint32_t ISLAND_HEADER_WORDS = 4;
static const uint32_t ISLAND_HEADER_COUNT = 0;
static const uint32_t ISLAND_HEADER_LARGE_BODIES = 1;
static const uint32_t ISLAND_HEADER_CURSOR = 2;

static const uint32_t ISLAND_LABEL = 0;
static const uint32_t ISLAND_OF = 1;
static const uint32_t ISLAND_ROOT_SIZE = 2;
static const uint32_t ISLAND_START = 3;
static const uint32_t ISLAND_SIZE = 4;
static const uint32_t ISLAND_BODIES = 5;
static const uint32_t ISLAND_COLOR_BODIES = 6;
static const uint32_t ISLAND_REGIONS = 7;  // gpu_physics.cpp の ISLAND_REGIONS と同じ

static const uint32_t ISLAND_NONE = 0xFFFFFFFFu;   // 島に入らない(動かない物・世界にいない物)
static const uint32_t ISLAND_LARGE = 0xFFFFFFFEu;  // 大きな島(全体の方式で解く)

uint32_t IslandWord(uint32_t region, uint32_t index) {
    return ISLAND_HEADER_WORDS + region * g_bodyCount + index;
}

// 全体の方式のパス(色ごとの Dispatch)が扱う物か。島の方式では大きな島の物だけ(ほかは島ごとのグループが解く)
bool IsGlobalBody(uint32_t i) {
    return g_islandMode == 0 || g_islands[IslandWord(ISLAND_OF, i)] == ISLAND_LARGE;
}

// 枠は持ち主の物と同じ島(持ち主は動く物。両方が動く物なら同じ島)
bool IsGlobalSlot(uint32_t slot) {
    return IsGlobalBody(slot / g_slotsPerBody);
}

// 組(PxManifold、3 KB)を丸ごと局所に持つ関数は置かない: 組を局所に持つ Compute のパスは、別のキューの仕事(窓の描画)が割り込むと
// 結果が時々変わった(T-0098。局所のメモリの扱いが原因と見ているが未確認)。組はバッファの上で点ごとに読み書きする
// (BuildSlot・Recollide・PrepareManifolds。CPU は PxBuildManifold・PxRecollideManifold・PxWarmStartManifold で同じ手順)

// 前の小刻みの組を、点を 1 つずつバッファから読む形で渡す(PxBuildManifold の Previous。組を局所に写さない)
struct GpuPreviousManifold {
    uint32_t index;  // 組の番号(ManifoldIndex)
    uint32_t count;  // 0 なら前の組は無い

    uint32_t Count() { return count; }

    PxContactPoint Point(uint32_t j) { return g_points[PointIndex(index, j)]; }
};

// 前の小刻みの同じ組(持ち主の枠を探す。無ければ count = 0)
GpuPreviousManifold FindPrevious(uint32_t owner, uint32_t a, uint32_t b) {
    const uint32_t previousHalf = 1 - g_currentHalf;
    const uint32_t count = g_slotCounts[previousHalf * g_bodyCount + owner];
    GpuPreviousManifold previous;
    previous.index = 0;
    previous.count = 0;
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t index = ManifoldIndex(previousHalf, owner * g_slotsPerBody + k);
        if (g_manifolds[index].count > 0 && g_manifolds[index].bodyA == a && g_manifolds[index].bodyB == b) {
            previous.index = index;
            previous.count = g_manifolds[index].count;
            return previous;
        }
    }

    return previous;
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

// --- 広域の選別と接触の生成(Compute の Broadphase・Narrowphase と Work Graph の physics_graph.hlsl が共有)-----------------
// 持ち主 i の、近い相手を番号の昇順に g_candidates へ。枠の数を返す
uint32_t CollectCandidates(uint32_t i) {
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

    return count;
}

// 枠(相手がいる)の接触の幾何。点が無ければ count = 0
PxContactGeometry CollideGeometry(uint32_t slot, uint32_t other) {
    const uint32_t owner = slot / g_slotsPerBody;
    const PxBody bodyA = g_bodies[min(owner, other)];
    const PxBody bodyB = g_bodies[max(owner, other)];
    const PxParameters p = g_parameters[0];
    const int64_t margin = PxPairMargin(bodyA, bodyB, p, PxStepRate(p));

    return PxCollideBoxes(PxShapeOf(bodyA), PxShapeOf(bodyB), margin);
}

// PxMatchByProximity と同じ手順を、今の組の点をバッファの上で読み書きして(組を局所に持たない。BuildSlot)。
// 片方を変えたらもう片方も変える(gpu_physics_test・gpu_probe_physics_test が CPU とのビット一致で確かめる)
void MatchByProximityInBuffer(uint32_t index, uint32_t count, PxQuat rotationA, GpuPreviousManifold old,
                              int64_t proximity) {
    uint32_t usedOld = 0;
    uint32_t matchedNew = 0;
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t feature = g_points[PointIndex(index, k)].feature;
        for (uint32_t j = 0; j < old.Count(); ++j) {
            if (old.Point(j).feature != feature)
                continue;

            usedOld |= 1u << j;
            matchedNew |= 1u << k;
        }
    }

    const PxMat3 rotation = PxRotationMatrix(rotationA);
    for (uint32_t k = 0; k < count; ++k) {
        if ((matchedNew & (1u << k)) != 0)
            continue;

        PxContactPoint current = g_points[PointIndex(index, k)];
        const PxVec3 here = PxMulMat(rotation, current.localA, PX_UNIT_SHIFT);
        int32_t nearestIndex = -1;
        int64_t nearest = proximity;
        for (uint32_t j = 0; j < old.Count(); ++j) {
            const PxContactPoint oldPoint = old.Point(j);
            const PxVec3 there = PxMulMat(rotation, oldPoint.localA, PX_UNIT_SHIFT);
            const int64_t distance = (int64_t)PxLength(PxSub(here, there));
            const bool similar = PxDot(oldPoint.normal, current.normal, PX_UNIT_SHIFT) >= PX_NORMAL_SIMILARITY;
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
            current.rows[r].lambda = nearestPoint.rows[r].lambda;
            current.rows[r].penalty = nearestPoint.rows[r].penalty;
        }

        g_points[PointIndex(index, k)] = current;
    }
}

// 接触の幾何から組を作り、前の小刻みの同じ組から引き継ぐ。統計・β の質量・物の一覧も。
// PxBuildManifold と同じ手順を、組(3 KB)を局所に持たずにバッファの上で(T-0098: 組を局所に持つ Compute のパスは、
// 別のキューの仕事(描画)が割り込むと結果が時々変わった)
void BuildSlot(uint32_t slot, uint32_t other, PxContactGeometry geometry) {
    const uint32_t owner = slot / g_slotsPerBody;
    const uint32_t index = ManifoldIndex(g_currentHalf, slot);
    if (geometry.count == 0) {
        g_manifolds[index].count = 0;
        return;
    }

    const uint32_t a = min(owner, other);
    const uint32_t b = max(owner, other);
    const PxBody bodyA = g_bodies[a];
    const PxBody bodyB = g_bodies[b];
    const PxParameters p = g_parameters[0];
    const PxPenaltyRange range = PxPairPenaltyRange(bodyA, bodyB, p, PxStepRate(p));

    ManifoldHeader header;
    header.bodyA = a;
    header.bodyB = b;
    header.count = geometry.count;
    header.unused = 0;
    header.normal = geometry.normal;
    header.frictionQ16 = (int64_t)g_frictionQ16;
    header.beta = 0;
    header.minPenalty = range.low;
    header.maxPenalty = range.high;
    g_manifolds[index] = header;

    const PxBox shapeA = PxShapeOf(bodyA);
    const PxBox shapeB = PxShapeOf(bodyB);
    const GpuPreviousManifold previous = FindPrevious(owner, a, b);
    for (uint32_t k = 0; k < geometry.count; ++k) {
        const PxContactPoint contactPoint = PxMakeContactPoint(geometry.points[k], geometry.normal, shapeA, shapeB,
                                                               range.low);
        g_points[PointIndex(index, k)] = PxInheritFromPrevious(contactPoint, previous);
    }

    MatchByProximityInBuffer(index, geometry.count, bodyA.rotation, previous, p.proximityMatch);

    g_stats.InterlockedMax64(STATS_MAX_PENETRATION, (uint64_t)PxMaxPenetration(geometry));
    g_stats.InterlockedAdd(STATS_CONTACT_COUNT, geometry.count);
    InterlockedMax(g_touchMass[a], PxTouchMass(bodyB));
    InterlockedMax(g_touchMass[b], PxTouchMass(bodyA));
    if (PxIsDynamic(bodyA))
        AddIncident(a, slot | INCIDENT_IS_A);

    if (PxIsDynamic(bodyB))
        AddIncident(b, slot);
}

// --- 1 つの物を 1 グループで解く(1 スレッド = 1 接触点。T-0092。Compute の SolveColor と Work Graph の SolveBodyNode)---------------
// 1 スレッド = 1 物で順に足す形(T-0090)と同じ結果: 物の 6×6 は点ごとの寄与の整数の和(2^64 を法とする和なので順番に依存しない)。
// 点ごとの寄与をスレッドで並列に作り、ウェーブと共有メモリで足してから、1 スレッドが慣性の項を足して解く。
// 島ごとに解くグループ(physics_islands.hlsli。T-0094)は、グループを SOLVE_GROUP_THREADS ずつの「組」に分け、組ごとに 1 物を解く
static const uint32_t SOLVE_GROUP_THREADS = 64;
static const uint32_t SOLVE_SYSTEM_WORDS = 42;  // 6×6 + 6

// 島ごとに解くグループのスレッド数と、1 物を解くスレッド数(SolveIslands の .cso ごとに -D で決める。shaders/CMakeLists.txt)。
// グループの中で同時に解く物の数 = ISLAND_GROUP_THREADS / SOLVE_LANES(ほかの入口は 64 / 64 = 1)
#ifndef ISLAND_GROUP_THREADS
#define ISLAND_GROUP_THREADS 64
#endif
#ifndef ISLAND_SOLVE_LANES
#define ISLAND_SOLVE_LANES 64
#endif
static const uint32_t SOLVE_LANES = ISLAND_SOLVE_LANES;
static const uint32_t SOLVE_GROUP_MANIFOLDS = SOLVE_LANES / PX_MANIFOLD_POINTS;  // 1 回にまとめて見る組の数
static const uint32_t SOLVE_SUBGROUPS = ISLAND_GROUP_THREADS / SOLVE_LANES;

groupshared uint64_t g_systemSum[SOLVE_SUBGROUPS][SOLVE_SYSTEM_WORDS];

// 物 i の組の一覧の e 番目の組の k 番目の点の 3 行(点が無ければ 0)
PxBodySystem PointContribution(uint32_t i, PxBody body, uint32_t e, uint32_t k, int64_t alphaQ16, PxParameters p) {
    const uint32_t entry = g_incident[i * g_incidentPerBody + e];
    const bool isA = (entry & INCIDENT_IS_A) != 0;
    const uint32_t index = ManifoldIndex(g_currentHalf, entry & ~INCIDENT_IS_A);
    PxBodySystem contribution = (PxBodySystem)0;
    if (k >= g_manifolds[index].count)
        return contribution;

    const uint32_t partner = isA ? g_manifolds[index].bodyB : g_manifolds[index].bodyA;
    const PxVec3 partnerLinear = g_bodies[partner].deltaLinear;
    const PxVec3 partnerAngular = g_bodies[partner].deltaAngular;
    const PxVec3 linearA = PxSelect(isA, body.deltaLinear, partnerLinear);
    const PxVec3 angularA = PxSelect(isA, body.deltaAngular, partnerAngular);
    const PxVec3 linearB = PxSelect(isA, partnerLinear, body.deltaLinear);
    const PxVec3 angularB = PxSelect(isA, partnerAngular, body.deltaAngular);

    return PxAddPointRows(contribution, g_points[PointIndex(index, k)], g_manifolds[index].frictionQ16, isA, alphaQ16,
                          p.gapSlop, linearA, angularA, linearB, angularB);
}

// 1 つの値を組の和に足す。ウェーブが組の中に収まるなら(ウェーブの幅は 2 の冪なので SOLVE_LANES 以下なら収まる)ウェーブで足して代表が足す。
// 収まらなければ(ウェーブが 2 つの組にまたがる)スレッドごとに足す
void AddToSum(uint32_t subgroup, uint32_t word, int64_t value) {
    if (WaveGetLaneCount() > SOLVE_LANES) {
        InterlockedAdd(g_systemSum[subgroup][word], (uint64_t)value);
        return;
    }

    const uint64_t sum = WaveActiveSum((uint64_t)value);
    if (WaveIsFirstLane())
        InterlockedAdd(g_systemSum[subgroup][word], sum);
}

void AddToGroupSum(PxBodySystem system, uint32_t subgroup) {
    for (uint32_t v = 0; v < 36; ++v)
        AddToSum(subgroup, v, system.lhs.m[v]);

    for (uint32_t v = 0; v < 6; ++v)
        AddToSum(subgroup, 36 + v, system.rhs.v[v]);
}

// 物 i を組 subgroup(SOLVE_LANES 本のスレッド。lane は組の中の番号)で解く。グループの全部のスレッドが呼ぶ
// (中でグループのバリアを使う)。valid = false の組は何もしない
void SolveBodyInSubgroup(uint32_t i, uint32_t lane, uint32_t subgroup, bool valid, int64_t alphaQ16) {
    for (uint32_t word = lane; word < SOLVE_SYSTEM_WORDS; word += SOLVE_LANES)  // 1 物 64 本未満なら 1 本が 2 語以上
        g_systemSum[subgroup][word] = 0;

    GroupMemoryBarrierWithGroupSync();

    // --- 点ごとの寄与: スレッド lane は組 base + lane / 8 の点 lane % 8 ---
    const PxParameters p = g_parameters[0];
    PxBodySystem local = (PxBodySystem)0;
    if (valid) {
        const PxBody body = g_bodies[i];
        const uint32_t count = IncidentCount(i);
        for (uint32_t base = 0; base < count; base += SOLVE_GROUP_MANIFOLDS) {
            const uint32_t e = base + lane / PX_MANIFOLD_POINTS;
            if (e >= count)
                continue;

            const PxBodySystem contribution = PointContribution(i, body, e, lane % PX_MANIFOLD_POINTS, alphaQ16, p);
            for (uint32_t v = 0; v < 36; ++v)
                local.lhs.m[v] += contribution.lhs.m[v];

            for (uint32_t v = 0; v < 6; ++v)
                local.rhs.v[v] += contribution.rhs.v[v];
        }
    }

    AddToGroupSum(local, subgroup);
    GroupMemoryBarrierWithGroupSync();
    if (!valid || lane != 0)
        return;

    // --- 慣性の項 + 点の和を解く(SolveColor と同じ)---
    const PxBody body = g_bodies[i];
    PxBodySystem system = PxBeginSystemOf(body);
    for (uint32_t v = 0; v < 36; ++v)
        system.lhs.m[v] += (int64_t)g_systemSum[subgroup][v];

    for (uint32_t v = 0; v < 6; ++v)
        system.rhs.v[v] += (int64_t)g_systemSum[subgroup][36 + v];

    const PxSolveResult solved = PxSolveSymmetric6(system.lhs, system.rhs, PX_SOLVE_GAIN_SHIFT);
    if (!solved.ok)
        g_stats.InterlockedAdd(STATS_SOLVE_FAILURES, 1);

    const PxBody updated = PxApplySolution(body, solved.x);
    g_bodies[i].deltaLinear = updated.deltaLinear;
    g_bodies[i].deltaAngular = updated.deltaAngular;
}

// 物 i を 1 グループ(SOLVE_GROUP_THREADS)で解く。グループの全部のスレッドが呼ぶ
void SolveBodyInGroup(uint32_t i, uint32_t thread) {
    SolveBodyInSubgroup(i, thread, 0, true, (int64_t)g_alphaQ16);
}

// --- 組ごと・物ごとの手順(全体の方式のパスと、島ごとのグループ〔physics_islands.hlsli〕が共有)-----------------------
// 反復の途中の探し直し。PxRecollideManifold と同じ手順を、組(3 KB)を局所に写さずにバッファの上で(T-0098: 局所に写す形は、
// 別のキューの仕事が割り込むと結果が変わった。gpu_probe_physics_test の雑音つきの実行)
void RecollideSlot(uint32_t slot) {
    const uint32_t index = ManifoldIndex(g_currentHalf, slot);
    const ManifoldHeader header = g_manifolds[index];
    const PxBody bodyA = g_bodies[header.bodyA];
    const PxBody bodyB = g_bodies[header.bodyB];
    const PxParameters p = g_parameters[0];
    if (PxStepMotion(bodyA) + PxStepMotion(bodyB) <= p.recollideMinMotion)
        return;

    const PxBox shapeA = PxEstimatedShape(bodyA);
    const PxBox shapeB = PxEstimatedShape(bodyB);
    const PxContactGeometry geometry = PxCollideBoxes(shapeA, shapeB, p.collisionMargin);
    uint32_t count = header.count;
    for (uint32_t k = 0; k < geometry.count && count < PX_MANIFOLD_POINTS; ++k) {
        bool known = false;
        for (uint32_t j = 0; j < count; ++j)
            known = known || g_points[PointIndex(index, j)].feature == geometry.points[k].feature;

        if (known)
            continue;

        const PxContactPoint contactPoint = PxMakeContactPoint(geometry.points[k], geometry.normal, shapeA, shapeB,
                                                               header.minPenalty);
        g_points[PointIndex(index, count)] = PxLinearizePoint(contactPoint, bodyA, bodyB);
        count += 1;
    }

    if (count == header.count)
        return;

    g_manifolds[index].count = count;
    g_stats.InterlockedAdd(STATS_CONTACT_COUNT, count - header.count);
}

// λ と硬さの更新
void UpdateDualsSlot(uint32_t slot, int64_t alphaQ16) {
    const PxParameters p = g_parameters[0];
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

// --- 彩色の部品(Jones-Plassmann。全体の方式の ColorRound と島ごとのグループが共有)---
// まだ色の無い動く物のうち、色の無い隣のどれよりも優先度が高い物に、隣が使っていない一番小さい色を付ける。
// inBase は前の回の色の組(g_colors の先頭からの位置)
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

// 彩色の 1 回の物 i の色(前の回の色の組 inBase から読む)
int32_t NextColor(uint32_t i, uint32_t inBase) {
    int32_t color = g_colors[inBase + i];
    const PxBody body = g_bodies[i];
    if (color >= 0 || !PxIsDynamic(body) || body.active == 0 || !IsLocalMaximum(i, inBase))
        return color;

    color = 0;
    while (IsColorUsedByNeighbor(i, inBase, color))
        color += 1;

    return color;
}

#endif  // BICAMERAL_PHYSICS_BINDINGS_HLSLI
