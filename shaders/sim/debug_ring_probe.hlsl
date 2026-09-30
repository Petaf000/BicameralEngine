// debug_ring_probe.hlsl — デバッグのリング(common/debug_ring.hlsli)を compute シェーダーから確かめる(T-0003)。
// スレッド i(i < printThreadCount)が 4 つの型の引数で 1 行ずつ書き、スレッド 5 は assert を 1 つ書く。
// CPU(tests/gpu_debug_ring_test.cpp)が同じ値を std::format で作って、読み戻した行と比べる。
// printThreadCount をリングの容量より大きくすると、溢れた分を落として数えるかも確かめられる。
#define BICAMERAL_GPU_DEBUG 1  // Release でもリングを使う(このシェーダーはリングの確認そのもの)
#include "common/debug_ring.hlsli"

cbuffer ProbeConstants : register(b0) {
    uint32_t printThreadCount;
};

[numthreads(64, 1, 1)] void Main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t thread = dispatchThreadId.x;
    if (thread >= printThreadCount)
        return;

    const int32_t negative = -(int32_t)thread - 1;
    const uint64_t large = ((uint64_t)thread << 40) | 0xABCDu;
    const int64_t signed64 = -((int64_t)thread << 33) - 7;
    DEBUG_PRINT(DebugFormat::DebugRingProbe, thread, negative, large, signed64);
    DEBUG_ASSERT(thread != 5, DebugFormat::DebugRingProbeAssert, thread);
}
