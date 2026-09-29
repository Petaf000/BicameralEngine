# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-29 / チケット: T-0007(ログの仕組み・CPU 側)— 完了

## 状態(3 行以内)
- core にログを追加: `Log(Channel, Level, "書式 {}", ...)` がコンソール・VS の出力ウィンドウ・exe の横の logs/*.log に出る(ADR-0006)。
- HRESULT は `BICAMERAL_CHECK_HR(Channel, 式)` で、式・ファイル:行・エラー名・OS の説明文が Error で出る。caps.cpp の printf は全部ログに。
- 次は Work Graphs を自走させる前提のデバッグ(T-0003 → T-0005 → T-0008)。T-0008 は今回ユーザーの指摘で新設。

## 動いているもの(確認方法つき)
- `job.py build`(debug)/ `job.py build -Preset release` → 通る。MSVC の警告 0
- `job.py test` → smoke / singleton / log の 3/3 passed
- `job.py run -- --caps` → ログ形式で GPU の対応状況。ファイルは out/build/debug/bin/logs/bicameral-日時.log(場所つき)
- `job.py run -- --bogus` → `E core | 知らない引数` で EXIT 2。`--log-level warning` で Info が消える
- `python3 tools/archmap/archmap.py --check`(Linux 側)→ OK(コードへのリンク 8 個)

## 壊れている/未確認のもの(ファイル:行 と症状)
- CI は e74d36a で全ジョブ緑(debug/release ビルド+ctest・図・Pages)。図の core グループ(corelib)の見た目はページで未目視。
- コンソールのシンクは毎行 fflush、書き込みは鍵の中。毎フレーム大量に出すようになったら見直す(ADR-0006「影響」)。
- T-0006 からの持ち越し: ilammy/msvc-dev-cmd の Node 20 警告、.clang-tidy を CI で未実行、ubuntu-latest の 26 移行(10/19)、
  GPU テストを CI でどう回すか、RTX 3070 Ti が 2 つ列挙される件(T-0004)、vcpkg の DXC の版(T-0005)。

## このチャットで決めたこと(ADR にしたなら番号)
- ADR-0006 Accepted: ログは Singleton<Logger>・HRESULT はマクロ BICAMERAL_CHECK_HR・ファイルは exe の横の logs/(--log-dir で変更、20 個残す)。
- main は wmain(引数を UTF-16 で受け、日本語のパスを壊さない)。`--log-dir` / `--log-level` を追加。
- ゲームエンジンで定番の作り方の型は使ってよい(ユーザー)。合うのはデータ指向・サブシステム+一方向依存・ランタイムとツールの分離(docs/style.md に追記)。
- Work Graphs で自走させるならデバッグ機能が要る(ユーザー)→ T-0008 を新設。T-0003 のリングバッファはノードからも書ける形にする。

## 次にやること
NEXT.md の先頭(PC なら T-0003、PC に触れない日は T-0002)。

## 注意(次の Claude がハマりそうな所)
- **公開リポジトリ。** 鍵(.bicameral-runner/)・CLAUDE.local.md・PC 固有のパスをコミットしない。
- 鍵は `H:\BicameralEngine\.bicameral-runner\key.txt` にもある(.gitignore 済み)。`~/.bicameral-runner` を接続しなくても、
  リポジトリのマウントからコピーすれば `job.py check` が通る。
- チャンネルを足すときは core/log.h の `Channel` と log.cpp の `CHANNEL_NAMES` の両方(順番を合わせる)。
- core は pch を持たない静的ライブラリ。windows.h は core の .cpp で直接 include している(ヘッダには入れない。HRESULT は `HResult = long`)。
- Linux 側の整形: `python3 -m pip install --user clang-format==23.1.1` → `~/.local/bin/clang-format -i`(セッションごとに入れ直し)。
- `.github/` 以下は device_commit_files では書けない(保護)。runner/staging/ に書いてから device_bash で cp する。
- gh は PC で Petaf000 としてログイン済み。`job.py raw` の .ps1 から呼ぶ(CI の確認: `gh run list -R Petaf000/BicameralEngine -L 3`)。
- runner の結果 JSON(runner/logs/*.result.json)を cat しない。job.py の要約か .log を grep する。
- 外部コマンドを呼ぶ .ps1 で `$ErrorActionPreference='Stop'` にしない(PS 5.1 は stderr の警告で止まる)。
