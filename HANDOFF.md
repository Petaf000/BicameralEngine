# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-29 / チケット: T-0001(環境確認と caps プローブ)— 完了

## 状態(3 行以内)
- T-0001 完了。debug ビルド・smoke テスト・`--caps` が通り、Agility SDK 1.619(vcpkg)で WorkGraphsTier 1.0。
- VS2026 の「フォルダーを開く」→ F5 でも同じ表示をユーザーが確認済み。
- 次は T-0006(規約・CI・クリックできるアーキテクチャ図)。方針をユーザーに聞くところから。

## 動いているもの(確認方法つき)
- `job.py check` → OK
- `job.py build` → BUILD OK (debug)、/W4 警告なし。bin/D3D12/ に D3D12Core.dll・d3d12SDKLayers.dll がコピーされる
- `job.py test` → smoke 1/1 passed
- `job.py run -- --caps` → `D3D12Core : ...\bin\D3D12\D3D12Core.dll (D3D12SDKVersion 619)`、SM 6.8 / DXR 1.2 / Mesh 1.0 / WorkGraphs 1.0
- `job.py env` → OS・GPU・VS・ツール・DXC(docs/perf.md に記録済み)

## 壊れている/未確認のもの(ファイル:行 と症状)
- RTX 3070 Ti が別 LUID で 2 つ列挙される(`--caps` の Adapter 0 と 2)。仮想ディスプレイ(SudoMaker)経由の可能性、未確認。T-0004 で確かめる。
- vcpkg の DXC(DIRECTX_DXC_TOOL)の版と lib_6_9 対応は未確認。`job.py env` が見ているのは SDK 同梱の dxc。T-0005 で確認。
- runner.ps1 の修正(結果 JSON の行を文字列化)はランナー再起動後から効く。job.py は旧形式も読める。
- release / profile プリセットは未ビルド。

## このチャットで決めたこと(ADR にしたなら番号)
- ADR-0004 Accepted: Agility SDK と DXC は vcpkg のポート(directx12-agility / directx-dxc)。
- vcpkg の builtin-baseline = 2026.07.29(9e593bb…)。キャッシュは `out/vcpkg/`(ユーザー名が日本語+空白のため)。
- 読みやすさの規約を CLAUDE.md §4 に追加(ユーザーの常設の要望: ポートフォリオにする前提)。詳細は T-0006。

## 次にやること
NEXT.md の先頭 → T-0006。チケットの「決めること」3 点(GitHub 公開 / 図の案 / 整形の流儀)をユーザーに聞く。

## 注意(次の Claude がハマりそうな所)
- ランナーの鍵は `H:\BicameralEngine\.bicameral-runner\key.txt` にも置いてある(.gitignore 済み)。リポジトリを接続すれば
  `$HOME/mnt/BicameralEngine/.bicameral-runner/key.txt` を `~/.bicameral-runner-key` にコピーして使える。**絶対にコミットしない。**
- runner の結果 JSON(runner/logs/*.result.json)を cat しない(旧ランナーだと巨大)。job.py の要約か .log を grep する。
- 外部コマンドを呼ぶ .ps1 で `$ErrorActionPreference='Stop'` にしない(PS 5.1 は stderr の警告で止まる)。
- cmake は C:\Program Files\CMake の 3.31.5 が使われる(VS 同梱ではない)。VS 2026 のジェネレーター(.sln 生成)はこの版では使えない。
- VS の起動引数は `.vs/launch.vs.json`(コミットされない)。ユーザーの PC には `--caps` の設定が入っている。
