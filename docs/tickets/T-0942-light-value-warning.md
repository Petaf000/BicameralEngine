# T-0942 光の値を書かない物質の警告(D-610)

- Status: Done(2026-10-21 本体 main)
- 種類: 工学(小さな直し)
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: R-R0 の欄と一緒(docs/plan/ROADMAP.md)
- 設計: docs/design/23-radiation.md §2.3 / 決定: D-610・D-571・ADR-0395(欄 light_absorption_*・light_reflectance_*)

## 目的(1〜2 行)
パッケージの物質に光の値を書き忘れたら、ベイクで警告する(表は当たる。書き忘れた固体・液体は灰色で光を通さない・気体は透明として動く。D-610)。

## 完了条件(チェックできる形で)
- [x] ベイクの警告(sim/reaction_table.cpp の `WarnLightFields`。密度・粘り・安息角の書き忘れ〔T-0616・T-0819〕と同じ `WarnPhaseFields` の中で、物質ごとに `baked.warnings` へ)
- [x] 警告の頭 `sim::LIGHT_WARNING_PREFIX`(`"光: "`。坂の `SLOPE_WARNING_PREFIX` と同じ形。試験とパネルがほかの警告と見分ける)
- [x] 試験: reaction_package の `TestMissingLightWarning`(凝縮相の数だけ出る・気体には出ない・両帯を書けば消える・片方の帯だけ・前の欄だけ)
- [x] release のビルド(警告 0)・reaction_package・standard_packages・environment_package・luau_type_check

## 警告の形(作り手から見た違い)
- **固体・液体で光の値を 1 つも書かない**(4 欄のどれも無い) → 1 行:
  `光: 物質 X: 固体に光の値(light_absorption_*・light_reflectance_*)が無い(灰色で光を通さない〔両帯とも吸い込みは上限・照り返し 0.5〕として動かす。D-610)`
  前の欄 `radiation_absorption_mm2_per_mol` だけ書いた物質は「両帯の吸い込みに radiation_absorption_mm2_per_mol = N・照り返し 0 として動かす」(BakeSpeciesLight の決め方どおり)。
- **片方の帯だけ書いた**(吸い込み・照り返しのそれぞれで、short と long の片方だけ) → 欄ごとに 1 行「X を片方の帯だけ書いた(もう片方の帯も同じ値〔灰色〕として動かす)」。
  これは気体にも出す(気体でも片方だけ書けば両帯が同じ値になり、透明ではなくなるため。書き忘れか灰色のつもりかをベイクは見分けられない)。
- **気体で 1 つも書かない** → 知らせない(透明が自然。D-610)。
- 照り返しを書かず吸い込みを両帯書いた物質は知らせない(光の値を書いた物質の照り返しは 0。書いた人の意図として扱う)。

## game = true の時に誤りにするか → しない(警告のまま。仮)
- 既存の扱い: 固体の密度・液体の粘りの書き忘れ(T-0616・T-0819)は game = true でも警告のまま。安息角の書き忘れを game = true で誤りにするのは
  T-0836(D-525 の追記 = Q134)で決まっているが **main にはまだ入っていない**(ベイクに game の印が届く道もまだ無い)。
- 光の値は **密度に合わせて警告のまま**にした。理由: D-610 は書き忘れた固体・液体の既定(灰色で通さない)を「作り手に見せてよい既定」として決めた
  (安息角の既定 35 度は「崩れ方が作り手の意図と違いうる」ので誤りにする、とは性質が違う)。また今のサンプルの世界(grass_fire_river は game = true)
  と標準パッケージの凝縮相は光の値を書いていないので、誤りにすると読み込めなくなる。
- 誤りに上げたくなったら: T-0836 で game の印をベイクへ渡す道を作る時に、`LIGHT_WARNING_PREFIX` の警告を同じ道で誤りに変えれば済む(仕組みは変わらない)。

## 標準のパッケージ・サンプルの世界に出る警告(値は書かない)
- real_matter の凝縮相 4 つ(炭〔黒鉛〕carbon・セルロース cellulose・液体の水 water・氷 ice)に出る。場面 grass_fire_river は砂 silica にも出る。
- どれも「値を書くべき物質」(光を受ける面になる固体・液体。D-610 の既定の灰色で動くので今のふるまいは変わらない)。値(炭は黒・水は見える光を通す など)は
  サンプル・ゲームの中身なので書いていない(ゲームの中身へ)。書くまで読み込むたびに 1 物質 1 行の警告が出る。
- 試験への影響: 標準のパッケージに「坂以外の警告が無い」を確かめていた所(reaction_package の OtherWarningCount・standard_packages の none_of)は、
  光の警告を除いて数えるように直した(`LightWarningCount`・`OtherWarnings`。密度などの書き忘れの試験は前と同じく 1 件を数える)。

## 作業ログ
- 2026-10-21 本体: `WarnLightFields`・`LIGHT_WARNING_PREFIX` を足し、reaction_package に `TestMissingLightWarning` を足した。
  光の値の欄・既定の決め方(BakeSpeciesLight・light.hlsli・sun_column)は触っていない(wt6 が触っている)。表のバイトは変わらない(警告だけ)。
