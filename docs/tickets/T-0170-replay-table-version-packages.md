# T-0170 再生ファイルに表の版と読んだパッケージの一覧を残す

- Status: Review(作業役・wt3・ブランチ t-0170。司令塔のマージ待ち)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(T-0193 と一緒に。2 時間の約束の中)
- マイルストーン: M2 (docs/plan/ROADMAP.md。T-0157 から分けた)
- 設計: docs/design/15-save-replay-mods.md §2.2・§4 / 決定: ADR-0031・ADR-0033・ADR-0047・ADR-0050(新)

## 目的(1〜2 行)
再生ファイルに、初期状態の反応表の版と読んだパッケージの一覧(と「改造された世界」の印)を残し、再生の時に今の表と違えば知らせる。

## 完了条件(チェックできる形で)
- [x] 形式の版 2: 見出しの表の版 = 初期状態の表の版(今までは常に 0)。表ごとに版・読んだパッケージ(読んだ順)・改造された世界の印を持つ(save/replay_file)
- [x] 古い形式(版 1)を読む: 表の無い形として読む(15 §1 の変換)。予約の欄が 0 でない版 1・知らない版は理由つきで拒否(replay_file_test の TestReadsVersion1)
- [x] 再生の時: 表の中身がある → それを使う(T-0193)。中身が無い版 1 → 今のパッケージの表で再生して警告。
      版が書いてあって中身が無く、今の表と違う → 窓を開く前に止める(frame_loop の LoadWorldReactionTables)
- [x] 往復・壊れた表の拒否のテスト(replay_file_test の TestTablesRoundTrip・TestRejectsBrokenTables)

## メモ・参考
- 実装: engine/src/save/replay_file.{h,cpp}(ReplayTable・FindTable・表の読み書き)、frame/frame_loop.cpp(AttachReplayTables・LoadWorldReactionTables)。
- 表の中身(T-0193)と同じ形式の変更なので、一緒に入れた(ADR-0050)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役・wt3・ブランチ t-0170): T-0193 と一緒に形式の版 2 を入れた(T-0193 の作業ログと同じ実行)。版 1 は読める・知らない版は断る、をテストした。
  release で replay_file・reaction_replay_table・window_replay_* 6 本(no_packages を足した)を含む 11 本が通過。新しい警告なし。

## 引き継ぎメモ(HANDOFF に載せる状態)
- 動いているもの: `--record` の再生ファイルに表の版・パッケージの一覧・改造された世界の印・表の中身が入る(ログ「記録: …反応表 N 個」)。
  `--replay` は再生ファイルの表を使う(ログ「反応表: 再生ファイルの表を使う」)。確認: `-Filter "^(replay_file|reaction_replay_table|window_replay.*)$"`。
- 壊れているもの: なし。
- 決めたこと(Claude・実装の細部): ADR-0050。古い形式の扱い(読む)は 15 §1 の方針どおり。
- 注意: 再生ファイルの形式の版が 2 になった(REPLAY_FORMAT_VERSION)。ビルドのフォルダに残っている版 1 の window_replay.bcreplay は、
  record のテストが書き直す(版 1 のままでも今のパッケージの表で再生できる)。

## 判断待ち
- なし(「改造された世界」の印を実績・共有でどう扱うかは QUESTIONS Q8 のまま。印は残すだけ)。
