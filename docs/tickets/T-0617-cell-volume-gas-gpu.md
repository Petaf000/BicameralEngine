# T-0617 セルの体積の空きを GPU の気体の段に(投影・木は T-0667・T-0668 へ)

- Status: Done(2026-10-17 wt4。GPU の陽解法の段まで。投影は T-0667、多重解像度の木は T-0668 に分けた)
- 種類: 工学・GPU
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ROADMAP の T-0210(T-0593 から分けた)
- 設計: docs/design/07-transport.md §2.7・§2.8 / 決定: ADR-0268(D-505)

## 目的(1〜2 行)
CPU の気体の段(probe_gas → gas_reference の GasBox::openQ16)は固体・液体が占めない「空き」を読むが、GPU の気体の段は読まなかった。
密度のある固体が気体の区画にある世界(標準パッケージ real_matter に密度を書く wt2 の作業の後)で gpu_probe_gas が CPU と食い違わないよう、同じ空きを読む。

## 完了条件(チェックできる形で)
- [x] gas_step.hlsl(1 レベルの箱の GPU)が gas_reference.cpp と同じ式で空きを読む: 空きの体積で圧力(DeriveOpenCell)・
      基準 × 空き と比べる質量の差と気体の無いセル(Excess)・一部だけ開いた面の左右の力積(PartialImpulse・impulseRight)・
      帳簿(セルが受けた力積の和)・面の流れ(開き ÷ 空き・拡散 × 開き)。空きを読まない箱(openness = 0)は今までと同じ式
- [x] 仮の世界の GPU の気体の段(probe_gas_tick.hlsl の GasGather)がセルの気体の空きを sim/cell_volume と同じ数え方で作り
      (固体と液体を別々に頭打ち)、一部のセルが 1 つでもあれば設定の openness を立てる(CPU の CollectGasOpen が空の配列を返す条件と同じ)
- [x] gpu_probe_gas に「密度のある木箱と石の床」の場面(HW 60 刻み・WARP 3 刻み): 刻みごとのハッシュ・気体の欄・帳簿が CPU と同じ
- [ ] 投影(gas_projection)が空きを読む → **T-0667**(下)
- [ ] 多重解像度の木(gas_tree)が空きを読む → **T-0668**(下)

## 作ったもの
- `GasGpuParameters` に `openness`(+ 詰め物。120 バイト)、`GasGpuFace` の後ろに `impulseRight`(88 バイト)。gas_step の t3 = セルの空き(uint32 × セル)。
  ルート署名は SRV 4 本(26 語)。`GpuGas::Create` は `GasBox::openQ16` を t3 に写す(空なら 1 語の表と openness = 0)。
- gas_cell.hlsli: `GasScalePressure`(p̃ を作る部分を分けた)・`GasScaleTowardZero`・`DeriveOpenCellFromReference`。
- `ProbeGasSpecies` にモル体積と液体の印(40 バイト)、`ProbeGasHeader` に `openOffset`(96 バイト)。
- 多重解像度の木の GPU(gas_tree_step.hlsl)は `impulseRight` を読まない(面の頭が大きくなっただけ)。

## 結果(2026-10-17・release・RTX 3070 Ti)
- gpu_probe_gas(HW): 5 場面とも毎刻みハッシュ一致・気体の欄と帳簿が CPU と同じ。「密度のある木箱と石の床」は 60 刻み一致・
  塞がったセル 140・一部だけ開いたセル 12(石の孔 4 + 木箱 8)。gpu_probe_gas_warp(3 刻み)も通る。
- 既存の GPU の気体: gpu_gas・gpu_gas_flow・gpu_gas_blast・gpu_gas_species・gpu_gas_tree・gpu_gas_projection が通る(空きを読まない箱は今まで通り)。
- 費用: 気体の段 空きを読まない 1475〜1493 µs/刻み・空きを読む 1424 µs/刻み(差は測れる大きさでない。docs/perf.md)。

## 並走との重なり
- wt6(気体の区画のエネルギー。gas_reference・probe_gas): gas_reference.cpp の式(ComputeFaceForce・ComputeFaceFlow・ApplyImpulse・DeriveOpenCell)を
  変えたら gas_step.hlsl も同じに直す(gpu_probe_gas・gpu_gas が食い違って教える)。このチケットは CPU の式を変えていない。
- wt2(real_matter の密度): 標準パッケージの固体に密度が入ると、仮の世界の GPU の気体の段は空きを読む刻みになる(このチケットで CPU と一致)。
- probe_sim_gas.cpp の見出し(openOffset)と MakeGasSpecies を触った。T-0620(GPU の区画の保存点・ハッシュ)とは見出しの並びで重なりうる。

## 分けたもの(番号は司令塔の配り T-0667〜T-0669)
| 番号 | 中身 | 種類 | 作り手から見た違い |
|---|---|---|---|
| **T-0667** | 投影の解き方(gas_projection・CPU。GPU の投影〔T-0621〕もそれに合わせる)が空きを読む: 圧力の式のセルの容量 C_i を空きの倍、面の係数 K を開きの倍、予測の流れ F* を陽解法と同じ「開き ÷ 空き」で。外側の反復の真の質量の差は 基準 × 空き と比べる | 設計 + 工学(研究の色あり: 空きの小さいセルで多重格子が収束するか) | 今は投影を選ぶと固体が気体を素通しする(ProbeGasStepReport::opennessIgnored が立つ)。T-0667 の後は投影でも岩が煙を止める |
| **T-0668** | 多重解像度の木(gas_tree・GPU の木)が空きを読む: 粗いセルの空き = 子の空きの体積の平均、粗い・細かいの間の面の開き = 細かい側の面の開き(小さい方)。木の CPU と GPU を同じ式で | 設計 + 工学 | 今は木の気体は固体を素通し。T-0668 の後は遠くの粗い所でも岩・壁が気体を止める(粗くした壁に孔が開いて見えない) |

打ち切り条件(T-0667 の研究の部分): 空きが 1/256 のセルを含む 12 × 24 × 12 の箱で、V サイクルの上限(既定)の中で許容差に入らなければ、
「空きが 1/16 より小さいセルの面は投影の式から外し(閉じた面と同じ)、陽解法の閉じ込めと同じに扱う」案に移る。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-17(wt4): gas_step.hlsl と仮の世界の GPU の気体の段に空きを入れた(CPU の式は変えていない)。openness は設定の 1 語で、
  GasPrepare が型から 0 に戻し、GasGather が一部のセルを見つけたら原子的に立てる(CPU の「空の openQ16 = 読まない」と同じ条件)。
  gpu_probe_gas に密度のある木箱(セルロース 1500 kg/m³)と石の床(満ちた石 + 真ん中 2 × 2 は半分の石)の場面を足した。投影・木は T-0667・T-0668 に分けた。
