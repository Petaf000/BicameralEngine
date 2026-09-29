# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-30 / チケット: T-0009(ゴールから全部決める)— レビュー待ち

## 状態(3 行以内)
- ユーザーの指示「何か作る前に、ゴールから全部決める」(ADR-0007)で、L0〜L3 をユーザーと決定(docs/plan/DECISIONS.md、D-001〜D-321)。
- L4 詳細設計(docs/design/02・04〜16)、L5 ロードマップ(docs/plan/ROADMAP.md)、L6 運用(CLAUDE.md 原則 8、BACKLOG.md)を書いた。
- 残りはユーザーのまとめてレビュー: docs/plan/REVIEW.md の 14 項目に答えてもらい、反映して T-0009 を閉じる。**それまでコードは書かない。**

## 動いているもの(確認方法つき)
- コードは T-0007 から変えていない。`job.py build` / `job.py test`(3/3)/ `job.py run -- --caps` は前回のまま。
- `python3 tools/archmap/archmap.py --check`(Linux 側)→ OK

## 壊れている/未確認のもの
- 図(docs/architecture/map.yaml)はまだ旧設計のまま(ノードの名前・チケット番号)。M1 の最初(T-0010)で新しい設計に合わせて直す。
- 未確認の一覧は各設計文書の「未確認」(int64 と 64bit atomic・compute キューでの DispatchGraph・WARP の Work Graphs → T-0013)。

## このチャットで決めたこと
- ゴールは販売(D-001)。ゲームの中身はすべてユーザーが決める。エンジン公開・ゲーム非公開(D-006)。
- Model は全部 GPU、View と Controller は CPU(D-107、ADR-0001 改訂)。
- 機種が違っても完全に同じ結果。シミュは物理も含めて整数(D-205、ADR-0008)。SI 単位と保存則(D-206)。
- シミュに上限なし、重いと世界の時間が遅くなる(D-201・D-202)。活動がある所はどこでも 0.5m で計算(D-208)。
- できる限り Work Graphs、同等なら測って安い方(D-302、ADR-0002 Accepted)。ADR-0003 Accepted。

## 次にやること
NEXT.md の先頭(T-0009 のレビュー → M1)。

## 注意(次の Claude がハマりそうな所)
- **レビューの回答を反映する時**: 答えを DECISIONS.md に D-ID で足す → 影響する設計文書と ROADMAP を直す → REVIEW.md の該当項目に「→ D-xxx」を書く。
- 旧チケット T-0003/0004/0005/0008 は範囲を見直し済み(マイルストーンの行あり)。新しいチケット(T-0010〜)は ROADMAP の表にあり、着手時にファイルを作る。
- **公開リポジトリ。** 鍵(.bicameral-runner/)・CLAUDE.local.md・PC 固有のパスをコミットしない。ゲームの中身(製品の反応表など)も公開側に入れない(D-006)。
- 鍵は `H:\BicameralEngine\.bicameral-runner\key.txt` にもある(.gitignore 済み)。
- Linux 側の整形: `python3 -m pip install --user clang-format==23.1.1` → `~/.local/bin/clang-format -i`(セッションごとに入れ直し)。
- `.github/` 以下は device_commit_files では書けない(保護)。runner/staging/ に書いてから device_bash で cp する。
- runner の結果 JSON(runner/logs/*.result.json)を cat しない。job.py の要約か .log を grep する。
- 外部コマンドを呼ぶ .ps1 で `$ErrorActionPreference='Stop'` にしない(PS 5.1 は stderr の警告で止まる)。
