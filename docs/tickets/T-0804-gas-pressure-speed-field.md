# T-0804 最速を選ぶ欄: 環境の gas_pressure_speed(D-526 ②)

- Status: Done(2026-10-19。欄の名前と値の綴りは仮 = Q131)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ③' 運ぶ計算の共通の土台(T-0788 から分けた)
- 設計: docs/design/20-transport-substrate.md §1.1 / 決定: D-519・D-526・ADR-0353(Accepted)

## 目的
気体の圧力の伝わる速さを、世界ごとに環境の 1 行で選べるようにする(既定は本物の音速のまま = D-519)。
最速を選んだ世界は、ベイク(表と世界の置き方)でも T-0788 の検査(gas_fastest_check)で小さい空きの組を警告する(D-526 ①)。

## 完了条件
- [x] 型 data/types/bicameral.d.luau の Environment に `gas_pressure_speed`・読み込み(script/environment_package)・sim::WorldPhysics::gasPressureSpeed
- [x] 誤りは名前付きで止める(`"sound"` / `"instant"` 以外の文字・数・表)・書かない世界は今とビット単位で同じ(表の版も同じ)
- [x] 最速の世界: 仮の世界の CPU(ProbeGasField)が最速の投影で解く・ベイクが置いた世界の気体の区画を同じ関数で検査して警告
- [x] 多重解像度と GPU の気体(陽解法だけ)は本物の音速のまま流し、「最速はまだ効かない」と知らせる(黙って無視しない)
- [x] 試験・release 警告 0・archmap --check

## 作ったもの
- `environment.gas_pressure_speed = "sound" | "instant"`(書かない = sound)。sim::GasPressureSpeed(world_physics.h)。
- ProbeGasField: 表の環境が instant なら、options が陽解法でも「options の投影の設定 + 音速 0」で解く(C++ で最速の投影を選んだのとビット単位で同じ)。
  刻みごとに表から読み直す(ホットリロードで替わると、その刻みから最速・検査を初めから)。
- CheckLayoutFastestOpenings(table)(sim/gas_fastest_layout。GPU を知らない): 世界の組み立ての気体の区画 + world_cells の初めの中身の空きで
  FindGasSmallOpenJoints(刻みの中と同じ関数・同じ文 ProbeRegionJointWarning・空きは CollectProbeGasOpen を ProbeGasField と共有)。
  読み込み(LoadReactionTable・RebuildReactionTable)がベイクの後に呼び、表の warnings に足す
  (BakeReactionTable の中からは呼べない: bicameral_reaction は気体の層より下。cell_volume.cpp を bicameral_gas_projection へ移した)。
  コードで BakeReactionTable を直接呼ぶ表には付かない(試験は関数を直接呼ぶ)。
- GasInstantNotYetNotice(world, where)(頭 `最速はまだ効かない: `): MultiresGas::Create(Notices() とログ)・GPU の ProbeSim::CreateGas(ログ)。
- 試験: environment_package(読み込み・誤り・知らせの文)・cell_volume(TestWorldChoosesInstant: C++ の最速と同じハッシュ・同じ警告・差し替えの刻みに警告 /
  TestBakeWarnsInstant: 1/16 の壁は警告・音速と 1/2 は無し)・multires_gas(TestInstantNotYet)。

## 判断待ち(仮で進めた。Q131)
1. 欄の名前と綴り `gas_pressure_speed = "sound" | "instant"`(design/20 §1.1 は「音速 or "instant"」とだけ)。
2. 環境の instant は C++ の ProbeGasOptions の選択より強い(陽解法を選んだ区画でも最速)。音速を書いた投影を C++ で選んだ区画も音速 0 になる。
3. 多重解像度・GPU の世界は止めずに本物の音速で流し、作った時に知らせる(代案: 誤りで止める)。

## 残したもの
- GPU の仮の世界のホットリロードで instant に替えた時の知らせは無い(作った時だけ)。GPU と MR に投影が載る時(T-0621・T-0531)に、検査ごと載せる。

## 作業ログ
- 2026-10-19(本体・作業役): 欄・読み込み・型・仮の世界の CPU の切り替え・ベイクの警告・MR/GPU の知らせと試験 3 本。
