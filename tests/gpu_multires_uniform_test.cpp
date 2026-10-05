// gpu_multires_uniform_test.cpp — 一様なブロック(T-0102。刻む段の印 → TreeExpand → ExpandStepNode / StepExpanded、
// 要求の処理の頁)を GPU で走らせ、CPU リファレンス(sim::StepActive・ProcessRequests)と毎刻みビット一致することを確かめる。
// 場面は tests/multires_uniform_scene.h。
//   1. 頁が足りる / 2. 頁が 1 つ(頁の不足): 毎刻み、状態の全部(見出しの頁・セル・空きのスタック・数える欄…)と次の刻みの種を比べる
//   3. 一様な親へ値の違う一様な子を粗くする(親を頁に広げる)・同じ値の子を粗くする(一様のまま): 処理の後の状態の全部を比べる
//   4. 全部を刻む(RecordStep。StepExpanded)を数刻み: 状態の全部を比べる
// 時間は別に、暖機してから根 8³ 個の世界で「頁を配る段」だけを測る。引数は gpu_test_options.h。
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "multires_uniform_scene.h"
#include "sim/gpu_multires.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace {

    constexpr uint32_t WARMUP_STEPS = 400;     // 計測の前に刻みを何回投げるか(GPU のクロックを上げる)
    constexpr uint32_t MEASURE_ROOT_EDGE = 8;  // 計測の世界は根 8³ = 512 個
    constexpr uint32_t MEASURE_REPEATS = 8;
    constexpr uint64_t FULL_STEP_TICKS = 6;  // 4. 全部を刻む刻みの数

    bool RecordTick(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                    uint64_t tick) {
        if (!gpu.RecordRequests(list, test::UniformRequestsAt(tick)))
            return false;

        gpu.RecordQuietRequests(list, ring, tick);
        gpu.RecordProcessRequests(list, ring);

        return gpu.RecordStepActive(list, ring, test::STRESS_SEED, tick);
    }

    // 最初に食い違った所をログに出す
    void ReportFirstMismatch(const sim::MultiresNest& cpu, const sim::MultiresNest& gpu, std::string_view where) {
        for (size_t slot = 0; slot < cpu.blocks.size(); ++slot) {
            const MrBlock& a = cpu.blocks[slot];
            const MrBlock& b = gpu.blocks[slot];
            if (std::memcmp(&a, &b, sizeof(MrBlock)) != 0) {
                Log(Channel::Sim, Level::Error, "{}: 枠 {} の見出しが違う(種類 cpu {} / gpu {}・頁 cpu {} / gpu {})",
                    where, slot, a.kind, b.kind, a.page, b.page);
            }
        }

        for (size_t i = 0; i < cpu.cells.size(); ++i) {
            if (sim::HashReactionCell(cpu.cells[i]) == sim::HashReactionCell(gpu.cells[i]))
                continue;

            Log(Channel::Sim, Level::Error, "{}: セルのバッファの {} 番目が違う(一様の値は枠の数 {} まで)", where, i,
                cpu.blocks.size());
            break;
        }

        if (cpu.freeBlocks != gpu.freeBlocks)
            Log(Channel::Sim, Level::Error, "{}: 空きのスタックが違う", where);

        for (size_t i = 0; i < cpu.counters.size(); ++i) {
            if (cpu.counters[i] != gpu.counters[i])
                Log(Channel::Sim, Level::Error, "{}: 数える欄 {} が違う(cpu {} / gpu {})", where, i, cpu.counters[i],
                    gpu.counters[i]);
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

    std::expected<void, std::string> CompareWhole(const sim::MultiresNest& cpu, const sim::MultiresNest& gpu,
                                                  std::string_view where) {
        if (sim::HashWholeNest(cpu) == sim::HashWholeNest(gpu))
            return {};

        ReportFirstMismatch(cpu, gpu, where);

        return std::unexpected(std::format("{} で CPU と GPU が食い違う", where));
    }

    // 1・2: 活性で刻む場面を毎刻み比べる
    std::expected<uint64_t, std::string> RunActive(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                   gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                   uint32_t pages) {
        sim::MultiresNest cpu = test::MakeUniformNest(table, pages);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, {.activity = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read;
        for (uint64_t tick = 0; tick < test::UNIFORM_TICKS; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordTick(list, *gpu, ring.GpuAddress(), tick);
            };
            if (auto executed = Execute(queue, ring, *gpu, read, record); !executed)
                return std::unexpected(std::format("刻み {}: {}", tick, executed.error()));

            test::BeginUniformTick(cpu, tick);
            sim::StepActive(cpu, table, test::STRESS_SEED, tick);

            const std::string where = std::format("頁 {}・刻み {}", pages, tick);
            if (auto compared = CompareWhole(cpu, read, where); !compared)
                return std::unexpected(compared.error());

            const auto seeds = gpu->ReadSeeds();
            if (!seeds || seeds->dropped != 0)
                return std::unexpected(std::format("{}: 種の一覧を読めない・落ちた", where));

            if (seeds->slots != sim::SeedSlots(cpu))
                return std::unexpected(std::format("{}: 次の刻みの種が CPU と違う(cpu {} 個 / gpu {} 個)", where,
                                                   sim::SeedSlots(cpu).size(), seeds->slots.size()));
        }

        if (cpu.counters[MR_COUNTER_EXPANDED] == 0)
            return std::unexpected("場面の確認: 頁に広げたブロックが無い");

        if (pages == 1 && cpu.counters[MR_COUNTER_PAGE_SHORTAGE] != test::UNIFORM_TICKS)
            return std::unexpected("場面の確認: 頁が 1 つなのに毎刻み頁が足りなくならない");

        return sim::HashWholeNest(cpu);
    }

    // 3: 一様な親へ粗くする(energyDelta が 0 でなければ親を頁に広げる)
    std::expected<void, std::string> RunExpandParent(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                     gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                     int64_t energyDelta) {
        sim::MultiresNest cpu = test::MakeExpandParentNest(table, energyDelta);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity);
        if (!gpu)
            return std::unexpected(gpu.error());

        const MrRequest coarsen = test::ExpandParentCoarsenRequest();
        sim::MultiresNest read;
        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            if (!gpu->RecordUpload(list, cpu) || !gpu->RecordRequests(list, std::span(&coarsen, 1)))
                return false;

            gpu->RecordProcessRequests(list, ring.GpuAddress());

            return true;
        };
        if (auto executed = Execute(queue, ring, *gpu, read, record); !executed)
            return std::unexpected(executed.error());

        const uint32_t pagesBefore = sim::UsedWorldPages(cpu);
        sim::SubmitRequests(cpu, std::span(&coarsen, 1));
        sim::ProcessRequests(cpu);
        if (sim::UsedWorldPages(cpu) != pagesBefore + (energyDelta != 0 ? 1 : 0))
            return std::unexpected("場面の確認: 親を頁に広げたかが期待と違う");

        return CompareWhole(cpu, read, std::format("一様な親へ粗くする(エネルギーの差 {})", energyDelta));
    }

    // 4: 全部を刻む(RecordStep)を数刻み
    std::expected<void, std::string> RunFullStep(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                 gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = test::MakeUniformNest(table, 1);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity);
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read;
        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            if (!gpu->RecordUpload(list, cpu))
                return false;

            for (uint64_t tick = 0; tick < FULL_STEP_TICKS; ++tick)
                gpu->RecordStep(list, ring.GpuAddress(), test::STRESS_SEED, tick);

            return true;
        };
        if (auto executed = Execute(queue, ring, *gpu, read, record); !executed)
            return std::unexpected(executed.error());

        for (uint64_t tick = 0; tick < FULL_STEP_TICKS; ++tick)
            sim::StepNest(cpu, table, test::STRESS_SEED, tick);

        if (cpu.counters[MR_COUNTER_EXPANDED] != 1 || cpu.counters[MR_COUNTER_PAGE_SHORTAGE] != FULL_STEP_TICKS)
            return std::unexpected("場面の確認: 全部を刻む時に頁に広げた数・頁の不足の数が期待と違う");

        return CompareWhole(cpu, read, "全部を刻む");
    }

    // 頁を配る段(根 8³ 個 + 空きの枠 128。頁に広げたいブロックは無いので全部の枠を見るだけ)の平均の GPU 時間
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
                gpu->RecordStep(list, ring.GpuAddress(), test::STRESS_SEED, i);

            gpu->RecordTimestamp(list, 0);
            for (uint32_t i = 0; i < MEASURE_REPEATS; ++i)
                gpu->RecordExpandPass(list, ring.GpuAddress());

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

        return static_cast<double>(stamps[1] - stamps[0]) * 1000.0 / static_cast<double>(frequency) / MEASURE_REPEATS;
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_multires_uniform_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_multires_uniform_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_uniform_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_uniform_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        // 計測は CPU の重い比べる実行の前に(GPU が長く空くとクロックが下がる)。WARP では測らない
        const bool measure = options->adapter != gpu::AdapterKind::Warp;
        const auto expandMs = measure ? Measure(device->Get(), *queue, *ring, *table)
                                      : std::expected<double, std::string>(0.0);
        const auto ample = RunActive(device->Get(), *queue, *ring, *table, test::UNIFORM_WORLD_BLOCKS);
        const auto scarce = RunActive(device->Get(), *queue, *ring, *table, 1);
        const auto same = RunExpandParent(device->Get(), *queue, *ring, *table, 0);
        const auto different = RunExpandParent(device->Get(), *queue, *ring, *table, 1000);
        const auto full = RunFullStep(device->Get(), *queue, *ring, *table);
        const std::array<std::optional<std::string>, 5> errors = {
            ample ? std::nullopt : std::optional(ample.error()), scarce ? std::nullopt : std::optional(scarce.error()),
            same ? std::nullopt : std::optional(same.error()),
            different ? std::nullopt : std::optional(different.error()),
            full ? std::nullopt : std::optional(full.error())};
        for (const auto& error : errors) {
            if (!error)
                continue;

            Log(Channel::Gpu, Level::Error, "gpu_multires_uniform_test: FAILED ({})", *error);
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_multires_uniform_test"))
            return 1;

        if (!expandMs) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_uniform_test: FAILED(計測: {})", expandMs.error());
            return 1;
        }

        if (measure) {
            Log(Channel::Gpu, Level::Info, "GPU 時間: 頁を配る段(世界の枠 {}){:.4f} ms",
                (MEASURE_ROOT_EDGE * MEASURE_ROOT_EDGE * MEASURE_ROOT_EDGE) + 128, *expandMs);
        }

        Log(Channel::Gpu, Level::Info,
            "gpu_multires_uniform_test: OK({} 刻みで頁が足りる・1 つの両方で GPU と CPU がビット一致・種も一致・"
            "一様な親へ粗くする・全部を刻むも一致。要約 {:016x} / {:016x})",
            test::UNIFORM_TICKS, *ample, *scarce);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
