# T-0239 debug で覗き窓を開くと世界(物理)が起動ごとに違う(window_hot_reload_play・window_replay_peek が時々落ちる)

- Status: Done(2026-10-11。回避: debug でもシミュのシェーダーを -O3 で作る = ADR-0079。ドライバの中の仕組みは未確認のまま)
- 種類: 研究(決定性の不具合。ADR-0008・CLAUDE.md 原則 3)
- PC: 必須
- 見積もり: チャット 1 回分(下の「次に試すこと」)
- 関係: T-0096(覗き窓)・T-0098(物理を仮の世界に)・T-0195(window_hot_reload_*)・D-403・ADR-0011

## 症状
- debug のビルドで、覗き窓(`--peek 28,32,32 --peek-depth 9`)を開いた窓の再生が約半分の起動で記録と食い違う。最初に違う刻みは起動ごとに違う
  (S(2)〜S(44))。一度ずれると戻らない。release では起きない(window_replay_peek・window_hot_reload_play・gpu_probe_peek(_warp) が通過)。
- 食い違った刻みでは必ず **GPU の物理(積み木)が CPU の物理と違う**(`--check-physics` の「物理の突き合わせ: S(t) の物が CPU と違う」が同じ刻みに出る)。
  同じ食い違いの値が別の起動でも出る(例: S(4) = efe7790a…・物 39e82a48… が 2 回)= でたらめなメモリではなく、いくつかの決まった別の結果になる。
- window_replay_peek(debug)は 1 回 DEVICE_HUNG(DRED: 仮の世界の伝導の DispatchGraph〔活性の一覧の GPU の入力〕で止まった)。
- FX_ASSERT の「fixed.hlsli:N」は、どのファイルの FX_ASSERT でも fixed.hlsli と出る(行番号は呼んだ所の __LINE__)。:45 は physics_math.hlsli:45(物理)。
  :156・:68 は fixed.hlsli の 128bit のずらし・符号の桁あふれ。食い違う起動に多いが、assert 0 で食い違う起動もある(原因ではなく症状と見ている)。

## 分かったこと(2026-10-10。debug・RTX 3070 Ti・`--replay window_hot_reload.bcreplay --packages no_such_packages --check-physics`。1 回約 10 秒)
- マージ(71900ee)は覗き窓の道を変えていない(git diff 972460a 71900ee: gpu_multires は溢れの変種の伝導だけ・frame_loop は反応表のパネルだけ)。
  前からある不具合で、マージ前の 3 回の通過は運だったと見る。
- 覗き窓なし: 0/6 で通過(`--sim-load 400000/1500000 --sim-split 3` で重くしても)。覗き窓あり: 約 12/27 で食い違う。
- `--physics-compute`(広域の選別を Compute)でも食い違う(物理の Work Graph のせいではない)。
- 覗き窓の段を一時的なビットで飛ばして数えた(一時のコードは戻した):
  - 影を刻む RecordStep を丸ごと呼ばない: **0/16**。RecordStep の中で SetTick だけして GPU のコマンドを何も記録しない: **0/10**。
  - RecordStep の Dispatch を全部飛ばし、パイプラインの設定・BindRoot・UAV バリアだけ残す: **4/10**。
  - さらに絞って `SetComputeRootSignature` + `SetPipelineState(multires_step_wait の PSO)` + BindRoot だけ(Dispatch もバリアも無し): **2/12**。
  - → **刻みの PSO(debug は -Od)をシミュのリストに「設定するだけ」で、後ろの物理が壊れうる**。シェーダーの計算そのもの・範囲外の書き込みは要らない。
  - 覗き窓の入れ子のバッファの前後に 8 MiB の詰め物を置いても食い違う(隣のバッファへのはみ出しではない)。
  - 写し・細かくする・引き戻す・抽出を 1 つずつ飛ばしても 0 にならない(刻みを飛ばした時だけ 0)。細かくするを飛ばすと引き戻しが空の枠の頁
    (MR_NO_PAGE)で g_cells の前を書いて必ず DEVICE_HUNG(普段の流れでは起きない組み合わせ。引き戻しは影が 9 段ある前提)。
