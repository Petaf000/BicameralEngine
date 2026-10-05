// gpu_multires_conduction_test.cpp — 多重解像度の木の上の熱の伝導(sim::GpuMultires の RecordStep・RecordStepActive に
// MultiresStepOptions{.conduction = true}。Compute の段 shaders/sim/multires_conduct.hlsl)を GPU で走らせ、CPU リファレンス
// (sim::StepNest・StepActive)と毎刻みビット一致することを確かめる(T-0107)。
// 場面: 鎖(tests/multires_conduction_scene.h。熱が 6 段をまたいで根へ流れる)を全部刻む・活性だけ刻む、
// たくさんの要求の場面(tests/multires_activity_scene.h。深さ 26 段・木箱が燃える・影・端数の枠が足りなくなる)を活性だけ・全部。
// 状態の全部(見出し・セル・端数・空きのスタック・帳簿・数える欄)と次の刻みの種(活性)を毎刻み比べる。活性の場面は、伝導の段を
// Compute で投げる版と Work Graph で投げる版(GpuMultiresOptions::conductionGraph)の両方で走らせ、結果が同じことも確かめる。
// 活性のグラフは debug の GPU-based validation で作るのに数分かかるので、全部の場面で同じ GpuMultires を使い回す(大きさを揃える)。
// 計測(release のハードウェアだけ。D-302): 根 8³ の世界で、活性の刻み(伝導の段が Compute / Work Graph)と全部を刻む刻みの GPU 時間。
// 引数は gpu_test_options.h。
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "multires_activity_scene.h"
#include "multires_conduction_scene.h"
#include "sim/gpu_multires.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace {

    constexpr sim::MultiresStepOptions CONDUCTION = {.conduction = true};
    constexpr uint64_t CHAIN_TICKS = 80;
    constexpr uint64_t STRESS_ACTIVE_TICKS = 30;
    constexpr uint64_t STRESS_FULL_TICKS = 12;
    constexpr uint32_t WARMUP_STEPS = 400;     // 計測の前に刻みを何回投げるか(GPU のクロックを上げる)
    constexpr uint32_t MEASURE_ROOT_EDGE = 8;  // 計測の世界は根 8³ = 512 個(32m 角)
    constexpr uint32_t MEASURE_TICKS = 8;      // 最初の刻み(全部の根が種)の後に測る刻みの数

    // 場面 1 つの進め方
    struct SceneRun {
        const char* name = "";
        bool stress = false;  // たくさんの要求の場面(要求・影)か、鎖か
        bool active = false;  // 活性だけ刻むか、全部か
        bool graph = false;   // 活性の伝導の段を Work Graph で投げるか
        uint64_t ticks = 0;
    };

    // 1 刻みの GPU の記録(CPU の StepTick と同じ順)
    bool RecordTick(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                    const SceneRun& run, uint64_t tick, std::span<const MrRequest> requests) {
        if (!run.stress) {
            if (run.active)
                return gpu.RecordStepActive(list, ring, test::CONDUCTION_SEED, tick, CONDUCTION);

            gpu.RecordStep(list, ring, test::CONDUCTION_SEED, tick, CONDUCTION);
            return true;
        }

        if (!gpu.RecordRequests(list, requests))
            return false;

        const uint32_t shadowSlot = test::ACTIVITY_WORLD_BLOCKS;
        gpu.RecordProcessRequests(list, ring);
        if (tick == test::ACTIVITY_SHADOW_TICK) {
            gpu.RecordRefineShadow(list, ring, test::ACTIVITY_SHADOW_PARENT, shadowSlot, test::ACTIVITY_SHADOW_LEVELS,
                                   test::ActivityShadowPoint());
        } else if (tick == test::ACTIVITY_SHADOW_END_TICK) {
            gpu.RecordRemoveShadow(list, ring, shadowSlot, test::ACTIVITY_SHADOW_LEVELS);
        }

        if (run.active) {
            if (!gpu.RecordStepActive(list, ring, test::STRESS_SEED, tick, CONDUCTION))
                return false;
        } else {
            gpu.RecordStep(list, ring, test::STRESS_SEED, tick, CONDUCTION);
        }

        if (test::ActivityShadowExists(tick))
            gpu.RecordPullBack(list, ring, shadowSlot, test::ACTIVITY_SHADOW_LEVELS);

        return true;
    }

    // 1 刻みの CPU(RecordTick と同じ順)
    void StepTick(sim::MultiresNest& nest, const sim::BakedReactionTable& table, const SceneRun& run, uint64_t tick,
                  std::span<const MrRequest> requests) {
        if (run.stress) {
            test::BeginActivityTick(nest, tick, requests);
            test::EndActivityTick(nest, table, tick, run.active, CONDUCTION);
            return;
        }

        if (run.active)
            sim::StepActive(nest, table, test::CONDUCTION_SEED, tick, CONDUCTION);
        else
            sim::StepNest(nest, table, test::CONDUCTION_SEED, tick, CONDUCTION);
    }

    // 最初に食い違った所をログに出す
    void ReportFirstMismatch(const sim::MultiresNest& cpu, const sim::MultiresNest& gpu, uint64_t tick) {
        for (size_t slot = 0; slot < cpu.blocks.size(); ++slot) {
            const MrBlock& a = cpu.blocks[slot];
            const MrBlock& b = gpu.blocks[slot];
            if (std::memcmp(&a, &b, sizeof(MrBlock)) != 0) {
                Log(Channel::Sim, Level::Error,
                    "刻み {}: 枠 {} の見出しが違う(頁 cpu {} / gpu {}・端数 cpu {} / gpu {}・印 cpu {} / gpu "
                    "{}・忙しさ cpu {} / "
                    "gpu {})",
                    tick, slot, a.page, b.page, a.fraction, b.fraction, a.activeTick, b.activeTick, a.busyTick,
                    b.busyTick);
            }
        }

        for (size_t i = 0; i < cpu.cells.size(); ++i) {
            if (sim::HashReactionCell(cpu.cells[i]) == sim::HashReactionCell(gpu.cells[i]))
                continue;

            Log(Channel::Sim, Level::Error, "刻み {}: セル {} が違う(エネルギー cpu {} / gpu {})", tick, i,
                cpu.cells[i].energy, gpu.cells[i].energy);
            break;
        }

        for (size_t i = 0; i < cpu.fractions.size(); ++i) {
            if (cpu.fractions[i].energy == gpu.fractions[i].energy)
                continue;

            Log(Channel::Sim, Level::Error, "刻み {}: 端数 {} のエネルギーが違う(cpu {} / gpu {})", tick, i,
                cpu.fractions[i].energy, gpu.fractions[i].energy);
            break;
        }

        for (size_t i = 0; i < cpu.counters.size(); ++i) {
            if (cpu.counters[i] != gpu.counters[i])
                Log(Channel::Sim, Level::Error, "刻み {}: 数える欄 {} が違う(cpu {} / gpu {})", tick, i,
                    cpu.counters[i], gpu.counters[i]);
        }
    }

    // 1 本のリストを記録して投げ、読み戻す
    template <typename Record>
    std::expected<void, std::string> Execute(gpu::ImmediateQueue& queue, gpu::DebugRing& ring, sim::GpuMultires& gpu,
                                             sim::MultiresNest& read, const Record& record) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        ring.RecordBegin(list);
        if (!record(list))
            return std::unexpected("記録できない");

        gpu.RecordReadback(list);
        ring.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = ring.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        if (!gpu.Read(read))
            return std::unexpected("読み戻せない");

        return {};
    }

    // 状態の全部と、活性なら次の刻みの種
    std::expected<void, std::string> Compare(const sim::MultiresNest& cpu, const sim::MultiresNest& read,
                                             const sim::GpuMultires& gpu, const SceneRun& run, uint64_t tick) {
        if (sim::HashWholeNest(cpu) != sim::HashWholeNest(read)) {
            ReportFirstMismatch(cpu, read, tick);
            return std::unexpected("CPU と GPU が食い違う");
        }

        if (!run.active)
            return {};

        const auto seeds = gpu.ReadSeeds();
        if (!seeds || seeds->dropped != 0)
            return std::unexpected("種の一覧を読めない・落ちた");

        if (seeds->slots != sim::SeedSlots(cpu))
            return std::unexpected(std::format("次の刻みの種が CPU と違う(cpu {} 個 / gpu {} 個)",
                                               sim::SeedSlots(cpu).size(), seeds->slots.size()));

        return {};
    }

    sim::MultiresNest MakeSceneNest(const sim::BakedReactionTable& table, const SceneRun& run) {
        if (run.stress)
            return test::MakeActivityNest(table);

        // 鎖も、たくさんの要求の場面と同じ大きさの木に置く(GpuMultires を使い回すため)
        return test::MakeChainNest(table, test::MakeActivityNest(table).capacity);
    }

    std::expected<uint64_t, std::string> RunScene(gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                  sim::GpuMultires& gpu, const sim::BakedReactionTable& table,
                                                  const SceneRun& run) {
        sim::MultiresNest cpu = MakeSceneNest(table, run);
        sim::MultiresNest read;
        if (!gpu.UseConductionGraph(run.graph))
            return std::unexpected("伝導の段の Work Graph が無い");

        for (uint64_t tick = 0; tick < run.ticks; ++tick) {
            const std::vector<MrRequest> requests = run.stress ? test::MakeStressRequests(cpu, tick)
                                                               : std::vector<MrRequest>{};
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu.RecordUpload(list, cpu))
                    return false;

                return RecordTick(list, gpu, ring.GpuAddress(), run, tick, requests);
            };
            if (auto executed = Execute(queue, ring, gpu, read, record); !executed)
                return std::unexpected(std::format("{} 刻み {}: {}", run.name, tick, executed.error()));

            StepTick(cpu, table, run, tick, requests);
            if (auto compared = Compare(cpu, read, gpu, run, tick); !compared)
                return std::unexpected(std::format("{} 刻み {}: {}", run.name, tick, compared.error()));
        }

        Log(Channel::Gpu, Level::Info,
            "{}: {} 刻み一致(刻んだブロック {}・頁に広げた {}・端数の枠の残り {}・端数の不足 {})", run.name, run.ticks,
            cpu.counters[MR_COUNTER_SCHEDULED], cpu.counters[MR_COUNTER_EXPANDED],
            cpu.counters[MR_COUNTER_FREE_FRACTIONS], cpu.counters[MR_COUNTER_FRACTION_SHORTAGE]);

        return sim::HashWholeNest(cpu);
    }

    // withGraph = 伝導の段の Work Graph 版も走らせる(release の WARP では最初の DispatchGraph でデバイスが失われるので外す。
    // debug の WARP とハードウェアでは一致する。WARP の JIT の不具合と推定・未確認。ADR-0013 と同じ種類)
    std::expected<void, std::string> RunAll(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                            const sim::BakedReactionTable& table, bool withGraph) {
        const sim::MultiresCapacity capacity = test::MakeActivityNest(table).capacity;
        auto gpu = sim::GpuMultires::Create(device, table, capacity, {.activity = true, .conductionGraph = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        // 活性の場面は伝導の段が Compute の版(偶数番)と Work Graph の版(奇数番)を並べる
        const std::array<SceneRun, 6> runs = {
            SceneRun{.name = "鎖(活性・Compute)", .active = true, .ticks = CHAIN_TICKS},
            SceneRun{.name = "鎖(活性・Work Graph)", .active = true, .graph = true, .ticks = CHAIN_TICKS},
            SceneRun{
                .name = "たくさんの要求(活性・Compute)", .stress = true, .active = true, .ticks = STRESS_ACTIVE_TICKS},
            SceneRun{.name = "たくさんの要求(活性・Work Graph)",
                     .stress = true,
                     .active = true,
                     .graph = true,
                     .ticks = STRESS_ACTIVE_TICKS},
            SceneRun{.name = "鎖(全部)", .ticks = CHAIN_TICKS},
            SceneRun{.name = "たくさんの要求(全部)", .stress = true, .ticks = STRESS_FULL_TICKS}};
        std::array<uint64_t, runs.size()> hashes{};
        for (size_t i = 0; i < runs.size(); ++i) {
            if (runs[i].graph && !withGraph) {
                Log(Channel::Gpu, Level::Warning, "{}: 走らせない(release の WARP)", runs[i].name);
                hashes[i] = hashes[i - 1];
                continue;
            }

            auto hash = RunScene(queue, ring, *gpu, table, runs[i]);
            if (!hash)
                return std::unexpected(hash.error());

            hashes[i] = *hash;
        }

        if (hashes[0] != hashes[1] || hashes[2] != hashes[3])
            return std::unexpected("伝導の段の Compute 版と Work Graph 版が食い違う");

        return {};
    }

    // --- 計測(D-302)---

    struct ConductionTiming {
        std::array<double, 3> firstMs{};  // 最初の刻み(全部の根が種): 活性・Compute / 活性・Work Graph / 全部
        std::array<double, 3> laterMs{};  // その後の MEASURE_TICKS 刻みの平均
        uint32_t blocks = 0;              // 本物のブロックの数
    };

    double ToMilliseconds(const std::vector<uint64_t>& stamps, uint32_t from, uint32_t to, uint64_t frequency) {
        if (stamps.size() <= to || frequency == 0)
            return 0;

        return static_cast<double>(stamps[to] - stamps[from]) * 1000.0 / static_cast<double>(frequency);
    }

    // 計測の版 variant(0 活性・Compute / 1 活性・Work Graph / 2 全部)の 1 本のリスト(タイムスタンプ 0〜2)
    bool RecordMeasureVariant(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                              const sim::MultiresNest& initial, uint32_t variant) {
        constexpr uint64_t FIRST_TICK = 10000;
        bool recorded = gpu.RecordUpload(list, initial) && gpu.UseConductionGraph(variant == 1);
        for (uint32_t i = 0; i < WARMUP_STEPS; ++i)
            gpu.RecordStep(list, ring, test::STRESS_SEED, i);

        for (uint32_t i = 0; i <= MEASURE_TICKS; ++i) {
            if (i <= 1)
                gpu.RecordTimestamp(list, i);

            const uint64_t tick = FIRST_TICK + i;
            if (variant == 2)
                gpu.RecordStep(list, ring, test::STRESS_SEED, tick, CONDUCTION);
            else
                recorded = recorded && gpu.RecordStepActive(list, ring, test::STRESS_SEED, tick, CONDUCTION);
        }

        gpu.RecordTimestamp(list, 2);

        return recorded;
    }

    // 版(0 活性・Compute / 1 活性・Work Graph / 2 全部)ごとに 1 本のリストで: 写す → 全部を刻む暖機(伝導なし)→ 印 → 最初の刻み → 印
    // → MEASURE_TICKS 刻み → 印。版の順の影響(最初に使うパイプラインの準備)を除くため、版を 2 周して 2 周目を採る
    std::expected<ConductionTiming, std::string> Measure(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                         gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        const sim::MultiresNest initial = test::MakeActivityNest(table, MEASURE_ROOT_EDGE);
        auto gpu = sim::GpuMultires::Create(device, table, initial.capacity,
                                            {.activity = true, .conductionGraph = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        uint64_t frequency = 0;
        if (FAILED(queue.Native()->GetTimestampFrequency(&frequency)))
            return std::unexpected("タイムスタンプの周波数を読めない");

        constexpr uint32_t VARIANTS = 3;
        ConductionTiming timing;
        sim::MultiresNest read;
        for (uint32_t run = 0; run < 2 * VARIANTS; ++run) {
            const uint32_t variant = run % VARIANTS;
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                return RecordMeasureVariant(list, *gpu, ring.GpuAddress(), initial, variant);
            };
            if (auto executed = Execute(queue, ring, *gpu, read, record); !executed)
                return std::unexpected(executed.error());

            const std::vector<uint64_t> stamps = gpu->ReadTimestamps(3);
            timing.firstMs[variant] = ToMilliseconds(stamps, 0, 1, frequency);
            timing.laterMs[variant] = ToMilliseconds(stamps, 1, 2, frequency) / MEASURE_TICKS;
        }

        for (uint32_t slot = 0; slot < read.capacity.worldBlocks; ++slot)
            timing.blocks += read.blocks[slot].kind == MR_BLOCK_REAL ? 1 : 0;

        return timing;
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_multires_conduction_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_multires_conduction_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_conduction_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_conduction_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        // 計測は CPU の重い比べる実行の前に(GPU が長く空くとクロックが下がる)。release のハードウェアだけ(debug は検証の計装で意味がない)
#ifdef NDEBUG
        constexpr bool RELEASE = true;
#else
        constexpr bool RELEASE = false;
#endif
        const bool warp = options->adapter == gpu::AdapterKind::Warp;
        const bool measure = RELEASE && !warp;
        const auto timing = measure ? Measure(device->Get(), *queue, *ring, *table)
                                    : std::expected<ConductionTiming, std::string>(ConductionTiming{});
        if (!timing) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_conduction_test: FAILED(計測: {})", timing.error());
            return 1;
        }

        if (measure) {
            Log(Channel::Gpu, Level::Info,
                "GPU 時間(伝導あり・ブロック {} 個): 最初の刻み(全部が種)活性 Compute {:.3f}・活性 Work Graph "
                "{:.3f}・全部 {:.3f} ms / "
                "その後の平均 活性 Compute {:.3f}・活性 Work Graph {:.3f}・全部 {:.3f} ms",
                timing->blocks, timing->firstMs[0], timing->firstMs[1], timing->firstMs[2], timing->laterMs[0],
                timing->laterMs[1], timing->laterMs[2]);
        }

        if (auto result = RunAll(device->Get(), *queue, *ring, *table, !(RELEASE && warp)); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_conduction_test: FAILED ({})", result.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_multires_conduction_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_multires_conduction_test: OK(熱の伝導の GPU と CPU が毎刻みビット一致)");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
