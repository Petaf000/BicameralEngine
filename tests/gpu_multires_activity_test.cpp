// gpu_multires_activity_test.cpp — 多重解像度の木の上の活性(sim::GpuMultires::RecordStepActive。Work Graph の
// ActivitySeedNode → WakeFaceNode → ActivityStepNode)を GPU で走らせ、CPU リファレンス(sim::StepActive)と毎刻みビット一致することを
// 確かめる(T-0100)。場面は tests/multires_activity_scene.h。状態の全部(見出しの活性の印・セル・端数・空きのスタック・帳簿・数える欄)と
// 次の刻みの種の集合を毎刻み比べ、2 回走らせて一致を確かめる。
// 時間は別に、暖機してから「全部が種(根 64 個)」「静か」「全部を刻む Compute(RecordStep)」を測る。
// 引数は gpu_test_options.h。
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "multires_activity_scene.h"
#include "sim/gpu_multires.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace {

    // 比べる反応の丸め(T-0121)。既定の待ちの丸め(活性のグラフも。T-0124)。今までの丸めは T-0122 で消す
    sim::MultiresStepOptions& Rounding() {
        static sim::MultiresStepOptions rounding;
        return rounding;
    }

    constexpr uint32_t WARMUP_STEPS = 400;     // 計測の前に刻みを何回投げるか(GPU のクロックを上げる)
    constexpr uint32_t MEASURE_ROOT_EDGE = 8;  // 計測の世界は根 8³ = 512 個(32m 角)

    struct ActivityTiming {
        double allSeedsMs = 0;  // 全部の根が種の刻み(活性だけ刻む)
        double quietMs = 0;     // その次の刻み(進める反応のあるブロックとその隣だけ)
        double fullMs = 0;      // 全部を刻む Compute
        uint32_t allSeedsScheduled = 0;
        uint32_t quietScheduled = 0;
        uint32_t blocks = 0;  // 本物のブロックの数
    };

    // 1 刻みの GPU の記録(test::BeginActivityTick・EndActivityTick と同じ順)
    bool RecordTick(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                    uint32_t shadowSlot, uint64_t tick, std::span<const MrRequest> requests) {
        if (!gpu.RecordRequests(list, requests))
            return false;

        gpu.RecordProcessRequests(list, ring);
        if (tick == test::ACTIVITY_SHADOW_TICK) {
            gpu.RecordRefineShadow(list, ring, test::ACTIVITY_SHADOW_PARENT, shadowSlot, test::ACTIVITY_SHADOW_LEVELS,
                                   test::ActivityShadowPoint());
        } else if (tick == test::ACTIVITY_SHADOW_END_TICK) {
            gpu.RecordRemoveShadow(list, ring, shadowSlot, test::ACTIVITY_SHADOW_LEVELS);
        }

        if (!gpu.RecordStepActive(list, ring, test::STRESS_SEED, tick, Rounding()))
            return false;

        if (test::ActivityShadowExists(tick))
            gpu.RecordPullBack(list, ring, shadowSlot, test::ACTIVITY_SHADOW_LEVELS);

        return true;
    }

    // 最初に食い違った所をログに出す
    void ReportFirstMismatch(const sim::MultiresNest& cpu, const sim::MultiresNest& gpu, uint64_t tick) {
        for (size_t slot = 0; slot < cpu.blocks.size(); ++slot) {
            const MrBlock& a = cpu.blocks[slot];
            const MrBlock& b = gpu.blocks[slot];
            if (std::memcmp(&a, &b, sizeof(MrBlock)) != 0) {
                Log(Channel::Sim, Level::Error,
                    "刻み {}: 枠 {} の見出しが違う(種類 cpu {} / gpu {}・印 cpu {} / gpu {}・busyTick {} / "
                    "{}・wakeTick {} / {})",
                    tick, slot, a.kind, b.kind, a.activeTick, b.activeTick, a.busyTick, b.busyTick, a.wakeTick,
                    b.wakeTick);
            }
        }

        for (size_t i = 0; i < cpu.cells.size(); ++i) {
            if (sim::HashReactionCell(cpu.cells[i]) == sim::HashReactionCell(gpu.cells[i]))
                continue;

            Log(Channel::Sim, Level::Error, "刻み {}: 枠 {} セル {} が違う", tick, i / MR_BLOCK_CELLS,
                i % MR_BLOCK_CELLS);
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

    std::expected<uint64_t, std::string> RunActivity(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                     gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = test::MakeActivityNest(table);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, {.activity = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        const uint32_t shadowSlot = cpu.capacity.worldBlocks;
        sim::MultiresNest read;
        for (uint64_t tick = 0; tick < test::ACTIVITY_TICKS; ++tick) {
            const std::vector<MrRequest> requests = test::MakeStressRequests(cpu, tick);
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordTick(list, *gpu, ring.GpuAddress(), shadowSlot, tick, requests);
            };
            if (auto executed = Execute(queue, ring, *gpu, read, record); !executed)
                return std::unexpected(std::format("刻み {}: {}", tick, executed.error()));

            test::BeginActivityTick(cpu, tick, requests);
            test::EndActivityTick(cpu, table, tick, true, Rounding());

            // --- 状態の全部と次の刻みの種 ---
            if (sim::HashWholeNest(cpu) != sim::HashWholeNest(read)) {
                ReportFirstMismatch(cpu, read, tick);
                return std::unexpected(std::format("刻み {} で CPU と GPU が食い違う", tick));
            }

            const auto seeds = gpu->ReadSeeds();
            if (!seeds || seeds->dropped != 0)
                return std::unexpected(std::format("刻み {}: 種の一覧を読めない・落ちた", tick));

            if (seeds->slots != sim::SeedSlots(cpu))
                return std::unexpected(std::format("刻み {}: 次の刻みの種が CPU と違う(cpu {} 個 / gpu {} 個)", tick,
                                                   sim::SeedSlots(cpu).size(), seeds->slots.size()));
        }

        if (cpu.counters[MR_COUNTER_WAKE_TOO_DEEP] != 0)
            return std::unexpected("面をたどる再帰の上限に当たった");

        Log(Channel::Gpu, Level::Info, "活性 {} 刻み: 刻んだブロック {}", test::ACTIVITY_TICKS,
            cpu.counters[MR_COUNTER_SCHEDULED]);

        return sim::HashWholeNest(cpu);
    }

    double ToMilliseconds(const std::vector<uint64_t>& stamps, uint32_t from, uint32_t to, uint64_t frequency) {
        if (stamps.size() <= to || frequency == 0)
            return 0;

        return static_cast<double>(stamps[to] - stamps[from]) * 1000.0 / static_cast<double>(frequency);
    }

    // 1 本のリストで: 全部を刻む暖機(種はそのまま)→ 0 → 活性(全部の根が種)→ 1 → 活性(静か)× 8 → 2 → 全部を刻む × 8 → 3。
    // rounding で反応の丸めを選ぶ(待ちの丸めの眠っている所の費用を今までの丸めと比べる。T-0121)
    std::expected<ActivityTiming, std::string> Measure(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                       gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                       const sim::MultiresStepOptions& rounding) {
        const sim::MultiresNest initial = test::MakeActivityNest(table, MEASURE_ROOT_EDGE);
        auto gpu = sim::GpuMultires::Create(device, table, initial.capacity, {.activity = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        // 待ちの丸めの全部を刻むは重い(セルごとに log2 と 128bit の割り算)ので、1 本のリストの暖機を減らす(400 回で TDR になった。T-0121)
        const uint32_t warmupSteps = rounding.cutoffRounding ? WARMUP_STEPS : WARMUP_STEPS / 10;
        constexpr uint64_t FIRST_TICK = 10000;
        constexpr uint32_t REPEATS = 8;
        sim::MultiresNest read;
        bool uploaded = true;
        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            uploaded = gpu->RecordUpload(list, initial);
            for (uint32_t i = 0; i < warmupSteps; ++i)
                gpu->RecordStep(list, ring.GpuAddress(), test::STRESS_SEED, i, rounding);

            gpu->RecordTimestamp(list, 0);
            bool recorded = gpu->RecordStepActive(list, ring.GpuAddress(), test::STRESS_SEED, FIRST_TICK, rounding);
            gpu->RecordTimestamp(list, 1);
            for (uint32_t i = 1; i <= REPEATS; ++i)
                recorded &= gpu->RecordStepActive(list, ring.GpuAddress(), test::STRESS_SEED, FIRST_TICK + i, rounding);

            gpu->RecordTimestamp(list, 2);
            for (uint32_t i = 1; i <= REPEATS; ++i)
                gpu->RecordStep(list, ring.GpuAddress(), test::STRESS_SEED, FIRST_TICK + REPEATS + i, rounding);

            gpu->RecordTimestamp(list, 3);

            return uploaded && recorded;
        };
        if (auto executed = Execute(queue, ring, *gpu, read, record); !executed)
            return std::unexpected(executed.error());

        uint64_t frequency = 0;
        if (FAILED(queue.Native()->GetTimestampFrequency(&frequency)))
            return std::unexpected("タイムスタンプの周波数を読めない");

        uint32_t blocks = 0;
        for (uint32_t slot = 0; slot < read.capacity.worldBlocks; ++slot)
            blocks += read.blocks[slot].kind == MR_BLOCK_REAL ? 1 : 0;

        // 最初の刻みは全部の根(種)を刻む。残りは静かな刻みの平均
        const uint32_t quietTotal = read.counters[MR_COUNTER_SCHEDULED] - blocks;
        const std::vector<uint64_t> stamps = gpu->ReadTimestamps(4);
        return ActivityTiming{.allSeedsMs = ToMilliseconds(stamps, 0, 1, frequency),
                              .quietMs = ToMilliseconds(stamps, 1, 2, frequency) / REPEATS,
                              .fullMs = ToMilliseconds(stamps, 2, 3, frequency) / REPEATS,
                              .allSeedsScheduled = blocks,
                              .quietScheduled = quietTotal / REPEATS,
                              .blocks = blocks};
    }

    void LogTiming(std::string_view rounding, const ActivityTiming& timing) {
        Log(Channel::Gpu, Level::Info,
            "GPU 時間({}・ブロック {} 個): 活性(全部が種・{} ブロック){:.3f} ms・"
            "活性(静か・平均 {} ブロック){:.3f} ms・全部を刻む {:.3f} ms",
            rounding, timing.blocks, timing.allSeedsScheduled, timing.allSeedsMs, timing.quietScheduled, timing.quietMs,
            timing.fullMs);
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_multires_activity_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_multires_activity_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        Rounding() = {};
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_activity_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_activity_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        // 計測は CPU の重い比べる実行の前に(GPU が長く空くとクロックが下がる)。WARP では測らない(大きい世界の暖機が遅すぎる)
        const bool measure = options->adapter != gpu::AdapterKind::Warp;
        const auto timing = measure ? Measure(device->Get(), *queue, *ring, *table, Rounding())
                                    : std::expected<ActivityTiming, std::string>(ActivityTiming{});
        const auto first = RunActivity(device->Get(), *queue, *ring, *table);
        const auto second = RunActivity(device->Get(), *queue, *ring, *table);
        for (const auto* result : {&first, &second}) {
            if (*result)
                continue;

            Log(Channel::Gpu, Level::Error, "gpu_multires_activity_test: FAILED ({})", result->error());
            return 1;
        }

        if (*first != *second) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_activity_test: FAILED(2 回の実行が食い違う)");
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_multires_activity_test"))
            return 1;

        if (!timing) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_activity_test: FAILED(計測: {})", timing.error());
            return 1;
        }

        if (measure)
            LogTiming(Rounding().cutoffRounding ? "今までの丸め" : "待ちの丸め", *timing);

        Log(Channel::Gpu, Level::Info,
            "gpu_multires_activity_test: OK({} 刻みで GPU と CPU がビット一致・次の刻みの種も一致。要約 {:016x})",
            test::ACTIVITY_TICKS, *first);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
