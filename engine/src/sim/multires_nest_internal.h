// multires_nest_internal.h — multires_nest.cpp・multires_tree.cpp・multires_activity.cpp・multires_conduction.cpp が共有する、配列の読み書きと 1 刻みの小さな道具(外からは使わない)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "common/multires_conduction.hlsli"
#include "sim/multires_nest.h"

namespace bicameral::sim::nest_detail {

    // multires_activity.hlsli の Tree の約束(索引で引く)
    struct CpuTree {
        const MultiresNest* nest = nullptr;

        [[nodiscard]] multires::MrBlock Block(uint32_t slot) const { return nest->blocks[slot]; }

        [[nodiscard]] uint32_t Lookup(int32_t level, int64_t originX, int64_t originY, int64_t originZ) const {
            return LookupBlock(*nest, level, originX, originY, originZ);
        }
    };

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

    // 頁のセルの nest.cells の添字
    inline size_t PageCellAddress(const MultiresNest& nest, uint32_t page, uint32_t index) {
        return nest.blocks.size() + (size_t{page} * multires::MR_BLOCK_CELLS) + index;
    }

    // --- 1 刻み(multires_nest.cpp の StepBlocks。StepNest と StepActive が使う)---

    struct BlockStepResult {
        bool changed = false;   // セル(か端数)が 1 つでも変わった
        bool possible = false;  // 進める反応の規則があった
        bool expanded = false;  // この刻みに一様から頁に広げた
    };

    // stepped(枠ごとの 0 / 1)のブロックを 1 刻み: 一様なブロックは変わる時だけ頁に広げ(枠の順。足りなければ刻まずに種にする)、
    // conduction なら熱の伝導の変化を足してから反応を進める。伝導で変化を受け取ったブロックは stepped でなくても変わる。結果は枠ごと
    std::vector<BlockStepResult> StepBlocks(MultiresNest& nest, const ReactionTableView& view,
                                            std::span<const uint8_t> stepped, uint64_t worldSeed, uint64_t tick,
                                            bool conduction);

    // --- 熱の伝導(multires_conduction.cpp。T-0019)---

    // 刻むブロックのセルの面の流れを調べ、流れのある一様なブロック(自分の面か、細かい側から送られてくる面)に MR_PAGE_WANTED を付ける。
    // 粗い側で端数が要るブロックの印(枠ごとの 0 / 1)を返す
    std::vector<uint8_t> MarkConductionWants(MultiresNest& nest, const ReactionTableView& view,
                                             std::span<const uint8_t> stepped);

    // 端数が要るブロックに端数の枠を枠の順に配り(足りなければ数える)、刻むブロック(凍らせたものを除く)のセルの面の流れを
    // 頁のセルの添字(PageCellAddress)ごとの変化にして返す。凍らせたブロックとの面は流れない
    std::vector<multires::MrEnergyDelta> ComputeConduction(MultiresNest& nest, const ReactionTableView& view,
                                                           std::span<const uint8_t> stepped,
                                                           std::span<const uint8_t> frozen,
                                                           std::span<const uint8_t> wantsFraction);

    // 木を変えたブロックをつつく: 活性の種にし、忙しさの印を「つつかれた」にする(T-0100・T-0101)
    inline void PokeBlock(MultiresNest& nest, uint32_t slot) {
        nest.seeds[slot] = 1;
        nest.blocks[slot].busyTick = multires::MR_BUSY_POKED;
    }

}  // namespace bicameral::sim::nest_detail
