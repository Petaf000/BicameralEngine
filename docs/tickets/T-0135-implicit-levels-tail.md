# T-0135 陰解法の多重格子の段を作る費用を減らす(小さい段からの回を 1 グループで。この版)

- Status: Done(一部: LvTail を作って HW で CPU とビット一致・費用 −0.6〜−1.75 ms を測った。WARP で落ちるので既定にはしていない → **T-0147**。
  節の並び・ImTail・間接の Dispatch・大きさ → **T-0136**、形が変わらない刻み → **T-0137**)
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 2 時間以内(始める前の見積もり: T-0134 から分けた全部〔節の並び・ImTail の境・間接の Dispatch・GpuImplicit の大きさを
  GPU で決める・形が変わらない刻みは作り直さない・段を作る費用を減らす〕は 2 時間に入らない。GpuImplicit の V サイクルの記録〔段ごとの
  定数・並び・ImTail〕を GPU が決める形に書き直すのは implicit_conduct.hlsl と gpu_implicit.cpp の大きな書き直しになるため。
  → 「段を作る費用を減らす」に絞り、残りを **T-0136**(節の並び・ImTail の境・間接の Dispatch・大きさを上限から)と
  **T-0137**(形が変わらない刻みは作り直さない)に分けた)
- マイルストーン: M2 の並走(D-432)
- 設計: docs/design/17-multiresolution.md §6 / 決定: D-428・D-432・D-434・D-436 / ADR-0019

## 目的(1〜2 行)
T-0134 で GPU が作れるようになった多重格子の段の費用(上限 64 で 948 Dispatch・1.6〜2.7 ms。大半は述語で飛ばした空の回の Dispatch とバリア)を、
CPU とのビット一致を保ったまま減らす。

## 完了条件(チェックできる形で)
- [x] 小さい段からの回と、積んだ回の後の残りの回を 1 グループ(1024 スレッド)の LvTail で最後まで回す(shaders/sim/implicit_levels.hlsl。
      本体は Dispatch の回と同じ関数・同じ段の順)。Dispatch で積む回の数と LvTail の境は GpuImplicitLevelLimits で選べる
- [x] 熱い点・鎖・たくさんの要求で、全部 Dispatch・回 8 + 境 1024・全部 LvTail の 3 つが CPU と毎刻みビット一致(release の HW)。
      WARP は全部 Dispatch だけ(LvTail で落ちる → T-0147)
- [x] 前後の費用を測る(下の「結果」・docs/perf.md)
- [ ] LvTail を WARP で動かして既定にする → **T-0147**
- [ ] 節の並び・ImTail の境・間接の Dispatch・GpuImplicit の大きさを上限から → **T-0136**
- [ ] 形が変わらない刻みは作り直さない → **T-0137**

## 何をしたか(形。決めたのは Claude〔実装の細部〕)
- 回の 14 段の本体を `Round〜(番号)` の関数にし、Dispatch の入口(Lv〜)と LvTail が同じものを呼ぶ。今の回の段は `s_depth`(スレッドごとの変数。
  Dispatch は定数 g_roundDepth から、LvTail は回ごとに進める)。
- LvTail(1 グループ 1024 スレッド): 番号を 1024 ずつ回して本体を呼び、段の間は DeviceMemoryBarrierWithGroupSync。接頭和は塊(1024)ごとにグループの中の接頭和 →
  1 スレッドで塊の和(Dispatch の回と同じ語に同じ値)。段が置かれなかった時は数を 0 にして同じ道を通る(早い return をしない)。
- どこから LvTail か: ScheduleRound(CellLinks と RoundEnd が呼ぶ)が、回を回すなら細かい段の節が tailMaxNodes 以下の時に述語でその回を飛ばし、
  見出しの語 2(LV_TAIL_FROM)に始める回 + 1 を書く。回さなかった回の RoundEnd は次の回も飛ばし、語 2 はそのまま。Dispatch の回が尽きた後の残りも LvTail。
- GpuImplicitLevelLimits に dispatchRounds・tailMaxNodes(既定 63・0 = T-0134 の積み方で LvTail を積まない)。ルート定数 13。
- 試験: 全部 Dispatch・回 8 + 境 1024・全部 LvTail の 3 つ(WARP は全部 Dispatch だけ)。計測は 8 つの積み方。
- WARP で落ちる: LvTail を積むと release の WARP でデバイスが失われる(回が 0 で何もしないで抜ける時も。LvTail の中身を消すと通る)。
  早い return を無くす・バリアを素直なループの外に・接頭和を抜く、のどれでも落ちた。debug の WARP(-Od)は全部 Dispatch だけを確かめた(LvTail は未確認)。
  仮説: WARP の JIT が、1024 スレッドのグループで本体 14 個を入れたバリアの多い大きなシェーダーを扱えない(`s_depth` の静的変数の書き換えも疑わしい)。

