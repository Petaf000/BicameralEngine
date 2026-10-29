// multires_fields.cpp — 登録した欄の置き場と、木の操作に欄を付いて行かせる所(T-0711。ADR-0329。multires_fields.h)。
// 細かく/粗くする時の値は shaders/common/state_field.hlsli の関数(GPU と同じ)で決める。浮動小数点は使わない。
#include <algorithm>
#include <array>
#include <span>
#include <utility>

#include "common/multires_tree.hlsli"
#include "core/aliases.h"
#include "sim/multires_fields.h"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::state_field;
using bicameral::reaction::RxCell;
using bicameral::reaction::RxSpecies;

namespace bicameral::sim {

    namespace {

        // --- 値の番号 ---

        // 一様の値(セル・面の欄の一様なブロック・ブロックの欄は枠 slot、世界の欄は slot = 0)× 値の数
        size_t UniformAddress(const MultiresField& field, uint32_t slot, uint32_t component) {
            return (size_t{slot} * field.desc.components) + component;
        }

        // 欄の頁の中の番号(セル × 値の数。T-0711 からの並び)
        size_t InPage(const MultiresFieldStore& store, uint32_t index, uint32_t component) {
            return (size_t{index} * store.components) + component;
        }

        size_t PageValueCount(const MultiresFieldStore& store) {
            return size_t{MR_BLOCK_CELLS} * store.components;
        }

        // 登録した時の置き場(密: 欄の頁をセルの頁の数だけ・疎: 頁の表だけ。T-0921)
        MultiresFieldStore MakeStore(const MultiresNest& nest, const SfFieldDesc& desc) {
            MultiresFieldStore store;
            store.components = desc.components;
            if (desc.place == SF_PLACE_WORLD) {
                store.uniform.assign(desc.components, 0);
                return store;
            }

            store.uniform.assign(nest.blocks.size() * desc.components, 0);
            if (!SfOnCells(desc))
                return store;

            store.sparse = nest.sparseFields;
            store.cellPages = static_cast<uint32_t>(nest.cells.pages.size());
            if (store.sparse)
                store.pageOf.assign(store.cellPages, MULTIRES_NO_FIELD_PAGE);
            else
                store.pages.assign(size_t{store.cellPages} * PageValueCount(store), 0);

            return store;
        }

        // セルの頁 page に全部の値を書き始める前に: 疎なら前の欄の頁を返す(0 を書かない所に前の値を残さない。密は全部書くので要らない)
        void StartPage(MultiresFieldStore& store, uint32_t page);

        // --- 欄の頁(疎: 無い頁は全部 0 と読む・0 でない値を最初に書く時に作る。T-0921・20 §10.2)---

        int64_t LoadPage(const MultiresFieldStore& store, uint32_t page, uint32_t index, uint32_t component) {
            const int64_t* values = FieldPageValues(store, page);

            return values == nullptr ? 0 : values[InPage(store, index, component)];
        }

        // セルの頁 page の欄の頁を作る(疎で無ければ、返された欄の頁か新しい欄の頁を 0 で埋めて)
        int64_t* EnsurePage(MultiresFieldStore& store, uint32_t page) {
            if (!store.sparse)
                return store.pages.data() + (size_t{page} * PageValueCount(store));

            uint32_t& fieldPage = store.pageOf[page];
            if (fieldPage == MULTIRES_NO_FIELD_PAGE) {
                const size_t count = PageValueCount(store);
                if (store.freePages.empty()) {
                    fieldPage = static_cast<uint32_t>(store.pages.size() / count);
                    store.pages.resize(store.pages.size() + count, 0);
                } else {
                    fieldPage = store.freePages.back();
                    store.freePages.pop_back();
                    std::fill_n(store.pages.begin() + static_cast<ptrdiff_t>(fieldPage * count), count, 0);
                }
            }

            return store.pages.data() + (size_t{fieldPage} * PageValueCount(store));
        }

        // 書く(疎で無い頁に 0 を書くなら頁を作らない)
        void StorePage(MultiresFieldStore& store, uint32_t page, uint32_t index, uint32_t component, int64_t value) {
            if (value == 0 && FieldPageValues(store, page) == nullptr)
                return;

            EnsurePage(store, page)[InPage(store, index, component)] = value;
        }

        // セルの頁 page の欄を全部 0 に(疎なら欄の頁を返す)。セルの頁を返す・置き直す時に呼ぶので、
        // 使っていないセルの頁の欄はいつも 0(密と疎で状態の要約が同じになる約束)
        void ReleasePage(MultiresFieldStore& store, uint32_t page) {
            if (!store.sparse) {
                std::fill_n(store.pages.begin() + static_cast<ptrdiff_t>(size_t{page} * PageValueCount(store)),
                            PageValueCount(store), 0);
                return;
            }

            uint32_t& fieldPage = store.pageOf[page];
            if (fieldPage == MULTIRES_NO_FIELD_PAGE)
                return;

            store.freePages.push_back(fieldPage);
            fieldPage = MULTIRES_NO_FIELD_PAGE;
        }

        void StartPage(MultiresFieldStore& store, uint32_t page) {
            if (store.sparse)
                ReleasePage(store, page);
        }

        // 粗くした余り(子のレベル level の単位の丸ごとの数。切り捨てなのでいつも 0 以上 = セルから減った)を
        // 世界の帳簿の「粗くした端数・出」の列 column へ(T-0762。帳簿の外なら AddWorldLedgerBits が数える)
        void AddFieldRemainder(MultiresNest& nest, int32_t level, uint32_t column, uint32_t remainder) {
            if (remainder == 0)
                return;

            AddWorldLedgerUnits(nest, WlSlot(WL_REASON_COARSEN_REMAINDER, WL_DIRECTION_OUT), level, column, remainder);
        }

        // 枠 slot(頁を持つ)の欄の頁を畳む時の値(MrFoldValueCell のセル。全部覆われていれば 0)
        int64_t FoldValue(const MultiresNest& nest, const MultiresField& field, uint32_t slot, uint32_t component) {
            const MrBlock& block = nest.blocks[slot];
            const uint32_t valueCell = MrFoldValueCell(block);
            if (valueCell >= MR_BLOCK_CELLS)
                return 0;

            return LoadPage(field.store, block.page, valueCell, component);
        }

