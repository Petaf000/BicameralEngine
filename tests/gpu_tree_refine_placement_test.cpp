// gpu_tree_refine_placement_test.cpp — 世界の木を細かくする段のセルの中身の置かれ方(T-1188。D-664・D-674・D-679)を GPU で走らせ、
// CPU リファレンス(sim::ProcessRequests の PlaceRefineOctantCells。T-1176)と状態の全部がビット単位で同じことを確かめる。
// 場面は tests/tree_refine_placement_scene.h(粒の層・斜面・液体の溜まり)。GPU は shaders/sim/multires_refine_place.hlsli。
//   - 置かれ方を使う世界(EnableRefineCellPlacement)を 1 段(根の八分の一)と 2 段の鎖(子の八分の一まで。2 段目は 1 段目が配った頁を読む)で
//   - 粒の詰まり 100 %(印 0)と 60 %(D-679 の既定)
//   - 置かれ方を使わない世界(SAME)も今まで通り一致
// 場面ごとに、要求を処理する段(RecordProcessRequests。置かれ方の世界は別の Work Graph のプログラム。ADR-0499)の GPU の時間を
// 同じ処理 8 回の最小で出す(docs/perf.md)。
// 引数は gpu_test_options.h。
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "sim/gpu_multires.h"
#include "tree_refine_placement_scene.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::test::tree_place;

namespace {

    // 最初に食い違ったセルをログに出す
    void ReportFirstMismatch(const sim::MultiresNest& cpu, const sim::MultiresNest& gpu, std::string_view where) {
        for (size_t slot = 0; slot < cpu.blocks.size(); ++slot) {
            if (std::memcmp(&cpu.blocks[slot], &gpu.blocks[slot], sizeof(MrBlock)) != 0)
                Log(Channel::Sim, Level::Error, "{}: 枠 {} の見出しが違う", where, slot);
        }

        const std::vector<RxCell> cpuCells = sim::DenseCellImage(cpu);
        const std::vector<RxCell> gpuCells = sim::DenseCellImage(gpu);
        for (size_t i = 0; i < cpuCells.size(); ++i) {
            if (sim::HashReactionCell(cpuCells[i]) == sim::HashReactionCell(gpuCells[i]))
                continue;

            Log(Channel::Sim, Level::Error,
                "{}: セルのバッファの {} 番目が違う(エネルギー cpu {} / gpu {}・成分の数 cpu {} / gpu "
                "{}。一様の値は枠の数 {} まで)",
                where, i, cpuCells[i].energy, gpuCells[i].energy, cpuCells[i].speciesCount, gpuCells[i].speciesCount,
                cpu.blocks.size());
            break;
        }

        if (cpu.fractions.size() == gpu.fractions.size() &&
            std::memcmp(cpu.fractions.data(), gpu.fractions.data(), cpu.fractions.size() * sizeof(MrFraction)) != 0)
            Log(Channel::Sim, Level::Error, "{}: 端数の枠が違う", where);
    }

    constexpr uint32_t TIMING_RUNS = 8;  // 時間は同じ処理のこの回数の最小

    // 時間を測るタイムスタンプ(要求を処理する段の前と後)
    struct Timestamps {
        ComPtr<ID3D12QueryHeap> heap;
        ComPtr<ID3D12Resource> readback;
    };

