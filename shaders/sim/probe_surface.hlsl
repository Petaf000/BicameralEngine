// probe_surface.hlsl — 筆の「面に沿って置く」(T-0332・docs/design/14 §2「実装(T-0332)」)のための、物のセルの印(1 セル 1 ビット)を作る Compute。
//
// データの流れ(sim/probe_surface.cpp が、シミュのフレームのリストの抽出の後ろに記録する。覗き窓と同じフック):
//   世界のセル S(t)(u0。2 世代のうち t & 1 の世代。読むだけ)→ セルの物質量の合計がしきい値以上なら 1
//   → 印の語(u1。PROBE_CELL_COUNT / 32 語。セルの番号 x + 64y + 4096z の 32 個ずつ。下位ビットが若い番号)
//   → 読み戻して CPU の editor/surface_pick が光線を格子に通して最初に当たる物のセルを探す。
// View のためだけ(世界の結果にも再生ファイルにも入らない。D-107・D-403)。1 スレッド = 1 語(32 セル)。
#include "common/probe_surface.hlsli"

RWStructuredBuffer<RxCell> g_world : register(u0);      // 世界のセル(2 世代 × PROBE_CELL_COUNT)。読むだけ
RWStructuredBuffer<uint32_t> g_surface : register(u1);  // 物のセルの印(PROBE_CELL_COUNT / 32 語)

cbuffer SurfaceConstants : register(b0) {
    uint32_t g_generationBase;  // 読む世代の先頭(0 か PROBE_CELL_COUNT)
};

static const uint32_t SURFACE_GROUP_THREADS = 64;

// clang-format off
[numthreads(SURFACE_GROUP_THREADS, 1, 1)] void BuildSurfaceMask(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t word = dispatchThreadId.x;
    if (word >= PROBE_SURFACE_WORDS)
        return;

    uint32_t bits = 0;
    for (uint32_t bit = 0; bit < 32; ++bit) {
        if (ProbeIsSolidCell(g_world[g_generationBase + (word * 32) + bit], PROBE_SURFACE_THRESHOLD_MICROMOLES))
            bits |= 1u << bit;
    }

    g_surface[word] = bits;
}
// clang-format on
