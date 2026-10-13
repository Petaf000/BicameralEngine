-- material_templates.luau — 実験室と筆の「置く」の材料のひな形(T-0399。読み手は engine/src/script/material_templates_package.h)。
-- 量は 1 セル(0.5 m 角 = 0.125 m³)あたりの µmol。物質は 1〜3 個で、書いた順に置く。order はパネルの並び(小さい順)。
-- サンプルの世界の値(D-456)。空気の成分は仮の世界の初めの世界と同じ(300 K・1 気圧の乾いた空気の O2 と N2)。
return {
    air = {
        label = "空気",
        order = 1,
        contents = {
            { species = "oxygen", micromoles = 1063800 },
            { species = "nitrogen", micromoles = 4014200 },
        },
    },
    -- 木: 体積の 1 割が木(密度 500 kg/m³ のセルロース)、残りは孔の中の空気(仮の世界の木箱の壁と同じ)
    wood = {
        label = "木",
        order = 2,
        contents = {
            { species = "cellulose", micromoles = 38600000 },
            { species = "oxygen", micromoles = 983000 },
            { species = "nitrogen", micromoles = 3697000 },
        },
    },
    -- 木炭: 体積の 1 割が炭(密度 300 kg/m³ の炭素 = 3.75 kg ≒ 312 mol)、残りは孔の中の空気
    charcoal = {
        label = "木炭",
        order = 3,
        contents = {
            { species = "carbon", micromoles = 312000000 },
            { species = "oxygen", micromoles = 983000 },
            { species = "nitrogen", micromoles = 3697000 },
        },
    },
    -- 二酸化炭素・窒素: 空気と同じ物質量(5078000 µmol)を 1 つの気体で
    carbon_dioxide = {
        label = "二酸化炭素",
        order = 4,
        contents = { { species = "carbon_dioxide", micromoles = 5078000 } },
    },
    nitrogen = {
        label = "窒素",
        order = 5,
        contents = { { species = "nitrogen", micromoles = 5078000 } },
    },
    -- 仮の魔素(T-0225。試験用の仮の値): 空気の 1 割の物質量。筆で「足す」と、そのセルの木や炭に触媒として効く
    mana_test = {
        label = "魔素(試験)",
        order = 6,
        contents = { { species = "mana_test", micromoles = 507800 } },
    },
}
