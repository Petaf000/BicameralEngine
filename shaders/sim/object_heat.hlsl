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
// 眠らせ方は 3 つ(g_sleeping。OH_SLEEP_*): 全部回す・全部を Dispatch して眠っているブロックは 1 語読んで抜ける・
// 起きている物の一覧(T-1077): 段ごとに OhListMark・OhListTotals・OhListPlace が物の印から一覧と間接の引数を作り、
// 起こす・①・②・③・⑤ は一覧だけを ExecuteIndirect で回す(眠っている物の費用は物 1 つに 1 スレッドの印の読みだけ)。
// 反応の後に OhFinishObjects が一覧の物の起こす刻みの最小を直す。どの形でも結果はビット単位で同じ(試験 gpu_object_heat・gpu_object_heat_nest)。
#include "common/object_heat.hlsli"

RWStructuredBuffer<RxCell> g_voxels : register(u0);
RWStructuredBuffer<OhVoxelHeat> g_heats : register(u1);
RWStructuredBuffer<OhFlow> g_flows : register(u2);
RWStructuredBuffer<OhFlow> g_carries : register(u3);     // 物ごとの対の端数(真実)
RWStructuredBuffer<OhFlow> g_coarse : register(u4);      // 物ごとのセルへ渡す量(セルの単位)
RWStructuredBuffer<RxCell> g_cells : register(u5);       // 囲むセル
RWStructuredBuffer<uint32_t> g_failures : register(u6);  // [0] 量が負になる・成分の枠があふれた数
RWStructuredBuffer<OhBlockState> g_blockStates : register(u7);
RWStructuredBuffer<OhVoxelWait> g_waits : register(u8);            // ボクセルごとの反応の待ち(真実)
RWStructuredBuffer<uint32_t> g_cellChanged : register(u9);         // 囲むセルごとの最後に変わった段
RWStructuredBuffer<OhObjectState> g_objectStates : register(u10);  // 物ごとの一覧の印(眠らせ方。真実ではない)
// [頭 OH_LIST_HEADER][一覧の物 × 物の数][一覧の物のブロックの始まり × 物の数][物の印 × 物の数][グループの和 × 2 × グループの数]
// [一覧のブロック × ブロックの数](グループ = 物 OH_LIST_GROUP_OBJECTS 個)
RWStructuredBuffer<uint32_t> g_lists : register(u11);
RWStructuredBuffer<uint32_t> g_dispatchArgs
    : register(u12);  // 間接の引数(D3D12_DISPATCH_ARGUMENTS × OH_LIST_ARGS_COUNT)
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
    uint32_t g_sleeping;  // OH_SLEEP_*(眠らせ方)
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

// --- 一覧(T-1077)---

bool Listed() {
    return g_sleeping == OH_SLEEP_LISTED;
}

uint32_t BlockListBase() {
    return OH_LIST_HEADER + (3 * g_objectCount) +
           (2 * ((g_objectCount + OH_LIST_GROUP_OBJECTS - 1) / OH_LIST_GROUP_OBJECTS));
}

// 段の通し番号 position のブロック(一覧なら一覧の position 番目。外なら OH_NO_BLOCK = g_blockCount 以上)
uint32_t BlockAt(uint32_t position) {
    if (!Listed())
        return position;

    return position < g_lists[1] ? g_lists[BlockListBase() + position] : OH_NO_BLOCK;
}

// 1 グループ = 1 物の段の物(一覧なら一覧の position 番目。外なら OH_NO_BLOCK)
uint32_t ObjectAt(uint32_t position) {
    if (!Listed())
        return position < g_objectCount ? position : OH_NO_BLOCK;

    return position < g_lists[0] ? g_lists[OH_LIST_HEADER + position] : OH_NO_BLOCK;
}

uint32_t BlockCountOf(OhObject object) {
    return object.blocksX * object.blocksY * object.blocksZ;
}

// --- 起こす(1 スレッド = 1 ブロック)---

