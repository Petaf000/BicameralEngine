# T-0124 研究: ハードウェアで待ちの丸めが合わない・活性のグラフを待ちの丸めに

- Status: Done(2026-10-06)
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
   (2026-10-06 の作業役の約束: 2 時間で、①debug(-Od)と release の差・DXIL を見る ②式の形を変える ③活性のグラフの止まり方を二分する、まで。
   ③で原因の場所が絞れなければ、グラフは今までの丸めのままにして最小の再現を残す)

## 結論(2026-10-06)
- **症状 1(wakeTick の下位 32bit が 0xFFFFFFFF)**: 原因は RxStepCellWait の最後の式 `wait >= RX_WAIT_NEVER − changedTick ? RX_WAIT_NEVER : changedTick + wait`
  (DXIL では `select(icmp uge(wait, xor(c, −1)), −1, add(wait, c))` = 飽和する足し算の定形)。release(-O3)の DXIL は正しいが、HW では下位 32bit が
  0xFFFFFFFF・上位は 0 か 4 になる。debug(-Od)の DXIL なら HW でも CPU と一致した。→ NVIDIA のドライバのコンパイラがこの定形を 64bit で正しく下ろせない(推定。
  ドライバの中は見ていない)。T-0121 で試した `min` の形(umin(wait, ~c) + c)も同じ定形なので効かなかった。
  **回避**: reaction.hlsli の RxWakeTickOf(`wait == RX_WAIT_NEVER ? RX_WAIT_NEVER : changedTick + wait`)。待ちも印も 2^62 未満なので溢れない
  (FX_ASSERT。R8「飽和しない」にも合う)。CPU の結果は変わらない(到達する値の範囲で同じ式)。
- **症状 2(活性のグラフで DEVICE_HUNG)**: 二分した結果(どれも HW の release、gpu_multires_activity):
  ① 待ちの刻みのセルの評価を空にした(反応の核を呼ばない)版 → 止まらない ② 待ちの刻みの中身を今までの丸めの MrStepCellDetailed にした版 → 止まる
  ③ groupshared の配列をやめて 64bit の atomic の最小にした版 → 止まる ④ 今までの丸めの反応(StepBlockCells・MrUniformWouldChange)をグラフから除き、待ちの丸めだけにした版 → 止まらず CPU と一致。
  debug(-Od)でも止まる。→ 待ちの式ではなく、**反応の核(RxStepCell 系)を 1 つのノードに 3〜4 か所展開すると止まる**(ActivityStepNode が 今までの一様 + 今までの頁 + 待ちの頁 + 待ちの一様)。
  コードの大きさ・局所配列(RxCell・候補の [16 x i64])のスクラッチの量のどれかがドライバの Work Graph のノードの扱いに触れる、と推定(未確認。Compute のシェーダーは同じ数でも動く)。
  **回避**: 活性のグラフの反応は待ちの丸めだけにした(今までの丸めは伝導を入れる刻み〔反応はグラフで刻まない〕だけ。T-0122 で消す予定のもの)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-06: 症状 1 を debug(-Od)と release の差と DXIL で絞り、飽和する足し算の形を RxWakeTickOf に替えて直した(HW の gpu_multires が待ちの丸めで一致)。
  症状 2 は 4 通りに二分し、反応の核を 1 つのノードに 3〜4 か所展開すると止まることまで(原因は推定)。活性のグラフを待ちの丸めだけにし、
  起こす段 WakeDue が種の一覧に足す形に(種の一覧を 1 + 世界の枠 × 2 + つつく上限に)。HW のテスト(多重解像度・活性・静か・一様)を待ちの丸めに戻した。
  眠っている所の費用を perf.md に(静かな刻み 0.42 ms。毎刻み全部を刻む観察の枠 4 つの評価が大半と推定。待ちの評価を軽くするのは T-0123)。
