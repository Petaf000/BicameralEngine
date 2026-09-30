// gpu_fixed_bench.cpp — 整数の数学ライブラリのルーチンの費用を GPU で測る(T-0010、docs/design/04-numerics-determinism.md §6)。
// shaders/bench/fixed_bench.hlsl を演算ごとにコンパイルした .cso(bin/shaders/bench/fixed_bench_<名前>.cso)を順に走らせ、
// タイムスタンプで時間を測る。1 回あたりの時間から「混ぜるだけ」(base32 / base64)の時間を引いたものを演算の費用とし、
// 浮動小数点の掛け算(fmul)の何回分かでも表す。結果は Markdown の表の行としてログに出す(docs/perf.md・04 §6 に貼る)。
//
// ctest には登録しない(時間がかかり、結果は機械しだいで合否が無い)。走らせ方: `job.py run -Preset release -Exe gpu_fixed_bench`。
// 引数は gpu_test_options.h(--warp・--queue)。WARP での時間は GPU の目安にならない。
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"

using namespace bicameral;

namespace {

    // --- 演算の一覧(shaders/CMakeLists.txt の BICAMERAL_BENCH_OPERATIONS と同じ順・同じ名前)---
    struct BenchOperation {
        const char* name;
        const char* description;
        int width;  // 32 / 64: 混ぜる処理(base32 / base64)の時間を引く。0: 浮動小数点(引かない)
    };

    constexpr BenchOperation BENCH_OPERATIONS[] = {
        {.name = "base32", .description = "混ぜるだけ(32bit)", .width = 32},
        {.name = "base64", .description = "混ぜるだけ(64bit)", .width = 64},
        {.name = "add32", .description = "32bit の足し算", .width = 32},
        {.name = "mul32", .description = "32bit の掛け算", .width = 32},
        {.name = "div32", .description = "32bit の割り算(除数が毎回変わる)", .width = 32},
        {.name = "add64", .description = "int64 の足し算", .width = 64},
        {.name = "mul64", .description = "int64 の掛け算(下位 64bit)", .width = 64},
        {.name = "div64", .description = "int64 の割り算(除数が毎回変わる)", .width = 64},
        {.name = "fadd", .description = "float の足し算", .width = 0},
        {.name = "fmul", .description = "float の掛け算", .width = 0},
        {.name = "fdiv", .description = "float の割り算(除数が毎回変わる)", .width = 0},
        {.name = "mulshift32", .description = "FxMulShiftS32", .width = 32},
        {.name = "mulshift64", .description = "FxMulShiftS64", .width = 64},
        {.name = "mulfull128", .description = "FxMulU64Full", .width = 64},
        {.name = "divs64", .description = "FxDivS64(除数が毎回変わる)", .width = 64},
        {.name = "divshift64", .description = "FxDivShiftS64", .width = 64},
        {.name = "div128", .description = "FxDivU128By64", .width = 64},
        {.name = "recip32", .description = "FxDivRecipU32(逆数は作ってある)", .width = 32},
        {.name = "recip64", .description = "FxDivRecipU64(逆数は作ってある)", .width = 64},
        {.name = "recips64", .description = "FxDivRecipS64(逆数は作ってある)", .width = 64},
        {.name = "makerecip64", .description = "FxMakeRecipU64(逆数を作る)", .width = 64},
        {.name = "sqrt32", .description = "FxSqrtU32", .width = 32},
        {.name = "sqrt64", .description = "FxSqrtU64", .width = 64},
        {.name = "exp2", .description = "FxExp2Q32", .width = 64},
        {.name = "log2", .description = "FxLog2U64", .width = 64},
        {.name = "exp", .description = "FxExpQ32", .width = 64},
        {.name = "ln", .description = "FxLnU64", .width = 64},
        {.name = "sincos", .description = "FxSinCosTurn32", .width = 32},
        {.name = "hash64", .description = "FxHash64", .width = 64},
    };

