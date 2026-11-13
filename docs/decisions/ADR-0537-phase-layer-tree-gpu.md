# ADR-0537 相の層の配り(セルを読む配り手)を GPU の木の段に・相の層の欄の頁を割り当ての段で取る規則(T-1231。T-1252 段 2 の 8 本目)

- Status: Accepted(一旦の決定・2026-11-13 wt6 作業役。1 は土台の共通部分〔欄の頁の取り方〕の規則の変更なので司令塔の受け取りが要る = 仮)
- 日付: 2026-11-13
- 決めた人: Claude(wt6 の作業役)
- 親: ADR-0532(GPU の欄の頁)・ADR-0512 の 1(相の層の配り手)・ADR-0500 改訂・D-674
- 実装: shaders/sim/multires_graph.hlsl(RefinePhaseLayerCarried・CoarsenPhaseLayerCarried)・shaders/sim/multires_field_access.hlsli(CarriedOwnerNeed)・
  engine/src/sim/multires_tree.cpp(AllocateOwnerPages・CoarsenNeedsOwnerPage)・engine/src/sim/gpu_multires.cpp(RecordUpload の断りを外した)・tests/gpu_field_region_test.cpp の 12

## 背景
CPU の相の層の配り(RefinePhaseLayerOctant・CoarsenPhaseLayerOctant)は欄の値が全部 0 の所でも 0 でない Ec を書きうる:
粗く = 凝縮相だけの子と気体だけの子(Ec は 0・欄の頁が無いことが多い)から混ざった親(Ec ≠ 0)。細かく = 満ちたセル(混ざらない・Ec 0)を
置かれ方で配ると混ざった子(PlEvenCondensedEnergy)。CPU はその時 StorePage の EnsurePage で配りの最中に頁を作っていた(割り当ての段は
「親か子が欄の頁を持つ時」だけ取る)。GPU の木の段は要求ごとに並べて走るので、配りの最中に取ると頁の番号が CPU と揃わない。

## 決定
1. **相の層の欄(SF_OWNER_PHASE_LAYER)の頁は割り当ての段で控えめに取る(CPU と GPU の同じ規則)**: 細かくする = 頁を持つ親なら(親が欄の頁を
   持たなくても)鎖の子の全部に。粗くする = 親が頁を持つ枠になり(広げる親も)まだ欄の頁が無ければ 1 つ。ほかの配り手は今の規則のまま。
   値は変わらない(要約は密の並び = 今と同じ)。増えるのは全部 0 の欄の頁だけ(畳む段が返す)。溢れの世界も同じ規則(配りは一般のまま)。
2. **細かくする段**: 置かれ方(か SAME の写し)の後・親を覆う前に DeviceMemoryBarrierWithGroupSync を 1 つ挟み、t < 64(親の八分の一のセル)が
   子 8 つを g_cells から j ごとに読み直して(FX_UNROLL の 8 回。書き換えた構造体の配列を動的な添字で読まない = T-0397)子のエネルギーと子の Ec を書く。
   最後の重みのある子は回しの中の比べで選ぶ(total − ほかの子の和)。一般の配り(RefineCarriedFields)は相の層の欄を飛ばす。
3. **粗くする段**: 頁を持つ枠になる親の八分の一は、同じスレッドが書いた result.cell(バリア不要)と子 8 つのセル・Ec から PlCoarsenCondensedEnergy。
   一様のままの親は一般の SfOwnerCoarsen(CPU と同じ)。保存量でない = 帳簿は無い。
4. 溢れを使う世界(MR_WIDE_CELLS)は CPU と同じく一般の配り(#ifndef MR_WIDE_CELLS)。影の鎖・引き戻し・畳む・広げるは一般の配り(CPU と同じ)。
5. RecordUpload は相の層の配り手の欄を断らない。

## 作り手から見た違い
- 無い(エンジンの中の頁の取り方。相の層の世界で全部 0 の欄の頁が一時に増える = 器の見積もり〔Q189〕の数が少し増える)。

## 退けた案
- 割り当ての段がセルを読んで「0 でない Ec を書く所」だけ取る: 置かれ方の結果が要るので、割り当ての段で配りを 2 回走らせることになる。
- 配りの最中に GPU が頁を取る(影の鎖の形): 要求ごとのグループが並べて走るので、取る順が CPU の要求の順と揃わない。

## 要る現象の一覧(docs/design/21 §3)との照らし
- 今のまま(全部): 相の層の配りの式は CPU と 1 つの源(phase_layer.hlsli)のまま。GPU で同じ現象が同じに起きる。
