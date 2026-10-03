// multires_graph.hlsl — 多重解像度の木を細かくする・粗く戻す・影を引き戻すを Work Graph で(T-0017・T-0018。17 §1・§3・§5)。
// 細かくする鎖は再帰(1 レベル = 1 グループ。段の数は実行時に決まる。D-302)。
// 入口は 4 つ: RefineNode(CPU の入力 = 手で決めた影の鎖 / GPU の入力 = 許可した要求の鎖。multires_tree.hlsl の TreeAllocate が作る)・
// CoarsenRequestNode(GPU の入力 = 許可した粗くする要求)・PullBackNode・RemoveShadowNode(CPU の入力)。
// CPU リファレンスは engine/src/sim/multires_tree.cpp の RefineRequestLevel・ApplyCoarsen と、multires_nest.cpp の RefineShadowLevel・
// PullBackLevel・RemoveShadowChain(同じ順・同じ関数)。
//
// 順番に依存しない理由: 1 刻みに 1 つの親は 1 つの要求だけが触る(取り合いは TreeSettle で決着)ので、グループどうしの書き込みは重ならない。
// 1 グループの中でスレッドは別々のセルに書き、見出し・返す枠・索引はスレッド 0 だけが書く。数える欄と帳簿は足し算。
// 次のレベルは前のレベルの書き込みを読むので、出力の前に Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC)、バッファは globallycoherent。
#include "sim/multires_bindings.hlsli"

// 再帰の深さの上限(要求の鎖は MR_MAX_CHAIN_LEVELS 段まで)
static const uint32_t MR_MAX_RECURSION = MR_MAX_CHAIN_LEVELS;

struct MrChainRecord {
    uint32_t slot;
    uint32_t levelsLeft;
};

struct MrCoarsenRecord {
    uint32_t request;  // MR_NO_BLOCK なら何もしない(GPU の入力の空の代わり)
};

groupshared uint32_t gs_octantFraction;
groupshared uint32_t gs_restFraction;

// 手で決めた影の鎖の点(ルート定数)
MrRequest ShadowPoint() {
    return MrMakeRequest(MR_REQUEST_REFINE, g_pointLevel, (int64_t)FX_U64(g_pointXHigh, g_pointXLow),
                         (int64_t)FX_U64(g_pointYHigh, g_pointYLow), (int64_t)FX_U64(g_pointZHigh, g_pointZLow));
}

// 要求の返す端数の枠に足す(スレッド 0 だけ)
void ReleaseFraction(uint32_t request, uint32_t fractionSlot) {
    const uint32_t count = g_states[request].releaseCount;
    g_states[request].releases[count] = fractionSlot;
    g_states[request].releaseCount = count + 1;
}

// 1 段を細かくする(1 スレッド = 子のセル t。親のセル t も読む)
void RefineLevel(MrRefineRecord record, uint32_t t) {
    const bool fromRequest = record.request != MR_NO_BLOCK;
    const bool real = record.kind == MR_BLOCK_REAL;
    MrRequest target = ShadowPoint();  // HLSL の ?: は構造体を返せない
    uint32_t childSlot = record.childSlot;
    uint32_t candidate = MR_NO_FRACTION;
    if (fromRequest) {
        target = g_requests[record.request];
        const MrRequestState state = g_states[record.request];
        childSlot = PoppedBlock(state, record.depth);
        candidate = PoppedFraction(state, record.depth);
    }

    const MrBlock parent = g_blocks[record.parentSlot];
    const uint32_t octant = MrOctantOfPoint(parent, target.x, target.y, target.z, target.level);

    // --- 端数: 親の八分の一に 0 でない端数があるか・親の残りに残るか(本物だけ)---
    if (t == 0) {
        gs_octantFraction = 0;
        gs_restFraction = 0;
    }

    GroupMemoryBarrierWithGroupSync();
    if (real && !MrFractionIsZero(LoadFraction(parent.fraction, t))) {
        if (MrOctantOfCell(t) == octant)
            InterlockedOr(gs_octantFraction, 1u);
        else
            InterlockedOr(gs_restFraction, 1u);
    }

    GroupMemoryBarrierWithGroupSync();
    const uint32_t childFraction = gs_octantFraction != 0 ? candidate : MR_NO_FRACTION;

    // --- セルと端数: 子は親と同じ数 ---
    const uint32_t parentCell = MrParentCellOfChild(octant, t);
    g_cells[CellAddress(childSlot, t)] = g_cells[CellAddress(record.parentSlot, parentCell)];
    if (childFraction != MR_NO_FRACTION)
        g_fractions[CellAddress(childFraction, t)] = LoadFraction(parent.fraction, parentCell);

    // --- 本物なら親を覆う(全部のスレッドが親を読み終えてから)---
    GroupMemoryBarrierWithGroupSync();
    if (real && t < MR_OCTANT_CELLS) {
        const uint32_t index = MrOctantCell(octant, t);
        g_cells[CellAddress(record.parentSlot, index)] = RxMakeEmptyCell(0);
        if (parent.fraction != MR_NO_FRACTION)
            g_fractions[CellAddress(parent.fraction, index)] = MrMakeEmptyFraction();
    }

    if (t != 0)
        return;

    // --- 見出し・返す枠・索引(スレッド 0)---
    if (fromRequest && candidate != MR_NO_FRACTION && childFraction == MR_NO_FRACTION)
        ReleaseFraction(record.request, candidate);

    if (real && parent.fraction != MR_NO_FRACTION && gs_restFraction == 0) {
        ReleaseFraction(record.request, parent.fraction);
        g_blocks[record.parentSlot].fraction = MR_NO_FRACTION;
    }

    if (real)
        g_blocks[record.parentSlot].children[octant] = childSlot;

    g_blocks[childSlot] = MrMakeChildBlock(parent, record.parentSlot, octant, record.kind, childFraction);
    if (real)
        IndexInsert(childSlot);
}

