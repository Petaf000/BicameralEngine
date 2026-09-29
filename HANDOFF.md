# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-29 / チケット: T-0006(見せられるリポジトリにする)— 途中(整形だけ残り)

## 状態(3 行以内)
- GitHub の公開リポジトリ Petaf000/BicameralEngine に push 済み。CI(Windows debug/release ビルド + ctest、図の検査、Pages デプロイ)が緑。
- クリックできるアーキテクチャ図(案 C)が https://petaf000.github.io/BicameralEngine/ に出ている。Doxygen は /api/。
- 残りは整形の流儀の決定 → .clang-format・CI の整形チェック・既存コードの整形。

## 動いているもの(確認方法つき)
- `job.py check` → OK。`job.py build` / `job.py test` は T-0001 のまま通る
- `python3 tools/archmap/archmap.py --check`(Linux 側)→ OK(コードへのリンク 5 個)
- CI: `job.py raw` で `gh run list -R Petaf000/BicameralEngine -L 3`。初回の run は全ジョブ緑(ビルド 1〜1.5 分)
- `job.py git push` → 通常の push は通る(force・削除・+refspec は git.ps1 が拒否)

## 壊れている/未確認のもの(ファイル:行 と症状)
- ci.yml の actions を最新のメジャー(checkout@v7 ほか)に上げた直後。その run の結果は次のチャットで確認する
  (Node.js 20 の非推奨警告を消すため。ilammy/msvc-dev-cmd@v1 は新版が無いので警告が残る)。
- ubuntu-latest は 2026-10-19 から Ubuntu 26 に移る(GitHub の告知)。docs ジョブが落ちたらまずここを疑う。
- GPU を使うテストを CI でどう回すか(WARP の Work Graphs 対応は未確認)。テストができたときに決める。
- T-0001 からの持ち越し: RTX 3070 Ti が 2 つ列挙される件(T-0004)、vcpkg の DXC の版(T-0005)。

## このチャットで決めたこと(ADR にしたなら番号)
- ADR-0005 Accepted: 公開リポジトリ・ライセンスなし・CI は GitHub Actions・図は案 C(+Doxygen 併設)・README は英語。
- Claude が通常の push をしてよい(scripts/jobs/git.ps1 を変更)。
- 個人的な背景は CLAUDE.local.md(git 管理外)へ。作者メールは 84941361+Petaf000@users.noreply.github.com(リポジトリの git config)。
  公開前に 3 コミットを作り直して、履歴からも個人情報を消した。ブランチ名は main。
- 整形の流儀: 未決。候補 4 つ(Google 風 100 桁 / Microsoft 風 / Unreal 風 / LLVM 風)をコード例つきで提示済み、ユーザーの返事待ち。

## 次にやること
NEXT.md の先頭 → T-0006 の残り: 整形の流儀をユーザーに聞く(答えが無ければ Google 風 100 桁を推す)→ .clang-format
(ReflowComments: false。日本語コメントは 1 文字 2 桁で数えられる)→ CI に整形チェック → 既存コードを整形 → docs/style.md と CLAUDE.md §4。

## 注意(次の Claude がハマりそうな所)
- **公開リポジトリ。** 鍵(.bicameral-runner/)・CLAUDE.local.md・PC 固有のパスをコミットしない。
- `.github/` 以下は device_commit_files では書けない(保護)。runner/staging/ に書いてから device_bash で cp する。
- 図のノードが指す関数を移す・改名したら docs/architecture/map.yaml を直す(CI の archmap --check が落ちる)。
- gh は PC で Petaf000 としてログイン済み(scope に workflow あり)。gh は `job.py raw` の .ps1 から呼ぶ。
- ランナーの鍵は `H:\BicameralEngine\.bicameral-runner\key.txt` にも置いてある(.gitignore 済み)。
- runner の結果 JSON(runner/logs/*.result.json)を cat しない。job.py の要約か .log を grep する。
- 外部コマンドを呼ぶ .ps1 で `$ErrorActionPreference='Stop'` にしない(PS 5.1 は stderr の警告で止まる。gh・git の進捗表示も stderr)。
