# T-0766 GPU の MR の伝導の段を当てる段に載せる

- Status: In Progress(2026-10-18。wt3)
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
- [ ] gpu_multires の伝導・小刻み・陰解法の組(HW と WARP)・^multires_ の CPU・transport_apply・cell_view・archmap --check が通る(ビット一致)
- [ ] 費用を docs/perf.md に前後(版ごとの実行ファイルを交互に)

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
