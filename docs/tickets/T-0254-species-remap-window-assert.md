# T-0254 窓の世界で物質の数が変わる差し替えの GPU の assert を直し、エディタのホットリロードを既定で Remap に

- Status: Review(一部完了。作業役・wt2・ブランチ t-0254。司令塔のマージ待ち。残りは T-0266)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(作業役 2 時間以内)
- マイルストーン: M2(docs/plan/ROADMAP.md。D-438)
- 設計: docs/design/13-scripting-game-model.md §2.5 / 決定: D-438・ADR-0065・ADR-0047 / 元: T-0242 の「分けたもの」

## 目的(1〜2 行)
窓(エディタ + 覗き窓)で物質の一覧が変わる表を当てると reaction.hlsli:179(RxChemicalEnergy の桁あふれ)の assert が毎フレーム出て
DEVICE_HUNG になるのを直し、エディタのホットリロードの既定を Remap にして、窓の確認を ctest に入れる。

## 完了条件(チェックできる形で)
- [ ] 原因を突き止めて直す → 1 つ(覗き窓)は直した。残り(debug の窓の世界が付け替えの直後から release と違う)は **T-0266**
- [ ] エディタのホットリロードの既定を Remap に → **T-0266**(残りの原因が直るまで `--species-remap` の時だけ。一度入れて戻した)
- [x] window_hot_reload_record に `--species-remap` を付け、play の正規表現を差し替え 3 回に
- [x] release で窓の再現が通る・debug でも流す(結果は作業ログ。debug は差し替えの後に assert が出る → T-0266)

## 原因(1 は直した・2 は未解決 → T-0266)
1. **覗き窓が刻みの途中で終わったフレームで、付け替えた後の写しを前の表で読んでいた(直した)**。
   覗き窓(sim/probe_peek)の `ApplyDueTables` は、世界が刻み s の始めに替えた表を「抽出の境界の刻み t > s」の時だけ当てていた。
   窓のフレームは刻みの途中で終わることがある(1 刻み 4 単位をフレームの予算で分ける)。刻み s の適用の単位(単位 0)を含んで s の途中で
   終わったフレームの抽出の境界は S(s) で、GPU の世界は適用の単位の前に 2 世代のセルを新しい表の ID に付け替える
   (ProbeSim::RecordSpeciesRemap)ので、S(s) の写しはもう新しい ID。覗き窓はそのフレームだけ前の表で写し・影の鎖を回していた
   (物質の ID が 1 つずれて h0・比熱を引く。次のフレームで表を替えて作り直すので 1 フレームだけ)。
   gpu_probe_peek_test・gpu_probe_sim_test のフレームはいつも刻みの境界で終わるので通っていた。
   直し: GPU の覗き窓は `s <= t` で替える(差し替えはその刻みの適用の単位を記録するフレームにだけ積まれるので、t == s なら必ず付け替えた後)。
   CPU リファレンス(ProbePeekReference)は刻みの境界だけで抽出し、S(s) は付け替える前なので `s < t` のまま。
2. **未解決: debug の窓では、物質を足す差し替えの刻みの直後から世界が release と違い、reaction.hlsli:179 の assert が出る**。
   - 最後の debug の window_hot_reload_record(rebase 後): 起動の直後の assert は 0 なのに、オゾンを足す差し替え(刻み 100)の直後から
     fixed.hlsli:179(= reaction.hlsli:179)が 208 件。その記録を **release で再生すると S(100) まで一致し、S(101) から違う**。
     起動からずれる T-0239 とは別で、付け替えの刻みに debug と release の世界が分かれる。
   - 同じ窓の debug の起動でも出ない時がある(覗き窓の `<=`・`<` のどちらでも、assert 0 の起動が 1 回ずつ)。出るかどうかはフレームの区切り方
     (刻みの途中で終わるか)によると見ている(未確認)。
   - release は記録と再生が一致する(差し替え 3 回・ハッシュ 106 個)が、同じ所で黙って桁あふれしているかもしれない(未確認。D-428 の嘘になる)。
   - 疑う所(T-0266): 差し替えのフレームの区切り(適用の単位が フレームの先頭/途中)ごとの RecordTableSwap → RecordSpeciesRemap → 適用 → RefreshTable、
     2 世代の付け替えの世代の取り違え(刻みの途中から始まるフレーム)、物理(積み木)が持つ物質 ID、保存点。
     確かめ方: debug の記録を release で再生して最初に違う刻みを見る(上のやり方)・gpu_probe_sim_test に刻みの途中で区切るフレームの差し替えを足す。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-10〜11(作業役 wt2・前の作業役が中断 → 引き継いで約 1 時間 40 分): 前の作業役の未コミットの変更(覗き窓の `<=`・既定の Remap・
  ctest)を活かし、debug のシェーダーの -Od を外す試しの変更は戻した。debug の窓を `<=` と `<` で流し比べ、差し替えの後の assert の群れは
  起動の直後から assert の出る(T-0239 の)起動にだけ出ると分かった(「原因」2)。CMake の正規表現は `{3}` を持たないので play の正規表現は 3 回並べた。
  release: window_hot_reload_copy・_record・_play(差し替え 3 回・ハッシュ 106 個一致)・window_replay_peek・window_replay_record・
  window_table_edit(_copy)・gpu_probe_peek・gpu_probe_sim・reaction_hot_reload・reaction_replay_table・reaction_species_remap が通過。
  rebase 後の debug の window_hot_reload_record で起動の直後の assert 0 のまま差し替えの後に 208 件出て、その記録を release で再生すると
  S(101)(差し替えの刻み + 1)から違った → T-0239 だけでは説明できないので、既定の Remap は戻して(frame_loop・main・table_hot_reload は main のまま)
  残りを T-0266 に分けた。rebase 後の release: 上の 16 本(float_check_* を足して)が通過。

## 引き継ぎメモ(HANDOFF に載せる状態)
- エディタのホットリロードはまだ `--species-remap` の時だけ Remap(既定にするのは T-0266)。
- window_hot_reload_record は `--species-remap` 付き(速度の書き換え → オゾンを足す → 消す。再生は差し替え 3 回)。
- window_hot_reload_* の debug: record は差し替えの後に assert が出ても通る(T-0266)。play は T-0239 の分だけ時々落ちる(前から)。
- debug の窓の世界が正しいかは、debug の記録を release で再生すれば最初に違う刻みが分かる(T-0239 と T-0266 の切り分けに使える)。
- 覗き窓の `ApplyDueTables` は GPU が `<=`・CPU リファレンスが `<`(理由はコメントと「原因」1)。刻みの途中で終わるフレームの差し替えを
  CPU と突き合わせる試験は無い(gpu_probe_peek_test のフレームは刻みの境界で終わる)。

## 判断待ち
- なし

## 分けたもの(司令塔が ROADMAP に足すか決める)
- **T-0266 窓の世界が物質を足す差し替えの直後から debug と release で違う(reaction.hlsli:179)を直し、エディタのホットリロードを既定で Remap に**:
  再現 `ctest --preset debug -R "^window_hot_reload_(copy|record)$"` の後、`bicameral(release) --replay out/build/debug/tests/window_hot_reload.bcreplay
  --peek 28,32,32 --peek-depth 9` で差し替えの刻み + 1 から違う(出ない起動もある)。上の「原因」2 の疑う所から。直したら
  frame/table_hot_reload の既定を Remap に(T-0254 で一度入れた形: frame_loop は方針を渡さず、`--species-remap` は --auto-reload の物質の段だけ)。