        // 導かれる値の頁を眠らせる時の値: 覆われていないセルの平均(−∞ 向きの切り捨て。全部覆われていれば 0。T-0854・20 §10.3)
        int64_t SleepMean(const MultiresNest& nest, const MultiresField& field, uint32_t slot, uint32_t component) {
            const MrBlock& block = nest.blocks[slot];
            SfMeanSum sum = SfMakeMeanSum();
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (!MrIsCoveredCell(block, index))
                    sum = SfAddMeanValue(sum, LoadPage(field.store, block.page, index, component));
            }

            return SfMeanValue(sum);
        }

        // 配り手 owner で同じ値 8 つを粗くすると、値 parentValue・余り 0 になるか(一様な親のまま粗くできるか。T-1252)
        bool OwnerCoarsensTo(uint32_t owner, int64_t childValue, int64_t parentValue) {
            SfFamily family{};
            std::ranges::fill(family.values, childValue);
            const SfCoarsenSum sum = SfOwnerCoarsen(owner, family);

            return SfCoarsenValue(sum) == parentValue && SfCoarsenRemainder(sum) == 0;
        }

        // 子ブロックのセル index が親のセルの何番目の子か(MrChildCell の j。T-1252)
        uint32_t ChildOfCell(uint32_t index) {
            return (MrCellX(index) & 1u) | ((MrCellY(index) & 1u) << 1) | ((MrCellZ(index) & 1u) << 2);
        }

