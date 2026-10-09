// gpu_gas_test.cpp — 気体の流れの GPU 版(sim::GpuGas、shaders/sim/gas_step.hlsl。T-0185 G3 の第 1 段)が CPU リファレンス
// (sim/gas_reference の StepGas。G1・G2)と毎刻みビット一致することと、G1・G2 の合格の条件を GPU の結果でも満たすことを確かめる。
// 場面(gas_reference_test と同じ作り): 閉じた箱の保存・周期的な箱の運動量・静止大気・熱い泡・風で煙(MC と 1 次の風上のにじみの比べ)・
// 微量の成分(乱数の丸め)・minmod と切り捨て。刻みごとに GPU のセルと帳簿を読み戻し、CPU の状態のハッシュと帳簿を比べる。
// 引数: gpu_test_options.h(--warp・--queue)と
//   --scene <名前>: 1 つの場面だけ(既定は全部)
//   --substeps n・--passes n・--dump <ファイル>: 食い違いを調べる用(小刻みの数・小刻みの段をどこで止めるか・最初の刻みの後の中身を書き出す。
//     HW と WARP で書き出して diff すると、どの面のどの欄が違うかが分かる。T-0185)
//   --profile: CPU とは比べず、大きい箱(--size n、既定 32 → n × n × n)で 1 刻みの GPU 時間(タイムスタンプ)を測る(--ticks 既定 120)
#include "sim/gpu_gas.h"
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "sim/gas_reference.h"

