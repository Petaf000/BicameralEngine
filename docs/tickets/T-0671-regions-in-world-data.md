# T-0671 区画(気体・液体)を世界のデータに書いて、アプリで載せる

- Status: Todo
- 種類: 工学 + 設計
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ROADMAP の T-0210(T-0620 から分けた)・⑤ の T-0414・T-0415 の下支え
- 設計: docs/design/07-transport.md §2.7 / 決定: ADR-0172(世界の組み立て)・ADR-0300(仮)

## 目的(1〜2 行)
区画(気体・液体)をパッケージ(世界のデータ)に書き、アプリ(--world sample など)が ProbeSimOptions::gas・liquid に載せて窓で流れるようにする。
今はアプリに区画を載せる道が無い(試験だけが ProbeSimOptions で作る)。

## 完了条件(チェックできる形で)
- [ ] 書き方を決める(T-0620 の判断待ち 2。仮 = 案 A: world_cells に gas_regions・liquid_regions = 最小の角と大きさ〔セルの座標〕・液体は移流の有無)。
      書かなければ流れない(D-455)。区画が格子からはみ出す・基準の大気が無い(気体)・重力が 0(液体)は読む時の誤り(場所の付いた理由)
- [ ] WorldLayoutDefinition → BakeWorldLayout → WorldLayout に区画、frame_loop の CreateSimulation が ProbeSimOptions へ
- [ ] サンプルの世界(data/packages/combustion_test)に木箱を囲む気体の区画、⑤ の場面のパッケージに水の区画(中身は場面のチケットで)
- [ ] 基準の大気が無い世界(T-0590 の判断待ち 5。仮 = 区画を断る)
- [ ] アプリで燃える木箱の煙が上がる画像(T-0619 の y を上にそろえた後)

## メモ・参考
- script/world_layout_package が world_cells を名前のまま読む。区画の大きさの上限(気体は 1 刻み 1〜2 ms/3456 セル。D-468)を読む時に知らせる。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
