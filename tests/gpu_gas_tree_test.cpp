// gpu_gas_tree_test.cpp — 気体の多重解像度の木の GPU 版(sim::GpuGasTree、shaders/sim/gas_tree_step.hlsl。T-0209・T-0356)が CPU リファレンス
// (sim/gas_tree の StepGasTree。T-0208・T-0320)と毎刻みビット一致することを確かめる。全部のレベルが同じ小刻み(σ = 0)の木と、
// レベルごとの小刻み(レベル 0 は 24・レベル 1 は 48。gas_tree_test の SUBCYCLED。名前が subcycled_ の場面)の木。
// 場面: 閉じた 2 レベルの箱の乱れ(gas_tree_test の MakeDisturbedTwoLevelBox。熱い・冷たいセル・煙・動くセル。端数を通る)・
// レベルをまたぐ静止大気(動かないことも)・開いた境界と風(細かい範囲が開いた境界に接する。MC・乱数の丸め)・
// minmod と切り捨て・全部レベル 0 の木(周期的な箱)。分けた形は閉じた箱・静止大気・開いた境界と風と、粗いセルが細かい範囲へ速く流れ込む箱
// (粗い側から細かい面が何度も引く時の抑え〔committedOutflow・taken〕を通る。CPU の GasTreeStepStats で通ったことも確かめる)。刻みごとに GPU のセル・端数・帳簿(whole + 2^-64)を読み戻して CPU と比べる。
// 引数: gpu_test_options.h(--warp・--queue)と --scene <名前>(1 つの場面だけ)・--ticks n(場面の刻みの数を替える)
//   --profile: CPU とは比べず、大きい木(レベル 0 の n³〔--size n、既定 16〕の真ん中 (n/2)³ をレベル 1 に)で 1 刻みの GPU 時間(タイムスタンプ)を測る
//              (--ticks 既定 20。--subcycled でレベルごとの小刻み〔レベル 0 は 24・レベル 1 は 48〕、無ければ全部 48。
//              --uniform で細かくしない〔全部レベル 0 の n³〕。T-0358)
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

    GasTree MakeAirTree(const GasConfig& config, std::span<const GasTreeRegion> regions,
                        const GasTreeOptions& options = {}) {
        return MakeGasTree(config, regions, AIR_FRACTIONS_Q32, AIR_TEMPERATURE, SURFACE_PRESSURE, options);
    }

    // レベルごとの小刻み(T-0320): レベル 0 は 24 回(ν = 0.47)、レベル 1 は 24 × 2 回(ν = 0.47)。gas_tree_test と同じ
    constexpr GasTreeOptions SUBCYCLED = {.subcycleBaseLevel = 0, .maxSubcycleGap = 1};

    GasConfig MakeSubcycledConfig() {
        GasConfig config = MakeAirConfig(8, 4, 8);
        config.substeps = 24;
        return config;
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
        bool expectLimits =
            false;  // 粗い側から細かい面が何度も引く時の抑えを通ることも確かめる(CPU の GasTreeStepStats)
    };

    // 8 × 4 × 8 のレベル 0 の真ん中 4 × 4 を x と z でレベル 1 に(y は全部。gas_tree_test の MIDDLE_REGION)
    constexpr std::array<GasTreeRegion, 1> MIDDLE_REGION = {
        GasTreeRegion{.level = 1, .origin = {4, 0, 4}, .size = {8, 8, 8}}};

    // gas_tree_test の MakeDisturbedTwoLevelBox(境の両側の熱いセル・冷たいセル・煙・動く粗いセル)
    GasTree MakeDisturbedTwoLevelBox(const GasConfig& config, const GasTreeOptions& options = {}) {
        GasTree tree = MakeAirTree(config, MIDDLE_REGION, options);
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

    // --- 分けた形(T-0356)---

    Scene MakeSubcycledClosedScene() {
        return {
            .name = "subcycled_closed", .tree = MakeDisturbedTwoLevelBox(MakeSubcycledConfig(), SUBCYCLED), .ticks = 8};
    }

    Scene MakeSubcycledStaticScene() {
        return {.name = "subcycled_static",
                .tree = MakeAirTree(MakeSubcycledConfig(), MIDDLE_REGION, SUBCYCLED),
                .ticks = 4,
                .expectStill = true};
    }

    // 開いた境界と風(乱数の丸めの小刻みの通し番号がレベルごと)
    Scene MakeSubcycledOpenWindScene() {
        GasConfig config = MakeSubcycledConfig();
        config.boundary = {GasBoundary::Open, GasBoundary::Periodic, GasBoundary::Wall};
        config.windVelocity = {(int32_t)(2 * METER_PER_SECOND), 0, 0};
        config.randomSeed = 0x5EED;
        const std::array<GasTreeRegion, 1> region = {GasTreeRegion{.level = 1, .origin = {0, 0, 2}, .size = {6, 8, 6}}};
        GasTree tree = MakeAirTree(config, region, SUBCYCLED);
        AddSmoke(tree.cells[tree.Find(1, 1, 3, 3)]);
        AddSmoke(tree.cells[tree.Find(0, 4, 2, 2)]);
        Heat(config, tree.cells[tree.Find(1, 2, 6, 4)], AIR_TEMPERATURE + 40000);
        Heat(config, tree.cells[tree.Find(0, 5, 1, 1)], AIR_TEMPERATURE + 40000);
        return {.name = "subcycled_open_wind", .tree = tree, .ticks = 8};
    }

    // 細かい範囲(レベル 0 の x 2〜5)の手前の粗いセルの列が +x へ 1500 m/s で流れ込む: 粗いセルの 1 面の 4 枚の細かい面が粗い小刻みの間に
    // 2 回ずつ引き、出ていく割合の合計が 1 を超える(× 2^g で数える・使える分 = 1 − committedOutflow・中身 − taken の抑えを通る)
    Scene MakeSubcycledFastScene() {
        GasConfig config = MakeSubcycledConfig();
        config.randomSeed = 0xFA57;
        GasTree tree = MakeAirTree(config, MIDDLE_REGION, SUBCYCLED);
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t z = 2; z < 6; ++z)
                SetMoving(tree.cells[tree.Find(0, 1, y, z)], 1500, 0);
        }

        AddSmoke(tree.cells[tree.Find(0, 1, 1, 3)]);
        return {.name = "subcycled_fast", .tree = tree, .ticks = 4, .expectLimits = true};
    }

    std::vector<Scene> MakeScenes() {
        std::vector<Scene> scenes;
        scenes.push_back(MakeClosedScene());
        scenes.push_back(MakeStaticScene());
        scenes.push_back(MakeOpenWindScene());
        scenes.push_back(MakeMinmodFloorScene());
        scenes.push_back(MakeSingleLevelScene());
        scenes.push_back(MakeSubcycledClosedScene());
        scenes.push_back(MakeSubcycledStaticScene());
        scenes.push_back(MakeSubcycledOpenWindScene());
        scenes.push_back(MakeSubcycledFastScene());

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
        GasTreeStepStats stats;
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

            StepGasTree(cpu, &stats);
            bool same = SameTotals(cpu.ledger, ledger);
            for (uint32_t index = 0; index < cpu.cells.size(); ++index)
                same = same && SameCell(cpu.cells[index], cells[index]) &&
                       SameFraction(cpu.fractions[index], fractions[index]);

            if (!same)
                return std::unexpected(std::format("刻み {} で CPU と食い違う: {}", tick + 1,
                                                   FirstDifference(cpu, cells, fractions, ledger)));
        }

        if (cpu.maxSubcycleExponent != 0)
            Log(Channel::Sim, Level::Info,
                "gpu_gas_tree_test: {} の抑え(CPU): 割合 {}(粗い風上 {}・使える分 < 1 {})・物質量 {}(出した分を引いた "
                "{})",
                scene.name, stats.limitedOutflow, stats.limitedSubcycledOutflow, stats.limitedAfterCommitted,
                stats.limitedAmounts, stats.limitedAfterTaken);

        if (scene.expectLimits && (stats.limitedSubcycledOutflow == 0 || stats.limitedAfterCommitted == 0))
            return std::unexpected("粗い側から細かい面が何度も引く時の抑えを通らなかった(場面を強くする)");

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
        bool profile = false;
        bool subcycled = false;  // 計測: レベルごとの小刻み
        bool uniform =
            false;  // 計測: 細かくしない(全部レベル 0 の木。1 レベルの箱の GPU と同じ形の場面で、木の段の形の費用を見る)
        uint32_t size = 16;  // 計測: レベル 0 の箱の一辺(4 の倍数)
    };

    // --- 計測(T-0358): CPU と比べず、大きい木で 1 刻みの GPU 時間 ---
    // 閉じた箱(重さあり)の真ん中をレベル 1 に。同じいちばん細かい細かさの一様な箱は gpu_gas_test --profile --size 2n(小刻みはレベル 0 の 2 倍が要る)
    int Profile(ID3D12Device5* device, GpuParts& parts, const TreeTestOptions& options);

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
            else if (argument == "--size" && hasValue)
                options.size = (uint32_t)std::stoul(arguments[++index]);
            else if (argument == "--profile")
                options.profile = true;
            else if (argument == "--subcycled")
                options.subcycled = true;
            else if (argument == "--uniform")
                options.uniform = true;
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
                "使い方: gpu_gas_tree_test [--warp] [--queue direct|compute] [--scene 名前] [--ticks n] "
                "[--profile [--size n] [--subcycled] [--uniform]]");
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
        if (treeOptions->profile)
            return Profile(device->Get(), parts, *treeOptions);

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

    int Profile(ID3D12Device5* device, GpuParts& parts, const TreeTestOptions& options) {
        const uint32_t size = options.size;
        GasConfig config = MakeAirConfig(size, size, size);
        if (options.subcycled)
            config.substeps = 24;

        const std::array<GasTreeRegion, 1> region = {
            GasTreeRegion{.level = 1, .origin = {size / 2, size / 2, size / 2}, .size = {size, size, size}}};
        const std::span<const GasTreeRegion> regions = options.uniform ? std::span<const GasTreeRegion>()
                                                                       : std::span<const GasTreeRegion>(region);
        GasTree tree = MakeAirTree(config, regions, options.subcycled ? SUBCYCLED : GasTreeOptions{});
        for (uint32_t index = 0; index < tree.cells.size(); index += 7)
            AddSmoke(tree.cells[index]);

        for (uint32_t index = 3; index < tree.cells.size(); index += 11)
            Heat(config, tree.cells[index], AIR_TEMPERATURE + 30000);

        auto gas = GpuGasTree::Create(device, tree);
        if (!gas || !gas->EnableTiming(device)) {
            Log(Channel::Sim, Level::Error, "gpu_gas_tree_test: FAILED(計測の準備に失敗)");
            return 1;
        }

        uint64_t frequency = 0;
        parts.queue.Native()->GetTimestampFrequency(&frequency);
        const uint64_t ticks = options.ticks != 0 ? options.ticks : 20;
        for (uint32_t round = 0; round < 3; ++round) {  // 1 回目はパイプラインの初回の分を含むので捨てる
            ID3D12GraphicsCommandList10* list = parts.queue.Begin();
            parts.debugRing.RecordBegin(list);
            if (round == 0)
                gas->RecordUpload(list);

            gas->RecordTimestamp(list, 0);
            for (uint64_t tick = 0; tick < ticks; ++tick)
                gas->RecordStep(list, parts.debugRing.GpuAddress());

            gas->RecordTimestamp(list, 1);
            gas->RecordReadback(list);
            gas->RecordTimingReadback(list);
            parts.debugRing.RecordReadbackAndReset(list);
            if (!parts.queue.ExecuteAndWait()) {
                Log(Channel::Sim, Level::Error, "gpu_gas_tree_test: FAILED(GPU での実行に失敗)");
                return 1;
            }

            (void)parts.debugRing.Drain();
            const std::optional<uint64_t> elapsed = gas->TimingTicks();
            if (!elapsed || frequency == 0)
                return 1;

            const double milliseconds = (double)*elapsed * 1000.0 / (double)frequency / (double)ticks;
            Log(Channel::Sim, Level::Info,
                "gpu_gas_tree_test: 計測 {} 回目 レベル 0 {}³{} = {} セル・面 {}・{}・1 刻みの段 "
                "{}: 1 刻み {:.3f} ms",
                round, size, options.uniform ? "(細かくしない)" : " の真ん中をレベル 1 に", tree.cells.size(),
                gas->FaceCount(), options.subcycled ? "小刻み 24 / 48" : "小刻み 48", gas->PassesPerTick(),
                milliseconds);
        }

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::vector<char*>(argv, argv + argc));
    SingletonFinalizer::Finalize();

    return exitCode;
}
