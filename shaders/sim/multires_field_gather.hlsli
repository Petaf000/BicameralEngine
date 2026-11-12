// multires_field_gather.hlsli — 集めて書く段(T-1252 段 2。ADR-0500 の 3・20 §10.7・ADR-0532 の 4・8)の 3 つの段の中身。
// CPU の GatherNestField(sim/multires_fields.cpp)と欄の頁の像(pageOf・空きのスタック・頁の値)までビット単位で同じにする。
// 2 つの入口が含める: 置き場 1 つの試験の段(multires_field_gather.hlsl)と、GPU の世界の木の段(multires_field_gather_tree.hlsl。
// 欄の区画の置き場・世界の枠の見出し・書いた枠をつつく)。段の中身は 1 つ(同じ頁の取り方・同じ足し方)。
// 含める側が先に定義するもの:
//   uint32_t GatherStoreWord(uint32_t address) / void SetGatherStoreWord(uint32_t address, uint32_t value)
//                                         欄の置き場(置き場の始まりからの語。並びは common/multires_field_pages.hlsli)
//   uint32_t GatherWorkWord(uint32_t address) / void SetGatherWorkWord(uint32_t address, uint32_t value)   作業
//   uint32_t GatherCellPage(uint32_t slot)  世界の枠のセルの頁(一様なら MR_NO_PAGE か MR_PAGE_WANTED)
//   uint32_t GatherWorldBlocks()            世界の枠の数(作業の並びの blockOf の長さ)
// 1 グループ = GATHER_THREADS スレッド(印・書くは 1 グループ = 世界の枠 1 つ・1 スレッド = セル 1 つ。頁を取るは 1 グループだけ)。
#ifndef BICAMERAL_MULTIRES_FIELD_GATHER_HLSLI
#define BICAMERAL_MULTIRES_FIELD_GATHER_HLSLI

#include "common/multires_field_pages.hlsli"

static const uint32_t GATHER_THREADS = 512;  // = MR_BLOCK_CELLS

int64_t GatherPair(uint32_t low, uint32_t high) {
    return (int64_t)(((uint64_t)high << 32) | (uint64_t)low);
}

int64_t LoadGatherWork(uint32_t address) {
    return GatherPair(GatherWorkWord(address), GatherWorkWord(address + 1));
}

int64_t LoadGatherStore(uint32_t address) {
    return GatherPair(GatherStoreWord(address), GatherStoreWord(address + 1));
}

void StoreGatherValue(uint32_t address, int64_t value) {
    SetGatherStoreWord(address, (uint32_t)((uint64_t)value));
    SetGatherStoreWord(address + 1, (uint32_t)((uint64_t)value >> 32));
}

uint32_t GatherComponents() {
    return GatherStoreWord(MF_HEADER_COMPONENTS);
}

uint32_t GatherStateAddress(uint32_t slot) {
    return MfWorkStateAddress(GatherWorldBlocks(), slot);
}

// --- 1. 印: 作業が 0 でない枠の段の結果(グループの全部のスレッドが同じ値を返す。書くのは呼ぶ側のスレッド 0)---

groupshared uint32_t gs_gatherNonZero;

uint32_t GatherMarkState(uint32_t slot, uint32_t index) {
    const uint32_t block = GatherWorkWord(MfWorkBlockOfAddress(slot));
    if (index == 0)
        gs_gatherNonZero = 0;

    GroupMemoryBarrierWithGroupSync();

    if (block != MF_NO_WORK) {
        const uint32_t components = GatherComponents();
        uint32_t any = 0;
        for (uint32_t c = 0; c < components; ++c) {
            const uint32_t address = MfWorkValueAddress(GatherWorldBlocks(), components, block, index, c);
            any |= GatherWorkWord(address) | GatherWorkWord(address + 1);  // 0 との比べは 2 語の OR(T-1078)
        }

        if (any != 0)
            InterlockedOr(gs_gatherNonZero, 1u);
    }

    GroupMemoryBarrierWithGroupSync();

    // 一様な枠(MR_NO_PAGE・MR_PAGE_WANTED)は頁に広げてから書く(CPU の ExpandGatheredBlocks と同じ選び)
    if (gs_gatherNonZero == 0)
        return MF_GATHER_NONE;

    return GatherCellPage(slot) >= MR_PAGE_WANTED ? MF_GATHER_WANTED : MF_GATHER_TOUCHED;
}

// --- 2. 頁を取る(1 グループ。枠の順のプレフィックス和)---

groupshared uint32_t gs_gatherScan[GATHER_THREADS];
groupshared uint32_t gs_gatherWritten;
groupshared uint32_t gs_gatherWanted;

// 欄の頁が要る枠か(TOUCHED で頁が無い)
bool GatherNeedsPage(uint32_t slot) {
    if (GatherWorkWord(GatherStateAddress(slot)) != MF_GATHER_TOUCHED)
        return false;

    return GatherStoreWord(MfPageOfAddress(GatherCellPage(slot))) == MF_NO_PAGE;
}

// 1 グループの value の累積和(含む)を gs_gatherScan に
void GatherInclusiveScan(uint32_t i, uint32_t value) {
    gs_gatherScan[i] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint32_t offset = 1; offset < GATHER_THREADS; offset <<= 1) {
        uint32_t sum = gs_gatherScan[i];
        if (i >= offset)
            sum += gs_gatherScan[i - offset];

        GroupMemoryBarrierWithGroupSync();
        gs_gatherScan[i] = sum;
        GroupMemoryBarrierWithGroupSync();
    }
}

