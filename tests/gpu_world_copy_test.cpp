// gpu_world_copy_test.cpp — 世界の写しの早送り(sim/world_copy。T-0412・ADR-0198)の決定性と、本物の世界を変えないこと。
//
// 確かめること(仮の世界 + 積み木・試験の表。つつき・押すコマンドは gpu_probe_rewind_test と同じ形):
//   - 基準: 本物の世界だけを TICKS 刻み流した、刻みごとの世界のハッシュ(セル + 物)
//   - 写しを取りながら: 別の本物の世界を同じコマンド・ばらばらの単位の数で流し、刻み FORK_TICK の境界で写しを取る。早送りは
//     本物のフレームの後ろに同じキューで投げる(1 回に COPY_TICKS_PER_FRAME 刻み。本物の刻みの合間)。
//       1. 本物のハッシュ列が基準と S(1)〜S(TICKS) で全部一致(写しは本物を変えない。D-403)
//       2. 写しに本物と同じコマンド(FORK_TICK 以降)を当てて TICKS まで → 写しのハッシュ列(セル + 物)が基準と全部一致
//       3. 写しのセルのハッシュが CPU リファレンス(ProbeReference + PhysicsWorld。物理の熱を次の刻みの初めにセルへ足す。T-0561)と
//          毎刻み一致・読み戻したセルの木の量と最高温度が CPU と同じ(10 刻みごと)
//   - 同じ写しを使い直す: 本物が刻み SECOND_TICK に来たら、コマンドを当てずに写しを取り直し、条件「最高温度が閾値以下」で止める。
//     止まった刻みが、CPU リファレンスで同じ条件を数えた刻みと同じ(閾値は CPU の最高温度の道のりから決める)・
//     (条件は木の量だったが、現実の伝導率〔T-1066〕では火をつけたセルが 1 刻み目に熱分解し、刻み 40 から木の量が変わらない。
//      火をつけたセルが冷める最高温度で止める。T-1163)
//     止まるまでのセルのハッシュが CPU と一致・つつきを当てていないので基準と違う刻みがある
//   - 重さの集め方(T-0413・ADR-0204): 本物はばらばらの単位の数(刻みがフレームをまたぐ)で流しても、sim::TickCostAccumulator が
//     全部の刻みの重さを集め、活性のブロック数が要約と同じ。写しは全部の刻みに GPU の時間があり、活性のブロック数が基準の同じ刻みと
//     同じ(GPU の時間は揺れるので、決定的な値 = 刻みの数とブロック数で比べる)。写しを取っても本物のブロック数は基準と同じ。
//     重さの検査(frame::CheckWeight)が全部の刻みを判定し、ブロックの和が基準と同じ
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
#include "frame/weight_check.h"
#include "gpu/device.h"
#include "gpu/queue.h"
#include "gpu_test_options.h"
#include "reaction_test_table.h"
#include "sim/physics_world.h"
#include "sim/probe_sim.h"
#include "sim/tick_cost.h"
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
    constexpr uint64_t THRESHOLD_SAMPLE = 4;      // 2 つ目の閾値 = CPU の最高温度の道のりの、この番目の読み戻しの値
    constexpr uint32_t SAMPLE_INTERVAL = 10;
    constexpr uint64_t COPY_TICKS_PER_FRAME = 16;  // 読み戻す刻み(10 刻みごと)で切れる
    constexpr std::array<uint32_t, 5> FRAME_UNITS = {3, 7, 5, 11, 4};

    constexpr int64_t METER = int64_t{1} << 20;
    constexpr int32_t UNIT = 1 << 30;                 // Q1.30 の 1
    constexpr uint32_t PUSH_IMPULSE_MNS = 5'000'000;  // 5000 N·s

    // 押す(CPU の PhysicsWorld::Push にも同じ値を渡す)
    struct Push {
        uint64_t tick = 0;
        uint32_t sequence = 0;
        std::array<int64_t, 3> origin{};
        std::array<int32_t, 3> direction{};
    };

    const std::array<Push, 2> PUSHES = {{
        {.tick = FORK_TICK,
         .sequence = 1,
         .origin = {10 * METER, 11 * METER / 2, 6 * METER},
         .direction = {0, 0, UNIT}},
        {.tick = 70,
         .sequence = 4,
         .origin = {10 * METER + 3 * METER / 10, 44 * METER / 5, 26 * METER},
         .direction = {0, 0, -UNIT}},
    }};

    ProbeCommand MakePush(const Push& push) {
        return MakePushCommand(push.tick, push.sequence, push.origin, push.direction, PUSH_IMPULSE_MNS);
    }

    // 刻みの順・番号の順(GPU のキューの約束)
    std::vector<ProbeCommand> MakeCommands() {
        return {
            MakePokeCommand(0, 0, 28, 32, 32),  // 木箱の壁に火をつける
            MakePush(PUSHES[0]),
            MakePokeCommand(FORK_TICK, 2, 36, 32, 32),  // 写しの始めの刻みのコマンド(写しにも当たる)
            MakePokeCommand(DIVERGING_POKE_TICK, 3, 20, 40, 24),
            MakePush(PUSHES[1]),
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

    // CPU リファレンスで見た道のり(刻み → セルのハッシュ、読み戻す刻み → 木の量・最高温度)
    struct ReferencePath {
        std::map<uint64_t, uint64_t> cellHashes;
        std::map<uint64_t, uint64_t> wood;
        std::map<uint64_t, uint64_t> peak;  // 最高温度(mK)
    };

    // CPU の世界: セル(ProbeReference)と物(PhysicsWorld)。物理の刻み t の熱の入口を刻み t + 1 の初めにセルへ足す(GPU と同じ。T-0561)
    struct CpuWorld {
        ProbeReference reference;
        PhysicsWorld physics;
        std::vector<physics::PxHeatEntry> heat;  // 前の刻みの物理の熱の入口

        CpuWorld(const BakedReactionTable& table, const PhysicsScene& scene)
            : reference(table), physics(scene, PhysicsParametersFor(StandardWorldPhysics())) {
        }

        // due: この刻みのコマンド(押すのは pushes が true の時だけ。コマンドを当てない写しは押さない)
        void Advance(uint64_t tick, std::span<const ProbeCommand> due, bool pushes) {
            reference.Advance(tick, due, nullptr, heat);
            for (const Push& push : PUSHES) {
                if (pushes && push.tick == tick) {
                    physics.Push(physics::PxMakeVec3(push.origin[0], push.origin[1], push.origin[2]),
                                 physics::PxMakeVec3(push.direction[0], push.direction[1], push.direction[2]),
                                 PUSH_IMPULSE_MNS);
                }
            }

            physics.Step();
            heat = physics.HeatEntries();
        }
    };

    void Record(const ProbeReference& reference, uint64_t tick, uint64_t sampleStart, uint32_t cellulose,
                ReferencePath& path) {
        path.cellHashes[tick] = reference.StateHash(tick);
        if (tick >= sampleStart && (tick - sampleStart) % SAMPLE_INTERVAL == 0) {
            path.wood[tick] = ProbeSpeciesAmount(reference.State(tick), cellulose);
            path.peak[tick] = ProbeMaxTemperature(reference.Caches(tick));
        }
    }

    // CPU リファレンス: コマンドありで TICKS まで(full)と、SECOND_TICK からコマンドなしで SECOND_LIMIT 刻み(second)
    std::pair<ReferencePath, ReferencePath> RunReference(const BakedReactionTable& table, const PhysicsScene& scene,
                                                         uint32_t cellulose) {
        const std::vector<ProbeCommand> commands = MakeCommands();
        CpuWorld world(table, scene);
        std::optional<CpuWorld> second;
        ReferencePath full;
        ReferencePath without;

        for (uint64_t tick = 0; tick < TICKS; ++tick) {
            if (tick == SECOND_TICK) {
                second = world;
                Record(second->reference, tick, SECOND_TICK, cellulose, without);
            }

            std::vector<ProbeCommand> due;
            rng::copy_if(commands, std::back_inserter(due),
                         [&](const ProbeCommand& command) { return command.targetTick == tick; });
            world.Advance(tick, due, true);
            Record(world.reference, tick + 1, FORK_TICK, cellulose, full);
        }

        for (uint64_t tick = SECOND_TICK; tick < SECOND_TICK + SECOND_LIMIT; ++tick) {
            second->Advance(tick, {}, false);
            Record(second->reference, tick + 1, SECOND_TICK, cellulose, without);
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
        Driver(gpu::Queue& queue, ProbeSim& simulation)
            : m_queue(queue), m_simulation(simulation), m_costs(simulation.UnitsPerTick()) {}

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
                for (const ProbeTickHash& hash : readback.hashes) {
                    m_hashes[hash.tick] = hash.WorldHash();
                    m_blocks[hash.tick] = hash.scheduledBlocks;
                }

                // 刻みごとの重さ(刻みはフレームをまたぐ。T-0413)
                std::vector<TickCost> costs;
                m_costsMatched = m_costs.Add(readback, costs) && m_costsMatched;
                for (const TickCost& cost : costs)
                    m_costBlocks[cost.tick] = cost.gpuTimestampTicks > 0 ? cost.scheduledBlocks : UINT32_MAX;

                m_debugAssertCount += readback.debugAssertCount;
            }

            return true;
        }

        [[nodiscard]] const std::map<uint64_t, uint64_t>& Hashes() const { return m_hashes; }
        [[nodiscard]] uint32_t DebugAssertCount() const { return m_debugAssertCount; }
        [[nodiscard]] const std::map<uint64_t, uint32_t>& Blocks() const { return m_blocks; }
        // 重さを集めた刻みの活性のブロック数(GPU の時間が 0 なら UINT32_MAX)
        [[nodiscard]] const std::map<uint64_t, uint32_t>& CostBlocks() const { return m_costBlocks; }
        [[nodiscard]] bool CostsMatched() const { return m_costsMatched; }

    private:
        gpu::Queue& m_queue;
        ProbeSim& m_simulation;
        uint64_t m_position = 0;
        uint64_t m_frame = 0;
        std::map<uint64_t, uint64_t> m_hashes;
        std::map<uint64_t, uint32_t> m_blocks;
        TickCostAccumulator m_costs;
        std::map<uint64_t, uint32_t> m_costBlocks;
        bool m_costsMatched = true;
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
        std::map<uint64_t, uint64_t> peak;
        std::map<uint64_t, uint32_t> blocks;     // 活性のブロック数(T-0413)
        std::vector<frame::WeightTick> weights;  // 刻みごとの重さ(GPU の時間が 0 の刻みは入れない)
        bool weightsAligned = false;             // tickGpuTimestampTicks が hashes と同じ数
    };

    void CollectHashes(const WorldCopyResult& result, CopyPath& path, double frequency) {
        path.weightsAligned = result.tickGpuTimestampTicks.size() == result.hashes.size();
        for (size_t index = 0; index < result.hashes.size(); ++index) {
            const ProbeTickHash& hash = result.hashes[index];
            path.worldHashes[hash.tick] = hash.WorldHash();
            path.cellHashes[hash.tick] = hash.hash;
            path.blocks[hash.tick] = hash.scheduledBlocks;
            const uint64_t gpuTicks = path.weightsAligned ? result.tickGpuTimestampTicks[index] : 0;
            if (gpuTicks > 0) {
                path.weights.push_back({.tick = hash.tick,
                                        .gpuMilliseconds = static_cast<double>(gpuTicks) * 1000.0 / frequency,
                                        .scheduledBlocks = hash.scheduledBlocks});
            }
        }
    }

    // 刻み from〜to のブロック数が全部同じ刻みの数(どちらかに無ければ違うと数える)
    uint64_t CountSameBlocks(const std::map<uint64_t, uint32_t>& got, const std::map<uint64_t, uint32_t>& expected,
                             uint64_t from, uint64_t to) {
        uint64_t same = 0;
        for (uint64_t tick = from; tick <= to; ++tick) {
            const auto a = got.find(tick);
            const auto b = expected.find(tick);
            if (a != got.end() && b != expected.end() && a->second == b->second)
                ++same;
        }

        return same;
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
        const auto [referenceFull, referenceSecond] = RunReference(table, scene, cellulose);
        const uint64_t threshold = std::next(referenceSecond.peak.begin(), THRESHOLD_SAMPLE)->second;
        uint64_t expectedEnd = 0;
        for (const auto& [tick, amount] : referenceSecond.peak) {
            if (amount <= threshold) {
                expectedEnd = tick;
                break;
            }
        }

        Log(Channel::Sim, Level::Info, "CPU: 刻み {} からの木の量 {} → {}・最高温度 {} → {} mK(閾値 {}・着く刻み {})",
            SECOND_TICK, referenceSecond.wood.begin()->second, referenceSecond.wood.rbegin()->second,
            referenceSecond.peak.begin()->second, referenceSecond.peak.rbegin()->second, threshold, expectedEnd);

        // --- 写しを取りながら本物を流す ---
        auto copy = WorldCopy::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, real->simulation->Table(),
                                      SimOptions(scene));
        failures.Check(copy.has_value(), "写しを作れた");
        if (!copy)
            return failures.count;

        const double frequency = static_cast<double>(real->queue->TimestampFrequency());
        CopyPath first;
        CopyPath second;
        uint32_t starts = 0;
        const std::array<uint64_t, 2> breaks = {FORK_TICK, SECOND_TICK};
        const auto onFrame = [&](uint64_t tick, bool atBoundary) {
            copy->Complete();
            if (!copy->Initialized())
                return copy->RecordInitialization();

            if (starts == 1 && copy->Finished() && first.worldHashes.empty())
                CollectHashes(copy->Result(), first, frequency);  // 使い直すと結果は空になる

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

                                         path.wood[sample.tick] = ProbeSpeciesAmount(sample.cells, cellulose);
                                         const uint64_t peak = ProbeMaxTemperature(sample.thermal);
                                         path.peak[sample.tick] = peak;
                                         return !startFirst && peak <= threshold;
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
        CollectHashes(copy->Result(), second, frequency);

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
        failures.Check(first.wood == referenceFull.wood && first.peak == referenceFull.peak,
                       "読み戻した木の量と最高温度が CPU と同じ(10 刻みごと)");

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
            failures.Check(referenceSecond.wood.contains(tick) && referenceSecond.wood.at(tick) == amount &&
                               second.peak.at(tick) == referenceSecond.peak.at(tick),
                           "使い直した写しの木の量と最高温度が CPU と同じ");
        }

        // --- 重さの集め方(T-0413。決定的な値 = 刻みの数と活性のブロック数で比べる)---
        const uint64_t firstTicks = TICKS - FORK_TICK;
        failures.Check(driver.CostsMatched() && baseDriver.CostsMatched(),
                       "本物の終えた刻みと要約の数が毎フレーム合う");
        failures.Check(CountSameBlocks(driver.CostBlocks(), driver.Blocks(), 1, TICKS) == TICKS,
                       "本物(刻みがフレームをまたぐ)の全部の刻みの重さを集め、ブロック数が要約と同じ");
        failures.Check(CountSameBlocks(driver.Blocks(), baseDriver.Blocks(), 1, TICKS) == TICKS,
                       "写しを取っても本物の活性のブロック数が基準と同じ");
        failures.Check(first.weightsAligned && first.weights.size() == firstTicks && second.weightsAligned &&
                           second.weights.size() == second.blocks.size(),
                       "写しの全部の刻みに GPU の時間がある");
        failures.Check(CountSameBlocks(first.blocks, baseDriver.Blocks(), FORK_TICK + 1, TICKS) == firstTicks,
                       "写しの活性のブロック数が基準の同じ刻みと同じ");

        uint64_t baselineBlocks = 0;
        for (uint64_t tick = FORK_TICK + 1; tick <= TICKS; ++tick)
            baselineBlocks += baseDriver.Blocks().at(tick);

        const frame::WeightReport weight = frame::CheckWeight(first.weights, {.tickMilliseconds = 1000.0});
        Log(Channel::Sim, Level::Info, "1 つ目の写しの {}", frame::DescribeWeight(weight, 60.0));
        failures.Check(weight.ticks == firstTicks && weight.blockSum == baselineBlocks &&
                           weight.verdict == frame::WeightVerdict::Fits,
                       "重さの検査が全部の刻みを判定し、ブロックの和が基準と同じ");

        // --- 重さ ---
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