        // 親の欄 parent の内訳の欄(T-1177。無ければ MULTIRES_NO_FIELD。親 1 つに内訳は 1 つ)
        uint32_t BreakdownOf(const MultiresNest& nest, uint32_t parent) {
            for (uint32_t f = 0; f < nest.fields.size(); ++f) {
                if (SfBreakdown(nest.fields[f].desc) && nest.fields[f].desc.parent == parent)
                    return f;
            }

            return MULTIRES_NO_FIELD;
        }

    }  // namespace

    uint32_t RegisterNestField(MultiresNest& nest, std::string name, const SfFieldDesc& desc) {
        if (name.empty() || !SfDescValid(desc) || FindNestField(nest, name) != MULTIRES_NO_FIELD)
            return MULTIRES_NO_FIELD;

        // 内訳(T-1177): 親は先に登録した保存量の欄(置き場・値の数が同じ)・親 1 つに内訳は 1 つ
        if (SfBreakdown(desc) &&
            (desc.parent >= nest.fields.size() || !SfBreakdownPairValid(desc, nest.fields[desc.parent].desc) ||
             BreakdownOf(nest, desc.parent) != MULTIRES_NO_FIELD))
            return MULTIRES_NO_FIELD;

        // 根を置く前だけ(置いた後に足すと、使っている頁の欄の値が決まらない)
        FX_ASSERT(nest.counters[MR_COUNTER_FREE_BLOCKS] == nest.capacity.worldBlocks);

        MultiresField field;
        field.name = std::move(name);
        field.desc = desc;
        field.store = MakeStore(nest, desc);

        // --- 保存量の欄: 世界の帳簿の後ろに値の数だけ列を足す(帳簿はまだ空なので並べ直しは要らない。T-0762)---
        if (SfConserved(desc)) {
            FX_ASSERT(std::ranges::all_of(nest.ledger, [](uint64_t value) { return value == 0; }));
            nest.capacity.ledgerColumns += desc.components;
            nest.ledger.assign(WlLedgerWords(nest.capacity.ledgerColumns), 0);
        }

        nest.fields.push_back(std::move(field));

        return static_cast<uint32_t>(nest.fields.size() - 1);
    }

    uint32_t FindNestField(const MultiresNest& nest, std::string_view name) {
        for (uint32_t i = 0; i < nest.fields.size(); ++i) {
            if (nest.fields[i].name == name)
                return i;
        }

        return MULTIRES_NO_FIELD;
    }

    // --- セル・面の欄 ---

    int64_t LoadNestField(const MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t index, uint32_t component) {
        const MultiresField& entry = nest.fields[field];
        FX_ASSERT(SfOnCells(entry.desc) && component < entry.desc.components);
        const MrBlock& block = nest.blocks[slot];
        if (MrIsUniform(block))
            return MrIsCoveredCell(block, index) ? 0 : entry.store.uniform[UniformAddress(entry, slot, component)];

        return LoadPage(entry.store, block.page, index, component);
    }

    void StoreNestField(MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t index, uint32_t component,
                        int64_t value) {
        MultiresField& entry = nest.fields[field];
        FX_ASSERT(SfOnCells(entry.desc) && component < entry.desc.components);
        FX_ASSERT(SfValueFits(entry.desc, value));
        FX_ASSERT(!MrIsUniform(nest.blocks[slot]));
        StorePage(entry.store, nest.blocks[slot].page, index, component, value);
    }

    void StoreNestFieldUniform(MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t component, int64_t value) {
        MultiresField& entry = nest.fields[field];
        FX_ASSERT(SfOnCells(entry.desc) && component < entry.desc.components);
        FX_ASSERT(SfValueFits(entry.desc, value));
        FX_ASSERT(MrIsUniform(nest.blocks[slot]));
        entry.store.uniform[UniformAddress(entry, slot, component)] = value;
    }

    // --- ブロック・世界の欄 ---

    int64_t LoadNestBlockField(const MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t component) {
        const MultiresField& entry = nest.fields[field];
        FX_ASSERT(entry.desc.place == SF_PLACE_BLOCK && component < entry.desc.components);

        return entry.store.uniform[UniformAddress(entry, slot, component)];
    }

    void StoreNestBlockField(MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t component, int64_t value) {
        MultiresField& entry = nest.fields[field];
        FX_ASSERT(entry.desc.place == SF_PLACE_BLOCK && component < entry.desc.components);
        FX_ASSERT(SfValueFits(entry.desc, value));
        entry.store.uniform[UniformAddress(entry, slot, component)] = value;
    }

    int64_t LoadNestWorldField(const MultiresNest& nest, uint32_t field, uint32_t component) {
        const MultiresField& entry = nest.fields[field];
        FX_ASSERT(entry.desc.place == SF_PLACE_WORLD && component < entry.desc.components);

        return entry.store.uniform[component];
    }

    void StoreNestWorldField(MultiresNest& nest, uint32_t field, uint32_t component, int64_t value) {
        MultiresField& entry = nest.fields[field];
        FX_ASSERT(entry.desc.place == SF_PLACE_WORLD && component < entry.desc.components);
        FX_ASSERT(SfValueFits(entry.desc, value));
        entry.store.uniform[component] = value;
    }

    uint32_t NestConservedIndex(const MultiresNest& nest, uint32_t field) {
        if (!SfConserved(nest.fields[field].desc))
            return MULTIRES_NO_FIELD;

        uint32_t index = 0;
        for (uint32_t f = 0; f < field; ++f) {
            if (SfConserved(nest.fields[f].desc))
                index += nest.fields[f].desc.components;
        }

        return index;
    }

    void ClearFieldStore(MultiresFieldStore& store) {
        std::ranges::fill(store.uniform, 0);
        if (!store.sparse) {
            std::ranges::fill(store.pages, 0);
            return;
        }

        store.pages.clear();
        store.freePages.clear();
        std::ranges::fill(store.pageOf, MULTIRES_NO_FIELD_PAGE);
    }

    NestFieldMemory MeasureNestFields(const MultiresNest& nest) {
        uint64_t usedBlocks = 0;
        uint64_t pagedBlocks = 0;
        for (const MrBlock& block : nest.blocks) {
            usedBlocks += block.kind != MR_BLOCK_UNUSED ? 1 : 0;
            pagedBlocks += block.kind != MR_BLOCK_UNUSED && !MrIsUniform(block) ? 1 : 0;
        }

        NestFieldMemory memory;
        for (const MultiresField& field : nest.fields) {
            const MultiresFieldStore& store = field.store;
            const uint64_t valueBytes = uint64_t{store.components} * sizeof(int64_t);
            memory.uniformBytes += (field.desc.place == SF_PLACE_WORLD ? 1 : usedBlocks) * valueBytes;
            if (!SfOnCells(field.desc))
                continue;

            // --- 密: 頁を持つブロックの数だけ(セルの頁と同じだけ要る)/ 疎: 値のある頁 + 頁の表 ---
            const uint64_t pageBytes = PageValueCount(store) * sizeof(int64_t);
            if (!store.sparse) {
                memory.pageBytes += pagedBlocks * pageBytes;
                continue;
            }

            memory.tableBytes += pagedBlocks * sizeof(uint32_t);
            for (const uint32_t fieldPage : store.pageOf)
                memory.pageBytes += fieldPage == MULTIRES_NO_FIELD_PAGE ? 0 : pageBytes;
        }

        return memory;
    }

    uint64_t CountBreakdownMisfits(const MultiresNest& nest) {
        uint64_t misfits = 0;
        for (uint32_t f = 0; f < nest.fields.size(); ++f) {
            const SfFieldDesc& desc = nest.fields[f].desc;
            if (!SfBreakdown(desc))
                continue;

            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                const MrBlock& block = nest.blocks[slot];
                if (block.kind == MR_BLOCK_UNUSED)
                    continue;

                const uint32_t cells = MrIsUniform(block) ? 1u : MR_BLOCK_CELLS;
                for (uint32_t index = 0; index < cells; ++index) {
                    for (uint32_t c = 0; c < desc.components; ++c) {
                        const int64_t parent = LoadNestField(nest, desc.parent, slot, index, c);
                        misfits += SfBreakdownFits(parent, LoadNestField(nest, f, slot, index, c)) ? 0u : 1u;
                    }
                }
            }
        }

        return misfits;
    }

    // --- 集めて書く段(T-1252)---

    NestGatherWork MakeNestGatherWork(const MultiresNest& nest, uint32_t field) {
        FX_ASSERT(field < nest.fields.size() && SfOnCells(nest.fields[field].desc));
        NestGatherWork work;
        work.field = field;
        work.components = nest.fields[field].desc.components;
        work.blockOf.assign(nest.capacity.worldBlocks, MULTIRES_NO_FIELD_PAGE);

        return work;
    }

    void AddNestGatherWork(NestGatherWork& work, uint32_t slot, uint32_t index, uint32_t component, int64_t value) {
        FX_ASSERT(slot < work.blockOf.size() && index < MR_BLOCK_CELLS && component < work.components);
        uint32_t& block = work.blockOf[slot];
        if (block == MULTIRES_NO_FIELD_PAGE) {
            block = static_cast<uint32_t>(work.slots.size());
            work.slots.push_back(slot);
            work.values.resize(work.values.size() + (size_t{MR_BLOCK_CELLS} * work.components), 0);
        }

        work.values[(((size_t{block} * MR_BLOCK_CELLS) + index) * work.components) + component] += value;
    }

    int64_t NestGatherWorkTotal(const NestGatherWork& work, uint32_t component) {
        int64_t total = 0;
        for (size_t i = component; i < work.values.size(); i += work.components)
            total += work.values[i];

        return total;
    }

    namespace {

        // 作業のブロック block の値の先頭(MR_BLOCK_CELLS × 値の数)
        int64_t* WorkValues(NestGatherWork& work, uint32_t block) {
            return work.values.data() + (size_t{block} * MR_BLOCK_CELLS * work.components);
        }

        bool WorkBlockEmpty(NestGatherWork& work, uint32_t block) {
            const int64_t* values = WorkValues(work, block);

            return std::all_of(values, values + (size_t{MR_BLOCK_CELLS} * work.components),
                               [](int64_t value) { return value == 0; });
        }

        // 疎な欄の使っている頁の数
        uint32_t UsedPages(const MultiresFieldStore& store) {
            return static_cast<uint32_t>((store.pages.size() / PageValueCount(store)) - store.freePages.size());
        }

        // 欄の頁をもう 1 つ取れるか(予算 0 = 決めない)
        bool PageAvailable(const MultiresFieldStore& store) {
            return !store.sparse || store.pageBudget == 0 || UsedPages(store) < store.pageBudget;
        }

        // (b) 触った一様な枠を頁に広げる(ExpandWantedPages と同じ枠の順。値はビット単位で変わらない)
        void ExpandGatheredBlocks(MultiresNest& nest, NestGatherWork& work, std::span<const uint32_t> order,
                                  NestGatherReport& report) {
            bool wanted = false;
            for (const uint32_t block : order) {
                MrBlock& header = nest.blocks[work.slots[block]];
                if (MrIsUniform(header) && !WorkBlockEmpty(work, block)) {
                    header.page = MR_PAGE_WANTED;
                    wanted = true;
                }
            }

            if (wanted)
                report.expanded = static_cast<uint32_t>(nest_detail::ExpandWantedPages(nest).size());
        }

        // (c) 作業のブロック block の値を欄の頁へ足し、作業を 0 に
        void WriteGathered(MultiresNest& nest, NestGatherWork& work, uint32_t block) {
            MultiresField& field = nest.fields[work.field];
            const uint32_t slot = work.slots[block];
            const MrBlock& header = nest.blocks[slot];
            int64_t* values = WorkValues(work, block);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                for (uint32_t c = 0; c < work.components; ++c) {
                    int64_t& value = values[(size_t{index} * work.components) + c];
                    if (value == 0)
                        continue;

                    FX_ASSERT(MrIsSteppedCell(header, index));
                    const int64_t sum = LoadPage(field.store, header.page, index, c) + value;
                    FX_ASSERT(SfValueFits(field.desc, sum));
                    StorePage(field.store, header.page, index, c, sum);
                    value = 0;
                }
            }

            nest_detail::PokeBlock(nest, slot);
        }

    }  // namespace

    NestGatherReport GatherNestField(MultiresNest& nest, NestGatherWork& work) {
        NestGatherReport report;
        std::vector<uint32_t> order(work.slots.size());
        for (uint32_t block = 0; block < order.size(); ++block)
            order[block] = block;

        rng::sort(order, [&](uint32_t a, uint32_t b) { return work.slots[a] < work.slots[b]; });
        ExpandGatheredBlocks(nest, work, order, report);

        // --- (a) 欄の頁を枠の順に取る(k 番目に要る枠が k 番目の頁 = プレフィックス和の順)・(c) 書く ---
        MultiresFieldStore& store = nest.fields[work.field].store;
        uint32_t taken = 0;
        for (const uint32_t block : order) {
            const MrBlock& header = nest.blocks[work.slots[block]];
            FX_ASSERT(header.kind == MR_BLOCK_REAL);
            if (WorkBlockEmpty(work, block))
                continue;

            if (MrIsUniform(header)) {
                report.cellPageShortage += 1;
                continue;
            }

            const bool needsPage = FieldPageValues(store, header.page) == nullptr;
            if (needsPage && !PageAvailable(store)) {
                report.pageShortage += 1;
                continue;
            }

            taken += needsPage ? 1u : 0u;
            WriteGathered(nest, work, block);
            report.blocks += 1;
        }

        report.pagesTaken = taken;

        return report;
    }

    void SetNestFieldPageBudget(MultiresNest& nest, uint32_t field, uint32_t pages) {
        FX_ASSERT(field < nest.fields.size() && SfOnCells(nest.fields[field].desc));
        nest.fields[field].store.pageBudget = pages;
    }

    uint32_t UsedNestFieldPages(const MultiresNest& nest, uint32_t field) {
        const MultiresFieldStore& store = nest.fields[field].store;
        return store.sparse ? UsedPages(store) : store.cellPages;
    }

    // --- この刻みだけの欄(T-1252)---

    void ConsumeTickOnlyField(
        MultiresNest& nest, uint32_t field,
        const std::function<void(uint32_t slot, uint32_t index, uint32_t component, int64_t value)>& apply) {
        MultiresField& entry = nest.fields[field];
        FX_ASSERT(SfTickOnly(entry.desc));
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                for (uint32_t c = 0; c < entry.desc.components; ++c) {
                    const int64_t value = LoadNestField(nest, field, slot, index, c);
                    if (value != 0)
                        apply(slot, index, c, value);
                }
            }
        }

        ClearFieldStore(entry.store);
    }

    bool TickOnlyFieldsClear(const MultiresNest& nest) {
        for (const MultiresField& field : nest.fields) {
            if (!SfTickOnly(field.desc))
                continue;

            const MultiresFieldStore& store = field.store;
            if (!rng::all_of(store.uniform, [](int64_t value) { return value == 0; }))
                return false;

            // 使っている欄の頁だけを見る(疎の返した頁は古い値のままでよい。取る時に 0 で埋める)
            for (uint32_t page = 0; page < store.cellPages; ++page) {
                const int64_t* values = FieldPageValues(store, page);
                if (values != nullptr &&
                    !std::all_of(values, values + PageValueCount(store), [](int64_t value) { return value == 0; }))
                    return false;
            }
        }

        return true;
    }

    // --- 木の操作が呼ぶ ---
    namespace nest_fields {

        void ReleaseBlockPages(MultiresNest& nest, uint32_t slot) {
            const MrBlock& block = nest.blocks[slot];
            if (MrIsUniform(block))
                return;

            for (MultiresField& field : nest.fields) {
                if (SfOnCells(field.desc))
                    ReleasePage(field.store, block.page);
            }
        }

        void ReleaseZeroPages(MultiresNest& nest, uint32_t slot) {
            const MrBlock& block = nest.blocks[slot];
            if (MrIsUniform(block))
                return;

            for (MultiresField& field : nest.fields) {
                if (!SfOnCells(field.desc) || !field.store.sparse)
                    continue;

                const int64_t* values = FieldPageValues(field.store, block.page);
                if (values != nullptr &&
                    std::all_of(values, values + PageValueCount(field.store), [](int64_t value) { return value == 0; }))
                    ReleasePage(field.store, block.page);
            }
        }

        void PlaceRoot(MultiresNest& nest, uint32_t slot) {
            const MrBlock& block = nest.blocks[slot];
            for (MultiresField& field : nest.fields) {
                if (field.desc.place == SF_PLACE_WORLD)
                    continue;

                // --- ブロックの欄・一様なブロックのセル・面の欄: 一様の値を 0 に ---
                if (!SfOnCells(field.desc) || MrIsUniform(block)) {
                    std::fill_n(field.store.uniform.begin() + static_cast<ptrdiff_t>(UniformAddress(field, slot, 0)),
                                field.desc.components, 0);
                    continue;
                }

                // --- 頁を持つ: 欄の頁を 0 に(疎なら返す)---
                ReleasePage(field.store, block.page);
            }
        }

        void ExpandPage(MultiresNest& nest, uint32_t slot, uint32_t page) {
            const MrBlock& block = nest.blocks[slot];
            FX_ASSERT(MrIsUniform(block));
            for (MultiresField& field : nest.fields) {
                if (!SfOnCells(field.desc))
                    continue;

                // 0 は疎の無い頁に書かない(全部 0 の一様なブロックを広げても欄の頁を作らない。T-0921)
                StartPage(field.store, page);
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const bool covered = MrIsCoveredCell(block, index);
                    for (uint32_t c = 0; c < field.desc.components; ++c) {
                        const int64_t value = covered ? 0 : field.store.uniform[UniformAddress(field, slot, c)];
                        StorePage(field.store, page, index, c, value);
                    }
                }
            }
        }

        bool PageFoldable(const MultiresNest& nest, uint32_t slot) {
            const MrBlock& block = nest.blocks[slot];
            FX_ASSERT(!MrIsUniform(block));
            for (const MultiresField& field : nest.fields) {
                // 導かれる値は畳むのを止めない(眠る時に平均して残す。T-0854・20 §10.3)
                if (!SfOnCells(field.desc) || SfDerived(field.desc))
                    continue;

                for (uint32_t c = 0; c < field.desc.components; ++c) {
                    const int64_t value = FoldValue(nest, field, slot, c);
                    for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                        const int64_t expected = MrIsCoveredCell(block, index) ? 0 : value;
                        if (LoadPage(field.store, block.page, index, c) != expected)
                            return false;
                    }

                    // 内訳(T-1177): 残り(親 − 内訳。舞う粒)があれば起きている。止まれる側(層)だけなら畳める
                    if (SfBreakdown(field.desc) &&
                        SfBreakdownKeepsAwake(FoldValue(nest, nest.fields[field.desc.parent], slot, c), value))
                        return false;
                }
            }

            return true;
        }

        void FoldPage(MultiresNest& nest, uint32_t slot) {
            for (MultiresField& field : nest.fields) {
                if (!SfOnCells(field.desc))
                    continue;

                for (uint32_t c = 0; c < field.desc.components; ++c) {
                    const int64_t value = SfDerived(field.desc)
                                              ? SfSleepValueOf(field.desc, SleepMean(nest, field, slot, c))
                                              : FoldValue(nest, field, slot, c);
                    field.store.uniform[UniformAddress(field, slot, c)] = value;
                }

                // 呼ぶ側がセルの頁を返すので、欄の頁も返す(T-0921)
                ReleasePage(field.store, nest.blocks[slot].page);
            }
        }

        // 置かれ方の重み(親の八分の一のセルごと。T-1157)。印の欄が無い・口が無い・一様な親なら作らない
        struct OctantPlacement {
            bool any = false;
            std::array<bool, MR_OCTANT_CELLS> placed{};
            std::array<NestChildWeights, MR_OCTANT_CELLS> weights{};
        };

        OctantPlacement MakeOctantPlacement(const MultiresNest& nest, uint32_t parentSlot, uint32_t octant) {
            OctantPlacement placement;
            const bool marked = rng::any_of(nest.fields, [](const MultiresField& field) {
                return SfOnCells(field.desc) && SfRefinePlacement(field.desc) != SF_REFINE_SAME;
            });
            if (!marked || !nest.refinePlacement || MrIsUniform(nest.blocks[parentSlot]))
                return placement;

            placement.any = true;
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local)
                placement.placed[local] = nest.refinePlacement(nest, parentSlot, MrOctantCell(octant, local),
                                                               placement.weights[local]);

            return placement;
        }

        // 親のセルの値 1 つを子 8 つへ重みで配る(子の番号は MrChildCell の j)。重みの和が 0・口が重みを返さないセル・
        // SAME の規則なら子は親と同じ数。残りは重みのある最後の子(重みの無い子に端数を入れない)
        std::array<int64_t, 8> PlaceChildValues(int64_t value, uint32_t rule, const NestChildWeights& weights,
                                                bool placed) {
            std::array<int64_t, 8> children{};
            const std::array<uint64_t, 8>& weight = rule == SF_REFINE_CONDENSED ? weights.condensed : weights.open;
            uint64_t weightSum = 0;
            uint32_t last = 7;
            for (uint32_t j = 0; j < 8; ++j) {
                weightSum += weight[j];
                last = weight[j] > 0 ? j : last;
            }

            if (!placed || rule == SF_REFINE_SAME || weightSum == 0) {
                children.fill(SfRefineValue(value));
                return children;
            }

            int64_t given = 0;
            for (uint32_t j = 0; j < 8; ++j) {
                if (j == last)
                    continue;

                children[j] = SfPlacedValue(value, weight[j], weightSum, 8);
                given += children[j];
            }

            children[last] = SfPlacedLastValue(value, given, 8);

            return children;
        }

        // 内訳を持つ親の欄の配り(T-1177): 子の親 = 内訳を内訳の印で配った値 + 残り(親 − 内訳)を親の印で配った値
        struct BreakdownSource {
            const MultiresFieldStore* store = nullptr;  // 内訳の欄(無ければ nullptr)
            uint32_t rule = SF_REFINE_SAME;             // 内訳の配り方
        };

        // 置かれ方の印の欄(と内訳を持つ親の欄)を、親の八分の一のセルごとに重みで子へ配る
        void RefinePlaced(MultiresFieldStore& store, uint32_t components, uint32_t rule, uint32_t parentPage,
                          uint32_t octant, uint32_t childPage, const OctantPlacement& placement,
                          const BreakdownSource& breakdown) {
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                const uint32_t parentCell = MrOctantCell(octant, local);
                const NestChildWeights& weights = placement.weights[local];
                const bool placed = placement.placed[local];
                for (uint32_t c = 0; c < components; ++c) {
                    const int64_t value = LoadPage(store, parentPage, parentCell, c);
                    std::array<int64_t, 8> children{};
                    if (breakdown.store == nullptr) {
                        children = PlaceChildValues(value, rule, weights, placed);
                    } else {
                        const int64_t part = LoadPage(*breakdown.store, parentPage, parentCell, c);
                        FX_ASSERT(SfBreakdownFits(value, part));
                        const std::array<int64_t, 8> parts = PlaceChildValues(part, breakdown.rule, weights, placed);
                        const std::array<int64_t, 8> rest = PlaceChildValues(value - part, rule, weights, placed);
                        for (uint32_t j = 0; j < 8; ++j)
                            children[j] = parts[j] + rest[j];
                    }

                    for (uint32_t j = 0; j < 8; ++j)
                        StorePage(store, childPage, MrChildCell(local, j), c, children[j]);
                }
            }
        }

        // 頁を持つ親の八分の一のセルを 0 に(子に覆われた。T-0711)
        void CoverParentOctant(MultiresFieldStore& store, uint32_t components, uint32_t parentPage, uint32_t octant) {
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                const uint32_t index = MrOctantCell(octant, local);
                for (uint32_t c = 0; c < components; ++c)
                    StorePage(store, parentPage, index, c, 0);
            }
        }

        // 配り手の欄を登録した世界か(T-1252。無ければセルの配りの口は何もしない = 今とビット単位で同じ)
        bool AnyOwned(const MultiresNest& nest) {
            return rng::any_of(nest.fields, [](const MultiresField& field) { return SfOwned(field.desc); });
        }

        // 相の層の配り(細かくする): 親の八分の一のセルごとに、セルの中身を配った後の子 8 つを読んで子の Ec と(混ざった親なら)子のエネルギーを書く。
        // セルの中身の配り(置かれ方・SAME)の後・親を覆う前に呼ばれる(Refine の終わり)。子の層は成分が同じなので付け替えで残る
        void RefinePhaseLayerOctant(MultiresNest& nest, MultiresField& field, const MrBlock& parent, uint32_t octant,
                                    uint32_t childPage) {
            const std::span<const RxSpecies> species = nest.phaseLayer.species;
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                const uint32_t parentCell = MrOctantCell(octant, local);
                const RxCell parentCellValue = nest_detail::LoadPageCell(nest, parent.page, parentCell);
                std::array<RxCell, MR_CHILDREN_PER_CELL> children{};
                std::array<int64_t, MR_CHILDREN_PER_CELL> energies{};
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                    children[j] = nest_detail::LoadPageCell(nest, childPage, MrChildCell(local, j));

                nest_detail::RefinePhaseLayerCell(
                    species, parentCellValue, LoadPage(field.store, parent.page, parentCell, 0), children, energies);
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                    const uint32_t index = MrChildCell(local, j);
                    nest_detail::StorePageCell(nest, childPage, index, children[j]);
                    StorePage(field.store, childPage, index, 0, energies[j]);
                }
            }
        }

        // 相の層の配り(粗くする): 粗くした親のセル(頁を持つ親)と子 8 つから親の Ec。保存量でないので余りは無い
        void CoarsenPhaseLayerOctant(MultiresNest& nest, uint32_t f, uint32_t childSlot) {
            const MrBlock& child = nest.blocks[childSlot];
            const MrBlock& parent = nest.blocks[child.parent];
            const std::span<const RxSpecies> species = nest.phaseLayer.species;
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                const uint32_t parentCell = MrOctantCell(child.parentOctant, local);
                std::array<RxCell, MR_CHILDREN_PER_CELL> children{};
                std::array<int64_t, MR_CHILDREN_PER_CELL> energies{};
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                    const uint32_t index = MrChildCell(local, j);
                    children[j] = LoadNestCell(nest, childSlot, index);
                    energies[j] = LoadNestField(nest, f, childSlot, index, 0);
                }

                const RxCell parentCellValue = nest_detail::LoadPageCell(nest, parent.page, parentCell);
                StorePage(nest.fields[f].store, parent.page, parentCell, 0,
                          nest_detail::CoarsenPhaseLayerCell(species, parentCellValue, children, energies));
            }
        }

        void RefineOwners(MultiresNest& nest, uint32_t parentSlot, uint32_t octant, uint32_t childSlot,
                          uint32_t childPage) {
            if (!AnyOwned(nest))
                return;

            const MrBlock& parent = nest.blocks[parentSlot];
            for (MultiresField& field : nest.fields) {
                if (!SfOwned(field.desc))
                    continue;

                const uint32_t owner = field.desc.owner;
                const uint32_t components = field.desc.components;
                MultiresFieldStore& store = field.store;

                // --- 一様な親: 子も一様(配り手の一様の配り。配り手の約束: 一様な親の子は一様)---
                if (MrIsUniform(parent)) {
                    for (uint32_t c = 0; c < components; ++c)
                        store.uniform[UniformAddress(field, childSlot, c)] = SfOwnerRefineUniform(
                            owner, store.uniform[UniformAddress(field, parentSlot, c)]);
                    continue;
                }

                // --- 頁を持つ親: 親のセルごとに子 2³ へ配り手の関数で(相の層の配りはセルを読む。ADR-0512 の 1)---
                StartPage(store, childPage);
                if (owner == SF_OWNER_PHASE_LAYER && !nest.wideCells) {
                    RefinePhaseLayerOctant(nest, field, parent, octant, childPage);
                    CoverParentOctant(store, components, parent.page, octant);
                    continue;
                }

                for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                    const uint32_t parentCell = MrOctantCell(octant, local);
                    for (uint32_t c = 0; c < components; ++c) {
                        const int64_t value = LoadPage(store, parent.page, parentCell, c);
                        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                            StorePage(store, childPage, MrChildCell(local, j), c, SfOwnerRefineChild(owner, value, j));
                    }
                }

                CoverParentOctant(store, components, parent.page, octant);
            }
        }

        // 子 childSlot の配り手の欄を親の八分の一へ(配り手の粗くする関数。保存量の余りは一般の段と同じ帳簿の列へ。T-1252)
        void CoarsenOwners(MultiresNest& nest, uint32_t childSlot) {
            const MrBlock& child = nest.blocks[childSlot];
            const MrBlock& parent = nest.blocks[child.parent];
            for (uint32_t f = 0; f < nest.fields.size(); ++f) {
                MultiresField& field = nest.fields[f];
                if (!SfOwned(field.desc))
                    continue;

                // 相の層の配り: 頁を持つ親はセルを読む(一様な親は一様のまま = 一般の形。ADR-0512 の 1)
                if (field.desc.owner == SF_OWNER_PHASE_LAYER && !nest.wideCells && !MrIsUniform(parent)) {
                    CoarsenPhaseLayerOctant(nest, f, childSlot);
                    continue;
                }

                const bool conserved = SfConserved(field.desc);
                const uint32_t column = conserved ? NestFieldLedgerBase(nest) + NestConservedIndex(nest, f) : 0;
                for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                    const uint32_t parentCell = MrOctantCell(child.parentOctant, local);
                    for (uint32_t c = 0; c < field.desc.components; ++c) {
                        SfFamily family{};
                        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                            family.values[j] = LoadNestField(nest, f, childSlot, MrChildCell(local, j), c);

                        const SfCoarsenSum sum = SfOwnerCoarsen(field.desc.owner, family);
                        if (conserved)
                            AddFieldRemainder(nest, child.level, column + c, SfCoarsenRemainder(sum));

                        // 一様な親は一様のまま(CoarsenKeepsUniform が見た。配り手の約束: 同じ値 8 つは同じ値・余り 0)
                        if (MrIsUniform(parent)) {
                            FX_ASSERT(SfCoarsenValue(sum) ==
                                      field.store.uniform[UniformAddress(field, child.parent, c)]);
                            continue;
                        }

                        StorePage(field.store, parent.page, parentCell, c, SfCoarsenValue(sum));
                    }
                }
            }
        }

        void Refine(MultiresNest& nest, uint32_t parentSlot, uint32_t octant, uint32_t childSlot, uint32_t childPage) {
            const MrBlock& parent = nest.blocks[parentSlot];
            const bool uniform = MrIsUniform(parent);
            FX_ASSERT(uniform == (childPage == MR_NO_PAGE));
            const OctantPlacement placement = MakeOctantPlacement(nest, parentSlot, octant);
            for (MultiresField& field : nest.fields) {
                const uint32_t components = field.desc.components;

                // --- ブロックの欄: 子のブロックは親の値 ---
                if (field.desc.place == SF_PLACE_BLOCK) {
                    for (uint32_t c = 0; c < components; ++c)
                        field.store.uniform[UniformAddress(field, childSlot, c)] = field.store.uniform[UniformAddress(
                            field, parentSlot, c)];
                    continue;
                }

                // 配り手の欄は一般の段で写さない(RefineOwners が配る。T-1252)
                if (!SfOnCells(field.desc) || SfOwned(field.desc))
                    continue;

                // --- セル・面の欄: 子は親と同じ数(一様なら値 1 つ)---
                MultiresFieldStore& store = field.store;
                if (uniform) {
                    for (uint32_t c = 0; c < components; ++c)
                        store.uniform[UniformAddress(field, childSlot, c)] = SfRefineValue(
                            store.uniform[UniformAddress(field, parentSlot, c)]);
                    continue;
                }

                StartPage(store, childPage);
                const uint32_t rule = SfRefinePlacement(field.desc);
                const uint32_t breakdownField = BreakdownOf(nest, (uint32_t)(&field - nest.fields.data()));
                BreakdownSource breakdown;
                if (breakdownField != MULTIRES_NO_FIELD)
                    breakdown = {.store = &nest.fields[breakdownField].store,
                                 .rule = SfRefinePlacement(nest.fields[breakdownField].desc)};

                if (placement.any && (rule != SF_REFINE_SAME || breakdown.rule != SF_REFINE_SAME)) {
                    RefinePlaced(store, components, rule, parent.page, octant, childPage, placement, breakdown);
                } else {
                    for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                        const uint32_t parentCell = MrParentCellOfChild(octant, index);
                        for (uint32_t c = 0; c < components; ++c)
                            StorePage(store, childPage, index, c,
                                      SfRefineValue(LoadPage(store, parent.page, parentCell, c)));
                    }
                }

                // --- 親を覆う(覆われた親のセルは 0。一様な親は覆われた所を 0 と読むので書かない)---
                CoverParentOctant(store, components, parent.page, octant);
            }

            // --- セルの配りの口: 配り手の欄(セルの中身の配りの後。T-1252)---
            RefineOwners(nest, parentSlot, octant, childSlot, childPage);
        }

        bool CoarsenKeepsUniform(const MultiresNest& nest, uint32_t childSlot, uint32_t parentSlot) {
            FX_ASSERT(MrIsUniform(nest.blocks[parentSlot]));
            if (nest.fields.empty())
                return true;

            // 頁を持つ子は控えめに「広げる」(欄の頁を 64 組ずつ調べる手間を取らない。欄のある世界だけの扱い)
            const bool childUniform = MrIsUniform(nest.blocks[childSlot]);
            for (const MultiresField& field : nest.fields) {
                // 導かれる値は木の形・頁の使い方を変えない(一様な親は自分の値のまま。T-0854)
                if (!SfOnCells(field.desc) || SfDerived(field.desc))
                    continue;

                if (!childUniform)
                    return false;

                // 同じ値 8 つの和 ÷ 8 は同じ値・余り 0。配り手の欄は配り手の粗くする関数で(T-1252)
                for (uint32_t c = 0; c < field.desc.components; ++c) {
                    const int64_t childValue = field.store.uniform[UniformAddress(field, childSlot, c)];
                    const int64_t parentValue = field.store.uniform[UniformAddress(field, parentSlot, c)];
                    if (!SfOwned(field.desc) && childValue != parentValue)
                        return false;

                    if (SfOwned(field.desc) && !OwnerCoarsensTo(field.desc.owner, childValue, parentValue))
                        return false;
                }
            }

            return true;
        }

        void Coarsen(MultiresNest& nest, uint32_t childSlot) {
            const MrBlock& child = nest.blocks[childSlot];
            const MrBlock& parent = nest.blocks[child.parent];
            const bool parentUniform = MrIsUniform(parent);
            for (uint32_t f = 0; f < nest.fields.size(); ++f) {
                MultiresField& field = nest.fields[f];
                // ブロック・世界の欄: 親は自分の値のまま。配り手の欄は CoarsenOwners が(T-1252)
                if (!SfOnCells(field.desc) || SfOwned(field.desc))
                    continue;

                // 一様な親の導かれる値は親の値のまま(眠った所の粗い値。CoarsenKeepsUniform も見ない。T-0854)
                if (parentUniform && SfDerived(field.desc))
                    continue;

                // 保存量(= VOLUME)の欄の余りを載せる帳簿の列(値 0 の列)
                const bool conserved = SfConserved(field.desc);
                const uint32_t column = conserved ? NestFieldLedgerBase(nest) + NestConservedIndex(nest, f) : 0;
                for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                    const uint32_t parentCell = MrOctantCell(child.parentOctant, local);
                    for (uint32_t c = 0; c < field.desc.components; ++c) {
                        SfCoarsenSum sum = SfMakeCoarsenSum();
                        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                            sum = SfAddCoarsenChild(sum, LoadNestField(nest, f, childSlot, MrChildCell(local, j), c));

                        const int64_t value = SfCoarsenValue(sum);
                        if (conserved)
                            AddFieldRemainder(nest, child.level, column + c, SfCoarsenRemainder(sum));

                        if (parentUniform) {
                            FX_ASSERT(value == field.store.uniform[UniformAddress(field, child.parent, c)]);
                            continue;
                        }

                        StorePage(field.store, parent.page, parentCell, c, value);
                    }
                }
            }

            // --- 粗くする時のセルの配りの口: 配り手の欄(セルの中身と一般の欄の後。T-1252)---
            CoarsenOwners(nest, childSlot);

            // --- 呼ぶ側が子のセルの頁を返すので、欄の頁も返す(T-0921)---
            if (MrIsUniform(child))
                return;

            for (MultiresField& field : nest.fields) {
                if (SfOnCells(field.desc))
                    ReleasePage(field.store, child.page);
            }
        }

        void RefineShadow(MultiresNest& nest, uint32_t parentSlot, uint32_t octant, uint32_t childSlot,
                          uint32_t childPage) {
            const bool uniformParent = MrIsUniform(nest.blocks[parentSlot]);
            for (uint32_t f = 0; f < nest.fields.size(); ++f) {
                MultiresField& field = nest.fields[f];
                const uint32_t components = field.desc.components;

                // --- ブロックの欄: 影のブロックは親の値 ---
                if (field.desc.place == SF_PLACE_BLOCK) {
                    for (uint32_t c = 0; c < components; ++c)
                        field.store.uniform[UniformAddress(field, childSlot, c)] = field.store.uniform[UniformAddress(
                            field, parentSlot, c)];
                    continue;
                }

                if (!SfOnCells(field.desc))
                    continue;

                // --- セル・面の欄: 子は親と同じ数(影はいつも頁を持つ。親は一様でも頁でも LoadNestField で読む)。
                //     配り手の欄は配り手の細かくする関数で(ADR-0500 の 1。T-1252)---
                StartPage(field.store, childPage);
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const uint32_t parentCell = MrParentCellOfChild(octant, index);
                    const uint32_t j = ChildOfCell(index);
                    for (uint32_t c = 0; c < components; ++c) {
                        const int64_t value = LoadNestField(nest, f, parentSlot, parentCell, c);
                        StorePage(field.store, childPage, index, c,
                                  !SfOwned(field.desc) ? SfRefineValue(value)
                                  : uniformParent      ? SfOwnerRefineUniform(field.desc.owner, value)
                                                       : SfOwnerRefineChild(field.desc.owner, value, j));
                    }
                }
            }
        }

        void PullBackShadow(MultiresNest& nest, uint32_t shadowSlot) {
            const MrBlock& shadow = nest.blocks[shadowSlot];
            FX_ASSERT(shadow.kind == MR_BLOCK_SHADOW && !MrIsUniform(shadow));
            for (uint32_t f = 0; f < nest.fields.size(); ++f) {
                MultiresField& field = nest.fields[f];
                const uint32_t components = field.desc.components;

                // --- ブロックの欄: 親の値に戻す ---
                if (field.desc.place == SF_PLACE_BLOCK) {
                    for (uint32_t c = 0; c < components; ++c)
                        field.store.uniform[UniformAddress(field, shadowSlot, c)] = field.store.uniform[UniformAddress(
                            field, shadow.parent, c)];
                    continue;
                }

                if (!SfOnCells(field.desc))
                    continue;

                // --- セル・面の欄: 親のセルごとに子 2³ の和を親 × 8 に ---
                for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                    const uint32_t parentCell = MrOctantCell(shadow.parentOctant, local);
                    for (uint32_t c = 0; c < components; ++c) {
                        const int64_t parentValue = LoadNestField(nest, f, shadow.parent, parentCell, c);
                        SfFamily family{};
                        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                            family.values[j] = LoadPage(field.store, shadow.page, MrChildCell(local, j), c);

                        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                            StorePage(field.store, shadow.page, MrChildCell(local, j), c,
                                      SfOwned(field.desc)
                                          ? SfOwnerPullBackChild(field.desc.owner, parentValue, family, j)
                                          : SfPullBackChild(parentValue, family, j));
                    }
                }
            }
        }

    }  // namespace nest_fields

}  // namespace bicameral::sim
