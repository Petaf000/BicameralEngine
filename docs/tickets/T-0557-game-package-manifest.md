# T-0557 ゲーム本体の名前を外す(マニフェストの game = true)

- Status: Done(仮の決定はユーザー未確認。ADR-0250)
- 種類: 工学
- PC: 必須
- 見積もり: 1 チャット
- マイルストーン: D-471 ⑤(T-0414 の作り心地の記録 1 から)
- 設計: docs/design/13-scripting-game-model.md §2.2 / 決定: D-454・D-486・ADR-0183・ADR-0240・ADR-0250(Proposed)

## 目的(1〜2 行)
ゲーム本体の名前が combustion_test に決め打ちで、標準パッケージの値を書き直せるのは本体だけだったため、場面を自分の名前で作れなかった。
どの場面・ゲームもパッケージの名前によらず本体になれるようにし、T-0414 の場面を ADR-0240 の起動の形(`--packages data/scenes/<場面>`)で開く。

## 完了条件(チェックできる形で)
- [x] マニフェストに `game = true` と書いたパッケージがゲーム本体になる(名前に依らない)。2 つは誤り・形の誤りはそのパッケージを読まない
- [x] サンプルの世界は本体が要る(無ければ止まる)。ブランクの世界は game = true があれば本体、無ければ全部 Mod
- [x] `DEFAULT_BASE_PACKAGE` を消す。サンプル・試験のパッケージに game = true
- [x] 場面のフォルダを `data/scenes/grass_fire_river/grass_fire_river` に直し、`bicameral --packages data/scenes/grass_fire_river` で開ける
- [x] release のビルド(警告 0)・試験(下の作業ログ)・archmap --check

## 作り手から見た違い(判断待ち。仮で案 A。ユーザー未確認)
- **案 A(仮で実装): マニフェストに `game = true`** — パッケージを見れば本体か分かり、配る時もフォルダのまま。起動の仕方は変わらない。
  1 つのフォルダに本体の候補を 2 つ置いて切り替えることはできない(フォルダを分ける)。
- 案 B: 起動の引数 `--game <名前>` — 同じフォルダで本体を切り替えられるが、起動の仕方を覚えて一緒に配る必要がある。
- 案 C: フォルダにパッケージが 1 つだけならそれが本体 — 書くことは無いが、Mod を 1 つ足すと本体が決まらなくなる(暗黙の規則)。
- 取り消し: 本体を決める所は `ResolveGamePackage`(luau_package.cpp)1 か所なので、B・C へも移れる。
- 振る舞いの変化: ブランクの世界に `--packages data/packages` を渡すと、サンプルが本体として読まれる(前は Mod として扱われ読まれなかった)。
- 気をつける所: 本体のマニフェストが形・型で落ちると本体と分からず「読まなかった Mod」になる。ブランクの世界は起動を続けて警告に出す
  (サンプルの世界は「見つからない」で止まる)。

## 作業ログ
- 2026-10-15(wt3): ゲーム本体をマニフェストの `game = true` で決めるようにした(ResolveGamePackage。PackageSetResult・LoadedReactionTable に
  gamePackage。ReactionTableSource は basePackage → requireGame)。サンプル・場面・試験のパッケージに game = true、型の定義に game。
  場面のフォルダを grass_fire_river に直した(git mv)。13 §2.2・ADR-0250(Proposed)。
