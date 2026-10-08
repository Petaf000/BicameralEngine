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
        bool expanded = false;  // この刻みに一様から頁に広げた

        // --- 待ちの丸め(T-0115)---
        bool evaluated = false;                       // 反応を評価した(wakeTick を求め直した)
        uint64_t wakeTick = reaction::RX_WAIT_NEVER;  // 評価したセルの次に評価の要る刻みの印の最小
    };

    // 待ちの丸め(T-0115): 刻む前に、つつかれたままのブロックを「刻み tick の直前に変わった」印にし、すぐ評価する(StepNest と StepActive で同じ)
    void ResolvePokes(MultiresNest& nest, uint64_t tick);

    // 待ちの丸め(T-0115): 刻んだ結果を見出しに書く。変わった・頁に広げたブロックは busyTick = この刻みの印・次の刻みに評価、
    // 評価して変わらなければ wakeTick = 求めた最小(StepNest と StepActive で同じ)
    void RecordWaitResults(MultiresNest& nest, std::span<const BlockStepResult> results, uint64_t tick);

    // stepped(枠ごとの 0 / 1)のブロックを 1 刻み: 一様なブロックは変わる時だけ頁に広げ(枠の順。足りなければ刻まずに種にする)、
    // options.conduction なら熱の伝導の変化を足してから反応を進める。伝導で変化を受け取ったブロックは stepped でなくても変わる。結果は枠ごと。
    // 伝導を小刻みに分ける時(T-0108)、wakeMark が 0 でなければ(活性)、小刻みで変わったブロックの面の隣にその印を付けて
    // 以後の小刻みから刻む(stepped を書き足す)。0 なら(全部を刻む)起こさない
    std::vector<BlockStepResult> StepBlocks(MultiresNest& nest, const ReactionTableView& view,
                                            std::span<uint8_t> stepped, uint64_t worldSeed, uint64_t tick,
                                            const MultiresStepOptions& options, uint32_t wakeMark);

    // 伝導の変化をセル(と端数)に足す。端数の枠が無いブロックへの変化は整数の単位だけ(multires_nest.cpp)
    void ApplyEnergyDelta(MultiresNest& nest, uint32_t slot, uint32_t index, reaction::RxCell& cell,
                          const multires::MrEnergyDelta& delta);

    // 枠 slot の本物のブロックの面の隣(八分の一ごと。細かい側へは面をたどる)に刻む印 mark を付ける(multires_activity.cpp の
    // 種の起こし方と同じ。忙しさの印は変えない。T-0108 の小刻みで変わったブロックに使う)
    void WakeAround(MultiresNest& nest, uint32_t slot, uint32_t mark);

    // --- 熱の伝導(multires_conduction.cpp。T-0019・T-0108)---

    // 刻みの初めの論理のセルの熱(初めて読んだ時に作る)。一様なブロックは覆われていないセルが全部同じなので枠ごとに 1 つ
    // (読むのは刻むセルと、その面の先の覆われていないセルだけ)。頁に広げても論理のセルは変わらないので使い回せる。
    // 小刻み(T-0108)でセルが変わったブロックは Invalidate で作り直させる
    class CellThermals {
    public:
        CellThermals(const MultiresNest& nest, const ReactionTableView& view);

        [[nodiscard]] multires::MrThermal At(uint32_t slot, uint32_t index);
        void Invalidate(uint32_t slot);

    private:
        const MultiresNest* m_nest;
        ReactionTableView m_view;
        std::vector<multires::MrThermal> m_values;  // 枠ごとに [セル × 512][一様の値]
        std::vector<uint8_t> m_known;
    };

    // 刻むブロック(stepped)のセルの面の流れを調べ、流れのある一様なブロック(自分の面か、細かい側から送られてくる面)に MR_PAGE_WANTED を付ける。
    // 粗い側で端数が要るブロックの印(枠ごとの 0 / 1)を返す。面の係数は options の小刻みの 1 回分(T-0108)
    std::vector<uint8_t> MarkConductionWants(MultiresNest& nest, CellThermals& thermals,
                                             std::span<const uint8_t> stepped, const MultiresStepOptions& options);

    // 端数が要るブロックに端数の枠を枠の順に配り(足りなければ数える)、刻むブロック(凍らせたものを除く)のセルの面の流れを
    // 頁のセルの添字(PageCellAddress)ごとの変化 deltas に足す。凍らせたブロックとの面は流れない
    void ComputeConduction(MultiresNest& nest, CellThermals& thermals, std::span<const uint8_t> stepped,
                           std::span<const uint8_t> frozen, std::span<const uint8_t> wantsFraction,
                           const MultiresStepOptions& options, std::span<multires::MrEnergyDelta> deltas);

    // 細かいレベルの熱の陰解法(T-0119。multires_implicit_conduction.cpp): 流れを計算する側が基準より細かい面を全部、陰解法の 1 刻みで解き、
    // 変化を deltas に足す(端数の枠は ComputeConduction が配った後)。陰解法の系に入れるブロックか(陽解法はそのブロックの面を計算しない)
    [[nodiscard]] bool InImplicitConduction(const MultiresNest& nest, uint32_t slot,
                                            const MultiresStepOptions& options);
    void AddImplicitConduction(MultiresNest& nest, CellThermals& thermals, std::span<const uint8_t> frozen,
                               const MultiresStepOptions& options, std::span<multires::MrEnergyDelta> deltas);

    // 伝導の小刻み(T-0108)を最後の 1 回の手前まで進め、最後の小刻みの変化を返す(呼ぶ側が反応と一緒に足す。分けない時は T-0019 と同じ順)。
    // 途中で変化を足したブロックは results の changed に、頁に広げたブロックは expanded に書く(multires_conduction.cpp)
    std::vector<multires::MrEnergyDelta> StepConduction(MultiresNest& nest, const ReactionTableView& view,
                                                        std::span<uint8_t> stepped, const MultiresStepOptions& options,
                                                        uint32_t wakeMark, std::span<BlockStepResult> results);

    // 一様で頁に広げたい(MR_PAGE_WANTED)ブロックに頁を配り、足りなかったブロック(この刻み・小刻みは凍らせる)の印を返す。
    // 広げたブロックは results の expanded に書く(multires_nest.cpp)
    std::vector<uint8_t> ExpandForStep(MultiresNest& nest, std::span<BlockStepResult> results);

    // 木を変えたブロックをつつく: 活性の種にし、忙しさの印を「つつかれた」にする(T-0100・T-0101)
    inline void PokeBlock(MultiresNest& nest, uint32_t slot) {
        nest.seeds[slot] = 1;
        nest.blocks[slot].busyTick = multires::MR_BUSY_POKED;
    }

}  // namespace bicameral::sim::nest_detail
