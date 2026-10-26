# T-1176 木の細かくする段のセルの中身を「置かれ方で配る」(CPU と GPU)

- Status: Todo
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
