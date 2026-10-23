-- units.luau — 熱伝導率の書き直しの倍率(T-0089)。
-- species.luau が標準パッケージ real_matter の現実の値に掛けて書き直す(T-0498。作り手が自分の世界の値を決める形の見本)。
-- 2026-10-24 に試験の倍率(固体 20000・気体 5000)を外して 1 にした(D-637・T-1066)。セルの伝導率は体積で重み付けた平行の道になり、
-- 燃え広がりは放射と気体の流れで支える(T-1018)。現実の値(mW/(m·K))のまま
local SOLID_CONDUCTIVITY_SCALE = 1
local GAS_CONDUCTIVITY_SCALE = 1

return {
    -- 現実の値(mW/(m·K))→ 試験の値
    solid_conductivity = function(real_mw_per_m_k)
        return real_mw_per_m_k * SOLID_CONDUCTIVITY_SCALE
    end,
    gas_conductivity = function(real_mw_per_m_k)
        return real_mw_per_m_k * GAS_CONDUCTIVITY_SCALE
    end,
}
