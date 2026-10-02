# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-10-03 / チケット: T-0093 Nsight で物理の詰まり方・SASS の命令数 — 完了

## 状態(3 行以内)
- Nsight Graphics の GPU Trace をランナーから(ngfx)動かせるようにし、物理の山は **SM 0.9%・DRAM 1.3% = 演算でも帯域でもなく待ち時間で詰まる**と記録した。
- 6×6 の解(1 スレッド)が 1 刻み約 2.0 ms(刻みの 56%)、山で使う色は 4〜5(16 のうち 11〜12 が空)。04 §6 の SASS の列(動的な命令数)を埋めた。
- 次は T-0017(多重解像度)。ADR-0014 は承認済み。物理の詰めは M1 の後に T-0094(島ごと + 空の色)→ T-0095(6×6 を数スレッドで)。

## 動いているもの(確認方法つき)
- `job.py build`(debug / release 警告なし)・`job.py tidy` 警告なし(57)・`python3 tools/archmap/archmap.py --check` OK(82)。
- `job.py test -Preset release -Filter "gpu_fixed|gpu_physics_mass_ratio"` 3/3(T-0093 で触ったのはベンチだけ。物理のシェーダーは変えていない)。
- `job.py run -Preset release -Exe gpu_fixed_bench [-- --only <演算> --iterations n --groups n]`(全部なら表。solve6 = PxSolveSymmetric6 を足した)。
- SASS の命令数: `python3 tools/fixed_bench/sass_count.py ps1 'out\nsight\bench' <演算 6 個まで> > ~/w/b.ps1` → `job.py raw ~/w/b.ps1` → `sass_count.py report out/nsight/bench`。
- 物理の GPU の記録: ngfx の `--activity "GPU Trace Profiler" --exe <bin>\gpu_physics_test.exe --args "--scene pile --segment 10" --start-after-submits 30 --limit-to-submits 1
  --architecture "Ampere GA10x" --metric-set-id 1 --auto-export` → `<out>/BASE/GPUTRACE_FRAME.xls`(タブ区切りの「指標 値」。投入全体の合計で、パスごとには出ない)。

## 壊れている/未確認のもの(ファイル:行 と症状)
- GPU Trace の書き出し(--auto-export)は投入全体の合計だけ。パスごと・ソースの行ごとの内訳は .ngfx-gputrace(独自の圧縮形式)を GUI で開かないと見られない。
- 静的な SASS は見られていない(GetCachedBlob は圧縮された独自形式。GUI の Shader Pipelines なら見られるはず。未確認)。
- 空の色の Dispatch 1 回の費用(約 3 µs)は ColorRound からの見積もり(未確認)。
- (前から)Work Graph のノードの局所の変数が約 5〜6 KB を超えると GPU が固まる。WARP の gpu_physics_test は未確認。物理の GPU 版は世界の刻みに未組み込み。
  山の種の約 2 割で止まった後の数 cm/s の動き(BACKLOG)。PIX・`--replay --bisect`・セーブ・AMD は未確認/未着手。

## このチャットで決めたこと(ADR にしたなら番号)
- 04 §6「書き方の方針」は変えない(物理は処理量ではなく待ち時間で詰まっているので、逆数・int64 の範囲の見直しではなく、連鎖の中の平方根・128÷64 を減らすか並列にするのが効く)。
- 2026-10-03 ユーザー決定(判断資料のスライドを見て、全部 Claude のおすすめ): **ADR-0014 を Accepted**。空の色は単独ではやらず T-0094 に含める。
  6×6 は T-0095(T-0094 の後)で数スレッドに分けて解く。平方根と割り算を減らす変更は T-0095 で足りないときだけ研究として。

## 次にやること
NEXT.md の先頭(T-0017 原理: 多重解像度)。

## 注意(次の Claude がハマりそうな所)
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
- 物理のルート定数は 14 個(色ごとの GPU の入力のために SolveBodyNode の入口の番号と u12 のアドレスを足した)。UAV は u0〜u12。
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
- **HLSL で `point` も予約語**(`line` と同じくジオメトリシェーダーの修飾子)。変数名に使うと「modifiers must appear before type」。
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

