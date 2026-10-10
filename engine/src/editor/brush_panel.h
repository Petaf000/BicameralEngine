// brush_panel.h — 世界に物を置く筆のパネル(T-0222・D-449。docs/design/14 §2「実装(T-0222)」)。
//
// データの流れ:
//   世界が今使っている反応表(UseTable)→ 筆の材料の一覧(sim::MakeLabMaterials。表にある物質だけ)
//   → パネルで材料・大きさ(半径)・量(%)・温度・置き換えか足すかを選び、「筆を持つ」
//   → 窓の左クリックが断面に当たったセル(フレームのループ)→ MakeCommand が置くコマンド(sim::MakePlaceCommand)にする
//   → フレームのループが刻みを付けて GPU のキューへ・再生ファイルへ(つつきと同じ道)。世界は GPU だけが変える(D-107)。
// パネルの選んだ値は View の状態で、世界にも再生ファイルにも入らない(入るのはコマンドだけ)。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sim/lab_box.h"
#include "sim/probe_sim.h"

namespace bicameral::editor {

    class BrushPanel {
    public:
        // 世界が今使っている反応表(版が変わった時だけ材料の一覧を作り直す)。Build の前に呼ぶ
        void UseTable(std::shared_ptr<const sim::BakedReactionTable> table, uint64_t version);

        // ImGui のフレームの中で呼ぶ
        void Build();

        // 筆を持っている(左クリックがつつきでなく「置く」になる)
        [[nodiscard]] bool Holding() const { return m_holding && !m_materials.empty(); }

        // セル (x, y, z) を中心に今の筆で置くコマンド(targetTick は 0。フレームのループが付ける)
        [[nodiscard]] sim::ProbeCommand MakeCommand(uint32_t x, uint32_t y, uint32_t z, uint32_t sequence) const;

        // 置いたことが GPU から戻った(PROBE_EVENT_PLACE_APPLIED)
        void NotePlaced(const sim::ProbeEvent& event);

        // --- 人がいない確認(--auto-place): 決まったフレームに筆で置く・足す・はみ出す・当たらない値を投げ、火を付ける ---
        void StartAuto() { m_autoRunning = true; }

        // フレーム frame に投げる自動のコマンド(sequence は通しで増やす)
        [[nodiscard]] std::vector<sim::ProbeCommand> TakeAutoCommands(uint64_t frame, uint32_t& sequence);

        // 当てられるはずの置くコマンドが全部 GPU で当たった(当たらない値のコマンドは当たっていない)
        [[nodiscard]] bool AutoPassed() const;
        [[nodiscard]] std::string AutoSummary() const;

    private:
        [[nodiscard]] const sim::LabMaterial* FindMaterial(std::string_view name) const;
        [[nodiscard]] sim::ProbeCommand MakeCommandWith(const sim::LabMaterial& material,
                                                        const sim::ProbePlaceShape& shape, uint32_t amountPercent,
                                                        uint32_t sequence) const;

        std::shared_ptr<const sim::BakedReactionTable> m_table;
        uint64_t m_tableVersion = 0;
        std::vector<sim::LabMaterial> m_materials;

        // --- 操作の状態(View。世界に入らない)---
        bool m_holding = false;
        std::string m_materialName = "木";  // 表が替わっても同じ名前の材料を選び続ける
        int m_radiusCells = 2;
        int m_amountPercent = 100;  // 材料の 1 セルあたりの量の何 %
        int m_temperatureKelvin = 300;
        bool m_replace = true;  // 置き換え(実験室の「置く」と同じ。D-439)か、足すか

        // --- 結果 ---
        uint32_t m_placedCount = 0;
        std::string m_lastPlaced;

        // --- 自動の確認 ---
        bool m_autoRunning = false;
        uint32_t m_autoExpected = 0;  // 投げた、当たるはずの置くコマンドの数
        uint32_t m_autoSent = 0;      // 投げた全部(当たらない値も)
    };

}  // namespace bicameral::editor
