// patch_panel.h — 魔法のパッチを置くパネル(T-0325・D-449。docs/design/14 §2「実装(T-0325)」・02 §5.1・ADR-0095)。
// 中身(どの規則に・どの効き方を・どれだけ)を触って決めるための道具。値はすべて試験用の仮のもの(D-006)。
//
// データの流れ:
//   世界が今使っている反応表(UseTable)→ 規則を名前で選ぶ・効き方(PatchKinds()。common/magic_patch.hlsli の MP_KIND_* の一覧)と強さ・
//   範囲(中心 + 半幅の直方体。筆の直方体と同じ選び方。または両端の角)・期間を選ぶ
//   → 置く先が「仮の世界」なら sim::MakePatchCommand(PROBE_COMMAND_TYPE_PATCH)を TakeWorldCommands で返し、フレームのループが
//     刻みを付けて GPU のキューへ・再生ファイルへ(置くコマンドと同じ道)。「実験室」なら sim::LabSession::ApplyPatch(実験室のコマンド)。
//   → 置いたパッチの一覧(残りの刻み・断られた数): 仮の世界は GPU から戻るイベント PROBE_EVENT_PATCH_APPLIED(足せた / 断った)、
//     実験室は箱の CPU リファレンスの列で同じ関数(MpAddPatch)を当てて判定する。
//   → 範囲を描く(描画側だけ。BuildPatchRangePreview → editor/brush_preview の ProjectBrushPreview で画面の線分に。実験室は小さな断面)。
// パネルの選んだ値は View の状態で、世界にも再生ファイルにも入らない(入るのはコマンドだけ。D-107)。
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/brush_preview.h"
#include "sim/lab_session.h"
#include "sim/probe_sim.h"

namespace bicameral::editor {

    class LabPanel;

    // --- 効き方の一覧(common/magic_patch.hlsli の MP_KIND_*。種類を足したら行と MakePatchEffect の分岐を足す)---

    enum class PatchStrength : uint8_t {
        None,          // 強さが無い(止める)
        Shift,         // 2^shift の shift(MP_MAX_SHIFT まで)
        JoulesPerMol,  // 下げる活性化エネルギー(J/mol。MP_MAX_CATALYST_JOULES_PER_MOL まで)
    };

    struct PatchKind {
        uint32_t kind = 0;  // MP_KIND_*
        std::string_view label;
        PatchStrength strength = PatchStrength::None;
    };

    [[nodiscard]] std::span<const PatchKind> PatchKinds();

    // 効き方の語(MpMake*Effect)。strength は種類の強さ(None なら見ない)
    [[nodiscard]] uint32_t MakePatchEffect(uint32_t kind, int64_t strength);

    // 効き方の語を人が読む文に(一覧の表示)
    [[nodiscard]] std::string DescribePatchEffect(uint32_t effect);

    // パッチの範囲(レベル 0 のセルの直方体。両端を含む)の線: 直方体の 12 辺と、断面(軸 sliceAxis の plane)で切った四角形。
    // 仮の世界の格子(gridSize)の中に切る。全部外なら空
    [[nodiscard]] BrushPreview BuildPatchRangePreview(const std::array<int32_t, 3>& low,
                                                      const std::array<int32_t, 3>& high, uint32_t sliceAxis,
                                                      uint32_t slicePlane, uint32_t gridSize);

    class PatchPanel {
    public:
        // 世界が今使っている反応表(規則の名前の一覧)。Build の前に呼ぶ
        void UseTable(std::shared_ptr<const sim::BakedReactionTable> table, uint64_t version);

        // ImGui のフレームの中で呼ぶ(lab = 実験室のパネル。実験室に置く時に箱を使う)
        void Build(LabPanel& lab);

        // --- 仮の世界(フレームのループが呼ぶ)---
        // このフレームに投げるパッチのコマンド(targetTick は 0。フレームのループが付ける。sequence は通しで増やす)
        [[nodiscard]] std::vector<sim::ProbeCommand> TakeWorldCommands(uint64_t frame, uint32_t& sequence);

        // 左クリックのセルを範囲の中心にする(つつきの代わり。仮の世界に置く時だけ)
        [[nodiscard]] bool Picking() const { return m_picking && m_target == Target::World && m_table != nullptr; }
        void PickCenter(const render::CellCoordinate& cell);

        // GPU から戻った PROBE_EVENT_PATCH_APPLIED(投げた順に届く。place = 列の番号 / PROBE_PATCH_REFUSED)
        void NoteApplied(const sim::ProbeEvent& event);

        // 読み戻した刻み(残りの刻みの表示)
        void NoteTick(uint64_t tick) { m_worldTick = tick; }

        // 世界が刻み tick の境界へ戻った(その刻みから先に当たったパッチは、流し直すともう一度届く)
        void NoteRewound(uint64_t tick);

