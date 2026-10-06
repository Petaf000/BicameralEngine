# T-0115 世界を待ちの丸めに切り替える(CPU)

- Status: Done(CPU の多重解像度まで。GPU・仮の世界・起こす compute・RxStepCell と D-424 の下限を消すのは T-0121)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(作業役 2 時間の約束で範囲を絞った。T-0105 の「分けたチケット」の CPU の部分)
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: docs/design/02-reaction-system.md §3.1・17 §5「活性」 / 決定: D-429・ADR-0018(追記 T-0115)

## 目的(1〜2 行)
多重解像度の木の CPU リファレンス(StepNest・StepActive・伝導の段を含む StepBlocks)を待ちの丸め(RxStepCellWait)に切り替え、
見出しに 64bit の tc(busyTick)と起こす刻み(wakeTick)を持つ。活性だけ刻んでも全部刻んだ時と毎刻みビット一致を保つ。

## 完了条件(チェックできる形で)
- [x] 見出し MrBlock: busyTick を 64bit に(= ADR-0018 の tc。印 = 刻み + 1)、wakeTick(64bit)と padding を足す(104 バイト。CPU と GPU で同じ並び)
- [x] StepNest・StepActive・伝導の段・一様なブロックの判定を待ちの丸めに(既定)。種 = 見出しを全部なめて wakeTick が来たブロック + つつかれたブロック
- [x] 既存の CPU のテスト(multires・multires_activity・quiet・uniform・conduction・subcycle)が新しい丸めで通る(活性と全部がビット一致・保存則・2 回一致)
- [x] GPU と比べるテストは今までの丸め(MultiresStepOptions::cutoffRounding)で今までどおり通る
- [ ] (T-0121 へ)GPU(HW・WARP)を待ちの丸めに・起こす compute・仮の世界(ProbeStepCell)・RxStepCell と RX_EXTENT_CUTOFF_FRACTION を消す・眠っている所の費用を GPU で測る

## 決めたこと(Claude。実装の細部。ADR-0018 追記)
- 刻みと tc は**印**(MrChangeMark = 刻み + 1)で RxStepCellWait に渡す。初めの状態(0)は「刻み 0 の前に変わった」。
- つつかれた(木の変更・影を作った)ブロックは、刻む前に busyTick = この刻みの印(静かさの判定は今までどおり)にし、wakeTick = 0(すぐ評価)。
  評価は「刻みの直前に変わった」(tc = 印 − 1)として行い、次の刻みにもう一度評価する(次からは tc = この刻みの印で引き直す。記憶が無いので偏らない)。
- 変わった・頁に広げた・つつかれたブロックは busyTick = この刻みの印・wakeTick = 次の刻みの印(伝導の面の隣も次の刻みに起こすため)。
  評価して変わらなければ wakeTick = セルの RxWaitStep::wakeTick の最小(RecordWaitResults。StepNest と StepActive で同じ)。
- 一様なブロックは wakeTick が来た時だけ、刻むセルを 1 つずつ評価する(MrUniformWaitOf。待ちはセルの ID ごとに違う)。1 つでも変われば頁に広げる。
- 影のブロックは作った時に MR_BUSY_POKED(MrMakeChildBlock。GPU も同じ関数なので見出しは一致)。
- 移行の間だけ MultiresStepOptions::cutoffRounding(今までの丸め)を残す。GPU と比べるテスト・覗き窓(probe_peek。仮の世界の写し)が使う。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-06: 見出しに 64bit の busyTick・wakeTick。StepBlocks に ResolvePokes・一様なブロックの待ちの評価、StepNest・StepActive に RecordWaitResults、
  StepActive の種に見出しを全部なめた「起こす刻みが来た」ブロック(WaitSeedSlots も)。GPU のテストと probe_peek は cutoffRounding。
- 結果(debug): multires・multires_activity・quiet・uniform・conduction・subcycle が新しい丸めで通過(活性と全部が毎刻みビット一致・保存則)。
  GPU との比較(今までの丸め)も通過: gpu_multires_warp・_activity_warp・_quiet_warp・_uniform_warp・gpu_probe_peek(HW・WARP)・gpu_reaction(HW・WARP)。
  計測(CPU。活性の場面 120 刻み): 刻んだブロック 4377(今までの丸めなら 3786。+16%: 最初の刻みに全部を評価する・つついたブロックを次の刻みにもう一度評価する・
  遅い反応が起きる分)。眠っている所の GPU の費用は T-0121。
- 遅くなった: debug の multires_conduction 38 → 153 秒・multires_subcycle 154 → 564 秒(規則ごとに log2 を 2 回と 128bit の割り算。T-0122)。

## 引き継ぎメモ(HANDOFF に載せる内容。司令塔がマージ後に反映)
- 状態: T-0115 完了(CPU)。多重解像度の CPU は既定で待ちの丸め。GPU・仮の世界・覗き窓はまだ今までの丸め(cutoffRounding)。
- 動いているもの: `job.py build`(debug)・`-Filter "^multires"`(CPU 7 本)・GPU の warp の比較(gpu_multires_warp・_activity_warp・_uniform_warp・_quiet_warp)・
  gpu_probe_peek・reaction 5 本・float_check。
- 壊れている/未確認: HW の gpu_multires_*・gpu_multires_conduction・subcycle は流していない(見出しの並びが変わったので T-0121 の最初に流す)。
  許容差つきで畳んだ時(FoldQuietPages の T-0104 の道)は tc・wakeTick を書き直していない(値の差は許容差の中だが「古い乱数・新しい f」。
  GPU の T-0112 と見出しを合わせるため。T-0121 で直す)。影の引き戻し(PullBackShadowChain)も tc を書かない(観察だけ。T-0121)。
- 決めたこと: 上の「決めたこと」(ADR-0018 追記)。
- 次: T-0121(GPU と仮の世界を待ちの丸めに・起こす compute・RxStepCell と D-424 の下限を消す・眠っている所の費用を GPU で測る)。
- 注意: debug の assert は Windows でダイアログを出してテストが止まる(ランナーの上限 30 分まで固まる。最初の版の multires_uniform で 1 回固まった。
  原因は DenseRoots の tc がつつかれたままの値で RxStepCellWait の FX_ASSERT(changedTick < tick) が落ちたと推定〔未確認。直した版は 8 秒で通る〕)。T-0123 でテストの assert を標準エラーに出して止まらないように。
