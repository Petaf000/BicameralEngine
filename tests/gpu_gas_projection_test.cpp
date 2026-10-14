// gpu_gas_projection_test.cpp — 気体の圧力の投影(T-0531。CPU の模型は sim/gas_projection・07 §2.6)の GPU 版を CPU とビット一致で確かめ、測る。
// 今は解く部分(GpuImplicit::RecordSolve。一般の右辺の V サイクル・止める判定は式の誤差〔残差 × D〕が許容差 mg 以下)だけ:
// CPU の模型(StepGasProjection)が実際に解いた系(各刻み・外側の反復の解き直しも)を見る口(solveObserver)で受け、
// 同じ系(CPU の BuildImplicitGrid が作った段・右辺・初めの近似)を GPU で解き直して、解いた値と V の回数がビット一致するかを確かめる。
// 式を作る段・当てる段・外側の反復の GPU 版はまだ(T-0531 の引き継ぎ)。
// 場面: 閉じた箱(一辺 n。重さあり)の 熱い泡(+30 K・同じ圧力)/ 扉(x の半分が 2% 高い圧力・同じ温度)/ 強い爆発(真ん中 1 セル 5 倍の圧力・1500 K)。
// 音速は最速(0)と 340 m/s。外側の反復は既定(上限 4・100 mg。ADR-0234)。
//
// 引数: [--edge n](既定 16)[--ticks n](既定 3)[--measure](GPU の時間を表に出す)と GPU の試験の共通の引数(--warp・--queue・--no-gbv)。
// ctest は一辺 16・3 刻み(gpu_gas_projection)。計測は `job.py run -Preset release -Exe gpu_gas_projection_test -- --no-gbv --measure --edge 32`。
// 合わなければ失敗(どの場面・何回目の解き・何番のセルか)。
#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "sim/gas_projection.h"
#include "sim/gas_reference.h"
#include "sim/gpu_implicit.h"
#include "sim/world_physics.h"
#include "standard_world_physics.h"

using namespace bicameral;

namespace {

    // --- 試験の空気(gas_projection_test と同じ。N2 79% + O2 21% と煙の印)---
    constexpr uint32_t SPECIES_N2 = 0;
    constexpr uint32_t SPECIES_SMOKE = 2;
    constexpr std::array<uint32_t, 3> AIR_FRACTIONS_Q32 = {3393024163u, 901943132u, 0u};
    constexpr uint32_t AIR_TEMPERATURE = 293150;                   // mK
    constexpr uint64_t SURFACE_PRESSURE = 101325ull * 1000000ull;  // µPa
    constexpr uint32_t REAL_SOUND = 340000;                        // mm/s
    constexpr uint32_t BUBBLE_HEAT = 30000;                        // mK
    constexpr uint32_t BLAST_TEMPERATURE = 1500000;                // mK

