// gpu_work_graph_test.cpp — Work Graphs が動くかを一番小さなグラフで確かめる(T-0013)。
// shaders/sim/work_graph_probe.hlsl の Root → Leaf を CPU のレコード 1 つで起動し、Leaf が 64bit の atomic で足した
// 合計と回数を期待値と比べる。キュー(direct / compute)とアダプタ(ハードウェア / WARP)を引数で変えて ctest に登録する。
// 確かめること: 06「未確認」(compute キューで DispatchGraph)・16 §4(WARP で Work Graphs)・raw バッファへの 64bit atomic。
// 引数は gpu_test_options.h。
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu/work_graph.h"
#include "gpu_test_options.h"

using namespace bicameral;

namespace {

    // work_graph_probe.hlsl と同じ値
    constexpr uint64_t ROOT_THREAD_COUNT = uint64_t{16} * 64;  // ROOT_GROUP_COUNT × numthreads
    constexpr uint32_t VALUE_SHIFT = 20;
    constexpr uint32_t MULTIPLIER = 3;
    constexpr uint64_t RESULT_BYTES = 16;  // [0] 合計、[8] 回数

    struct RootRecord {
        uint32_t multiplier;
    };

    struct Results {
        uint64_t sum = 0;
        uint64_t leafCount = 0;
    };

    // --- GPU で走らせる ---

    expected<Results, std::string> RunGraph(ID3D12Device5* device, gpu::ImmediateQueue& queue) {
        const auto library = gpu::LoadShader("sim/work_graph_probe.cso");
        if (!library)
            return unexpected(library.error());

        const ComPtr<ID3D12RootSignature> rootSignature = gpu::CreateRootSignature(device, {.uavCount = 1});
        if (!rootSignature)
            return unexpected("ルート署名を作れない");

        const auto graph = gpu::WorkGraph::Create(device, rootSignature.Get(), *library, L"Probe");
        if (!graph)
            return unexpected(graph.error());

        const uint32_t entrypoint = graph->EntrypointIndex(L"Root");
        if (entrypoint == UINT32_MAX)
            return unexpected("入口のノード Root が無い");

        const ComPtr<ID3D12Resource> results =
            gpu::CreateBuffer(device, RESULT_BYTES, gpu::BufferKind::UnorderedAccess);
        const ComPtr<ID3D12Resource> readback = gpu::CreateBuffer(device, RESULT_BYTES, gpu::BufferKind::Readback);
        if (!results || !readback)
            return unexpected("バッファを作れない");

        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return unexpected("コマンドリストを始められない");

        list->SetComputeRootSignature(rootSignature.Get());
        graph->SetProgram(list, true);
        list->SetComputeRootUnorderedAccessView(0, results->GetGPUVirtualAddress());
        const RootRecord record{.multiplier = MULTIPLIER};
        gpu::WorkGraph::DispatchFromCpu(list, entrypoint, &record, 1, sizeof(record));
        gpu::RecordCopyToReadback(list, results.Get(), readback.Get());
        if (!queue.ExecuteAndWait())
            return unexpected("GPU での実行に失敗");

        Results values;
        if (!gpu::ReadBuffer(readback.Get(), std::as_writable_bytes(span(&values, 1))))
            return unexpected("読み戻せない");

        return values;
    }

    int Run(span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::WorkGraph, Level::Error, "使い方: gpu_work_graph_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::WorkGraph, Level::Info, "gpu_work_graph_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));

        const auto device = gpu::Device::Create(options->adapter);
        if (!device) {
            Log(Channel::WorkGraph, Level::Error, "gpu_work_graph_test: FAILED ({})", device.error());
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        const auto results = queue ? RunGraph(device->Get(), *queue) : unexpected(queue.error());
        if (!results) {
            Log(Channel::WorkGraph, Level::Error, "gpu_work_graph_test: FAILED ({})", results.error());
            return 1;
        }

        // Σ_{i=1..N} i × MULTIPLIER を VALUE_SHIFT だけずらしたもの(2^32 を超える)
        const uint64_t expectedSum = (ROOT_THREAD_COUNT * (ROOT_THREAD_COUNT + 1) / 2 * MULTIPLIER) << VALUE_SHIFT;
        Log(Channel::WorkGraph, Level::Info, "Leaf の回数 {}(期待 {})、合計 {:#x}(期待 {:#x})", results->leafCount,
            ROOT_THREAD_COUNT, results->sum, expectedSum);

        if (results->leafCount != ROOT_THREAD_COUNT || results->sum != expectedSum) {
            Log(Channel::WorkGraph, Level::Error, "gpu_work_graph_test: FAILED(結果が期待と違う)");
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_work_graph_test"))
            return 1;

        Log(Channel::WorkGraph, Level::Info, "gpu_work_graph_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();  // ログを閉じる

    return exitCode;
}
