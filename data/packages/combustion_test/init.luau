-- init.luau — 分類 → 鍵 → 値 の表を返す(ADR-0031)。元素・物質・規則は 1 つの鍵 = 1 つ。Mod は鍵を足して増やす。
return {
    elements = require("elements"),
    species = require("species"),
    reactions = require("reactions"),
}
