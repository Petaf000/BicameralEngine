// multires_graph.hlsl — 多重解像度の入れ子を細かくする・粗く戻す・影を引き戻すを Work Graph の再帰で(T-0017。17 §1・§3)。
// 1 レベル = 1 グループ。グループがそのレベルを書き終えたら、次のレベルを自分のノードに投げる(段の数は実行時に決まる。D-302)。
// 入口は 4 つ(CPU の入力): RefineNode・CoarsenNode・PullBackNode・RemoveShadowNode。
// CPU リファレンスは engine/src/sim/multires_nest.cpp の RefineLevel・CoarsenLevel・PullBackLevel・RemoveShadowChain(同じ順・同じ関数)。
//
// 順番に依存しない理由: 1 レベルの中でスレッドは別々のセルに書く。端数の枠を取るのはスレッド 0 だけ(鎖は 1 本なので取る順が決まる)。
// 数える欄の加算(落ちた端数・溢れ・影の余裕)は足し算なので順に依存しない。
// 次のレベルは前のレベルの書き込みを読むので、出力の前に Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC)、バッファは globallycoherent。
#include "sim/multires_bindings.hlsli"

// 再帰の深さの上限(GPU の鎖は 9 段。CPU のテストは 24 段まで)
static const uint32_t MR_MAX_RECURSION = 24;

struct MrRefineRecord {
    uint32_t parentSlot;
    uint32_t childSlot;
    uint32_t levelsLeft;  // このレベルを含めて残りの段の数
    uint32_t kind;        // MR_BLOCK_REAL か MR_BLOCK_SHADOW
};

struct MrChainRecord {
    uint32_t slot;
    uint32_t levelsLeft;
};

groupshared uint32_t gs_anyFraction;
groupshared uint32_t gs_fractionSlot;

// 点を含む八分の一(ルート定数の点)
uint32_t OctantOfPoint(MrBlock block) {
    const int64_t x = (int64_t)FX_U64(g_pointXHigh, g_pointXLow);
    const int64_t y = (int64_t)FX_U64(g_pointYHigh, g_pointYLow);
    const int64_t z = (int64_t)FX_U64(g_pointZHigh, g_pointZLow);

    return MrOctantBitOfPoint(block.originX, block.level, x, g_pointLevel) |
           (MrOctantBitOfPoint(block.originY, block.level, y, g_pointLevel) << 1) |
           (MrOctantBitOfPoint(block.originZ, block.level, z, g_pointLevel) << 2);
}

MrBlock MakeChildBlock(MrBlock parent, MrRefineRecord record, uint32_t octant, uint32_t fractionSlot) {
    MrBlock child = MrMakeUnusedBlock();
    child.originX = MrChildOrigin(parent.originX, octant & 1u);
    child.originY = MrChildOrigin(parent.originY, (octant >> 1) & 1u);
    child.originZ = MrChildOrigin(parent.originZ, (octant >> 2) & 1u);
    child.level = parent.level + 1;
    child.kind = record.kind;
    child.parent = record.parentSlot;
    child.parentOctant = octant;
    child.fraction = fractionSlot;

    return child;
}

// clang-format は HLSL のノードの属性を並べ崩すので、属性つきの宣言だけ整形を止める
// clang-format off

