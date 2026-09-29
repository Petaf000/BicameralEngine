// gpu_fixed_test.cpp — 整数の数学ライブラリの自己テストを GPU で走らせ、CPU と 1 値ずつ比べる(T-0013、D-307)。
// shaders/sim/fixed_selftest.hlsl を 65536 スレッドで走らせ、同じ関数(shaders/common/fixed_selftest.hlsli)を
// CPU で呼んだ結果とビット単位で比べる。要約(digest)は tests/fixed_test.cpp と同じ作り方なので、表示どうしも比べられる。
// Debug のビルドではシェーダーが -Od(最適化なし)なので、debug と release の両方で走らせて最適化の影響も見る(04「未確認」)。
// 引数は gpu_test_options.h。
#include "common/fixed_selftest.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"

using namespace bicameral;
using namespace bicameral::fx;
using Microsoft::WRL::ComPtr;

namespace {

    constexpr uint32_t CASE_COUNT = 65536;      // tests/fixed_test.cpp の FIXED_SELF_TEST_CASES と同じ
    constexpr uint32_t THREADS_PER_GROUP = 64;  // fixed_selftest.hlsl の numthreads
    constexpr size_t VALUE_COUNT = size_t{CASE_COUNT} * FX_SELF_TEST_OUTPUT_COUNT;
    constexpr int MAX_REPORTED_MISMATCHES = 10;

    // debug のビルドではシェーダーの FX_ASSERT がデバッグのリングに書く(T-0003)。assert が 1 件でもあれば失敗
    constexpr gpu::RootSignatureLayout ROOT_LAYOUT{.uavCount = 1, .debugRing = true};

    using GpuValues = std::expected<std::vector<uint64_t>, std::string>;

    // --- GPU で走らせる ---

    struct SelfTestPipeline {
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12PipelineState> pipeline;
        ComPtr<ID3D12Resource> output;
        ComPtr<ID3D12Resource> readback;
    };

