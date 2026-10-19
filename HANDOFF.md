# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-10-19(本体の作業役・第 11 巡の続き 2: マージ後の main の確かめ・D-527〜D-531 の記録。**ユーザーの指示で作業を止めた**〔設計の見直し〕)

## 状態(3 行以内)
- **止めた(2026-10-19 14 時台)。claude/26 の ★ の反映と、案 B の設計(現象の一覧)を会話で詰めてから再開する**(★ の反映は設計が詰まった後にまとめて)。D-528 は保留に戻した(決定 4 の手順を通していない。案 B で拡散の家族と運動量の家族の端数をそれぞれ決め直す)。止めたブランチ(マージしない): wt2 t-0784(ad1a2c3。ビルドが通らない途中)・wt3 t-0807(c3e46dd。気体の活性は済み。液体の WIP は止まった水が眠らない 1 件と main との衝突が残る)・wt4 t-0813(結果は報告待ち)・wt5 t-0834(557528b。チケットだけ)・wt6 t-0820(32359d7。実装済み・試験の一部と文書 3 つが未コミット・wt6 のランナーが止まっている)・t-0751(0e1572f。マージ待ち)。
- **2026-10-19 止めた状態(第 11 巡の続き 2)**: ユーザーの指示で作業を止めた(設計の見直しのため)。main = 下の続き + D-527〜D-531 の記録。**マージ後の試験は release ビルド(警告 0)・CPU 30 本・GPU 4 組 28 本が通った。流していないのは GPU の 2 組(gpu_gas の 11 本)**と、走っていた gpu_probe_sim の 4 本(下)。t-0751(wt5・0e1572f)は**マージしていない**(main での試験の前に止めた)。
- **2026-10-19 第 11 巡の続き**: main = T-0804(最速を選ぶ欄)+ t-0724(T-0724 + T-0670)+ t-0819 + t-0778 + t-0787 + 記録。**注意: t-0819・t-0778・t-0787 のマージ後の release ビルドと試験はまだ**(H: の空きが約 470 MB でビルドを止めた。空いたら下の「残り」を流す)。
- **2026-10-19 第 11 巡(再開)**: main = 第 10 巡の締め + D-521〜D-525 + t-0766(T-0766・T-0773)+ t-0790(T-0790)+ t-0796(T-0796。設計だけ)+ t-0788(T-0788)+ t-0761(T-0761)+ この記録。作業中(マージしない): wt2 t-0724(仕上げ済み 5a27c72。T-0670 を上に入れてからマージ)/ wt3 T-0807 / wt4 T-0787 / wt5 T-0778 / wt6 T-0819。
- ユーザーの決定 D-521(T-0725・T-0726 を前倒し。それまで仮の世界は世界いっぱいで遅いまま)・D-522(T-0778 は端数の持ち越し)・D-523(ADR-0347 Accepted)・D-524(T-0787 は 2 本試してから閉じる)・D-525(ADR-0359・0360 は全部案 1)・D-526(ADR-0353 Accepted。最速を選ぶ欄は環境に 1 行 = T-0804)。
- **③' 運ぶ計算の共通の土台(D-509)**: 段 0〜4 はほぼ済み(残り T-0724・T-0784・T-0778・T-0787・T-0804)。次は段 5 の縦切り(T-0807 → T-0725・T-0726)。「続き」は司令塔として並走(COMMANDER.md)。

## この仕事でしたこと(2026-10-19 第 11 巡の続き・本体の作業役)
- **T-0804**(main で直接): 環境の `gas_pressure_speed = "sound" | "instant"`。仮の世界の CPU は instant で最速の投影・読み込み(LoadReactionTable・RebuildReactionTable)が
  CheckLayoutFastestOpenings(sim/gas_fastest_layout。cell_volume.cpp を bicameral_gas_projection へ移した。BakeReactionTable の中からは層の向きで呼べない)で警告・
  MR と GPU は「最速はまだ効かない」と知らせる。release 警告 0・environment_package・standard_packages・probe_gas・gas_projection(_plates)・multires_gas・reaction_package(_scene・_runtime_stops)・cell_volume が通った(マージ前)。仮は Q131。
- マージ: **t-0724**(衝突 probe_sim_gas.cpp: GPU の「まだ効かない」を BuildGasWork へ = 表の差し替えでも出る。区画が世界いっぱいになったので cell_volume の読み込みの試験は世界を分ける壁に)→ release 警告 0 まで。
  **t-0819**(衝突なし)・**t-0778**(perf.md の行を両方)・**t-0787**(probe_gas.cpp: 熱の列の行と CollectProbeGasOpen を両方)。
- 記録: ROADMAP(T-0804・T-0724・T-0670・T-0819・T-0778・T-0787 を Done・T-0687 の読み替え・T-0813・T-0814 の行)・DECISIONS(D-507 に取り直しの説明・D-522 に ADR-0357・D-524 に ADR-0355)・QUESTIONS Q131〜Q136。
- **マージ後の確かめ(2026-10-19 続き 2・本体の作業役)**: release ビルド警告 0。CPU 30 本(^multires 14 本・cell_view・cell_volume・transport_apply・reaction_package(_scene・_runtime_stops)・environment_package・standard_packages・luau_type_check・probe_liquid・rest_return・probe_phase・gas_reference・gas_projection(_plates)・gas_tree・probe_gas・multires_liquid・multires_gas)が通った。
  GPU: window_hot_reload_* 6 本・window_play_* / gravity_reload_* 8 本・window_rebuild_* / blank_* / mixer_* 7 本・gpu_world_copy・gpu_probe_rewind(_regions)・gpu_probe_gas(_warp)・gpu_probe_liquid(_warp) 7 本が通った。落ちた試験は無い(直したもの無し)。archmap --check OK。
  **流していない(止めた)**: `^gpu_gas(|_flow|_blast|_species|_projection)$`(5)・`^gpu_gas_(warp|flow_warp|blast_warp|species_warp|tree.*)$`(6)。キューから runner/_to_delete へ退かせた。gpu_probe_sim(_warp)・gpu_probe_sim_tables(_warp) は止めた時に GPU の順番待ちで走っていた(結果は runner/logs/20261019-044729-test.*。この HANDOFF の時点では未確認)。再開したら最初にこの 2〜3 組を流す。
- 記録: D-527(眠らせるのは (a)・止まりかけの粗視化は研究 T-0833)・D-528(端数の共通の決まり T-0834。wt5 が t-0751 の上で始めた)・D-529(穏やかな火の場面 T-0835)・D-530(混ざるの判断待ち 3 つ。開いた端は外の基準の大気と混ざる = T-0837)・D-531(Q129・Q130 確定)・D-522/D-525/D-526 の追記(Q131〜Q135 確定。game = true で安息角の書き忘れを誤りに = T-0836)。QUESTIONS の Q129〜Q136 を消した。ADR-0357・0361 の仮を外した。ROADMAP に T-0828・T-0829・T-0833〜T-0836。
  **番号の注意**: 9d75617 では Q129・Q130 の確定を D-530 として書いたが、D-530 は混ざるの判断待ちに使い、Q129・Q130 は D-531 へ付け直した。T-0836 = game = true で安息角の書き忘れを誤りに・T-0837 = 混ざるの開いた端。
- t-0751 のマージ(止めた): 再開したら main へ入れ、multires_diffusion・^multires・cell_view・transport_apply・environment_package・reaction_package・archmap --check・float_check → ROADMAP の T-0751 を Done・ADR-0367 を Accepted(D-530)。
  → GPU(4〜8 本ずつ): window_hot_reload_*・window_gravity_reload_*・window_play_table_*・window_play_record/play・window_rebuild_*・window_blank_*・window_mixer_*・gpu_world_copy・gpu_probe_rewind(_regions)・gpu_probe_gas(_warp)・gpu_probe_liquid(_warp)・gpu_probe_sim(_warp)・gpu_probe_sim_tables(_warp)・gpu_gas$・gpu_gas_flow・_blast・_species・_tree・_projection(warp も)。archmap --check は通った。

## 前の仕事(2026-10-19 第 11 巡・本体の作業役)
- 文書: DECISIONS D-521〜D-526・QUESTIONS に「①〜④ は D-521〜D-524 で答え済み」の注・ROADMAP ③'(T-0725・T-0726 を前倒し・T-0807 を T-0725 の前・T-0819〜T-0821 の置き場所・T-0766・T-0773・T-0790・T-0796 を Done)・ADR-0347・0359・0360・0353 を Accepted・ROADMAP に T-0804・NEXT・COMMANDER(第 11 巡の配り)。
- マージ(int-12 で 1 本ずつ → main へ早送り): **t-0766**(衝突は map.yaml の注と perf.md の行だけ。両方残した)→ release 警告 0・^multires 13 本・GPU 17 本(gpu_multires_conduction(_warp)・subcycle 4 本・wide 7 本・stress・limits・near_fold(_warp))。**t-0790**(自動。transport_apply.hlsli は両方の足し分が別の所)→ 和集合 CPU 16 本(^multires・cell_view・transport_apply・cell_volume を 1 本の -Filter「^(multires.*|cell_view|transport_apply|cell_volume)$」で)+ GPU の 4 組(上の 17 本 + gpu_multires_implicit_conduction(_levels)(_chain) の 4 本)・archmap --check。**t-0796**(設計だけ)→ archmap --check。**t-0788**(自動。map.yaml)→ gas_projection・gas_projection_plates・cell_volume・probe_gas・multires_gas・rest_return・archmap --check。GPU の和集合は t-0788 を入れた後のビルドで流した。**t-0761**(自動)→ CPU の和集合をもう一度(^multires・gas_projection(_plates)・cell_volume・cell_view・transport_apply・probe_gas・rest_return)+ debug の multires_state。
- 運転: 1 回目の release ビルドが bicameral.exe のリンクで「別のプロセスが使用中」で落ちた(一時的。もう一度で通った)。順番待ちの間に git を先に通すときは runner/queue の試験のファイルを runner/hold へ退かせて戻した。

## 前の仕事(2026-10-18)
- 2026-10-18 5 本のマージの確かめと記録(t-0767・t-0772・t-0770・t-0759・t-0758。D-518・Q120〜Q123)・第 10 巡(int-10・int-11・t-0781・t-0782)・止めた状態の記録。詳しい節は ed3d6f7 の HANDOFF.md。

