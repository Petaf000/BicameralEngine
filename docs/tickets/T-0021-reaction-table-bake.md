# T-0021 反応表のベイク(Luau から・整数・元素とエネルギーの検査)と、公開用の試験の反応表

- Status: Done(この範囲。魔素はユーザーの中身待ち・ランタイムをパッケージの表に切り替えるのは T-0157)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(2 時間の約束の中。並走の作業ツリー wt3)
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: docs/design/02-reaction-system.md §1〜2・§7、13 §2 / 決定: D-206・D-313・D-318・D-320・ADR-0030・ADR-0031・ADR-0032

## 目的(1〜2 行)
Luau のパッケージで書いた反応表を、CPU のベイクで GPU 用の整数の表にし、壊れた表(元素が釣り合わない・形の誤り)をベイクで落とす。
今の反応の核が使っている試験の表(T-0014 の C++ の表)を、Luau から作ってビットで同じ表になることを確かめる。

## 完了条件(チェックできる形で)
- [x] Luau の反応表(分類 elements・species・reactions)→ ReactionTableDefinition(script/reaction_package)。値は整数だけ・単位は欄の名前・
      A は 10 進の文字列でも(誤差なし)・知らない欄・小数・範囲の外は誤り(どのパッケージの・どの欄かを付ける)
- [x] ベイクの検査: 元素の釣り合い・知らない元素と物質・係数・次数(今まで通り)+ 文献の反応熱との食い違いの警告(02 §2 の 2)
- [x] ベイクが定義の並びに依らない(名前のバイト順で ID。A の仮数の末尾の 0 も正規化。ADR-0032)
- [x] 公開用の試験の表のパッケージ(tests/packages/combustion_test。現実の元素と化学だけ・値に出典か「未確認」)をベイクすると、
      C++ の試験の表(MakeCombustionTestTable)と GPU に載せる配列(物質・規則・索引・速度の表)がビットで同じ
- [x] Mod が物質と規則を足せる(水性ガス反応で確認)。壊れた Mod はベイクで落ちる
- [ ] 魔素(元素)と魔素を使う反応 → **ユーザーの中身待ち**(ゲームの中身。02 §5・D-418・D-419)
- [ ] 湧き出し・吸い込み(02 §2 の 3。保存則の明示的な例外と集計)→ T-0024(魔法の発動と一緒に)
- [x] (シミュのコード)ID が変わった後も release の全部のテストが通る(下の作業ログ)

## メモ・参考
- 実装: engine/src/script/reaction_package.{h,cpp}(`ReadReactionTableDefinition`・`ParseDecimal`。ライブラリ bicameral_reaction_script)、
  engine/src/sim/reaction_table.cpp(`CanonicalOrder`・`CheckDeclaredEnthalpy`・A の正規化)、`RuleDefinition::declaredReactionEnthalpy`・
  `BakedReactionTable::warnings`。
- 試験の表: tests/packages/combustion_test(package・init・elements・species・reactions・units)。中身は reaction_test_table.cpp と同じ。
- テスト: tests/reaction_package_test.cpp(ctest の `reaction_package`。debug で 0.7 秒)。
- 書き方の例は reaction_package.h の先頭と、試験のパッケージ。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(1 チャット目・並走の作業ツリー wt3・ブランチ t-0021): Luau の反応表を定義に読む reaction_package(整数だけ・単位は欄の名前・
  A は 10 進の文字列・知らない欄は誤り)と、試験の表のパッケージ tests/packages/combustion_test を書いた。ベイクを名前のバイト順で ID を振る形にし
  (並びに依らない。A も正規化)、文献の反応熱の警告を足した(ADR-0032)。reaction_package_test で C++ の試験の表とビットで同じ・誤りを落とす・Mod が足せる。
  ID が変わったので release の全部のテストを流した(CPU 44 本・GPU 50 本。結果は引き継ぎメモ)。

## 引き継ぎメモ(HANDOFF に載せる状態)
- 動いているもの: `-Filter "^reaction_package$"`(debug で 0.7 秒)。CPU だけ。ランタイム(frame_loop)と GPU のテストは今まで通り C++ の試験の表を使う
  (中身はビットで同じ)。
- 壊れているもの: なし。
- 決めたこと(Claude・実装の細部): ADR-0032(Luau の書き方・ベイクは名前のバイト順で ID・文献の反応熱は警告)。
- 注意:
  - **物質と規則の ID が変わった**(名前のバイト順。carbon 1・carbon_dioxide 2・carbon_monoxide 3・cellulose 4・nitrogen 5・oxygen 6・water_vapor 7)。
    ID を決め打ちするコードは無かった(全部名前で引いている)。マージ後に他の作業ツリーのテストで ID を決め打ちしていないか気を付ける。
  - 試験の表は C++(reaction_test_table.cpp)と Luau(tests/packages/combustion_test)の 2 か所にある。片方を変えたらもう片方も変える
    (reaction_package_test が食い違いを落とす)。T-0157 でランタイムをパッケージに切り替えたら C++ の方を消せる。
  - 試験の表の値の出典: 生成エンタルピーは CODATA の値を丸めたもの、比熱は NIST-JANAF の 298.15 K の値として T-0014 から写した(一つずつの照合は未確認)。
    セルロースの値・熱伝導率の現実の値は未確認。速度は熱分解だけ文献(Bradbury–Sakai–Shafizadeh 1979 の開始反応を 1 段に単純化)、ほかの 4 本は仮の値。

## 判断待ち
- **(ROADMAP の行の「+ 魔素」)魔素の元素と、魔素を使う反応**: ゲームの中身なのでユーザーが決める。仕組みは今の形で書ける
  (元素を 1 つ足し、魔素を含む物質と反応を足すだけ。保存則の検査もそのまま効く)。
  決めること: 魔素の原子量(重さ。運ぶ・溜まる・沈むに効く)・魔素を含む物質(気体の魔素・魔石など)・魔素が関わる反応とその反応熱(D-419: 魔法の熱と力は魔素の消費で釣り合う)。
  プレイヤーへの影響: これが決まるまで、試験の世界で「魔素が反応して熱を出す・物が変わる」場面は作れない(燃焼・熱分解の場面だけ)。おすすめ: 設計チャットで 1 種類の魔素の気体と 1〜2 本の反応から決める。
- **(軽い)公開用の試験の表をどこまで広げるか**: 今は T-0014 の場面の範囲(燃焼・熱分解・Boudouard)だけ。水性ガス反応(C + H2O → CO + H2)や水素の燃焼を足すと、
  試験の世界で「湿った木は燃えにくい・水蒸気で炭が消える」のような連鎖を見せられる(公開の試験の場面が豊かになる)が、ゲームの中身と取り違えられやすい。
  おすすめ: 相変化(蒸発・凝縮)が入るまでは今の範囲のまま。Mod のテストには水性ガス反応を使った(公開の表には入れていない)。

## 分けたもの
- T-0157 ランタイムがパッケージから反応表を読む(frame_loop の `BakeReactionTable(MakeCombustionTestTable())` を、ゲームのデータのフォルダの
  パッケージのベイクに置き換える・データのフォルダの置き場所と配り方・読み込みの失敗の出し方・C++ の試験の表を消すか残すか)。T-0139(ホットリロード)の前に。
- 湧き出し・吸い込み(02 §2 の 3)は T-0024(魔法の発動)で一緒に。
