# T-1337 時間を飛ばす試作の最も粗い段の掃き出しを 4 → 2 回に(Q180 の切り替え)

- Status: Done(2026-11-12 wt4・ブランチ t-1337-coarse-sweeps-2。main へは本体が入れる)
- 種類: 工学
- PC: 必須(CPU の試験・GPU の比べ)
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列 6(docs/plan/ROADMAP.md。T-1139 の測りの続き)
- 設計: docs/design/20-transport-substrate.md §14.2 / 決定: Q180(2026-11-12 ユーザー「おすすめでいい」= 2 回)・T-1139

## 目的(1〜2 行)
時間を飛ばす試作(sim/time_step_tree)の列の解きの V の最も粗い段の掃き出し(TimeTreeSetup::coarsestSweeps)の既定を 4 → 2 回にする。
CPU と GPU が同じ回数で解くように口を通し、記録したハッシュを取り直し、V 1 回の費用を測る。

## 完了条件(チェックできる形で)
- [x] 既定の 1 か所: `TimeTreeSetup::coarsestSweeps = 2`(毎刻みの解き〔ImplicitOptions・EquilibriumOptions の既定 4〕は変えない)
- [x] GPU の部品(GpuEquilibriumUpload)に掃き出しの回数の口(`GpuEquilibriumUploadOptions::coarsestSweeps`。既定は ImplicitOptions の 4 のまま)。
      列を見る口(TimeTreeSpanSolve)・列の系のファイル(版 3)に回数を持たせ、gpu_time_step_span_test が同じ回数で CPU・GPU を解く(`--coarsest n` で上書き)。
      起動時の費用の測り(TimeTreeCostProbeOptions::coarsestSweeps = TimeTreeSetup の既定)も同じ回数で
- [x] CPU: time_step_tree・time_step_span_dump・time_step_event_merge・time_step_choose_dump 系 / GPU: gpu_time_step_span・gpu_time_step_event・gpu_time_step_event_cost・gpu_time_step_choose 系
- [x] 記録したハッシュの取り直し(下の表)・V 1 回の費用(perf.md 2026-11-12)
- [x] release のビルド(警告 0)・archmap --check

## どの系の結果が変わるか(多重格子の段は共通なので)
- **変わる**: 時間を飛ばす試作(time_step_tree)の列の解きだけ(TimeTreeSetup を使う所 = time_step_tree_test・gpu_time_step_span_test・gpu_time_step_choose_test の入力の写し)。
  起動時の費用の式の値(D-662。gpu_time_tree_cost)は V が軽くなる分だけ小さく測れる(値の比べは試験に無い)。
- **変わらない**: 圧力(gas_projection)・支え(equilibrium・equilibrium_block の既定 8 回)・伝導(implicit_conduction・multires_implicit_conduction)・case_b の圧力。
  これらは ImplicitOptions / EquilibriumOptions の既定(最も粗い段 4 回)をそのまま使い、この変更は既定の構造体に触らない。
- 毎刻みの写し(time_step_tree_test の比べの相手。V 4・掃き出し 4)はファイルの名前どおり 4 回のまま。

## 取り直したハッシュ(掃き出し 2 回の既定。2026-11-12 release)
| 試験 | 要約 |
|---|---|
| time_step_tree(forward)早送り・セーブ 1 通り目(木 mixed) | db846df407c56dc0 |
| 同じ・退く道 1 本目 | b5c91b4d7b779662 |
| time_step_span_dump(火事の系・木 fine) | c43e7d20a0e9b7a9 |
| time_step_event_merge(列のまとめ・木 fine) | 3abbf058afcbf3ae |
- T-1139 の表の「×1・2 回」の要約 29e092b50d262c7a(fire-cost)が新しい既定の値(fire-cost は ctest に無いので流していない)。
- V 1 回の費用(perf.md): 本物の記録の形で 0.65〜0.73 倍・1 出来事(段の重みを含む)0.75〜0.84 倍(見込みの「約半分」より小さい = 段 0・重み・Dispatch の固定の分)。

## メモ・参考
- 作り手から見た違い(D-686): 温度の差は最大 0.27 mK(倍率 ×10 の場面)。燃える前線の位置と燃えた量は同じ。世界のハッシュは変わる。(本体の版から)
- T-1139 の測り(火事 18 分・shape=fine): 2 回は 4 回との差が ×1 で 0.0002 mK・×10 で 0.27 mK。前線・燃えた量は同じ。
- `TimeTreeSolveCost` の既定(RTX 3070 Ti の測りの値)は 4 回の時の値のまま(起動時に測る口が上書きする)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-11-12 本体: ★(claude/48)の反映でチケットを作った(この名前)。作業役の版(T-1337-coarsest-sweeps-default.md)と 2 枚になったので、本体 25 回目のマージで作業役の中身をこの名前へまとめた。
- 2026-11-12(wt4・ブランチ t-1337-coarse-sweeps-2。main 94b20ace の上): 本体がまだチケットのファイルを作っていないので、★ の文から作った(本体とぶつかったら本体の版を正に)。
- 2026-11-12(wt4・続き): release 警告 0・time_step_tree・time_step_span_dump・gpu_time_step_span・time_step_event_merge・gpu_time_step_event・gpu_time_step_event_cost・time_step_choose_dump 系 6 本・gpu_time_step_choose 系 6 本・archmap --check 合格。V 1 の費用を 4 回と 2 回で同じ系に(perf.md)。ハッシュは上の表。
- 2026-11-12 本体(25 回目): main へ --no-ff。チケットを 1 枚にまとめた。
