// multires_nest.h — 多重解像度の木の CPU リファレンス(17 §1・§3・§5。T-0017・T-0018・T-0100・T-0101。ADR-0015・ADR-0016)。
// ブロックの枠・セル・端数・空きのスタック・索引・世界の帳簿を配列で持ち、GPU(sim/gpu_multires)と同じ順・同じ関数
// (shaders/common/multires.hlsli・multires_tree.hlsli)で操作する。テストは CPU と GPU の配列をそのまま比べる(索引だけは引いた結果を比べる)。
//
// 枠の範囲: 世界の枠 [0, worldBlocks) は木の管理(要求)が空きのスタックから取る。観察の枠 [worldBlocks, +observerBlocks)
// (影・世界の写し)は呼ぶ側が直接決める(覗いても世界の枠の番号が変わらない)。
// 世界の木を変えるのは要求だけ(SubmitRequests → ProcessRequests)。根は最初に PlaceRootBlock で置く。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "common/multires_tree.hlsli"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 細かくする先の点(pointLevel のセルの単位の世界の座標)
    struct MultiresPoint {
        int64_t x = 0;
        int64_t y = 0;
        int64_t z = 0;
        int32_t level = 0;
    };

    struct MultiresCapacity {
        uint32_t worldBlocks = 0;     // 世界の枠の数
        uint32_t observerBlocks = 0;  // 観察の枠の数
        uint32_t fractions = 0;       // 端数の枠の数
        uint32_t indexEntries = 0;    // 索引の大きさ(2 の冪。世界の枠の 2 倍以上を勧める)
        uint32_t ledgerColumns = 1;   // 帳簿の列(1 + 物質の数)
        int32_t rootLevel = 0;        // 根のレベル
    };

    struct MultiresNest {
        MultiresCapacity capacity;

        // --- 状態(CPU と GPU で配列のまま一致する)---
        std::vector<multires::MrBlock> blocks;        // 世界の枠 + 観察の枠
        std::vector<reaction::RxCell> cells;          // 枠 × MR_BLOCK_CELLS
        std::vector<multires::MrFraction> fractions;  // 端数の枠 × MR_BLOCK_CELLS
        std::vector<uint32_t> freeBlocks;             // 世界の枠の空きのスタック(数は counters[MR_COUNTER_FREE_BLOCKS])
        std::vector<uint32_t> freeFractions;  // 端数の枠の空きのスタック(数は counters[MR_COUNTER_FREE_FRACTIONS])
        std::vector<uint64_t> ledger;         // 世界の帳簿 [MR_LEDGER_LEVELS × ledgerColumns]
        std::array<uint32_t, multires::MR_COUNTER_COUNT> counters{};

        // --- 活性の種(T-0100)。次の StepActive で刻むブロックの元(世界の枠ごとに 0 / 1)。
        //     GPU は順の決まらない一覧で持つので、集合として比べる(状態の要約に入れない)---
        std::vector<uint8_t> seeds;

        // --- 索引(見出しから作り直せる。状態に入らない)---
        std::vector<uint32_t> index;

        // --- 1 刻みの要求(状態に入らない。数は counters[MR_COUNTER_REQUESTS])---
        std::vector<multires::MrRequest> requests;     // MR_MAX_REQUESTS 個
        std::vector<multires::MrRequestState> states;  // MR_MAX_REQUESTS 個
        std::vector<uint32_t> claims;                  // 世界の枠ごとの取り合いの印
    };

    // 保存量の合計(最も細かい単位 × 2^-64。256bit の 2 の補数、下の語から)
    using Wide256 = std::array<uint64_t, 4>;

    struct ConservedTotals {
        std::vector<Wide256> elements;  // 添字 = 元素
        Wide256 energy{};

        bool operator==(const ConservedTotals&) const = default;
    };

    [[nodiscard]] MultiresNest MakeMultiresNest(const MultiresCapacity& capacity);

    // --- 世界の木 ---

    // 根(本物、親なし。レベルは capacity.rootLevel)を空きのスタックから取った枠に置き、索引に入れる。cells は MR_BLOCK_CELLS 個。枠を返す
    uint32_t PlaceRootBlock(MultiresNest& nest, int64_t originX, int64_t originY, int64_t originZ,
                            std::span<const reaction::RxCell> cells);

    // 要求を一覧の後ろに足す(一覧は決定的な順に。1 刻み MR_MAX_REQUESTS 件まで)
    void SubmitRequests(MultiresNest& nest, std::span<const multires::MrRequest> requests);

    // 一覧の要求を処理する(解決 → 確定 → 割り当て → 適用 → 解放 → 索引の作り直し。17 §5)。一覧は空になる
    void ProcessRequests(MultiresNest& nest);

    // 索引で (レベル, 原点) の本物のブロックを引く(無ければ MR_NO_BLOCK)
    [[nodiscard]] uint32_t LookupBlock(const MultiresNest& nest, int32_t level, int64_t originX, int64_t originY,
                                       int64_t originZ);

    // 索引を空にして、使っている本物のブロックを全部入れ直す
    void RebuildIndex(MultiresNest& nest);

    // --- 観察の枠(呼ぶ側が枠を決める)---

    // 世界の写し(MR_BLOCK_MIRROR。刻まない・世界のハッシュに入らない)を置く。写し直すときも同じ。cells は MR_BLOCK_CELLS 個。
    // 木の形をしていない世界(仮の世界の 64³)の 1 ブロックを、影の鎖の根の親にするため(T-0096)
    void PlaceMirrorBlock(MultiresNest& nest, uint32_t slot, int32_t level, int64_t originX, int64_t originY,
                          int64_t originZ, std::span<const reaction::RxCell> cells);

    // parentSlot の、点を含む八分の一を観察の影 firstChildSlot に細かくし、それを levelCount 段続ける(子の枠は firstChildSlot から順)。
    // 親に触れない(世界に返さない)
    void RefineShadowChain(MultiresNest& nest, uint32_t parentSlot, uint32_t firstChildSlot, uint32_t levelCount,
                           const MultiresPoint& point);

    // 影の鎖を捨てる(世界に返さない)
    void RemoveShadowChain(MultiresNest& nest, uint32_t firstSlot, uint32_t levelCount);

    // 影の鎖を上から順に親へ引き戻す(firstShadowSlot の親は本物か写しのブロック)
    void PullBackShadowChain(MultiresNest& nest, const BakedReactionTable& table, uint32_t firstShadowSlot,
                             uint32_t levelCount);

    // --- 刻みと要約 ---

    // 刻むセル(本物の葉と影のセル)の反応を 1 刻み(全部を刻む。活性の正しさを確かめる基準)
    void StepNest(MultiresNest& nest, const BakedReactionTable& table, uint64_t worldSeed, uint64_t tick);

    // 活性のブロックだけ刻む(T-0100。multires_activity.hlsli): 種とその面の隣に印(activeTick)を付けて刻み、
    // 進める規則があったブロックを次の種にする。観察の枠は全部刻む。結果のセルは StepNest と同じになる
    void StepActive(MultiresNest& nest, const BakedReactionTable& table, uint64_t worldSeed, uint64_t tick);

    // 静かな本物の葉(MR_QUIET_TICKS 刻みを超えて種でも進める規則もない。multires_activity.hlsli)を粗くする要求を、
    // 一覧の後ろに世界の枠の順で足す(T-0101)。一覧が一杯なら足さずに数える。ProcessRequests の前に、StepActive で刻む木に使う
    void SubmitQuietCoarsenRequests(MultiresNest& nest, uint64_t tick);

    // 活性の種の枠の一覧(枠の順)
    [[nodiscard]] std::vector<uint32_t> SeedSlots(const MultiresNest& nest);

    // 世界(本物の葉のセルと端数)の要約。影は入らない
    [[nodiscard]] uint64_t HashRealLeaves(const MultiresNest& nest);

    // 状態の全部(見出し〔活性と忙しさの印も〕・セル・端数・空きのスタック・帳簿・数える欄)の要約。CPU と GPU を比べるため。索引・要求・種は入らない
    [[nodiscard]] uint64_t HashWholeNest(const MultiresNest& nest);

    // 本物の葉のセルと世界の帳簿の、元素の数とエネルギーの合計を finestLevel の単位 × 2^-64 で
    // (finestLevel は使っている最も細かいレベルと、帳簿に落ちた最も細かいレベル以上)
    [[nodiscard]] ConservedTotals ComputeConservedTotals(const MultiresNest& nest, const BakedReactionTable& table,
                                                         int32_t finestLevel);

}  // namespace bicameral::sim