    constexpr uint32_t THREADS_PER_GROUP = 256;  // fixed_bench.hlsl の numthreads
    constexpr uint32_t GROUP_COUNT = 4096;       // 約 100 万スレッド(GPU を埋める)
    constexpr uint32_t THREAD_COUNT = THREADS_PER_GROUP * GROUP_COUNT;
    constexpr uint32_t UNROLL = 4;  // fixed_bench.hlsl の BENCH_UNROLL
    constexpr double TARGET_MILLISECONDS = 40.0;
    constexpr int REPEAT_COUNT = 5;  // 中央値を取る

    // --- 1 回の計測 ---

    struct BenchContext {
        ID3D12Device5* device = nullptr;
        gpu::ImmediateQueue* queue = nullptr;
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12Resource> results;
        ComPtr<ID3D12QueryHeap> queryHeap;
        ComPtr<ID3D12Resource> timestampReadback;
        double ticksPerMillisecond = 0;
    };

    expected<BenchContext, std::string> CreateContext(ID3D12Device5* device, gpu::ImmediateQueue& queue) {
        BenchContext context{.device = device, .queue = &queue};
        context.rootSignature = gpu::CreateRootSignature(device, {.uavCount = 1, .rootConstantCount = 1});
        context.results = gpu::CreateBuffer(device, uint64_t{THREAD_COUNT} * 8, gpu::BufferKind::UnorderedAccess);
        context.timestampReadback = gpu::CreateBuffer(device, 2 * sizeof(uint64_t), gpu::BufferKind::Readback);
        const D3D12_QUERY_HEAP_DESC heapDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 2};

