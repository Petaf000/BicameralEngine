// brush_panel.h — 世界に物を置く筆のパネル(T-0222・T-0245・D-449。docs/design/14 §2「実装(T-0222)」「実装(T-0245)」)。
//
// データの流れ:
//   世界が今使っている反応表(UseTable)→ 材料の行(editor/material_rows。表の全物質から 3 つまで・mol・g・kPa。ひな形から読み込める)
//   → パネルで形(球・直方体・円柱)と大きさ・温度・置き換えか足すかを選び、「筆を持つ」
//   → 窓の左ボタンを押した・押したまま動いた・離したセル(フレームのループ)→ editor/brush_stroke が置く中心の列にする
//     (間隔・1 フレームの上限・待っているコマンドの上限)→ 中心ごとに置くコマンド(sim::MakePlaceCommand)
//   → フレームのループが刻みを付けて GPU のキューへ・再生ファイルへ(つつきと同じ道)。世界は GPU だけが変える(D-107)。
// パネルの選んだ値は View の状態で、世界にも再生ファイルにも入らない(入るのはコマンドだけ)。
// 元に戻す(T-0278・ADR-0101): 運び(押してから、離して置き終えるまで)ごとにコマンドの番号を控え、「元に戻す」(ボタン・Ctrl+Z)で
//   フレームのループが最後の運びを除いて保存点から流し直す(editor/brush_undo)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/brush_stroke.h"
#include "editor/brush_undo.h"
#include "editor/material_rows.h"
#include "render/debug_camera.h"
#include "sim/lab_box.h"
#include "sim/probe_sim.h"

namespace bicameral::editor {

    class BrushPanel {
    public:
        // 世界が今使っている反応表(版が変わった時だけ材料の一覧を作り直す)。Build の前に呼ぶ
        void UseTable(std::shared_ptr<const sim::BakedReactionTable> table, uint64_t version);

        // ImGui のフレームの中で呼ぶ
        void Build();

        // 筆を持っている(左ボタンがつつきでなく「置く」になる)
        [[nodiscard]] bool Holding() const { return m_holding && m_table != nullptr; }

        // 当て方が「面に沿って」(カーソルの光線が最初に当たる物の手前に置く。T-0332・editor/surface_pick)。
        // フレームのループは、この間(と --auto-place の間)だけ物の印を読み戻す
        [[nodiscard]] bool OnSurface() const { return m_onSurface; }

        // 1 フレームの左ボタンの入力(押した・押したまま通った・離した断面のセル)を、今の筆の置くコマンドの列にする
        // (targetTick は 0。フレームのループが付ける)。waitingCommands = GPU のキューとフレームのループで待っているコマンドの数
        [[nodiscard]] std::vector<sim::ProbeCommand> Paint(std::span<const render::CellCoordinate> pressed,
                                                           std::span<const render::CellCoordinate> dragged,
                                                           bool released, size_t waitingCommands, uint32_t& sequence);

        // cell を中心にした今の筆の形(カーソルの下に形を描く。T-0279・editor/brush_preview。材料は入らない)
        [[nodiscard]] sim::ProbePlaceShape PreviewShape(const render::CellCoordinate& cell) const {
            return MakeShape({cell.x, cell.y, cell.z});
        }

        // 置いたことが GPU から戻った(PROBE_EVENT_PLACE_APPLIED)
        void NotePlaced(const sim::ProbeEvent& event);

        // --- 元に戻す(T-0278・ADR-0101)---
        // 元に戻すを頼まれた(ボタン・Ctrl+Z・--auto-place)。フレームのループがフレームごとに 1 回取る
        [[nodiscard]] bool TakeUndoRequest() { return std::exchange(m_undoRequested, false); }

        // 最後の運びのコマンドの番号(無ければ空)。塗っている途中(Stroking)は戻さない
        [[nodiscard]] std::span<const uint32_t> LastStroke() const;
        [[nodiscard]] bool Stroking() const { return m_stroke.Active(); }

        // 最後の運びを元に戻した。firstTick = その運びが最初に当たった刻み・rewindTick = 戻した保存点の刻み
        // (UINT64_MAX = まだ GPU へ足していなかったので除いただけ)・latest = 戻す前に読み戻した最後の状態
        // (流し直した同じ刻みの状態と違えば、運びが世界を変えていた = 本当に除けた)
        void NoteUndone(uint64_t firstTick, uint64_t rewindTick, const sim::ProbeTickHash& latest);
        void NoteUndoFailed(std::string reason);

        // 世界が刻み tick の境界へ戻った(巻き戻しも元に戻すも)。その刻みから先に置いた記録を消す(流し直すともう一度届く)
        void NoteRewound(uint64_t tick);

        // 読み戻した状態 S(tick) のハッシュ(元に戻した後の流し直しを、戻す前の同じ刻みと比べる)
        void NoteHash(uint64_t tick, uint64_t worldHash);

