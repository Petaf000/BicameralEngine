// physics_islands.hlsli — 物理の 1 小刻みを「島ごとに 1 グループ」で解く(T-0094。08 §2 の 3〜4)。
// 島 = 組(接触)でつながった動く物の集まり。島どうしはその小刻みの拘束を共有しないので、島ごとに別々に解いても近似にならない。
// 色の順番・反復の回数・6×6 の解き方は全体の方式(physics_step.hlsl の ColorRound〜UpdateVelocity)と同じなので、CPU とビット一致のまま。
//
// データの流れ(engine/src/sim/gpu_physics.cpp の RecordIslandSubstep):
//   PrepareManifolds の後 → FindIslands(1 グループ): 物ごとの印(島の一番小さい物の番号)を求め、物の数が g_islandBodyLimit 以下の島に
//   番号を付けて島ごとの物の並びを作る(u13)。それより大きい島の物は ISLAND_LARGE にし、u14 に「大きな島がある」を書く
//   → SolveIslands(1 グループ = 1 島): 島の中で彩色 → 反復(探し直し・色ごとの解・λ の更新・速度)をグループのバリアだけで回す。
//   使う色だけを回す(空の色を起こさない)。大きな島は全体の方式のパスが u14・u15 の述語つきで解く(無ければ飛ばされる)。
//
// 順番に依存しない理由: 印は「つながった物のうち一番小さい番号」で一意(伝え方の順や回数によらない)。島の番号と島の中の物の並びは
// 原子的な加算の順で決まらないが、島どうしは独立・同じ色の物どうしも独立なので、解く順は結果を変えない。
// 6×6 の和は整数の和(physics_bindings.hlsli の SolveBodyInSubgroup)。
// バッファは同じグループの中でだけ読み書きするので、グループのバリア(AllMemoryBarrierWithGroupSync)で見える(globallycoherent は要らない)。
#ifndef BICAMERAL_PHYSICS_ISLANDS_HLSLI
#define BICAMERAL_PHYSICS_ISLANDS_HLSLI

#include "sim/physics_bindings.hlsli"

static const uint32_t FIND_ISLANDS_THREADS = 1024;

groupshared uint32_t g_islandChanged;  // 伝える 1 回・彩色の 1 回で何か変わったか

// 島ごとのグループの色の区切り(色の順の物の並びの中の位置)
groupshared uint32_t g_islandColorCount;
groupshared uint32_t g_islandColorSize[PX_GPU_MAX_COLORS];
groupshared uint32_t g_islandColorStart[PX_GPU_MAX_COLORS];
groupshared uint32_t g_islandColorFill[PX_GPU_MAX_COLORS];

// --- 小さな部品 -------------------------------------------------------------------------------
uint32_t IslandValue(uint32_t region, uint32_t index) {
    return g_islands[IslandWord(region, index)];
}

void SetIslandValue(uint32_t region, uint32_t index, uint32_t value) {
    g_islands[IslandWord(region, index)] = value;
}

// 島に入る物: 世界にいる動く物(世界にいない物は組を持たない。PxPairMargin)
bool IsIslandMember(uint32_t i) {
    return g_bodies[i].massMilligrams != 0 && g_bodies[i].active != 0;
}

// 物 i の印の候補: 自分・隣(動く物の組の相手)の印と、自分の印の物の印(飛ばして早く縮める)の一番小さいもの
uint32_t LowestNearbyLabel(uint32_t i) {
    uint32_t lowest = IslandValue(ISLAND_LABEL, i);
    lowest = min(lowest, IslandValue(ISLAND_LABEL, lowest));
    const uint32_t count = IncidentCount(i);
    for (uint32_t e = 0; e < count; ++e) {
        const uint32_t j = PartnerOf(g_incident[i * g_incidentPerBody + e]);
        if (g_bodies[j].massMilligrams != 0)
            lowest = min(lowest, IslandValue(ISLAND_LABEL, j));
    }

    return lowest;
}