// 許可した粗くする要求の 1 段(1 スレッド = 親の八分の一のセル t。multires_tree.cpp の ApplyCoarsen)
void CoarsenRequest(uint32_t request, uint32_t t) {
    const MrRequestState state = g_states[request];
    const uint32_t childSlot = state.target;
    const MrBlock child = g_blocks[childSlot];
    const uint32_t parentSlot = child.parent;
    const MrBlock parent = g_blocks[parentSlot];
    const uint32_t octant = child.parentOctant;

    // --- 子 2³ を 1 つに ---
    MrChildren children;
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
        const uint32_t index = MrChildCell(t, j);
        children.cells[j] = g_cells[CellAddress(childSlot, index)];
        children.fractions[j] = LoadFraction(child.fraction, index);
    }

    const MrCoarsened result = MrCoarsenCell(children);
    if (result.lostCount != 0)
        InterlockedAdd(g_counters[MR_COUNTER_LOST], result.lostCount);

    if (result.overflowCount != 0)
        InterlockedAdd(g_counters[MR_COUNTER_OVERFLOW], result.overflowCount);

    if (result.energyLostBits != 0)
        AddToLedger(child.level, 0, result.energyLostBits);

    for (uint32_t k = 0; k < result.lostSpeciesCount; ++k)
        AddToLedger(child.level, 1 + result.lostSpecies[k], result.lostBits[k]);

    // --- 親の端数: 枠が無ければ取っておいた枠(八分の一の外を 0 で始める)。残りに 0 でない端数があるか ---
    if (t == 0) {
        gs_octantFraction = 0;
        gs_restFraction = 0;
    }

    GroupMemoryBarrierWithGroupSync();
    const uint32_t parentFraction = parent.fraction != MR_NO_FRACTION ? parent.fraction : PoppedFraction(state, 0);
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / MR_OCTANT_CELLS; ++k) {
        const uint32_t index = t + (k * MR_OCTANT_CELLS);
        if (MrOctantOfCell(index) == octant)
            continue;

        if (parent.fraction == MR_NO_FRACTION)
            g_fractions[CellAddress(parentFraction, index)] = MrMakeEmptyFraction();
        else if (!MrFractionIsZero(g_fractions[CellAddress(parentFraction, index)]))
            InterlockedOr(gs_restFraction, 1u);
    }

    if (!MrFractionIsZero(result.fraction))
        InterlockedOr(gs_octantFraction, 1u);

    const uint32_t index = MrOctantCell(octant, t);
    g_cells[CellAddress(parentSlot, index)] = result.cell;
    g_fractions[CellAddress(parentFraction, index)] = result.fraction;

    GroupMemoryBarrierWithGroupSync();
    if (t != 0)
        return;

    // --- 見出し・返す枠・索引(スレッド 0。CPU と同じ順に返す)---
    uint32_t kept = parentFraction;
    if (gs_octantFraction == 0 && gs_restFraction == 0) {
        ReleaseFraction(request, parentFraction);
        kept = MR_NO_FRACTION;
    }

    if (child.fraction != MR_NO_FRACTION)
        ReleaseFraction(request, child.fraction);

    g_states[request].releaseBlock = childSlot;
    g_blocks[parentSlot].fraction = kept;
    g_blocks[parentSlot].children[octant] = MR_NO_BLOCK;
    IndexRemove(childSlot);
    g_blocks[childSlot] = MrMakeUnusedBlock();
}

