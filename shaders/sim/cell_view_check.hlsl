// cell_view_check.hlsl — セルの見立て(common/cell_view.hlsli の MakeCellView。T-0713)が HLSL でもコンパイルできることを確かめるだけの
// シェーダー。どの段も読み込まない(実行しない)。GPU の段が見立てを呼ぶようになったら(T-0714 の後)消す。
#include "common/cell_view.hlsli"

StructuredBuffer<RxSpecies> g_species : register(t0);
StructuredBuffer<CvSpeciesPhase> g_phases : register(t1);
StructuredBuffer<RxCell> g_cells : register(t2);
RWStructuredBuffer<CvCellView> g_views : register(u0);

cbuffer RootConstants : register(b0) {
    uint32_t g_cellCount;
    int32_t g_level;
};

// 見立ての表の約束(Species と Phase)
struct CheckTable {
    uint32_t unused;

    RxSpecies Species(uint32_t id) { return g_species[id]; }
    CvSpeciesPhase Phase(uint32_t id) { return g_phases[id]; }
};

[numthreads(64, 1, 1)] void Main(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= g_cellCount)
        return;

    CheckTable table;
    table.unused = 0;
    g_views[thread.x] = MakeCellView(table, g_cells[thread.x], g_level, 0);
}
