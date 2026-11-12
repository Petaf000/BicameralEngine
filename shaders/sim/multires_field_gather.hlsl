// multires_field_gather.hlsl — 集めて書く段(T-1252 段 2。ADR-0500 の 3・20 §10.7)の GPU 版。CPU の GatherNestField(sim/multires_fields.cpp)と
// 欄の頁の像(pageOf・空きのスタック・頁の値)までビット単位で同じにする。並びは common/multires_field_pages.hlsli。
// 3 つの段を順に(段の間は UAV のバリア):
//   GatherMark   (1 グループ = 世界の枠 1 つ): 作業が 0 でない枠に印。頁を持つ枠は TOUCHED・一様な枠は WANTED(木の段が頁に広げてから
//                もう一度この段を回す。CPU の ExpandGatheredBlocks と同じ枠を選ぶ)
//   GatherAssign (1 グループだけ): 枠の順のプレフィックス和で、欄の頁の無い TOUCHED の枠に頁を取る(k 番目は空きのスタックの上から k 番目・
//                その後は高さの上。取れる数を超えたら SHORT = 書かない・作業に残す)。報告の数と空きの数・高さを書く
//   GatherWrite  (1 グループ = 世界の枠 1 つ・1 スレッド = セル 1 つ): ADD / NEW の枠の作業を欄の頁へ足し、作業を 0 に(NEW は頁を 0 にしてから)
// 入力: u0 = 欄の置き場・u1 = 作業・t0 = 枠ごとの (セルの頁, 種類) の 2 語(木の段に載せる時は g_blocks を読む)・b0 = 世界の枠の数。
// 書いた枠をつつく(CPU の PokeBlock)のは木の段に載せる時(GPU の世界の枠の見出しを持つ段)。tests/gpu_field_gather_test.cpp が CPU と比べる。
#include "common/multires_field_pages.hlsli"

RWStructuredBuffer<uint32_t> g_store : register(u0);
RWStructuredBuffer<uint32_t> g_work : register(u1);
StructuredBuffer<uint32_t> g_blockWords : register(t0);

cbuffer RootConstants : register(b0) {
    uint32_t g_worldBlocks;
};

static const uint32_t GATHER_THREADS = 512;  // = MR_BLOCK_CELLS

int64_t LoadPair(uint32_t low, uint32_t high) {
    return (int64_t)(((uint64_t)high << 32) | (uint64_t)low);
}

int64_t LoadWork(uint32_t address) {
    return LoadPair(g_work[address], g_work[address + 1]);
}

int64_t LoadStore(uint32_t address) {
    return LoadPair(g_store[address], g_store[address + 1]);
}

void StoreValue(uint32_t address, int64_t value) {
    g_store[address] = (uint32_t)((uint64_t)value);
    g_store[address + 1] = (uint32_t)((uint64_t)value >> 32);
}

uint32_t CellPages() {
    return g_store[MF_HEADER_CELL_PAGES];
}

uint32_t Capacity() {
    return g_store[MF_HEADER_CAPACITY];
}

uint32_t Components() {
    return g_store[MF_HEADER_COMPONENTS];
}

uint32_t PageOfSlot(uint32_t slot) {
    return g_blockWords[slot * 2u];
}

// --- 1. 印 ---

groupshared uint32_t gs_nonZero;

[numthreads(GATHER_THREADS, 1, 1)] void GatherMark(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint32_t slot = groupId.x;
    const uint32_t index = threadId.x;
    const uint32_t block = g_work[MfWorkBlockOfAddress(slot)];
    if (index == 0)
        gs_nonZero = 0;

    GroupMemoryBarrierWithGroupSync();

    if (block != MF_NO_WORK) {
        const uint32_t components = Components();
        uint32_t any = 0;
        for (uint32_t c = 0; c < components; ++c)
            any |= LoadWork(MfWorkValueAddress(g_worldBlocks, components, block, index, c)) != 0 ? 1u : 0u;

        if (any != 0)
            InterlockedOr(gs_nonZero, 1u);
    }

    GroupMemoryBarrierWithGroupSync();

    if (index != 0)
        return;

    // 一様な枠(MR_NO_PAGE・MR_PAGE_WANTED)は頁に広げてから書く(CPU の ExpandGatheredBlocks と同じ選び)
    uint32_t state = MF_GATHER_NONE;
    if (gs_nonZero != 0)
        state = PageOfSlot(slot) >= MR_PAGE_WANTED ? MF_GATHER_WANTED : MF_GATHER_TOUCHED;

    g_work[MfWorkStateAddress(g_worldBlocks, slot)] = state;
}

// --- 2. 頁を取る(枠の順のプレフィックス和)---

groupshared uint32_t gs_scan[GATHER_THREADS];
groupshared uint32_t gs_written;
groupshared uint32_t gs_wanted;

// 欄の頁が要る枠か(TOUCHED で頁が無い)
bool NeedsPage(uint32_t slot) {
    if (g_work[MfWorkStateAddress(g_worldBlocks, slot)] != MF_GATHER_TOUCHED)
        return false;

    return g_store[MfPageOfAddress(PageOfSlot(slot))] == MF_NO_PAGE;
}

