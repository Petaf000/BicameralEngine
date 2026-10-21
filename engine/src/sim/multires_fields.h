// multires_fields.h — 多重解像度の世界の登録した欄(運ぶ計算の共通の土台の段 1 ①。T-0711。ADR-0329。設計 §2.1・§3)。
// 欄はセル(成分 + エネルギー)の外に持ち越す値。登録の形は shaders/common/state_field.hlsli(GPU と 1 つの源)。
// セル・面の欄は「欄の頁」に置く: 並びはセルと同じ [一様の値 × 枠][頁 × MR_BLOCK_CELLS](頁と同じ番号)× 値の数。
// 木の操作(根を置く・頁に広げる・畳む・細かく・粗く)は nest_fields の関数を呼んで欄を付いて行かせる。
// 状態のハッシュ・保存点・写しは multires_state.h の一覧を回る(欄を登録すると一覧に入る)。
// 観察の枠(影・写し)も欄の頁を持ち、影を細かくする/引き戻す・写しを置くに欄が付いて行く(T-0761。ADR-0361。世界の値には触れない。D-403)。
// 欄の種類(T-0854。ADR-0399): 真実 / 導かれる値 / 温めた始まり(state_field.hlsli の SF_FLAG_DERIVED・SF_FLAG_WARM_START)。
// 導かれる値は眠る(頁を畳む)時に平均して残し、保存点に入らない(温めた始まりは D-549 の仮 (A) で入る)。
// まだ欄を持たない所(仮。ADR-0329): 溢れを使う世界の特別扱い(欄は溢れと関係しない)・GPU(段 6。T-0920)・
// ある所にだけ持つ(疎な頁。T-0921)。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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

    // --- 木の操作が呼ぶ(multires_tree・multires_nest・multires_activity)---
    namespace nest_fields {

        // 根・世界の写しを置いた枠 slot の欄を 0 に(一様なら値 1 つ、頁を持つなら頁を全部。前に使った枠・頁の値を残さない)。
        // 写しの欄の値は呼ぶ側が置いた後に StoreNestField・StoreNestBlockField で書く(写す元の世界が欄を持つ時)
        void PlaceRoot(MultiresNest& nest, uint32_t slot);

        // 一様な枠 slot を頁 page に広げる前に、欄の頁を一様の値で埋める(覆われたセルは 0)
        void ExpandPage(MultiresNest& nest, uint32_t slot, uint32_t page);

        // 頁を持つ枠 slot の欄の頁が一様か(覆われていないセルが全部同じ・覆われたセルは 0)。欄が無ければ true。
        // 導かれる値(SF_FLAG_DERIVED)は調べない(眠る時に平均して残すので、畳むのを止めない。T-0854)
        [[nodiscard]] bool PageFoldable(const MultiresNest& nest, uint32_t slot);

        // 頁を持つ枠 slot の欄を値 1 つに戻す(頁を返す前に。PageFoldable の後)。導かれる値は覆われていないセルの平均を
        // 残す(SfSleepValue。20 §10.3 の (b)。起きた時は ExpandPage が子は親と同じ数で配る)
        void FoldPage(MultiresNest& nest, uint32_t slot);

        // 親 parentSlot の八分の一 octant を子 childSlot(頁 childPage。一様なら MR_NO_PAGE)に細かくする: 子は親と同じ数。
        // 頁を持つ親の覆われたセルは 0 に。子の見出しを書く前に呼ぶ
        void Refine(MultiresNest& nest, uint32_t parentSlot, uint32_t octant, uint32_t childSlot, uint32_t childPage);

        // 一様な親 parentSlot へ子 childSlot を粗くしても、欄が一様のままか(違えば親を頁に広げる)。導かれる値は見ない(T-0854)
        [[nodiscard]] bool CoarsenKeepsUniform(const MultiresNest& nest, uint32_t childSlot, uint32_t parentSlot);

        // 子 childSlot を親の八分の一へ粗くする(和 ÷ 8。VOLUME の欄の余りは世界の帳簿の「粗くした端数・出」のその欄の列へ)。親の頁(要るなら広げた後)に書き、
        // 子の見出しを消す前に呼ぶ
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
