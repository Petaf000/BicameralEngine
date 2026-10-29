// multires_fields.h — 多重解像度の世界の登録した欄(運ぶ計算の共通の土台の段 1 ①。T-0711。ADR-0329。設計 §2.1・§3)。
// 欄はセル(成分 + エネルギー)の外に持ち越す値。登録の形は shaders/common/state_field.hlsli(GPU と 1 つの源)。
// セル・面の欄は「欄の頁」に置く: 並びはセルと同じ [一様の値 × 枠][頁 × MR_BLOCK_CELLS](頁と同じ番号)× 値の数。
// 木の操作(根を置く・頁に広げる・畳む・細かく・粗く)は nest_fields の関数を呼んで欄を付いて行かせる。
// 状態のハッシュ・保存点・写しは multires_state.h の一覧を回る(欄を登録すると一覧に入る)。
// 観察の枠(影・写し)も欄の頁を持ち、影を細かくする/引き戻す・写しを置くに欄が付いて行く(T-0761。ADR-0361。世界の値には触れない。D-403)。
// 欄の種類(T-0854。ADR-0399): 真実 / 導かれる値 / 温めた始まり(state_field.hlsli の SF_FLAG_DERIVED・SF_FLAG_WARM_START)。
// 導かれる値は眠る(頁を畳む)時に平均して残し、保存点に入らない(温めた始まりは D-549 の仮 (A) で入る)。
// ある所にだけ持つ(疎な頁。T-0921・ADR-0403。20 §10.2): セル・面の欄の頁は 0 でない値を最初に書く時に作り、眠る判定で全部 0 なら・
// セルの頁を返す時に返す。無い頁は全部 0 と読む。疎か密かは MultiresNest::sparseFields で、どちらも結果はビット単位で同じ。
// まだ欄を持たない所(仮。ADR-0329): 溢れを使う世界の特別扱い(欄は溢れと関係しない)・GPU(段 6。T-0920)。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "common/state_field.hlsli"
#include "sim/multires_nest.h"

namespace bicameral::sim {

    inline constexpr uint32_t MULTIRES_NO_FIELD = UINT32_MAX;

    // 欄を登録する(作った直後・根を置く前に)。返すのは欄の番号(登録の順)。名前が空・重なる・登録が不正なら MULTIRES_NO_FIELD
    [[nodiscard]] uint32_t RegisterNestField(MultiresNest& nest, std::string name,
                                             const state_field::SfFieldDesc& desc);

    // 名前で欄を引く(無ければ MULTIRES_NO_FIELD)
    [[nodiscard]] uint32_t FindNestField(const MultiresNest& nest, std::string_view name);

    // --- セル・面の欄 ---

    // 枠 slot のセル index の値(一様なら値 1 つ・覆われたセルは 0)
    [[nodiscard]] int64_t LoadNestField(const MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t index,
                                        uint32_t component);

    // 頁を持つ枠のセルに書く(値は型の範囲の中)
    void StoreNestField(MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t index, uint32_t component,
                        int64_t value);

    // 一様な枠の値 1 つを書く
    void StoreNestFieldUniform(MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t component, int64_t value);

    // --- ブロック・世界の欄 ---

    [[nodiscard]] int64_t LoadNestBlockField(const MultiresNest& nest, uint32_t field, uint32_t slot,
                                             uint32_t component);
    void StoreNestBlockField(MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t component, int64_t value);
    [[nodiscard]] int64_t LoadNestWorldField(const MultiresNest& nest, uint32_t field, uint32_t component);
    void StoreNestWorldField(MultiresNest& nest, uint32_t field, uint32_t component, int64_t value);

    // 保存量の欄 field の値 0 の通し番号(登録の順に保存量の欄の値を数える。ConservedTotals::fields の添字で、
    // 世界の帳簿の列は NestFieldLedgerBase + これ。T-0762)。保存量の欄でなければ MULTIRES_NO_FIELD
    [[nodiscard]] uint32_t NestConservedIndex(const MultiresNest& nest, uint32_t field);

    // 登録した欄の置き場の使っている大きさ(T-0921。M-0 の 1 セルの大きさの見積もり用。確保した容量ではない)。
    // 一様の値は使っている枠の数だけ・密の頁は頁を持つブロックの数だけ・疎の頁は値のある欄の頁だけ + 頁の表(頁を持つブロック × 4 バイト)
    struct NestFieldMemory {
        uint64_t uniformBytes = 0;  // 一様の値(セル・面の欄の枠ごと + ブロック・世界の欄)
        uint64_t pageBytes = 0;     // 欄の頁
        uint64_t tableBytes = 0;    // 疎の頁の表
    };

