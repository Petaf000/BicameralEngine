-- package.luau — 試験用の反応表のパッケージ(T-0021)。今の仮の世界のゲーム本体でもある(ランタイムが exe の横の data/packages から読む。T-0157)。
-- 現実の元素と化学だけで、ゲームの中身ではない(D-006)。中身は engine/src/sim/reaction_test_table.cpp(C++ に手で書いた試験の表)と同じ。
-- ベイクするとビットで同じ表になる(tests/reaction_package_test.cpp・reaction_package_scene_test.cpp)。書き方は engine/src/script/reaction_package.h。
return { format = 1, entry = "init" }
