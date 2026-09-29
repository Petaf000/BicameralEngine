# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-29 / チケット: T-0001(環境確認と caps プローブ)— 途中

## 状態(3 行以内)
- git を始めた(最初のコミット T-0000)。debug ビルド・smoke テスト・`--caps` まで通った。
- Agility SDK 無しで WorkGraphsTier 1.0 が出た。Agility SDK の入れ方は ADR-0004(Proposed)でユーザーの判断待ち。
- 残り: ADR-0004 の決定と実施 / VS2026 の F5 確認(ユーザー)。

## 動いているもの(確認方法つき)
- `job.py check` → OK(heartbeat の時差警告も解消)
- `job.py build` → BUILD OK (debug)、/W4 警告なし
- `job.py test` → smoke 1/1 passed
- `job.py run -- --caps` → RTX 3070 Ti: SM 6.8 / DXR 1.2 / Mesh 1.0 / WorkGraphs 1.0
- `job.py env` → OS・GPU・VS・ツール・DXC の版(docs/perf.md に記録済み)

## 壊れている/未確認のもの(ファイル:行 と症状)
- RTX 3070 Ti が別 LUID で 2 つ列挙される(`--caps` の Adapter 0 と 2)。仮想ディスプレイ(SudoMaker)経由の可能性があるが未確認。
  アダプタ選択を書く T-0004 で、どちらを使うべきか確かめる。
- runner.ps1 の修正(結果 JSON の行を文字列化)は、ランナーを次に起動し直したときから効く。job.py は旧形式も読める。
- release / profile プリセットは未ビルド。

## このチャットで決めたこと(ADR にしたなら番号)
- vcpkg の builtin-baseline = 2026.07.29(9e593bb…)。
- vcpkg のキャッシュは `out/vcpkg/`(CMakePresets の environment)。ユーザー名が日本語+空白のため。
- ADR-0004(Proposed): Agility SDK / DXC の入れ方。A = vcpkg ポート(推し)/ B = NuGet 直接 / C = 今は入れない。

## 次にやること
NEXT.md の先頭 → T-0001 の「完了条件」の未チェック項目から(ADR-0004 をユーザーに聞く)

## 注意(次の Claude がハマりそうな所)
- ランナーの鍵は `H:\BicameralEngine\.bicameral-runner\key.txt` にも置いてある(.gitignore 済み)。リポジトリを接続すれば
  `$HOME/mnt/BicameralEngine/.bicameral-runner/key.txt` を `~/.bicameral-runner-key` にコピーして使える。**絶対にコミットしない。**
- runner の結果 JSON(runner/logs/*.result.json)を cat しない(旧ランナーだと巨大)。job.py の要約か .log を grep する。
- 外部コマンドを呼ぶ .ps1 で `$ErrorActionPreference='Stop'` にしない(PS 5.1 は stderr の警告で止まる)。
- cmake は C:\Program Files\CMake の 3.31.5 が使われている(VS 同梱ではない)。
