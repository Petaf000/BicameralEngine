# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-29 / チケット: T-0006(見せられるリポジトリにする)— 完了

## 状態(3 行以内)
- GitHub の公開リポジトリ Petaf000/BicameralEngine に push 済み。CI(Windows debug/release ビルド + ctest、図の検査、Pages デプロイ)が緑。
- クリックできるアーキテクチャ図(案 C)が https://petaf000.github.io/BicameralEngine/ に出ている。Doxygen は /api/。
- 整形・名前の規約を決めて docs/style.md・.clang-format(CI で検査)・.clang-tidy に。C++23 化とプリコンパイルヘッダ(engine/src/pch.h)も。

## 動いているもの(確認方法つき)
- `job.py check` → OK。`job.py build` / `job.py test` は T-0001 のまま通る
- `job.py test` → smoke と singleton の 2/2 passed
- `python3 tools/archmap/archmap.py --check`(Linux 側)→ OK(コードへのリンク 5 個)
- CI: `job.py raw` で `gh run list -R Petaf000/BicameralEngine -L 3`。初回の run は全ジョブ緑(ビルド 1〜1.5 分)
- `job.py git push` → 通常の push は通る(force・削除・+refspec は git.ps1 が拒否)

## 壊れている/未確認のもの(ファイル:行 と症状)
- ilammy/msvc-dev-cmd@v1 は Node.js 20 の非推奨警告が出る(新版が無い)。他の actions は最新メジャーに上げた。
- .clang-tidy は置いただけで CI では走らせていない(compile_commands.json が要る。Windows ジョブで回すかは後で)。
- ubuntu-latest は 2026-10-19 から Ubuntu 26 に移る(GitHub の告知)。docs ジョブが落ちたらまずここを疑う。
- GPU を使うテストを CI でどう回すか(WARP の Work Graphs 対応は未確認)。テストができたときに決める。
- T-0001 からの持ち越し: RTX 3070 Ti が 2 つ列挙される件(T-0004)、vcpkg の DXC の版(T-0005)。

## このチャットで決めたこと(ADR にしたなら番号)
- ADR-0005 Accepted: 公開リポジトリ・ライセンスなし・CI は GitHub Actions・図は案 C(+Doxygen 併設)・README は英語。
- Claude が通常の push をしてよい(scripts/jobs/git.ps1 を変更)。
- 個人的な背景は CLAUDE.local.md(git 管理外)へ。作者メールは 84941361+Petaf000@users.noreply.github.com(リポジトリの git config)。
  公開前に 3 コミットを作り直して、履歴からも個人情報を消した。ブランチ名は main。
- 規約(ユーザーの流儀): インデント 4・{ は同じ行・関数は大文字始まり・class メンバー m_・関数内は小文字始まり・enum 値とマクロは大文字スネーク・
  struct メンバーは小文字始まり・1 行の if は {} 省略・早期リターン・疎結合・カスタムログ。詳細は docs/style.md。ADR-0005 に追記。
- 追加の決定(同日): 定数は大文字スネーク(MAX_DEPTH)、enum の値は UpperCamel(NotSupported)、static メンバーは s_、名前空間の中もインデント。
- ユーザー提供の singleton_template を engine/src/core/singleton.h に取り込み(規約に合わせ、破棄順・二重 delete・作り直しを修正)。
  core は静的ライブラリ bicameral_core。tests/singleton_test.cpp が通る(ctest 2/2)。
- **引き継ぎの区切りは機能(チケット)ごと。ツール呼び出しの回数では区切らない**(CLAUDE.md §3・docs/claude/ を更新)。

## 次にやること
NEXT.md の先頭(T-0007 ログ、または T-0003 / T-0004)。

## 注意(次の Claude がハマりそうな所)
- **公開リポジトリ。** 鍵(.bicameral-runner/)・CLAUDE.local.md・PC 固有のパスをコミットしない。
- Linux 側の整形: `~/.local/bin/clang-format`(`python3 -m pip install --user clang-format==23.1.1`。セッションごとに入れ直しが要るかも)。CI と同じ版にする。
- `.github/` 以下は device_commit_files では書けない(保護)。runner/staging/ に書いてから device_bash で cp する。
- 図のノードが指す関数を移す・改名したら docs/architecture/map.yaml を直す(CI の archmap --check が落ちる)。
- gh は PC で Petaf000 としてログイン済み(scope に workflow あり)。gh は `job.py raw` の .ps1 から呼ぶ。
- ランナーの鍵は `H:\BicameralEngine\.bicameral-runner\key.txt` にも置いてある(.gitignore 済み)。
- runner の結果 JSON(runner/logs/*.result.json)を cat しない。job.py の要約か .log を grep する。
- 外部コマンドを呼ぶ .ps1 で `$ErrorActionPreference='Stop'` にしない(PS 5.1 は stderr の警告で止まる。gh・git の進捗表示も stderr)。