// clang-format は HLSL のノードの属性を並べ崩すので、属性つきの宣言だけ整形を止める
// clang-format off

// 親の八分の一を子ブロックに細かくし、次の段を自分に投げる(1 スレッド = 子のセル 1 つ)
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(512, 1, 1)]
[NodeMaxRecursionDepth(MR_MAX_RECURSION)]
[NodeIsProgramEntry]
void RefineNode(DispatchNodeInputRecord<MrRefineRecord> input, uint32_t thread : SV_GroupIndex,
                [MaxRecords(1)] [NodeId("RefineNode")] NodeOutput<MrRefineRecord> next) {
    const MrRefineRecord record = input.Get();
    if (record.levelsLeft > 0)
        RefineLevel(record, thread);

    // --- 次のレベル ---
    Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC);
    const bool recurse = record.levelsLeft > 1;
    GroupNodeOutputRecords<MrRefineRecord> output = next.GetGroupNodeOutputRecords(recurse ? 1 : 0);
    if (recurse && thread == 0) {
        const bool fromRequest = record.request != MR_NO_BLOCK;
        const uint32_t childSlot = fromRequest ? PoppedBlock(g_states[record.request], record.depth) : record.childSlot;
        output.Get().request = record.request;
        output.Get().parentSlot = childSlot;
        output.Get().childSlot = childSlot + 1;
        output.Get().levelsLeft = record.levelsLeft - 1;
        output.Get().kind = record.kind;
        output.Get().depth = record.depth + 1;
    }

    output.OutputComplete();
}

// 許可した粗くする要求: 子ブロックを親の八分の一へ戻す(1 スレッド = 親のセル 1 つ)
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(64, 1, 1)]
[NodeIsProgramEntry]
void CoarsenRequestNode(DispatchNodeInputRecord<MrCoarsenRecord> input, uint32_t thread : SV_GroupIndex) {
    const uint32_t request = input.Get().request;
    if (request == MR_NO_BLOCK)
        return;

    CoarsenRequest(request, thread);
}

// clang-format on

// clang-format off

// 影の子 2³ を親 × 8 に引き戻し、次はその下の影を(1 スレッド = 親のセル 1 つ。影の鎖の枠は連続)
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(64, 1, 1)]
[NodeMaxRecursionDepth(MR_MAX_RECURSION)]
[NodeIsProgramEntry]
void PullBackNode(DispatchNodeInputRecord<MrChainRecord> input, uint32_t thread : SV_GroupIndex,
                  [MaxRecords(1)] [NodeId("PullBackNode")] NodeOutput<MrChainRecord> next) {
    const MrChainRecord record = input.Get();
    const MrBlock shadow = g_blocks[record.slot];
    const RxCell parent = g_cells[CellAddress(shadow.parent, MrOctantCell(shadow.parentOctant, thread))];

    MrShadowFamily family;
    family.clamped = 0;
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
        family.cells[j] = g_cells[CellAddress(record.slot, MrChildCell(thread, j))];

    const MrShadowFamily pulled = MrPullBackShadow(MakeTable(), parent, family);
    if (pulled.clamped != 0)
        InterlockedAdd(g_counters[MR_COUNTER_SHADOW_CLAMPED], pulled.clamped);

    for (uint32_t k = 0; k < MR_CHILDREN_PER_CELL; ++k)
        g_cells[CellAddress(record.slot, MrChildCell(thread, k))] = pulled.cells[k];

    // --- 次のレベル ---
    Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC);
    const bool recurse = record.levelsLeft > 1;
    GroupNodeOutputRecords<MrChainRecord> output = next.GetGroupNodeOutputRecords(recurse ? 1 : 0);
    if (recurse && thread == 0) {
        output.Get().slot = record.slot + 1;
        output.Get().levelsLeft = record.levelsLeft - 1;
    }

    output.OutputComplete();
}

// 影の鎖を捨てる(世界に返さない。見出しを空にするだけ)
[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
void RemoveShadowNode(ThreadNodeInputRecord<MrChainRecord> input) {
    const MrChainRecord record = input.Get();
    for (uint32_t i = 0; i < record.levelsLeft; ++i)
        g_blocks[record.slot + i] = MrMakeUnusedBlock();
}

// clang-format on
