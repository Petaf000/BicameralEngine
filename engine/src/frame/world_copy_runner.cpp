// world_copy_runner.cpp — 写しの早送りを本物の刻みの合間に回す(T-0412・ADR-0198)。約束は world_copy_runner.h。
#include "frame/world_copy_runner.h"

#include <algorithm>
#include <format>
#include <utility>

#include "core/log.h"

namespace bicameral::frame {

    namespace {

        constexpr uint64_t MAX_TICKS_PER_FRAME = 64;  // 1 フレームの早送りの上限(写しも 1 リストの単位の上限で切る)

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
                                 const CreateCopy& create) {
        m_droppedTicks = droppedTicks;
        UpdateAuto(cursor);
        Collect(compute);
        if (m_copy && m_copy->Pending())
            return;  // GPU にある写しのリストは 1 組だけ

        if (m_request) {
            if (!Prepare(compute, tableVersion, create))
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

    // 写しを作り(表が替わっていたら作り直す)、初期化を投げる。写しを取れる状態なら true
    bool WorldCopyRunner::Prepare(gpu::Queue& compute, uint64_t tableVersion, const CreateCopy& create) {
        if (m_copy && m_copyTableVersion != tableVersion)
            m_copy.reset();  // GPU にリストは無い(Pending でない)

        if (!m_copy) {
            auto created = create();
            if (!created) {
                Log(Channel::Sim, Level::Error, "写しの早送り: {}", created.error());
                m_progress = {.failed = true, .message = "写しを作れない: " + created.error()};
                m_request.reset();
                if (m_auto == AutoStage::Running)
                    m_auto = AutoStage::Failed;

                return false;
            }

            m_copy.emplace(std::move(*created));
            m_copyTableVersion = tableVersion;
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
        m_progress.message = result.failed ? "早送りが途中で失敗した(理由はログ)"
                                           : std::format("写しの間に本物が追いつけず捨てた刻み {}(D-468)",
                                                         m_droppedTicks - m_droppedAtStart);
        Log(Channel::Sim, Level::Info, "写しの早送り: {} 刻み・{} リスト・1 刻み {:.1f} µs・待ったフレーム {}・{}",
            result.ticksRun, result.frames, m_progress.tickGpuMicroseconds, m_progress.waitedFrames,
            m_progress.message);

        if (m_auto != AutoStage::Running)
            return;

        for (const sim::ProbeTickHash& hash : result.hashes)
            m_copyHashes[hash.tick] = hash.WorldHash();

        m_auto = result.failed || m_copyHashes.empty() ? AutoStage::Failed : AutoStage::Comparing;
        if (m_auto == AutoStage::Failed)
            m_autoSummary = "写しの早送りが終わらなかった";
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

    void WorldCopyRunner::NoteWorldHash(uint64_t tick, uint64_t worldHash) {
        if (m_auto != AutoStage::Off && tick > AUTO_START_TICK)
            m_worldHashes[tick] = worldHash;
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
        for (const auto& [tick, hash] : m_copyHashes) {
            const auto found = m_worldHashes.find(tick);
            if (found == m_worldHashes.end())
                ++missing;
            else if (found->second != hash)
                ++different;
        }

        m_auto = different == 0 && missing == 0 ? AutoStage::Passed : AutoStage::Failed;
        m_autoSummary = std::format(
            "写しの刻み {}〜{} の {} 刻みが本物と同じハッシュ(違う {}・本物に無い {})・1 刻み {:.1f} µs・"
            "VRAM {} MiB・待ったフレーム {}・写しの間に本物が捨てた刻み {}",
            m_copyHashes.begin()->first, m_copyHashes.rbegin()->first, m_copyHashes.size() - different - missing,
            different, missing, m_progress.tickGpuMicroseconds, m_progress.vramBytes >> 20, m_progress.waitedFrames,
            m_droppedTicks - m_droppedAtStart);
        Log(Channel::Sim, m_auto == AutoStage::Passed ? Level::Info : Level::Error, "--auto-copy: {}", m_autoSummary);
    }

}  // namespace bicameral::frame
