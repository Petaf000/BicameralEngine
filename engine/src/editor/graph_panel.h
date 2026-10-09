// graph_panel.h — エディタの「Work Graphs と性能」のパネル(T-0143、docs/design/14 §2・16 §1)。
//
// データの流れ: シミュのリストの待たない読み戻し(sim/probe_sim の ProbeFrameReadback: 単位ごとのタイムスタンプ・伝導のグラフのノードのカウンタ)
//   → フレームのループが 1 秒ずつ足す(frame_loop の Stats)→ GraphPanelStatus に詰める → BuildGraphPanel が表にする(読むだけ)。
// Work Graphs はノードごとの時間を測れない(タイムスタンプは DispatchGraph の前後にしか打てない)ので、時間は単位(= DispatchGraph 1 回)ごと、
// ノードごとは起動・レコードの数と上限の余裕を出す(16 §1・T-0008 のカウンタ)。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/work_graph_stats.h"

namespace bicameral::editor {

    // 刻みの中の単位 1 種の GPU 時間
    struct UnitTime {
        std::string name;
        bool workGraph = false;            // Work Graph の単位(DispatchGraph)か
        double millisecondsPerTick = 0.0;  // 1 刻みあたり(直近 1 秒の平均)
    };

    struct GraphPanelStatus {
        const gpu::GraphStatsLayout* layout = nullptr;  // 伝導のグラフのノードと計器の名前・上限(ProbeSim が持つ)
        gpu::GraphStatsSnapshot stats;                  // 直近 1 秒の合計(数は足し、最大は最大)
        uint64_t ticks = 0;                             // その間に終えた刻み(1 刻みあたりにするため)
        std::vector<UnitTime> units;                    // 刻みの中の順
    };

    // ImGui の窓「Work Graphs と性能」を作る(EditorOverlay::Build の中で呼ぶ)
    void BuildGraphPanel(const GraphPanelStatus& status);

}  // namespace bicameral::editor
