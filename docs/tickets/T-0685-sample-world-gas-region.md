# T-0685 サンプルの世界(燃える木箱)に気体の区画を書く

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ROADMAP の T-0210(T-0671 から分けた)
- 設計: docs/design/07-transport.md §2.7 / 決定: ADR-0310(仮)

## 目的(1〜2 行)
サンプルの世界(data/packages/combustion_test)に木箱を囲む気体の区画を書き、アプリで燃える木箱の煙が上がるようにする。

## 完了条件(チェックできる形で)
- [ ] combustion_test の gas_regions(木箱を囲む 16 × 14 × 16 など。gpu_probe_rewind_test --regions と同じ箱が目安)
- [ ] サンプルの世界の窓の試験のうち CPU リファレンスと比べるもの(--auto-lab・--auto-lab-compare・gpu の実験室)が区画も CPU で流す
      (ProbeReference::EnableGas に SetLayoutRegions と同じ値)。区画の重さが窓の試験の上限に収まる
- [ ] アプリで燃える木箱の煙が上がる画像(T-0619 の y を上にそろえた後)

## メモ・参考
- T-0671 では、サンプルの世界の窓の試験(多数)と CPU の一致の試験が全部区画を流すことになるので、場面(data/scenes/grass_fire_river)だけに書いた。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
