# T-0112 ほぼ同じ頁を畳む・端数の枠を返すを GPU に

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: M2(docs/plan/ROADMAP.md)
- 設計: 17 §5「ほぼ同じ頁を畳む」 / 決定: D-428・ADR-0015 追記(T-0104)

## 目的(1〜2 行)
T-0104 の CPU リファレンス(multires_activity.cpp の FoldQuietPages の許容差つき)を GPU の TreeFoldCheck → TreeFold に載せ、毎刻み CPU とビット一致させる。

## 完了条件(チェックできる形で)
- [x] 許容差をルート定数か定数バッファで渡す(ルート定数 1 語に詰めた。MrPackFoldTolerance。63 / 64 語)
- [x] TreeFoldCheck: ちょうど静かになったブロックの集計(MrFoldStats。1 グループ = 1 ブロック。ウェーブで最小・最大・和を縮約。1 スレッドで 512 セルは数 ms かかった)
- [x] TreeFold(1 グループ・枠の順): 頁と端数の枠を空きへ。平均の値・余り・端数は TreeFoldCheck が書く(帳簿は繰り上げつきの atomic。順によらず CPU とビット一致)
- [x] 鎖の場面(--residue と同じく畳まれるまで)と CheckNearFoldUnit 相当で HW・WARP が CPU と毎刻みビット一致。許容差なしなら今までと同じ

## メモ・参考
- 帳簿の足し算は他の段では atomic(桁あふれを繰り上げない)。畳む段は 1 グループで順に足すので繰り上げられる。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-06: GPU に載せた。TreeFoldCheck が端数を帳簿へ(スレッド = セル)・ほぼ同じかをウェーブの縮約で集計(最初の 1 スレッドで 512 セルの版は 7.5 ms → 0.047 ms)して平均を書き、
  TreeFold が枠の順の累積和で頁と端数の枠を積む。帳簿は繰り上げつきの atomic(順によらない)。許容差はルート定数 1 語(MrPackFoldTolerance)。
  テスト: gpu_multires_uniform(許容差つきの活性の場面・ほぼ同じ頁の 1 回)と新しい gpu_multires_near_fold(--near-fold。鎖を 2200 刻み、頁が全部畳まれるまで)が HW・WARP で毎刻み CPU と一致。
