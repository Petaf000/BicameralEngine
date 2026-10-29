# T-1270 測り: 境の面の案 (c) を既定にした時に変わる試験の差と費用(T-1269 の続き。既定はまだ変えない)

- Status: Doing(2026-10-29 wt6・t-1270-border-default-cost)
- 種類: 研究(測りだけ。CPU の試作 time_step_tree の中。土台に足さない)
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列 6(docs/plan/ROADMAP.md。司令塔が行を足す)
- 設計: docs/design/20-transport-substrate.md §14.2 / 決定: D-642・D-669・ADR-0493・ADR-0507(Proposed)

## 目的(1〜2 行)
Q181(予算 0.1 K を「どの歩の終わりでも」に読み替えるか)の答えが (c) なら、期待を取り直すだけで既定を替えられるようにする。
(c) c0 を既定にした時に変わる試験(night・fire・forward・choose_dump・gpu_time_step_choose)の値と費用を、今の既定と並べて表にする。

## 完了条件(チェックできる形で)
- [ ] 試験に `border=<none|mismatch|restart|gap0|c<n>>` を足す(どの場面にも効く。既定 none = 今の既定。毎刻みの側には効かない)
- [ ] night・fire(混ぜた木・1 m・0.5 m)・forward・choose-dump・fire-cost(広い木 big・bigfine)・forward-estimate を none と c0 で流し、差と費用を表に
- [ ] 既定を (c) にした時に直す期待の一覧(どの試験の・どの値が・なぜ)を書く。gpu_time_step_choose は GPU の表(T-1271)が要る
- [ ] docs/perf.md に日付と機械とともに

## 研究の打ち切り条件
測りだけ。広い木で (c) c0 の解きが今の 2 倍を超えたら、その場面の形(結びつきの強い物が広くつながる所)を書いて止め、司令塔へ回す(閾値の見直しは別チケット)。

## メモ・参考
- 毎刻みの写しは wt4 の out/build/release/bin/time_step_tree_reference_*_v4*.bin を wt6 へ写した(作り直しは 645 秒)。
- GPU の刻みを選ぶ段の範囲(MakeTimeTreeChooseTables の supported)は border = None の時だけ(T-1269)。c0 の choose-dump は「範囲の設定」の確かめが落ちる(T-1271 で表を足すまで)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
