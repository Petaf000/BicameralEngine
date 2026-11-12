// multires_field_access.hlsli — 木の段(multires_tree.hlsl・multires_graph.hlsl)が GPU の世界の欄の区画
// (u4 の層の置き場の後ろ。並びは common/multires_field_pages.hlsli。始まりの語 g_external2・載せた欄の数 g_external3)を読み書きする道具
// (T-1252 段 2 の 4 本目。ADR-0500 の 3・ADR-0532 の 3・5)。CPU リファレンスは sim/multires_tree.cpp の AllocateOwnerPages・ReleaseAll と
// sim/multires_fields.cpp の RefineOwners・CoarsenOwners・CoarsenKeepsUniform・ExpandPage(同じ規則・同じ順)。
// 区画を持たない世界(g_external3 = 0)は何も読まない。木の段の間は u4 = 層の置き場と区画の 1 本のバッファ(sim/gpu_multires.cpp の BindLayers)。
// この刻みだけの欄は木の段では全部 0(刻みの境目で 0。CPU の TickOnlyFieldsClear)なので、細かく・粗くでは値を書かず、頁を返すだけ。
// 64bit の値の 0 との比べ・同じ値どうしの比べは 32bit の 2 語で(T-1078・T-1159)。
#ifndef BICAMERAL_MULTIRES_FIELD_ACCESS_HLSLI
#define BICAMERAL_MULTIRES_FIELD_ACCESS_HLSLI

#include "common/multires_field_pages.hlsli"
#include "common/state_field.hlsli"
#include "sim/multires_layer_access.hlsli"

// --- 区画の表と置き場の頭 ---

uint32_t CarriedFieldCount() {
    return g_external3;
}

uint32_t CarriedTable(uint32_t field, uint32_t word) {
    return g_layers[g_external2 + MfRegionTableAddress(field, word)];
}

uint32_t CarriedOwner(uint32_t field) {
    return CarriedTable(field, MF_TABLE_OWNER);
}

bool CarriedOwned(uint32_t field) {
    return CarriedOwner(field) != SF_NO_OWNER;
}

// 欄 field の置き場の始まり(g_layers の語)
uint32_t CarriedStoreBase(uint32_t field) {
    return g_external2 + CarriedTable(field, MF_TABLE_STORE);
}

uint32_t CarriedHeader(uint32_t field, uint32_t word) {
    return g_layers[CarriedStoreBase(field) + word];
}

uint32_t CarriedComponents(uint32_t field) {
    return CarriedHeader(field, MF_HEADER_COMPONENTS);
}

// セルの頁 cellPage の欄の頁(無い・セルの頁が無いなら MF_NO_PAGE)
uint32_t CarriedPageOf(uint32_t field, uint32_t cellPage) {
    if (cellPage == MR_NO_PAGE)
        return MF_NO_PAGE;

    return g_layers[CarriedStoreBase(field) + MfPageOfAddress(cellPage)];
}

void SetCarriedPageOf(uint32_t field, uint32_t cellPage, uint32_t fieldPage) {
    g_layers[CarriedStoreBase(field) + MfPageOfAddress(cellPage)] = fieldPage;
}

// 空きのスタックの位置 position の欄の頁
uint32_t CarriedFreePage(uint32_t field, uint32_t position) {
    return g_layers[CarriedStoreBase(field) + MfFreeAddress(CarriedHeader(field, MF_HEADER_CELL_PAGES), position)];
}

void SetCarriedFreePage(uint32_t field, uint32_t position, uint32_t fieldPage) {
    g_layers[CarriedStoreBase(field) + MfFreeAddress(CarriedHeader(field, MF_HEADER_CELL_PAGES), position)] = fieldPage;
}

// --- 値(int64 = 下位・上位の 2 語)---

uint32_t CarriedValueAddress(uint32_t field, uint32_t fieldPage, uint32_t index, uint32_t component) {
    const uint32_t base = CarriedStoreBase(field);

    return base + MfValueAddress(g_layers[base + MF_HEADER_CELL_PAGES], g_layers[base + MF_HEADER_CAPACITY],
                                 g_layers[base + MF_HEADER_COMPONENTS], fieldPage, index, component);
}

uint32_t CarriedUniformAddress(uint32_t field, uint32_t slot, uint32_t component) {
    return g_external2 +
           MfUniformAddress(CarriedTable(field, MF_TABLE_UNIFORM), CarriedComponents(field), slot, component);
}

int64_t LoadCarriedWords(uint32_t address) {
    return (int64_t)FX_U64(g_layers[address + 1], g_layers[address]);
}

void StoreCarriedWords(uint32_t address, int64_t value) {
    g_layers[address] = (uint32_t)((uint64_t)value);
    g_layers[address + 1] = (uint32_t)((uint64_t)value >> 32);
}

bool CarriedWordsNonZero(uint32_t address) {
    return (g_layers[address] | g_layers[address + 1]) != 0u;
}

// 欄の頁 fieldPage(MF_NO_PAGE なら 0)のセル index の値
int64_t LoadCarriedPage(uint32_t field, uint32_t fieldPage, uint32_t index, uint32_t component) {
    if (fieldPage == MF_NO_PAGE)
        return 0;

    return LoadCarriedWords(CarriedValueAddress(field, fieldPage, index, component));
}

