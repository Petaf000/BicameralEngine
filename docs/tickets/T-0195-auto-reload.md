# T-0195 窓での自動の確認 `--auto-reload`

- Status: Review(作業役・wt3・ブランチ t-0194。司令塔のマージ待ち)
- 種類: 工学
- PC: 必須
- 見積もり: T-0194 と合わせてチャット 1 回分
- マイルストーン: M2 (docs/plan/ROADMAP.md。T-0139 から分けた)
- 設計: docs/design/14-editor-tools.md §2「実装(T-0194・T-0195)」/ 決定: ADR-0047・ADR-0050・ADR-0054

## 目的(1〜2 行)
窓を開いて、パッケージのファイルを書き換え → 刻みの境界で差し替え → 記録 → 別の起動で再生してハッシュ列が一致、を人なしで確かめる。

## 完了条件(チェックできる形で)
- [x] `--auto-reload`(frame/auto_reload。`--editor --packages <写し>` と一緒に。`--replay` とは使えない): 刻み 10 で reactions.luau を壊す
      → 読み直しに失敗して差し替わらない → 木の燃焼の前指数因子を 2 倍に直す → 差し替わる → 30 刻み流して元の中身に戻し、終える。
      どこかで違えば終了コード 1
- [x] ctest window_hot_reload_copy(data/packages をビルドのフォルダへ写す)→ window_hot_reload_record(火をつけた木箱の壁を覗きながら記録)
      → window_hot_reload_play(写しのフォルダ無しで、再生ファイルの表だけで再生。同じ刻みに差し替わり、ハッシュ列が一致)
- [x] 書き換えるのは --packages で渡した写しだけ(--packages が無ければ始めない)

## メモ・参考
- 実装: engine/src/frame/auto_reload.{h,cpp}(AutoReload)・frame/frame_loop(CreateAutoReload・RunFrame で Poll の後に Update・終わりの確かめ)・
  main.cpp(`--auto-reload`)・tests/CMakeLists.txt(window_hot_reload_*)。
- 記録の側は --frames 3000 を上限にしている(並走中でも 1 分半ほどで終わる)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役・wt3・ブランチ t-0194): AutoReload を足し、ctest を 3 本足した。debug で window_hot_reload_copy・_record(104 秒)・
  _play(121 秒)が 1 回目で通過。release でも 3 本が通過(_record 38 秒・_play 51 秒)。

## 引き継ぎメモ(HANDOFF に載せる状態)
- 動いているもの: `bicameral --editor --auto-reload --packages <写し> --record <file>`。確認: `-Filter "^window_hot_reload"`(3 本・約 4 分)。
- 壊れているもの: なし。
- 注意: 書き換える所は combustion_test/reactions.luau の `rate = { a = "2e10"`(木の燃焼)。試験のパッケージのその行を変えたら
  frame/auto_reload.cpp の RATE_BEFORE も変える(無いと --auto-reload は始めずに 1)。

## 判断待ち
- なし(エディタの確認の道具。遊びへの影響は無い)。
