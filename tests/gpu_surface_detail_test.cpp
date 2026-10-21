// gpu_surface_detail_test.cpp — 表面の細かい形の整数の関数を GPU の Compute で走らせ、CPU と 1 値ずつ比べ、費用を測る(研究 R-T1・T-0879)。
// shaders/sim/surface_detail.hlsl の 2 つの入口(Contacts・Openness)を、tests/surface_detail_scene.h の場面で、差分の 3 つの持ち方
// (差分なし・一覧・格子)ごとに走らせる。結果は tests/surface_detail_test.cpp の CPU と同じ関数(shaders/common/surface_detail.hlsli)の値と
// ビット単位で比べる。時間はタイムスタンプ(暖機の後 5 回の中央値)。引数は gpu_test_options.h。
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "sim/surface_detail.h"
#include "surface_detail_scene.h"

using namespace bicameral;
using namespace bicameral::surface;

namespace {

    static_assert(sizeof(SdConfig) == 64);
    static_assert(sizeof(SdCarve) == 32);
    static_assert(sizeof(SdProbePoint) == 16);
    static_assert(sizeof(SdContact) == 16);
    static_assert(sizeof(SdColumnOpenness) == 64);

    constexpr uint32_t SRV_COUNT = 7;
    // debug のビルドではシェーダーの FX_ASSERT がデバッグのリングに書く(T-0003)
    constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
        .uavCount = 2, .rootConstantCount = 4, .debugRing = true, .srvCount = SRV_COUNT};
    constexpr uint32_t THREADS_PER_GROUP = 64;
    constexpr int REPEAT_COUNT = 5;

    template <typename T>
    std::span<const std::byte> Bytes(std::span<const T> values) {
        return std::as_bytes(values);
    }

    struct Pipelines {
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12PipelineState> contacts;
        ComPtr<ID3D12PipelineState> openness;
    };

    std::expected<Pipelines, std::string> CreatePipelines(ID3D12Device5* device) {
        Pipelines result;
        result.rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        const auto contacts = gpu::LoadShader("sim/surface_detail_contacts.cso");
        const auto openness = gpu::LoadShader("sim/surface_detail_openness.cso");
        if (!result.rootSignature || !contacts || !openness)
            return std::unexpected("ルート署名かシェーダーを作れない");

        result.contacts = gpu::CreateComputePipeline(device, result.rootSignature.Get(), *contacts);
        result.openness = gpu::CreateComputePipeline(device, result.rootSignature.Get(), *openness);
        if (!result.contacts || !result.openness)
            return std::unexpected("パイプラインを作れない");

        return result;
    }

    // --- 場面を GPU に置く ---
    struct SceneBuffers {
        std::vector<gpu::ResidentBuffer> inputs;  // t0〜t6
        ComPtr<ID3D12Resource> contacts;
        ComPtr<ID3D12Resource> openness;
        ComPtr<ID3D12Resource> contactsReadback;
        ComPtr<ID3D12Resource> opennessReadback;
        ComPtr<ID3D12QueryHeap> queryHeap;
        ComPtr<ID3D12Resource> timestampReadback;
    };

    std::expected<SceneBuffers, std::string> CreateSceneBuffers(ID3D12Device5* device,
                                                                const sim::SurfaceDetailScene& scene,
                                                                std::span<const SdProbePoint> points) {
        SceneBuffers buffers;
        const SdConfig config = scene.config;
        const std::span<const SdConfig> configSpan(&config, 1);
        const std::span<const std::byte> inputs[SRV_COUNT] = {Bytes(std::span(scene.columnHeights)),
                                                              Bytes(std::span(scene.carveBegin)),
                                                              Bytes(std::span(scene.registered)),
                                                              Bytes(std::span(scene.gridIndex)),
                                                              Bytes(std::span(scene.gridNodes)),
                                                              Bytes(configSpan),
                                                              Bytes(points)};
        for (const auto& bytes : inputs) {
            buffers.inputs.push_back(gpu::CreateResidentBuffer(device, bytes));
            if (!buffers.inputs.back().Valid())
                return std::unexpected("入力のバッファを作れない");
        }

        const uint64_t contactBytes = points.size() * sizeof(SdContact);
        const uint64_t opennessBytes = uint64_t{scene.ColumnCount()} * sizeof(SdColumnOpenness);
        buffers.contacts = gpu::CreateBuffer(device, contactBytes, gpu::BufferKind::UnorderedAccess);
        buffers.openness = gpu::CreateBuffer(device, opennessBytes, gpu::BufferKind::UnorderedAccess);
        buffers.contactsReadback = gpu::CreateBuffer(device, contactBytes, gpu::BufferKind::Readback);
        buffers.opennessReadback = gpu::CreateBuffer(device, opennessBytes, gpu::BufferKind::Readback);
        buffers.timestampReadback = gpu::CreateBuffer(device, 4 * sizeof(uint64_t), gpu::BufferKind::Readback);
        const D3D12_QUERY_HEAP_DESC heapDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 4};
        if (!buffers.contacts || !buffers.openness || !buffers.contactsReadback || !buffers.opennessReadback ||
            !buffers.timestampReadback ||
            FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&buffers.queryHeap)))) {
            return std::unexpected("出力のバッファかクエリのヒープを作れない");
        }

        return buffers;
    }

    // --- 1 回走らせる(当たり → 開き具合。それぞれの時間を ms で)---
    struct RunTimes {
        double contactsMs = 0;
        double opennessMs = 0;
    };

    std::expected<RunTimes, std::string> RunOnce(gpu::ImmediateQueue& queue, const Pipelines& pipelines,
                                                 const SceneBuffers& buffers, const gpu::DebugRing& debugRing,
                                                 uint32_t mode, uint32_t pointCount, uint32_t columnCount, bool upload,
                                                 double ticksPerMillisecond) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        debugRing.RecordBegin(list);
        if (upload)
            gpu::RecordResidentUploads(list, buffers.inputs);

        list->SetComputeRootSignature(pipelines.rootSignature.Get());
        list->SetComputeRootUnorderedAccessView(0, buffers.contacts->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1, buffers.openness->GetGPUVirtualAddress());
        const uint32_t constants[4] = {mode, pointCount, 0, (uint32_t)test::OPENNESS_BOTTOM_UM};
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing.GpuAddress());
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), 4, constants, 0);
        for (uint32_t i = 0; i < SRV_COUNT; ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i),
                                                   buffers.inputs[i].buffer->GetGPUVirtualAddress());

        list->EndQuery(buffers.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        list->SetPipelineState(pipelines.contacts.Get());
        list->Dispatch((pointCount + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP, 1, 1);
        list->EndQuery(buffers.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->SetPipelineState(pipelines.openness.Get());
        list->Dispatch(columnCount, 1, 1);
        list->EndQuery(buffers.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
        list->ResolveQueryData(buffers.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3,
                               buffers.timestampReadback.Get(), 0);
        gpu::RecordCopyToReadback(list, buffers.contacts.Get(), buffers.contactsReadback.Get());
        gpu::RecordCopyToReadback(list, buffers.openness.Get(), buffers.opennessReadback.Get());
        debugRing.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        if (debugRing.Drain().assertCount > 0)
            return std::unexpected("GPU の FX_ASSERT が出た");

        uint64_t timestamps[3] = {};
        if (!gpu::ReadBuffer(buffers.timestampReadback.Get(), std::as_writable_bytes(std::span(timestamps))))
            return std::unexpected("タイムスタンプを読み戻せない");

        return RunTimes{.contactsMs = (double)(timestamps[1] - timestamps[0]) / ticksPerMillisecond,
                        .opennessMs = (double)(timestamps[2] - timestamps[1]) / ticksPerMillisecond};
    }

    // --- CPU と比べる ---
    int Compare(const sim::SurfaceDetailScene& scene, std::span<const SdProbePoint> points, uint32_t mode,
                const SceneBuffers& buffers) {
        std::vector<SdContact> gpuContacts(points.size());
        std::vector<SdColumnOpenness> gpuOpenness(scene.ColumnCount());
        if (!gpu::ReadBuffer(buffers.contactsReadback.Get(), std::as_writable_bytes(std::span(gpuContacts))) ||
            !gpu::ReadBuffer(buffers.opennessReadback.Get(), std::as_writable_bytes(std::span(gpuOpenness)))) {
            return 1;
        }

        const SdSceneData data = scene.Data();
        int mismatches = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            const SdContact cpu = SdContactAt(data, scene.config, mode, points[i].xUm, points[i].zUm, points[i].yUm);
            if (std::memcmp(&cpu, &gpuContacts[i], sizeof(cpu)) == 0)
                continue;

            if (mismatches++ < 5)
                Log(Channel::Gpu, Level::Error, "当たりの食い違い: 点 {} cpu {} gpu {}", i, cpu.penetrationUm,
                    gpuContacts[i].penetrationUm);
        }

        for (uint32_t column = 0; column < scene.ColumnCount(); ++column) {
            const SdColumnOpenness cpu = SdColumnOpennessAt(data, scene.config, mode, column % scene.config.columnsX,
                                                            column / scene.config.columnsX, test::OPENNESS_BOTTOM_UM);
            if (std::memcmp(&cpu, &gpuOpenness[column], sizeof(cpu)) == 0)
                continue;

            if (mismatches++ < 10)
                Log(Channel::Gpu, Level::Error, "開き具合の食い違い: 柱 {} fill cpu {} gpu {}", column, cpu.fillQ16[1],
                    gpuOpenness[column].fillQ16[1]);
        }

        return mismatches;
    }

    double Median(std::vector<double> values) {
        rng::sort(values);
        return values[values.size() / 2];
    }

    const char* ModeName(uint32_t mode) {
        return mode == SD_MODE_LIST ? "一覧(候補 B)" : (mode == SD_MODE_GRID ? "格子(候補 A)" : "差分なし");
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_surface_detail_test [--warp] [--queue direct|compute] [--no-gbv]");
            return 2;
        }

        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_surface_detail_test: FAILED ({})", device.error());
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        const auto pipelines = CreatePipelines(device->Get());
        const sim::SurfaceDetailScene scene = test::MakeBusyScene();
        const std::vector<SdProbePoint> points = test::MakeBodyPoints();
        const auto buffers = CreateSceneBuffers(device->Get(), scene, points);
        const auto debugRing = gpu::DebugRing::Create(device->Get());
        if (!queue || !pipelines || !buffers || !debugRing) {
            Log(Channel::Gpu, Level::Error, "gpu_surface_detail_test: FAILED(準備に失敗)");
            return 1;
        }

        uint64_t frequency = 0;
        if (FAILED(queue->Native()->GetTimestampFrequency(&frequency)))
            return 1;

        const double ticksPerMillisecond = (double)frequency / 1000.0;
        int failures = 0;
        bool uploaded = false;
        for (const uint32_t mode : {SD_MODE_NATURAL, SD_MODE_LIST, SD_MODE_GRID}) {
            std::vector<double> contactTimes;
            std::vector<double> opennessTimes;
            for (int repeat = 0; repeat <= REPEAT_COUNT; ++repeat) {
                const auto times = RunOnce(*queue, *pipelines, *buffers, *debugRing, mode, (uint32_t)points.size(),
                                           scene.ColumnCount(), !uploaded, ticksPerMillisecond);
                uploaded = true;
                if (!times) {
                    Log(Channel::Gpu, Level::Error, "gpu_surface_detail_test: FAILED ({})", times.error());
                    return 1;
                }

                if (repeat == 0)
                    continue;  // 暖機

                contactTimes.push_back(times->contactsMs);
                opennessTimes.push_back(times->opennessMs);
            }

            const int mismatches = Compare(scene, points, mode, *buffers);
            failures += mismatches;
            Log(Channel::Gpu, Level::Info,
                "{}: 当たり {} 点 {:.4f} ms・開き具合 {} 柱 × 4 セル {:.4f} ms(中央値)・CPU との食い違い {}",
                ModeName(mode), points.size(), Median(contactTimes), scene.ColumnCount(), Median(opennessTimes),
                mismatches);
        }

        if (failures != 0) {
            Log(Channel::Gpu, Level::Error, "gpu_surface_detail_test: FAILED(GPU と CPU が食い違う)");
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_surface_detail_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_surface_detail_test: OK(GPU と CPU がビット一致)");
        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();
    return exitCode;
}
