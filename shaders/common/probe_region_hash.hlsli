// probe_region_hash.hlsli — 区画(気体・液体)の持ち越す欄を、世界の状態のハッシュ(ハッシュの表の hash)に足す寄与(T-0620)。
//
// 世界の状態のハッシュ = Σ セルの寄与(ProbeCellHash)+ Σ 区画のセルの寄与 + 区画の帳簿の寄与(どれも mod 2^64 の和。04 R2)。
// GPU は HashCells の後で GasHash・LiquidHash(区画があるときだけ)が同じ欄へ 64bit の atomic で足し、CPU は
// ProbeReference::StateHash が ProbeGasField::HashTerm・ProbeLiquidField::HashTerm を足す(ビット一致)。
// 1 つの寄与 = 種(区画の種類と欄)と番号から始め、値を順に混ぜ、最後に仕上げる。浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_PROBE_REGION_HASH_HLSLI
#define BICAMERAL_PROBE_REGION_HASH_HLSLI

#include "common/fixed.hlsli"

#ifdef __cplusplus
namespace bicameral::sim {
    using ::bicameral::fx::FxHashCombine;
    using ::bicameral::fx::FxMix64;
#endif

    // 種(セルの寄与 ProbeCellHash と、区画の種類・欄の間で混ざらないように)
    FX_CONST uint64_t PROBE_GAS_CELL_HASH_SEED = FX_U64(0x67617343u, 0x454c4c31u);
    FX_CONST uint64_t PROBE_GAS_LEDGER_HASH_SEED = FX_U64(0x6761734cu, 0x45444731u);
    FX_CONST uint64_t PROBE_LIQUID_CELL_HASH_SEED = FX_U64(0x6c697143u, 0x454c4c31u);
    FX_CONST uint64_t PROBE_LIQUID_LEDGER_HASH_SEED = FX_U64(0x6c69714cu, 0x45444731u);

    FX_FN uint64_t ProbeRegionHashBegin(uint64_t seed, uint32_t index) {
        return FxHashCombine(seed, (uint64_t)index);
    }

    FX_FN uint64_t ProbeRegionHashAdd(uint64_t hash, int64_t value) {
        return FxHashCombine(hash, (uint64_t)value);
    }

    FX_FN uint64_t ProbeRegionHashEnd(uint64_t hash) {
        return FxMix64(hash);
    }

#ifdef __cplusplus
}  // namespace bicameral::sim
#endif

#endif  // BICAMERAL_PROBE_REGION_HASH_HLSLI