    std::expected<SelfTestPipeline, std::string> CreatePipeline(ID3D12Device5* device) {
        const auto bytecode = gpu::LoadShader("sim/fixed_selftest.cso");
        if (!bytecode) return std::unexpected(bytecode.error());
        SelfTestPipeline result;
        result.rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!result.rootSignature) return std::unexpected("ルート署名を作れない");
        result.pipeline = gpu::CreateComputePipeline(device, result.rootSignature.Get(), *bytecode);
        result.output = gpu::CreateBuffer(device, VALUE_COUNT * sizeof(uint64_t), gpu::BufferKind::UnorderedAccess);
        result.readback = gpu::CreateBuffer(device, VALUE_COUNT * sizeof(uint64_t), gpu::BufferKind::Readback);
        if (!result.pipeline || !result.output || !result.readback) {
            return std::unexpected("パイプラインかバッファを作れない");
        }
        return result;
    }

    GpuValues RunOnGpu(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType) {
        auto queue = gpu::ImmediateQueue::Create(device, queueType);
        if (!queue) return std::unexpected(queue.error());
        const auto pipeline = CreatePipeline(device);
        if (!pipeline) return std::unexpected(pipeline.error());
        const auto debugRing = gpu::DebugRing::Create(device);
        if (!debugRing) return std::unexpected(debugRing.error());

        ID3D12GraphicsCommandList10* list = queue->Begin();
        if (list == nullptr) return std::unexpected("コマンドリストを始められない");
        debugRing->RecordBegin(list);
        list->SetComputeRootSignature(pipeline->rootSignature.Get());
        list->SetPipelineState(pipeline->pipeline.Get());
        list->SetComputeRootUnorderedAccessView(0, pipeline->output->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing->GpuAddress());
        list->Dispatch(CASE_COUNT / THREADS_PER_GROUP, 1, 1);
        gpu::RecordCopyToReadback(list, pipeline->output.Get(), pipeline->readback.Get());
        debugRing->RecordReadbackAndReset(list);
        if (!queue->ExecuteAndWait()) return std::unexpected("GPU での実行に失敗");
        const gpu::DebugRingContents debugOutput = debugRing->Drain();
        if (debugOutput.assertCount > 0) {
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));
        }

        std::vector<uint64_t> values(VALUE_COUNT);
        if (!gpu::ReadBuffer(pipeline->readback.Get(), std::as_writable_bytes(std::span(values)))) {
            return std::unexpected("読み戻せない");
        }
        return values;
    }

    // --- CPU と比べる ---

    struct Comparison {
        uint64_t cpuDigest = 0;
        uint64_t gpuDigest = 0;
        int mismatchCount = 0;
    };

    // 1 つの case の値を CPU で計算し直して比べる。最初の数件の食い違いは入力と両方の値を表示する
    // (どの関数がずれたかは値の番号で分かる: fixed_selftest.hlsli)
    void CompareCase(uint32_t caseIndex, std::span<const uint64_t> gpuCaseValues, Comparison& comparison) {
        const FxU128 inputs = FxSelfTestInputs(caseIndex);
        const FxSelfTestOutput output = FxSelfTestCase(inputs.hi, inputs.lo);
        for (uint32_t valueIndex = 0; valueIndex < FX_SELF_TEST_OUTPUT_COUNT; ++valueIndex) {
            const uint64_t cpuValue = output.values[valueIndex];
            const uint64_t gpuValue = gpuCaseValues[valueIndex];
            comparison.cpuDigest = FxHashCombine(comparison.cpuDigest, cpuValue);
            comparison.gpuDigest = FxHashCombine(comparison.gpuDigest, gpuValue);
            if (cpuValue == gpuValue) continue;
            if (comparison.mismatchCount++ >= MAX_REPORTED_MISMATCHES) continue;
            Log(Channel::Gpu, Level::Error, "食い違い: case {} value[{}]: a={:016x} b={:016x} cpu={:016x} gpu={:016x}",
                caseIndex, valueIndex, inputs.hi, inputs.lo, cpuValue, gpuValue);
        }
    }

    // 食い違った値の数を返す。要約の作り方は tests/fixed_test.cpp の TestSelfTestDigest と同じ
    int CompareWithCpu(std::span<const uint64_t> gpuValues) {
        Comparison comparison;
        for (uint32_t caseIndex = 0; caseIndex < CASE_COUNT; ++caseIndex) {
            CompareCase(caseIndex,
                        gpuValues.subspan(size_t{caseIndex} * FX_SELF_TEST_OUTPUT_COUNT, FX_SELF_TEST_OUTPUT_COUNT),
                        comparison);
        }
        Log(Channel::Gpu, Level::Info, "selftest: {} cases, cpu digest {:016x}, gpu digest {:016x}, 食い違い {}",
            CASE_COUNT, comparison.cpuDigest, comparison.gpuDigest, comparison.mismatchCount);
        return comparison.mismatchCount;
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_fixed_test [--warp] [--queue direct|compute]");
            return 2;
        }
        Log(Channel::Gpu, Level::Info, "gpu_fixed_test: adapter {}, queue {}", gpu::AdapterKindName(options->adapter),
            test::QueueTypeName(options->queueType));
        const auto device = gpu::Device::Create(options->adapter);
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_fixed_test: FAILED ({})", device.error());
            return 1;
        }
        const GpuValues gpuValues = RunOnGpu(device->Get(), options->queueType);
        if (!gpuValues) {
            Log(Channel::Gpu, Level::Error, "gpu_fixed_test: FAILED ({})", gpuValues.error());
            return 1;
        }
        if (CompareWithCpu(*gpuValues) != 0) {
            Log(Channel::Gpu, Level::Error, "gpu_fixed_test: FAILED(GPU と CPU が食い違う)");
            return 1;
        }
        if (!test::PassesValidation(*device, "gpu_fixed_test")) return 1;
        Log(Channel::Gpu, Level::Info, "gpu_fixed_test: OK(GPU と CPU がビット一致)");
        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();  // ログを閉じる
    return exitCode;
}