## 結果(2026-10-09、release、RTX 3070 Ti。多重格子の段を作る段だけ。暖機 20 回の後 5 回の最小。本体と wt4 のランナーが試験中の負荷あり)
| 場面(節・段の数) | 前: 全部 Dispatch(948 Dispatch) | 上限 = 段の数(参考) | 回 8・境 256(124 Dispatch) | 回 8・境 1024 | 全部 LvTail |
|---|---|---|---|---|---|
| 熱い点(585・4 段) | 1.94 ms | 0.40 ms | **0.19 ms** | 0.36 ms | 0.23 ms |
| 鎖(5055・5 段) | 1.13 ms | 0.41 ms | **0.46 ms** | 0.57 ms | 0.96 ms |
| たくさんの要求(100723・8 段) | 2.64 ms | 2.06 ms | **2.08 ms** | 2.07 ms | 19.3 ms |
- 空の回の固定費は消え、上限 = 段の数(CPU が段の数を知っている時)と同じか軽い。小さい場面は LvTail が段ごとの Dispatch より軽い(段の数が多い熱い点で 0.4 → 0.19)。
- たくさんの要求の残り 2 ms は大きい段(節 1 万〜2 万)の回そのもの。全部 LvTail は 19 ms(1 グループが大きい段を回す)なので境は小さく保つ。
- おすすめの形は回 8・境 256(T-0147 で WARP が通ったら既定に)。

## 判断待ち
- なし(回の積み方は実装の細部で、作る段は番号まで同じ。遊びへの影響なし)。

## 分けたもの(新しいチケット。ROADMAP には司令塔が足す)
- **T-0147**(工学): LvTail(多重格子の段を作る回を 1 グループで)を WARP で動かして既定(回 8・境 256)にする。release の WARP でデバイスが失われる原因を
  切り分ける(debug の WARP・`s_depth` の静的変数を引数に・1024 → 256 スレッド・本体を減らした LvTail で二分)。HW では CPU とビット一致を確かめ済み。
- **T-0136**(工学): 陰解法の節の並び(長い行の節を後ろに・色ごとの並び・縮約の並び)・ImTail の境(節 1024 以下・隣 32 以下・最も粗い段)を GPU で決める・
  間接の Dispatch の引数(段の数・節の数・隣の数から)・GpuImplicit の大きさを上限から決める(今は CPU の系の数で Create)。
  GpuImplicit の V サイクルの記録が CPU の知る段の形(m_levelOffsets・並び・m_tailDepth)で組まれているので、段ごとの定数を GPU のバッファから読む形
  (ExecuteIndirect の引数・段の表)への書き直しになる。T-0132(伝導の段から呼ぶ)の前に要る。
- **T-0137**(工学): 形が変わらない刻みは段を作り直さない(重みだけ毎刻み)。形〔セル・面の集合〕が同じかを GPU が判定し、同じなら番号付け〔表・接頭和・並べ替え〕を
  飛ばして係数・熱容量・重みだけ足し直す。LvTail の後の費用(下の「結果」)を見て、効く場面があるかを測ってから決める。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役、wt2・約 1.7 時間): 始める前の見積もりで全部は入らないと判断し、段を作る費用を減らすことに絞った(T-0136・T-0137 に分けた)。
  回の本体を関数にして 1 グループの LvTail から呼ぶ形にし、release の HW で 3 つの積み方が CPU とビット一致、費用は 1.94 / 1.13 / 2.64 → 0.19 / 0.46 / 2.08 ms。
  release の WARP で LvTail を積むとデバイスが失われ、3 回形を変えても直らなかったので、既定は前の積み方のまま・WARP は前の積み方だけ確かめる形で止めた(T-0147)。
  tidy・debug の HW は時間の約束で流していない。

## 引き継ぎメモ(HANDOFF に載せるもの。司令塔がマージ後に反映)
- 状態: T-0135 一部完了。多重格子の段を作る回を 1 グループで回す LvTail を作った(implicit_levels.hlsl・GpuImplicitLevelLimits の dispatchRounds・tailMaxNodes)。
  HW で CPU と番号まで毎刻みビット一致、費用は回 8・境 256 で 1.94 / 1.13 / 2.64 → 0.19 / 0.46 / 2.08 ms。WARP で落ちるので既定は T-0134 の積み方(LvTail を積まない)。
- 動いているもの: `-Filter "^gpu_multires_implicit_build(_warp)?$"`(release の HW 約 5 分〔3 つの積み方 + 計測〕・WARP 約 1 分 / debug の WARP 約 1.6 分)。
  計測は `job.py run -Preset release -Exe gpu_multires_implicit_build_test -- --queue compute --measure-only`(8 つの積み方)。
- 壊れているもの: LvTail を積むと release の WARP でデバイスが失われる(T-0147)。既定では積まないので、今の試験と CI には出ない。
- 決めたこと(Claude・実装の細部): ADR-0019 追記(T-0135: 回の本体を関数にして Dispatch と LvTail で共有・どこから LvTail か・既定にしない理由)。
- 判断待ち: なし。
- 注意: 節の並び・ImTail・間接の Dispatch・GpuImplicit の大きさは T-0136、形が変わらない刻みは T-0137。tidy(release)・debug の HW は未実行。
  計測は本体と wt4 が試験中の負荷ありで取った(idle で取り直すなら上の計測のコマンド)。
