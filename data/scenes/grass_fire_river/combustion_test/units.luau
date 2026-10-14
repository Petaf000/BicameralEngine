-- units.luau — 熱伝導率の試験の倍率(サンプルの世界 combustion_test/units.luau と同じ値。T-0089)。
-- サンプルの世界のファイルは require で読めない(require は自分のパッケージと依存のパッケージだけ。サンプルの世界は
-- 標準パッケージでないので依存にできない)ので写した(作り心地の記録 3)。放射(T-0028)と気体の流れ(T-0210)が入ったら外す。
local SOLID_CONDUCTIVITY_SCALE = 20000
local GAS_CONDUCTIVITY_SCALE = 5000

return {
    solid_conductivity = function(real_mw_per_m_k)
        return real_mw_per_m_k * SOLID_CONDUCTIVITY_SCALE
    end,
    gas_conductivity = function(real_mw_per_m_k)
        return real_mw_per_m_k * GAS_CONDUCTIVITY_SCALE
    end,
}
