# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-10-29(本体の作業役 18 回目・記録の取りこぼし〔T-1233・T-1234 Done〕・Q179〜Q181・司令塔の仮の決定)

## この仕事でしたこと(2026-10-29・本体の作業役・18 回目: 記録の取りこぼし・Q179〜Q181・runner/ready の印のマージ)
- **記録の取りこぼし**: wt5 が main に入れた **T-1233**(84511ac8。alloca 1909 → 1048)・**T-1234**(d20c43a4)を Done に。合わせた後の main の dxil_alloca の上限の表は **1144**(4d60f487。main で増えたシミュのシェーダー 3 本)。
  ROADMAP に T-1212(原因は境の面 → T-1269)・T-1257・T-1260(Done・退く道で閉じた)・T-1261・T-1266・T-1269・T-1270・T-1271 の行。COMMANDER に今巡の配り(**司令塔の次は T-1290・ADR-0513**)。
- QUESTIONS に **Q179**(陰解法の段を既定の世界で持ち続けるか・ADR-0505・仮 (a) 残す)・**Q180**(T-1139 の最も粗い段の掃き出し 4 → 2・仮 4 回)・**Q181**(精度の予算 0.1 K を歩の終わりで守るか・ADR-0507・仮 = 読み替えない・既定 None)。
- **司令塔が仮で決めたもの(ユーザー未確認・取り消しやすい)**:
  - dxil_alloca の上限の表を 1048 → 1144 に上げた(T-1233 の後に main へ入ったシミュのシェーダー 3 本の分。下げるのは T-1266)。
  - T-1189: 同じレベルに隣のブロックが無い所は「無い」と読む(下は詰まり切る・横は傾き 0)。
  - T-1227: 粗い葉では光が葉の面から出直す(量は保つ・結果は升目の鍵と変わる)。光の配りの端数の置き場の違いは ADR-0500 に追記するかを後で決める。
  - T-1252: 一様な親の持ち主の欄は子も一様。
  - T-1260 は退く道で閉じる(試験の線 6 % のまま・直しは T-1261)。
- 次の番号は **D-684・ADR-0479・Q182**(本体)。T は T-1272〜T-1274。


## この仕事でしたこと(2026-10-29・本体の作業役・17 回目: t-1250・t-1133・t-1117 の 1 本目のマージ)
- **マージ(--no-ff・3 本とも衝突なし)**: wt4 fc58c4ce(t-1250-implicit-fold-regression)→ 35597282。**T-1250 Done・ADR-0505 Proposed のまま**(残る問い = 現実の伝導率では陰解法が約 2 mm より細かいセルだけで働く。境目の表は wt4 T-1263)。
  wt3 6bc89872(t-1133-mixed-size-diagonal)→ ccc99db4。**T-1133 Done・ADR-0504 Proposed(Q178・仮で既定 ①)**。T-1260(研究・wt3)を ROADMAP に。T-1067 のチケットの「下りる・周りなし」を 93 → 96 kN/m(+2.1 %・比べ用)に。
  wt6 8312073b(t-1117-rc-multires-tree の 1 本目。先頭 091eb31a = T-1212 は含めない)→ 1d92802f。ROADMAP の T-1117 を「1 本目 main・続きは T-1227・T-1228」。
- **ADR-0500 → Accepted(D-683・司令塔の一旦の決定・作り手から見た違いなし)**。QUESTIONS に **Q178**(支えの違う大きさのセルが絡む段の角の斜めの棒)。COMMANDER の番号の配り方に 1 行(**司令塔の次は T-1275・ADR-0508**)。
- 試験(release・警告 0): CPU multires_implicit・multires_implicit_tree・support_mixed(309 s)・support_law・support_fracture_refine・support_fracture_refine_scenes・support_fracture_thick(860 s)・support_cost・cascade_tree_grid・cascade_light(10 本 1962 s)+ HW gpu_multires_implicit・_tree・_build・gpu_multires_near_fold・gpu_equilibrium_block(5 本)+ gpu_cascade_tree_grid・_warp・gpu_cascade_light・gpu_radiation_cost(4 本)合格
- archmap --check OK(227 本・写しの上で)。clang-format --dry-run --Werror を変わった C++ 16 ファイルに: ずれ無し。→ push
- 次の番号は **D-684・ADR-0479・Q179**(本体)。T は T-1272〜1274 が本体(この仕事では使っていない)。


## この仕事でしたこと(2026-10-29・本体の作業役・16 回目〔設計〕: 司令塔が預かった土台の 3 件)
- **ADR-0500 を改訂(Proposed・Claude の一旦の決定・作り手から見た違い無し = 問いにしない)**: 根は「登録が欄の書き手と寿命を 1 通りしか知らない」。
  登録に **配り手**(SF_SCALE_OWNER + 配り手の番号。細かく/粗くはセルの配りの口が書く・一般の段は写さない)と **寿命**(SF_FLAG_TICK_ONLY = 源の項)を足し、
  **散らばった書き手は欄へ直に足さず、作業へ atomic → 共通の「集めて書く段」が枠の順のプレフィックス和で頁を取る**(頁が尽きたら計器の誤り・量は捨てない)。
  偶奇の 2 枚の欄は作らない(持ち越しは 1 枚)。変わったブロックの印 = 今の busyTick(セルの真実を書く 1 つの関数で書く・費用だけの読み手は「全部変わった」とハッシュ一致で確かめる)。
  Ec = 相の層の配り手・放射の持ち越し = 光の配り手(12 値・保存量)・源の項 = この刻みだけ。要る現象の一覧(21 §3)の全部に照らし、代案 3 つ(A おすすめ / B 家族ごとの置き場 / C 一般の規則を増やす)を表で比べた。
- 設計 20 に §10.7(欄の書き手と寿命)を足した。
- チケット: **T-1252**(土台・段 1 CPU / 段 2 GPU)を作った・**T-1227・T-1228 のファイルを作った**(段 1 は今すぐ仮の世界で・土台に触らない / 段 2 は T-1252 段 2 の後)・T-1230・T-1231・T-1161 を ADR-0500 を指すように。
  ROADMAP「土台の変更要求の束」に T-1252・T-1227・T-1228 の行。T-1253 は COMMANDER で wt3 に配られていたので使わず、GPU は T-1252 の段 2 にした(作りかけた T-1253 のファイルは _to_delete/)。
- wt6 の T-1117 の材料(司令塔経由)を入れた: (1) 源の項の口と受けのあった葉の印 = 決定 4・(2) 鍵 (葉の枠・セル・面・帯)= 決定 5・(3) 頁の無い所 = 決定 3。**t-1117 の 1 本目は土台の前に main へ入れてよい**(ADR に書いた)。
- main の docs/tickets/T-1117 は wt6 のブランチの版が新しいので触っていない(マージで入る)。
- 次の番号は **D-683・ADR-0479・Q178**(本体。T は T-1252 を使った・T-1253〜 は wt3 などに配られているので本体の次の T は司令塔に貰う)(この仕事では T-1252 を使った。D・ADR・Q は使っていない)。


## この仕事でしたこと(2026-10-29・本体の作業役・15 回目: T-1251・t-1207・t-1241・t-1238・t-1188 のマージ)
- **T-1251 Done**: t-1251-old-cpu-tests(b061e5ce)を main へ --no-ff。release で radiation_reference・standard_packages・probe_radiation(294 s)・window_lab・window_lab_blank・window_lab_compare・cascade_light(1052 s)合格。
- **T-1207 Done**: wt4 t-1207-cantilever-root 9687cbea を main へ(衝突なし)。**ADR-0501 は Proposed のまま(Q177・仮で既定)**。ROADMAP の T-0960 の行と T-0944 のチケットに「3 次元の対は曲げの差も同じ芯で」。
  試験(release・警告 0): support_law・support_mixed・support_fracture(616 s)・support_fracture_thick(623 s)・support_fracture_settle・support_cost・gpu_equilibrium_block 合格。archmap --check OK(227 本・写しの上で)。
- **T-1241・T-1238 Done**: wt3 t-1241-layer-world-option a4f51560 → t-1238-uniform-layer-header d7721d12 を順に main へ(衝突なし)。ROADMAP の T-1239 に「T-1188・T-1240 の後に回した」。
- **T-1188 Done**: wt2 t-1188-refine-gpu-2 45adf52d を main へ(衝突なし。multires_nest.h は T-1188 がコメントだけ・T-1238 は枠ごとの番号の組で、意味のぶつかりも無い)。ADR-0499 Accepted。残りは T-1189・T-1240。
- 3 本のマージの後の試験(release・警告 0): luau_type_check・environment_package・world_layout・multires_state・tree_refine_placement・multires_layers + GPU gpu_multires・gpu_multires_layers・gpu_lab_box・gpu_tree_refine_placement(HW)合格。archmap --check OK(227 本)。→ push
- COMMANDER の「番号の配り方」に司令塔 2 回目の行(**司令塔の次は T-1272・ADR-0508**)。
- 次の番号は **D-683・ADR-0479・T-1252・Q178**(本体。この仕事では使っていない)。

(前の版の最終更新: 2026-10-29(本体の作業役 14 回目・wt3 T-1236 を main へ・T-1251 の直しをブランチに・Q177))

## この仕事でしたこと(2026-10-29・本体の作業役・14 回目: wt3 t-1236 のマージ・T-1251)
- **マージ(--no-ff・衝突なし)**: wt3 da63d0f7(t-1236-layer-placement)。**T-1236 Done**(層つきの根の置き口 PlaceRootBlock・パッケージの world_cells の settled_umol〔名前は仮〕・ベイクの誤り・(a)/(b) を値 UNWRITTEN_GRAIN_LAYERS で切り替え・仮 = (a))。
  **ADR-0489 は Proposed のまま(Q175)**。ROADMAP: T-1236 Done・T-1241(層を使う世界をパッケージで選ぶ口と、層を書かなかった粒の警告。wt3 が着手)の行。
- 試験(release・警告 0・ビルド 700 s): world_layout・multires_layers・multires・multires_state・multires_sparse・multires_wide・multires_uniform・luau_type_check・play_metric 合格。archmap --check OK(227 本)。→ push
- **T-1251(途中)**: 3 本とも試験の期待の古さ(エンジンの不具合ではない)。直しはブランチ **t-1251-old-cpu-tests fe834bde**(main 20799ae7 の上。release のビルドは通った・警告 0。**試験は未実行**。main には入れていない)。
  radiation_reference = T-1066(f55584f1)でセルロース・炭が放射の吸い込みを書く / standard_packages = T-1164(278c9469)で combustion_test が地面の大気 300 K を書き直す / window_lab = D-637(熱伝導率の倍率 20000 → 1)で実験室の箱の隣の根は 4 mK しか上がらない → 幅 1 K → 1 mK。詳しくはチケット。
  次: そのブランチで radiation_reference・standard_packages・window_lab・window_lab_blank・window_lab_compare・probe_radiation・cascade_light を流し、合格なら main へ --no-ff。
- QUESTIONS に **Q177**(司令塔がユーザーに出した判断待ち: 支えで梁の厚さの途中で細かさが変わる所の偽の曲げを引く直し。ADR-0501・wt4 T-1207・t-1207-cantilever-root 9687cbea・未マージ。仮 = 既定)。
- 次の番号は **D-683・ADR-0479・T-1252・Q178**(本体。この仕事では Q177 を使った)。

(前の版の最終更新: 2026-10-29(本体の作業役 13 回目・wt4 T-1211・wt6 T-1235 を main へ・T-1250・T-1251 を作った))

## この仕事でしたこと(2026-10-29・本体の作業役・13 回目: wt4 t-1211・wt6 t-1235 のマージ)
- **マージ(--no-ff)**: wt4 60133b6f(t-1211-phase-layer-world)→ 28b7e124。衝突なし。**T-1211 Done**(担当分 = セルの段の核・反応 2 回・1 セルの GPU のビット一致。世界へつなぐのは T-1230〜T-1232)。
  **ADR-0497 Accepted**(Claude の一旦の決定・作り手から見た違いなし)・**ADR-0500 Proposed**(土台に足りないもの = 相の層のエネルギー Ec の欄の置き方。**司令塔が設計として預かる**。T-1230・T-1231 はその後)。
  wt6 b48663d3(t-1235-radiation-skip-positive。27bb6f11 の後の「間接の起動の既定を切る〔radiationIndirectLevels = false〕」が載っていた)→ b0666fa9。衝突は docs/perf.md の表の行だけ(両方残した)。**T-1235 Done・ADR-0495 Accepted**。
- ROADMAP: 土台の変更要求の束に T-1250・T-1251(順 0)・T-1211(Done)・T-1230〜T-1232・T-1235(Done)の行。
- チケットを作った: **T-1250**(main の HW で落ちる gpu_multires_implicit・_tree・_build〔SegFault〕・gpu_multires_near_fold の、いつから落ちるかの切り分けと直し。implicit_conduction_blocks/levels〔_chain〕も同じ組か。wt5 が確かめ中)・
  **T-1251**(前から落ちている CPU の radiation_reference〔line 394 allZero〕・standard_packages〔line 269 overrides〕・window_lab〔300004 mK〕)。どちらも Todo(司令塔が配る)。
- 試験(release・警告 0・ビルド 267 s): phase_layer・probe_phase_porous_stone(687 s)・cascade_light(561 s)・probe_radiation・float_check 5 本(9 本)+
  GPU gpu_phase_layer・_warp・gpu_radiation_cost・gpu_play_metric・gpu_probe_sim・gpu_probe_physics_radiation(6 本 124 s)合格。archmap --check OK(227 本)。clang-format --dry-run --Werror を変わった C++ 9 ファイルに: ずれ無し。→ push
- COMMANDER の「番号の配り方」に 2026-10-29 追加 2(wt4 T-1255〜T-1256・ADR-0501 / wt6 ADR-0502 / 本体 T-1250〜T-1251。**司令塔の次は T-1257・ADR-0503**)。
- 次の番号は **D-683・ADR-0479・T-1252・Q177**(本体。この仕事では T-1250・T-1251 を使った)。

(前の版の最終更新: 2026-10-29(本体の作業役 12 回目・wt3 T-1220/T-1221・wt5 T-1202 段 1/T-1223 を main へ・main で gpu_multires_implicit 系・near_fold を 1 回))

## この仕事でしたこと(2026-10-29・本体の作業役・12 回目: wt3 t-1221・wt5 t-1202/t-1223 のマージ)
- **マージ(--no-ff)**: wt3 17479a6c(t-1221-layer-gpu。T-1220 の CPU〔rebase 後 d67725a8〕と T-1221 の GPU の 1 段目を含む。元の t-1220-layer-cpu-rest 5ca34fdb は別に入れていない)。
  **T-1220・T-1221 Done・ADR-0489 は Proposed のまま(Q175)**。ROADMAP に T-1220・T-1221(Done)・T-1236〜T-1239・T-1240(T-1188 の後)の行。衝突なし。
  wt5 64fe0358(t-1202-shell-multigrid-rebased。T-1202 段 1 = CPU リファレンスの多重格子)→ da710a72(t-1223-shell-step-cost。T-1223 の 1 回目)。衝突は docs/perf.md の表の行だけ(両方残した)。
  ROADMAP に T-1202 の注(段 1 を main へ)・T-1222・T-1223 の行(どちらも Doing / Todo)。元の t-1202-shell-multigrid は消していない。
- 試験(release・警告 0): t-1221 後 = multires_layers・tree_refine_placement・placement_refine・multires・multires_state・multires_sparse・multires_wide・multires_activity・multires_quiet・multires_uniform・float_check 5 本(15 本 170 s)+
  GPU gpu_multires_layers・_warp・gpu_multires_uniform・_warp・gpu_multires(5 本 307 s)合格。t-1223 後 = object_shell(--quick 393 s)・float_check 5 本・gpu_object_shell_flow 合格。
  archmap --check OK(225 本)。clang-format の --dry-run --Werror を変わった C++ 18 ファイルに: ずれ無し。→ push
- **main(f668e8e2)で HW の GPU 試験を 1 回(直していない。司令塔へ)**: gpu_multires_implicit = 落ちる(「計測: 鎖を測れない」gpu_multires_implicit_test.cpp:512)・
  gpu_multires_implicit_tree = SegFault(鎖を CPU で作った直後)・gpu_multires_implicit_build = SegFault(アダプタの後すぐ)・
  gpu_multires_near_fold = 落ちる(4000 刻み一致だが「頁が全部畳まれる・端数の枠を返す、になっていない」畳んだ頁 0。gpu_multires_conduction_test.cpp:742)・
  gpu_multires_implicit_tree_stress = 合格。**wt5 の t-1233 と同じ形で main でも落ちる = t-1233 の変更のせいではない**(T-1221 の前の da710a72 の上の wt5 でも落ちていたので、T-1221 のせいでもないと見る。いつから落ちるかは未確認)。