        // --- 人がいない確認(--auto-place): 決まったフレームに筆で置く・足す・はみ出す・当たらない値を投げ、火を付け、
        //     直方体をドラッグで塗り(1 フレームに置ききれない分を次のフレームへ)、自由に組んだ材料の円柱を足し、つついてから
        //     円柱・直方体の運びを 1 つずつ元に戻す(T-0278。記録は元に戻した後の流れ。別の起動の --replay で同じハッシュ列)---
        void StartAuto() { m_autoRunning = true; }

        // フレーム frame に投げる自動のコマンド(sequence は通しで増やす)。surface = 今の物の印(sim/probe_surface。無ければ空)
        [[nodiscard]] std::vector<sim::ProbeCommand> TakeAutoCommands(uint64_t frame, size_t waitingCommands,
                                                                      std::span<const uint32_t> surface,
                                                                      uint32_t& sequence);

        // 当てられるはずの置くコマンドが全部 GPU で当たった(当たらない値のコマンドは当たっていない)
        [[nodiscard]] bool AutoPassed() const;
        [[nodiscard]] std::string AutoSummary() const;

    private:
        enum class ShapeKind : uint8_t {
            Sphere,
            Box,
            Cylinder,
        };

        [[nodiscard]] const sim::LabMaterial* FindMaterial(std::string_view name) const;
        [[nodiscard]] sim::ProbePlaceShape MakeShape(const BrushCell& center) const;
        [[nodiscard]] BrushExtents Extents() const;
        [[nodiscard]] uint32_t TemperatureMilliKelvin() const;
        void BuildShape();
        void BuildMaterial();
        void RunAutoStrokes(uint64_t frame, uint32_t& sequence, std::vector<sim::ProbeCommand>& commands);
        void RunAutoPaint(uint64_t frame, size_t waitingCommands, uint32_t& sequence,
                          std::vector<sim::ProbeCommand>& commands);
        void RunAutoUndo(uint64_t frame);
        void RunAutoSurface(uint64_t frame, size_t waitingCommands, std::span<const uint32_t> surface,
                            uint32_t& sequence, std::vector<sim::ProbeCommand>& commands);
        void BuildUndo();
        void BeginStroke();

        std::shared_ptr<const sim::BakedReactionTable> m_table;
        uint64_t m_tableVersion = 0;
        std::vector<sim::LabMaterial> m_materials;  // ひな形

        // --- 操作の状態(View。世界に入らない)---
        bool m_holding = false;
        ShapeKind m_shape = ShapeKind::Sphere;
        int m_radiusCells = 2;  // 球と円柱の半径・直方体の x の半幅
        int m_halfY = 2;        // 直方体の y の半幅
        int m_halfZ = 2;        // 直方体の z の半幅
        int m_halfLength = 2;   // 円柱の軸の向きの半幅
        int m_axis = 1;         // 円柱の軸(0 = x・1 = y〔縦〕・2 = z)
        size_t m_preset = 1;    // ひな形(m_materials の添字。初めは木)
        MaterialRows m_rows{};
        int m_temperatureKelvin = 300;
        bool m_replace = true;     // 置き換え(実験室の「置く」と同じ。D-439)か、足すか
        bool m_onSurface = false;  // 当て方: 断面(false)か面に沿って(T-0332)
        BrushStroke m_stroke;

        // --- 元に戻す ---
        std::vector<BrushStrokeSequences> m_strokes;  // 古い順(上限 MAX_UNDO_STROKES)
        bool m_undoRequested = false;
        std::string m_undoMessage;

        struct UndoCheck {
            uint64_t tick = 0;  // 戻す前に読み戻した最後の状態の刻み
            uint64_t hashBefore = 0;
            bool observed = false;  // 流し直して同じ刻みの状態が届いた
            bool changed = false;   // それが戻す前と違った
        };
        std::vector<UndoCheck> m_undoChecks;

        // --- 結果 ---
        std::vector<uint64_t> m_placedTicks;  // 置いたことが届いた刻み(巻き戻すと、戻した刻みから先を消す)
        uint32_t m_placedCount = 0;           // 届いた回数(巻き戻しで減らない)
        uint32_t m_paintedCount = 0;          // 投げた置くコマンドの数(ドラッグの分も)
        std::string m_lastPlaced;

        // --- 自動の確認 ---
        bool m_autoRunning = false;
        uint32_t m_autoExpected = 0;    // 投げた、当たるはずの置くコマンドの数
        uint32_t m_autoSent = 0;        // 投げた 1 回ずつの筆の運び(当たらない値も)
        uint32_t m_autoPaintSteps = 0;  // ドラッグの自動の手順を済ませた数
        uint32_t m_autoPainted = 0;     // ドラッグで投げた置くコマンドの数
        uint32_t m_autoUndoRequests = 0;
        uint32_t m_autoUndone = 0;          // 元に戻した運び
        uint32_t m_autoUndoneCommands = 0;  // 元に戻した運びの置くコマンドの数
        bool m_autoUndoFailed = false;
        uint32_t m_autoSurfacePainted = 0;  // 面に沿ってドラッグで投げた置くコマンドの数(T-0332)
        uint32_t m_autoSurfaceMatched = 0;  // 光線が木箱の壁の外に接するセルを拾った数
        bool m_autoSurfaceDone = false;
    };

}  // namespace bicameral::editor
