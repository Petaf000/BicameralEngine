// probe_view.hlsl — T-0004 の仮の世界(common/probe_sim.hlsli)を画面いっぱいの三角形 1 枚で描く。
//
// データの流れ: シミュ(compute キュー)が抽出の 3 組のどれかに書く → 描画(direct キュー)がフェンスを待ってから、
// フレームの定数(frame。CPU がフレームごとに書く)が指す方を読む(06 §4)。浮動小数点はここ(描画)だけ。
#include "common/probe_sim.hlsli"

StructuredBuffer<uint32_t> extraction0 : register(t0);
StructuredBuffer<uint32_t> extraction1 : register(t1);
StructuredBuffer<uint32_t> extraction2 : register(t2);
ByteAddressBuffer frame : register(t3);  // [0] 読む抽出(0〜2) [1] 描く幅(px) [2] 描く高さ(px)

struct VertexOutput {
    float4 position : SV_Position;
};

// 頂点バッファなしで画面を覆う三角形(頂点 3 つ)
VertexOutput VSMain(uint vertexId : SV_VertexID) {
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOutput output;
    output.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

// 量(0〜PROBE_POKE_AMOUNT)を黒 → 赤 → 黄 → 白に
float3 HeatColor(uint32_t value) {
    const float t = sqrt(saturate((float)value / (float)PROBE_POKE_AMOUNT));
    return saturate(float3(t * 3.0, t * 3.0 - 1.0, t * 3.0 - 2.0));
}

float4 PSMain(VertexOutput input) : SV_Target {
    const uint3 constants = frame.Load3(0);
    const float2 viewport = float2(constants.y, constants.z);
    // 正方形の格子を、画面の短い辺に合わせて真ん中に置く
    const float side = min(viewport.x, viewport.y);
    const float2 origin = (viewport - side) * 0.5;
    const float2 local = (input.position.xy - origin) / side;
    if (any(local < 0.0) || any(local >= 1.0)) return float4(0.02, 0.02, 0.03, 1);

    const uint2 cell = min((uint2)(local * PROBE_GRID_SIZE), PROBE_GRID_SIZE - 1);
    const uint32_t cellIndex = ProbeCellIndex(cell.x, cell.y);
    const uint32_t value = constants.x == 0   ? extraction0[cellIndex]
                           : constants.x == 1 ? extraction1[cellIndex]
                                              : extraction2[cellIndex];
    return float4(HeatColor(value), 1);
}
