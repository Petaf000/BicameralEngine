// play_metric_cli.cpp — bicameral --measure-metrics(play_metric_cli.h。T-0432・ADR-0206)。
#include "frame/play_metric_cli.h"

#include <format>
#include <fstream>
#include <iterator>

#include "core/log.h"
#include "gpu/queue.h"
#include "sim/physics_scene.h"
#include "sim/play_metric_measure.h"
#include "sim/world_physics.h"

namespace bicameral::frame {

    namespace {

        constexpr uint64_t TICKS_PER_SUBMIT = 600;  // 1 回に投げる早送りの刻み(待つのは CPU だけ。本物の世界は無い)
        constexpr uint64_t TICKS_PER_SECOND = 60;
        constexpr uint32_t MAX_IDLE_ROUNDS = 64;  // 何も投げない回が続いたら止める(作りの誤りの安全網)

        std::expected<std::vector<sim::PlayMetricValue>, std::string> ReadBaseline(const fs::path& path) {
            std::error_code error;
            if (path.empty() || !fs::exists(path, error))
                return std::vector<sim::PlayMetricValue>{};

            std::ifstream file(path, std::ios::binary);
            if (!file)
                return std::unexpected(std::format("前の版の値を読めない: {}", path.string()));

            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            return sim::ParsePlayMetricValues(text);
        }

        bool WriteBaseline(const fs::path& path, const std::vector<sim::PlayMetricValue>& values) {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << sim::SerializePlayMetricValues(values);

            return static_cast<bool>(file);
        }

        // 量の道のり(--log-level debug。指標のしきい値を決める材料)
        void LogSamples(const sim::PlayMetricValue& value) {
            std::string path;
            for (const auto& [tick, quantity] : value.samples)
                path += std::format(" {}:{}", tick, quantity);

            Log(Channel::Sim, Level::Debug, "遊びの指標 {} の道のり(刻み:量):{}", value.key, path);
        }

        // 指標を全部測る(続けて投げて待つ)
        std::expected<std::vector<sim::PlayMetricValue>, std::string> Measure(const FrameLoopOptions& options,
                                                                              const sim::BakedReactionTable& table) {
            gpu::DeviceOptions deviceOptions = gpu::DefaultDeviceOptions();
            deviceOptions.gpuBasedValidation = false;  // 物理のシェーダーの計装が数分かかる(フレームのループと同じ)
            auto device = gpu::Device::Create(options.adapter, deviceOptions);
            if (!device)
                return std::unexpected(device.error());

            auto queue = gpu::Queue::Create(device->Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, L"PlayMetrics");
            if (!queue)
                return std::unexpected(queue.error());

            // 物理の場面は本物の世界と同じ形(--no-physics なら無し)
            const sim::PhysicsScene scene = sim::MakeLayoutPhysicsScene(table.worldLayout);
            const bool stack = options.physics && !table.worldLayout.bodies.empty();
            auto measurer = sim::PlayMetricMeasurer::Create(
                device->Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, table,
                {.physicsScene = stack ? &scene : nullptr,
                 .physicsOptions = {.broadphaseGraph = !options.physicsComputeBroadphase},
                 .physicsParameters = sim::PhysicsParametersFor(table.environment)});
            if (!measurer)
                return std::unexpected(measurer.error());

            uint32_t idleRounds = 0;
            size_t logged = 0;
            while (!measurer->Finished() && idleRounds < MAX_IDLE_ROUNDS) {
                const std::vector<ID3D12CommandList*> lists = measurer->RecordNext(TICKS_PER_SUBMIT);
                idleRounds = lists.empty() ? idleRounds + 1 : 0;
                if (!lists.empty() && !queue->WaitCpu(queue->Submit(lists)))
                    return std::unexpected("GPU を待てない(デバイスの喪失?)");

                measurer->Complete();
                for (; logged < measurer->Values().size(); ++logged) {
                    const sim::PlayMetricValue& value = measurer->Values()[logged];
                    const sim::WorldCopyResult& result = measurer->Results()[logged];
                    Log(Channel::Sim, Level::Info, "遊びの指標 {}: {} 刻み({})・GPU {} リスト", value.key, value.ticks,
                        value.reached ? "着いた" : "上限", result.frames);
                    LogSamples(value);
                }
            }

            if (!measurer->Finished())
                return std::unexpected("指標の早送りが進まない");

            return measurer->Values();
        }

    }  // namespace

    int RunPlayMetricCommand(const FrameLoopOptions& options, const PlayMetricCommandOptions& command) {
        const auto loaded = LoadLoopReactionTable(options);
        if (!loaded) {
            Log(Channel::Sim, Level::Error, "遊びの指標: 表を読めない: {}", loaded.error());
            return 1;
        }

        const auto previous = ReadBaseline(command.baselinePath);
        if (!previous) {
            Log(Channel::Sim, Level::Error, "遊びの指標: {}", previous.error());
            return 1;
        }

        const auto values = Measure(options, (*loaded)->table);
        if (!values) {
            Log(Channel::Sim, Level::Error, "遊びの指標: {}", values.error());
            return 1;
        }

        const std::vector<sim::PlayMetricDiff> diffs = sim::ComparePlayMetrics(*values, *previous);
        Log(Channel::Sim, Level::Info, "{}(表の版 {:016x})", sim::FormatPlayMetricReport(diffs, TICKS_PER_SECOND),
            (*loaded)->tableVersion);

        if (command.saveBaseline) {
            if (!WriteBaseline(command.baselinePath, *values)) {
                Log(Channel::Sim, Level::Error, "遊びの指標: 前の版の値を書けない: {}", command.baselinePath.string());
                return 1;
            }

            Log(Channel::Sim, Level::Info, "遊びの指標: 測った値を前の版として書いた: {}",
                command.baselinePath.string());
        }

        const bool allMeasured = rng::all_of(*values, &sim::PlayMetricValue::measured);
        return allMeasured ? 0 : 1;
    }

}  // namespace bicameral::frame
