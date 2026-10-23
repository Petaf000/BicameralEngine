// object_heat.hlsl — 物の熱(物の箱のボクセルの熱と反応・セルとの対の面の 4 つの口)の GPU の段(T-1064・ADR-0434・20 §12.4)。
// 式は shaders/common/object_heat.hlsli。CPU リファレンス(tests/object_heat_cpu.h)と同じ段の順で、ビット一致を tests/gpu_object_heat_test.cpp が確かめる。
// 全部の物を 1 回の Dispatch でまとめて回す(物ごとに投げない)。1 刻み = 小刻みごとに ①〜④ + ⑤:
//   ① OhPrepare(1 スレッド = 1 ボクセル): 温度・熱容量・物の中の伝導率 → u1
//   ② OhFlowStep(1 スレッド = 1 ボクセル): 囲むセル(u5 を読むだけ)との口と隣との伝導 → 自分のボクセル(u0)に当て、セルへ出す分を u2 に
//   ③ OhReduce(1 グループ = 1 物): u2 を物ごとに足し、対の端数(u3。真実)を持ち越して粗い量を u4 に(整数の足し算なので順によらない)
//   ④ OhApplyCells(1 スレッド = 物の入っているセル 1 つ): そのセルの物の u4 を足してセルへ(u5)
//   ⑤ OhReact(1 スレッド = 1 ボクセル): 反応(RxStepCellWait。セルと同じ関数)
// 眠っている物を回さない形(起きている物の一覧)は T-1077。今は渡した物を全部回す。
#include "common/object_heat.hlsli"

RWStructuredBuffer<RxCell> g_voxels : register(u0);
RWStructuredBuffer<OhVoxelHeat> g_heats : register(u1);
RWStructuredBuffer<OhFlow> g_flows : register(u2);
RWStructuredBuffer<OhFlow> g_carries : register(u3);     // 物ごとの対の端数(真実)
RWStructuredBuffer<OhFlow> g_coarse : register(u4);      // 物ごとのセルへ渡す量(セルの単位)
RWStructuredBuffer<RxCell> g_cells : register(u5);       // 囲むセル
RWStructuredBuffer<uint32_t> g_failures : register(u6);  // [0] 量が負になる・成分の枠があふれた数
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
};

FX_CONST uint32_t OH_THREADS = 64;

uint32_t ThreadIndex(uint3 dispatchThreadId) {
    return dispatchThreadId.x + (dispatchThreadId.y * g_rowThreads);
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

// --- ① 下ごしらえ ---

[numthreads(OH_THREADS, 1, 1)] void OhPrepare(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t index = ThreadIndex(dispatchThreadId);
    if (index >= g_voxelCount)
        return;

    g_heats[index] = OhPrepareVoxel(MakeTable(), g_voxels[index]);
}

    // --- ② 流れ ---

    [numthreads(OH_THREADS, 1, 1)] void OhFlowStep(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t index = ThreadIndex(dispatchThreadId);
    if (index >= g_voxelCount)
        return;

    const OhVoxelInfo info = g_infos[index];
    const OhObject object = g_objects[info.object];
    if (g_substep >= object.substeps) {
        g_flows[index] = OhZeroFlow();
        return;
    }

    const GpuObjectHeatTable table = MakeTable();
    const RxCell air = g_cells[object.cell];
    const RxThermal airThermal = RxComputeThermal(table, air);
    const uint64_t airConductivity = ((uint64_t)HcCellConductance(table, air)) << HC_CONDUCTANCE_SHIFT;
    const OhVoxelHeat heat = g_heats[index];
    int64_t inner = 0;
    for (uint32_t direction = 0; direction < 6; ++direction) {
        if ((info.shape & (1u << direction)) == 0)
            continue;

        const uint32_t other = (uint32_t)((int32_t)index + OhNeighbourOffset(object, direction));
        inner -= OhInnerConduction(object, heat, g_heats[other]);
    }

    const int64_t sunlight = g_lightOn != 0 ? OhLightShare(g_light[index], g_substep, object.substeps) : (int64_t)0;
    const OhVoxelStep step = OhApplyVoxel(table, MakeGases(), object, info.shape, g_voxels[index], heat, air,
                                          airThermal, airConductivity, inner, sunlight);
    g_voxels[index] = step.cell;
    g_flows[index] = step.toCell;
    if (step.ok == 0)
        InterlockedAdd(g_failures[0], 1u);
}

// --- ③ 物ごとの和と対の端数(1 グループ = 1 物)---

groupshared int64_t gs_sum[OH_THREADS][OH_MAX_GASES + 1];

[numthreads(OH_THREADS, 1, 1)] void OhReduce(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint32_t objectIndex = groupId.x;
    const uint32_t lane = threadId.x;
    const OhObject object = g_objects[objectIndex];
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
}

// --- ⑤ 反応 ---

[numthreads(OH_THREADS, 1, 1)] void OhReact(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t index = ThreadIndex(dispatchThreadId);
    if (index >= g_voxelCount)
        return;

    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const RxWaitStep step = RxStepCellWait(MakeTable(), g_voxels[index], seed, tick, tick - 1, (uint64_t)index);
    g_voxels[index] = step.cell;
}
