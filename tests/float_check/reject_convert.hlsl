// 検査のテスト用(tests/CMakeLists.txt の float_check_*)。整数から float への変換と sqrt。DXIL の検査が拒否しなければならない。
RWStructuredBuffer<uint> outputs : register(u0);

[numthreads(64, 1, 1)] void Main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    outputs[dispatchThreadId.x] = (uint)sqrt(dispatchThreadId.x * 0.5f);
}
