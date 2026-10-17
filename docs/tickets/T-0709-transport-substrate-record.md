# T-0709 運ぶ計算の共通の土台の記録(③' 段 0)

- Status: Done
- 種類: 設計
- PC: 必須(マージと試験)
- 見積もり: チャット 1 回分
- マイルストーン: ③' 運ぶ計算の共通の土台(docs/plan/ROADMAP.md)
- 設計: docs/design/20-transport-substrate.md / 決定: ADR-0326・D-509〜D-515

## 目的(1〜2 行)
2026-10-17 にユーザーが決めた「運ぶ計算の共通の土台」(設計への 6 つの答え)をリポジトリに記録し、止めていたブランチの扱いを決定どおりに片付ける。

## 完了条件(チェックできる形で)
- [x] ADR-0326(Accepted)・docs/design/20(01 の地図に 1 行・07 §2.7・§2.8・§3.1 に置き換えの注)
- [x] DECISIONS D-509〜D-515・D-501 に「D-511 の縦切りで見直す」
- [x] ROADMAP ③'(段 0〜9・T-0709〜T-0756)・消える/吸収される行の注
- [x] QUESTIONS: Q109 取り下げ・土台で消える/変わる Q に注・Q113(T-0622 の仮)
- [x] ADR-0310 を Withdrawn・ADR-0256・0266・0312・0316 に注
- [x] t-0414b(T-0697)と t-0622(T-0622)を main へ。t-0689・t-0668・t-0686 はマージしない(ブランチは残す)
- [x] main の上で release のビルド(警告 0)・CPU の全部の試験・gpu_reaction・gpu_probe_place・archmap --check

## メモ・参考
- 公開リポジトリなので、設計の例 5 の物質名は架空の一般名(drift_mist・seep_stone・元素 Xa)にした(D-006)。風(`wind_mm_per_s`)は D-514 で外した。
- 設計の材料(runner/design/ の substrate-decided・survey-engine・survey-author)は git 管理外。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-17(本体): t-0414b・t-0622 を main へ(衝突なし)。release ビルド警告 0(1 回目はリンクのファイルの掴み合いで落ち、作り直しで通った)・CPU 73 本・GPU 12 本・archmap OK。
  ADR-0326・20・D-509〜D-515・ROADMAP ③'・QUESTIONS・HANDOFF・NEXT・COMMANDER を書いた。