// --- 島分け(1 グループ。FindIslands)-------------------------------------------------------------
// 印を小さい方へ伝える。値は減る一方で、変わらなくなった時の値は「つながった物の一番小さい番号」(どの順に伝えても同じ)。
// 1 回の途中で隣が書き換えるのを読むことがあるが、読む値はどれも本当の値以上なので結果は変わらない。何も変わらない回が来たら終わり
void PropagateLabels(uint32_t thread) {
    for (;;) {
        if (thread == 0)
            g_islandChanged = 0;

        AllMemoryBarrierWithGroupSync();

        for (uint32_t i = thread; i < g_bodyCount; i += FIND_ISLANDS_THREADS) {
            const uint32_t label = IslandValue(ISLAND_LABEL, i);
            if (label == ISLAND_NONE)
                continue;

            const uint32_t lowest = LowestNearbyLabel(i);
            if (lowest >= label)
                continue;

            SetIslandValue(ISLAND_LABEL, i, lowest);
            g_islandChanged = 1;
        }

        AllMemoryBarrierWithGroupSync();
        const bool changed = g_islandChanged != 0;
        GroupMemoryBarrierWithGroupSync();  // 全員が読んでから次の回で 0 にする
        if (!changed)
            break;
    }
}

// 印ごとの物の数から島を決める: 小さい島に番号と物の並びの場所を、大きい島の印の物に ISLAND_LARGE を
void AssignIslands(uint32_t thread) {
    for (uint32_t i = thread; i < g_bodyCount; i += FIND_ISLANDS_THREADS) {
        const uint32_t label = IslandValue(ISLAND_LABEL, i);
        if (label != ISLAND_NONE)
            InterlockedAdd(g_islands[IslandWord(ISLAND_ROOT_SIZE, label)], 1);
    }

    AllMemoryBarrierWithGroupSync();

    for (uint32_t i = thread; i < g_bodyCount; i += FIND_ISLANDS_THREADS) {
        if (IslandValue(ISLAND_LABEL, i) != i)  // 島の一番小さい物だけ
            continue;

        const uint32_t size = IslandValue(ISLAND_ROOT_SIZE, i);
        if (size > g_islandBodyLimit) {
            SetIslandValue(ISLAND_OF, i, ISLAND_LARGE);
            InterlockedAdd(g_islands[ISLAND_HEADER_LARGE_BODIES], size);
            continue;
        }

        uint32_t island = 0;
        uint32_t start = 0;
        InterlockedAdd(g_islands[ISLAND_HEADER_COUNT], 1, island);
        InterlockedAdd(g_islands[ISLAND_HEADER_CURSOR], size, start);
        SetIslandValue(ISLAND_OF, i, island);
        SetIslandValue(ISLAND_START, island, start);
        SetIslandValue(ISLAND_SIZE, island, 0);  // 下で物を並べながら数える
    }

    AllMemoryBarrierWithGroupSync();
}

// 物を島の並びに入れる(並びの中の順は原子的な加算の順。解く順は結果を変えない)
void PlaceIslandBodies(uint32_t thread) {
    for (uint32_t i = thread; i < g_bodyCount; i += FIND_ISLANDS_THREADS) {
        const uint32_t label = IslandValue(ISLAND_LABEL, i);
        if (label == ISLAND_NONE)
            continue;

        const uint32_t island = IslandValue(ISLAND_OF, label);
        if (i != label)
            SetIslandValue(ISLAND_OF, i, island);

        if (island == ISLAND_LARGE)
            continue;

        uint32_t position = 0;
        InterlockedAdd(g_islands[IslandWord(ISLAND_SIZE, island)], 1, position);
        SetIslandValue(ISLAND_BODIES, IslandValue(ISLAND_START, island) + position, i);
    }
}

