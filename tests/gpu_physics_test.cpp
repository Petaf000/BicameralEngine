// gpu_physics_test.cpp — 整数の AVBD の 1 刻みを GPU の Compute(sim::GpuPhysics、shaders/sim/physics_step.hlsl)で走らせ、
// CPU リファレンス(sim::PhysicsWorld)とビット一致することを確かめる(T-0090)。
// 区間(--segment 刻み)ごとに GPU の物を全部読み戻し、状態のハッシュと統計(食い込み・接触の数・色の数)を CPU と比べる。
// GPU はもう 1 回走らせ、区間ごとのハッシュが 1 回目と同じこと(2 回の実行で一致)も確かめる。
// 引数: gpu_test_options.h(--warp・--queue)と --scene stack|mass_ratio|pile|wall・--ticks n(既定: release は場面の全部、debug は 120)・--segment n(既定 60)
//   --broadphase compute|graph: 広域の選別 → 接触の幾何を Compute の 2 パスか Work Graph か(既定 graph。T-0092)
//   --solver compute|graph: 色ごとの解き方を Compute の Dispatch か Work Graph か(既定 compute。T-0092)
//   --islands off|compute: 島ごとに 1 グループで解く方式(T-0094。既定 off)。compute なら区間ごとに島の印も CPU と比べる
//   --island-limit n: これより物の多い島は全体の方式(既定 64)・--island-threads n・--island-lanes n: 1 島のグループのスレッド数と
//   1 物を解くスレッド数(既定 256・64。組み合わせは shaders/CMakeLists.txt の SolveIslands)
//   --scene wall: 大きな島(れんが 300 個)
//   --keep-empty-colors: 全体の方式で、使わない色の解も投げる(述語で飛ばさない。T-0094 の比べる用)
//   --profile: CPU とは比べず、2 回目の実行でパスの種類ごとの GPU 時間(タイムスタンプ)を出す(T-0092)
#include "sim/gpu_physics.h"
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "sim/physics_scene.h"
#include "sim/physics_world.h"

using namespace bicameral;
using namespace bicameral::physics;

namespace {

    // debug の CPU リファレンスは release の約 20 倍遅い(physics_test と同じ)ので、debug の ctest は初めの部分だけ
#ifdef NDEBUG
    constexpr uint64_t DEFAULT_TICK_LIMIT = UINT64_MAX;
#else
    constexpr uint64_t DEFAULT_TICK_LIMIT = 120;
#endif

    struct PhysicsTestOptions {
        std::string sceneName = "stack";
        uint64_t tickLimit = DEFAULT_TICK_LIMIT;
        uint64_t segmentTicks = 60;
        bool profile = false;
        bool broadphaseGraph = true;
        sim::GpuPhysicsSolver solver = sim::GpuPhysicsSolver::Compute;
        sim::GpuPhysicsIslands islands = sim::GpuPhysicsIslands::Off;
        uint32_t islandLimit = 64;
        uint32_t islandThreads = 256;
        uint32_t islandLanes = 64;
        bool skipEmptyColors = true;
    };

    struct GpuRun {
        std::vector<uint64_t> hashes;  // 区間の終わりごと
        double gpuMilliseconds = 0;    // 投げてから終わるまで(読み戻しを含む)の合計
    };

    sim::PhysicsScene MakeScene(std::string_view name) {
        if (name == "mass_ratio")
            return sim::MakeMassRatioScene();

        if (name == "pile")
            return sim::MakePileScene(1);

        if (name == "wall")
            return sim::MakeWallScene();

        return sim::MakeStackScene();
    }

    // 最初に食い違った物と欄をログに出す
    void ReportFirstDifference(const std::vector<sim::PhysicsBody>& cpu, const std::vector<sim::PhysicsBody>& gpu) {
        for (size_t i = 0; i < cpu.size() && i < gpu.size(); ++i) {
            const sim::PhysicsBody& a = cpu[i];
            const sim::PhysicsBody& b = gpu[i];
            const bool same = PxHashBody(0, a) == PxHashBody(0, b);
            if (same)
                continue;

            Log(Channel::Physics, Level::Error,
                "  物 {}: 位置 cpu ({}, {}, {}) / gpu ({}, {}, {})、速度 cpu ({}, {}, {}) / gpu ({}, {}, {})、色 cpu "
                "{} / gpu {}",
                i, a.position.x, a.position.y, a.position.z, b.position.x, b.position.y, b.position.z, a.velocity.x,
                a.velocity.y, a.velocity.z, b.velocity.x, b.velocity.y, b.velocity.z, a.color, b.color);
            return;
        }
    }

