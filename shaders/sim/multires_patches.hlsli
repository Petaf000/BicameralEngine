// multires_patches.hlsli — 多重解像度の刻み(反応を評価するシェーダー)が魔法のパッチの列を読むための結び付けと表(T-0024・ADR-0095)。
// 列の形は common/magic_patch.hlsli。u5 は外のバッファの枠だが、刻みの間は GpuMultires::BindRoot がパッチの列を結ぶ
// (外のバッファを使う Compute〔覗き窓・陰解法・実験室のコマンド〕は RecordExternalDispatch で自分のバッファを結ぶ。ルート署名の語は増えない)。
// 反応を評価しないシェーダー(木の管理など)は含めない。
#ifndef BICAMERAL_MULTIRES_PATCHES_HLSLI
#define BICAMERAL_MULTIRES_PATCHES_HLSLI

#include "sim/multires_bindings.hlsli"

RWStructuredBuffer<uint32_t> g_magicPatches : register(u5);  // MP_LIST_WORDS 語

// reaction.hlsli の Table の約束 + パッチの列(MrStepCellWait が MpPatchTable で重ねる)
struct GpuStepTable {
    uint32_t unused;

    RxSpecies Species(uint32_t id) { return g_species[id]; }

    RxRule Rule(uint32_t id) { return g_rules[id]; }

    uint32_t RuleIndex(uint32_t position) { return g_ruleIndex[position]; }

    uint64_t Rate(uint32_t rule, uint32_t kelvin) {
        return g_rates.Load<uint64_t>((rule * RX_RATE_TABLE_KELVINS + kelvin) * 8);
    }

    uint32_t PatchWord(uint32_t word) { return g_magicPatches[word]; }
};

GpuStepTable MakeStepTable() {
    GpuStepTable table;
    table.unused = 0;

    return table;
}

#endif  // BICAMERAL_MULTIRES_PATCHES_HLSLI
