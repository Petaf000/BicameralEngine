// gpu_place_bench.cpp — 置くコマンド(PROBE_COMMAND_TYPE_PLACE。ADR-0067・ADR-0089)が 1 刻みにたくさん来た時の、
// 適用の単位(probe_tick.hlsl の ApplyCommands。1 スレッドで番号の順)の GPU 時間を測る(T-0280)。
//
// 筆のドラッグは 1 フレームに 8 個・止めている間に塗ると 1 刻みに約 64 個溜まる(ADR-0089 §2)。いちばん重い形
// (直方体の半幅 8 = 17³ = 4913 セル・球の半径 8 = 2109 セル)を、置き換え(木)と足す(木炭)で、1 刻みに 0・1・8・64 個当て、
// 当てた刻みの適用の単位と伝導の単位(起こしたブロックを計算する)の時間を取る。
// 置く場所は格子に 4 × 4 × 4 の間隔 16 で並べる(8 個は 2 × 2 × 2)。同じ場面を REPEAT 刻み続けて、中央値と最大を出す。
// 結果は Markdown の表の行としてログに出す(docs/perf.md に貼る)。当たった置くコマンドのイベントの数が合わなければ失敗。
//
// ctest には登録しない(時間は機械しだい)。走らせ方: `job.py run -Preset release -Exe gpu_place_bench`。
// 引数は gpu_test_options.h(--warp)。WARP での時間は GPU の目安にならない。
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu/queue.h"
#include "gpu_test_options.h"
#include "sim/lab_box.h"
#include "sim/probe_sim.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli・probe_place.hlsli の定数

namespace {

    constexpr uint32_t REPEAT = 8;      // 1 つの場面を続けて当てる刻みの数
    constexpr uint32_t LATTICE = 4;     // 置く場所の格子(1 軸の数)
    constexpr uint32_t SPACING = 16;    // 置く場所の間隔(セル)
    constexpr uint32_t HALF_WIDTH = 8;  // 半幅・半径(上限 PROBE_PLACE_MAX_RADIUS)

    struct Scenario {
        std::string_view name;
        uint32_t kind = PROBE_PLACE_SHAPE_SPHERE;
        bool replace = true;
        uint32_t count = 0;  // 1 刻みに当てる数
    };

    constexpr std::array<Scenario, 13> SCENARIOS = {{
        {.name = "なし", .count = 0},
        {.name = "球 r8 置き換え", .kind = PROBE_PLACE_SHAPE_SPHERE, .replace = true, .count = 1},
        {.name = "球 r8 置き換え", .kind = PROBE_PLACE_SHAPE_SPHERE, .replace = true, .count = 8},
        {.name = "球 r8 置き換え", .kind = PROBE_PLACE_SHAPE_SPHERE, .replace = true, .count = 64},
        {.name = "球 r8 足す", .kind = PROBE_PLACE_SHAPE_SPHERE, .replace = false, .count = 1},
        {.name = "球 r8 足す", .kind = PROBE_PLACE_SHAPE_SPHERE, .replace = false, .count = 8},
        {.name = "球 r8 足す", .kind = PROBE_PLACE_SHAPE_SPHERE, .replace = false, .count = 64},
        {.name = "直方体 8³ 置き換え", .kind = PROBE_PLACE_SHAPE_BOX, .replace = true, .count = 1},
        {.name = "直方体 8³ 置き換え", .kind = PROBE_PLACE_SHAPE_BOX, .replace = true, .count = 8},
        {.name = "直方体 8³ 置き換え", .kind = PROBE_PLACE_SHAPE_BOX, .replace = true, .count = 64},
        {.name = "直方体 8³ 足す", .kind = PROBE_PLACE_SHAPE_BOX, .replace = false, .count = 1},
        {.name = "直方体 8³ 足す", .kind = PROBE_PLACE_SHAPE_BOX, .replace = false, .count = 8},
        {.name = "直方体 8³ 足す", .kind = PROBE_PLACE_SHAPE_BOX, .replace = false, .count = 64},
    }};

