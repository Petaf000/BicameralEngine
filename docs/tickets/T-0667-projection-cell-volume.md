# T-0667 気体の圧力の投影(gas_projection・GPU の解き)がセルの空きを読む

- Status: Doing
- 種類: 設計 + 工学(研究の色: 空きの小さいセルを含む式で、熱の多重格子の粗い段が収束するか)
- PC: 必須(GPU の解きのビット一致)
- 見積もり: チャット 1 回分(2 時間で区切る)
- マイルストーン: ROADMAP の T-0210(T-0617 から分けた)
- 設計: docs/design/07-transport.md §2.6・§2.8 / 決定: ADR-0268(D-505)・ADR-0254・ADR-0234 / 関係: T-0503・T-0530・T-0531・T-0617

## 目的(1〜2 行)
投影の解き方は固体の空きを読まず(ProbeGasStepReport::opennessIgnored)、投影を選んだ世界では岩・壁が気体を素通しした。
陽解法(gas_reference・gas_step.hlsl)と同じ空きの数え方で圧力の式を作り、投影でも塞がった所を気体が通らないようにする。

## 完了条件(チェックできる形で)
- [ ] 圧力の式(BeginGasPressure)が空きを読む: セルの容量 C_i と状態の容量(E_i = 基準 × 空き との質量の差)を空きの倍
      (圧力の下限の空き 1/256 は陽解法の DeriveOpenCell と同じ)、面の係数 K_f を面の開き(両側の空きの小さい方)の倍、
      予測の流れ F* の運動量を各側の「開き ÷ 空き」倍・静水圧の項を K_f で。開き 0 の面はつながりにも流れにも入れない
- [ ] 当てる段(FinishGasPressure)が面ごとの K_f で流れを作る(力積は陽解法と同じ ComputeFaceForce〔一部だけ開いた面・気体の無いセル〕)
- [ ] 空きを読まない箱(openQ16 が空)は今までとビット単位で同じ(gas_projection_test・gpu_gas_projection の既存の場面がそのまま通る)
- [ ] gas_projection_test: 塞がった壁(空き 0 の板)の向こうへ 0 µmol・一部だけ開いた孔(空き 1/2・1/256)を通る量は陽解法と同じ向き
      (壁なし > 孔 1/2 > 孔 1/256 > 塞がった壁 = 0)・毎刻みの保存・2 回の実行でハッシュ一致
- [ ] 研究: 空きが 1/256 のセルを含む 12 × 24 × 12 の箱で、内側の解き(V の上限 8)が許容差に入るか(粗い段の係数のまとめ方を比べる)
- [ ] gpu_gas_projection: 同じ空きの場面の解きを GPU で解き直して x と V の回数がビット一致(HW。WARP は軽い組)
- [ ] probe_gas: 投影でも空きを読む(opennessIgnored を消す)・probe_gas_test が通る
- [ ] release のビルドが警告 0・`python3 tools/archmap/archmap.py --check`

## 研究の打ち切り条件(T-0617 のチケットに書いたものを使う。始める前に書いた。2026-10-17)
- 空きが 1/256 のセルを含む 12 × 24 × 12 の箱で、V サイクルの上限(既定 8)の中で許容差(内側 10 mg)に入らなければ、
  「空きが 1/16 より小さいセルの面は投影の式から外し(閉じた面と同じ)、陽解法の閉じ込めと同じに扱う」案に移る。
- 粗い段の係数のまとめ方は「今の熱の多重格子のまま(子の容量の和・面の係数の和 ÷ 2)」をまず測り、上の条件に当たった時だけ
  Galerkin(和のまま)を 1 回比べる。熱の多重格子の部品(implicit_conduction)は wt2(T-0653)が触るので、まとめ方を変えるなら引数で選ぶ形にし、既定は変えない。
- 時間: 2 時間で区切る。残りは引き継ぎメモへ。

## 並走との重なり
- wt2(T-0653 陰解法の安全網・t-0653): implicit_conduction.cpp の MakeBounds(安全網の範囲)と implicit_conduct.hlsl の安全網を触る。
  このチケットは熱の多重格子の段を作る部品(BuildImplicitGrid)と V サイクル(CycleImplicitGrid・GpuImplicit::RecordSolve)を**呼ぶだけ**で変えない予定 → 重ならない。
- wt6(T-0647 gas_reference・gas_tree): gas_reference.cpp の末尾の投影の部品(BeginGasPressure・FinishGasPressure・PredictedFlow)を触る。
  wt6 が陽解法の式(Excess・ComputeFaceFlow・DeriveOpenCell)を変えたら、投影の部品も同じに直す。

## メモ・参考

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