        // 仮の世界の効いている・待っているパッチの範囲の線(断面 sliceAxis・slicePlane。描画側)
        [[nodiscard]] std::vector<BrushPreviewSegment> WorldRangeSegments(uint32_t sliceAxis,
                                                                          uint32_t slicePlane) const;

        // 範囲の線を背景に引く(editor/editor_overlay)
        static void DrawRanges(std::span<const BrushPreviewLine> lines);

        // --- 人がいない確認(--auto-patch): 実験室で木を燃やしながらセルロースを使う規則を止め、止めた間は木が減らない
        //     (止めない同じ実験では減る)・列が一杯なら断る・記録から再生しても同じ。仮の世界に止めるパッチと当たらないパッチを投げ、
        //     足せた・断られたが GPU から戻る(--record で記録し、別の起動の --replay で同じハッシュ列)---
        void StartAuto() { m_autoPending = true; }
        [[nodiscard]] bool AutoPassed() const;
        [[nodiscard]] std::string AutoSummary() const;

    private:
        enum class Target : uint8_t {
            World,
            Lab,
        };

        enum class State : uint8_t {
            Waiting,  // 投げた・まだ当たっていない
            Applied,  // 列に足せた
            Refused,  // 断られた(列が一杯・当てられない)
        };

        // 置いたパッチ 1 つ(表示用。View)
        struct Placed {
            Target target = Target::World;
            uint32_t order = 0;  // 投げた順(仮の世界のイベントを投げた順に結ぶ)
            std::string rule;
            uint32_t effect = 0;
            uint32_t durationTicks = 0;
            std::array<int32_t, 3> low{};
            std::array<int32_t, 3> high{};
            uint64_t startTick = 0;  // 当たった刻み(待っている間は 0)
            State state = State::Waiting;
        };

        // 今の選択から作るパッチ(規則の ID が表に無ければ空)
        struct Composed {
            uint32_t rule = 0;
            uint32_t effect = 0;
            uint32_t durationTicks = 0;
            std::array<int32_t, 3> low{};
            std::array<int32_t, 3> high{};
        };

        [[nodiscard]] bool Compose(Composed& composed) const;
        bool PlaceInWorld(const Composed& composed);
        bool PlaceInLab(LabPanel& lab, const Composed& composed);
        [[nodiscard]] Placed MakePlaced(Target target, const Composed& composed);

        void BuildRule();
        void BuildEffect();
        void BuildRange();
        void BuildList(LabPanel& lab);
        void BuildLabSlice(LabPanel& lab);

        void RunAutoLab(LabPanel& lab);
        [[nodiscard]] std::string RunAutoLabExperiment(LabPanel& lab, bool stop, uint64_t& celluloseLeft);
        [[nodiscard]] std::string PlaceAutoLabPatches(LabPanel& lab, std::span<const uint32_t> rules,
                                                      sim::LabCellRange whole);
        void QueueAutoWorld();

        std::shared_ptr<const sim::BakedReactionTable> m_table;
        uint64_t m_tableVersion = 0;

        // --- 操作の状態(View。世界に入らない)---
        Target m_target = Target::World;
        int m_rule = 0;        // 表の規則 ID
        std::string m_filter;  // 規則の名前で絞る
        size_t m_kind = 1;     // PatchKinds() の添字(初めは止める)
        int m_shift = 4;       // 2^shift
        int m_joulesPerMol = 20000;
        int m_durationTicks = 60;
        bool m_corners = false;                      // 範囲を両端の角で(false = 中心 + 半幅)
        bool m_picking = false;                      // 左クリックのセルを中心に
        std::array<int, 3> m_center = {28, 32, 32};  // 仮の世界の木箱の壁(--auto-ignite の火)
        std::array<int, 3> m_halfCells = {3, 3, 3};
        std::array<int, 3> m_low = {0, 0, 0};
        std::array<int, 3> m_high = {7, 7, 7};
        int m_labSliceZ = 3;

        // --- 置いたもの ---
        std::vector<Placed> m_placed;
        std::vector<sim::ProbeCommand> m_worldQueue;  // 次の TakeWorldCommands で返す
        uint32_t m_nextOrder = 0;
        uint64_t m_worldTick = 0;
        std::vector<uint32_t> m_labPredictList;  // 実験室の同じ刻みに置いたパッチを足した列(断るかの判定。MpAddPatch)
        uint64_t m_labPredictTick = UINT64_MAX;
        std::string m_message;

        // --- 自動の確認 ---
        bool m_autoPending = false;
        bool m_autoLabDone = false;
        bool m_autoLabPassed = false;
        bool m_autoWorldQueued = false;
        std::string m_autoLabSummary;
        std::vector<std::pair<uint32_t, State>>
            m_autoWorldExpected;  // 仮の世界に投げた自動のパッチ(投げた順, 届くはずの結果)
    };

}  // namespace bicameral::editor
