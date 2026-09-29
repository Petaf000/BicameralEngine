// fixed_selftest.hlsl — 整数の数学ライブラリの自己テストを GPU で走らせる compute シェーダー(T-0010 で作り、T-0013 で走らせる)。
// スレッド i が FxSelfTestInputs(i) の入力で FxSelfTestCase を呼び、結果 FX_SELF_TEST_OUTPUT_COUNT 個を outputs に書く。
// CPU(tests/fixed_test.cpp)が同じ関数で作る列とビット単位で一致すれば、GPU と CPU の計算が同じ(D-307)。
// ビルドの中で DXC がこのファイルを通すので、fixed.hlsli が HLSL として正しいこともここで確かめている。
#include "common/fixed_selftest.hlsli"

RWByteAddressBuffer outputs : register(u0);

[numthreads(64, 1, 1)] void Main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t caseIndex = dispatchThreadId.x;
    const FxU128 inputs = FxSelfTestInputs(caseIndex);
    const FxSelfTestOutput output = FxSelfTestCase(inputs.hi, inputs.lo);
    const uint32_t baseAddress = caseIndex * FX_SELF_TEST_OUTPUT_COUNT * 8;
    for (uint32_t i = 0; i < FX_SELF_TEST_OUTPUT_COUNT; ++i)
        outputs.Store<uint64_t>(baseAddress + i * 8, output.values[i]);
}
