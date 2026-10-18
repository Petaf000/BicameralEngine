# T-0775 GPU の気体の段に粘りと静止の丸め → 標準パッケージに既定の値(T-0758 から分けた)

- Status: Done(2026-10-18)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: ③' 運ぶ計算の共通の土台 段 4(T-0758 の続き。ROADMAP)
- 設計: docs/design/20-transport-substrate.md §2.3・§2.6 / 決定: D-516・ADR-0332(Proposed)・Q121

## 目的(1〜2 行)
T-0758 で CPU の気体(gas_reference・仮の世界と MR の 1 段)に入れた粘り(運動量の拡散)と静止の丸めを、GPU の気体の段にも入れ、CPU とビット一致にする。
その後で標準パッケージ earth_environment に `gas_eddy_viscosity_mm2_per_s` の既定の値を書き、「何も書かなくても乱した空気が静まる」にする。

## 完了条件(チェックできる形で)
- [x] gas_step.hlsl・probe_gas_tick.hlsl が gas_flow.hlsli の GfViscousMomentum・GfViscousEnergy を呼ぶ(セルごとに 6 面を集める形なら atomic なし)・静止の丸め(rest_rounding.hlsli の RrShouldRest)
- [x] GpuGas::Create・ProbeSim の気体の区画が粘りの値を書いた世界を断るのをやめる(凍結の気体の木の assert も見直す)
- [x] 粘りを書いた場面で GPU(HW・WARP)と CPU がビット一致・保存の検査(gpu_probe_gas・gpu_gas 系)
- [x] earth_environment に既定の値(値は ⑤ の場面で決める。Q121 ②。仮の値で入れるならチケットに理由)と、基準の取り直し(値が変わる試験の数字と理由)
- [x] release 警告 0・archmap --check

## メモ・参考
- 既定 0 の世界は今とビット単位で同じ(T-0758)。値を書くと GPU の区画が断るのは「黙って違う結果にしない」ため(ADR-0332)。
- T-0758 の測り: 6 m 角の部屋に 1 秒の火、粘り 0.2 m²/s で 60 秒の最大の速さ 0.24 m/s → 22 mm/s。熱の残りの自然の対流は残る(D-516 の C)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-18(本体の作業役): gas_step.hlsl に粘りの 2 段(GasViscosity = セルが 6 面を集める・GasViscosityApply)。設定の頭に eddyViscosity・成分の値に viscosity
  (GasGpuSpecies 32 バイト)。仮の世界の GPU は物質ごとの粘り・粘りの段を投げるかを表から・静止の丸め(前は GPU が無視していた)を書き戻す段に・帳簿に丸めの内訳。
  断るのをやめた。earth_environment に仮の値 0.2 m²/s(気体の試験は StandardWorldPhysics = 粘り 0 なので値は変わらない。ADR-0332 の追記 1)。
  試験: gpu_gas(粘りの 3 場面を species の組に)・gpu_probe_gas(粘り + 丸め 1 m/s・粘り + 石の床)が HW・WARP で毎刻みビット一致。release 警告 0。
  費用(未確認・目安): gpu_probe_gas の適用の単位は燃える木箱 12.7 ms/刻み・粘り + 丸め 13.8 ms(場面の動きも違うので段だけの費用は測っていない)。
