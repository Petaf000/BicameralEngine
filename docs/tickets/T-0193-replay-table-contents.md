# T-0193 差し替えた表の中身を再生ファイルに残す

- Status: Review(作業役・wt3・ブランチ t-0170。司令塔のマージ待ち)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(T-0170 と一緒に。2 時間の約束の中)
- マイルストーン: M2 (docs/plan/ROADMAP.md。T-0139 から分けた)
- 設計: docs/design/15-save-replay-mods.md §2.2 / 決定: ADR-0031 の 6・ADR-0032・ADR-0047・ADR-0050(新)

## 目的(1〜2 行)
再生ファイルだけで(元のパッケージのフォルダが無くても)、ホットリロードの差し替えをまたいだ記録を同じ表で再生でき、ハッシュ列が一致するようにする。

## 完了条件(チェックできる形で)
- [x] 記録: 初期状態の表と、記録したコマンドの差し替えの印(PROBE_COMMAND_TYPE_TABLE)の版の表の中身(合わせた表の正準なバイト列 script::TableBytes)を残す
      (LoadedReactionTable::tableBytes・frame_loop の AttachReplayTables)
- [x] 再生: 中身から表を作り直す(script::RebuildReactionTable: 中身のハッシュ = 版 → script::ParseTableBytes〔ScriptValue の ReadCanonicalBytes〕
      → 読み戻した表の版も確かめる → 定義 → ベイク。Luau も型の定義も使わない)。再生ファイルの表を TableHotReload::AddKnown で知っている表に足す
- [x] パッケージのフォルダを消して、差し替えをまたいだ記録を再生 → 刻みごとのハッシュが全部一致・作り直した表の中身が同じ(reaction_replay_table。CPU リファレンス)
- [x] 窓: 記録を `--packages <無いフォルダ>` で再生して一致(ctest window_replay_no_packages)
- [x] 壊れた中身(1 ビット)・違う版・空は拒否

## メモ・参考
- 実装: script/script_value(ReadCanonicalBytes・ReadCanonicalString)・script/luau_package(ParseTableBytes)・
  script/reaction_table_loader(RebuildReactionTable・LoadedReactionTable::tableBytes)・frame/table_hot_reload(AddKnown・Find・InitialVersion)・
  frame/frame_loop(RebuildReplayTables・LoadWorldReactionTables・AttachReplayTables)・save/replay_session(ReplayPlayer::File)。
- 読み手(reaction_package.cpp・luau_package.cpp)の欄は変えていない(bicameral.d.luau も変えていない)。反応の核と反応表の GPU 用の形には触っていない。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役・wt3・ブランチ t-0170): 何を残すかを「合わせた表の中身」に決めた(ADR-0050。Luau のソースやベイクした GPU 用の表は採らない)。
  形式の版 2 の読み書き・中身から表を作り直す道・記録と再生のつなぎ・テスト(replay_file_test に表と版 1・reaction_replay_table・window_replay_no_packages)を書いた。
  最初の reaction_replay_table は「差し替えで結果が変わる」の確かめが落ちた(書き換えた carbon_combustion が 40 刻みでは効かない)→
  木の燃焼の速度と窒素の熱容量を書き換える形にした(gpu_probe_sim_test の MakeSwappedTable と同じ考え)。再生のハッシュは最初から 40/40 一致。
  release のビルドは新しい警告なし。release で `-Filter "^(replay_file|reaction_replay_table|reaction_package|reaction_hot_reload|luau_package|window_replay.*)$"`
  11 本が通過(window_replay_* 6 本は並走の負荷で 1 本 約 45 秒)。表の中身は今の試験の表で 1 つ 2781 バイト。

## 引き継ぎメモ(HANDOFF に載せる状態)
- 動いているもの: ホットリロード(`--editor`)をまたいだ `--record` の記録を、パッケージのフォルダ無しで `--replay` できる(再生ファイルの表を使う)。
  確認: `-Filter "^(reaction_replay_table|window_replay_no_packages)$"`(後者は window_replay_record が先に走る)。
- 壊れているもの: なし(未確認: 窓でホットリロードをまたいだ記録 → 再生の手の確認はしていない。窓の自動確認は T-0195 の `--auto-reload`)。
- 決めたこと(Claude・実装の細部): ADR-0050(残すのは合わせた表の中身。Luau のソース・GPU 用の表は採らない)。
- 注意:
  - LoadedReactionTable に tableBytes が増えた(LoadReactionTable が埋める)。表を作る別の道を足したら、ここも埋めないと記録に表が残らない
    (記録の時に「中身を持っていない」と警告して、その表を飛ばす)。
  - 再生ファイルに表があると `--packages` は読まない(再生の表は記録の表。今のパッケージと違っても気にしない)。

## 判断待ち
- なし。

## 分けたもの
- なし(T-0195 の `--auto-reload` で、窓の記録 → 再生をホットリロードをまたいで確かめる時に、この道も通る)。
