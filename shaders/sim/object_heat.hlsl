// object_heat.hlsl — 物の熱(物の箱のボクセルの熱と反応・セルとの対の面の 4 つの口)の GPU の段(T-1064・ADR-0434・ADR-0451・ADR-0459・20 §12.4)。
// 式は shaders/common/object_heat.hlsli。CPU リファレンス(tests/object_heat_cpu.h)と同じ段の順で、ビット一致を tests/gpu_object_heat_test.cpp が確かめる。
// 全部の物を 1 回の Dispatch でまとめて回す(物ごとに投げない)。物の中は 64 ボクセル以下のブロックに分け、静かなブロックは眠らせる(T-1098)。
// 1 刻み = 小刻みごとに 段(起こす → ①〜④)+ 反応の段 ⑤:
//   起こす OhWakeBlocks(1 スレッド = 1 ブロック): 自分・隣・囲むセル・光が最後の評価から変わったか → この段で回すか(u7)
//   ① OhPrepare(1 グループ = 1 ブロック): 起きているブロックのボクセルの温度・熱容量・物の中の伝導率 → u1
//   ② OhFlowStep(1 グループ = 1 ブロック): 囲むセル(u5 を読むだけ)との口と隣との伝導 → 自分のボクセル(u0)に当て、セルへ出す分を u2 に。
//      ブロックが変わったか・静かか(どの流れも 0)を u7 に
//   ③ OhReduce(1 グループ = 1 物): u2 を物ごとに足し、対の端数(u3。真実)を持ち越して粗い量を u4 に(整数の足し算なので順によらない)
//   ④ OhApplyCells(1 スレッド = 物の入っているセル 1 つ): そのセルの物の u4 を足してセルへ(u5)。変わったセルの段を u9 に
//   ⑤ OhReact(1 グループ = 1 ブロック): 最後の評価から変わった・起こす刻みが来たブロックだけ反応(RxStepCellWait。ボクセルの changedTick は u8)
// 眠っているブロックは 1 語読んで抜ける。物ごとの「起きている物の一覧」は T-1077。
#include "common/object_heat.hlsli"

RWStructuredBuffer<RxCell> g_voxels : register(u0);
RWStructuredBuffer<OhVoxelHeat> g_heats : register(u1);
RWStructuredBuffer<OhFlow> g_flows : register(u2);
RWStructuredBuffer<OhFlow> g_carries : register(u3);     // 物ごとの対の端数(真実)
RWStructuredBuffer<OhFlow> g_coarse : register(u4);      // 物ごとのセルへ渡す量(セルの単位)
RWStructuredBuffer<RxCell> g_cells : register(u5);       // 囲むセル
RWStructuredBuffer<uint32_t> g_failures : register(u6);  // [0] 量が負になる・成分の枠があふれた数
RWStructuredBuffer<OhBlockState> g_blockStates : register(u7);
RWStructuredBuffer<OhVoxelWait> g_waits : register(u8);     // ボクセルごとの反応の待ち(真実)
RWStructuredBuffer<uint32_t> g_cellChanged : register(u9);  // 囲むセルごとの最後に変わった段
StructuredBuffer<RxSpecies> g_species : register(t0);
StructuredBuffer<RxRule> g_rules : register(t1);
StructuredBuffer<uint32_t> g_ruleIndex : register(t2);
ByteAddressBuffer g_rates : register(t3);
StructuredBuffer<OhSpecies> g_objectSpecies : register(t4);
StructuredBuffer<OhObject> g_objects : register(t5);
StructuredBuffer<OhVoxelInfo> g_infos : register(t6);
StructuredBuffer<int64_t> g_light : register(t7);
StructuredBuffer<OhCellRange> g_ranges : register(t8);
StructuredBuffer<uint32_t> g_gasSpecies : register(t9);  // 対の面の気体の番号 → 物質 ID(OH_MAX_GASES 個。0 = 使わない)
StructuredBuffer<OhBlock> g_blocks : register(t10);

