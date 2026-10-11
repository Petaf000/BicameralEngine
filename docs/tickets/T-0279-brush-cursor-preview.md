# T-0279 カーソルの下に筆の形を描く

- Status: Review(作業役・wt2・ブランチ t-0279〔t-0266 の上〕。司令塔のマージ待ち。t-0266 の後にマージする)
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 2 時間以内(T-0266 の載せ直しと同じ作業役)
- マイルストーン: M2(docs/plan/ROADMAP.md)
- 設計: docs/design/14-editor-tools.md §2「実装(T-0279)」/ 決定: D-449・D-107 / 元: T-0245 の「分けたもの」

## 目的(1〜2 行)
筆を持っている時、カーソルの下に筆の形(球・直方体・円柱)と大きさを描き、置く前にどこに当たるかが分かるようにする。
描画側だけ(シミュに入れない)。

## 完了条件(チェックできる形で)
- [x] DebugViewController が押していない時もカーソルの断面のセルを返す(CursorCell。断面・カメラを動かすと付いてくる)
- [x] 置くコマンドと同じ読み方(common/probe_place.hlsli)で、断面の上の覆うセルの縁と形を囲む箱の線を作り、画面に描く
- [x] 人がいなくても確かめられる: ctest brush_preview(CPU。形の式から数えたセルの数・縁の数・切り方・射影の逆・CursorCell)
- [x] release と debug のビルド・ctest brush_preview・brush_stroke・brush_undo・debug_camera(作業ログ)

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-11(作業役 wt2・約 35 分): editor/brush_preview(線を作る・画面へ)・render::ProjectToPixel・DebugViewController::CursorCell・
  BrushPanel::PreviewShape・エディタの殻の DrawBrushPreview(ImGui の背景の描画リスト)・frame_loop の MakeBrushPreview。ctest brush_preview を足した。
  release・debug のビルドが通り、debug: brush_preview・brush_stroke・brush_undo・debug_camera、release: その 4 本 + window_replay_record・
  window_replay_editor・window_place_record・window_place_play が通過。窓で人が見た確認はしていない(線の色・太さは仮)。

## 引き継ぎメモ(HANDOFF に載せる状態)
- 筆を持つと、カーソルの下に断面の切り口(濃い黄色。置くとそこのセルに当たる)と形を囲む箱(薄い白)が出る。
  editor/brush_preview(線を作る)・render::ProjectToPixel(格子の点 → 画素)・DebugViewController::CursorCell。
- スクリーンショットの比較は足していない(ImGui の線は --screenshot の写しに入るか未確認。CPU の試験で形を確かめる)。

## 判断待ち
- なし(見た目の色・太さは仮で Claude が決めた。直しやすい: editor_overlay.cpp の DrawBrushPreview)

## 分けたもの(司令塔が ROADMAP に足すか決める)
- なし(「まだ無いもの」は design/14 §2 に書いた: 形の 3D の表面・足す/置き換えで色を変える・材料の色)
