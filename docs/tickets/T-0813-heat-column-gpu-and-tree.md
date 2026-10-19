# T-0813 熱の列(陽解法の安全網)を GPU の気体の段と木へ

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ③' 運ぶ計算の共通の土台 段 3 ④ の残り(T-0787 から分けた)
- 設計: docs/design/20-transport-substrate.md §2.4 ② / 決定: D-508・D-524・ADR-0355

## 目的(1〜2 行)
T-0787 で CPU の箱(gas_reference.cpp の LimitMovedEnergy)に入れた「当てる段の上限の熱の列」を、GPU の箱の段(shaders/sim/gas_step.hlsl)・
CPU の木(gas_tree.cpp)・GPU の木の段にも同じ式(gas_flow.hlsli の GfMovedHeatPart・GfHeatRoom + transport_apply の TxMakeLimit)で入れる。

## 完了条件(チェックできる形で)
- [ ] GPU の箱: 強い爆発(1 セル 40 倍・2600 K。gas_reference_test の MakeBlastBox と同じ)で CPU の箱とビット一致・縮めた面の数も一致
- [ ] GPU の気体の試験(gpu_gas_*・gpu_probe_gas・gpu_multires の気体)がそのまま通る(ふつうの場面では縮めない)
- [ ] gpu_probe_gas_test の MakeOptions の `.limitMovedHeat = false`(T-0787 の仮)を外し、燃える木箱(約 440 気圧の爆発で列が縮める)も CPU と一致
- [ ] CPU の木・GPU の木も同じ(木は粗い風上が何度も引くので「出した熱」を taken と同じく小刻みをまたいで数える)
- [ ] 縮めた面の数を GPU の段からも読み戻せる(帳簿の欄か数える欄)

## メモ・参考(T-0787 の作業役の見立て)
- 今の GPU の箱の段: 段 7(セル: 出ていく成分の和 OutgoingAddress)→ 段 8 GasFaceFinal(面: 成分を抑え、塊のエネルギーと運動量・帳簿)→ 段 9 GasTransfer。
  熱の列は「抑えた後の成分で塊のエネルギーを出す」→「セルで熱のぶんの和と倍率」→「面で割合と成分を縮めてエネルギーと運動量・帳簿」の 3 段が要る:
  GasFaceFinal を (a) 成分を抑えて熱のぶんを面に書く と (c) 倍率を当ててエネルギー・運動量・帳簿 に分け、間に (b) セルの段(6 面の熱のぶんの和 →
  TxMakeLimit(GfHeatRoom(力の後の熱), 和) の出る側の倍率を g_sums へ)を足す。帳簿の物質量の加算は (c) へ移す(縮めた後の量で)。
- CPU の箱の順(gas_reference.cpp の Substep): LimitOutflow → ComputeMovedAmounts → LimitMovedAmounts → LimitMovedEnergy → Transfer。
- ふつうの場面では縮めないので、入れる前後で GPU の値はビット単位で同じのはず(違ったら式の写し間違い)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
