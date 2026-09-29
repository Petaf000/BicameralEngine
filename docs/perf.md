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

## 記録
| 日付 | 項目 | 条件 | 結果 | GPU / ドライバ | コミット |
|---|---|---|---|---|---|
