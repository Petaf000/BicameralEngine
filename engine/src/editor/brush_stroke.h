// brush_stroke.h — 筆をドラッグして塗る時に、どこへ置くか(置く中心の列)を決める(T-0245・ADR-0089。docs/design/14 §2)。
//
// データの流れ:
//   窓の左ボタンを押した・押したまま動いた・離したセル(render/debug_view_controller の TakeDrag)
//   → BrushStroke::Paint が、前に置いた中心から今のセルまでを「間隔」ごとに区切った中心の列にする(1 フレームに置ける数まで)
//   → 筆のパネル(editor/brush_panel)が中心ごとに置くコマンドを作る → フレームのループが刻みを付けて GPU のキューと再生ファイルへ。
// 置く場所を決めるのは CPU の Controller(D-107)で、決めた結果はコマンドとして記録されるので、再生は記録だけで同じ世界になる
// (ここは再生の時に動かない)。GPU も D3D12 も ImGui も知らない(CPU だけのテスト brush_stroke_test)。
//
// 間隔(同じセルに重ねない): 進む向きのいちばん大きく動く軸 a について、
//   足す   = 2 × 半幅[a] + 1(形を囲む箱がその軸で重ならない。同じセルに 2 度足さない)
//   置き換え = max(1, 半幅[a])(重なってもよい〔同じ材料で置き換えるだけ〕ので、隙間が出ない詰め方)
// 1 フレームに置く数の上限 MAX_DABS_PER_FRAME と、GPU のキューで待つコマンドの上限 MAX_WAITING_COMMANDS(止めている間も 1 刻みに
// 溜まり過ぎない)を超える分は、次のフレームに続きから置く(目標のセルは覚えておく)。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bicameral::editor {

    // 格子のセル(x, y, z)
    using BrushCell = std::array<uint32_t, 3>;

    // 形を囲む箱の、中心から各軸の端のセルまでの半幅(common/probe_place.hlsli の extentX・Y・Z と同じ)
    using BrushExtents = std::array<uint32_t, 3>;

    class BrushStroke {
    public:
        static constexpr uint32_t MAX_DABS_PER_FRAME = 8;
        static constexpr size_t MAX_WAITING_COMMANDS = 64;

        // このフレームに置ける数(waitingCommands = GPU のキューとフレームのループで待っているコマンドの数)
        [[nodiscard]] static uint32_t Budget(size_t waitingCommands);

        // 2 つの中心の間の間隔(進む向き = from → to のいちばん大きく動く軸で決める)
        [[nodiscard]] static uint32_t Spacing(const BrushCell& from, const BrushCell& to, const BrushExtents& extents,
                                              bool add);

        // 1 フレームの入力を置く中心の列にする。pressed = 押したセル(筆の運びを始める。最初の 1 つはそこに置く)・
        // dragged = 押したまま通ったセル(古い順)・released = 離した(目標まで置き終えたら終わる)
        [[nodiscard]] std::vector<BrushCell> Paint(std::span<const BrushCell> pressed,
                                                   std::span<const BrushCell> dragged, bool released,
                                                   const BrushExtents& extents, bool add, uint32_t budget);

        [[nodiscard]] bool Active() const { return m_active; }

    private:
        void WalkToTarget(const BrushExtents& extents, bool add, uint32_t& budget, std::vector<BrushCell>& dabs);
        [[nodiscard]] bool TargetReached(const BrushExtents& extents, bool add) const;

        bool m_active = false;    // 筆の運びの途中(押してから、離して目標まで置き終えるまで)
        bool m_released = false;  // 離した(目標まで置いたら終わる)
        BrushCell m_last{};       // 最後に置いた中心
        BrushCell m_target{};     // 置きに行く先(最後に通ったセル)
    };

}  // namespace bicameral::editor
