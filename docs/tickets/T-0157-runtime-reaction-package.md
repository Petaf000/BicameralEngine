# T-0157 ランタイムがパッケージの反応表を読む

- Status: Done(この範囲。テストの表をパッケージへ寄せるのは T-0169・再生ファイルに表の版を残すのは T-0170)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(2 時間の約束の中。並走の作業ツリー wt3・ブランチ t-0157)
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: docs/design/02-reaction-system.md §2、13 §2 / 決定: D-318・D-320・D-428・ADR-0031・ADR-0032・ADR-0033

## 目的(1〜2 行)
ゲーム本体・エディタ・テストが、C++ に書いた表ではなく Luau のパッケージからベイクした反応表を使えるようにする。
読み込みの失敗(壊れた表・検査で落ちる)は起動時にはっきり止める。今の場面がビット単位で前と同じ結果になることをテストで確かめる。

## 完了条件(チェックできる形で)
- [x] 置き場所と配り方: リポジトリの data/packages → ビルドが exe の横の data/packages に写す(ADR-0033)
- [x] 入り口 script::LoadReactionTable(engine/src/script/reaction_table_loader.*)。ランタイム(frame_loop。エディタも同じ exe)が起動時に使う。`--packages <dir>`
- [x] 失敗(フォルダが無い・ゲーム本体が無い / 読めない・形の誤り・ベイクの検査)は窓を開く前に止まる。Luau として読めない Mod は除いて続け警告
- [x] 今の場面が C++ の表と毎刻みビットで同じ(reaction_package_scene)
- [ ] (分けた)テストの表をパッケージへ寄せて C++ の表を消す(T-0169)・再生ファイルに表の版を残す(T-0170)

## メモ・参考
- 実装: engine/src/script/reaction_table_loader.{h,cpp}・frame/frame_loop.cpp の LoadWorldReactionTable・engine/CMakeLists.txt の bicameral_data。
- テスト: reaction_package(入り口の成功と失敗 6 通り)・reaction_package_scene(今の場面)・reaction_package_runtime_stops(壊れた表で exe が止まる。
  tests/packages_broken)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(1 チャット目・並走の作業ツリー wt3・ブランチ t-0157): 試験の表のパッケージを tests/packages → data/packages に移し、ビルドが bin/data に写す形にした。
  入り口 script::LoadReactionTable(reaction_table_loader)を作り、frame_loop が窓を開く前に使う(`--packages`。失敗は「反応表のパッケージ(フォルダ): …」で止まる)。
  テスト: reaction_package に入り口の成功と失敗 6 通り、reaction_package_scene(今の場面を 60 刻み、パッケージの表と C++ の表で毎刻みハッシュ・最後のバイト・抽出が同じ。
  炭と CO2 ができる場面)、reaction_package_runtime_stops(壊れた表で exe が止まる)。ADR-0033。
  release で `-Filter "^(smoke|singleton|log|sim_scheduler|debug_camera|replay_file|reaction|multires|fixed|physics|float_check|luau|time_control|image)"` 42 本(約 7 分。
  image_* は exe がパッケージの表で写して基準の画像と一致)と `-Filter "^(window_replay|gpu_reaction)"` 7 本(約 6 分)が通過。debug で reaction_package・_scene(62 秒)・
  _runtime_stops・reaction が通過。release・debug とも新しい警告なし(implicit_conduction.cpp の C4189 は前から。wt2 の範囲)。tidy は流していない。
  読んでベイクする時間は release で約 13 ms(並走の負荷あり。正式な計測ではないので perf.md には書いていない)。

## 引き継ぎメモ(HANDOFF に載せる状態)
- 動いているもの: `bicameral`(エディタも)は exe の横の data/packages から反応表をベイクして使う(ログに「反応表: パッケージ …・版 …・読んでベイク … ms」)。
  確認: `-Filter "^(reaction|luau)"`(release で約 12 秒)。window_replay_*・image_* も exe 経由でパッケージの表を使って通る。
- 壊れているもの: なし。
- 決めたこと(Claude・実装の細部): ADR-0033(置き場所 data/ → bin/data・起動時にベイク・失敗は窓の前で止める・読めない Mod は除いて警告)。
- 注意:
  - **試験の表のパッケージは data/packages/combustion_test に移った**(tests/packages は無くなった)。中身は C++ の表(reaction_test_table.cpp)と同じで、
    片方を変えたら両方(reaction_package と reaction_package_scene が食い違いを落とす)。
  - data/ のファイルを変えたらビルドで bin/data に写る(bicameral_data。写す前に bin/data を消す)。exe だけを別の所へ写すときは data/ も一緒に。
  - GPU と CPU を比べるテスト(gpu_*・multires_* など)は今まで通り C++ の表(中身はビットで同じ)。並走の作業ツリーと衝突しないよう移していない。

## 判断待ち
- なし(置き場所・失敗の扱いは実装の細部として ADR-0033 で決めた。Mod の上書きの扱いは QUESTIONS Q8 のまま)。

## 分けたもの
- T-0169 テストの反応表をパッケージに寄せて C++ の試験の表を消す(gpu_*・multires_*・reaction_* の約 30 本が MakeCombustionTestTable を使う。
  並走の作業ツリー〔wt2 の陰解法のテストなど〕と衝突しやすいので、並走が落ち着いてから一度に)。
- T-0170 再生ファイルに表の版と読んだパッケージの一覧を残し、再生の時に違えば知らせる(15 §4・ADR-0031 の 6。今は版をログに出すだけ)。
