# T-0266 窓の世界が物質を足す差し替えの直後から debug と release で違う(reaction.hlsli:179)を直し、エディタのホットリロードを既定で Remap に

- Status: Done(2026-10-11。main に統合 c044ed4)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(作業役 2 時間以内)
- マイルストーン: M2(docs/plan/ROADMAP.md。D-438)
- 設計: docs/design/13-scripting-game-model.md §2.5 / 決定: D-438・D-428・ADR-0065(追記)・ADR-0079 / 元: T-0254 の「分けたもの」

## 目的(1〜2 行)
debug の窓で物質を足す差し替え(`--species-remap`。オゾンを足す)の直後から reaction.hlsli:179(RxMulS64Checked。RxChemicalEnergy の桁あふれ)の
assert が出て、release の再生と S(101) から食い違う原因を突き止めて直し、エディタのホットリロードの既定を Remap にする。

## 完了条件(チェックできる形で)
- [x] 原因を突き止める → debug のシミュのシェーダーの -Od(T-0239・ADR-0079)。付け替えの側の穴ではなかった(下の「原因」)
- [x] release でも黙って桁あふれしていないか確かめる → release の記録 3 本を debug(assert つき)で再生して assert 0 件・ハッシュ全部一致
- [x] エディタのホットリロードの既定を Remap に(frame/table_hot_reload の既定。frame_loop は方針を渡さない。ADR-0065 に追記)
- [x] debug と release の窓の確認(window_hot_reload_* は --species-remap 付き)を通す(作業ログ)
- [x] 読んで見つけた、フレームの区切り方による差し替えの穴(生の置くコマンド)を塞ぐ

## 原因
1. **T-0254 の食い違いは -Od のシェーダー(T-0239)だった。** T-0254 の debug の記録はどれも ADR-0079(debug でもシミュのシェーダーは -O3)の前の
   -Od のビルド(T-0254 の作業ログ「-Od を外す試しの変更は戻した」)。古いログの assert の数はフレームごとに 64・128・256 の倍数
   (1 刻みに決まった数のセルが毎刻み桁あふれ = 世界が壊れていた)。ADR-0079 の「-Od で作った記録は記録した時にもう壊れていた」と同じ。
   -O3 の今の debug では(2026-10-11・RTX 3070 Ti・6 ランナー並走):
   - debug の記録(--editor --auto-reload --species-remap --auto-ignite --frames 3000)覗き窓あり 7 回(ctest 1 + 6)・なし 3 回: assert 0 件、
     release で再生して全部一致(差し替えの刻みは 29〜112・74〜164・92〜221 とばらけた。フレームの区切りもばらけている)。
   - release の記録 3 本(覗き窓あり)を debug で再生: assert 0 件・全部一致 → release も同じ所で黙って桁あふれはしていない(D-428 の嘘は無い)。
   - 前の失敗率(-Od で 4/9 ほど)なら 10 回続けて通る見込みは 1% 未満。
