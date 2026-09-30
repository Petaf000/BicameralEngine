# 計測記録

比較できるように、日付・GPU・ドライバ・プリセット・条件を必ず書く。

## 環境
2026-09-29 時点(`job.py env`)

| 項目 | 値 |
|---|---|
| OS | Windows 11 Home build 26200(開発者モード: 有効) |
| GPU | NVIDIA GeForce RTX 3070 Ti(Ampere) / ドライバ 32.0.15.9597 |
| その他のアダプタ | SudoMaker Virtual Display Adapter(仮想ディスプレイ。アダプタ選択で除外する)。RTX 3070 Ti が別の LUID でもう 1 つ列挙され、そちらは画面を持たない(2026-09-30 `--caps`。T-0004 で窓の画面を持つ方を選ぶようにした) |
| Visual Studio | 2026 Community 18.10.2(ビルドに使用)/ 2022 17.14 / 2019 16.11 も同居 |
| MSVC | 14.51.36231 |
| cmake | 3.31.5(C:\Program Files\CMake が PATH 先頭) |
| ninja | VS 2026 同梱 |
| vcpkg | VS 2026 同梱(VC\vcpkg) |
| dxc | Windows SDK 10.0.26100.0 同梱 1.8.2502(lib_6_8 / cs_6_8 あり、lib_6_9 なし) |
| D3D12 | OS 標準ランタイムで SM 6.8 / DXR 1.2 / Mesh 1.0 / WorkGraphs 1.0(Agility SDK 無し)。Agility SDK 1.619 でも同じ |
| 整数の機能(2026-09-30、`--caps`) | Int64ShaderOps: yes / 64bit atomic: typed yes・groupshared yes・ヒープの記述子 yes(raw バッファは SM 6.6 以上で必須) |
| WARP(2026-09-30、`--caps`) | OS の WARP(Agility SDK 1.619 の D3D12Core 経由)で FL 12_1 / SM 6.8 / WorkGraphs 1.0 / DXR 1.1 / Mesh 1.0 / Int64・64bit atomic 全部 yes |

## 記録
| 日付 | 項目 | 条件 | 結果 | GPU / ドライバ | コミット |
|---|---|---|---|---|---|
| 2026-09-30 | fixed の自己テストの GPU と CPU の一致(gpu_fixed_test) | 65536 case × 18 値、compute キュー、debug(シェーダー -Od)と release | 全部一致(要約 85c154e666febd92)。1 回の実行 1.4〜2.1 s(デバイス作成と CPU 側の再計算を含む) | RTX 3070 Ti / 32.0.15.9597、WARP | 18da8e4 |
| 2026-09-30 | 最小の Work Graph(gpu_work_graph_test) | Root(16 グループ × 64)→ Leaf 1024 レコード、64bit atomic、direct / compute キュー | 全部正しい。裏のメモリ 139656 B(最小も同じ) | RTX 3070 Ti / 32.0.15.9597 | 18da8e4 |
| 2026-09-30 | 同上 | 同上 | 全部正しい。裏のメモリ 20480 B(最小 1024 B) | WARP(Windows 11 build 26200) | 18da8e4 |
| 2026-09-30 | fixed の自己テストに逆数の掛け算を足した後の一致(gpu_fixed_test) | 65536 case × 21 値、compute キュー、debug と release | 全部一致(要約 605bc2e41947d188) | RTX 3070 Ti / 32.0.15.9597、WARP | (T-0010 追加分) |
| 2026-09-30 | 整数のルーチンの費用(gpu_fixed_bench、release) | 約 100 万スレッド × 依存の連鎖、40ms 前後に合わせ 5 回の中央値、クロックは固定しない | fmul = 0.11 ps を 1 として: int64 の足し算 3.2・掛け算 4.2・割り算 113、32bit の割り算 19.6、逆数の掛け算 64bit 33・32bit 8.2、128bit の積 25、128÷64 1925、sqrt64 604、exp2 553、log2 1190、sincos 264(全部の表は 04 §6) | RTX 3070 Ti / 32.0.15.9597 | (T-0010 追加分) |
| 2026-09-30 | 128÷64 を逆数 + Newton 法に替えた後(gpu_fixed_bench、release) | 同上。div128・divshift64 の除数を毎回変えるように直した | fmul = 1 として: `FxDivU128By64` 247(前 1925)・`FxDivShiftS64` 247(前 1829)・`FxMakeRecipU64` 301(前 1494)。同じ除数なら 75。自己テストの要約 605bc2e41947d188 は変わらず(ビット一致) | RTX 3070 Ti / 32.0.15.9597 | (T-0084) |
| 2026-09-30 | フレームのループの CPU 時間(`bicameral --frames N`、release) | 窓 1280×720、165Hz の画面、記録済みのリストを使い回す、仮の刻み(128² の拡散)1 刻み/バッチ、先行 2 | vsync あり: 165 fps、CPU 1.4〜1.6 ms/フレーム(うち Present 0.9〜1.1 ms。投げるまでの仕事は約 0.5 ms)。vsync なし: 2700〜3700 fps、CPU 0.26〜0.30 ms/フレーム(うち Present 0.18〜0.20、仕事は約 0.08 ms)。CPU が描画の枠で GPU を待った回数 0。世界 59.6〜60.0 刻み/秒 | RTX 3070 Ti / 32.0.15.9597 | (T-0004) |
| 2026-09-30 | 刻みの数の切り替え方(仮の刻みのバッチの GPU 時間、release、vsync なし) | 1 本のリストに刻みの枠を 8 つ持ち、ExecuteIndirect の数 0/1 で切り替え(空の ExecuteIndirect 16 回 + UAV バリア)vs 刻みの数ごとに記録したリストを選ぶ | 0.534 ms/バッチ → 0.040 ms/バッチ(空の ExecuteIndirect 1 回あたり約 30 µs)。刻みの数ごとのリストに替えた。vsync ありでは GPU のクロックが下がり 0.85 → 0.10〜0.18 ms | RTX 3070 Ti / 32.0.15.9597 | (T-0004) |
| 2026-09-30 | シミュが重いときの描画(R-LOOP-2、`--sim-load n`、release、vsync あり) | 刻みの拡散に結果に入らない計算を足す。描画は終わっている最新の抽出を読み(待たない)、抽出は 3 組、バッチは 2 つまで重ねる | シミュ 3.1〜3.6 ms/刻み: 165 fps・世界 59.9 刻み/秒(影響なし)。12 ms/刻み: 103〜105 fps・世界 59.6。25 ms/刻み: 40 fps・世界 39.5(捨てた刻み 84)。34〜46 ms/刻み: 23 fps(描画のキューを優先度 HIGH にすると 30 fps)・世界 19〜22。**1 刻みの Dispatch が長いと、描画のキューはシミュを待っていなくても GPU の上でシミュのバッチの速さに引きずられる**(原因は未確認: 同時実行・プリエンプション・DWM)。対策の候補は 06 §4 の「刻みを分けて投げる」(T-0012) | RTX 3070 Ti / 32.0.15.9597 | (T-0004) |
