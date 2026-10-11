# T-0243 通常のゲームの起動でも反応表のファイルを見る(D-438)・プレイ中に法則が変わったら改造の印(D-437)

- Status: Done(2026-10-11)
- 種類: 工学
- PC: 必須
- 見積もり: 小〜中(作業役 1 時間ほど)
- マイルストーン: M2(docs/plan/ROADMAP.md。D-438)
- 設計: docs/design/13-scripting-game-model.md §2.5 / 決定: D-437・D-438・ADR-0047(改訂)/ 元: T-0223 の「分けたもの」

## 目的(1〜2 行)
`--editor` でなくても約 0.25 秒ごとに指紋を見て反応表を当てる。読み直した表は「改造された世界」の印付き(記録にも残る)。
物質の一覧が変わる差し替えは今の方針(Reject。T-0266 が直るまで)のまま。

## 完了条件
- [x] frame/table_hot_reload を再生でなければ常に見る(frame_loop の watch の条件)
- [x] 読み直した表に modifiedWorld を付ける・世界の印(TableHotReload::WorldModified)・「状態」パネルと差し替えのログに出す
- [x] `--auto-reload` を `--editor` なしでも使える・ctest window_hot_reload_game_(copy|record|play)
- [x] ADR-0047 の改訂・13 の該当行

## 作業ログ
- 2026-10-11(作業役・本体): frame_loop の watch を `replayPath.empty()` に(--editor でなくても 15 フレームごとに指紋)。TableHotReload::Poll が読み直した表を写して
  modifiedWorld を付け、TakeSwaps が世界の印(WorldModified)を立てて差し替えの行に「(改造された世界)」を出す。「状態」パネルの反応表の欄にも出す。
  `--auto-reload` は --editor なしでも使える。ctest window_hot_reload_game_(copy|record|play)(record は --editor なし・play は再生の差し替えの行の印を正規表現で見る)。
  ADR-0047 に改訂の節・13 の行。物質の一覧が変わる差し替えは Reject のまま(T-0266 の後に Remap)。
- テスト: debug・release の game_record が通過(debug 90 秒・release 36 秒)。play は再生 OK・印も出たが、正規表現の括弧を逃がしていなかったので直して流し直し、debug・release とも window_hot_reload_game_* が通過(release は window_hot_reload_*・window_replay_*・window_lab・window_table_edit* と合わせて 19 本)。

