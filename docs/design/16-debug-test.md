# 16 デバッグとテスト(L4 詳細設計)

対応する決定: D-205・D-307(ビット一致・保存則)/ ADR-0006(ログ)/ 旧 T-0003・T-0008(GPU デバッグ・Work Graphs のデバッグ)

## 目的
GPU の中だけで走り切る世界を、症状から原因に辿れるようにする。決定性を毎回確かめる。

## 1. GPU → CPU のデバッグ出力
- **printf / assert のリング**: シェーダー(Work Graphs のノードも含む)から書ける。書式の ID + 引数(整数)+ 場所(ファイル・行・ノード名・刻み・セル/レコードの ID)。
  CPU が読んで、ログ(ADR-0006)に `Channel::Gpu` / `Channel::WorkGraph` で出す。溢れたら落とした数を数える。
- **ノードごとのカウンタ**: 起動数・入力レコード数・出力レコード数・最大の深さ。毎フレームの要約をログへ、細かいものはエディタのパネル(14)へ。
- **上限の検出**: 出力レコード数(仕様の上限: スレッド起動ノードは 8、ブロードキャスト/合体ノードは 256。宣言した MaxRecords を超えると未定義動作)・
  再帰の深さ(宣言した NodeMaxRecursionDepth。ノードの鎖は 32 まで)に近づいたら Warning。超える前にノード側で止めて数える(§1.2)。
- **連鎖の記録(トレース)**: 選んだ範囲(セル・刻み)だけ「どのレコードがどのレコードを生んだか」をファイルへ。重すぎたら(伝播の時間が 2 倍を超える)サンプリングかカウンタだけに落とす(旧 T-0008 の打ち切り条件)。
- **D3D12 の debug layer / GPU-based validation / DRED**: debug プリセットで有効。

### 1.1 実装(T-0003、2026-09-30)
- **リング**(`shaders/common/debug_ring.hlsli` / `engine/src/gpu/debug_ring.{h,cpp}`):
  - シェーダーは `DEBUG_PRINT(DebugFormat::名前, 引数...)` / `DEBUG_ASSERT(条件, DebugFormat::名前, 引数...)`。引数は 0〜6 個、
    `uint32_t`・`int32_t`・`uint64_t`・`int64_t`・`bool`(04 R1。浮動小数点は持ち込まない)。compute でも Work Graphs のノードでも同じ。
  - 書式の文字列は CPU だけが持つ(`shaders/common/debug_formats.hlsli` の `DEBUG_FORMAT(名前, チャンネル, 場所, 書式)`)。
    GPU は書式の番号・`__LINE__`・引数を 64 バイトのレコードに書く。場所(ファイル名・ノード名)は書式の一覧に書き、
    刻み・セル・レコードの ID は引数で渡す(T-0008 はノード名・世代・レコード ID をこの形で出す)。
  - バインドは u0 space1 のルートの UAV(`gpu::RootSignatureLayout::debugRing`)。容量 4096 件 / フレーム(256 KiB)。
    溢れたら書かずに数える(落とした数 = 要求の数 − 容量)。
  - CPU: `RecordBegin` → 書く → `RecordReadbackAndReset`(読み戻し + 見出しを 0 に戻す)→ 待つ → `Drain`(assert を先に、
    既定 32 行までログへ。残りは件数だけ)。読み戻しのバッファは枠(slot)ごと(`gpu::ReadbackRing`。T-0004)。
    フレームを重ねるときは slot = バッチの番号 % 枠の数にし、その枠のリストが終わってから `Drain(max, slot)` する。
  - 有効になるのは Debug のシミュのシェーダーだけ(shaders/CMakeLists.txt が `-DBICAMERAL_GPU_DEBUG=1` を付ける)。
    それ以外では何もしない(リングも宣言しない)。`FX_ASSERT`(fixed.hlsli)もここへつないだ。bench には付けない。
- **検証**(`engine/src/gpu/device.{h,cpp}` の `gpu::Device`):
  - `DeviceOptions{debugLayer, gpuBasedValidation, dred}`。既定は debug プリセット(`BICAMERAL_GPU_VALIDATION`)で全部有効。
  - debug layer の報告は `ID3D12InfoQueue1` のコールバックで `Channel::Gpu` のログへ(ERROR/CORRUPTION → Error、WARNING → Warning)。
    エラーの数を数え、GPU のテストは最後に 0 件であることを確かめる(`test::PassesValidation`)。
  - デバイスが失われたら `LogDeviceRemoved` が理由・DRED のブレッドクラム(止まったコマンドの前後)・ページフォールトの場所をログへ。
    ImmediateQueue が自動で呼ぶ。キューとリストには名前を付ける(DRED の記録に出る)。