    [[nodiscard]] NestFieldMemory MeasureNestFields(const MultiresNest& nest);

    // 内訳の欄(SF_FLAG_BREAKDOWN。T-1177)の状態の検査: 0 ≤ 内訳 ≤ 親 でない値の数(世界と観察の枠の全部。一様な枠は値 1 つで数える)
    [[nodiscard]] uint64_t CountBreakdownMisfits(const MultiresNest& nest);

    // --- 集めて書く段(T-1252。ADR-0500 の 3・20 §10.7)---
    // 順の決まらない書き手(光線など)は欄へ直に足さず、自分の作業(NestGatherWork。刻みの中だけ)へ整数で足す(順によらない。R3・R4)。
    // 作業の鍵は (本物の葉の世界の枠・セル・値)。升目 → 葉の写し(粗い葉は 2^L 角の和)は書き手の側(T-1117 の葉ごとの写し)。
    // GatherNestField が触った枠を枠の順に回り、(a) 欄の頁が無ければ枠の順に頁を取る(プレフィックス和と同じ番号の順。GPU の段 2 も同じ)、
    // (b) 一様な枠は頁に広げてから(ExpandWantedPages と同じ枠の順。値はビット単位で変わらない)、(c) 作業の値を欄へ足し、書いた枠をつつく。
    // 頁の予算(SetNestFieldPageBudget)が尽きた・セルの頁が尽きた枠は書かず、値は作業に残す(量を捨てない。報告の pageShortage で計器の誤り)
    struct NestGatherWork {
        uint32_t field = MULTIRES_NO_FIELD;
        uint32_t components = 0;
        std::vector<uint32_t> blockOf;  // [世界の枠] → 作業のブロックの番号(MULTIRES_NO_FIELD_PAGE = 触っていない)
        std::vector<uint32_t> slots;    // 作業のブロック → 世界の枠(足した順。集める時に枠の順に並べる)
        std::vector<int64_t> values;    // [作業のブロック × MR_BLOCK_CELLS × 値の数]
    };

    struct NestGatherReport {
        uint32_t blocks = 0;            // 書いた枠
        uint32_t pagesTaken = 0;        // 新しく取った欄の頁
        uint32_t expanded = 0;          // 頁に広げた一様な枠
        uint32_t pageShortage = 0;      // 欄の頁の予算が尽きて書かなかった枠(計器の誤り。値は作業に残る)
        uint32_t cellPageShortage = 0;  // セルの頁が尽きて広げられなかった一様な枠(同上)
    };

    [[nodiscard]] NestGatherWork MakeNestGatherWork(const MultiresNest& nest, uint32_t field);

    // 書き手が足す(slot は本物の世界の枠・index は刻むセル)
    void AddNestGatherWork(NestGatherWork& work, uint32_t slot, uint32_t index, uint32_t component, int64_t value);

    // 作業に残っている値の和(頁が尽きて書かなかった分。保存の検査で欄の和に足す)
    [[nodiscard]] int64_t NestGatherWorkTotal(const NestGatherWork& work, uint32_t component);

    [[nodiscard]] NestGatherReport GatherNestField(MultiresNest& nest, NestGatherWork& work);

    // 疎な欄の頁の予算(世界の作りで前もって決める。D-468。0 = 決めない = 今まで通り要るだけ取る)
    void SetNestFieldPageBudget(MultiresNest& nest, uint32_t field, uint32_t pages);

    // 使っている欄の頁の数(疎: 値のある頁・密: セルの頁の数)
    [[nodiscard]] uint32_t UsedNestFieldPages(const MultiresNest& nest, uint32_t field);

    // --- この刻みだけの欄(SF_FLAG_TICK_ONLY。T-1252。ADR-0500 の 2)---
    // 当てる段の読む口: 欄 field の 0 でない値を枠の順・セルの順に 1 回の掃きで渡し、欄を 0 にする(頁を返す)。使い手はまだ無い(T-1228)
    void ConsumeTickOnlyField(
        MultiresNest& nest, uint32_t field,
        const std::function<void(uint32_t slot, uint32_t index, uint32_t component, int64_t value)>& apply);