## 前の仕事(2026-10-17)
- 2026-10-17 ③' 段 0〜4 のマージの確かめと記録(t-0712〜t-0721 の 11 本 + t-0757・t-0720。D-516・D-517・Q114〜Q119)。詳しい節は 59ef841 の HANDOFF.md。
- ユーザーの決定 D-509〜D-515・ADR-0326・docs/design/20・ROADMAP ③'(段 0〜9)を記録し、t-0414b(T-0697)・t-0622(T-0622)を main へ。t-0689・t-0668・t-0686 は捨てた(ブランチは残す)。詳しい節は d2a10a2 の HANDOFF.md。
- 第 9 巡の終わり(d105aa6): 第 9 巡の続き(T-0647・T-0653・T-0654・T-0623・T-0667・T-0676・T-0671・T-0672)のマージを release で確かめ、D-508 と止めたブランチ 5 本を記録。詳しい節は d105aa6 の HANDOFF.md。
- 第 9 巡の前半: T-0613 液体の GPU 版 / T-0631 気体の市松の根っこ(音速の頭打ち 約 372 K)/ T-0646・T-0656 流れの欄は物質に付く / T-0616 固体・液体に密度(D-505)/
  T-0597 比熱は c_p のまま(D-506)/ T-0617 GPU の気体の陽解法が空きを読む / T-0620 の前半(区画のある世界の保存点・写し・ハッシュ)。probe_gas の煙の高さの物差しを直し、燃える木箱の基準を取り直した(D-507)。詳しい節は 80fab2e の HANDOFF.md。
- 第 8 巡: T-0619 格子の y を上に / T-0598 GPU の気体を何種でも / T-0611 相変化を規則の対で / T-0593・T-0612 セルの体積を相で分け合う。全体の見取り図(claude/24)。詳しい節は 1aada77 の HANDOFF.md。
- それより前: 第 7 巡の詳しい節は c8c4a52、第 5 巡と第 6 巡は a90acdd、第 4 巡は 68a3b07、第 3 巡は c0d8c42、第 1・2 巡は f778b4a、2026-10-13 の節は 2960562 の HANDOFF.md にある(400 行に収めるため消した)。

