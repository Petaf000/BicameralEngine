# T-0719 気体の陽解法の芯を共有 .hlsli へ(土台の段 3 ①)

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 運ぶ計算の共通の土台 段 3 ①(docs/plan/ROADMAP.md ③'・ADR-0326)
- 設計: docs/design/20-transport-substrate.md §2.3・§2.7・§4 / docs/design/07-transport.md §2 / 決定: D-509・ADR-0326・ADR-0043

## 目的(1〜2 行)
気体の陽解法の固有の部分(導く値・力積と重さ・質量流束・MUSCL と乱数の丸め・pdV・音速の頭打ち)を shaders/common/gas_flow.hlsli の 1 つの源にし、
CPU(gas_reference.cpp・probe_gas.cpp)と GPU(gas_step.hlsl・probe_gas_tick.hlsl)が同じ関数を呼ぶ(heat_conduction.hlsli・liquid_flow.hlsli と同じ形)。値は今とビット単位で同じ。

## 完了条件(チェックできる形で)
- [x] release のビルド(警告 0)
- [x] gas_reference・gas_projection・probe_gas(fire・dew 組)が通る(CPU)
- [x] gpu_gas・gpu_gas_flow・gpu_gas_blast・gpu_gas_species・gpu_probe_gas が通る = CPU と GPU が毎刻みビット一致
- [x] archmap --check

## メモ・参考
- 置き場所と名前は wt6 の T-0716(liquid_flow.hlsli・`Lq`・`bicameral::liquid`)にそろえた: `shaders/common/gas_flow.hlsli`・接頭辞 `Gf`・C++ の名前空間 `bicameral::gas`。
- 入力の分け方(T-0720 の MR の 1 段の気体がそのまま呼べるように):
  - セルの見立て `GfTotals`(物質量・熱容量・化学・質量の和。`GfAddSpecies` で 1 成分ずつ)と導く値 `GfDerived`(`GfDeriveRaw` / `GfDerive`。空き open は引数)。
    T-0713 の `MakeCellView` ができたら、その気体の相の分を `GfTotals` と空きに渡すだけにする。
  - セルの欄(エネルギー・運動量 ×3): 関数は値で受けて値で返す(置き場は呼ぶ側。後で登録の欄)。
  - 見立ての値 `GfParams`(係数 6 つ・α・頭打ちの温度・重力・MUSCL・乱数の丸め・断熱・空きを読むか)。係数はレベルの関数にできる
    (今は 0.125 m³ のセルの定数 GF_PRESSURE_PER_AMOUNT_TEMPERATURE_Q32 が式に入っている。MR では体積をレベルの関数にする所 = T-0720 で見る)。
- 共有した関数: GfDeriveRaw / GfDerive(温度・本物の圧力・空きの体積・p̃ と音速の頭打ち)・GfKineticEnergy・GfExcess・GfDrivingPressure・GfFaceWeight・
  GfMirrorImpulse / GfSharedImpulse / GfPartialImpulse・GfFaceFlow・GfRawFraction・GfEffectiveOutflow / GfLimitFraction / GfLimitMovedAmount・
  GfLimitedSlope / GfSpeciesRatio / GfHalfSlope / GfMovedAmountQ32・GfFaceHash / GfRoundAmount・GfMovedMomentum / GfMovedEnergy(pdV)・GfGravityWork・
  仮の世界の glue の GfGasEnergyPart・GfMomentumAfterMassChange・GfKineticMilliJoule。
- 周りの決まり(箱・面の列挙・壁と開いた境界・帳簿・区画・出ていく上限の集め方)は呼ぶ側のまま(段 1 ④ の当てる段へ移すのは後)。
  出ていく上限の換算と欄が物質に付くは式だけ共有。D-508 の力積の上限はコードにまだ無い(入れる時は gas_flow.hlsli に)。
- gas_cell.hlsli は GPU の欄の並び(GasGpuCell・GasGpuDerived・GAS_P・GAS_SPECIES)から gas_flow.hlsli を呼ぶ薄い包みにした(式は持たない)。
  凍結の気体の木(gas_tree.cpp・gas_tree_step.hlsl)は触っていない: 木が借りる gas_detail の部品・gas_cell.hlsli の名前はそのままで、中身が gas_flow を呼ぶ。
- 触らなかった: セルの気体の空き(probe_gas_tick.hlsl の GasOpenOf と sim/cell_volume の ShareCellVolume)は T-0713 の見立てに任せる。投影(gas_projection)は CPU だけ。
- 二重書きの減り(空行・コメントだけの行を除くコード行): gas_reference.cpp 881 → 780(−101)・probe_gas.cpp 368 → 353(−15)・
  gas_step.hlsl 569 → 511(−58)・probe_gas_tick.hlsl 442 → 418(−24)・gas_cell.hlsli 148 → 79(−69。式は 0)。共有 gas_flow.hlsli は 343 行(式は 1 か所)。
  CPU と GPU で同じ式を 2 回書いていた所は 0(残る CPU/GPU の違いは回し方・欄の読み書き・帳簿の足し方だけ)。
- 確かめ方: 移す前と後の gas_reference.cpp を、48 の乱数の場面(境界 3 種・MUSCL 3 種・断熱/等温・乱数の丸めの有無・空き・成分 6 で外の配列・生成のエネルギー)で
  20 刻み回し、ハッシュと帳簿が全部一致(コンテナの g++)。glue の 3 式は 300 万回の乱数入力で旧の式と一致。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-17(wt4・作業役): shaders/common/gas_flow.hlsli を作り、gas_reference.cpp・probe_gas.cpp・gas_step.hlsl・probe_gas_tick.hlsl の式を全部そこへ寄せた。
  gas_cell.hlsli は GPU の包みに。HLSL では shared が予約語(GfPartialImpulse の引数は fullImpulse)。
  試験(release): gas_reference・gas_projection・probe_gas・gpu_gas(_flow/_blast/_species)・gpu_probe_gas が通る。警告 0。archmap OK。
  次(T-0720): MR の 1 段の気体は、T-0713 の見立てから GfTotals と空きを作り、GfParams の係数をレベルの値にして同じ関数を呼ぶ。
