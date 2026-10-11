// gpu_gas_tree_test.cpp — 気体の多重解像度の木の GPU 版(sim::GpuGasTree、shaders/sim/gas_tree_step.hlsl。T-0209)が CPU リファレンス
// (sim/gas_tree の StepGasTree。T-0208)と毎刻みビット一致することを確かめる。全部のレベルが同じ小刻み(σ = 0)の木。
// 場面: 閉じた 2 レベルの箱の乱れ(gas_tree_test の MakeDisturbedTwoLevelBox。熱い・冷たいセル・煙・動くセル。端数を通る)・
// レベルをまたぐ静止大気(動かないことも)・開いた境界と風(細かい範囲が開いた境界に接する。MC・乱数の丸め)・
// minmod と切り捨て・全部レベル 0 の木(周期的な箱)。刻みごとに GPU のセル・端数・帳簿(whole + 2^-64)を読み戻して CPU と比べる。
// 引数: gpu_test_options.h(--warp・--queue)と --scene <名前>(1 つの場面だけ)・--ticks n(場面の刻みの数を替える)
#include "sim/gpu_gas_tree.h"
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "sim/gas_tree.h"

#include <cmath>

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    // --- 試験の空気(gas_tree_test と同じ)---
    constexpr uint32_t SPECIES_N2 = 0;
    constexpr uint32_t SPECIES_SMOKE = 2;
    constexpr std::array<uint32_t, 3> AIR_FRACTIONS_Q32 = {3393024163u, 901943132u, 0u};
    constexpr uint32_t AIR_TEMPERATURE = 293150;                   // mK
    constexpr uint64_t SURFACE_PRESSURE = 101325ull * 1000000ull;  // µPa
    constexpr int64_t METER_PER_SECOND = (int64_t)1 << 20;
    constexpr int64_t ROUGH_CELL_MASS = 150000;  // mg

    GasConfig MakeAirConfig(uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ) {
        GasConfig config;
        config.sizeX = sizeX;
        config.sizeY = sizeY;
        config.sizeZ = sizeZ;
        config.speciesCount = 3;
        config.species[0] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};
        config.species[1] = {.molarMass = 31999, .heatCapacity = 21064, .formationEnergy = 0};
        config.species[2] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};
        config.substeps = 48;  // 全部のレベルが同じ小刻み: レベル 1 の ν = 0.47(T-0208)

        return config;
    }

    GasTree MakeAirTree(const GasConfig& config, std::span<const GasTreeRegion> regions) {
        return MakeGasTree(config, regions, AIR_FRACTIONS_Q32, AIR_TEMPERATURE, SURFACE_PRESSURE);
    }

    void Heat(const GasConfig& config, GasCell& cell, uint32_t temperature) {
        GasCell heated = MakeGasCell(config, cell.amounts, temperature);
        heated.momentum = cell.momentum;
        cell = heated;
    }

    void AddSmoke(GasCell& cell) {
        const uint64_t half = cell.amounts[SPECIES_N2] / 2;
        cell.amounts[SPECIES_N2] -= half;
        cell.amounts[SPECIES_SMOKE] += half;
    }

    // 粗いセルを速度 (vx, 0, vz) m/s で動かす(運動エネルギー P² × 2^-41 ÷ m µJ をエネルギーへ足す。gas_tree_test と同じ)
    void SetMoving(GasCell& cell, int64_t velocityX, int64_t velocityZ) {
        const int64_t momentumX = ROUGH_CELL_MASS * velocityX * METER_PER_SECOND;
        const int64_t momentumZ = ROUGH_CELL_MASS * velocityZ * METER_PER_SECOND;
        cell.momentum = {momentumX, 0, momentumZ};
        const double square = ((double)momentumX * (double)momentumX) + ((double)momentumZ * (double)momentumZ);
        const double kinetic = square / std::ldexp(1.0, 41) / (double)ROUGH_CELL_MASS;
        cell.energy += (int64_t)std::llround(kinetic / 1000.0);
    }

    // --- 場面 ---

    struct Scene {
        std::string name;
        GasTree tree;
        uint64_t ticks = 0;
        bool expectStill = false;  // 静止大気: 状態が少しも変わらないことも確かめる
    };

    // 8 × 4 × 8 のレベル 0 の真ん中 4 × 4 を x と z でレベル 1 に(y は全部。gas_tree_test の MIDDLE_REGION)
    constexpr std::array<GasTreeRegion, 1> MIDDLE_REGION = {
        GasTreeRegion{.level = 1, .origin = {4, 0, 4}, .size = {8, 8, 8}}};

    // gas_tree_test の MakeDisturbedTwoLevelBox(境の両側の熱いセル・冷たいセル・煙・動く粗いセル)
    GasTree MakeDisturbedTwoLevelBox(const GasConfig& config) {
        GasTree tree = MakeAirTree(config, MIDDLE_REGION);
        Heat(config, tree.cells[tree.Find(0, 1, 1, 2)], AIR_TEMPERATURE + 60000);
        Heat(config, tree.cells[tree.Find(1, 4, 2, 5)], AIR_TEMPERATURE + 60000);
        Heat(config, tree.cells[tree.Find(1, 6, 3, 4)], AIR_TEMPERATURE - 20000);
        AddSmoke(tree.cells[tree.Find(1, 5, 5, 6)]);
        AddSmoke(tree.cells[tree.Find(0, 6, 2, 6)]);
        SetMoving(tree.cells[tree.Find(0, 6, 1, 3)], -2, -1);

        return tree;
    }

    Scene MakeClosedScene() {
        return {.name = "closed", .tree = MakeDisturbedTwoLevelBox(MakeAirConfig(8, 4, 8)), .ticks = 8};
    }

    Scene MakeStaticScene() {
        return {.name = "static",
                .tree = MakeAirTree(MakeAirConfig(8, 4, 8), MIDDLE_REGION),
                .ticks = 4,
                .expectStill = true};
    }

    // 開いた境界(x)と風 2 m/s。細かい範囲は x の開いた境界に接する(レベル 1 の外の層)。y は周期的(細かいセルも回る)
    Scene MakeOpenWindScene() {
        GasConfig config = MakeAirConfig(8, 4, 8);
        config.boundary = {GasBoundary::Open, GasBoundary::Periodic, GasBoundary::Wall};
        config.windVelocity = {(int32_t)(2 * METER_PER_SECOND), 0, 0};
        config.randomSeed = 0x5EED;
        const std::array<GasTreeRegion, 1> region = {GasTreeRegion{.level = 1, .origin = {0, 0, 2}, .size = {6, 8, 6}}};
        GasTree tree = MakeAirTree(config, region);
        AddSmoke(tree.cells[tree.Find(1, 1, 3, 3)]);
        AddSmoke(tree.cells[tree.Find(1, 4, 0, 5)]);
        AddSmoke(tree.cells[tree.Find(0, 4, 2, 2)]);
        Heat(config, tree.cells[tree.Find(1, 2, 6, 4)], AIR_TEMPERATURE + 40000);
        Heat(config, tree.cells[tree.Find(0, 5, 1, 1)], AIR_TEMPERATURE + 40000);

        return {.name = "open_wind", .tree = tree, .ticks = 8};
    }

    // minmod と切り捨て(乱数の丸めなし)の閉じた 2 レベルの箱
    Scene MakeMinmodFloorScene() {
        GasConfig config = MakeAirConfig(8, 4, 8);
        config.reconstruction = GasReconstruction::Minmod;
        config.stochasticRounding = false;
        return {.name = "minmod_floor", .tree = MakeDisturbedTwoLevelBox(config), .ticks = 6};
    }

    // 全部レベル 0 の木(周期的な箱・重さなし。1 レベルの箱の GPU と同じ値になる形)
    Scene MakeSingleLevelScene() {
        GasConfig config = MakeAirConfig(8, 2, 4);
        config.boundary = {GasBoundary::Periodic, GasBoundary::Periodic, GasBoundary::Periodic};
        config.gravity = false;
        config.substeps = 24;
        GasTree tree = MakeAirTree(config, {});
        Heat(config, tree.cells[tree.Find(0, 2, 1, 1)], AIR_TEMPERATURE + 50000);
        AddSmoke(tree.cells[tree.Find(0, 5, 0, 2)]);
        SetMoving(tree.cells[tree.Find(0, 6, 1, 3)], 3, 1);

        return {.name = "single_level", .tree = tree, .ticks = 6};
    }

    std::vector<Scene> MakeScenes() {
        std::vector<Scene> scenes;
        scenes.push_back(MakeClosedScene());
        scenes.push_back(MakeStaticScene());
        scenes.push_back(MakeOpenWindScene());
        scenes.push_back(MakeMinmodFloorScene());
        scenes.push_back(MakeSingleLevelScene());

        return scenes;
    }

    // --- 比べる ---

    bool SameCell(const GasCell& a, const GasCell& b) {
        return a.amounts == b.amounts && a.energy == b.energy && a.momentum == b.momentum;
    }

    bool SameFraction(const GasTreeFraction& a, const GasTreeFraction& b) {
        return a.amounts == b.amounts && a.energy == b.energy && a.momentum == b.momentum;
    }

    bool SameTotals(const GasTreeTotals& a, const GasTreeTotals& b) {
        return a.amounts == b.amounts && a.energy == b.energy && a.momentum == b.momentum;
    }

    // 最初に食い違うセル(場所と、整数部か端数か)
    std::string FirstDifference(const GasTree& cpu, const std::vector<GasCell>& cells,
                                const std::vector<GasTreeFraction>& fractions, const GasTreeTotals& ledger) {
        for (uint32_t index = 0; index < cpu.cells.size(); ++index) {
            const bool cellDiffers = !SameCell(cpu.cells[index], cells[index]);
            if (!cellDiffers && SameFraction(cpu.fractions[index], fractions[index]))
                continue;

            const GasTreePlace& place = cpu.places[index];
            return std::format("セル {}(レベル {}・{}, {}, {})の{}(CPU のエネルギー {}・GPU {})", index, place.level,
                               place.coordinate[0], place.coordinate[1], place.coordinate[2],
                               cellDiffers ? "整数部" : "端数", cpu.cells[index].energy, cells[index].energy);
        }

        if (!SameTotals(cpu.ledger, ledger))
            return std::format("帳簿(エネルギーの whole: CPU {}・GPU {})", cpu.ledger.energy.whole,
                               ledger.energy.whole);

        return "(見つからない)";
    }

    struct GpuParts {
        gpu::ImmediateQueue queue;
        gpu::DebugRing debugRing;
    };

    uint32_t CountFractions(const std::vector<GasTreeFraction>& fractions);

    // 場面を GPU と CPU で 1 刻みずつ進め、毎刻み比べる。端数を持つセルの数を返す
    std::expected<uint32_t, std::string> RunScene(ID3D12Device5* device, GpuParts& parts, const Scene& scene) {
        auto gas = GpuGasTree::Create(device, scene.tree);
        if (!gas)
            return std::unexpected(gas.error());

        GasTree cpu = scene.tree;
        std::vector<GasCell> cells;
        std::vector<GasTreeFraction> fractions;
        GasTreeTotals ledger;
        for (uint64_t tick = 0; tick < scene.ticks; ++tick) {
            ID3D12GraphicsCommandList10* list = parts.queue.Begin();
            if (list == nullptr)
                return std::unexpected("コマンドリストを始められない");

            parts.debugRing.RecordBegin(list);
            if (tick == 0)
                gas->RecordUpload(list);

            gas->RecordStep(list, parts.debugRing.GpuAddress());
            gas->RecordReadback(list);
            parts.debugRing.RecordReadbackAndReset(list);
            if (!parts.queue.ExecuteAndWait())
                return std::unexpected("GPU での実行に失敗(デバイスが失われた)");

            const gpu::DebugRingContents debugOutput = parts.debugRing.Drain();
            if (debugOutput.assertCount > 0)
                return std::unexpected(
                    std::format("GPU の FX_ASSERT が {} 件(刻み {})", debugOutput.assertCount, tick));

            if (!gas->Read(cells, fractions, ledger))
                return std::unexpected("読み戻せない");

            StepGasTree(cpu);
            bool same = SameTotals(cpu.ledger, ledger);
            for (uint32_t index = 0; index < cpu.cells.size(); ++index)
                same = same && SameCell(cpu.cells[index], cells[index]) &&
                       SameFraction(cpu.fractions[index], fractions[index]);

            if (!same)
                return std::unexpected(std::format("刻み {} で CPU と食い違う: {}", tick + 1,
                                                   FirstDifference(cpu, cells, fractions, ledger)));
        }

        if (!scene.expectStill)
            return CountFractions(cpu.fractions);

        for (uint32_t index = 0; index < cells.size(); ++index) {
            if (!SameCell(cells[index], scene.tree.cells[index]))
                return std::unexpected(std::format("静止大気が動いた(セル {})", index));
        }

        return CountFractions(cpu.fractions);
    }

    // 端数を持つセルの数(レベルをまたぐ面の受け渡しを通ったか)
    uint32_t CountFractions(const std::vector<GasTreeFraction>& fractions) {
        uint32_t count = 0;
        for (const GasTreeFraction& fraction : fractions) {
            const bool any = fraction.energy != 0 || fraction.amounts[0] != 0 || fraction.momentum[0] != 0 ||
                             fraction.momentum[2] != 0;
            count += any ? 1 : 0;
        }

        return count;
    }

    struct TreeTestOptions {
        std::string scene;
        uint64_t ticks = 0;  // 0 = 場面の既定
    };

    // 自分の引数を取り除いて返す(残りは gpu_test_options.h へ)
    std::optional<TreeTestOptions> TakeTreeOptions(std::vector<char*>& arguments) {
        TreeTestOptions options;
        std::vector<char*> rest = {arguments.front()};
        for (size_t index = 1; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            const bool hasValue = index + 1 < arguments.size();
            if (argument == "--scene" && hasValue)
                options.scene = arguments[++index];
            else if (argument == "--ticks" && hasValue)
                options.ticks = std::stoull(arguments[++index]);
            else
                rest.push_back(arguments[index]);
        }

        arguments = rest;
        return options;
    }

    int Run(std::vector<char*> arguments) {
        const auto treeOptions = TakeTreeOptions(arguments);
        const auto options = test::ParseGpuTestOptions(std::span(arguments));
        if (!treeOptions || !options) {
            Log(Channel::Sim, Level::Error,
                "使い方: gpu_gas_tree_test [--warp] [--queue direct|compute] [--scene 名前] [--ticks n]");
            return 2;
        }

        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!device) {
            Log(Channel::Sim, Level::Error, "gpu_gas_tree_test: FAILED(デバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto debugRing = gpu::DebugRing::Create(device->Get());
        if (!queue || !debugRing) {
            Log(Channel::Sim, Level::Error, "gpu_gas_tree_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        GpuParts parts{.queue = std::move(*queue), .debugRing = std::move(*debugRing)};
        uint32_t sceneCount = 0;
        uint32_t failureCount = 0;  // 食い違っても残りの場面は走らせる
        for (Scene& scene : MakeScenes()) {
            if (!treeOptions->scene.empty() && treeOptions->scene != scene.name)
                continue;

            if (treeOptions->ticks != 0)
                scene.ticks = treeOptions->ticks;

            const auto started = chr::steady_clock::now();
            const auto result = RunScene(device->Get(), parts, scene);
            if (!result) {
                Log(Channel::Sim, Level::Error, "gpu_gas_tree_test: FAILED({}: {})", scene.name, result.error());
                ++failureCount;
                continue;
            }

            Log(Channel::Sim, Level::Info,
                "gpu_gas_tree_test: {}(セル {}・端数を持つセル {})の {} 刻みが毎刻み CPU とビット一致({:.1f} s)",
                scene.name, scene.tree.cells.size(), *result, scene.ticks,
                chr::duration<double>(chr::steady_clock::now() - started).count());
            ++sceneCount;
        }

        if (!test::PassesValidation(*device, "gpu_gas_tree_test") || failureCount > 0)
            return 1;

        Log(Channel::Sim, Level::Info, "gpu_gas_tree_test: OK({} 場面・adapter {})", sceneCount,
            gpu::AdapterKindName(options->adapter));
        return sceneCount > 0 ? 0 : 1;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::vector<char*>(argv, argv + argc));
    SingletonFinalizer::Finalize();

    return exitCode;
}