        if (!context.rootSignature || !context.results || !context.timestampReadback ||
            FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&context.queryHeap)))) {
            return unexpected("ルート署名・バッファ・クエリのヒープを作れない");
        }

        uint64_t frequency = 0;
        if (FAILED(queue.Native()->GetTimestampFrequency(&frequency)))
            return unexpected("タイムスタンプの周波数を得られない");

        context.ticksPerMillisecond = static_cast<double>(frequency) / 1000.0;

        return context;
    }

    // iterationCount 回の反復を 1 回走らせ、かかった時間(ms)を返す
    expected<double, std::string> RunOnce(const BenchContext& context, ID3D12PipelineState* pipeline,
                                          uint32_t iterationCount) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return unexpected("コマンドリストを始められない");

        list->SetComputeRootSignature(context.rootSignature.Get());
        list->SetPipelineState(pipeline);
        list->SetComputeRootUnorderedAccessView(0, context.results->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstant(1, iterationCount, 0);
        list->EndQuery(context.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        list->Dispatch(GROUP_COUNT, 1, 1);
        list->EndQuery(context.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(context.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                               context.timestampReadback.Get(), 0);
        if (!context.queue->ExecuteAndWait())
            return unexpected("GPU での実行に失敗");

        uint64_t timestamps[2] = {};
        if (!gpu::ReadBuffer(context.timestampReadback.Get(), std::as_writable_bytes(span(timestamps))))
            return unexpected("タイムスタンプを読み戻せない");

        return static_cast<double>(timestamps[1] - timestamps[0]) / context.ticksPerMillisecond;
    }

    // --- 1 つの演算を測る: 反復回数を目標の時間に合わせてから、REPEAT_COUNT 回の中央値 ---

    struct Measurement {
        double nanosecondsPerStep =
            0;  // GPU 全体で 1 ステップ(演算 + 混ぜる)を処理するのにかかる時間(スループットの逆数)
        uint32_t iterationCount = 0;
        double milliseconds = 0;
    };

    expected<Measurement, std::string> Measure(const BenchContext& context, const BenchOperation& operation) {
        const auto bytecode = gpu::LoadShader(std::string("bench/fixed_bench_") + operation.name + ".cso");
        if (!bytecode)
            return unexpected(bytecode.error());

        const ComPtr<ID3D12PipelineState> pipeline =
            gpu::CreateComputePipeline(context.device, context.rootSignature.Get(), *bytecode);
        if (!pipeline)
            return unexpected("パイプラインを作れない");

        uint32_t iterationCount = 4;
        for (;;) {  // 反復回数を目標の時間に近づける(最初の 1 回はウォームアップも兼ねる)
            const auto time = RunOnce(context, pipeline.Get(), iterationCount);
            if (!time)
                return unexpected(time.error());

            if (*time >= TARGET_MILLISECONDS * 0.5 || iterationCount >= (1u << 24))
                break;

            const double scale = *time <= 0.01 ? 64.0 : std::min(64.0, TARGET_MILLISECONDS / *time);
            iterationCount = static_cast<uint32_t>(std::max(2.0, scale) * iterationCount);
        }

        std::vector<double> times;
        for (int repeat = 0; repeat < REPEAT_COUNT; ++repeat) {
            const auto time = RunOnce(context, pipeline.Get(), iterationCount);
            if (!time)
                return unexpected(time.error());

            times.push_back(*time);
        }

        rng::sort(times);
        const double median = times[times.size() / 2];
        const double steps = double{THREAD_COUNT} * iterationCount * UNROLL;

        return Measurement{
            .nanosecondsPerStep = median * 1.0e6 / steps, .iterationCount = iterationCount, .milliseconds = median};
    }

    // --- 表にする ---

    void Report(span<const Measurement> measurements) {
        auto find = [&](string_view name) {
            for (size_t index = 0; index < std::size(BENCH_OPERATIONS); ++index) {
                if (name == BENCH_OPERATIONS[index].name)
                    return measurements[index].nanosecondsPerStep;
            }

            return 0.0;
        };

        const double base32 = find("base32");
        const double base64 = find("base64");
        const double floatMultiply = find("fmul");
        Log(Channel::Gpu, Level::Info,
            "| 演算 | 1 回(ps、GPU 全体) | 混ぜる分を引いた費用(ps) | fmul の何回分 | 反復 | 時間(ms) |");
        Log(Channel::Gpu, Level::Info, "|---|---|---|---|---|---|");

        for (size_t index = 0; index < std::size(BENCH_OPERATIONS); ++index) {
            const BenchOperation& operation = BENCH_OPERATIONS[index];
            const Measurement& measurement = measurements[index];
            const double base = operation.width == 32 ? base32 : operation.width == 64 ? base64 : 0.0;
            const bool isBase = index < 2;
            const double net = isBase ? measurement.nanosecondsPerStep : measurement.nanosecondsPerStep - base;

            Log(Channel::Gpu, Level::Info, "| {} {} | {:.2f} | {:.2f} | {:.1f} | {} | {:.1f} |", operation.name,
                operation.description, measurement.nanosecondsPerStep * 1000, net * 1000, net / floatMultiply,
                measurement.iterationCount, measurement.milliseconds);
        }
    }

    int Run(span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_fixed_bench [--warp] [--queue direct|compute]");
            return 2;
        }

        const auto device = gpu::Device::Create(options->adapter);
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_fixed_bench: {}", device.error());
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        if (!queue)
            return 1;

        const auto context = CreateContext(device->Get(), *queue);
        if (!context) {
            Log(Channel::Gpu, Level::Error, "gpu_fixed_bench: {}", context.error());
            return 1;
        }

        Log(Channel::Gpu, Level::Info, "gpu_fixed_bench: adapter {}, queue {}, {} スレッド × 展開 {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType), THREAD_COUNT, UNROLL);

        std::vector<Measurement> measurements;
        for (const BenchOperation& operation : BENCH_OPERATIONS) {
            const auto measurement = Measure(*context, operation);
            if (!measurement) {
                Log(Channel::Gpu, Level::Error, "{}: {}", operation.name, measurement.error());
                return 1;
            }

            measurements.push_back(*measurement);
        }

        Report(measurements);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();  // ログを閉じる

    return exitCode;
}
