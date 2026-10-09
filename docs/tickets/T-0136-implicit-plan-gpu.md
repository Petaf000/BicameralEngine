# T-0136 陰解法の節の並び・ImTail の境・間接の Dispatch の引数・GpuImplicit の大きさを GPU で決める(この版)

- Status: Done(費用が小さい場面で増えた分を減らすのは **T-0154** に分けた)
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 2 時間以内(始める前の見積もり: 書き直しは implicit_conduct.hlsl・gpu_implicit.cpp の V サイクルの記録と、写す口
  〔GpuImplicitBuild・GpuImplicitLevels〕だけで 1.5 時間ほど。費用を減らす工夫は測ってから別チケット、と決めて入った。実際は約 1.7 時間)
- マイルストーン: M2 の並走(D-432)
- 設計: docs/design/17-multiresolution.md §6 / 決定: D-428・D-432・D-434・D-436 / ADR-0019(T-0136 の追記)

## 目的(1〜2 行)
GpuImplicit の V サイクルの記録を、CPU の知る段の形(m_levelOffsets・節の並び・m_tailDepth)から、GPU のバッファの段の表と ExecuteIndirect の引数から
読む形に書き直す。伝導の段から陰解法を呼ぶ(T-0132)には、CPU が系の形を知らないまま刻めることが要る。

## 完了条件(チェックできる形で)
- [x] GpuImplicit を上限(GpuImplicitLimits)から作れる。作業場の番地は上限、数(セル・面・段の数・段ごとの節)は計画のバッファ(u9)から読む
- [x] 刻みの初めに GPU が長い行の節の一覧(色 0・色 1・縮約)・ImTail の境・全部の間接の Dispatch の引数を作る(ImPlanLevels・ImPlanArgs)
- [x] V サイクルは記録の上限の段まで ExecuteIndirect で積む(要らない段は 0 グループ)。セル・面の段も間接の引数
- [x] GPU の系(GpuImplicitBuild・GpuImplicitLevels)を写して、CPU の系を一度も写さずに(RecordReset・上限は系の 2 倍)解いた結果が
      CPU と毎刻みビット一致(熱い点・鎖・たくさんの要求。release の HW・WARP / debug の WARP)
- [x] CPU の系を写す試験(gpu_multires_implicit・gpu_multires_implicit_tree)も今までどおり一致。GPU が決めた ImTail の境は前の CPU の判定と同じ(計測の表)
- [x] 前後の費用を測る(下の「結果」。負荷あり)
- [ ] 小さい場面で増えた固定費(空の間接の Dispatch とバリア)を減らす → **T-0154**

## 何をしたか(形。決めたのは Claude〔実装の細部〕。ADR-0019 の T-0136 の追記)
- 計画(u9)の先頭は implicit_levels.hlsl の見出しと同じ並び(段の数・段ごとの節の始まりと数)。GpuImplicitLevels::RecordCopyTo が見出しをそのまま写し、
  GpuImplicitBuild::RecordCopyTo が面の数の 1 語を写す。CPU の系は RecordUpload が同じ見出しを書く(static_assert で並びを縛った)。
- 子の一覧は番号の一覧の「面の上限 × 2」の後ろに置き、ChildAt で `2 × 面の上限 + k − 2 × 面の数` に直して読む(childStart は CPU と同じ値のまま)。
- 短い節は並べ直さない: 段の節の範囲を 1 スレッド = 1 節で回し、節の印(隣 > 16・子の隣の最大 > 16)で長い行の節だけ飛ばす。長い行の節は後ろのグループで
  一覧から(番号の昇順。塊ごとの印の bit の順位)。止める判定は色 0 → 色 1 の長い節の一覧を続けて読む。
- V サイクルは再帰を記録の上限の段(GpuImplicitTuning::dispatchLevels、既定 8。Create(grid) は min(8, 段の数))まで開いて積む:
  下り × 段 → ImTail → 最も粗い段の掃き出し(段は計画から)→ 上り × 段。下りがその段より深くなる系は ImTail が受け持つ(値は同じ)。
- API: Create(device, limits, tuning) / Create(device, grid, tuning)・LimitsOf・RecordReset・RecordCopySystem(…, header, faceCountOffset)・
  RecordCopyLevels(…, header, …)。TailDepth()・LevelCount() は消し、GPU が決めた値は GpuImplicitCost の levelCount・tailDepth で読む。
- 試験: gpu_multires_implicit_build_test は GpuImplicit を上限(系の 2 倍 + 16・段 64)から作り、RecordReset の後 GPU の系と段だけを写して解く(CPU の系を写さない)。