// 親の八分の一を子ブロックに細かくする(1 スレッド = 子のセル 1 つ)
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(512, 1, 1)]
[NodeMaxRecursionDepth(MR_MAX_RECURSION)]
[NodeIsProgramEntry]
void RefineNode(DispatchNodeInputRecord<MrRefineRecord> input, uint32_t thread : SV_GroupIndex,
                [MaxRecords(1)] [NodeId("RefineNode")] NodeOutput<MrRefineRecord> next) {
    const MrRefineRecord record = input.Get();
    const MrBlock parent = g_blocks[record.parentSlot];
    const uint32_t octant = OctantOfPoint(parent);
    const bool real = record.kind == MR_BLOCK_REAL;

    // --- セル: 子は親と同じ数 ---
    const uint32_t parentCell = MrParentCellOfChild(octant, thread);
    g_cells[CellAddress(record.childSlot, thread)] = g_cells[CellAddress(record.parentSlot, parentCell)];

    // --- 端数: 本物の子だけ、親の八分の一に 0 でない端数があれば枠を取って写す ---
    if (thread == 0)
        gs_anyFraction = 0;

    GroupMemoryBarrierWithGroupSync();
    if (thread < MR_OCTANT_CELLS && !MrFractionIsZero(LoadFraction(parent.fraction, MrOctantCell(octant, thread))))
        InterlockedOr(gs_anyFraction, 1u);

    GroupMemoryBarrierWithGroupSync();
    if (thread == 0) {
        gs_fractionSlot = MR_NO_FRACTION;
        if (real && gs_anyFraction != 0)
            InterlockedAdd(g_counters[MR_COUNTER_FRACTION_BLOCKS], 1u, gs_fractionSlot);
    }

    GroupMemoryBarrierWithGroupSync();
    const uint32_t childFraction = gs_fractionSlot;
    if (childFraction != MR_NO_FRACTION)
        g_fractions[CellAddress(childFraction, thread)] = LoadFraction(parent.fraction, parentCell);

    // --- 本物なら親を覆う(全部のスレッドが親を読み終えてから、覆われた親のセルと端数を空に)---
    GroupMemoryBarrierWithGroupSync();
    if (real && thread < MR_OCTANT_CELLS) {
        const uint32_t index = MrOctantCell(octant, thread);
        g_cells[CellAddress(record.parentSlot, index)] = RxMakeEmptyCell(0);
        if (parent.fraction != MR_NO_FRACTION)
            g_fractions[CellAddress(parent.fraction, index)] = MrMakeEmptyFraction();
    }

    if (thread == 0) {
        g_blocks[record.childSlot] = MakeChildBlock(parent, record, octant, childFraction);
        if (real)
            g_blocks[record.parentSlot].children[octant] = record.childSlot;
    }

    // --- 次のレベル ---
    Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC);
    const bool recurse = record.levelsLeft > 1;
    GroupNodeOutputRecords<MrRefineRecord> output = next.GetGroupNodeOutputRecords(recurse ? 1 : 0);
    if (recurse && thread == 0) {
        output.Get().parentSlot = record.childSlot;
        output.Get().childSlot = record.childSlot + 1;
        output.Get().levelsLeft = record.levelsLeft - 1;
        output.Get().kind = record.kind;
    }

    output.OutputComplete();
}

// 子ブロックを親の八分の一へ粗く戻し、次は親を(1 スレッド = 親のセル 1 つ)
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(64, 1, 1)]
[NodeMaxRecursionDepth(MR_MAX_RECURSION)]
[NodeIsProgramEntry]
void CoarsenNode(DispatchNodeInputRecord<MrChainRecord> input, uint32_t thread : SV_GroupIndex,
                 [MaxRecords(1)] [NodeId("CoarsenNode")] NodeOutput<MrChainRecord> next) {
    const MrChainRecord record = input.Get();
    const MrBlock child = g_blocks[record.slot];
    const uint32_t parentSlot = child.parent;

    // --- 子 2³ を 1 つに ---
    MrChildren children;
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
        const uint32_t index = MrChildCell(thread, j);
        children.cells[j] = g_cells[CellAddress(record.slot, index)];
        children.fractions[j] = LoadFraction(child.fraction, index);
    }

    const MrCoarsened result = MrCoarsenCell(children);
    if (result.lostCount != 0)
        InterlockedAdd(g_counters[MR_COUNTER_LOST], result.lostCount);

    if (result.overflowCount != 0)
        InterlockedAdd(g_counters[MR_COUNTER_OVERFLOW], result.overflowCount);

    // --- 端数: どれかが 0 でなく、親が端数の枠を持っていなければ取る ---
    if (thread == 0)
        gs_anyFraction = 0;

    GroupMemoryBarrierWithGroupSync();
    if (!MrFractionIsZero(result.fraction))
        InterlockedOr(gs_anyFraction, 1u);

    GroupMemoryBarrierWithGroupSync();
    if (thread == 0) {
        uint32_t fractionSlot = g_blocks[parentSlot].fraction;
        if (gs_anyFraction != 0 && fractionSlot == MR_NO_FRACTION) {
            InterlockedAdd(g_counters[MR_COUNTER_FRACTION_BLOCKS], 1u, fractionSlot);
            g_blocks[parentSlot].fraction = fractionSlot;
        }

        gs_fractionSlot = fractionSlot;
    }

    GroupMemoryBarrierWithGroupSync();
    const uint32_t index = MrOctantCell(child.parentOctant, thread);
    g_cells[CellAddress(parentSlot, index)] = result.cell;
    if (gs_fractionSlot != MR_NO_FRACTION)
        g_fractions[CellAddress(gs_fractionSlot, index)] = result.fraction;

    if (thread == 0) {
        g_blocks[parentSlot].children[child.parentOctant] = MR_NO_BLOCK;
        g_blocks[record.slot] = MrMakeUnusedBlock();
    }

    // --- 次のレベル(親)---
    Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC);
    const bool recurse = record.levelsLeft > 1;
    GroupNodeOutputRecords<MrChainRecord> output = next.GetGroupNodeOutputRecords(recurse ? 1 : 0);
    if (recurse && thread == 0) {
        output.Get().slot = parentSlot;
        output.Get().levelsLeft = record.levelsLeft - 1;
    }

    output.OutputComplete();
}

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
