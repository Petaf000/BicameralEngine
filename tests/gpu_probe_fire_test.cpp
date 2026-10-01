// gpu_probe_fire_test.cpp — 仮の世界(sim/probe_sim)の木箱に火をつけて長く走らせる(T-0089、02 のテストの「燃焼の場面を 10 分」)。
//
// 確かめること:
//   - 36,000 刻み(世界時間 10 分)の間、刻みごとに「エネルギーの合計 = 前の合計 + つつきの分」(伝導と反応で保存。D-206)
//   - 最後の全部のセルを読み戻し、元素ごとの数が初めの世界と完全に同じ
//   - 同じ入力でもう一度走らせ、最初の REPEAT_TICKS 刻みのハッシュ列が同じ(決定性)
//   - 木箱が燃え広がる(壁のセルの大半が炭になる)。燃え方の経過(1 秒ごと)をログに出す(伝導率の試験の値を決めるため)
//   - debug layer のエラーとシェーダーの assert が 0 件
// 走らせ方: ctest(gpu)か `job.py run -Preset release -Exe gpu_probe_fire_test`(release の方がずっと速い)。
// 引数: gpu_test_options.h(--warp は使えない。ADR-0013)、--ticks n(既定 36000)、
//   燃え方を試す値(試験の表の値を決めるため。既定は表のまま): --pokes n(火をつける回数。同じセルを刻み 0 に n 回)・
//   --solid-percent p・--gas-percent p(固体・気体の熱伝導率を試験の表の p %にする)
#include <algorithm>
#include <charconv>
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
#include "sim/probe_sim.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    constexpr uint64_t DEFAULT_TICKS = 36000;
    constexpr uint64_t REPEAT_TICKS = 3600;
    constexpr uint32_t TICKS_PER_FRAME = 60;  // 1 フレーム = 世界時間 1 秒(単位 180 個 ≤ MAX_UNITS_PER_FRAME)
    constexpr std::array<uint32_t, 3> IGNITION_CELL = {28, 32, 32};  // 木箱の壁(sim/probe_sim.cpp の初めの世界)

    // 「燃えた」壁のセル: 炭が 1e8 µmol 以上(壁のセルが全部炭になると約 2.3e8)。「燃えている」: 600 K 以上
    constexpr uint32_t BURNT_CARBON_MICROMOLES = 100000000;
    constexpr uint32_t BURNING_MILLIKELVIN = 600000;

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition)
                return;

            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    // --- 読み戻し ---

    template <typename T>
    std::vector<T> ReadBack(ID3D12Device5* device, ID3D12Resource* source, uint64_t offsetBytes, size_t count) {
        std::vector<T> values(count);
        const uint64_t bytes = count * sizeof(T);
        auto queue = gpu::ImmediateQueue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        const ComPtr<ID3D12Resource> readback = gpu::CreateBuffer(device, bytes, gpu::BufferKind::Readback);
        if (!queue || !readback)
            return {};

        ID3D12GraphicsCommandList10* list = queue->Begin();
        if (list == nullptr)
            return {};

        list->CopyBufferRegion(readback.Get(), 0, source, offsetBytes, bytes);
        if (!queue->ExecuteAndWait() || !gpu::ReadBuffer(readback.Get(), std::as_writable_bytes(std::span(values))))
            return {};

        return values;
    }

    // --- 燃え方の経過 ---

    struct FireSnapshot {
        uint32_t burntWalls = 0;
        uint32_t burningCells = 0;
        uint32_t maxMilliKelvin = 0;
    };

    FireSnapshot Summarize(std::span<const uint32_t> extraction, std::span<const uint8_t> walls) {
        FireSnapshot snapshot;
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            const uint32_t temperature = extraction[size_t{index} * PROBE_EXTRACTION_CELL_WORDS];
            const uint32_t carbon = extraction[size_t{index} * PROBE_EXTRACTION_CELL_WORDS + 3];
            snapshot.maxMilliKelvin = std::max(snapshot.maxMilliKelvin, temperature);
            snapshot.burningCells += temperature >= BURNING_MILLIKELVIN ? 1 : 0;
            snapshot.burntWalls += walls[index] != 0 && carbon >= BURNT_CARBON_MICROMOLES ? 1 : 0;
        }

        return snapshot;
    }

    // --- 走らせる ---

    struct FireRun {
        bool ok = false;
        std::vector<ProbeTickHash> hashes;
        bool energyConserved = true;
        uint32_t debugAssertCount = 0;
        FireSnapshot last;
        uint64_t firstFullyBurntSecond = 0;  // 壁の 9 割が燃えた最初の秒(0 なら無い)
        double conductMicroseconds = 0.0;    // 伝導と反応の単位の GPU 時間の合計
        std::vector<reaction::RxCell> finalCells;
    };

    // 1 フレーム(TICKS_PER_FRAME 刻み)の読み戻しを確かめて足す。energy は前の状態のエネルギーの合計(進める)
    void AccumulateFrame(FireRun& run, const ProbeFrameReadback& readback, uint32_t unitsPerTick, double usPerTimestamp,
                         uint64_t& energy) {
        for (const ProbeTickHash& state : readback.hashes) {
            run.energyConserved = run.energyConserved && state.energy == energy + state.sourceEnergy;
            energy = state.energy;
        }

        for (size_t tick = 0; tick * unitsPerTick < readback.unitGpuTicks.size(); ++tick) {
            run.conductMicroseconds += static_cast<double>(
                                           readback.unitGpuTicks[tick * unitsPerTick + PROBE_UNIT_CONDUCT]) *
                                       usPerTimestamp;
        }

        run.hashes.insert(run.hashes.end(), readback.hashes.begin(), readback.hashes.end());
        run.debugAssertCount += readback.debugAssertCount;
    }

    struct FireOptions {
        uint64_t ticks = DEFAULT_TICKS;
        uint32_t pokes = 1;
        uint32_t solidPercent = 100;
        uint32_t gasPercent = 100;
    };

    FireRun RunFire(ID3D12Device5* device, const BakedReactionTable& table, uint64_t ticks, uint32_t pokes,
                    bool logTimeline) {
        FireRun run;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"FireTestSim");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table);
        if (!queue || !simulation) {
            Log(Channel::Sim, Level::Error, "作れない: {}{}", queue ? "" : queue.error(),
                simulation ? "" : simulation.error());
            return run;
        }

        const std::vector<reaction::RxCell> initial = MakeProbeInitialWorld(table);
        const uint32_t cellulose = table.SpeciesId("cellulose");
        std::vector<uint8_t> walls(PROBE_CELL_COUNT, 0);
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index)
            walls[index] = ProbeViewAmount(initial[index], cellulose) > 0 ? 1 : 0;

        const auto wallCount = static_cast<uint32_t>(rng::count(walls, uint8_t{1}));
        std::vector<ProbeCommand> ignition;
        ignition.reserve(pokes);
        for (uint32_t sequence = 0; sequence < pokes; ++sequence)
            ignition.push_back(MakePokeCommand(0, sequence, IGNITION_CELL[0], IGNITION_CELL[1], IGNITION_CELL[2]));

        uint64_t energy = ProbeEnergySum(initial);
        const double usPerTimestamp = 1'000'000.0 / static_cast<double>(queue->TimestampFrequency());
        const uint32_t unitsPerTick = simulation->UnitsPerTick();

        for (uint64_t tick = 0; tick < ticks; tick += TICKS_PER_FRAME) {
            const uint64_t frame = tick / TICKS_PER_FRAME;
            const auto slot = static_cast<uint32_t>(frame % ProbeSim::FRAME_SLOT_COUNT);
            const auto target = static_cast<uint32_t>(frame % PROBE_EXTRACTION_COUNT);
            const auto count = static_cast<uint32_t>(std::min<uint64_t>(TICKS_PER_FRAME, ticks - tick));
            ID3D12CommandList* list = simulation->RecordFrame(
                slot,
                {.firstTick = tick,
                 .unitCount = count * unitsPerTick,
                 .extract = true,
                 .extractionTarget = target,
                 .commands = tick == 0 ? std::span<const ProbeCommand>(ignition) : std::span<const ProbeCommand>()});

            if (list == nullptr || !queue->WaitCpu(queue->Submit(list)))
                return run;

            AccumulateFrame(run, simulation->ReadFrame(slot), unitsPerTick, usPerTimestamp, energy);

            const std::vector<uint32_t> extraction = ReadBack<uint32_t>(device, simulation->Extraction(target), 0,
                                                                        PROBE_EXTRACTION_BLOCK_OFFSET);
            if (extraction.empty())
                return run;

            run.last = Summarize(extraction, walls);
            const uint64_t second = (tick + count) / TICKS_PER_FRAME;
            if (run.firstFullyBurntSecond == 0 && run.last.burntWalls * 10 >= wallCount * 9)
                run.firstFullyBurntSecond = second;

            const bool logThis = second <= 30 || second % 30 == 0;
            if (logTimeline && logThis) {
                Log(Channel::Sim, Level::Info,
                    "  {:>4} 秒: 燃えた壁 {:>3} / {}  600 K 以上のセル {:>5}  最高 {:.0f} K  計算したブロック {}",
                    second, run.last.burntWalls, wallCount, run.last.burningCells,
                    static_cast<double>(run.last.maxMilliKelvin) / 1000.0,
                    run.hashes.empty() ? 0 : run.hashes.back().scheduledBlocks);
            }
        }

        const uint64_t generationOffset = (ticks & 1) * uint64_t{PROBE_CELL_COUNT} * sizeof(reaction::RxCell);
        run.finalCells = ReadBack<reaction::RxCell>(device, simulation->Cells(), generationOffset, PROBE_CELL_COUNT);
        run.ok = run.hashes.size() == ticks && run.finalCells.size() == PROBE_CELL_COUNT;

        return run;
    }

    std::vector<uint64_t> CountAllElements(const BakedReactionTable& table, std::span<const reaction::RxCell> cells) {
        std::vector<uint64_t> total(table.elementNames.size(), 0);
        for (const reaction::RxCell& cell : cells) {
            const std::vector<uint64_t> counts = CountElements(table, cell);
            for (size_t element = 0; element < total.size(); ++element)
                total[element] += counts[element];
        }

        return total;
    }

    // name n を取り除いて読む(無ければ value のまま。残りは gpu_test_options.h)
    template <typename T>
    void TakeNumber(std::vector<char*>& arguments, std::string_view name, T& value) {
        const auto found = rng::find_if(arguments, [name](const char* argument) { return argument == name; });
        if (found == arguments.end() || found + 1 == arguments.end())
            return;

        const std::string_view text = *(found + 1);
        std::from_chars(text.data(), text.data() + text.size(), value);
        arguments.erase(found, found + 2);
    }

    FireOptions TakeFireOptions(std::vector<char*>& arguments) {
        FireOptions options;
        TakeNumber(arguments, "--ticks", options.ticks);
        TakeNumber(arguments, "--pokes", options.pokes);
        TakeNumber(arguments, "--solid-percent", options.solidPercent);
        TakeNumber(arguments, "--gas-percent", options.gasPercent);

        return options;
    }

    // 試験の表の熱伝導率を、固体(セルロース・炭)と気体で別の % にする
    ReactionTableDefinition MakeTable(const FireOptions& options) {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        for (SpeciesDefinition& species : definition.species) {
            const bool solid = species.name == "cellulose" || species.name == "carbon";
            const uint64_t percent = solid ? options.solidPercent : options.gasPercent;
            species.thermalConductivity = static_cast<uint32_t>(uint64_t{species.thermalConductivity} * percent / 100);
        }

        return definition;
    }

}  // namespace

