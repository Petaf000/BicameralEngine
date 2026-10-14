-- package.luau — サンプルの世界(D-456)のパッケージ。ランタイムの --world sample のゲーム本体で、テストもこの表を読む
-- (tests/reaction_test_table.h)。書き方は engine/src/script/reaction_package.h。
-- 標準パッケージ(data/standard。現実の元素・物質・反応・地球の環境)の上に作り、遊びのための値だけを書く(D-490 ①・ADR-0183 の追記・T-0498):
--   書き直す = 熱伝導率の試験の倍率(species.luau・units.luau)。足す = 試験の場面(木箱の世界・積み木)・材料のひな形・抽出の物質・
--   実験室の既定・遊びの指標と、触るための仮の魔素 mana_test(試験用の仮の値。T-0225。ゲームの中身ではない。D-006)。
-- 依存の標準パッケージは data/packages に無くても棚から読む(script::AddStandardDependencies)。
return {
    format = 1,
    title = "サンプルの世界(燃焼の試験)",
    depends = { "real_elements", "real_matter", "real_reactions", "earth_environment" },
    entry = "init",
}