- COMMANDER の「番号の配り方」に 2026-10-29 追加(wt3 T-1253〜T-1254。**司令塔の次は T-1255・ADR-0501**)。
- 次の番号は **D-683・ADR-0479・T-1250・Q177**(本体。この仕事では使っていない)。

(前の版の最終更新: 2026-10-29(本体の作業役 11 回目・wt4 T-1213/T-1214・wt6 T-1229(+ T-1161 の 1 段目)・wt2 T-1206/T-1192 を main へ・Q174〜Q176))

## この仕事でしたこと(2026-10-29・本体の作業役・11 回目: wt4 t-1213・wt6 t-1229・wt2 t-1206/t-1192 のマージ・Q174〜Q176)
- **マージ(--no-ff)**: wt4 4fe67487(T-1213・T-1214 Done・ADR-0493 Accepted = ADR-0487 の 3 を置き換え)→ ROADMAP に T-1213・T-1214 の行(Done)・**D-662 の注に「選ぶ段も起動時に測る」(司令塔の仮の決め)**。
  wt6 14af6d9c(t-1161 9df6b27b を含む。T-1161 の 1 段目〔段ごとの GPU 時間の印 engine/src/gpu/stage_stamps・放つ面の量をセルごとに 1 回〕+ T-1229 Done〔放射の空の所を GPU が飛ばす・ProbeSimOptions::radiationSkipIdle 既定 true〕)。衝突は docs/perf.md の表の行だけ(両方残した)。
  wt2 dee2f998(T-1206 Done・**ADR-0491 Proposed のまま = Q176**)→ d735bc2e(T-1192 Done)。ROADMAP: T-1192 Done・T-0939 の注・T-1206 の行・**T-1207 の行(研究)に「上の段だけ細かい片持ち梁の根の柱の移り目 10.08 %(T-1206 の後の残り)」**(T-1207 のチケットのファイルはまだ無い)。
- 試験(release・警告 0): t-1213 後 = time_step_tree(1162 s)・time_step_span_dump・time_step_choose_dump・gpu_time_step_span・gpu_time_step_choose 合格。
  t-1229 + t-1192 後 = ^support_ 9 本(support_fracture_window 822 s・_thick 704 s)・cascade_light(668 s)・cascade_light_swing(991 s)・cascade_tree_grid・probe_radiation・sky_port・sun_column・float_check 5 本 + GPU 6 本(gpu_sun_column・gpu_cascade_light・gpu_probe_sim・gpu_probe_physics_radiation・gpu_play_metric・gpu_radiation_cost)合格。PC が重い時で時間は長め。→ push
- archmap --check OK(225 本。t-1229 の後に Linux 側で)。
- QUESTIONS に **Q174〜Q176**(司令塔がユーザーに出した判断待ち。どれも仮で進めている): Q174 層の流れの費用 約 1 ms(仮 A 重ねて隠す)/ Q175 層を使う世界で層として置かなかった粒(仮 A 舞う粒。司令塔の見立て = B)/ Q176 固定した面を面の点の形を既定に(仮 = 既定)+ support_fracture_refine_scenes の D の確かめ。
- COMMANDER の「番号の配り方」に 2026-10-27 の追加と 2026-10-29 の司令塔の配り(司令塔の次は T-1253・ADR-0501)。
- **残り(次の本体の作業役)**: 4. wt3 t-1220-layer-cpu-rest 5ca34fdb(T-1220 Done・ADR-0489 Proposed = Q175)/ 5. wt5 t-1202-shell-multigrid-rebased 64fe0358 → t-1223-shell-step-cost da710a72(T-1202 の段 1 = CPU の多重格子・T-1223。元の t-1202-shell-multigrid は古いまま放置・消さない)。
- 次の番号は **D-683・ADR-0479・T-1250・Q177**(本体。この仕事では Q174〜Q176 だけ使った)。

(前の版の最終更新: 2026-10-27(本体の作業役 10 回目・wt3 T-1160・wt5 T-1200/T-1201 を main へ・ADR-0486 Accepted・T-1201 Done))

## この仕事でしたこと(2026-10-27・本体の作業役・10 回目: wt3 t-1160・wt5 t-1200 のマージ)
- **マージ(--no-ff・衝突なし)**: wt3 cd764aac(T-1160 の 1 回目: 相の層の共通の核 shaders/common/phase_layer.hlsli・tests/phase_layer_test.cpp・ADR-0486。世界へつなぐのは T-1211)→ **ADR-0486 Accepted(D-682)**・T-1160 の判断待ち 3 点を閉じた → push 7074ac45。
  wt5 308a4ead(T-1200 物のまわりの空気の層の流れを GPU に〔物 1 つ = 1 グループ・CPU とビット一致〕・T-1201 圧力の反復の規則)→ tests/CMakeLists.txt・shaders/CMakeLists.txt・docs/perf.md は自動で両方残った。
  **T-1201 Done**(判断待ち 2 つを D-677 で閉じた: 収まるまで解く・上限 1,024 が既定。道 B の多重格子は必須の T-1202)。T-1200 の判断待ちの 2 つ目も D-677 で閉じた(1 つ目 = groupshared に載らない物は T-1202 で見直す)。
- 試験(release・警告 0): t-1160 後 = phase_layer・probe_phase_porous_stone・float_check 5 本(7 本 702 s。PC が重い時で、wt3 では 214 s)合格。t-1200 後 = object_shell(--quick)・gpu_object_shell_flow・float_check 5 本 合格(object_shell 787 s)。
- clang-format 23(--dry-run --Werror)を変わった 6 ファイルに: ずれ無し。archmap --check OK(225 本)。
- COMMANDER の「番号の配り方」に 2026-10-26 22:06〜 の司令塔の配り(wt3 T-1220〜・wt5 T-1222〜・wt2 T-1224〜・wt6 T-1227〜・wt4 T-1230〜。司令塔の次は T-1233・ADR-0494)。
- 次の番号は **D-683・ADR-0479・T-1189・Q174**(本体。この仕事では使っていない)。

(前の版の最終更新: 2026-10-26(本体の作業役 9 回目・wt6 T-1164・wt2 T-1191・wt4 T-1112 を main へ・ユーザーの答え D-677〜D-682 の記録))

## この仕事でしたこと(2026-10-26・本体の作業役・9 回目: wt6 t-1164・wt2 t-1191 のマージ・答えの記録)
- **マージ(--no-ff・衝突なし)**: wt6 278c9469(T-1164: 放射の持ち越しを鍵で引く密な表・刻みの偶奇で 2 つ〔ADR-0484〕・D-670・D-671 の既定・GPU の正負 2 側・combustion_test の地面 300 K)→ push c97d803f。
  **main で落ちていた window_replay_play・_rewind・_peek は直った**(持ち越しの疎な表 2^19 の溢れで、落ちる鍵が atomic の順で決まっていた)。
  wt2 dc911060(T-1191 Done: 違う大きさの面の自重の応力の差 29 % → 1 % 未満・ADR-0485)→ push 83faf928。
- **注意: GPU の放射の段の溢れは release でも Error に出る**(ADR-0484 の口。出たら表の大きさか鍵の数を疑う)。
- 試験(release・警告 0): t-1164 後 = probe_radiation・probe_gas 558 s・gpu_probe_sim・gpu_probe_physics_radiation(新)・gpu_play_metric・window_replay_record・_play・_rewind・_peek 合格。
  t-1191 後 = ^support_ 9 本(support_fracture_window 620 s)合格。
- **ユーザーの答え(2026-10-26 22:17)を記録**: D-677(層の圧力は収まるまで・上限 1,024 を既定。**T-1202 の多重格子で燃える物 1 つの費用を D-667 の見込みの桁へ戻すのが必須**。ROADMAP に T-1202 の行)・
  D-678(ADR-0481 の 5 = (a)。T-1210 着手可)・D-679(Q173 閉じた: 粒の詰まりは任意・既定 60 % と警告。T-1188 で)・D-680(ADR-0480・ADR-0485 Accepted・support_mixed の線 5 %)・
  D-681(combustion_test の地面 300 K・参照の温度は高さ 0)・D-682(T-1160 の 3 点。**ADR-0486 は wt3 のマージ後に Accepted にする**)。
- wt4 470e3203(T-1112 Done: セルの窓を選ぶ段を GPU に・ADR-0487)→ push。衝突は docs/perf.md の表の 1 行だけ(両方残した)。試験 time_step_choose_dump・gpu_time_step_choose・time_step_tree(release)合格。
- **残り(2 時間の区切りで止めた)**: wt3 t-1160-phase-energy(cd764aac。入れたら ADR-0486 Accepted)→ wt5 t-1200-shell-gpu(308a4ead。入れたら T-1201 の判断待ちを D-677 で閉じる)の順。
- 次の番号は **D-683・ADR-0479・T-1189・Q174**(本体)。

(前の版の最終更新: 2026-10-26(本体の作業役 8 回目・T-1176 の CPU と傾きの式・wt3 T-1209・wt4 T-1089 を main へ))

## この仕事でしたこと(2026-10-26・本体の作業役・8 回目: T-1176 の CPU と傾きの式・wt3 T-1209・wt4 T-1089 のマージ)
- **T-1176(Doing。GPU と既定 on は T-1188 に分けた)**:
  - 傾きの式 SfSurfaceSlopeQ16・SfClampColumnSlopeQ16 を 32bit で計算する形に(表面は −1〜2・差は ±3 なので収まる。64bit の差を作らない = T-1078・T-1159 の形にならない)。
    gpu_placement_refine に傾きの式の 8192 件(入口 placement_refine_check.hlsl の g_mode = 1)→ HW の RTX と WARP で合格。
  - 物質の相の印 RxSpecies::flags に RX_SPECIES_LIQUID・GRAIN・ANCHORED(ベイクが相と solid_motion から。語は増えない)。量が µmol の体積 SfAmountVolume・SfMaterialVolume・SfAddSettleOf・PlaceMaterial::molarVolumeMm3PerMol。
  - CPU リファレンス engine/src/sim/multires_refine_cells.cpp: PlaceTreeCell(数の空間・器 = MR_CELL_VOLUME_MM3・量 = 親 × 8・傾きは同じ親のブロックの中の隣の列)・
    エネルギーは熱容量の重み(D-674)・端数は SAME・寄せないセルは触らない。EnableRefineCellPlacement で表を持たせる(欄の refinePlacement も同じ置かれ方)。
    **MultiresNest::refineCells は既定で空 = 今の世界はビット単位で同じ**(GPU の RefineLevel がまだ SAME のため)。試験 tree_refine_placement(保存・SAME・温度・層・斜面・決定性)。
  - 判断待ち: 【エンジン】粒の詰まりを作り手が書くか → **QUESTIONS Q173**(仮 = 100 %)。
- **マージ(--no-ff・衝突なし)**: wt3 6079606c(T-1209 Done: multires_sparse の 6 件は帳簿の理由が 11 → 12 で区画の要約が伸びただけ・基準を取り直した・ADR-0481 の 5 に (a) を仮で。T-1210 は ADR-0481 の 5 の (a) をユーザーが確かめるまで着手しない)・
  wt4 9ee5ccfd(T-1089 Done: 刻みの e をセルごと・木の窓が本当に 2 m・unitEdgeCells 既定 1)。
- 試験(release・警告 0): CPU multires・multires_state・field_storage・placement_refine・tree_refine_placement・multires_sparse 66 s・time_step_span_dump・time_step_event_merge・multires_edge・multires_gas・
  GPU gpu_multires・gpu_placement_refine(・_warp)・gpu_time_step_span・gpu_time_step_event・gpu_time_step_event_cost 合格 → push。time_step_tree は wt4 で通っている(main では PC が空いた時に 1 回)。
- 次の番号は **D-677・ADR-0479・T-1189・Q174**(本体。この仕事で T-1188・Q173 を使った)。

(前の版の最終更新: 2026-10-26(本体の作業役 7 回目・整形・wt5・wt2・wt3・wt4 の 4 本を main へ・T-1176 の設計の詰め))

## この仕事でしたこと(2026-10-26・本体の作業役・7 回目: time_step_tree の確かめ・整形・4 本のマージ・T-1176 の設計の詰め)
- **前の作業役の残り**: time_step_tree(main・release)合格 1053 s。clang-format(CI と同じ 23・--dry-run --Werror)を全体で: ずれ 5 ファイルを整形・
  生成物 shaders/common/radiation_directions.hlsli は tools/radiation_directions/generate.py が clang-format off / on で囲むように直して作り直した(値は同じ。28ff11ec)。archmap --check OK(225 本。Linux 側で数秒で終わった)。
- **マージ(--no-ff・衝突なし)**: wt5 035fd1c6(T-1185 Done・T-1186 打ち切り・T-1143 に D-673 で出ない現象)・wt2 96e4e152(T-1171 R-B3 Done・ADR-0480 Proposed 粗さで閉じる・D-675)・
  wt3 656b99bf(T-1167 Done・T-1177 Done = SF_FLAG_BREAKDOWN と内訳の欄・ADR-0481 Proposed)・wt4 b669fcdf(T-1175 Done・窓を列と木の既定に・D-672 確定。ADR-0477 は Accepted のまま)。
- 4 本の後の試験(release・警告 0): CPU 21 本(object_heat・object_heat_nest・grass_wake・object_shell 704 s・case_b_box・case_b_r2・support_law・support_mixed・support_fracture 435 s・_window 813 s・_thick 554 s・_refine・_refine_scenes・_settle・support_cost・multires_state・field_storage・placement_refine・time_step_lab・time_step_span_dump・time_step_event_merge)・GPU 2 本(gpu_time_step_span・gpu_time_step_event)合格 → push。time_step_tree は wt4 で通っている(main では PC が空いた時に 1 回)
- 記録: ROADMAP(T-1175・T-1152 の D-672・T-1089/T-1112 の申し送り・T-1191〜T-1193・T-0939 の注・D-663 の行に T-1186 の残りの道)・20 §9.3 の注(ADR-0480)・T-1144(運ぶ欄の帳簿を消した)・T-1166(582 m/s は T-1167 で直した)・COMMANDER(wt2・wt3・wt4 の追加の番号。次は T-1215・ADR-0488)。
- **T-1176 は設計の詰めだけ**(チケットの「設計の詰め」1〜10: 木のセルは濃度なので数の空間で器 = V0・µmol と molarVolume・相の印を RxSpecies::flags へ・端数は SAME・D-674 の熱容量の重み・隣の 15 セル・GPU の形・影は SAME)。コードは次の作業役。
- 次の番号は **D-677・ADR-0479・T-1188・Q173**(本体の範囲。この仕事では使っていない)。

(前の版の最終更新: 2026-10-26(本体の作業役・試験と push・wt2・wt4・wt3・wt5 の 4 本を main へ・QUESTIONS の棚卸し))

## この仕事でしたこと(2026-10-26・本体の作業役・4 本のマージと棚卸し)
- **main の試験と push**: wt6 のマージ(t-1163・t-1113)と D-669〜D-676 の後の main を release で(警告 0): CPU 10 本(probe_radiation・reaction_package_scene・blank_world・physics_heat・probe_liquid・rest_return・slope_check・cascade_tree_grid・cascade_light 616 s・sky_port)・
  GPU 6 本(gpu_probe_sim・gpu_probe_blank・gpu_world_copy・gpu_play_metric・gpu_probe_rewind・gpu_cascade_light)合格 → push(9b5e8781)。
- **マージ(--no-ff)**: wt2 7e70fa35(T-1134 下りる道をほかの場面でも・T-1135 割れの先の速さの上限)・wt4 777b6ae1(T-1153・T-1169・T-1170・T-1173・T-1174。T-1175 は wt4 で作業中)→ **ADR-0477 Accepted(D-669 窓・半径 2 m)**・
  wt3 5a4cc737(案 B R3 の研究 T-1070・T-1071・T-1091〜T-1093・T-1105・T-1106・T-1136〜T-1138・T-1166 int64 の溢れの直し。場面の作り方は wt3 が sim/placement_refine に置き換え済み)・
  wt5 4f3c948f(T-1142・T-1143・T-1144 物の箱が囲む空気の層。衝突は tests/CMakeLists.txt の一覧の 1 行だけ・両方残した。shaders/common/object_heat.hlsli が変わった)。前の 3 本は衝突なし。
