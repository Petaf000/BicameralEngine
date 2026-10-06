// multires_nest.h — 多重解像度の木の CPU リファレンス(17 §1・§3・§5。T-0017・T-0018・T-0100・T-0101。ADR-0015・ADR-0016)。
// ブロックの枠・セル・端数・空きのスタック・索引・世界の帳簿を配列で持ち、GPU(sim/gpu_multires)と同じ順・同じ関数
// (shaders/common/multires.hlsli・multires_tree.hlsli)で操作する。テストは CPU と GPU の配列をそのまま比べる(索引だけは引いた結果を比べる)。
//
// 枠の範囲: 世界の枠 [0, worldBlocks) は木の管理(要求)が空きのスタックから取る。観察の枠 [worldBlocks, +observerBlocks)
// (影・世界の写し)は呼ぶ側が直接決める(覗いても世界の枠の番号が変わらない)。
// 世界の木を変えるのは要求だけ(SubmitRequests → ProcessRequests)。根は最初に PlaceRootBlock で置く。
// セル(T-0102): 見出しの枠とセルの頁は別のプール。一様なブロックは頁を持たず値 1 つ(multires.hlsli の先頭)。
// セルを読むときは LoadNestCell(一様でも頁でも同じ「論理のセル」)を使う。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "common/multires_tree.hlsli"
#include "sim/implicit_conduction.h"
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
        uint32_t pages = 0;           // 世界の頁の数(観察の枠の頁は別に観察の枠の数だけ。T-0102)
        uint32_t indexEntries = 0;    // 索引の大きさ(2 の冪。世界の枠の 2 倍以上を勧める)
        uint32_t ledgerColumns = 1;   // 帳簿の列(1 + 物質の数)
        int32_t rootLevel = 0;        // 根のレベル
    };

    struct MultiresNest {
        MultiresCapacity capacity;

        // --- 状態(CPU と GPU で配列のまま一致する)---
        std::vector<multires::MrBlock> blocks;  // 世界の枠 + 観察の枠
        std::vector<reaction::RxCell> cells;  // [一様の値 × 枠][頁 × MR_BLOCK_CELLS](頁 = 観察の枠の数 + pages。T-0102)
        std::vector<multires::MrFraction> fractions;  // 端数の枠 × MR_BLOCK_CELLS
        // [世界の枠の空きのスタック(数は counters[MR_COUNTER_FREE_BLOCKS])][世界の頁の空きのスタック(MR_COUNTER_FREE_PAGES)]
        std::vector<uint32_t> freeBlocks;
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

        // --- 最後の刻みの陰解法の費用(T-0119。状態に入らない。計測用)---
        ImplicitCost implicitCost;
        uint32_t implicitCells = 0;  // 陰解法の系に入れたセルの数(境のセルを含む)
        std::vector<std::array<uint32_t, 2>>
            implicitLevels;  // 多重格子の段ごとの [節の数, 1 節の隣の最大](GPU の分け方を見る)

        // true なら、陰解法を入れた刻みの系(解く前の形)を implicitGrid に写す(GPU の GpuImplicit と比べる試験・計測用。T-0127)
        bool captureImplicitGrid = false;
        ImplicitGrid implicitGrid;
        // true なら、陰解法の系を作る時の木(陽解法の流れの後・変化を足す前)と凍った枠を写す(GPU で系を作る GpuImplicitBuild と
        // 比べる試験用。T-0129)。写しの中の写しは空
        bool captureImplicitNest = false;
        std::shared_ptr<const MultiresNest> implicitNest;
        std::vector<uint8_t> implicitFrozen;
    };

    // 1 刻みの選択
    struct MultiresStepOptions {
        // 熱の伝導(T-0019。multires_conduction.hlsli)。GPU(sim/gpu_multires の RecordStep・RecordStepActive)も同じ結果(T-0107)
        bool conduction = false;

        // --- 細かいレベルの熱の刻み(T-0108。multires_conduction.hlsli の「細かいレベルの刻み」)---
        // レベル k の伝導を 1 刻みに 4^clamp(k − subcycleBaseLevel, 0, maxSubcycleGap) 回の小刻みに分ける。
        // subcycleBaseLevel は面の係数が熱容量の上限で頭打ちにならない最後のレベル(MrSubcycleBaseLevel。最も伝わりやすい物質で決める)。
        // maxSubcycleGap = 0 なら分けない(T-0019 と同じ結果)。上限は 3(64 回。17 §4)。GPU も同じ結果(T-0109)
        int32_t subcycleBaseLevel = 0;
        uint32_t maxSubcycleGap = 0;

        // --- 細かいレベルの熱の陰解法(方式②。T-0119。ADR-0019・D-434 の案 a)---
        // true なら、流れを計算する側(細かい側)のレベルが subcycleBaseLevel より細かい面を、陰解法の 1 刻みで解く
        // (multires_implicit_conduction.cpp)。maxSubcycleGap は 0 のこと(方式①と②は混ぜない)。GPU の伝導の段はまだ(解く段は T-0127、系を作るのは T-0129、呼ぶのは T-0132)
        bool implicitConduction = false;
        // V サイクルの上限。新しい温度の誤差の見込みが 1 mK 以下になったら止める(ADR-0019)。D-436(鋭い熱でも解き切る)なので
        // 上限は「届かない時の安全のため」だけの大きさにする(当たったら安全網が陽解法の流れに戻す)
        uint32_t implicitMaxCycles = 64;
        // 陰解法にする最も細かいレベル = subcycleBaseLevel + implicitMaxGap。それより細かい所は陽解法のまま(頭打ちで遅い)。
        // 温度の端数(mK × 2^16)の精度で、面の流れの誤差は約 4^Δk × 2^-16 mK になり、Δk 8 を超えると 1 mK の判定に届かない(T-0119)
        uint32_t implicitMaxGap = 8;

        // --- 反応の丸め(T-0115。ADR-0018)---
        // false(既定): 待ちの丸め(reaction.hlsli の RxStepCellWait。D-429)。見出しの busyTick = tc、wakeTick = 次に評価の要る刻みの印。
        // true: 今までの丸め(RxStepCell と D-424 の下限)。GPU はまだこちらなので、GPU と比べるテストだけが使う(T-0121 で消す)
        bool cutoffRounding = false;
    };

    constexpr uint32_t MULTIRES_MAX_SUBCYCLE_GAP = 3;

    // 保存量の合計(最も細かい単位 × 2^-64。256bit の 2 の補数、下の語から)
    using Wide256 = std::array<uint64_t, 4>;

    struct ConservedTotals {
        std::vector<Wide256> elements;  // 添字 = 元素
        Wide256 energy{};

        bool operator==(const ConservedTotals&) const = default;
    };

    [[nodiscard]] MultiresNest MakeMultiresNest(const MultiresCapacity& capacity);

    // --- セル(T-0102)---

    // 枠 slot のセル index(一様なら値か、覆われていれば空。頁なら頁のセル)
    [[nodiscard]] reaction::RxCell LoadNestCell(const MultiresNest& nest, uint32_t slot, uint32_t index);

    // 使っている世界の頁の数(観察の枠の頁は数えない)
    [[nodiscard]] uint32_t UsedWorldPages(const MultiresNest& nest);

    // --- 世界の木 ---

    // 根(本物、親なし。レベルは capacity.rootLevel)を空きのスタックから取った枠に置き、索引に入れる。cells は MR_BLOCK_CELLS 個。
    // 全部同じなら一様(頁なし)、でなければ頁を取る(T-0102)。枠を返す
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

    // 刻むセル(本物の葉と影のセル)を 1 刻み(全部を刻む。活性の正しさを確かめる基準)。options.conduction なら熱の伝導の後に反応。
    // 一様なブロックは変わる(反応が進む・伝導の流れがある)時だけ、枠の順で頁に広げて刻む(頁が足りなければ刻まずに数え、種にする。T-0102)。
    // 伝導は面の隣を木から探し、レベルの違う面も保存量をビット単位で保って受け渡す(影は自分のブロックの中だけ。T-0019)
    void StepNest(MultiresNest& nest, const BakedReactionTable& table, uint64_t worldSeed, uint64_t tick,
                  const MultiresStepOptions& options = {});

    // 活性のブロックだけ刻む(T-0100。multires_activity.hlsli): 種とその面の隣に印(activeTick)を付けて刻み、
    // 進める規則があった・変わったブロックを次の種にする。観察の枠は全部刻む。結果のセルは StepNest と同じになる。
    // 待ちの丸め(T-0115)では、種 = 見出しを全部なめて起こす刻み(wakeTick)が来たブロック + つつかれたブロック。
    // 変わったブロックは次の刻みに起こす(伝導の面の隣も起こすため)
    void StepActive(MultiresNest& nest, const BakedReactionTable& table, uint64_t worldSeed, uint64_t tick,
                    const MultiresStepOptions& options = {});

    // 頁を持つ世界のブロックで、刻み tick にちょうど静かになり(multires_activity.hlsli の MrWantsFoldCheck)一様なものを、
    // 値 1 つに戻して頁を枠の順に空きのスタックへ返す(T-0103)。SubmitQuietCoarsenRequests の前に、StepActive で刻む木に使う
    void FoldQuietPages(MultiresNest& nest, uint64_t tick);

    // ほぼ同じ頁も畳む(T-0104。D-428。17 §5「ほぼ同じ頁を畳む」): 調べる時は上と同じ。覆われていないセルの差が tolerance の中なら
    // 平均の値 1 つに戻し、切り捨てで余った単位(セルの数未満)を世界の帳簿へ移す(合計はビット一致)。
    // 同じ時に、端数の枠を持つブロックは端数を全部帳簿へ移して枠を返す(1 単位未満 = 計器で測れない差。ADR-0017 追記)。
    // tolerance が MrExactFoldTolerance なら上と同じ結果(ビット単位で同じ時だけ畳む・端数は返さない)。
    // GPU は GpuMultires::RecordFoldPages の許容差つき(T-0112)
    void FoldQuietPages(MultiresNest& nest, const BakedReactionTable& table, uint64_t tick,
                        const multires::MrFoldTolerance& tolerance);

    // 頁を持つブロックの覆われていないセルの集計(T-0104。畳む判定と計測に使う)
    [[nodiscard]] multires::MrFoldStats CollectFoldStats(const MultiresNest& nest, const ReactionTableView& table,
                                                         uint32_t slot);

    // 静かな本物の葉(MR_QUIET_TICKS 刻みを超えて種でも進める規則もない。multires_activity.hlsli)を粗くする要求を、
    // 一覧の後ろに世界の枠の順で足す(T-0101)。一覧が一杯なら足さずに数える。ProcessRequests の前に、StepActive で刻む木に使う
    void SubmitQuietCoarsenRequests(MultiresNest& nest, uint64_t tick);

    // 活性の種の枠の一覧(枠の順)
    [[nodiscard]] std::vector<uint32_t> SeedSlots(const MultiresNest& nest);

    // 待ちの丸め(T-0115)で刻み tick の種になる世界の本物のブロックの枠の一覧(枠の順): 種の印があるか、起こす刻み(wakeTick)が来た
    [[nodiscard]] std::vector<uint32_t> WaitSeedSlots(const MultiresNest& nest, uint64_t tick);

    // 世界(本物の葉のセルと端数)の要約。影は入らない
    [[nodiscard]] uint64_t HashRealLeaves(const MultiresNest& nest);

    // 状態の全部(見出し〔活性と忙しさの印も〕・セル・端数・空きのスタック・帳簿・数える欄)の要約。CPU と GPU を比べるため。索引・要求・種は入らない
    [[nodiscard]] uint64_t HashWholeNest(const MultiresNest& nest);

    // 本物の葉のセルと世界の帳簿の、元素の数とエネルギーの合計を finestLevel の単位 × 2^-64 で
    // (finestLevel は使っている最も細かいレベルと、帳簿に落ちた最も細かいレベル以上)
    [[nodiscard]] ConservedTotals ComputeConservedTotals(const MultiresNest& nest, const BakedReactionTable& table,
                                                         int32_t finestLevel);

}  // namespace bicameral::sim