cbuffer RootConstants : register(b0) {
    uint32_t g_voxelCount;
    uint32_t g_objectCount;
    uint32_t g_rangeCount;
    uint32_t g_substep;  // 小刻みの番号
    uint32_t g_lightOn;
    uint32_t g_tickLow;
    uint32_t g_tickHigh;
    uint32_t g_seedLow;
    uint32_t g_seedHigh;
    uint32_t g_rowThreads;  // Dispatch の x のスレッドの数(グループの数が 65535 を超える時は y に折る)
    uint32_t g_blockCount;
    uint32_t g_pass;      // 段の通し番号(1 から)
    uint32_t g_sleeping;  // 0 = 全部のブロックを回す(眠らせ方の厳密さの比べ)
};

FX_CONST uint32_t OH_THREADS = 64;

uint32_t ThreadIndex(uint3 dispatchThreadId) {
    return dispatchThreadId.x + (dispatchThreadId.y * g_rowThreads);
}

uint32_t GroupIndex(uint3 groupId) {
    return groupId.x + (groupId.y * (g_rowThreads / OH_THREADS));
}

// reaction.hlsli の Table の約束 + 物の熱の物質の値
struct GpuObjectHeatTable {
    uint32_t unused;

    RxSpecies Species(uint32_t id) { return g_species[id]; }

    RxRule Rule(uint32_t id) { return g_rules[id]; }
    RxRule RateRule(uint32_t id) { return g_rules[id]; }

    uint32_t RuleIndex(uint32_t position) { return g_ruleIndex[position]; }

    uint64_t Rate(uint32_t rule, uint32_t kelvin) {
        return g_rates.Load<uint64_t>((rule * RX_RATE_TABLE_KELVINS + kelvin) * 8);
    }

    OhSpecies ObjectSpecies(uint32_t id) { return g_objectSpecies[id]; }
};

struct GpuGasList {
    uint32_t unused;

    uint32_t GasSpecies(uint32_t gas) { return g_gasSpecies[gas]; }
};

GpuObjectHeatTable MakeTable() {
    GpuObjectHeatTable table;
    table.unused = 0;

    return table;
}

GpuGasList MakeGases() {
    GpuGasList gases;
    gases.unused = 0;

    return gases;
}

uint64_t Tick() {
    return FX_U64(g_tickHigh, g_tickLow);
}

// --- 起こす(1 スレッド = 1 ブロック)---

[numthreads(OH_THREADS, 1, 1)] void OhWakeBlocks(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t index = ThreadIndex(dispatchThreadId);
    if (index >= g_blockCount)
        return;

    const OhBlock block = g_blocks[index];
    const OhObject object = g_objects[block.object];
    const OhBlockState state = g_blockStates[index];
    uint32_t newest = state.changedPass;
    for (uint32_t direction = 0; direction < 6; ++direction) {
        const uint32_t other = OhNeighbourBlock(object, block, direction);
        if (other != OH_NO_BLOCK)
            newest = max(newest, g_blockStates[other].facePass[direction ^ 1u]);
    }

    const bool awake = OhExchangeAwake(state, newest, g_cellChanged[object.cell], g_lightOn != 0);
    g_blockStates[index].awake = (g_sleeping == 0 || awake) ? 1u : 0u;
}

    // --- ① 下ごしらえ(1 グループ = 1 ブロック)---

    [numthreads(OH_THREADS, 1, 1)] void OhPrepare(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint32_t blockIndex = GroupIndex(groupId);
    if (blockIndex >= g_blockCount || g_blockStates[blockIndex].awake == 0)
        return;

    const OhBlock block = g_blocks[blockIndex];
    const uint32_t voxel = OhBlockVoxel(g_objects[block.object], block, threadId.x);
    if (voxel == OH_NO_BLOCK)
        return;

    g_heats[voxel] = OhPrepareVoxel(MakeTable(), g_voxels[voxel]);
}

// --- ② 流れ(1 グループ = 1 ブロック)---

groupshared uint32_t
    gs_marks;  // ビット 0 = どれかが変わった・ビット 1 = 静かでない・ビット 2〜7 = 変わったボクセルが並ぶ面

