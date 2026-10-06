// gpu_multires_quiet_test.cpp — 静かなブロックを粗くする(sim::GpuMultires::RecordQuietRequests。Compute の TreeQuiet)を GPU で走らせ、
// CPU リファレンス(sim::SubmitQuietCoarsenRequests)と毎刻みビット一致することを確かめる(T-0101)。場面は tests/multires_quiet_scene.h。
// 1 回目は毎刻み、状態の全部(見出しの活性と忙しさの印・セル・端数・空きのスタック・帳簿・数える欄)と次の刻みの種の集合を比べる。
// 2 回目は 15 刻みずつ 1 本のリストで投げ(読み戻しは最後だけ)、最後の要約が 1 回目と一致することを確かめる。
// 時間は別に、暖機してから根 8³ 個の世界で「静かな葉を探す段」だけを測る。引数は gpu_test_options.h。
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "multires_quiet_scene.h"
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
    constexpr uint32_t MEASURE_ROOT_EDGE = 8;  // 計測の世界は根 8³ = 512 個
    constexpr uint32_t MEASURE_REPEATS = 8;
    constexpr uint32_t TICKS_PER_LIST = 15;  // 2 回目の 1 本のリストの刻み(RecordRequests の写しの枠 16 まで)

    // 1 刻みの GPU の記録(test::BeginQuietTick → StepActive と同じ順)
    bool RecordTick(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                    uint64_t tick) {
        if (!gpu.RecordRequests(list, test::QuietRequestsAt(tick)))
            return false;

        gpu.RecordFoldPages(list, ring, tick);
        gpu.RecordQuietRequests(list, ring, tick);
        gpu.RecordProcessRequests(list, ring);

        return gpu.RecordStepActive(list, ring, test::STRESS_SEED, tick, Rounding());
    }

    // 刻み [first, end) を 1 本のリストに
    bool RecordTicks(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                     uint64_t first, uint64_t end) {
        for (uint64_t tick = first; tick < end; ++tick) {
            if (!RecordTick(list, gpu, ring, tick))
                return false;
        }

        return true;
    }

    // 最初に食い違った所をログに出す
    void ReportFirstMismatch(const sim::MultiresNest& cpu, const sim::MultiresNest& gpu, uint64_t tick) {
        for (size_t slot = 0; slot < cpu.blocks.size(); ++slot) {
            const MrBlock& a = cpu.blocks[slot];
            const MrBlock& b = gpu.blocks[slot];
            if (std::memcmp(&a, &b, sizeof(MrBlock)) != 0) {
                Log(Channel::Sim, Level::Error,
                    "刻み {}: 枠 {} の見出しが違う(種類 cpu {} / gpu {}・忙しさ cpu {} / gpu {})", tick, slot, a.kind,
                    b.kind, a.busyTick, b.busyTick);
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

    // 1 回目: 毎刻み CPU と比べる
    std::expected<uint64_t, std::string> RunCompared(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                     gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = test::MakeActivityNest(table);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, {.activity = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read;
        for (uint64_t tick = 0; tick < test::QUIET_TICKS; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordTick(list, *gpu, ring.GpuAddress(), tick);
            };
            if (auto executed = Execute(queue, ring, *gpu, read, record); !executed)
                return std::unexpected(std::format("刻み {}: {}", tick, executed.error()));

            test::BeginQuietTick(cpu, tick, test::QuietRequestsAt(tick));
            sim::StepActive(cpu, table, test::STRESS_SEED, tick, Rounding());

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

        if (cpu.counters[MR_COUNTER_QUIET_DEFERRED] == 0 || test::CountRealBlocks(cpu) != test::ACTIVITY_ROOTS)
            return std::unexpected("場面の確認: 一覧が一杯で回した数が 0・終わりに根だけになっていない");

        Log(Channel::Gpu, Level::Info, "静かな葉 {} 刻み: 粗くする要求 {}・一覧が一杯で回した {}", test::QUIET_TICKS,
            cpu.counters[MR_COUNTER_QUIET_REQUESTS], cpu.counters[MR_COUNTER_QUIET_DEFERRED]);

        return sim::HashWholeNest(cpu);
    }

    // 2 回目: TICKS_PER_LIST 刻みずつ投げ、最後だけ読む
    std::expected<uint64_t, std::string> RunBatched(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                    gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        const sim::MultiresNest initial = test::MakeActivityNest(table);
        auto gpu = sim::GpuMultires::Create(device, table, initial.capacity, {.activity = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read;
        for (uint64_t first = 0; first < test::QUIET_TICKS; first += TICKS_PER_LIST) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (first == 0 && !gpu->RecordUpload(list, initial))
                    return false;

                return RecordTicks(list, *gpu, ring.GpuAddress(), first,
                                   std::min(first + TICKS_PER_LIST, test::QUIET_TICKS));
            };
            if (auto executed = Execute(queue, ring, *gpu, read, record); !executed)
                return std::unexpected(std::format("刻み {} から: {}", first, executed.error()));
        }

        return sim::HashWholeNest(read);
    }

    // 静かな葉を探す段(根 8³ 個 + 空きの枠 128。静かな葉は無いので一覧は変わらない)の平均の GPU 時間
    std::expected<double, std::string> Measure(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                               const sim::BakedReactionTable& table) {
        const sim::MultiresNest initial = test::MakeActivityNest(table, MEASURE_ROOT_EDGE);
        auto gpu = sim::GpuMultires::Create(device, table, initial.capacity);
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read;
        bool uploaded = true;
        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            uploaded = gpu->RecordUpload(list, initial);
            for (uint32_t i = 0; i < WARMUP_STEPS; ++i)
                gpu->RecordStep(list, ring.GpuAddress(), test::STRESS_SEED, i, Rounding());

            gpu->RecordTimestamp(list, 0);
            for (uint32_t i = 0; i < MEASURE_REPEATS; ++i)
                gpu->RecordQuietRequests(list, ring.GpuAddress(), 10000 + i);

            gpu->RecordTimestamp(list, 1);

            return uploaded;
        };
        if (auto executed = Execute(queue, ring, *gpu, read, record); !executed)
            return std::unexpected(executed.error());

        uint64_t frequency = 0;
        if (FAILED(queue.Native()->GetTimestampFrequency(&frequency)))
            return std::unexpected("タイムスタンプの周波数を読めない");

        const std::vector<uint64_t> stamps = gpu->ReadTimestamps(2);
        if (stamps.size() < 2 || frequency == 0)
            return std::unexpected("タイムスタンプを読めない");

        if (read.counters[MR_COUNTER_REQUESTS] != 0)
            return std::unexpected("根だけの世界で静かな葉の要求が出た");

        return static_cast<double>(stamps[1] - stamps[0]) * 1000.0 / static_cast<double>(frequency) / MEASURE_REPEATS;
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_multires_quiet_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_multires_quiet_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        Rounding() = {};
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_quiet_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_quiet_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        // 計測は CPU の重い比べる実行の前に(GPU が長く空くとクロックが下がる)。WARP では測らない
        const bool measure = options->adapter != gpu::AdapterKind::Warp;
        const auto quietMs = measure ? Measure(device->Get(), *queue, *ring, *table)
                                     : std::expected<double, std::string>(0.0);
        const auto first = RunCompared(device->Get(), *queue, *ring, *table);
        const auto second = RunBatched(device->Get(), *queue, *ring, *table);
        for (const auto* result : {&first, &second}) {
            if (*result)
                continue;

            Log(Channel::Gpu, Level::Error, "gpu_multires_quiet_test: FAILED ({})", result->error());
            return 1;
        }

        if (*first != *second) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_quiet_test: FAILED(2 回の実行が食い違う)");
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_multires_quiet_test"))
            return 1;

        if (!quietMs) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_quiet_test: FAILED(計測: {})", quietMs.error());
            return 1;
        }

        if (measure) {
            Log(Channel::Gpu, Level::Info, "GPU 時間: 静かな葉を探す段(世界の枠 {}){:.4f} ms",
                (MEASURE_ROOT_EDGE * MEASURE_ROOT_EDGE * MEASURE_ROOT_EDGE) + 128, *quietMs);
        }

        Log(Channel::Gpu, Level::Info,
            "gpu_multires_quiet_test: OK({} 刻みで GPU と CPU "
            "がビット一致・次の刻みの種も一致・まとめて投げても一致。要約 {:016x})",
            test::QUIET_TICKS, *first);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