- 4 本の後の試験(release・警告 0): CPU 13 本(object_heat・object_heat_nest・grass_wake・object_shell 1077 s・case_b_box・case_b_r2・case_b_r3 814 s・support_fracture 583 s・_refine・_refine_scenes 599 s・_settle・time_step_lab・time_step_span_dump)・GPU 3 本(gpu_object_heat・gpu_object_heat_nest・gpu_time_step_span)合格。time_step_tree(約 1400 s)は push の後に流した(下の注)。archmap は変わったファイルの参照 10 本を解決できることだけ確かめた(PC が重く全体の --check が device_bash の時間内に終わらない。次に軽い時に全体を)。
- **QUESTIONS の棚卸し**: 古い判断待ち 40 本を閉じた(答えでエンジンの作りが変わらない仮 = 今のおすすめを決定扱い〔D-539〕/ 後の決定に置き換わった)。各 ADR・チケットの末尾に 1 行・QUESTIONS は末尾の「閉じた問い」に 1 行ずつ。
  Q31(ゲームの中身)は公開リポジトリから外した(非公開の控え claude/19)。**残る古い判断待ち: Q93 ②(液体の速さの上限 15 m/s。案 B に上限があるか未確認)・Q105・Q148**。
  Q128 ① は案 B に置き換わる(案 B の判定で案 A に退いた時だけ戻る)。答え済みで節が残っているもの(Q153・Q161・Q167〜Q172 など)はそのまま。
- 次の番号は **D-677・ADR-0479・T-1188・Q173**(本体に配られた範囲。この仕事では使っていない)。

(前の版の最終更新: 2026-10-26 本体の作業役・T-1159 芯 2 の GPU の回しと 2-3)

## この仕事でしたこと(2026-10-26・本体の作業役・T-1159)
- **T-1159 Done(t-1159-c1-gpu を main へ)**: 置かれ方で配るの GPU の回し `shaders/sim/placement_refine.hlsli`(1 スレッド = 子 1 つ・子の番号は MrChildCell の j)と入口 `sim/placement_refine_check.hlsl`。
  state_field.hlsli の置かれ方の式が初めて DXC(-WX)を通った。`gpu_placement_refine`(・_warp)で sim::PlaceRefineCell と 1 値ずつビット一致(乱数 2000 + 端の場面 9 × 子の体積 3 通り。4 m は 32bit を超える)。
  子の番号の写し替え `sim::PlaceIndexOfMrChild`。2-3 眠る時に捨てる印 `SF_FLAG_SLEEP_DISCARD`(導かれる値だけ)・`SfSleepValueOf`・`RegisterWarmStartField` の discardOnSleep。今の世界は印も口も無いので結果は変わらない。
- **GPU の食い違い(直した・注意)**: `SfOpenVolume`(64bit の差を 0 で止める)が RTX 3070 Ti だけで差の上の 32bit が別の値(物質の数の 5)になった。差だけ・比べだけなら正しく、
  2 つを並べると(select・印 0/1 の掛け算・上下に分けた select のどれでも)壊れる。DXIL は正しく WARP は合う。T-1078 と同じ根で、定数でなくても起きる。直し = 32bit の 2 つに分けて借りを手で回す。
  → **司令塔へ: CLAUDE.md §4 の約束を「64bit の引き算と同じ値どうしの比べ(0 で止める・min・選ぶ)を並べない」に広げることを勧める**(T-1159 のメモ)。同じ形の残り(SfSurfaceSlopeQ16・SfClampColumnSlopeQ16)は T-1176 で GPU の試験に。
- 分けた: **T-1176**(木の細かくする段のセルの中身を置かれ方で・隣の親の読み方・GPU の欄・影)・**T-1177**(2-2 内訳。wt3 T-1136 の後)・**T-1178**(2-5 支えを世界の木へ。wt2 T-1134 の形 1〜5。(4) 子の中身 = 据わる固体は SAME はここで確かめた)。
- 判断待ち(仮): 【エンジン】眠る時に捨てるは欄ごとの印(取り消しは印を外すだけ)。
- 試験(release・警告 0・archmap OK): placement_refine・field_storage・warm_start_save・multires・multires_state・multires_edge・multires_gas 403 s・multires_liquid 413 s・gpu_placement_refine・gpu_placement_refine_warp・gpu_multires・gpu_multires_uniform。
- 次の番号は **D-669・ADR-0479・T-1182・Q173**(T-1179〜T-1181 は wt4 に配った)。

(前の版の最終更新: 2026-10-26 本体の作業役・wt4・wt2・wt5 の 3 本を main へ・番号の訂正)

## この仕事でしたこと(2026-10-26・本体の作業役・3 本のマージ)
- **wt4 t-1063-rt2-budget の c38ac3c2 を --no-ff**(衝突なし): R-T2 の GPU(T-1111 長い刻みの GPU 版・T-1109 CyclesFor・T-1141 1 出来事を 1 本の系に・T-1152 費用の式と起動時の測り ProbeTimeTreeSolveCost・T-1140 GPU の時計)・ADR-0472 Accepted。T-1152 の判断待ち 1 つ(起動時の測りで GPU の時計を上げるか。仮 = 上げる・heatNs 0.4 秒・0 で取り消せる)はチケットに。wt4 はこの後に T-1153・T-1169・T-1170 を積んでいる(先頭 7fdae61c。次のマージで)。
  - T-0691 の GfRestKineticExcess(gas_flow.hlsli・gas_reference.cpp)はこのマージで変わっていない。GPU の熱の列(t-0813)はこのブランチに入っていないので関係なし。t-0813 のマージの時の注(GPU の列へも足す)はそのまま。
  - 試験(release): time_step_lab 24 s・time_step_tree 1418 s・time_step_span_dump・time_step_event_merge 176 s・gpu_time_step_span・gpu_time_step_event・gpu_time_step_event_cost 合格。
- **wt2 t-0872-rb2-fracture の c7c91837 を --no-ff**(衝突は docs/perf.md の表の行だけ・両方残した): 支え T-1120(混ざるセル・ADR-0470 Proposed)・T-1086(D-649 の既定)・T-1067(割れかけの所を 0.25 m に下りる道・CPU)・T-1104(混ざる系の GPU・支えのレベルの逆を直した)・T-1087(閉じた領域)。分けた T-1133・T-1134(Todo)。T-1067 の判断待ち 1 つ(細かくする線・戻す線。仮 = 分ける 1/2・戻す 1/4 未満が 120 刻み続いた後・周り 1 セル。SupportFractureOptions の値)。
  - 試験(release): equilibrium 606 s・warm_start_save・equilibrium_block・support_law 177 s・support_mixed 94 s・support_fracture 571 s・_window 630 s・_thick 300 s・_refine 36 s・_settle 110 s・gpu_equilibrium_block 合格。
- **wt5 t-1064-object-heat-gpu の d11d9a55 を --no-ff**(衝突は docs/perf.md の表の行だけ): 物の熱 T-1097 の切り分け・放射の吸い込みの頭打ちの直し OhAbsorptanceQ16(ADR-0434 の紙の「焦点のボクセルが燃え切る」は記録に下げた・燃え続ける 1 s 以上は残す)・T-1077 の GPU と多重解像度(① 一様のブロックを頁に広げる・② 起きている物の一覧。残り ③ ハッシュとセーブ)。T-1142(= ROADMAP の T-1158 の 1 段目)は Open。
  - **注(今は許す)**: 物の熱のルート署名が 63/64 語(足す余地 1 語)・tests の object_heat_nest_scene.h が sim/multires_nest_internal.h を include している(試験の中だけ)。
  - 試験(release): object_heat・object_heat_nest 49 s・grass_wake・multires・multires_state・multires_edge・gpu_object_heat・gpu_object_heat_nest 合格(multires の 3 本は 3 本を合わせた main で)。
- 3 本とも release 警告 0・archmap OK・push 済み。**合わせた確かめ**(3 本の後の main・release): multires・reaction_package・standard_packages・probe_gas 546 s・radiation_scene 249 s 合格。
- **番号の訂正**: 前の記録の「次は T-1163」は誤り。司令塔が作業役に配った番号: wt6 T-1163〜T-1165・ADR-0478 / wt3 T-1166〜T-1168・ADR-0471 / wt4 T-1153・T-1169・T-1170・T-1173〜T-1175・ADR-0477 / wt2 T-1135・T-1171・T-1172・ADR-0476 / wt5 T-1143・T-1144・ADR-0473。
- **マージしていないブランチ(この後)**: wt3 t-1105-air-agitation(T-1136)・wt6 t-1113-radiation-all(T-1113)・wt4 の c38ac3c2 より後(T-1153・T-1169・T-1170)・wt2・wt5 のマージした点より後の分。
- 次の番号は **D-669・ADR-0479・T-1176・Q173**。T-1159 はこの仕事では着手していない。

(前の版の最終更新: 2026-10-26 本体の作業役・Q167〜Q172 の答えの記録 D-663〜D-668・T-1157)

- **ユーザーの答え(2026-10-26 05:00 頃・すべておすすめ)**: Q167 → D-663(運ぶ欄は移した質量に比例・案 B の運動量の式の時に)・Q168 → D-664(細かくする時は置かれ方で配る)・
  Q169 → D-665(混ざるセルだけ相ごとのエネルギー・probe_phase を現実の値で決め直す)・Q170 → D-666(状態で細かくするを測り、超える所だけ面の皮)・
  Q171 → D-667(物の箱を外へ広げた流れる空気の層・層 1 段流れなしから・帳簿で移す)・Q172 → D-668(T-1136 の 2 本目が外れたら粒の速さの置き場を足してよい)。
- ADR-0469 は項目ごとの状態に分けた(問いにしなかった項目は一旦の決定)。直す順は ROADMAP「土台の変更要求の束」: T-1158(3-4)→ T-1157・T-1159(芯 2)→ T-1160(3-1〜3-3)→ T-1161(芯 4)→ T-1162(芯 5)→ 案 B の運動量の式(1-1)。
  設計: 20 §2.3(D-663)・§10.2・§10.5・§10.6・§12.5・17 §3.2。COMMANDER に「質問で作業を止めない」。
- **T-1157 Done(1 本目・t-1157-c1-refine を main へ)**: 置かれ方で配る式を state_field.hlsli に(CPU と GPU の 1 つの源)・登録の印 `SF_FLAG_PLACE_CONDENSED` / `SF_FLAG_PLACE_OPEN`(無印 = SAME)・
  CPU リファレンス sim/placement_refine(wt3 の SettleColumn・SettlePictureCell と乱数の絵でビット一致・3 次元の保存)・欄の細かくするへの口 `MultiresNest::refinePlacement`。
  今の世界はどの欄にも印が無く口も無いので結果は変わらない(multires の CPU の試験がそのまま通る)。**未確認**: 新しい式の DXC(include するシェーダーがまだ無い)・GPU のビット一致 → T-1159。
  残り(木のセルの中身を置かれ方で・隣のブロックの読み方・GPU・2-2・2-3・2-5・影)は T-1159。wt3 の t-1105-air-agitation をマージする時は、場面の作り方の SettleColumn・SettlePictureCell を sim/placement_refine に置き換える(同じ答え)。
- ~~次の番号は **D-669・ADR-0479・T-1163・Q173**~~(誤り。上で直した)。

(前の版の最終更新: 2026-10-26 本体の作業役・T-1132 土台の変更要求の束・ADR-0469)

- **T-1132(文書だけ)**: 下の「土台に足りないもの」の全部と T-1110 の問いを **ADR-0469(Proposed)** の 5 つの芯にまとめ、答えでエンジンの作りが変わるものだけ **QUESTIONS Q167〜Q172**(【エンジン】)に足した。ほかの項目は ADR-0469 のおすすめを仮として進めてよい。
- 次の番号は **D-663・ADR-0479・T-1157・Q173**(ADR-0469・T-1132 は使った)。

(前の版の最終更新: 2026-10-26 本体の作業役・t-1062-light-alt を main へ・D-661・D-662)

## この仕事でしたこと(2026-10-26・本体の作業役・wt6 の光のマージ)
- **t-1062-light-alt(03c26edf)を main へ --no-ff**(衝突なし): 空との交換の口(T-1145・sky_port・GpuSkyPort・ADR-0474 Proposed・CPU⇔GPU ビット一致)・
  D-658 の既定化(熱の放射も組で待たせる。emitSlots 15・thermalInSlots true)と組の式 (bx + 4 bz + 6 by)(T-1146 Done)・
  T-1147(口を 64³ 4.42 → 0.46〜0.55 ms。向きを刻みで回す形は値で選べるが既定は切)・T-1100〜T-1102・T-1116・T-1117 の記録。
- 記録: **D-661**(早送りの待ちの見積もりは今の変化の速さが続くと見なす外挿)・**D-662**(費用の値は起動時に小さい系を解いて測る・機種の表は持たない)。どちらも 2026-10-26 ユーザー。ADR-0472・wt4 の T-1111。
- 通した試験: release 警告 0・archmap OK・CPU 7 本(sky_port・sun_column・cascade_light 801 s・cascade_light_swing 783 s・reaction_package・standard_packages・radiation_scene 240 s)・GPU 3 本(gpu_sun_column 4 s・gpu_cascade_light 48 s・gpu_cascade_light_wide 30 s)。push 済み
- **マージしていないブランチ(2026-10-26)**:
  - wt2 t-0872-rb2-fracture: T-1120・T-1086 Done・T-1067 作業中
  - wt3 t-1105-air-agitation: T-1106 Done・T-1136 作業中
  - wt4 t-1063-rt2-budget: T-1111・T-1109 Done・T-1141 作業中(ADR-0472 は D-661・D-662 で答え済み)
  - wt5 t-1064-object-heat-gpu: T-1097 の切り分け・T-1077 作業中
  - wt6 t-1113-radiation-all: T-1113 作業中(t-1062-light-alt の上。今は main の上と同じ土台)
- **司令塔が設計役へ回す土台に足りないもの**(作業役は足さずに印を付けた。CLAUDE.md §1-9):
  - wt6(光): 口の吸った分をセルへ当てる段・RC の持ち越しへ足す口・帳簿の世界の外との出入りの行・掃き直しに要る「変わったブロックの印」と前の値の写し
  - wt3(粒・空気): C-1 の細かくする規則に沈めて配る・複数の欄を一緒に見る・隣の親を読む
  - wt2(支え): 温めた始まりの欄の、眠る時は捨てる扱い
  - wt5(物の熱): 物の中のボクセルどうしの放射・物を育てる口・物の箱が囲む空気のボクセル(D-660 で要る)・膜の厚さ
  - 本体 T-0691(気体): 運動量も移した質量に比例させて運ぶ形(ADR-0355 の追記。案 B の運動量の式と一緒に)
- **T-1110(研究)は 1 回目で打ち切り = 設計へ**(docs/tickets/T-1110): 薄い凝縮相の層の厚さを熱の道にする捨てる試作(ブランチ t-1110-layer-path 086e1188・push 済み・**main へ入れない**)で
  probe_phase の氷が 18 秒で 1.5 %・水たまりが 3 %(試験の線 9 割・3 割)。約 30 倍足りない。根はセルの温度が 1 つ(溶ける氷・沸く水は接する面で熱を吸う)。
  土台の問い(設計へ): (a) セルの中の凝縮相の層の形 (b) 層と気体を 2 つの温度に分けるか(ADR-0434 の膜と同じ考え)(c) 接する固体の側の境界の層(現実の石で √(αt) ≈ 8 mm)。
  司令塔の判断: probe_phase の場面と線を現実の値で決め直すか(水たまりは現実なら膜沸騰でほとんど湯気にならない見込み・未確認)。probe_phase は DISABLED のまま。
- 次の番号は **D-663・ADR-0479・T-1157・Q167**(wt6 に T-1154〜T-1156・ADR-0478 を配った)。

(前の版の最終更新: 2026-10-26 本体の作業役・T-0691 を main へ・D-660)

## この仕事でしたこと(2026-10-26・本体の作業役・T-0691)
- **T-0691 を main へ**(t-0691-gas-stable を --no-ff)。燃える木箱(probe_gas)の負の熱の根: 塊は運動量を割合 F で、質量を移す物質量(MUSCL)で持って出るので、風上に残る運動のエネルギーが (1 − F) K を超え、
  その差(燃える木箱で F 0.7167・μ 0.7200、1 セル 4 J)が熱の列(ADR-0355)を通り抜けて熱を食っていた(全部「運ぶ側」。T-0814 の見込み 1)。
  熱の列の出ていく和にその差を足した(gas_reference.cpp の RestKineticExcess・gas_flow.hlsli の GfRestKineticExcess / GfKineticShare。ADR-0355 追記)。ふつうの流れでは列が効かないので値は変わらない。
