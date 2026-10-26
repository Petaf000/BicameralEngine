// multires_fields.cpp — 登録した欄の置き場と、木の操作に欄を付いて行かせる所(T-0711。ADR-0329。multires_fields.h)。
// 細かく/粗くする時の値は shaders/common/state_field.hlsli の関数(GPU と同じ)で決める。浮動小数点は使わない。
#include <algorithm>
#include <array>
#include <utility>

#include "common/multires_tree.hlsli"
#include "core/aliases.h"
#include "sim/multires_fields.h"

using namespace bicameral::multires;
using namespace bicameral::state_field;

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

                if (!SfOnCells(field.desc))
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
                for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                    const uint32_t index = MrOctantCell(octant, local);
                    for (uint32_t c = 0; c < components; ++c)
                        StorePage(store, parent.page, index, c, 0);
                }
            }
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

                // 同じ値 8 つの和 ÷ 8 は同じ値・余り 0
                for (uint32_t c = 0; c < field.desc.components; ++c) {
                    if (field.store.uniform[UniformAddress(field, childSlot, c)] !=
                        field.store.uniform[UniformAddress(field, parentSlot, c)])
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
                if (!SfOnCells(field.desc))
                    continue;  // ブロック・世界の欄: 親は自分の値のまま

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

                // --- セル・面の欄: 子は親と同じ数(影はいつも頁を持つ。親は一様でも頁でも LoadNestField で読む)---
                StartPage(field.store, childPage);
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const uint32_t parentCell = MrParentCellOfChild(octant, index);
                    for (uint32_t c = 0; c < components; ++c)
                        StorePage(field.store, childPage, index, c,
                                  SfRefineValue(LoadNestField(nest, f, parentSlot, parentCell, c)));
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
                                      SfPullBackChild(parentValue, family, j));
                    }
                }
            }
        }

    }  // namespace nest_fields

}  // namespace bicameral::sim