[numthreads(OH_THREADS, 1, 1)] void OhWakeBlocks(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t index = BlockAt(ThreadIndex(dispatchThreadId));
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
    g_blockStates[index].awake = (g_sleeping == OH_SLEEP_ALL || awake) ? 1u : 0u;
}

    // --- ① 下ごしらえ(1 グループ = 1 ブロック)---

    [numthreads(OH_THREADS, 1, 1)] void OhPrepare(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint32_t blockIndex = BlockAt(GroupIndex(groupId));
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
    const uint64_t airConductivity = ((uint64_t)HcCellConductance(table, air, HC_ROOM_TEMPERATURE_MK))
                                     << HC_CONDUCTANCE_SHIFT;  // 室温の値(膜の温度の倍率は対の面で掛ける。T-1066)
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
    const uint32_t blockIndex = BlockAt(GroupIndex(groupId));
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

    // 変わった・静かでない: 物を次の段の一覧に入れる(隣のブロックへの伝わりも物の中だけ)
    if (marks != 0)
        InterlockedMax(g_objectStates[block.object].busyPass, g_pass);

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
    const uint32_t objectIndex = ObjectAt(GroupIndex(groupId));
    const uint32_t lane = threadId.x;
    if (objectIndex == OH_NO_BLOCK)
        return;

    const OhObject object = g_objects[objectIndex];

    // 物の全部のブロックが眠っていれば、記録は全部 0(静かなブロックの流れは 0)なので、ボクセルを読まずに粗い量 0 で抜ける(T-1098)
    const uint32_t blockCount = BlockCountOf(object);
    if (lane == 0) {
        gs_anyAwake = 0;
        g_objectStates[objectIndex].reducedPass = g_pass;
    }

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
    for (uint32_t o = 0; o < range.objectCount; ++o) {
        // この段に ③ を回さなかった物(一覧の外)の粗い量は 0(眠っている物の端数は粗い側の 1 単位に満たない)
        if (g_objectStates[range.objectBegin + o].reducedPass == g_pass)
            sum = OhAddFlow(sum, g_coarse[range.objectBegin + o]);
    }

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
    const uint32_t blockIndex = BlockAt(GroupIndex(groupId));
    if (blockIndex >= g_blockCount)
        return;

    const uint64_t tick = Tick();
    if (g_sleeping != OH_SLEEP_ALL && !OhReactAwake(g_blockStates[blockIndex], tick))
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
    if (reacted != 0) {
        g_blockStates[blockIndex].changedPass = g_pass;
        InterlockedMax(g_objectStates[block.object].busyPass, g_pass);
    }

    for (uint32_t face = 0; face < 6; ++face) {
        if ((reacted & (2u << face)) != 0)
            g_blockStates[blockIndex].facePass[face] = g_pass;
    }
}

// --- 起きている物の一覧(T-1077。g_sleeping = OH_SLEEP_LISTED の時だけ。g_substep = 小刻みの番号か OH_LIST_REACT)---
// 3 段で作る(どれも物の数に比例して並列。一つのグループで物を順になめると、物 1 万個で 1 回 70 µs ほどかかった):
//   OhListMark(1 グループ = 物 OH_LIST_GROUP 個): 物を一覧に入れるか・ブロックの数を印に書き、グループの和を書く(整数の足し算なので順によらない)
//   OhListTotals(1 グループ): グループの和の前の和(R5)→ グループの始まり・一覧の物とブロックの数・間接の引数
//   OhListPlace(1 グループ = 物 OH_LIST_GROUP 個): グループの中の前の和で一覧の中の位置を決め、物と物のブロックを一覧に書く
// 一覧から外れる物は、前に起こしたブロックの awake を 0 に戻す(全部を回した時に眠っているブロックが書く値)

static const uint32_t OH_LIST_GROUP = OH_LIST_GROUP_OBJECTS;
static const uint32_t OH_LIST_TOTAL_THREADS = 1024;

uint32_t ListGroupCount() {
    return (g_objectCount + OH_LIST_GROUP - 1) / OH_LIST_GROUP;
}

uint32_t ListMarkBase() {
    return OH_LIST_HEADER + (2 * g_objectCount);
}

uint32_t ListGroupBase() {
    return OH_LIST_HEADER + (3 * g_objectCount);
}

