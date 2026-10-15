# T-0597 気体の比熱を定圧で書く選択をパッケージから書けるようにし、既定を決める

- Status: Todo(ADR-0259 のユーザーの答え待ち)
- 種類: 工学 + 設計
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ROADMAP の T-0210(T-0591 から分けた)
- 設計: docs/design/07-transport.md §2.7 / 決定: ADR-0259(Proposed)

## 目的(1〜2 行)
T-0591 で足した `ReactionTableDefinition::gasHeatCapacityAtConstantPressure`(気体の c_v = c_p − R をベイクが作る)は C++ からしか書けない。
ADR-0259 の答え(案 A 既定を true / 案 B 今のまま / 案 C 表ごとに宣言)に合わせ、パッケージに書く場所と real_matter の値を決める。

## 完了条件(チェックできる形で)
- [ ] 案 A か C: パッケージの書き方(例: 分類 species の外の表の欄)・型(data/types/bicameral.d.luau)・reaction_package の読み込み・試験
- [ ] 案 A: real_matter を定圧で読む。凝縮相の condensed の書き忘れを探す(ベイクが R 以下を断る)。場面(草原の延焼など)と GPU の試験の期待値を測り直す
- [ ] CPU リファレンスと GPU のビット一致(表が同じなので変わらない見込み)・保存則の検査

## メモ・参考
- ベイクの実装は engine/src/sim/reaction_table.cpp の BakeSpecies(H0 は書いた c_p から。298.15 K の内部エネルギー = 生成エンタルピー − RT)。
- 試験: tests/probe_gas_test.cpp の TestHeatCapacityAtConstantVolume。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
