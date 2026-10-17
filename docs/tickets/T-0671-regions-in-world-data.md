# T-0671 区画(気体・液体)を世界のデータに書いて、アプリで載せる

- Status: Done(サンプルの世界の区画と煙の画像は分けた。下の「残り」)
- 種類: 工学 + 設計
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ROADMAP の T-0210(T-0620 から分けた)・⑤ の T-0414・T-0415 の下支え
- 設計: docs/design/07-transport.md §2.7 / 決定: ADR-0172(世界の組み立て)・ADR-0300(仮)・ADR-0310(仮。書き方)

## 目的(1〜2 行)
区画(気体・液体)をパッケージ(世界のデータ)に書き、アプリ(--world sample など)が ProbeSimOptions::gas・liquid に載せて窓で流れるようにする。
今はアプリに区画を載せる道が無い(試験だけが ProbeSimOptions で作る)。

## 完了条件(チェックできる形で)
- [x] 書き方を決める(仮 = ADR-0310: world_cells の隣の分類 `gas_regions`・`liquid_regions`。下の判断待ち 1)。
      書かなければ流れない(D-455)。2 つ目・格子からはみ出す・基準の大気が無い(気体)・重力が 0(液体)・綴りは読む時の誤り(区画の名前の付いた理由)
- [x] WorldLayoutDefinition → BakeWorldLayout(環境の値と基準の大気の後へ移した)→ WorldLayout::gasRegion・liquidRegion →
      `sim::SetLayoutRegions` が ProbeSimOptions へ(frame_loop の CreateSimulation・WorldCopyFactory・PlayMetricFactory・--measure-metrics・
      PlayMetricMeasurer〔元の世界と写しに区画を渡していなかったのも直した〕)。起動のログに区画の箱とセルの数
- [~] ⑤ の場面(data/scenes/grass_fire_river/flow_regions.luau)に 川の水 = 液体の区画(4 × 5 × 64)・川と両岸の上の空気 = 気体の区画(20 × 8 × 32)。
      **サンプルの世界(combustion_test)には書かない**(--auto-lab などサンプルの世界の窓の試験は CPU リファレンスと比べ、区画を入れると CPU 側にも要る。
      重さも全部の窓の試験に乗る)→ 残り 1
- [x] 基準の大気が無い世界(T-0590 の判断待ち 5): 仮 = 気体の区画を読む時に断る(ADR-0310 の 2)
- [~] アプリの画像: 場面の写し(川の手前 x 36 に 1200 K の草の帯を足した写し。runner の外へは出さない)を 1500 フレーム流した立体の表示(温度)は
      撮れた(区画の上の空気が温まって広がる)。水と湯気の見分けは既定の表示では付かない(温度だけ。C キーの色分けを起動の引数で選べない)→ 残り 2

## メモ・参考
- script/world_layout_package が world_cells を名前のまま読む。区画の大きさの上限(気体は 1 刻み 1〜2 ms/3456 セル。D-468)を読む時に知らせる。

## 判断待ち(ユーザー。仮で進めたもの)
1. **区画の書き方**(作り手から見た違い。ADR-0310):
   - 案 A'(仮で実装): world_cells の隣の分類 `gas_regions.<名前> = { min_cell, size_cells, boundary? }`・`liquid_regions.<名前> = { min_cell, size_cells, advection? }`。
     セルの中身(world_cells)・置く物(world_bodies)と同じ「世界の組み立て」の書き方(鍵 = 名前・Mod は鍵を足す)。場面のパッケージに数行。
   - 案 A(T-0620 の元の案): world_cells の中に gas_regions・liquid_regions の鍵。world_cells の鍵はセルの領域の名前なので、同じ表に別の種類の鍵が混ざる
     (「gas_regions」という名前の領域が書けなくなる)。
   - 案 B: environment に範囲 / 案 C: 起動の引数(採らない)。
   - おすすめ: A'(取り消しやすい。名前を変えるだけで A に寄せられる)。
2. **区画の数**: 今は 1 つずつまで(ProbeSim の区画が 1 つ)。川と池・洞窟の水など場所が離れた区画を書きたいなら、区画を複数にする(費用は区画ごとの作業場と段)。
   ⑤ の 2 つ目の場面(氷が溶けて流れる)で要るかを見てから。
3. **気体の区画の重さの知らせ**(D-468): 今は起動のログに箱とセルの数を出すだけ。読む時に「1 刻み 〇 ms の見込み」を警告するかは測ってから
   (この場面 5120 セル + 液体 1280 セルで GPU 2.7〜2.9 ms/投入・等速の予算を超えた刻み 0)。

## 残り(分けた・引き継ぎ)
1. サンプルの世界の木箱を囲む気体の区画(--auto-lab の CPU の一致に区画を入れる・窓の試験の重さ)→ T-0685
2. 水と湯気の見え方(既定の表示は温度だけ。起動の引数で色分けする量を選ぶ・湯気〔水蒸気〕を枠に足すか)→ T-0686

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-17(wt5): 区画を世界のデータに書いた(分類 gas_regions・liquid_regions。読む = world_layout_package・誤り = BakeWorldLayout・
  載せる = sim::SetLayoutRegions を世界・写し・指標の写しの全部で)。⑤ の場面に川の液体の区画と上の空気の気体の区画。world_layout_test に
  区画の読み・誤り(2 つ目・はみ出し・綴り・大気なし・重力 0)、standard_packages_test に場面の区画。ADR-0310(仮)。分けた: T-0685・T-0686。
