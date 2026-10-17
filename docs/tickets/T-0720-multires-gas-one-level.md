# T-0720 MR の 1 段で気体(運ぶ計算の共通の土台の段 3 ②)

- Status: Done(2026-10-17。載せ替えは T-0763・T-0764・T-0765)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ③' 運ぶ計算の共通の土台 段 3 ②(ROADMAP)
- 設計: docs/design/20-transport-substrate.md §2〜§3・§6 段 3 / 決定: D-509・D-514・D-515・ADR-0326・ADR-0256・ADR-0268・ADR-0286

## 目的(1〜2 行)
多重解像度の世界(根のレベル 0 だけ・子なし = 仮の世界と同じ形)で気体を流す。持ち越す値は登録した欄(T-0711)、気体の空きはセルの見立て(T-0713)、
式は gas_flow.hlsli(T-0719)の同じ関数。基準: 仮の世界の気体(区画 = 世界いっぱい・陽解法)と同じ場面で毎刻みビット一致。

## 完了条件(チェックできる形で)
- [x] `engine/src/sim/multires_gas.{h,cpp}`: `MultiresGas::Create`(欄の登録)・`Step`・保存の和と帳簿
- [x] 小刻みを面の一覧で回す口: `GasFaceLink`・`GasBox::cellLayers`・`StepGasLinks(box, links)`・`MakeGasBoxLinks`(sim/gas_reference.h)。箱の `StepGas(box)` は今まで通り
- [x] セル ⇔ 気体の箱の写しを 1 か所に: `sim/gas_cells.{h,cpp}`(`GasAtmosphereLayers`・`LoadGasCells`・`StoreGasCells`・`CollectGasSpecies`)。
      仮の世界の `ProbeGasField::Step` もこれを呼ぶ(probe_gas.cpp から移した。値は同じ)
- [x] `multires_gas_test`(release): 静かな大気(閉じた端・開いた端。20 刻み)・燃える木箱(段の外で O2 → CO2 + 熱・N2 を除く = 気体の質量が増える/減る。60 刻み)・
      熱い泡(開いた端・閉じた端。60 刻み)で、全部のセル・運動量の欄・前の質量・運動エネルギーの和・帳簿(エネルギー・運動量)・刻みの報告が毎刻みビット一致。保存点に戻して進めると同じハッシュ
- [x] probe_gas・gas_reference・gas_projection・gas_tree・cell_volume・probe_phase・multires_liquid(release)が今まで通り
- [x] gpu_probe_gas(probe_gas.cpp を触ったので。GPU と CPU の仮の世界が毎刻みビット一致のまま。rebase の前に確かめた)
- [x] release のビルド 警告 0・archmap --check

## メモ・参考
- **形**: 刻むブロック = 根のレベルの本物のブロック全部(活性を見ない)。セルの並びは「ブロックの順 × ブロックの中の番号」。世界の箱(根のブロックの外接の箱)の座標から
  面の一覧(`GasFaceLink`: 両側 + 外側の隣 + 軸 + 面の番号)を作り、gas_reference の小刻み(Substep)をそのまま回す。小刻みは前から面の一覧しか見ていなかったので、
  変えたのは「セルの層」を箱の並びの割り算から `cellLayers` に出しただけ(値は同じ)。面の番号(乱数の丸め)と開いた端の外の層は箱の座標から = 仮の世界と同じ数。
- **縦は z**(上 = +z。世界の端 world_edge.hlsli〔T-0721〕・気体の木・気体の箱と同じ)。気体の箱の軸 = 世界の軸なので欄の運動量の並びは x・y・z。
  **食い違い**: T-0717 の液体は y を上にしている(LiquidGrid::HeightCells)。どちらかにそろえる(おすすめ z。T-0764)。
- **欄**(T-0711 の登録): `gas.momentum`(セル ×3・gas・VOLUME・保存量)・`gas.kinetic_energy`(セル・同)・`gas.mass_before`(セル・SAME・ハッシュ)・
  `gas.ledger_momentum`(世界 ×3)・`gas.ledger_energy`(世界)。保存点・写し・状態のハッシュは登録を回るので気体の分は直さずに入る。
- **見立て**: 気体の空きは `MakeCellView(...).gasOpen`(仮の世界の ShareCellVolume + GasOpenQ16 と同じ値。T-0713 の試験 + この試験の空きの数で一致)。
  全部のセルが全部なら空にする(仮の世界と同じ = GfParams の「空きを読む」が 0)。一様なブロックは見立てを 1 回。
  気体のぶんのエネルギーと成分は今はセルから(gas_cells の GasEnergyPart。見立ての CvPhaseSums に化学のエネルギーの和が無いため)。
- **世界の端**: 上・下・横ごとに wall(押し返す。帳簿は壁の力積)/ open(外は層の基準の大気。出入りは帳簿)。仮の世界の軸ごとの境界より細かい(上だけ開くなど)。
  箱の中の抜け(ブロックの無い所)は壁。値は T-0721 の WeWorld の端の種類(WE_WALL = 0・WE_OPEN = 1)と同じ。
