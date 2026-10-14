# ADR-0230 開発ビルドの坂の計器(成分込みの ΔG を整数で数える。仮の世界から)

- Status: Accepted(D-487 ② の実装)。固体の活量の欄 `condensed` と、生きた GPU の世界で数えるかは Proposed(T-0518 の判断待ち 1・2)
- 日付: 2026-10-15
- 決めた人: ユーザー(D-487 ②)。数え方と仮の扱いは Claude が提案(T-0518)

## 背景
- D-487 ② 坂を上る書き間違いは警告して動かす。ベイクで警告し、開発ビルドの計器にも出す。
- ベイクの検査(ADR-0218)は標準の ΔG° だけを見るので、一方向の規則が生成物が溜まって釣り合いを越える分(T-0491 の boudouard の 1645〜1738 K の 179 回)は捕まらない。

## 決定
1. 計器は**見るだけ**(D-465)。シミュのコード・表・ハッシュ・GPU の段は変えない。
2. 数え方は**整数**(engine/src/sim/slope_gauge): ΔG = ΔG°(T) + R T Σ ν ln a(mJ/mol)。ΔG°(T) はベイクの検査と同じ整数の表(1 K ごと)、
   ln は fixed.hlsli の FxLnU64(Q32)。温度は 1 K に切り捨て。気体の活量は分圧 / 1 bar(a = n R T / (V p°))、凝縮相は 1。
   進んだ反応ごとに ΔG > 0 を「坂を上った」、そのうち ΔG° ≤ 0 を「成分しだい」として、規則ごとに回・進行度・Σ ξ ΔG を数える。
3. 進行度は**CPU リファレンスの刻みの核をなぞって**取り出す(成分が変わったセルだけ)。CPU リファレンスは GPU とビット一致なので GPU の版は作らない。
4. 最初に付けるのは**仮の世界(ProbeSim)**。場面は遊びの指標(ADR-0206)の場面で、`bicameral --slope-gauge <刻み>` が流してログに出す
   (坂を上ったら警告。終了コードは 0)。パネル(T-0539)・生きた GPU の世界(T-0540)・多重解像度(T-0541)は後。
5. **(Proposed)** 凝縮相は物質の任意の欄 `condensed`(既定 false = 気体)。GPU の表に入らない。M2 で相が入ったら置き換える。

## 影響
- SpeciesDefinition::condensed・BakedReactionTable::speciesCondensed(CPU だけ)。型 data/types の Species に `condensed: boolean?`。
  標準パッケージ real_matter の炭・セルロースに `condensed = true`(表の版が変わる。世界は変わらない)。
- sim/reaction_slope に RuleNetTerms・RuleStandardGibbs(ベイクの検査と計器が同じ ΔG° を使う)。
- MeasurePlayMetricReference に刻みごとの見張り(PlayMetricReferenceWatch。省けば今までどおり)。