// 欄の頁へ書く(CPU の StorePage: 頁が無い所へは 0 だけ書いてよい = 書かない。0 でない値は割り当ての段が頁を取ってある)
void StoreCarriedPage(uint32_t field, uint32_t fieldPage, uint32_t index, uint32_t component, int64_t value) {
    if (fieldPage == MF_NO_PAGE) {
        FX_ASSERT(((uint32_t)((uint64_t)value) | (uint32_t)((uint64_t)value >> 32)) == 0u);
        return;
    }

    StoreCarriedWords(CarriedValueAddress(field, fieldPage, index, component), value);
}

int64_t LoadCarriedUniform(uint32_t field, uint32_t slot, uint32_t component) {
    return LoadCarriedWords(CarriedUniformAddress(field, slot, component));
}

void StoreCarriedUniform(uint32_t field, uint32_t slot, uint32_t component, int64_t value) {
    StoreCarriedWords(CarriedUniformAddress(field, slot, component), value);
}

// 枠 slot の一様の値に 0 でないものがあるか(CPU の OwnerUniformNonZero)
bool CarriedUniformNonZero(uint32_t field, uint32_t slot) {
    const uint32_t components = CarriedComponents(field);
    bool any = false;
    for (uint32_t c = 0; c < components; ++c)
        any = any || CarriedWordsNonZero(CarriedUniformAddress(field, slot, c));

    return any;
}

// ブロックの欄の値(CPU の LoadNestField: 一様な枠は覆われたセルが 0・ほかは一様の値。頁を持つ枠は欄の頁か 0)
int64_t LoadCarriedCell(uint32_t field, MrBlock block, uint32_t slot, uint32_t index, uint32_t component) {
    if (MrIsUniform(block))
        return MrIsCoveredCell(block, index) ? (int64_t)0 : LoadCarriedUniform(field, slot, component);

    return LoadCarriedPage(field, CarriedPageOf(field, block.page), index, component);
}

// 子のブロックのセル index は、親のセルの何番目の子か(MrChildCell の j。CPU の ChildOfCell)
uint32_t CarriedChildOfCell(uint32_t index) {
    return (MrCellX(index) & 1u) | ((MrCellY(index) & 1u) << 1) | ((MrCellZ(index) & 1u) << 2);
}

// --- 粗くしても一様な親のままでよいか(CPU の CoarsenKeepsUniform の載せる欄の分)---
// 載せる欄があれば、頁を持つ子は広げる(控えめ)。一様な子は配り手の欄は配り手の粗くする関数で親の値と余り 0・この刻みだけの欄は同じ値
bool CoarsenKeepsUniformCarried(uint32_t childSlot, uint32_t parentSlot) {
    const uint32_t count = CarriedFieldCount();
    if (count == 0)
        return true;

    if (!MrIsUniform(g_blocks[childSlot]))
        return false;

    bool keeps = true;
    for (uint32_t f = 0; f < count; ++f) {
        const uint32_t owner = CarriedOwner(f);
        const uint32_t components = CarriedComponents(f);
        for (uint32_t c = 0; c < components; ++c) {
            const int64_t childValue = LoadCarriedUniform(f, childSlot, c);
            const int64_t parentValue = LoadCarriedUniform(f, parentSlot, c);
            int64_t coarse = childValue;
            uint32_t remainder = 0;
            if (owner != SF_NO_OWNER) {
                SfFamily family;
                FX_UNROLL
                for (uint32_t j = 0; j < SF_CHILDREN; ++j)
                    family.values[j] = childValue;

                const SfCoarsenSum sum = SfOwnerCoarsen(owner, family);
                coarse = SfCoarsenValue(sum);
                remainder = SfCoarsenRemainder(sum);
            }

            const bool sameLow = (uint32_t)((uint64_t)coarse) == (uint32_t)((uint64_t)parentValue);
            const bool sameHigh = (uint32_t)((uint64_t)coarse >> 32) == (uint32_t)((uint64_t)parentValue >> 32);
            keeps = keeps && sameLow && sameHigh && remainder == 0u;
        }
    }

    return keeps;
}

// --- 割り当ての段: 要求 i が配り手の欄 field に取る頁の数(CPU の AllocateOwnerPages の規則)---
// 細かくする: 頁を持つ親が欄の頁を持てば鎖の段の数(子は親の値から配る)。
// 粗くする: 親が頁を持つ枠になり(広げる親も)まだ欄の頁が無く、子が欄の頁か 0 でない一様の値を持つか、広げる親の一様の値が 0 でない時 1
uint32_t CarriedOwnerNeed(uint32_t field, uint32_t i, MrRequestState state) {
    const MrBlock target = g_blocks[state.target];
    if (g_requests[i].op == MR_REQUEST_REFINE)
        return !MrIsUniform(target) && CarriedPageOf(field, target.page) != MF_NO_PAGE ? state.levels : 0u;

    const MrBlock parent = g_blocks[target.parent];
    const bool expanded = state.pageNeed != 0;
    if (MrIsUniform(parent) && !expanded)
        return 0;

    if (!expanded && CarriedPageOf(field, parent.page) != MF_NO_PAGE)
        return 0;

    const bool childValues = MrIsUniform(target) ? CarriedUniformNonZero(field, state.target)
                                                 : CarriedPageOf(field, target.page) != MF_NO_PAGE;

    return childValues || (expanded && CarriedUniformNonZero(field, target.parent)) ? 1u : 0u;
}

