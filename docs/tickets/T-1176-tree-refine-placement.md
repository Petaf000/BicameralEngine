# T-1176 木の細かくする段のセルの中身を「置かれ方で配る」(CPU と GPU)

- Status: Doing(2026-10-26 本体: 設計の詰めだけ。コードは次の作業役)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 2 回分
- マイルストーン: 試作の列・土台の変更要求の束(docs/plan/ROADMAP.md の T-1159 の行から分けた)
- 設計: docs/design/20-transport-substrate.md §10.5・17 §3.2 / 決定: D-664・ADR-0469 の 2-1

## 目的(1〜2 行)
世界の木を細かくする時(CPU の multires_tree.cpp RefineRequestLevel・GPU の multires_graph.hlsl RefineLevel)、セルの中身(成分の量・エネルギー・端数)を
SAME ではなく置かれ方で子 2³ へ配る。欄(SF_FLAG_PLACE_*)は同じ重みを読む(MultiresNest::refinePlacement を木が自分で作る)。

## 完了条件
- [ ] 物質の表から置かれ方の値(相 → SF_SETTLE_*・密度・粒の詰まり)を作る(セルの量は µmol なので mg へ。モル質量は表)
- [ ] 隣の親の列の詰まりを読む(6 方向。隣が別のブロック・別のレベル・無い時の読み方を決める。世界の端は下 = 詰まり切る・上 = 空)・SfSurfaceSlopeQ16
- [ ] CPU と GPU(shaders/sim/placement_refine.hlsli の回しを RefineLevel から)でビット一致。SfSurfaceSlopeQ16・SfClampColumnSlopeQ16 の GPU の試験(64bit の差と比べを並べる形。T-1159 のメモ)
- [ ] 気体だけ・詰まり切った・据わる固体のセルは今とビット単位で同じ(今の多重解像度の試験がそのまま通る)。物質ごとの和はビット単位
- [ ] エネルギーは子の温度が揃う配り(熱容量の重み。T-1157 の一旦の決定)・端数の枠の扱い
- [ ] 観察の影(RefineShadow)を揃えるか決める(17 §3.2。1 本目は SAME のまま)
- [ ] 結果が変わる所(砂・水の混ざるセルを細かくする場面)の比べ: 0.25 m の 25° の斜面で空気の速さ(T-1106 の 2.8 m/s → 1 mm/s)

## メモ・参考
- GPU の子の番号は MrChildCell の j。CPU リファレンスの PlacedChildren とは sim::PlaceIndexOfMrChild で写す。
- wt3 の t-1105-air-agitation をマージする時、場面の作り方の SettleColumn・SettlePictureCell を sim/placement_refine に置き換える(HANDOFF)。

## 設計の詰め(2026-10-26 本体の作業役。コードはまだ。次の作業役はこの順で作る)
1. **量の単位**: 木のセルの数はレベルによらず濃度(multires.hlsli の「レベル k の物質量 = 8^-k µmol」)。子 8 つの和は親 × 8(子の単位)。
   置かれ方は「数の空間」で回す: 子 1 つの器 = レベル 0 のセルの体積 V0(mm³。レベルによらない)・配る量 = 親の数 × 8。寄せないセルは親の数のまま(SAME)。
2. **物質の値**(セルは µmol。mg と密度に直さない): 体積は表の molarVolume(mm³/mol。RxSpecies に既にあり GPU の表にも載る)から「数 × molarVolume ÷ 10^6」。
   state_field.hlsli に µmol の版の体積の関数を足し、SfAddSettle をそれで呼ぶ形に(mg の版は wt3 の場面のために残す。CPU と GPU の 1 つの源)。
   相: SpeciesPhase Gas → SF_SETTLE_GAS・Liquid → LIQUID・Solid + SolidMotion Loose → GRAIN・Fixed → ANCHORED。GPU の表へは RxSpecies::flags のビットで
   (RX_SPECIES_LIQUID・RX_SPECIES_GRAIN・RX_SPECIES_ANCHORED。語は増えない)。**粒の詰まり(%)は表に値が無い**(未確認。case_b は自分の表)→ 一旦 0(= 100 %)。
   パッケージに値を足すなら土台の値の追加なので司令塔へ(【エンジン】作り手が粒の詰まりを書くか)。
3. **端数(2^-64 単位)**: 整数部だけを置かれ方で配り、端数は子へ SAME(子 8 つの和 = 親 × 8 で保存はビット単位。1 単位未満なので形に効かない)。端数の枠の数は今と同じ。
4. **エネルギー(D-674)**: 子の熱(µJ。RxThermal::heat の親 × 8)を子の熱容量 C_j(Σ 数 × 比熱)の重みで配る(FxMulDivS64・最後の子が残り)。
   子のエネルギー(mJ)= (Σ 数 × H0 + 熱_j) ÷ 1000 の切り捨て・最後の子が残りで和を保つ(温度の差は 1 mJ ÷ C_j 以下)。C の和が 0 なら SAME。
5. **寄せないセル**(気体だけ・詰まり切った・据わる固体・物を含む)は SAME の道へ早く抜ける(今とビット単位で同じを道で保証する)。
6. **隣の読み方**: 傾きは SfSurfaceSlopeQ16(列の表面 = 下・ここ・上の詰まりの和 − 1。SfColumnSurfaceQ16)。要るのは親のセルの x±1・z±1 の列(各 3 セル)とここの列 = 15 セル。
   同じブロックの中は頁から、外は索引で同じレベルの隣のブロック。隣が無い・別のレベル → 横は bothSides = false(傾き 0。1 本目の仮)、
   下が無い → 世界の端は詰まり切る / 木の中は粗い親のセルの詰まり(1 本目は詰まり切ると読む仮)、上が無い → 空。
   GPU は RefineLevel のグループで、親の八分の一の 2³ セル + 周り 1 の 4³ の詰まりを先にグループ共有へ集める(子のブロック 64 スレッド = 子のセル)。
7. **GPU の形**: RefineLevel の「セルと端数」で t = 子のセル → 親のセル MrParentCellOfChild(octant, t)・子の番号 j(MrChildCell の j)。同じ親を読む 8 スレッドの組で
   PrChildWeightsOf の空きを集めて PrGasShare。成分は親の並びのまま量 0 の成分を消して詰める(同じ中身なら同じ並び)。溢れ(MR_WIDE_CELLS)の道は 1 本目は SAME のまま(印で分ける)。
8. **影(RefineShadow)**: 1 本目は SAME のまま(17 §3.2。観察は真実を変えない)。
9. **今の世界**: 標準の表で水・ばらける固体の混ざるセルを細かくする試験は結果が変わる(D-664 で置かれ方が既定なので既定 on。変わる試験は数えて期待値を直す)。
   今の CPU の多重解像度の試験(multires・multires_state・multires_edge・multires_gas・multires_liquid)のどれが変わるかを最初に流して数える。
10. **GPU の試験に入れる残り**: SfSurfaceSlopeQ16(64bit の差と符号の比べ)・SfClampColumnSlopeQ16(min)を placement_refine_check.hlsl から乱数の入力で呼び、CPU と 1 値ずつ比べる
    (HW の RTX で上の 32bit が壊れたら SfOpenVolume と同じく 32bit に分けて借りを手で回す)。

## 作業ログ
- 2026-10-26 本体: 4 本のマージと試験の待ちの間に、木のセルの単位(濃度)・µmol と molarVolume・端数は SAME・D-674 の熱容量の重み・隣の 15 セル・GPU の形を詰めた(上の 1〜10)。コードはまだ。
