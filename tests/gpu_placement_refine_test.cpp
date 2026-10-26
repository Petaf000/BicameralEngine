// gpu_placement_refine_test.cpp — 細かくする時の「置かれ方で配る」を GPU で走らせ、CPU リファレンスと 1 値ずつ比べる(T-1159。D-664・20 §10.5)。
// shaders/sim/placement_refine_check.hlsl(回しは shaders/sim/placement_refine.hlsli・式は shaders/common/state_field.hlsli)を
// 乱数の親のセルと端の場面(気体だけ・詰まり切った・据わる固体・物・半分の砂・凝縮相がごくわずか・傾きの頭打ち)で走らせ、
// sim::PlaceRefineCell・PlaceChildWeights(columns = 2・depth = 2)と、子の番号を写し替えて(sim::PlaceIndexOfMrChild)ビット単位で比べる。
// 子の体積は 3 通り(32bit を超える体積も。ルート定数は上下の 32bit で渡す)。引数は gpu_test_options.h。
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "sim/placement_refine.h"

using namespace bicameral;
using namespace bicameral::sim;
using namespace bicameral::state_field;

namespace {

    constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
        .uavCount = 1, .rootConstantCount = 4, .debugRing = true, .srvCount = 2};
    constexpr uint32_t CELLS_PER_GROUP = 8;  // placement_refine_check.hlsl の CELLS_PER_GROUP
    constexpr uint32_t RANDOM_CELLS = 2000;
    constexpr int MAX_REPORTED_MISMATCHES = 10;

    // --- 物質: 空気・水・砂(60 %)・泥(50 %)・石(据わる)。tests/placement_refine_test.cpp と同じ ---
    const std::vector<PlaceMaterial> MATERIALS = {
        {SF_SETTLE_GAS, 1, 0},       {SF_SETTLE_LIQUID, 1000, 0},   {SF_SETTLE_GRAIN, 2650, 60},
        {SF_SETTLE_GRAIN, 1500, 50}, {SF_SETTLE_ANCHORED, 2700, 0},
    };

    // splitmix64(決まった種で同じ場面)
    class Random {
    public:
        explicit Random(uint64_t seed) : m_state(seed) {}

        uint64_t Next(uint64_t bound) {
            m_state += 0x9E3779B97F4A7C15ull;
            uint64_t value = m_state;
            value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
            value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;

            return (value ^ (value >> 31)) % bound;
        }

    private:
        uint64_t m_state;
    };

    // 器の体積 % を量(mg)に(密度 kg/m³ = 1e-3 mg/mm³)
    int64_t MassOfPercent(uint64_t capacityMm3, uint32_t percent, uint32_t densityKgPerM3) {
        return (int64_t)((capacityMm3 / 100 * percent / 1000) * densityKgPerM3);
    }

    // --- 場面: 乱数のセル + 端の場面 ---
    std::vector<PlaceCell> MakeCells(uint64_t childVolumeMm3, uint64_t seed) {
        const uint64_t capacity = childVolumeMm3 * 8;
        Random random(seed);
        std::vector<PlaceCell> cells;
        for (uint32_t i = 0; i < RANDOM_CELLS; ++i) {
            PlaceCell cell;
            cell.massMg.assign(MATERIALS.size(), 0);
            cell.massMg[0] = MassOfPercent(capacity, (uint32_t)random.Next(120), 1);
            for (uint32_t m = 1; m < 4; ++m)
                cell.massMg[m] = random.Next(3) == 0
                                     ? 0
                                     : MassOfPercent(capacity, (uint32_t)random.Next(40), MATERIALS[m].densityKgPerM3);

            cell.massMg[4] = random.Next(12) == 0 ? MassOfPercent(capacity, (uint32_t)random.Next(30), 2700) : 0;
            cell.blocked = random.Next(15) == 0;
            cell.slopeXQ16 = (int64_t)random.Next(400000) - 200000;
            cell.slopeZQ16 = random.Next(4) == 0 ? 0 : (int64_t)random.Next(400000) - 200000;
            cells.push_back(cell);
        }

        // --- 端の場面 ---
        const auto make = [&](std::vector<int64_t> massMg, int64_t slopeX, int64_t slopeZ, bool blocked) {
            cells.push_back(
                {.massMg = std::move(massMg), .slopeXQ16 = slopeX, .slopeZQ16 = slopeZ, .blocked = blocked});
        };
        const int64_t fullSand = MassOfPercent(capacity, 70, 2650);
        const int64_t halfSand = MassOfPercent(capacity, 30, 2650);
        make({800000, 0, 0, 0, 0}, 0, 0, false);                                      // 気体だけ
        make({8000, 0, fullSand, 0, 0}, 0, 0, false);                                 // 詰まり切った
        make({8000, 8000000, 0, 0, MassOfPercent(capacity, 50, 2700)}, 0, 0, false);  // 据わる固体
        make({8000, 8000000, 0, 0, 0}, 0, 0, true);                                   // 物
        make({800000, 0, halfSand, 0, 0}, 0, 0, false);                               // 半分の砂
        make({800000, 0, halfSand, 0, 0}, 1 << 20, -(1 << 20), false);                // 傾きの頭打ち
        make({800000, 0, 3, 0, 0}, 40000, 40000, false);                              // 凝縮相がごくわずか(重みの和 0)
        make({0, MassOfPercent(capacity, 20, 1000), halfSand, 0, 0}, -30000, 20000, false);  // 気体なし
        make({7, 1, 1, 1, 0}, 5, -5, false);                                                 // 小さい値の端数

        return cells;
    }

    // --- GPU で走らせる ---
    struct Buffers {
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12PipelineState> pipeline;
        std::vector<gpu::ResidentBuffer> inputs;  // t0・t1
        ComPtr<ID3D12Resource> output;
        ComPtr<ID3D12Resource> readback;
    };

    std::vector<uint64_t> PackParents(std::span<const PlaceCell> cells) {
        std::vector<uint64_t> words;
        for (const PlaceCell& cell : cells) {
            words.push_back(cell.blocked ? 1 : 0);
            words.push_back((uint64_t)cell.slopeXQ16);
            words.push_back((uint64_t)cell.slopeZQ16);
            for (const int64_t mass : cell.massMg)
                words.push_back((uint64_t)mass);
        }

        return words;
    }

    std::expected<Buffers, std::string> CreateBuffers(ID3D12Device5* device, std::span<const PlaceCell> cells) {
        Buffers buffers;
        const auto bytecode = gpu::LoadShader("sim/placement_refine_check.cso");
        if (!bytecode)
            return std::unexpected(bytecode.error());

        buffers.rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!buffers.rootSignature)
            return std::unexpected("ルート署名を作れない");

        buffers.pipeline = gpu::CreateComputePipeline(device, buffers.rootSignature.Get(), *bytecode);
        std::vector<uint32_t> materials;
        for (const PlaceMaterial& material : MATERIALS)
            materials.insert(materials.end(), {material.kind, material.densityKgPerM3, material.packedFractionPct});

        const std::vector<uint64_t> parents = PackParents(cells);
        buffers.inputs.push_back(gpu::CreateResidentBuffer(device, std::as_bytes(std::span(materials))));
        buffers.inputs.push_back(gpu::CreateResidentBuffer(device, std::as_bytes(std::span(parents))));
        const uint64_t outputBytes = cells.size() * 8 * (MATERIALS.size() + 3) * sizeof(uint64_t);
        buffers.output = gpu::CreateBuffer(device, outputBytes, gpu::BufferKind::UnorderedAccess);
        buffers.readback = gpu::CreateBuffer(device, outputBytes, gpu::BufferKind::Readback);
        if (!buffers.pipeline || !buffers.inputs[0].Valid() || !buffers.inputs[1].Valid() || !buffers.output ||
            !buffers.readback)
            return std::unexpected("パイプラインかバッファを作れない");

        return buffers;
    }

    std::expected<std::vector<uint64_t>, std::string> RunOnGpu(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType,
                                                               std::span<const PlaceCell> cells,
                                                               uint64_t childVolumeMm3) {
        auto queue = gpu::ImmediateQueue::Create(device, queueType);
        if (!queue)
            return std::unexpected(queue.error());

        const auto buffers = CreateBuffers(device, cells);
        if (!buffers)
            return std::unexpected(buffers.error());

        const auto debugRing = gpu::DebugRing::Create(device);
        if (!debugRing)
            return std::unexpected(debugRing.error());

        ID3D12GraphicsCommandList10* list = queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        debugRing->RecordBegin(list);
        gpu::RecordResidentUploads(list, buffers->inputs);
        list->SetComputeRootSignature(buffers->rootSignature.Get());
        list->SetPipelineState(buffers->pipeline.Get());
        list->SetComputeRootUnorderedAccessView(0, buffers->output->GetGPUVirtualAddress());
        const uint32_t constants[4] = {(uint32_t)MATERIALS.size(), (uint32_t)cells.size(),
                                       (uint32_t)(childVolumeMm3 & 0xFFFFFFFFu), (uint32_t)(childVolumeMm3 >> 32)};
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), 4, constants, 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing->GpuAddress());
        for (uint32_t i = 0; i < 2; ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i),
                                                   buffers->inputs[i].buffer->GetGPUVirtualAddress());

        list->Dispatch(((uint32_t)cells.size() + CELLS_PER_GROUP - 1) / CELLS_PER_GROUP, 1, 1);
        gpu::RecordCopyToReadback(list, buffers->output.Get(), buffers->readback.Get());
        debugRing->RecordReadbackAndReset(list);
        if (!queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        if (debugRing->Drain().assertCount > 0)
            return std::unexpected("GPU の FX_ASSERT が出た");

        std::vector<uint64_t> values(cells.size() * 8 * (MATERIALS.size() + 3));
        if (!gpu::ReadBuffer(buffers->readback.Get(), std::as_writable_bytes(std::span(values))))
            return std::unexpected("読み戻せない");

        return values;
    }

    // --- CPU と比べる(子 j の値の並び: 寄せたか・凝縮相の量・空きの体積・物質ごとの量)---
    struct Comparison {
        int mismatches = 0;
        int settledCells = 0;
    };

    void CompareCell(const PlaceCell& cell, uint32_t cellIndex, uint64_t childVolumeMm3, std::span<const uint64_t> gpu,
                     Comparison& comparison) {
        const PlacedChildren placed = PlaceRefineCell(MATERIALS, cell, 2, 2, childVolumeMm3);
        const PlaceWeights weights = PlaceChildWeights(MATERIALS, placed, childVolumeMm3);
        const size_t stride = MATERIALS.size() + 3;
        comparison.settledCells += placed.settled ? 1 : 0;
        for (uint32_t j = 0; j < 8; ++j) {
            const uint32_t index = PlaceIndexOfMrChild(placed, j);
            std::vector<uint64_t> cpu = {placed.settled ? 1u : 0u, weights.condensed[index], weights.open[index]};
            for (const int64_t mass : placed.massMg[index])
                cpu.push_back((uint64_t)mass);

            const std::span<const uint64_t> child = gpu.subspan(((size_t)cellIndex * 8 + j) * stride, stride);
            for (size_t value = 0; value < stride; ++value) {
                if (cpu[value] == child[value])
                    continue;

                if (comparison.mismatches++ < MAX_REPORTED_MISMATCHES)
                    Log(Channel::Gpu, Level::Error, "食い違い: 子の体積 {} セル {} 子 {} 値 {}: cpu {} gpu {}",
                        childVolumeMm3, cellIndex, j, value, (int64_t)cpu[value], (int64_t)child[value]);
            }
        }
    }

    bool RunVolume(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType, uint64_t childVolumeMm3, uint64_t seed) {
        const std::vector<PlaceCell> cells = MakeCells(childVolumeMm3, seed);
        const auto gpuValues = RunOnGpu(device, queueType, cells, childVolumeMm3);
        if (!gpuValues) {
            Log(Channel::Gpu, Level::Error, "gpu_placement_refine_test: FAILED ({})", gpuValues.error());
            return false;
        }

        Comparison comparison;
        for (uint32_t i = 0; i < cells.size(); ++i)
            CompareCell(cells[i], i, childVolumeMm3, *gpuValues, comparison);

        Log(Channel::Gpu, Level::Info, "子の体積 {} mm³: セル {}(寄せた {})・食い違い {}", childVolumeMm3, cells.size(),
            comparison.settledCells, comparison.mismatches);

        return comparison.mismatches == 0 && comparison.settledCells > 0;
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_placement_refine_test [--warp] [--queue direct|compute]");
            return 2;
        }

        const auto device = gpu::Device::Create(options->adapter);
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_placement_refine_test: FAILED ({})", device.error());
            return 1;
        }

        // 子の体積: 0.25 m・1 m・4 m の立方体(4 m は 32bit を超える)
        bool passed = true;
        passed &= RunVolume(device->Get(), options->queueType, 250ull * 250 * 250, 11);
        passed &= RunVolume(device->Get(), options->queueType, 1000ull * 1000 * 1000, 23);
        passed &= RunVolume(device->Get(), options->queueType, 4000ull * 4000 * 4000, 37);
        if (!passed) {
            Log(Channel::Gpu, Level::Error, "gpu_placement_refine_test: FAILED(GPU と CPU が食い違う)");
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_placement_refine_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_placement_refine_test: OK(GPU と CPU がビット一致)");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();  // ログを閉じる

    return exitCode;
}
