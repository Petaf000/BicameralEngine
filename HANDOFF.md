# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-10-14 夜(司令塔のチャット)/ ユーザーの答え(Q35・Q37・Q41・Q42・Q46〜Q60)を D-487〜D-493 として反映。コードは変えていない

## 状態(3 行以内)
- main = 8d4e356(第 1 巡のマージ efd83fa の後に release の CPU 50・GPU 9・窓と画像 31 本が全部通過 + 第 2 巡の t-0373・t-0269・t-0426b)。
- ② 中身の器は T-0453(ミキサーの重力・空気を今の世界へ)・T-0443/T-0444 を残して済んだ。③ は案の下書き(docs/design/19)があり、ユーザーの答え(Q52)待ち。
- **判断待ち Q46〜Q52**(どれも仮で進めてある)。**「続き」は司令塔として並走(COMMANDER.md)**。GPU の順番待ちが 1 回 30〜90 分になったので、GPU を使う作業役は 3 体までにする。

## この仕事でしたこと(2026-10-13 の 3 回目・司令塔)
- 反映: 設計チャットの追加の決定 D-485(中身を書く手間を減らす道具)・D-486(⑤ の場面はエディタで作る)(222845b)。開始の手順に preferences と COMMANDER.md を足した(b2b0d16。読み落として 1 本ずつ進めたため)。
- T-0436(本体): 単体の無い元素の原子は捨てて報告(D-484)。
- 第 1 巡(マージ済み): T-0398 世界の組み立て → 分類 world_cells・world_bodies(ADR-0172)/ T-0399 材料のひな形・抽出の物質 → material_templates・view_quantities・editor_defaults(ADR-0174)/
  T-0400 環境の値 → 分類 environment(重力・音速の基準。`sim::WorldPhysics`。窓の WorldPhysicsOf は消えた。ADR-0176)/ T-0411 刻み 0 の保存点 / T-0426 試験の分け直し(前半)/ docs/design/19 坂の原理の案。
  新しい分類はどれも反応表と同じ読み込み・同じ表の版(T-0398 の共有の決め事。つなぎ目は reaction_table.h・reaction_package.cpp・reaction_table.cpp の「同じ読み込みに足した分類」の節に 1 行ずつ)。
- 第 2 巡(マージ済み): T-0373 標準パッケージ(data/standard の 5 つ)・`--mix`・分類 atmosphere(ADR-0182・ADR-0183)と T-0452 ミキサーのパネル / T-0269 音速の頭打ちを安定の上限から(473 K)/ T-0426 の後半。
- 第 2 巡(後からマージ): T-0427(遅いのは Compute のパイプラインのドライバのコンパイル。gpu_multires_test は GpuMultiresOptions::pipelineCache で使い回す。仕組みは T-0455)・T-0380(作業場の形の成分の位置を表で引く。861 / 306 ns/セル〔64 グループ〕。1 セル複数スレッドは T-0458)。
- 第 2 巡のマージの後の試験: main の GPU・窓 31 本(100304)・wt3 の多重解像度 5 本・wt4 の溢れの debug 6 本が通過。
- 第 3 巡(マージ済み): T-0453 ミキサーの「世界を作り直す」(RunFrameLoop が窓と GPU を残して次の FrameLoop を作る・--auto-rebuild・ADR-0192)/
  T-0444 環境の値のホットリロード(重力をルート定数で・表の差し替えの刻みで切り替え・sim::CheckWorldPhysics。ADR-0194)/ T-0396 転換の道の費用(--measure-convert。Q55)/
  T-0426 を閉じた(debug も 1 本 10 分以内。implicit_conduction を形 × 場面に・gpu_gas_blast を分けた・WARP は debug でも伝導の Work Graph 版を外した)。判断待ちは Q46〜Q55。
- 第 4 巡(マージ済み): T-0412 世界の写しの早送り(④ の最初。sim::WorldCopy・ADR-0198)/ T-0455 試験が遅かった原因 = VS の開発者環境が環境変数を化けさせてドライバのシェーダーキャッシュが外れていた(_vsenv.ps1 を直した。gpu_multires_wide_* が 1 本 50 s → 2〜8 s)/
  T-0470 気体の木の環境の差し替え(ADR-0200)・T-0479 分けた小刻みの木の偽の流れの直し(ADR-0201)。判断待ちは Q46〜Q57。
- 第 5 巡(マージ済み): T-0413 重さの検査(写しで刻みごとの GPU 時間と活性のブロック数を集め、予算・演出のスローの窓と比べる。本物の刻みが予算を超えたら警告。ADR-0204)/
  T-0432 遊びの指標の自動テスト(分類 play_metrics・表を変えたら写しで測って前の版との差。サンプルに sealed_fire_out。ADR-0206)。**④ 測る道具はこれで揃った**。判断待ちは Q46〜Q59。
