-- package.luau — 試験用の反応表のパッケージ(T-0021)。今の仮の世界のゲーム本体でもある(ランタイムが exe の横の data/packages から読む。T-0157)。
-- 現実の元素と化学 + 触るための仮の魔素 mana_test(試験用の仮の値。T-0225)だけで、ゲームの中身ではない(D-006)。テストもこの表を読む
-- (tests/reaction_test_table.h。C++ の写しは T-0169 で消した)。書き方は engine/src/script/reaction_package.h。
return { format = 1, entry = "init" }
