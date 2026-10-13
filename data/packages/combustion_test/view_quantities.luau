-- view_quantities.luau — 抽出に写す物質(枠 1〜3)と、デバッグ表示の色が 1 になる量(T-0399。読み手は
-- engine/src/script/material_templates_package.h)。表示の「色分けする量」(C キー)で温度の次に並ぶ。量は 1 セルあたりの µmol。
return {
    -- 空気の酸素からの減り(幅 = 空気のセルの O2。material_templates の air と同じ)
    slot_1 = { label = "O2 の減り", species = "oxygen", span_micromoles = 1063800, depletion = true },
    slot_2 = { label = "CO2", species = "carbon_dioxide", span_micromoles = 1100000 },
    -- 炭は木箱の壁のセルが全部炭になった量(約 2.3e8 µmol)で 1
    slot_3 = { label = "炭", species = "carbon", span_micromoles = 240000000 },
}
