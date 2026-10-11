# T-0169 テストの試験の反応表を C++ からパッケージへ移す

- Status: Done(2026-10-11。本体)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: M2 (docs/plan/ROADMAP.md。T-0157 から分けた)
- 設計: docs/design/02-reaction-system.md §2.1 / 決定: ADR-0033(追記)・ADR-0032

## 目的(1〜2 行)
試験の反応表の中身を data/packages/combustion_test の 1 か所だけにする。約 40 本のテストが使っていた C++ の写し(engine/src/sim/reaction_test_table)を消し、
テストはパッケージを読む(片方を変えたらもう片方も、をやめる)。

## 完了条件(チェックできる形で)
- [x] テストの `MakeCombustionTestTable` / `MakeSpeciesChangedTestTable` がパッケージの表を読む(tests/reaction_test_table.*。各テストの差分は include の 1 行)
- [x] engine/src/sim/reaction_test_table.* を消す。上限・溢れの試験の表など試験の中で作る特別な表は C++ のまま
- [x] テストの期待値(ハッシュ・ビット一致)は変えずに通る(ベイクした表がビットで同じ)

## メモ・参考
- 定義の並びは名前のバイト順(パッケージの鍵の順)。C++ の表の並びとは違うが、ベイクが名前の順に並べ直すので表は同じ(ADR-0032)。
  並びの番号で定義を触っていた reaction_test の TestBakeRejectsBrokenRules は名前で引く形に直した(番号のままだと「次数 1 の反応物なし」が落ちなくなる)。
- 並走の作業ツリーが新しいテストで `#include "sim/reaction_test_table.h"` を使っていたら、マージの時に `#include "reaction_test_table.h"` に直し、
  tests/CMakeLists.txt の末尾の一覧に exe の名前を足す。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-11(本体・作業役): tests/reaction_test_table.{h,cpp}(ライブラリ bicameral_test_reaction_table。リポジトリの data/packages/combustion_test を Luau で読んで定義を返す。
  関数の名前は前と同じ)を作り、試験の表を使う 37 本のテストは include の 1 行だけ替えた(リンクは tests/CMakeLists.txt の末尾の一覧)。engine/src/sim/reaction_test_table.* を消した。
  reaction_test の壊れた表の試験は名前で引く形に・reaction_package は C++ と比べる試験を「ベイクできて数が合う」に・reaction_package_scene はランタイムの表とテストの表で比べる。
  debug / release のビルドは警告 0。release の試験の表を使うテスト(reaction.*・multires.*・lab_box・gpu_reaction*・gpu_multires*・gpu_lab_box*・gpu_probe_*)が期待値を変えずに通った。

## 引き継ぎメモ
- 新しいテストが試験の表を使うなら `#include "reaction_test_table.h"`(tests/ の中)と、tests/CMakeLists.txt の末尾の一覧に exe の名前を足す。
- 並走の作業ツリーのマージで `sim/reaction_test_table.h` が出てきたら、上の 2 つに直す(engine 側のファイルはもう無い)。
- 試験の表の値を変えるのは data/packages/combustion_test だけ。ハッシュを固定しているテスト(gpu_*・multires_* など)の期待値も変わる。

## 判断待ち
- なし(置き場所・特別な表を C++ のままにするのは実装の細部。ADR-0033 追記)

## 分けたもの
- なし
