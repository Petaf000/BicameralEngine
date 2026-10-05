// multires_nest_internal.h — multires_nest.cpp と multires_tree.cpp が共有する、配列の読み書きの小さな道具(外からは使わない)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sim/multires_nest.h"

namespace bicameral::sim::nest_detail {

    // --- セル(nest.cells = [一様の値 × 枠][頁 × 512]。T-0102)---

    inline reaction::RxCell& UniformAt(MultiresNest& nest, uint32_t slot) {
        return nest.cells[slot];
    }

    inline const reaction::RxCell& UniformAt(const MultiresNest& nest, uint32_t slot) {
        return nest.cells[slot];
    }

    inline reaction::RxCell& PageCellAt(MultiresNest& nest, uint32_t page, uint32_t index) {
        return nest.cells[nest.blocks.size() + (size_t{page} * multires::MR_BLOCK_CELLS) + index];
    }

    // 頁を持つ枠のセル
    inline reaction::RxCell& CellAt(MultiresNest& nest, uint32_t slot, uint32_t index) {
        FX_ASSERT(!multires::MrIsUniform(nest.blocks[slot]));

        return PageCellAt(nest, nest.blocks[slot].page, index);
    }

    // 世界の頁の空きのスタック(nest.freeBlocks の後ろ)
    inline uint32_t& FreePageAt(MultiresNest& nest, uint32_t position) {
        return nest.freeBlocks[size_t{nest.capacity.worldBlocks} + position];
    }

    // 空きのスタックの上から頁を 1 つ取る(呼ぶ側が空きを確かめる)
    inline uint32_t PopPage(MultiresNest& nest) {
        FX_ASSERT(nest.counters[multires::MR_COUNTER_FREE_PAGES] > 0);

        return FreePageAt(nest, --nest.counters[multires::MR_COUNTER_FREE_PAGES]);
    }

    // 端数(枠が無ければ空)
    inline multires::MrFraction FractionAt(const MultiresNest& nest, uint32_t fractionSlot, uint32_t index) {
        if (fractionSlot == multires::MR_NO_FRACTION)
            return multires::MrMakeEmptyFraction();

        return nest.fractions[(size_t{fractionSlot} * multires::MR_BLOCK_CELLS) + index];
    }

    inline void SetFraction(MultiresNest& nest, uint32_t fractionSlot, uint32_t index,
                            const multires::MrFraction& fraction) {
        nest.fractions[(size_t{fractionSlot} * multires::MR_BLOCK_CELLS) + index] = fraction;
    }

    // 刻む中で頁に広げたい(MR_PAGE_WANTED)世界のブロックに、枠の順で頁を配って一様の値で埋める。
    // 頁が足りなければ一様に戻して数え、種にする(次の刻みにまた試す)。広げた枠を枠の順に返す(multires_nest.cpp。T-0102)
    std::vector<uint32_t> ExpandWantedPages(MultiresNest& nest);

    // 木を変えたブロックをつつく: 活性の種にし、忙しさの印を「つつかれた」にする(T-0100・T-0101)
    inline void PokeBlock(MultiresNest& nest, uint32_t slot) {
        nest.seeds[slot] = 1;
        nest.blocks[slot].busyTick = multires::MR_BUSY_POKED;
    }

}  // namespace bicameral::sim::nest_detail
