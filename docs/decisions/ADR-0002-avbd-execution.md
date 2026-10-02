# ADR-0002 AVBD の実行形態(Work Graphs か Compute か)

- Status: Accepted(2026-09-30、D-302)
- 日付: 2026-09-29
- 決めた人: ユーザー

## 背景
当初案は「AVBD の 1 反復 = Work Graph の 1 ノード実行」。
VBD 系は頂点をグラフ彩色し、色ごとに全体同期を取りながら反復する。Work Graphs はノード間に任意の全体バリアを張れず、
グラフは有限で終わる必要がある(再帰の深さにも上限)。

## 提案
- ソルバーの反復(色 × 反復回数)は Compute Dispatch で回す。GPU 上のまま。
- Work Graphs は AVBD に「何を渡すか」を決める側: 活性化した物体の選別・接触ペアの生成・化学反応による構造変化の反映。

## 未確認
- Work Graphs の仕様・ドライバの現状で、上の制約が今も同じか(T-0005 で実際に確かめる)
- 反復回数が少ない場合に、再帰ノードで表現して性能が出るか(実験する価値はある)

## 影響
AVBD は化学の結果として「崩れ方」を解く役。化学(状態)→ AVBD(崩れ方)の一方向の分担になる。

## 決定(2026-09-30)
**できる限り Work Graphs。** 反復回数が決まっていて、Work Graphs でも Compute でも結果が変わらないものは、両方作って測り、安い方を採る。
AVBD は整数で決定的に解く研究(R-PHYS、08-bodies-physics.md)とあわせて進める。


## 計測(T-0092、2026-10-02。RTX 3070 Ti / 32.0.15.9597、release)
整数の AVBD の 1 刻み(T-0090 の Compute 版、CPU とビット一致)の部分ごとに、Work Graphs 版と Compute 版を作って測った。
どちらも同じ関数(shaders/common/physics_step.hlsli・shaders/sim/physics_bindings.hlsli)を呼び、3 場面の全部の刻みで CPU とビット一致する(tests/gpu_physics_test)。
時間はパスごとのタイムスタンプ(`gpu_physics_test --profile`)、1 刻みあたり。

| 部分 | Compute | Work Graphs | 採った方 |
|---|---|---|---|
| 広域の選別 → 接触の生成(岩の山) | 0.306 ms(Broadphase 0.138 + Narrowphase 0.168。全部の枠を起動して空なら抜ける) | 0.315 ms(グラフ 0.260 + 組を作る Compute 0.055。相手のいる枠だけを起動) | **Work Graphs**(差は 3% で揺れの内。「できる限り Work Graphs」) |
| 色ごとの解(岩の山・11 反復 × 16 色) | **2.95 ms**(1 色 1 Dispatch、全部の物を起動して色で弾く) | 5.31 ms(1 色 1 DispatchGraph、その色の物だけを GPU の入力で起動) | **Compute**(1.8 倍速い) |
| 色ごとの解(質量比・物 1 つ・15 色が空) | **1.46 ms**(8.3 µs/色) | 3.75 ms(21.3 µs/色) | Compute |

- **DispatchGraph 1 回の固定費は約 13〜20 µs**(空の色: Dispatch は 1.5〜3 µs)。色の数 × 反復の数だけ全体の同期が要る部分は、この固定費のぶん Work Graphs が負ける。
  当初の案(ソルバーの反復は Compute、Work Graphs は何を渡すかを決める側)どおりになった。
- 色ごとの解は、T-0090 の「1 スレッド = 1 物」から「1 グループ = 1 物、1 スレッド = 1 接触点、物の 6×6 は点ごとの寄与の整数の和(ウェーブと共有メモリで足す)」に替えた。
  和は 2^64 を法とするので順番に依存せず、CPU とビット一致のまま。岩の山の 1 刻み 12.1 → 3.75 ms(色ごとの解 11.2 → 2.95 ms)。
- **Work Graph のノードの局所の変数が約 5〜6 KB を超えると GPU が固まる**(DEVICE_HUNG。局所の配列 640 × 8 B は動き、768 × 8 B で固まる。同じコードは Compute なら動く。
  RTX 3070 Ti / 32.0.15.9597。仕様の上限か、ドライバの不具合かは未確認)。組(PxManifold、3 KB)を局所に 2 つ持つ「組を作る」処理はノードに入れられないので、
  グラフは接触の幾何(約 300 B)までにして、組を作るのは Compute の BuildManifolds にした。前の組は局所に写さず、点を 1 つずつバッファから読む(PxPreviousManifold・GpuPreviousManifold)。
- 比べた方(広域の選別の Compute・色ごとの解の Work Graph)も残し、ctest(gpu_physics_stack_alternative)で壊れていないことを確かめる。
- 物の数が増えた時(広域の選別を空間ハッシュにする T-0045)に、広域の選別はもう一度測る。