    std::expected<Timestamps, std::string> CreateTimestamps(ID3D12Device5* device) {
        Timestamps result;
        result.readback = gpu::CreateBuffer(device, 2 * sizeof(uint64_t), gpu::BufferKind::Readback);
        const D3D12_QUERY_HEAP_DESC heapDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 2, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&result.heap))) || !result.readback)
            return std::unexpected("タイムスタンプを作れない");

        return result;
    }

    // 1 つの場面: 細かくする前の世界を写し、要求を 1 つ処理して読み戻し、CPU の同じ処理と比べる。
    // 同じ処理を TIMING_RUNS 回くり返し、要求を処理する段の GPU の時間の最小を ms で返す
    std::expected<double, std::string> RunScene(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                const Timestamps& timestamps, const Materials& materials, bool placed,
                                                int32_t level, std::string_view name) {
        const World initial = MakeWorld(materials, placed, false);
        const World expected = MakeWorld(materials, placed, true, level);
        auto gpu = sim::GpuMultires::Create(device, materials.table, initial.nest.capacity);
        if (!gpu)
            return std::unexpected(gpu.error());

        uint64_t frequency = 0;
        if (FAILED(queue.Native()->GetTimestampFrequency(&frequency)) || frequency == 0)
            return std::unexpected("タイムスタンプの周波数を読めない");

        double bestMs = 1.0e9;
        for (uint32_t run = 0; run < TIMING_RUNS; ++run) {
            ID3D12GraphicsCommandList10* list = queue.Begin();
            if (list == nullptr)
                return std::unexpected("コマンドリストを始められない");

            const MrRequest refine = MrMakeRequest(MR_REQUEST_REFINE, level, 1, 1, 1);
            ring.RecordBegin(list);
            if (!gpu->RecordUpload(list, initial.nest) || !gpu->RecordRequests(list, std::span(&refine, 1)))
                return std::unexpected("写せない");

            list->EndQuery(timestamps.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            gpu->RecordProcessRequests(list, ring.GpuAddress());
            list->EndQuery(timestamps.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            list->ResolveQueryData(timestamps.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timestamps.readback.Get(),
                                   0);
            gpu->RecordReadback(list);
            ring.RecordReadbackAndReset(list);
            if (!queue.ExecuteAndWait())
                return std::unexpected("GPU での実行に失敗");

            std::array<uint64_t, 2> times{};
            if (!gpu::ReadBuffer(timestamps.readback.Get(), std::as_writable_bytes(std::span(times))))
                return std::unexpected("タイムスタンプを読めない");

            bestMs = std::min(bestMs,
                              static_cast<double>(times[1] - times[0]) * 1000.0 / static_cast<double>(frequency));
        }

        const gpu::DebugRingContents debugOutput = ring.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("{}: GPU の FX_ASSERT が {} 件", name, debugOutput.assertCount));

        sim::MultiresNest read = initial.nest;
        if (!gpu->Read(read))
            return std::unexpected("読み戻せない");

        if (sim::HashWholeNest(read) != sim::HashWholeNest(expected.nest)) {
            ReportFirstMismatch(expected.nest, read, name);
            return std::unexpected(std::format("{} で CPU と GPU が食い違う", name));
        }

        Log(Channel::Gpu, Level::Info, "{}: 一致(要約 {:016x})・要求を処理する段 {:.4f} ms({} 回の最小)", name,
            sim::HashWholeNest(read), bestMs, TIMING_RUNS);

        return bestMs;
    }

    std::expected<void, std::string> RunScenes(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                               const sim::BakedReactionTable& baked) {
        const auto timestamps = CreateTimestamps(device);
        if (!timestamps)
            return std::unexpected(timestamps.error());

        const Materials full = MakeMaterials(baked);
        const Materials packed = MakeMaterials(baked, sim::DEFAULT_PACKING_PERCENT);
        struct Scene {
            const Materials* materials;
            bool placed;
            int32_t level;
            std::string_view name;
        };
        const Scene scenes[] = {
            {&full, false, 1, "SAME・1 段"},
            {&full, true, 1, "置かれ方・1 段・詰まり 100 %"},
            {&full, true, 2, "置かれ方・2 段の鎖・詰まり 100 %"},
            {&packed, true, 1, "置かれ方・1 段・詰まり 60 %"},
            {&packed, true, 2, "置かれ方・2 段の鎖・詰まり 60 %"},
        };
        for (const Scene& scene : scenes) {
            const auto ran = RunScene(device, queue, ring, *timestamps, *scene.materials, scene.placed, scene.level,
                                      scene.name);
            if (!ran)
                return std::unexpected(ran.error());
        }

        // 場面の確かめ: 置かれ方の世界は SAME の世界と違い、詰まり 60 % は 100 % と違う(配る所が本当に動いている)
        const uint64_t same = sim::HashWholeNest(MakeWorld(full, false).nest);
        const uint64_t placedFull = sim::HashWholeNest(MakeWorld(full, true).nest);
        const uint64_t placedPacked = sim::HashWholeNest(MakeWorld(packed, true).nest);
        if (same == placedFull || placedFull == placedPacked)
            return std::unexpected("場面の確かめ: 置かれ方・詰まりで世界が変わらない");

        return {};
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_tree_refine_placement_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_tree_refine_placement_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const auto baked = sim::BakeReactionTable(sim::MakeSpeciesChangedTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!baked || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_tree_refine_placement_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error,
                "gpu_tree_refine_placement_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        if (auto ran = RunScenes(device->Get(), *queue, *ring, *baked); !ran) {
            Log(Channel::Gpu, Level::Error, "gpu_tree_refine_placement_test: FAILED ({})", ran.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_tree_refine_placement_test"))
            return 1;

        Log(Channel::Gpu, Level::Info,
            "gpu_tree_refine_placement_test: OK(置かれ方の 1 段・2 段の鎖・詰まり 100 % と 60 %・SAME で GPU と CPU "
            "がビット一致)");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
