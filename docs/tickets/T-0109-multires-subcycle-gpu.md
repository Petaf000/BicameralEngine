# T-0109 細かいレベルの熱の刻みを GPU に

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: docs/design/17-multiresolution.md §4・§5「熱の伝導」/ 決定: ADR-0017 追記(T-0108)・ADR-0011(予算)・D-302

## 目的(1〜2 行)
T-0108 の CPU リファレンス(レベルごとの小刻み・粗い小刻みの終わりの受け渡し・小刻みで変わったブロックの隣を起こす)を GPU に載せ、
HW・WARP で状態の全部と次の刻みの種が毎刻み CPU とビット一致する。

## 完了条件(チェックできる形で)
- [x] RecordStep・RecordStepActive が MultiresStepOptions の subcycleBaseLevel・maxSubcycleGap を受ける(今は FX_ASSERT で 0 だけ)
- [x] 小刻みごとに段を分けて投げる(Work Graphs に全体の同期が無いため。CPU は最大回数を積み、要らない回は空で抜ける)
- [x] 鎖・たくさんの要求の場面を分けて刻み、HW・WARP で毎刻み CPU とビット一致
- [x] 計測: 空の DispatchGraph / Dispatch の費用(未確認)・小刻み 64 回の 1 刻みの GPU 時間(docs/perf.md)。予算(ADR-0011)を超えるなら Δkmax を下げる
  → 超えた(鎖 34 ms/刻み)。原因は小刻み 1 回の遅延で工学で縮められる見込みなので、上限は下げずに T-0111 に分けた(ADR-0017 追記)

## メモ・参考
- CPU の順: nest_detail::StepConduction(multires_conduction.cpp)。小刻みごとに 印 → 頁 → 端数の枠 → 流れ(その小刻みに始まるレベルだけ)→
  終わるレベルに変化を足す → 変わったブロックの隣を起こす(WakeAround。刻む集合は増えるだけ)。最後の小刻みの変化は反応と一緒に足す。
- GPU の今の段(T-0107): ConductMark → TreeExpand → TreeFractions → ConductPrepare → ConductFlows → ConductApply(足して反応)。
  ConductApply を「終わるレベルに足す」と「最後に足して反応」に分け、変化(伝導の作業場の頁のセルごとの 16 B)は終わるまで 0 に戻さない。
  起こすのは活性のグラフの WakeFaceNode と同じ道(数える欄 MR_COUNTER_SCHEDULED も一致させる)。
- 面の式は MrSubstepSameLevelFlow・MrSubstepCrossLevelFlow(shaders/common/multires_conduction.hlsli。今の GPU は shift 0 で呼ぶ)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-06: 小刻みごとに段を積む(RecordConduction → RecordConductSubstep・RecordSubstepEnd。新しい段 ConductEnd・ConductEndNode)。小刻みの番号・Δkmax・基準は stepFlags に、
  印は刻みの印 + 小刻みの番号の 2 語に。起こすのは使い終わった種の一覧を空にして活性のグラフをもう一度投げる。テストは gpu_multires_subcycle(_warp)(--subcycle)。
- 2026-10-06: HW・WARP(debug・release)で鎖・たくさんの要求が毎刻み CPU と一致(一発で通った)。計測: 空の小刻み 19〜44 µs・中身のある小刻み約 0.54 ms で
  Δkmax 3 の鎖は 34 ms/刻み → 予算超え。遅延を縮めるのは T-0111 に分けた。段は Compute のまま(D-302)。