## 動いているもの(確認方法つき)
- **テスト(2026-10-17 ③' 段 0〜4 のマージの後・release)**: 上の「状態」のとおり(試験の組は runner/main-batches.txt〔git 管理外〕の形で 4〜9 本ずつ。CPU 3 組・GPU 15 組〔^gpu_probe_l を足した〕・窓 9 組・image 1 組。-Filter は「|」を使わず文字の組で)。燃える木箱の長い測りは `slope_check_test --burn` を raw で切り離して(Start-Process)流すとランナーを塞がない。
- それより前のテストの記録(どの組をどの順で流したか)は前の版の HANDOFF.md(2026-10-12 は 2960562 の前・2026-10-11 以前は aa3e55c)と各チケットの「テストの結果」にある。
- `job.py test` は分けて投げる(GPU の組は 4〜8 本ずつ。T-0426 で長い試験を分けた)。例: `-Filter "^gpu_multires(_warp|_activity|_activity_warp|_quiet|_quiet_warp|_uniform|_uniform_warp)?$"`・
  `-Filter ^gpu_multires_conduction`・`-Filter gpu_multires_subcycle`・`-Filter "gpu_probe|window_replay"`・CPU だけの組
  (`-Filter "^(smoke|singleton|log|sim_scheduler|debug_camera|replay_file|multires|fixed|physics|float_check)"`)と `-Filter reaction`。
  裏で投げて runner/logs/<job>.result.json を待つ(cat しない。job.py の要約で見る)。テストの表示した行は out/build/<preset>/Testing/Temporary/LastTest.log(走っている間は .tmp)。
- `python3 tools/archmap/archmap.py --check` は Linux 側で動く。

## 壊れている/未確認のもの(ファイル:行 と症状)
- **サンプルの世界と ⑤ の場面では、写しと遊びの指標が過渡期のため使えない(D-521。多重解像度の世界へ移るまで)**: t-0724 で仮の世界を世界いっぱいにすると 1 刻みが約 85 ms でフレームの予算を超え、写しが 1 刻みも進まない(D-468)ので、写しで進む窓の試験 3 組(window_copy_*・window_regions_copy_*・window_metrics_*)を DISABLED にする(戻す条件は tests/CMakeLists.txt と ADR-0342。t-0724 のマージで入る)。t-0724 はホットリロードの窓の試験 4 本が CheckRegionTableSwap で落ちるので、T-0670 を上に入れてからマージする。
- **気体・液体の区画の残り**: 刻みの終わりの熱が負のセルは 0(T-0676。probe_gas の EXPECT(0))。強い爆発の 1 小刻みの中だけ熱が一時的に負になる(T-0691。D-508 で案を決めた・土台の設計の後)。気体の市松は燃える木箱の下半分に残る(T-0658)。**投影 × 最速 × 小さい空きのセルは外側の反復が発散する(T-0689・止めた)ので使わない**(probe_gas の既定は陽解法)。区画で分かれた世界の最速の投影は 1 刻みで釣り合いきらない(T-0688)。多重解像度の木は空きを読まない(T-0668・止めた)。
- **debug で覗き窓を開くと世界(物理)が起動ごとに違った(T-0239。回避済み)**: -Od のシミュのシェーダー(局所メモリが大きい)を同じキューで設定すると後ろの物理が壊れうる(ドライバの中の仕組みは未確認。T-0098 と同じ仲間)。debug でもシミュのシェーダーを -O3 で作る(ADR-0079)。PIX で 1 行ずつ追う時の `BICAMERAL_SIM_SHADER_OD=ON` のビルドでは食い違うことがある。release で食い違ったら・局所メモリの大きいシミュのシェーダーを足した後に戻ったら、チケットの打ち切り条件から。**FX_ASSERT の「fixed.hlsli:N」はどのファイルの assert でも fixed.hlsli と出る(行番号は呼んだ所の __LINE__)**。
- **debug の HW の gpu_multires が重い(T-0263)**: -O3 で落ちなくなったが、6 つのランナーの並走で 1200 秒の上限に届く。統合の確かめは release の gpu_multires で。
- **GPU の溢れ(T-0176・T-0211〔一部〕)**: 溢れの世界(wideCells の GpuMultires)で使えるのは全部を刻む刻み(伝導あり〔陽解法〕・なし)だけ。陰解法は RecordStep の FX_ASSERT で止まる(T-0238)。要求の処理(粗くする・細かくする・影・畳む)・活性のグラフ(RecordStepActive は false)・覗き窓・実験室はまだ溢れを知らない(T-0237・T-0212・T-0238)。溢れの領域は頁ごとの見出し 72 語 + 4 KiB の塊の置き場(空きのスタック。shaders/common/multires_wide.hlsli。T-0236)。書く面の塊が足りないと PlanWideSide が要求と刻み直す印を残し、WideAllocate(1 グループ・頁の番号の順のプレフィックス和で余りを返して配る)と WideRetryBlock(印のセルだけ ①②③ をやり直す)を全部を刻む段・頁に広げて刻む段・伝導の足す段の後に入れた(RecordWideRetry)。置き場が尽きた時だけ待たせて MR_COUNTER_LIMIT_PRODUCTS に数える。VRAM は見出し 288 B/頁 + 置き場(既定は 頁の数 × 2 と 50 の大きい方の塊 × 4 KiB)。ADR-0052 追記 2。残る当座の上限は 1 セル 24 種(超えると待たせる。T-0248)。
  **注意**: 頁を配る道を足したら溢れを空にする(今は StepExpandedWait の ClearWidePage と ConductPrepareBlock の ClearWideSideWords の 2 か所)。伝導の段の溢れの変種は 1 スレッド 1 セルで上限なしの形のセル(約 300 B)を持つので HW ではレジスタが溢れうる(ns/セル は T-0248 で測る)。**刻みの段を足したら、その後に RecordWideRetry を呼ぶ**(要求は段ごとに WideAllocate・WideRetryBlock が消す)。刻み直しは刻みの初めの busyTick で刻む。頁を空きに返しても塊は頁に付いたまま(要求の処理で返すかは T-0237)。
- **上限の当座のふるまい(T-0022。仮 = QUESTIONS Q19)**: (CPU の多重解像度の世界は EnableWideCells で ①③ が起きない。T-0187。既定は使わない)8 種のセルで 9 種目を作る反応は枠が空くまで進まない・17 本以上は平均 16/N の速さ・入りきらない子は細かいまま。
  多重解像度の世界の刻みでは MR_COUNTER_LIMIT_PRODUCTS・_CANDIDATES に数える(T-0163)。仮の世界ではまだ数えていない(T-0177)。ほかに数えているのは 1 セルの反応の GPU
  (reaction_cells.hlsl の u1)と、粗くするのを断った数(MR_COUNTER_COARSEN_FULL)。粗くする要求の解決(TreeResolve)は 1 スレッドで 64 セルを MrCoarsenCell するので、要求 1 件ごとに重い(未計測)。
- **HW の Work Graph は反応の核を 1 ノードに 3〜4 か所展開すると DEVICE_HUNG**(T-0124。原因は推定・BACKLOG)。反応はどこも待ちの丸めだけ(今までの丸めは T-0130 で消した)。
  **HW の Work Graph で止まったら、まず DXIL の局所の配列を数える**(`grep -c "= alloca \[3 x i32\]"` など。T-0397 は転換した規則を動的な添字で読む所の alloca が原因の見込みだった)。
- 待ちの丸めの 1 回の評価は重い(セルの規則ごとに log2 2 回と 128bit の割り算)。GPU は眠っているブロックを評価しないので費用は起きている所だけ(T-0123)。
  CPU の StepNest・StepActive(確かめる側)は全部を評価するので、debug の CPU のテストは T-0115 の前より遅いまま(multires_conduction・subcycle)。
  1 本のリストに 400 回積むと TDR になった(T-0123 の前。計測の暖機は 40 回のまま)。
- **細かいレベルの熱の小刻みの GPU は Δkmax 3 で 8.4 ms/刻み**(T-0111 で 40 から縮めた。鎖は Δkmax 1・2・3 で 0.55・2.2・8.4 ms)。小刻み 1 回 = 印 25・頁 3・端数 4・埋める 2・流れ 25・
  終わり + 起こすグラフ 53 µs(活性 Compute。一時的にタイムスタンプを打って測った)。段 6 つ + 起こすグラフの固定費 ≈ 50 µs × 64 が床。どうするかは QUESTIONS Q3(判断待ち)。
- 小刻みの数は呼ぶ側の maxSubcycleGap で決まり、GPU は最大回数を全部積む(空の小刻みも 41〜49 µs。T-0111 で 512 スレッドのグループが空で抜ける分 19 → 41 µs に増えた〔全部を刻む時〕)。ゲームの値は未定(T-0111 の後)。
- 基準 + 3 段より細かい所は今までどおり 1 段ごとに約 1/4 遅い。方式②は研究 T-0110(並走、D-432)。
- 基準(subcycleBaseLevel)は呼ぶ側が渡す値で、表から自動では決めていない(テストは tests/multires_conduction_scene.h の SubcycleTestOptions = 2)。ゲームの表を作る時(T-0021)にベイクで決める。
- 伝導で粗い側が取った端数の枠は、許容差つきの畳む段(CPU・GPU)なら静かになった時に返る。許容差なしの時は木の変更でしか返らない(たくさんの要求の小刻みで不足 9880)。
- 熱が通った所は温度差が約 1 mK 未満で流れが止まって静かになるが、ビット単位で同じに戻らない(許容差つきなら畳める。CPU・GPU)。
- 畳む 2 段は、畳むものが無い刻みで 0.0131 → 0.0197 ms(世界の枠 640。TreeFoldCheck が大きくなった分)。ほぼ同じ頁 2 つ + 端数の枠 1 つを返す刻みは 0.047 ms
  (docs/perf.md)。たくさんのブロックが同じ刻みに静かになってもグループは並列。重ければ成分の回(グループの同期 約 20 回)をまとめる(未着手)。
- 許容差なし(MrExactFoldTolerance)で呼ぶと、熱が通った葉は組の中のエネルギーがビット単位で違うので粗くならない(鎖の場面で本物 14・頁 14 のまま。T-0113。ゲームは D-435 の許容差で呼ぶ想定)。
- release のビルドで implicit_conduction.cpp(158) に C4189('added' が未使用。T-0110 の FX_ASSERT の中だけで使う変数)。並走の T-0117 の範囲なので触っていない。
- 影のブロックは自分の中だけ伝導する(外は断熱。親との受け渡しは引き戻し)。
- **WARP(2026-10-13 から debug も)では、伝導の段の Work Graph 版(multires_conduct_graph.hlsl)の最初の DispatchGraph でデバイスが失われる**(DXGI_ERROR_DRIVER_INTERNAL_ERROR)。
  debug の WARP とハードウェア(debug・release)では CPU と一致する。WARP の JIT の不具合と推定(未確認。ADR-0013 と同じ種類)。既定の Compute 版は release の WARP でも一致。
  テストは WARP で(debug も。T-0426。どの変更から debug でも落ちるようになったかは未確認)Work Graph 版を外している(gpu_multires_conduction_test.cpp の RunAll。--subcycle も同じ)。
- 伝導の段の Compute は活性の時も全部の枠の数だけグループを投げる(一覧の数を超えたグループはすぐ抜ける)。世界の枠が数万になったら ExecuteIndirect か Work Graph 版を測り直す。
- (前から)取り合いの丸めの残る偏り・「一様」はビット単位・頁の不足は「刻まない」だけ・反応の核のセルが変わらない種・観察の影の親も粗くなる・活性の固定費・PIX・セーブ・AMD は未確認/未着手。

## このチャットで決めたこと(ADR にしたなら番号)
- 2026-10-19 第 11 巡: ユーザーの決定 D-521〜D-526(上の「状態」)。本体の作業役が決めた細部: ROADMAP の T-0807・T-0819〜T-0821 の行の置き場所・GPU の和集合に陰解法の伝導 4 本を足したこと。
- 2026-10-18 5 本のマージの記録: ユーザーの決定 D-518(上の軸は y)。本体の作業役が決めた細部: rest_return の場面の並び(t-0759 → t-0758)・Q120〜Q123 の問いの立て方・T-0775〜T-0777 の番号。
- 2026-10-17 ③' 段 0〜4 のマージの記録: ユーザーの決定 D-516(T-0723)。本体の作業役が決めた細部: Q114〜Q118 の問いの立て方(ADR ごとに 1 問)・ROADMAP の新しい行の置き場所(分けた元の行の後。T-0758 は T-0723 の後で「T-0720 の後」の印)。
- 2026-10-17 T-0709: ユーザーの決定 D-509〜D-515(ADR-0326)。本体の作業役が決めた細部: 段 0〜9 のチケット番号の割り振り(T-0709〜T-0756。設計 §6 の表どおり。段 9 は 9 本に分けた)・公開側の例 5 の物質名(drift_mist・seep_stone・Xa)。
- 2026-10-17 第 9 巡の終わり: ユーザーの決定 D-508(T-0691 = 案 C + A)。ユーザーの指示で実装を止め、運ぶ計算の共通の土台を設計中。本体の作業役が決めた細部は無い(記録だけ)。
- 2026-10-17 第 9 巡: ユーザーの決定(木箱の基準を新しい比熱で取り直す)を D-507 に。本体の作業役が決めた細部: probe_gas の煙の高さを「上から数えた 95 % の高さ」に・`--burn` の既定 24,000 刻み。
- 2026-10-17: ユーザーの決定 A〜F を D-501〜D-506 に(上の「この仕事でしたこと」)。本体の作業役が決めた細部: probe_phase の試験の石に密度 2000(壁に戻す)・水たまりの閾値 3 割(ADR-0268 の追記 1)。
- 2026-10-13(2 回目): ユーザーの 2026-10-13 の決定 D-481〜D-483 を反映。実装の細部として、世界の物理の値は sim::WorldPhysics(既定 0)にまとめ、
  標準の値は StandardWorldPhysics() に置いた(T-0373 で標準パッケージへ移す。ADR-0171 追記 1)。
- 2026-10-13(ユーザーの 2026-10-12 の決定を反映): D-461〜D-476(DECISIONS)。この仕事で新しく決めたことは無い(Q44・Q45 は聞くだけ)。
- 2026-10-11 以前に Claude が決めた実装の細部(溢れの持ち方・核のセルの形・待ちの丸め・畳む段 など)は各 ADR の追記とチケットの「決めたこと」にある
  (この節の前の版は aa3e55c の HANDOFF.md)。

## 次にやること
NEXT.md の先頭(**③' 運ぶ計算の共通の土台**: t-0724 + T-0670 をマージ → T-0807(活性のブロックだけで流す)→ 段 5 の縦切り T-0725・T-0726〔D-521 で前倒し〕。並走で T-0778・T-0787・T-0804・T-0784・T-0819〜T-0821)。
③ の残り(T-0531・T-0625・T-0539〜T-0541・T-0537・T-0504・T-0533)は土台の段の空きで。⑤ の場面は段 5 の後。
判断待ちは QUESTIONS の Q129・Q130(T-0761)・Q124〜Q128・Q120〜Q123・Q114〜Q118・Q113・Q110・Q105〜Q108 ほか(NEXT の「判断待ち」)。
エンジンの仕組みは作り手から見た違いで、ゲームの中身ならプレイヤーと遊びへの影響で聞く(CLAUDE.md §5・D-476)。

## 注意(次の Claude がハマりそうな所)
- **`--auto-*` の窓(T-0350)**: `--auto-*` で起動した窓は debug の CRT の検査(STL の反復子の検査など)をダイアログでなく標準エラーへ出してすぐ落ちる(main.cpp)。窓の自動の確認が「止まる」ではなく「落ちる」になったら標準エラーを見る。
- **GPU の溢れ(T-0176)**: 溢れの並びは shaders/common/multires_wide.hlsli(C++ と HLSL で共有)。読むのは今の面(頁の先頭の語)。GPU のセルの溢れを読む段を足す時は
  `MR_WIDE_CELLS` の変種を作り(g_overflowBase は変種だけが宣言する。既定のシェーダーの cbuffer を大きくすると implicit_build・lab_box のルート署名と合わなくなる)、
  RecordXxx で m_wideCells の時にその変種を選ぶ。ルート署名はもう 1 語も空いていない。
- **世界のセルの溢れ(T-0187)**: 溢れを使う世界(nest.wideCells)で頁のセル・端数を書く所は StoreWidePageCell・StoreWideFraction を使う(nest.cells に直接書くと溢れと食い違う。
  LoadWideNestCell の FX_ASSERT が「溢れがあるならインラインは 8 種」を確かめる)。頁・端数の枠を空きに返す所は ClearPageOverflow・ClearFractionOverflow。
  新しく頁を返す道を足したら、溢れを空にするか、溢れのある頁を返さないこと(次に使う所に古い溢れが残る)。
- **核のセルの形(T-0175)**: reaction.hlsli の核の関数は `template <typename Table, typename Cell>`。セルの成分を増やす所は RxHasRoomForSpecies で確かめてから
  RxCellWithRoom で場所を用意して書く(RxWideCell は std::vector なので、場所を用意せずに species[speciesCount] へ書くと範囲外)。新しい関数で成分の位置ごとの
  作業場が要る時は RxUsage のように「形ごとの入れ物 + 引数で型を決める」にする(RX_MAX_CELL_SPECIES の配列を核に足すと RxWideCell で溢れる)。
  HashReactionCell・CountElements は多重定義になったので、関数名をそのまま std::ranges の射影に渡せない(ラムダで包む)。
- **上限の印の数える器(T-0163)**: MR_COUNTER_LIMIT_*(multires.hlsli)は CPU の StepPagedBlock と GPU の StepPagedWait・ConductApplyBlock(ApplyCell)の 3 か所。
  反応を刻む段を足す時はここにも足す(数える器は HashWholeNest に入るので、忘れると gpu_multires_* が食い違って教える)。
  分布の測定は reaction_package_scene_test の --distribution(進む規則の数は伝導の前のセルで数えた近似。ProbeReference::ChangedMarks を読む)。
- **静かな葉を粗くする判定(T-0113)**: multires_activity.hlsli の MrNeedsQuietCheck・MrIsCoarsenableQuietLeaf・MrCoarsenGroupWithin(Cells の約束: Cell(index)・Temperature(cell))。
  CPU は multires_activity.cpp の CheckQuietLeaves(SubmitQuietRequests の初め)、GPU は multires_tree.hlsl の TreeQuietCheck(PassQuietCheck。木の段は 12 個)。
  quietCheck は busyTick が変わると自動で古くなる(書き手を増やしても消す必要はない)。セルを書き換える段を足す時に busyTick を書き忘れると、古い判定のまま粗くなる。
  線の場面は tests/multires_quiet_scene.h の MakeLineNest(要求は 1 つの親に 1 回の処理で 1 つなので 1 つずつ処理する)。
- **GPU の眠り(T-0123)**: multires_wait_step.hlsli の StepBlockWait の初めで眠っているブロックを飛ばす。これは「ブロックのセル・tc・刻むセルの形を変えるものは
  必ずつつく(PokeBlock / MR_BUSY_POKED)か busyTick を書く」という約束に頼っている。セルを書き換える段を足す時につつき忘れると、GPU だけ起きずに CPU と食い違う
  (CPU は全部を評価するので、毎刻みの比べで見つかる)。GPU の計測は同じ exe を続けて 2〜3 回流し、2 回目以降を使う。
- **仮の世界の待ちの丸め(T-0122)**: probe_conduct.hlsl の ConductBlock(BlockMinTick で起こす刻みの最小 → tc・起こす刻みを書く)と probe_tick.hlsl の
  ApplyCommand(つつきの tc)・WakeDueBlocks(probe_sim.cpp の RecordUnit が適用の後に投げる)。印の場所は probe_sim.hlsli の PROBE_SCHEDULE_CHANGED_WORD・_WAKE_WORD。
  CPU は probe_sim.cpp の ProbeReference::Advance(全部を計算し、ブロックの待ちの最小 ≤ 次の刻みの印を POSSIBLE に)と ProbeInitialBlockWakes。
  元のテストの世界(木箱と空気)は熱が広がり続けて、1500 刻みでも待ちで起きるブロックが 0 だった。起こす道は gpu_probe_sim の TestSlowWake だけが通る。
- **1 セルの反応の試験(T-0130)**: reaction.hlsli の RxLoneCell・RxAdvanceLoneCell(C++ は reaction_table の AdvanceLoneReactionCell)。changedTick は「刻み」の単位
  (多重解像度は印 = 刻み + 1 を渡す。MrStepCellWait)。gpu_reaction は区間(50 刻み)の初めに「直前の刻みに変わった」とみなす(刻みは 1 から)ので、
  区間の切り方を変えると CPU 側(CompareSegment)も合わせる。reaction_test の「いろいろなセル」の要約と gpu_reaction の要約はもう同じにならない。
  stepFlags の 8 は空き(MR_STEP_CUTOFF_ROUNDING を消した)。計測の暖機(gpu_multires_conduction・_activity)は待ちの丸めで 40 回(400 回は TDR)。
- **伝導の段の待ちの丸め(T-0125)**: multires_conduct.hlsli の ConductMarkBlock(最初の小刻みに一様なブロックを EvaluatesUniform → EvaluateUniformCells →
  GroupMinTick → FinishWaitBlock か頁の印)と ConductApplyBlock(ApplyCell が MrStepCellWait、最後に FinishWaitBlock)。multires_wait_step.hlsli を含めて
  CurrentChangeMark・MinTick・FinishWaitBlock を使い回す。GroupMinTick は 1 回の段で 1 回だけ呼ぶ(2 回続けるとスレッド 0 の初期化と読みが競合する)。
  伝導の Work Graph 版(multires_conduct_graph.hlsl)の ConductApplyNode の反応の核は 1 か所(待ちの丸め)。印の段は Compute だけ。
- **GPU の待ちの丸め(T-0121・T-0124)**: gpu_multires.cpp の RecordStep → RecordStepWait(RecordWake〔WakeDue〕→ m_stepWaitPipeline → PassExpand → m_stepExpandedWaitPipeline)。
  活性は RecordStepActive の初めに RecordWake(STEP_FLAG_WAKE_SEEDS。m_activityWrite = この刻みの一覧)→ グラフ(ActivityStepNode・ObserverStepNode = StepBlockWait、ExpandStepNode = StepExpandedWait)。
  シェーダーは multires_step.hlsl の WakeDue・StepWait・StepExpandedWaitPass と multires_wait_step.hlsli(CPU の StepBlocks と RecordWaitResults の GPU 版)。
  HW の不具合を調べた手順と結果は T-0124 のチケットの「結論」。
  **活性のグラフのノードに反応の核(MrStepCell* / RxStepCell*)を足さない**(1 ノード 2 か所まで。3〜4 か所で HW が止まった)。HW を止めうる実験は wt2 のランナーが idle の時に。
  device_bash は 60〜75 秒で切れるので、テストは submit.py で投げて sleep 40 ずつ待つ。
- **ほぼ同じ頁を畳む GPU(T-0112)**: gpu_multires.cpp の RecordFoldPages(list, ring, tick, tolerance)(許容差なしの版は MrExactFoldTolerance で呼ぶ)。
  multires_tree.hlsl の TreeFoldCheck(端数を帳簿へ → FoldsExactly か FoldsNearly〔CollectFoldCells・CollectFoldSpecies〕)→ TreeFold。groupshared を使うので
  TreeFoldCheck の中の分岐はグループで一様に保つ(ウェーブの縮約もある)。テストは gpu_multires_uniform_test の RunActive(許容差つき)・RunNearFoldUnit と
  gpu_multires_conduction_test --near-fold(RunNearFoldChain)。ほぼ同じ頁の場面 MakeNearFoldUnit は tests/multires_uniform_scene.h に移した(CPU と GPU のテストで共有)。
- **ほぼ同じ頁を畳む(T-0104)**: CPU は multires_activity.cpp の FoldQuietPages(nest, table, tick, tolerance)(許容差なしなら古い FoldQuietPages(nest, tick) を呼ぶ)。
  式は multires.hlsli の末尾(MrFoldTolerance・MrFoldStats・MrAddFoldCell・MrFoldStatsWithin・MrFloorDivideEnergy・MrFoldStatsValue)と multires_activity.hlsli の MrWantsFractionReturn。
  帳簿へは AddLedgerBits(繰り上げつき)・AddFoldRemainder・ReturnFractionsToLedger。GPU は T-0112(上)。
  計測は `job.py run -Preset release -Exe multires_conduction_test -- --residue`(約 90 秒)。テストの場面の BeginUniformTick には許容差つきの版もある(T-0112)。ほかの場面(BeginQuietTick など)は許容差なし。
  map.yaml の multiresfold は FoldPage を指す(FoldQuietPages が 2 つあり archmap が引けないため)。
- **伝導の段のスレッド(T-0111)**: multires_conduct.hlsli の CONDUCT_THREADS = 512(印・流れ・足して反応・ConductBegin)と CONDUCT_LIGHT_THREADS = 64(埋める・小刻みの終わり。
  multires_conduct.hlsl と multires_conduct_graph.hlsl の numthreads も合わせる)。印と流れは CacheBlockFaces → GroupMemoryBarrierWithGroupSync の後に面ごとの計算(早く抜けるのはグループで揃う条件だけ)。
  段の初めから終わりまでセルも木も変わらない前提で groupshared を使っている。段の中でセルや木を書き換えるものを足すなら、この前提を見直す。
  計測は `job.py run -Preset release -Exe gpu_multires_conduction_test -- --queue compute --subcycle --measure-only`(約 10 秒。wt2 のランナーが idle の時に)。
- **細かいレベルの熱の刻みの GPU(T-0109)**: gpu_multires.cpp の RecordConduction(小刻みを 4^maxSubcycleGap 回積む)→ RecordConductSubstep(ConductMark → TreeExpand → TreeFractions →
  ConductPrepare → ConductFlows)→ RecordSubstepEnd(ConductEnd + 活性なら起こすグラフ)→ 最後に ConductApply。段の中で始まる・終わるは multires_conduct.hlsli の SubstepBegins / SubstepEnds
  (stepFlags から。CurrentSubstep・SubcycleGap・SubcycleBaseLevel)。CPU の StepConduction と同じ順なので片方を変えたらもう片方も。段を足すなら RecordConductStage
  (Compute / Work Graph を選ぶ)を通す。伝導の Work Graph 版の見出しは 4 つ(GRAPH_INPUT_CONDUCT_HEADERS。u12 の EXPAND_RECORDS は見出しの後ろ 160 バイトに動いた)。
  伝導の作業場の印は 16 語 / 枠(CONDUCT_MARK_BYTES・CONDUCT_MARK_WORDS)。RecordStepActive は活性のグラフの後すぐにこの刻みの種の一覧を UAV に戻す(起こす一覧に使い回すため)。
  stepFlags の下位 8bit だけが MR_STEP_* のフラグ(上は小刻みの設定)。
- **細かいレベルの熱の刻み(T-0108)**: CPU は multires_conduction.cpp の nest_detail::StepConduction(MarkSubstepBlocks → MarkConductionWants → ExpandForStep →
  ComputeConduction → ApplyEndingDeltas → WakeChangedBlocks)。StepBlocks(multires_nest.cpp)が呼び、最後の小刻みの変化を StepPagedBlock で反応と一緒に足す。
  式は multires_conduction.hlsli の MrSubcycleShift・MrSubstepBegins / Ends・MrSubstepSameLevelFlow・MrSubstepCrossLevelFlow・MrSubcycleBaseLevel(古い MrSameLevelFlow などは shift 0 の包み)。
  CellThermals は 1 刻みに 1 つ作って小刻みで使い回し、変化を足したブロックだけ Invalidate する(multires_nest_internal.h に移した)。
  StepBlocks は stepped を書き足す(span<uint8_t>)・wakeMark(活性の印。全部を刻む時は 0)。起こすのは nest_detail::WakeAround(multires_activity.cpp。種の起こし方から忙しさの印を除いたもの)。
- runner/logs は数が多く、device_bash から `ls -t` すると固まる。`ls -U ... | grep <日付> | sort | tail` で探す。
- **熱の伝導の GPU(T-0107)**: 段は shaders/sim/multires_conduct.hlsl(ConductBegin・ConductMark・ConductPrepare・ConductFlows・ConductApply。中身は multires_conduct.hlsli)と
  multires_tree.hlsl の TreeExpand(配った・凍らせた印)・TreeFractions(端数の枠を枠の順に。伝導の一覧の数を Work Graph の見出しに写す)。呼ぶ順は gpu_multires.cpp の RecordConduction。
  CPU の StepBlocks と同じ順なので、片方を変えたらもう片方も。印の種類は multires_bindings.hlsli の CONDUCT_MARK_*(刻みの印 = MrActivityMark)。
- u6 は [世界の枠の空き][取り合いの印][索引][世界の頁の空き](g_treeWords。番地は TreeClaimAddress・TreeIndexAddress・FreePageAddress)。CPU の木では別々の配列のままで、
  RecordUpload(MakeTreeWordsImage)と Read が詰め替える(取り合いの印は読み戻さない)。
- GPU の入力(u12)の見出しは 頁に広げる一覧 + 伝導の一覧 3 つ(埋める・流れ・足す。同じレコード)。伝導の一覧のレコードは頁に広げる一覧のレコードの後ろ(ConductRecordsOffset。世界の枠の数で動く)。
- 同じリストで RecordUpload を 2 回呼ばない(2 回目はバッファが UAV のまま CopyResource になる)。計測で版を並べる時は版ごとに別のリストにした。
- **熱の伝導(T-0019)**: 式は shaders/common/multires_conduction.hlsli(MrFindFaceNeighbor・MrCellThermal・MrSameLevelFlow・MrCrossLevelFlow・MrSplitCrossFlow)。
  CPU は multires_nest.cpp の nest_detail::StepBlocks(StepNest・StepActive で共有。前の StepPagedBlock / StepBlock を置き換えた)→ multires_conduction.cpp の
  MarkConductionWants(一様なブロックに MR_PAGE_WANTED・端数の印)→ ExpandWantedPages → ComputeConduction(端数の枠を配る・変化の表)→ 変化を足して反応。
  GPU(T-0107)も同じ順。粗いセルの変化は細かい側から足し込む(GPU は 64bit の atomic の足し算。端数の桁上がりも整数なので順に依存しない)。
- 数える欄は 24 個(23 MR_COUNTER_FRACTION_SHORTAGE。GPU は TreeFractions が書く)。
- CpuTree(Tree の約束)は multires_nest_internal.h の nest_detail::CpuTree に移した(活性と伝導が共有)。テストは自分の TestTree を持つ。
- multires_conduction_test は debug で約 3 分(保存量の 256bit の合計は 4 刻みごと)。たくさんの要求の場面の全部を刻む木が重い。
- **取り合いの丸め(T-0106)**: reaction.hlsli の RxSumUsage(要求・消費の合計)・RxShrink(切り捨てと丸め)・RxOverdraws・RxRevokeRoundUps・RxResolveContention(..., randomSeed)。
  反応の結果が変わったので、乱数や取り合いの式を変えると reaction_contention_test の表(期待・切り捨て・丸め)で偏りを見られる。sim のソースで `round` も変数名に使えない(`step` にした)。
- **頁を畳む(T-0103)**: 規則は multires.hlsli の末尾(MrFoldValueCell・MrFoldsCell)と multires_activity.hlsli の MrWantsFoldCheck。CPU は multires_activity.cpp の FoldQuietPages、
  GPU は multires_tree.hlsl の TreeFoldCheck(1 グループ = 世界の枠 1 つ・512 スレッド = セル)→ TreeFold(1 グループの累積和)。gpu_multires.cpp の RecordFoldPages(tick)。
  呼ぶ側の順は FoldQuietPages / RecordFoldPages → SubmitQuietCoarsenRequests / RecordQuietRequests → ProcessRequests(テストの BeginQuietTick・BeginUniformTick・各 GPU テストの RecordTick)。
  数える欄は 23 個(22 MR_COUNTER_FOLDED)。木の管理の段は 10 個(TREE_PASS_COUNT。TREE_SHADERS と static_assert)。
  **取り合いの印(g_claims / nest.claims)は要求の処理の外では全部 MR_NO_CLAIM という約束**を TreeFoldCheck が借りている。要求の処理の外で取り合いの印を使うものを足すなら、畳む段の置き場を変える。
- HLSL の `[numthreads] void A(...) {}` の直後に `[numthreads] void B(` を置くと clang-format が B を字下げする(TreeFillIndex がそう)。間に普通の関数を置くと直る(TreeFold の前の FoldBlock)。
- **一様なブロック(T-0102)**: 規則は shaders/common/multires.hlsli の末尾(MrIsUniform・MrIsCoveredCell・MrUniformCell・MrHasSteppedCell・MrUniformWouldChange・MrSameCell)と
  multires_tree.hlsli の MrCoarsenKeepsUniform・MrCoarsenPageNeed。**セルを読むときは論理のセル**(CPU は LoadNestCell、GPU は LoadBlockCell / LoadCell)。
  頁のセルの番地は CPU は nest_detail::PageCellAt / CellAt(頁を持つ枠だけ)、GPU は PageCellAddress(page, index) = 枠の数 + page × 512 + index(古い CellAddress は無い)。
  一様の値は nest.cells[枠](GPU は g_cells[枠])。頁の空きのスタックは nest.freeBlocks[世界の枠の数 + i](数は MR_COUNTER_FREE_PAGES = 19)。
  MultiresCapacity に pages(世界の頁の数)を足した。テストの MakeMultiresCapacity は pages = 世界の枠の数(不足しない)。
- 刻みの順(活性): 活性のグラフ(ActivityStepNode は一様なら印だけ)→ 観察の枠 → RecordExpandPages(TreeExpand → u12〔T-0107 前は u13〕の後ろの一覧 MR_GRAPH_INPUT_EXPAND_* を
  GPU の入力に ExpandStepNode)。全部を刻む RecordStep は Main → TreeExpand → StepExpanded(世界の枠の数だけグループ)。CPU は StepActive / StepNest の後ろの ExpandWantedPages。
  u12 の一覧の見出しの入口の番号と番地は RecordUpload が書く(MakeGraphInputImage。TreeExpand は数だけ書く)。数える欄は 22 個(19 頁の空き・20 広げた数・21 頁の不足)。
- 累積和 InclusiveScan(multires_tree.hlsl)は 3 本(枠・端数・頁)になった。MrRequestState に pageNeed・pageBase・releasePage を足した。
- **静かなブロックを粗くする(T-0101)**: 規則は shaders/common/multires_activity.hlsli の末尾(MR_QUIET_TICKS・MrIsQuietLeaf・MrWantsQuietCoarsen・MrCellChanged)。
  CPU は multires_activity.cpp の SubmitQuietCoarsenRequests、GPU は multires_tree.hlsl の TreeQuiet(gpu_multires.cpp の RecordQuietRequests。1 グループ 512 スレッド、InclusiveScan を使い回す)。
  1 刻みの順: 外からの要求(RecordRequests)→ RecordQuietRequests(tick)→ RecordProcessRequests → RecordStepActive(CPU は tests/multires_quiet_scene.h の BeginQuietTick)。
  **忙しさの印 busyTick**: つつく所は全部 PokeBlock(CPU は multires_nest_internal.h、GPU は multires_bindings.hlsli。種にして MR_BUSY_POKED を書く)。
  ActivitySeedNode / WakeSeed が POKED を刻みの印に直し、ActivityStepNode / StepActive がセルが変わった時に刻みの印を書く。木をつつく所を足したら PokeBlock を使う。
  全部を刻む StepNest だけの木では POKED のまま(粗くならない)。
- 見出し MrBlock は 88 B(activeTick・busyTick・padding)。HashWholeNest は両方の印を含む。数える欄は 17 QUIET_REQUESTS・18 QUIET_DEFERRED を足した(20 個のまま)。
- テストの場面の要求の一覧は 1 刻みに 1 つの親を 1 つの要求しか取れない: 同じ根の中を同じ刻みに細かくすると後ろは取り合いで落ちる(場面は空気の点を 1 刻みに 1 つずつ出す)。
- **木の上の活性(T-0100)**: 規則と面の隣は shaders/common/multires_activity.hlsli(MrWakeAcross・MrNearOctant・MrFindContaining。Tree の約束 = Block・Lookup)。
  CPU は engine/src/sim/multires_activity.cpp(StepActive。種 nest.seeds は世界の枠ごとの 0 / 1)、GPU は shaders/sim/multires_activity_graph.hlsl
  (ActivitySeedNode → WakeFaceNode〔スレッド起動・再帰 MR_MAX_WAKE_DEPTH 28〕→ ActivityStepNode、観察の枠は ObserverStepNode)と gpu_multires.cpp の RecordStepActive。
  **活性のグラフは `GpuMultires::Create(..., {.activity = true})` の時だけ作る**(反応の核を含んで大きく、debug の GPU-based validation で 1 回 2〜3 分。
  木の管理のグラフに入れたら gpu_probe_peek が 300 s を超えて落ちた)。
- 種の一覧(u13。T-0107 前は u14)は 2 本を刻みごとに入れ替える(m_activityCurrent)。要求の処理はこの刻みの一覧へ(細かくした親と子・粗くした親をつつく)、刻むノードは次の刻みの一覧へ書く。
  先頭は D3D12_NODE_GPU_INPUT そのもの + 予約の数 + 落とした数、レコード 0 は空(MR_NO_BLOCK)。RecordStep(全部を刻む)だけを使う所では一覧が空にされず、一杯になると落とすだけ(害はない)。
- **多重解像度のルート署名は UAV 14 個(u13 活性の一覧)・ルート定数 24 個で 62 / 64 語**(T-0107 で u6 にまとめた)。あと 2 語。
- 見出しの padding は activeTick になった(刻みの印 = 刻み + 1)。HashWholeNest は印を含み、HashRealLeaves・HashBlock は含まない。数える欄は 20 個(15 刻んだ数・16 再帰の上限で止まった数)。
- テストの場面は tests/multires_activity_scene.h(根 4×4×4・木箱の周りを T-0018 のたくさんの要求で・影 4 段)。総当たりの面の隣は同じファイルの BruteForceScheduled。
  CPU のテストの 2 回目は比べる相手と総当たりを省いている(debug で遅いので)。計測は根 8³ で、WARP では測らない(暖機が遅すぎる)。
- **多重解像度の木の管理(T-0018)**: 約束は shaders/common/multires_tree.hlsli(要求 MrRequest 40 B・途中の値 MrRequestState・索引の番地・帳簿)。
  CPU は engine/src/sim/multires_tree.cpp(ProcessRequests = Resolve → Settle → Allocate → Apply → ReleaseAll → RebuildIndex)、
  GPU は shaders/sim/multires_tree.hlsl の 6 段(.cso は multires_tree_<snake>)と multires_graph.hlsl(RefineNode は手で決めた影の鎖〔request = MR_NO_BLOCK〕と
  要求の鎖の両方・CoarsenRequestNode)。呼ぶ順は gpu_multires.cpp の RecordProcessRequests。GPU の入力は u12(見出し 2 つ + レコード。割り当ての段が書く。
  DispatchGraph の間だけ NON_PIXEL_SHADER_RESOURCE。レコード 0 件にしないため何もしないレコードを 1 件)。
- 数える欄は 20 個(MR_COUNTER_*。T-0100 で 15・16 を足した)。旧 MR_COUNTER_FRACTION_BLOCKS は無い(端数の枠の空きの数 MR_COUNTER_FREE_FRACTIONS)。要求の数 MR_COUNTER_REQUESTS は
  RecordRequests が CopyBufferRegion で書き、解放の段が 0 に戻す(一覧が空の時に送る)。RecordRequests の写しは 1 本のリストで 16 回まで(REQUEST_UPLOAD_SLOTS)。
- **GPU の時間は、CPU の重い処理の後に測ると暖機しても 4〜6 倍に出る**(GPU が長く空いてクロックが下がる)。gpu_multires_test は計測をたくさんの要求の前に置いた。
- MultiresNest は MakeMultiresNest(MultiresCapacity) で作る(世界の枠・観察の枠・端数・索引〔2 の冪〕・帳簿の列〔1 + 物質の数〕・根のレベル)。根は PlaceRootBlock(空きから取る)。
  覗き窓は世界の枠 0(観察の枠だけ)。GPU のバッファは 0 の大きさを作れないので最低 16 B。
- device_bash から `job.py` を `timeout` で切っても、ランナーのジョブは `--timeout` まで走り続ける。長いテストは `--timeout 290` で投げて、runner/logs/<job>.result.json を待つ。
- **6×6 のウェーブの解(T-0095)**: shaders/sim/physics_solve6_wave.hlsli。レーン t < 21 が下三角 (r, c)(t = r(r+1)/2 + c)、21〜26 が右辺。CPU の PxSolveSymmetric6 と
  要素ごとの式を揃えるため、共有の部品(PxDiagonalShift・PxCholeskyRadicand・PxCholeskyRoot・PxRhsTopBit。physics_math.hlsli)を両方が使う。片方を変えたらもう片方も。
  使う条件は physics_bindings.hlsli の CanSolveInWave(ウェーブ 32 以上かつ SOLVE_GROUP_THREADS〔64〕以下)。中は WaveReadLaneAt だけで、ウェーブの全部のレーンが一様に呼ぶこと
  (途中で return しない)。sim のソースで `round` も変数名に使えない(浮動小数点の検査が落とす。`digit` にした)。
- **述語(T-0094)**: `GpuPhysics` の u13(色ごとの「その色の物がいる」、uint64 × 16)は BeginSubstep が UAV で 0 にし、FinishColoring が書き、
  色ごとの解に使う間だけ PREDICATION(gpu_physics.cpp の RecordSubstep・RecordColorPredicateStates・RecordSolveColors)。リストの終わりで COMMON に戻り、
  次のリストの BeginSubstep で UAV に昇格する(だから小刻みの初めに必ず UAV で触ってから PREDICATION に移す)。述語と DispatchGraph は組み合わせない
  (述語をかける色ごとの解は Compute。SkipsEmptyColors)。`SetPredication(nullptr, ...)` で戻すのを忘れない(後ろのパスが黙って飛ぶ)。
- 物理のルート署名は UAV 14 個(u13 述語)・ルート定数 14 個で 48 / 64 語(T-0099 で詰めた)。足すときは BindRoot の static_assert(定数の大きさ・UAV の数)も合う。
- 島の方式(T-0094)は T-0099 で消した。もう一度試すなら git の 6c8dee9 から(physics_islands.hlsli・gpu_physics_islands.cpp)。
- **前のチャットのテストの exe が残っていると、build が `LNK1104: cannot open file 'bin\gpu_physics_test.exe'` で落ちる**(ctest の timeout で切れた後に残った)。
  `job.py raw` で `Get-Process gpu_*` を見て、古いものを `Stop-Process` してから build し直す。
- 前のコミットで archmap が落ちていた(map.yaml の gpu_probe_physics_test の RunGpu → `GpuRun::Execute` に直した)。
- **物理の GPU のパスで組(PxManifold 3 KB)を局所の変数に持たない**(T-0098)。持つと、別のキュー(窓の描画)が割り込んだ時だけ結果が時々変わり、
  火(伝導の Work Graph)と重なると GPU が固まった。テスト(描画なし)では出ない。gpu_probe_physics_test の 2 回目が優先度の高い direct のキューで雑音を流して確かめる。
  GPU の BuildSlot・Recollide・PrepareManifolds は CPU の PxBuildManifold・PxRecollideManifold・PxWarmStartManifold と同じ手順をバッファの上で書いた別の実装。片方を変えたらもう片方も。
- **仮の世界の物理(T-0098)**: ProbeSim の単位 [2] = ProbeSim::RecordPhysics(GpuPhysics::RecordStep の後にルートを結び直す)。物のバッファは仮の刻みの u13
  (物理なしなら重さの捨て場を仮に結ぶ。物の数はフレームの入力の見出し PROBE_HEADER_BODY_COUNT)。押すは probe_tick.hlsl の ApplyPush、
  物のハッシュは FlushEvents の中の StoreBodyHash、抽出の物の欄は StoreBodyView。描画は probe_view.hlsl の LoadBodyView・IntersectBodies・BodySliceColor。
- 窓の debug は物理のパイプラインの作成に約 60 s(window_replay_* の TIMEOUT を 240 にした)。release は初回約 20 s、以後はドライバのキャッシュで約 1 s。
- **覗き窓(T-0096)**: sim/probe_peek(ProbePeek・CPU リファレンス ProbePeekReference・状態 PeekState)。GpuMultires のルート署名に u4・u5(外のバッファ)と
  外の定数 4 語を足し、`SetExternalViews` + `RecordExternalDispatch` で shaders/sim/multires_peek.hlsl(MirrorWorld・ExtractShadow)を投げる。
  ProbeSim は `ProbeFrameInput::afterExtract` を抽出の後(UAV バリアの後)に呼ぶ。抽出の覗きの欄の並びは probe_sim.hlsli(PROBE_EXTRACTION_PEEK_*)、
  Extract が段の数 0 を書き、覗き窓が後ろで書き直す。描画は probe_view.hlsl の PeekSliceColor・OnPeekSliceFrame・PeekBoxEdges。
  潜った段は ProbeViewConstants の flags のビット 16〜19(VIEW_PEEK_DEPTH_SHIFT)。段 k のブロックは「点を含む 8 の倍数に揃ったブロック」(カメラの寄せ先。FocusOnPeek)。
  断面の位置は軸の座標をちょうど面に置き直している(細かい段では面がセルの境目に来るため)。
- 別のリストで GpuMultires の入れ子を読み戻すときは注意: `RecordReadback` は UAV → COPY_SOURCE の遷移を書くので、バッファが UAV にいるリストの中で呼ぶ
  (gpu_probe_peek_test はフックの中で呼んでいる。リストの終わりで COMMON に戻る)。
- gpu_probe_peek_test の GPU 時間の差は暖機していないので 7 倍に出る。覗きの費用は窓の要約(シミュ GPU ms/投入)で比べる。
- **WARP(T-0097)**: 使うのは exe の横の d3d10warp.dll(NuGet 1.0.21)。版を上げるときはルートの CMakeLists.txt の `BICAMERAL_WARP_VERSION` と `BICAMERAL_WARP_SHA256` を両方変える
  (取ってくるのは configure の時。`_deps/warp-<版>` が無ければ取る)。ログに Warning「WARP が exe の横の版ではない」が出たら OS の WARP を読んでいる。
  NuGet の版の一覧には `1.65535.20-preview` もあるが、1.0.21 より古いプレビュー(版の数字が大きいだけ)。
- **多重解像度(T-0017)**: 式は shaders/common/multires.hlsli(Mr 接頭辞。C++ は bicameral::multires)。CPU リファレンスは engine/src/sim/multires_nest(ライブラリ bicameral_multires)、
  GPU は sim/gpu_multires(Work Graph shaders/sim/multires_graph.hlsl の RefineNode・CoarsenNode・PullBackNode・RemoveShadowNode と Compute の multires_step.hlsl)。
  1 段の操作は CPU と GPU で同じ順(セルを全部書いてから見出し)。端数の枠は要求ごとに割り当ての段が配る(T-0018。スレッド 0 だけが見出しと返す枠を書く)。
  次のレベルが前のレベルの書き込みを読むので、ノードの RW バッファは globallycoherent・出力の前に `Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC)`。
  場面は tests/multires_test_scene.h(根 = 枠 0、本物の鎖 = 枠 1〜9、影の鎖 = 枠 10〜18、点のセル (3,4,5) は 600 K の木箱)。
- **HLSL で `point` は予約語**(multires.hlsli でも踏んだ。`coordinate` にした)。
- **GPU の時間は暖機してから測る**: 短い投入ばかりだと GPU のクロックが上がらず、全部が 6〜8 倍に出る(T-0017 で 16 ms → 2.3 ms)。gpu_multires_test は刻みを 400 回投げてから測る。
- 細かくした子は親と同じ数で始まるので、反応で子どうしが違わないと端数は出ない(900 K の木箱は O2 を使い切って止まり、端数が 0 だった)。
- **Nsight(T-0093)**: ngfx は `$env:ProgramFiles\NVIDIA Corporation\Nsight Graphics 2025.2.0\host\windows-desktop-nomad-x64\ngfx.exe`。ランナー(非管理者)から動く。
  性能カウンタは NVIDIA コントロールパネルの「すべてのユーザーに GPU パフォーマンスカウンタへのアクセスを許可」(2026-10-03 にユーザーが有効にした)。無いと「GPU Performance Counters Unavailable」。
  ngfx のヘルプ(`--help-all`)は UTF-16 で出る。書き出しの .xls は ASCII のタブ区切り(UTF-16 で読むと化ける)。1 回の記録は 7〜30 秒。
  記録は投入(ExecuteCommandLists)単位で `--start-after-submits`・`--limit-to-submits` で選ぶ(窓の無いテストでも使える)。
- **診断の作法(T-0093)**: シェーダーの一部の費用は「同じ計算をもう 1 回させて結果を捨てる(使ったことにするため、ありえない値のときだけ統計に足す)」で測った。ハッシュが変わらないことを確かめる。コミットしない。
- gpu_fixed_bench の `--iterations` を付けると調整なしで 5 回だけ投げる(投入 0〜4)。`--groups 1` は 1 グループ(8 ワープ)で、1 歩の時間 ≈ 1 本の連鎖の待ち時間。
- **物理の GPU の構成(T-0092)**: 小刻み = BeginSubstep → **Work Graph(physics_graph.hlsl の BroadphaseNode → NarrowphaseNode)** → BuildManifolds(Compute)→ PrepareManifolds →
  彩色 → 反復(探し直し・**SolveColor = 1 グループ 64 スレッド = 1 物**・UpdateDuals)→ Finish。呼ぶ順は gpu_physics.cpp の RecordSubstep / RecordIterations。
  バッファとルート定数と共有の関数(CollectCandidates・CollideGeometry・BuildSlot・SolveBodyInGroup・FindPrevious)は shaders/sim/physics_bindings.hlsli(Compute とグラフで共有)。
  比べた方は `GpuPhysicsOptions{.broadphaseGraph = false}`(Compute の Broadphase・Narrowphase)と `.solver = GpuPhysicsSolver::Graph`(SolveBodyNode。色ごとの物の一覧 u12 と GPU の入力 u11 を FinishColoring が作る)。
- **Work Graph のノードに大きい局所の変数を置かない**(約 5〜6 KB で GPU が固まる。組 PxManifold は 3 KB)。前の組は `PxBuildManifold` の `Previous` 型で点を 1 つずつ読む(CPU は PxPreviousManifold、GPU は GpuPreviousManifold)。
  固まった時の切り分けは、ノードの中身を少しずつ足して release で `--ticks 60` を走らせるのが早かった(debug の DRED は DispatchGraph で止まったとしか言わない)。
- **ブロードキャストのノードの出力は 1 グループ 256 件まで**(超えると CreateStateObject が E_INVALIDARG。広域の選別は 16 スレッド × 枠 16)。
- 計測の名前の表(gpu_physics.cpp の PASS_NAMES・SHADER_NAMES)は Pass の順。static_assert で数を確かめている(clang-format が並べ直すので、置き換えの編集は崩れやすい)。
- 色ごとの GPU の入力(u11)と物の一覧(u12)は、反復の間だけ NON_PIXEL_SHADER_RESOURCE(solver = Graph のとき。RecordColorListStates)。
- **物理の GPU(T-0090)**: パスは shaders/sim/physics_step.hlsl(入口 13 個 → `physics_<snake>.cso`。shaders/CMakeLists.txt の foreach)、呼ぶ順は gpu_physics.cpp の RecordSubstep
  (PhysicsWorld::Substep と同じ順に揃える。片方を変えたらもう片方も)。物・組・点の構造体は physics_step.hlsli で、C++ と HLSL の並びを揃えるため 64bit を 8 の倍数の位置に置き bool を使わない
  (大きさは physics_world.h の static_assert: 物 448・点 368・組 3016・パラメータ 96)。組は構造化バッファの要素の上限 2048 B を超えるので GPU では見出し(u1)と点(u9)に分けている。
- **gpu_physics_test は GPU-based validation を切っている**(debug の -Od のシェーダーの計装でパイプラインの作成が数分を超えた)。debug layer は有効。
  パイプライン 12 個の作成は debug で ctest の下だと約 60 s かかる(job.py run だとドライバのキャッシュで数秒のことがある)。パイプラインは 1 回だけ作って 2 回の実行で使い回している。
- HLSL で `point`・`half` も変数名に使えない(`point` はジオメトリシェーダーの修飾子、`half` は型。浮動小数点の検査も落とす)。physics_step.hlsli では `contactPoint`。
- 物理の GPU は区間の数(色・彩色の回数)を読み戻さずに固定の数だけ投げる。統計の overflow(gpu_physics.h)が 0 でなければ結果は信用できない。
- ランナーが止まっていたら、画面操作で起動できる: エクスプローラーで H:\BicameralEngine\.bicameral-runner\start-runner.cmd をダブルクリック(git 管理外。PowerShell の窓が開く)。
  エクスプローラーと PowerShell は「見る・左クリック」だけの許可なので、打鍵はできない。Google ドライブの窓が手前に来ると操作が止まるので、それも許可に入れる。
- **物理の試作を Linux で速く回す**(T-0091): device_bash の Linux に g++ 11 がある。tools/physics_lab の avbd_solver.cpp・box_collision.cpp と
  engine/src/sim/physics_scene.cpp(整数版なら physics_world.cpp も)を `g++ -O2 -std=c++20 -I tools/physics_lab -I engine/src -I shaders` で、小さな main と一緒にビルドできる
  (physics_lab.cpp は <format> が g++ 11 に無いので使わない)。double の山 1 種 3 s、2 コアで種 48 個を 80 s。整数版のハッシュは MSVC と一致する。
  山はカオスなので、基準の判定は種 1 個ではなく種 24〜48 個の割合で比べる(1 個だと ±数個ぶんの揺れで見誤る)。
- 物理の点は最大 8(生成 4 + 途中の探し直し 4)、法線は点ごと(`PhysicsContactPoint::normal`)。GPU に載せる時は探し直しが反復の途中のパス 1 つになる。
  探し直しは組を増やさない(増やすと彩色が変わり、同じ色の物が拘束を共有して並列に解けない)。
- **物理(T-0016)**: 式は shaders/common/physics_math.hlsli(ベクトル・四元数・128bit の平方根・6×6)・physics_collision.hlsli(直方体の接触)・
  physics_solver.hlsli(行・6×6 の組み立て・パラメータ PxDefaultParameters)。呼ぶ順は engine/src/sim/physics_world.cpp。試作 tools/physics_lab は同じ手順の double。
  **試作と整数版は手順を揃えてある**ので、片方を変えたらもう片方も変える(physics_lab の --integer で並べて比べる)。
- 物理の名前: 行列の行は `PxMatRow`(`PxRow` は拘束の行の構造体)。0 で埋めるのは `PX_ZERO(型)`(C++ は `型{}`、HLSL は `(型)0`)。
  HLSL の `half` は型名なので変数名に使えない(浮動小数点の検査が落とす)。`halfAngle` などにする。
- release で FX_ASSERT を効かせたいときは `BICAMERAL_FORCE_ASSERT=1` を定義する(fixed.hlsli。physics_checked_test がそう)。
- clang-tidy の NestingThreshold は 3(`{}` の入れ子)。ループを 3 重にするときは内側を `{}` なしにするか関数に分ける。HLSL 共通のファイルでは範囲 for が書けないので、
  添字を他にも使う形にするか PX_ZERO を使う(modernize-loop-convert が出る)。
- **仮の世界のセル**(T-0089): `cells`(u0、RxCell × 2 世代)と `thermal`(u12、HcThermalCache × 2 世代)。キャッシュはセルから決まる値という約束
  (ProbeStepCell の近道と、コンダクタンスを成分が変わった時だけ作り直すのがこれに頼る)。セルを書き換えたら必ず `ProbeMakeCache` で作り直す(つつきがそう)。
  反応の表は t1〜t4(既定のヒープ)。初めの世界と表は最初の RecordFrame で写す(`ProbeSim::RecordInitialization`)。
- **ProbeSim::Create は反応の表を取る**(`BakeReactionTable(MakeCombustionTestTable())`)。ProbeReference も表を取る。
- **WARP の Work Graph の JIT は、ノードの関数が少し複雑になると落ちる**(ADR-0013)。compute なら同じコードが動く。
- 抽出のセルの部分は `PROBE_EXTRACTION_BLOCK_OFFSET`(= セル × 4)語。CPU 側は `MakeProbeExtractionCells` で同じものを作れる。
- HLSL で `linear` は補間の修飾子(`line`・`point` と同じく変数名に使えない)。
- `shaders/common/probe_world.hlsli` は C++ では bicameral::sim の中で reaction と fx を using する。

- **反応の核**(T-0014、02 §3.1): 評価は `shaders/common/reaction.hlsli`(テンプレートの Table 型で表を読む。C++ は `sim::ReactionTableView`、
  HLSL は `reaction_cells.hlsl` の `GpuReactionTable`)。ベイクは `sim/reaction_table.cpp`、試験の表は `sim/reaction_test_table.cpp`(ライブラリ bicameral_reaction。GPU を知らない)。
  表の構造体の大きさは reaction_table.h の static_assert(RxSpecies 24・RxRule 80・RxCell 112 バイト)。HLSL の構造体を変えたら揃える。
- **HLSL の `?:` は構造体を返せない**(「conditional operator only supports results with numeric scalar...」)。if / else で。
- **WARP で GPU-based validation を有効にすると、大きなシェーダー(reaction_cells)の計装と JIT が 15 分を超えて終わらない。** gpu_reaction_test は WARP だけ GBV を切る。
  WARP は最初の Dispatch で JIT に約 8 秒かかる(以後は 20 ms)。
- 小数点のリテラル(`1e6` も)と `frac`・`floor`・`exp` などの名前はシミュのソースで禁止(浮動小数点の検査)。100 万は `1000000` と書く。
- release では FX_ASSERT が空になるので、assert の中だけで使う局所変数を作ると C4189(未使用)になる。式を FX_ASSERT の中に書く。
- **2026-09-30〜10-01 にコーディング規約を変えた(docs/style.md)。** 全コードを書き直し済み(ビルド・tidy 警告なし・テスト 28/28・archmap OK)。
  - 中身が 1 行の if / else / ループは `{}` なしで改行 + 字下げ(同じ行に書かない)。連鎖は分岐ごと(1 行の分岐だけ `{}` なし)。
    clang-format は `{}` を付け外ししない(RemoveBracesLLVM は入れ子の if を 1 文と見て外すので使わない)。
    clang-tidy の readability-braces-around-statements は外した(連鎖の混在を許さないため)。**`{}` の付け方は手で守る。**
  - 処理の区切りに空行(style.md「空行」の目安)。function-size の行数は空行も数えるので 70 にした。
  - 短い名前: `fs::` `rng::` `views::` `chr::`(core/aliases.h)・`ComPtr`(gpu/com_ptr.h)。使うファイルで include する。
    std 直下(`std::optional`・`std::span`・`std::format` など)は `std::` を付ける(10-01 に戻した)。ほかの長い名前空間はそのファイルの中だけで短くする。
  - 宣言は `=` の直後で折り返さない(.clang-format の PenaltyBreakAssignment)。行末コメントで収まらなければコメントを上の行へ。
  - 構造体・クラスの欄はまとまりごとに空行 + `// --- 〇〇 ---`。`// clang-format off` の範囲(HLSL のノード)も手で同じ見た目に揃える。
- `job.py test` を全部走らせると 3 分を超える(gpu_probe_sim が約 60 秒)。device_bash の 180 秒と `--timeout` で途中で切れるので、
  `-Filter` で分けて走らせる(例: `gpu_probe_sim` / `window_replay|gpu_debug_device` / `float_check` / 残り)。
- **同じコマンドリストを、キューのフェンスが前の実行を越える前に投げ直さない**(debug layer [553]。debug layer はその実行を捨て、release は黙って走る)。
  だからシミュのリストは ProbeSim のフレームの枠ごとに毎フレーム記録し直す(枠の前のリストが終わるまで、その枠は使わない)。`gpu::Queue::Execute` はフェンスを進めずに投げる。
- `job.py run` で窓のループが固まると、run の timeout(600 秒)まで runner が塞がる。試すときは `job.py run --timeout 60 -Preset ...` にする。
- **GPU のテストを足すとき**: tests/CMakeLists.txt の `bicameral_add_gpu_test(<名前>)` と `bicameral_add_gpu_test_case(<テスト名> <exe> <gpu|warp> <引数>)`。
  exe は bin/ に出て、bin/D3D12(Agility SDK)と bin/shaders をそのまま使う(agility_sdk.cpp を exe ごとに入れている)。
- テストの出力は printf ではなく Log(日本語をそのまま出せる)。終わりに `SingletonFinalizer::Finalize()`。
- **HLSL のノードの属性は clang-format が崩す**ので、work_graph_probe.hlsl のように属性つきの宣言を `// clang-format off/on` で囲む。
- Work Graph の起動順: SetComputeRootSignature → WorkGraph::SetProgram(最初だけ initialize=true)→ SetComputeRoot* → DispatchFromCpu。
- バッファは COMMON で作り、最初の UAV の使用で暗黙に昇格させている。読み戻しは gpu::RecordCopyToReadback(UAV → COPY_SOURCE)。
- archmap は CMake の `function(名前` も定義として見つける(T-0013 で追加)。C++ のメンバー関数は `Class::Method` で書く。
- **シミュのコードに `sqrt`・`pow`・`floor`・`lerp` などの名前の変数や関数を作らない**(ソースの検査が拒否する)。FxSqrt のように接頭辞を付ける。
- 描画だけの共通の HLSL は shaders/common ではなく shaders/render/ に置く(common は浮動小数点禁止)。
- **ベンチの演算を足すとき**: shaders/CMakeLists.txt の BICAMERAL_BENCH_OPERATIONS・fixed_bench.hlsl の BENCH_OP の分岐・
  tests/gpu_fixed_bench.cpp の BENCH_OPERATIONS・tools/fixed_bench/dxil_count.py の ORDER を同じ順で直す(番号 = 並び順)。
  定数の除数や、前の結果に依存しない連鎖はドライバがまとめて消すので、値は実行時のものにして前の結果に依存させる。
- **fixed.hlsli の約束**: 64bit の定数は `FX_U64(上位, 下位)`、関数は `FX_FN`、定数は `FX_CONST`。桁あふれしうる計算は符号なしで。
  自己テストに関数を足したら `FX_SELF_TEST_OUTPUT_COUNT` を増やす(要約 85c154e666febd92 が変わる。gpu_fixed_test も自動で追従する)。
- `job.py tidy` は debug のビルドフォルダの compile_commands.json を使う。先に `job.py build`。
- tools/dxil_float_check/ という空のフォルダが PC に残っている(削除の許可が無く消せなかった。git には入らない)。
- **公開リポジトリ。** 鍵(.bicameral-runner/)・CLAUDE.local.md・PC 固有のパスをコミットしない。ゲームの中身も公開側に入れない(D-006)。
- Linux 側の整形: `python3 -m pip install --user clang-format==23.1.1 pyyaml` → `~/.local/bin/clang-format -i`(セッションごとに入れ直し)。
- `.github/` 以下は device_commit_files では書けない(保護)。device_bash の cp や python なら書ける。
- runner の結果 JSON(runner/logs/*.result.json)を cat しない。job.py の要約か .log を grep する。
- 外部コマンドを呼ぶ .ps1 で `$ErrorActionPreference='Stop'` にしない(PS 5.1 は stderr の警告で止まる)。.ps1 は UTF-8(BOM 付き)+ CRLF。
  ただし `job.py raw` に渡す .ps1 は本文が埋め込まれるので BOM を付けない。
- Windows の Python がパイプに書く文字は CP932 になる。ビルドの中で呼ぶ Python は `sys.stdout.reconfigure(encoding="utf-8")` する。
- **fixed.hlsli の割り算を変えるとき**: 先に 64bit の剰余演算を Python で真似て突き合わせると早い(T-0084 はそうした)。fixed_test の `TestDivide128` が _udiv128 と境界まで比べる。
- **デバッグのリングを使うシェーダー**: ルート署名を `gpu::CreateRootSignature(device, {.uavCount = n, .debugRing = true})` で作り、
  `ring.RecordBegin(list)` → `SetComputeRootUnorderedAccessView(layout.DebugRingIndex(), ring.GpuAddress())` → 書く → `ring.RecordReadbackAndReset(list)` → 待つ → `ring.Drain()`。
  fixed.hlsli を使うシミュのシェーダーは Debug で FX_ASSERT がリングを使うので、必ずリングを結ぶ(結ばないと PSO / 実行が壊れる)。
- 書式を足すときは shaders/common/debug_formats.hlsli に `DEBUG_FORMAT(名前, チャンネル, 場所, 書式)` を 1 行。HLSL では `DebugFormat::名前`。
- **HLSL で `line` は予約語**(ジオメトリシェーダーの修飾子)。変数名に使うと「modifiers must appear before type」になる。
- デバッグ表示の定数は engine/src/render/probe_view_constants.h(96 バイト)と shaders/render/probe_view.hlsl の LoadConstants を揃える。
  光線の式は debug_camera.cpp の RayThroughPixel と probe_view.hlsl の MakeRay で同じにする(ずれるとクリックした所と見えている所が合わない)。
- `render/debug_camera`・`render/debug_view_controller` は bicameral_view(D3D12 を知らない)。D3D12 の型を持ち込まない(debug_camera_test が GPU なしで動くように)。
- 抽出の大きさを変えたら gpu_probe_sim_test の ReadExtraction も変える(PROBE_EXTRACTION_WORDS)。
- debug layer は、デバイスを作った後に有効にするとデバイスが失われる。`gpu::Device::Create` は必ず最初のデバイスより前に設定する(1 プロセス 1 回の想定)。
- `DebugRing` と `ReadbackRing` の読み戻しは枠(slot)ごと。`Create(device, slotCount)` → `RecordReadbackAndReset(list, slot)` → その枠のリストが終わってから `Drain(max, slot)` / `Read(slot, ...)`。
- **フレームのループ(frame/frame_loop.cpp)**: 抽出の 3 組の約束はファイルの先頭に書いた(抽出 n は、終わっている抽出が n − 2 以上のときだけ)。
  抽出の組の数を変えるときはこの条件も変える(破ると描画とシミュが同じ抽出を同時に使う)。
- **バックバッファを作り直す前・終わる前は `Queue::Flush()`**(最後の Submit の値を待つだけだと、その後ろの Present がまだ走っていて debug layer が CORRUPTION を出す)。
- 仮の刻み(sim/probe_sim・shaders/sim/probe_tick.hlsl・probe_conduct.hlsl・shaders/common/probe_sim.hlsli)の中身(64³ の熱の伝導)は段ごとに本物に置き換える前提。
  形(単位の列・単位ごとのタイムスタンプ・刻みの最後のハッシュ・64 バイトのコマンド・枠ごとの入力・イベントのリング・GPU の入力の活性の一覧)は本物にも使う。
  単位を足すときは probe_sim.hlsli の単位の表・ProbeSim::RecordUnit・UnitsPerTick を揃える。ハッシュの単位は必ず刻みの最後(次の刻みの適用より前の S(t+1) を取る)。
- ハッシュの表の読み戻しは、そのフレームにハッシュの単位があるときだけ(無いのに UAV → COPY_SOURCE を入れると状態が合わない)。ProbeSim::RecordReadbacks。
- HLSL の `[numthreads] void F(...) {}` が続くと clang-format が次の行を字下げして崩す。probe_tick.hlsl は入口の範囲をまとめて `// clang-format off/on` で囲んだ。
- HLSL の 64bit の剰余は避ける(表の番号は下位 32bit を 2 の冪でマスク)。WaveActiveSum は uint64_t で使える(DXC・RTX 3070 Ti・WARP で確認)。
- `job.py run` は窓を開く(ユーザーの画面に出る)。自動の確認は `--frames n --auto-click` で終わらせる。
- clang-tidy: `std::optional` のメンバーは unchecked-optional-access で大量に警告が出る。作れたものだけを受け取る形(ProbeSim・FrameLoopParts)にする。
- HLSL の `[numthreads(...)] void Name(` は archmap が定義として見つけない。map.yaml ではその .hlsl の普通の関数を指す。
- **コマンドを足すときの約束**(ProbeSim::RecordFrame が確かめる): (targetTick, sequence) の昇順・targetTick ≥ `NextApplyTick(カーソル)`・数 ≤ `FreeCommandSlots()` かつ 256。
  CPU の控え(ProbeSim の m_commandTail・m_queuedTicks)は「記録したら GPU で実行される」前提。記録したリストを投げなかった場合は控えがずれる(今は失敗 = 終了なので問題なし)。
- probe_sim のルート署名は UAV 12 個(u7 コマンドキュー・u8 刻みのイベントの一時置き場・u9/u10 活性の一覧・u11 予定の印)。
  バッファの結び方は shaders/sim/probe_bindings.hlsli(compute と Work Graph で共有)。イベントのリングの見出しは [0] 書こうとした数 [1] 一時置き場で落とした数。
- **GPU の入力の DispatchGraph**: 入力(見出しとレコード)は NON_PIXEL_SHADER_RESOURCE か COMMON でなければならない。活性の一覧はフレームの始めに
  COMMON → UAV、伝導の前後だけ入力の組を UAV ⇄ NON_PIXEL_SHADER_RESOURCE、フレームの終わりに UAV → COMMON(ProbeSim::RecordConduct)。
  **レコードを 0 件にしない**(WARP が固まる。一覧の先頭は必ず PROBE_NO_BLOCK)。SetProgram の後にルートの引数を結び直している(念のため)。
- **スレッド起動のノードの出力**: 局所の配列に集めて `GetThreadNodeOutputRecords(n)` で一括に出すと WARP で予定がずれた。展開したループで 0 件か 1 件ずつ出す(probe_conduct.hlsl)。
- GPU のテストの ctest の TIMEOUT は 300 秒(固まったときに runner を 25 分塞がないため)。`job.py test` を device_bash から呼ぶときは
  `timeout 170 python3 ~/job.py test --timeout 160 ...` にする(device_bash は 180 秒で切れ、ジョブは runner で走り続ける)。
- `--record` / `--replay` の相対パスは runner の作業フォルダ(bin/)から。ctest の window_replay_* は `out/build/<preset>/tests/window_replay.bcreplay` を使う。
- 窓の ctest(window_replay_*)は画面に窓が 2 回出る(約 6 秒ずつ)。CI はラベル gpu なので走らない。
- **Work Graphs のカウンタ**(T-0008、16 §1.2): ルート署名に `.graphStats = true`(u1 space1。`GraphStatsIndex()`、SRV の番号が 1 つずれる)。
  `gpu::WorkGraphStats::Create(device, layout, slotCount)` → `RecordBegin` → `SetComputeRootUnorderedAccessView(layout.GraphStatsIndex(), stats.GpuAddress())`
  → 数える → `RecordReadbackAndReset(list, slot)` → 終わったら `Read(slot)` → `Report`。HLSL のノード番号と `GraphStatsLayout` の並びを揃える
  (伝導は probe_sim.hlsli の `PROBE_STATS_*` と probe_sim.cpp の `MakeConductStatsLayout`)。ブロードキャストのノードは `WgCountLaunch` をグループの 1 スレッドだけで呼ぶ。
- ウェーブの関数(WaveActiveSum など)はスレッド起動のノードの中でも動く(HW・WARP で確認)。
- **windows.h の `near`・`far` はマクロ**。変数名に使うと意味の分からない構文エラーになる。
- job.py の build の要約に `add_custom_command(TARGET main POST_BUILD` が出るのは、vcpkg の使い方の表示(構成し直した時)。エラーではない。
- **連鎖のトレース**(T-0087、16 §1.3): ルート署名に `.graphTrace = true`(u2 space1。`GraphTraceIndex()`、SRV の番号がさらに 1 つずれる)。
  `gpu::GraphTrace::Create(device, filter, slotCount)` → `RecordBegin`(最初だけ範囲を写すので非 const)→ `SetComputeRootUnorderedAccessView(layout.GraphTraceIndex(), trace.GpuAddress())`
  → 書く → `RecordReadbackAndReset(list, slot)` → `Read(slot)` → `gpu::SortGraphTrace`。HLSL の `GtReserve` はウェーブの全部のレーンが呼ぶ(書かないレーンは 0 件)。
  伝導の記録の種類は probe_sim.hlsli の `PROBE_TRACE_*`、CPU の予想は sim/probe_trace の `AppendExpectedProbeTrace`(範囲の判定を GPU と同じにする)。
- `std::map` を持つ構造体を値で返すと clang-tidy の bugprone-exception-escape(ムーブが noexcept でない)。呼ぶ側の物に書く形にした(probe_trace.cpp の BuildTickTree)。
- python の `"""` の中に C++ の `"\n"` を書くと本物の改行になる(heredoc の python で編集するとき)。`<<'EOF'` の cat で書くか、`\\n` にする。
- **トレースの範囲を変えるとき**(T-0088): `GraphTrace::SetFilter` / `ProbeSim::SetTraceFilter` は次の `RecordBegin(list, slot)` から効く。
  容量は `Create` の capacity(`ProbeSimOptions::traceCapacity`)までに切り詰める。ランタイムは `frame::TRACE_CAPACITY_PER_FRAME`(65,536)をいつも確保。
  `RecordBegin` に slot を渡す(slot ごとのアップロードと、読むときの範囲)。
- exe の横のフォルダは `core/paths.h` の `ExecutableDirectory()`(ログ・シェーダー・トレースの既定の置き場所)。