// 1 グループの value の累積和(含む)を gs_scan に
void InclusiveScan(uint32_t i, uint32_t value) {
    gs_scan[i] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint32_t offset = 1; offset < GATHER_THREADS; offset <<= 1) {
        uint32_t sum = gs_scan[i];
        if (i >= offset)
            sum += gs_scan[i - offset];

        GroupMemoryBarrierWithGroupSync();
        gs_scan[i] = sum;
        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(GATHER_THREADS, 1, 1)] void GatherAssign(uint3 threadId : SV_GroupThreadID) {
    const uint32_t i = threadId.x;
    const uint32_t chunk = (g_worldBlocks + GATHER_THREADS - 1u) / GATHER_THREADS;
    const uint32_t begin = min(i * chunk, g_worldBlocks);
    const uint32_t end = min(begin + chunk, g_worldBlocks);
    if (i == 0) {
        gs_written = 0;
        gs_wanted = 0;
    }

    // --- 数える(スレッドごとに続いた枠の塊。塊の順 = 枠の順)---
    uint32_t needs = 0;
    uint32_t touched = 0;
    uint32_t wanted = 0;
    for (uint32_t slot = begin; slot < end; ++slot) {
        const uint32_t state = g_work[MfWorkStateAddress(g_worldBlocks, slot)];
        needs += NeedsPage(slot) ? 1u : 0u;
        touched += state == MF_GATHER_TOUCHED ? 1u : 0u;
        wanted += state == MF_GATHER_WANTED ? 1u : 0u;
    }

    InclusiveScan(i, needs);
    const uint32_t total = gs_scan[GATHER_THREADS - 1u];
    const uint32_t freeCount = g_store[MF_HEADER_FREE_COUNT];
    const uint32_t top = g_store[MF_HEADER_TOP];
    const uint32_t available = MfPagesAvailable(Capacity(), g_store[MF_HEADER_BUDGET], freeCount, top);
    const uint32_t granted = min(total, available);

    // --- 頁を配る: k 番目に要る枠が空きのスタックの上から k 番目(その後は高さの上)---
    const uint32_t cellPages = CellPages();
    uint32_t k = gs_scan[i] - needs;
    uint32_t written = 0;
    for (uint32_t slot = begin; slot < end; ++slot) {
        const uint32_t stateAddress = MfWorkStateAddress(g_worldBlocks, slot);
        if (g_work[stateAddress] != MF_GATHER_TOUCHED)
            continue;

        if (!NeedsPage(slot)) {
            g_work[stateAddress] = MF_GATHER_ADD;
            written += 1;
            continue;
        }

        if (k >= granted) {
            g_work[stateAddress] = MF_GATHER_SHORT;
            k += 1;
            continue;
        }

        uint32_t fieldPage = MfFreshPage(k, freeCount, top);
        if (MfTakesFreePage(k, freeCount))
            fieldPage = g_store[MfFreeAddress(cellPages, MfFreePosition(k, freeCount))];

        g_store[MfPageOfAddress(PageOfSlot(slot))] = fieldPage;
        g_work[stateAddress] = MF_GATHER_NEW;
        written += 1;
        k += 1;
    }

    InterlockedAdd(gs_written, written);
    InterlockedAdd(gs_wanted, wanted);
    GroupMemoryBarrierWithGroupSync();

    if (i != 0)
        return;

    // --- 空きの数・高さ・報告(touched は数えるだけ: 書いた + 尽きた)---
    const uint32_t fromFree = min(granted, freeCount);
    g_store[MF_HEADER_FREE_COUNT] = freeCount - fromFree;
    g_store[MF_HEADER_TOP] = top + (granted - fromFree);
    g_store[MF_HEADER_WRITTEN] = gs_written;
    g_store[MF_HEADER_TAKEN] = granted;
    g_store[MF_HEADER_SHORTAGE] = total - granted;
    g_store[MF_HEADER_WANTED] = gs_wanted;
}

    // --- 3. 書く ---

    [numthreads(GATHER_THREADS, 1, 1)] void GatherWrite(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint32_t slot = groupId.x;
    const uint32_t index = threadId.x;
    const uint32_t state = g_work[MfWorkStateAddress(g_worldBlocks, slot)];
    if (state != MF_GATHER_ADD && state != MF_GATHER_NEW)
        return;

    const uint32_t cellPages = CellPages();
    const uint32_t capacity = Capacity();
    const uint32_t components = Components();
    const uint32_t block = g_work[MfWorkBlockOfAddress(slot)];
    const uint32_t fieldPage = g_store[MfPageOfAddress(PageOfSlot(slot))];
    for (uint32_t c = 0; c < components; ++c) {
        const uint32_t workAddress = MfWorkValueAddress(g_worldBlocks, components, block, index, c);
        const uint32_t address = MfValueAddress(cellPages, capacity, components, fieldPage, index, c);
        const int64_t before = state == MF_GATHER_NEW ? 0 : LoadStore(address);
        StoreValue(address, before + LoadWork(workAddress));
        g_work[workAddress] = 0;
        g_work[workAddress + 1] = 0;
    }
}
