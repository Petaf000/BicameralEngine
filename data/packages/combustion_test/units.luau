-- units.luau — 熱伝導率の試験の倍率(T-0089。reaction_test_table.cpp と同じ)。
-- 現実の値(mW/(m·K))だと木の中を 0.5 m 伝わるのに数日かかり、空気の伝導はほぼ 0(現実の火は放射と対流で広がる。M3)。
-- 「リアルっぽいが、反応が遅すぎない」ように大きくする(ユーザー 2026-10-01)。倍率は gpu_probe_fire_test で決めた。
-- M3 で放射と対流が入ったら見直す。
local SOLID_CONDUCTIVITY_SCALE = 20000
local GAS_CONDUCTIVITY_SCALE = 5000

return {
    -- 現実の値(mW/(m·K))→ 試験の値
    solid_conductivity = function(real_mw_per_m_k)
        return real_mw_per_m_k * SOLID_CONDUCTIVITY_SCALE
    end,
    gas_conductivity = function(real_mw_per_m_k)
        return real_mw_per_m_k * GAS_CONDUCTIVITY_SCALE
    end,
}
