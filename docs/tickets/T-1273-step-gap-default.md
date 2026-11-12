# T-1273 時間を飛ばす試作の境の面の扱いの既定を (c) Coupling に(Q181 の切り替え・ADR-0507 Accepted)

- Status: Done(2026-11-12 wt4・ブランチ t-1273-step-gap-default。main へは本体が入れる。T-1337 の後に)
- 種類: 工学
- PC: 必須(CPU の試験・GPU の比べ)
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列 6(docs/plan/ROADMAP.md。T-1269・T-1270・T-1271 の続き)
- 設計: docs/design/20-transport-substrate.md §14 / 決定: Q181(2026-11-12 ユーザー「おすすめでいい」= (c) を既定に)・ADR-0507

## 目的(1〜2 行)
刻みを選ぶ単位どうしの刻みの差を「面の結びつきの数(係数 × 2^e ÷ 容量)≤ 1 の e まで 1 段・それより長い e では 0 段」にする形 (c)(TimeTreeBorder::Coupling・couplingShift 0)を既定にする。
精度の予算 0.1 K(D-642)を「どの歩の終わりでも」守る意味に読み替える(歩の終わりの差 11〜13 mK)。

## 完了条件(チェックできる形で)
- [x] 既定の 2 行: `TimeTreeSetup::border = TimeTreeBorder::Coupling`(couplingShift 0)・time_step_tree_test の `borderMode = "c0"`(none = 前の既定は border=none で選べるまま)
- [x] GPU の選ぶ段の表(T-1271 の looseExponents)が既定の choose-dump でも作られ、gpu_time_step_choose(既定の写し)が CPU とビット一致
      (既定の time_step_choices.bin と border=c0 の time_step_choices_coupling.bin がバイト一致〔cmp〕・gpu_time_step_choose 合格)
- [x] CPU: time_step_tree・time_step_span_dump・time_step_event_merge・time_step_choose_dump 系 / GPU: gpu_time_step_span・gpu_time_step_event・gpu_time_step_event_cost・gpu_time_step_choose 系
- [x] 記録したハッシュの取り直し(早送りの要約・火事の系・列のまとめ。下の表)
- [x] release のビルド(警告 0)・archmap --check

## 取り直したハッシュ(既定 = 境 c0・掃き出し 2 回〔T-1337 込み〕。2026-11-12 release・main d9eed9db の上)
| 試験 | 要約 | T-1337 の表(境 none)との比べ |
|---|---|---|
| time_step_tree(forward)早送り・セーブ 1 通り目(木 mixed) | dcc01d2c3f9b0a7d | 変わった(前 db846df407c56dc0) |
| 同じ・退く道 1 本目 | b5c91b4d7b779662 | 同じ |
| time_step_span_dump(火事の系・木 fine) | c43e7d20a0e9b7a9 | 同じ |
| time_step_event_merge(列のまとめ・木 fine) | 3abbf058afcbf3ae | 同じ |
- 火事の系・列のまとめ(木 fine・火事)は境 c0 でも同じ要約 = この場面では面の結びつきが強く、どの単位も隣の差 1 段のまま(T-1270 の「火事 1.003 倍以下」と合う)。
  変わったのは木 mixed の早送りだけ。退く道 1 本目が同じ理由は調べていない(要約が同じことだけを記録。未確認)。
- time_step_tree の試験の時間が 1040 秒(T-1337 の回・境 none)→ 1580〜1690 秒(境 c0 の 2 回)。ほかの作業ツリーのビルド・試験と重なっていたので、
  境 c0 の重さか揺れかは未確認(1 刻みずつ 55.6 秒・一気に 65.5 秒〔1 通り目〕)。重さを見るなら fire-cost の測り(ctest に無い)で。

## メモ・参考
- 作り手から見た違い(D-684): 歩の終わりの差が境の所でも予算の内。静かな夜は軽くなり、火事はほぼ同じ。結果のハッシュは変わる。(本体の版から)
- T-1270 の材料: 夜は写しの刻みでの毎刻みとの差が 5〜6 mK → 11 mK(予算の内)・静かな夜の解きが 0.46〜0.73 倍(軽く)・火事 1.003 倍以下。書き直す試験の期待は無い見込み。
- ctest の time_step_choose_dump_coupling(border=c0)は既定と同じになった(書く .bin がバイト一致)。**仮(司令塔の仮のまま)**: その組を消し、
  代わりに前の既定 None(border=none。表を持たない道)の組 time_step_choose_dump_none・gpu_time_step_choose_none を置いた(本数は同じ。
  None は選べるまま残るので、その道の GPU 版のビット一致も残す)。区画 2 つ以上・池の border=c0 の組は既定と同じ値を明示するだけなのでそのまま。
- 変わるのは時間を飛ばす試作(TimeTreeSetup を使う所)だけ。毎刻みの解き・圧力・支え・伝導は変わらない。
- T-1337(最も粗い段の掃き出し 2 回)の上に重ねた(ハッシュの取り直しを重ねない = このブランチのハッシュは T-1337 込み)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-11-12 本体: ★(claude/48)の反映でチケットを作った(この名前)。作業役の版(T-1273-border-coupling-default.md)と 2 枚になったので、本体 25 回目のマージで作業役の中身をこの名前へまとめた。
- 2026-11-12(wt4・ブランチ t-1273-step-gap-default。t-1337-coarse-sweeps-2 の上): 本体がまだチケットのファイルを作っていないので ★ の文から作った。既定の 2 行を変えてコミット。
- 2026-11-12(wt4 2 本目): ランナーに積まれていた 3 つ(build・time_step 系 6・choose 系 12)が合格。既定の choose-dump と border=c0 の .bin がバイト一致 → c0 の組を None の組に置き換え(仮)。rebase main(d9eed9db)→ release 警告 0 → time_step 系 6(-Show でハッシュ)・choose 系 12 合格・archmap --check。Status Done。
- 2026-11-12 本体(25 回目): main へ --no-ff。チケットを 1 枚にまとめた。