- **2026-10-14 夜の反映**: ユーザーが判断待ちに全部答えた → D-487(③ は案 A・引きずりと坂を上らせる構造は一般の形を ③ で)・D-488〜D-492(仮の案はほぼそのまま。サンプルを標準パッケージの上に作り直す・大気は高さごと・エディタは UE と同じ形)・
  D-493(パッチを全部外す)。ADR 11 本を Accepted に(0095 は Superseded)。新しいチケット T-0491〜T-0499(ROADMAP)。QUESTIONS は Q30・Q31 だけ残した。
- **注意**: VS の Developer PowerShell(Enter-VsDevShell)から exe を起動すると、ドライバのキャッシュが外れて起動のたびに数十秒かかるはず(T-0455 から推定・未確認)。ランナーのジョブは直した。
- **試験の名前の変化**: gpu_gas → gpu_gas + gpu_gas_flow / gpu_probe_sim → + _tables / gpu_multires → + _stress・_limits / implicit_tree・subcycle・implicit_conduction を分けた(T-0426 のチケット「HANDOFF に写す」)・
  新しい world_layout・environment_package・standard_packages・window_place_tick0_*・window_blank_rewind・window_mixer_*・window_mix_start。

## 前の仕事(2026-10-13 の 2 回目)
- **反映**(ec9c049): 2026-10-13 の設計チャットの決定を DECISIONS へ。D-481(Q45: 生き物が魔素を操る = 押す + 解く。解くは反応表の一般の触媒。D-451 を改訂)・
  D-482(調整の負担を減らす道具: 影響の見える化・法則のノード表示・組み合わせの導出・遊びの指標の自動テスト)・D-483(魔法の遊び方〔一旦の決定。中身は非公開〕から出る
  エンジンへの要求: 坂を世界に描く・押す手応え・刻む模様は素材と形だけ・直前の操作の繰り返し)。02 §5・09 §0・18 §3・14 §2・10 §1・12 §3・15 §2・ROADMAP(④ に T-0432、
  ⑤ に T-0433〜T-0435)・QUESTIONS の Q45 を閉じた。プロジェクトの claude/22 に「反映済み ec9c049」と書いた。
- **T-0372 の続き(Done)**: 器の既定の重力を 0 に(`PxDefaultParameters().gravity`・`GasConfig::gravityMicrometersPerSecond2`)。世界の値は新しい
  `engine/src/sim/world_physics.h` の `WorldPhysics`(既定 0 = ブランク・`StandardWorldPhysics()` = 地球。T-0373 で標準パッケージへ移す)→ `PhysicsParametersFor`・`ApplyWorldPhysics`。
  窓は `frame_loop.cpp` の `WorldPhysicsOf`(ブランク → 0・サンプル → 標準)。標準の重力で書いた試験(物理・気体・仮の世界の物理・巻き戻し・physics_lab)は標準の値を明示。ADR-0171 追記 1・ADR-0151 の 5。
- **T-0423(Done)**: physics_test に `gravity`(無重力で浮いた物・積んだ物がビット単位で動かない / 1 s の落下が重力に比例〔月 ÷ 地球 = 0.16519〕・½gt² と 1.7% / 上限 100 m/s² の積み木が止まる)。
  gpu_physics_test に `--gravity`(ctest: gpu_physics_stack_zero_gravity・_pile_moon・_stack_max_gravity)。gas_reference に重力の節(箱の高さの圧力の差が重力に比例・どの重力でも静止大気が動かない・
  閉じた箱の保存則〔無重力では重さの仕事 0〕)。gpu_gas に 6 場面(closed_*_gravity・closed_moon・static_moon・static_max_gravity・bubble_zero_gravity)。前の仕事(① の反映)は 886c52c の HANDOFF.md。

## 2026-10-12 の 2 巡の並走で入ったもの(main aa3e55c まで)
- T-0374 気体の木の形の SRV を既定のヒープへ(32³ が約 30 倍速く)/ T-0384 一様な値を読む段が溢れを読む(完了)/ T-0380 大きい形のセルの場面を軽く(Doing・一部)/
  T-0377 転換の物質 ID の付け替え(Done)/ T-0378 実験室の転換の再生の試験(Done)/ T-0397 転換で HW の Work Graph が止まった件(Done。Table に RateRule)/
  T-0371 ブランクの世界を起動の既定に(サンプルは `--world sample`。Done)/ T-0372 重力を世界のデータに(R は器に残す案 A を仮に・ADR-0171 Proposed・T-0423 を分けた・Doing)/
  T-0410 アップロードのヒープの洗い出しと直し(Done。gpu_gas の直した後の値は次の --profile の時に perf.md へ)/
  T-0426 長い GPU 試験の ctest を分けた・ランナーの上限から GPU の待ちを除いた(Doing。残りは 10 分を超える試験の分け直し)/
  T-0427 試験の Work Graph の作成が遅い件(研究・Todo)/ T-0394 キューが作れない件(チケットのファイルだけ)。