// 1 つのボクセルの流れ。戻り値は gs_marks のビット
uint32_t FlowVoxel(OhObject object, uint32_t voxel) {
    const OhVoxelInfo info = g_infos[voxel];
    const GpuObjectHeatTable table = MakeTable();
    const RxCell air = g_cells[object.cell];
    const RxThermal airThermal = RxComputeThermal(table, air);
    const uint64_t airConductivity = ((uint64_t)HcCellConductance(table, air)) << HC_CONDUCTANCE_SHIFT;
    const OhVoxelHeat heat = g_heats[voxel];
    int64_t inner = 0;
    bool faceFlow = false;
    for (uint32_t direction = 0; direction < 6; ++direction) {
        if ((info.shape & (1u << direction)) == 0)
            continue;

        const uint32_t other = (uint32_t)((int32_t)voxel + OhNeighbourOffset(object, direction));
        const int64_t flow = OhInnerConduction(object, heat, g_heats[other]);
        inner -= flow;
        faceFlow = faceFlow || flow != 0;
    }

    const int64_t sunlight = g_lightOn != 0 ? OhLightShare(g_light[voxel], g_substep, object.substeps) : (int64_t)0;
    const RxCell before = g_voxels[voxel];
    const OhVoxelStep step = OhApplyVoxel(table, MakeGases(), object, info.shape, before, heat, air, airThermal,
                                          airConductivity, inner, sunlight);
    const bool changed = !RxSameCell(step.cell, before);
    g_voxels[voxel] = step.cell;
    g_flows[voxel] = step.toCell;
    if (step.ok == 0)
        InterlockedAdd(g_failures[0], 1u);

    if (changed)
        g_waits[voxel].changedTick = Tick() - 1;

    return (changed ? 1u : 0u) | ((faceFlow || step.flowsZero == 0) ? 2u : 0u);
}

[numthreads(OH_THREADS, 1, 1)] void OhFlowStep(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint32_t blockIndex = GroupIndex(groupId);
    if (blockIndex >= g_blockCount || g_blockStates[blockIndex].awake == 0)
        return;

    const OhBlock block = g_blocks[blockIndex];
    const OhObject object = g_objects[block.object];
    const uint32_t voxel = OhBlockVoxel(object, block, threadId.x);
    const bool skipped = g_substep >= object.substeps;
    if (threadId.x == 0)
        gs_marks = 0;

    GroupMemoryBarrierWithGroupSync();
    if (voxel != OH_NO_BLOCK) {
        if (skipped)
            g_flows[voxel] = OhZeroFlow();
        else {
            const uint32_t mark = FlowVoxel(object, voxel);
            InterlockedOr(gs_marks, mark | ((mark & 1u) != 0 ? OhVoxelFaces(object, threadId.x) << 2 : 0u));
        }
    }

    GroupMemoryBarrierWithGroupSync();
    if (threadId.x != 0 || skipped)
        return;

    const uint32_t marks = gs_marks;
    if ((marks & 1u) != 0)
        g_blockStates[blockIndex].changedPass = g_pass;

    g_blockStates[blockIndex].quiet = marks == 0 ? 1u : 0u;
    g_blockStates[blockIndex].exchangePass = g_pass;
    for (uint32_t face = 0; face < 6; ++face) {
        if ((marks & (4u << face)) != 0)
            g_blockStates[blockIndex].facePass[face] = g_pass;
    }
}

// --- ③ 物ごとの和と対の端数(1 グループ = 1 物)---

groupshared int64_t gs_sum[OH_THREADS][OH_MAX_GASES + 1];
groupshared uint32_t gs_anyAwake;

