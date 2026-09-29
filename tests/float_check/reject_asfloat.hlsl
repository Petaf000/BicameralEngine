// 検査のテスト用。ソースに float の型を書かず、asfloat で浮動小数点の計算を忍び込ませる。DXIL の検査が拒否しなければならない。
RWStructuredBuffer<uint> outputs : register(u0);

[numthreads(64, 1, 1)] void Main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    outputs[dispatchThreadId.x] = asuint(asfloat(dispatchThreadId.x) * asfloat(0x3f000000u));
}
