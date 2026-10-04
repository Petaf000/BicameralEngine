// multires_nest_internal.h — multires_nest.cpp と multires_tree.cpp が共有する、配列の読み書きの小さな道具(外からは使わない)。
#pragma once

#include <cstddef>
#include <cstdint>

#include "sim/multires_nest.h"

namespace bicameral::sim::nest_detail {

    inline reaction::RxCell& CellAt(MultiresNest& nest, uint32_t slot, uint32_t index) {
        return nest.cells[(size_t{slot} * multires::MR_BLOCK_CELLS) + index];
    }

    inline const reaction::RxCell& CellAt(const MultiresNest& nest, uint32_t slot, uint32_t index) {
        return nest.cells[(size_t{slot} * multires::MR_BLOCK_CELLS) + index];
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

    // 木を変えたブロックをつつく: 活性の種にし、忙しさの印を「つつかれた」にする(T-0100・T-0101)
    inline void PokeBlock(MultiresNest& nest, uint32_t slot) {
        nest.seeds[slot] = 1;
        nest.blocks[slot].busyTick = multires::MR_BUSY_POKED;
    }

}  // namespace bicameral::sim::nest_detail