// スレッド i が枠の塊 [begin, end) を受け持つ(塊の順 = 枠の順)。TOUCHED を ADD・NEW・SHORT に分け、報告と空きの数・高さを頭に書く
void GatherAssignPages(uint32_t i) {
    const uint32_t worldBlocks = GatherWorldBlocks();
    const uint32_t chunk = (worldBlocks + GATHER_THREADS - 1u) / GATHER_THREADS;
    const uint32_t begin = min(i * chunk, worldBlocks);
    const uint32_t end = min(begin + chunk, worldBlocks);
    if (i == 0) {
        gs_gatherWritten = 0;
        gs_gatherWanted = 0;
    }

    // --- 数える ---
    uint32_t needs = 0;
    uint32_t wanted = 0;
    for (uint32_t slot = begin; slot < end; ++slot) {
        needs += GatherNeedsPage(slot) ? 1u : 0u;
        wanted += GatherWorkWord(GatherStateAddress(slot)) == MF_GATHER_WANTED ? 1u : 0u;
    }

    GatherInclusiveScan(i, needs);
    const uint32_t total = gs_gatherScan[GATHER_THREADS - 1u];
    const uint32_t freeCount = GatherStoreWord(MF_HEADER_FREE_COUNT);
    const uint32_t top = GatherStoreWord(MF_HEADER_TOP);
    const uint32_t available = MfPagesAvailable(GatherStoreWord(MF_HEADER_CAPACITY), GatherStoreWord(MF_HEADER_BUDGET),
                                                freeCount, top);
    const uint32_t granted = min(total, available);

    // --- 頁を配る: k 番目に要る枠が空きのスタックの上から k 番目(その後は高さの上)---
    const uint32_t cellPages = GatherStoreWord(MF_HEADER_CELL_PAGES);
    uint32_t k = gs_gatherScan[i] - needs;
    uint32_t written = 0;
    for (uint32_t slot = begin; slot < end; ++slot) {
        const uint32_t stateAddress = GatherStateAddress(slot);
        if (GatherWorkWord(stateAddress) != MF_GATHER_TOUCHED)
            continue;

        if (!GatherNeedsPage(slot)) {
            SetGatherWorkWord(stateAddress, MF_GATHER_ADD);
            written += 1;
            continue;
        }

        if (k >= granted) {
            SetGatherWorkWord(stateAddress, MF_GATHER_SHORT);
            k += 1;
            continue;
        }

        uint32_t fieldPage = MfFreshPage(k, freeCount, top);
        if (MfTakesFreePage(k, freeCount))
            fieldPage = GatherStoreWord(MfFreeAddress(cellPages, MfFreePosition(k, freeCount)));

        SetGatherStoreWord(MfPageOfAddress(GatherCellPage(slot)), fieldPage);
        SetGatherWorkWord(stateAddress, MF_GATHER_NEW);
        written += 1;
        k += 1;
    }

    InterlockedAdd(gs_gatherWritten, written);
    InterlockedAdd(gs_gatherWanted, wanted);
    GroupMemoryBarrierWithGroupSync();

    if (i != 0)
        return;

    // --- 空きの数・高さ・報告 ---
    const uint32_t fromFree = min(granted, freeCount);
    SetGatherStoreWord(MF_HEADER_FREE_COUNT, freeCount - fromFree);
    SetGatherStoreWord(MF_HEADER_TOP, top + (granted - fromFree));
    SetGatherStoreWord(MF_HEADER_WRITTEN, gs_gatherWritten);
    SetGatherStoreWord(MF_HEADER_TAKEN, granted);
    SetGatherStoreWord(MF_HEADER_SHORTAGE, total - granted);
    SetGatherStoreWord(MF_HEADER_WANTED, gs_gatherWanted);
}

// --- 3. 書く(ADD / NEW の枠の作業を欄の頁へ足し、作業を 0 に。NEW は頁を 0 にしてから)。書いた枠なら true ---

bool GatherWriteCell(uint32_t slot, uint32_t index) {
    const uint32_t state = GatherWorkWord(GatherStateAddress(slot));
    if (state != MF_GATHER_ADD && state != MF_GATHER_NEW)
        return false;

    const uint32_t cellPages = GatherStoreWord(MF_HEADER_CELL_PAGES);
    const uint32_t capacity = GatherStoreWord(MF_HEADER_CAPACITY);
    const uint32_t components = GatherComponents();
    const uint32_t block = GatherWorkWord(MfWorkBlockOfAddress(slot));
    const uint32_t fieldPage = GatherStoreWord(MfPageOfAddress(GatherCellPage(slot)));
    for (uint32_t c = 0; c < components; ++c) {
        const uint32_t workAddress = MfWorkValueAddress(GatherWorldBlocks(), components, block, index, c);
        const uint32_t address = MfValueAddress(cellPages, capacity, components, fieldPage, index, c);
        const int64_t before = state == MF_GATHER_NEW ? 0 : LoadGatherStore(address);
        StoreGatherValue(address, before + LoadGatherWork(workAddress));
        SetGatherWorkWord(workAddress, 0);
        SetGatherWorkWord(workAddress + 1, 0);
    }

    return true;
}

#endif  // BICAMERAL_MULTIRES_FIELD_GATHER_HLSLI
