# Bicameral Engine

魔法研究 × オープンワールドのゲームのための、GPU 常駐シミュレーションエンジン。
世界の状態を VRAM に置き、反応の連鎖を DirectX 12 Work Graphs で走らせる。

- ビジョン: [docs/design/00-vision.md](docs/design/00-vision.md)
- アーキテクチャ: [docs/design/01-architecture.md](docs/design/01-architecture.md)
- Claude と開発する仕組み: [docs/claude/WORKFLOW.md](docs/claude/WORKFLOW.md)

## はじめての準備(ユーザーが 1 回だけ)

1. このフォルダを `H:\BicameralEngine`(など)に置き、PowerShell で:
   ```powershell
   cd H:\BicameralEngine
   git init
   git add -A
   git commit -m "T-0000: プロジェクトの立ち上げ"
   ```
2. claude.ai でプロジェクト「Bicameral Engine」を作り、指示欄に
   [docs/claude/project-instructions.md](docs/claude/project-instructions.md) の本文を貼る。
3. スキル `bicameral-start` を保存する。リポジトリの場所を `H:\BicameralEngine` 以外にしたら、スキルの中のパスを直す。

## 毎回の始め方

1. 通常の PowerShell(管理者ではない)でランナーを起動:
   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File H:\BicameralEngine\runner\runner.ps1
   ```
   初回に固定の鍵が `%USERPROFILE%\.bicameral-runner\key.txt` に作られる(以後ずっと同じ)。
2. デスクトップアプリで、プロジェクトの新しいチャットに「続き」と送る。
   フォルダ 2 つ(リポジトリと鍵フォルダ)の接続許可が出るので承認する。鍵を貼る必要はない。
3. 使わないときはランナーのウィンドウを閉じる(閉じている間は、どのチャットからも実行できない)。
   鍵を作り直すときは `runner.ps1 -RotateKey`。

定型文の一覧: [docs/claude/prompts.md](docs/claude/prompts.md)

## Visual Studio 2026 で開く

「フォルダーを開く」でこのフォルダを選ぶ → 構成で `debug` を選ぶ → スタートアップ項目 `bicameral.exe`、引数 `--caps`。

## 動作環境

Windows 11 / DX12 Ultimate(SM 6.8・Work Graphs・DXR 1.1・Mesh Shader)対応 GPU / Visual Studio 2026
