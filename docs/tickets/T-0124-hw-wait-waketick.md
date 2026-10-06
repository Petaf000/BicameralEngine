# T-0124 研究: ハードウェアで待ちの丸めが合わない・活性のグラフを待ちの丸めに

- Status: Todo
- 種類: **研究**(原因が読めない。ドライバのコンパイラの不具合の疑い)
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: ADR-0018 / 前のチケット: T-0121

## 目的(1〜2 行)
GPU の待ちの丸め(T-0121)で、ハードウェア(NVIDIA)だけ RxStepCellWait の戻り値の wakeTick が 0x00000000FFFFFFFF になる原因を見つけて直し、
HW でも CPU と毎刻みビット一致させる(WARP は一致している)。その後、活性のグラフを待ちの丸めにし(T-0121 で WARP は一致・HW で止まったので戻した)、
起こす段(WakeDue)で種を集め、眠っている所の費用を perf.md に(静かな場面を今までの丸めと比べる)。

## 分かっていること(T-0121)
- 症状: 評価して変わらなかったブロックの wakeTick が 4294967295(CPU は 929・193・RX_WAIT_NEVER など)。busyTick・セル・頁は一致。
- 正しく動くもの(HW と WARP で一致): 見出しへの 64bit の書き込み・RX_WAIT_NEVER の書き込み・FxLog2U64・FxMulHiU64・FxDivU128By64・RxWaitTicks・
  同じセルで直接呼んだ RxCollectCandidatesWait の wait。
- 効かなかったこと: groupshared の 64bit の配列を 32bit 2 つに・最小を 32bit ずつ選ぶ・RxStepCellWait の最後の式を min に・RxWaitStep の欄の順。
- 活性のグラフ(multires_activity_graph.hlsl)に multires_wait_step.hlsli を含めて IsCutoffRounding() の分岐を足した版は、HW では今までの丸めの刻みでも
  DXGI_ERROR_DEVICE_HUNG(gpu_multires_activity・_quiet・_uniform。WARP は待ちの丸めで 3 組とも一致)。差分は T-0121 のコミットの前の作業ツリーにあった(ticket の作業ログ)。
  待ちの丸めの全部を刻むを 1 本のリストで 400 回積むと TDR になる(重い。計測の暖機は減らす)。
- 疑い: RxStepCellWait の中の式(RX_WAIT_NEVER − changedTick の比べ・足し算)か、RxWaitStep(RxCell の配列を含む大きい構造体)を返す所のドライバの最適化。

## 進め方と打ち切り条件
1. DXIL を見る(dxc -dumpbin で multires_step_wait.cso の RxStepCellWait の後ろ)。HW の ISA は Nsight で(T-0093 の手順)。
2. RxStepCellWait の最後の数行を、セルの wakeTick だけを返す関数に分ける(大きい構造体を返さない)・式を 32bit ずつに分ける、を 1 つずつ試す。
3. **打ち切り**: 2 を試して直らなければ、HW のテストは今までの丸めのまま(Rounding())にし、ドライバの版と再現の手順を BACKLOG に書いて次(T-0125)へ。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