- 見立て(未確認): debug の -Od の刻みの PSO はレジスタの溢れ(ローカルメモリ)が大きく、それを設定するとドライバがシェーダーのローカルメモリの
  置き場を作り直し、同じキューの後ろ(次のリスト)の物理の Compute(これも -Od で溢れる)の途中の値が壊れる、というドライバ側の振る舞い。
  release(-O3)は溢れが小さく起きない、と合う。仕様の違反(こちらの誤り)か、ドライバの不具合かはまだ分からない。

## 次に試すこと(打ち切り条件つき)
1. debug でもシミュのシェーダー(少なくとも multires_step と physics_*)を -O1 / -O3 で作り(BICAMERAL_GPU_DEBUG=1 は残す)、`0` を 10 回流す。
   0/10 なら -Od の PSO が引き金と確定 → 回避(debug のシミュのシェーダーは最適化する)を ADR に残し、window_hot_reload_play を 4 回以上流して閉じる。
2. 1 で消えなければ: `--warp` で同じ再生を 10 回(WARP で起きなければ NVIDIA のドライバ側)。食い違った刻みの GPU と CPU の物の値を出して(どの欄が・どれだけ)
   でたらめか小さな差かを見る。Nsight Graphics で刻みの PSO のローカルメモリの大きさを見る。
3. 打ち切り: 1・2 で 1 時間たっても仕組みが絞れなければ、debug の窓のテスト(window_hot_reload_play・window_replay_peek)は覗き窓を外すか release だけにして、
   決定性の確かめは release と CPU リファレンスに任せる案を QUESTIONS に出す(ユーザー判断)。

## 結論(2026-10-11)
- debug でもシミュのシェーダーを -O3 で作る(shaders/CMakeLists.txt の BICAMERAL_DXC_SIM_CONFIG_FLAGS。-Zi・BICAMERAL_GPU_DEBUG=1 は残す。
  `BICAMERAL_SIM_SHADER_OD=ON` で -Od に戻せる)と、覗き窓つきの再生(`--check-physics`)が **10/10 で一致・GPU の assert 0 件**(-Od では約 12/27 が食い違い)。
  → 引き金は -Od の重いパイプライン(局所メモリが大きい)と確定。回避を ADR-0079 に残した。T-0098(組を局所に持つ Compute が別のキューの割り込みで
  時々違う結果)と同じ仲間と見る。ドライバの中の仕組み(スクラッチの作り直しか・割り込みの退避か)は未確認。
- 物理のシェーダーの読み: 6×6 の解き方(physics_math.hlsli の PxSolveSymmetric6・PxFactorScaled6・PxSubstitute6)・グループで解く形
  (physics_bindings.hlsli の SolveBodyInGroup・FindPrevious・BuildSlot)に未初期化の読み・処理の順に依る書き方(04 の R1〜R8)は見当たらなかった
  (全部のシミュのシェーダーを読んだわけではない)。
- 記録の互換: debug(-O3)の記録を release が、release の記録を debug(-O3)が再生して一致。-Od で記録した古い window_hot_reload.bcreplay は
  -O3 のどちらで再生しても刻み 4 から食い違う(記録した時にもう壊れていたと見る)。
- 窓のテスト(debug・-O3): window_hot_reload_play と window_replay_peek を `ctest --repeat until-fail:4` で続けて 4 回ずつ通過。
- 打ち切りの条件(残りの研究をまた開く時): release で覗き窓つきの再生が食い違ったら、または局所メモリの大きいシミュのシェーダーを足した後に debug で
  食い違いが戻ったら、Nsight で局所メモリの大きさを見て、その関数を T-0098 の形(バッファの上で読み書き)に直す。1 時間で絞れなければ司令塔に聞く。

## 作業ログ
- 2026-10-10(作業役・本体): 切り分け 1 回目。覗き窓の刻みの PSO を設定するだけで物理が壊れうる所まで絞った(上)。直したものは無い。
  一時の切り分けのコード(bin の peek_skip.txt のビットで段を飛ばす・詰め物)は戻した。かかった時間 約 1 時間 50 分(司令塔のジョブ待ちを除く)。
- 2026-10-11(作業役・本体): debug でもシミュのシェーダーを -O3 で作る形(前の作業役の shaders/CMakeLists.txt の書きかけ)で 10/10 一致・
  窓の再生のテスト 4 回ずつ通過 → ADR-0079 に回避を書いて閉じた。debug の gpu_multires は DEVICE_HUNG しなくなったが 1200 秒の上限に届いた(T-0263)。
  かかった時間 約 40 分(ジョブ待ちを除く)。
