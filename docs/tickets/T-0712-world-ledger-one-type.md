# T-0712 世界の帳簿を 1 つの型にする(運ぶ計算の共通の土台の段 1 ②。MR の CPU から)

- Status: Done(CPU。GPU は実験室の箱の改造と畳む段の繰り上げだけ。下の「残り」)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(2 時間)
- マイルストーン: 運ぶ計算の共通の土台 段 1 ②(runner/design/substrate-decided.md §2.4 ⑤・§6)
- 設計: substrate-decided §2.4・survey-engine §3「帳簿と保存の検査」/ 決定: 2026-10-17 ユーザー(土台・決定 4)/ ADR-0331

## 目的
帳簿を 1 つの型(列 = 量 × 理由)にし、保存の検査を「Σ 登録した保存量 − 帳簿 = 一定」の 1 関数にする。まず MR の CPU の今の出入りを載せる。

## 完了条件
- [x] 共有 .hlsli の型(shaders/common/world_ledger.hlsli): 理由 8 × 向き 2 × レベル × 列。整数・決定的・CPU と GPU が同じ番地
- [x] 検査の 1 関数: ComputeConservedTotals = Σ セル − (入 − 出)(engine/src/sim/multires_nest.cpp)
- [x] MR の CPU の出入りを載せる: 粗くした端数(欄 0。前の帳簿と同じ番地)・改造(実験室の箱の置く/温度のコマンド)
- [x] 今の MR の保存の試験が新しい関数で通る(release: ^multires_ 8 本・lab_box)。lab_box はコマンドのある刻みも合計が同じ・細かくする→置く→燃やすを通して刻み 0 と同じ、に強めた
- [x] GPU の実験室の箱(lab_box.hlsl)も改造を同じ欄へ(CPU とビット一致は gpu_lab_box で確かめる)
- [x] 今ある帳簿の型をどう寄せるか(下の「寄せ方」)

## 作り(要点。詳しくは ADR-0331)
- 番地 = [欄 = 理由 × 2 + 向き][レベル − MR_LEDGER_LEVEL_MIN][列]。向きは出・入の 2 本の符号なし(GPU は atomic の足し算だけ)。値はそのレベルの単位 × 2^-64、
  丸ごとの量は 21・22 段粗い段へ(段の下限 -16 → -24)。列 = 0 エネルギー・1 + 物質 ID・その後ろに登録した欄の保存量(T-0711)。
- 改造は「前の中身を全部 出 + 後の中身を全部 入」(差を取らない)。
- MR に無い出入り: 物理の熱(MR に物理がまだ無い。段 8)・世界の端・壁の力積・重さの仕事・速さの上限・静止の丸め(気体・液体が MR に載る段 2〜4)。理由の定数だけ先に置いた。

## 寄せ方(今の型 7・和 9・GPU 5 → この型)
| 今の型 | 中身 | 新しい欄 | 段 |
|---|---|---|---|
| MultiresNest::ledger(MR) | 粗くした端数 | 粗くした端数・出(欄 0。済み) | 済み |
| GasLedger(gas_reference)/ ProbeGasLedger(probe_gas) | 物質・エネルギー = 開いた境界の出入り + 重さの仕事、運動量 | 物質・エネルギーの開いた境界 → 世界の端(符号で出/入)・重さの仕事 → 重さの仕事(エネルギーの列。今は 1 本に混ざっているので分ける)・運動量 → 壁の力積 / 世界の端 | 3 ①②(共有 .hlsli へ移す時。PS の区画の帳簿は段 4 で消える) |
| ProbeLiquidLedger(probe_liquid) | 外からの運動量(壁・重さ・摩擦・空になった面・速さの上限)・位置のエネルギーの出入り | 壁 → 壁の力積・重さ → 重さの仕事・摩擦(底と粘り)→ 壁の力積(地面が受ける)・速さの上限 → 速さの上限・空になった面 → **9 つ目の理由「欄の合わせ直し」を仮で足す**(下の判断待ち 2)・位置のエネルギー → 重さの仕事 | 2 ③ |
| GasTreeTotals(gas_tree。2^-64) | 合計そのもの | 寄せない(気体の木は凍結した答え合わせの基準。段 7 の後に退く) | — |
| PhysicsEnergyLedger・PxHeatLedger | 物の力学的エネルギー・失った分 | 物の力学的エネルギーを登録の保存量にする(物理は登録の 1 項)。セルへ渡す熱は MR だけを数える時「物理の熱・入」 | 8(物理の結合を MR へ) |
| ProbeReference::SourceEnergy・PhysicsHeatEnergy(PS) | 足した熱・物理の熱 | 改造・入 / 物理の熱・入(SourceEnergy の中身は未確認: 熱源のコマンドなら改造) | 8(PS を退かせる時に消える) |
| 和 9(SumGas・SumGasTree・ComputeConservedTotals・SumPhysicsEnergy・ProbeEnergySum・液体の位置/運動のエネルギー・運動量の和・気体の運動のエネルギー) | | ComputeConservedTotals 1 つ。登録した欄の保存量は「登録の欄ごとの和」を回る(T-0711 の登録に「和を取る関数」を 1 つ持たせる) | 2〜3 |
| GPU 5(gasWork・GpuGas・GpuGasTree・liquidWork・BufferLedger) | | BufferLedger 1 つ(この型)。ほかは段 6 で GPU に載せる時に消える | 6 |