// 受け渡しの段: 物を一覧に入れるか。入れたら印を書く(小刻みを飛ばす物は受け渡しを回さないので、最後に回した段を変えない)
bool ListExchangeObject(uint32_t objectIndex) {
    const OhObject object = g_objects[objectIndex];
    OhObjectState state = g_objectStates[objectIndex];
    const bool listed = OhObjectExchangeListed(state, g_cellChanged[object.cell], g_lightOn != 0);
    if (listed) {
        if (g_substep < object.substeps)
            state.lastListed = g_pass;

        state.listedTick = g_tickLow;
        state.flags |= OH_OBJECT_AWAKE_LEFT;
    } else if ((state.flags & OH_OBJECT_AWAKE_LEFT) != 0) {
        for (uint32_t b = 0; b < BlockCountOf(object); ++b)
            g_blockStates[object.blockBegin + b].awake = 0;

        state.flags &= ~OH_OBJECT_AWAKE_LEFT;
    }

    g_objectStates[objectIndex] = state;

    return listed;
}

groupshared uint32_t gs_listObjects[OH_LIST_TOTAL_THREADS];
groupshared uint32_t gs_listBlocks[OH_LIST_TOTAL_THREADS];

// gs_listObjects・gs_listBlocks の前 count 個の前の和(含む。Hillis–Steele。グループの全部のスレッドが呼ぶ)
void InclusiveListScan(uint32_t lane, uint32_t count) {
    GroupMemoryBarrierWithGroupSync();
    for (uint32_t width = 1; width < count; width *= 2) {
        const uint32_t objectsLeft = (lane < count && lane >= width) ? gs_listObjects[lane - width] : 0u;
        const uint32_t blocksLeft = (lane < count && lane >= width) ? gs_listBlocks[lane - width] : 0u;
        GroupMemoryBarrierWithGroupSync();
        if (lane < count) {
            gs_listObjects[lane] += objectsLeft;
            gs_listBlocks[lane] += blocksLeft;
        }

        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(OH_LIST_GROUP, 1, 1)] void OhListMark(uint3 groupId : SV_GroupID, uint32_t lane : SV_GroupIndex) {
    const uint32_t objectIndex = (groupId.x * OH_LIST_GROUP) + lane;
    if (lane == 0) {
        gs_listObjects[0] = 0;
        gs_listBlocks[0] = 0;
    }

    GroupMemoryBarrierWithGroupSync();
    if (objectIndex < g_objectCount) {
        const bool react = g_substep == OH_LIST_REACT;
        const bool listed = react ? OhObjectReactListed(g_objectStates[objectIndex], Tick())
                                  : ListExchangeObject(objectIndex);
        const uint32_t blocks = listed ? BlockCountOf(g_objects[objectIndex]) : 0u;
        g_lists[ListMarkBase() + objectIndex] = listed ? (blocks << 1) | 1u : 0u;
        if (listed) {
            InterlockedAdd(gs_listObjects[0], 1u);
            InterlockedAdd(gs_listBlocks[0], blocks);
        }
    }

    GroupMemoryBarrierWithGroupSync();
    if (lane != 0)
        return;

    g_lists[ListGroupBase() + (2 * groupId.x)] = gs_listObjects[0];
    g_lists[ListGroupBase() + (2 * groupId.x) + 1] = gs_listBlocks[0];
}

void WriteListArgs(uint32_t slot, uint32_t groups) {
    const uint32_t base = 3 * slot;
    g_dispatchArgs[base] = min(groups, OH_LIST_ROW_GROUPS);
    g_dispatchArgs[base + 1] = max((groups + OH_LIST_ROW_GROUPS - 1) / OH_LIST_ROW_GROUPS, 1u);
    g_dispatchArgs[base + 2] = 1;
}

[numthreads(OH_LIST_TOTAL_THREADS, 1, 1)] void OhListTotals(uint32_t lane : SV_GroupIndex) {
    uint32_t objectsBefore = 0;  // ここまでの区切りの和(グループで一様)
    uint32_t blocksBefore = 0;
    for (uint32_t first = 0; first < ListGroupCount(); first += OH_LIST_TOTAL_THREADS) {
        const uint32_t group = first + lane;
        const uint32_t address = ListGroupBase() + (2 * group);
        const bool inside = group < ListGroupCount();
        const uint32_t objects = inside ? g_lists[address] : 0u;
        const uint32_t blocks = inside ? g_lists[address + 1] : 0u;
        gs_listObjects[lane] = objects;
        gs_listBlocks[lane] = blocks;
        InclusiveListScan(lane, OH_LIST_TOTAL_THREADS);

        // --- グループの始まり(含まない前の和)を同じ所に書き戻す ---
        if (inside) {
            g_lists[address] = objectsBefore + gs_listObjects[lane] - objects;
            g_lists[address + 1] = blocksBefore + gs_listBlocks[lane] - blocks;
        }

        objectsBefore += gs_listObjects[OH_LIST_TOTAL_THREADS - 1];
        blocksBefore += gs_listBlocks[OH_LIST_TOTAL_THREADS - 1];
        GroupMemoryBarrierWithGroupSync();  // 次の区切りが前の和を書き直す前に、全部が読み終える
    }

    if (lane != 0)
        return;

    g_lists[0] = objectsBefore;
    g_lists[1] = blocksBefore;
    WriteListArgs(OH_LIST_ARGS_WAKE, (blocksBefore + OH_THREADS - 1) / OH_THREADS);
    WriteListArgs(OH_LIST_ARGS_BLOCKS, blocksBefore);
    WriteListArgs(OH_LIST_ARGS_OBJECTS, objectsBefore);
}

    [numthreads(OH_LIST_GROUP, 1, 1)] void OhListPlace(uint3 groupId : SV_GroupID, uint32_t lane : SV_GroupIndex) {
    const uint32_t objectIndex = (groupId.x * OH_LIST_GROUP) + lane;
    const uint32_t mark = objectIndex < g_objectCount ? g_lists[ListMarkBase() + objectIndex] : 0u;
    const uint32_t blocks = mark >> 1;
    gs_listObjects[lane] = mark & 1u;
    gs_listBlocks[lane] = blocks;
    InclusiveListScan(lane, OH_LIST_GROUP);
    if ((mark & 1u) == 0)
        return;

    const uint32_t groupAddress = ListGroupBase() + (2 * groupId.x);
    const uint32_t position = g_lists[groupAddress] + gs_listObjects[lane] - 1;
    const uint32_t offset = g_lists[groupAddress + 1] + gs_listBlocks[lane] - blocks;
    g_lists[OH_LIST_HEADER + position] = objectIndex;
    g_lists[OH_LIST_HEADER + g_objectCount + position] = offset;

    const uint32_t blockBegin = g_objects[objectIndex].blockBegin;
    for (uint32_t b = 0; b < blocks; ++b)
        g_lists[BlockListBase() + offset + b] = blockBegin + b;
}

// 反応の後: 一覧の物の起こす刻みの最小を直す(1 グループ = 一覧の物 1 つ。一覧の外の物のブロックは変わっていない)
[numthreads(OH_THREADS, 1, 1)] void OhFinishObjects(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint32_t objectIndex = ObjectAt(GroupIndex(groupId));
    const uint32_t lane = threadId.x;
    if (objectIndex == OH_NO_BLOCK)
        return;

    const OhObject object = g_objects[objectIndex];
    uint64_t earliest = RX_WAIT_NEVER;
    for (uint32_t b = lane; b < BlockCountOf(object); b += OH_THREADS) {
        const uint64_t wake = g_blockStates[object.blockBegin + b].minWakeTick;
        earliest = wake < earliest ? wake : earliest;
    }

    gs_wake[lane] = earliest;
    GroupMemoryBarrierWithGroupSync();
    for (uint32_t width = OH_THREADS / 2; width > 0; width /= 2) {
        if (lane < width)
            gs_wake[lane] = gs_wake[lane + width] < gs_wake[lane] ? gs_wake[lane + width] : gs_wake[lane];

        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0)
        g_objectStates[objectIndex].minWakeTick = gs_wake[0];
}