- **probe_gas の DISABLED を外した**(全部 OK: 燃える木箱 熱が負のセル 0・熱の重心 11324 ミリセル・露の 4 場面も 0)。止めたままは reaction_package_scene・lab_box・gpu_probe_fire(T-1113)・probe_phase(T-1110)。
- 分けた: **T-1130**(力積の側で小刻みの中だけ熱が負・露の燃える木箱で 1 小刻み 4 セル)・**T-1131**(投影で燃える木箱を流すと区画のほぼ全部の熱が負。前からある見込み)。T-0814 は Done。
- **wt4 の t-0813(GPU の熱の列。main へはまだ)をマージする時は GfRestKineticExcess も GPU の列へ**(無いと燃える木箱で CPU と一致しない。ROADMAP の T-0813 の行に注)。
- 根から一つの芯で解く案(運動量も移す質量に比例して運ぶ)は、案 B の運動量の式(土台)を決める時に一緒に(ADR-0355 追記。司令塔へ)。
- 記録: **D-660**(2026-10-26 ユーザー。wt5 T-1097: 標準の木・草・紙は熱分解で燃える気体を出す・物の周りの空気を物と同じ細かさで持つ口が要る)。
  **wt5 の物の放射の吸い込みの頭打ちの直し(OhAbsorptanceQ16)で ADR-0434 の「紙が燃え切る」が崩れた**(T-1097 で扱う)。
- 通した試験: release 警告 0・archmap OK・CPU 7 本(probe_gas 544 s・multires_gas・gas_reference〔衝撃波の管の値そのまま〕・gas_projection・face_open_gas・gas_base_layers・gas_tree)・GPU 6 本(gpu_gas・gpu_gas_flow・gpu_gas_blast・gpu_gas_species・gpu_gas_tree・gpu_probe_gas 180 s)
- T-1110 は時間切れで着手していない。

(前の版の最終更新: 2026-10-25 本体の作業役・T-1066 を main へ・D-658・D-659)

## この仕事でしたこと(2026-10-25・本体の作業役・T-1066 の仕上げ)
- **T-1066 を main へ入れた**(t-1066-c3 を main へ rebase して --no-ff。セルの伝導率を体積の平行の道に・気体の伝導率 ∝ √T・混ざる速さ ∝ T^1.5・試験の倍率を外す。D-637・D-639・ADR-0454)。D-648(滑らか)に合っている(HcGasScaleAt は mK ごとの整数の平方根)。
- gpu_lab_box の落ちは試験の前提の崩れ: 現実の伝導率で火が広がらず刻み 200 に燃えている所が無い・1500 K のセルはどちらの表も孔の O2 で頭打ち → 燃焼を速くした表が箱を変えなかった。燃え始め 600 K の木を 2 か所に置いて直した(判定は緩めていない)。
- 通した試験(release): GPU 6 本(gpu_lab_box・gpu_reaction・gpu_multires_implicit_conduction・gpu_object_heat・gpu_multires_conduction・gpu_probe_sim)+ 前の巡目の 11 本・CPU 8 本(コードは 4 回目の CPU 104 本と同じ)。警告 0・archmap OK。
- 止めたままの試験: reaction_package_scene・lab_box・gpu_probe_fire(T-1113)・probe_gas(T-0691)・probe_phase(T-1110)。
- .worktrees/t1066 は中身が main と同じだったので消した。リモートの t-1066-c3 は rebase 前(39fffecc)のまま(force push は git ジョブが拒否するので残した。main に全部入っている)。
- ユーザーの答え: **D-658**(Q156 = 熱の放射も組に分けて順に待たせる。ADR-0444 に注)・**D-659**(案 B の粒を 2 つの状態で持つ。T-1105 の ①)。

(前の版の最終更新: 2026-10-25 司令塔・claude/40・41 の決定の反映 D-647〜D-657)

## この仕事でしたこと(2026-10-25・司令塔・設計の決定の反映。文書だけ)
- claude/40 の ★(Q163〜Q166 の答え)→ **D-647**(標準の物質に放射の吸い込みを現実の桁で。Q161 を閉じた)・**D-648**(伝導率の温度の関数は滑らか。ADR-0465・T-1109)・**D-649**(共役な向きを刻みをまたいで持ち越す。ADR-0455・T-1086)・**D-650**(大きな火事の最中に寝た時の早送りの待ちはゲーム側。ADR-0390・T-1048)。
- claude/41 の ★(描画の目標の線 S67〜S73)→ **D-651〜D-657**(基準機でも映画並みのライティング・削る基準・退き方・時間にためる条件・応えの合格線・取り分は ⑥ まで描画 12 ms・回し方)。docs/design/10 §8.1・06 §4・ROADMAP の M7 と ⑥・D-209・D-635 に注。
- CLAUDE.md §2 に「上書きは一時ファイル → mv」。
- 2026-10-24 夜にランナーへ積まれていた T-1066 の GPU 12 本は 2026-10-25 22:06 に流れ、**gpu_lab_box が 1 本落ちた**(表の差し替え: 途中で替える: 表を替えても箱が変わらない。gpu_lab_box_test.cpp:730)。CPU の組は通っていた。ブランチ t-1066-c3(39fffecc・push 済み)。
- .worktrees/t1066(t-1066-conductance)には**コミットしていない変更が残っている**(object_heat.hlsli・試験 4 本など)。t-1066-c3 に入った分か確かめてから消す。

(前の版の最終更新: 2026-10-24 本体の作業役・RC を main へ〔D-636〕・T-1066 式の直し〔D-637・D-639〕)

## 状態(2026-10-22 夕方 止めた。ユーザーの指示で切りのいい所で止めた)
**(1) main に入ったもの(2026-10-22 の巡目)**
- C-0/C-1 の土台: T-0931 の 1 チャット目(CPU・セルの頁を成分の数の幅に・ADR-0417)・T-0919 成分を持つ節と支えを 1 つの系で(ADR-0418)・T-0922/T-0955 解き始めを C-1 の欄に(ADR-0427)・T-0953/T-0954 基準の層と `fine_region`(ADR-0431)・T-1008 の CPU 分(木が細かい所の規則を読む・ADR-0436)。
- 面の開き具合: T-0937 欄(ADR-0420)・T-0976 気体も読む(ADR-0429)。
- 研究の合格: R2 の 1 本目(t-0869-r2・ADR-0416)・R-T2 の 1 本の列(t-0856-rt2・ADR-0410)・R-R2 日の光をレンズで集める(t-0878-rr2-lens・ADR-0425/0426)・**T-0991 物の箱の熱(t-0991-object-heat 51a04de・ADR-0434 Proposed。紙が虫眼鏡で燃え始め燃え続ける・CPU)**。
- サンプルの世界の光の現実の値。ユーザーの決定 D-616〜D-620・設計チャット S55〜S66 の反映 D-621〜D-632。
- 記録だけ: R-T2 の T-1013/T-1014(一旦 (B) 本筋 + 前線の緩衝 + 予算 1/16 mK)・T-0951 見積もりで閉じた・R-B2 の 1 チャット目は打ち切り(→ T-1042)・R3 の 2 本の打ち切り(根は粒と空気の引きずり T-1003)。

**(2) マージしていないブランチと理由**
- ~~t-0958-rr1-cpu・t-0972-rr1-fix・t-0959-rc-gpu・t-1010-rc-fast~~: **2026-10-24 main へ入れた**(t-1010-rc-fast を --no-ff。ほかの 3 本の中身は t-1010-rc-fast に含まれる〔同じ差分を rebase したもの〕ので個別にはマージしていない)。
- t-1066-conductance(本体 2026-10-24): T-1066 の式。CPU 7 本の基準の取り直しと probe_gas の負の熱の調べの後に main へ。
- t-0998-hourglass・t-1004-grain-moving: R3 の判定は T-1003 の後(番号の重なり ADR-0436・T-1019 はマージの時に付け直す)。
- t-0950-rt2-tree・t-0985-rt2-select・t-1013-rt2-jump・t-0951-gpu-estimate: R-T2 の続き(ADR-0423 は t-1013 のマージの時に (B) へ戻した注を足す)。
- t-0872-rb2-fracture: R-B2 の 3 本目(T-1042)の後。
- 前からのもの: t-0820・t-0751・t-0813・t-0807・t-0834・t-0784 など(T-0858 棚卸し)。
- 各作業ツリーが止めたブランチ(どれも push・マージしていない・main へ rebase 済み):
  - wt2 `t-0872-rb2-fracture` 14c2c73: R-B2 割れ。3 本目 T-1042(判定を材料の長さ ℓ で平均)で棚・洞窟は合格、厚い片持ち梁は 0.5/0.25 m の差 11 %(線 ±10 %)。
    止めた所: 棚 D を 8 m に直した後の試験が未。次: D の再実行・ℓ = 1 m で A・ADR-0440 の追記。【エンジン】ℓ の既定と割れの判定の細かさの下限(ℓ/2)。
  - wt3 `t-1004-grain-moving` 30d7298: T-1003 根を直した(粒が流体から受け取る運動量を 1/65536 にしていた桁の誤り。ADR-0441)。空気の中の砂の塊がほぼ自由落下に。R2 の合格は保つ。
    副作用で R3 の空気ありの砂山が崩れすぎる(芯 33° → 0〜10°)→ T-1043。止めた所: ctest `^case_b`・`^face_opening$`・archmap は未。
  - wt4 `t-1053-rt2-error` a885a1c(t-1013-rt2-jump・t-0985-rt2-select・t-0951-gpu-estimate を含む流れ): T-1053 試し 2 + 前線の緩衝は 2 本とも落ち(夜の精度・池の細かさ)採らない。
    次: ADR-0445 に落ちを記録・別の選び方を出す。R-T2 の待ち時間の線(夜 5 秒・火事 30 秒)は今の形では届かない見込み(T-0951)。
  - wt5 `t-0861-rf1-wake` e640139: R-F1 は下調べとチケットだけ(合格の仮・2 本の形・退く道)。
  - wt6 `t-1010-rc-fast` 41aca14(t-0959-rc-gpu の 2 コミットを含む): T-1010 1 本目(放つ面の組・ADR-0444)で 48³ は全場面 1 ms の内、64³ の照り返す地面は 1.05〜1.15 ms(前は 4.1〜7.8)。
    熱も待たせるので遠くの火の熱が最大 0.23 秒遅れて届き、平均が収まるのに約 7 倍の刻み(→ Q156)。2 本目は 1 回測っただけで揺れ(原因未確認)。

**(3) 判断待ち**: Q153(物として書いた時だけ。仮。T-1064・T-1065 の後)・Q154(D-633 の予算の後に測り直す)・Q156(光の研究 D-636 の後)・Q157(割れの ℓ。もう少し測ってから)(docs/plan/QUESTIONS.md)。2026-10-24 に Q147・Q150・Q151・Q152・Q155 は答え済み(D-636・D-638・D-640・D-639・D-637)。

**(4) 再開したら最初にやること**(NEXT.md の先頭と同じ): D-633 の予算の測り(T-1063)・物の熱の GPU 版と燃えている物の数の費用(T-1064・T-1065)・光の別の運び方の研究(T-1062。先に t-0958-rr1-cpu・t-0972-rr1-fix・t-0959-rc-gpu・t-1010-rc-fast を main へ)・止めたブランチの続き(T-1042・T-1003〔→ T-1043〕・T-1053・T-0861・T-1010) → 未着手の研究(R4 濡れ・R6a・M1・R-G1・R-O1・R-B3〜B6・縦切り)。
次の番号は **D-647・ADR-0469・T-1114・Q163**(T-1110 は本体が使った。T-1085〜T-1109・T-1111〜T-1112・ADR-0455〜0468 は司令塔が作業役に配った。COMMANDER)。

## この仕事でしたこと(2026-10-24・本体の作業役・3 回目: 4 つの作業ツリーのマージ・D-646)
- **マージ**(`--no-ff`・衝突なし。release・1 本ずつ): wt5 t-1064-object-heat-gpu f0065e7(T-1098・ADR-0459)・wt6 t-1062-light-alt 40e2348(光の別の運び方の研究の記録と tools/light_exchange。既定は変えない)・
  wt3 t-1043-grain-remeasure(R3 の砂の系統。T-1003 の単位の直し・grainRestLock・T-1043・ADR-0449)・wt4 t-1063-rt2-budget 0268260(R-T2 の系統。既定 = 予算 0.1 K・緩衝なし・単位 4 m・e ≤ 8 は V 1)。
  確かめ: object_heat・gpu_object_heat・face_opening・case_b_box・case_b_gauge・case_b_r1・case_b_r2・case_b_r3・time_step_lab・time_step_tree・cascade_light・gpu_cascade_light。
  wt3 の t-1070・t-1071 はコードを入れず、チケット(T-1070・T-1071・T-1091)と ADR-0457・ADR-0463(Rejected)だけ写した。wt2 t-0872-rb2-fracture は試験が 1 本落ちているので入れていない。
- 記録: **D-646**(光は RC のまま・筋は数秒の平均で ±12 %。Q162 を閉じた)。ADR-0424 の問い・ADR-0458・ADR-0423 に注。ROADMAP に R-T2(T-1073〜T-1075・T-1088〜T-1090・T-1108・T-1109)・光(T-1079〜T-1081・T-1094・T-1095 は閉じた)・T-1097・T-1098 の行。
- Q161 は司令塔の一旦の決定で「書く」。T-1066 の続きは **ブランチ t-1066-conductance-2(aa0111b。rebase したので新しい名前で push。古い t-1066-conductance は 96dca3b のまま)**:
  real_matter のセルロース・炭に放射の吸い込み(長い帯の光と同じ値)を書いた WIP。草原は手前 14 s・川 18 s で燃え広がる(ac・ak で与えた回)。既定の場面ではまだ広がらない(未確認の原因)。
  燃える場面の試験 4 本 + gpu_probe_fire は、放射の段の無い世界なので値を書いても直らない。reaction_package(光の警告の試験)・object_heat(wt5 の共有の口と式が 2 つ)が WIP で落ちる。probe_gas の負の熱は T-0691 が先。→ T-1066 は main へまだ。
  氷・水たまりは 0.5 m の中心どうしで伝導を数えるため → T-1110(層の厚さで道の長さ)。
- **wt2 t-0872-rb2-fracture(1aa90c0)を 2026-10-24 main へ**(--no-ff・衝突なし。support_fracture 905 s・_window 1452 s・_thick 573 s・_settle 92 s・support_law・gpu_equilibrium_block 合格。時間の枠 3000 s の内)。
- T-1113(放射の段をすべての世界へ。T-0586・T-0973 をまとめた)を立てた。T-1066 は放射の段の無い世界の燃える場面の試験を T-1113 を指して DISABLED にしてから main へ(司令塔の決定)。

## この仕事でしたこと(2026-10-24・本体の作業役・2 回目: T-1066 の調べ・wt5 のマージ)
- **マージ**(`--no-ff`・衝突なし): t-1064-object-heat-gpu(wt5 ba0cfe6。T-1064 物の熱の GPU 版・T-1065 費用・T-1078 GPU の食い違いの直し・ADR-0451)。確かめ(release・1 本ずつ・警告 0): object_heat・gpu_object_heat 合格。
- CLAUDE.md §4 に「シミュの HLSL の式に 32bit を超える定数を書かない(32bit の定数の積と割り算に。T-1078・ADR-0451)」。ROADMAP に T-1076〜T-1078(T-1078 は Done)。
- **T-1066 はまだ main へ入れない**(ブランチ t-1066-conductance 96dca3b・push 済み): 負の熱(probe_gas)は式の誤りでなく T-0691(気体の段の陽解法の安定)が表に出たもの・lab_box の範囲の外は基準の変化。
  面の流れの切り捨てが現実の空気の粗いセルで拡散率を −23 % にしていたので最も近い整数に丸めた(物差し 2 本が通った)。32bit を超える定数も外した。GPU 5 本ビット一致。
  **本物の値で燃え広がるのは放射が支えた時だけ**(草原: 吸い込みを書くと手前 15 s・川 20 s、書かないと広がらない)。燃える場面の試験 4 本 + gpu_probe_fire はこの理由で落ちる → **Q161【エンジン】**(標準の物質に放射の吸い込みを書くか。Q158 は司令塔の番号と重なったので付け直した)。gpu_probe_gas は時間切れ(310 s。前 180 s)。

## この仕事でしたこと(2026-10-24・本体の作業役・RC を main へ・T-1066)
- **マージ**(D-636。`--no-ff`・衝突なし): t-1010-rc-fast(41aca14。T-0877・T-0958・T-0972・T-0959・T-1010 の中身。共通の核 cascade_light.hlsli・CPU リファレンス・GPU 版・合格 6 項目の試験・ADR-0444)。
  t-0959-rc-gpu・t-0958-rr1-cpu・t-0972-rr1-fix は同じ差分を rebase したもの(git cherry で中身が同じ)なので個別にはマージしていない。
  確かめ(release・1 本ずつ・警告 0): cascade_light・gpu_cascade_light・sun_column・gpu_sun_column・sun_lens・reaction_package・standard_packages 全部合格(gpu_sun_lens という試験は無い)。
  ADR-0413・ADR-0424・ADR-0432・ADR-0444 に D-636 の注。ADR-0423(刻みの長さ)は wt4 のブランチにあり RC と別の話なので注は足していない。
