// gpu_sun_column_test.cpp — 太陽の影の柱の GPU(sim/gpu_sun_column。shaders/sim/sun_column.hlsl)が CPU リファレンス
// (sim/sun_column の SweepSunColumn)とビット一致すること(T-0910・23 §10 R-R0)。場面は tests/sun_column_scene.h(CPU の試験と同じ)。
//   壁(0.5 m・0.25 m の世界)・温室・一部を細かくした木 × 真上・斜め・3 軸とも斜めの太陽で、出力の語(帳簿 + セルごとの吸った・返した)が全部同じ。
//   同じ場面を 2 回掃いても同じ。空との交換の口(sim/sky_port。T-1145)の掃きの和も CPU と同じ。費用: 64³ の地形(0.5 m)を真上と斜めの太陽で(release の HW。docs/perf.md)。
// 引数は gpu_test_options.h。
#include <array>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "sim/gpu_multires.h"
#include "sim/gpu_sun_column.h"
#include "sim/sky_port.h"
#include "sun_column_scene.h"

using namespace bicameral;
namespace mr = bicameral::multires;

namespace {

    constexpr uint32_t MEASURE_WARMUP = 20;
    constexpr uint32_t MEASURE_REPEATS = 10;

    struct Scene {
        const char* name;
        sim::MultiresNest nest;
        int32_t rayLevel;
    };

    struct GpuContext {
        ID3D12Device5* device = nullptr;
        gpu::ImmediateQueue* queue = nullptr;
        gpu::DebugRing* ring = nullptr;
        const sim::BakedReactionTable* table = nullptr;
    };

    sim::MultiresNest RefinedWall(const sim::BakedReactionTable& table) {
        sim::MultiresNest nest = test::MakeSunWorld(table, 0, 1, 1, 1, test::WallScene, 1);
        const mr::MrRequest refine = mr::MrMakeRequest(mr::MR_REQUEST_REFINE, 1, 2, 2, 2);
        sim::SubmitRequests(nest, std::span(&refine, 1));
        sim::ProcessRequests(nest);

        return nest;
    }

    std::vector<Scene> MakeScenes(const sim::BakedReactionTable& table) {
        std::vector<Scene> scenes;
        scenes.push_back({"壁 0.5 m", test::MakeSunWorld(table, 0, 1, 1, 1, test::WallScene), 0});
        scenes.push_back({"壁 0.25 m", test::MakeSunWorld(table, 1, 2, 2, 2, test::WallScene), 1});
        scenes.push_back({"温室 0.5 m", test::MakeSunWorld(table, 0, 1, 1, 1, test::GlassHouse), 0});
        scenes.push_back({"ガラスの板 0.25 m", test::MakeSunWorld(table, 1, 2, 2, 2, test::GroundAndPanel), 1});
        scenes.push_back({"一部を細かくした壁", RefinedWall(table), 1});

        return scenes;
    }

    std::array<sim::SunLight, 3> Suns() {
        return {sim::SunLight{.direction = {0, -1, 0},
                              .irradianceShortMilliwatts = test::SUN_IRRADIANCE_SHORT,
                              .irradianceLongMilliwatts = test::SUN_IRRADIANCE_LONG},
                sim::SunLight{.direction = {2, -3, 0},
                              .irradianceShortMilliwatts = test::SUN_IRRADIANCE_SHORT,
                              .irradianceLongMilliwatts = test::SUN_IRRADIANCE_LONG},
                sim::SunLight{.direction = {-3, -5, 2},
                              .irradianceShortMilliwatts = test::SUN_IRRADIANCE_SHORT,
                              .irradianceLongMilliwatts = test::SUN_IRRADIANCE_LONG}};
    }

    // 場面 1 つの GPU の木と影の柱(パイプラインの作成が重いので、場面ごとに 1 回作って太陽を替えて使い回す)
    struct GpuScene {
        sim::GpuMultires nest;
        sim::GpuSunColumn sun;
    };

