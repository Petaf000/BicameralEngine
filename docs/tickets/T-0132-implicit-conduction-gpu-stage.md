# T-0132 GPU の伝導の段から陰解法を呼ぶ(全部を刻む Compute と活性の刻みで CPU とビット一致)

- Status: Done(陰解法の上限を超えた刻みの扱いは判断待ち → T-0178、固定費を減らすのは T-0179)
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 2 時間以内(始める前の見積もり: 系を作る・段・解く部品は T-0129〜T-0154 で揃っているので、伝導の段へのつなぎ
  〔流れの段で陰解法のブロックを飛ばす・解いた変化を伝導の表へ足す・活性の一覧〕と試験で 1.5 時間。計測は同じ試験の中で。実際は約 1.6 時間)
- マイルストーン: M2 の並走(D-432)
- 設計: docs/design/17-multiresolution.md §6 / 決定: D-428・D-432・D-434(案 a)・D-436 / ADR-0019(T-0132 の追記)

## 目的(1〜2 行)
MultiresStepOptions::implicitConduction を GPU の刻み(GpuMultires::RecordStep・RecordStepActive)でも効くようにする。D-434 案 a
(方式① は分けない + 方式② を基準より細かい Δk 1〜8)・D-436(V の上限 64 まで解き切る)の形のまま、CPU の StepNest・StepActive と毎刻みビット一致。

## 完了条件(チェックできる形で)
- [x] GPU の伝導の段が implicitConduction の刻みで陰解法を呼ぶ(流れの段は基準より細かいブロックを飛ばし、流れの後に系を作って解き、
      変化を伝導の表へ足す)。CPU の系を一度も写さない
- [x] 熱い点・鎖・たくさんの要求で、全部を刻む(Compute)・活性の刻みの両方が CPU と毎刻みビット一致(状態の全部・次の刻みの種・V の回数・安全網)。HW・WARP
- [x] 鎖・熱い点・たくさんの要求の ms/刻みを測る(下の「結果」。本体・wt3・wt4 が idle の時。docs/perf.md)
- [ ] 系の上限を超えた刻みの扱い → **T-0178**(判断待ち)
- [ ] debug(HW・WARP)と tidy は時間の約束で流していない

## 何をしたか(形。決めたのは Claude〔実装の細部〕。ADR-0019 の T-0132 の追記)
- **GpuMultiresImplicit**(engine/src/sim/gpu_multires_implicit.*。GpuMultires::EnableImplicitConduction(device, limits) で作り GpuMultires が持つ):
  RecordConduction の最後の小刻みの流れの後・ConductApply の前に、GpuImplicitBuild(凍った印を見る)→ GpuImplicitLevels → GpuImplicit
  (RecordReset → 系と段を写す → 前の刻みの数から ShapeFrom で選んだ形で RecordStep)→ BuildApply → RecordCostReadback → RecordRelease。
- **流れの段で飛ばす**: multires_conduct.hlsli の InImplicitConduction(CPU の nest_detail::InImplicitConduction と同じ)。印は stepFlags のビット 3、
  implicitMaxGap − 1 はビット 5〜7(GPU は 1〜8 だけ。ルート署名に空きが 1 語しかないので定数を足さなかった)。
- **BuildApply**(implicit_build.hlsl): 解いたセルの「後 − 刻みの初め」と端数を伝導の変化の表へ(AddConductDelta を multires_bindings.hlsli へ移した)。
  活性の刻みでは、変わったセルのブロックを伝導の一覧へ(CPU は頁のブロックを全部 StepPagedBlock に通すので、眠っているブロックにも足す)。