- **T-1066**(D-637・D-639・ADR-0454): セルの伝導率を体積で重み付けた平行の道に(heat_conduction.hlsli。RxSpecies の空き欄を molarVolume に)・気体の伝導率 ∝ √T・混ざる速さ ∝ T^1.5(HcGasScaleAt)・
  試験の倍率を外した(units.luau 2 つを倍率 1・C++ の試験の表)。仮の世界のコンダクタンスは本道で毎回作り直す。分けた T-1082〜T-1084。
  **main へはまだ入れていない(ブランチ t-1066-conductance。push していない)**: release 警告 0・archmap OK だが、CPU 94 本で 7 本が落ちる
  (reaction_package_scene・probe_gas〔燃える木箱で負の熱。調べが要る〕・probe_phase・multires_conduction・lab_box・multires_subcycle・multires_implicit_tree。
  どれも伝導で熱が速く回る前提の基準。表と次にやることは docs/tickets/T-1066〔ブランチの上〕)。GPU の試験はまだ。

## この仕事でしたこと(2026-10-24・本体の作業役・ユーザーの答えの記録。文書だけ)
- DECISIONS **D-633〜D-641**: 時間を飛ばす精度の予算の見直し(計器で見分けられる細かさ・例 0.1 K)・基準機 RTX 3070 Ti(最低機は ⑥ の後)・シミュと描画の分け方は ⑥ の後(仮の線 10 ms)・光は RC を土台に別の運び方と並べて研究(Q147)・混ざった所の伝導率は体積の平行 + 試験の倍率を外す(Q155)・面の開き具合の欄を形と物の 2 つに(Q150)・混ざる速さと伝導率を温度で(Q152)・刻みを選ぶ単位は物の長さ(Q151)・崩れ始めと止まる角度の差は法則の定数。
- QUESTIONS: Q147・Q150・Q151・Q152・Q155 を答え済みに。Q153・Q154・Q156 に注。**Q157**(割れの材料の長さ ℓ)を足した。ADR-0390・ADR-0434・ADR-0429 に注。
- ROADMAP: T-1062(光の別の運び方の研究)・T-1063(D-633 の予算の測り)・T-1064(物の熱の GPU 版)・T-1065(燃えている物の数の費用)・T-1066(D-637・D-639 の式の直し)。T-0959・T-0977・T-1015 の行を決定に合わせた。
- **司令塔へ**: T-1043(D-641 の差を T-1003 の後に測り直す)・T-1042(Q157)は wt のブランチにあり main の ROADMAP に行が無い。マージの時に D-641・Q157 の注を足す。ADR-0423(D-640)・ADR-0424(D-636)も wt のブランチなので、マージの時に注を足す。

## この仕事でしたこと(2026-10-22・本体の作業役・止めたブランチの書き足し。文書だけ)
- HANDOFF の (2) に作業ツリーごとの止めたブランチ(wt2〜wt6)。QUESTIONS に **Q156【エンジン】**(熱の放射も組に分けて待たせるか。T-1010)。判断待ちは Q147・Q150〜Q156。

## この仕事でしたこと(2026-10-22・本体の作業役・t-0991-object-heat のマージ・止める前の記録)
- **マージ**(`--no-ff`・衝突なし): t-0991-object-heat(51a04de。試験 object_heat・ADR-0434 Proposed・T-1016〜T-1018・docs/design/20 §12.4)。
  確かめ(release・1 本ずつ・debug/release のビルド警告 0): object_heat・sun_lens・reaction_package・radiation_scene・multires・field_storage・standard_packages。全部合格。
- 記録: ROADMAP の T-0991(CPU で合格・残り)・T-0872(R-B2 の 1 チャット目の打ち切りと T-1042)・T-0861(作業中 wt5)・T-0951(見積もりで閉じた・T-1053 wt4)。
  QUESTIONS に Q153 の注(T-1018)と **Q155【エンジン】**(混ざった所の熱の伝わりやすさ)。worker-common.md に raw の .ps1 の BOM と .part の 1 行。COMMANDER の番号。

## この仕事でしたこと(2026-10-22・本体の作業役・R-T2 の T-1013・T-1014 の記録。文書だけ)
- ROADMAP: T-1013 の行に結果(退く道の 1 本目は精度は完全だが速くならない・T-1014 の火事の前線は緩衝で 0.000 mK・**司令塔の一旦の決定 (B) 本筋 + 前線の緩衝 + 予算 1/16 mK へ戻す**・CPU の試作は 1 回の解き 3〜4 µs/セル)・T-1014 の行(池は未解決 → Q154)・T-0951 を作業中(wt4)。t-1013-rt2-jump はまだマージしない。
- QUESTIONS に **Q154【エンジン】**(刻みの長さの細かさの合格の線を池で読み替えるか。(A) 9 割が 1 段の内だけにする / (B) 原因を調べ切るまで変えない)。
- ADR-0390 に「T-1013・T-1014 の後、(B) へ戻した(一旦の決定)」の注。**ADR-0423 は main に無い(wt4 のブランチ)ので、t-1013-rt2-jump のマージの時に同じ 1 行を足す**。
- COMMANDER: wt3 の付け直しに T-1046・ADR-0442、wt4 に T-1047〜T-1049・ADR-0443。次の番号は **D-633・ADR-0444・T-1050・Q155**。

## この仕事でしたこと(2026-10-22・本体の作業役・t-0953-base-layer の T-1008 分のマージ)
- **マージ**(`--no-ff`・衝突なし): t-0953-base-layer(ee7f509。T-1008 の CPU 分: 木の細かくする/粗くする要求が種からの距離で `GradedCoarseness` を読む・呼ぶ側が選んで呼ぶ形で、呼ばない今の木は変わらない・試験 multires_graded・ADR-0436)。release のビルドが通った(警告 0)。
  確かめ(release・1 本ずつ): multires_graded(114 秒)・multires・multires_activity・multires_grading・environment_package。全部合格。archmap --check OK。
- 記録: ROADMAP の T-1008 を「CPU 済み。残り T-1028 GPU とゲームの道で呼ぶ・20 km の実際の葉の数」・T-0872(R-B2)を作業中(wt2 t-0872-rb2-fracture)。worker-common.md に「job.py を 20 秒前後で切ると投入されないことがある」。
  COMMANDER: wt2 に T-1040〜T-1042・ADR-0440(T-1009・T-1029・T-1030 は使っていない)。**番号の重なり: ADR-0436(main の graded-tree-requests と wt3 の grain-moving-friction)・T-1019(描画 M7 と wt3 の impact-turn)。wt3 のマージの時に付け直す**。wt3 に T-1043〜T-1045・ADR-0441。次の番号は **D-633・ADR-0442・T-1046・Q154**。
  R3: wt3 の T-1019(縦の運動量を横へ)も打ち切り(ADR-0438 Rejected)。真空なら 0.25 m で 6.50 m まで伸びる → 根は粒と空気の引きずり(T-1003。砂時計と同じ根)。R3 の判定は T-1003 の後(司令塔)。T-1003 作業中(wt3)・T-1034・T-1035 の行。

## この仕事でしたこと(2026-10-22・本体の作業役・t-0953-base-layer のマージ・R-T2/R3 の記録)
- **マージ**(`--no-ff`・衝突なし): t-0953-base-layer(e1a4119。T-0953 基準の層を粗さをまたいで整数の和が合う形に〔10 km の柱で速さ 0・揺れ 0・境の段差 0〕・T-0954 細かい所の半径と移り目を世界のデータで〔fine_region。木はまだ読まない → T-1008〕・ADR-0431)。release のビルドが通った(警告 0)。
  確かめ(release・1 本ずつ): gas_base_layers・multires_grading・environment_package・gas_projection・multires_gas・equilibrium・standard_packages。全部合格(gas_base_layers 25 秒・gas_projection 63 秒・multires_gas 287 秒・equilibrium 1221 秒〔ほかの作業ツリーと重なって長め〕・ほかは 2 秒未満)。archmap --check OK。
- 記録: ROADMAP の T-0953・T-0954 を Done・T-1008 を作業中(wt2)・M-0(T-0851)の行に「10 km の空気の柱の残りの課題は T-0953 で片付いた」・perf.md の見込み(作業役が書いた分がマージで入った)。DECISIONS の D-544 に注「細かい所の半径は種からの距離(T-0954・D-623 と同じ)」。
  R-T2: T-0985 は 2 通り目でも火事の精度(5.6 mK)と池の細かさが直らず退く道へ(ADR-0423・ADR-0433)・T-1013(wt4 作業中・t-0985-rt2-select の上)・T-1014・T-1015 の行。t-0985-rt2-select(993130a)はまだマージしない。
  R3: T-0997 = 砂山は書いた値 ±1°(D-620)・T-1004 は打ち切り(ADR-0436 Rejected。根は落ちた柱の縦の運動量が消えて横へ向かないこと〔見込み〕)→ 司令塔の言う T-1019(wt3)。**T-1019 は描画の研究 M7 の R1 と番号が重なる。司令塔が付け直す**。t-0998-hourglass・t-1004-grain-moving はまだマージしない。
  QUESTIONS **Q151【エンジン】**(刻みの長さを選ぶ単位をブロック 8³ セルから決まった物の長さに。おすすめ = 物の長さ・火事で刻みが 100 % 揃う〔今 84 %〕・細かい所の解き 1.19 倍)。worker-common.md に「half を変数名に使わない」。
  COMMANDER: wt2 T-1028〜T-1030・ADR-0436、wt4 T-1031〜T-1033・ADR-0437、wt3 T-1034〜T-1036・ADR-0438。wt5 T-1037〜T-1039・ADR-0439。次の番号は **D-633・ADR-0440・T-1040・Q154**。
  T-0991: 設計(20 §12.4・ADR-0434 Proposed)と CPU の試作が研究の 2 本目で合格(0.1 mm の紙が虫眼鏡で 1.1 s で燃え始め、レンズを外して 12.2 s 燃え続けた・保存 0・細かさの差 0.68 %)。残り T-1016(wt5)・T-1017。t-0991-object-heat(2cc9127)はまだマージしない。QUESTIONS **Q152【エンジン】**(混ざる速さと伝導率を温度で。おすすめ = 変える)・**Q153【エンジン】**(小さい物が燃え続けるのは物として書いた時だけ。おすすめ = そうする)。

## この仕事でしたこと(2026-10-22・本体の作業役・t-0976 のマージ後の GPU の気体の確かめ)
- main 3d284fc の release(最新)で GPU の気体の組 14 本を流した: gpu_gas・_flow・_blast・_species・_tree・gpu_probe_gas・各 _warp 6 本・gpu_gas_projection・gpu_gas_projection_t0915(657 秒)。**全部合格**。t-0976(GfFaceFlowOpen・欄の入った箱を GpuGas が断る)で GPU の気体は崩れていない。直したものなし。

## この仕事でしたこと(2026-10-22・本体の作業役・設計チャット S55〜S66 の反映。文書だけ)
- 出どころ: claude/37(★ S55〜S61)・claude/38(★ S62〜S66)。★ の文言はそのまま DECISIONS **D-621〜D-632** に(S55 = D-621 … S66 = D-632)。
- docs/design: 10-rendering(§0 に描画だけの光・§6 に D-621・**§8 決定の要約・§9 下書き・研究**〔claude/37 §1〜§7・claude/38 §1〜§6・§8 を写した〕)・
  23-radiation(§2.2 に「色合いは描画だけ」。**D-618 と矛盾しない**: 描画は場を読まず同じ値から解き、色合いも読む。法則は帯の合計だけ)・
  14-editor-tools(描画の嘘の層の色分け・色の欄を 2 つに・感覚のつまみ・物質の値のいじり方〔claude/38 §7〕・小さな実験室の見本)・09(§6.1 肌・毛・髪)・17(§3.1 法則の細かさの下限と 2 つの木)。
- ROADMAP に「描画の研究(M7)」の節: T-1019〜T-1024(R1〜R6)・T-1025(雲)・T-1026(肌と髪)・T-1027(法則の細かさの下限の測り)。次の番号は **D-633・ADR-0436・T-1028・Q151**。
- **司令塔へ**: プロジェクト文書 claude/37・claude/38 の ★ の見出しを「反映済み」に書き戻すのは、この作業役に Projects ツールが無くてできなかった。

## この仕事でしたこと(2026-10-22・本体の作業役・t-0976-face-open-families のマージ)
- **マージ**(`--no-ff`・衝突なし): t-0976-face-open-families(3a02e5b。気体〔陽解法・陰の圧力〕も面の開き具合 `FoFaceOpen` を読む・溝の外へ煙も熱も漏れない・欄が 0 なら今とビット一致・
  熱の伝導は欄を読まない仮・試験 face_open_gas・ADR-0429 Proposed)。release のビルドが通った(警告 0)。
  確かめ(release・1 本ずつ): CPU face_opening(91 秒)・face_open_gas(181 秒)・multires_gas(434 秒)・gas_projection(95 秒)・probe_gas(1203 秒)・equilibrium(867 秒〔作業役は流していなかった〕)。全部合格(時間はほかの作業ツリーのビルドと重なって長め)。archmap --check・float_check OK。
- 記録: ROADMAP の T-0976 を Done・T-0975 に「GPU の気体の段にも面の塞ぐ割合の表を」・T-0977 は Q150 の後・T-0991 を作業中(wt5 t-0991-object-heat)。
  QUESTIONS **Q150【エンジン】**(面の開き具合の欄を形と物の 2 値に分けるか。おすすめ = 分ける。T-0977 の前に)。COMMANDER: wt5 に T-1016〜T-1018・ADR-0434〜0435。
  次の番号は **D-621・ADR-0436・T-1019・Q151**。

## この仕事でしたこと(2026-10-22・本体の作業役・t-0878-rr2-lens のマージ)
- **マージ**(`--no-ff`・衝突なし): t-0878-rr2-lens(55cfbb7。R-R2 日の光をレンズで集める CPU リファレンス〔sim/sun_lens・shaders/common/sun_lens.hlsli〕・屈折の値の欄 `light_refraction_permille`・
  燃え始めは溜めた熱で判定〔ADR-0426 Proposed〕・ADR-0425)。release のビルドが通った(警告 0)。
  確かめ(release): CPU sun_column・sun_lens(3.1 秒)・reaction_package・standard_packages / GPU gpu_sun_column(3.1 秒)。全部合格。archmap --check・float_check OK。
- 記録: ROADMAP の R-R2(T-0878)を CPU で合格(2 本目。虫眼鏡で紙が 0.17 秒で燃え始める・焦点の距離は薄いレンズの式と +2.3 %)・T-0990〜T-0992 の行
  (**T-0991 は土台**: 焦点の熱では 0.5 m のセルの平均が 0.001 K しか動かず、紙は燃え始めても燃え続けない見込み。物の箱 S45 か面ごとの熱の置き場が要る)・T-0959 を作業中(wt6 t-0959-rc-gpu)。
  COMMANDER: wt6 に T-1010〜T-1012・ADR-0432。次の番号は **D-621・ADR-0433・T-1013・Q150**。

## この仕事でしたこと(2026-10-22・本体の作業役・t-0922-warmstart のマージ)
- **マージ**(`--no-ff`・衝突なし): t-0922-warmstart(d0c05e8。陰の圧力と支えの解き始めを C-1 の印 `SF_FLAG_WARM_START` の欄に〔sim/equilibrium_fields〕・保存点から続けてハッシュ一致・
  環境のパッケージの `balance_cycles`〔V の回数を世界のデータで。T-0955〕・試験 warm_start_save・ADR-0427 Proposed)。release のビルドが通った(警告 0)。
  確かめ(release): CPU 8 本 reaction_package・environment_package・standard_packages・gas_projection(59 秒)・equilibrium(641 秒)・warm_start_save(27 秒)・support_law(182 秒)・field_storage・
  GPU(1 本ずつ)gpu_gas_projection_t0915(388 秒)・gpu_equilibrium_block(4.4 秒)。全部合格。archmap --check・float_check OK。
