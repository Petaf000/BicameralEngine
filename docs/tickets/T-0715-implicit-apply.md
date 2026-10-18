# T-0715 陰の伝導(ImplicitGrid)の出力を当てる段へ(段 1 ⑤)

- Status: Done(2026-10-18)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(T-0768 と同じ作業役)
- マイルストーン: ROADMAP「③' 運ぶ計算の共通の土台」段 1 ⑤
- 設計: 20 §2.4・17 §4 / 決定: ADR-0334(Proposed)の 8・ADR-0019

## 目的(1〜2 行)
陰の伝導(多重格子で解く細かいレベルの熱)の出力を、陽の伝導と同じ当てる段(transport_apply.hlsli)で変化の表へ足す。陰解法の値はビット一致のまま。

## 完了条件(チェックできる形で)
- [x] 陰解法の 1 刻みを「面の流れまで解く」(SolveImplicitFlows)と「当てる」に分け、当てるのは当てる段の 1 か所(TxApplyFaceFlow)
- [x] MR の木(AddImplicitConduction)・系だけで解く StepImplicit の両方が同じ当てる段を通る
- [x] ^multires の試験(陰解法の multires_implicit・multires_implicit_tree を含む)・transport_apply がビット一致で通る
- [x] release 警告 0・archmap --check

## 形
- 面の流れは種類(陰の伝導)が面の並びごとに 1 回解いて渡す(V サイクル → 面の流れ → 安全網。今までと同じ)。当てる段の面 1 つの形
  `TxApplyFaceFlow(world, 持ち主, 相手, gap, flow)`: 同じレベルは逆向きに同じ値、違うレベルは粗い側に整数部 + 端数(MrSplitCrossFlow。TxFlowCell の粗い面と同じ)。
  世界の約束は TxFlowCell の Add・HasFraction だけ。
- MR の木: 変化の表へ直接(陽の面と同じ AddConductionDelta)。前は系のセルに足してから「足した後 − 仮のエネルギー」を写していた(同じ 128bit の和なのでビット一致)。
- 系だけで解く StepImplicit(試験・GPU と比べる側): 系のセルを世界にして(ImplicitGridWorld)同じ当てる段で足す。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-18(wt5 の作業役。T-0768 と同じブランチ): 上の形で入れた。transport_apply_test に面の並びの当てる段の試験(同じレベル・違うレベル 1〜3・端数の置き場の有無で
  送った分 = 受けた分)を足した。見つけたこと: 陰解法で解くブロックは陽の流れ(ComputeConduction)を飛ばすので、開いた世界の端(T-0721)の面を誰も流さない
  (陰の系は SAME・COARSER の面だけ)。T-0790 に分けた。