    struct Timing {
        double medianMicroseconds = 0.0;
        double maxMicroseconds = 0.0;
    };

    struct ScenarioResult {
        bool ok = false;
        Timing apply;
        Timing conduct;
        uint32_t cellsPerCommand = 0;
    };

    // 置く場所(番号 index の中心)。8 個以下は 2 × 2 × 2 の隅から、それより多いと 4 × 4 × 4
    std::array<uint32_t, 3> Center(uint32_t index, uint32_t count) {
        const uint32_t perAxis = count <= 8 ? 2 : LATTICE;
        const uint32_t step = count <= 8 ? SPACING * 2 : SPACING;
        const auto at = [&](uint32_t axisIndex) {
            return (axisIndex * step) + (SPACING / 2);
        };

        return {at(index % perAxis), at((index / perAxis) % perAxis), at(index / (perAxis * perAxis))};
    }

    ProbePlaceShape MakeShape(const Scenario& scenario, std::array<uint32_t, 3> center) {
        const bool box = scenario.kind == PROBE_PLACE_SHAPE_BOX;

        return {.x = center[0],
                .y = center[1],
                .z = center[2],
                .radius = HALF_WIDTH,
                .replace = scenario.replace,
                .kind = scenario.kind,
                .size2 = box ? HALF_WIDTH : 0,
                .size3 = box ? HALF_WIDTH : 0};
    }

    Timing Summarize(std::vector<double> samples) {
        if (samples.empty())
            return {};

        rng::sort(samples);

        return {.medianMicroseconds = samples[samples.size() / 2], .maxMicroseconds = samples.back()};
    }

    uint32_t CoveredCells(const ProbeCommand& command, uint32_t speciesCount) {
        const ProbePlace place = DecodePlaceCommand(command, speciesCount);
        uint32_t covered = 0;
        for (uint32_t z = ProbePlaceBegin(place.z, place.extentZ); z < ProbePlaceEnd(place.z, place.extentZ); ++z) {
            for (uint32_t y = ProbePlaceBegin(place.y, place.extentY); y < ProbePlaceEnd(place.y, place.extentY); ++y) {
                for (uint32_t x = ProbePlaceBegin(place.x, place.extentX); x < ProbePlaceEnd(place.x, place.extentX);
                     ++x)
                    covered += ProbePlaceCovers(place, x, y, z) ? 1 : 0;
            }
        }

        return covered;
    }

