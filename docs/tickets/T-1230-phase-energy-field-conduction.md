# T-1230 層のエネルギー Ec の欄を世界の木へ・伝導の面を部分ごとに(T-1211 から分けた)

- Status: Doing(2026-11-12 wt6 6 回目: ③ の 1 段目 = 部分の面の式を GPU でビット一致に確かめる試験と TxPartPairsOf〔ブランチ t-1230-gpu-parts〕。**③ の残りは GPU に Ec の置き場が無いので T-1252 段 2 待ち〔司令塔へ〕**。2026-11-12 wt6: ★8 安全網の陽の流れを面ごとに渡す形が CPU・GPU の構造体に入った〔ブランチ t-1230-implicit-gpu〕。③ GPU の系の 2 節と陽の部分の道は続き。② は ready の印〔t-1230-implicit-parts〕。2026-11-05 wt6 4 回目: 段 2 ② 木の陰の系で混ざったセルを 2 節に が CPU で入った〔ブランチ t-1230-implicit-parts〕。① は ready の印〔t-1230-part-faces〕。③ GPU は続き。2026-11-05 wt6 3 回目: 段 2 ① 陽の世界の当てる段へのつなぎが CPU で入った〔ブランチ t-1230-part-faces〕。木の陰の系〔②〕と GPU〔③〕は続き。2026-11-05 wt6 2 回目: 段 2 ② の系の形と費用の測りが入った〔ブランチ t-1230-implicit-two-node〕。打ち切り条件に当たらない。世界の当てる段〔①〕と木の陰の系へのつなぎは続き。2026-11-05 wt6: 段 2 の 1 回目 = 面の式〔部分つきの片側・重なりで組む面の式〕と陰の領域の測りが入った〔ブランチ t-1230-conduction-face〕。世界の当てる段へのつなぎと陰の系は続き。2026-10-30 wt6: 段 1 = 欄 + T-1231 の CPU の配りが入った。伝導の面は段 2。前: 2026-10-29 wt4 が T-1211 から分けた。**ADR-0500 改訂〔2026-10-29 本体 16 回目〕で形が決まった: T-1252 段 1 の後**)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 2 回分
- マイルストーン: 土台の変更要求の束(docs/plan/ROADMAP.md の T-1160 の行)
- 設計: docs/design/20-transport-substrate.md §10.6 / 決定: ADR-0486 の 1・4・ADR-0497・ADR-0500 / 親: T-1211

## 目的
世界の木(と仮の世界)のセルに Ec を持たせ(疎の欄・ハッシュ・セーブ)、伝導の面(陽 TxConduction・陰)を部分ごとに分けて当てる。

## 完了条件
- [ ] Ec の欄の登録(ADR-0500 の決定 1・5。SF_PLACE_CELL・I64・**SF_SCALE_OWNER(相の層の配り手)**・真実・ハッシュ・保存量でない。配り手の表に相の層の配りを足す)。頁は混ざったセルのある所だけ・無い頁は 0(LoadNestField / FieldPageValues で読む)
- [ ] セーブと読み込みで Ec が戻る・状態のハッシュに入る(2 回の実行で一致)
- [ ] 伝導の面: 下 = 層・上 = 気体・横 = 高さで分ける(PlBottomSide・PlTopSide・PlLateralSide)・層の上の面(PlInnerFlowOf)。混ざったセルの無い盤面は今とビット単位で同じ
- [ ] 陽(TxConduction)と陰の両方。CPU リファレンスと GPU のビット一致(小さい盤面)
- [ ] release のビルド(警告 0)・archmap --check

## メモ
- 重なり: wt5 T-1233(shaders/common/heat_conduction.hlsli・reaction.hlsli・object_heat.hlsli)。面の片側の関数は phase_layer.hlsli の側に置き、heat_conduction.hlsli は呼ぶだけにする。
- 1 列の試験(tests/phase_layer_test.cpp)の面の当て方(Faces・PlBottomSide…)が世界の段の雛形。

## メモ(2026-10-29 本体 16 回目)
- 土台(登録の印・配り手の表・粗くする口)は T-1252。このチケットは使い手として 1 行で載せる。

## 作りの詰め(2026-10-30 wt6。T-1252 段 1 が main に入った〔47be5612〕後に読んだ。着手はしていない)
**結論: 欄の部分は T-1252 段 1 の上に乗る。伝導の面の部分は土台の共通部分(当てる段・見立て)に新しい形が要るので、司令塔の決めが先。**
1. **Ec の欄(T-1252 段 1 の上に乗る)**: `SF_OWNER_PHASE_LAYER` を配り手の表に 1 行(SF_OWNER_COUNT 2)・`SfMakeOwnerDesc` で登録(セル・I64・真実・ハッシュ・保存量でない)。
   セーブ・ハッシュは一般の段のまま(コードの変更なし)。ただし配り手の関数は今 `(owner, 親の値, 子の番号)` だけで、相の層の配りは子のセルの中身(熱容量・相)を読む。
   state_field.hlsli の注(「セルの中身を読む配り手は引数に子の重みを足す。使い手の作業」)のとおり `RefineOwners`・`CoarsenOwners`(multires_fields.cpp)が子のセル(細かくした後の中身)を渡す形に広げる。
   **Ec を登録して配りを入れない版は作らない**(配り手が 0 を返すと混ざった子の層が 0 mJ になる = 誤った状態)。→ 欄の登録と T-1231 の配り(細かく = D-674 を 2 回・粗く = Σ 子の凝縮相 ÷ 8 → PlRemixCondensedEnergy)を
   **同じ作業役が 1 本で**(T-1230 段 1 = 欄 + T-1231 の CPU)。GPU の配りは T-1252 段 2 の後。
