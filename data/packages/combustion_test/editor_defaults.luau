-- editor_defaults.luau — 実験室と置く操作の既定(T-0399。読み手は engine/src/script/material_templates_package.h)。
-- lab_fill = 実験室の箱を満たす材料(material_templates の鍵)・place_material = 「置く」で初めに選ぶ材料・
-- temperature_mk = 箱を満たす温度と、置く温度の初め(mK)。
return {
    lab_fill = "air",
    place_material = "wood",
    temperature_mk = 300000,
}
