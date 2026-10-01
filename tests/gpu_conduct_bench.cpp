// gpu_conduct_bench.cpp — 伝導と反応の Work Graph(shaders/sim/probe_conduct.hlsl)の時間を、伝播の規模(計算したブロックの数)ごとに測る
// (T-0005・T-0089)。仮の刻み(sim/probe_sim)を 1 フレームに 16 刻みずつ走らせ、単位ごとのタイムスタンプから伝導と反応の単位の GPU 時間を、
// ハッシュの表から計算したブロックの数とエネルギーの合計を取る。規模は最初につつく点で変える:
//   空気の 1 点 / 8 点 / 64 点(空気を温めて、熱の差が流れの切り捨てを下回ると止まる。止まるまでの刻みも測る)/
//   木箱の壁の 1 点(燃え広がる。反応の費用)
// 結果は Markdown の表の行としてログに出す(docs/perf.md に貼る)。エネルギーの合計がつつきの分から変わっていたら失敗。
//
// ctest には登録しない(時間がかかり、時間は機械しだい)。走らせ方: `job.py run -Preset release -Exe gpu_conduct_bench`。
// 引数は gpu_test_options.h(--warp)と、--trace(連鎖のトレースを全部の刻み・格子の全体で有効にして、その費用を測る。T-0087)か
// --trace-idle(容量は確保して範囲は無効。実行中に範囲を変えられるようにしたときの、使っていない間の費用。T-0088)。
// WARP での時間は GPU の目安にならない。
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu/queue.h"
#include "gpu_test_options.h"
#include "sim/probe_sim.h"
#include "sim/probe_trace.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    constexpr uint32_t TICKS_PER_FRAME = 16;
    constexpr uint32_t
        TRACE_CAPACITY = 1u << 20;  // --trace: 1 フレーム(16 刻み)に書ける記録(16 MiB。全部のブロックが活性でも足りる)

    struct Scenario {
        const char* name;
        uint32_t pointsPerAxis;  // つつく点は 1 軸にこの数ずつの格子(1 なら真ん中の 1 点)。0 なら木箱の壁の 1 点
        uint64_t maxTicks;       // ここまでに止まらなければ打ち切る
    };

    constexpr std::array<Scenario, 4> SCENARIOS = {{
        {.name = "空気 1 点", .pointsPerAxis = 1, .maxTicks = 2000},
        {.name = "空気 8 点", .pointsPerAxis = 2, .maxTicks = 600},
        {.name = "空気 64 点", .pointsPerAxis = 4, .maxTicks = 300},
        {.name = "木箱の壁 1 点", .pointsPerAxis = 0, .maxTicks = 3600},
    }};

    constexpr std::array<uint32_t, 3> CRATE_WALL_CELL = {28, 32, 32};  // 木箱の壁(sim/probe_sim.cpp の初めの世界)

    // 計算したブロックの数の区切り(この数以下)
    constexpr std::array<uint32_t, 6> BUCKET_LIMITS = {8, 64, 256, 1024, 2048, PROBE_BLOCK_COUNT};

    struct Bucket {
        uint64_t samples = 0;
        uint64_t blocks = 0;
        double microseconds = 0.0;
    };

    struct ScenarioResult {
        bool ok = false;

        // --- 広がり方 ---
        uint64_t ticks = 0;
        uint64_t quietTick = 0;  // 計算したブロックが 0 になった最初の刻み(0 なら止まらなかった)
        uint32_t maxBlocks = 0;
        bool energyConserved = true;

        // --- 時間(計算したブロックの数の範囲ごと・単位ごと)---
        std::array<Bucket, BUCKET_LIMITS.size()> buckets{};
        std::array<double, PROBE_FIXED_UNITS_PER_TICK> unitMicroseconds{};  // 単位ごとの合計(適用・伝導・検査と出力)
    };

    std::vector<ProbeCommand> MakePokes(uint32_t pointsPerAxis) {
        std::vector<ProbeCommand> commands;
        if (pointsPerAxis == 0) {
            commands.push_back(MakePokeCommand(0, 0, CRATE_WALL_CELL[0], CRATE_WALL_CELL[1], CRATE_WALL_CELL[2]));
            return commands;
        }

        const uint32_t spacing = PROBE_GRID_SIZE / pointsPerAxis;
        const uint32_t count = pointsPerAxis * pointsPerAxis * pointsPerAxis;
        const auto place = [&](uint32_t index) {
            return index * spacing + spacing / 2;
        };

        for (uint32_t sequence = 0; sequence < count; ++sequence) {
            const uint32_t x = sequence % pointsPerAxis;
            const uint32_t y = (sequence / pointsPerAxis) % pointsPerAxis;
            const uint32_t z = sequence / (pointsPerAxis * pointsPerAxis);
            commands.push_back(MakePokeCommand(0, sequence, place(x), place(y), place(z)));
        }

        return commands;
    }

    void AddSample(ScenarioResult& result, uint32_t blocks, double microseconds) {
        if (blocks == 0)
            return;

        for (size_t index = 0; index < BUCKET_LIMITS.size(); ++index) {
            if (blocks > BUCKET_LIMITS[index])
                continue;

            Bucket& bucket = result.buckets[index];
            ++bucket.samples;
            bucket.blocks += blocks;
            bucket.microseconds += microseconds;

            return;
        }
    }

    // 1 フレーム(TICKS_PER_FRAME 刻み)の読み戻しを集計に足す
    // energy: 前の状態のエネルギーの合計(読んだ分だけ進める)
    void AccumulateFrame(ScenarioResult& result, const ProbeFrameReadback& readback, uint32_t unitsPerTick,
                         double microsecondsPerTick, uint64_t& energy) {
        for (uint32_t offset = 0; offset < TICKS_PER_FRAME; ++offset) {
            const ProbeTickHash& state = readback.hashes[offset];
            const size_t firstUnit = size_t{offset} * unitsPerTick;
            const auto unitMicroseconds = [&](uint32_t unit) {
                return static_cast<double>(readback.unitGpuTicks[firstUnit + unit]) * microsecondsPerTick;
            };

            AddSample(result, state.scheduledBlocks, unitMicroseconds(PROBE_UNIT_CONDUCT));
            for (uint32_t unit = 0; unit < unitsPerTick; ++unit)
                result.unitMicroseconds[unit] += unitMicroseconds(unit);

            result.maxBlocks = std::max(result.maxBlocks, state.scheduledBlocks);
            result.energyConserved = result.energyConserved && state.energy == energy + state.sourceEnergy;
            energy = state.energy;
            if (state.scheduledBlocks == 0 && result.quietTick == 0)
                result.quietTick = state.tick;
        }
    }

    ScenarioResult RunScenario(ID3D12Device5* device, const BakedReactionTable& table, const Scenario& scenario,
                               const gpu::GraphTraceFilter& trace, uint32_t traceCapacity) {
        ScenarioResult result;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"ConductBench");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table,
                                           {.trace = trace, .traceCapacity = traceCapacity});
        if (!queue || !simulation)
            return result;

        const double microsecondsPerTick = 1'000'000.0 / static_cast<double>(queue->TimestampFrequency());
        const std::vector<ProbeCommand> pokes = MakePokes(scenario.pointsPerAxis);
        uint64_t energy = ProbeEnergySum(MakeProbeInitialWorld(table));
        const uint32_t unitsPerTick = simulation->UnitsPerTick();

        for (uint64_t tick = 0; tick < scenario.maxTicks && result.quietTick == 0; tick += TICKS_PER_FRAME) {
            const auto slot = static_cast<uint32_t>((tick / TICKS_PER_FRAME) % ProbeSim::FRAME_SLOT_COUNT);
            const std::span<const ProbeCommand> commands = tick == 0 ? std::span<const ProbeCommand>(pokes)
                                                                     : std::span<const ProbeCommand>();

            ID3D12CommandList* list = simulation->RecordFrame(
                slot,
                {.firstTick = tick, .firstUnit = 0, .unitCount = TICKS_PER_FRAME * unitsPerTick, .commands = commands});

            if (list == nullptr || !queue->WaitCpu(queue->Submit(list)))
                return result;

            const ProbeFrameReadback readback = simulation->ReadFrame(slot);
            if (readback.hashes.size() != TICKS_PER_FRAME || readback.droppedTraceCount > 0)
                return result;

            AccumulateFrame(result, readback, unitsPerTick, microsecondsPerTick, energy);
            result.ticks = tick + TICKS_PER_FRAME;
        }

        Log(Channel::Sim, Level::Info, "伝導の裏のメモリ: {} バイト", simulation->ConductBackingMemoryBytes());
        result.ok = true;

        return result;
    }

    void Report(const Scenario& scenario, const ScenarioResult& result) {
        Log(Channel::Sim, Level::Info,
            "{}: {} 刻み  止まった刻み {}  計算したブロックの最大 {} / {}  エネルギーの保存 {}", scenario.name,
            result.ticks, result.quietTick == 0 ? std::string("(止まらない)") : std::to_string(result.quietTick),
            result.maxBlocks, PROBE_BLOCK_COUNT, result.energyConserved ? "OK" : "NG");

        const double ticks = static_cast<double>(std::max<uint64_t>(result.ticks, 1));

        Log(Channel::Sim, Level::Info, "{}: 単位の平均 µs/刻み: 適用 {:.1f}  伝導と反応 {:.1f}  検査と出力 {:.1f}",
            scenario.name, result.unitMicroseconds[PROBE_UNIT_APPLY] / ticks,
            result.unitMicroseconds[PROBE_UNIT_CONDUCT] / ticks,
            result.unitMicroseconds[PROBE_FIXED_UNITS_PER_TICK - 1] / ticks);

        uint32_t lower = 1;

        for (size_t index = 0; index < BUCKET_LIMITS.size(); ++index) {
            const Bucket& bucket = result.buckets[index];
            if (bucket.samples > 0) {
                const auto samples = static_cast<double>(bucket.samples);
                const double averageBlocks = static_cast<double>(bucket.blocks) / samples;
                const double averageMicroseconds = bucket.microseconds / samples;

                Log(Channel::Sim, Level::Info, "| {} | {}〜{} | {} | {:.0f} | {:.1f} | {:.1f} |", scenario.name, lower,
                    BUCKET_LIMITS[index], bucket.samples, averageBlocks, averageMicroseconds,
                    averageMicroseconds * 1000.0 / averageBlocks);
            }

            lower = BUCKET_LIMITS[index] + 1;
        }
    }

}  // namespace