2. **伝導の面(土台の決めが要る)**: 今の当てる段は種類の口が面によらない(`TxMakeSelfSide` がセルごとに 1 回 = T-0768。`Slope(view)`・`SideCoefficient(view, level, shift)`)。
   部分ごとの面は (a) 面の向きで片側が変わる(下 = 層・上 = 気体・横 = 高さの重なりで最大 4 つ)、(b) 当てる先が 2 つ(層の面の流れは Ec とセルのエネルギーの両方・気体の面はセルのエネルギーだけ)。
   `MrEnergyDelta`(whole・fraction)にも Ec の行き先が無い。違うレベルの面の端数(ADR-0017)をどちらの部分へ入れるかの規則も無い。陰の伝導(implicit_conduction の ImplicitCell)は 1 セル 1 未知数で、
   混ざったセルを 2 未知数 + 層の上の面にするのは多重格子の系の形の変更。
   - 案 A(おすすめ): 種類の口に「面の向きつきの片側」を足す(`SideFor(view, level, shift, face)`。混ざらないセルは今の値を返す = 今とビット一致)と、当てる段の変化に部分の列(`partDelta`。欄の持ち主へ集めて書く段で)。
     陰は混ざったセルを陽で流す(安全網と同じ道)から始め、2 未知数は測ってから。土台の変更 = 当てる段・見立て(Ec を読む)・変化の表の 1 列。
   - 案 B: 混ざったセルの面だけ伝導の段の外の別の段で流す(当てる段を変えない)。継ぎはぎ(同じ面が 2 つの段に分かれる)なので退ける見込み。
   - 作り手から見た違いは無い(エンジンの中の値)= ユーザーへの問いではなく、司令塔が土台の形として決める。横の面の「高さの重なりで 4 つに分ける」関数はまだ無い(PlLateralSide は割合を受けるだけ)。
3. 重なり: T-1233 は main に入った(Done)。T-1252 段 2(GPU・wt2)・T-1240(GPU の層。wt2)と同じ木の段・欄に触るので、GPU の部分は wt2 の後。

## 作業ログ
- 2026-10-29 wt4: T-1211 から分けて作った。
- 2026-10-30 wt6: T-1252 段 1 の後に読んで作りの詰めを書いた(欄は乗る・T-1231 の CPU と 1 本に・伝導の面は当てる段の形の決めが先 = 司令塔へ)。着手はしていない。
- 2026-10-30 wt6(2 回目): ADR-0512 を書き、段 1(欄・CPU の配り)を入れた。試験 phase_layer_tree。伝導の面は段 2(引き継ぎメモ)。
- 2026-11-05 wt6: 段 2 の 1 回目(ブランチ t-1230-conduction-face)。面の式(PlPartSideOf・PlPartFaceFlow)と試験 phase_layer_face・陰の領域で混ざったセルを陽で流す時の測り(phase_layer_explicit_lag)。
  **測りの結論: 陽で流すと 0.1 K に収まらない(陰の領域 = mm のセルで 1〜31 K)。小刻みでも収まらない → 2 未知数の系をおすすめ(仮)**。世界の当てる段へのつなぎは次。
- 2026-11-05 wt6(2 回目。ブランチ t-1230-implicit-two-node。司令塔が (b) を一旦の決定に): 陰の系(sim/implicit_conduction)は節・面の一般の形なので、**系の作りを変えずに 2 節が入る**ことを確かめた
  (同じ座標の 2 節 = 多重格子の縮約で同じ親へ・同じ色の隣は掃き出しの初めの値)。面の組の値の口(PlPairOverlapsOf・PlSlotConductance・PlInnerConductance)を phase_layer.hlsli に分けた(PlPartFaceFlow・PlInnerFlow はビット単位で同じ)。
  試験 phase_layer_implicit(2 節の系の保存・安全網なし・打ち切り条件・費用)。**費用: 節 +12.5 %(見込みどおり)・面 +5〜17 %・V サイクル ×0.69〜1.76・節の計算 ×0.77〜1.98**。①(陽の世界の当てる段)は引き継ぎ。
- 2026-11-05 wt6(3 回目。ブランチ t-1230-part-faces。司令塔が世界の口 2 つを一旦の決定に): ① 陽の世界の当てる段へつないだ(CPU)。
  伝導の種類だけの当てる段 TxFlowConductionCell(shaders/common/transport_conduction_parts.hlsli)・見立ての部分 CV_VIEW_PHASE_LAYER(CellViews::Parts)・層の部分の変化 AddCondensed(nest.phaseLayer.conductionDeltas → Ec の欄)。
  違うレベルの面の層の高さの写しは clamp(h × 2^d − j, 0, 1)(仮)。21 §3 との照らしは ADR-0512 の「段 2 ① 世界の当てる段の口」。試験 phase_layer_world。
- 2026-11-05 wt6(4 回目。ブランチ t-1230-implicit-parts): ② 木の陰の系で混ざったセルを 2 節に(CPU)。
- 2026-11-12 wt6(6 回目。ブランチ t-1230-gpu-parts): ③ の 1 段目 = 面の式の GPU の確かめ(phase_layer_faces.hlsl・gpu_phase_layer_test)・陰の組の面を TxPartPairsOf へ。HLSL で通らなかった ?: の構造体を直した。③ の残りは T-1252 段 2 待ち(下の引き継ぎメモ)。
- 2026-11-12 wt6(5 回目): ② を rebase main(94b20ace)・time_step_tree と time_step_choose_dump 系 7/7 合格 → ready の印。★8 安全網の陽の流れを面ごとに渡す形(ImplicitFace・ImGpuFace。ブランチ t-1230-implicit-gpu)。③ は続き。

