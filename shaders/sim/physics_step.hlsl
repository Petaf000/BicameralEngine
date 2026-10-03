// physics_step.hlsl — 整数の AVBD の 1 刻み(08 §2。T-0090)を GPU の Compute で走らせるパスの列。入口ごとに 1 つの .cso(shaders/CMakeLists.txt)。
// 手順は shaders/common/physics_step.hlsli(CPU の engine/src/sim/physics_world.cpp と共通)、呼ぶ順番と結び付けは engine/src/sim/gpu_physics.cpp。
// バッファと組の置き場所の部品は physics_bindings.hlsli(Work Graph 版の physics_graph.hlsl と共有。T-0092)。
#include "sim/physics_bindings.hlsli"

// --- 初め: 場面の物を写し、組の枠を空にする ---------------------------------------------------------
[numthreads(64, 1, 1)] void Initialize(uint3 id : SV_DispatchThreadID) {
    const uint32_t i = id.x;
    if (i < PX_GPU_MAX_COLORS) {  // 色ごとの GPU の入力の変わらない欄(入口・レコードの場所と間隔)
        const uint32_t base = i * COLOR_INPUT_BYTES;
        const uint64_t records = (((uint64_t)g_colorBodiesHigh << 32) | (uint64_t)g_colorBodiesLow) +
                                 (uint64_t)i * g_bodyCount * COLOR_RECORD_STRIDE_BYTES;
        g_colorInputs.Store2(base, uint2(g_solveEntry, 0));
        g_colorInputs.Store<uint64_t>(base + 8, records);
        g_colorInputs.Store<uint64_t>(base + 16, (uint64_t)COLOR_RECORD_STRIDE_BYTES);
    }

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

    if (i < PX_GPU_MAX_COLORS) {
        g_colorInputs.Store(i * COLOR_INPUT_BYTES + 4, 0);  // 色の物の一覧を空に
        g_colorPredicates.Store<uint64_t>(i * 8, 0);        // その色の物がいる(T-0094)
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

    g_slotCounts[g_currentHalf * g_bodyCount + i] = CollectCandidates(i);
}

    // --- 接触の生成: 枠ごとに接触を作り、前の小刻みの同じ組から引き継ぐ。β の質量と物の一覧も -----------------------
    [numthreads(64, 1, 1)] void Narrowphase(uint3 id : SV_DispatchThreadID) {
    const uint32_t slot = id.x;
    if (slot >= g_bodyCount * g_slotsPerBody)
        return;

    const uint32_t owner = slot / g_slotsPerBody;
    if (slot % g_slotsPerBody >= g_slotCounts[g_currentHalf * g_bodyCount + owner]) {
        g_manifolds[ManifoldIndex(g_currentHalf, slot)].count = 0;
        return;
    }

    const uint32_t other = g_candidates[slot];
    BuildSlot(slot, other, CollideGeometry(slot, other));
}

// Work Graph の広域の選別 → 接触の幾何(physics_graph.hlsl)の後で、枠ごとに組を作る(T-0092)。使わない枠は BroadphaseNode が空にした
[numthreads(64, 1, 1)] void BuildManifolds(uint3 id : SV_DispatchThreadID) {
    const uint32_t slot = id.x;
    if (slot >= g_bodyCount * g_slotsPerBody)
        return;

    const uint32_t owner = slot / g_slotsPerBody;
    if (slot % g_slotsPerBody >= g_slotCounts[g_currentHalf * g_bodyCount + owner])
        return;

    BuildSlot(slot, g_candidates[slot], g_geometry[slot]);
}

    // --- β・硬さの引き継ぎ・線形化(組ごと)-------------------------------------------------------------
    [numthreads(64, 1, 1)] void PrepareManifolds(uint3 id : SV_DispatchThreadID) {
    const uint32_t slot = id.x;
    if (!IsLiveSlot(slot))
        return;

    // PxWarmStartManifold → PxLinearizeManifold と同じ手順を、組(3 KB)を局所に写さずに点ごとに(Recollide と同じ理由。T-0098)
    const PxParameters p = g_parameters[0];
    const uint32_t index = ManifoldIndex(g_currentHalf, slot);
    const ManifoldHeader header = g_manifolds[index];
    const PxBody bodyA = g_bodies[header.bodyA];
    const PxBody bodyB = g_bodies[header.bodyB];
    g_manifolds[index].beta = PxBetaOf(PxMaxU64(g_touchMass[header.bodyA], g_touchMass[header.bodyB]), p);
    for (uint32_t k = 0; k < header.count; ++k) {
        PxContactPoint contactPoint = g_points[PointIndex(index, k)];
        for (uint32_t r = 0; r < 3; ++r)
            contactPoint.rows[r] = PxDecayPenalty(contactPoint.rows[r], p.gammaQ16, header.minPenalty,
                                                  header.maxPenalty);

        g_points[PointIndex(index, k)] = PxLinearizePoint(contactPoint, bodyA, bodyB);
    }
}

// --- 彩色の 1 回(Jones-Plassmann): 前の回の色だけを読み、次の組に書く(NextColor は physics_bindings.hlsli)--------------
[numthreads(64, 1, 1)] void ColorRound(uint3 id : SV_DispatchThreadID) {
    const uint32_t i = id.x;
    if (i >= g_bodyCount)
        return;

    g_colors[(1 - g_colorIn) * g_bodyCount + i] = NextColor(i, g_colorIn * g_bodyCount);
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
    if (color >= PX_GPU_MAX_COLORS) {
        Flag(OVERFLOW_COLORS);
        return;
    }

    g_colorPredicates.Store<uint64_t>(color * 8, 1);  // この色の Dispatch を飛ばさない(述語。T-0094)

    // 色ごとの物の一覧(Work Graph で解くとき。並びは原子的な加算の順で決まらないが、同じ色の物は互いに独立)
    uint32_t position = 0;
    g_colorInputs.InterlockedAdd(color * COLOR_INPUT_BYTES + 4, 1, position);
    g_colorBodies[color * g_bodyCount + position] = i;
}

// --- 反復の途中の探し直し(組ごと)----------------------------------------------------------------
[numthreads(64, 1, 1)] void Recollide(uint3 id : SV_DispatchThreadID) {
    const uint32_t slot = id.x;
    if (IsLiveSlot(slot))
        RecollideSlot(slot);  // physics_bindings.hlsli
}

    // --- 1 つの色の物を解く(1 グループ = 1 物。同じ色の物は拘束を共有しないので並列に解ける。T-0092)--------------------------------------------------------
    [numthreads(SOLVE_GROUP_THREADS, 1, 1)] void SolveColor(uint3 groupId : SV_GroupID,
                                                            uint32_t thread : SV_GroupIndex) {
    const uint32_t i = groupId.x;
    if (i >= g_bodyCount || g_bodies[i].color != g_color)  // グループで一様(1 グループ = 1 物)
        return;

    SolveBodyInGroup(i, thread);
}

// --- λ と硬さの更新(組ごと)-------------------------------------------------------------------------
[numthreads(64, 1, 1)] void UpdateDuals(uint3 id : SV_DispatchThreadID) {
    const uint32_t slot = id.x;
    if (IsLiveSlot(slot))
        UpdateDualsSlot(slot, (int64_t)g_alphaQ16);
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
