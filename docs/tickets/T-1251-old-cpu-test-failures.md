# T-1251 main で前から落ちている CPU の試験 3 本を直す

- Status: Todo
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
- [ ] 3 本それぞれ、落ち始めたコミットと原因を作業ログに書く
- [ ] 不具合ならコードを直す・仕様が変わって期待が古いなら期待を直し、根拠(ADR・チケット)を試験のコメントに書く
- [ ] 3 本と、直した所に関わる試験(window_lab_blank・window_lab_compare・probe_radiation・cascade_light)が release で合格

## メモ・参考
- window_lab の 300004 mK はほぼ 300 K のまま = 隣の根の木へ熱が渡っていない(放射の空の刻みを飛ばす T-1229・T-1235 や持ち越しの表 T-1164 の前から落ちていたかは未確認)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29 本体 13 回目: チケットを作った(司令塔の配り)。