## 引き継ぎメモ(2026-10-30 wt6。ブランチ t-1230-ec-field。司令塔の会話の入れ物の再起動で区切った)
- **入った(段 1 = 欄 + T-1231 の CPU)**: 配り手の表に SF_OWNER_PHASE_LAYER(一般の形 = 子は親と同じ数・粗く = 和 ÷ 8 の床)。EnablePhaseLayerEnergy で欄「phase_layer_energy」(セル・I64・真実・ハッシュ・保存量でない)と物質の表。
  セルの配りの口(multires_fields.cpp の RefineOwners / CoarsenOwners)が頁を持つ親の時だけ multires_phase_layer.cpp を呼ぶ: 細かく = 層の熱・気体の熱を子の凝縮相・気体の熱容量の重みで別々に(D-674 を 2 回)配り子のエネルギーも書き直す /
  粗く = Σ 子の凝縮相の分 ÷ 8 の床を部分の熱 ≥ 0 に収める。式は phase_layer.hlsli(GPU と 1 つの源。子ごとの関数)。試験 phase_layer_tree(写しは今と同じ・和は親 × 8・部分の温度を保つ・粗くで戻る・ハッシュ・保存点・2 回同じ)。
- **ADR-0512(Proposed)**: 配りの口の形と、伝導の面を部分ごとに当てる土台の形。21 §3 の一覧の照らしは全行済み。**足りなかった現象: 横の面で両側の層の高さが違う時**(案 A の「面の向きつきの片側」だけでは 4 つの重なりの流れが決まらない)→ ADR で「部分 2 つまでの片側 + 重なりで組む面の式」に広げた(判断待ち 1・司令塔)。
  未確認の行: #44・45・58・59・57・84・66(薄い層を陰の領域で陽に流す時の遅れ = 0.1 K の予算)・#86・87(陰から外した時の費用)。段 2 で測る。
- **残り(この順)**: 段 2(CPU)= 伝導の面(部分つきの片側・重なりの面の式・部分の列・陰の系から混ざったセルを外す条件)・陽で流す時の遅れの測り(露・水たまり・氷を陰の領域で。参照は 1 列の試験)/
  反応・運ぶ後の PlRemixCondensedEnergy(T-1232)/ 段 3(GPU)= T-1252 段 2 の後に配りと面(子ごとの関数をそのまま呼ぶ)。溢れを使う世界(wideCells)の配りは一般の形のまま(未対応)。
- 運転: 細かくした子のセルを LoadPageCell で読み直し StorePageCell で書き戻している(層は成分が同じなので付け替えで残る見込み・層を使う世界の混ざったセルの試験はまだ)。

## 段 2 の測り(2026-11-05 wt6): 混ざったセルを陰の系から外して陽で流した時、0.1 K に収まるか
**試験**: tests/phase_layer_explicit_lag_test.cpp(ctest phase_layer_explicit_lag。CPU・測りの模型 = 浮動小数点の後退オイラー。物質の値は表に近い丸めた値・熱容量一定・相変化なし・横の面なし)。
1 列(下 = 固体 96 セル・満ちた層のセル・混ざったセル〔層 + 気体〕・空気 96 セル・端は断熱)を刻み 1/60 秒で 20 秒。面の式はエンジンと同じ形(片側 = 伝導率 × 面積 ÷ (2 × 中心から面まで)・係数 = 両側の小さい方・陽は さらに 熱容量 ÷ 8・層の上の面は直列)。
参照 R = 全部を陰(混ざったセルは 2 未知数 = 案 (b) の答え)。比べたもの: n=1 = 今の案(陽)/ n=4・64 = (a) 小刻み / n=0 = (c) セルの中の 2 つだけ陰(隣は刻みの初めの温度で固定。測りの途中で足した案)。
数は「初めの 1 秒の後の差の最大(層・気体・隣のセルの最大。K)」。初めの 1 秒は参照そのものの時間の誤差(刻み 1/3840 秒との差)が 0.1〜2.5 K あるので判定から外した(表示は出す)。
| 場面 \ セル | 31 mm | 7.8 mm | 2.0 mm | 0.49 mm |
|---|---|---|---|---|
| 露 0.07 mm(石の温度で付いた)n=1 / 4 / 64 / (c) | 0.005 / 0.003 / 0.002 / 0.0005 | 0.080 / 0.048 / 0.038 / 0.012 | **1.35** / 0.079 / 0.078 / 0.074 | **12.8** / **5.4** / 0.079 / 0.079 |
| 露(空気の温度で付いた) | 0.038 / 0.024 / 0.019 / 0.0004 | **0.14** / 0.091 / 0.074 / 0.012 | **1.35** / **0.12** / **0.11** / 0.093 | **12.1** / **5.2** / 0.083 / 0.083 |
| 水たまり 5 mm(日なたの石) | 0.004 / 0.003 / 0.002 / 0.0002 | 0.012 / 0.011 / 0.010 / 0.009 | **3.0** / **0.44** / 0.039 / 0.038 | **6.7** / **3.0** / 0.039 / 0.039 |
| 氷 2 cm(冷えた石・暖かい空気) | 0.055 / 0.034 / 0.028 / 0.001 | 0.083 / 0.048 / 0.037 / 0.024 | **3.6** / **0.105** / **0.104** / **0.101** | **25.2** / **23.6** / **9.9** / **0.106** |
| 鉄の板の上の水の膜 0.5 mm | **0.25** / **0.16** / **0.13** / 0.005 | **0.24** / **0.16** / **0.13** / 0.040 | **6.2** / **0.17** / **0.17** / **0.21** | **31.2** / **10.9** / **0.21** / **0.36** |
- 20 秒に固体が失った熱の比(陽 ÷ 参照)は n=1 で 0.05(氷・0.49 mm)〜1.20。薄い部分(氷の上の 0.02 mm の気体のすき間・露の層)の 1/8 の上限が面の熱を絞る。
- **陰の領域はどこか**: 陰にするのは subcycleBaseLevel より細かいレベル(面の係数が 熱容量 ÷ 8 で頭打ちになるレベル)。現実の物質では 空気 ≈ 1.7 mm・鉄 ≈ 3.9 mm・銅 ≈ 4 mm より細かいセル(模型の G·Δt = C/8 から。未確認: 世界の既定の表での MrSubcycleBaseLevel の値)。
  → 陰の領域の混ざったセル(2 mm・0.5 mm の列)は、陽(n=1)で 1〜31 K 外れる。31 mm より粗い所は世界でも全部が陽なので、この比べの外(今の 1 列の試験の形)。