    // CPU の統計と GPU の統計を比べる(区間の最後の刻み)
    std::string CompareStats(const sim::PhysicsStepStats& cpu, const sim::GpuPhysicsStats& gpu) {
        if (gpu.overflow != 0)
            return std::format("GPU の固定の数が足りない(overflow {:#x})", gpu.overflow);

        if (cpu.maxPenetration != gpu.maxPenetration || cpu.contactCount != gpu.contactCount ||
            cpu.colorCount != gpu.colorCount || cpu.solveFailures != gpu.solveFailures) {
            return std::format("統計が食い違う: 食い込み {}/{}・接触 {}/{}・色 {}/{}・分解の失敗 {}/{}(cpu/gpu)",
                               cpu.maxPenetration, gpu.maxPenetration, cpu.contactCount, gpu.contactCount,
                               cpu.colorCount, gpu.colorCount, cpu.solveFailures, gpu.solveFailures);
        }

        return {};
    }

    // 島の印(最後の小刻み)を CPU と比べる(島の方式のときだけ。T-0094)。report なら島の数と一番大きい島の物の数もログに
    std::string CompareIslandLabels(const std::vector<uint32_t>& cpu, const sim::GpuPhysics& physics, bool report) {
        const std::vector<uint32_t> gpu = physics.ReadIslandLabels();
        if (gpu.empty())
            return {};

        if (gpu != cpu) {
            for (size_t i = 0; i < cpu.size() && i < gpu.size(); ++i) {
                if (cpu[i] != gpu[i])
                    return std::format("島の印が食い違う(物 {}: cpu {} / gpu {})", i, cpu[i], gpu[i]);
            }

            return "島の印の数が食い違う";
        }

        if (!report)
            return {};

        std::vector<uint32_t> sizes(cpu.size(), 0);
        for (uint32_t label : cpu) {
            if (label != UINT32_MAX)
                sizes[label] += 1;
        }

        const auto islandCount = std::ranges::count_if(sizes, [](uint32_t size) { return size > 0; });
        Log(Channel::Physics, Level::Info, "  島 {} 個・一番大きい島 {} 物(CPU と一致)", islandCount,
            std::ranges::max(sizes));

        return {};
    }

    // パスの種類ごとの GPU 時間(1 刻みあたり)
    void ReportProfile(const sim::GpuPhysics& physics, ID3D12CommandQueue* queue, uint64_t tickCount) {
        uint64_t frequency = 0;
        if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0)
            return;

        const auto milliseconds = [&](const sim::GpuPhysicsPassTime& pass) {
            return (double)pass.timestampTicks * 1000.0 / (double)frequency;
        };

        double total = 0;
        for (const sim::GpuPhysicsPassTime& pass : physics.ProfileResult())
            total += milliseconds(pass);

