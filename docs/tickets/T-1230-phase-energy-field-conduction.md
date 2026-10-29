# T-1230 層のエネルギー Ec の欄を世界の木へ・伝導の面を部分ごとに(T-1211 から分けた)

- Status: Todo(2026-10-29 wt4 が T-1211 から分けた。**ADR-0500 改訂〔2026-10-29 本体 16 回目〕で形が決まった: T-1252 段 1 の後**)
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

## 作業ログ
- 2026-10-29 wt4: T-1211 から分けて作った。
