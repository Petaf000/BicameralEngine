// world_copy_runner.cpp — 写しの早送りを本物の刻みの合間に回す(T-0412・ADR-0198)。約束は world_copy_runner.h。
#include "frame/world_copy_runner.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

#include "core/log.h"

namespace bicameral::frame {

    namespace {

        constexpr uint64_t MAX_TICKS_PER_FRAME = 64;  // 1 フレームの早送りの上限(写しも 1 リストの単位の上限で切る)
        constexpr double TICKS_PER_SECOND = 60.0;     // 刻みを秒に直す(06 §1)

    }  // namespace

    void WorldCopyRunner::Cancel() {
        m_request.reset();
        if (!m_progress.running)
            return;

        m_cancelled = true;
        m_progress.running = false;
        m_progress.message = "止めた";
    }

    void WorldCopyRunner::Update(gpu::Queue& compute, const sim::ProbeSim& world, SimCursor cursor,
                                 uint64_t tableVersion, double spareMilliseconds, uint64_t droppedTicks,
                                 double tickBudgetMilliseconds, const std::function<CreateCopy()>& makeCreate) {
        m_droppedTicks = droppedTicks;
        m_tickBudgetMilliseconds = tickBudgetMilliseconds;
        UpdateAuto(cursor);
        Collect(compute);
        if (m_copy && m_copy->Pending())
            return;  // GPU にある写しのリストは 1 組だけ

        if (m_request) {
            if (!Prepare(compute, tableVersion, makeCreate))
                return;

            // 写しを取るのは、本物の最後のリストが刻みの境界で終わっている時だけ(sim/world_copy.h の約束)
            if (cursor.unit != 0) {
                m_progress.message = "本物の刻みの境界を待っている";
                return;
            }

            Start(compute, world, cursor, droppedTicks);
            return;
        }

        if (m_copy && m_copy->Running() && !m_cancelled)
            Advance(compute, spareMilliseconds);
    }

    // --- 写しを作る・取る・進める ---

