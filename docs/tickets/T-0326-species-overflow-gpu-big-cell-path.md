# T-0326 GPU の溢れの世界の全部を刻む段に「大きいセルの道」を足す(溢れるセルだけ少ないスレッドの段で刻む。T-0272 から分けた)

- Status: Todo
- 種類: 工学
- PC: 必須(HW)
- 見積もり: チャット 1 回分
- マイルストーン: T-0272 と同じ(docs/plan/ROADMAP.md)
- 設計: 02 §3・17(R-MULTI-4)・ADR-0052(追記 4・7)。関係: T-0272・T-0273・T-0290・D-448

## 目的
- T-0272 で伝導の段(512 スレッド)から上限の無い形のセル(RxGpuWideCell)を無くした(反応は WideDeferredPass へ回す)。残りの全部を刻む段
  (StepPagedWaitWide・StepExpandedWait の溢れの変種。64 スレッド・1 スレッド 8 セル)と刻み直し(WideRetry)はまだ 1 スレッドが上限の無い形のセルを
  何個も持ち、32 種のセルの頁が並ぶ場面で 1 刻み 17〜176 ms(T-0273)。
- 溢れるセルだけを WideDeferredPass と同じ型の段(印 → 1 グループ = 1 枠、または 1 セル 1 ウェーブ)に回し、全部を刻む段から上限の無い形のセルを無くす
  (ADR-0052 追記 4 の (1)。① の結果を作業場に置いて ③ は写すだけにする (2) も測って選ぶ)。

## 完了条件
- [ ] 全部を刻む段・頁に広げて刻む段の溢れの変種が上限の無い形のセルを持たない(印を付けて後の段へ回す)。CPU の EnableWideCells の世界と HW・WARP が毎刻みビット一致(gpu_multires_wide・gpu_multires_warp)
- [ ] 32 種のセルの計測の場面(--measure-wide)の ns/セル が T-0273(554〜777 / 328〜369)より悪くならない。空気だけの場面も悪くならない(docs/perf.md)
- [ ] HW の compute キューで `--repeat-wide many,many-conduct,limits,limits-conduct 3` が通る

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)

## 引き継ぎメモ