int main(int argc, char** argv) {
    std::vector<char*> arguments(argv, argv + argc);
    const FireOptions fire = TakeFireOptions(arguments);
    const uint64_t ticks = fire.ticks;
    const auto options = test::ParseGpuTestOptions(std::span(arguments));
    const auto table = BakeReactionTable(MakeTable(fire));
    if (!options || !table || ticks < REPEAT_TICKS) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_probe_fire_test [--ticks n(≥ {})]", REPEAT_TICKS);
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    Failures failures;
    Log(Channel::Sim, Level::Info,
        "木箱の壁 ({}, {}, {}) に {} 回火をつけて {} 刻み(世界時間 {} 秒)。熱伝導率 固体 {} %・気体 {} %",
        IGNITION_CELL[0], IGNITION_CELL[1], IGNITION_CELL[2], fire.pokes, ticks, ticks / TICKS_PER_FRAME,
        fire.solidPercent, fire.gasPercent);
    const FireRun run = RunFire(device->Get(), *table, ticks, fire.pokes, true);
    const FireRun repeat = RunFire(device->Get(), *table, REPEAT_TICKS, fire.pokes, false);

    const bool repeatSame = repeat.ok && run.ok &&
                            rng::equal(repeat.hashes, std::span(run.hashes).first(REPEAT_TICKS), {},
                                       &ProbeTickHash::hash, &ProbeTickHash::hash);
    const bool elementsSame = run.ok && CountAllElements(*table, run.finalCells) ==
                                            CountAllElements(*table, MakeProbeInitialWorld(*table));

    Log(Channel::Sim, Level::Info, "{} 刻み: S({}) = {:016x}  壁の 9 割が燃えた {} 秒  伝導と反応 平均 {:.1f} µs/刻み",
        ticks, ticks, run.hashes.empty() ? 0 : run.hashes.back().hash, run.firstFullyBurntSecond,
        run.conductMicroseconds / static_cast<double>(std::max<uint64_t>(ticks, 1)));

    failures.Check(run.ok, "走らせられた");
    failures.Check(run.energyConserved, "刻みごとにエネルギーの合計 = 前の合計 + つつきの分");
    failures.Check(elementsSame, "元素ごとの数が初めの世界と同じ");
    failures.Check(repeatSame, std::format("もう一度走らせて最初の {} 刻みのハッシュ列が同じ", REPEAT_TICKS));
    failures.Check(run.firstFullyBurntSecond > 0, "木箱が燃え広がる(壁の 9 割が炭になる)");
    failures.Check(run.debugAssertCount == 0 && repeat.debugAssertCount == 0, "シェーダーの assert が 0 件");

    const bool passed = failures.count == 0 && test::PassesValidation(*device, "gpu_probe_fire_test");
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_fire_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
