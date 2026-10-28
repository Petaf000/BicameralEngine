// multires_wide_cell.cpp — 多重解像度の世界のセルの溢れ(multires_nest.h の「セルの溢れ」。T-0187・02 §3・17)。
// セルと端数は「インライン RX_MAX_CELL_SPECIES 個(nest.cells〔頁は成分の数に合わせた幅。T-0931〕・nest.fractions)+ 頁・端数の枠ごとの溢れ」に分けて持つ。
// 読む時は合わせて上限の無い形(RxWideCell・MrWideFraction)にし、書く時は先頭をインラインへ・残りを溢れへ。
// 溢れはセルの番号の順に詰める(1 セルを書き直すと後ろのセルの位置がずれる)。並びは書いた順に依らず、内容だけで決まる。
// 成分ごとの層(T-1220。ADR-0489): セルの溢れの領域は成分と同じ並びの層(layers。層のある領域だけ)を持つ。層のある頁に書く時は、
// 書く前のセル(インライン + 溢れ)と物質で結んで付け替える(インライン ↔ 溢れの間で物質が動いても層は物質についていく)。
#include <algorithm>

#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        // 溢れの領域のセル index の範囲を、species・amounts(と層 layers。空 = 全部 0)で置き換える
        template <typename Amount>
        void ReplaceOverflow(MultiresOverflowArea& area, uint32_t index, std::span<const uint32_t> species,
                             std::span<const Amount> amounts, std::span<const uint64_t> layers) {
            FX_ASSERT(species.size() == amounts.size());
            FX_ASSERT(layers.empty() || layers.size() == species.size());
            const uint32_t begin = area.offsets[index];
            const uint32_t end = area.offsets[index + 1];
            const auto oldCount = static_cast<int64_t>(end - begin);
            const auto newCount = static_cast<int64_t>(species.size());
            if (oldCount == 0 && newCount == 0)
                return;  // 溢れの無いセル(ほとんど)は何もしない

            // --- 層: 0 でない層を初めて書く時だけ領域に層の並びを取る(T-1220)---
            const bool anyLayer = std::ranges::any_of(layers, [](uint64_t layer) { return layer != 0; });
            const bool layered = anyLayer || !area.layers.empty();
            if (layered && area.layers.size() != area.species.size())
                area.layers.assign(area.species.size(), 0);

            if (layered) {
                area.layers.erase(area.layers.begin() + begin, area.layers.begin() + end);
                if (layers.empty())
                    area.layers.insert(area.layers.begin() + begin, species.size(), 0);
                else
                    area.layers.insert(area.layers.begin() + begin, layers.begin(), layers.end());
            }

            area.species.erase(area.species.begin() + begin, area.species.begin() + end);
            area.amounts.erase(area.amounts.begin() + begin, area.amounts.begin() + end);
            area.species.insert(area.species.begin() + begin, species.begin(), species.end());
            area.amounts.insert(area.amounts.begin() + begin, amounts.begin(), amounts.end());

            const int64_t shift = newCount - oldCount;
            for (uint32_t later = index + 1; later <= MR_BLOCK_CELLS; ++later)
                area.offsets[later] = static_cast<uint32_t>(static_cast<int64_t>(area.offsets[later]) + shift);
        }

        // 一覧(species・amounts の先頭 count 個)の、インラインに入らない後ろの部分
        // (層 layers は一覧と同じ位置。空 = 全部 0)
        template <typename Amount>
        void WriteTail(MultiresOverflowArea& area, uint32_t index, const std::vector<uint32_t>& species,
                       const std::vector<Amount>& amounts, uint32_t count, std::span<const uint64_t> layers = {}) {
            const uint32_t inlineCount = std::min(count, RX_MAX_CELL_SPECIES);
            const std::span<const uint32_t> tailSpecies(species.data() + inlineCount, count - inlineCount);
            const std::span<const Amount> tailAmounts(amounts.data() + inlineCount, count - inlineCount);
            const std::span<const uint64_t> tailLayers = layers.empty() ? layers : layers.subspan(inlineCount);
            ReplaceOverflow(area, index, tailSpecies, tailAmounts, tailLayers);
        }

        bool AreaIsEmpty(const MultiresOverflowArea& area) {
            return area.offsets[MR_BLOCK_CELLS] == 0;
        }

        void ClearArea(MultiresOverflowArea& area) {
            area.offsets.fill(0);
            area.species.clear();
            area.amounts.clear();
            area.layers.clear();
        }

        // 上限の無い形のセルのインラインの部分(StoreWidePageCell の分け方)
        RxCell InlinePart(const RxWideCell& cell) {
            RxCell narrow = RxMakeEmptyCell(cell.energy);
            narrow.speciesCount = std::min(cell.speciesCount, RX_MAX_CELL_SPECIES);
            for (uint32_t i = 0; i < narrow.speciesCount; ++i) {
                narrow.species[i] = cell.species[i];
                narrow.amounts[i] = cell.amounts[i];
            }

            return narrow;
        }

    }  // namespace

    void EnableWideCells(MultiresNest& nest) {
        const size_t pageCount = size_t{nest.capacity.observerBlocks} + nest.capacity.pages;
        nest.wideCells = true;
        nest.cellOverflow.assign(pageCount, MultiresOverflowArea{});
        nest.fractionOverflow.assign(nest.capacity.fractions, MultiresOverflowArea{});
        nest.uniformOverflow.assign(nest.blocks.size(), MultiresUniformOverflow{});
    }

    RxWideCell LoadWideUniform(const MultiresNest& nest, uint32_t slot) {
        RxWideCell wide = RxWidenCell(nest_detail::UniformAt(nest, slot));
        if (!nest.wideCells)
            return wide;

        const MultiresUniformOverflow& tail = nest.uniformOverflow[slot];
        FX_ASSERT(tail.species.empty() || wide.speciesCount == RX_MAX_CELL_SPECIES);
        wide.species.insert(wide.species.end(), tail.species.begin(), tail.species.end());
        wide.amounts.insert(wide.amounts.end(), tail.amounts.begin(), tail.amounts.end());
        wide.speciesCount += static_cast<uint32_t>(tail.species.size());

        return wide;
    }

    void StoreWideUniform(MultiresNest& nest, uint32_t slot, const RxWideCell& value) {
        FX_ASSERT(nest.wideCells || RxFitsInline(value));
        RxCell& narrow = nest_detail::UniformAt(nest, slot);
        narrow = RxMakeEmptyCell(value.energy);
        narrow.speciesCount = std::min(value.speciesCount, RX_MAX_CELL_SPECIES);
        for (uint32_t i = 0; i < narrow.speciesCount; ++i) {
            narrow.species[i] = value.species[i];
            narrow.amounts[i] = value.amounts[i];
        }

        if (!nest.wideCells)
            return;

        MultiresUniformOverflow& tail = nest.uniformOverflow[slot];
        const uint32_t inlineCount = narrow.speciesCount;
        tail.species.assign(value.species.begin() + inlineCount, value.species.begin() + value.speciesCount);
        tail.amounts.assign(value.amounts.begin() + inlineCount, value.amounts.begin() + value.speciesCount);
    }

    RxWideCell LoadWideNestCell(const MultiresNest& nest, uint32_t slot, uint32_t index) {
        const MrBlock& block = nest.blocks[slot];
        if (nest.wideCells && MrIsUniform(block))
            return MrIsCoveredCell(block, index) ? RxMakeEmptyWideCell(0) : LoadWideUniform(nest, slot);

        RxWideCell wide = RxWidenCell(LoadNestCell(nest, slot, index));
        if (!nest.wideCells)
            return wide;

        const MultiresOverflowArea& area = nest.cellOverflow[block.page];
        const uint32_t begin = area.offsets[index];
        const uint32_t end = area.offsets[index + 1];
        FX_ASSERT(begin == end || wide.speciesCount == RX_MAX_CELL_SPECIES);
        wide.species.insert(wide.species.end(), area.species.begin() + begin, area.species.begin() + end);
        wide.amounts.insert(wide.amounts.end(), area.amounts.begin() + begin, area.amounts.begin() + end);
        wide.speciesCount += end - begin;

        return wide;
    }

    MrWideFraction LoadWideFraction(const MultiresNest& nest, uint32_t fractionSlot, uint32_t index) {
        MrWideFraction wide = MrWidenFraction(nest_detail::FractionAt(nest, fractionSlot, index));
        if (!nest.wideCells || fractionSlot == MR_NO_FRACTION)
            return wide;

        const MultiresOverflowArea& area = nest.fractionOverflow[fractionSlot];
        const uint32_t begin = area.offsets[index];
        const uint32_t end = area.offsets[index + 1];
        FX_ASSERT(begin == end || wide.speciesCount == RX_MAX_CELL_SPECIES);
        wide.species.insert(wide.species.end(), area.species.begin() + begin, area.species.begin() + end);
        wide.amounts.insert(wide.amounts.end(), area.amounts.begin() + begin, area.amounts.begin() + end);
        wide.speciesCount += end - begin;

        return wide;
    }

    void StoreWidePageCell(MultiresNest& nest, uint32_t page, uint32_t index, const RxWideCell& cell) {
        FX_ASSERT(nest.wideCells || RxFitsInline(cell));

        // --- 層のある頁: 書く前のセル(インライン + 溢れ)と物質で結んで付け替える(T-1220)---
        if (nest_detail::PageKeepsLayers(nest, page)) {
            const RxWideCell before = nest_detail::LoadWidePageCell(nest, page, index);
            const std::vector<uint64_t> layers = nest_detail::LoadWidePageCellLayers(nest, page, index);
            const std::span<const uint32_t> species(before.species.data(), before.speciesCount);
            nest_detail::StoreWidePageCellWithLayers(nest, page, index, cell,
                                                     nest_detail::RebindLayers(species, layers, cell));
            return;
        }

        const RxCell narrow = InlinePart(cell);
        nest_detail::StorePageCell(nest, page, index, narrow);

        if (nest.wideCells)
            WriteTail(nest.cellOverflow[page], index, cell.species, cell.amounts, cell.speciesCount);
    }

    void StoreWideFraction(MultiresNest& nest, uint32_t fractionSlot, uint32_t index, const MrWideFraction& fraction) {
        FX_ASSERT(nest.wideCells || fraction.speciesCount <= RX_MAX_CELL_SPECIES);
        MrFraction narrow = MrMakeEmptyFraction();
        narrow.energy = fraction.energy;
        narrow.speciesCount = std::min(fraction.speciesCount, RX_MAX_CELL_SPECIES);
        for (uint32_t i = 0; i < narrow.speciesCount; ++i) {
            narrow.species[i] = fraction.species[i];
            narrow.amounts[i] = fraction.amounts[i];
        }

        nest_detail::SetFraction(nest, fractionSlot, index, narrow);
        if (nest.wideCells)
            WriteTail(nest.fractionOverflow[fractionSlot], index, fraction.species, fraction.amounts,
                      fraction.speciesCount);
    }

    void ClearPageOverflow(MultiresNest& nest, uint32_t page) {
        if (nest.wideCells)
            ClearArea(nest.cellOverflow[page]);
    }

    void ClearFractionOverflow(MultiresNest& nest, uint32_t fractionSlot) {
        if (nest.wideCells && fractionSlot != MR_NO_FRACTION)
            ClearArea(nest.fractionOverflow[fractionSlot]);
    }

    bool PageHasOverflow(const MultiresNest& nest, uint32_t page) {
        return nest.wideCells && page < nest.cellOverflow.size() && !AreaIsEmpty(nest.cellOverflow[page]);
    }

    bool FractionHasOverflow(const MultiresNest& nest, uint32_t fractionSlot) {
        return nest.wideCells && fractionSlot != MR_NO_FRACTION && !AreaIsEmpty(nest.fractionOverflow[fractionSlot]);
    }

    // --- 成分ごとの層(T-1220)---

    namespace nest_detail {

        RxWideCell LoadWidePageCell(const MultiresNest& nest, uint32_t page, uint32_t index) {
            RxWideCell wide = RxWidenCell(LoadPageCell(nest, page, index));
            if (!nest.wideCells)
                return wide;

            const MultiresOverflowArea& area = nest.cellOverflow[page];
            const uint32_t begin = area.offsets[index];
            const uint32_t end = area.offsets[index + 1];
            wide.species.insert(wide.species.end(), area.species.begin() + begin, area.species.begin() + end);
            wide.amounts.insert(wide.amounts.end(), area.amounts.begin() + begin, area.amounts.begin() + end);
            wide.speciesCount += end - begin;

            return wide;
        }

        std::vector<uint64_t> LoadWidePageCellLayers(const MultiresNest& nest, uint32_t page, uint32_t index) {
            const MultiresCellPage& cells = nest.cells.pages[page];
            const uint32_t inlineCount = cells.energy.empty() ? 0u : cells.counts[index];
            const MrCellLayers inlineLayers = LoadPageCellLayers(nest, page, index);
            std::vector<uint64_t> layers(inlineLayers.amounts.begin(), inlineLayers.amounts.begin() + inlineCount);
            if (!nest.wideCells)
                return layers;

            const MultiresOverflowArea& area = nest.cellOverflow[page];
            for (uint32_t k = area.offsets[index]; k < area.offsets[index + 1]; ++k)
                layers.push_back(area.layers.empty() ? 0 : area.layers[k]);

            return layers;
        }

        void StoreWidePageCellWithLayers(MultiresNest& nest, uint32_t page, uint32_t index, const RxWideCell& cell,
                                         std::span<const uint64_t> layers) {
            FX_ASSERT(nest.wideCells || RxFitsInline(cell));
            FX_ASSERT(layers.size() == cell.speciesCount);
            MrCellLayers inlineLayers;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                FX_ASSERT(layers[i] <= cell.amounts[i]);
                if (i < RX_MAX_CELL_SPECIES)
                    inlineLayers.amounts[i] = layers[i];
            }

            StorePageCellWithLayers(nest, page, index, InlinePart(cell), inlineLayers);
            if (nest.wideCells)
                WriteTail(nest.cellOverflow[page], index, cell.species, cell.amounts, cell.speciesCount, layers);
        }

    }  // namespace nest_detail

}  // namespace bicameral::sim
