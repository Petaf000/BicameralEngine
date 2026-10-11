# T-0351 実験室の細かくした所の熱を陰解法で(QUESTIONS Q40 の B)

- Status: Done(2026-10-11)
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 1 体・2 時間の約束の中
- マイルストーン: M2 (docs/plan/ROADMAP.md。D-449)
- 設計: docs/design/14-editor-tools.md §2「実装(T-0351)」・17 §4 / 決定: ADR-0137(追記 2)・ADR-0019・D-434・D-436・D-428

## 目的(1〜2 行)
細かくできる実験室の箱(refineBlocks > 0)だけ、細かいレベルの熱を陰解法(方式②)で解き、細かい所の熱が本当の速さで伝わるようにする
(Q40 の B。T-0314 から分けたもの)。GPU と CPU は刻みごとにビット一致のまま。

## 完了条件(チェックできる形で)
- [x] `sim::LabStepOptions(shape)`: 細かくできない箱は今まで通り(LAB_STEP_OPTIONS)。細かくできる箱は基準 = レベル 0・陰解法(implicitMaxGap = 2)・上限 `LabImplicitLimits(shape)`
- [x] CPU(StepLabBox)と GPU(GpuLabBox::Create で EnableImplicitConduction・RecordTick)が同じ選択を使う
- [x] lab_box(CPU): 細かい所の熱い点の広がりの増え方が、レベル 0 の理論の値・小刻み(世界の陽解法)と 5% 以内で合い、陰解法なしは遅い。保存がビット単位
- [x] gpu_lab_box(HW・WARP): 細かい所に火を付けた箱(陰解法の刻み)で毎刻みビット一致
- [x] QUESTIONS Q40 に「B で進めた」と書く

## メモ・参考
- 実装: sim/lab_box(`LabStepOptions`・`LabImplicitLimits`・StepLabBox)・sim/gpu_lab_box(Create で EnableImplicitConduction・RecordTick)。
- 測り方(lab_box の TestRefinedHeatSpeed): 熱い点の超過エネルギーの広がり(2 次のモーメント)の 1 刻みの増えは 2αΔt × 3 軸で、陽解法でも後退 Euler でも
  離散の式のまま正確(Σ x² L g = 2 Σ g)。時間の刻み方によらず「熱の伝わる速さ」を比べられる。理論の値 = 細かくしない 3×3×3 の根の箱(レベル 0。頭打ちでない)。
- 基準のレベル(頭打ちにならない最後のレベル): 空気・木・木炭・二酸化炭素・窒素 = 2、薄い空気(1/16)・魔素(試験)= 0。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-11(作業役・本体): 細かくできる箱だけ陰解法(基準 = レベル 0・implicitMaxGap 2・枠が全部入る上限)を CPU と GPU の刻みに入れた。
  試験の表のふつうの材料はレベル 2 まで頭打ちにならない(A でも本当の速さ)と分かったので、頭打ちになる薄い空気で速さを比べた:
  理論 0.7075・陰解法 0.6884・小刻み 0.6881・陰解法なし 0.0469(レベル 0 のセルの辺の 2 乗 / 刻み)。gpu_lab_box(HW・WARP)の細かい所に火を付けた箱が毎刻みビット一致。
- 2026-10-12(作業役・本体): 統合の確かめの release の ctest で gpu_lab_box(HW)が後の部分の実験を作る所で E_OUTOFMEMORY(CreateCommandQueue)。
  陰解法を走らせたキューを捨てた後に新しいキューが作れなくなる(VRAM・コミットは余っている。WARP は通る)と調べて分かったので、
  実験室のキューを使い回す形(sim::PooledImmediateQueue・ADR-0137 追記 3)にして通した。原因の調べは候補(ID 未割り当て)。

## 引き継ぎメモ(HANDOFF に載せる内容)
- 細かくできる箱(refineBlocks > 0)の刻みは `sim::LabStepOptions(shape)`(陰解法)。CPU と GPU で必ず同じものを使う(LAB_STEP_OPTIONS を直接使わない)。
- 細かくできる箱の記録は、この変更の前と後でハッシュが変わる(BLAB 版 6 の記録は同じ形のまま。古い細かくできる箱の記録は残していない)。
- 実験室のキューは `PooledImmediateQueue` で使い回す(陰解法の後に新しいキューを作ると NVIDIA で E_OUTOFMEMORY。ADR-0137 追記 3)。
- 費用は未計測(ROADMAP M2 の候補。ID 未割り当て)。GPU の陰解法の段の VRAM は上限の大きさで先に取る。

## 判断待ち
- Q40 は B で進めた(仮・ユーザー未確認。A に戻すなら LabStepOptions の 1 か所)。

## 分けたもの
- (候補。ID 未割り当て): 陰解法を走らせたキューを捨てると次のキューが作れない件の原因の調べ(ADR-0137 追記 3)。
- (候補。ID 未割り当て): 細かくできる箱の陰解法の費用(1 刻みの GPU 時間・VRAM)を測り、LabImplicitLimits の大きさを見直す。
