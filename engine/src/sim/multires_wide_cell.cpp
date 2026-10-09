// multires_wide_cell.cpp — 多重解像度の世界のセルの溢れ(multires_nest.h の「セルの溢れ」。T-0187・02 §3・17)。
// セルと端数は「インライン RX_MAX_CELL_SPECIES 個(nest.cells・nest.fractions。GPU と同じ並び)+ 頁・端数の枠ごとの溢れ」に分けて持つ。
// 読む時は合わせて上限の無い形(RxWideCell・MrWideFraction)にし、書く時は先頭をインラインへ・残りを溢れへ。
// 溢れはセルの番号の順に詰める(1 セルを書き直すと後ろのセルの位置がずれる)。並びは書いた順に依らず、内容だけで決まる。
#include <algorithm>

#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        // 溢れの領域のセル index の範囲を、species・amounts の [first, first + count) で置き換える
        template <typename Amount>
        void ReplaceOverflow(MultiresOverflowArea& area, uint32_t index, std::span<const uint32_t> species,
                             std::span<const Amount> amounts) {
            FX_ASSERT(species.size() == amounts.size());
            const uint32_t begin = area.offsets[index];
            const uint32_t end = area.offsets[index + 1];
            const auto oldCount = static_cast<int64_t>(end - begin);
            const auto newCount = static_cast<int64_t>(species.size());
            if (oldCount == 0 && newCount == 0)
                return;  // 溢れの無いセル(ほとんど)は何もしない


            area.species.erase(area.species.begin() + begin, area.species.begin() + end);
            area.amounts.erase(area.amounts.begin() + begin, area.amounts.begin() + end);
            area.species.insert(area.species.begin() + begin, species.begin(), species.end());
            area.amounts.insert(area.amounts.begin() + begin, amounts.begin(), amounts.end());

            const int64_t shift = newCount - oldCount;
            for (uint32_t later = index + 1; later <= MR_BLOCK_CELLS; ++later)
                area.offsets[later] = static_cast<uint32_t>(static_cast<int64_t>(area.offsets[later]) + shift);
        }

        // 一覧(species・amounts の先頭 count 個)の、インラインに入らない後ろの部分
        template <typename Amount>
        void WriteTail(MultiresOverflowArea& area, uint32_t index, const std::vector<uint32_t>& species,
                       const std::vector<Amount>& amounts, uint32_t count) {
            const uint32_t inlineCount = std::min(count, RX_MAX_CELL_SPECIES);
            const std::span<const uint32_t> tailSpecies(species.data() + inlineCount, count - inlineCount);
            const std::span<const Amount> tailAmounts(amounts.data() + inlineCount, count - inlineCount);
            ReplaceOverflow(area, index, tailSpecies, tailAmounts);
        }

        bool AreaIsEmpty(const MultiresOverflowArea& area) {
            return area.offsets[MR_BLOCK_CELLS] == 0;
        }

        void ClearArea(MultiresOverflowArea& area) {
            area.offsets.fill(0);
            area.species.clear();
            area.amounts.clear();
        }

    }  // namespace

    void EnableWideCells(MultiresNest& nest) {
        const size_t pageCount = size_t{nest.capacity.observerBlocks} + nest.capacity.pages;
        nest.wideCells = true;
        nest.cellOverflow.assign(pageCount, MultiresOverflowArea{});
        nest.fractionOverflow.assign(nest.capacity.fractions, MultiresOverflowArea{});
    }

    RxWideCell LoadWideNestCell(const MultiresNest& nest, uint32_t slot, uint32_t index) {
        RxWideCell wide = RxWidenCell(LoadNestCell(nest, slot, index));
        const MrBlock& block = nest.blocks[slot];
        if (!nest.wideCells || MrIsUniform(block))
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
        RxCell& narrow = nest_detail::PageCellAt(nest, page, index);
        narrow = RxMakeEmptyCell(cell.energy);
        narrow.speciesCount = std::min(cell.speciesCount, RX_MAX_CELL_SPECIES);
        for (uint32_t i = 0; i < narrow.speciesCount; ++i) {
            narrow.species[i] = cell.species[i];
            narrow.amounts[i] = cell.amounts[i];
        }

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

}  // namespace bicameral::sim
