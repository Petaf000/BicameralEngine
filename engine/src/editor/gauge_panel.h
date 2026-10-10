// gauge_panel.h — エディタの「計器と比べる」パネル(14 §2・T-0221・D-449)。実験室(editor/lab_panel)の箱について、
// 見るセルの温度・物質量・反応の速さ(物質量の刻みごとの差)と、箱全体の物質量・最高温度を時間のグラフで見る。
// 同じ保存点(今の実験の記録を刻み t で切ったもの)から、A = 元の実験の続き・B = 条件を 1 つ足したもの、を並べて流し、
// 同じ刻みの値を重ねて描く。B に足す条件は「実験室」のパネルで今組んでいる「置く」か「温度だけ」(範囲も同じ)。
//
// データの流れ:
//   sim::LabSession の計器(刻むたびに読み戻した GPU の箱から sim::SampleLabGauge。世界に何も返さない。ADR-0035 の「見るだけ」)
//   → ここで浮動小数点に直して描く(表示だけ。sim は整数のまま)。
//   比べる: sim::MakeLabComparisonPlan → sim::RunLabComparison(同じ箱で A → B を初めの箱から流し直す)→ 2 本の列を重ねて描く。
//   比べた後の箱は B の状態。「元の実験に戻す」で比べる前の記録を流し直す。
// --auto-lab-compare(--editor と一緒に): 木を燃やす実験 → グラフの値が CPU リファレンスだけで流した値と一致 →
//   刻み 20 を保存点に隣へ冷たい木を足した B と比べ、A が元の実験と同じ・B が刻み 20 から違う・B も CPU と一致、を確かめる。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>

#include "sim/lab_comparison.h"

namespace bicameral::editor {

    class LabPanel;

    // グラフにする量
    enum class GaugeQuantity : uint8_t {
        CellTemperature,  // 見るセルの温度(K)
        MaxTemperature,   // 箱の最高温度(K)
        CellAmount,       // 見るセルの物質量(mol)
        BoxAmount,        // 箱全体の物質量(mol)
        CellRate,         // 見るセルの反応の速さ(物質の増え方。mol/s)
        BoxRate,          // 箱全体の反応の速さ(mol/s)
    };

    class GaugePanel {
    public:
        // ImGui のフレームの中で、実験室のパネルの後に呼ぶ(実験室の箱が無ければ案内だけ)
        void Build(LabPanel& lab);

        // --- 人がいない確認(--auto-lab-compare)---
        void StartAuto() { m_autoPending = true; }
        [[nodiscard]] bool AutoPassed() const { return m_autoPassed; }
        [[nodiscard]] const std::string& AutoSummary() const { return m_autoSummary; }

    private:
        void BuildGauge(sim::LabSession& session);
        void BuildQuantityControls(const sim::BakedReactionTable& table);
        void BuildCompare(LabPanel& lab, sim::LabSession& session);
        void BuildComparisonGraph();
        void RunComparison(LabPanel& lab, sim::LabSession& session);
        void RunAuto(LabPanel& lab);
        [[nodiscard]] std::expected<std::string, std::string> AutoCompare(LabPanel& lab);

        // --- 見るもの(View。世界に入らない)---
        GaugeQuantity m_quantity = GaugeQuantity::CellTemperature;
        int m_species = 1;                      // 物質量・速さの物質 ID
        std::array<int, 3> m_cell = {3, 3, 3};  // 見るセル
        int m_windowTicks = 600;                // グラフに出す直近の刻みの数

        // --- 比べる ---
        int m_savePointTick = 0;
        int m_compareTicks = 120;
        bool m_changeTemperatureOnly = false;
        std::optional<sim::LabComparison> m_comparison;
        std::optional<sim::LabRecording> m_original;  // 比べる前の実験(「元の実験に戻す」)
        std::string m_error;
        std::string m_message;

        bool m_autoPending = false;
        bool m_autoPassed = false;
        std::string m_autoSummary;
    };

}  // namespace bicameral::editor