- 記録: ROADMAP の T-0922・T-0955 を Done(**解き始めをセーブに入れた量: 陰の圧力 8 B/セル・支え 24 B/セル・起きた所 100 万セルで 8 MB・保存点 +2.4 ms**)・T-0993・T-0994 の行・
  T-0953/T-0954 を作業中(wt2 t-0953-base-layer)。DECISIONS の D-608 に注(量を測った・今は切らない)。COMMANDER: wt2 に T-1007〜T-1009・ADR-0431(T-0995 は使っていない)。
  次の番号は **D-621・ADR-0432・T-1010・Q150**。

## この仕事でしたこと(2026-10-22・本体の作業役・09:29〜09:30 のユーザーの答えの記録。文書だけ)
- DECISIONS **D-618**(一旦の決定。描画は法則の放射の場〔世界空間の RC・1 刻み 1 ms・各刻み ±20 % 揺れ〕を読まず、同じ光源〔σT⁴〕と物質の光の値〔ADR-0395〕から描画の予算で毎フレーム自分で解く。S12 は光源・値・式が同じ意味。D-570 に注)・**D-619**(Q145: 照り返しの持ち越しをセーブとハッシュに入れる。1 セルあたり 0.05〜0.13 B)・**D-620**(Q149: 書いた安息角は山が止まる角度。R3 の 3 本目 T-0997)。
- 設計 10 §0〜§2・23 §1・§3.4・§10 R-R1 に反映。QUESTIONS: Q145 を答え済みに・Q149 を答え済みで足した・Q147 に「GPU の費用を T-0959 で測ってから決める」の注。
- ROADMAP: T-0959(R-R1 の GPU と費用。完了条件 = 今の形と約 8 倍の形の両方・RTX 3070 Ti・照り返す地面あり/なし)・T-1006(D-618 の確かめ。描画 M7)・T-0997・T-0998 の行、T-0869 の R3 の状態。COMMANDER: wt3 に T-1003〜T-1005・ADR-0430。次の番号は **D-621・ADR-0431・T-1007・Q150**。

## この仕事でしたこと(2026-10-22・本体の作業役・t-0937-face-opening のマージ)
- **マージ**(`--no-ff`・衝突なし): t-0937-face-opening(eb2ea78。面の開き具合の欄 `face.blocked`〔導かれる値〕・`FoFaceOpen` を MultiresLiquid が読む・粒が穴を通る口 `FoGrainFaceOpen`・試験 face_opening・ADR-0420 Proposed)。
  release のビルドが通った(警告 0)。確かめ: CPU `^(face_opening|probe_liquid|surface_detail|multires.*|field_storage)$` 18 本(multires.* 14 本。face_opening 36.7 秒・multires_gas 227 秒・計 648 秒)・GPU gpu_probe_liquid(22.2 秒。順番待ち 226 秒)・gpu_surface_detail(1.3 秒。順番待ち 598 秒)。全部合格。archmap --check・float_check OK。
- 記録: ROADMAP の T-0937(CPU 済み)と T-0975〜T-0977 の行(T-0976 は wt5 作業中)・COMMANDER(wt5 に T-1000〜T-1002・ADR-0429。次は **D-618・ADR-0430・T-1003・Q149**)・
  runner/worker-common.md に「試験の時間を比べる時は、ほかの作業ツリーのビルドとの重なりも見る」。

## この仕事でしたこと(2026-10-22・本体の作業役・番号の表と作業中の行の直し)
- COMMANDER の番号の表: wt6 t-0972-rr1-fix が T-0987・ADR-0424 を使った・wt6 に T-0878 + T-0990〜T-0992・ADR-0425〜0426・wt2 に T-0993〜T-0995・ADR-0427。T-0983・T-0988・T-0989 は使わない。次は **D-618・ADR-0428・T-0996・Q148**(下で進めた)。
- **マージ**(`--no-ff`): t-0970-block-gpu(bb9590c。成分を持つ節の GPU 版・ADR-0422・T-0946 Done)・t-0963-r2-place1(46a76e7。R2 の 2 本目・置き場 ①・ADR-0421 Proposed。perf.md の行の衝突だけ・両方残した)。release のビルドが通った(警告 0)。
  QUESTIONS Q148(案 B の既定の置き場 ① / ④。今は聞かなくてよい)。COMMANDER: wt3 に T-0996〜T-0999・ADR-0428。次は **D-618・ADR-0429・T-1000・Q149**。
  確かめ(release・1 本ずつ): GPU gpu_equilibrium_block(5.6 秒)・gpu_gas_projection_t0915(812 秒。前は 341〜381 秒)/ CPU support_law(167 秒)・equilibrium(635 秒)・case_b_box・case_b_gauge・case_b_r1(372 秒)・case_b_r2(46 秒)。全部合格。
- **t0915 の 812 秒の調べ(本体の作業役)**: コードの遅れではなく試験の待ち。GPU の順番待ちは 0 秒で、実行中(06:00〜06:14)に wt4(06:03〜06:10)と wt5(06:12〜06:18)の release の全ビルド(505 個)が重なった(この試験は毎刻み CPU の部品と比べるので CPU の取り合いで延びる。ビルドは GPU の順番待ちに入らない)。ビルドの重ならない時に今の main で流し直して **365.8 秒**。128³ の陰の圧力(`--t0845 --edge 128 --ticks 2 --measure`)は **16.3〜19.4 ns/セル**(T-0918 の 16.4〜19.4 と同じ。式 3.7〜3.8・段の重み 1.7・V 5.2〜6.2・当てる 2.3〜3.1・運ぶ 2.3〜4.6)。t-0970 のマージは陰の圧力のシェーダー・コードに触れていない(equilibrium_block は別のシェーダー)ので 25b6d73^1 との比べはしていない。
- QUESTIONS **Q147【エンジン】光の筋の合格を瞬間で見るか数秒の平均で見るか**(おすすめ (A) 平均)。Q145 は答え待ちのまま。ROADMAP の R-R1(Q147 待ち・2 本のブランチはマージしない)・R-R2(作業中 wt6)。

## この仕事でしたこと(2026-10-22・本体の作業役・t-0931-sparse-cell のマージ・サンプルの世界の光の値)
- **マージ**(`--no-ff`・衝突なし): t-0931-sparse-cell(7193072。`MultiresNest::cells` を `MultiresCellStore`〔一様の値 + 成分の数に合わせた幅の頁〕に・`DenseCellImage`・ADR-0417)。
  main の今に `nest.cells[...]`・`CellAt`・`PageCellAt` を使う所は残っておらず、口の直しは要らなかった(release のビルドが通った・警告 0)。
- **確かめ(release)**: CPU 19 本(field_storage・multires* 17 本・lab_box・equilibrium・support_law・case_b_box)・
  **GPU 5 本(gpu_multires・gpu_multires_activity・gpu_lab_box〔T-0931 の作業役が比べ方だけ直して流していなかった 3 本〕・gpu_world_copy・gpu_probe_sim)** が通った。
- **サンプルの世界の光の値**(T-0942 の続き。サンプルの世界は現実の科学で D-006 に当たらない): real_matter の carbon・cellulose・water・ice と場面 grass_fire_river の silica に
  ADR-0395 の 4 欄を現実の値で書いた(炭 = 両帯真っ黒・水と氷 = 見える光はほぼ透明〔a 3〕・赤外はよく吸う・セルロース = 見える光 40 % 返す・砂 = 見える光 35 % 返す。
  決め方と未確認の所は real_matter の出典・場面の species.luau・T-0942 のチケットの表)。標準とサンプルの世界に光の警告は出なくなった。
  試験を元の厳しさに戻した: reaction_package(`OtherWarnings` は坂だけ除く・`TestMissingLightWarning` は「標準 0 件 → 消すと 1 件」)・standard_packages(坂以外の警告が無い +
  場面 grass_fire_river に光の警告が無い)。sun_column の掃きの場面は試験の石・ガラス・空気だけなので結果は変わらない(`TestLightFields` のセルロースの期待だけ書いた値に。意図した変化)。
  試験: reaction_package・standard_packages・environment_package・luau_type_check・reaction_package_scene・sun_column・gpu_sun_column が通った。
  警告の数に触れうる 8 本(blank_world・probe_phase・cell_volume・cell_view・world_layout・case_b_box・coupling_table・multires_edge)も通った。archmap --check OK。
- **記録**: ROADMAP(T-0931 = 1 チャット目済み・T-0966〜T-0968 の行・T-0937 作業中・T-0942 の値)・perf.md の M-0 の節に詰めた後の値(空気 41.5・燃えている所 81.5・地面と水と空気 94.4 B/セル)。
  ここまでを c96cf3d で push。
- **続けてマージ**(`--no-ff`・衝突なし): t-0869-r2(9b6a20d。案 B の R2 の 1 本目: 流体 ↔ 粒・孔の引きずり。測った行が全部合格・ADR-0416)・
  t-0919-c0-components(f841f1d。成分を持つ節 equilibrium_block と支えを 1 つの系で解く〔`SupportSolve::Coupled` 既定〕・ADR-0418。T-0945 の中身も済んだ → Done)・
  t-0856-rt2(8499224。R-T2 の 1 本の列の試作 time_step_lab。試し 1 が合格の項目を通った・ADR-0410 Proposed)。
  確かめ(release・警告 0): t-0869-r2・t-0919 の後に CPU 7 本(case_b_box・case_b_gauge・case_b_r1〔252 秒〕・case_b_r2・equilibrium〔915 秒〕・support_law・gas_projection)・GPU gpu_gas_projection_t0915(成分 1 の道)。
  t-0856-rt2 の後に time_step_lab。archmap --check OK。
- **記録**: ROADMAP(R2 = 1 本目合格・2 本目 T-0963 作業中・T-0919 CPU 済み〔着く刻みと割れの判定の前後〕・T-0945 Done・T-0969〜T-0971 の行〔T-0970 作業中 wt2〕・
  R-T2 = 1 本の列で試し 1 合格〔川 1 晩 124 → 6.7 秒・火事 18 分 26 → 1.9 秒。CPU 試作〕・T-0950 作業中 wt4・T-0951・T-0952 の行)・
  QUESTIONS **Q146【エンジン】**(刻みの長さの歩の途中のセーブ)→ 2026-10-22 02:46 ユーザーが答えた: DECISIONS **D-616**(= (A) 歩の記録も保存)・**D-617**(支えは `SupportSolve::Coupled` の 1 つの系。ADR-0418 の該当の仮を外した。V の回数と棒の硬さは T-0971 で測ってから Claude が決める)・
  COMMANDER(次の番号 **D-618・ADR-0424・T-0987・Q147**)。

## 前の仕事(2026-10-21・本体の作業役・T-0942・t-0918-c0-compact・t-0957-rr0-fast のマージ)
- **T-0942**(80c626c。docs/tickets/T-0942): ベイクが固体・液体で光の値(light_absorption_*・light_reflectance_*)を 1 つも書かない物質と、片方の帯だけ書いた欄を警告する
  (`sim::LIGHT_WARNING_PREFIX` = `"光: "`。表は当たる。気体の書き忘れは知らせない)。game = true でも警告のまま(密度・粘りに合わせた。仮。T-0836 の道で上げられる)。
  **標準の凝縮相 4 つ(carbon・cellulose・water・ice)と場面の砂 silica に出る**(値はサンプルの中身なので書いていない)。試験: reaction_package に TestMissingLightWarning、標準の「坂以外の警告が無い」は光の警告も除く。
- **マージ**(`--no-ff`・衝突なし): t-0918-c0-compact(0d5161c。部品の作業場と気体の箱の面を詰める。M-1 の数え方で 1775〜1787 → 883〜902 B/セル・起きた所の上限 約 50 万 → 約 100 万セル〔VRAM〕・
  128³ で 16.4〜19.4 ns/セル。今の時間の線 10 ms では 53〜61 万セルで時間が先に効く)・t-0957-rr0-fast(b35e1b1。64³ で斜め 2.61 → 0.59 ms・真上 0.93 → 0.22 ms。ビット一致は前と同じ)。
- **確かめ(release・警告 0)**: T-0942 の後に reaction_package・standard_packages・environment_package・luau_type_check と光の警告に触れうる 7 本(probe_phase・sun_column・world_layout・coupling_table・cell_view・multires_edge・blank_world)。
  マージの後に CPU 5 本(reaction_package・standard_packages・sun_column・gas_projection・equilibrium〔約 10 分〕)・GPU 5 本(gpu_gas・gpu_gas_warp・gpu_probe_gas・gpu_gas_projection・gpu_gas_projection_t0915)・GPU 3 本(gpu_sun_column・gpu_sun_column_warp・gpu_probe_sim)。全部通った(本数はログの N で数えた)。archmap --check OK。
- **記録**: ROADMAP(T-0918 = 1 済み・2〜4 は T-0953〜T-0955 の行・T-0919 作業中・T-0957 Done・T-0877 の状態・T-0942 Done)・perf.md の M-0 の節に詰めた後の値・QUESTIONS に Q145・COMMANDER(次の番号 **D-616・ADR-0420・T-0975・Q146**)。

## 前の仕事(2026-10-21・本体の作業役・image_probe_slice の基準・t-0944-rb1-arch のマージ)
- **image_probe_slice の基準を置き換えた**: `tests/images/probe_slice.png` を今の main(81a6e50)の release の出力から `tools/image_compare/image_compare.py update` で作り直した。
  原因は ee2802e(t-0778。1 mg 未満の液体の端数の持ち越し)による**意図した変化**(前の節の二分)。司令塔の判断で案 (1)(基準の置き換え)。案 (2)(断面の色を量 0 で連続に)はしない(エディタの見せ方の決め事を試験の都合で変えない)。
  **注意**: probe_view.hlsl の SliceColor は量 0 の直上に色の段差(差 19)があるので、300 K すれすれのセルの mK の違いで画像の試験が揺れやすい。シミュの小さな変化で落ちたら、まず二分して意図した変化かを見る。
  確かめ: release で `^image_probe_(slice|volume)$`(capture を含む 4 本)が通った。
- **マージ**(`--no-ff`・衝突なし): t-0944-rb1-arch(628ffd3。段の斜めの棒〔ka/12・ADR-0415〕・`SupportLaw::FaceForces`〔表面の応力〕・support_law_test にアーチと洞窟・T-0944・T-0960)。
  **司令塔の判断**: `SupportOptions::stairBraces` の既定を true にした(取り消しやすい仮。ka/12 は合わせ込みの値なので T-0919 を入れた時に測り直す)。
  確かめ(release・警告 0): support_law(137 秒)= 梁 3 つ(張り出し 0.984/0.996・片持ち 0.982〜0.999・両端固定 0.952/0.983)・アーチ(棒あり 0.959/0.984)・洞窟(棒あり 0.959〜0.997)が ±5 % 合格・表面の応力 1.000/0.998。equilibrium(721 秒)が通った。
- **記録**: DECISIONS D-614(Q143: 硬さは 1 値で始め、軸にそろった 3 値は後から足せる欄)・D-615(Q144: 溝の真実は (a) セルの固い物の量・溝に沿って流れる要望は T-0937)・QUESTIONS から Q143・Q144 を消した・
  ADR-0405・ADR-0407 の該当の仮を外した・ADR-0415 に既定 true(司令塔の仮)・ROADMAP(R-B1 の行・T-0960 の行・T-0919〔斜めの棒を斜めの 2 節を結ぶ面として持つ・速さの測りを完了条件に〕・T-0937〔D-615 の要望〕・T-0938)・
  T-0919 のチケット・COMMANDER(次の番号 **D-616・ADR-0418・T-0969・Q145**)。

## 前の仕事(2026-10-21・本体の作業役・t-0915-c0-gpu・t-0910-rr0・t-0871-rb1・t-0926-r1-root のマージ)
- **マージ**(`--no-ff`): t-0915-c0-gpu(93673f2。GpuEquilibrium〔shaders/sim/equilibrium.hlsl・equilibrium_core.hlsli・engine/src/sim/gpu_equilibrium〕・陰の圧力の GPU を部品の上へ・既定を部品の道に・前の fixedCycles の枝を消した・gpu_gas_projection_t0915・ADR-0406)= 衝突なし →
  t-0910-rr0(6f6786d。sun_column・gpu_sun_column〔_warp〕・D-610 の光の既定を shaders/common/light.hlsli・luau_type_check の型の文の長さの上限を外した)= 衝突なし →
  t-0871-rb1(325b411。engine/src/sim/support_law・support_law_test・ADR-0407)= perf.md の行が衝突 → 両方残した →
  t-0926-r1-root(3cc9227。case_b_flow・物の粘り・case_b_r1・T-0868 5e6da13 を含む・ADR-0409)= tests/CMakeLists.txt の support_law と case_b_r1 が衝突 → 両方残した。
