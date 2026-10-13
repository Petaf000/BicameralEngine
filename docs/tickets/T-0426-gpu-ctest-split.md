# T-0426 GPU の ctest を順番待ちに合う大きさに分け直す

- Status: In Progress
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- 設計: D-460(GPU の順番待ち)・T-0005(ctest の上限)・T-0263(gpu_multires を 2 本に分けた)。関係: T-0385・T-0397

## 目的
GPU を使う試験は全部のランナーで 1 本ずつ(D-460)なので、1 本が長いと他のランナーが全部待つ。ctest の 1 本を 10 分に収め、
上限(ctest の TIMEOUT・ジョブの上限)が順番待ちに食われないようにする。CPU リファレンスとの比べ方(ビット一致の中身)は変えない。

## 分かったこと(2026-10-12)
- **ctest の TIMEOUT は順番待ちを含まない。** test.ps1 は GPU の試験の組の全体で 1 回だけミューテックスを取り(ctest の外)、その中で ctest を流す。
  順番待ちを含んでいたのは**ジョブの上限**(job.py --timeout → ランナーの WaitForExit): debug の gpu_multires_wide はジョブの上限 1500 秒のうち
  563 秒を待ちに使い、ctest の 1200 秒に届く前にジョブごと切られた。
- **release の gpu_multires_wide 1147.80 秒は試験そのものの時間**(順番待ちの後。パイプラインの作り直しではない: 場面ごとに作るのは前からで、
  run の --part wide は debug の HW で 285 秒・WARP で 459 秒)。差は CPU の混み具合と見ている(ビルドと CPU だけの試験は順番待ちせず並走する。
  試験は CPU リファレンスと毎刻み比べるので CPU に縛られる。WARP の試験も CPU)。**未確認**: release の場面ごとの秒は ctest の出力に出ていない。
- **重いのは場面ごとの Work Graph の作成**(debug の HW。2026-10-12 08:00〜09:00 の ctest):「ワークグラフ Multires」のログが出るまで 1 場面 95〜130 秒
  (上限まで・伝導は 384 秒)、作った後の刻みは 2〜20 秒。同じグラフを場面ごとに作り直すたびに毎回かかる。05:26 の run(--part wide)では
  一様の値の溢れが作成込みで 2.2 秒だったので、いつも遅いわけではない。WARP の作成は 6 秒。**原因は未確認**(候補: CPU の混み〔他のランナーの
  ビルド・CPU の試験は順番待ちしない〕/ ドライバのシェーダーキャッシュが当たらない〔ctest は作業ディレクトリが out/build/<p>/tests、run は bin〕)。
  4 つの組に分けた最初の形では wide_coarsen(4 場面)が 600 秒で切られたので、1 組 3 場面までにした。
- 1 本が 10 分を超えたことのある GPU の試験(各ランナーのログの最長。2026-10-10〜12。並走で混んでいた時を含む):
  gpu_multires_wide(release 1148・debug 726)・gpu_multires_implicit_conduction(release 1112)・同 _warp(release 861)・
  gpu_multires_subcycle_warp(debug 1163・release 621)・gpu_multires_implicit_tree(debug 894・release 782)・gpu_gas_warp(debug 889・release 630)・
  gpu_multires(debug 777・release 633)・gpu_multires_subcycle(debug 783)・gpu_probe_sim(debug 755)・gpu_gas(debug 690)・
  gpu_multires_uniform(debug 638)・gpu_probe_sim_warp(debug 636)。

## やったこと
1. **gpu_multires_wide を 6 本に分けた**(gpu_multires_test の --part に組を足した。場面の中身・比べ方は同じ。1 組 3 場面まで):
   | 古い名前 | 新しい名前 | --part | 場面 |
   |---|---|---|---|
   | gpu_multires_wide | gpu_multires_wide_limits | wide-limits | 上限・溢れ(伝導あり・なし。伝導が働いている確かめも) |
   | 〃 | gpu_multires_wide_overflow | wide-overflow | 大きい溢れ(伝導あり・なし)・溢れの世界で細かくする |
   | 〃 | gpu_multires_wide_many | wide-many | 上限まで(64 種の表) |
   | 〃 | gpu_multires_wide_many_conduct | wide-many-conduct | 上限まで・伝導 |
   | 〃 | gpu_multires_wide_coarsen | wide-coarsen | 溢れの端数・粗くする和集合・端数の枠の塊 |
   | 〃 | gpu_multires_wide_uniform | wide-uniform | 一様の値の溢れ(写す・刻む・反応なし)・塊の置き場が尽きる |
   | gpu_multires_warp(全部) | gpu_multires_warp(--part chains)と gpu_multires_wide_warp(--part wide) | | WARP も鎖と溢れの 2 本 |
   上限は溢れの組と WARP が 900 秒(gpu_multires は 1200 秒のまま)。--part wide・all は今まで通り全部を流す。
   --part coarsen(T-0329 の切り分け用)は wide-coarsen + wide-uniform(「塊の置き場が尽きる」も流すようになった)。
   古い名前で -Filter していたら `^gpu_multires_wide` で 7 本(HW 6・WARP 1)が当たる。
