# T-0219 反応表のパネル

- Status: Review(作業役・wt2・ブランチ t-0219。司令塔のマージ待ち)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(作業役 2 時間以内)
- マイルストーン: M2(docs/plan/ROADMAP.md。D-449)
- 設計: docs/design/14-editor-tools.md §2「実装(T-0219)」/ 決定: D-449・D-438・ADR-0047(ホットリロード)・ADR-0031〜0033

## 目的(1〜2 行)
`--editor` で反応表(元素・物質・規則)を一覧・検索し、値をその場で変えて Luau のファイルに書き戻す。ホットリロードがそのまま世界に当てる。
保存則の検査(元素の釣り合い・生成エンタルピーの差と書いた反応熱・ベイクの警告)の結果を並べる。中身を「触りながら決める」ための道具(D-449)。

## 完了条件(チェックできる形で)
- [x] パネル「反応表」(editor/reaction_table_panel。editor_overlay.cpp には 2 行だけ): 規則・物質・元素のタブ、名前と式で検索
- [x] 値を押して変えて「書き戻す」: A・Ea・書いた反応熱・係数・生成エンタルピー・比熱・熱伝導率・原子量。
      「始まる温度」(k = 0.01 /s になる温度。表示の定義は仮)を入れると Ea を決めて書く。「最後の書き戻しを戻す」
- [x] 書き戻しは .luau の該当するリテラルの字面だけを差し替える(コメント・並び・ほかの値は 1 バイトも変えない。script/luau_literal_edit)。
      値が式(`solid(120)` など)・2 か所にある値は書き戻せない(理由を浮き出しで出す)
- [x] 保存則の検査の表示: 規則ごとの元素の釣り合い・生成エンタルピーの差(反応熱)・書いた反応熱・ベイクの警告・読み直しの失敗(古い表のまま)
- [x] テスト reaction_table_edit: 書き戻す → 読み直す → 変えた値だけ違う同じ表 → 戻す → 元のファイル・元の版。同じ値はファイルを変えない。
      係数を変えて元素が釣り合わない → 読み直しが落ちる。再生ファイルの表は書き戻せない。字句(コメント・長い括弧・関数の本体)
- [x] 人がいない確認 `--auto-table-edit`(ctest window_table_edit_copy → window_table_edit): パネルの書き戻しで木の燃焼の速さを変える
      → ホットリロードで当たる → パネルの「戻す」で戻す → ファイルが元の中身・元の版の表に戻って終える

## メモ・参考
- 実装: engine/src/script/luau_literal_edit.{h,cpp}(字句を読み、表の鍵の並びでリテラルを探して差し替える。Luau は走らせない)・
  script/reaction_table_edit.{h,cpp}(表の中身 TableBytes → 定義 → 行と保存則の検査。書き戻す所はパッケージのフォルダを読んだ順の後ろから探す)・
  editor/reaction_table_panel.{h,cpp}(ImGui。--auto-table-edit の状態機械も)・frame/frame_loop.cpp(UseTable・終わりの確かめ)・main.cpp。
- 書き戻すフォルダはホットリロードが見ているフォルダ(`--packages`。無ければ exe の横の data/packages = ビルドの写し)。
  **リポジトリの data/packages を直すなら `--packages <リポジトリ>/data/packages` で起動する**(既定のままだと次のビルドの写しで消える)。
- 書いた後の検査(型検査・ベイク)はホットリロードの道をそのまま通る。落ちたら古い表のまま、パネルの上に理由が出る。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-10(作業役・wt2・ブランチ t-0219): luau_literal_edit・reaction_table_edit・reaction_table_panel と `--auto-table-edit` を足した。
  reaction_table_edit は 1 回目で通過(debug 23 秒)。window_table_edit は 1 回目で通過(debug 87〜108 秒)。
  `-Filter "^(reaction.*|luau.*|window_hot_reload.*|window_table_edit.*|window_lab)$"` 18 本のうち 17 本が通過。window_hot_reload_play が
  刻み 17〜24 のハッシュが記録と違って落ちた(差し替えの刻み 45 より前)。流し直しても刻み 8〜11 などで落ちる(2 回とも。原因は未確認)。

## 引き継ぎメモ(HANDOFF に載せる状態)
- 動いているもの: `bicameral --editor [--packages <dir>]` のパネル「反応表」。確認: `-Filter "^(reaction_table_edit|window_table_edit_copy|window_table_edit)$"`。
- **未確認の失敗**: window_hot_reload_play(T-0195)がこのブランチで 2 回続けて落ちた(再生の刻み 8〜24 のどこかで 4〜8 刻み続けて
  ハッシュが記録と違い、その後は合う。差し替えより前)。パネルは世界に触れない(ファイルを読むだけ)が、記録の側(--editor)で最初のフレームに
  一覧を作る(パッケージのフォルダを読む)ので、フレームが 1 回重くなる。仮説: 重いフレームで捨てた刻みのまわりで、記録か再生の
  ハッシュの読み戻しが別の刻みの値を取る(main にもある時間に依る不具合が出やすくなった)。確かめ方: main で同じ 3 本を流す・
  frame_loop の BuildEditor の `ReactionTable().UseTable` を外して流す。パネルのせいなら、一覧を作るのをパネルを開いた時だけにする。
- 注意: --auto-table-edit が書き換えるのは combustion_test の reactions.cellulose_combustion.rate.a(--auto-reload と同じ値。一緒には使わない)。

## 判断待ち
- **「始まる温度」の定義**(仮・ユーザー未確認): 速度定数 k = A·exp(−Ea/RT) が 0.01 /s(1 秒に 1% が反応する速さ)になる温度にした。
  プレイヤーと遊びへの影響: エディタの表示だけで、世界の反応は変わらない(書き戻すのは Ea)。今の試験の表では 590〜1090 K になり、
  reactions.luau のコメント「600〜1000 K で燃え始める」と合う。別案: 0.1 /s(はっきり燃えて見える温度。約 30〜60 K 高く出る)・
  量の 1% が 1 刻みで反応する温度。おすすめ: 今の 0.01 /s のまま(取り消しは定数 1 つ。editor/reaction_table_panel.cpp の START_RATE_PER_SECOND)。

## 分けたもの
- なし(物質を足す・消す変更のパネル〔D-438 の T-0223〕・組成の書き換えはこのチケットに入れていない。組成は今のホットリロードが当てないので表示だけ)。
