# T-0713 セルの見立て(運ぶ計算の共通の土台の段 1 ③)

- Status: Done(2026-10-17。置き換えは T-0714)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 土台の段 1(runner の設計 substrate-decided §6。ROADMAP への反映は本体)
- 設計: 土台 §2.2・§3 / 決定: 2026-10-17 ユーザーの決定 1〜6(未反映)・ADR-0333(Proposed)

## 目的(1〜2 行)
流れが読むセルの値(相ごとの量・空き・熱・伝導の係数・圧力・液体の満ち具合)を、共有 .hlsli の 1 つの関数 `MakeCellView` で刻みごとに作る形を作り、
今の値(熱のキャッシュ・MrCellThermal・cell_volume・液体と気体の区画の数え方)とビット単位で同じことを確かめる。

## 完了条件(チェックできる形で)
- [x] `shaders/common/cell_view.hlsli`: `MakeCellView(表, セル, レベル, 基準の圧力)`・1 成分ずつ足す形(CvMakeViewSums・CvAddSpecies・CvViewFromSums)・`CvCellVolumePicoliters(レベル)`
- [x] CPU の表: `engine/src/sim/cell_view.{h,cpp}`(`MakeSpeciesPhases`・`CellViewTable`。相の表はここ 1 か所)
- [x] HLSL でコンパイルできる(`shaders/sim/cell_view_check.hlsl`。実行しない。GPU の段が呼んだら消す)
- [x] `cell_view_test`(release): いろいろなセル 3009 × レベル -3・0・2・5 と、MR の CPU の木(鎖の場面を伝導で 20 刻み。7168 セル・レベル 0〜6)と、
      木箱の根のセル・溢れの形のセル(RxWideCell。9 成分)で、MrCellThermal・RxComputeThermal・MrFaceLimit・HcMakeCache(レベル 0)・ShareCellVolume・
      LiquidOpenQ16・GasOpenQ16・LiquidMass・LiquidVolume・ReadState の式・DeriveOpenCell の圧力の式とビット一致
- [x] 費用を docs/perf.md に(CPU。GPU は未確認)
- [x] release のビルド 警告 0・archmap --check

## メモ・参考
- **相の表の合わせ方(T-0710 と)**: wt2 の t-0710 にはまだ `phase` の形が無かった(読んだ時点で reaction_table.h は今のまま)。見立ては相を
  `CvSpeciesPhase { molarVolume, molarMass, phase(gas 0 / liquid 1 / solid 2) }` の 1 本で読み、作るのは `sim::MakeSpeciesPhases` の 1 か所
  (今は speciesCondensed・speciesLiquid から)。T-0710 が入ったら、ここをベイクの phase から作るように直すだけ(見立てと試験は変えない)。
  phase の値の並び(gas / liquid / solid)を T-0710 と同じにすると写すだけになる(本体がマージの時に確かめる)。
- **wt6(T-0716。液体の芯を共有 .hlsli へ)が呼ぶ形**: `CvCellView` の `liquid.mass`(ng)・`liquidMass`(mg)・`liquid.volume`・`liquidOpen`・`liquidRoom`・
  `liquidFill`・`liquidEquivalentMass`・`heat`・`heatCapacity` が probe_liquid の ReadState(LiquidCellState)の mass・volume・open・room・equivalent・heat・capacity と
  同じ値(wall = liquidOpen == 0)。CvSpeciesPhase は ProbeLiquidSpecies と同じ 16 バイト(並びは molarVolume・molarMass・phase/flags)。
  液体の柱の「上を支える割合」は隣が要るので見立てに入れていない(液体の種類の側)。
- **気体の圧力の温度はセル全体の温度**(区画の気体は気体のぶんのエネルギーから気体だけの温度を出す。凝縮相の無いセルでも運動のエネルギーを引く所が違う)。
  式は同じで、試験は「同じ物質量・温度・空きなら同じ値」を確かめた。置き換え(T-0714・段 3)で基準を取り直す。ADR-0333 ⑥。
- **レベル**: 物質量がレベルの単位なので、体積・空き・満ち具合・圧力はレベルによらない(試験でレベル -3〜5 が同じ値)。0.125 m³ の定数は
  `CV_CELL_VOLUME_LEVEL_UNITS`(レベルの単位)と `CvCellVolumePicoliters(k)`(本当の pL)に分けた。
- 入れていないもの: 下の支え・孔の詰まり・吸い上げ(隣と物質の欄が要る。段 9)・運動のエネルギー(登録の欄 T-0711 の後)・欄から速さ。
- 費用(CPU release・1 スレッド): 見立て 545 ns/セル・今の別々の数え方 343 ns/セル(見立ては圧力・相ごとの和・液体の割り算も作る。T-0714 で置き換える時、
  1 刻みに 1 回作れば今の 2〜3 回の数え直し〔伝導・気体・液体〕より安い見込み。未確認)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-17(wt5 の作業役): cell_view.hlsli・sim/cell_view・cell_view_check.hlsl・cell_view_test を足した。release のビルド 警告 0・cell_view_test 通過
  (3009 セル × 4 レベル + MR の木 7168 セル + 溢れの形)。費用を perf.md に。ADR-0333(Proposed)。置き換えは T-0714。