int main(int argc, char** argv) {
    // --trace・--trace-idle だけはこのベンチの引数(残りは gpu_test_options.h)
    std::vector<char*> arguments(argv, argv + argc);
    const auto takeArgument = [&arguments](std::string_view name) {
        const auto found = rng::find_if(arguments, [name](const char* argument) { return argument == name; });
        if (found == arguments.end())
            return false;

        arguments.erase(found);

        return true;
    };

    const bool traced = takeArgument("--trace");
    const bool idle = takeArgument("--trace-idle");
    const uint32_t traceCapacity = idle ? TRACE_CAPACITY : 0;

    const gpu::GraphTraceFilter trace = traced
                                            ? ProbeTraceFilterForCells(
                                                  0, UINT64_MAX, {0, 0, 0},
                                                  {PROBE_GRID_SIZE, PROBE_GRID_SIZE, PROBE_GRID_SIZE}, TRACE_CAPACITY)
                                            : gpu::GraphTraceFilter{};

    const auto options = test::ParseGpuTestOptions(std::span(arguments));
    if (!options) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_conduct_bench [--warp] [--trace | --trace-idle]");
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    Log(Channel::Sim, Level::Info, "連鎖のトレース: {}",
        traced ? "全部を記録する" : (idle ? "容量だけ確保して無効(T-0088)" : "無効"));
    Log(Channel::Sim, Level::Info,
        "| 規模 | 計算したブロック | 刻みの数 | 平均ブロック | 伝導と反応 µs/刻み | ns/ブロック |");
    Log(Channel::Sim, Level::Info, "|---|---|---|---|---|---|");
    const auto table = BakeReactionTable(MakeCombustionTestTable());
    if (!table) {
        Log(Channel::Sim, Level::Error, "試験の反応の表をベイクできない: {}", table.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    bool passed = true;

    for (const Scenario& scenario : SCENARIOS) {
        const ScenarioResult result = RunScenario(device->Get(), *table, scenario, trace, traceCapacity);
        if (!result.ok)
            Log(Channel::Sim, Level::Error, "{}: 走らせられない", scenario.name);

        Report(scenario, result);
        passed = passed && result.ok && result.energyConserved;
    }

    passed = passed && test::PassesValidation(*device, "gpu_conduct_bench");
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_conduct_bench({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
