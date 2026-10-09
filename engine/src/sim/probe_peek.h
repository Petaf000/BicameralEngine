// probe_peek.h — 覗き窓(T-0096。17 §3 B「見るだけなら変わらない」): 仮の世界(sim/probe_sim の 64³)の 1 点の周りを
// 観察の影の鎖(k = 1〜9、0.5 m → 約 1 mm)にして、デバッグ表示が読む抽出の覗きの欄へ写す。
//
// データの流れ(ProbeSim の抽出の後のフック。ProbeFrameInput::afterExtract):
//   世界のセル S(t) の、点を含む 8³ のブロック → 入れ子の枠 0(世界の写し MR_BLOCK_MIRROR。毎回写し直す)
//   → 初めてなら細かくする(影の鎖 = 枠 1〜9、shaders/sim/multires_graph.hlsl)
//     それ以外は、世界の刻みが進んでいれば影を 1 刻み(multires_step.hlsl)→ 親へ引き戻す(上から順に)
//   → 影のセルの温度と見る物質の量を抽出の覗きの欄へ(shaders/sim/multires_peek.hlsl。並びは probe_sim.hlsli)
// 世界のバッファは読むだけなので、覗いても世界のハッシュ列は変わらない(window_replay_peek の ctest が確かめる)。
// 影の時間: 抽出は 1 フレームに 1 回までなので、世界が 1 フレームに何刻み進んでも影は 1 刻み(器具の中の時間はゆっくりでよい。17 §4)。
//
// 覗く場所は CPU が決める View の状態(D-107 の Controller。世界には入らない)。CPU リファレンスは ProbePeekReference(同じ順・同じ関数)。
// 反応表の差し替え(ホットリロード。T-0194・ADR-0047): 世界が刻み s の始めに表を替えたら、QueueTableSwap(s, 表) で知らせる。
// 覗き窓は s より後の境界(S(t)、t > s)を初めて抽出する時に表を替え、影の鎖を捨てて今の世界の写しから作り直す
// (前の表で作った影の待ちの予定と熱を持ち越さない。ADR-0018 の「表を変えるものは『変わった』にしてから評価する」を、作り直しで満たす)。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "common/probe_sim.hlsli"
#include "sim/gpu_multires.h"
#include "sim/multires_nest.h"
#include "sim/probe_sim.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // --- 入れ子の枠(shaders/sim/multires_peek.hlsl の PEEK_MIRROR_SLOT と同じ)---
    inline constexpr uint32_t PEEK_MIRROR_SLOT = 0;
    inline constexpr uint32_t PEEK_FIRST_SHADOW_SLOT = 1;
    inline constexpr uint32_t PEEK_LEVEL_COUNT = PROBE_PEEK_MAX_LEVELS;
    inline constexpr uint32_t PEEK_BLOCK_CAPACITY = PEEK_FIRST_SHADOW_SLOT + PEEK_LEVEL_COUNT;
    static_assert(PROBE_PEEK_BLOCK_CELLS == multires::MR_BLOCK_CELLS &&
                  PROBE_PEEK_BLOCK_EDGE == multires::MR_BLOCK_EDGE);

    // 覗く世界のセル
    struct PeekCell {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t z = 0;

        bool operator==(const PeekCell&) const = default;
    };

    // 覗く点(一番細かいレベルのセルの単位): 世界のセルの真ん中
    [[nodiscard]] MultiresPoint PeekPointOfCell(PeekCell cell);

    // 覗く場所の状態(GPU と CPU リファレンスで共通。どの順で何を記録するかを決める)
    class PeekState {
    public:
        void Look(PeekCell cell) {
            m_requested = cell;
            m_hasRequest = true;
        }

        void Stop() { m_hasRequest = false; }

        // 反応表が変わった: 次の Plan で影の鎖を捨てて作り直す(T-0194)
        void TableChanged() { m_tableChanged = true; }

        [[nodiscard]] bool IsLooking() const { return m_hasRequest; }
        [[nodiscard]] PeekCell Requested() const { return m_requested; }

        // 1 回の抽出で何をするか(Plan を呼ぶと、その結果を記録した前提で状態が進む)
        struct Plan {
            bool removeShadow = false;  // 前の影の鎖を捨てる
            bool mirror = false;        // 世界の写しを写し直す
            bool refine = false;        // 影の鎖を作る
            bool step = false;          // 影を 1 刻み
            bool pullBack = false;      // 親へ引き戻す
            PeekCell cell;              // 覗いているセル(mirror のとき)
        };

        [[nodiscard]] Plan NextPlan(uint64_t tick);

    private:
        PeekCell m_requested;
        bool m_hasRequest = false;
        bool m_tableChanged = false;

        // --- 影の鎖の今 ---
        PeekCell m_active;
        bool m_hasActive = false;
        uint64_t m_lastTick = 0;  // 影が最後に追いついた世界の刻み
    };

    // --- GPU ---
    class ProbePeek {
    public:
        [[nodiscard]] static std::expected<ProbePeek, std::string> Create(ID3D12Device5* device,
                                                                          const BakedReactionTable& table);

        void Look(PeekCell cell) { m_state.Look(cell); }
        void Stop() { m_state.Stop(); }
        [[nodiscard]] bool IsLooking() const { return m_state.IsLooking(); }
        [[nodiscard]] PeekCell Requested() const { return m_state.Requested(); }

        // 世界が刻み tick の始めに table へ替えた(T-0194)。表は写して持つ(呼んだ後は持たなくてよい)
        void QueueTableSwap(uint64_t tick, const BakedReactionTable& table) {
            m_pendingTables.push_back({tick, table});
        }

        // ProbeFrameInput::afterExtract から呼ぶ(シミュのフレームのリストの抽出の後ろ)
        void RecordAfterExtract(ID3D12GraphicsCommandList10* list, const ProbeExtractContext& context);

        // テスト: 入れ子(見出し・セル・端数・数える欄)を読み戻す
        void RecordReadback(ID3D12GraphicsCommandList10* list) { m_nest.RecordReadback(list); }
        [[nodiscard]] bool Read(MultiresNest& nest) const { return m_nest.Read(nest); }

    private:
        struct PendingTable {
            uint64_t tick = 0;
            BakedReactionTable table;
        };

        ProbePeek(GpuMultires&& nest, const std::array<uint32_t, PROBE_VIEW_SPECIES_COUNT>& viewSpecies)
            : m_nest(std::move(nest)), m_viewSpecies(viewSpecies) {}

        void ApplyDueTables(const ProbeExtractContext& context);

        GpuMultires m_nest;
        std::vector<PendingTable> m_pendingTables;  // 世界が替えた、まだ覗き窓が替えていない表(刻みの順)
        ComPtr<ID3D12PipelineState> m_mirrorPipeline;
        ComPtr<ID3D12PipelineState> m_extractPipeline;
        std::array<uint32_t, PROBE_VIEW_SPECIES_COUNT> m_viewSpecies{};
        PeekState m_state;
        bool m_uploaded = false;  // 空の入れ子を写したか(最初のフックで 1 回)
    };

    // --- CPU リファレンス(ProbePeek とビット一致するはずのもの。CLAUDE.md 原則 4)---
    class ProbePeekReference {
    public:
        explicit ProbePeekReference(const BakedReactionTable& table);

        void Look(PeekCell cell) { m_state.Look(cell); }
        void Stop() { m_state.Stop(); }

        // ProbePeek::QueueTableSwap と同じ(table は呼ぶ側が持ち続ける)
        void QueueTableSwap(uint64_t tick, const BakedReactionTable& table) {
            m_pendingTables.push_back({tick, &table});
        }

        // 抽出 1 回ぶん。world は S(tick) の 1 世代(PROBE_CELL_COUNT 個)
        void Advance(std::span<const reaction::RxCell> world, uint64_t tick);

        [[nodiscard]] const MultiresNest& Nest() const { return m_nest; }

        // 抽出の覗きの欄(PROBE_EXTRACTION_PEEK_WORDS 語。GPU の ExtractShadow と同じ値。書かない語は 0)
        [[nodiscard]] std::vector<uint32_t> Extraction() const;

    private:
        struct PendingTable {
            uint64_t tick = 0;
            const BakedReactionTable* table = nullptr;
        };

        const BakedReactionTable* m_table;
        std::vector<PendingTable> m_pendingTables;
        std::array<uint32_t, PROBE_VIEW_SPECIES_COUNT> m_viewSpecies{};
        MultiresNest m_nest;
        PeekState m_state;
    };

}  // namespace bicameral::sim