    sim::GasConfig MakeAirConfig(uint32_t edge) {
        sim::GasConfig config;
        config.sizeX = edge;
        config.sizeY = edge;
        config.sizeZ = edge;
        config.speciesCount = 3;
        config.species[0] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};  // N2
        config.species[1] = {.molarMass = 31999, .heatCapacity = 21064, .formationEnergy = 0};  // O2
        config.species[2] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};  // 煙の印
        sim::ApplyWorldPhysics(sim::StandardWorldPhysics(), config);

        return config;
    }

    sim::GasBox MakeAirBox(uint32_t edge) {
        const sim::GasConfig config = MakeAirConfig(edge);
        return sim::MakeGasBox(
            config, sim::MakeHydrostaticReference(config, AIR_FRACTIONS_Q32, AIR_TEMPERATURE, SURFACE_PRESSURE));
    }

    // 同じ圧力のまま温度を変える(物質量を温度に反比例させる)
    void SetTemperature(sim::GasBox& box, uint32_t index, uint32_t temperature, uint64_t pressurePercent) {
        std::array<uint64_t, sim::GAS_MAX_SPECIES> amounts = box.cells[index].amounts;
        for (uint64_t& amount : amounts)
            amount = amount * pressurePercent * AIR_TEMPERATURE / (100 * (uint64_t)temperature);

        box.cells[index] = sim::MakeGasCell(box.config, amounts, temperature);
    }

    enum class Scene : uint8_t { Bubble, Door, Blast };

    constexpr std::array<Scene, 3> SCENES = {Scene::Bubble, Scene::Door, Scene::Blast};
    constexpr std::array<uint32_t, 2> SPEEDS = {0, REAL_SOUND};

    std::string_view SceneName(Scene scene) {
        switch (scene) {
            case Scene::Bubble: return "泡";
            case Scene::Door: return "扉";
            case Scene::Blast: return "爆発";
        }

        return "";
    }

    sim::GasBox MakeScene(Scene scene, uint32_t edge) {
        sim::GasBox box = MakeAirBox(edge);
        const auto center = (int32_t)(edge / 2);
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const auto x = (int32_t)(index % edge);
            const auto y = (int32_t)((index / edge) % edge);
            const auto z = (int32_t)(index / (edge * edge));
            if (scene == Scene::Bubble) {
                const int32_t dx = x - center;
                const int32_t dy = y - center;
                const int32_t dz = z - (center / 2);
                if ((dx * dx) + (dy * dy) + (dz * dz) <= 2)
                    SetTemperature(box, index, AIR_TEMPERATURE + BUBBLE_HEAT, 100);
            } else if (scene == Scene::Door) {
                if (x < center)
                    SetTemperature(box, index, AIR_TEMPERATURE, 102);
            } else if (x == center && y == center && z == center) {
                SetTemperature(box, index, BLAST_TEMPERATURE, 500);
                sim::GasCell& cell = box.cells[index];
                const uint64_t moved = cell.amounts[SPECIES_N2] / 2;
                cell.amounts[SPECIES_N2] -= moved;
                cell.amounts[SPECIES_SMOKE] += moved;
            }
        }

        return box;
    }

    // --- GPU で解き直す ---

    struct Replay {
        ID3D12Device5* device = nullptr;
        gpu::ImmediateQueue* queue = nullptr;
        gpu::DebugRing* ring = nullptr;
        uint64_t frequency = 0;
        uint32_t toleranceMilligrams = 0;
        sim::ImplicitOptions solver;
        std::optional<sim::GpuImplicit> gpu;

        // --- 結果 ---
        std::string failure;
        uint32_t solves = 0;
        uint32_t cycles = 0;
        double milliseconds = 0;
    };

    std::expected<void, std::string> SolveOnGpu(Replay& replay, const sim::GasPressureSolve& solve,
                                                std::vector<int64_t>& values, sim::GpuImplicitCost& cost,
                                                double& milliseconds) {
        if (!replay.gpu) {
            auto created = sim::GpuImplicit::Create(replay.device, solve.grid);
            if (!created)
                return std::unexpected(created.error());

            replay.gpu.emplace(std::move(*created));
        }

        sim::GpuImplicit& gpu = *replay.gpu;
        const std::vector<int64_t> thresholds = sim::GasPressureThresholds(solve.grid, replay.toleranceMilligrams);
        ID3D12GraphicsCommandList10* list = replay.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        replay.ring->RecordBegin(list);
        bool recorded = gpu.RecordUpload(list, solve.grid);
        recorded = recorded && gpu.RecordUploadSolve(list, solve.rhs, solve.before, thresholds);
        gpu.RecordTimestamp(list, 0);
        recorded = recorded && gpu.RecordSolve(list, replay.ring->GpuAddress(), replay.solver);
        gpu.RecordTimestamp(list, 1);
        if (!recorded)
            return std::unexpected("記録できない(系が上限を超えた)");

        gpu.RecordSolutionReadback(list, (uint32_t)solve.after.size());
        gpu.RecordRelease(list);
        replay.ring->RecordReadbackAndReset(list);
        if (!replay.queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = replay.ring->Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        values.resize(solve.after.size());
        const std::vector<uint64_t> stamps = gpu.ReadTimestamps(2);
        if (!gpu.ReadSolution(values, cost) || stamps.size() != 2)
            return std::unexpected("読み戻せない");

        milliseconds = (double)(stamps[1] - stamps[0]) * 1000.0 / (double)replay.frequency;

        return {};
    }

    void CompareSolve(Replay& replay, const sim::GasPressureSolve& solve) {
        if (!replay.failure.empty())
            return;

        std::vector<int64_t> values;
        sim::GpuImplicitCost cost;
        double milliseconds = 0;
        if (auto solved = SolveOnGpu(replay, solve, values, cost, milliseconds); !solved) {
            replay.failure = std::format("解き {}: {}", replay.solves, solved.error());
            return;
        }

        if (cost.cycles != solve.cycles) {
            replay.failure = std::format("解き {}: V の回数 GPU {}・CPU {}", replay.solves, cost.cycles, solve.cycles);
            return;
        }

        const auto mismatch = std::ranges::mismatch(values, solve.after);
        if (mismatch.in1 != values.end()) {
            const auto cell = (size_t)(mismatch.in1 - values.begin());
            replay.failure = std::format("解き {}: セル {} の圧力 GPU {}・CPU {}(V {})", replay.solves, cell,
                                         *mismatch.in1, *mismatch.in2, solve.cycles);
            return;
        }

        replay.solves += 1;
        replay.cycles += cost.cycles;
        replay.milliseconds += milliseconds;
    }

    struct SceneResult {
        uint32_t solves = 0;
        uint32_t cycles = 0;
        double milliseconds = 0;     // 2 刻み目からの解きの GPU 時間の合計
        double worstTickMs = 0;      // 2 刻み目からの 1 刻みの解きの時間の最大
        uint32_t measuredTicks = 0;  // 2 刻み目から
        uint32_t outerIterations = 0;
    };

    std::expected<SceneResult, std::string> RunScene(Replay& replay, Scene scene, uint32_t speed, uint32_t edge,
                                                     uint32_t ticks) {
        sim::GasBox box = MakeScene(scene, edge);
        sim::GasProjection state;
        sim::GasProjectionOptions options;
        options.pressure.soundSpeedMmPerS = speed;
        replay.toleranceMilligrams = options.toleranceMilligrams;
        replay.solver = options.solver;
        const sim::GasPressureSolveObserver observer = [&replay](const sim::GasPressureSolve& solve) {
            CompareSolve(replay, solve);
        };
        options.solveObserver = &observer;

        SceneResult result;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            const uint32_t solvesBefore = replay.solves;
            const uint32_t cyclesBefore = replay.cycles;
            const double msBefore = replay.milliseconds;
            const sim::GasProjectionCost cost = sim::StepGasProjection(box, state, options);
            if (!replay.failure.empty())
                return std::unexpected(std::format("刻み {}: {}", tick, replay.failure));

            result.solves += replay.solves - solvesBefore;
            result.cycles += replay.cycles - cyclesBefore;
            result.outerIterations += cost.outerIterations;
            if (tick == 0)
                continue;

            const double tickMs = replay.milliseconds - msBefore;
            result.milliseconds += tickMs;
            result.worstTickMs = std::max(result.worstTickMs, tickMs);
            result.measuredTicks += 1;
        }

        return result;
    }

    std::expected<void, std::string> RunAll(Replay& replay, uint32_t edge, uint32_t ticks, bool measure) {
        for (const Scene scene : SCENES) {
            for (const uint32_t speed : SPEEDS) {
                replay.gpu.reset();
                const auto result = RunScene(replay, scene, speed, edge, ticks);
                if (!result)
                    return std::unexpected(
                        std::format("{}・音速 {} mm/s: {}", SceneName(scene), speed, result.error()));

                Log(Channel::Sim, Level::Info, "{}³ {}・音速 {}: 解き {}(外側 {})・V {} が CPU とビット一致", edge,
                    SceneName(scene), speed == 0 ? "最速" : "340 m/s", result->solves, result->outerIterations,
                    result->cycles);
                if (!measure || result->measuredTicks == 0)
                    continue;

                // 表の行: | 一辺 | 場面 | 音速 | 刻み | 解き/刻み | V/刻み | 解きの GPU ms/刻み(平均 / 最大)|
                Log(Channel::Sim, Level::Info, "| {}³ | {} | {} | {} | {:.2f} | {:.2f} | {:.3f} / {:.3f} |", edge,
                    SceneName(scene), speed == 0 ? "最速" : "340 m/s", ticks, (double)result->solves / ticks,
                    (double)result->cycles / ticks, result->milliseconds / result->measuredTicks, result->worstTickMs);
            }
        }

        return {};
    }

    int Run(std::span<char*> arguments) {
        std::vector<char*> rest;
        uint32_t edge = 16;
        uint32_t ticks = 3;
        bool measure = false;
        for (size_t index = 0; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            if (argument == "--edge" && index + 1 < arguments.size())
                edge = (uint32_t)std::stoul(arguments[++index]);
            else if (argument == "--ticks" && index + 1 < arguments.size())
                ticks = (uint32_t)std::stoul(arguments[++index]);
            else if (argument == "--measure")
                measure = true;
            else
                rest.push_back(arguments[index]);
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options || edge < 4 || ticks == 0) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_gas_projection_test [--warp] [--queue direct|compute] [--no-gbv] [--edge N] [--ticks N] "
                "[--measure]");
            return 2;
        }

        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_gas_projection_test: FAILED(デバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        uint64_t frequency = 0;
        if (!queue || !ring || FAILED(queue->Native()->GetTimestampFrequency(&frequency))) {
            Log(Channel::Gpu, Level::Error, "gpu_gas_projection_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        Replay replay{.device = device->Get(), .queue = &*queue, .ring = &*ring, .frequency = frequency};
        if (auto ran = RunAll(replay, edge, ticks, measure); !ran) {
            Log(Channel::Gpu, Level::Error, "gpu_gas_projection_test: FAILED({})", ran.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_gas_projection_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_gas_projection_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
