# T-1251 main で前から落ちている CPU の試験 3 本を直す

- Status: Done
- 種類: 工学(小)
- PC: 必須(release の ctest)
- 見積もり: チャット 1 回分
- マイルストーン: (今の順の前提の掃除。docs/plan/ROADMAP.md)
- 設計: docs/design/23-radiation.md(radiation_reference)・パッケージ(standard_packages)・窓(window_lab)/ 決定: —

## 目的(1〜2 行)
main でも前から落ちている CPU の試験 radiation_reference・standard_packages・window_lab を、原因を決めて直す(期待のずれか不具合かを分ける)。

## 落ちている形
- radiation_reference: tests/radiation_reference_test.cpp line 394 の allZero
- standard_packages: tests/standard_packages_test.cpp line 269 の overrides
- window_lab(bicameral --world sample --editor --play --auto-lab --frames 20): 「隣の根の木が温まっていない 300004 mK」

## 完了条件(チェックできる形で)
- [x] 3 本それぞれ、落ち始めたコミットと原因を作業ログに書く
- [x] 不具合ならコードを直す・仕様が変わって期待が古いなら期待を直し、根拠(ADR・チケット)を試験のコメントに書く
- [x] 3 本と、直した所に関わる試験(window_lab_blank・window_lab_compare・probe_radiation・cascade_light)が release で合格

## メモ・参考
- window_lab の 300004 mK はほぼ 300 K のまま = 隣の根の木へ熱が渡っていない(放射の空の刻みを飛ばす T-1229・T-1235 や持ち越しの表 T-1164 の前から落ちていたかは未確認)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29 本体 13 回目: チケットを作った(司令塔の配り)。
- 2026-10-29 本体 14 回目: 原因を決めた(3 本とも試験の期待の古さ。エンジンの不具合ではない)。ブランチ t-1251-old-cpu-tests に直しをコミット(release のビルドは通った・警告 0。**試験は未実行**。会話の入れ物の再起動で止めた)。
  radiation_reference: T-1066(f55584f1・Q161 の一旦の決定)で real_matter のセルロース(11000000)と炭(40000000)が放射の吸い込みを書くようになった → その 2 つは書いた値、ほかは 0 と期待を直した。
  standard_packages: T-1164(278c9469・仮)で combustion_test が earth_environment の environment.atmosphere_profile(地面 300 K)を書き直す → その書き直しも許し、思わない書き直しを表示するようにした。
  window_lab: D-637・T-1066(熱伝導率の試験の倍率 20000 → 1)で、放射の段を持たない実験室の箱では隣の根の木が 57 刻みで 4 mK しか上がらない → 温まったと見る幅を 1 K → 1 mK に(engine/src/editor/lab_panel.cpp)。
  次: release で radiation_reference・standard_packages・window_lab・window_lab_blank・window_lab_compare・probe_radiation・cascade_light を流し、合格なら main へ。
- 2026-10-29 本体 15 回目: release で radiation_reference・standard_packages・probe_radiation(294 s)・window_lab・window_lab_blank・window_lab_compare・cascade_light(1052 s・PC が重い)が合格。main へ --no-ff。Done。