- 3337c85(T-0397 の記録)の後のマージ: t-0410・t-0372・t-0377 の 2 回目・t-0426・t-0371(aa3e55c)。試験の名前が変わった分は T-0426 のチケット。
- **新しい Table 型には RateRule が要る**(T-0397)。**実験室・試験で ImmediateQueue を作っては捨てる形を新しく足さない**(ADR-0137 追記 3。T-0394)。
- GPU の順番待ち(D-460)は効いているが、1 本が長いと全部が待つ。GPU の組は 4〜8 本ずつに分け、上限は短めに(COMMANDER)。
- それより前の「本体で入ったもの / 統合で入ったもの / 並走で入ったもの」の節(2026-10-11〜12)は、400 行に収めるために消した。
  aa3e55c の HANDOFF.md と各チケットの作業ログ・引き継ぎメモにある。

## 動いているもの(確認方法つき)
- **テスト(2026-10-13 この仕事・release)**: physics_*(math・gravity・4 場面)と *_checked・gas_reference・gpu_physics の 7 本(新しい重力の 3 本を含む)・gpu_gas(新しい 6 場面を含む)・
  gpu_probe_physics・_compute・gpu_probe_rewind・窓と画像とブランクの組(`^(window_|image_|gpu_probe_blank)`)が通過。gas_tree はビルドだけ(既定の重力を明示しただけ)。
- **テスト(2026-10-12 その 3 のマージの後・release)**: CPU 52 本・陰解法 10 本・多重解像度/実験室/ブランク 20 本・窓と画像 28 本・溢れの大きい組 8 本が全部通過。
- 2026-10-11 以前の各チケットのテストの記録(どの組をどの順で流したか)は aa3e55c の HANDOFF.md と各チケットの「テストの結果」にある。
- `job.py test` は分けて投げる(GPU の組は 4〜8 本ずつ。T-0426 で長い試験を分けた)。例: `-Filter "^gpu_multires(_warp|_activity|_activity_warp|_quiet|_quiet_warp|_uniform|_uniform_warp)?$"`・
  `-Filter ^gpu_multires_conduction`・`-Filter gpu_multires_subcycle`・`-Filter "gpu_probe|window_replay"`・CPU だけの組
  (`-Filter "^(smoke|singleton|log|sim_scheduler|debug_camera|replay_file|multires|fixed|physics|float_check)"`)と `-Filter reaction`。
  裏で投げて runner/logs/<job>.result.json を待つ(cat しない。job.py の要約で見る)。テストの表示した行は out/build/<preset>/Testing/Temporary/LastTest.log(走っている間は .tmp)。
- `python3 tools/archmap/archmap.py --check` は Linux 側で動く。

## 壊れている/未確認のもの(ファイル:行 と症状)
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
- 2026-10-13(2 回目): ユーザーの 2026-10-13 の決定 D-481〜D-483 を反映。実装の細部として、世界の物理の値は sim::WorldPhysics(既定 0)にまとめ、
  標準の値は StandardWorldPhysics() に置いた(T-0373 で標準パッケージへ移す。ADR-0171 追記 1)。
- 2026-10-13(ユーザーの 2026-10-12 の決定を反映): D-461〜D-476(DECISIONS)。この仕事で新しく決めたことは無い(Q44・Q45 は聞くだけ)。
- 2026-10-11 以前に Claude が決めた実装の細部(溢れの持ち方・核のセルの形・待ちの丸め・畳む段 など)は各 ADR の追記とチケットの「決めたこと」にある
  (この節の前の版は aa3e55c の HANDOFF.md)。

## 次にやること
NEXT.md の先頭(D-471 の順)。判断待ちは QUESTIONS(Q44 が次の PC チャットの最初)。ユーザーに聞く時は、エンジンの仕組みなら作り手から見た違い、
ゲームの中身ならプレイヤーと遊びへの影響で聞く(CLAUDE.md §5・D-476)。

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
- gpu_conduct_bench は `--trace` を自分で取り除いてから gpu_test_options.h に渡す(知らない引数で止まるので)。
- **トレースの範囲を変えるとき**(T-0088): `GraphTrace::SetFilter` / `ProbeSim::SetTraceFilter` は次の `RecordBegin(list, slot)` から効く。
  容量は `Create` の capacity(`ProbeSimOptions::traceCapacity`)までに切り詰める。ランタイムは `frame::TRACE_CAPACITY_PER_FRAME`(65,536)をいつも確保。
  `RecordBegin` に slot を渡す(slot ごとのアップロードと、読むときの範囲)。
- exe の横のフォルダは `core/paths.h` の `ExecutableDirectory()`(ログ・シェーダー・トレースの既定の置き場所)。
- 一度だけ debug のリンクが `LNK1236: corrupt or invalid COFF sections`(graph_trace.cpp.obj)で落ち、もう一度 build したら通った(release と続けて投げた直後)。
