// gpu_probe_physics_test.cpp — 仮の世界の刻みに入れた物理(T-0098)の GPU と CPU リファレンスのビット一致。
//
// 確かめること(仮の世界に積み木 sim::MakeProbeStackScene を入れ、決まった刻みに光線で押して崩す):
//   - 刻みごとの物の状態のハッシュ(ハッシュの表の欄)が、CPU の sim::PhysicsWorld(同じ刻みに Push → Step)と毎刻み一致
//   - 押すコマンドのイベント(押した物の番号)が CPU の Push の結果と同じ。最初の押しは 5 段目の箱(番号 6)に当たる
//   - フレームの切れ目を単位の途中に置いても(物理の単位がフレームをまたいでも)結果が同じ。2 回目は重さの単位を 3 つ入れ、
//     単位をほぼ 1 つずつ投げる(窓の再生の --sim-load・--sim-split と同じ形)。2 回の実行で世界のハッシュ列が一致
//   - 最後の抽出の物の欄と、読み戻した物が CPU の物と一致。物理の統計の overflow が 0
//   - debug layer のエラーとシェーダーの assert が 0 件
// 物理の単位の GPU 時間(タイムスタンプ)をログに出す(docs/perf.md)。
// 引数: gpu_test_options.h(--warp)と --ticks n(進める刻み。既定 240)
#include <algorithm>
#include <charconv>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "sim/physics_world.h"
#include "sim/probe_sim.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    // 引数で決まること
    struct TestConfig {
        uint64_t ticks = 240;
        bool computeBroadphase =
            false;          // --broadphase-compute: 広域の選別を Compute で(既定は ProbeSim と同じ Work Graph)
        bool noise = true;  // 2 回目に別のキューの雑音と重い単位を流すか(WARP では重すぎるので流さない)
    };

    constexpr int64_t METER = int64_t{1} << 20;
    constexpr int32_t UNIT = 1 << 30;                                  // Q1.30 の 1
    constexpr uint32_t PUSH_IMPULSE_MNS = 5'000'000;                   // 5000 N·s(500 kg の箱に 10 m/s)
    constexpr uint32_t FIRST_PUSH_HIT = 6;                             // 5 段目の箱(地面が 0、1 段目が 1)
    constexpr std::array<uint32_t, 5> FRAME_UNITS = {3, 7, 5, 11, 4};  // フレームの単位の数(刻みの途中で切れる)
    constexpr std::array<uint32_t, 5> SMALL_FRAME_UNITS = {1, 1, 2, 1,
                                                           3};  // 重さの単位を入れた実行(単位がほぼ 1 つずつ)

    struct Push {
        uint64_t tick = 0;
        std::array<int64_t, 3> origin{};
        std::array<int32_t, 3> direction{};
    };

    // 1 回目: 5 段目の箱の真ん中を −z の側から +z へ(回さずに押し出す)。2 回目: 上の方を +z の側から、中心を外して(回す)
    const std::array<Push, 2> PUSHES = {{
        {.tick = 30, .origin = {10 * METER, 11 * METER / 2, 6 * METER}, .direction = {0, 0, UNIT}},
        {.tick = 90, .origin = {10 * METER + 3 * METER / 10, 44 * METER / 5, 26 * METER}, .direction = {0, 0, -UNIT}},
    }};

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition)
                return;

            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    // 木箱の壁に火をつけ(伝導と反応の Work Graph を物理と同じ刻みで忙しくする。窓の --auto-click の最初の 1 回と同じ)、決まった刻みに押す
    std::vector<ProbeCommand> MakePushCommands() {
        std::vector<ProbeCommand> commands = {MakePokeCommand(0, 0, 28, 32, 32)};
        for (uint32_t index = 0; index < PUSHES.size(); ++index) {
            const Push& push = PUSHES[index];
            commands.push_back(MakePushCommand(push.tick, index + 1, push.origin, push.direction, PUSH_IMPULSE_MNS));
        }

        return commands;
    }

    physics::PxVec3 ToVec3(const std::array<int64_t, 3>& value) {
        return physics::PxMakeVec3(value[0], value[1], value[2]);
    }

    physics::PxVec3 ToVec3(const std::array<int32_t, 3>& value) {
        return physics::PxMakeVec3(value[0], value[1], value[2]);
    }

    // --- CPU リファレンス ---

    struct Expected {
        std::vector<uint64_t> bodyHashes;  // [t] = S(t + 1) の物のハッシュ
        std::vector<ProbeEvent> pushEvents;
        std::vector<PhysicsBody> finalBodies;
    };

    Expected RunReference(const PhysicsScene& scene, uint64_t ticks) {
        Expected expected;
        PhysicsWorld world(scene, physics::PxDefaultParameters());
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            for (const Push& push : PUSHES) {
                if (push.tick != tick)
                    continue;

                const uint32_t hit = world.Push(ToVec3(push.origin), ToVec3(push.direction), PUSH_IMPULSE_MNS);
                expected.pushEvents.push_back({.tick = tick,
                                               .type = PROBE_EVENT_BODY_PUSHED,
                                               .place = hit == UINT32_MAX ? PROBE_PUSH_NOTHING : hit});
            }

            world.Step();
            expected.bodyHashes.push_back(world.StateHash());
        }

        expected.finalBodies = world.Bodies();

        return expected;
    }

    // --- GPU ---

    struct Run {
        bool ok = false;
        std::vector<ProbeTickHash> hashes;
        std::vector<ProbeEvent> events;
        std::vector<PhysicsBody> bodies;
        std::vector<uint32_t> bodyView;  // 最後の抽出の物の欄
        GpuPhysicsStats stats;
        uint32_t debugAssertCount = 0;
        uint64_t physicsUnitTicks = 0;  // 物理の単位のタイムスタンプの刻みの合計
        uint32_t physicsUnits = 0;
        double microsecondsPerTimestamp = 0.0;
    };

    std::vector<uint32_t> ReadBodyView(ID3D12Device5* device, ID3D12Resource* extraction) {
        constexpr size_t WORDS = PROBE_BODY_VIEW_HEADER_WORDS + size_t{PROBE_MAX_VIEW_BODIES} * PROBE_BODY_VIEW_WORDS;
        std::vector<uint32_t> words(WORDS);
        auto queue = gpu::ImmediateQueue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        const ComPtr<ID3D12Resource> readback = gpu::CreateBuffer(device, WORDS * 4, gpu::BufferKind::Readback);
        if (!queue || !readback)
            return {};

        ID3D12GraphicsCommandList10* list = queue->Begin();
        if (list == nullptr)
            return {};

        list->CopyBufferRegion(readback.Get(), 0, extraction, uint64_t{PROBE_EXTRACTION_BODY_OFFSET} * 4, WORDS * 4);
        if (!queue->ExecuteAndWait() || !gpu::ReadBuffer(readback.Get(), std::as_writable_bytes(std::span(words))))
            return {};

        return words;
    }

    void Accumulate(Run& run, const ProbeFrameReadback& readback, uint32_t unitsPerTick) {
        run.hashes.insert(run.hashes.end(), readback.hashes.begin(), readback.hashes.end());
        run.events.insert(run.events.end(), readback.events.begin(), readback.events.end());
        run.debugAssertCount += readback.debugAssertCount;
        for (size_t index = 0; index < readback.unitGpuTicks.size(); ++index) {
            if ((readback.firstUnit + index) % unitsPerTick != PROBE_UNIT_PHYSICS)
                continue;

            run.physicsUnitTicks += readback.unitGpuTicks[index];
            ++run.physicsUnits;
        }
    }

    // 1 回分の GPU の実行。second = true なら窓に近い形(重さの単位・単位をほぼ 1 つずつ・毎フレーム抽出・待たずに投げる・別のキューの雑音)
    class GpuRun {
    public:
        GpuRun(ID3D12Device5* device, const TestConfig& config, bool second)
            : m_device(device), m_config(config), m_second(second) {}

        Run Execute(const BakedReactionTable& table, const PhysicsScene& scene) {
            if (!Create(table, scene))
                return std::move(m_run);

            const std::vector<ProbeCommand> commands = MakePushCommands();
            const std::span<const uint32_t> frameUnits = m_second ? std::span<const uint32_t>(SMALL_FRAME_UNITS)
                                                                  : std::span<const uint32_t>(FRAME_UNITS);
            const uint64_t totalUnits = m_config.ticks * m_unitsPerTick;
            uint64_t position = 0;  // 投げた単位の数

            for (uint32_t frame = 0; position < totalUnits; ++frame) {
                const auto units = static_cast<uint32_t>(
                    std::min<uint64_t>(frameUnits[frame % frameUnits.size()], totalUnits - position));
                const bool last = position + units == totalUnits;
                const ProbeFrameInput input{
                    .firstTick = position / m_unitsPerTick,
                    .firstUnit = static_cast<uint32_t>(position % m_unitsPerTick),
                    .unitCount = units,
                    .extract = m_second || last,  // 2 回目は窓と同じく毎フレーム(刻みの途中でも)抽出する
                    .extractionTarget = last ? 0 : frame % PROBE_EXTRACTION_COUNT,
                    .commands = frame == 0 ? std::span<const ProbeCommand>(commands) : std::span<const ProbeCommand>(),
                    .readPhysics = last};

                if (!SubmitFrame(frame, input))
                    return std::move(m_run);

                position += units;
            }

            return Finish();
        }

    private:
        bool Create(const BakedReactionTable& table, const PhysicsScene& scene) {
            const uint32_t busyIterations = m_config.noise ? 1200000u : 30000u;
            auto queue = gpu::Queue::Create(m_device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"PhysicsProbeTestSim");
            auto simulation = ProbeSim::Create(m_device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table,
                                               {.busyIterations = m_second ? busyIterations : 0u,
                                                .busyPieces = m_second ? 3u : 1u,
                                                .physicsScene = &scene,
                                                .physicsOptions = {.broadphaseGraph = !m_config.computeBroadphase}});

            // 雑音: 別の優先度の高い direct のキューで重い仕事を流す(窓の描画のように、物理の途中に割り込ませる)
            auto noiseQueue = gpu::Queue::Create(m_device, D3D12_COMMAND_LIST_TYPE_DIRECT, L"PhysicsProbeTestNoise",
                                                 D3D12_COMMAND_QUEUE_PRIORITY_HIGH);
            auto noise = ProbeSim::Create(m_device, D3D12_COMMAND_LIST_TYPE_DIRECT, table,
                                          {.busyIterations = 200000, .busyPieces = 4});
            if (!queue || !simulation || !noiseQueue || !noise) {
                Log(Channel::Sim, Level::Error, "作れない: {}{}", queue ? "" : queue.error(),
                    simulation ? "" : simulation.error());
                return false;
            }

            m_queue = std::make_unique<gpu::Queue>(std::move(*queue));
            m_simulation = std::make_unique<ProbeSim>(std::move(*simulation));
            m_noiseQueue = std::make_unique<gpu::Queue>(std::move(*noiseQueue));
            m_noise = std::make_unique<ProbeSim>(std::move(*noise));
            m_unitsPerTick = m_simulation->UnitsPerTick();
            m_run.microsecondsPerTimestamp = 1'000'000.0 / static_cast<double>(m_queue->TimestampFrequency());

            return true;
        }

        // 1 フレームを投げる。1 回目は毎フレーム待つ。2 回目は枠を使い回す前だけ待つ(最大 FRAME_SLOT_COUNT 個が GPU に並ぶ)
        bool SubmitFrame(uint32_t frame, const ProbeFrameInput& input) {
            const auto slot = static_cast<uint32_t>(frame % ProbeSim::FRAME_SLOT_COUNT);
            if (m_fences[slot] != 0) {
                if (!m_queue->WaitCpu(m_fences[slot]))
                    return false;

                Accumulate(m_run, m_simulation->ReadFrame(slot), m_unitsPerTick);
                m_fences[slot] = 0;
            }

            ID3D12CommandList* list = m_simulation->RecordFrame(slot, input);
            if (list == nullptr)
                return false;

            m_fences[slot] = m_queue->Submit(list);
            if (m_second && m_config.noise && !SubmitNoise(slot))
                return false;

            if (m_second && !input.readPhysics)
                return true;

            return DrainAll(slot);
        }

        bool SubmitNoise(uint32_t slot) {
            if (!m_noiseQueue->IsComplete(m_noiseFences[slot]))
                return true;

            if (m_noiseFences[slot] != 0)
                (void)m_noise->ReadFrame(slot);

            constexpr uint32_t NOISE_UNITS = 5;
            const uint32_t noiseUnitsPerTick = m_noise->UnitsPerTick();
            ID3D12CommandList* list = m_noise->RecordFrame(
                slot, {.firstTick = m_noiseUnits / noiseUnitsPerTick,
                       .firstUnit = static_cast<uint32_t>(m_noiseUnits % noiseUnitsPerTick),
                       .unitCount = NOISE_UNITS});
            if (list == nullptr)
                return false;

            m_noiseFences[slot] = m_noiseQueue->Submit(list);
            m_noiseUnits += NOISE_UNITS;

            return true;
        }

        // 全部を待って、古い順に読む
        bool DrainAll(uint32_t newestSlot) {
            if (!m_queue->WaitIdle())
                return false;

            for (uint32_t offset = 1; offset <= ProbeSim::FRAME_SLOT_COUNT; ++offset) {
                const uint32_t pending = (newestSlot + offset) % ProbeSim::FRAME_SLOT_COUNT;
                if (m_fences[pending] == 0)
                    continue;

                Accumulate(m_run, m_simulation->ReadFrame(pending), m_unitsPerTick);
                m_fences[pending] = 0;
            }

            return true;
        }

        Run Finish() {
            if (!m_noiseQueue->WaitIdle())
                return std::move(m_run);

            m_run.bodies = m_simulation->Physics()->ReadBodies();
            m_run.stats = m_simulation->Physics()->ReadStats();
            m_run.bodyView = ReadBodyView(m_device, m_simulation->Extraction(0));
            m_run.ok = m_run.hashes.size() == m_config.ticks && !m_run.bodyView.empty();

            return std::move(m_run);
        }

        // --- 設定 ---
        ID3D12Device5* m_device;
        TestConfig m_config;
        bool m_second;

        // --- 作ったもの ---
        std::unique_ptr<gpu::Queue> m_queue;
        std::unique_ptr<ProbeSim> m_simulation;
        std::unique_ptr<gpu::Queue> m_noiseQueue;
        std::unique_ptr<ProbeSim> m_noise;
        uint32_t m_unitsPerTick = 0;

        // --- 投げた状態 ---
        std::array<uint64_t, ProbeSim::FRAME_SLOT_COUNT> m_fences{};  // 枠ごとの投げたリストのフェンス(0 = 読んだ)
        std::array<uint64_t, ProbeSim::FRAME_SLOT_COUNT> m_noiseFences{};
        uint64_t m_noiseUnits = 0;
        Run m_run;
    };

    // --- 比べる ---

    uint32_t CountHashMismatches(const Run& run, const Expected& expected) {
        uint32_t mismatches = 0;
        for (size_t index = 0; index < run.hashes.size() && index < expected.bodyHashes.size(); ++index) {
            if (run.hashes[index].bodyHash == expected.bodyHashes[index])
                continue;

            if (mismatches == 0) {
                Log(Channel::Sim, Level::Error, "刻み {} の物のハッシュが違う: GPU {:016x} CPU {:016x}",
                    run.hashes[index].tick, run.hashes[index].bodyHash, expected.bodyHashes[index]);
            }

            ++mismatches;
        }

        return mismatches;
    }

    std::vector<ProbeEvent> PushEvents(const Run& run) {
        std::vector<ProbeEvent> events;
        rng::copy_if(run.events, std::back_inserter(events),
                     [](const ProbeEvent& event) { return event.type == PROBE_EVENT_BODY_PUSHED; });

        return events;
    }

    int64_t ReadViewInt64(std::span<const uint32_t> words, size_t index) {
        return static_cast<int64_t>(uint64_t{words[index]} | (uint64_t{words[index + 1]} << 32));
    }

    // 抽出の物の欄が CPU の物と同じか(probe_sim.hlsli の並び)
    bool SameBodyView(std::span<const uint32_t> view, const std::vector<PhysicsBody>& bodies) {
        if (view.empty() || view[0] != bodies.size())
            return false;

        for (size_t index = 0; index < bodies.size(); ++index) {
            const PhysicsBody& body = bodies[index];
            const std::span<const uint32_t> words = view.subspan(
                PROBE_BODY_VIEW_HEADER_WORDS + index * PROBE_BODY_VIEW_WORDS, PROBE_BODY_VIEW_WORDS);
            const uint32_t flags = (body.massMilligrams > 0 ? PROBE_BODY_VIEW_DYNAMIC : 0) |
                                   (body.active != 0 ? PROBE_BODY_VIEW_ACTIVE : 0);
            const bool same = ReadViewInt64(words, 0) == body.position.x &&
                              ReadViewInt64(words, 2) == body.position.y &&
                              ReadViewInt64(words, 4) == body.position.z &&
                              static_cast<int32_t>(words[6]) == body.rotation.x &&
                              static_cast<int32_t>(words[7]) == body.rotation.y &&
                              static_cast<int32_t>(words[8]) == body.rotation.z &&
                              static_cast<int32_t>(words[9]) == body.rotation.w &&
                              static_cast<int32_t>(words[10]) == body.halfExtent.x && words[13] == flags;
            if (!same)
                return false;
        }

        return true;
    }

    uint64_t BodiesHash(uint64_t tick, const std::vector<PhysicsBody>& bodies) {
        return GpuPhysics::StateHash(tick, bodies);
    }

    // 値なしの引数 flag を取り除き、あったか返す
    bool TakeFlag(std::vector<char*>& arguments, std::string_view flag) {
        const auto found = rng::find_if(arguments,
                                        [flag](const char* argument) { return std::string_view(argument) == flag; });
        if (found == arguments.end())
            return false;

        arguments.erase(found);
        return true;
    }

    // --ticks n を取り除く(残りは gpu_test_options.h へ)
    uint64_t TakeTicks(std::vector<char*>& arguments, uint64_t fallback) {
        for (size_t index = 1; index + 1 < arguments.size(); ++index) {
            if (std::string_view(arguments[index]) != "--ticks")
                continue;

            const std::string_view text = arguments[index + 1];
            uint64_t value = fallback;
            std::from_chars(text.data(), text.data() + text.size(), value);
            arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index),
                            arguments.begin() + static_cast<std::ptrdiff_t>(index) + 2);

            return value;
        }

        return fallback;
    }

    // 引数(--ticks n・--broadphase-compute を取り除いてから gpu_test_options.h へ)
    std::optional<TestConfig> ParseConfig(std::vector<char*>& arguments, std::optional<test::GpuTestOptions>& options) {
        TestConfig config{.ticks = TakeTicks(arguments, 240)};
        config.computeBroadphase = TakeFlag(arguments, "--broadphase-compute");
        options = test::ParseGpuTestOptions(std::span(arguments));
        if (!options || config.ticks <= PUSHES.back().tick)
            return std::nullopt;

        config.noise = options->adapter != gpu::AdapterKind::Warp;

        return config;
    }

    // 2 回の実行を CPU と比べて、失敗の数を返す
    int Judge(const Run& first, const Run& second, const Expected& expected, uint64_t ticks) {
        const uint32_t mismatches = CountHashMismatches(first, expected);
        const uint32_t secondMismatches = CountHashMismatches(second, expected);
        const std::vector<ProbeEvent> pushes = PushEvents(first);
        const bool sameRuns = first.ok && second.ok &&
                              rng::equal(first.hashes, second.hashes, {}, &ProbeTickHash::WorldHash,
                                         &ProbeTickHash::WorldHash);

        for (const ProbeEvent& event : pushes)
            Log(Channel::Sim, Level::Info, "  刻み {} の押し: 物 {}", event.tick, event.place);

        Log(Channel::Sim, Level::Info,
            "物のハッシュ: GPU {:016x} CPU {:016x}  一致しない刻み {}(2 回目 {})  統計: 接触 {} 色 {} overflow {}",
            first.hashes.empty() ? 0 : first.hashes.back().bodyHash,
            expected.bodyHashes.empty() ? 0 : expected.bodyHashes.back(), mismatches, secondMismatches,
            first.stats.contactCount, first.stats.colorCount, first.stats.overflow);
        Log(Channel::Sim, Level::Info, "物理の単位: {} 回、平均 {:.0f} µs(暖機なし)", first.physicsUnits,
            static_cast<double>(first.physicsUnitTicks) * first.microsecondsPerTimestamp /
                std::max(1u, first.physicsUnits));

        Failures failures;
        failures.Check(first.ok && second.ok, "走らせられた");
        failures.Check(mismatches == 0 && first.hashes.size() == expected.bodyHashes.size(),
                       "刻みごとの物のハッシュが CPU リファレンスとビット一致");
        failures.Check(pushes == expected.pushEvents, "押すコマンドのイベント(押した物)が CPU と同じ");
        failures.Check(!pushes.empty() && pushes[0].place == FIRST_PUSH_HIT, "最初の押しが 5 段目の箱に当たった");
        failures.Check(sameRuns && secondMismatches == 0,
                       "2 回目(重さの単位・単位をほぼ 1 つずつ・別のキューの雑音)も世界のハッシュ列が 1 回目と一致");
        failures.Check(BodiesHash(ticks, first.bodies) == BodiesHash(ticks, expected.finalBodies),
                       "読み戻した物が CPU の物と一致");
        failures.Check(SameBodyView(first.bodyView, expected.finalBodies), "抽出の物の欄が CPU の物と一致");
        failures.Check(first.stats.overflow == 0, "物理の統計の overflow が 0");
        failures.Check(first.debugAssertCount == 0 && second.debugAssertCount == 0, "シェーダーの assert が 0 件");

        return failures.count;
    }

}  // namespace

