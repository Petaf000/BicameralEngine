# T-0007 ログの仕組み(CPU 側)

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分

## 目的
どこかが壊れたときに、どのサブシステムのどこで壊れたかがすぐ分かるようにする(ユーザーの要望 2026-09-29)。
GPU 側のデバッグ出力(リングバッファ)は T-0003。ここは CPU 側と、その出力先の共通化。

## やること(案)
- `engine/src/core/log.h`: サブシステム名つき・重大度つきのログ。`std::format` の書式。例: `Log(Channel::REACTION, Level::WARN, "...", ...)`(`ERROR` は windows.h のマクロと衝突するので使えない)
- HRESULT の失敗を「呼んだ API・ファイル:行・HRESULT の意味」つきで出すマクロ/関数(`std::source_location`)。
- 出力先: コンソール + OutputDebugString(VS の出力ウィンドウ)+ ファイル(out/logs/)。
- caps.cpp の printf を置き換える。
- 依存の向き: 各サブシステム → log の一方向だけ。log は他のサブシステムを知らない。

## 決めること(ユーザー)
- ログの生存期間の持ち方。ユーザー提供の `singleton_template.h/.cpp`(mozc 式)を使う案がある。
  使うなら直したい点: `Finalize()` が map を空にしない(2 回呼ぶと二重 delete)/ 破棄の順番が不定(unordered_map)で、
  依存し合うシングルトンの順序が保証されない / `FinalizeClass` の後で `getInstance` を呼ぶと call_once 済みで nullptr になる /
  名前が規約(docs/style.md)と違う。代案: main が作って参照を渡す、またはログだけ関数静的変数で持つ。

## 完了条件
- [ ] サブシステム名つきのログが 3 つの出力先に出る
- [ ] HRESULT の失敗が API 名と場所つきで出る
- [ ] caps.cpp がログを使う
- [ ] 図(map.yaml)にノードを足す
