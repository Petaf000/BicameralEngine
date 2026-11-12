// phase_layer_faces.hlsl — 混ざったセル(層 + 気体)の伝導の面を部分ごとに流す式(shaders/common/transport_conduction_parts.hlsli)を
// GPU で走らせる compute シェーダー(T-1230 段 2 ③ の 1 段目・ADR-0512 の 2)。世界の木の段(陽の当てる段・陰の系を作る段)へつなぐ前に、
// 面の式そのものが RTX でも CPU とビット単位で同じかを確かめる(64bit の最小・割り算・シフトの組。CLAUDE.md §4・T-1078・T-1159)。
// 1 スレッド = 面 1 つ(セル i とセル j の面。向き・レベル・レベルの差・段は番号から決める)。セルと Ec は t4・t5、表は t0〜t3、
// 結果は u0 に面ごとに PROBE_VALUES 語(int64)。CPU リファレンスは tests/gpu_phase_layer_test.cpp の ProbeFace(同じ関数を同じ順に)。
#include "common/transport_conduction_parts.hlsli"

RWStructuredBuffer<int64_t> g_probes : register(u0);
StructuredBuffer<RxSpecies> g_species : register(t0);
StructuredBuffer<RxRule> g_rules : register(t1);
StructuredBuffer<uint32_t> g_ruleIndex : register(t2);
ByteAddressBuffer g_rates : register(t3);
StructuredBuffer<RxCell> g_cells : register(t4);
StructuredBuffer<int64_t> g_condensedEnergies : register(t5);

cbuffer RootConstants : register(b0) {
    uint32_t g_cellCount;  // 面の数も同じ
};

static const uint32_t PROBE_VALUES = 20;  // gpu_phase_layer_test.cpp の FACE_PROBE_VALUES と同じ

// reaction.hlsli の Table の約束(表の読み方。phase_layer_cells.hlsl と同じ)
struct GpuReactionTable {
    uint32_t unused;

    RxSpecies Species(uint32_t id) { return g_species[id]; }

    RxRule Rule(uint32_t id) { return g_rules[id]; }
    RxRule RateRule(uint32_t id) { return g_rules[id]; }

    uint32_t RuleIndex(uint32_t position) { return g_ruleIndex[position]; }

    uint64_t Rate(uint32_t rule, uint32_t kelvin) {
        return g_rates.Load<uint64_t>((rule * RX_RATE_TABLE_KELVINS + kelvin) * 8);
    }
};

// --- 面の番号から決める値(CPU の FaceProbeSetup と同じ式)---
uint32_t ProbeOther(uint32_t face) {
    return (face * 389u + 17u) % g_cellCount;
}

uint32_t ProbeDirection(uint32_t face) {
    return face % 6u;
}

uint32_t ProbeGap(uint32_t face) {
    return (face / 6u) % 3u;
}

int32_t ProbeLevel(uint32_t face) {
    return (int32_t)((face / 7u) % 12u);
}

void StorePair(uint32_t base, uint32_t offset, TxPartPair pair) {
    g_probes[base + offset] = (int64_t)pair.conductance;
    g_probes[base + offset + 1] = pair.explicitFlow;
}

[numthreads(64, 1, 1)] void Main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t face = dispatchThreadId.x;
    if (face >= g_cellCount)
        return;

    GpuReactionTable table;
    table.unused = 0;
    const uint32_t other = ProbeOther(face);
    const uint32_t direction = ProbeDirection(face);
    const uint32_t gap = ProbeGap(face);
    const int32_t level = ProbeLevel(face);
    const uint32_t repeatShift = gap != 0 ? 2u * ((face / 5u) % 2u) : 0u;
    const uint32_t row = (face / 18u) & ((1u << gap) - 1u);
    const RxCell selfCell = g_cells[face];
    const RxCell otherCell = g_cells[other];
    const int64_t selfEnergy = g_condensedEnergies[face];
    const int64_t otherEnergy = g_condensedEnergies[other];

    // --- 片側(混ざらないセルは今の熱のキャッシュと同じ値の部分 1 つ)と、粗い側を細かい面の段へ写す ---
    const PlPartSide self = PlPartSideOf(table, selfCell, selfEnergy, direction);
    PlPartSide otherSide = PlPartSideOf(table, otherCell, otherEnergy, direction ^ 1u);
    if (gap != 0)
        otherSide = TxPartSideOnRow(otherSide, gap, row);

    // --- 陽の面の流れ・層の上の面・粗い側へ送る分(TxFlowConductionCell と同じ組み方)---
    const PlFaceFlow flow = TxPartFaceFlow(self, otherSide, level, gap, repeatShift);
    const PlLayer layer = PlLayerOf(table, selfCell);
    int64_t inner = 0;
    if (layer.mixed != 0) {
        const TxCellParts parts = TxMakeCellParts(PlPartsOf(table, selfCell, selfEnergy), layer);
        inner = TxPartInnerFlow(parts.lateral.lower, parts.lateral.upper, parts.seriesConductance, level);
    }

    int64_t sent = flow.total;
    int64_t coarseWhole = 0;
    if (gap != 0) {
        const MrCrossTransfer transfer = MrSplitCrossFlow(flow.total, gap, (face & 1u) != 0);
        sent = -transfer.fineDelta;
        coarseWhole = transfer.coarseDelta.whole;
    }

    const int64_t selfScaled = TxScaleCondensed(flow.selfCondensed, sent, flow.total);
    const int64_t coarseCondensed = TxCoarseCondensedWhole(TxScaleCondensed(flow.neighborCondensed, sent, flow.total),
                                                           gap);

    // --- 陰の系の組の面(TxPartPairsOf)---
    const TxPartPairs pairs = TxPartPairsOf(self, otherSide, level, gap);
    const PlPairOverlaps overlaps = PlPairOverlapsOf(self, otherSide);

    const uint32_t base = face * PROBE_VALUES;
    g_probes[base + 0] = flow.total;
    g_probes[base + 1] = flow.selfCondensed;
    g_probes[base + 2] = flow.neighborCondensed;
    g_probes[base + 3] = inner;
    g_probes[base + 4] = sent;
    g_probes[base + 5] = selfScaled;
    g_probes[base + 6] = coarseCondensed;
    StorePair(base, 7, pairs.lowerLower);
    StorePair(base, 9, pairs.upperUpper);
    StorePair(base, 11, pairs.lowerUpper);
    StorePair(base, 13, pairs.upperLower);
    g_probes[base + 15] = (int64_t)FX_U64(overlaps.upperUpper, overlaps.lowerLower);
    g_probes[base + 16] = (int64_t)FX_U64(overlaps.upperLower, overlaps.lowerUpper);
    g_probes[base + 17] = (int64_t)FX_U64(self.splitQ16, otherSide.splitQ16);
    g_probes[base + 18] = (int64_t)FX_U64(layer.mixed, PlLayerOf(table, otherCell).mixed);
    g_probes[base + 19] = coarseWhole;
}