- **(a) 小刻み**: n=64 でも 2 mm の氷 0.104 K・鉄の膜 0.17 K・0.49 mm の氷 9.9 K。残るのは小刻みの大きさではなく「陽のセルと陰の隣が 1 刻みずれて受け渡す」分(隣の温度を刻みの初めで固定するため)。費用は面 3 つ × n。→ 退ける。
- **(c) セルの中だけ陰(2 × 2)**: 小刻みの極限(上限なし)で、2 mm で 0.07〜0.21 K・0.49 mm の鉄の膜 0.36 K。ずれの分は (a) と同じに残る。→ 予算の外(退く道としてだけ残す)。
- **(b) 2 未知数(混ざったセルを系の節 2 つ + 層の上の面の 1 本に)**: 参照そのもの(差 0)。**おすすめ(仮・ユーザー未確認)**。
  費用の見込み(未確認): 未知数 = 混ざったセルの数だけ増える(面のセル。8³ のブロックを平らな水面が横切ると 64 / 512 = +12.5 %)・面 = 層の上の面 +1 本・横の面は重なりで最大 3 本(下どうし・上どうし・斜め 1 つ)。
  多重格子の縮約は 2 つの節を同じ親へ(熱容量は和)。作業はチャット 2〜3 回(陰の系の形 = 土台の共通部分の変更。ADR-0512 の判断待ち 2 に書いた)。

### (b) の研究の部分: 打ち切り条件と退く道(着手の前に)
- 研究の部分は「薄い部分(熱容量が面の係数より何桁も小さい節)で V サイクルが収まるか」だけ(ほかは工学)。
- 打ち切り条件: 露・水たまり・氷・鉄の膜の場面(2 mm・0.49 mm)で、V サイクルの回数が混ざったセルの無い同じ場面の 2 倍を超える、または上限(implicitMaxCycles 64)に当たって安全網が陽に戻す刻みが出る。
- 退く道(この順): ① 薄い部分の節を縮約の 1 段目で相手の部分へ寄せる(多重格子の前処理。答えは変えない)→ ② (c) セルの中だけ陰(予算の外は 2 mm で 0.21 K・0.49 mm で 0.36 K = 【エンジン】の問いとして「mm の細かさの混ざったセルの温度が 0.1 K を超えてずれてよいか」を出す)。

## 面の式(2026-11-05 wt6。shaders/common/phase_layer.hlsli の「部分つきの面の片側と、重なりで組む面の式」)
- `PlPartSideOf(table, cell, Ec, face)` → `PlPartSide`(範囲 [0, 境)・[境, 1) の部分 2 つまで。混ざらないセルは HcMakeCache と同じ値の部分 1 つ)。`PlMixedSide(parts, layer, face, 道の高さ)` は見立てから組む口(世界の当てる段が見立てから呼ぶ想定)。
- `PlPartFaceFlow(自分, 隣)` → 和(セルのエネルギーへ)・自分の層が出した分・隣の層が受けた分(Ec へ)。組 4 つ(下どうし・上どうし・斜め 2 つ)を重なりの割合で HcFaceFlow。
- 試験 phase_layer_face: 混ざらない面は HcFaceFlow とビット単位で同じ・下/上の面は PlBottomSide/PlTopSide と同じ値・両側から同じ値・高さ違いの横の面の行き先・横に並んだ 2 セルの保存と温度の揃い。
- まだ世界の当てる段(transport_apply の TxFlowCell)・見立て(CV_VIEW_PHASE_LAYER)・部分の列にはつないでいない(次の段。混ざったセルの無い世界は今の道のまま)。

## 引き継ぎメモ(2026-11-05 wt6。ブランチ t-1230-conduction-face。2 時間の区切り)
- **入った**: 面の式(上)と試験 phase_layer_face・測り phase_layer_explicit_lag。今の道は何も変えていない(新しい関数だけ。混ざらない世界はビット単位で同じ)。
- **残り(この順)**: ① 陽の世界(粗いレベル)の当てる段へつなぐ: 伝導の種類に部分つきの口・見立てで Ec を読む・部分の列(セルごとの 64bit の和)と欄の持ち主へ集めて書く段・違うレベルの面の端数(ADR-0512 の 2 の規則)・層の上の面(PlInnerFlowOf)を同じ列へ。
  CPU の木の試験(混ざったセルの池・横の高さ違い・混ざらない世界のビット一致)。② (b) 陰の系を 2 未知数に(上の打ち切り条件を先に)。③ GPU(T-1252 段 2 の後。面の式は同じ phase_layer.hlsli を呼ぶ)。
- 運転: device_bash から `clang-format -i` を作業ツリーに打つと止まる(消せないフォルダで一時ファイルを消せない)。scratch で整形して cp → mv で置き換えた。

## 段 2 ② の系の形と費用(2026-11-05 wt6 2 回目。tests/phase_layer_implicit_test.cpp・ctest phase_layer_implicit)
- 形: 混ざったセルは系の節 2 つ(層・気体。同じ座標)。面は部分つきの片側の組ごとに 1 本(重なり PlPairOverlapsOf・係数 = min(PlSlotConductance) × HC_LIMIT_PER_CONDUCTANCE × 4^L = 陽の PlPartFaceFlow と同じ値・頭打ちなし)。
  層の上の面は PlInnerConductance(陽の PlInnerFlow と同じ値)。**sim/implicit_conduction(BuildImplicitGrid・StepImplicit・多重格子)は変えていない**: 同じ座標の 2 節は最も細かい段の縮約で同じ親へ(熱容量の和)・
  粗い段で同じ座標になれば 1 節にまとまる・同じ色の隣(層 ⇄ 気体)は赤黒の掃き出しで「掃き出しの初めの値」を読む(今の約束のまま)。→ 土台の共通部分の変更は、木の系を作る側(multires_implicit_conduction.cpp)と系の大きさの見込み(ImBlockCellBound)だけになる見込み。
