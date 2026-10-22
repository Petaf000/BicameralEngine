# T-1028 木の要求が細かい所の規則を読む形を GPU とゲームの道へ(T-1008 から分けた)

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: 試作の列 1(C-1 の後)
- 設計: docs/design/20-transport-substrate.md §11.1 / ADR-0436 / 元: T-1008

## 目的
T-1008 は CPU の `SubmitGradedRefineRequests`・`FilterGradedCoarsenRequests`(種からの距離で規則を読む)を作った。GPU の要求の段に同じものを入れ、ゲームの道で呼ぶ。

## 完了条件
- [ ] GPU(GpuMultires の RecordQuietRequests の後)で同じ 2 つ。multires_graded_test の場面で CPU と毎刻みビット一致(木のハッシュ)
- [ ] ゲームの道(本体のループ)で世界のデータ fineRegion を渡して呼ぶ
- [ ] 20 km の木の実際の葉の数を docs/perf.md の見込み(T-0954)と比べる

## 作業ログ