    std::expected<GpuScene, std::string> MakeGpuScene(const GpuContext& context, const sim::MultiresNest& nest) {
        auto gpu = sim::GpuMultires::Create(context.device, *context.table, nest.capacity);
        if (!gpu)
            return std::unexpected(gpu.error());

        auto sun = sim::GpuSunColumn::Create(context.device, *gpu, *context.table, nest.capacity.worldBlocks);
        if (!sun)
            return std::unexpected(sun.error());

        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr || !gpu->RecordUpload(list, nest) || !context.queue->ExecuteAndWait())
            return std::unexpected("木を写せない");

        return GpuScene{.nest = std::move(*gpu), .sun = std::move(*sun)};
    }

    // 掃いて読み戻す(repeats 回掃く。averageMs があれば暖機の後に、前後のタイムスタンプから 1 回の平均)
    std::expected<std::vector<uint64_t>, std::string> RunOnGpu(const GpuContext& context, GpuScene& scene,
                                                               const mr::SunParams& params, uint32_t repeats,
                                                               double* averageMs) {
        const D3D12_GPU_VIRTUAL_ADDRESS ringAddress = context.ring->GpuAddress();

        // --- 暖機(計測の時だけ。GPU のクロックを上げる)---
        if (averageMs != nullptr) {
            ID3D12GraphicsCommandList10* warm = context.queue->Begin();
            if (warm == nullptr || !scene.sun.RecordWorkUpload(warm, params))
                return std::unexpected("暖機の作業を写せない");

            context.ring->RecordBegin(warm);
            for (uint32_t round = 0; round < MEASURE_WARMUP; ++round)
                scene.sun.RecordSweep(warm, scene.nest, ringAddress);

            context.ring->RecordReadbackAndReset(warm);
            if (!context.queue->ExecuteAndWait())
                return std::unexpected("暖機で掃けない");

            (void)context.ring->Drain();
        }

        // --- 掃く(タイムスタンプは最初と最後)---
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr || !scene.sun.RecordWorkUpload(list, params))
            return std::unexpected("作業を写せない");

        context.ring->RecordBegin(list);
        scene.nest.RecordTimestamp(list, 0);
        for (uint32_t repeat = 0; repeat < repeats; ++repeat)
            scene.sun.RecordSweep(list, scene.nest, ringAddress);

        scene.nest.RecordTimestamp(list, 1);
        scene.nest.RecordReadback(list);  // タイムスタンプを写す
        scene.sun.RecordReadback(list);
        context.ring->RecordReadbackAndReset(list);
        if (!context.queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = context.ring->Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        uint64_t frequency = 0;
        const std::vector<uint64_t> stamps = scene.nest.ReadTimestamps(2);
        if (averageMs != nullptr && SUCCEEDED(context.queue->Native()->GetTimestampFrequency(&frequency)) &&
            stamps.size() == 2 && frequency != 0)
            *averageMs = static_cast<double>(stamps[1] - stamps[0]) * 1000.0 /
                         (static_cast<double>(frequency) * static_cast<double>(repeats));

        std::vector<uint64_t> words = scene.sun.Read();
        if (words.empty())
            return std::unexpected("読み戻せない");

        return words;
    }

    // 食い違いの最初の語を表示
    void ReportMismatch(std::span<const uint64_t> cpu, std::span<const uint64_t> gpu, const std::string& where) {
        if (cpu.size() != gpu.size()) {
            Log(Channel::Gpu, Level::Error, "{}: 語の数が違う(CPU {}・GPU {})", where, cpu.size(), gpu.size());
            return;
        }

        for (size_t word = 0; word < cpu.size(); ++word) {
            if (cpu[word] != gpu[word]) {
                Log(Channel::Gpu, Level::Error, "{}: 語 {} が違う(CPU {}・GPU {})", where, word, cpu[word], gpu[word]);
                return;
            }
        }
    }

    std::expected<uint32_t, std::string> CompareScenes(const GpuContext& context) {
        uint32_t compared = 0;
        for (const Scene& scene : MakeScenes(*context.table)) {
            auto gpuScene = MakeGpuScene(context, scene.nest);
            if (!gpuScene)
                return std::unexpected(std::format("{}: {}", scene.name, gpuScene.error()));

            for (const sim::SunLight& light : Suns()) {
                const auto params = sim::MakeSunParams(scene.nest, light, scene.rayLevel);
                if (!params)
                    return std::unexpected(std::format("{}: {}", scene.name, params.error()));

                const std::vector<uint64_t> cpu = sim::SweepSunColumn(scene.nest, *context.table, *params);
                const auto gpu = RunOnGpu(context, *gpuScene, *params, 2, nullptr);  // 2 回: 0 にし直すことも確かめる
                if (!gpu)
                    return std::unexpected(std::format("{}: {}", scene.name, gpu.error()));

                const std::string where = std::format("{}・向き {},{},{}", scene.name, light.direction[0],
                                                      light.direction[1], light.direction[2]);
                if (cpu != *gpu) {
                    ReportMismatch(cpu, *gpu, where);
                    return std::unexpected(where + ": GPU と CPU が食い違う");
                }

                const uint64_t entered = cpu[mr::SunLedgerWord(0, mr::SUN_LEDGER_ENTERED)];
                Log(Channel::Gpu, Level::Info, "{}: ビット一致(語 {}・短い帯の入り {})", where, cpu.size(), entered);
                ++compared;
            }
        }

        return compared;
    }

    // 空との交換の口(sim/sky_port。T-1145): 口の掃きを 1 本ずつ GPU で走らせて正と負に足した語が、CPU の SweepSkyPort と同じ。
    // 計測の地形では掃きの本数 × 1 本の時間を記録する(timed のとき)
    std::expected<void, std::string> CompareSkyPort(const GpuContext& context, const sim::MultiresNest& nest,
                                                    const char* name, bool timed) {
        sim::SkyPortParams params{.referenceMk = 293150,
                                  .skyShortMilliwatts = 100000,
                                  .skyLongMilliwatts = sim::SkyBlackLongMilliwatts(250000)};
        const auto plan = sim::PlanSkyPort(params);
        const auto cpu = sim::SweepSkyPort(nest, *context.table, params);
        if (!plan || !cpu)
            return std::unexpected(std::format("{}: 口を作れない", name));

        auto gpuScene = MakeGpuScene(context, nest);
        if (!gpuScene)
            return std::unexpected(std::format("{}: {}", name, gpuScene.error()));

        std::array<std::vector<uint64_t>, 2> sums;
        for (std::vector<uint64_t>& sum : sums)
            sum.assign(sim::SunColumnWordCount(nest), 0);

        double totalMs = 0.0;
        for (const sim::SkyPortSweep& sweep : *plan) {
            const auto sunParams = sim::MakeSunParams(nest, sweep.light, params.rayLevel);
            if (!sunParams)
                return std::unexpected(std::format("{}: {}", name, sunParams.error()));

            double ms = 0.0;
            const auto words = RunOnGpu(context, *gpuScene, *sunParams, timed ? MEASURE_REPEATS : 1,
                                        timed ? &ms : nullptr);
            if (!words)
                return std::unexpected(std::format("{}: {}", name, words.error()));

            std::vector<uint64_t>& sum = sums[sweep.negative ? 1 : 0];
            for (size_t word = 0; word < words->size(); ++word)
                sum[word] += (*words)[word];

            totalMs += ms;
        }

        for (size_t side = 0; side < 2; ++side) {
            if (sums[side] != cpu->words[side]) {
                ReportMismatch(cpu->words[side], sums[side], std::format("{}・空との交換の口・側 {}", name, side));
                return std::unexpected(std::format("{}: 空との交換の口で GPU と CPU が食い違う", name));
            }
        }

        Log(Channel::Gpu, Level::Info, "{}: 空との交換の口(掃き {} 本・正と負)がビット一致", name, plan->size());
        if (timed)
            Log(Channel::Gpu, Level::Info,
                "GPU 時間: {}・空との交換の口(既定の 4 向き × 正と負 = 掃き {} 本・光線の細かさ 0): {:.4f} "
                "ms(掃きごとに "
                "{} 回の平均の和)",
                name, plan->size(), totalMs, MEASURE_REPEATS);

        return {};
    }

    std::expected<void, std::string> Measure(const GpuContext& context) {
        const sim::MultiresNest terrain = test::MakeSunWorld(*context.table, 0, 8, 8, 8, test::Terrain);
        auto gpuScene = MakeGpuScene(context, terrain);
        if (!gpuScene)
            return std::unexpected(gpuScene.error());

        for (const sim::SunLight& light : Suns()) {
            for (const int32_t rayLevel : {0, 1}) {
                const auto params = sim::MakeSunParams(terrain, light, rayLevel);
                if (!params)
                    return std::unexpected(params.error());

                double ms = 0.0;
                const auto gpu = RunOnGpu(context, *gpuScene, *params, MEASURE_REPEATS, &ms);
                if (!gpu)
                    return std::unexpected(gpu.error());

                if (*gpu != sim::SweepSunColumn(terrain, *context.table, *params))
                    return std::unexpected("計測の地形で GPU と CPU が食い違う");

                Log(Channel::Gpu, Level::Info,
                    "GPU 時間: 64³ の地形(0.5 m・根 512)・向き {},{},{}・光線の細かさ {}・光線 {} 本: {:.4f} ms(0 "
                    "にする段を含む・"
                    "{} 回の平均。ビット一致)",
                    light.direction[0], light.direction[1], light.direction[2], rayLevel,
                    params->rayCountB * params->rayCountC, ms, MEASURE_REPEATS);
            }
        }

        return {};
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_sun_column_test [--warp] [--queue direct|compute] [--no-gbv]");
            return 2;
        }

        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_sun_column_test: FAILED ({})", device.error());
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_sun_column_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        const sim::BakedReactionTable table = test::MakeSunTable();
        const GpuContext context{.device = device->Get(), .queue = &*queue, .ring = &*ring, .table = &table};
        const auto compared = CompareScenes(context);
        if (!compared) {
            Log(Channel::Gpu, Level::Error, "gpu_sun_column_test: FAILED ({})", compared.error());
            return 1;
        }

        const sim::MultiresNest portScene = test::MakeSunWorld(table, 0, 2, 1, 2, test::Terrain);
        if (auto port = CompareSkyPort(context, portScene, "地形 16 × 4 × 16 m", false); !port) {
            Log(Channel::Gpu, Level::Error, "gpu_sun_column_test: FAILED ({})", port.error());
            return 1;
        }

        // 計測は WARP では省く(ビット一致は上で見た)
        if (options->adapter != gpu::AdapterKind::Warp) {
            const sim::MultiresNest terrain = test::MakeSunWorld(table, 0, 8, 8, 8, test::Terrain);
            if (auto port = CompareSkyPort(context, terrain, "64³ の地形", true); !port) {
                Log(Channel::Gpu, Level::Error, "gpu_sun_column_test: FAILED ({})", port.error());
                return 1;
            }

            if (auto measured = Measure(context); !measured) {
                Log(Channel::Gpu, Level::Error, "gpu_sun_column_test: FAILED(計測: {})", measured.error());
                return 1;
            }
        }

        if (!test::PassesValidation(*device, "gpu_sun_column_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_sun_column_test: OK({} 組の場面と太陽で GPU と CPU がビット一致)",
            *compared);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