// 要求 i(取った頁のある要求)の d 番目に取る欄の頁を結ぶセルの頁(細かく = 鎖の d 段目の子の頁・粗く = 親の頁か広げる新しい頁)
uint32_t CarriedOwnerCellPage(uint32_t i, MrRequestState state, uint32_t d) {
    if (g_requests[i].op == MR_REQUEST_REFINE)
        return PoppedPage(state, d);

    if (state.pageNeed != 0)
        return PoppedPage(state, 0);

    return g_blocks[g_blocks[state.target].parent].page;
}

// --- 1 つずつ取る・返す(影の鎖・引き戻し。木の段の要求の並びの外。T-1252 段 2 の 5 本目)---

void SetCarriedHeader(uint32_t field, uint32_t word, uint32_t value) {
    g_layers[CarriedStoreBase(field) + word] = value;
}

// 欄の頁が尽きた所を数える(計器。GPU だけ。CPU は上限が無い。欄 0 の頭の MF_HEADER_TREE_SHORTAGE)
void CountCarriedPagesShort(uint32_t count) {
    InterlockedAdd(g_layers[CarriedStoreBase(0) + MF_HEADER_TREE_SHORTAGE], count);
}

bool CarriedValueNonZero(int64_t value) {
    return ((uint32_t)((uint64_t)value) | (uint32_t)((uint64_t)value >> 32)) != 0u;
}

// セルの頁 cellPage の欄の頁を CPU と同じ順で決める(スレッド 0 だけが呼ぶ)。release なら前から持つ頁を空きの上へ返す
// (CPU の StartPage = ReleasePage)・wanted(0 でない値を書く)で頁が無ければ空きの上から、無ければ高さの上から 1 つ取る
// (CPU の StorePage の EnsurePage)。返すのは値を書く欄の頁(書かないなら MF_NO_PAGE)。取った頁は呼ぶ側が全部のセルを書く
uint32_t RestartCarriedPage(uint32_t field, uint32_t cellPage, bool release, bool wanted) {
    uint32_t fieldPage = CarriedPageOf(field, cellPage);
    uint32_t freeCount = CarriedHeader(field, MF_HEADER_FREE_COUNT);
    if (release && fieldPage != MF_NO_PAGE) {
        SetCarriedFreePage(field, freeCount, fieldPage);
        SetCarriedPageOf(field, cellPage, MF_NO_PAGE);
        freeCount += 1;
        fieldPage = MF_NO_PAGE;
    }

    if (fieldPage == MF_NO_PAGE && wanted) {
        const uint32_t top = CarriedHeader(field, MF_HEADER_TOP);
        if (freeCount > 0) {
            freeCount -= 1;
            fieldPage = CarriedFreePage(field, freeCount);
        } else if (top < CarriedHeader(field, MF_HEADER_CAPACITY)) {
            fieldPage = top;
            SetCarriedHeader(field, MF_HEADER_TOP, top + 1);
        } else {
            CountCarriedPagesShort(1);
        }

        if (fieldPage != MF_NO_PAGE)
            SetCarriedPageOf(field, cellPage, fieldPage);
    }

    SetCarriedHeader(field, MF_HEADER_FREE_COUNT, freeCount);

    return fieldPage;
}

// --- グループの縮約(全部のスレッドが呼ぶ。thread 0 が消す。結果はグループで一様)---

groupshared uint32_t gs_carriedFlags;
groupshared uint32_t gs_carriedPage;

uint32_t CarriedGroupOr(uint32_t thread, uint32_t mine) {
    if (thread == 0)
        gs_carriedFlags = 0;

    GroupMemoryBarrierWithGroupSync();
    if (mine != 0)
        InterlockedOr(gs_carriedFlags, mine);

    GroupMemoryBarrierWithGroupSync();
    const uint32_t flags = gs_carriedFlags;
    GroupMemoryBarrierWithGroupSync();  // 次に呼ぶ時に消す前に、全部が読み終える

    return flags;
}

// スレッド 0 が RestartCarriedPage で決めた欄の頁を全部のスレッドへ(全部のスレッドが呼ぶ)
uint32_t SharedRestartCarriedPage(uint32_t thread, uint32_t field, uint32_t cellPage, bool release, bool wanted) {
    if (thread == 0)
        gs_carriedPage = RestartCarriedPage(field, cellPage, release, wanted);

    AllMemoryBarrierWithGroupSync();
    const uint32_t fieldPage = gs_carriedPage;
    GroupMemoryBarrierWithGroupSync();

    return fieldPage;
}

#endif  // BICAMERAL_MULTIRES_FIELD_ACCESS_HLSLI
