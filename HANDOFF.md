# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-29 / チケット: T-0000(プロジェクトの立ち上げ)

## 状態

- リポジトリの骨格・ランナー・運用ルールができたところ。**まだ一度もビルドしていない。**
- engine/ は `--caps`(GPU の対応状況表示)だけの最小構成。

## 動いているもの

- なし(未ビルド)

## 壊れている/未確認のもの

- CMakePresets.json + vcpkg(directx-headers)の configure が通るか未確認
- VS2026 同梱 vcpkg を _vsenv.ps1 が見つけられるか未確認
- caps.cpp のビルド未確認(OPTIONS21 の定義が vcpkg の directx-headers にあるはず)

## 次にやること

NEXT.md の先頭 → T-0001

## 注意

- ランナーは固定鍵(%USERPROFILE%\.bicameral-runner\key.txt)。Claude が鍵フォルダを接続して自分で読む。
