// lab_panel.h — エディタの「実験室」のパネル(14 §2・T-0142・ADR-0037)。小さな箱(sim/lab_box。8³ セル・4 m 角・300 K の空気)に
// 物質と温度を置いて反応を試し、GPU と CPU リファレンスを刻みごとに並べて比べる。食い違ったら最初の刻みとセルを出す。
//
// データの流れ:
//   パネルの操作(置く・温度・刻む・戻す)→ sim::LabSession(置く操作はコマンド。箱のセルを CPU から書かない。D-107)
//   → 読み戻した GPU の箱と CPU リファレンスの箱 → パネルに断面の温度・選んだセルの成分を並べて出す。
//   記録の保存・読み込み(コマンドの列 + ハッシュの列 + 使った表の中身。sim::SerializeLabRecording)→ 再生で同じ実験を流し直す。
//   記録の表をこの箱が持っていなければ、中身から作り直して(script::RebuildReactionTable)足す(別の起動の記録。T-0217)。
// 箱は仮の世界(ProbeSim)とは別の世界で、自分の compute キューで 1 刻みずつ待って進める(エディタの道具。ADR-0001)。
// GPU の箱は「箱を作る」を押した時に作る(debug では多重解像度のパイプラインの作成に時間がかかるので、使わない時は作らない)。
// 反応表は世界と同じ表(フレームのループが毎フレーム UseTable で渡す。T-0172・T-0194)。ホットリロードで世界の表が替わったら、
// 次の刻みから箱も新しい表で続ける(sim::LabSession::ChangeTable。T-0218・ADR-0055)。「最新の表で初めから」は今までの操作を
// 初めの箱から新しい表で同じ刻みまで流し直す(RerunWithLatestTable。T-0194 の案 A)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "script/reaction_table_loader.h"
#include "sim/lab_session.h"

namespace bicameral::editor {

    class LabPanel {
    public:
        explicit LabPanel(ID3D12Device* device);

        // 世界が今使っている反応表(版が変わった時だけ箱の表を替える。中身〔tableBytes〕は記録に残す)。Build の前に呼ぶ
        void UseTable(std::shared_ptr<const script::LoadedReactionTable> loaded);

        // ImGui のフレームの中で呼ぶ
        void Build();

        // --- 人がいない確認(--auto-lab): 最初の Build で、木を置いて火を付ける実験 → 記録 → 再生を流して確かめる ---
        void StartAuto() { m_autoPending = true; }
        [[nodiscard]] bool AutoFailed() const { return m_autoFailed; }

    private:
        bool CreateSession();
        void RunAuto();
        [[nodiscard]] std::expected<void, std::string> ReplayInNewSession(const sim::LabRecording& recorded);

        void BuildControls();
        void BuildStatus();
        void BuildSlice();
        void BuildCell() const;
        void BuildRecording();
        void Report(const std::expected<void, std::string>& result);

        ComPtr<ID3D12Device5> m_device;
        std::shared_ptr<const script::LoadedReactionTable> m_loaded;  // 世界と同じ表と中身(UseTable)
        std::shared_ptr<const sim::BakedReactionTable> m_table;       // その表(m_loaded の中を指す)
        uint64_t m_tableVersion = 0;
        uint32_t m_tableChanges = 0;  // 箱の表を替えた回数(表示用)
        std::optional<sim::LabSession> m_session;
        std::vector<sim::LabMaterial> m_materials;
        std::string m_error;    // 最後の失敗(GPU・ファイル)
        std::string m_message;  // 最後の操作の結果

        // --- 操作の状態(View。世界に入らない)---
        std::array<int, 3> m_cell = {3, 3, 3};
        int m_material = 1;
        int m_temperatureKelvin = 300;
        bool m_running = false;  // 毎フレーム 1 刻み
        std::string m_recordingPath = "lab_recording.blab";

        bool m_autoPending = false;
        bool m_autoFailed = false;
    };

}  // namespace bicameral::editor