- 場面: レベル 8(≈ 2.0 mm)と 10(≈ 0.49 mm)の 8³(端は断熱)・y < 3 は石 330 K・y = 3 は混ざったセル 64 個(水の層 290 K + 空気 300 K)・上は空気 300 K。60 刻み・V サイクルは 1 mK で止め・上限 64。1 節 = 混ざったセルをセル全体の 1 節で(今の系の大きさ)。
| 水面 | L | 節 | 面 | V サイクル(60 刻みの和) | 節の計算 | 安全網 |
|---|---|---|---|---|---|---|
| 平ら 30 % | 8 / 10 | 512 → 576(×1.125) | ×1.131 | 62 → 60 / 183 → 148 | ×1.40 / ×0.90 | 0 |
| 段違い 30 % / 60 % | 8 / 10 | ×1.125 | ×1.173 | 62 → 61 / 183 → 181 | ×1.43 / ×1.10 | 0 |
| 薄い層 3.5 %(露) | 8 / 10 | ×1.125 | ×1.131 | 62 → 109 / 200 → 139 | ×1.98 / ×0.77 | 0 |
| 気体のすき間 4 % | 8 / 10 | ×1.125 | ×1.048 | 62 → 75 / 183 → 240 | ×1.75 / ×1.46 | 0 |
- **打ち切り条件(V サイクルが 1 節の 2 倍を超える・安全網)には当たらない**(最大 ×1.76・安全網 0)。研究の部分はこの場面で閉じてよい(未確認: 違うレベルの面を含む木・広い水面での回数)。
- 費用の見込みの確かめ: 節 +12.5 % は平らな水面が 8³ を横切る時の値どおり。ただし 1 刻みの計算(節の計算の数)は多重格子の段の数が変わるため ×0.77〜1.98 と幅がある(レベル 8 の露が最大。薄い層の節が対角を小さくし、縮約の段が増えない所で回数が増える)。ブロックの中の水面のセルの割合に比例する分だけ増える(水面の無いブロックは今と同じ)。
- 1 節と 2 節の答えは違う(水面の層 302 K に対し 1 節のセル全体 292 K = 層と気体を混ぜた温度)。2 節が 1 列の測りの参照と同じ形。

## 引き継ぎメモ(2026-11-05 wt6 2 回目。ブランチ t-1230-implicit-two-node。2 時間の区切り)
- **入った**: 面の組の値の口(PlPairOverlapsOf・PlSlotConductance・PlInnerConductance。今の関数はビット単位で同じ)・試験 phase_layer_implicit(上の表)。今の世界の道は何も変えていない。
- **① 陽の世界の当てる段へつなぐ(次。作りの詰め)**:
  1. 見立て: 頁を持つ相の層の欄があるブロックだけ、セルごとに部分つきの片側を 3 つ(下の面・上の面・横の面。PlPartSideOf の face = 2・3・0)作って見立ての置き場(CellViews)の横に置く(CV_VIEW_PHASE_LAYER。混ざらないセルは作らない = 今の見立てのまま)。
     木のセルは LoadPageCell で読み、Ec は LoadNestPhaseLayerEnergy(欄の頁が無ければ 0)。レベル k のセルの量・エネルギーはレベルの単位なので PlPartsOf は 0.5 m のセルと同じ式で読める(係数は × 4^k = MrScaleByLevel)。
  2. 当てる段: TxFlowCell の面ごとに、世界が「どちらかが混ざったセル」と言えば部分の道(PlPartFaceFlow。片側の係数は × 4^k・小刻み 4^σ と C/8 の上限は MrSubstepFaceLimit と同じ置き方)、言わなければ今の TxSideFaceFlow(ビット単位で同じ)。
     和は今の own.whole へ・層の分は世界の新しい口 AddCondensed(slot, index, 層の変化)で部分の列(セルごとの 64bit。頁を持つブロックだけ)へ。層の上の面(PlInnerFlowOf)も同じ列へ(セルのエネルギーは変えない)。
  3. 違うレベルの面(ADR-0512 の 2): 粗い側の部分の列へは MrSplitCrossFlow の whole だけ(端数はセルのエネルギーにだけ)・細かい側は両方。片側の重なりは細かい側の面の位置で(粗い側の層の高さを細かい面の高さの範囲に写す。**未決め: 粗いセルの層の高さ h を細かいセルの段 j〔2^gap 段のどれか〕に写す式**。案: 細かい面の範囲 [j/2^g, (j+1)/2^g) と粗い側の [0, h) の重なりを Q16 で)。
  4. 欄の持ち主へ集めて書く段(ADR-0500 の 3): 当てる段の後・反応の前に、部分の列を Ec の欄へ足す(StoreNestField。頁の無いセルは列も 0)。
  5. 試験: 木の CPU で 混ざったセルの池(平ら・段違い)・違うレベルの水面・混ざらない世界のビット一致(multires 系・time_step 系・phase_layer_tree の今の組)。
- **② 木の陰の系へ(① の後)**: multires_implicit_conduction.cpp の系を作る所で、混ざったセルを 2 節(上の試験の Build と同じ形)・面を組ごとに・安全網で陽に戻す時は ① の部分の道・流れの当て先は ① の AddCondensed。
  系の大きさの見込み ImBlockCellBound は混ざったセルを 2 つと数える(土台の共通部分 = 司令塔に印。上限の超え方は今の Q22 の案 A のまま)。
- **③ GPU**: ② の CPU が通ってから。式は phase_layer.hlsli のまま呼ぶ(HLSL の配列を動的な添字で読まない形にしてある)。
- 運転: `job.py run -Exe <CPU だけの試験>` も GPU の順番待ちに入る(ランナーが止まる)。試験の表示を見るのは `job.py test -Filter ... -Show`。

