# T-0398 サンプルの世界の組み立て(木箱・空気・温度・積み木)をパッケージで書く

- Status: Doing
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: D-471 ② 中身の器(docs/plan/ROADMAP.md。T-0371 から分けた)
- 2026-10-13 に新しい指針に合わせて見直した: エンジンは中身を持たない(D-454・D-464)。サンプル「現実」は ⑤ の場面に要る分だけ(D-471 ②)。
  中身は作り手と同じやり方(Luau のパッケージ)で書く(D-486)。ブランクの世界(D-455)では何も読まずに動く。物理定数は D-480・ADR-0171(Accepted)の形に揃える
- 設計: docs/design/13-scripting-game-model.md §2.6・14 §2 / 決定: D-454〜D-456・ADR-0151(追記 1)

## 目的(1〜2 行)
sim::MakeProbeInitialWorld(空気の中の木箱・空気の成分・300 K)と sim::MakeProbeStackScene(積み木)を、サンプルの世界のパッケージ(Luau)の値から組み立てる。エンジンのソースから「空気」「セルロース」の名前と量を消す。

## 完了条件(チェックできる形で)
- [ ] 世界の組み立ての分類と型の定義(data/types)・読む所
- [ ] --world sample がパッケージの組み立てを読み、今の C++ の組み立てと同じセル(ビット一致)になる試験
- [ ] 今の試験(--world sample・サンプルのパッケージを読む試験)の結果(ハッシュ・画像)が変わらない

## メモ・参考
- 洗い出しの一覧は T-0371 のチケット。合わせた表の版(TableVersion)は全部の分類を含むので、反応表と同じ読み込みに新しい分類を足すと
  表の版が変わる。世界の組み立ては別の読み込み(または版に入れない分類)にするかをここで決める。

## 共有の決め事(wt3・wt4 も従う)
2026-10-13 wt2 が決めた(エンジンの作りの細部。作り手から見た違いは下の「判断待ち」)。T-0399(wt3)・T-0400(wt4)も同じ形で足す。

1. **新しい分類は反応表と同じ読み込みに入れる**(LoadPackages の合わせた表。別の読み込みは作らない)。
   - 表の版 `TableVersion`・中身 `TableBytes` に入る。サンプルのパッケージに分類を足すと版の値は変わってよい(版の値を書き込んだ試験は無い。
     世界のハッシュは版を含まない)。
   - 理由: 再生ファイルが表の中身だけから世界を作り直せる(T-0193。初めの世界・環境の値がディスクのパッケージに頼らない)。型検査・
     Mod の合わせ方(AddOnly。鍵ごと)・誤りの出し方・ホットリロードをそのまま使える。
   - 世界のデータだけを変えても版が変わるので、ホットリロードは表の差し替え(印のコマンド)を入れる。法則は同じなので世界は変わらない。
2. **ファイルを分類ごとに分ける**(衝突を減らす。共通の所は 1 行ずつ):
   - 読む所: `engine/src/script/<分類>_package.{h,cpp}`(ライブラリ bicameral_reaction_script)。
     `std::expected<sim::<X>Definition, std::string> Read<X>(const PackageSetResult&)`。欄の道具は `script/package_fields.h`
     (CheckRecord・ReadIntegerField・ReadCategory など。この作業で reaction_package.cpp から出した。wt3・wt4 はコミット
     073eca1「T-0398: パッケージの欄の道具を分け…」(ブランチ t-0398。つなぎ目の節も入っている)を cherry-pick してよい。マージで同じ変更は重ならない)。
     知らない欄は誤り・数は整数だけ・誤りは「パッケージ X: 分類.鍵.欄: 何が違うか」。
   - 型: `engine/src/sim/<分類>.{h,cpp}`(ライブラリ bicameral_reaction。浮動小数点なし)。名前で書いた定義と、物質を ID に直したもの。
   - つなぐ所(印の節に 1 行ずつ): `ReactionTableDefinition` と `BakedReactionTable`(sim/reaction_table.h の「同じ読み込みに足した分類」の節)・
     `ReadReactionTableDefinition`(script/reaction_package.cpp の末尾の節)・`BakeReactionTable`(sim/reaction_table.cpp の末尾の節。
     名前 → ID の解決と検査。**知らない物質は誤り**)。
   - これで LoadReactionTable・LoadBlankReactionTable・RebuildReactionTable(再生)・試験の MakeCombustionTestTable の全部が同じ道を通る。
   - 型の定義: `data/types/bicameral.d.luau` に分類ごとの節(`export type` と `PackageEntry` の欄)を足す。
   - サンプルの値: `data/packages/combustion_test/<分類>.luau` を足し、`init.luau` に 1 行(サンプルの世界のゲーム本体。標準パッケージへの
     分け方は T-0373)。
3. **分類が無い = 空**(ブランクの世界・Mod だけの表)。エンジンは既定の値(名前・量)を持たず、空で動く。
4. 試験の表から物質を消す道具(tests/reaction_test_table.cpp の MakeSpeciesChangedTestTable など)は、消した物質を指す分類の中身を空にする
   (ベイクが「知らない物質」で落ちないように)。
5. 分類の名前: wt2 は `world_cells`(セルの組み立て)・`world_bodies`(物)を使う。wt3・wt4 は別の名前で(例 `material_templates`・`environment`)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
