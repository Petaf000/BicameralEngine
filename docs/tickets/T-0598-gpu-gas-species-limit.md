# T-0598 GPU の気体の段の成分の上限 4 を外す(セルの成分の並びで回す)

- Status: Done
- 種類: 工学・GPU
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: ROADMAP の T-0210(T-0591 の GPU 側を分けた)
- 設計: docs/design/07-transport.md §2.7 / 決定: ADR-0256・ADR-0258

## 目的(1〜2 行)
CPU の 1 レベルの箱は T-0591 で成分の数の上限を外した(GasPerSpecies・丸めの鍵は物質 ID)。GPU の gas_step・gas_tree_step と CPU の木(gas_tree)は
欄 4 つ(GAS_MAX_SPECIES・GasGpuCell 64 バイト)のままで、GpuGas::Create は 5 つ以上と物質 ID の鍵を断る。T-0592(ProbeSim の GPU の刻みに気体)の後で外す。

## 完了条件(チェックできる形で)
- [x] GPU の気体の段が何種でも回す(形は ADR-0284: 成分ごとの欄を「頭の並び + 成分ごとの面」の ByteAddressBuffer に・欄の数はルート定数・
      回す数は設定の speciesCount。箱の中のセルは同じ欄の並びなので MUSCL の隣の成分は欄の番号で引ける。丸めの鍵は設定〔仮の世界は物質 ID〕)
- [x] 書き換えた構造体の配列を動的な添字で読まない(T-0397)。成分のループは件の欄を直に読み書きし、成分の数のローカルの配列を作らない
      (気体の段は Compute。FX_UNROLL の決まった数の形は要らなかった)
- [x] 4 種以下の今の GPU の試験とビット一致・5 種以上で CPU と毎刻みビット一致(gpu_gas_species の six_wind・six_blast・
      gpu_probe_gas の 6 種の空気〔N2・O2・Ar・CO2・H2O・CO〕の静かな大気と燃える木箱)
- [x] 木(gas_tree・gas_tree_step)も同じ形に(CPU の木も GasPerSpecies・MakeGasTree の assert を外した。gpu_gas_tree の six_closed・six_subcycled_wind)

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-15(wt5・作業役): ADR-0284 の形で gas_step・gas_tree_step・probe_gas_tick・CPU の木の上限 4 を外した。初めは件ごと(頭 + 欄)に並べたが、
  仮の世界(欄の数 8 に使う欄 4)で気体の段が 1.64 → 2.82 ms になったので、状態と作業は頭の並び + 成分ごとの面にした(1.57 ms。一様な箱 32³ の 3 種
  10.0 → 8.4 ms・6 種 10.6 ms)。外・設定・帳簿は件ごとのまま。費用は docs/perf.md の T-0598。
- 判断待ち: なし(作り手から見た違いは「GPU でも表の気体を何種書いても流れる」だけ)。
