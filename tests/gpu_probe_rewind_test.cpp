// gpu_probe_rewind_test.cpp — 巻き戻し(保存点 + 再生。T-0143・ADR-0036)の決定性。
//
// 確かめること(仮の世界 + 積み木。最初のフレームで、つつき・押すコマンドを全部 GPU のキューへ足す):
//   - 基準: 別の ProbeSim で最初から TICKS 刻み流した、刻みごとの世界のハッシュ(セル + 物)
//   - 巻き戻す側: 刻み 20・60・100 の境界で保存点へ写しながら 120 刻みまで進み、
//       1. 保存点(刻み 20)へ戻し、刻み 20 以降のコマンドを足し直して TICKS まで → S(21)〜S(TICKS) が基準と全部一致
//          (戻すときにキューで待っていたコマンドを捨てられること。戻した後も 60・100 で写し直す)
//       2. 保存点(刻み 100)へ戻して TICKS まで → 一致(巻き戻した後にもう一度巻き戻す)
//       3. 保存点(刻み 60)へ戻し、コマンドを足し直さずに進める → 刻み 70 のつつきの後で基準と違う(試験が効いていることの確かめ)
//   - フレームの単位の数はばらばら(刻みの途中で切れる)。保存点の刻みでだけ、境界で切ってから写す
//   - シェーダーの assert と debug layer のエラーが 0 件
// 保存点 1 つの大きさと、写す・戻すフレームの GPU の時間をログに出す(docs/perf.md)。
// 引数: gpu_test_options.h(--warp)
#include <algorithm>
#include <array>
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
#include "sim/probe_sim.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    constexpr uint64_t TICKS = 140;
    constexpr uint64_t FIRST_LEG_TICKS = 120;                      // 巻き戻す前に進む刻み
    constexpr std::array<uint64_t, 3> SAVE_TICKS = {20, 60, 100};  // 保存点 i に写す刻み
    constexpr uint64_t DIVERGING_POKE_TICK = 70;  // 足し直さないと違いが出る最初のコマンド(保存点 1 の後)
    constexpr std::array<uint32_t, 5> FRAME_UNITS = {3, 7, 5, 11, 4};
    constexpr uint64_t FAR_TICK = uint64_t{1} << 40;  // 保存点の無い時の「次の境界」

    constexpr int64_t METER = int64_t{1} << 20;
    constexpr int32_t UNIT = 1 << 30;                 // Q1.30 の 1
    constexpr uint32_t PUSH_IMPULSE_MNS = 5'000'000;  // 5000 N·s

    // 刻みの順・番号の順(GPU のキューの約束)
    std::vector<ProbeCommand> MakeCommands() {
        return {
            MakePokeCommand(0, 0, 28, 32, 32),  // 木箱の壁に火をつける
            MakePushCommand(30, 1, {10 * METER, 11 * METER / 2, 6 * METER}, {0, 0, UNIT}, PUSH_IMPULSE_MNS),
            MakePokeCommand(45, 2, 36, 32, 32),
            MakePokeCommand(DIVERGING_POKE_TICK, 3, 20, 40, 24),
            MakePushCommand(90, 4, {10 * METER + 3 * METER / 10, 44 * METER / 5, 26 * METER}, {0, 0, -UNIT},
                            PUSH_IMPULSE_MNS),
            MakePokeCommand(110, 5, 32, 20, 40),
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

    // 1 つの ProbeSim を 1 フレームずつ投げて待つ(並べない。巻き戻しの正しさだけを見る)
    class Driver {
    public:
        Driver(gpu::Queue& queue, ProbeSim& simulation) : m_queue(queue), m_simulation(simulation) {}

        // 今の境界(m_tick)から刻み to の境界まで。first の最初のフレームで restoreFrom から戻し、commands を足す。
        // 保存点の刻みに来たら、そのフレームを境界で切ってから次のフレームの先頭で写す(save = true のとき)
        bool Advance(uint64_t to, std::span<const ProbeCommand> commands, uint32_t restoreFrom, bool save) {
            const uint32_t unitsPerTick = m_simulation.UnitsPerTick();
            const uint64_t end = to * unitsPerTick;
            bool first = true;

            while (m_position < end) {
                const uint64_t tick = m_position / unitsPerTick;
                const auto unit = static_cast<uint32_t>(m_position % unitsPerTick);
                const uint32_t saveIndex = save && unit == 0 && !first ? SaveIndex(tick) : NO_SAVE_POINT;
                uint64_t units = std::min<uint64_t>(FRAME_UNITS[m_frame % FRAME_UNITS.size()], end - m_position);
                units = std::min(units, NextSaveBoundary(tick, save) * unitsPerTick - m_position);

                const ProbeFrameInput input{.firstTick = tick,
                                            .firstUnit = unit,
                                            .unitCount = static_cast<uint32_t>(units),
                                            .commands = first ? commands : std::span<const ProbeCommand>(),
                                            .saveTo = saveIndex,
                                            .restoreFrom = first ? restoreFrom : NO_SAVE_POINT};
                if (!Submit(input, saveIndex != NO_SAVE_POINT || input.restoreFrom != NO_SAVE_POINT))
                    return false;

                m_position += units;
                first = false;
            }

            return true;
        }

        // 保存点へ戻す前に、境界を保存点の刻みに合わせる
        void JumpTo(uint64_t tick) { m_position = tick * m_simulation.UnitsPerTick(); }

        // 集めたハッシュを取り出して空にする(刻み → 世界のハッシュ)
        std::map<uint64_t, uint64_t> TakeHashes() { return std::exchange(m_hashes, {}); }

        [[nodiscard]] uint32_t DebugAssertCount() const { return m_debugAssertCount; }
        [[nodiscard]] double SaveRestoreMicroseconds() const { return m_saveRestoreMicroseconds; }
        [[nodiscard]] uint32_t SaveRestoreFrames() const { return m_saveRestoreFrames; }

    private:
        static uint32_t SaveIndex(uint64_t tick) {
            const auto found = rng::find(SAVE_TICKS, tick);
            return found == SAVE_TICKS.end() ? NO_SAVE_POINT : static_cast<uint32_t>(found - SAVE_TICKS.begin());
        }

        // 刻み tick の途中(か始め)より後の、最初の保存点の刻み(無ければ十分先)。始めにいる保存点はこのフレームの先頭で写す
        static uint64_t NextSaveBoundary(uint64_t tick, bool save) {
            if (save) {
                for (const uint64_t saveTick : SAVE_TICKS) {
                    if (saveTick > tick)
                        return saveTick;
                }
            }

            return FAR_TICK;
        }

        bool Submit(const ProbeFrameInput& input, bool measure) {
            ID3D12CommandList* list = m_simulation.RecordFrame(0, input);
            if (list == nullptr)
                return false;

            const uint64_t fence = m_queue.Submit(list);
            if (!m_queue.WaitCpu(fence))
                return false;

            const ProbeFrameReadback readback = m_simulation.ReadFrame(0);
            for (const ProbeTickHash& hash : readback.hashes)
                m_hashes[hash.tick] = hash.WorldHash();

            m_debugAssertCount += readback.debugAssertCount;
            ++m_frame;
            if (measure) {
                // 写す・戻すフレーム全体(コピー + 単位)の時間。単位の時間を引いて写す・戻すだけの目安にする
                uint64_t unitTicks = 0;
                for (const uint64_t ticks : readback.unitGpuTicks)
                    unitTicks += ticks;

                const uint64_t total = readback.gpuEndTimestamp - readback.gpuBeginTimestamp;
                m_saveRestoreMicroseconds += static_cast<double>(total - std::min(total, unitTicks)) * 1'000'000.0 /
                                             static_cast<double>(m_queue.TimestampFrequency());
                ++m_saveRestoreFrames;
            }

            return true;
        }

        gpu::Queue& m_queue;
        ProbeSim& m_simulation;
        uint64_t m_position = 0;  // 投げた単位の数(刻み × 単位の数 + 単位)
        uint64_t m_frame = 0;
        std::map<uint64_t, uint64_t> m_hashes;
        uint32_t m_debugAssertCount = 0;
        double m_saveRestoreMicroseconds = 0.0;
        uint32_t m_saveRestoreFrames = 0;
    };

    // segment の刻みが全部 baseline と一致し、from + 1 〜 to が揃っているか
    bool MatchesBaseline(const std::map<uint64_t, uint64_t>& segment, const std::map<uint64_t, uint64_t>& baseline,
                         uint64_t from, uint64_t to, std::string_view label) {
        uint32_t mismatches = 0;
        uint64_t firstMismatch = UINT64_MAX;
        for (uint64_t tick = from + 1; tick <= to; ++tick) {
            const auto got = segment.find(tick);
            const auto expected = baseline.find(tick);
            if (got != segment.end() && expected != baseline.end() && got->second == expected->second)
                continue;

            ++mismatches;
            firstMismatch = std::min(firstMismatch, tick);
        }

        Log(Channel::Sim, Level::Info, "{}: S({})〜S({}) のうち一致しない刻み {}(最初 {})", label, from + 1, to,
            mismatches, firstMismatch == UINT64_MAX ? 0 : firstMismatch);

        return mismatches == 0;
    }

    struct Created {
        std::unique_ptr<gpu::Queue> queue;
        std::unique_ptr<ProbeSim> simulation;
    };

    std::optional<Created> Create(ID3D12Device5* device, const BakedReactionTable& table, const PhysicsScene& scene,
                                  uint32_t savePoints) {
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"RewindTestSim");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table, {.physicsScene = &scene});
        if (!queue || !simulation) {
            Log(Channel::Sim, Level::Error, "作れない: {}{}", queue ? "" : queue.error(),
                simulation ? "" : simulation.error());
            return std::nullopt;
        }

        if (savePoints > 0 && !simulation->CreateSavePoints(device, savePoints))
            return std::nullopt;

        return Created{.queue = std::make_unique<gpu::Queue>(std::move(*queue)),
                       .simulation = std::make_unique<ProbeSim>(std::move(*simulation))};
    }

    int RunTest(ID3D12Device5* device, const BakedReactionTable& table) {
        const PhysicsScene scene = MakeProbeStackScene();
        const std::vector<ProbeCommand> commands = MakeCommands();
        std::optional<Created> base = Create(device, table, scene, 0);
        std::optional<Created> rewind = Create(device, table, scene, static_cast<uint32_t>(SAVE_TICKS.size()));
        Failures failures;
        failures.Check(base && rewind, "作れた");
        if (!base || !rewind)
            return failures.count;

        Log(Channel::Sim, Level::Info, "保存点 1 つ: {:.1f} MiB(物 {})",
            static_cast<double>(rewind->simulation->SavePointBytes()) / (1024.0 * 1024.0), scene.bodies.size());

        // --- 基準 ---
        Driver baseDriver(*base->queue, *base->simulation);
        failures.Check(baseDriver.Advance(TICKS, commands, NO_SAVE_POINT, false), "基準を流せた");
        const std::map<uint64_t, uint64_t> baseline = baseDriver.TakeHashes();

        // --- 写しながら進む → 戻す ---
        Driver driver(*rewind->queue, *rewind->simulation);
        ProbeSim& simulation = *rewind->simulation;
        failures.Check(driver.Advance(FIRST_LEG_TICKS, commands, NO_SAVE_POINT, true), "保存点を写しながら進めた");
        failures.Check(MatchesBaseline(driver.TakeHashes(), baseline, 0, FIRST_LEG_TICKS, "写しながら"),
                       "保存点を写しても世界のハッシュ列が変わらない");
        for (uint32_t index = 0; index < SAVE_TICKS.size(); ++index)
            failures.Check(simulation.SavePointTick(index) == SAVE_TICKS[index], "保存点が決まった刻みを持つ");

        // 1. 刻み 20 へ(キューには 30 以降のコマンドが待っている)
        driver.JumpTo(SAVE_TICKS[0]);
        const std::vector<ProbeCommand> from20 = CommandsFrom(SAVE_TICKS[0]);
        failures.Check(driver.Advance(TICKS, from20, 0, true), "刻み 20 へ戻して進めた");
        failures.Check(MatchesBaseline(driver.TakeHashes(), baseline, SAVE_TICKS[0], TICKS, "刻み 20 から"),
                       "刻み 20 へ巻き戻した後のハッシュ列が、最初から流した時と同じ");

        // 2. 刻み 100 へ(戻した後に写し直した保存点)
        driver.JumpTo(SAVE_TICKS[2]);
        failures.Check(driver.Advance(TICKS, CommandsFrom(SAVE_TICKS[2]), 2, false), "刻み 100 へ戻して進めた");
        failures.Check(MatchesBaseline(driver.TakeHashes(), baseline, SAVE_TICKS[2], TICKS, "刻み 100 から"),
                       "もう一度巻き戻した後のハッシュ列も同じ");

        // 3. 刻み 60 へ戻し、コマンドを足し直さない → つつきの刻みの後で違う
        driver.JumpTo(SAVE_TICKS[1]);
        failures.Check(driver.Advance(TICKS, {}, 1, false), "刻み 60 へ戻して(コマンドなしで)進めた");
        const std::map<uint64_t, uint64_t> withoutCommands = driver.TakeHashes();
        failures.Check(MatchesBaseline(withoutCommands, baseline, SAVE_TICKS[1], DIVERGING_POKE_TICK, "足し直さず(前)"),
                       "足し直さなくても、つつきの刻みまでは同じ");
        failures.Check(withoutCommands.contains(DIVERGING_POKE_TICK + 1) &&
                           withoutCommands.at(DIVERGING_POKE_TICK + 1) != baseline.at(DIVERGING_POKE_TICK + 1),
                       "足し直さないと、つつきの後で違う(戻すとキューのコマンドが捨てられている)");

        failures.Check(baseDriver.DebugAssertCount() == 0 && driver.DebugAssertCount() == 0,
                       "シェーダーの assert が 0 件");
        Log(Channel::Sim, Level::Info, "写す・戻すフレームの単位以外の GPU 時間: 平均 {:.0f} µs({} フレーム。暖機なし)",
            driver.SaveRestoreMicroseconds() / std::max(1u, driver.SaveRestoreFrames()), driver.SaveRestoreFrames());

        return failures.count;
    }

}  // namespace

int main(int argc, char** argv) {
    std::vector<char*> arguments(argv, argv + argc);
    const std::optional<test::GpuTestOptions> options = test::ParseGpuTestOptions(std::span(arguments));
    const auto table = BakeReactionTable(MakeCombustionTestTable());
    if (!options || !table) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_probe_rewind_test [--warp] [--no-gbv]");
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    // 物理のシェーダーは GPU-based validation の計装に数分かかるので切る(gpu_probe_physics_test と同じ。debug layer は有効)
    gpu::DeviceOptions deviceOptions = test::TestDeviceOptions(*options);
    deviceOptions.gpuBasedValidation = false;
    auto device = gpu::Device::Create(options->adapter, deviceOptions);
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    Log(Channel::Sim, Level::Info, "仮の世界 + 積み木を {} 刻み。保存点 {} {} {}", TICKS, SAVE_TICKS[0], SAVE_TICKS[1],
        SAVE_TICKS[2]);
    const int failures = RunTest(device->Get(), *table);
    const bool passed = failures == 0 && test::PassesValidation(*device, "gpu_probe_rewind_test");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
