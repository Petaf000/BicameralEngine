# T-0598 GPU の気体の段の成分の上限 4 を外す(セルの成分の並びで回す)

- Status: Todo
- 種類: 工学・GPU
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: ROADMAP の T-0210(T-0591 の GPU 側を分けた)
- 設計: docs/design/07-transport.md §2.7 / 決定: ADR-0256・ADR-0258

## 目的(1〜2 行)
CPU の 1 レベルの箱は T-0591 で成分の数の上限を外した(GasPerSpecies・丸めの鍵は物質 ID)。GPU の gas_step・gas_tree_step と CPU の木(gas_tree)は
欄 4 つ(GAS_MAX_SPECIES・GasGpuCell 64 バイト)のままで、GpuGas::Create は 5 つ以上と物質 ID の鍵を断る。T-0592(ProbeSim の GPU の刻みに気体)の後で外す。

## 完了条件(チェックできる形で)
- [ ] GPU の気体の段がセルの成分の並び(物質 ID。RxCell のインライン 8 + 溢れ)を回す。MUSCL の隣の成分は物質 ID で探す。丸めの鍵は物質 ID
- [ ] 書き換えた構造体の配列を動的な添字で読まない(T-0397)。成分の数で回すループは FX_UNROLL の決まった数
- [ ] 4 種以下の今の GPU の試験とビット一致(鍵が欄の番号の箱)・5 種以上で CPU(probe_gas)と毎刻みビット一致
- [ ] 木(gas_tree・gas_tree_step)も同じ形に(木の CPU は MakeGasTree が 4 つまでを assert している)

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
