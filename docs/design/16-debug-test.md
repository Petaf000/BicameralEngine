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
- 残り(T-0087): 連鎖のトレース・2 回走らせてトレースが一致するテスト・CPU リファレンスとの最初の食い違いの報告・PIX の調査。

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
- PIX で Work Graphs のノードごとの時間・レコードが見られる範囲。