## 結果(2026-10-09、release、RTX 3070 Ti。**本体と wt3 のランナーが長い試験中の負荷あり**。docs/perf.md には入れていない)
`job.py run -Preset release -Exe gpu_multires_implicit_tree_test -- --queue compute --measure-only`(T-0127 と同じ場面。暖機 100 刻みの後、5 回の最小)。
前 = main(8fd2aca)・後 = この版。同じ時間帯に 1 回ずつ。ms/刻み、初めの 12(8)刻みの平均 / 後の平均。
| 場面(上限 16・ImTail 1024・32) | 段 / ImTail の境 | 前(V 1 回の Dispatch) | 後(V 1 回の Dispatch〔空を含む〕) |
|---|---|---|---|
| 熱い点(512 セル) | 4 / 0 | 0.26(3) | 0.62〜0.82(41) |
| 鎖(1907 セル) | 5 / 4 | 1.82 / 1.54〜2.90 / 2.57(43) | 2.97〜2.99 / 3.03〜3.88(51) |
| たくさんの要求(2 万セル) | 8 / 8 | 8.0〜9.3(80) | 9.97〜10.1(81) |
- ImTail の境(段 0 / 4・4 / 5・2 / 5〔隣 128〕・0 / 5〔2048〕・8 / 8・7 / 8)は全部の形で前の CPU の判定と同じ。
- 熱い点は全部の段が ImTail なのに、記録は 4 段ぶんの空の間接の Dispatch(V 1 回 38 個)とバリアが残るので +0.35〜0.55 ms。鎖・たくさんの要求は負荷のぶれ
  (同じ形の 2 回で 1.8〜2.9 ms)より差が小さく、はっきりしない。idle で取り直すなら上のコマンドを前後で(T-0154 で)。
- 計画の 2 Dispatch(ImPlanLevels 64 グループ・ImPlanArgs 1 スレッド)は刻みごとに 1 回。

## 判断待ち
- なし(記録の形・計画の作り方は実装の細部。解いた値は前と同じで、遊びへの影響なし)。

## 分けたもの(新しいチケット。ROADMAP には司令塔が足す)
- **T-0154**(工学): 陰解法の V サイクルの空の間接の Dispatch の固定費を減らす。段の数を GPU が決めるので、記録は上限の段まで積み、使わない段も
  0 グループの ExecuteIndirect とバリアが残る(熱い点で V 1 回 3 → 41 Dispatch・+0.35〜0.55 ms〔負荷あり〕)。案: (a) dispatchLevels を伝導の段
  (T-0132)の場面ごとに選ぶ・(b) 下りが止まった後の段を ImTail と同じ 1 グループの「上り下りの残り」にまとめる(全部 ImTail の場面は V 1 回 3 Dispatch に戻る)・
  (c) V サイクル全体を Work Graphs に。idle で前後を測って選ぶ。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役、wt2・約 1.7 時間): 計画のバッファ(段の形の見出し・節の印・長い行の節の一覧・段の表)と間接の引数を GPU で作る ImPlanLevels・ImPlanArgs を足し、
  V サイクルを記録の上限の段まで ExecuteIndirect で積む形に書き直した。GpuImplicit は上限から作り、GPU の系と段を写すだけで解ける(RecordReset)。
  debug の WARP・release の HW と WARP で gpu_multires_implicit(_tree・_build)(_warp)が全部通過(CPU と毎刻みビット一致)。
  負荷ありの計測で、全部 ImTail の小さい場面の固定費が増えた(T-0154 に分けた)。debug の HW・tidy は時間の約束で流していない。

## 引き継ぎメモ(HANDOFF に載せるもの。司令塔がマージ後に反映)
- 状態: T-0136 完了。GpuImplicit の V サイクルの形(段の数・節の範囲・長い行の節・ImTail の境・Dispatch の大きさ)を GPU のバッファから読む。
  GpuImplicit は上限(GpuImplicitLimits)から作り、GPU の系(GpuImplicitBuild)と段(GpuImplicitLevels)を写すだけで解ける。CPU と毎刻みビット一致。
- 動いているもの: `-Filter "^gpu_multires_implicit(_tree|_build)?(_warp)?$"`(release の HW・WARP 6 本 約 19 分〔負荷あり〕/ debug の WARP 3 本 約 6 分)。
  計測は `job.py run -Preset release -Exe gpu_multires_implicit_tree_test -- --queue compute --measure-only`。
- 壊れているもの: なし。
- 決めたこと(Claude・実装の細部): ADR-0019 追記(T-0136: 上限から作る・計画の見出しは implicit_levels.hlsl の見出しと同じ並び・子の一覧の番地の直し・
  短い節は並べ直さず長い行の節だけ一覧・記録の上限の段まで開いた V サイクルを ExecuteIndirect で)。
- 判断待ち: なし。
- 注意: 全部 ImTail の小さい場面は空の間接の Dispatch の分だけ重い(熱い点 0.26 → 0.6〜0.8 ms、負荷あり。T-0154)。GpuImplicitTuning::dispatchLevels
  (既定 8)より下りが深い系は、そこから下を ImTail が受け持つ(値は同じ・遅くなりうる)。ルート署名は UAV 11 本(40 / 64 語)。
  RecordUpload・RecordReset はバッファが COMMON(コマンドリストの初め)の前提(今までと同じ)。debug の HW・tidy は未実行。