## 状態の登録(T-0711・wt3)との合わせ方
- 登録の欄の「保存量か(帳簿の列)」が true の欄は、帳簿の列 1 + 物質の数 + k(登録の順)を持つ。列の数 = MultiresCapacity::ledgerColumns は登録から作る。
- 登録が欄ごとに「その欄の保存量の和」を出す関数を 1 つ持てば、ComputeConservedTotals は セル(エネルギー・元素)+ 登録の欄 − 帳簿 を回るだけ(ConservedTotals に欄の列を足す)。
- wt3 の t-0711(2026-10-17 16:55 に読んだ作業中の版): 登録 SfFieldDesc に SF_FLAG_CONSERVED があり、VOLUME の欄を粗くした余りを
  欄ごとの「欄の帳簿」(MultiresField::ledger。int64・[MR_LEDGER_LEVELS × 値の数]・MrLedgerAddress)に置いている(「T-0712 に入るまでの置き場」とコメントあり)。
  **合わせ方(マージの後に 1 本)**: SF_FLAG_CONSERVED の欄の値 c を世界の帳簿の列 1 + 物質の数 + (欄の保存量の通し番号)に。余りは符号つきなので
  WlEnergyEntry と同じく正 → 粗くした端数・出、負 → 粗くした端数・入(WlSlot(WL_REASON_COARSEN_REMAINDER, 向き))、
  余りは子のレベルの単位の丸ごとの数なので AddWorldLedgerUnits で。MultiresField::ledger と LoadNestFieldLedger は消す。
  ledgerColumns は「1 + 物質の数 + 保存量の欄の値の数」を登録から作る。両方とも MR_LEDGER_LEVELS を使うので、この版の段の下限 -24 はそのまま効く。

## 判断待ち(仮〔ユーザー未確認〕で進めたもの。どれも取り消しやすい)
1. **欄はいつも 16 本持つ**(作り手から見た違い: なし。費用だけ。実験室の箱で帳簿が約 230 KB・毎刻み読み戻してハッシュする)。重ければ「使う理由の欄だけ持つ」に変える(番地の関数 1 つ)。おすすめ: 今のまま測ってから。
2. **理由に「欄の合わせ直し」(相の質量が変わって消えた運動量)を 9 つ目に足すか**: 作り手から見た違い: 上限の計器(§1.4)で「水が蒸発して消えた勢い」が速さの上限と分かれて見える。おすすめ: 足す(段 2 ③ で。今は定数を置いていない)。
3. **表の差し替えで帳簿を付け替えない**(作り手から見た違い: 物質の一覧が変わる表に替えた刻みの前後で保存の検査を比べられない〔元素の定義が変わるので元々比べられない〕)。名前で付け替えるのは段 8。

## 残り(司令塔へ)
- GPU は畳む段(AddLedgerCarrying)と実験室の箱の改造だけがこの型を書く。GPU の確かめは gpu_lab_box・gpu_multires_[qu] の 2 組だけ回した(通った)。gpu_multires の鎖・wide の組は回していない(欄 0 の番地は前と同じ・帳簿の大きさだけ変わった)。
- 読み戻し・ハッシュの費用(帳簿が 16 × 88 × 列 語)は未計測。

## 作業ログ
- 2026-10-17(wt4・作業役): world_ledger.hlsli を作り、MR の帳簿を欄 0 に・保存の検査を Σ − (入 − 出) に・実験室の箱の改造を CPU と GPU で載せた。
  段の下限 -24。release のビルド警告 0・^multires_ 8 本・lab_box が通った。ADR-0331。
- 同: GPU(release)gpu_lab_box(gpu・warp)・gpu_multires_quiet / uniform(gpu・warp)が通った(CPU とビット一致。改造の欄と畳む段の繰り上げ)。wt3 の登録との合わせ方を書いた。