        Log(Channel::Physics, Level::Info, "パスの GPU 時間(1 刻みあたり、合計 {:.3f} ms):", total / (double)tickCount);
        for (const sim::GpuPhysicsPassTime& pass : physics.ProfileResult()) {
            Log(Channel::Physics, Level::Info, "  {:<16} {:7.3f} ms  {:6.1f} 回  {:7.2f} µs/回  {:5.1f}%", pass.name,
                milliseconds(pass) / (double)tickCount, (double)pass.count / (double)tickCount,
                milliseconds(pass) * 1000.0 / (double)pass.count, milliseconds(pass) * 100.0 / total);
        }
    }

    struct GpuParts {
        gpu::ImmediateQueue queue;
        sim::GpuPhysics physics;
        gpu::DebugRing debugRing;
    };

    // パイプラインは 1 回だけ作る(debug のシェーダーはドライバのコンパイルに数十秒かかることがある)
    std::expected<GpuParts, std::string> CreateGpuParts(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType,
                                                        const sim::PhysicsScene& scene,
                                                        const PhysicsTestOptions& testOptions) {
        auto queue = gpu::ImmediateQueue::Create(device, queueType);
        if (!queue)
            return std::unexpected(queue.error());

        const sim::GpuPhysicsOptions physicsOptions{.broadphaseGraph = testOptions.broadphaseGraph,
                                                    .solver = testOptions.solver,
                                                    .skipEmptyColors = testOptions.skipEmptyColors,
                                                    .islands = testOptions.islands,
                                                    .islandBodyLimit = testOptions.islandLimit,
                                                    .islandThreads = testOptions.islandThreads,
                                                    .islandLanes = testOptions.islandLanes};
        auto physics = sim::GpuPhysics::Create(device, scene, PxDefaultParameters(), physicsOptions);
        if (!physics)
            return std::unexpected(physics.error());

        auto debugRing = gpu::DebugRing::Create(device);
        if (!debugRing)
            return std::unexpected(debugRing.error());

        return GpuParts{.queue = std::move(*queue), .physics = std::move(*physics), .debugRing = std::move(*debugRing)};
    }

    // GPU で初めから tickCount 刻み走らせる。world を渡したら、区間ごとに CPU も進めて比べる
    std::expected<GpuRun, std::string> RunGpu(GpuParts& parts, const PhysicsTestOptions& options, uint64_t tickCount,
                                              sim::PhysicsWorld* world) {
        gpu::ImmediateQueue& queue = parts.queue;
        sim::GpuPhysics& physics = parts.physics;
        gpu::DebugRing& debugRing = parts.debugRing;
        GpuRun run;
        bool initialize = true;
        while (initialize || physics.Tick() < tickCount) {
            ID3D12GraphicsCommandList10* list = queue.Begin();
            if (list == nullptr)
                return std::unexpected("コマンドリストを始められない");

            debugRing.RecordBegin(list);
            if (initialize)
                physics.RecordInitialize(list, debugRing.GpuAddress());

            initialize = false;

            const uint64_t end = std::min(tickCount, physics.Tick() + options.segmentTicks);
            while (physics.Tick() < end)
                physics.RecordStep(list, debugRing.GpuAddress());

            physics.RecordReadback(list);
            debugRing.RecordReadbackAndReset(list);

            const auto started = chr::steady_clock::now();
            if (!queue.ExecuteAndWait())
                return std::unexpected("GPU での実行に失敗(デバイスが失われた)");

            run.gpuMilliseconds += chr::duration<double, std::milli>(chr::steady_clock::now() - started).count();
            physics.AccumulateProfile();
            const gpu::DebugRingContents debugOutput = debugRing.Drain();
            if (debugOutput.assertCount > 0) {
                return std::unexpected(
                    std::format("GPU の FX_ASSERT が {} 件(刻み {} まで)", debugOutput.assertCount, end));
            }

            const std::vector<sim::PhysicsBody> bodies = physics.ReadBodies();
            const sim::GpuPhysicsStats stats = physics.ReadStats();
            run.hashes.push_back(sim::GpuPhysics::StateHash(end, bodies));
            if (world == nullptr)
                continue;

            while (world->Tick() < end)
                world->Step();

            if (world->StateHash() != run.hashes.back()) {
                ReportFirstDifference(world->Bodies(), bodies);
                return std::unexpected(std::format("刻み {} の状態が CPU と食い違う(cpu {:016x} / gpu {:016x})", end,
                                                   world->StateHash(), run.hashes.back()));
            }

            const std::string statsError = CompareStats(world->Stats(), stats);
            if (!statsError.empty())
                return std::unexpected(std::format("刻み {}: {}", end, statsError));

            const std::string islandError = CompareIslandLabels(world->IslandLabels(), physics, end == tickCount);
            if (!islandError.empty())
                return std::unexpected(std::format("刻み {}: {}", end, islandError));
        }

        return run;
    }

    sim::GpuPhysicsSolver ParseSolver(std::string_view name) {
        return name == "graph" ? sim::GpuPhysicsSolver::Graph : sim::GpuPhysicsSolver::Compute;
    }

    std::optional<PhysicsTestOptions> TakePhysicsOptions(std::vector<char*>& arguments) {
        PhysicsTestOptions options;
        for (size_t index = 1; index < arguments.size();) {
            const std::string_view name = arguments[index];
            if (name == "--profile" || name == "--keep-empty-colors") {
                options.profile = options.profile || name == "--profile";
                options.skipEmptyColors = options.skipEmptyColors && name != "--keep-empty-colors";
                arguments.erase(arguments.begin() + (std::ptrdiff_t)index);
                continue;
            }

            if (index + 1 >= arguments.size())
                break;

            const std::string_view value = arguments[index + 1];
            if (name == "--scene")
                options.sceneName = value;
            else if (name == "--ticks")
                options.tickLimit = std::stoull(std::string(value));
            else if (name == "--broadphase")
                options.broadphaseGraph = value == "graph";
            else if (name == "--solver")
                options.solver = ParseSolver(value);
            else if (name == "--islands")
                options.islands = value == "compute" ? sim::GpuPhysicsIslands::Compute : sim::GpuPhysicsIslands::Off;
            else if (name == "--island-limit")
                options.islandLimit = (uint32_t)std::stoul(std::string(value));
            else if (name == "--island-threads")
                options.islandThreads = (uint32_t)std::stoul(std::string(value));
            else if (name == "--island-lanes")
                options.islandLanes = (uint32_t)std::stoul(std::string(value));
            else if (name == "--segment")
                options.segmentTicks = std::max<uint64_t>(1, std::stoull(std::string(value)));
            else {
                ++index;
                continue;
            }

            arguments.erase(arguments.begin() + (std::ptrdiff_t)index, arguments.begin() + (std::ptrdiff_t)index + 2);
        }

        const std::array<std::string_view, 4> scenes = {"stack", "mass_ratio", "pile", "wall"};
        if (std::ranges::find(scenes, options.sceneName) == scenes.end())
            return std::nullopt;

        return options;
    }

    // 2 回目: GPU だけ。区間ごとのハッシュが 1 回目と同じこと(--profile ならパスの時間も取る)
    std::expected<GpuRun, std::string> RunSecond(GpuParts& parts, ID3D12Device5* device,
                                                 const PhysicsTestOptions& options, uint64_t tickCount,
                                                 const GpuRun& first) {
        if (options.profile && !parts.physics.EnableProfiling(device))
            return std::unexpected("タイムスタンプのヒープを作れない");

        auto second = RunGpu(parts, options, tickCount, nullptr);
        if (!second)
            return std::unexpected(std::format("2 回目の GPU の実行: {}", second.error()));

        if (second->hashes != first.hashes)
            return std::unexpected("2 回目の GPU の実行が 1 回目と食い違う");

        return second;
    }

    int Run(std::vector<char*> arguments) {
        const auto physicsOptions = TakePhysicsOptions(arguments);
        const auto options = test::ParseGpuTestOptions(std::span(arguments));
        if (!physicsOptions || !options) {
            Log(Channel::Physics, Level::Error,
                "使い方: gpu_physics_test [--warp] [--queue direct|compute] [--scene stack|mass_ratio|pile|wall] "
                "[--ticks "
                "n] "
                "[--segment n] [--broadphase compute|graph] [--solver compute|graph] [--islands off|compute] "
                "[--island-limit n] [--island-threads n] [--island-lanes n] [--keep-empty-colors] [--profile]");
            return 2;
        }

        const sim::PhysicsScene scene = MakeScene(physicsOptions->sceneName);
        const uint64_t tickCount = std::min(physicsOptions->tickLimit, scene.tickCount);
        Log(Channel::Physics, Level::Info,
            "gpu_physics_test: {} を {} 刻み(区間 {})、adapter {}, queue {}, 広域の選別 {}, 島 {}", scene.name,
            tickCount, physicsOptions->segmentTicks, gpu::AdapterKindName(options->adapter),
            test::QueueTypeName(options->queueType), physicsOptions->broadphaseGraph ? "Work Graph" : "Compute",
            physicsOptions->islands == sim::GpuPhysicsIslands::Off
                ? std::string("なし")
                : std::format("上限 {} 物・{} スレッド・1 物 {} スレッド", physicsOptions->islandLimit,
                              physicsOptions->islandThreads, physicsOptions->islandLanes));

        // GPU-based validation は切る: 物理のシェーダー(-Od の debug)の計装で、パイプラインを作るのが数分を超えて終わらなかった
        // (gpu_reaction_test の WARP と同じ症状)。debug layer は残す
        gpu::DeviceOptions deviceOptions = test::TestDeviceOptions(*options);
        deviceOptions.gpuBasedValidation = false;
        const auto device = gpu::Device::Create(options->adapter, deviceOptions);
        if (!device) {
            Log(Channel::Physics, Level::Error, "gpu_physics_test: FAILED(デバイスを作れない)");
            return 1;
        }

        // --- 1 回目: CPU と比べる ---
        sim::PhysicsWorld world(scene, PxDefaultParameters());
        const auto cpuStarted = chr::steady_clock::now();
        auto parts = CreateGpuParts(device->Get(), options->queueType, scene, *physicsOptions);
        if (!parts) {
            Log(Channel::Physics, Level::Error, "gpu_physics_test: FAILED({})", parts.error());
            return 1;
        }

        const auto first = RunGpu(*parts, *physicsOptions, tickCount, physicsOptions->profile ? nullptr : &world);
        if (!first) {
            Log(Channel::Physics, Level::Error, "gpu_physics_test: FAILED({})", first.error());
            return 1;
        }

        const double totalSeconds = chr::duration<double>(chr::steady_clock::now() - cpuStarted).count();

        const auto second = RunSecond(*parts, device->Get(), *physicsOptions, tickCount, *first);
        if (!second) {
            Log(Channel::Physics, Level::Error, "gpu_physics_test: FAILED({})", second.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_physics_test"))
            return 1;

        if (physicsOptions->profile)
            ReportProfile(parts->physics, parts->queue.Native(), tickCount);

        Log(Channel::Physics, Level::Info,
            "gpu_physics_test: OK({} を {} 刻みで {}・2 回の実行で一致。最後のハッシュ {:016x}。"
            "GPU 1 刻み {:.2f} ms(2 回目、読み戻しを含む)/ 1 回目の合計 {:.1f} s)",
            scene.name, tickCount, physicsOptions->profile ? "GPU だけ(CPU と比べない)" : "GPU と CPU がビット一致",
            first->hashes.back(), second->gpuMilliseconds / (double)tickCount, totalSeconds);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::vector<char*>(argv, argv + argc));
    SingletonFinalizer::Finalize();

    return exitCode;
}