int main(int argc, char** argv) {
    std::vector<char*> arguments(argv, argv + argc);
    std::optional<test::GpuTestOptions> options;
    const auto config = ParseConfig(arguments, options);
    const auto table = BakeReactionTable(MakeCombustionTestTable());
    if (!config || !options || !table) {
        Log(Channel::Sim, Level::Error,
            "使い方: gpu_probe_physics_test [--warp] [--ticks n(> {})] [--broadphase-compute]", PUSHES.back().tick);
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    // 物理のシェーダーは GPU-based validation の計装に数分かかるので切る(gpu_physics_test と同じ。debug layer は有効)
    gpu::DeviceOptions deviceOptions = test::TestDeviceOptions(*options);
    deviceOptions.gpuBasedValidation = false;
    auto device = gpu::Device::Create(options->adapter, deviceOptions);
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    const PhysicsScene scene = MakeProbeStackScene();
    Log(Channel::Sim, Level::Info, "仮の世界に積み木(物 {})を入れて {} 刻み。刻み {} と {} に押す", scene.bodies.size(),
        config->ticks, PUSHES[0].tick, PUSHES[1].tick);

    const Expected expected = RunReference(scene, config->ticks);
    const Run first = GpuRun(device->Get(), *config, false).Execute(*table, scene);
    const Run second = GpuRun(device->Get(), *config, true).Execute(*table, scene);
    const int failures = Judge(first, second, expected, config->ticks);

    const bool passed = failures == 0 && test::PassesValidation(*device, "gpu_probe_physics_test");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