    // 刻みの境目の検査: この刻みだけの欄が全部 0 か(StepNest・StepActive の初めに FX_ASSERT)
    [[nodiscard]] bool TickOnlyFieldsClear(const MultiresNest& nest);

    // --- 木の操作が呼ぶ(multires_tree・multires_nest・multires_activity)---
    namespace nest_fields {

        // 頁を持つ枠 slot の疎な欄の頁のうち、全部 0 のものを返す(眠る判定で。読む値は変わらない。T-0921)
        void ReleaseZeroPages(MultiresNest& nest, uint32_t slot);

        // 頁を持つ枠 slot の欄の頁を返す(枠を捨てる前に。影の鎖を捨てる時。T-0921)
        void ReleaseBlockPages(MultiresNest& nest, uint32_t slot);

        // 根・世界の写しを置いた枠 slot の欄を 0 に(一様なら値 1 つ、頁を持つなら頁を全部。前に使った枠・頁の値を残さない)。
        // 写しの欄の値は呼ぶ側が置いた後に StoreNestField・StoreNestBlockField で書く(写す元の世界が欄を持つ時)
        void PlaceRoot(MultiresNest& nest, uint32_t slot);

        // 一様な枠 slot を頁 page に広げる前に、欄の頁を一様の値で埋める(覆われたセルは 0)
        void ExpandPage(MultiresNest& nest, uint32_t slot, uint32_t page);

        // 頁を持つ枠 slot の欄の頁が一様か(覆われていないセルが全部同じ・覆われたセルは 0)。欄が無ければ true。
        // 導かれる値(SF_FLAG_DERIVED)は調べない(眠る時に平均して残すので、畳むのを止めない。T-0854)
        [[nodiscard]] bool PageFoldable(const MultiresNest& nest, uint32_t slot);

        // 頁を持つ枠 slot の欄を値 1 つに戻す(頁を返す前に。PageFoldable の後)。導かれる値は覆われていないセルの平均を
        // 残す(SfSleepValueOf。20 §10.3 の (b)。眠る時に捨てる印の欄は 0。起きた時は ExpandPage が子は親と同じ数で配る)
        void FoldPage(MultiresNest& nest, uint32_t slot);

        // 親 parentSlot の八分の一 octant を子 childSlot(頁 childPage。一様なら MR_NO_PAGE)に細かくする: 子は親と同じ数。
        // 頁を持つ親の覆われたセルは 0 に。子の見出しを書く前に呼ぶ。配り手の欄(SF_SCALE_OWNER)は最後にセルの配りの口が
        // 配り手の細かくする関数で書く(セルの中身の配りの後。T-1252)
        void Refine(MultiresNest& nest, uint32_t parentSlot, uint32_t octant, uint32_t childSlot, uint32_t childPage);

        // 一様な親 parentSlot へ子 childSlot を粗くしても、欄が一様のままか(違えば親を頁に広げる)。導かれる値は見ない(T-0854)
        [[nodiscard]] bool CoarsenKeepsUniform(const MultiresNest& nest, uint32_t childSlot, uint32_t parentSlot);

        // 子 childSlot を親の八分の一へ粗くする(和 ÷ 8。VOLUME の欄の余りは世界の帳簿の「粗くした端数・出」のその欄の列へ)。親の頁(要るなら広げた後)に書き、
        // 子の見出しを消す前に呼ぶ。配り手の欄は一般の段の後に粗くする時のセルの配りの口が配り手の粗くする関数で書く(T-1252)
        void Coarsen(MultiresNest& nest, uint32_t childSlot);

        // --- 観察の影(親を覆わない・世界に返さない。D-403。ADR-0361)---

        // 親 parentSlot の八分の一 octant から影 childSlot(頁 childPage)の欄を作る: 子は親と同じ数・ブロックの欄は親の値。
        // 親の欄は変えない(覆われた親のセルは 0 のまま読む)
        void RefineShadow(MultiresNest& nest, uint32_t parentSlot, uint32_t octant, uint32_t childSlot,
                          uint32_t childPage);

        // 影 shadowSlot の欄を親へ引き戻す: 親のセルごとに子 2³ の和を親 × 8 に(state_field.hlsli の SfPullBackChild)・
        // ブロックの欄は親の値に。親・世界の欄・世界の帳簿には触れない
        void PullBackShadow(MultiresNest& nest, uint32_t shadowSlot);

    }  // namespace nest_fields

}  // namespace bicameral::sim
