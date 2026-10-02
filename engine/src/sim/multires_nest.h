// multires_nest.h — 多重解像度の入れ子の細分の CPU リファレンス(17 §1・§3。T-0017。ADR-0015)。
// ブロックの枠・セル・端数を配列で持ち、細かくする・粗くする・刻む・影を引き戻すを、GPU(sim/gpu_multires)と同じ順・同じ関数
// (shaders/common/multires.hlsli)で行う。テストは CPU と GPU の配列をそのまま比べる。
//
// この段階(原理の確認)の割り当て: ブロックの枠は呼ぶ側が決める(鎖は firstSlot から順に)。端数の枠は数える欄から順に取り、返さない。
// 疎な木・ブロックプール・端数の解放は T-0018。浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "common/multires.hlsli"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 細かくする先の点(pointLevel のセルの単位の世界の座標)
    struct MultiresPoint {
        int64_t x = 0;
        int64_t y = 0;
        int64_t z = 0;
        int32_t level = 0;
    };

    struct MultiresNest {
        std::vector<multires::MrBlock> blocks;
        std::vector<reaction::RxCell> cells;          // 枠 × MR_BLOCK_CELLS
        std::vector<multires::MrFraction> fractions;  // 端数の枠 × MR_BLOCK_CELLS
        std::array<uint32_t, multires::MR_COUNTER_COUNT> counters{};
    };

    // 保存量の合計(最も細かい単位 × 2^-64。256bit の 2 の補数、下の語から)
    using Wide256 = std::array<uint64_t, 4>;

    struct ConservedTotals {
        std::vector<Wide256> elements;  // 添字 = 元素
        Wide256 energy{};

        bool operator==(const ConservedTotals&) const = default;
    };

    [[nodiscard]] MultiresNest MakeMultiresNest(uint32_t blockCapacity, uint32_t fractionCapacity);

    // 根のブロック(本物、親なし)を置く。cells は MR_BLOCK_CELLS 個
    void PlaceRootBlock(MultiresNest& nest, uint32_t slot, int32_t level, int64_t originX, int64_t originY,
                        int64_t originZ, std::span<const reaction::RxCell> cells);

    // parentSlot の、点を含む八分の一を firstChildSlot に細かくし、それを levelCount 段続ける(子の枠は firstChildSlot から順)。
    // kind が MR_BLOCK_REAL なら親を覆う(親のセルは空になる)、MR_BLOCK_SHADOW なら親に触れない
    void RefineChain(MultiresNest& nest, uint32_t parentSlot, uint32_t firstChildSlot, uint32_t levelCount,
                     uint32_t kind, const MultiresPoint& point);

    // deepestSlot から親へ levelCount 段、粗く戻す(本物の鎖)
    void CoarsenChain(MultiresNest& nest, uint32_t deepestSlot, uint32_t levelCount);

    // 影の鎖を捨てる(世界に返さない)
    void RemoveShadowChain(MultiresNest& nest, uint32_t firstSlot, uint32_t levelCount);

    // 刻むセル(本物の葉と影のセル)の反応を 1 刻み
    void StepNest(MultiresNest& nest, const BakedReactionTable& table, uint64_t worldSeed, uint64_t tick);

    // 影の鎖を上から順に親へ引き戻す(firstShadowSlot の親は本物のブロック)
    void PullBackShadowChain(MultiresNest& nest, const BakedReactionTable& table, uint32_t firstShadowSlot,
                             uint32_t levelCount);

    // 世界(本物の葉のセルと端数)の要約。影は入らない
    [[nodiscard]] uint64_t HashRealLeaves(const MultiresNest& nest);

    // 全部(見出し・セル・端数・数える欄)の要約。CPU と GPU を比べるため
    [[nodiscard]] uint64_t HashWholeNest(const MultiresNest& nest);

    // 本物の葉のセルの元素の数とエネルギーの合計を、finestLevel の単位 × 2^-64 で(finestLevel は使っている最も細かいレベル以上)
    [[nodiscard]] ConservedTotals ComputeConservedTotals(const MultiresNest& nest, const BakedReactionTable& table,
                                                         int32_t finestLevel);

}  // namespace bicameral::sim
