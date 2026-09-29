# ADR-0004 Agility SDK と DXC の入れ方

- Status: Accepted(A 案)
- 日付: 2026-09-29
- 決めた人: ユーザー(「Claude がやりやすい方法で」→ A)

## 背景
- T-0001 で `--caps` を実行したところ、Agility SDK を入れていない状態(OS 標準の D3D12 ランタイム)で
  WorkGraphsTier 1.0 / SM 6.8 / DXR 1.2 / Mesh Shader 1.0 が出た(Windows 11 build 26200、RTX 3070 Ti、ドライバ 32.0.15.9597)。
- DXC は Windows SDK 10.0.26100 同梱の 1.8.2502 で、lib_6_8 / cs_6_8 に対応(lib_6_9 は無し)。
- つまり「この PC で開発を進める」だけなら、今は Agility SDK も別の DXC も必須ではない。
- 一方で、OS 標準のランタイムは Windows Update で中身が変わり、他の PC(Windows 10 / 古い 11)には Work Graphs が無い。
- vcpkg の baseline(2026.07.29)に次のポートがある:
  - `directx12-agility` 1.619.4(今使っている `directx-headers` 1.619.4 と同じ版)
  - `directx-dxc` 2026-05-27
- nuget.org はクラウド側から届かず、NuGet の最新版は未確認。

## 提案(A を推す)
**A. vcpkg のポートで入れる。** `directx12-agility` と `directx-dxc` を vcpkg.json に足し、
D3D12SDKVersion / D3D12SDKPath を exe からエクスポートし、D3D12Core.dll(と debug 用の D3D12SDKLayers.dll)を
`bin\D3D12\` にコピーする CMake を書く。

- 根拠: 依存の入れ方が vcpkg だけで済む。版は builtin-baseline で固定され、ヘッダとランタイムの版が揃う。
- 未確認: ポートが dll をどこに置き、CMake ターゲットを出すか(入れてみて確かめる)。

## 検討した代案
- **B. NuGet パッケージ(Microsoft.Direct3D.D3D12)を CMake の file(DOWNLOAD) + SHA256 で取る。**
  preview 版も含めて版を自由に選べる。依存の入れ方が 2 通り(vcpkg と NuGet)になる。
- **C. 今は入れない。** 今の PC では動くので、最も手間が少ない。SM 6.9 やプレビュー機能が要るとき、
  または他の PC で動かすときに入れる。OS 更新で挙動が変わるリスクを負う。

## 影響
- A/B: 実行ファイルの横に D3D12 フォルダが要る(配布物に含める)。デバッグレイヤーの版もヘッダと揃う。
- C: T-0001 の完了条件「Agility SDK 導入」を外すか、別チケットに移す。

## 実施結果(2026-09-29)
- vcpkg.json に `directx12-agility` と `directx-dxc` を追加。exe は engine/src/platform/agility_sdk.cpp で
  D3D12SDKVersion(= ヘッダの D3D12_SDK_VERSION = 619)と D3D12SDKPath(`.\D3D12\`)をエクスポート。
- engine/CMakeLists.txt の POST_BUILD で D3D12Core.dll と d3d12SDKLayers.dll を `bin/D3D12/` にコピー。
- `--caps` で `D3D12Core : ...\bin\D3D12\D3D12Core.dll (D3D12SDKVersion 619)` を確認。WorkGraphsTier 1.0 のまま。
- DXC は CMake 変数 DIRECTX_DXC_TOOL(vcpkg の tools/directx-dxc/dxc.exe)で使う。
- 確認(2026-09-30、T-0011): vcpkg の DXC は **1.9.2602.24**(SDK 同梱の 1.8.2502 より新しい)。`lib_6_9` でノードのシェーダーをコンパイルできる。
  ビルドはこちらだけを使う(SDK 同梱のものは使わない)。lib_6_9 を実行できるかはドライバと Agility SDK 次第で、未確認(T-0013)。
