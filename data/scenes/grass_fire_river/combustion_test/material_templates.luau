-- material_templates.luau — 筆の「置く」と実験室の材料(量は 1 セルあたりの µmol。world_cells と同じ値)。
-- 遊び方の見本: 火が来る前に筆で水や砂を置いて防火帯を作る・草を足して火の道を作る。
return {
    air = {
        label = "空気",
        order = 1,
        contents = {
            { species = "oxygen", micromoles = 1063800 },
            { species = "nitrogen", micromoles = 4014200 },
        },
    },
    grass = {
        label = "草",
        order = 2,
        contents = {
            { species = "cellulose", micromoles = 1200000 },
            { species = "oxygen", micromoles = 1063800 },
            { species = "nitrogen", micromoles = 4014200 },
        },
    },
    water = {
        label = "水",
        order = 3,
        contents = { { species = "water", micromoles = 2100000000 } },
    },
    soil = {
        label = "砂",
        order = 4,
        contents = {
            { species = "silica", micromoles = 2000000000 },
            { species = "oxygen", micromoles = 425520 },
            { species = "nitrogen", micromoles = 1605680 },
        },
    },
}
