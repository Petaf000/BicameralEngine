# T-0121 GPU を待ちの丸めに(全部を刻む Compute)

- Status: Done(全部を刻む Compute の待ちの丸めを WARP で CPU とビット一致まで。ハードウェアの不具合と活性のグラフは T-0124、伝導の段・畳み・引き戻しの tc は T-0125)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(作業役 2 時間の約束。ハードウェアの不具合の調べで 1.5 時間に達したので範囲を絞った)
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: docs/design/02-reaction-system.md §3.1・17 §5「活性」 / 決定: D-429・ADR-0018(追記 T-0121)

## 目的(1〜2 行)
GPU の多重解像度(全部を刻む Compute・活性の Work Graph)を待ちの丸め(RxStepCellWait)に切り替え、見出しの tc(busyTick)と起こす刻み(wakeTick)を
CPU の StepNest・StepActive と同じに書く。起こす仕組みは見出しを全部なめる compute。

## 完了条件(チェックできる形で)
- [x] 最初に HW の gpu_multires_* を全部流し、T-0115 の見出しの変更(MrBlock 104 バイト)で壊れていないことを確かめる(release で 8 本通過)
- [x] 起こす段(multires_step.hlsl の WakeDue。1 スレッド = 1 枠): つつかれたブロックを「この刻みに変わった」にし、活性の刻みなら起こす刻みの来たブロックを種の一覧へ
- [x] 全部を刻む Compute(StepWait・StepExpandedWaitPass)が待ちの丸めで刻み、見出しを CPU の StepNest と同じに書く。WARP が CPU と毎刻みビット一致(gpu_multires_warp)
- [ ] (T-0124 へ)ハードウェアで一致: 待ちの wakeTick の上位 32bit が落ちる(下の作業ログ)。直るまでハードウェアのテストは今までの丸めで比べる
- [ ] (T-0124 へ)活性のグラフ(ActivityStepNode・ExpandStepNode・ObserverStepNode)を待ちの丸めに: 分岐を入れた版は WARP で 3 組(activity・quiet・uniform)とも
  CPU と毎刻みビット一致したが、ハードウェアでは今までの丸めの道でもデバイスが止まった(DXGI_ERROR_DEVICE_HUNG)ので戻した。RecordStepActive は待ちの丸めなら false
- [ ] (T-0125 へ)伝導の段を待ちの丸めに・許容差つきの畳みと影の引き戻しで tc を書き直す
- [ ] (T-0124 へ)眠っている所の費用を perf.md に(活性の刻みが待ちの丸めになってから。起こす段の費用だけは測った)

## 決めたこと(Claude。実装の細部。ADR-0018 追記)
- 移行の間、GPU にも今までの丸めを残す(stepFlags の MR_STEP_CUTOFF_ROUNDING = MultiresStepOptions::cutoffRounding)。伝導の段と覗き窓(probe_peek)は今までの丸め。
  ルート定数は足さない(stepFlags の下位 8bit に 2 つ: CUTOFF_ROUNDING = 8・WAKE_SEEDS = 16)。
- 起こす段がつつかれたブロックに直接 busyTick = 印・wakeTick = 印 + 1 を書く(CPU は ResolvePokes で wakeTick = 0 にし、刻んだ後に RecordWaitResults が印 + 1 を書く。
  間に wakeTick を読むのは一様なブロックの評価だけなので、GPU はそこで「busyTick = 印」も起こす刻みが来たとみなす)。
- 待ちの丸めで刻むのは 1 グループ = 1 枠(64 スレッド × 8 セル)。変わったかは groupshared の OR、wakeTick の最小はスレッドごとの最小を下位・上位 32bit に分けて
  groupshared に置き、スレッド 0 が順に見る。一様なブロックの評価(MrUniformWaitOf)も 64 スレッドで分ける(変わるかは OR・最小は同じ)。
- 種の一覧の大きさを 1 + 世界の枠 × 2 + つつかれる上限に(刻みの間の種と起こす段の種で同じ枠が 2 回入りうる。ReadSeeds は並べて重複を消す)。
- GPU の busyTick の印を 64bit(MrChangeMark)に(活性のグラフ・伝導の段・TreeQuiet・TreeFoldCheck)。
- 待ちの丸めの GPU は伝導を入れる刻みをまだ受けない(RecordStep の FX_ASSERT。T-0125)。
- 活性のグラフ用の部品(StepBlockWait・StepExpandedWait・WakeDue の種の一覧への足し込み・MR_STEP_WAKE_SEEDS)は multires_wait_step.hlsli と WakeDue に残した(T-0124 で使う)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-06: 最初に release の HW で gpu_multires_*(8 本: gpu_multires・_implicit・_activity・_quiet・_uniform・_conduction・_subcycle・_near_fold)が通ることを確かめた。
  起こす段・待ちの丸めの刻み(multires_wait_step.hlsli)・活性のグラフの分岐・テストの切り替え。WARP は 4 組とも CPU と毎刻みビット一致。
- ハードウェア(NVIDIA)では、評価して変わらなかったブロックの wakeTick が 0x00000000FFFFFFFF になる(CPU は 929 など)。調べたこと: 見出しへの 64bit の書き込み・
  定数 RX_WAIT_NEVER の書き込みは正しい / FxLog2U64・FxMulHiU64・FxDivU128By64・RxWaitTicks を実行時の値で呼ぶと HW と WARP で一致 / 同じセルで
  RxCollectCandidatesWait を直接呼んだ wait も一致 / それなのに RxStepCellWait(MrStepCellWait)の戻り値の wakeTick は最初のセルから 0xFFFFFFFF。
  groupshared を 32bit に分ける・最小を 32bit ずつ選ぶ・RxStepCellWait の式を min に書き換える・RxWaitStep の欄の順を変える、はどれも効かなかった(書き換えは戻した)。
  → T-0124(研究)。直るまでハードウェアのテストは今までの丸めで比べる(各テストの Rounding())。
- 活性のグラフに待ちの丸めの分岐(multires_wait_step.hlsli を含める)を入れると、WARP は一致したがハードウェアでは今までの丸めの刻みでも DEVICE_HUNG。
  時間の約束で戻した(活性のグラフ・種の一覧の大きさ・GPU の busyTick の 64bit の印も今まで通り)。活性・静か・一様のテストは今までの丸めで比べる。
