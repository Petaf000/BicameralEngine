// gpu_world_copy_test.cpp — 世界の写しの早送り(sim/world_copy。T-0412・ADR-0198)の決定性と、本物の世界を変えないこと。
//
// 確かめること(仮の世界 + 積み木・試験の表。つつき・押すコマンドは gpu_probe_rewind_test と同じ形):
//   - 基準: 本物の世界だけを TICKS 刻み流した、刻みごとの世界のハッシュ(セル + 物)
//   - 写しを取りながら: 別の本物の世界を同じコマンド・ばらばらの単位の数で流し、刻み FORK_TICK の境界で写しを取る。早送りは
//     本物のフレームの後ろに同じキューで投げる(1 回に COPY_TICKS_PER_FRAME 刻み。本物の刻みの合間)。
//       1. 本物のハッシュ列が基準と S(1)〜S(TICKS) で全部一致(写しは本物を変えない。D-403)
//       2. 写しに本物と同じコマンド(FORK_TICK 以降)を当てて TICKS まで → 写しのハッシュ列(セル + 物)が基準と全部一致
//       3. 写しのセルのハッシュが CPU リファレンス(ProbeReference)と毎刻み一致・読み戻したセルの木の量が CPU と同じ(10 刻みごと)
//   - 同じ写しを使い直す: 本物が刻み SECOND_TICK に来たら、コマンドを当てずに写しを取り直し、条件「木の量が閾値以下」で止める。
//     止まった刻みが、CPU リファレンスで同じ条件を数えた刻みと同じ(閾値は CPU の木の量の道のりから決める)・
//     止まるまでのセルのハッシュが CPU と一致・つつきを当てていないので基準と違う刻みがある
//   - シェーダーの assert と debug layer のエラーが 0 件
// 写しの VRAM(既定のヒープの主なバッファ)と、早送りの 1 刻みの GPU の時間をログに出す(docs/perf.md)。
// 引数: gpu_test_options.h(--warp・--no-gbv)
#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu/queue.h"
#include "gpu_test_options.h"
#include "reaction_test_table.h"
#include "sim/probe_sim.h"
#include "sim/world_copy.h"
#include "sim/world_physics.h"
#include "standard_world_physics.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    // CPU リファレンス(全部のセルを毎刻み)が debug で 1 刻み 1 秒ほどかかるので、刻みは少なめ(GPU の順番待ち。D-460)
    constexpr uint64_t TICKS = 80;
    constexpr uint64_t FORK_TICK = 20;    // 1 つ目の写しを取る刻み
    constexpr uint64_t SECOND_TICK = 40;  // 写しを使い直す刻み(1 つ目の早送りが終わった後)
    constexpr uint64_t SECOND_LIMIT = 60;
    constexpr uint64_t DIVERGING_POKE_TICK = 50;  // 使い直した写しに当てないつつき
    constexpr uint64_t THRESHOLD_SAMPLE = 4;      // 2 つ目の閾値 = CPU の木の量の道のりの、この番目の読み戻しの値
    constexpr uint32_t SAMPLE_INTERVAL = 10;
    constexpr uint64_t COPY_TICKS_PER_FRAME = 16;  // 読み戻す刻み(10 刻みごと)で切れる
    constexpr std::array<uint32_t, 5> FRAME_UNITS = {3, 7, 5, 11, 4};

    constexpr int64_t METER = int64_t{1} << 20;
    constexpr int32_t UNIT = 1 << 30;                 // Q1.30 の 1
    constexpr uint32_t PUSH_IMPULSE_MNS = 5'000'000;  // 5000 N·s

    // 刻みの順・番号の順(GPU のキューの約束)
    std::vector<ProbeCommand> MakeCommands() {
        return {
            MakePokeCommand(0, 0, 28, 32, 32),  // 木箱の壁に火をつける
            MakePushCommand(FORK_TICK, 1, {10 * METER, 11 * METER / 2, 6 * METER}, {0, 0, UNIT}, PUSH_IMPULSE_MNS),
            MakePokeCommand(FORK_TICK, 2, 36, 32, 32),  // 写しの始めの刻みのコマンド(写しにも当たる)
            MakePokeCommand(DIVERGING_POKE_TICK, 3, 20, 40, 24),
            MakePushCommand(70, 4, {10 * METER + 3 * METER / 10, 44 * METER / 5, 26 * METER}, {0, 0, -UNIT},
                            PUSH_IMPULSE_MNS),
        };
    }

    std::vector<ProbeCommand> CommandsFrom(uint64_t tick) {
        std::vector<ProbeCommand> commands = MakeCommands();
        std::erase_if(commands, [&](const ProbeCommand& command) { return command.targetTick < tick; });

        return commands;
    }

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition)
                return;

            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    // CPU リファレンスで見た道のり(刻み → セルのハッシュ、読み戻す刻み → 木の量)
    struct ReferencePath {
        std::map<uint64_t, uint64_t> cellHashes;
        std::map<uint64_t, uint64_t> wood;
    };

    void Record(const ProbeReference& reference, uint64_t tick, uint64_t sampleStart, uint32_t cellulose,
                ReferencePath& path) {
        path.cellHashes[tick] = reference.StateHash(tick);
        if (tick >= sampleStart && (tick - sampleStart) % SAMPLE_INTERVAL == 0)
            path.wood[tick] = ProbeSpeciesAmount(reference.State(tick), cellulose);
    }

    // CPU リファレンス: コマンドありで TICKS まで(full)と、SECOND_TICK からコマンドなしで SECOND_LIMIT 刻み(second)
    std::pair<ReferencePath, ReferencePath> RunReference(const BakedReactionTable& table, uint32_t cellulose) {
        const std::vector<ProbeCommand> commands = MakeCommands();
        ProbeReference reference(table);
        std::optional<ProbeReference> second;
        ReferencePath full;
        ReferencePath without;

        for (uint64_t tick = 0; tick < TICKS; ++tick) {
            if (tick == SECOND_TICK) {
                second = reference;
                Record(*second, tick, SECOND_TICK, cellulose, without);
            }

            std::vector<ProbeCommand> due;
            rng::copy_if(commands, std::back_inserter(due),
                         [&](const ProbeCommand& command) { return command.targetTick == tick; });
            reference.Advance(tick, due);
            Record(reference, tick + 1, FORK_TICK, cellulose, full);
        }

        for (uint64_t tick = SECOND_TICK; tick < SECOND_TICK + SECOND_LIMIT; ++tick) {
            second->Advance(tick, {});
            Record(*second, tick + 1, SECOND_TICK, cellulose, without);
        }

        return {std::move(full), std::move(without)};
    }

    struct Created {
        std::unique_ptr<gpu::Queue> queue;
        std::unique_ptr<ProbeSim> simulation;
    };

    ProbeSimOptions SimOptions(const PhysicsScene& scene) {
        return {.physicsScene = &scene, .physicsParameters = PhysicsParametersFor(StandardWorldPhysics())};
    }

    std::optional<Created> Create(ID3D12Device5* device, const BakedReactionTable& table, const PhysicsScene& scene) {
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"WorldCopyTestSim");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table, SimOptions(scene));
        if (!queue || !simulation) {
            Log(Channel::Sim, Level::Error, "作れない: {}{}", queue ? "" : queue.error(),
                simulation ? "" : simulation.error());
            return std::nullopt;
        }

        return Created{.queue = std::make_unique<gpu::Queue>(std::move(*queue)),
                       .simulation = std::make_unique<ProbeSim>(std::move(*simulation))};
    }

    // 本物の世界を 1 フレームずつ投げる。写しがあれば、本物のフレームの後ろに写しのリストを同じキューで投げ、両方を待つ
    class Driver {
    public:
        Driver(gpu::Queue& queue, ProbeSim& simulation) : m_queue(queue), m_simulation(simulation) {}

        // 刻み to の境界まで。commands は最初のフレームで足す。breakAt の境界ではフレームを切り、onBoundary(刻み) を呼ぶ
        // (写しを取る・早送りを回す)
        template <typename OnFrame>
        bool Advance(uint64_t to, std::span<const ProbeCommand> commands, std::span<const uint64_t> breakAt,
                     OnFrame&& onFrame) {
            const uint32_t unitsPerTick = m_simulation.UnitsPerTick();
            const uint64_t end = to * unitsPerTick;
            bool first = true;

            while (m_position < end) {
                const uint64_t tick = m_position / unitsPerTick;
                uint64_t units = std::min<uint64_t>(FRAME_UNITS[m_frame % FRAME_UNITS.size()], end - m_position);
                for (const uint64_t boundary : breakAt) {
                    if (boundary * unitsPerTick > m_position)
                        units = std::min(units, boundary * unitsPerTick - m_position);
                }

                const ProbeFrameInput input{.firstTick = tick,
                                            .firstUnit = static_cast<uint32_t>(m_position % unitsPerTick),
                                            .unitCount = static_cast<uint32_t>(units),
                                            .commands = first ? commands : std::span<const ProbeCommand>()};
                ID3D12CommandList* list = m_simulation.RecordFrame(0, input);
                if (list == nullptr)
                    return false;

                m_queue.Execute(list);
                m_position += units;
                first = false;
                ++m_frame;

                // 本物のリストの後ろ・次のリストの前(写しを取るなら、本物が刻みの境界で終わっている)
                const bool atBoundary = m_position % unitsPerTick == 0;
                std::vector<ID3D12CommandList*> copyLists = onFrame(m_position / unitsPerTick, atBoundary);
                const uint64_t fence = m_queue.Submit(copyLists);
                if (!m_queue.WaitCpu(fence))
                    return false;

                const ProbeFrameReadback readback = m_simulation.ReadFrame(0);
                for (const ProbeTickHash& hash : readback.hashes)
                    m_hashes[hash.tick] = hash.WorldHash();

                m_debugAssertCount += readback.debugAssertCount;
            }

            return true;
        }

        [[nodiscard]] const std::map<uint64_t, uint64_t>& Hashes() const { return m_hashes; }
        [[nodiscard]] uint32_t DebugAssertCount() const { return m_debugAssertCount; }

    private:
        gpu::Queue& m_queue;
        ProbeSim& m_simulation;
        uint64_t m_position = 0;
        uint64_t m_frame = 0;
        std::map<uint64_t, uint64_t> m_hashes;
        uint32_t m_debugAssertCount = 0;
    };

    uint32_t CountMismatches(const std::map<uint64_t, uint64_t>& got, const std::map<uint64_t, uint64_t>& expected,
                             uint64_t from, uint64_t to, std::string_view label) {
        uint32_t mismatches = 0;
        uint64_t firstMismatch = 0;
        for (uint64_t tick = from; tick <= to; ++tick) {
            const auto a = got.find(tick);
            const auto b = expected.find(tick);
            if (a != got.end() && b != expected.end() && a->second == b->second)
                continue;

            if (mismatches++ == 0)
                firstMismatch = tick;
        }

        Log(Channel::Sim, Level::Info, "{}: S({})〜S({}) のうち一致しない刻み {}(最初 {})", label, from, to, mismatches,
            firstMismatch);

        return mismatches;
    }

    // 写しの道のり(早送りの結果のハッシュと、条件に渡された木の量)
    struct CopyPath {
        std::map<uint64_t, uint64_t> worldHashes;
        std::map<uint64_t, uint64_t> cellHashes;
        std::map<uint64_t, uint64_t> wood;
    };

    void CollectHashes(const WorldCopyResult& result, CopyPath& path) {
        for (const ProbeTickHash& hash : result.hashes) {
            path.worldHashes[hash.tick] = hash.WorldHash();
            path.cellHashes[hash.tick] = hash.hash;
        }
    }

    int RunTest(ID3D12Device5* device, const BakedReactionTable& table) {
        Failures failures;
        const PhysicsScene scene = MakeLayoutPhysicsScene(table.worldLayout);
        const uint32_t cellulose = table.SpeciesId("cellulose");
        const std::vector<ProbeCommand> commands = MakeCommands();

        // --- 基準(本物だけ)---
        std::optional<Created> base = Create(device, table, scene);
        std::optional<Created> real = Create(device, table, scene);
        failures.Check(base && real, "本物の世界を作れた");
        if (!base || !real)
            return failures.count;

        Driver baseDriver(*base->queue, *base->simulation);
        failures.Check(
            baseDriver.Advance(TICKS, commands, {}, [](uint64_t, bool) { return std::vector<ID3D12CommandList*>(); }),
            "基準を流せた");
        const std::map<uint64_t, uint64_t>& baseline = baseDriver.Hashes();

        // --- CPU リファレンス ---
        const auto [referenceFull, referenceSecond] = RunReference(table, cellulose);
        const uint64_t threshold = std::next(referenceSecond.wood.begin(), THRESHOLD_SAMPLE)->second;
        uint64_t expectedEnd = 0;
        for (const auto& [tick, amount] : referenceSecond.wood) {
            if (amount <= threshold) {
                expectedEnd = tick;
                break;
            }
        }

        Log(Channel::Sim, Level::Info, "CPU: 刻み {} からの木の量 {} → {}(閾値 {}・着く刻み {})", SECOND_TICK,
            referenceSecond.wood.begin()->second, referenceSecond.wood.rbegin()->second, threshold, expectedEnd);

        // --- 写しを取りながら本物を流す ---
        auto copy = WorldCopy::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, real->simulation->Table(),
                                      SimOptions(scene));
        failures.Check(copy.has_value(), "写しを作れた");
        if (!copy)
            return failures.count;

        CopyPath first;
        CopyPath second;
        uint32_t starts = 0;
        const std::array<uint64_t, 2> breaks = {FORK_TICK, SECOND_TICK};
        const auto onFrame = [&](uint64_t tick, bool atBoundary) {
            copy->Complete();
            if (!copy->Initialized())
                return copy->RecordInitialization();

            if (starts == 1 && copy->Finished() && first.worldHashes.empty())
                CollectHashes(copy->Result(), first);  // 使い直すと結果は空になる

            // 写しを取る(本物のリストが刻みの境界で終わった直後)
            const bool startFirst = starts == 0 && atBoundary && tick == FORK_TICK;
            const bool startSecond = starts == 1 && atBoundary && tick == SECOND_TICK && copy->Finished();
            if (startFirst || startSecond) {
                ++starts;
                CopyPath& path = startFirst ? first : second;
                WorldCopyQuery query{.maxTicks = startFirst ? TICKS - FORK_TICK : SECOND_LIMIT,
                                     .sampleIntervalTicks = SAMPLE_INTERVAL,
                                     .commands = startFirst ? CommandsFrom(FORK_TICK) : std::vector<ProbeCommand>(),
                                     .until = [&path, cellulose, threshold, startFirst](const WorldCopySample& sample) {
                                         if (sample.cells.empty())
                                             return false;

                                         const uint64_t amount = ProbeSpeciesAmount(sample.cells, cellulose);
                                         path.wood[sample.tick] = amount;
                                         return !startFirst && amount <= threshold;
                                     }};
                return copy->RecordStart(*real->simulation, tick, std::move(query));
            }

            return copy->RecordNext(COPY_TICKS_PER_FRAME);
        };

        Driver driver(*real->queue, *real->simulation);
        failures.Check(driver.Advance(TICKS, commands, breaks, onFrame), "写しを取りながら本物を流せた");

        // 2 つ目の早送りの残り(本物は TICKS で止まったので、写しだけを回す)
        for (uint32_t frame = 0; frame < 200 && !copy->Finished(); ++frame) {
            copy->Complete();
            const std::vector<ID3D12CommandList*> lists = copy->RecordNext(COPY_TICKS_PER_FRAME);
            if (lists.empty())
                continue;

            failures.Check(real->queue->WaitCpu(real->queue->Submit(lists)), "写しだけを回せた");
        }

        copy->Complete();
        failures.Check(starts == 2 && copy->Finished() && !copy->Result().failed, "写しを 2 回使って終えた");
        CollectHashes(copy->Result(), second);

        // --- 1. 本物は変わらない ---
        failures.Check(CountMismatches(driver.Hashes(), baseline, 1, TICKS, "写しを取った本物 と 基準") == 0,
                       "写しを取っても本物のハッシュ列が変わらない(D-403)");

        // --- 2・3. 1 つ目の写し = 本物 = CPU ---
        failures.Check(first.worldHashes.size() == TICKS - FORK_TICK, "1 つ目の写しが上限まで走った");
        failures.Check(CountMismatches(first.worldHashes, baseline, FORK_TICK + 1, TICKS, "1 つ目の写し と 基準") == 0,
                       "同じコマンドなら写しのハッシュ列(セル + 物)が本物と同じ");
        failures.Check(CountMismatches(first.cellHashes, referenceFull.cellHashes, FORK_TICK + 1, TICKS,
                                       "1 つ目の写し と CPU") == 0,
                       "写しのセルのハッシュが CPU リファレンスと毎刻み同じ");
        failures.Check(first.wood == referenceFull.wood, "読み戻した木の量が CPU と同じ(10 刻みごと)");

        // --- 使い直した写し(コマンドなし・条件で止まる)---
        const WorldCopyResult& result = copy->Result();
        Log(Channel::Sim, Level::Info, "2 つ目の写し: 刻み {} → {}(条件 {})", result.startTick, result.endTick,
            result.reached);
        failures.Check(result.reached && result.endTick == expectedEnd, "条件に着いた刻みが CPU で数えた刻みと同じ");
        failures.Check(CountMismatches(second.cellHashes, referenceSecond.cellHashes, SECOND_TICK + 1, result.endTick,
                                       "2 つ目の写し と CPU") == 0,
                       "使い直した写しのセルのハッシュも CPU と同じ");
        // 刻み DIVERGING_POKE_TICK のつつきを当てていないので、その次の刻みから本物と違う(コマンドが効いていることの確かめ)
        const bool diverged = rng::any_of(second.worldHashes, [&](const auto& entry) {
            return entry.first > DIVERGING_POKE_TICK && entry.first <= TICKS &&
                   baseline.at(entry.first) != entry.second;
        });
        failures.Check(result.endTick > DIVERGING_POKE_TICK && diverged,
                       "つつきを当てない写しは本物と違う(コマンドが効いている)");
        for (const auto& [tick, amount] : second.wood) {
            failures.Check(referenceSecond.wood.contains(tick) && referenceSecond.wood.at(tick) == amount,
                           "使い直した写しの木の量が CPU と同じ");
        }

        // --- 重さ ---
        const double frequency = static_cast<double>(real->queue->TimestampFrequency());
        Log(Channel::Sim, Level::Info,
            "写しの VRAM {} MiB・早送り 1 刻みの GPU 時間 {:.1f} µs({} 刻み・{} リスト。2 つ目)",
            copy->VramBytes() >> 20,
            static_cast<double>(result.gpuTimestampTicks) * 1'000'000.0 / frequency /
                static_cast<double>(std::max<uint64_t>(1, result.ticksRun)),
            result.ticksRun, result.frames);
        failures.Check(
            baseDriver.DebugAssertCount() == 0 && driver.DebugAssertCount() == 0 && result.debugAssertCount == 0,
            "シェーダーの assert が 0 件");

        return failures.count;
    }

}  // namespace

int main(int argc, char** argv) {
    std::vector<char*> arguments(argv, argv + argc);
    const std::optional<test::GpuTestOptions> options = test::ParseGpuTestOptions(std::span(arguments));
    const auto table = BakeReactionTable(MakeCombustionTestTable());
    if (!options || !table) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_world_copy_test [--warp] [--no-gbv]");
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    // 物理のシェーダーは GPU-based validation の計装に数分かかるので切る(gpu_probe_rewind_test と同じ。debug layer は有効)
    gpu::DeviceOptions deviceOptions = test::TestDeviceOptions(*options);
    deviceOptions.gpuBasedValidation = false;
    auto device = gpu::Device::Create(options->adapter, deviceOptions);
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    const int failures = RunTest(device->Get(), *table);
    const bool passed = failures == 0 && test::PassesValidation(*device, "gpu_world_copy_test");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