void FindIslandsInGroup(uint32_t thread) {
    if (thread < ISLAND_HEADER_WORDS)
        g_islands[thread] = 0;

    for (uint32_t i = thread; i < g_bodyCount; i += FIND_ISLANDS_THREADS) {
        SetIslandValue(ISLAND_LABEL, i, IsIslandMember(i) ? i : ISLAND_NONE);
        SetIslandValue(ISLAND_OF, i, ISLAND_NONE);
        SetIslandValue(ISLAND_ROOT_SIZE, i, 0);
    }

    PropagateLabels(thread);
    AssignIslands(thread);
    PlaceIslandBodies(thread);
    if (thread == 0)
        g_islandPredicate.Store<uint64_t>(0, g_islands[ISLAND_HEADER_LARGE_BODIES] != 0 ? 1 : 0);
}

// --- 島の中の彩色(SolveIslands の初め)---------------------------------------------------------------
// 全体の方式の ColorRound と同じ 1 回(NextColor)を、色の付く物が無くなるまで繰り返す(CPU の ColorBodies と同じ止め方)。
// 前の回の色だけを読み、次の組に書く。最後の回の色の組を返す
uint32_t ColorIslandRounds(uint32_t start, uint32_t size, uint32_t thread) {
    uint32_t colorIn = 0;  // BeginSubstep が組 0 を「色なし」にした
    bool changed = true;
    while (changed) {
        if (thread == 0)
            g_islandChanged = 0;

        AllMemoryBarrierWithGroupSync();

        const uint32_t inBase = colorIn * g_bodyCount;
        const uint32_t outBase = (1 - colorIn) * g_bodyCount;
        for (uint32_t k = thread; k < size; k += ISLAND_GROUP_THREADS) {
            const uint32_t i = IslandValue(ISLAND_BODIES, start + k);
            const int32_t color = NextColor(i, inBase);
            if (color != g_colors[inBase + i])
                g_islandChanged = 1;

            g_colors[outBase + i] = color;
        }

        AllMemoryBarrierWithGroupSync();
        changed = g_islandChanged != 0;  // グループで一様
        colorIn = 1 - colorIn;
        GroupMemoryBarrierWithGroupSync();  // 全員が読んでから次の回で 0 にする
    }

    return colorIn;
}

// 色を物に移し、島の物の並びを色の順に並べ直す(ISLAND_COLOR_BODIES)。塗れなかった物・色が多すぎるのは印(FinishColoring と同じ)
void SortIslandByColor(uint32_t start, uint32_t size, uint32_t colorIn, uint32_t thread) {
    if (thread < PX_GPU_MAX_COLORS) {
        g_islandColorSize[thread] = 0;
        g_islandColorFill[thread] = 0;
    }

    if (thread == 0)
        g_islandColorCount = 0;

    GroupMemoryBarrierWithGroupSync();

    for (uint32_t k = thread; k < size; k += ISLAND_GROUP_THREADS) {
        const uint32_t i = IslandValue(ISLAND_BODIES, start + k);
        const int32_t color = g_colors[colorIn * g_bodyCount + i];
        g_bodies[i].color = color;
        if (color < 0) {
            Flag(OVERFLOW_UNCOLORED);  // 島の物はどれも世界にいる動く物
            continue;
        }

        g_stats.InterlockedMax(STATS_COLOR_COUNT, (uint32_t)(color + 1));
        if (color >= PX_GPU_MAX_COLORS) {
            Flag(OVERFLOW_COLORS);
            continue;
        }

        InterlockedAdd(g_islandColorSize[color], 1);
        InterlockedMax(g_islandColorCount, (uint32_t)(color + 1));
    }

    GroupMemoryBarrierWithGroupSync();
    if (thread == 0) {
        uint32_t offset = 0;
        for (uint32_t c = 0; c < g_islandColorCount; ++c) {
            g_islandColorStart[c] = offset;
            offset += g_islandColorSize[c];
        }
    }

    GroupMemoryBarrierWithGroupSync();

    for (uint32_t k = thread; k < size; k += ISLAND_GROUP_THREADS) {
        const uint32_t i = IslandValue(ISLAND_BODIES, start + k);
        const int32_t color = g_bodies[i].color;
        if (color < 0 || color >= PX_GPU_MAX_COLORS)
            continue;

        uint32_t position = 0;
        InterlockedAdd(g_islandColorFill[color], 1, position);
        SetIslandValue(ISLAND_COLOR_BODIES, start + g_islandColorStart[color] + position, i);
    }

    AllMemoryBarrierWithGroupSync();
}