- 伝導の段の Work Graph 版(conductionGraph)を使う時も、陰解法の刻みの足す段だけ Compute(一覧の数を Work Graph の見出しへ写すのは TreeFractions で、その後に足すため)。
- GpuImplicit に RecordRelease(COMMON に戻す。同じリストに何刻みも積むため)・CellsBuffer・RecordCostReadback / ReadCost を足した。
- 試験 tests/gpu_multires_implicit_conduction_test.cpp(新): 熱い点(12 刻み)・鎖(36)・たくさんの要求(8。要求・影つき)を、全部を刻む(Compute)と
  活性の刻みで、CPU の StepNest・StepActive と毎刻み比べる(状態の全部・次の刻みの種・V の回数・安全網)。系の上限は CPU が先に刻んだ系の 2 倍 + 16。
  release の HW では 1 刻み全体と陰解法の段の内訳の ms を測る(陰解法あり / なし)。

## 結果(2026-10-09、release、RTX 3070 Ti。本体・wt3・wt4 のランナーが idle の時。ビルド後の 1 回目は捨てた。docs/perf.md)
`job.py run -Preset release -Exe gpu_multires_implicit_conduction_test -- --queue compute --measure-only`(同じ場面を 2 回流して 2 回目。1 刻み = 1 本のリスト)。
ms/刻み。初め = 2〜12(8)刻みの平均、後 = 13 刻み目からの平均。V の上限は木の既定 64(D-436)。
| 場面 | 刻み方 | 陰解法あり(初め / 後) | 内訳(後): 系を作る・多重格子の段・写して解く・足す | 陰解法なし(陽解法の頭打ち) |
|---|---|---|---|---|
| 熱い点(512 セル・V 2) | 全部 / 活性 | 1.50 / 1.52 | 0.07・0.89〜0.94・0.43〜0.46・0.00(初め) | 0.06 / 0.10 |
| 鎖(1907 セル・V 4) | 全部 | 5.16 / **4.31** | 0.10・1.11・3.01・0.00 | 0.08 |
| 鎖 | 活性 | 4.57 / **4.22** | 0.09・1.06・2.90・0.00 | 0.15 |
| たくさんの要求(2 万セル・V 15) | 全部 / 活性 | 23.5 / 23.9 | 0.21〜0.33・3.0・10.9〜11.0・0.00(初め) | 8.4 / 8.4 |
- 負荷ありの回(本体の試験中)もほぼ同じ(鎖の後 4.2〜4.4 ms)。もっと重い負荷の時(前の HW の試験の中)は鎖の後 8.6〜9.6 ms とぶれた。
- 内訳の「写して解く」は T-0154 の解く段だけの値(熱い点 0.37〜0.52・鎖 3.1 ms〔上限 64〕)とほぼ同じ。新しく足した費用は**多重格子の段**(上限 64 段まで回を積み、
  要らない回は述語で飛ばすがバリアが残る。熱い点 0.83・鎖 1.1・たくさんの要求 3.0 ms)と系を作る段(0.07〜0.25 ms)。解いた変化を足す段は 0.01 ms 未満。
- 鎖の案 a の 1 刻み ≈ 4.2〜4.3 ms(Q3 の目標 5 ms の中)。たくさんの要求(試験のための深さ 26 段・Δk ≤ 8 で 2 万セル)は 23.5 ms で
  予算(シミュに約 12 ms)を超える。D-434 の「重い場面が頻発すると見込まれるなら分ける段を減らす(Δkmax 2)」の材料(ゲームで頻発するかは未確認)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役、wt2・約 1.6 時間): GpuMultiresImplicit(系を作る → 段 → 解く → 変化を伝導の表へ)を足し、伝導の流れの段で陰解法のブロックを飛ばす印を
  stepFlags に詰めた。最初のビルドで release の HW・WARP とも熱い点・鎖・たくさんの要求の全部・活性が CPU と毎刻みビット一致(V の回数・安全網も)。
  計測で、伝導の段から呼ぶと多重格子の段の空の回(0.8〜3 ms)が新しい固定費になると分かった(T-0179)。idle の時に測り直した(負荷ありとほぼ同じ)。release で gpu_multires_implicit(_tree・_build)・gpu_multires_conduction(_warp)も通過。
  debug(HW・WARP)と tidy は時間の約束で流していない。

