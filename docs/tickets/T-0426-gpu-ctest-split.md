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
6. **wide の 6 本・WARP の 2 本を release で流した**(2026-10-13・待ち 0 秒・全部通った。場面ごとの秒。HW は 1 場面ごとに Work Graph を作るのに
   約 50 秒かかっていて〔WARP は同じ場面が 2〜26 秒〕、それが大半〔T-0427〕):
   | 名前 | release HW / WARP(秒) | 場面ごと(HW) |
   |---|---|---|
   | gpu_multires_wide_limits | 105.6 | 53.0・51.8 |
   | gpu_multires_wide_overflow | 162.3 | 52.5・55.4・53.7 |
   | gpu_multires_wide_many | 314.7 | 313.8(1 場面。これ以上は分けられない) |
   | gpu_multires_wide_many_conduct | 154.6 | 153.3 |
   | gpu_multires_wide_coarsen | 443.3 | 91.7・206.3(粗くする和集合)・144.5 |
   | gpu_multires_wide_uniform | 211.0 | 55.1・52.7・50.2・52.4 |
   | gpu_multires_warp(鎖) | WARP 40.4 | |
   | gpu_multires_wide_warp(溢れ全部) | WARP 114.9 | 8.8・4.3・6.4・3.3・8.6・25.8・25.2・5.4・4.6・3.1・1.6・5.7・5.9・5.8 |
7. **gpu_multires(鎖)を 3 本に**(gpu_multires_test の `--part chains-real|chains-stress|chains-limits`。`--part chains` は 3 つを続けて = WARP):
   | 古い名前 | 新しい名前 | --part | 場面 | release HW(2026-10-13) |
   |---|---|---|---|---|
   | gpu_multires | gpu_multires | chains-real | 本物の鎖(release は 2 回)・影の鎖・時間の計測(release) | 61.8 s(12.8・12.1・12.2) |
   | 〃 | gpu_multires_stress | chains-stress | たくさんの要求(release は 2 回) | 33.7 s(17.1・16.0) |
   | 〃 | gpu_multires_limits | chains-limits | 粗くすると入りきらない子・上限・上限・伝導(溢れなし) | 37.4 s(12.3・12.2・12.2) |
   上限は 1200 → 900 秒。2 回の実行の一致は組の中で比べる(本物の鎖・たくさんの要求それぞれ)。
8. **gpu_multires_implicit_tree(HW)を場面で 2 本に**(gpu_multires_implicit_tree_test の `--scene hot,chain,stress`。既定は全部 = WARP):
   | 名前 | --scene | release HW(2026-10-13) |
   |---|---|---|
   | gpu_multires_implicit_tree | hot,chain | 276.3 s(計測 約 220 s・比べる 熱い点 12.7・鎖 40.0) |
   | gpu_multires_implicit_tree_stress | stress | 100.8 s(CPU で作る 5.0・比べる 21.6) |
   | gpu_multires_implicit_tree_warp | (全部) | WARP 191.9 s |
   「たくさんの要求だけ形を変えた比べを release に限る」は、並びの最後かどうかではなく場面の印(TreeScene::stress)で見るようにした。上限 1200 → 900 秒。
9. **gpu_multires_subcycle(+_warp)を鎖とたくさんの要求の 2 本ずつに**(gpu_multires_conduction_test の `--scenes all|chain|stress`。
   Compute 版と Work Graph 版〔Compute 版の CPU の結果と比べる〕の組は分けない。計測〔release の HW〕は鎖の組だけ):
   gpu_multires_subcycle(chain)・gpu_multires_subcycle_stress(stress)・gpu_multires_subcycle_warp(chain)・gpu_multires_subcycle_stress_warp(stress)。
   上限 2400 → 1200 秒。場面ごとの秒をログに出す。release(2026-10-13): HW の鎖 239.8 s(計測 約 230 s・比べる 4.3・1.1・3.0)・
   HW のたくさんの要求 55.6 s(GpuMultires の作成 約 34 s・11.7・0.5・8.4)・WARP の鎖 201.1 s(42.8・157.1。Work Graph 版は release の WARP では流さない)・
   WARP のたくさんの要求 219.5 s(124.3・94.0)。
10. **implicit_conduction を release で流した**(2026-10-13): HW none,blocks 542.8 s(計測 約 183 s・178.8・180.5)/ HW levels,cheapest 643.2 s
   (188.2・**一番安い積み方 454.3**)/ WARP none,blocks 332.7 s(237.6・94.9)/ WARP levels,cheapest 397.6 s(205.8・191.6)。
   HW の levels,cheapest が 10 分を超えたので、HW を 3 本に: gpu_multires_implicit_conduction(none,blocks)・
   gpu_multires_implicit_conduction_squeeze(levels)・**gpu_multires_implicit_conduction_cheapest(cheapest。新しい名前)**。WARP は 2 本のまま。

