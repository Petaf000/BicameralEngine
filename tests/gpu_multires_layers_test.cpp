// gpu_multires_layers_test.cpp — セルの成分ごとの層の量(T-1221。ADR-0481 の 5・20 §10.5)を GPU の世界(sim/gpu_multires)で持ち、
// CPU リファレンス(sim::StepNest・StepActive の層の付け替え。T-1210・T-1220)と毎刻みビット一致することを確かめる。
//   1. 写す像(CPU だけ): 像にして戻すと状態の要約が同じ・層の頁が足りなければ断る
//   2. 層のある場面: 根の頁の全部のセルに層を書き、全部を刻む(RecordStep)・伝導つき・活性だけ刻む(RecordStepActive)を毎刻み
//      読み戻して比べる(状態の全部 = 層まで。読み戻しは毎回新しい入れ物へ: CPU の前の層に頼らず GPU の層だけで一致する)。
//      0 ≤ 層 ≤ 量 で、燃えて減った物質の層が GPU でも減る(試験が空でない)
//   3. まとめて刻む: 1 本のリストで写してから何刻みも刻み、最後に 1 回だけ読み戻して比べる(層が VRAM で刻みをまたいで続く)
//   4. 層の無い場面: 層の頁を持つ GPU でも、層の無い世界は今とビット単位で同じ(CPU と一致)
// 時間(WARP では測らない): 層のある場面の 1 刻み(全部を刻む)を層の頁あり・なしで測り、層を合わせる段の分を見る(docs/perf.md)。
// 場面は tests/multires_test_scene.h の根(要求は出さない: 細かくする / 粗くする時の層の配りは T-1240)。引数は gpu_test_options.h。
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "multires_test_scene.h"
#include "reaction_test_table.h"
#include "sim/gpu_multires.h"
#include "sim/gpu_multires_layers.h"
#include "sim/multires_state.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace {

    constexpr uint64_t LAYER_TICKS = 24;    // 毎刻み比べる場面の刻みの数
    constexpr uint64_t BATCH_TICKS = 12;    // まとめて刻む場面の刻みの数
    constexpr uint32_t LAYER_PAGES = 4;     // GPU の層の頁の数
    constexpr uint32_t WARMUP_STEPS = 200;  // 計測の前の刻み(GPU のクロックを上げる)
    constexpr uint32_t MEASURE_STEPS = 64;  // 計測の刻み

    enum class StepMode : uint8_t {
        Full,
        Conduction,
        Active,
    };

    std::string_view ModeName(StepMode mode) {
        switch (mode) {
            case StepMode::Full: return "全部を刻む";
            case StepMode::Conduction: return "伝導つき";
            case StepMode::Active: return "活性だけ";
        }

        return "?";
    }

    // セルの成分ごとに 量 × 3 ÷ 4 の層(multires_layers_test の QuarterLayers と同じ)
    sim::MrCellLayers ThreeQuarterLayers(const RxCell& cell) {
        sim::MrCellLayers layers;
        for (uint32_t i = 0; i < cell.speciesCount; ++i)
            layers.amounts[i] = cell.amounts[i] / 4 * 3;

        return layers;
    }

    // 試験の根(頁を持つ)。layered なら全部のセルに層を書く
    sim::MultiresNest MakeScene(const sim::BakedReactionTable& table, bool layered) {
        sim::MultiresNest nest = test::MakeMultiresNestForTest(table);
        if (!layered)
            return nest;

        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const RxCell cell = sim::LoadNestCell(nest, test::MULTIRES_ROOT_SLOT, index);
            sim::StoreNestCellWithLayers(nest, test::MULTIRES_ROOT_SLOT, index, cell, ThreeQuarterLayers(cell));
        }

        return nest;
    }

    // 根の 0 でない層の和(試験が空でないかを見る)
    uint64_t RootLayerSum(const sim::MultiresNest& nest) {
        uint64_t sum = 0;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const sim::MrCellLayers layers = sim::LoadNestCellLayers(nest, test::MULTIRES_ROOT_SLOT, index);
            for (const uint64_t layer : layers.amounts)
                sum += layer;
        }

        return sum;
    }

    sim::MultiresStepOptions OptionsOf(StepMode mode) {
        sim::MultiresStepOptions options;
        options.conduction = mode == StepMode::Conduction;

        return options;
    }

    void StepCpu(sim::MultiresNest& nest, const sim::BakedReactionTable& table, StepMode mode, uint64_t tick) {
        if (mode == StepMode::Active) {
            sim::StepActive(nest, table, test::MULTIRES_TEST_SEED, tick, OptionsOf(mode));
            return;
        }

        sim::StepNest(nest, table, test::MULTIRES_TEST_SEED, tick, OptionsOf(mode));
    }

    bool RecordGpuStep(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                       StepMode mode, uint64_t tick) {
        if (mode == StepMode::Active)
            return gpu.RecordStepActive(list, ring, test::MULTIRES_TEST_SEED, tick, OptionsOf(mode));

        gpu.RecordStep(list, ring, test::MULTIRES_TEST_SEED, tick, OptionsOf(mode));

        return true;
    }

    // record を 1 本のリストで走らせ、新しい入れ物に読み戻す
    template <typename Record>
    std::expected<sim::MultiresNest, std::string> Execute(gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                          sim::GpuMultires& gpu, const Record& record) {
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

        if (const gpu::DebugRingContents output = ring.Drain(); output.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", output.assertCount));

        sim::MultiresNest read;
        if (!gpu.Read(read))
            return std::unexpected("読み戻せない");

        return read;
    }

    std::expected<void, std::string> Compare(const sim::MultiresNest& cpu, const sim::MultiresNest& read,
                                             std::string_view where) {
        if (sim::CountLayerMisfits(read) != 0)
            return std::unexpected(std::format("{}: GPU の層が量を超える", where));

        if (RootLayerSum(cpu) != RootLayerSum(read))
            return std::unexpected(
                std::format("{}: 層の和が違う(cpu {} / gpu {})", where, RootLayerSum(cpu), RootLayerSum(read)));

        if (sim::HashWholeNest(cpu) != sim::HashWholeNest(read))
            return std::unexpected(std::format("{}: CPU と GPU の状態の要約が違う", where));

        return {};
    }

    sim::GpuMultiresOptions GpuOptions(StepMode mode, uint32_t layerPages) {
        sim::GpuMultiresOptions options;
        options.activity = mode == StepMode::Active;
        options.layerPages = layerPages;

        return options;
    }

    // --- 1. 写す像(CPU だけ)---
    std::expected<void, std::string> TestImage(const sim::BakedReactionTable& table) {
        sim::MultiresNest nest = MakeScene(table, true);
        const uint64_t before = sim::HashWholeNest(nest);
        auto image = sim::MakeGpuLayerImage(nest, LAYER_PAGES);
        if (!image || image->used != 1)
            return std::unexpected("像: 層のある頁 1 つを写せない");

        sim::ApplyGpuLayerImage(nest, image->words, LAYER_PAGES);
        if (sim::HashWholeNest(nest) != before)
            return std::unexpected("像: 写して戻すと状態の要約が変わる");

        if (sim::MakeGpuLayerImage(nest, 0))
            return std::unexpected("像: 層の頁が 0 なのに写せてしまう");

        return {};
    }

    // --- 2・4. 毎刻み比べる ---
    std::expected<void, std::string> RunEachTick(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                 gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                 StepMode mode, bool layered) {
        sim::MultiresNest cpu = MakeScene(table, layered);
        const uint64_t firstSum = RootLayerSum(cpu);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, GpuOptions(mode, LAYER_PAGES));
        if (!gpu)
            return std::unexpected(gpu.error());

        const std::string scene = std::format("{}・{}", ModeName(mode), layered ? "層あり" : "層なし");
        uint64_t lastSum = firstSum;
        for (uint64_t tick = 0; tick < LAYER_TICKS; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordGpuStep(list, *gpu, ring.GpuAddress(), mode, tick);
            };
            auto read = Execute(queue, ring, *gpu, record);
            if (!read)
                return std::unexpected(std::format("{} 刻み {}: {}", scene, tick, read.error()));

            StepCpu(cpu, table, mode, tick);
            if (auto compared = Compare(cpu, *read, std::format("{} 刻み {}", scene, tick)); !compared)
                return compared;

            lastSum = RootLayerSum(*read);
        }

        if (layered && !(lastSum < firstSum && lastSum > 0))
            return std::unexpected(
                std::format("{}: 層が減らない・全部消えた(初め {}・終わり {})", scene, firstSum, lastSum));

        Log(Channel::Gpu, Level::Info, "{}: {} 刻み毎回一致(層の和 {} → {})", scene, LAYER_TICKS, firstSum, lastSum);

        return {};
    }

    // --- 3. まとめて刻む ---
    std::expected<void, std::string> RunBatch(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                              const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = MakeScene(table, true);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, GpuOptions(StepMode::Full, LAYER_PAGES));
        if (!gpu)
            return std::unexpected(gpu.error());

        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            if (!gpu->RecordUpload(list, cpu))
                return false;

            for (uint64_t tick = 0; tick < BATCH_TICKS; ++tick)
                gpu->RecordStep(list, ring.GpuAddress(), test::MULTIRES_TEST_SEED, tick);

            return true;
        };
        auto read = Execute(queue, ring, *gpu, record);
        if (!read)
            return std::unexpected(std::format("まとめて: {}", read.error()));

        for (uint64_t tick = 0; tick < BATCH_TICKS; ++tick)
            StepCpu(cpu, table, StepMode::Full, tick);

        return Compare(cpu, *read, std::format("まとめて {} 刻み", BATCH_TICKS));
    }

    // --- 時間: 層のある場面の 1 刻み(全部を刻む)を層の頁あり・なしで ---
    std::expected<double, std::string> MeasureStep(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                   gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                   uint32_t layerPages) {
        const sim::MultiresNest cpu = MakeScene(table, true);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, GpuOptions(StepMode::Full, layerPages));
        if (!gpu)
            return std::unexpected(gpu.error());

        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            if (!gpu->RecordUpload(list, cpu))
                return false;

            for (uint32_t i = 0; i < WARMUP_STEPS; ++i)
                gpu->RecordStep(list, ring.GpuAddress(), test::MULTIRES_TEST_SEED, i);

            gpu->RecordTimestamp(list, 0);
            for (uint32_t i = 0; i < MEASURE_STEPS; ++i)
                gpu->RecordStep(list, ring.GpuAddress(), test::MULTIRES_TEST_SEED, WARMUP_STEPS + i);

            gpu->RecordTimestamp(list, 1);

            return true;
        };
        if (auto read = Execute(queue, ring, *gpu, record); !read)
            return std::unexpected(std::format("計測: {}", read.error()));

        const std::vector<uint64_t> stamps = gpu->ReadTimestamps(2);
        if (stamps.size() != 2)
            return std::unexpected("計測: タイムスタンプを読めない");

        uint64_t frequency = 0;
        if (FAILED(queue.Native()->GetTimestampFrequency(&frequency)) || frequency == 0)
            return std::unexpected("計測: タイムスタンプの周波数を読めない");

        return static_cast<double>(stamps[1] - stamps[0]) * 1000.0 / static_cast<double>(frequency) / MEASURE_STEPS;
    }

    std::expected<void, std::string> RunScenes(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                               const sim::BakedReactionTable& table) {
        if (auto image = TestImage(table); !image)
            return image;

        for (const StepMode mode : {StepMode::Full, StepMode::Conduction, StepMode::Active}) {
            if (auto run = RunEachTick(device, queue, ring, table, mode, true); !run)
                return run;
        }

        if (auto plain = RunEachTick(device, queue, ring, table, StepMode::Full, false); !plain)
            return plain;

        return RunBatch(device, queue, ring, table);
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_multires_layers_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_multires_layers_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_layers_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_layers_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        // 計測は比べる実行の前に(WARP では測らない)
        if (options->adapter != gpu::AdapterKind::Warp) {
            const auto withLayers = MeasureStep(device->Get(), *queue, *ring, *table, LAYER_PAGES);
            const auto without = MeasureStep(device->Get(), *queue, *ring, *table, 0);
            if (!withLayers || !without) {
                Log(Channel::Gpu, Level::Error, "gpu_multires_layers_test: FAILED(計測)");
                return 1;
            }

            Log(Channel::Gpu, Level::Info,
                "GPU 時間: 1 刻み(全部を刻む・根 1 頁に層)層の頁あり {:.4f} ms・なし {:.4f} ms(層の置き場 {} B)",
                *withLayers, *without, uint64_t{LAYER_PAGES} * (2 + (MR_BLOCK_CELLS * RX_MAX_CELL_SPECIES * 3)) * 4);
        }

        if (auto scenes = RunScenes(device->Get(), *queue, *ring, *table); !scenes) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_layers_test: FAILED ({})", scenes.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_multires_layers_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_multires_layers_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
