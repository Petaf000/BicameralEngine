# T-0597 気体の比熱を定圧で書く選択をパッケージから書けるようにし、既定を決める

- Status: Doing(ユーザーの決定 F 2026-10-17 = 案 A。DECISIONS へは本体が D-501〜D-506 として書く)
- 種類: 工学 + 設計
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ROADMAP の T-0210(T-0591 から分けた)
- 設計: docs/design/07-transport.md §2.7 / 決定: ADR-0259(Accepted。追記 1)

## 目的(1〜2 行)
T-0591 で足した `ReactionTableDefinition::gasHeatCapacityAtConstantPressure`(気体の c_v = c_p − R をベイクが作る)は C++ からしか書けない。
ADR-0259 の答え(案 A 既定を true / 案 B 今のまま / 案 C 表ごとに宣言)に合わせ、パッケージに書く場所と real_matter の値を決める。

## 完了条件(チェックできる形で)
- [x] 案 A: 既定を true(`ReactionTableDefinition::gasHeatCapacityAtConstantPressure`)。書く場所 = `heat_capacity_mj_per_mol_k` の意味(型の注に「気体は c_p」)。
      パッケージの切り替えは置かない(仮。下の判断待ち 1)
- [x] real_matter を定圧で読む。凝縮相の condensed の書き忘れ: 標準パッケージ・combustion_test・grass_fire_river の全部の物質を見た(凝縮相は全部 condensed か phase。ベイクは断らない)
- [x] 坂の検査を定圧の比熱で見る(sim/reaction_slope の SpeciesPressureHeatCapacity。気体 = c_v + R)。坂の警告と静的な一覧は変わらない
- [ ] 数の動いた試験と理由(下)・燃える木箱の幅(T-0491・Q63)
- [ ] CPU リファレンスと GPU のビット一致(表が同じなので変わらない見込み)・保存則の検査

## メモ・参考
- ベイクの実装は engine/src/sim/reaction_table.cpp の BakeSpecies(H0 は書いた c_p から。298.15 K の内部エネルギー = 生成エンタルピー − RT)。
- 試験: tests/probe_gas_test.cpp の TestHeatCapacityAtConstantVolume。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