- **確かめ(release・警告 0・archmap --check OK・float_check 5 本)**: t-0915 の後に CPU 25 本(gas_projection・gas_projection_plates・equilibrium・case_b_*・field_storage・multires_* 15 本・surface_detail・probe_* 4 本)・
  GPU 20 本(gpu_gas_projection・gpu_gas_projection_t0915・gpu_probe_gas・gpu_probe_sim・gpu_world_copy・gpu_gas〔_flow/_blast/_species/_tree〕と各 _warp)が通った。
  4 つのマージの後に CPU 11 本(sun_column・support_law・equilibrium・luau_type_check・reaction_package〔_scene〕・radiation_reference・standard_packages・environment_package・blank_world・world_layout)・
  GPU gpu_sun_column・gpu_sun_column_warp・case_b_box・case_b_gauge・case_b_r1(1 本ずつ。case_b_r1 254 秒)が通った。
- **image_probe_slice が落ちた(原因が分かった・2026-10-21 基準を置き換えた〔原因 ee2802e・意図した変化・断面の 0 の直上に色の段差があるので 300 K すれすれのセルで揺れやすい〕)**: ずれた画素 1045 / 921600(0.1134 %。許容 0.1 %・色の差の最大 19・範囲 (338,36)-(974,510))。
  **原因は ee2802e(Merge t-0778: 1 mg 未満の液体の端数を面の欄で持ち越す。D-522 案 b・ADR-0357 Proposed)**。本体で手で二分(release・`^image_probe_slice$`):
  7f5d11e 通る・06722c2(t-0819)通る・**ee2802e 落ちる(1045)**・51f6a9b 落ちる・d35b5c4 落ちる(どれも同じ 1045 画素)。06722c2..ee2802e の間は t-0778 の 2 コミット(d460ec3・7ec1034)だけ。image_probe_volume は main でも通る。
  **意図した変化(不具合ではない)と判断**: t-0778 は液体の流れ方を変えた(1 mg までの面の流れを持ち越して順番の刻みにまとめて動かす)ので、燃える木箱の世界の 4 秒後の温度がわずかに変わる。描画・シェーダー・物質の値は触っていない。
  ずれ方: ずれた画素はすべて断面の「活性なブロック」の上の数セル(1 セル約 11×13 画素 × 約 7 セル)。色は 2 つだけで、(10,48,61)〔温度が 300 K をごくわずかに超える = HeatColor の小さな赤〕と
  (19,59,80)〔300 K 以下 = COLD_SLICE_COLOR〕が入れ替わった(前は小さな赤 → 今は冷たい色 814 画素・逆 231 画素)。色の差 8 以下の小さな違いが 33951 画素(赤の 22〜26 の揺れ = 温い所の温度のわずかな差)。
  **根(見せ方の段差)**: probe_view.hlsl の SliceColor は `amount > 0 ? HeatColor(amount) : COLD_SLICE_COLOR` で、量 0 とごくわずかの間に色の段差(差 19)がある。だから 300 K すれすれのセルの mK の違いで画像の試験が落ちる。
  案: (1) 基準を置き換える(`python3 tools/image_compare/image_compare.py update out/build/release/tests/images/probe_slice.bmp tests/images/probe_slice.png` 相当。変わるのは上の数セルの色だけ)/
  (2) (1) に加えて、断面の色を量 0 で連続にする(HeatColor(0) を COLD_SLICE_COLOR に合わせるなど。両方の画像の基準が変わる)と、シミュの小さな変化で画像の試験が落ちにくくなる。おすすめは (1)。(2) は見せ方の決め事なので司令塔・ユーザーが決める。
  → **2026-10-21 置き換えた(上の節)**。二分の後、本体は main(d35b5c4)に戻して release でビルドし直した。
- **-Filter の「|」**: 崩れていなかった。main・wt2 の `^(a|b.*|c)$` 形の 10 本のログを `ctest -N` の今の一覧と突き合わせ、流れた本数がすべて一致(崩れて見えたのは runner/out/main_tests.txt が 10-15 の古い一覧だったため)。
  test.ps1 は直していない。worker-common.md の書き方を直した。前の巡目の確かめの流し直しは要らない。
- **記録**: ROADMAP(T-0915 Done・T-0939 の行・T-0918 作業中・T-0876 R-R0 Done〔費用 2.09〜2.61 ms > 仮の線 1 ms → T-0957〕・T-0877 作業中・T-0871 梁 3 つ合格・T-0919 に R-B1 で要る・T-0868 ほぼ合格・T-0869 R2 作業中)・
  QUESTIONS Q143 に材料(格子の梁なら軸にそろった 3 値はそのまま入る・おすすめ 1 値で始めて 3 値は後から足せる欄)・COMMANDER(次の番号 **D-614・ADR-0417・T-0966・Q145**)。

## 前の仕事(2026-10-21・本体の作業役・t-0879-rt1 のマージ)
- **マージ**(`--no-ff`・衝突なし。perf.md は自動で合わさった): t-0879-rt1(671b86a。shaders/common/surface_detail.hlsli〔整数の関数〕・shaders/render/surface_detail_float.hlsli〔小数の版〕・shaders/sim/surface_detail.hlsl・engine/src/sim/surface_detail〔差分の候補 A 格子・B 一覧〕・tests/surface_detail_test・gpu_surface_detail_test〔+ _warp〕・ADR-0405 Proposed・docs/perf.md・map.yaml)。
- **確かめ**: release のビルド警告 0・CPU 20 本が通った(surface_detail 24 秒・probe_liquid・field_storage・multires_* 13 本・equilibrium・case_b_box・case_b_gauge。約 19 分)・GPU 4 本が通った(gpu_surface_detail・gpu_surface_detail_warp・gpu_probe_liquid・gpu_world_copy)・archmap --check OK・float_check(sim・common・engine/src/sim)OK。直したものなし。
- **記録**: ROADMAP(T-0879 を「1 本目合格(23 §10 の 5 つ)」・T-0936 物の物理の接触から地面の当たりの関数を呼ぶ口・T-0937 21 §2.7 の塞ぐ面の欄・T-0938 溝を掘った時の真実/彫った形の重なり/掘った土の行き先/セーブの差分の行・R1 と R-T2 を作業中に)・
  QUESTIONS **Q144【エンジン】溝を掘った時の真実を (a) セルの固い物の量 か (b) 形の差分 か(仮で (a))**・COMMANDER(wt3 の T-0947〜T-0949・ADR-0409、wt4 の T-0856 + T-0950〜T-0952・ADR-0410、次の番号 **D-614・ADR-0411・T-0953・Q145**)。
- **司令塔へ**: 肌理の値(石・土・木の振れ幅・割れ目)の既定はゲームの中身なので claude/19 へ回す(T-0879「問い」)。差分の持ち方は候補 B(彫った形の一覧。差分が約 1/120)を仮のおすすめ(ADR-0405)。

## 前の仕事(2026-10-21・本体の作業役・t-0921-sparse のマージ)
- **マージ**(`--no-ff`・衝突なし): t-0921-sparse(488bd88。`MultiresField::store`〔MultiresFieldStore〕・`MultiresNest::sparseFields`〔既定は疎〕・眠る判定で全部 0 の頁を返す・tests/multires_sparse_test・ADR-0403・perf.md の 1 セルのバイト)。
- **確かめ**: release のビルド警告 0・CPU 35 本が通った(multires_sparse 60 秒・equilibrium 562 秒・field_storage・multires_*・gas_*・probe_*・case_b_*ほか。約 40 分)・GPU 15 本が通った(gpu_world_copy・gpu_probe_rewind・gpu_probe_rewind_regions・gpu_probe_gas・gpu_probe_liquid・gpu_probe_sim・gpu_multires_conduction・gpu_multires_implicit_conduction・gpu_multires_wide_* 7 本。約 12 分)・archmap --check OK・float_check OK。
- **記録**: ROADMAP(T-0921 Done・T-0930 GPU の疎な欄の頁〔T-0920 の中〕・T-0931 セルの真実の量を疎に・T-0932 セルごとの疎〔研究〕の行)・CLAUDE.md §4 に「欄の値は `LoadNestField` か `FieldPageValues(store, page)` で読む」・
  perf.md の M-0 の節に要点(セルの状態 112 B が一番大きい・静かな空気の欄は疎で 40 → 8 B)・COMMANDER(wt5 の T-0871 + T-0944〜T-0946・ADR-0407〜0408、次の番号 **D-614・ADR-0409・T-0947・Q144**)。
- **司令塔へ**: C-0 の GPU 版(wt2)のマージで `field.values` を直接読む所があれば `LoadNestField` か `FieldPageValues(field.store, page)` に(T-0921「司令塔へ」)。返したセルの頁の欄は 0 になる(前は古い値が要約に残っていた)。

## 前の仕事(2026-10-21・本体の作業役・t-0853-c0 と t-0900-m1 のマージ)
- **マージ**(`--no-ff`): t-0853-c0(5b3a546。部品 engine/src/sim/equilibrium・shaders/common/equilibrium.hlsli・陰の圧力を部品の上へ・tests/gas_projection_scenes.h・equilibrium_test・20 §13.1・ADR-0397 Proposed)→ t-0900-m1(1bead7c。docs/perf.md・T-0900・tests/gpu_test_memory.h)。
  衝突は t-0853-c0 の 2 か所だけ(map.yaml の案 B の箱の辺と C-0 の部品の辺・tests/CMakeLists.txt の case_b_box と equilibrium)= 両方残した。t-0900-m1 は自動で合わさった。
- **確かめ**: release のビルド警告 0・CPU 34 本が通った(equilibrium 521 秒・gas_projection・case_b_*・field_storage・multires_*・gas_*・probe_*ほか。約 38 分)・GPU 11 本が通った(gpu_gas_projection・gpu_gas・gpu_gas_flow・gpu_probe_gas・gpu_probe_liquid・gpu_probe_sim・gpu_probe_rewind*・gpu_world_copy・gpu_multires_conduction・gpu_multires_implicit_conduction)・archmap --check OK・float_check(sim・common)OK。
- **記録**: ROADMAP(T-0853 を CPU 版済み + 残り T-0915〜T-0919 の行・T-0842〜T-0845 を Done・T-0851 の注・R-B1 に「C-0 の上で始められる」)・QUESTIONS **Q143【エンジン】支えの値を 1 つの硬さか向きごとの硬さか(R-B1 の結果待ち。T-0919)**・
  COMMANDER(wt2 の T-0939〜T-0941・ADR-0406・ADR-0398 は未使用・次の番号 **D-608・ADR-0407・T-0942・Q144**)。
- **司令塔へ**: C-0 の部品の道は今は既定で切ってある(`equilibrium = false`)。T-0915 の GPU の比べが通ったら既定にして前の道(Project の fixedCycles の枝)を消す(T-0853 の仮)。
  B0 の陰の圧力は `PressureSolver` を返す関数を 1 つ足して部品へ差し替えられる(T-0866 の注)。T-0922(C-1 の温めた始まりの付け替え)は始められる。t-0842・t-0843・t-0845 のブランチは中身が main に入った(t-0844・t-0846・t-0847 は入っていない = 下の表のまま)。

## 前の仕事(2026-10-21・本体の作業役・t-0866-b0b1 と t-0854-c1 のマージ)
- **マージ**(`--no-ff`・衝突なし): t-0866-b0b1(d5b23ec。engine/src/sim/case_b/〔ライブラリ bicameral_case_b〕・tests/case_b_box_test〔ctest case_b_box・case_b_gauge〕・21 §5.4・map.yaml のノード 2 つ)→
  t-0854-c1(87da3ee。state_field.hlsli の印 SF_FLAG_DERIVED・SF_FLAG_WARM_START・nest_fields の眠る/起こすの分岐・multires_state の保存点は SfSaved の欄だけ・HashTruthLeaves・tests/field_storage_test・ADR-0399)。
- **確かめ**: release のビルド警告 0・CPU 33 本(case_b_box・case_b_gauge・field_storage・multires_*・gas_*・probe_*・cell_view・cell_volume・transport_apply・rest_return・各パッケージ)が通った(約 29 分)・GPU 8 本(gpu_world_copy・gpu_probe_rewind・gpu_probe_rewind_regions・gpu_probe_gas・gpu_probe_liquid・gpu_probe_sim・gpu_multires_conduction・gpu_multires_implicit_conduction)が通った(state_field.hlsli を GPU と共有しているため)・archmap --check OK。直したものなし
- **M-0・M-1 の結果(wt4 t-0900-m1・T-0900。RTX 3070 Ti)**: M-1 = 陰の圧力 18〜22 ns/セル(128³)・VRAM 1.78 KB/セル。液体の見込み: 柔らかい水 V 2 で 18〜23 ns・本物の水 V 4 で 28〜30 ns。
  どれも陽解法の気体の 1/8 以下で、費用は R1 の判定を縛らない。M-0 = 真実だけの 1 セルは 24+12k B(空気・土で 48 B)・20 km の真実は 0.4〜1.5 GiB(地表 4 層か半径 512 m なら 2.7〜9 GiB)+ 起きた所の作業場 0.15〜0.85 GiB・
  10 km の空気の柱の整数の精度は足りる。ROADMAP の T-0851・T-0852 を Done に。**t-0900-m1 は t-0845 の上なのでマージしない(C-0 の後)**。
- **記録**: ROADMAP(T-0866・T-0867 Done・T-0854 は 1 チャット目済み・T-0920〜T-0922 の行・T-0868 に作業中)・QUESTIONS **Q142【エンジン】粒の密度は粒そのもの(砂 2650)・積もった密度は詰まり方から導く(仮)**・
  Q137 に「D-549 (A) が C-1 で実装された・取り消しは SF_WARM_START_IN_STATE の 1 行」の注・COMMANDER に第 2 の配りと次の番号 **D-608・ADR-0406・T-0939・Q143**。
- **司令塔へ(マージ後に直すもの・各チケットから)**: Q142 が仮のままなら 21 §1.2 の例 1・4 を粒そのものの値に直す(反映の作業役)。C-0 の作業役へ: 温めた始まりの欄は `SfMakeDerivedDesc(SF_PLACE_CELL, SF_TYPE_I64, phase, true, 1)` で登録すれば (A) の扱い(T-0854「司令塔へ」)。
  B0 の陰の圧力は C-0 ができたら `PressureSolver` を返す関数を 1 つ足して差し替える(箱・段は変えない)。

## 前の仕事(2026-10-21・本体の作業役・反映の確かめの直し。文書だけ)
- 反映の確かめ(3 体・Projects で claude/ の最新版と突き合わせ)で直した 8 件: 20 §9.2・§13.1 の記録を研究の合格の項目に(S38)/ 14 の重さの検査に衝撃の区の 1 行 / 05 §3・D-583 の魔素を「普通の物質の形」だけにし、重さ・染み通り・吸い付き・湧き出しは一般の能力に(D-006)/ M1 が落ちた時は案 ④ → 案 A・案 A の前にユーザーに上げる(21 §5.1・D-556・ADR-0376)/ D-555 の B-d を一旦の決定に。
- 速い連鎖(Δx/Δt を超える割れ・爆轟)は (a) 固定の小刻み か (b) この宇宙の速さとして遅く の未決に(22 §3.3・20 §2.7・D-546・ADR-0373 §7・**Q141**)/ 粗くしても同じ結果かは賭け 2 で測る・通らない所は粗くせず費用か広さで払う(D-544・20 §11.1・ADR-0373 §5)。
- DECISIONS の古い行の注: D-471(⑥ の下見の前倒し)・D-509(押し返し方の値への読み替え)・D-551(D-532 の参照に)・D-312 と 07 §3(D-544 で置き換え)。archmap --check OK。次の番号は D-608・ADR-0401・T-0925・Q142。

## 前の仕事(2026-10-21・本体の作業役・reflect-a5 のマージ)
- **マージ** reflect-a5(232c365。11-audio・ADR-0388〜0389・D-590〜D-596・T-0885)を `--no-ff` で。衝突なし。DECISIONS の D-590〜D-596 が D-607 の後ろに入ったので、番号順(D-588 と D-600 の間)に並べ直した。
- **渡し物を当てた**(runner/handoff-to-a5.md・T-0885 の「持ち主へ」): 20 §14.6(法則の音 D-591・匂い D-595)・§8.1 の「音は A5」→「音は 11」/ 09 §3 知覚を置き換え・§7 に読む口の 1 行・冒頭の決定に D-593・ADR-0389 /
  05 §5 に「地域の印」(D-594。仮)/ 06 §3 のイベントに音の出来事と続く音のまとめ・スナップショットに体の状態 / 11 §2.6(他の文書から来る要求: 割れ・破れの出来事・登録制の口・帳簿の列・刻みの長さの境)・§3.1 の後に雷の音の強さ(D-588)・R-A1 の見出しと §7 の地域の印。
