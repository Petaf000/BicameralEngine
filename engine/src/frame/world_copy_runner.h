// world_copy_runner.h — エディタの「写しの早送り」を本物の刻みの合間に回す(T-0412・ADR-0198。D-468・D-470。14 §2)。
//
// フレームのループが毎フレーム、本物のシミュのリストを投げた後に Update を呼ぶ。写しのリスト(sim::WorldCopy)は本物と同じ
// compute キューの、本物のリストの後ろへ投げる。写しを取るのは本物のカーソルが刻みの境界にある時だけ(本物の最後のリストが
// 境界で終わっている = その刻みの状態 S(t) がそろっている。sim/world_copy.h の約束)。
// 早送りの量は、そのフレームのシミュの予算の余り(SimScheduler::SpareMilliseconds)に入る刻みの数だけ(D-468: 写しのせいで
// 本物を遅らせない。余りが無いフレームは進めず、待ったフレームを数える)。測ったことのない間は 1 刻みずつ。
// 写しは 1 つだけ(仮。T-0412 の判断待ち)。作るのは最初の依頼の時(それまで VRAM を使わない)。表を差し替えた後の依頼は作り直す。
// 作る(パイプラインの作成。キャッシュに当たらないと 10 秒を超える)のは別のスレッドで、フレームのループを止めない(D-468)。
// 写しにはコマンドを当てない(今の状態から何もしなければどうなるか)。中身(何を測るか)は依頼の条件(editor::CopyRequest)で決まる。
// 重さの検査(T-0413・ADR-0204): 終えたら刻みごとの GPU の時間と活性のブロック数を予算(依頼の値か、この機械の今の予算)と
// 演出のスローの窓で判定する(frame/weight_check)。予算を超えた刻みは直すべき不具合として警告のログ(D-468)。
//
// 人がいない確かめ(--auto-copy): 本物が刻み AUTO_START_TICK に着いたら上限 AUTO_TICKS 刻みの写しを取り、終わったら写しの
// 刻みごとの世界のハッシュが、本物が同じ刻みに読み戻したハッシュと全部同じかを比べる(本物にコマンドを足さない起動で。
// 写しは本物と同じ結果になる・写しを取っても本物は変わらないことは、--record と別の起動の --replay で確かめる)。
// 重さの集め方が決定的なこと: 写しの刻みごとの活性のブロック数が本物の同じ刻みと全部同じで、重さを全部の刻みで測れたこと(T-0413)。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "editor/copy_panel.h"
#include "frame/sim_scheduler.h"
#include "frame/weight_check.h"
#include "gpu/queue.h"
#include "sim/world_copy.h"

namespace bicameral::frame {

    class WorldCopyRunner {
    public:
        // 写しを作る関数(別のスレッドで呼ぶ。フレームのループのデータは作る時に写して持つ)
        using CreateCopy = std::function<std::expected<sim::WorldCopy, std::string>()>;

        static constexpr uint64_t AUTO_START_TICK = 60;
        static constexpr uint64_t AUTO_TICKS = 600;
        static constexpr uint32_t AUTO_INTERVAL_TICKS = 60;

        // 次の Update で写しを取って早送りを始める(走っていれば止めてから)
        void Request(const editor::CopyRequest& request) { m_request = request; }
        void Cancel();

        // 本物のシミュのリストを投げた後に呼ぶ。cursor = 本物の次に投げる単位・droppedTicks = 本物が追いつけず捨てた刻みの累計・
        // tickBudgetMilliseconds = この機械の今の等速の 1 刻みの予算(依頼に予算が無い時の重さの検査に使う)
        // makeCreate は写しを作る時だけ呼ぶ(今の表などを写した CreateCopy を返す)
        void Update(gpu::Queue& compute, const sim::ProbeSim& world, SimCursor cursor, uint64_t tableVersion,
                    double spareMilliseconds, uint64_t droppedTicks, double tickBudgetMilliseconds,
                    const std::function<CreateCopy()>& makeCreate);

        [[nodiscard]] const editor::CopyProgress& Progress() const { return m_progress; }

        // --- 人がいない確かめ(--auto-copy)---
        void StartAuto() { m_auto = AutoStage::Waiting; }
        void NoteWorldHash(const sim::ProbeTickHash& hash);  // 本物の読み戻した刻みの要約
        [[nodiscard]] bool AutoPassed() const { return m_auto == AutoStage::Passed; }
        [[nodiscard]] bool AutoFinished() const { return m_auto == AutoStage::Passed || m_auto == AutoStage::Failed; }
        [[nodiscard]] const std::string& AutoSummary() const { return m_autoSummary; }

    private:
        enum class AutoStage : uint8_t { Off, Waiting, Running, Comparing, Passed, Failed };

        void Collect(gpu::Queue& compute);
        [[nodiscard]] bool Prepare(gpu::Queue& compute, uint64_t tableVersion,
                                   const std::function<CreateCopy()>& makeCreate);
        void Start(gpu::Queue& compute, const sim::ProbeSim& world, SimCursor cursor, uint64_t droppedTicks);
        void Advance(gpu::Queue& compute, double spareMilliseconds);
        void Submit(gpu::Queue& compute, const std::vector<ID3D12CommandList*>& lists);
        void RefreshProgress(const gpu::Queue& compute);
        void Weigh(const gpu::Queue& compute);
        void UpdateAuto(SimCursor cursor);
        [[nodiscard]] sim::WorldCopyQuery MakeQuery(const editor::CopyRequest& request);

        std::optional<sim::WorldCopy> m_copy;
        uint64_t m_copyTableVersion = 0;
        std::future<std::expected<sim::WorldCopy, std::string>> m_creating;  // 別のスレッドで作っている写し
        uint64_t m_creatingTableVersion = 0;
        uint64_t m_fence = 0;
        bool m_cancelled = false;
        std::optional<editor::CopyRequest> m_request;
        editor::CopyRequest m_active;  // 走っている問いの依頼(重さの検査の予算は始めた時に埋める)
        double m_tickBudgetMilliseconds = 0.0;
        editor::CopyProgress m_progress;
        // 条件が測った値(刻み, 値)。写しの条件の関数が書く(フレームのループの外から持つので共有)
        std::shared_ptr<std::vector<std::pair<uint64_t, uint64_t>>>
            m_samples = std::make_shared<std::vector<std::pair<uint64_t, uint64_t>>>();
        uint64_t m_droppedAtStart = 0;
        uint64_t m_droppedTicks = 0;

        // --- 自動の確かめ ---
        AutoStage m_auto = AutoStage::Off;
        std::map<uint64_t, uint64_t> m_worldHashes;  // 本物(AUTO_START_TICK より後)
        std::map<uint64_t, uint64_t> m_copyHashes;
        std::map<uint64_t, uint32_t> m_worldBlocks;  // 本物の刻みごとの活性のブロック数
        std::map<uint64_t, uint32_t> m_copyBlocks;
        std::string m_autoSummary;
    };

}  // namespace bicameral::frame
