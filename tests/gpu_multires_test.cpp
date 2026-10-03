// gpu_multires_test.cpp — 多重解像度の木(sim/gpu_multires。Compute の段・Work Graph の再帰)を GPU で走らせ、
// CPU リファレンス(sim/multires_nest)と毎刻みビット一致することを確かめる(T-0017、17 §6 の基準 1〜2。T-0018、17 §5 の木の管理)。
// 場面は tests/multires_test_scene.h(本物の鎖・影の鎖・たくさんの要求)。状態の全部(見出し・セル・端数・空きのスタック・帳簿・数える欄)を
// 毎刻み比べ、GPU の索引で本物のブロックが全部引けることも確かめる。本物の鎖とたくさんの要求は 2 回走らせて一致を確かめる。
// 時間は別に、1 本のリストで暖機(GPU のクロックを上げる)してから測る(1 刻みの予算に収まるか)。
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
        uint32_t freeFractions = 0;
    };

    // 鎖の出来事と刻みの GPU 時間(ms)
    struct ChainTiming {
        double createMs = 0;  // 細かくする要求の処理(本物)/ 影を作る
        double stepMs = 0;    // 鎖があるときの刻み(影なら引き戻しを含む)
        double endMs = 0;     // 粗くする要求 1 段の処理(本物)/ 影を捨てる
        double emptyMs = 0;   // 要求が 0 件の処理(本物だけ)
    };

    // 1 刻みの GPU の記録(test::StepMultiresScene と同じ順)
    bool RecordTick(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                    test::MultiresScenario scenario, uint64_t tick) {
        using test::MultiresScenario;
        const sim::MultiresPoint point = test::MakeMultiresPoint(test::MULTIRES_LEVELS);
        if (!gpu.RecordRequests(list, test::MultiresRequestsAt(scenario, tick)))
            return false;

        gpu.RecordProcessRequests(list, ring);
        if (tick == test::MULTIRES_REFINE_TICK && scenario == MultiresScenario::Shadow) {
            gpu.RecordRefineShadow(list, ring, test::MULTIRES_ROOT_SLOT, test::MULTIRES_SHADOW_SLOT,
                                   test::MULTIRES_LEVELS, point);
        } else if (tick == test::MULTIRES_COARSEN_TICK && scenario == MultiresScenario::Shadow) {
            gpu.RecordRemoveShadow(list, ring, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS);
        }

        gpu.RecordStep(list, ring, test::MULTIRES_TEST_SEED, tick);
        if (test::MultiresShadowExists(scenario, tick))
            gpu.RecordPullBack(list, ring, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS);

        return true;
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

        const bool sameStacks = cpu.freeBlocks == gpu.freeBlocks && cpu.freeFractions == gpu.freeFractions;
        Log(Channel::Sim, Level::Error, "刻み {}: 空きのスタック {}・帳簿 {}", tick, sameStacks ? "一致" : "不一致",
            cpu.ledger == gpu.ledger ? "一致" : "不一致");
    }

    // 1 刻みを GPU で走らせて読み戻す(record がリストに記録する)
    template <typename Record>
    std::expected<void, std::string> ExecuteTick(gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                 sim::GpuMultires& gpu, sim::MultiresNest& read, uint64_t tick,
                                                 const Record& record) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        ring.RecordBegin(list);
        if (!record(list))
            return std::unexpected(std::format("刻み {} を記録できない", tick));

        gpu.RecordReadback(list);
        ring.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected(std::format("刻み {} の GPU での実行に失敗", tick));

        const gpu::DebugRingContents debugOutput = ring.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("刻み {}: GPU の FX_ASSERT が {} 件", tick, debugOutput.assertCount));

        if (!gpu.Read(read))
            return std::unexpected("読み戻せない");

        return {};
    }

    // CPU と GPU の状態の全部が一致し、GPU の索引で本物のブロックが全部引けるか
    std::expected<void, std::string> CompareTick(const sim::MultiresNest& cpu, const sim::MultiresNest& read,
                                                 uint64_t tick) {
        if (sim::HashWholeNest(cpu) != sim::HashWholeNest(read)) {
            ReportFirstMismatch(cpu, read, tick);
            return std::unexpected(std::format("刻み {} で CPU と GPU が食い違う", tick));
        }

        if (test::CountIndexMismatches(read) != 0)
            return std::unexpected(std::format("刻み {}: GPU の索引で引けない本物のブロックがある", tick));

        return {};
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
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity);
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read;
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordTick(list, *gpu, ring.GpuAddress(), scenario, tick);
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, tick, record); !executed)
                return std::unexpected(executed.error());

            test::StepMultiresScene(cpu, table, scenario, tick);
            if (auto compared = CompareTick(cpu, read, tick); !compared)
                return std::unexpected(compared.error());
        }

        return RunResult{.digest = sim::HashWholeNest(cpu), .freeFractions = cpu.counters[MR_COUNTER_FREE_FRACTIONS]};
    }

    // たくさんの要求(取り合い・枠が足りない・無効・索引の作り直し・帳簿)。要求は CPU の木から作る(GPU の木と同じ)
    std::expected<RunResult, std::string> RunStress(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                    gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = test::MakeStressNest(table);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity);
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read;
        for (uint64_t tick = 0; tick < test::STRESS_TICKS; ++tick) {
            const std::vector<MrRequest> requests = test::MakeStressRequests(cpu, tick);
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                if (!gpu->RecordRequests(list, requests))
                    return false;

                gpu->RecordProcessRequests(list, ring.GpuAddress());
                gpu->RecordStep(list, ring.GpuAddress(), test::STRESS_SEED, tick);

                return true;
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, tick, record); !executed)
                return std::unexpected(executed.error());

            sim::SubmitRequests(cpu, requests);
            sim::ProcessRequests(cpu);
            sim::StepNest(cpu, table, test::STRESS_SEED, tick);
            if (auto compared = CompareTick(cpu, read, tick); !compared)
                return std::unexpected(compared.error());
        }

        const auto& counters = cpu.counters;
        Log(Channel::Gpu, Level::Info,
            "たくさんの要求 {} 刻み: 適用 {}・済み {}・取り合い {}・枠不足 {}・無効 {}・落ちた端数 {}",
            test::STRESS_TICKS, counters[MR_COUNTER_GRANTED], counters[MR_COUNTER_ALREADY],
            counters[MR_COUNTER_CONFLICT], counters[MR_COUNTER_NO_SPACE], counters[MR_COUNTER_INVALID],
            counters[MR_COUNTER_LOST]);

        return RunResult{.digest = sim::HashWholeNest(cpu), .freeFractions = counters[MR_COUNTER_FREE_FRACTIONS]};
    }

    // 1 本のリストで: 暖機の刻み → タイムスタンプ 0 → 鎖を作る → 1 → 刻む(影なら引き戻しも)→ 2 → 鎖を終える(本物は 1 段)→ 3
    // → 要求 0 件の処理(本物だけ)→ 4
    void RecordChainEvents(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                           bool real, bool& recorded) {
        const sim::MultiresPoint point = test::MakeMultiresPoint(test::MULTIRES_LEVELS);
        const auto level = static_cast<int32_t>(test::MULTIRES_LEVELS);
        const std::array<MrRequest, 1> refine = {test::MakeMultiresRequest(MR_REQUEST_REFINE, point, level)};
        const std::array<MrRequest, 1> coarsen = {test::MakeMultiresRequest(MR_REQUEST_COARSEN, point, level)};
        gpu.RecordTimestamp(list, 0);
        if (real) {
            recorded &= gpu.RecordRequests(list, refine);
            gpu.RecordProcessRequests(list, ring);
        } else {
            gpu.RecordRefineShadow(list, ring, test::MULTIRES_ROOT_SLOT, test::MULTIRES_SHADOW_SLOT,
                                   test::MULTIRES_LEVELS, point);
        }

        gpu.RecordTimestamp(list, 1);
        gpu.RecordStep(list, ring, test::MULTIRES_TEST_SEED, WARMUP_STEPS);
        if (!real)
            gpu.RecordPullBack(list, ring, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS);

        gpu.RecordTimestamp(list, 2);
        if (real) {
            recorded &= gpu.RecordRequests(list, coarsen);
            gpu.RecordProcessRequests(list, ring);
        } else {
            gpu.RecordRemoveShadow(list, ring, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS);
        }

        gpu.RecordTimestamp(list, 3);
        if (real) {
            recorded &= gpu.RecordRequests(list, {});
            gpu.RecordProcessRequests(list, ring);
        }

        gpu.RecordTimestamp(list, 4);
    }

    std::expected<ChainTiming, std::string> MeasureChain(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                         gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                         test::MultiresScenario scenario) {
        const sim::MultiresNest initial = test::MakeMultiresNestForTest(table);
        auto gpu = sim::GpuMultires::Create(device, table, initial.capacity);
        ID3D12GraphicsCommandList10* list = gpu ? queue.Begin() : nullptr;
        if (list == nullptr)
            return std::unexpected("計測の準備ができない");

        ring.RecordBegin(list);
        if (!gpu->RecordUpload(list, initial))
            return std::unexpected("初めの状態を写せない");

        const D3D12_GPU_VIRTUAL_ADDRESS ringAddress = ring.GpuAddress();
        for (uint32_t i = 0; i < WARMUP_STEPS; ++i)
            gpu->RecordStep(list, ringAddress, test::MULTIRES_TEST_SEED, i);

        bool recorded = true;
        RecordChainEvents(list, *gpu, ringAddress, scenario == test::MultiresScenario::Real, recorded);
        gpu->RecordReadback(list);
        ring.RecordReadbackAndReset(list);
        uint64_t frequency = 0;
        if (!recorded || !queue.ExecuteAndWait() || ring.Drain().assertCount > 0 ||
            FAILED(queue.Native()->GetTimestampFrequency(&frequency))) {
            return std::unexpected("計測の実行に失敗");
        }

        const std::vector<uint64_t> stamps = gpu->ReadTimestamps(5);
        return ChainTiming{.createMs = ToMilliseconds(stamps, 0, 1, frequency),
                           .stepMs = ToMilliseconds(stamps, 1, 2, frequency),
                           .endMs = ToMilliseconds(stamps, 2, 3, frequency),
                           .emptyMs = ToMilliseconds(stamps, 3, 4, frequency)};
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
        // 計測は CPU の重いたくさんの要求の前に(GPU が長く空くとクロックが下がり、暖機しても時間が数倍に出る)
        const auto realTiming = MeasureChain(device->Get(), *queue, *ring, *table, MultiresScenario::Real);
        const auto shadowTiming = MeasureChain(device->Get(), *queue, *ring, *table, MultiresScenario::Shadow);
        const auto stress = RunStress(device->Get(), *queue, *ring, *table);
        const auto stressAgain = RunStress(device->Get(), *queue, *ring, *table);
        for (const auto* result : {&first, &second, &shadow, &stress, &stressAgain}) {
            if (*result)
                continue;

            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED ({})", result->error());
            return 1;
        }

        if (first->digest != second->digest || stress->digest != stressAgain->digest) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED(2 回の実行が食い違う)");
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_multires_test"))
            return 1;

        if (!realTiming || !shadowTiming) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED(計測)");
            return 1;
        }

        Log(Channel::Gpu, Level::Info,
            "GPU 時間: 細かくする要求({} 段){:.3f} ms・刻み(鎖あり){:.3f} ms・粗くする要求(1 段){:.3f} ms・"
            "要求 0 件の処理 {:.3f} ms・端数の枠の空き {}",
            test::MULTIRES_LEVELS, realTiming->createMs, realTiming->stepMs, realTiming->endMs, realTiming->emptyMs,
            second->freeFractions);
        Log(Channel::Gpu, Level::Info, "GPU 時間(影): 影を作る {:.3f} ms・刻み + 引き戻し {:.3f} ms・捨てる {:.3f} ms",
            shadowTiming->createMs, shadowTiming->stepMs, shadowTiming->endMs);
        Log(Channel::Gpu, Level::Info,
            "gpu_multires_test: OK(本物の鎖・影の鎖の {} 刻み・たくさんの要求の {} 刻みで GPU と CPU がビット一致。"
            "要約 本物 {:016x} / 影 {:016x} / たくさん {:016x})",
            test::MULTIRES_END_TICK, test::STRESS_TICKS, first->digest, shadow->digest, stress->digest);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
