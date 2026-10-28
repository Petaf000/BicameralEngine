# T-1211 2 つのエネルギーを世界へつなぐ(D-665・ADR-0486)

- Status: Done(2026-10-29 wt4: セルの段の核・反応 2 回・1 セルの GPU のビット一致まで。世界の段は T-1230〜T-1232 に分けた)
- 種類: 工学
- PC: 必須
- 設計: docs/design/20-transport-substrate.md §10.6 / 決定: D-665・D-666・ADR-0486・**ADR-0497(セルの段の形)**・**ADR-0500(欄の持ち方。Proposed)** / 親: docs/tickets/T-1160-phase-energy.md
- 試験: phase_layer(CPU)・gpu_phase_layer・gpu_phase_layer_warp(tests/gpu_phase_layer_test.cpp)

## 目的
T-1160 の共通の核(shaders/common/phase_layer.hlsli)を世界の木(と仮の世界)へ: Ec の欄(疎・ハッシュ・セーブ)・伝導(陽 TxConduction・陰)の面を部分ごとに・反応 2 回(凝縮相の項を持つ規則 / 気体だけの規則の印をベイクで)・気体と液体の区画が部分の h を持ち出す・細かく/粗くする(T-1176 の熱容量の重みを部分ごとに)・CPU と GPU のビット一致(PlEvenCondensedEnergy の 64bit の差と比べは GPU の試験で確かめる)・混ざったセルの頁の数と GPU の時間を測る

## 完了条件(この作業役の分)
- [x] セルの段の関数(phase_layer.hlsli): PlInnerFlowOf → PlReactCell(PlBeginReact・PlEndReact・PlGasReactView・PlEndGasReact)→ PlRemixCondensedEnergy。混ざっているかはセルの中身から(ADR-0497)
- [x] 反応 2 回(ADR-0486 の 5): 凝縮相の項を持つ規則は層の温度・気体だけの規則は気体の温度。振り分けは表を包む PlRulesOf(回さない側の速さを 0。reaction.hlsli は触らない)
- [x] 1 セルの GPU とのビット一致(shaders/sim/phase_layer_cells.hlsl・1024 セル × 9 場面 × 400 刻み・RTX と WARP)。PlEvenCondensedEnergy の 64bit の差と比べ・床の割り算を GPU で確かめた
- [ ] 世界の欄・伝導の面 → **T-1230** / 細かく/粗くする → **T-1231** / 世界の反応と運ぶ・世界のビット一致・頁の数と GPU の時間の測り → **T-1232**

## 土台に足りないもの(司令塔へ。ADR-0500)
- (a) 欄の登録: Ec は今の規則のどれにも合わない(VOLUME ⇔ 保存量の検査で通らない・内訳は親が欄でなく Ec が負になる)。おすすめ = 「セルの配りが書く欄」の印を足す(一般の段は写さない)。
- (b) 当てる段: 粗くする時のセルの配りの口(今は細かくする refineCells だけ)。どちらも wt2(multires_tree・T-1188)に重なる。

## 判断待ち(仮で進めたもの)
- なし(作り手から見た違いのある問いは無い。ADR-0500 は土台の形なので司令塔へ)。

## 後に回したもの
- 規則の振り分けの印をベイクに(今は速さを読むたびに物質の相から出す。T-1232 の最後の項)。
- 気体だけの規則の 2 回目は、1 回目で混ざらなくなったセルでは 1 つのエネルギーで回す(Ec = 0。PlRemixCondensedEnergy と同じ)。

## ゲームの中身へ
- なし。

## 作業ログ
- 2026-10-26 wt3: T-1160 から分けて作った。
- 2026-10-27 wt4(1 人目): セルの段の形(ADR-0497 下書き)・PlStepLoneCell・1 セルの GPU の試験(gpu_phase_layer)を書いた(コミットせずに区切り)。
- 2026-10-29 wt4(2 人目): 1 人目の途中を WIP でコミット(release のビルド・phase_layer・gpu_phase_layer(_warp)が通った)。
  反応 2 回を核に(PlRulesOf・PlReactCell。気体だけの規則が無かったので試験の表に試験だけの燃える気体と規則を 1 つ足し、9 場面目で 2 回目の反応を通ることを確かめる)。
  Ec の欄の持ち方を読んで ADR-0500(Proposed。土台の 2 点を司令塔へ)。世界の段を T-1230〜T-1232 に分けた。
  試験(release): phase_layer・probe_phase_porous_stone・gpu_phase_layer(RTX 24 秒)・gpu_phase_layer_warp。1024 セル × 400 刻みで混ざった 114・混ざらなくなった 11・気体だけの規則が進んだ 113・部分の熱が負 0。