- **帳簿**: 物質は世界の帳簿の列(理由 = 世界の端。物質を変えるのは開いた端だけなので理由は正しい)。エネルギー(開いた端 + 重さの仕事)と
  運動量(壁の力積 + 開いた端 + 無くなった気体)は気体の箱の帳簿が理由を分けていないので、世界の欄に置いた(液体と同じ形。T-0763)。
  → ComputeConservedTotals はまだエネルギー・運動量の気体の分を合わせられない(T-0763 で直す)。
- **断るもの**: 根のレベル ≠ 0(係数は 0.5 m のセル)・溢れのセル・頁が枠より少ない・基準の大気や音速の無い表・小刻みの安定の上限。投影は無い(D-515 で既定は陽解法)。
- 費用(CPU release 1 スレッド・2 × 2 × 3 ブロック = 6144 セル・小刻み 24。perf.md): 仮の世界 → 多重解像度 = 静かな大気 133 → 134 ms/刻み・燃える木箱 338 → 338・熱い泡 289 → 286(揺れの中)。
  層の基準の大気を毎刻み作り直している(Create の時は世界の箱が分からないため。安いが、活性で収める時に一緒に見る)。

## 載せ替えの口(仮。継ぎはぎを残さないため、ここを替える)
1. **当てる段へ**(T-0765): 上限は気体の箱の小刻みの中(出る側だけ: LimitOutflow・LimitMovedAmounts)。T-0714 の当てる段(TxFlowCell。出る側と入る側の倍率の min)へ
   面の生の包み(成分ごとの量・エネルギー・運動量)を渡す形に替える。入る側の上限が増えるので値は変わりうる(差を説明して基準を取り直す。D-487 ③)。
   係数をレベルの関数にする(0.125 m³ の PRESSURE_PER_AMOUNT_TEMPERATURE_Q32・Δx = 0.5 m の centralFlow など → レベル k。小刻み 2^σ)のも同じ本で。
2. **帳簿**(T-0763): gas_reference の GasLedger を理由(世界の端・壁の力積・重さの仕事)ごとに分け、世界の帳簿の列へ。世界の欄 gas.ledger_* を消す(液体の T-0769 と一緒に)。
3. **世界の端の口**(T-0764): T-0721 のマージ後、`MultiresGasOptions` の top / bottom / sides をやめて世界の端(WeWorld)から読む。縦の軸を液体とそろえる。
4. 活性のブロックの一覧(T-0723 の後)・投影(T-0722)・欄が物質に付くを当てる段の 1 か所へ(T-0718。今は gas_cells の LoadGasCells)。
5. 報告の型は仮の世界の `ProbeGasStepReport` の別名(比べるため)。仮の世界を退かせる時(T-0724 の後)に名前を移す。

## 分けたもの
- **T-0763**(工学・CPU): 気体の帳簿を理由ごとに世界の帳簿へ(T-0769 の液体と同じ形)。
- **T-0764**(工学・CPU): 気体・液体の世界の端を T-0721 の口から読む + 多重解像度の縦の軸をそろえる(液体は y・気体と世界の端は z)。
  → 気体の半分は T-0772 で片付けた(上の軸は world_edge.hlsli の WE_UP_AXIS の 1 か所・端は nest.edges.world から)。液体の半分は T-0774。
- **T-0765**(工学・CPU): 気体を当てる段(TxFlowCell)に載せ、係数をレベルの関数に(段 3 の多重レベルの前提)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-17(wt4・作業役): t-0717(液体)をこのブランチにマージしてから始めた(木の公開の口 ExpandNestPage・StoreNestCell・PokeNestBlock と同じ形にそろえるため)。
  gas_reference に面の一覧の口(StepGasLinks)、gas_cells(仮の世界から写しを移した)、multires_gas と試験を足した。最初の版から全部の場面で毎刻みビット一致。
  release のビルド 警告 0・multires_gas・multires_liquid・probe_gas・probe_phase・cell_volume・gas_reference・gas_projection・gas_tree が通る・archmap OK。
  gpu_probe_gas(probe_gas.cpp を触ったので)も通った。
- 2026-10-17(続き): main(T-0714・T-0717・T-0721・T-0723)へ rebase。T-0723 が仮の世界の気体に足した静止の丸め(restSpeedUnits)を、
  移した写し(gas_cells の StoreGasCells)へ入れ、多重解像度の気体にも同じ設定を足した(帳簿は gas.ledger_momentum に含む。列を分けるのは T-0763)。
  試験に「熱い泡 + 静止の丸め」を足した。rebase 後に release のビルド 警告 0・multires_gas・rest_return・multires_liquid・gas_reference・probe_gas を回した(結果は返答)。
