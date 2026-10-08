# T-0025 `--frames N --screenshot`・画像比較

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(作業役 1 体・2 時間以内)
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: docs/design/16-debug-test.md §5 / 決定: D-107・ADR-0011・ADR-0035(時間の操作は世界の結果を変えない)

## 目的
決まった画面を写して基準の画像と比べ、描画が壊れたら気づけるようにする。描画は浮動小数点なので、GPU・ドライバの違いで少しずれうる前提の比べ方にする。

## 完了条件
- [x] 写る世界を毎回同じにする: `--screenshot-tick t`(刻み t の始めで止めて S(t) を描いたフレームを写して終える)・`--auto-ignite`(最初の刻みに載る火)
  (`--frames N --screenshot` は T-0015 で済み。ただし写る刻みが実行ごとに違うので比較には使えない)
- [x] 比べ方: tools/image_compare/image_compare.py(色の許容 8・数の許容 0.1 %・差の画像・大きさ違いは失敗。Python の標準だけ)
- [x] テスト: ctest image_probe_volume・image_probe_slice(写す + 比べる。gpu)・image_compare_selftest(CI でも回る)・sim_scheduler(止まる刻み)
- [x] 基準の画像の置き場所(tests/images/*.png)と作り方・置き換え方を 16 §5 に。map.yaml

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役・wt4): `--frames N --screenshot` は T-0015 で既にあった。シミュは現実の時間で進むので、最後のフレームの刻みは決まらない →
  SimScheduler::SetStopTick と、抽出の刻みを覚えて S(t) を描いたフレームを写す `--screenshot-tick` を足した(足りなければ単位 0 個の抽出だけのリスト)。
- 窓のクリックはフレームに付くので載る刻みが決まらない → `--auto-ignite`(最初のフレームの 1 回だけ。最初に投げる刻みに必ず載る)。
- 比べる道具は Python の標準だけで BMP/PNG を読み書き(基準は RGB の PNG で 1 枚 10〜20 KB)。同じ機械で 2 回写して差 0 を確かめた。
- ScreenshotCapture::WriteBmp が置き場所のフォルダを作るように。

## 引き継ぎメモ(HANDOFF に載せる内容)
- `bicameral --frames 3000 --screenshot-tick 240 --auto-ignite --screenshot <bmp>` で決まった画面。`--editor` は重ねない(数が変わる)。
- ctest `image_*` は debug で 1 つ約 2 分(パイプラインの作成)。窓が出るので gpu ラベル。比べ方の自己試験 `image_compare_selftest` はラベル無し(CI)。
- 基準の置き換え: `python3 tools/image_compare/image_compare.py update out/build/debug/tests/images/<名前>.bmp tests/images/<名前>.png`(16 §5.3)。
- 描画(probe_view)・世界の中身(初めの世界・伝導・反応)を変えると画像も変わる。意図した変化なら基準を置き換えて、理由をコミットに書く。

## 分けたもの
- なし(別の GPU で許容を決め直すのは、AMD などの機械を用意した時に 16 §4 で。D-207 の機種間と一緒)

## 判断待ち
- なし(比べ方の許容と基準の置き場所は実装の細部として Claude が決めた。遊びへの影響は無い)