## 引き継ぎメモ(2026-11-05 wt6 3 回目。ブランチ t-1230-part-faces)
- **入った(CPU)**: 相の層の欄がある世界の伝導は ComputeConduction が TxFlowConductionCell を呼ぶ(無い世界は今の TxFlowCell のまま)。混ざらない面は TxSideFaceFlow そのもの(ビット単位で同じ。試験 1)。
  混ざったセルの面は TxPartFaceFlow(片側の係数は MrSubstepFaceLimit・MrSubstepCoarseLimit と同じ置き方 × 重なり)・層の上の面は TxPartInnerFlow。Ec の変化は AddCondensed → 伝導の変化を足す所(StepPagedBlock・ApplyEndingDeltas)で欄へ。
  印の回(MarkConductionWants)は混ざったセルの面なら粗い側を頁に広げ端数の枠を求める(部分の道の流れは丸ごとの流れと違うので)・層と気体の温度が違う混ざったセルは一様なブロックでも頁に広げる。
  PlSideSlot の faceLimit を capacityLimit(熱容量の上限)に持ち替えた(値はビット単位で同じ。木のレベルの係数を組むため)。
- **試験 phase_layer_world**: 1 混ざったセルの無い世界は欄の有無でビット単位で同じ / 2 1 刻みの混ざったセルのエネルギーと Ec が面の式(PlPartFaceFlow・PlInnerFlowOf)の手計算とビット単位で同じ(平ら・段違い)/
  3 120 刻みで保存量(ComputeConservedTotals)が一定・部分の熱が負にならない / 4 八分の一を細かくした木(違うレベルの面)でも同じ。
- **まだ(この順)**: ② 木の陰の系(multires_implicit_conduction.cpp)で混ざったセルを 2 節に(phase_layer_implicit の Build と同じ形・安全網で陽に戻す時は TxPartFaceFlow・流れの当て先は AddCondensed・ImBlockCellBound は 2 と数える)。
  今は陰のブロックの混ざったセルは丸ごと 1 節のまま(陰の領域の混ざったセルの部分は動かない = 未対応)。/ 溢れを使う世界(wideCells)は部分の道を通らない(未対応)/ ③ GPU(式は同じ hlsli・世界の口 2 つを GPU の伝導の世界にも)。
- 問い: 無し(写しの式・口の形は作り手から見た違いが無い)。

## 状態(2026-11-05 wt6 3 回目の締め。会話の入れ物の再起動で止まった。ブランチ t-1230-part-faces・46470b53)
- コードと文書(ADR-0512 の「段 2 ① 世界の当てる段の口」= 口 2 つと 21 §3 の照らし)はコミット済み。release ビルドは警告 0 で通った(最後の直しの後も)。
- 試験: 直しの前の回で phase_layer・phase_layer_tree・phase_layer_face・phase_layer_explicit_lag・phase_layer_implicit・field_owner・tree_refine_placement・multires_activity・multires_conduction・multires_implicit_tree が合格。
  phase_layer_world は 1(欄の有無でビット一致)・2(手計算とビット一致・平ら/段違い)・3(根だけの保存・部分の熱)が合格し、4(細かくした木)の温度の見る所だけ落ちた
  (覆われた根のセルを読んでいた = 試験の誤り。保存量と部分の熱は合格)。覆われていないセルを選ぶ直しを入れ、ビルドは通ったが**試験はまだ流していない**。
- multires_implicit_tree が 158 秒(前の回 21 秒)。ほかの作業ツリーのビルドとの重なりの見込みだが**未確認**(欄の無い世界の道は今と同じ。次の回で単独で測る)。
- **ready の印は書いていない**(phase_layer_world の直しの確かめ・rebase main・multires_implicit_tree の時間の確かめが先)。
- 次の一手: `job.py test -Filter '^(phase_layer_world|multires_implicit_tree)$'` → 合格なら rebase main・archmap --check → runner/ready に印。その後 ②(木の陰の系で 2 節)→ ③ GPU。

## 段 2 ② 木の陰の系で混ざったセルを 2 節に(2026-11-05 wt6 4 回目。ブランチ t-1230-implicit-parts)
- **入った(CPU)**: engine/src/sim/multires_implicit_conduction.cpp の系を作る所だけ(共通の段 BuildImplicitGrid・SolveImplicitFlows・implicit_conduction.hlsli は変えていない = 圧力・支え・物の箱の系はビット単位で同じ)。
  - 節: 混ざったセル(見立ての部分 CV_VIEW_PHASE_LAYER の mixed・部分の熱容量の上限がどちらも 0 でない)は層・気体の 2 節(同じ座標・番号 n と n + 1)。熱容量 = 見立ての部分の横の片側の capacityLimit × 8(2^16 単位に切り捨てた値 = 陽の部分の道の上限と同じ値)・温度 = 部分の温度。
  - 面: どちらかが混ざったセルなら部分つきの片側の組ごとに 1 本(PlPairOverlapsOf・係数 = min(PlSlotConductance) × HC_LIMIT_PER_CONDUCTANCE × 4^kf・組の順は 下どうし・上どうし・下 × 上・上 × 下)。違うレベルは粗い側を TxPartSideOnRow で細かい面の段へ写す(陽と同じ)。混ざらない面は今の 1 本(ビット一致)。
  - 層の上の面: 未知数の混ざったセルだけ(全部のセルの面の後ろに、層の節の番号の順)。境のセル(陽のブロック)の層の上の面は陽の TxFlowConductionCell が流す(面で分けた分割のまま)。
  - 開いた世界の端: 混ざったセルは部分ごとに外のセルと 1 本ずつ(安全網で戻す流れ = TxPartPairFlow の組の値)。
  - 当てる: 和はセルのエネルギーへ(今の TxApplyFaceFlow)・層の節の分は AddCondensedDelta で Ec へ(違うレベルは送った分 MrSplitCrossFlow を細かい側から・粗い側は TxCoarseCondensedWhole = 陽と同じ)。層の上の面はセルのエネルギーを変えず層の分だけ。
  - 系の大きさの見込み: CountUnknowns が混ざったセルを 2 と数える(ImBlockCellBound の式は変えていない。予算の和に入る)。
