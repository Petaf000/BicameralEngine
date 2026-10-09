# T-0140 ベイク前の型検査(Luau.Analysis)

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(2 時間の約束の中。並走の作業ツリー wt3・ブランチ t-0140)
- マイルストーン: M2 (docs/plan/ROADMAP.md。T-0020 から分けた)
- 設計: docs/design/13-scripting-game-model.md §2・§2.3 / 決定: D-318・D-320・ADR-0030・ADR-0031・ADR-0032・ADR-0033・ADR-0046

## 目的(1〜2 行)
パッケージの Luau を、走らせる前に型検査し、誤りを「どのファイルの何行目・何が違うか」ではっきり出す。
反応表・物質・元素の定義の形は、型の定義ファイル(data/types/bicameral.d.luau)に型で書く。

## 完了条件(チェックできる形で)
- [x] vcpkg の luau port に Luau.Analysis が入っているか確かめる → 入っている(`unofficial::luau::Luau.Analysis`。追加の入れ方は不要)
- [x] 型の定義ファイル(マニフェスト・entry の表・元素・物質・反応・速度)。試験の表のパッケージがそのまま通る
- [x] 読む前の型検査(全部のファイルを strict・require はパッケージの中だけ・殻で見えないグローバルは検査でも見えない)
- [x] 誤りの出し方: 「ファイル:行:列: 何が違うか: Luau の説明」。返す表は欄ごとに突き合わせ、欄を書いた行に出す(require した先のファイルでも)
- [x] LoadPackages が型の誤りのあるパッケージ(と依存するもの)を読まない。ランタイムの入り口(LoadReactionTable)が型の定義を読んで使う
- [x] 決定性: 同じ検査器で 2 回・間にほかのパッケージ・作り直した検査器で、同じ診断の列(luau_type_check_test)

## メモ・参考
- 実装: engine/src/script/luau_type_check.{h,cpp}(`LuauTypeChecker`・`TypeDiagnostic`・`FormatDiagnostic`)、
  型の定義 data/types/bicameral.d.luau(ビルドが bin/data/types に写す)、`PackageLoadOptions::typeChecker`、`ReactionTableSource::typeDefinitions`。
- テスト: tests/luau_type_check_test.cpp(ctest の `luau_type_check`)。
- Luau の新しい型ソルバーは「省ける欄を書かなかった表」を、省ける欄つきの型へ渡せない(表の欄の型を不変として扱う。`read` を付けても同じ。
  `read` は鍵の型〔`[string]: T`〕には付けられない)。実験で確かめた。だから返す表を型 1 つに丸ごと渡す検査はせず、欄ごとに降りる(ADR-0046)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(1 チャット目・並走の作業ツリー wt3・ブランチ t-0140): vcpkg の luau に Luau.Analysis があるのを確かめ、`LuauTypeChecker`(新しい型ソルバー・strict・
  殻で見えないグローバルを外す・require はパッケージの中だけ)と型の定義 data/types/bicameral.d.luau を書いた。返す表を型 1 つに丸ごと渡す形は
  省ける欄で正しい表も落ちる(実験)ので、欄ごとに降りて葉だけ Luau に確かめさせる形にし(ShapeWalker)、誤りを欄を書いた行に付け替えた。
  LoadPackages(`typeChecker`)と LoadReactionTable(`typeDefinitions`)につないだ。luau_type_check_test(試験の表が通る・誤り 12 通りの場所と種類・
  LoadPackages が読まない・決定性・定義の誤り)。ADR-0046、13 §2.3、map.yaml の typecheck ノード。
  debug で `^(luau_|reaction_package)` 6 本通過(下の rebase 後の結果も参照)。

## 引き継ぎメモ(HANDOFF に載せる状態)
- 動いているもの: ランタイム(`bicameral`)とテストの入り口 LoadReactionTable が、パッケージを走らせる前に型検査する。
  確認: `-Filter "^(luau_|reaction_package)"`(debug。reaction_package_scene が 66 秒、ほかは数秒)。
- 壊れているもの: なし。
- 決めたこと(Claude・実装の細部): ADR-0046(型の定義は data/types・新しい型ソルバー・strict・欄ごとに突き合わせる・誤りのあるパッケージは読まない)。
- 仮(ユーザー未確認): 下の「判断待ち」の 1(型の誤りは読まない)。
- 注意:
  - **読み手(reaction_package.cpp・luau_package.cpp のマニフェスト)の欄を変えたら、data/types/bicameral.d.luau も変える**。
    変えないと新しい欄が「知らない欄」になり、ゲーム本体のパッケージが型検査で落ちて起動が止まる(並走の T-0026 気体などで物質の欄が増えるときに当たる)。
  - data/ に types/ が増えた(ビルドが bin/data/types に写す。exe だけを別の所へ写すときは data/ も一緒に)。
  - Luau.Analysis は bicameral_script に PRIVATE でつないだ(知るのは luau_type_check.cpp だけ)。リンクの時間と exe の大きさが少し増える。
  - 試験の表のパッケージ(data/packages/combustion_test)は変えていない(型を付けなくても通る)。
  - LuauTypeChecker は 1 つの Frontend を持つ(スレッドをまたいで同時に使わない)。

## 判断待ち
1. **型検査で誤りが出たパッケージ(Mod)をどう扱うか**(仮で案 A で進めた・ユーザー未確認):
   - 案 A 読まない(今): 誤りのある Mod は、走らせても動いたかもしれなくても入らない。プレイヤーは「この Mod は○○.luau の 12 行目が違う」と
     知らされ、半端な法則の世界で遊ぶことがない(D-428)。Mod を作る人は型を合わせる手間が増える。
   - 案 B 警告だけで読む: 動く Mod は入る。代わりに型の誤りが読み手の誤り(欄・型)や実行時の誤りとして後で出ることがあり、
     どの行が悪いかが分かりにくい。
   - 案 C ゲーム本体は A、Mod は B: 本体の表は必ず正しく、Mod の敷居は低い。
   - おすすめ: A(取り消しやすい。`LoadPackages` の型検査の結果を理由に入れるか警告に回すかの 1 か所を変えるだけ)。
   - 補足: 型を付けずに書いた正しいパッケージは通る(試験の表がそのまま通る)。誤りとして出るのは、知らない名前・型の合わない値・
     綴りの誤りの欄・require の先が無い・文法の誤りなど、どのみち読み込みで落ちるか、黙って違う値になるものだけ。

## 分けたもの
- なし(Mod を作る人向けに型の定義を配る方法・エディタでの表示は、エディタの T-0023 / ホットリロードの T-0139 で必要になったら)。
