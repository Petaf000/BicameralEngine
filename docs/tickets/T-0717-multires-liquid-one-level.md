# T-0717 MR の 1 段で液体(運ぶ計算の共通の土台の段 2 ②)

- Status: Done(2026-10-17。載せ替えは T-0714・T-0718・T-0762)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ③' 運ぶ計算の共通の土台 段 2 ②(ROADMAP)
- 設計: docs/design/20-transport-substrate.md §2〜§3・§6 段 2 / 決定: D-509・D-513・ADR-0326・ADR-0266・ADR-0276・ADR-0268・ADR-0286

## 目的(1〜2 行)
多重解像度の世界(根のレベルだけ・子なし = 仮の世界と同じ形)で液体を流す。セルは MakeCellView(T-0713)から、持ち越す値は登録した欄(T-0711)、
式は liquid_flow.hlsli(T-0716)の同じ関数。基準: 仮の世界の液体(区画 = 世界いっぱい)と同じ場面で毎刻みビット一致。

## 完了条件(チェックできる形で)
- [x] `engine/src/sim/multires_liquid.{h,cpp}`: `MultiresLiquid::Create`(欄の登録)・`Step`・保存の和と帳簿
- [x] 木の公開の口: `ExpandNestPage`・`StoreNestCell`・`PokeNestBlock`(sim/multires_nest.h。段の外からセルに書く流れが使う)
- [x] `multires_liquid_test`(release): 池に溜まる → 堤を消すと抜ける(16³・1800 + 1800 刻み / 64³ = 仮の世界の格子いっぱい・30 + 30 刻み)・
      ダムを壊す + 段の外で水を半分 / 2 倍(24 × 16 × 8・360 刻み)・液体の無い 64³ の世界(頁もハッシュも変えない)で、
      全部のセル・面の運動量・面の運動のエネルギー・位置のエネルギー・前の質量・帳簿・刻みの報告が毎刻みビット一致。保存点に戻して進めると同じハッシュ
- [x] probe_liquid・probe_phase・cell_view・multires_state・multires_uniform(release)が今まで通り
- [x] release のビルド 警告 0・archmap --check

## メモ・参考
- **形**: 刻むブロック = 根のレベルの本物のブロック全部(活性を見ない)。面の隣は索引(LookupBlock)で引き、無ければ世界の端 = 閉じた面
  (仮の世界の区画の外と同じ扱い)。セルの並びは「ブロックの順 × ブロックの中の番号」で仮の世界と違うが、回し方が Jacobi 型(刻みの初めの値だけから決め、
  足し引きは整数)なので順によらずビット単位で同じ。位置のエネルギーの高さは刻むブロックの一番下の段から(仮の世界の「区画の床から」)。
- **欄**(T-0711 の登録): `liquid.momentum`(面 ×3・液体・VOLUME・保存量)・`liquid.kinetic_energy`(面 ×3・同)・`liquid.potential_energy`(セル・同)・
  `liquid.mass_before`(セル・液体・SAME・ハッシュに入る。保存量ではないので T-0762 の「VOLUME ⇔ 保存量」で SAME)・`liquid.ledger_momentum`(世界 ×3)・`liquid.ledger_energy`(世界)。
  保存点・写し・状態のハッシュは登録を回るので、液体の分は直さずに入る。
- **見立て**: `MakeLiquidCellView(cv) = LqMakeCellView(cv.liquidMass, cv.liquid.volume, cv.solid.volume, CV_CELL_VOLUME_LEVEL_UNITS, cv.heat, cv.heatCapacity)`。
  前任のメモの `CvCellVolumePicoliters(level)` ではなくレベルの単位の体積を渡す(見立ての体積は物質量 = レベルの単位から作るので、セルの体積もレベルの単位。
  レベル 0 では同じ数)。**確かめたこと**: `CvCellView::liquidEquivalentMass` と `LqCellView::equivalent`・`liquidOpen` と `open` は同じ式(FX_ASSERT で毎回見る。
  試験の全部のセルで一致)。液体の物質: 見立ての相の表(凝縮相 かつ liquid)と仮の世界の speciesLiquid は同じ(ベイクで liquid ⇒ condensed)。
- **一様なブロック**: 見立ては 1 回だけ作る。書き戻すのはセルか欄が変わったブロックだけ(一様なら頁に広げる)。液体の無い世界は頁を増やさない。
  頁に戻す(畳む)のは今の FoldQuietPages(欄の一様も見る)に任せる。
- **載せ替えの口(仮。継ぎはぎを残さないため、ここを替える)**:
  1. `LiquidStep::LimitAndApply`・`ApplyDelta`(上限・当てる): 仮の世界の「出る側だけの上限(LqLimitOutflow)+ 成分の増減の包み」をこのファイルに持つ。
     T-0714 の当てる段(出る側と入る側の倍率の min・生の包み・溢れのセル)へ、面ごとの生の包みを作って渡す形に替える(**T-0770**。T-0714 は main に入ったが包みは熱だけなので広げる)。
     入る側の上限が増えるので、その時ビット単位では変わりうる(差を説明して基準を取り直す。D-487 ③)。
  2. `LiquidStep::FollowMassChange`(欄が物質に付く)→ T-0718(当てる段の 1 か所へ)。
  3. `LiquidStep::AddLedger`・世界の欄 `liquid.ledger_*`(帳簿)→ **T-0769**(世界の帳簿の列 = 量 × 理由へ。T-0762 の合流が入ったので列はもうある)。
     それまで `ComputeConservedTotals` の液体の欄の和は液体が動くと変わる(液体の世界でこの検査を使わない)。
  4. `BuildGrid`(全部のブロック)→ 活性のブロックの一覧(T-0723 の後)。`Step` を呼ぶのは今は呼ぶ側(試験)。1 刻みの段の列(気体 → 液体 → 伝導)への登録は T-0714。
  5. 報告の型は仮の世界の `ProbeLiquidStepReport` の別名(比べるため)。仮の世界を退かせる時(T-0724 の後)に名前を移す。
  6. 複製: `ApplyDelta`・`CellDelta`・`MoveLiquid` は仮の世界(probe_liquid.cpp)と同じ形を持つ(仮の世界は基準として残し、縦切りの後に捨てる方針。D-513)。
- **断るもの**: 根のレベル以外のブロック・子(FX_ASSERT)・溢れのセルを使う世界・頁が世界の枠より少ない世界(液体はどのブロックにも書くことがある)。
  レベル ≠ 0 のセルの 1 辺は `LiquidCellEdgeMillimeters(level)`(0.5 m × 2^-k)にしたが、レベルの単位の質量と柱の圧力の合わせ方は未確認(違うレベルの面と一緒に段 2 の後)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-17(wt6・作業役): sim/multires_liquid(MultiresLiquid。欄の登録・1 刻み・保存の和)と木の公開の口(ExpandNestPage・StoreNestCell・PokeNestBlock)を足した。
  multires_liquid_test が最初の版から全部の場面で毎刻みビット一致(池 16³ 3600 刻み・ダム + 水を半分 / 2 倍 360 刻み・64³ の池・液体の無い 64³・保存点に戻して同じハッシュ)。
  費用(CPU 1 スレッド): 池 16³ 仮の世界 4.2 → 5.7 ms/刻み・64³ 1.54 → 1.68 s/刻み(perf.md)。見立ての equivalent・open は液体の芯の値と同じ式(FX_ASSERT)。
  release のビルド 警告 0・probe_liquid・probe_phase・cell_view・multires_state・multires_uniform 通過・archmap OK。GPU の試験は回していない(GPU のコードは変えていない)。