[numthreads(OH_THREADS, 1, 1)] void OhReduce(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint32_t objectIndex = groupId.x;
    const uint32_t lane = threadId.x;
    const OhObject object = g_objects[objectIndex];

    // 物の全部のブロックが眠っていれば、記録は全部 0(静かなブロックの流れは 0)なので、ボクセルを読まずに粗い量 0 で抜ける(T-1098)
    const uint32_t blockCount = object.blocksX * object.blocksY * object.blocksZ;
    if (lane == 0)
        gs_anyAwake = 0;

    GroupMemoryBarrierWithGroupSync();
    for (uint32_t b = lane; b < blockCount; b += OH_THREADS) {
        if (g_blockStates[object.blockBegin + b].awake != 0)
            InterlockedOr(gs_anyAwake, 1u);
    }

    GroupMemoryBarrierWithGroupSync();
    if (gs_anyAwake == 0) {
        if (lane == 0)
            g_coarse[objectIndex] = OhSplitCarry(object, OhZeroFlow(), g_carries[objectIndex]).coarse;

        return;
    }

    OhFlow sum = OhZeroFlow();
    for (uint32_t v = lane; v < object.voxelCount; v += OH_THREADS)
        sum = OhAddFlow(sum, g_flows[object.voxelBegin + v]);

    gs_sum[lane][0] = sum.energy;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g)
        gs_sum[lane][g + 1] = sum.amounts[g];

    GroupMemoryBarrierWithGroupSync();
    for (uint32_t width = OH_THREADS / 2; width > 0; width /= 2) {
        if (lane < width) {
            FX_UNROLL
            for (uint32_t k = 0; k < OH_MAX_GASES + 1; ++k)
                gs_sum[lane][k] += gs_sum[lane + width][k];
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if (lane != 0)
        return;

    OhFlow total;
    total.energy = gs_sum[0][0];
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g)
        total.amounts[g] = gs_sum[0][g + 1];

    const OhCarryStep step = OhSplitCarry(object, total, g_carries[objectIndex]);
    g_carries[objectIndex] = step.carry;
    g_coarse[objectIndex] = step.coarse;
}

    // --- ④ セルへ当てる ---

    [numthreads(OH_THREADS, 1, 1)] void OhApplyCells(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t index = ThreadIndex(dispatchThreadId);
    if (index >= g_rangeCount)
        return;

    const OhCellRange range = g_ranges[index];
    OhFlow sum = OhZeroFlow();
    for (uint32_t o = 0; o < range.objectCount; ++o)
        sum = OhAddFlow(sum, g_coarse[range.objectBegin + o]);

    const OhAmountChange change = OhApplyToCell(MakeGases(), g_cells[range.cell], sum);
    g_cells[range.cell] = change.cell;
    if (change.ok == 0)
        InterlockedAdd(g_failures[0], 1u);

    if (!OhFlowIsZero(sum))
        g_cellChanged[range.cell] = g_pass;
}

// --- ⑤ 反応(1 グループ = 1 ブロック)---

groupshared uint32_t gs_reacted;  // ビット 0 = どれかが変わった・ビット 1〜6 = 変わったボクセルが並ぶ面
groupshared uint64_t gs_wake[OH_THREADS];

[numthreads(OH_THREADS, 1, 1)] void OhReact(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint32_t blockIndex = GroupIndex(groupId);
    if (blockIndex >= g_blockCount)
        return;

    const uint64_t tick = Tick();
    if (g_sleeping != 0 && !OhReactAwake(g_blockStates[blockIndex], tick))
        return;

    const OhBlock block = g_blocks[blockIndex];
    const OhObject object = g_objects[block.object];
    const uint32_t voxel = OhBlockVoxel(object, block, threadId.x);
    const uint32_t lane = threadId.x;
    if (lane == 0)
        gs_reacted = 0;

    gs_wake[lane] = RX_WAIT_NEVER;
    GroupMemoryBarrierWithGroupSync();
    if (voxel != OH_NO_BLOCK) {
        const RxCell cell = g_voxels[voxel];
        const OhVoxelWait wait = g_waits[voxel];
        const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
        const RxWaitStep step = RxStepCellWait(MakeTable(), cell, seed, tick, wait.changedTick, (uint64_t)voxel);
        if (!RxSameCell(step.cell, cell)) {
            g_voxels[voxel] = step.cell;
            g_waits[voxel].changedTick = tick;
            InterlockedOr(gs_reacted, 1u | (OhVoxelFaces(object, lane) << 1));
        }

        g_waits[voxel].wakeTick = step.wakeTick;
        gs_wake[lane] = step.wakeTick;
    }

    GroupMemoryBarrierWithGroupSync();
    for (uint32_t width = OH_THREADS / 2; width > 0; width /= 2) {
        if (lane < width)
            gs_wake[lane] = gs_wake[lane + width] < gs_wake[lane] ? gs_wake[lane + width] : gs_wake[lane];

        GroupMemoryBarrierWithGroupSync();
    }

    if (lane != 0)
        return;

    g_blockStates[blockIndex].minWakeTick = gs_wake[0];
    g_blockStates[blockIndex].reactPass = g_pass;
    const uint32_t reacted = gs_reacted;
    if (reacted != 0)
        g_blockStates[blockIndex].changedPass = g_pass;

    for (uint32_t face = 0; face < 6; ++face) {
        if ((reacted & (2u << face)) != 0)
            g_blockStates[blockIndex].facePass[face] = g_pass;
    }
}