// --- 島の中の反復の部品 -------------------------------------------------------------------------
// 島の物が持ち主の枠(物 × slotsPerBody)を全部のスレッドで分けて回す。recollide = true なら探し直し、false なら λ の更新
void ForEachIslandSlot(uint32_t start, uint32_t size, uint32_t thread, bool recollide, int64_t alphaQ16) {
    const uint32_t itemCount = size * g_slotsPerBody;
    for (uint32_t item = thread; item < itemCount; item += ISLAND_GROUP_THREADS) {
        const uint32_t owner = IslandValue(ISLAND_BODIES, start + item / g_slotsPerBody);
        const uint32_t slot = owner * g_slotsPerBody + item % g_slotsPerBody;
        if (!IsLiveSlot(slot))
            continue;

        if (recollide)
            RecollideSlot(slot);
        else
            UpdateDualsSlot(slot, alphaQ16);
    }

    AllMemoryBarrierWithGroupSync();
}

// 1 つの色の物を、組(SOLVE_LANES 本)ごとに 1 物ずつ解く。同じ色の物は拘束を共有しないので同時に解ける
void SolveIslandColor(uint32_t start, uint32_t color, uint32_t thread, int64_t alphaQ16) {
    const uint32_t count = g_islandColorSize[color];
    const uint32_t first = start + g_islandColorStart[color];
    const uint32_t subgroup = thread / SOLVE_LANES;
    const uint32_t lane = thread % SOLVE_LANES;
    for (uint32_t base = 0; base < count; base += SOLVE_SUBGROUPS) {
        const uint32_t k = base + subgroup;
        const bool valid = k < count;
        const uint32_t i = valid ? IslandValue(ISLAND_COLOR_BODIES, first + k) : 0;
        SolveBodyInSubgroup(i, lane, subgroup, valid, alphaQ16);
        AllMemoryBarrierWithGroupSync();  // 次の組・次の色が Δ を読む
    }
}

void UpdateIslandVelocities(uint32_t start, uint32_t size, uint32_t thread) {
    const int64_t rate = PxStepRate(g_parameters[0]);
    for (uint32_t k = thread; k < size; k += ISLAND_GROUP_THREADS) {
        const uint32_t i = IslandValue(ISLAND_BODIES, start + k);
        g_bodies[i] = PxUpdateVelocity(g_bodies[i], rate);
    }

    AllMemoryBarrierWithGroupSync();
}

// --- 1 つの島を 1 グループで解く(SolveIslands)。全体の方式の RecordColoring・RecordIterations と同じ順 ---------------
void SolveIslandInGroup(uint32_t island, uint32_t thread) {
    const uint32_t start = IslandValue(ISLAND_START, island);
    const uint32_t size = IslandValue(ISLAND_SIZE, island);
    const uint32_t colorIn = ColorIslandRounds(start, size, thread);
    SortIslandByColor(start, size, colorIn, thread);

    // 本反復は α = 1、最後の 1 回は α = 0。途中で接触を探し直し、最後の本反復の後で速度を決める
    const PxParameters p = g_parameters[0];
    for (uint32_t iteration = 0; iteration <= p.iterations; ++iteration) {
        if (iteration == p.recollideIteration)
            ForEachIslandSlot(start, size, thread, true, 0);

        const int64_t alphaQ16 = iteration < p.iterations ? PX_ALPHA_ONE_Q16 : 0;
        for (uint32_t color = 0; color < g_islandColorCount; ++color)
            SolveIslandColor(start, color, thread, alphaQ16);

        if (iteration < p.iterations)
            ForEachIslandSlot(start, size, thread, false, alphaQ16);

        if (iteration + 1 == p.iterations)
            UpdateIslandVelocities(start, size, thread);
    }
}

#endif  // BICAMERAL_PHYSICS_ISLANDS_HLSLI
