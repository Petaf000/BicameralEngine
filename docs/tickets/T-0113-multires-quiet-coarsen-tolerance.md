# T-0113 静かな所を粗くするのも「計器で測れない差だけ」(D-430)

- Status: Done(2026-10-09)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(CPU と GPU)
- マイルストーン: M2(docs/plan/ROADMAP.md)
- 設計: 17 §5「静かなブロックを粗くする」「ほぼ同じ頁を畳む」 / 決定: D-430・D-428

## 目的(1〜2 行)
静かな葉を粗くする(T-0101)のを、粗くして混ざる 2×2×2 のセルの差が許容差(T-0104・Q4)の中の時だけにする。細く撒いた火薬の線・薄い膜・霜の模様が、置いて待っただけで薄まらない。

## 完了条件(チェックできる形で)
- [x] 判定: 葉の頁の 2×2×2 の組ごとに MrFoldStatsWithin(許容差なしなら組の中がビット単位で同じ)。一様な葉はいつも粗くできる(MrCoarsenGroupWithin)
- [x] 判定は静かな葉の忙しさの印ごとに 1 回(見出しの quietCheck に「調べた busyTick の下位 30bit・結果」。毎刻み 512 セルを読まない)。
      粗くできない葉は兄弟の要求を塞がない(MrWantsQuietCoarsen は粗くできる兄弟にだけ譲る)
- [x] CPU(SubmitQuietCoarsenRequests → CheckQuietLeaves)と GPU(TreeQuietCheck → TreeQuiet)が毎刻み一致(HW・WARP)。線の場面(MakeLineNest)で
      2 K 熱い線・O2 が 2 倍の線は 59 刻み待っても残り、組ごとに許容差の中なら今までどおり粗くなる(許容差なし: 葉 2 だけ刻み 17 / D-435: 葉 1・2 が刻み 17・18)
- [x] 許容差なしの振る舞いが変わった: multires_quiet の場面は期待を変えずに通った(燃え尽きた木箱の鎖は組の中がビット単位で同じ。粗くした 59・根だけになった刻み 218 は前と同じ)。
      粗くしたブロックは組がビット単位で同じだったことをテストが自分で確かめる

## メモ・参考
- 許容差の値は QUESTIONS Q4(T-0104)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09: 最初に t-0135・t-0138・t-0025 を合わせた main の release 93 本(39 + 34 + 20)が通過(直したものなし)。
  見出しの padding を quietCheck にし(大きさは同じ)、静かな葉を忙しさの印ごとに 1 回調べる(CPU の CheckQuietLeaves・GPU の TreeQuietCheck〔1 グループ = 枠・スレッド = 組 64〕)。
  許容差は畳むのと同じ値を呼ぶ側が渡す(SubmitQuietCoarsenRequests / RecordQuietRequests の許容差つき。渡さない版は完全に同じ)。ADR-0015 追記(T-0113)。
  線の場面(tests/multires_quiet_scene.h の MakeLineNest)を CPU と GPU のテストに足した。計測は docs/perf.md(T-0113)。