### HANDOFF に写す「試験の名前の一覧の変化」(T-0426 全体)
- 消えた名前: gpu_multires_wide(→ gpu_multires_wide_limits / _overflow / _many / _many_conduct / _coarsen / _uniform)。
- 足した名前: gpu_multires_wide_{limits,overflow,many,many_conduct,coarsen,uniform}・gpu_multires_wide_warp・gpu_multires_stress・gpu_multires_limits・
  gpu_multires_implicit_conduction_squeeze(+_warp)・gpu_multires_implicit_conduction_cheapest(HW だけ)・gpu_multires_implicit_tree_stress・gpu_multires_subcycle_stress(+_warp)・
  gpu_gas_flow(+_warp)・gpu_probe_sim_tables(+_warp)。
- 名前は同じで中身が減った: gpu_multires(鎖の本物・影だけ)・gpu_multires_warp(鎖だけ)・gpu_multires_implicit_conduction(+_warp。none,blocks だけ)・
  gpu_multires_implicit_conduction_squeeze(HW は levels だけ。WARP は levels,cheapest)・
  gpu_multires_implicit_tree(HW は熱い点・鎖だけ)・gpu_multires_subcycle(+_warp。鎖だけ)・gpu_gas(+_warp。箱だけ)・gpu_probe_sim(+_warp。framings だけ)。
- `-Filter` の目安: 鎖 `^gpu_multires(_stress|_limits|_warp)?$`・溢れ `^gpu_multires_wide`・陰解法 `^gpu_multires_implicit`・
  小刻み `^gpu_multires_subcycle`・気体 `^gpu_gas(_flow)?(_warp)?$`(`^gpu_gas` は gpu_gas_tree なども当たる)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-12(wt5・t-0426): 調べた(ctest の TIMEOUT は待ちを含まない・ジョブの上限が含む・重いのは Work Graph の作成)。gpu_multires_wide を 6 本・
  gpu_multires_warp を 2 本・gpu_multires_implicit_conduction(+_warp)を 2 本ずつに分け、ランナーの上限から GPU の待ちを除いた。
  debug の HW で 4 組の形を流した(混んでいた時): limits(5 場面)532.7 s・many 172.3 s・many_conduct 476.3 s・coarsen(4 場面)600 s で切れた
  → 1 組 3 場面までに分け直した。この試験のジョブは GPU を 1459 秒待ち、ジョブの上限 3600 秒で WARP の途中で切られた(上限から待ちを除く理由)。
  **6 組の形・implicit_conduction の 2 本はビルドだけで、まだ流していない**(下の「続き」)。
- 2026-10-13(wt5・t-0426): gpu_gas(+_warp)を場面の 2 組に・gpu_probe_sim(+_warp)を --part の 2 本に分けた。release の 8 本が通った
  (秒は上の 4・5)。debug の秒と、1〜3 の続き(wide の 6 本・implicit_conduction の 2 本を流す・implicit_tree・gpu_multires)はまだ。
- 2026-10-13 その 2(wt5・t-0426b): release で wide の 6 本 + WARP 2 本・implicit_conduction の 4 本を流した(全部通った。秒は 6・10)。
  gpu_multires(鎖)を 3 本・implicit_tree(HW)を 2 本・subcycle(HW・WARP)を 2 本ずつ・implicit_conduction の HW の squeeze を 2 本に分けた。
  release の新しい 10 本も通った(秒は 7〜9。cheapest の分け直しは release のビルドだけで、流していない)。release の 1 本の最長は wide_coarsen 443 s・
  implicit_conduction 543 s・cheapest(分ける前の形で 454 s)。**debug はビルドだけで、まだ流していない**(GPU の順番待ちが 1 回 10〜25 分で、2 時間に入らなかった)。

## 続き(次の作業役へ)
1. **debug で全部を流して秒を表に足す**(10 分を超えた組はさらに分ける)。2026-10-13 その 2 の終わりに wt5 のランナーへ投げた debug の試験の結果を見る
   (runner/logs の 2026-10-13 18 時台〔日本時間〕以降の `*-test.log`。`-Show` 付きなので場面ごとの秒が出ている):
   `'^gpu_multires(_stress|_limits)?$|^gpu_multires_implicit_tree'`・`'^gpu_multires_subcycle|^gpu_multires_implicit_conduction'`。
   まだ流していない debug の組: `'^gpu_multires_(wide|warp)'`・`'^gpu_gas(_flow)?(_warp)?$|^gpu_probe_sim(_tables)?(_warp)?$'`。
2. release の gpu_multires_implicit_conduction_cheapest を 1 回流す(分けた後の形。約 460 s の見込み)。
3. release でも 10 分に近いもの: implicit_conduction(none,blocks)543 s・wide_coarsen 443 s・cheapest 454 s。CPU が混むと 1.5〜2 倍になるので、
   debug で 10 分を超えたら場面か形で分ける(wide_coarsen は「粗くする和集合」206 s が重い。implicit_conduction は計測 約 183 s を別にもできる)。
   wide_many(1 場面 314 s)はこれ以上分けられない。重さは HW の場面ごとの Work Graph の作成(1 場面 約 50 s・WARP は数秒)で、T-0427 が直せば全部軽くなる。
4. Work Graph の作成が遅い原因は T-0427(研究。wt3)。

## 判断待ち
- なし(ユーザーの判断が要るものは無い。試験の名前が変わったことは統合の時に HANDOFF へ)。

