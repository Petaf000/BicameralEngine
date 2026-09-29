---
name: bicameral-start
description: Bicameral Engine(魔法研究×オープンワールドのための自作 GPU 常駐エンジン)の作業を始める・再開するときに使う。
---

# Bicameral Engine のセッション開始

ユーザーに言われなくても、次を順にやる。途中経過は実況しない。

## 1. PC のフォルダを接続する

1. ToolSearch で remote-devices のツール(`mcp__remote-devices__get_device_info`、device_bash など)を読み込む
2. `get_device_info` の `connectedFolders` を見る
3. `H:\BicameralEngine`(リポジトリ)が無ければ、`mcp__remote-devices__device_request_folder_access` で 1 回依頼する
   (reason: 「Bicameral Engine のリポジトリを編集し、ランナーの鍵を読んでビルドやテストを実行するため」)。
   ランナーの鍵はリポジトリの `.bicameral-runner\key.txt`(git 管理外)にあるので、別のフォルダは要らない
4. remote-devices のツールが無い(チャットが PC にリンクされていない)ときは、デスクトップアプリでこのチャットを開いて
   メッセージを送ってもらうよう頼む。フォルダは接続できたのに device_bash が無いときは、そう伝えて次のメッセージを待つ
5. 設計の相談だけなら、PC 無しの「設計チャット」として進めてよい

リポジトリのフォルダの外には触れない。

## 2. ランナーを使えるようにする

- `$HOME/mnt/BicameralEngine/scripts/runner_submit.py` を `~/job.py` にコピー。job.py は
  `$HOME/mnt/BicameralEngine/.bicameral-runner/key.txt` を直接読む(鍵をコピーしない)。**鍵の中身を表示・返答・保存しない**(cat しない)
- `python3 ~/job.py check` で稼働と鍵 ID の一致を確認する
  - 停止中・heartbeat が無い → ユーザーにランナーの起動だけを頼む
  - 鍵 ID 不一致 → `~/.bicameral-runner-key` など古い鍵が残っていれば消す。それでも合わなければランナーの起動し直しを頼む
- git・cmake・ビルドは Linux 側で実行しない(status も)。すべて `job.py` 経由

## 3. 引き継ぎを読む

`$HOME/mnt/BicameralEngine` の CLAUDE.md → HANDOFF.md → NEXT.md をこの順に全部読む。
次に、今回やるチケット(ユーザーの指定、無ければ NEXT.md の先頭)の docs/tickets/ のファイルだけを読む。
設計書(docs/design/)は、そのチケットに関係する 1 枚だけ読む。

以降の運用ルール(引き継ぎの条件・コンテキストの使い方・git)は CLAUDE.md に従う。

## 4. 着手する

ユーザーに 2〜3 行だけ返して、すぐ作業に入る:
- 今の状態(HANDOFF.md の要点)
- このチャットでやるチケットと、完了条件のうち今回狙うもの

「前の設計チャットの結論を反映して」と言われたら、このプロジェクトの過去チャットを検索して「反映用まとめ」を探し、
docs/inbox/ も確認して、該当ファイルに反映してから作業に入る。