2. **gpu_multires_implicit_conduction を上限の形(--squeeze)で 2 本に**: gpu_multires_implicit_conduction(none,blocks。release の HW の計測もこちら)・
   gpu_multires_implicit_conduction_squeeze(levels,cheapest)。WARP も同じ(_warp・_squeeze_warp)。上限 900 秒。形ごとの秒をログに出す。
3. **ジョブの上限から GPU の順番待ちを除く**(runner/runner.ps1・scripts/jobs/_gpulock.ps1): _gpulock.ps1 が待った秒を
   `$env:JOB_GPU_WAIT` のファイル(runner/running/<job>.gpuwait)に書き、ランナーは上限にその秒を足して数える(足すのは 6 時間まで)。
   **ランナーを起動し直すまで効かない**(古いランナーは環境変数を渡さないので、_gpulock.ps1 は何もしない)。
4. **gpu_gas(+_warp)を場面の 2 組に**(2026-10-13): gpu_gas_test の `--scene` に名前をカンマで並べられるようにした(並べた名前が場面に
   無ければ失敗。名前を変えた時に組から黙って落ちないように)。組は tests/CMakeLists.txt の GPU_GAS_BOX_SCENES・GPU_GAS_FLOW_SCENES:
   | 名前 | 場面 | release(2026-10-13・待ち 0 秒) |
   |---|---|---|
   | gpu_gas / gpu_gas_warp | 箱: closed・periodic・single_layer・static・bubble・closed_zero_gravity・closed_moon・closed_max_gravity・bubble_zero_gravity | HW 58.0 s / WARP 389.6 s |
   | gpu_gas_flow / gpu_gas_flow_warp | 流れ: wind_mc・wind_upwind(wind_mc と比べるので同じ組)・wind_minmod_floor・trace・sound・shock・shock_isothermal・blast・static_moon・static_max_gravity | HW 24.2 s / WARP 362.7 s |
   上限は 1800 → 900 秒。WARP の重いのは bubble(137 s)と bubble_zero_gravity・static の 3 つ(28 s 前後)・風の 3 つ(70〜80 s)。
5. **gpu_probe_sim(+_warp)を 2 本に**: gpu_probe_sim_test に `--part all|framings|tables` を足し、確かめごとの秒をログに出す。
   | 名前 | --part | 確かめ | release(2026-10-13) |
   |---|---|---|---|
   | gpu_probe_sim / _warp | framings | 分け方・イベントの溢れ・記録と再生・遅い起こし | HW 73.3 s(34.3・7.4・13.5・17.3)/ WARP 97.2 s(49.9・10.4・17.1・19.5) |
   | gpu_probe_sim_tables / _tables_warp | tables | 表の差し替え・物質の付け替え・反応の上限 | HW 37.6 s(15.4・13.6・8.0)/ WARP 53.8 s(21.0・21.4・11.3) |
   debug(10 分超えだったのは debug の 755 s・636 s)は未計測。release の比で HW は約 2:1 に分かれる(debug の framings は約 500 s の見込み)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-12(wt5・t-0426): 調べた(ctest の TIMEOUT は待ちを含まない・ジョブの上限が含む・重いのは Work Graph の作成)。gpu_multires_wide を 6 本・
  gpu_multires_warp を 2 本・gpu_multires_implicit_conduction(+_warp)を 2 本ずつに分け、ランナーの上限から GPU の待ちを除いた。
  debug の HW で 4 組の形を流した(混んでいた時): limits(5 場面)532.7 s・many 172.3 s・many_conduct 476.3 s・coarsen(4 場面)600 s で切れた
  → 1 組 3 場面までに分け直した。この試験のジョブは GPU を 1459 秒待ち、ジョブの上限 3600 秒で WARP の途中で切られた(上限から待ちを除く理由)。
  **6 組の形・implicit_conduction の 2 本はビルドだけで、まだ流していない**(下の「続き」)。
- 2026-10-13(wt5・t-0426): gpu_gas(+_warp)を場面の 2 組に・gpu_probe_sim(+_warp)を --part の 2 本に分けた。release の 8 本が通った
  (秒は上の 4・5)。debug の秒と、1〜3 の続き(wide の 6 本・implicit_conduction の 2 本を流す・implicit_tree・gpu_multires)はまだ。

## 続き(次の作業役へ)
0. 2026-10-13 に分けた gpu_gas(_flow)・gpu_probe_sim(_tables)と WARP を debug で流して秒を表に足す(10 分を超えたら組を分け直す)。
1. debug・release で `-Filter '^gpu_multires_(wide|warp)'` と `'^gpu_multires_implicit_conduction'` を流し、1 本ずつの秒をこの表に足す
   (GPU の待ちが長いので、ランナーを起動し直して上限から待ちを除いた後が楽)。10 分を超える組があればさらに分ける。
2. まだ分けていない 10 分超えの候補: gpu_multires_subcycle_warp(gpu_multires_conduction_test。T-0397 が触っているので後で)・
   gpu_multires_implicit_tree(場面を全部 CPU で先に作るので、場面で分けるなら MakeScenes も分ける)・
   gpu_multires(鎖。release は 2 回目の実行と計測を含む)。gpu_gas_warp・gpu_probe_sim は 2026-10-13 に分けた(上の 4・5)。
3. Work Graph の作成が遅い原因は T-0427(研究)。

## 判断待ち
- なし(ユーザーの判断が要るものは無い。試験の名前が変わったことは統合の時に HANDOFF へ)。