- テスト: `gpu_debug_ring_test`(デコード・compute・溢れ・空に戻るか・ノードから)、`gpu_debug_device_test`
  (わざと誤った呼び出しでエラーが数えられるか・`RemoveDevice` の後に DRED の記録を読めるか)。ハードウェアと WARP の両方。

### 1.2 実装(T-0008、2026-09-30): ノードのカウンタと上限の検出
- **カウンタ**(`shaders/common/work_graph_stats.hlsli` / `engine/src/gpu/work_graph_stats.{h,cpp}`):
  - u1 space1 のルートの UAV(`gpu::RootSignatureLayout::graphStats`)。ノード 16 個 × 8 語 + 容量の計器 8 個(544 バイト)。
    ノードの語: 起動の数(スレッド起動 = スレッド、ブロードキャスト/合体 = グループ)・受け取ったレコード・出したレコード・
    止めた出力・1 回の起動の要求の最大・再帰した段の最大・止めた再帰。計器は使った量の最大(活性の一覧の長さ・キューで待つ数など)。
  - シェーダー: `WgCountLaunch(ノード, 入力の数)`・`WgCountOutputs(ノード, 要求, 出した数)`・
    `WgGrantOutputs(ノード, 要求, MaxRecords)`(切って数える)・`WgTryRecurse(ノード, GetRemainingRecursionLevels(), 宣言の深さ, 出したいか)`・
    `WgGaugePeak(計器, 値)`。ウェーブで足して(最大を取って)から 1 回だけ atomic。足し算と最大だけなので実行の順番によらず、
    **同じ入力なら毎回同じ数**(伝導の試験で、フレームの分け方を変えてもノードのカウンタが一致。ハードウェアと WARP でも一致)。
  - **Release でも有効**(上限の手前で止めるのは結果の正しさの一部。デバッグのリングと違う)。費用: 伝導の単位 +0〜3 µs/刻み
    (ほぼ揺れの内)・適用の単位 +0.4 µs(docs/perf.md)。
  - どのノードが何番か・宣言した上限は、グラフを作る側が `gpu::GraphStatsLayout` に書く(HLSL の番号と同じ順。
    伝導は `probe_sim.hlsli` の `PROBE_STATS_*` と probe_sim.cpp の `MakeConductStatsLayout`)。
  - CPU: `RecordBegin` → 数える → `RecordReadbackAndReset(list, slot)`(全部を 0 に戻す)→ その枠が終わったら `Read(slot)` → `Report`。
    ProbeSim は `ReadFrame` の中で読んで報告し、`ProbeFrameReadback::graphStats` に入れる。
- **ログ**: 毎フレームの 1 行の要約は `Channel::WorkGraph` の Trace(`--log-level trace` で出る。毎フレーム Debug に出すと 1 秒 164 行になるので)。
  フレームのループは 1 秒ごとと最後に、その間の合計を Info で出す(`1 秒: 伝導(ProbeConduct): WakeBlocks 起動 … | ConductBlock … | 活性の一覧 最大 …/…`)。
- **上限の検出**(`EvaluateGraphStats`。GPU なしで試せる):
  - 止めた(結果が変わった): 出力の上限を越える要求・再帰の深さの上限で自分へ出したかった・計器が容量を越えた → Warning。
  - 近い: 1 回の起動の出力の要求が `warnOutputRecords` 以上(出す数が構造で決まるノードは 0 = 見ない)・再帰の深さが宣言の 3/4 以上・
    計器が容量の `warnPercent`(既定 75%)以上 → Warning。
  - 同じ Warning(種類 × 番号)は 600 回の報告に 1 回だけログへ(毎フレーム出続けてログを埋めない)。`Report` は毎回全部を返す。
- **ノードの printf の場所**: 書式の一覧の「場所」にノード名(`work_graph_limits_probe/Fan`)、行は `__LINE__`、刻み・レコードの ID は引数
  (伝導の `ProbeBlockOutOfRange` は刻みとブロックの番号)。
- **バッキングメモリ**は数えられない(仕様にドライバの使った量を知る手段が無い)。作った時に大きさをログに出すだけ。
  試験のグラフ(再帰 8 段)は RTX 3070 Ti で 1,405,328 B(最小も同じ)・WARP で 17,920 B(最小 224 B)。
- 試験: `gpu_work_graph_stats_test`(`shaders/sim/work_graph_limits_probe.hlsl`。Spawn → Fan(MaxRecords 4)→ Leaf、Chain(再帰 8 段)。
  ふつう・近い・越える・64 レコードのばらばら・もう一度ふつう で、カウンタが CPU の予想と一致、検出の種類、止めたときの printf の中身と場所)。
  `gpu_probe_sim_test` は伝導のカウンタが CPU リファレンスの「計算するブロックの数」の合計と一致し、分け方によらないことを確かめる。
