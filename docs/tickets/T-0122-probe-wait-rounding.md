# T-0122 仮の世界と覗き窓を待ちの丸めに(D-424 を置き換える)

- Status: Done(2026-10-06。今までの丸めのコードを消すのは T-0130 に分けた)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: 02 §3.1・06 §2 / 決定: D-429・ADR-0018 / 前のチケット: T-0115・T-0121・T-0124・T-0125

## 目的(1〜2 行)
仮の世界(probe_world.hlsli の ProbeStepCell。CPU リファレンスと GPU の ConductBlock)と覗き窓(probe_peek の入れ子)を待ちの丸め(RxStepCellWait)にし、
世界のどこも D-424 の下限を使わないようにする。眠ったブロックは次に評価の要る刻みに起こす。

## 完了条件(チェックできる形で)
- [x] 仮の世界が待ちの丸めで刻み、HW・WARP が CPU と毎刻みビット一致(gpu_probe_sim・_trace・_physics・_peek・_fire・window_replay)
- [x] 眠っているブロックが待ちの来た刻みに起き(WakeDueBlocks)、CPU と一致する試験(gpu_probe_sim の「遅い反応」: 一様な木の壁の世界 340 K で 7 ブロック)
- [x] 覗き窓の入れ子も待ちの丸め(cutoffRounding を渡さない)
- [x] D-424 を「置き換えた」に(DECISIONS・02 §3.1)
- [ ] (T-0130 へ)cutoffRounding・RxStepCell・RxScaleExtent・RX_EXTENT_CUTOFF_FRACTION を消す

## 決めたこと(Claude。実装の細部。ADR-0018 追記〔T-0122〕)
- tc と起こす刻みは仮の世界の活性のブロック(4³)ごとに 64bit の印(ProbeChangeMark = 刻み + 1)。予定の印のバッファ(u11)の後ろに置く(ルート署名は変えない)。
- つつきは tc = 刻み t の印 − 1(刻みの直前に変わった)。計算したブロックは変わったら tc = 印、変わったか次の刻みに評価が要れば次の一覧へ(起こす刻み = RX_WAIT_NEVER)、
  ほかは起こす刻み(セルの最小。32bit 2 回の最小)を書く。適用の単位の後の WakeDueBlocks が起こす刻みの来たブロックを一覧へ(一覧の容量は変わらない)。
- 初めの起こす刻みは CPU が作って写す(ProbeInitialBlockWakes: 刻み 0 を tc = 0 で計算してみる)。PROBE_BLOCK_FLAG_POSSIBLE は「次の刻みに評価が要る」。
- 試験用に ProbeSimOptions::initialWorld・ProbeReference の初めの世界を足した。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-06: 2 時間の約束で、古いコードを消す所を T-0130 に分けた。仮の世界の GPU は blockSchedule の後ろに tc と起こす刻み・WakeDueBlocks を足し、
  ConductBlock は RxStepCellWait(1 ノードの反応の核は 1 か所のまま)。元のテスト世界は 1500 刻みでも待ちで起きるブロックが 0(熱が広がり続けて眠らない)ので、
  一様な木の壁の世界を遅い反応が数十刻みで起きる温度(探して 340 K)にした試験を足した。release で gpu_probe・window_replay の 13 本、debug で WARP の 3 本が通過。tidy(release)は前からの 1 件だけ。作業 約 1 時間 15 分。