#include <cmath>

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    // --- 試験の空気(gas_reference_test と同じ。N2 79% + O2 21% と、N2 と同じ値の煙の印)---
    constexpr uint32_t SPECIES_N2 = 0;
    constexpr uint32_t SPECIES_SMOKE = 2;
    constexpr std::array<uint32_t, 3> AIR_FRACTIONS_Q32 = {3393024163u, 901943132u, 0u};
    constexpr uint32_t AIR_TEMPERATURE = 293150;                   // mK
    constexpr uint64_t SURFACE_PRESSURE = 101325ull * 1000000ull;  // µPa
    constexpr int64_t METER_PER_SECOND = (int64_t)1 << 20;
    constexpr int64_t ROUGH_CELL_MASS = 150000;  // mg

    uint32_t passLimit = 9;         // --passes(調べる用。小刻みの段をここまでで止める)
    uint32_t substepsOverride = 0;  // --substeps(食い違いを小刻み 1 つで探す時)
    // --dump <ファイル>(調べる用): 最初の刻みの後の面・セルごとの合計・セルを 1 行 1 語でファイルへ(HW と WARP の出力を diff する)
    std::string dumpPath;

    GasConfig MakeAirConfig(uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ) {
        GasConfig config;
        config.sizeX = sizeX;
        config.sizeY = sizeY;
        config.sizeZ = sizeZ;
        config.speciesCount = 3;
        config.species[0] = {.molarMass = 28013, .heatCapacity = 29124, .formationEnergy = 0};
        config.species[1] = {.molarMass = 31999, .heatCapacity = 29378, .formationEnergy = 0};
        config.species[2] = {.molarMass = 28013, .heatCapacity = 29124, .formationEnergy = 0};
        if (substepsOverride != 0)
            config.substeps = substepsOverride;

        return config;
    }

    GasBox MakeAirBox(const GasConfig& config) {
        return MakeGasBox(config,
                          MakeHydrostaticReference(config, AIR_FRACTIONS_Q32, AIR_TEMPERATURE, SURFACE_PRESSURE));
    }

    void Heat(GasBox& box, uint32_t index, uint32_t temperature) {
        const GasCell old = box.cells[index];
        GasCell cell = MakeGasCell(box.config, old.amounts, temperature);
        cell.momentum = old.momentum;
        box.cells[index] = cell;
    }

    void AddSmoke(GasBox& box, uint32_t index) {
        GasCell& cell = box.cells[index];
        const uint64_t half = cell.amounts[SPECIES_N2] / 2;
        cell.amounts[SPECIES_N2] -= half;
        cell.amounts[SPECIES_SMOKE] += half;
    }

    uint32_t CellX(const GasBox& box, uint32_t index) {
        return index % box.config.sizeX;
    }

    uint32_t CellZ(const GasBox& box, uint32_t index) {
        return index / (box.config.sizeX * box.config.sizeY);
    }

    // --- 場面 ---

    struct Scene {
        std::string name;
        GasBox box;
        uint64_t ticks = 0;
    };

    // 閉じた箱に熱いセル・煙・動くセル(gas_reference_test の MakeDisturbedClosedBox)
    Scene MakeClosedScene() {
        GasBox box = MakeAirBox(MakeAirConfig(8, 8, 8));
        Heat(box, box.Index(3, 3, 1), AIR_TEMPERATURE + 50000);
        Heat(box, box.Index(4, 3, 1), AIR_TEMPERATURE + 50000);
        AddSmoke(box, box.Index(5, 5, 5));
        GasCell& moving = box.cells[box.Index(2, 6, 4)];
        moving.momentum[0] = ROUGH_CELL_MASS * 2 * METER_PER_SECOND;
        moving.momentum[2] = -ROUGH_CELL_MASS * METER_PER_SECOND;

        return {.name = "closed", .box = box, .ticks = 60};
    }

    // 周期的な箱(重力なし。大きさ 1 と 2 の周期的な軸も混ぜて、面が自分自身・同じ隣を 2 回向く形も確かめる)
    Scene MakePeriodicScene() {
        GasConfig config = MakeAirConfig(8, 2, 4);
        config.boundary = {GasBoundary::Periodic, GasBoundary::Periodic, GasBoundary::Periodic};
        config.gravity = false;
        std::vector<GasCell> reference = MakeHydrostaticReference(config, AIR_FRACTIONS_Q32, AIR_TEMPERATURE,
                                                                  SURFACE_PRESSURE);
        std::fill(reference.begin() + 1, reference.end(), reference.front());
        GasBox box = MakeGasBox(config, reference);
        Heat(box, box.Index(1, 1, 1), AIR_TEMPERATURE + 80000);
        box.cells[box.Index(5, 0, 2)].momentum = {ROUGH_CELL_MASS * 3 * METER_PER_SECOND,
                                                  -ROUGH_CELL_MASS * METER_PER_SECOND,
                                                  ROUGH_CELL_MASS * 2 * METER_PER_SECOND};
        AddSmoke(box, box.Index(6, 1, 0));

        return {.name = "periodic", .box = box, .ticks = 60};
    }

    Scene MakeSingleLayerScene() {
        GasConfig config = MakeAirConfig(6, 1, 3);
        config.boundary = {GasBoundary::Open, GasBoundary::Periodic, GasBoundary::Wall};
        config.windVelocity = {(int32_t)(METER_PER_SECOND), 0, 0};
        GasBox box = MakeAirBox(config);
        Heat(box, box.Index(2, 0, 0), AIR_TEMPERATURE + 40000);
        AddSmoke(box, box.Index(1, 0, 1));

        return {.name = "single_layer", .box = box, .ticks = 60};
    }

    Scene MakeStaticScene() {
        return {.name = "static", .box = MakeAirBox(MakeAirConfig(4, 4, 16)), .ticks = 300};
    }

    // 半径 1.5 セル(0.75 m)の +30 K の泡(中心 (5, 5, 4))。10 × 10 × 20 の閉じた箱で 2 秒
    Scene MakeBubbleScene() {
        GasBox box = MakeAirBox(MakeAirConfig(10, 10, 20));
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const int32_t dx = (int32_t)CellX(box, index) - 5;
            const int32_t dy = (int32_t)((index / box.config.sizeX) % box.config.sizeY) - 5;
            const int32_t dz = (int32_t)CellZ(box, index) - 4;
            if (dx * dx + dy * dy + dz * dz <= 2)
                Heat(box, index, AIR_TEMPERATURE + 30000);
        }

        return {.name = "bubble", .box = box, .ticks = 120};
    }

    // x は開いた境界で風 2 m/s、y は周期的、z は壁。4 セル幅の煙(x = 4〜7・z = 2〜3)を 3 秒
    Scene MakeWindScene(const char* name, GasReconstruction reconstruction, bool stochasticRounding) {
        GasConfig config = MakeAirConfig(32, 4, 6);
        config.boundary = {GasBoundary::Open, GasBoundary::Periodic, GasBoundary::Wall};
        config.windVelocity = {(int32_t)(2 * METER_PER_SECOND), 0, 0};
        config.reconstruction = reconstruction;
        config.stochasticRounding = stochasticRounding;
        config.randomSeed = 0x5EED;
        GasBox box = MakeAirBox(config);
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const uint32_t x = CellX(box, index);
            const uint32_t z = CellZ(box, index);
            if (x >= 4 && x <= 7 && z >= 2 && z <= 3)
                AddSmoke(box, index);
        }

        return {.name = name, .box = box, .ticks = 180};
    }

    // 微量の成分(1 セル 5 µmol × 8 セル)を風 2 m/s で 1 秒
    Scene MakeTraceScene() {
        Scene scene = MakeWindScene("trace", GasReconstruction::MonotonizedCentral, true);
        GasBox& box = scene.box;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const uint32_t z = CellZ(box, index);
            GasCell& cell = box.cells[index];
            cell.amounts[SPECIES_N2] += cell.amounts[SPECIES_SMOKE];
            cell.amounts[SPECIES_SMOKE] = CellX(box, index) == 4 && z >= 2 && z <= 3 ? 5 : 0;
        }

        scene.ticks = 60;
        return scene;
    }

    std::vector<Scene> MakeScenes() {
        std::vector<Scene> scenes;
        scenes.push_back(MakeClosedScene());
        scenes.push_back(MakePeriodicScene());
        scenes.push_back(MakeSingleLayerScene());
        scenes.push_back(MakeStaticScene());
        scenes.push_back(MakeBubbleScene());
        scenes.push_back(MakeWindScene("wind_mc", GasReconstruction::MonotonizedCentral, true));
        scenes.push_back(MakeWindScene("wind_upwind", GasReconstruction::Upwind, true));
        scenes.push_back(MakeWindScene("wind_minmod_floor", GasReconstruction::Minmod, false));
        scenes.push_back(MakeTraceScene());

        return scenes;
    }

    // --- 比べる ---

    bool SameLedger(const GasLedger& a, const GasLedger& b) {
        return a.amounts == b.amounts && a.energy == b.energy && a.momentum == b.momentum;
    }

    bool SameCell(const GasCell& a, const GasCell& b) {
        return a.amounts == b.amounts && a.energy == b.energy && a.momentum == b.momentum;
    }

    std::string FirstDifference(const GasBox& cpu, const std::vector<GasCell>& gpu) {
        for (size_t i = 0; i < cpu.cells.size() && i < gpu.size(); ++i) {
            const GasCell& a = cpu.cells[i];
            const GasCell& b = gpu[i];
            if (SameCell(a, b))
                continue;

            return std::format(
                "セル {}: 物質量 cpu {} {} {} / gpu {} {} {}、エネルギー cpu {} / gpu {}、運動量 cpu {} {} {} / gpu {} "
                "{} {}",
                i, a.amounts[0], a.amounts[1], a.amounts[2], b.amounts[0], b.amounts[1], b.amounts[2], a.energy,
                b.energy, a.momentum[0], a.momentum[1], a.momentum[2], b.momentum[0], b.momentum[1], b.momentum[2]);
        }

        return "セルは同じ(帳簿が違う)";
    }

    // 最初の刻みの後の GPU の中身を 1 行 1 語で(行頭に face/sum/cell と番号・語の番号)
    void DumpFirstTick(const GpuGas& gas, const std::vector<GasCell>& cells) {
        std::vector<uint64_t> faceWords;
        std::vector<uint64_t> sumWords;
        if (!gas.ReadDebug(faceWords, sumWords))
            return;

        std::ofstream out(dumpPath);
        constexpr size_t FACE_WORDS = 13;  // GasGpuFace(gas_gpu.hlsli)の 104 バイト
        constexpr size_t SUM_WORDS = 5;    // GasGpuCellSums の 40 バイト
        for (size_t i = 0; i < faceWords.size(); ++i)
            out << std::format("face {} {} {}\n", i / FACE_WORDS, i % FACE_WORDS, (int64_t)faceWords[i]);

        for (size_t i = 0; i < sumWords.size(); ++i)
            out << std::format("sum {} {} {}\n", i / SUM_WORDS, i % SUM_WORDS, sumWords[i]);

        for (size_t i = 0; i < cells.size(); ++i)
            out << std::format("cell {} {} {} {} {} {} {} {} {}\n", i, cells[i].amounts[0], cells[i].amounts[1],
                               cells[i].amounts[2], cells[i].amounts[3], cells[i].energy, cells[i].momentum[0],
                               cells[i].momentum[1], cells[i].momentum[2]);
    }

    struct GpuParts {
        gpu::ImmediateQueue queue;
        gpu::DebugRing debugRing;
    };

    // 場面を GPU と CPU で 1 刻みずつ進め、毎刻み比べる。GPU の最後の状態を(CPU の箱の形で)返す
    std::expected<GasBox, std::string> RunScene(ID3D12Device5* device, GpuParts& parts, const Scene& scene) {
        auto gas = GpuGas::Create(device, scene.box);
        if (!gas)
            return std::unexpected(gas.error());

        gas->SetDebugPassLimit(passLimit);
        GasBox cpu = scene.box;
        GasBox gpuState = scene.box;
        for (uint64_t tick = 0; tick < scene.ticks; ++tick) {
            ID3D12GraphicsCommandList10* list = parts.queue.Begin();
            if (list == nullptr)
                return std::unexpected("コマンドリストを始められない");

            parts.debugRing.RecordBegin(list);
            if (tick == 0)
                gas->RecordUpload(list);

            gas->RecordStep(list, parts.debugRing.GpuAddress());
            gas->RecordReadback(list);
            if (tick == 0 && !dumpPath.empty())
                gas->RecordDebugReadback(list);

            parts.debugRing.RecordReadbackAndReset(list);
            if (!parts.queue.ExecuteAndWait())
                return std::unexpected("GPU での実行に失敗(デバイスが失われた)");

            const gpu::DebugRingContents debugOutput = parts.debugRing.Drain();
            if (debugOutput.assertCount > 0)
                return std::unexpected(
                    std::format("GPU の FX_ASSERT が {} 件(刻み {})", debugOutput.assertCount, tick));

            if (!gas->Read(gpuState.cells, gpuState.ledger))
                return std::unexpected("読み戻せない");

            if (tick == 0 && !dumpPath.empty())
                DumpFirstTick(*gas, gpuState.cells);

            gpuState.tick = tick + 1;
            StepGas(cpu);
            if (HashGas(cpu) != HashGas(gpuState) || !SameLedger(cpu.ledger, gpuState.ledger)) {
                return std::unexpected(
                    std::format("刻み {} で CPU と食い違う: {}", tick + 1, FirstDifference(cpu, gpuState.cells)));
            }
        }

        return gpuState;
    }

    // --- G1・G2 の合格の条件(GPU の結果で)---

    int64_t MaxSpeed(const GasBox& box) {
        int64_t speed = 0;
        for (const GasCell& cell : box.cells) {
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                speed = std::max(speed, std::abs(GasVelocity(box, cell, axis)));
        }

        return speed;
    }

    int64_t HeatCentroidZ(const GasBox& box) {
        int64_t weighted = 0;
        int64_t total = 0;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const uint32_t z = CellZ(box, index);
            const int64_t excess = (int64_t)DeriveGasCell(box, box.cells[index], z).temperature -
                                   (int64_t)box.referenceDerived[z].temperature;
            if (excess <= 0)
                continue;

            weighted += excess * z;
            total += excess;
        }

        return total == 0 ? 0 : weighted * 1000 / total;
    }

    struct SmokeProfile {
        double centroid = 0;
        double spread = 0;
        double maxRatio = 0;
    };

    SmokeProfile MeasureSmoke(const GasBox& box) {
        double total = 0;
        double first = 0;
        double second = 0;
        SmokeProfile profile;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const GasCell& cell = box.cells[index];
            const auto smoke = (double)cell.amounts[SPECIES_SMOKE];
            const double x = CellX(box, index);
            total += smoke;
            first += smoke * x;
            second += smoke * x * x;
            const auto amount = (double)(cell.amounts[0] + cell.amounts[1] + cell.amounts[2]);
            profile.maxRatio = std::max(profile.maxRatio, smoke / amount);
        }

        profile.centroid = first / total;
        profile.spread = std::sqrt(std::max(0.0, (second / total) - (profile.centroid * profile.centroid)));

        return profile;
    }

    // 保存: 成分とエネルギーは初め + 帳簿、運動量も初め + 帳簿(閉じた箱なら帳簿の成分とエネルギーは 0)
    std::string CheckConservation(const GasBox& initial, const GasBox& final) {
        const GasTotals before = SumGas(initial);
        const GasTotals after = SumGas(final);
        for (uint32_t s = 0; s < initial.config.speciesCount; ++s) {
            if ((int64_t)after.amounts[s] != (int64_t)before.amounts[s] + final.ledger.amounts[s])
                return std::format("成分 {} が保存されない", s);
        }

        if (after.energy != before.energy + final.ledger.energy)
            return "エネルギーが保存されない";

        for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
            if (after.momentum[axis] != before.momentum[axis] + final.ledger.momentum[axis])
                return std::format("運動量 {} が「初め + 帳簿」と合わない", axis);
        }

        return {};
    }

    std::string CheckScene(const Scene& scene, const GasBox& final,
                           std::vector<std::pair<std::string, GasBox>>& results) {
        results.emplace_back(scene.name, final);
        if (std::string error = CheckConservation(scene.box, final); !error.empty())
            return error;

        if (scene.name == "closed" || scene.name == "bubble") {
            if (final.ledger.amounts != std::array<int64_t, GAS_MAX_SPECIES>{} || final.ledger.energy != 0)
                return "閉じた箱の帳簿に物かエネルギーが出入りした";
        }

        if (scene.name == "periodic" && final.ledger.momentum != std::array<int64_t, GAS_AXES>{})
            return "周期的な箱の帳簿に運動量が出入りした";

        if (scene.name == "static" && MaxSpeed(final) > 1)
            return std::format("静止大気が動いた(最大 {} 単位)", MaxSpeed(final));

        if (scene.name == "bubble") {
            const int64_t rise = HeatCentroidZ(final) - HeatCentroidZ(scene.box);
            const int64_t upward = GasVelocity(final, final.cells[final.Index(5, 5, 6)], 2);
            Log(Channel::Sim, Level::Info,
                "  熱い泡: 熱の重心が 2 秒で {} / 1000 セル上がる・泡の上の w = {} (2^-20 m/s)", rise, upward);
            if (rise < 1000 || upward < METER_PER_SECOND / 2)
                return "熱い泡が上がらない";
        }

        if (scene.name == "wind_mc" || scene.name == "trace") {
            const double shift = MeasureSmoke(final).centroid - MeasureSmoke(scene.box).centroid;
            const double expected = scene.name == "trace" ? 4.0 : 12.0;
            Log(Channel::Sim, Level::Info, "  {}: 煙の重心が {:.2f} セル流れる(風の速さどおりなら {:.0f})", scene.name,
                shift, expected);
            if (std::abs(shift - expected) > expected * 0.25)
                return "風で煙が流れない";
        }

        // MUSCL(MC)のにじみが 1 次の風上の 0.6 倍以下・煙の割合が初めの最大を超えない(G2)
        if (scene.name == "wind_upwind") {
            const auto mc = std::ranges::find(results, std::string("wind_mc"), &std::pair<std::string, GasBox>::first);
            if (mc == results.end())
                return {};

            const SmokeProfile initial = MeasureSmoke(scene.box);
            const SmokeProfile muscl = MeasureSmoke(mc->second);
            const SmokeProfile upwind = MeasureSmoke(final);
            Log(Channel::Sim, Level::Info,
                "  にじみ(3 秒): 広がり 風上 {:.2f}・MC {:.2f} セル、煙の割合の最大 初め {:.3f}・MC {:.3f}",
                upwind.spread, muscl.spread, initial.maxRatio, muscl.maxRatio);
            if (muscl.spread > upwind.spread * 0.6 || muscl.maxRatio > initial.maxRatio)
                return "MUSCL で煙のにじみが減らない";
        }

        return {};
    }

    // --- 計測: CPU と比べず、大きい箱で 1 刻みの GPU 時間 ---
    int Profile(ID3D12Device5* device, GpuParts& parts, uint32_t size, uint64_t ticks) {
        GasConfig config = MakeAirConfig(size, size, size);
        config.boundary = {GasBoundary::Open, GasBoundary::Periodic, GasBoundary::Wall};
        config.windVelocity = {(int32_t)(2 * METER_PER_SECOND), 0, 0};
        GasBox box = MakeAirBox(config);
        for (uint32_t index = 0; index < box.cells.size(); index += 7)
            AddSmoke(box, index);

        auto gas = GpuGas::Create(device, box);
        if (!gas || !gas->EnableTiming(device)) {
            Log(Channel::Sim, Level::Error, "gpu_gas_test: FAILED(計測の準備に失敗)");
            return 1;
        }

        uint64_t frequency = 0;
        parts.queue.Native()->GetTimestampFrequency(&frequency);
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
                Log(Channel::Sim, Level::Error, "gpu_gas_test: FAILED(GPU での実行に失敗)");
                return 1;
            }

            (void)parts.debugRing.Drain();
            const std::optional<uint64_t> elapsed = gas->TimingTicks();
            if (!elapsed || frequency == 0)
                return 1;

            const double milliseconds = (double)*elapsed * 1000.0 / (double)frequency / (double)ticks;
            Log(Channel::Sim, Level::Info, "gpu_gas_test: 計測 {} 回目 {}³ = {} セル・小刻み {}: 1 刻み {:.3f} ms",
                round, size, box.cells.size(), config.substeps, milliseconds);
        }

        return 0;
    }

    struct GasTestOptions {
        std::string scene;
        bool profile = false;
        uint32_t size = 32;
        uint64_t ticks = 120;
    };

    // 自分の引数を取り除いて返す(残りは gpu_test_options.h へ)
    std::optional<GasTestOptions> TakeGasOptions(std::vector<char*>& arguments) {
        GasTestOptions options;
        std::vector<char*> rest = {arguments.front()};
        for (size_t index = 1; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            const bool hasValue = index + 1 < arguments.size();
            if (argument == "--scene" && hasValue)
                options.scene = arguments[++index];
            else if (argument == "--profile")
                options.profile = true;
            else if (argument == "--size" && hasValue)
                options.size = (uint32_t)std::stoul(arguments[++index]);
            else if (argument == "--substeps" && hasValue)
                substepsOverride = (uint32_t)std::stoul(arguments[++index]);
            else if (argument == "--passes" && hasValue)
                passLimit = (uint32_t)std::stoul(arguments[++index]);
            else if (argument == "--dump" && hasValue)
                dumpPath = arguments[++index];
            else if (argument == "--ticks" && hasValue)
                options.ticks = std::stoull(arguments[++index]);
            else
                rest.push_back(arguments[index]);
        }

        arguments = rest;
        return options;
    }

    int Run(std::vector<char*> arguments) {
        const auto gasOptions = TakeGasOptions(arguments);
        const auto options = test::ParseGpuTestOptions(std::span(arguments));
        if (!gasOptions || !options) {
            Log(Channel::Sim, Level::Error,
                "使い方: gpu_gas_test [--warp] [--queue direct|compute] [--scene 名前] [--substeps n] [--passes n] "
                "[--dump ファイル] [--profile [--size n] [--ticks n]]");
            return 2;
        }

        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!device) {
            Log(Channel::Sim, Level::Error, "gpu_gas_test: FAILED(デバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto debugRing = gpu::DebugRing::Create(device->Get());
        if (!queue || !debugRing) {
            Log(Channel::Sim, Level::Error, "gpu_gas_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        GpuParts parts{.queue = std::move(*queue), .debugRing = std::move(*debugRing)};
        if (gasOptions->profile)
            return Profile(device->Get(), parts, gasOptions->size, gasOptions->ticks);

        std::vector<std::pair<std::string, GasBox>> results;
        uint32_t sceneCount = 0;
        uint32_t failureCount = 0;  // 食い違っても残りの場面は走らせる(どの場面が壊れたかを一度に見る)
        for (const Scene& scene : MakeScenes()) {
            if (!gasOptions->scene.empty() && gasOptions->scene != scene.name)
                continue;

            const auto started = chr::steady_clock::now();
            const auto final = RunScene(device->Get(), parts, scene);
            if (!final) {
                Log(Channel::Sim, Level::Error, "gpu_gas_test: FAILED({}: {})", scene.name, final.error());
                ++failureCount;
                continue;
            }

            if (const std::string error = CheckScene(scene, *final, results); !error.empty()) {
                Log(Channel::Sim, Level::Error, "gpu_gas_test: FAILED({}: {})", scene.name, error);
                ++failureCount;
                continue;
            }

            Log(Channel::Sim, Level::Info, "gpu_gas_test: {} の {} 刻みが毎刻み CPU とビット一致({:.1f} s)", scene.name,
                scene.ticks, chr::duration<double>(chr::steady_clock::now() - started).count());
            ++sceneCount;
        }

        if (!test::PassesValidation(*device, "gpu_gas_test") || failureCount > 0)
            return 1;

        Log(Channel::Sim, Level::Info, "gpu_gas_test: OK({} 場面・adapter {})", sceneCount,
            gpu::AdapterKindName(options->adapter));
        return sceneCount > 0 ? 0 : 1;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::vector<char*>(argv, argv + argc));
    SingletonFinalizer::Finalize();

    return exitCode;
}
