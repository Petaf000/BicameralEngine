# T-0001 環境確認と caps プローブ

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分

## 目的
この PC で DX12 Ultimate + SM6.8 + Work Graphs が使えることを、エンジン自身の出力で確定させる。
以後のすべての作業の土台なので、ここで環境の不明点をなくす。

## 完了条件
- [x] `job.py check` が OK(ランナー疎通・鍵 ID 一致)
- [x] `job.py env` の結果(GPU・ドライバ・VS・cmake)を docs/perf.md の「環境」に記録
- [x] `job.py build` が通る(debug)
- [x] `job.py test` が通る(smoke)
- [x] `job.py run -- --caps` で SM / DXR / Mesh Shader / WorkGraphsTier が表示される
- [x] Agility SDK(Work Graphs 対応版)を導入し、WorkGraphsTier が 1.0 になる
      - D3D12SDKVersion / D3D12SDKPath のエクスポートと、D3D12Core.dll を bin\D3D12\ に置く配線
      - vcpkg のポート(directx12-agility など)か NuGet か、どちらで入れるかは実際に確かめてから決め、ADR に残す
      - DXC(SM6.8 に対応する版)も同時に入れる
- [x] VS2026 で「フォルダーを開く」→ プリセット選択 → F5 で --caps が動くことをユーザーが確認
- [x] HANDOFF / NEXT 更新・コミット

## メモ・参考
- 骨格はまだ一度もビルドしていない。最初のエラーは vcpkg の場所(VCPKG_ROOT)か directx-headers の見つけ方の可能性が高い。
- Agility SDK は「最新版」を仮定しない。使う版をその時点で調べて決める。

## 作業ログ
- 2026-09-29: git init と最初のコミット(T-0000)。ランナーの結果 JSON(行がオブジェクトのまま入っていた)と job.py の heartbeat 時刻比較(時差を無視していた)を修正。
- build が通るまでに直したこと: vcpkg.json に builtin-baseline(2026.07.29)を追加 / vcpkg のキャッシュを out/vcpkg に移動(ユーザー名が日本語+空白で %LOCALAPPDATA% を扱えない。CMakePresets の environment と VCPKG_KEEP_ENV_VARS)/ build・test・run の .ps1 を外部コマンド実行時は ErrorActionPreference=Continue に(PS 5.1 が stderr の警告で止まる)。
- `--caps`: Agility SDK 無しで SM 6.8 / DXR 1.2 / Mesh 1.0 / WorkGraphs 1.0。DXC は SDK 同梱の 1.8.2502(lib_6_8 あり)。Agility SDK の入れ方は ADR-0004(Proposed)でユーザーの判断待ち。
- RTX 3070 Ti が別 LUID で 2 つ列挙される(未確認。仮想ディスプレイ経由の可能性)。caps に LUID を表示するようにした。
- Agility SDK 1.619 を vcpkg で導入(ADR-0004 Accepted: A 案)。`--caps` で exe 横の D3D12Core.dll が読まれることを表示するようにした。
- ユーザーが VS2026 の F5(.vs/launch.vs.json に args: --caps)で同じ表示を確認。完了。