- **DECISIONS の注**: D-452「音の項は D-590 で置き換え」。D-534 ③「物理の音(2 周目)」の読み直し(物理から合成する研究は D-590 で無くなった・体験版にも鳴らす音は要る・2 周目は法則の音の広がり R-A1)= **仮・QUESTIONS Q140【エンジン】**。
- **ROADMAP**: 列 0 の T-0885 を Done(M9 の注つき)・列 2〜6 の表に T-0886(R-A1。2 周目)の行・2 周目の行に R-S1(11 §5.1・D-596)と R-A1 の注・作業中の 5 本に「作業中(ブランチ名)」。
- **COMMANDER の番号の表**: 試作の巡目の配り(wt2〜wt6 の T・ADR の帯)を足し、次の番号を **D-608・ADR-0401・T-0925・Q141** に直した(前の記録の ADR-0393・T-0895 は配った帯と重なる)。
- **片付け**: `.git/stale-index-lock-a2merge` と `runner/_to_delete/main-merge-a2-stray/` は消す権限が無いので残した。**ユーザーが手で消してよい**(どちらも前の作業役の残骸。git は使っていない)。

## 前の仕事(2026-10-21・本体の作業役・反映のマージ a1〜a4・a8)
- **マージ**(`--no-ff`。衝突は DECISIONS の表の末尾だけ = 両方残して番号順): reflect-a2(abd336a)→ reflect-a3(36be86c)→ reflect-a4(d350520)→ **reflect-a8(dc20c70。A6 を新しい main に載せ直した 7a2d385 を含む。reflect-a6 353686f は入れていない)** → reflect-a1(d1a8a7b)。
  c8e120b(t-0820 の WIP)は reflect-a6・reflect-a8 のどちらにも入っていないことを確かめた。
- **渡し物を当てた**(各チケットの「持ち主へ渡すもの」): 20 §14.1〜§14.5 と §1.1・§1.2・§2.3・§2.4・§4・§7.2・§10.1・§10.4 / 02 §9.4(燃え始める強さ)・§9.5(割れ・破れ・電気分解・電池)/ 08 §11(器の口・表面の形・電線・刻む・木・止まった棚・操作の力)/
  21 §2.5・§2.8・§2.14 / 22 §6 の後 / 23 §10.1 / 24 §7 / 05 §3・§5 / 09 §0.1 / 15 §1(温めた始まり)/ 10 §0・§1 / 07 §1・§1.3・§4・研究 / 06 §1・§2・研究 / 17 §3 / 14 写しの早送り / 18 §3(刻む = D-582 で確定)。
- **01-architecture**: 設計書の一覧に 21〜24・6 つの家族の節・1 刻みの順を 20 §2.5 に・Work Graphs の節を D-546 に。**CLAUDE.md** §0・原則 5 を D-546(Compute 既定)に・原則 9 に D-551。
- **古い決定への注**(DECISIONS の行の日付の前に「〔2026-10-21 本体の注〕」): D-302・D-404・D-443・D-444・D-468・D-483・D-501・D-509・D-511・D-515・D-519・D-520・D-525・D-528。D-534 ⑥ と A4(D-570・D-576)は食い違いなし(注と 10 §0 に 1 行)。
  ADR の Status に一言: 0002・0252・0262・0263・0326・0359・0360。T-0028 は「止めた・続きは列 4」。
- **ROADMAP**: 列 0 を Done に・列 1a/1b の合格を 20 §13 に・T-0855 を T-0866/T-0867 に分けた・列 2〜6 の表(T-0866〜T-0869・T-0871〜T-0874・T-0859・T-0876〜T-0879・T-0861・T-0881・T-0856〔T-0891 をまとめた〕・T-0862・T-0884・T-0893・T-0894)・2 周目に T-0882・T-0883・⑦ の電気と金属に D-580/D-581・T-0803 に D-588。
- **QUESTIONS** Q137(D-549 温めた始まり)・Q138(割れ方の旗を作らない)・Q139(光の値を書かない物質)。**COMMANDER** に使った番号と次の番号(D-608・ADR-0393・T-0895・Q140)。
- runner/handoff-to-a1.md(20 行きは全部当てた記録。残りは §14.6 だけ)・runner/handoff-to-a5.md(11 行きと、a5 のマージ後に本体がすること)。handoff-to-a8.md は作っていない(05・09・15 は a8 で書き終わり、行き先は main で当てた)。

## 前の仕事(2026-10-21 前半・本体の作業役・T-0850)
- 運用の決定(CLAUDE.md 原則 9・10・§5・COMMANDER)・D-532〜D-539・ADR-0370・ROADMAP「★ 試作の列」の骨・main の健康(gpu_gas の 11 本が通った)。詳しくは 2954618 の HANDOFF.md と docs/tickets/T-0850。

## ブランチの棚卸し(N10b・D-537。2026-10-21。main の側から見た。消すのは司令塔とユーザーが決める・消さない)
main に入っていないブランチは 21 本(ahead = main に無いコミット数 / behind)。reflect-a1〜a4・a8 は 2026-10-21 に main へ入れた(reflect-a6 は a8 に載せ直した版が入ったので不要)。reflect-a5 も 2026-10-21 に main へ入れた。
| ブランチ | 先頭 | ahead/behind | 中身 | 扱い(案) |
|---|---|---|---|---|
| t-0751 | 0e1572f | 2/3 | T-0751 混ざる(ADR-0367・D-530)。Done・試験済み | **入れる**(棚卸しのマージ T-0858。案に依らない)→ 列 7 |
| t-0813 | b8898ab | 3/2 | T-0813 気体の木に…(release・gpu_gas・gpu_probe_gas 済み) | **入れる**(T-0858)。前の記録「案 B の判定までマージしない」は N10b で上書き |
| t-0820 | (wt6 が 10/21 に WIP 保存) | 5/3 | T-0820 占める層(物の占める体積を見立ての入力に。ADR-0360) | **仕上げて入れる**(T-0858。claude/28 §3)。物と場の口 = 列 3 / 案 B の前提 |
| t-0838 | 215090c | 2/0 | 広さの見積もり S1 の測り(perf.md 4 行・チケット・tests の --scale・gpu_test_memory.h) | **結果を記録して閉じる**(main へ入れる案: 測りの道具が M-0 で再び使える)→ 列 1a M-0 |
| t-0840 | 0e5a90f | 2/0 | 描画が乗った時の取り分(--render-load・perf.md の表・チケット。engine/src/render/render_load) | **結果を記録して閉じる**(main へ入れる案: --render-load は M-0・Q11 で使う)→ 列 1a M-0 |
| t-0842 | 0327f9c | **2026-10-21 C-0 の中で main へ入った** · 1/0 | 本物の音速の陰解法の賭けを CPU で(研究) | 結果を記録して閉じる(圧力の解き方 = Q1 の根拠)→ C-0 の入力 |
| t-0843 | 80f1804 | **2026-10-21 C-0 の中で main へ入った** · 3/0 | 本物の音速の投影の GPU 1 刻みの測り | 同上 → C-0・M-1 の入力 |
| t-0844 | 1cad3ec | 2/0 | 陰解法と陽解法の 3 つの違いの根(研究。Q1b の押し出す仕事の直し) | 同上 → C-0(Q1b の口) |
| t-0845 | c707f36 | **2026-10-21 C-0 の中で main へ入った** · 5/0 | 陰の圧力の通しの計測(32³ 32〜34・64³ 20〜23・128³ 18〜22 ns/セル。CPU と毎刻みビット一致) | **C-0 の最初の使い手の元**(T-0853 で移す)・M-1 の入力。マージするかは C-0 の設計で |
| t-0846 | 404a771 | 3/0 | 陰解法で 10〜20 m の波を保つ形(研究。θ を状態で・TR-BDF2) | 結果を記録して閉じる(Q1c。波は消えたまま進める)→ 2 周目で再開の元 |
| t-0847 | 357ee56 | 4/0 | 陰解法(CN)の育ちの根(研究) | 同上 |
| t-0807 | c3e46dd | 4/22 | T-0807 MR の気体と液体を活性のブロックだけで(途中) | **案 B の判定まで保留**(列 7。C-1 欄の持ち方と重なる) |
| t-0834 | 557528b | 3/3 | T-0834 端数の共通の決まり(券だけ。D-528 保留) | **案 B の判定まで保留**(列 7) |
| t-0784 | ad1a2c3 | 2/2 | GPU の陰解法の世界の端(途中・未ビルド) | **実装の段へ**(段 4。C-0 で作り直す見込み → 捨てる候補) |
| t-0725 | 932110f | 3/38 | T-0725 縦切りのチケットだけ | 列 7(縦切りは G2 の直後)。チケットだけ main へ写す案 |
| int-11 | d1d21da | 2/39 | 第 10 巡の統合の残り | 捨てる候補(中身は main に入った見込み・未確認) |
| t-0593 / t-0616 | ccb129b / fe25250 | 2/214・1/180 | 古い統合前の版(main に載せ直し済み) | 捨てる候補 |
| t-0668 / t-0686 / t-0689 | — | 1/132〜138 | 10/17 に捨てたブランチ(マージしない) | 捨てる候補(10/17 の決定どおり) |
- 測り(claude/26 決定 1)の要点 — **T-0838**(RTX 3070 Ti・release): MR の伝導は 256³ まで線形で 0.6〜0.8 ns/セル・約 130 B/セル(256³ 全部で約 10 ms・約 2 GiB)。活性の形に「世界の枠 1 個あたり約 19 ns」(256³ で約 0.6 ms)の
  世界の大きさに比例する費用がある(どの段かは未確認)。**陽解法の気体は約 285 ns/セル・約 1.3 KB/セル**(16.7 ms に収まるのは全部動く気体で約 5.8 万セル)。液体 約 4.5 ns・約 0.7 KB/セル。陰の熱 6〜11 ns・約 400 B/セル(256³ は VRAM で落ちる)。
  **T-0840**: 30 fps・描画 12.4 ms ならシミュは約 20.8 ms/フレーム。等速(60 刻み/秒)には 1 刻み ≤ 約 10 ms(今の草原は 78.8 ms)。D-209 の描画 12 ms を最低機の値とすると、1 刻みは 3070 Ti の約 6 ms 分が上限の見込み。→ ROADMAP の M-0(T-0851)の入力に書いた。

## claude/19(非公開の控え)の確かめ(2026-10-21)
- 照らした決定: claude/29 Q2・Q3b・Q3c・Q6 / claude/30 N3・N5・N9 / claude/31 S1〜S5 / claude/32 S27・S28 / claude/33 S31 の値 / claude/36 S46 / M6・M7 未決。
- 10/21 午前の設計チャットが書き戻した claude/19 には、S1〜S5(遊びの柱とセーブ・魔法の働き・作戦 8 本・CO)・S27・S28(料理 EQ)・S31(魔素の質量と溜まり方)・S46(持ち物)・N5(刻む)・M6・M7(未決)は入っている。
- **抜けている見込み**(設計チャットの記録の上では見当たらない): Q3b(大きいことの費用 = 体の蓄え + 世界の資源。気候・時刻は「使えると得」まで)・Q3c(事故は区切り・前触れ・焼け跡の回復で受ける)・
  Q6(ゲームに先を見る箱は置かない。小さく試す場は安い小屋・器)・N3(下り坂の連鎖が安いのは許す。レベルデザインで解く)・N9(魔石・蓄えは貯め込んでよい。大魔法の後は周りが薄くなり流れで戻る)・Q2 の作戦の一覧の検査の列。
  **この作業役には Projects ツールが無く、claude/19 を読み直して書き戻せなかった。** 司令塔が Projects で claude/19 を読み、上の 6 つが本当に無ければ「§魔法の遊び方」「§作戦」に足して書き戻す(T-0850 のチケットに足す文の案)。

## 前の仕事(2026-10-19 第 11 巡の続き 2・止めた状態)
- マージ後の確かめ(release 警告 0・CPU 30 本・GPU 32 本)・D-527〜D-531 の記録・止めたブランチ。詳しい節は edf3cbe の HANDOFF.md。
- t-0751 のマージ後に流す組(残しておく): multires_diffusion・^multires・cell_view・transport_apply・environment_package・reaction_package・archmap --check・float_check → ROADMAP の T-0751 を Done・ADR-0367 を Accepted(D-530)。
  → GPU(4〜8 本ずつ): window_hot_reload_*・window_gravity_reload_*・window_play_table_*・window_play_record/play・window_rebuild_*・window_blank_*・window_mixer_*・gpu_world_copy・gpu_probe_rewind(_regions)・gpu_probe_gas(_warp)・gpu_probe_liquid(_warp)・gpu_probe_sim(_warp)・gpu_probe_sim_tables(_warp)・gpu_gas$・gpu_gas_flow・_blast・_species・_tree・_projection(warp も)

## 前の仕事(2026-10-19 以前)
- 2026-10-19 第 11 巡(D-521〜D-526・t-0766・t-0790・t-0796・t-0788・t-0761 のマージ)は edf3cbe の HANDOFF.md。2026-10-18 は ed3d6f7、2026-10-17 は 59ef841・d2a10a2・d105aa6、第 9 巡の前半は 80fab2e、第 8 巡は 1aada77、
  第 7 巡は c8c4a52、第 5・6 巡は a90acdd、第 4 巡は 68a3b07、第 3 巡は c0d8c42、第 1・2 巡は f778b4a、2026-10-13 は 2960562 の HANDOFF.md(400 行に収めるため消した)。

## 動いているもの(確認方法つき)
- **テスト(2026-10-17 ③' 段 0〜4 のマージの後・release)**: 上の「状態」のとおり(試験の組は runner/main-batches.txt〔git 管理外〕の形で 4〜9 本ずつ。CPU 3 組・GPU 15 組〔^gpu_probe_l を足した〕・窓 9 組・image 1 組。-Filter は「|」を使わず文字の組で)。燃える木箱の長い測りは `slope_check_test --burn` を raw で切り離して(Start-Process)流すとランナーを塞がない。
- それより前のテストの記録(どの組をどの順で流したか)は前の版の HANDOFF.md(2026-10-12 は 2960562 の前・2026-10-11 以前は aa3e55c)と各チケットの「テストの結果」にある。
- `job.py test` は分けて投げる(GPU の組は 4〜8 本ずつ。T-0426 で長い試験を分けた)。例: `-Filter "^gpu_multires(_warp|_activity|_activity_warp|_quiet|_quiet_warp|_uniform|_uniform_warp)?$"`・
  `-Filter ^gpu_multires_conduction`・`-Filter gpu_multires_subcycle`・`-Filter "gpu_probe|window_replay"`・CPU だけの組
  (`-Filter "^(smoke|singleton|log|sim_scheduler|debug_camera|replay_file|multires|fixed|physics|float_check)"`)と `-Filter reaction`。
  裏で投げて runner/logs/<job>.result.json を待つ(cat しない。job.py の要約で見る)。テストの表示した行は out/build/<preset>/Testing/Temporary/LastTest.log(走っている間は .tmp)。
- `python3 tools/archmap/archmap.py --check` は Linux 側で動く。
- **確かめの組の決まり(2026-10-26)**: 帳簿・VisitNestState・MrBlock に触るマージは multires_sparse も流す(区画の要約の基準が変わる。T-1209)。

## 壊れている/未確認のもの(ファイル:行 と症状)
- **wt6 t-1164-outside-default はマージしていない**: window_replay_play・_rewind・_peek が S(33) で回ごとにハッシュが変わる・gpu_play_metric 41 → 203 秒。wt6 の次の作業役が main の実行ファイルで window_replay_record → play を流して main でも落ちるか確かめる(main で落ちたらここに書く)。
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
- 2026-10-21 反映の巡目(本体): ユーザーの決定(10/19〜21 の設計チャット)を D-532〜D-539・ADR-0370 に。本体の作業役が決めた細部(仮): ROADMAP の試作の行の番号(T-0851〜T-0858)・
  ブランチの扱いの案(上の表。測りのブランチ t-0838・t-0840 は道具ごと main へ入れる案)・体験版の要る現象 # の印(作戦 8 本の行から引いた)。全部 docs/tickets/T-0850-reflect-commander.md。
- 2026-10-19 以前の細部は edf3cbe の HANDOFF.md のこの節。

## 次にやること
NEXT.md の先頭(**反映の巡目の残り**: reflect-a5 のマージ〔runner/handoff-to-a5.md〕→ T-0858 棚卸しのマージ → **M-0・M-1(測り)→ C-0・C-1・B0・B1 → 列 2〜6**。ROADMAP「★ 試作の列」)。
問いは【エンジン】/【ゲーム】で振り分け、エンジンの作りが変わらない問いは聞かない(D-539)。判断待ちは QUESTIONS と各 reflect のチケット。

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