## 引き継ぎメモ(HANDOFF に載せるもの。司令塔がマージ後に反映)
- 状態: T-0132 完了。MultiresStepOptions::implicitConduction が GPU の刻み(RecordStep・RecordStepActive)でも効く(GpuMultires::EnableImplicitConduction の後)。
  熱い点・鎖・たくさんの要求で、全部・活性の刻みとも CPU と毎刻みビット一致(release の HW・WARP)。CPU の系は一度も写さない。
- 動いているもの: `-Filter "^gpu_multires_implicit_conduction(_warp)?$"`(release の HW 約 9 分〔計測込み・負荷あり〕・WARP 約 4 分)。計測は
  `job.py run -Preset release -Exe gpu_multires_implicit_conduction_test -- --queue compute --measure-only`(約 50 秒)。
- 壊れているもの: なし(release で gpu_multires_implicit(_tree・_build)(_warp)・gpu_multires_conduction(_warp)も通過。debug・tidy は未実行)。
- 決めたこと(Claude・実装の細部): ADR-0019 追記(T-0132)。
- 判断待ち: 1 件(下。T-0178)。
- 注意: GPU の implicitMaxGap は 1〜8(stepFlags の 3bit)。系の大きさは EnableImplicitConduction の上限(GpuMultiresImplicitLimits)で、超えた刻みは CPU と合わなくなる(T-0178)。
  陰解法の刻みは小刻みに分けない(maxSubcycleGap = 0。CPU と同じ約束)。GpuMultires は移動だけ(コピー不可)・デストラクタは .cpp。
- 注意: 計測(idle。ms/刻み): 鎖 4.2〜4.3(後)・熱い点 1.5・たくさんの要求 23.5。新しい固定費は多重格子の段の空の回(T-0179)。

## 判断待ち
- **Q: 細かい所の熱の陰解法の系が上限(GpuMultiresImplicitLimits。VRAM に先に取る大きさ)を超えた刻みに何をするか**(T-0178)。今は上限を大きく取って
  試験しているだけで、超えると GPU の系が途中で切れて CPU と合わなくなる(遊びでは熱の値が壊れうる = D-428 の「嘘」)。
  - 案 A(おすすめ): 入りきらないブロックはその刻みだけ陽解法(頭打ち)のまま進める。**遊びへの影響**: 熱はいつも流れ、保存も守る。ただし入りきらない所だけ、
    その刻みは細かい所の熱が本当より遅く伝わる(たくさんの細かい所が一度に熱くなった時だけ。頁・端数の枠が足りない時の「凍らせる」より穏やか)。
  - 案 B: 入りきらないブロックはその刻み凍らせる(頁が足りない時と同じ)。**遊びへの影響**: その所の熱がその刻み止まる。実装は簡単だが、見えると不自然。
  - 案 C: 上限を超えそうなら、細かくする要求を止める(細かい所を増やさない)。**遊びへの影響**: 熱は正しいが、近くで見ても細かくならない所が出る。
  - 補足(Claude が決める細部): どれも CPU リファレンスに同じ判定を入れてビット一致を保つ。選ぶブロックの順は頁と同じ枠の順。仮の案は選んでいない
    (今は上限を超えない試験だけなので、選ぶまで何も変わらない)。

## 分けたもの(新しいチケット。ROADMAP には司令塔が足す)
- **T-0178**(工学。判断待ちの後): 陰解法の系が上限を超えた刻みの扱い(上の判断待ち)。CPU の AddImplicitConduction にも同じ上限と判定を入れ、超える場面の試験を足す。
- **T-0179**(工学): 伝導の段から呼ぶ陰解法の固定費を減らす。多重格子の段の空の回(上限 64 段まで回を積む。熱い点 0.83・鎖 1.1・たくさんの要求 3.0 ms)
  ・形が変わらない刻みは系と段を作り直さない・V の上限 64 の空の回(T-0154 の続き)。LvTail を WARP で直す T-0147 の後に、LvTail を既定にするのも案。
  idle の計測(このチケットの表)を取り直してから決める。
