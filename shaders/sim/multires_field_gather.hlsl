// multires_field_gather.hlsl — 集めて書く段(T-1252 段 2。ADR-0500 の 3・20 §10.7)の置き場 1 つの GPU 版(試験の段)。
// 段の中身は sim/multires_field_gather.hlsli(GPU の世界の木の段 multires_field_gather_tree.hlsl と同じ)。CPU の GatherNestField と
// 欄の頁の像(pageOf・空きのスタック・頁の値)までビット単位で同じにする。並びは common/multires_field_pages.hlsli。
// 3 つの段を順に(段の間は UAV のバリア):
//   GatherMark   (1 グループ = 世界の枠 1 つ): 作業が 0 でない枠に印。頁を持つ枠は TOUCHED・一様な枠は WANTED
//   GatherAssign (1 グループだけ): 枠の順のプレフィックス和で、欄の頁の無い TOUCHED の枠に頁を取る(取れる数を超えたら SHORT)
//   GatherWrite  (1 グループ = 世界の枠 1 つ・1 スレッド = セル 1 つ): ADD / NEW の枠の作業を欄の頁へ足し、作業を 0 に
// 入力: u0 = 欄の置き場・u1 = 作業・t0 = 枠ごとの (セルの頁, 種類) の 2 語・b0 = 世界の枠の数。
// tests/gpu_field_gather_test.cpp が CPU と比べる。
#include "common/multires_field_pages.hlsli"

RWStructuredBuffer<uint32_t> g_store : register(u0);
RWStructuredBuffer<uint32_t> g_work : register(u1);
StructuredBuffer<uint32_t> g_blockWords : register(t0);

cbuffer RootConstants : register(b0) {
    uint32_t g_worldBlocks;
};

uint32_t GatherStoreWord(uint32_t address) {
    return g_store[address];
}

void SetGatherStoreWord(uint32_t address, uint32_t value) {
    g_store[address] = value;
}

uint32_t GatherWorkWord(uint32_t address) {
    return g_work[address];
}

void SetGatherWorkWord(uint32_t address, uint32_t value) {
    g_work[address] = value;
}

uint32_t GatherCellPage(uint32_t slot) {
    return g_blockWords[slot * 2u];
}

uint32_t GatherWorldBlocks() {
    return g_worldBlocks;
}

#include "sim/multires_field_gather.hlsli"

[numthreads(GATHER_THREADS, 1, 1)] void GatherMark(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint32_t state = GatherMarkState(groupId.x, threadId.x);
    if (threadId.x == 0)
        SetGatherWorkWord(GatherStateAddress(groupId.x), state);
}

    [numthreads(GATHER_THREADS, 1, 1)] void GatherAssign(uint3 threadId : SV_GroupThreadID) {
    GatherAssignPages(threadId.x);
}

[numthreads(GATHER_THREADS, 1, 1)] void GatherWrite(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    GatherWriteCell(groupId.x, threadId.x);
}
