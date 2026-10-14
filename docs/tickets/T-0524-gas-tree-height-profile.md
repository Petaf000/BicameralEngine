# T-0524 気体の木(多重解像度)の層の基準を高さの分布で作る

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ROADMAP「今の順」②(T-0210 の気体を世界に載せる段と一緒に)
- 設計: docs/design/07-transport.md §2.5・ADR-0182 の改訂 / 決定: D-490 ②・D-494
- 分けた元: T-0499(1 レベルの箱の層の基準は高さの分布で作れるようにした。木は一様な形のまま)

## 目的(1〜2 行)
`MakeGasTree`・`SetGasTreeEnvironment` の層の基準(レベル k の層の厚さ 2^-k)を、基準の大気の高さの分布(`sim::SampleAtmosphere` を
各レベルの層の高さで読んだ温度と成分)から作る。床の圧力は `sim::AtmospherePressureMicroPascal`。

## 完了条件
- [ ] 木の各レベルの層の基準が層ごとの温度と成分で作れる(一様なら今とビット一致)
- [ ] 高さの分布の静止大気が、どのレベル・どの重力でも動かない(gas_tree_test)・GPU の木とビット一致(gpu_gas_tree_test)
- [ ] 環境の値のホットリロード(SetGasTreeEnvironment)も同じ道

## メモ・参考
- 箱の床の世界の高さ(海面から)を気体の箱が知る必要がある(T-0210 で世界の座標と一緒に決める)。
- 1 レベルの版は `MakeHydrostaticReference(config, std::span<const GasReferenceLayer>, pressure)`(T-0499)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
