# T-0766 GPU の MR の伝導の段を当てる段に載せる

- Status: Done(2026-10-18。wt3。ADR-0343 Proposed)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(T-0773 と同じチャット)
- マイルストーン: ROADMAP「③' 運ぶ計算の共通の土台」段 1(T-0714 から分けた)
- 設計: 20 §2.2・§2.4・§2.7 / 決定: ADR-0334・ADR-0333(Proposed)・ADR-0343(Proposed。この形)

## 目的(1〜2 行)
GPU の MR の伝導の段(shaders/sim/multires_conduct.hlsli)の面の流れを、CPU と同じ当てる段(TxFlowCell + 種類 TxConduction)と
64bit の atomic の世界で出し、二重書きを無くす。コンパイルだけの確かめ(transport_apply_check.hlsl)を消す。GPU の見立ての相の表も。

## 完了条件(チェックできる形で)
- [x] 流れの段は TxFlowCell、印の段は TxFaceFlow(CPU の MarkCellWants と同じ)。世界の約束 GpuConductionWorld(凍らせ・陰解法の面・atomic)
- [x] 見立ては MakeCellViewParts(種類の ViewParts を定数で)。groupshared の写しは種類が読む部分だけ(伝導 = MrThermal の 4 語。T-0111 の大きさのまま)
- [x] GPU の相の表(表の語 t2。common/multires_table_words.hlsli。ルート署名に空きが無いので反応の索引と 1 本)
- [x] transport_apply_check.hlsl・cell_view_check.hlsl を消す
- [x] gpu_multires の伝導・小刻み・陰解法の組(HW と WARP)・^multires_ の CPU・transport_apply・cell_view・archmap --check が通る(ビット一致)
- [x] 費用を docs/perf.md に前後(版ごとの実行ファイルを交互に。揺れの中で同じ)

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-18(wt3 の作業役): 流れの段を TxFlowCell、印の段を TxFaceFlow に。世界 GpuConductionWorld(見立ての写しは種類が読む部分だけ = 16 バイト/セルのまま・
  ブロックの外の隣は自分のセルの外を向く面の写しから探す)。相の表と世界の端は表の語 t2(ルート署名 64/64 語)。check の 2 本を消した。
  **HW の落とし穴**: 共通の multires_bindings.hlsli に表の語の include を足しただけで引き戻しのノード(multires_graph の PullBackNode)の局所の配列が 31 → 35 個になり、
  HW の「たくさんの要求」の影を作る刻みで DEVICE_HUNG(WARP は一致)。古い multires_graph.cso に差し替えると通るので切り分けられた。読みは伝導の段だけが include する形に直した。
  費用は前後で同じ(perf.md)。試験は報告の通り。