    // 世界は場面をまたいで続ける(作り直すとパイプラインの作成で遅い)。tick は次に流す刻み
    ScenarioResult RunScenario(ProbeSim& simulation, gpu::Queue& queue, const BakedReactionTable& table,
                               const Scenario& scenario, uint64_t& tick) {
        ScenarioResult result;
        const std::vector<LabMaterial> materials = MakeLabMaterials(table);
        const std::string_view materialName = scenario.replace ? "木" : "木炭";
        const auto material = rng::find(materials, materialName, &LabMaterial::name);
        if (material == materials.end())
            return result;

        const double microsecondsPerTick = 1'000'000.0 / static_cast<double>(queue.TimestampFrequency());
        const uint32_t unitsPerTick = simulation.UnitsPerTick();
        const auto speciesCount = static_cast<uint32_t>(table.species.size());
        std::vector<double> apply;
        std::vector<double> conduct;

        for (uint32_t repeat = 0; repeat < REPEAT; ++repeat, ++tick) {
            std::vector<ProbeCommand> commands;
            for (uint32_t index = 0; index < scenario.count; ++index)
                commands.push_back(MakePlaceCommand(tick, index, MakeShape(scenario, Center(index, scenario.count)),
                                                    material->contents));

            if (!commands.empty())
                result.cellsPerCommand = CoveredCells(commands.front(), speciesCount);

            const auto slot = static_cast<uint32_t>(tick % ProbeSim::FRAME_SLOT_COUNT);
            ID3D12CommandList* list = simulation.RecordFrame(
                slot, {.firstTick = tick, .firstUnit = 0, .unitCount = unitsPerTick, .commands = commands});
            if (list == nullptr || !queue.WaitCpu(queue.Submit(list)))
                return result;

            const ProbeFrameReadback readback = simulation.ReadFrame(slot);
            const auto applied = static_cast<uint32_t>(
                rng::count(readback.events, PROBE_EVENT_PLACE_APPLIED, &ProbeEvent::type));
            if (readback.unitGpuTicks.size() != unitsPerTick || applied != scenario.count) {
                Log(Channel::Sim, Level::Error, "{} × {}: 当たった数 {}(単位 {})", scenario.name, scenario.count,
                    applied, readback.unitGpuTicks.size());
                return result;
            }

            apply.push_back(static_cast<double>(readback.unitGpuTicks[PROBE_UNIT_APPLY]) * microsecondsPerTick);
            conduct.push_back(static_cast<double>(readback.unitGpuTicks[PROBE_UNIT_CONDUCT]) * microsecondsPerTick);
        }

        result.apply = Summarize(apply);
        result.conduct = Summarize(conduct);
        result.ok = true;

        return result;
    }

    void Report(const Scenario& scenario, const ScenarioResult& result) {
        const double cells = static_cast<double>(result.cellsPerCommand) * scenario.count;
        const double nanosecondsPerCell = cells > 0.0 ? result.apply.medianMicroseconds * 1000.0 / cells : 0.0;
        Log(Channel::Sim, Level::Info, "| {} | {} | {} | {:.0f} | {:.1f} | {:.1f} | {:.1f} | {:.1f} |", scenario.name,
            scenario.count, result.cellsPerCommand, cells, result.apply.medianMicroseconds,
            result.apply.maxMicroseconds, nanosecondsPerCell, result.conduct.medianMicroseconds);
    }

    int Finish(int code) {
        bicameral::SingletonFinalizer::Finalize();

        return code;
    }

}  // namespace

int main(int argc, char** argv) {
    const auto options = test::ParseGpuTestOptions(std::span(argv, static_cast<size_t>(argc)));
    if (!options) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_place_bench [--warp]");
        return Finish(2);
    }

    auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        return Finish(1);
    }

    const auto table = BakeReactionTable(MakeCombustionTestTable());
    if (!table) {
        Log(Channel::Sim, Level::Error, "試験の反応の表をベイクできない: {}", table.error());
        return Finish(1);
    }

    auto queue = gpu::Queue::Create(device->Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, L"PlaceBench");
    auto simulation = ProbeSim::Create(device->Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, *table, {});
    if (!queue || !simulation) {
        Log(Channel::Sim, Level::Error, "キューか仮の世界を作れない");
        return Finish(1);
    }

    Log(Channel::Sim, Level::Info,
        "| 場面 | 1 刻みの数 | 1 個のセル | セルの合計 | 適用 µs(中央値) | 適用 µs(最大) | ns/セル | 伝導 µs(中央値) "
        "|");
    Log(Channel::Sim, Level::Info, "|---|---|---|---|---|---|---|---|");

    bool passed = true;
    uint64_t tick = 0;

    for (const Scenario& scenario : SCENARIOS) {
        const ScenarioResult result = RunScenario(*simulation, *queue, *table, scenario, tick);
        if (!result.ok)
            Log(Channel::Sim, Level::Error, "{} × {}: 走らせられない", scenario.name, scenario.count);

        Report(scenario, result);
        passed = passed && result.ok;
    }

    passed = passed && test::PassesValidation(*device, "gpu_place_bench");
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_place_bench({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");

    return Finish(passed ? 0 : 1);
}
