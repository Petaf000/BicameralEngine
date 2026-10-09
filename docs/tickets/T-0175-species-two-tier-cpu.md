# T-0175 成分の二段: CPU リファレンスで 1 セルの物質の上限を無くす(T-0163 から分けた)

- Status: Done(2026-10-09。核のセルの形〔RxCell / RxWideCell〕と 1 セルの CPU リファレンスまで。多重解像度の世界〔CPU〕の溢れと粗くする和集合は T-0187 に分けた)
- 種類: 工学
- 設計: 02 §3・§6・17(R-MULTI-4)。関係: T-0163(分布の測定・世界の刻みの数える器)・T-0022(事前調査)・D-401・D-418・D-428・QUESTIONS Q18・Q19

## 目的
1 セルの成分の数の上限(RX_MAX_CELL_SPECIES = 8)を無くす最初の段。反応の核(reaction.hlsli)が「インライン K 個 + 溢れ」の
2 段のセルを読み書きできる形にし、CPU リファレンス(上限なしの可変長)で 9 種目以上の生成物が待たずに進むようにする。
GPU の頁の溢れ領域と同じ刻みのやり直しは T-0176(この段の CPU と毎刻みビット一致させる)。

## やること
1. 核の読み書きを「セルの成分を i 番目から読む・足す・消す」の小さい関数に寄せる(今は RxCell::species[]・amounts[] を直接触る所が多い)。
   インラインの K は T-0163 の測定のとおり 8 のまま(分布の結果は T-0163 の作業ログと docs/perf.md)。
2. CPU のセル = インライン K + 溢れの可変長(ID の昇順はセル全体で保つ。溢れはインラインの続き)。RxApplyExtentsHeld の待たせる形
   (RX_LIMIT_PRODUCTS)を CPU では使わない設定にする(GPU が溢れ領域を持つまでは GPU と食い違うので、試験の表の場面で分けて確かめる)。
3. 粗くする時の和集合(MrAppendCoarsened)も溢れへ。MrCoarsenFits の「断る」は CPU では起きない。
4. 試験: reaction_test の TestLimits と multires_test の TestCoarsenFull・TestLimitsStep を「待たない・断らない」版にして、保存がビット一致。

## 完了条件
- CPU で 9 種目以上の生成物・8 種ずつ違う子を粗くする場面が、待たせる・断ることなく進み、元素とエネルギーがビット単位で保存される
- 上限に当たらない場面(試験の表・燃える木箱)は今と毎刻みビット一致(GPU のテストが今のまま通る)

## 見積もり
1 チャット(2 時間)。核の関数の寄せ方で溢れそうなら、1 だけで区切る。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役): 最初に t-0132・t-0026・t-0140 を合わせた main を release で 2 束(45 本・41 本)通過(直したものなし。gpu_multires の束は変更の後に流した)。
  核(reaction.hlsli)をセルの形 Cell のテンプレートにした: 形ごとに違うのは RxHasRoomForSpecies・RxCellWithRoom・RxEmptyCellLike・RxEmptyUsage の 4 つだけ
  (使う量は RxSumUsage の引数で型を決める。RxAddedOf・RxWaitStepOf。RxAdded・RxWaitStep は RxCell の typedef なので GPU と呼ぶ側は変わらない)。
  C++ の RxWideCell(engine/src/sim/reaction_wide_cell.h。std::vector で上限なし)で 9 種目の生成物を待たずに作り、16 種のセルが 17 種まで進む(保存はビット単位)。
  8 種以下ならインラインと毎刻みビット一致(reaction_test の TestWideLimits・TestWideCrowdedCell・TestWideMatchesInline)。
  2 時間の約束のため、多重解像度の世界(MultiresNest の溢れ・粗くする和集合・TestCoarsenFull / TestLimitsStep の上限なし版)は T-0187 に分けた。

## 分けたもの
- T-0187: 多重解像度の世界(CPU)のセルに溢れを持たせる(読む所・書く所を RxWideCell で・粗くする和集合・設定で切り替え)