- 続き(T-0087): 連鎖のトレース・2 回走らせて一致・CPU リファレンスとの最初の食い違い → §1.3。

### 1.3 実装(T-0087、2026-10-01): 連鎖のトレースと、CPU リファレンスとの最初の食い違い
- **入れ物**(`shaders/common/graph_trace.hlsli` / `engine/src/gpu/graph_trace.{h,cpp}`):
  - u2 space1 のルートの UAV(`gpu::RootSignatureLayout::graphTrace`、`GraphTraceIndex()`)。見出し 64 バイト + 16 バイトの記録 × 容量。
    記録 = 刻み(64bit)・種類(8bit)・主(24bit)・従(32bit)。主と従の意味は使う側が決める。
  - 範囲(`gpu::GraphTraceFilter`: 刻み [始め, 終わり)・場所の箱 [最小, 最大)・容量)は作った時に決め、最初の `RecordBegin` が見出しへ写す。
    毎フレームの読み戻しは数(先頭 16 バイト)だけを 0 に戻し、範囲は残す。無効なら容量 0(見出しだけ)で、シェーダーは見出しの 1 語を読むだけ。
  - シェーダー: `GtWantsTick(刻み)`・`GtWantsPlace(場所)` で範囲を見て、`GtRecord`(1 件。1 スレッドだけが書く所)か
    `GtReserve(件数)` + `GtStore`(ウェーブで足して 1 回だけ atomic。ウェーブの全部のレーンが呼ぶ)。溢れた分は書かずに数える。
  - CPU: `Read(slot)` → 記録(atomic の順)と落とした数。比べる前に `SortGraphTrace`((刻み, 種類, 主, 従)の順)。
- **書くのは順番に依存しない事実だけ**(04 R1〜R8)。伝導で「どの起こしが先に予定したか」は毎回変わるので書かない。書くもの(probe_sim.hlsli の `PROBE_TRACE_*`):
  つつき(適用。主 = ブロック、従 = セルの番号)・起こす(WakeBlocks。主 = 一覧のブロック、従 = 自分か 6 面の隣。格子の中を全部。
  どちらかが箱に入れば)・計算した(ConductBlock。主 = ブロック、従 = 値が変わったら 1)。場所の単位はブロックの座標。
  → 同じ入力なら、並べた記録の列は毎回同じ(2 回・フレームの分け方を変えて一致。ハードウェアと WARP)。
- **木に組む**(`engine/src/sim/probe_trace.{h,cpp}` の `FormatProbeTrace`): 刻み t の根 = 起こすの主(つつき、か刻み t − 1 で変わったブロック)、
  根の子 = 起こされて計算したブロック。複数の根に起こされたブロックの親は**番号の一番小さい根**(決まった規則)。変わった子は刻み t + 1 の根になる。
  同じブロックが一覧に 2 度入った(同じセルを 2 回つついた)ときは「×2」。
- **ランタイム**: `bicameral --trace <file> [--trace-ticks a:b] [--trace-cells x,y,z:x,y,z]`(セルの箱はそれを含むブロックの箱にする)。
  1 フレーム 65,536 件(1 MiB)まで、集めるのは 4M 件まで。落としたら Warning(範囲を狭めれば欠けない)。
- **CPU リファレンスとの突き合わせ**(16 §3 の道具の最初の形):
  - トレース: `AppendExpectedProbeTrace` が CPU リファレンス(`ProbeReference::ChangedBlocks`)から同じ範囲の記録を予想する。
    GPU は重なりを除いて(`UniqueProbeTrace`)比べ、`FirstProbeTraceMismatch` が最初に違う記録を出す。
  - 状態: `FirstDivergentTick`(刻みごとのハッシュ列)→ その刻みまで GPU を走らせ直して(決定的なので同じ)抽出を読み、
    `FindCellDivergence` が最初のブロック(番号の順)とその中の最初のセル(z, y, x の順)・値・食い違ったセルとブロックの数を出す。
  - 試験(`gpu_probe_trace_test`): CPU リファレンスだけ刻み 9 のつつきを (33, 32, 32) → (34, 32, 32) にずらすと、
    「S(10) で食い違った: 最初のブロック 1928 (8, 8, 7)・セル (33, 32, 31) GPU 2097152 CPU 0(食い違ったセル 12・ブロック 3)」と、
    トレースの最初の食い違い「刻み 9 つつき」が出る。
- **費用**(docs/perf.md): 範囲が無効なら変わらない(T-0008 と同じ)。全部を記録すると伝導の単位が +4〜8 µs/刻み(約 4000 ブロックで 1 刻み約 3 万件)。
  打ち切り条件(伝播の時間が 2 倍を超える)には遠いので、サンプリングにはしていない。Release でも使える。