- **試験 phase_layer_world に 5 を足した**: 混ざったセルの無い世界は欄の有無で陰の系でもビット単位で同じ(根だけ・細かくした木)/ 系の節と面が部分の面の式で手で数えた値と同じ(節 512 + 64・層と気体と石のセル)/
  1 刻みの変化が系の流れ(SolveImplicitFlows を同じ系で解き直した値)を節ごとに足した値とビット単位で同じ / 60 刻みで保存量一定・部分の熱が負にならない・層が気体より暖かい(根だけ・細かくした木 = 違うレベルの面と境のセル)。
- **土台に足りないもの(司令塔へ。足していない)**: 安全網(implicit_conduction.cpp の ExplicitFlows)の陽の流れは節の熱容量の 1/8 で頭打ち(重なりで縮めない)。
  部分の組の面は陽の TxPartPairFlow(重なりの割合の上限)と値が違う = 安全網に入った刻みだけ陽の部分の道と違う値になり、層の節は面が最大 13 本あるので「1 面 ≤ C/8 で凸結合」の保証が崩れうる(保存はビット単位のまま。今の場面は安全網 0)。
  直すなら ImplicitEdge の explicitFlow と同じ形を ImplicitFace に(呼ぶ側が面ごとに渡す)= 共通の系の構造体(gas_projection・equilibrium・case_b も使う)と GPU の ImGpuFace の変更。
- **③ GPU(次)**: ① の陽の部分の道も GPU はまだ(GpuConductionWorld に Parts・AddCondensed の 2 つの口・見立ての部分の写し)。陰は implicit_build.hlsl の番号の付け方を節の数の接頭和に(未知数・境のセル)・面の候補を「u × 6 + 面」から「u × 6 × 4 + 組」と層の上の面へ(CPU の面の順と同じに揃える: 面の順は安全網の足し算で効く)・
  ScanBlocks の予算の和も混ざったセルを 2 と数える・Apply が層の節の分を Ec の変化の列へ。量はチャット 2〜3 回の見込み(未確認)。

## 引き継ぎメモ(2026-11-05 wt6 4 回目の締め。会話の入れ物の再起動で止めた。ブランチ t-1230-implicit-parts)
- **どこまで**: ① は rebase main(bdeefa1a)して ready の印(runner/ready/t-1230-part-faces.txt・先頭 e1e5e92c。34 本合格)。② CPU は 97aee7b9 で入った(上の節)。release 警告 0・archmap --check OK。
- **通した試験(② の後・release)**: phase_layer 系・field_owner・multires 系(implicit・implicit_tree・conduction・subcycle ほか)・gas_projection・gas_projection_plates・equilibrium・equilibrium_block・time_step_lab の 29 本合格(2527 秒)。
- **未実行**: time_step_tree(25 分前後)・time_step_choose_dump 系(② は欄の無い世界の道を変えていないので同じはずだが未確認)。② の ready の印は書いていない(rebase main と上の 2 つの後)。
- **multires_implicit_tree の 158 秒の結論**: ほかのジョブとの重なり。単独で 21.4 秒(① の後)・20.6 秒(② の後)。main でも 20.6 / 232.6 秒の回がある(11/04)。不具合ではない。
- **次の一手**: t-1230-implicit-parts を rebase(① がマージされた main へ)→ time_step_tree → ready の印 → ③ GPU(上の節の「③ GPU(次)」。① の陽の部分の道の GPU も未)。安全網の面ごとの陽の流れの口は司令塔の判断待ち(上の「土台に足りないもの」)。

## ★8 安全網の陽の流れを面ごとに渡す(2026-11-12 wt6 5 回目。ブランチ t-1230-implicit-gpu。ユーザーの答え = (a) 直す)
- **土台の共通部分の変更(司令塔が docs/design/21 §3 と照らす)**: 陰の系の面 `ImplicitFace` に `explicitFlow`・`explicitFlowGiven`(ImplicitEdge::explicitFlow と同じ形)・
  GPU の `ImGpuFace` を 32 → 40 バイト(`int64_t explicitFlow` を足し `coarseFraction` を印の語 `flags` へ: IM_FACE_COARSE_FRACTION・IM_FACE_EXPLICIT_FLOW・IM_GPU_FACE_BYTES・ImFaceFlags)。
  CPU の ExplicitFlows・GPU の ImFlows は印のある面だけ渡された値を使う。**印の無い面(圧力・支え・case_b・物の箱・混ざらない伝導)は今の道のまま**。21 §3 の照らしは ADR-0512 の新しい節。
- 使い手: multires_implicit_conduction.cpp の部分の組の面 = `TxPartPairFlow(自分, 相手, 重なり, kf, d, 0)`・層の上の面 = `TxPartInnerFlow`(陽の部分の道の値 = 上限を重なりで縮める)。
- 試験: phase_layer_world に 6(系の面の陽の流れが手で数えた TxPartPairFlow・TxPartInnerFlow とビット単位で同じ / 写しの面の係数だけ 2^20 倍・掃き出し 0 回の赤黒で安全網を起こし、
  範囲を広げた同じ解き方と流れが違う面〔陽に戻った面〕のうち部分の組の面は渡した値そのもの)。運転: 根のレベル 0 の系は陽でも安定な所で、直しを強くした V サイクル(× 8〜512)・掃き出し 0 回だけでは安全網が起きなかった(印 0・超えた最大 0 mK)。
- 通した試験(release 警告 0): CPU = phase_layer・phase_layer_implicit・phase_layer_world・gas_projection・gas_projection_plates・equilibrium(861 秒)・equilibrium_block・case_b_r2・multires_implicit・multires_implicit_tree・float_check 系 5・dxil_alloca_check /
  GPU = gpu_multires_implicit・gpu_multires_implicit_tree・gpu_multires_implicit_build・gpu_multires_implicit_build_warp・gpu_multires_implicit_conduction・gpu_equilibrium_block・gpu_gas_projection(7/7)。archmap --check OK。
- GPU の木の系を作る段(implicit_build.hlsl)はまだ混ざったセルを作らない(印を付けない・explicitFlow = 0)= ③ で付ける。費用: 系のバッファの面の欄 1.25 倍。

