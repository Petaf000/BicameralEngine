// gpu_multires_test.cpp — 多重解像度の入れ子(sim/gpu_multires。Work Graph の再帰と Compute)を GPU で走らせ、
// CPU リファレンス(sim/multires_nest)と毎刻みビット一致することを確かめる(T-0017、17 §6 の基準 1〜2)。
// 場面は tests/multires_test_scene.h(本物の鎖と影の鎖。k = 0〜9)。全部の枠の見出し・セル・端数・数える欄を毎刻み比べる。
// 本物の鎖は 2 回走らせて一致を確かめる。時間は別に、1 本のリストで暖機(GPU のクロックを上げる)してから測る(1 刻みの予算に収まるか)。
// 引数は gpu_test_options.h。
#include "sim/gpu_multires.h"
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "multires_test_scene.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace {

    // 計測の前に刻みを何回投げるか(短い仕事の間は GPU のクロックが上がらず、時間が 6〜8 倍に出る)
    constexpr uint32_t WARMUP_STEPS = 400;

    struct RunResult {
        uint64_t digest = 0;
        uint32_t fractionBlocks = 0;
    };

    // 鎖の出来事と刻みの GPU 時間(ms)
    struct ChainTiming {
        double createMs = 0;  // 細かくする(本物)/ 影を作る
        double stepMs = 0;    // 鎖があるときの刻み(影なら引き戻しを含む)
        double endMs = 0;     // 粗くする(本物)/ 影を捨てる
    };

    // 1 刻みの GPU の記録(test::StepMultiresScene と同じ順)
    void RecordTick(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                    test::MultiresScenario scenario, uint64_t tick) {
        using test::MultiresScenario;
        const sim::MultiresPoint point = test::MakeMultiresPoint(test::MULTIRES_LEVELS);
        if (tick == test::MULTIRES_REFINE_TICK && scenario == MultiresScenario::Real) {
            gpu.RecordRefine(list, ring, test::MULTIRES_ROOT_SLOT, test::MULTIRES_REAL_SLOT, test::MULTIRES_LEVELS,
                             MR_BLOCK_REAL, point);
        } else if (tick == test::MULTIRES_REFINE_TICK && scenario == MultiresScenario::Shadow) {
            gpu.RecordRefine(list, ring, test::MULTIRES_ROOT_SLOT, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS,
                             MR_BLOCK_SHADOW, point);
        } else if (tick == test::MULTIRES_COARSEN_TICK && scenario == MultiresScenario::Real) {
            gpu.RecordCoarsen(list, ring, test::MULTIRES_REAL_SLOT + test::MULTIRES_LEVELS - 1, test::MULTIRES_LEVELS);
        } else if (tick == test::MULTIRES_COARSEN_TICK && scenario == MultiresScenario::Shadow) {
            gpu.RecordRemoveShadow(list, ring, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS);
        }

        gpu.RecordStep(list, ring, test::MULTIRES_TEST_SEED, tick);
        if (test::MultiresShadowExists(scenario, tick))
            gpu.RecordPullBack(list, ring, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS);
    }

    // 最初に食い違った所(枠・セル)をログに出す
    void ReportFirstMismatch(const sim::MultiresNest& cpu, const sim::MultiresNest& gpu, uint64_t tick) {
        for (size_t slot = 0; slot < cpu.blocks.size(); ++slot) {
            const MrBlock& a = cpu.blocks[slot];
            const MrBlock& b = gpu.blocks[slot];
            if (std::memcmp(&a, &b, sizeof(MrBlock)) != 0)
                Log(Channel::Sim, Level::Error, "刻み {}: 枠 {} の見出しが違う(種類 cpu {} / gpu {})", tick, slot,
                    a.kind, b.kind);
        }

        for (size_t i = 0; i < cpu.cells.size(); ++i) {
            if (sim::HashReactionCell(cpu.cells[i]) == sim::HashReactionCell(gpu.cells[i]))
                continue;

            Log(Channel::Sim, Level::Error, "刻み {}: 枠 {} セル {} が違う(エネルギー cpu {} / gpu {})", tick,
                i / MR_BLOCK_CELLS, i % MR_BLOCK_CELLS, cpu.cells[i].energy, gpu.cells[i].energy);
            break;
        }

        for (size_t i = 0; i < cpu.counters.size(); ++i) {
            if (cpu.counters[i] != gpu.counters[i])
                Log(Channel::Sim, Level::Error, "刻み {}: 数える欄 {} が違う(cpu {} / gpu {})", tick, i,
                    cpu.counters[i], gpu.counters[i]);
        }
    }

    double ToMilliseconds(const std::vector<uint64_t>& stamps, uint32_t from, uint32_t to, uint64_t frequency) {
        if (stamps.size() <= to || frequency == 0)
            return 0;

        return static_cast<double>(stamps[to] - stamps[from]) * 1000.0 / static_cast<double>(frequency);
    }

    std::expected<RunResult, std::string> RunScenario(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                      gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                      test::MultiresScenario scenario) {
        sim::MultiresNest cpu = test::MakeMultiresNestForTest(table);
        auto gpu = sim::GpuMultires::Create(device, table, test::MULTIRES_BLOCK_CAPACITY,
                                            test::MULTIRES_FRACTION_CAPACITY);
        if (!gpu)
            return std::unexpected(gpu.error());

        RunResult result;
        sim::MultiresNest read;
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            ID3D12GraphicsCommandList10* list = queue.Begin();
            if (list == nullptr)
                return std::unexpected("コマンドリストを始められない");

            ring.RecordBegin(list);
            if (tick == 0 && !gpu->RecordUpload(list, cpu))
                return std::unexpected("初めの状態を写せない");

            RecordTick(list, *gpu, ring.GpuAddress(), scenario, tick);
            gpu->RecordReadback(list);
            ring.RecordReadbackAndReset(list);
            if (!queue.ExecuteAndWait())
                return std::unexpected(std::format("刻み {} の GPU での実行に失敗", tick));

            const gpu::DebugRingContents debugOutput = ring.Drain();
            if (debugOutput.assertCount > 0)
                return std::unexpected(
                    std::format("刻み {}: GPU の FX_ASSERT が {} 件", tick, debugOutput.assertCount));

            if (!gpu->Read(read))
                return std::unexpected("読み戻せない");

            test::StepMultiresScene(cpu, table, scenario, tick);
            if (sim::HashWholeNest(cpu) != sim::HashWholeNest(read)) {
                ReportFirstMismatch(cpu, read, tick);
                return std::unexpected(std::format("刻み {} で CPU と GPU が食い違う", tick));
            }
        }

        result.digest = sim::HashWholeNest(cpu);
        result.fractionBlocks = cpu.counters[MR_COUNTER_FRACTION_BLOCKS];

        return result;
    }

    // 1 本のリストで: 暖機の刻み → タイムスタンプ 0 → 鎖を作る → 1 → 刻む(影なら引き戻しも)→ 2 → 鎖を終える → 3
    std::expected<ChainTiming, std::string> MeasureChain(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                         gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                         test::MultiresScenario scenario) {
        auto gpu = sim::GpuMultires::Create(device, table, test::MULTIRES_BLOCK_CAPACITY,
                                            test::MULTIRES_FRACTION_CAPACITY);
        ID3D12GraphicsCommandList10* list = gpu ? queue.Begin() : nullptr;
        if (list == nullptr)
            return std::unexpected("計測の準備ができない");

        ring.RecordBegin(list);
        if (!gpu->RecordUpload(list, test::MakeMultiresNestForTest(table)))
            return std::unexpected("初めの状態を写せない");

        const D3D12_GPU_VIRTUAL_ADDRESS ringAddress = ring.GpuAddress();
        for (uint32_t i = 0; i < WARMUP_STEPS; ++i)
            gpu->RecordStep(list, ringAddress, test::MULTIRES_TEST_SEED, i);

        const bool real = scenario == test::MultiresScenario::Real;
        const uint32_t firstSlot = real ? test::MULTIRES_REAL_SLOT : test::MULTIRES_SHADOW_SLOT;
        gpu->RecordTimestamp(list, 0);
        gpu->RecordRefine(list, ringAddress, test::MULTIRES_ROOT_SLOT, firstSlot, test::MULTIRES_LEVELS,
                          real ? MR_BLOCK_REAL : MR_BLOCK_SHADOW, test::MakeMultiresPoint(test::MULTIRES_LEVELS));
        gpu->RecordTimestamp(list, 1);
        gpu->RecordStep(list, ringAddress, test::MULTIRES_TEST_SEED, WARMUP_STEPS);
        if (!real)
            gpu->RecordPullBack(list, ringAddress, firstSlot, test::MULTIRES_LEVELS);

        gpu->RecordTimestamp(list, 2);
        if (real)
            gpu->RecordCoarsen(list, ringAddress, firstSlot + test::MULTIRES_LEVELS - 1, test::MULTIRES_LEVELS);
        else
            gpu->RecordRemoveShadow(list, ringAddress, firstSlot, test::MULTIRES_LEVELS);

        gpu->RecordTimestamp(list, 3);
        gpu->RecordReadback(list);
        ring.RecordReadbackAndReset(list);
        uint64_t frequency = 0;
        if (!queue.ExecuteAndWait() || ring.Drain().assertCount > 0 ||
            FAILED(queue.Native()->GetTimestampFrequency(&frequency))) {
            return std::unexpected("計測の実行に失敗");
        }

        const std::vector<uint64_t> stamps = gpu->ReadTimestamps(4);
        return ChainTiming{.createMs = ToMilliseconds(stamps, 0, 1, frequency),
                           .stepMs = ToMilliseconds(stamps, 1, 2, frequency),
                           .endMs = ToMilliseconds(stamps, 2, 3, frequency)};
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_multires_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_multires_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        using test::MultiresScenario;
        const auto first = RunScenario(device->Get(), *queue, *ring, *table, MultiresScenario::Real);
        const auto second = RunScenario(device->Get(), *queue, *ring, *table, MultiresScenario::Real);
        const auto shadow = RunScenario(device->Get(), *queue, *ring, *table, MultiresScenario::Shadow);
        for (const auto* result : {&first, &second, &shadow}) {
            if (*result)
                continue;

            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED ({})", result->error());
            return 1;
        }

        if (first->digest != second->digest) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED(2 回の実行が食い違う)");
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_multires_test"))
            return 1;

        const auto realTiming = MeasureChain(device->Get(), *queue, *ring, *table, MultiresScenario::Real);
        const auto shadowTiming = MeasureChain(device->Get(), *queue, *ring, *table, MultiresScenario::Shadow);
        if (!realTiming || !shadowTiming) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED(計測)");
            return 1;
        }

        Log(Channel::Gpu, Level::Info,
            "GPU 時間: 細かくする {} 段 {:.3f} ms・刻み(鎖あり){:.3f} ms・粗くする {:.3f} ms・端数のブロック {} 個",
            test::MULTIRES_LEVELS, realTiming->createMs, realTiming->stepMs, realTiming->endMs, second->fractionBlocks);
        Log(Channel::Gpu, Level::Info, "GPU 時間(影): 影を作る {:.3f} ms・刻み + 引き戻し {:.3f} ms・捨てる {:.3f} ms",
            shadowTiming->createMs, shadowTiming->stepMs, shadowTiming->endMs);
        Log(Channel::Gpu, Level::Info,
            "gpu_multires_test: OK(本物の鎖と影の鎖の {} 刻みで GPU と CPU がビット一致。要約 本物 {:016x} / 影 "
            "{:016x})",
            test::MULTIRES_END_TICK, first->digest, shadow->digest);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
