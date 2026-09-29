# 計測記録

比較できるように、日付・GPU・ドライバ・プリセット・条件を必ず書く。

## 環境
2026-09-29 時点(`job.py env`)

| 項目 | 値 |
|---|---|
| OS | Windows 11 Home build 26200(開発者モード: 有効) |
| GPU | NVIDIA GeForce RTX 3070 Ti(Ampere) / ドライバ 32.0.15.9597 |
| その他のアダプタ | SudoMaker Virtual Display Adapter(仮想ディスプレイ。アダプタ選択で除外する) |
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
