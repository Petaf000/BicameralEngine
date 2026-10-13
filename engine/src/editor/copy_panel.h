// copy_panel.h — エディタの「写しの早送り」のパネル(14 §2・D-470・T-0412・ADR-0198)。
//
// 今の世界の写しを作って別に早送りし、「何刻み(何秒)で条件に着くか」を答える。写しは世界に返さない(D-403)。
// データの流れ: パネルで条件を選んで「写しで早送り」→ TakeRequest(フレームのループが取る)→ frame/world_copy_runner が
// 本物の刻みの合間に sim::WorldCopy を回す → 進みと結果(CopyProgress)を EditorStatus で受けて出す。
// 中身は持たない(D-454): 条件は「最高温度がしきい値より下」「物質の量がしきい値以下」「上限まで」の形だけで、
// どの物質か・何 K かは作り手が選ぶ。写しにはコマンドを当てない(今の状態から何もしなければどうなるか)。
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "script/reaction_table_loader.h"

namespace bicameral::editor {

    // 止める条件(測る値もこれで決まる)
    enum class CopyCondition : uint8_t {
        Limit,             // 上限まで流す(測る値は最高温度)
        BelowTemperature,  // 最高温度がしきい値(mK)より下になったら止める(火が消えた の目安)
        SpeciesAtOrBelow,  // 物質の世界全体の量がしきい値(µmol)以下になったら止める
    };

    struct CopyRequest {
        uint64_t maxTicks = 0;              // 早送りの上限(刻み)
        uint32_t sampleIntervalTicks = 60;  // 測る間隔(刻み。セルの読み戻し)
        CopyCondition condition = CopyCondition::Limit;
        uint32_t temperatureMillikelvin = 0;  // BelowTemperature のしきい値
        uint32_t species = 0;                 // SpeciesAtOrBelow の物質 ID(今の表)
        uint64_t amountMicromoles = 0;        // SpeciesAtOrBelow のしきい値
    };

    // フレームのループが渡す進みと結果
    struct CopyProgress {
        bool running = false;
        bool finished = false;
        bool reached = false;  // 条件に着いた(finished の時)
        bool failed = false;
        uint64_t startTick = 0;
        uint64_t tick = 0;     // 写しの今の刻み(走っている時)
        uint64_t endTick = 0;  // 止めた刻み(finished の時)
        uint64_t maxTicks = 0;
        std::vector<std::pair<uint64_t, uint64_t>> samples;  // (刻み, 測った値。mK か µmol)
        bool measuresAmount = false;                         // samples が物質の量(でなければ最高温度)
        uint64_t vramBytes = 0;                              // 写しの VRAM(既定のヒープの主なもの)
        double tickGpuMicroseconds = 0.0;                    // 早送りの 1 刻みの GPU の時間
        uint64_t waitedFrames = 0;                           // 余りの予算が無くて進めなかったフレーム(D-468)
        std::string message;                                 // 失敗・待ちの理由
    };

    class CopyPanel {
    public:
        // 世界が今使っている表(物質の名前)
        void UseTable(std::shared_ptr<const script::LoadedReactionTable> table) { m_table = std::move(table); }

        // ImGui のフレームの中で呼ぶ
        void Build(const CopyProgress& progress, uint64_t ticksPerSecond);

        // 押されたボタンの依頼(無ければ空)
        [[nodiscard]] std::optional<CopyRequest> TakeRequest() { return std::exchange(m_request, std::nullopt); }
        [[nodiscard]] bool TakeCancel() { return std::exchange(m_cancel, false); }

    private:
        void BuildCondition();
        void BuildResult(const CopyProgress& progress, uint64_t ticksPerSecond) const;

        std::shared_ptr<const script::LoadedReactionTable> m_table;
        std::optional<CopyRequest> m_request;
        bool m_cancel = false;

        // --- 入力 ---
        int m_condition = 1;  // CopyCondition の番号(既定は「火が消えるまで」の目安 = 最高温度)
        int m_maxSeconds = 600;
        int m_intervalTicks = 60;
        int m_temperatureKelvin = 400;
        int m_speciesIndex = 0;  // 今の表の物質 ID - 1
        int m_amountMillimoles = 0;
    };

}  // namespace bicameral::editor