## 2. テストの種類
| テスト | 内容 | どこで回すか |
|---|---|---|
| 単体(CPU) | 数学ライブラリ・ベイクの検査・コマンドの組み立て | CI(GitHub Actions) |
| CPU リファレンス | シミュの各段を CPU で走らせ、期待値と比べる | CI |
| ビット一致 | 同じ場面を CPU と GPU で走らせ、状態のハッシュが一致 | ランナー(PC の GPU)と CI(WARP。ctest のラベル warp。§4) |
| 決定性 | 同じ再生ファイルを 2 回・フレームの分け方を変えて走らせ、ハッシュ列が一致 | ランナー |
| 保存則 | 毎刻みの合計の一致(デバッグ) | ランナー |
| 機種間 | NVIDIA と AMD で同じ再生ファイルのハッシュ列が一致 | 手動(AMD の機械を用意したら。D-207) |
| 画像 | スクショを基準画像と比べる(許容差) | ランナー |
| 性能 | 決まった場面の時間を docs/perf.md に記録。前回より 10% 以上遅くなったら警告 | ランナー |
| 浮動小数点の検査 | シミュのシェーダーの DXIL に float が無い | CI |

## 3. ずれの調べ方(ハッシュが合わなかった時)
- 刻みごとのハッシュ列で、ずれた最初の刻みを見つける → その刻みを段ごとのハッシュで調べる → ブロックごとのハッシュ → セルの値の差分、と狭めていく道具(`--replay --bisect`)。
- 最初の形(T-0087、§1.3): ハッシュ列 → 最初の刻み、その刻みまで走らせ直した全部のセル → 最初のブロックとセル(CPU リファレンスと比べる)。
  連鎖のトレースを CPU の予想と比べると、状態が食い違う前の「元の刻み」(例: つつきの場所の違い)が出る。段ごとのハッシュ・`--bisect` の引数はまだ。

## 4. 未確認
- ~~WARP が Work Graphs と SM 6.8 に対応していて CI で GPU テストが回せるか~~ → 回せる(T-0013、2026-09-30)。
  開発機の WARP(Windows 11 build 26200)は FL 12_1・SM 6.8・Work Graphs 1.0。CI(windows-2025)でも WARP のテスト
  (gpu_fixed_warp・gpu_work_graph_warp_*)が通った。GPU のテストは同じ exe を `--warp` 付きでも登録し、
  ハードウェアが要るもの(ラベル gpu)は CI で外す(`ctest --label-exclude gpu`)。ビット一致と決定性のテストもこの形で CI に入れる。
  WARP での速さは未測定(重いテストを CI に入れる時に測り、盤面の大きさを決める)。
- ~~GPU-based validation と Work Graphs を組み合わせて動くか~~ → 動く(T-0003、2026-09-30)。debug プリセットの GPU のテスト
  (Work Graph・自己テスト・リング)が、ハードウェア(開発機)と WARP の両方で debug layer のエラー 0 件で通る。
  GBV の分だけ遅い(gpu_fixed の 65536 ケースで約 7 秒)。重くなったら GBV だけ切る(`DeviceOptions`)。
- DRED のブレッドクラムが本物のハング(TDR)で「止まったコマンド」を指すか。`RemoveDevice` の試験では、
  終わったコマンドリストの記録は残らない(記録のあるリスト 0 本)ので、止まった所の表示は未確認。初めて本物のハングが起きた時に確かめる。
- PIX で Work Graphs のノードごとの時間・レコードが見られる範囲(T-0087 で公開の資料を調べた。開発機の PIX では未確認):
  - 見られる(公開の資料): DispatchGraph の引数と入力のレコードの生のバイト・ローカルのルート引数の表・ステートオブジェクトの中身(2306.21-preview)、
    DispatchGraph **全体**の時間(ToP-EoP・EoP-EoP)・ハードウェアのカウンタ・タイミングキャプチャ(同)、
    入口のノードのシェーダーのデバッグ(2412.12。プレビュー)、ノードごとに結んだリソースの一覧(2505.09)。
  - 見られない(資料に無い・「今後」とある): **ノードごとの時間**、ノードの間のレコードの流れ(グラフの構造とデータの流れの可視化は 2306 で「今後」)、
    動的に触ったリソース(2505.09 で未対応と明記)、裏のメモリの使った量。
  - だから、ノードごとの数と上限は §1.2 のカウンタ、レコードの流れは §1.3 のトレース、ノードごとの時間は単位ごとのタイムスタンプ
    (1 つのグラフ = 1 単位。ノードに分けた時間は取れない)で見る。PIX はシェーダーの 1 スレッドを追う・DispatchGraph 全体の時間と
    カウンタを見るのに使う。開発機に PIX を入れたら、上の「見られる」を実際に確かめてここを直す。