## 引き継ぎメモ(2026-11-12 wt6 5 回目。2 時間の区切り)
- **③ GPU の残り(この順)**: 1 陽の部分の道の GPU(shaders/sim/multires_conduct.hlsli の GpuConductionWorld に Parts・AddCondensed の 2 つの口・見立ての部分 CV_VIEW_PHASE_LAYER の写し・Ec の変化の列 → 欄へ集めて書く段)/
  2 陰の系の GPU(implicit_build.hlsl: 未知数の番号を節の数の接頭和に・面の候補を「u × 6 + 面」から組 4 つと層の上の面へ〔CPU の面の順と同じ〕・部分の組の面に IM_FACE_EXPLICIT_FLOW と TxPartPairFlow の値・
  BuildScanBlocks の予算の和で混ざったセルを 2 と数える・BuildApply が層の節の分を Ec の変化の列へ)/ 3 試験: gpu_multires_implicit_build に混ざったセルの場面(CPU の系とビット一致)・gpu_multires_implicit_conduction に相の層の欄の世界(HW/WARP)。
- 量の見込み: チャット 2〜3 回(未確認)。① の陽の道の GPU が先(陰の境のセルの層の上の面は陽が流すため)。

## 引き継ぎメモ(2026-11-12 wt6 6 回目。ブランチ t-1230-gpu-parts〔t-1230-implicit-gpu bdfc31d5 の上〕。司令塔の締めで区切った)
- **入った**: transport_conduction_parts.hlsli に TxPartPair・TxPartPairsOf(陰の系の部分の組の面 = 係数の G の側と安全網の陽の流れ。CPU の AddPartFaces・AddPartEdges が呼ぶ = GPU の木の系を作る段も同じ関数を呼ぶ形。値は同じ)。
  **HLSL で TxFlowConductionCell が通らなかった**(?: で構造体を選ぶ 3 か所。C++ だけでコンパイルしていたので見えなかった)→ TxCellPartSide を if に・TxFacePartSide を足した(値は同じ)。
  試験: shaders/sim/phase_layer_faces.hlsl + gpu_phase_layer_test に「区間の初めのセルと Ec で、陽の面の流れ・層の分・層の上の面・粗い側へ送る分〔MrSplitCrossFlow・TxScaleCondensed・TxCoarseCondensedWhole〕・
  陰の組の面 4 つ・重なり・段に写した境」を GPU で 1024 面 × 8 区間計算し CPU とビット一致(通った道の数も数えて 0 なら落とす)。dxil_alloca の上限に phase_layer_faces 6(phase_layer_cells と同じ数・同じ源 = 局所の RxCell の成分の回し)。
- **通した試験(release 警告 0)**: phase_layer・phase_layer_face・phase_layer_implicit・phase_layer_world・multires_implicit・float_check 系 5(dxil_alloca_check は上限を足す前に phase_layer_faces 6 で落ちた → 足した。足した後と gpu_phase_layer・gpu_phase_layer_warp は**未実行**)。
- **土台に足りないもの(司令塔へ。足していない)**: **GPU の多重解像度の木に登録の欄の置き場が無い**(gpu_multires・multires_bindings に欄が無い = T-1252 段 2〔GPU の欄の頁のプール・pageOf・空きのスタック・集めて書く段・木の配り手〕が未着手)。
  ③ の世界の口 Parts(Ec を読む)・AddCondensed(Ec の変化を集めて書く)と implicit_build.hlsl の混ざったセルの節は Ec が要るので、**T-1252 段 2 の後**(T-1231 の GPU の配りも同じ)。それまでに GPU でできるのはこの式の確かめまで。
- **③ の残りの作りの詰め(T-1252 段 2 の後・この順)**:
  1. 陽の道(multires_conduct.hlsli): TxCellParts は 232 B(PlSideSlot 32 B・PlPartSide 72 B)× 512 セル = 116 KB で groupshared(今 20 KB 使う・上限 32 KB)に置けない →
     混ざったかの 1 ビットだけを gs に(ブロック 512 + 外の隣 384)・部分は面ごとに Ec とセルから作る(案 a・おすすめ)か、流れの前の段で混ざったセルだけ VRAM の詰めた列へ(案 b)。a で測ってから。
     印の回(MarkCell)に MixedFace・InnerFlows。Ec の変化の列は伝導の変化の表 g_conduction(u11)の後ろに頁のセルごとの int64 で(新しい口を足さない)→ 当てる段の後に欄へ集めて書く。ルート署名は多重解像度で 64/64 語なので欄の口は T-1252 段 2 の形に乗る。
  2. 陰の系(implicit_build.hlsl): 未知数の番号を節の数(混ざったセル 2)の接頭和に・面の候補を組 4 つ + 層の上の面へ(CPU の面の順)・部分の組の面に IM_FACE_EXPLICIT_FLOW と TxPartPairsOf の値・ScanBlocks の予算で混ざったセルを 2・Apply が層の節の分を Ec の変化の列へ。
  3. 試験: gpu_multires_implicit_build に混ざったセルの場面(CPU の系とビット一致)・gpu_multires_implicit_conduction に相の層の欄の世界(HW/WARP)。
  注意: dxil_alloca — PlPartsOf/PlLayerOf を伝導の段から呼ぶと局所の配列が増える見込み(phase_layer_* で 6)。HW の Work Graph(T-0397)で止まらないか確かめる。
- **次の一手**: `test -Preset release -Filter '^(dxil_alloca_check|multires_implicit_tree|gpu_phase_layer|gpu_phase_layer_warp)$'` → 合格なら rebase(implicit-gpu が main に入った後)→ runner/ready に印。③ の残りは T-1252 段 2 の後。
- 運転: job.py test は `-Preset release` が要る(既定は debug で「テストが見つかりません」)。BICAMERAL_ROOT 無しで `job.py test -h` を 1 回打ってしまった(main のランナーで 2 秒で「見つかりません」で終わった・害なし)。
