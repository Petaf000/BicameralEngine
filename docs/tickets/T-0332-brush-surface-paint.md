# T-0332 筆で面に沿って置く・塗る

- Status: Done(2026-10-11。本体)
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 1 体・2 時間以内
- マイルストーン: M2(docs/plan/ROADMAP.md)
- 設計: docs/design/14-editor-tools.md §2「実装(T-0332)」/ 決定: ADR-0089 追記(T-0332)・D-107・D-403 / 元: T-0280 の「分けたもの」

## 目的(1〜2 行)
筆を断面の平面だけでなく、カーソルの光線が最初に当たる物(空気でないセル)の面の上に置き、ドラッグで面に沿って塗れるようにする。
光線の当たり判定は CPU で、読み戻した物の印から(シミュに入れない。記録されるのは置く位置だけ)。

## 完了条件(チェックできる形で)
- [x] 物の印(1 セル 1 ビット・64³ = 32 KiB): 抽出の後ろのフックで GPU が作り(shaders/sim/probe_surface.hlsl)、フレームの枠ごとに読み戻す(sim/probe_surface)。
      物とみなすのは物質量の合計がしきい値(仮 20 mol。common/probe_surface.hlsli)以上。CPU リファレンス BuildProbeSurfaceMask と gpu_probe_place で毎回ビット一致
- [x] 光線(editor/surface_pick。Amanatides–Woo の DDA): 最初に物のセルに入る直前の空いたセルと越えた面の軸。CPU のテスト surface_pick
- [x] 窓: 筆のパネルに「当て方: 断面 / 面に沿って」。左ボタン・ドラッグ・カーソルのセルが面に沿う(render::DebugViewController::SetCellPicker。拾えなければ断面)。
      ドラッグの間は押した時の印のまま。カーソルの下の表示(T-0279)は面の軸で形を切る
- [x] 自動の確認: `--auto-place` が元に戻すの後に、決まった光線で木箱の東の壁の外(x = 36)を拾ってドラッグで 6 個置く(window_place_record/play)

## メモ・参考
- 下書き: docs/tickets/T-0280-place-apply-cost.md「分けたもの」。
- 物の印は View の状態(D-107)。印は数フレーム前の世界で、置く位置はコマンドとして記録されるので、再生は印を使わない(D-403)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-11(作業役・本体): 物の印(sim/probe_surface・probe_surface.hlsl・common/probe_surface.hlsli。抽出の後ろのフック・枠ごとの読み戻し。
  ProbeExtractContext に枠の番号 slot)・光線(editor/surface_pick)・窓(DebugViewController::SetCellPicker・CursorRay、筆のパネルの「当て方」、
  frame_loop の UpdateSurfacePicker〔運びの途中は押した時の印〕・カーソルの下の表示は面の軸で切る)・`--auto-place` の面に沿う手順を足した。
  gpu_probe_place の印の比べ方は、刻みの途中で終わるフレームの抽出には適用の単位が当てた置くコマンドが入るので、刻みの境界のフレームだけにした。
- 同日: debug・release のビルドは警告なし。release の置く・つつく・再生・ホットリロードの束 30 本と debug の筆・置くの束が通過(HANDOFF の「テスト」)。かかった時間は約 1 時間(待ちを除く)。

## 引き継ぎメモ(HANDOFF に載せる内容)
- しきい値は仮 20 mol(common/probe_surface.hlsli の PROBE_SURFACE_THRESHOLD_MICROMOLES。表に相を持たせる時に置き換える)。
- 物の印を読み戻すのは、面に沿っての筆を持つ間と `--auto-place` の間だけ(ProbeSurface::SetEnabled)。印は数フレーム前の世界。
- 刻みの途中で終わったフレームの抽出 S(t) には、その刻みの適用の単位が当てたコマンドが入っている(CPU リファレンスの S(t) とは違う。描画も同じ)。

## 判断待ち
- なし(しきい値は仮。取り消しは定数 1 つ。エディタの見え方だけで世界の結果は変わらない)

## 分けたもの
- なし(まだ無いもの〔表の相で見分ける・面の向きに形を傾ける・覗き窓の細かいセルの面〕は 14 §2 に書いた。要る時に ROADMAP へ)