    // 写しを作り(表が替わっていたら作り直す。別のスレッドで)、初期化を投げる。写しを取れる状態なら true
    bool WorldCopyRunner::Prepare(gpu::Queue& compute, uint64_t tableVersion,
                                  const std::function<CreateCopy()>& makeCreate) {
        if (m_copy && m_copyTableVersion != tableVersion)
            m_copy.reset();  // GPU にリストは無い(Pending でない)

        if (!m_copy && !m_creating.valid()) {
            m_creating = std::async(std::launch::async, makeCreate());
            m_creatingTableVersion = tableVersion;
            m_progress.message = "写しの世界を作っている(パイプラインの作成)";
            return false;
        }

        if (!m_copy) {
            if (m_creating.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return false;

            auto created = m_creating.get();
            if (!created) {
                Log(Channel::Sim, Level::Error, "写しの早送り: {}", created.error());
                m_progress = {.failed = true, .message = "写しを作れない: " + created.error()};
                m_request.reset();
                if (m_auto == AutoStage::Running)
                    m_auto = AutoStage::Failed;

                return false;
            }

            m_copy.emplace(std::move(*created));
            m_copyTableVersion = m_creatingTableVersion;
            if (m_copyTableVersion != tableVersion) {
                m_copy.reset();  // 作っている間に表が替わった(次のフレームで作り直す)
                return false;
            }
        }

        if (m_copy->Initialized())
            return true;

        Submit(compute, m_copy->RecordInitialization());
        m_progress.message = "写しの世界を初期化している";

        return false;
    }

    void WorldCopyRunner::Start(gpu::Queue& compute, const sim::ProbeSim& world, SimCursor cursor,
                                uint64_t droppedTicks) {
        const editor::CopyRequest request = *std::exchange(m_request, std::nullopt);
        m_active = request;
        if (m_active.tickBudgetMilliseconds <= 0.0)
            m_active.tickBudgetMilliseconds = m_tickBudgetMilliseconds;

        m_samples->clear();
        m_cancelled = false;
        m_droppedAtStart = droppedTicks;

        const std::vector<ID3D12CommandList*> lists = m_copy->RecordStart(world, cursor.tick, MakeQuery(request));
        m_progress = {.running = !lists.empty(),
                      .failed = lists.empty(),
                      .startTick = cursor.tick,
                      .tick = cursor.tick,
                      .maxTicks = request.maxTicks,
                      .measuresAmount = request.condition == editor::CopyCondition::SpeciesAtOrBelow,
                      .vramBytes = m_copy->VramBytes(),
                      .message = lists.empty() ? "写しを取れない(理由はログ)" : ""};
        if (lists.empty() && m_auto == AutoStage::Running)
            m_auto = AutoStage::Failed;

        Submit(compute, lists);
    }

    // 余りの予算に入る刻みの数だけ進める(測るまでは 1 刻み。余りが無ければ進めない。D-468)
    void WorldCopyRunner::Advance(gpu::Queue& compute, double spareMilliseconds) {
        const sim::WorldCopyResult& result = m_copy->Result();
        uint64_t ticks = spareMilliseconds > 0.0 ? 1 : 0;
        if (result.ticksRun > 0 && result.gpuTimestampTicks > 0) {
            const double tickMilliseconds = static_cast<double>(result.gpuTimestampTicks) * 1000.0 /
                                            static_cast<double>(compute.TimestampFrequency()) /
                                            static_cast<double>(result.ticksRun);
            ticks = static_cast<uint64_t>(spareMilliseconds / tickMilliseconds);
        }

        ticks = std::min(ticks, MAX_TICKS_PER_FRAME);
        if (ticks == 0) {
            ++m_progress.waitedFrames;
            return;
        }

        Submit(compute, m_copy->RecordNext(ticks));
    }

    void WorldCopyRunner::Submit(gpu::Queue& compute, const std::vector<ID3D12CommandList*>& lists) {
        if (!lists.empty())
            m_fence = compute.Submit(lists);
    }

    // --- 終えたリストを読む ---

    void WorldCopyRunner::Collect(gpu::Queue& compute) {
        if (!m_copy || !m_copy->Pending() || !compute.IsComplete(m_fence))
            return;

        m_copy->Complete();
        if (!m_cancelled)
            RefreshProgress(compute);
    }

    void WorldCopyRunner::RefreshProgress(const gpu::Queue& compute) {
        const sim::WorldCopyResult& result = m_copy->Result();
        if (!m_progress.running)
            return;  // 初期化のリスト

        m_progress.tick = m_copy->Tick();
        m_progress.samples = *m_samples;
        if (result.ticksRun > 0) {
            m_progress.tickGpuMicroseconds = static_cast<double>(result.gpuTimestampTicks) * 1'000'000.0 /
                                             static_cast<double>(compute.TimestampFrequency()) /
                                             static_cast<double>(result.ticksRun);
        }

        if (!m_copy->Finished())
            return;

        m_progress.running = false;
        m_progress.finished = true;
        m_progress.reached = result.reached;
        m_progress.failed = result.failed;
        m_progress.endTick = result.endTick;
        if (!result.failed)
            Weigh(compute);

        m_progress.message = result.failed ? "早送りが途中で失敗した(理由はログ)"
                                           : std::format("写しの間に本物が追いつけず捨てた刻み {}(D-468)",
                                                         m_droppedTicks - m_droppedAtStart);
        Log(Channel::Sim, Level::Info, "写しの早送り: {} 刻み・{} リスト・1 刻み {:.1f} µs・待ったフレーム {}・{}",
            result.ticksRun, result.frames, m_progress.tickGpuMicroseconds, m_progress.waitedFrames,
            m_progress.message);

        if (m_auto != AutoStage::Running)
            return;

        for (const sim::ProbeTickHash& hash : result.hashes) {
            m_copyHashes[hash.tick] = hash.WorldHash();
            m_copyBlocks[hash.tick] = hash.scheduledBlocks;
        }

        m_auto = result.failed || m_copyHashes.empty() ? AutoStage::Failed : AutoStage::Comparing;
        if (m_auto == AutoStage::Failed)
            m_autoSummary = "写しの早送りが終わらなかった";
    }

    // 重さの検査(T-0413・ADR-0204): 刻みごとの GPU の時間と活性のブロック数を、予算と演出のスローの窓で判定する
    void WorldCopyRunner::Weigh(const gpu::Queue& compute) {
        const sim::WorldCopyResult& result = m_copy->Result();
        const auto frequency = static_cast<double>(compute.TimestampFrequency());
        std::vector<WeightTick> ticks;
        ticks.reserve(result.hashes.size());
        for (size_t index = 0; index < result.hashes.size() && index < result.tickGpuTimestampTicks.size(); ++index) {
            const uint64_t gpuTicks = result.tickGpuTimestampTicks[index];
            if (gpuTicks == 0)
                continue;  // 数えられなかった刻み

            ticks.push_back({.tick = result.hashes[index].tick,
                             .gpuMilliseconds = static_cast<double>(gpuTicks) * 1000.0 / frequency,
                             .scheduledBlocks = result.hashes[index].scheduledBlocks});
        }

        // スローの窓: 写しの始めから slowOffsetTicks 刻み目の刻みから(WeightTick::tick は作った状態の刻みなので + 1)
        WeightBudget budget{.tickMilliseconds = m_active.tickBudgetMilliseconds, .cautionRatio = m_active.cautionRatio};
        if (m_active.slowTicks > 0) {
            budget.slows.push_back({.firstTick = result.startTick + m_active.slowOffsetTicks + 1,
                                    .tickCount = m_active.slowTicks,
                                    .speedScale = m_active.slowSpeedScale});
        }

        m_progress.weight = CheckWeight(ticks, budget);
        m_progress.weighed = true;
        m_progress.tickBudgetMilliseconds = budget.tickMilliseconds;

        // 表示の線は 512 点まで(間引く時は区間の最大。重い刻みを見落とさない)
        constexpr size_t PLOT_POINTS = 512;
        const size_t bucket = std::max<size_t>(1, (ticks.size() + PLOT_POINTS - 1) / PLOT_POINTS);
        m_progress.tickMilliseconds.clear();
        for (size_t first = 0; first < ticks.size(); first += bucket) {
            double peak = 0.0;
            for (size_t index = first; index < std::min(first + bucket, ticks.size()); ++index)
                peak = std::max(peak, ticks[index].gpuMilliseconds);

            m_progress.tickMilliseconds.push_back(static_cast<float>(peak));
        }

        const WeightReport& report = m_progress.weight;
        const bool over = report.verdict == WeightVerdict::Over;
        Log(Channel::Sim, over ? Level::Warning : Level::Info, "重さの検査(1 刻みの予算 {:.2f} ms): {}{}",
            budget.tickMilliseconds, DescribeWeight(report, TICKS_PER_SECOND),
            over ? "。予算を超える刻みは直すべき不具合(D-468)" : "");
    }

    sim::WorldCopyQuery WorldCopyRunner::MakeQuery(const editor::CopyRequest& request) {
        sim::WorldCopyQuery query{.maxTicks = request.maxTicks, .sampleIntervalTicks = request.sampleIntervalTicks};
        query.until = [samples = m_samples, request](const sim::WorldCopySample& sample) {
            if (sample.cells.empty())
                return false;

            const bool amount = request.condition == editor::CopyCondition::SpeciesAtOrBelow;
            const uint64_t value = amount ? sim::ProbeSpeciesAmount(sample.cells, request.species)
                                          : sim::ProbeMaxTemperature(sample.thermal);
            samples->emplace_back(sample.tick, value);
            switch (request.condition) {
                case editor::CopyCondition::BelowTemperature: return value < request.temperatureMillikelvin;
                case editor::CopyCondition::SpeciesAtOrBelow: return value <= request.amountMicromoles;
                case editor::CopyCondition::Limit: return false;
            }

            return false;
        };

        return query;
    }

    // --- 人がいない確かめ(--auto-copy)---

    void WorldCopyRunner::NoteWorldHash(const sim::ProbeTickHash& hash) {
        if (m_auto == AutoStage::Off || hash.tick <= AUTO_START_TICK)
            return;

        m_worldHashes[hash.tick] = hash.WorldHash();
        m_worldBlocks[hash.tick] = hash.scheduledBlocks;
    }

    void WorldCopyRunner::UpdateAuto(SimCursor cursor) {
        if (m_auto == AutoStage::Waiting && cursor.tick >= AUTO_START_TICK) {
            m_request = editor::CopyRequest{.maxTicks = AUTO_TICKS, .sampleIntervalTicks = AUTO_INTERVAL_TICKS};
            m_auto = AutoStage::Running;
        }

        // 写しの最後の刻みまで本物が読み戻したら比べる
        if (m_auto != AutoStage::Comparing || m_worldHashes.empty() ||
            m_worldHashes.rbegin()->first < m_copyHashes.rbegin()->first)
            return;

        uint32_t different = 0;
        uint32_t missing = 0;
        uint32_t blocksDifferent = 0;
        for (const auto& [tick, hash] : m_copyHashes) {
            const auto found = m_worldHashes.find(tick);
            if (found == m_worldHashes.end())
                ++missing;
            else if (found->second != hash)
                ++different;

            const auto blocks = m_worldBlocks.find(tick);
            if (blocks != m_worldBlocks.end() && blocks->second != m_copyBlocks[tick])
                ++blocksDifferent;
        }

        // 重さを全部の刻みで測れた(活性のブロック数は決定的なので、本物と同じ刻みで同じ数)
        const WeightReport& weight = m_progress.weight;
        const bool weighed = m_progress.weighed && weight.ticks == m_copyHashes.size();
        m_auto = different == 0 && missing == 0 && blocksDifferent == 0 && weighed ? AutoStage::Passed
                                                                                   : AutoStage::Failed;
        m_autoSummary = std::format(
            "写しの刻み {}〜{} の {} 刻みが本物と同じハッシュ(違う {}・本物に無い {})・活性のブロック数が違う刻み {}・"
            "重さを測った刻み {} / {}・1 刻み {:.1f} µs・VRAM {} MiB・待ったフレーム {}・"
            "写しの間に本物が捨てた刻み {}。{}",
            m_copyHashes.begin()->first, m_copyHashes.rbegin()->first, m_copyHashes.size() - different - missing,
            different, missing, blocksDifferent, weight.ticks, m_copyHashes.size(), m_progress.tickGpuMicroseconds,
            m_progress.vramBytes >> 20, m_progress.waitedFrames, m_droppedTicks - m_droppedAtStart,
            DescribeWeight(weight, TICKS_PER_SECOND));
        Log(Channel::Sim, m_auto == AutoStage::Passed ? Level::Info : Level::Error, "--auto-copy: {}", m_autoSummary);
    }

}  // namespace bicameral::frame
