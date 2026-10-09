# ADR-0046 パッケージは走らせる前に Luau.Analysis で型検査し、返す表は型の定義と欄ごとに突き合わせる

- Status: Accepted(実装の細部。誤りのあるパッケージを読まない扱いは、ADR-0033 の失敗の扱いに揃えた。T-0140 の「判断待ち」も参照)
- 日付: 2026-10-09
- 決めた人: Claude(実装の細部。CLAUDE.md §5。型注釈を使いベイクの前に型検査を通すことは 13 §2 で決まっていた。T-0140)

## 背景
- 13 §2「型: Luau の型注釈を使い、ベイクの前に型検査を通す」。今は形の誤り(欄の型・知らない欄)を読み手(reaction_package)が
  走らせた後に見つけ、「どのパッケージの・どの欄か」までしか出せない(何行目かが分からない)。
- 13・ADR-0030 の「未確認」: vcpkg の luau port に Luau.Analysis が入っているか → **入っている**(0.729。`unofficial::luau::Luau.Analysis`)。
- Luau の新しい型ソルバー(0.729)で確かめたこと(T-0140 の実験):
  - `local m: { format: number, entry: string? } = t`(t は `{ format = 1 }` を入れた変数)は**通らない**。省ける欄を書かなかった表は、
    省ける欄つきの型へ渡せない(表の欄の型を不変として扱う)。`read` を付けても同じ。`read` は鍵の型(`[string]: T`)には付けられない。
  - 同じ表を、その場で型を付けて書けば通る(`local m: T = { format = 1 }`。表を書く所で型を見る)。
  - 欄の多い表 → 鍵の型(`{ a = 1, b = 2 }` → `{ [string]: number }`)は通る。
  つまり「entry が返す表を `PackageEntry` 型の変数に入れて確かめる」形では、正しい試験の表も落ちる。

## 決定
1. **型の定義ファイル** `data/types/bicameral.d.luau`(Luau の定義ファイル。`export type` だけ)。`PackageManifest`(package.luau が返す表)・
   `PackageEntry`(entry が返す表)・`Element`・`Species`・`ReactionRate`・`Reaction`。読み手(luau_package・reaction_package)と同じ形。
   ビルドが bin/data/types に写し、ランタイムは `<exe>/data/types/bicameral.d.luau` を読む(`ReactionTableSource::typeDefinitions` で変えられる)。
   エディタ(luau-lsp など)の定義ファイルとしても使える。
2. **検査器** `script::LuauTypeChecker`(engine/src/script/luau_type_check.*)。Luau.Analysis の Frontend(新しい型ソルバー)を 1 つ持つ。
   - 標準の型 + 型の定義。殻(ADR-0030)で見えないグローバル(os・debug・gcinfo・getfenv・setfenv・newproxy など)は外す(走らせる前に「知らない名前」で落ちる)。
   - 全部のファイルを strict で見る(ファイルの先頭の `--!nonstrict` などはそのファイルだけに効く)。
   - require はパッケージの中だけ(殻と同じ規則 `PackageModulePath`)。無い・外へ出る名前は「require の先が無い」。
   - 「引数に型を付けるとよい」(ExplicitFunctionAnnotationRecommended)は提案なので出さない。
3. **返す表は欄ごとに突き合わせる**(ShapeWalker): 返す表の型(Luau が推論したもの)と型の定義の型を、表の欄ごとに降りる。
   - 省けない欄が無い → 「欄が足りない」、型の定義に無い欄(鍵の型も無い所)→ 「知らない欄」(綴りの誤り)。どちらも欄を書いた行に出す。
   - 葉(数・文字列・鍵の型を付けた表など)は、合成したモジュールの 1 行 `local _n: <型> = value["species"]["water"][...]` で Luau に確かめさせ、
     誤りを葉の欄を書いた行に付け替える(require した先のファイルでも、そのファイルの行)。文には欄の道筋を付ける
     (例 `species.water.formation_enthalpy_j_per_mol: Expected this to be 'number', but got 'string'`)。
   - 整数か・範囲の中かは型で書けないので、今まで通り読み手が見る(ADR-0032)。
4. **出し方**: `TypeDiagnostic`(ファイル・行・列・何が違うか〔日本語の種類〕・Luau の説明〔英語のまま〕)。
   `FormatDiagnostic` = `combustion_test/species.luau:12:9: 型が合わない: ...`。
5. **読み込みへのつなぎ**: `PackageLoadOptions::typeChecker` があれば、LoadPackages はマニフェストを走らせる前に package.luau を、
   entry を走らせる前に全部のファイルを検査する。誤りのあるパッケージ(と依存するもの)は読まない(理由 = 「型検査: 」+ 最初の 3 件)。
   全部の診断は `PackageSetResult::typeDiagnostics`。ランタイムの入り口 LoadReactionTable は必ず検査する
   (ゲーム本体が落ちたら起動を止める・Mod は除いて警告。ADR-0033 の 3 と同じ)。型の定義が読めなければ失敗。
6. **決定性**: 検査のたびに Frontend のモジュールを捨て(`clear`)、診断はファイル → 行 → 列 → 種類 → 文の順に並べて重複を除く。
   型ソルバーの制約の順番を乱す設定(randomizeConstraintResolutionSeed)・時間の上限(moduleTimeLimitSec)は使わない。

## 検討した代案と、採らなかった理由
- 返す表を `local value: PackageEntry = require("init")` で丸ごと確かめる: 上の実験の通り、省ける欄(orders・entry など)を書かない正しい表が落ちる。
  通っても誤りの文が「PackageEntry ではない」だけで、どの欄かもどの行かも出ない。
- 書く人に型注釈を必須にする(`local species: { [string]: Species } = {...}`): 行は正確に出るが、書き忘れると正しいパッケージが落ちる
  (省ける欄の問題)。注釈は任意にした(書けば、その場で Luau が直接見る)。
- 古い型ソルバー(SolverMode::Old): 表の欄の扱いが違う(未確認)。新しいソルバーが Luau の既定の方向なので、新しい方に合わせて欄ごとに降りる形にした。
- 型の定義を C++ に埋め込む: 書く人のエディタにも同じファイルを渡したいので、data/ のファイルにした。

## 影響(何がしやすく/しにくくなるか)
- しやすい: Mod を書く人が、走らせる前に「どのファイルの何行目で何が違うか」を知れる。綴りの誤りの欄・型の誤り・知らない名前・殻で使えないもの
  (os など)・require の誤りが、ベイクの前に分かる。エディタで同じ型の定義を使える。
- しにくい: 起動時に型検査の時間がかかる(debug でテスト全体 1 秒ほど。正式な計測は未)。Luau の版を上げると型ソルバーの判断や文が変わることがある
  (luau_type_check_test が見る)。
- 注意: **読み手(reaction_package・luau_package)の欄を変えたら、型の定義も変える**。変えないと、新しい欄が「知らない欄」になり、
  試験の表のパッケージ(ゲーム本体)が型検査で落ちて起動が止まる(luau_type_check_test と reaction_package が落ちて知らせる)。
