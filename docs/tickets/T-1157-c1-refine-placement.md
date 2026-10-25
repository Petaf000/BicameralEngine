# T-1157 C-1 の改訂の 1 本目: 細かくする時は「置かれ方で配る」

- Status: Done(1 本目。残りは T-1159)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: 試作の列・土台の変更要求の束(docs/plan/ROADMAP.md)
- 設計: docs/design/20-transport-substrate.md §10.5・17 §3.2 / 決定: D-664・ADR-0469 の 2-1・D-532

## 目的(1〜2 行)
多重解像度で細かくする時の配り方を、SAME(子は親と同じ数)から「置かれ方で配る」1 つの規則にする。関数は state_field.hlsli(CPU と GPU の 1 つの源)に置き、
どの欄がどの置かれ方に乗るかは登録の印で決める(作業役が欄ごとに考えない)。

## 完了条件(チェックできる形で)
- [x] state_field.hlsli に置かれ方の関数(`SfAddSettle`・`SfSettledVolume`・`SfSettles`・`SfSettleFullnessQ16`・`SfColumnSurfaceQ16`・`SfSurfaceSlopeQ16`・`SfClampColumnSlopeQ16`・`SfColumnWeight`・`SfSettleTopChild`・`SfSettleBelowShare`・`SfOpenVolume`・`SfPlacedValue`)と登録の印(`SF_FLAG_PLACE_CONDENSED`・`SF_FLAG_PLACE_OPEN`・無印 = SAME。`SfRefinePlacement`・`SfDescValid` で 1 つだけ・真実のセル・面の欄だけ)
- [x] wt3 の `SettleColumn`・`SettlePictureCell`(T-1106)と同じ答え: tests/placement_refine_test.cpp に式を変えずに写した参照と、乱数の絵 40 枚 × 96 セル(n = 2: 寄せた 2452 セル・n = 4: 2491 セル)と縦の列 200 本で凝縮相の子の量・傾き・寄せる判定がビット一致
- [x] 物質ごとの和はビット単位で保つ(3 次元 500 通り)。気体だけ・詰まり切った・据わる固体・物は SAME。半分の砂は下の段に詰まり、気体は空きへ多く入る
- [x] 欄の細かくする(nest_fields::Refine)に置かれ方を渡す口(`MultiresNest::refinePlacement`)。口が無い世界と同じ重みを返す口の世界で細かくした後の状態の要約(HashWholeNest)が一致・偏った重みでは印の欄だけが配られ和 = 8 × 親・無印は SAME・重みの無い子に端数を入れない
- [x] release のビルド警告 0・archmap --check OK

## メモ・参考
- 1 本目に入れたものの形: 子の番号は `MrChildCell(local, j)` の j(x が bit 0・y が bit 1 = 重さの向き・z が bit 2)。残りは重みのある最後の子へ。一様な親の子は一様のまま(SAME)。
- 1 本目に入れないもの(T-1159 へ): 木の細かくする段のセルの中身(成分・エネルギー・端数)を置かれ方で配る・隣が別のブロック / 別のレベルの時の読み方・GPU の欄(段 6 で欄が GPU に来る時)・
  2-2 内訳・2-3 眠る時に捨てる・2-5 支えの細かくする形を世界の木へ・観察の影(RefineShadow)を揃えるか。
- 【エンジン】仮(ユーザー未確認)で進めたもの: エネルギーは子の温度が揃う配り(熱容量の重み)。作り手から見た違い: 細かくした直後の子の温度が親と同じ(重さの向きに沿った温度の偏りを作らない)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-26 本体: 記録(D-663〜D-668)の後に着手。CPU リファレンス sim/placement_refine(wt3 の形を移し、3 次元は x で分けてから z で分ける。2 次元は wt3 とビット一致)。
  気体は子の空きの体積で配り直す(wt3 は場面の圧力から書き直していたので比べない)。詰まりが Q16 の 1 目盛りに届かない列の重みの和 0 は同じ重みに(wt3 の場面では起きない)。
  欄は `MultiresNest::refinePlacement`(親のセルごとに子 8 つの重み)で配る。今の世界はどの欄にも印が無く口も無いので結果は変わらない(試験は全部そのまま通る)。
  通した試験(release): placement_refine・field_storage・multires_state・warm_start_save・multires_uniform・case_b_box・multires・multires_graded・multires_wide・multires_activity・multires_quiet ほか。
  **未確認**: state_field.hlsli を include するシェーダーがまだ無いので、新しい式が DXC で通るかは確かめていない(GPU の欄が来る T-1159 で)。GPU のビット一致も同じ。
