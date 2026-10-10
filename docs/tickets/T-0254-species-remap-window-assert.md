# T-0254 窓の世界で物質の数が変わる差し替えの GPU の assert を直し、エディタのホットリロードを既定で Remap に

- Status: Review(作業役・wt2・ブランチ t-0254。司令塔のマージ待ち)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(作業役 2 時間以内)
- マイルストーン: M2(docs/plan/ROADMAP.md。D-438)
- 設計: docs/design/13-scripting-game-model.md §2.5 / 決定: D-438・ADR-0065・ADR-0047 / 元: T-0242 の「分けたもの」

## 目的(1〜2 行)
窓(エディタ + 覗き窓)で物質の一覧が変わる表を当てると reaction.hlsli:179(RxChemicalEnergy の桁あふれ)の assert が毎フレーム出て
DEVICE_HUNG になるのを直し、エディタのホットリロードの既定を Remap にして、窓の確認を ctest に入れる。

## 完了条件(チェックできる形で)
- [x] 原因を突き止めて直す(下の「原因」)
- [x] エディタのホットリロードの既定を Remap に(frame/table_hot_reload の既定の引数。frame_loop はもう方針を渡さない)。
      `--species-remap` は --auto-reload にオゾンを足す・消す段を足すだけになった
- [x] window_hot_reload_record に `--species-remap` を付け、play の正規表現を差し替え 3 回に
- [x] release で窓の再現が通る・debug でも流す(結果は作業ログ)

## 原因(2 つ。1 は直した・2 は T-0239)
1. **覗き窓が刻みの途中で終わったフレームで、付け替えた後の写しを前の表で読んでいた(直した)**。
   覗き窓(sim/probe_peek)の `ApplyDueTables` は、世界が刻み s の始めに替えた表を「抽出の境界の刻み t > s」の時だけ当てていた。
   窓のフレームは刻みの途中で終わることがある(1 刻み 4 単位をフレームの予算で分ける)。刻み s の適用の単位(単位 0)を含んで s の途中で
   終わったフレームの抽出の境界は S(s) で、GPU の世界は適用の単位の前に 2 世代のセルを新しい表の ID に付け替える
   (ProbeSim::RecordSpeciesRemap)ので、S(s) の写しはもう新しい ID。覗き窓はそのフレームだけ前の表で写し・影の鎖を回していた
   (物質の ID が 1 つずれて h0・比熱を引く。次のフレームで表を替えて作り直すので 1 フレームだけ)。
   gpu_probe_peek_test・gpu_probe_sim_test のフレームはいつも刻みの境界で終わるので通っていた。
   直し: GPU の覗き窓は `s <= t` で替える(差し替えはその刻みの適用の単位を記録するフレームにだけ積まれるので、t == s なら必ず付け替えた後)。
   CPU リファレンス(ProbePeekReference)は刻みの境界だけで抽出し、S(s) は付け替える前なので `s < t` のまま。
2. **差し替えの後に続く assert の群れと DEVICE_HUNG は T-0239(debug で覗き窓を開くと起動の約半分で世界が壊れる)の現れ**と見ている。
   根拠(debug の覗き窓つきで --auto-reload --species-remap を流した起動 6 回のログ。out/build/debug/bin/logs と runner/logs の 2026-10-10〜11):
   - 差し替えの後に reaction.hlsli:179(ログでは fixed.hlsli:179 と出る)が 128〜512 件/フレーム続いた 2 回(T-0242 で見たものを含む)は、
     どちらも差し替えの前、起動の直後に fixed.hlsli:156・:45 の assert が出ている(T-0239 の「食い違う起動」の印)。
     起動の直後に assert の無い 2 回は、物質を足す・消す差し替えの後も assert 0 で通った。起動の直後に assert が出ても群れの出ない起動もある
     (直す前の `<` で 1 回)。
   - 1 回は物質を足す差し替えの前(起動の直後)に fixed.hlsli:179・:613 も出て、差し替えの前に DEVICE_HUNG(Sim の DispatchGraph)。
     T-0239 の作業ログの DEVICE_HUNG(伝導の DispatchGraph)と同じ所。
   - 群れが差し替えの直後に始まり 15 刻みほどで止まるのは、差し替えの刻みに全部のブロックを起こす(RefreshTable。T-0139)ので、
     眠っていた壊れたセルの熱を計算し直すからと見ている(未確認。群れの長さは起きたブロックが眠るまでの長さに合う)。
   release(assert 無し・T-0239 は起きない)では窓の記録と再生が差し替え 3 回で一致する(作業ログ)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-10〜11(作業役 wt2・前の作業役が中断 → 引き継いで約 1 時間 40 分): 前の作業役の未コミットの変更(覗き窓の `<=`・既定の Remap・
  ctest)を活かし、debug のシェーダーの -Od を外す試しの変更は戻した。debug の窓を `<=` と `<` で流し比べ、差し替えの後の assert の群れは
  起動の直後から assert の出る(T-0239 の)起動にだけ出ると分かった(「原因」2)。CMake の正規表現は `{3}` を持たないので play の正規表現は 3 回並べた。
  release: window_hot_reload_copy・_record・_play(差し替え 3 回・ハッシュ 106 個一致)・window_replay_peek・window_replay_record・
  window_table_edit(_copy)・gpu_probe_peek・gpu_probe_sim・reaction_hot_reload・reaction_replay_table・reaction_species_remap が通過。

## 引き継ぎメモ(HANDOFF に載せる状態)
- エディタのホットリロードは既定で物質を足す・消す表も当てる(名前で付け替える)。`--species-remap` は --auto-reload の物質の段だけ。
- window_hot_reload_record は `--species-remap` 付き(速度の書き換え → オゾンを足す → 消す。再生は差し替え 3 回)。
- debug の窓で差し替えの後に assert の群れ・DEVICE_HUNG が出たら、まず起動の直後の assert(fixed.hlsli:45・:68・:156)を見る。
  出ていれば T-0239 の起動(release では起きない)。window_hot_reload_* の debug は T-0239 の分だけ時々落ちる(前から)。
- 覗き窓の `ApplyDueTables` は GPU が `<=`・CPU リファレンスが `<`(理由はコメントと「原因」1)。刻みの途中で終わるフレームの差し替えを
  CPU と突き合わせる試験は無い(gpu_probe_peek_test のフレームは刻みの境界で終わる)。

## 判断待ち
- なし

## 分けたもの(司令塔が ROADMAP に足すか決める)
- なし(差し替えの後の debug の assert の群れは T-0239 に含める。T-0239 の手がかりとして「差し替えで全部のブロックを起こすと、壊れた起動では
  眠っていたセルの assert が出る」を足すとよい)