2. **読んで見つけた別の穴(直した。フレームの区切り方による)**: 刻みの途中で終わったフレーム(適用の単位が無い)で足した生の置くコマンドは
   次の刻み s に当たる。待っている表が次のフレームで s に当てられると、そのコマンドは前の表の ID のまま GPU のキューにあり(付け替えは
   そのフレームのコマンドだけ)、s の付け替えの後に別の物質を置いていた(ID がずれる。オゾンを足すと oxygen の後ろがずれる)。
   - 直し(frame_loop): 前のフレームまでに足した生の置くコマンドの最後の刻み + 1 を覚え(`m_queuedPlaceTickEnd`)、差し替えはそれ以降の
     刻みにだけ当てる(遅れは最大 1 刻み)。巻き戻すと 0 に戻す(キューは空になる)。
   - 巻き戻しで足し直す控え(m_rewindFeed)と履歴(m_commandHistory。付け替える前の写しを持っていた)の、差し替えの刻み以降の置くコマンドも
     新しい ID に付け替える。
   - 試験は無い(窓の確認の --auto-reload は置くコマンドを使わない)。下の「分けたもの」。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-11(作業役 wt2・約 1 時間 40 分): 付け替えの道(RecordTableSwap → RemapSpecies → 適用 → RefreshTable・覗き窓の ApplyDueTables・
  物理〔物質 ID を持たない〕・保存点〔差し替えで捨てる〕・熱のキャッシュ〔2 世代とも作り直す〕)を読んで穴が無いのを確かめ、-O3 の debug で
  記録 10 本・release の再生 10 本・release の記録 3 本の debug の再生がすべて一致・assert 0 を確かめた → 原因は ADR-0079 の -Od。
  エディタの既定を Remap にし、生の置くコマンドの穴を塞いだ。ADR-0065・design/13 §2.5 に追記。
  release: window_*(18 本)・reaction_hot_reload・reaction_replay_table・reaction_species_remap・float_check_*・gpu_probe_sim・gpu_probe_peek の
  25 本が通過。rebase(main 1f3e3b7。文書だけ)の後の debug: reaction_hot_reload・reaction_species_remap・window_place_record/play・
  window_hot_reload_copy/record/play・window_table_edit(_copy)の 9 本が通過(窓の assert 0 件)。

- 2026-10-11(作業役 wt2・載せ直し・約 1 時間 30 分〔テスト待ちが大半〕): `git rebase main`(c4cb51d。T-0243・T-0278 が入った後)。
  frame_loop.cpp・table_hot_reload.h・main.cpp の衝突を解いた: **通常の起動でもファイルを見る(T-0243)+ 既定は Remap(T-0266)**。
  D-438 が「通常のゲームの起動でも物質を足す・消す変更も名前で付け替えてその場で当てる」なので、エディタだけでなく通常の起動も Remap に揃えた
  (ADR-0047 の改訂の「Remap にするのは T-0266 の後」に追記・ADR-0065 の追記・design/13 を直した)。ctest window_hot_reload_game_record に
  `--species-remap` を足し、通常の起動の付け替え(オゾンを足す・消す)も確かめる。
  T-0278 の元に戻すとの組み合わせ: 運びは番号(sequence)で見分け、付け替え(RemapPlaceCommand)は番号を変えないので、控え・履歴の付け替えの
  後も運びを除ける。元に戻すは最後の差し替えより前の保存点へ戻らない(PlanBrushUndo)ので、足し直す控え・履歴はどれも新しい表の ID のまま当たる。
  release と debug のビルドが通り、`^(window_hot_reload.*|window_place_.*|window_table_edit.*|window_replay_.*|reaction_hot_reload|
  reaction_species_remap|reaction_replay_table|brush_undo|gpu_probe_(sim|peek))$` の 22 本が release・debug とも通過(release の 1 回目は
  window_place_play の時間切れと window_replay_peek の不一致〔S(1) から〕が出たが、流し直して 4 本とも通過。6 ランナー並走の重なり。
  window_place_play の時間切れは本体のランナーでも同じ日に出ている)。

## 引き継ぎメモ(HANDOFF に載せる状態)
- ホットリロードは既定で Remap(物質を足す・消す表も当てる)。エディタも通常の起動(T-0243 でファイルを見る)も。`--species-remap` は
  `--auto-reload` のオゾンの段を足すだけ。
- 生の操作で表を差し替える刻みは、前のフレームまでに足した置くコマンドが全部当たった後(frame_loop の TakeTableSwaps)。
- debug の窓の世界が正しいかは、debug の記録を release で再生・release の記録を debug で再生すれば分かる(今回のやり方)。

## 判断待ち
- なし(既定を Remap にするのは D-438・ADR-0065 の既定路線。実装の細部は Claude)

## 分けたもの(司令塔が ROADMAP に足すか決める)
- **T-0284(案)差し替えの前後の置くコマンドの試験**: `--auto-reload --species-remap` に、差し替えを待っている間に筆で置く段を足し、
  置いた物質が名前どおり(オゾンを足した後の oxygen が oxygen のまま)かを確かめる。今回の穴の直しは読んで確かめただけ。
