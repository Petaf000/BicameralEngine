# T-1240 GPU の層: 要求の処理(細かくする・粗くする・畳む・広げる)・一様の枠の層・溢れの成分の層

- Status: Doing(7 の 2 まで。T-1221 から分けた。T-1188〔wt2 の multires_tree・gpu_multires〕の後)
- 種類: 工学
- PC: 必須(GPU)
- 見積もり: チャット 2 回分(要求の処理 / 一様の枠と溢れ)
- マイルストーン: 土台の変更要求の束(ADR-0469 の 2-2 の続き)。T-1210 と同じ
- 設計: docs/design/20 §10.5 / 決定: ADR-0481 の 5・ADR-0489(Proposed)/ 前: T-1221

## 目的(1〜2 行)
T-1221 の 1 段目は、層の頁を GPU に置き、刻みの段の後に層を合わせるだけ。要求の処理(木の段)は層を配らず、
一様の枠の層・溢れを使う世界の層は GPU に写さない(RecordUpload が断る)。ここで CPU(T-1220)と同じ配りを GPU に入れる。

## 完了条件(チェックできる形で)
- [ ] 頁を配ったら層の頁を空に(今は層を合わせる段が「持ち主が頁を手放した層の頁」を 0 にするだけ。同じ枠に同じ頁が戻ると 0 のまま使う)
- [ ] 層の頁を GPU で取る・返す(層の頁の空きのスタック。今は CPU が写す時に詰めるだけで、GPU は新しく取れない)
- [ ] 細かくする(子は親と同じ層 / 置かれ方で配る時は層を沈む側。T-1176・T-1188 の GPU の配りに乗せる)・粗くする(成分ごとに層の和 ÷ 8)・
      畳む(層まで一様な時だけ)・広げる(一様の枠の層を頁へ写す)。CPU とビット一致
- [ ] 一様の枠の層(MultiresCellStore::uniformLayers)を GPU に置く(T-1238 の形に合わせる)
- [ ] 溢れを使う世界(wideCells)の溢れの成分の層(MultiresOverflowArea::layers。物質で結ぶ。溢れの段・WideRetry の後にも合わせる)
- [ ] 試験: gpu_multires_layers に要求のある場面(本物の鎖・粗くする・畳む)・溢れの世界・WARP

## メモ・参考
- T-1221 の形: shaders/common/multires_layers.hlsli(置き場の並び)・shaders/sim/multires_layers.hlsl(層を合わせる段。u4 に結ぶ)・
  engine/src/sim/gpu_multires_layers.cpp(写す像・戻す)。層の頁の見出しは「セルの頁・持ち主の枠」の 2 語。
- 木の段のシェーダー(multires_tree.hlsl・multires_graph.hlsl)は wt2(T-1188)が直しているので、その後に。
- ルート署名は 64/64 語なので、木の段が層の置き場を読むには u4(外のバッファの口)を使うか、ほかのバッファの後ろに置く(溢れと同じ)。
  木の段が u4 を使っていないかは着手の時に確かめる(2026-10-29 は未確認)。

## 設計の詰め(2026-10-29 wt2・作業役。コードはまだ書いていない)
着手の前提は揃った: T-1188・T-1238・T-1189(t-1189-refine-neighbors。ready の印済み)。このブランチ t-1240-layer-tree は t-1189 の上。

### 1. 層の置き場を木の段から読む口 → **u4 に結ぶ(仮・おすすめ)**
- 確かめた(2026-10-29 grep): 木の段(multires_tree.hlsl・multires_graph.hlsl・multires_refine_place.hlsli・multires_refine_halo.hlsl)は u4・u5・g_external* を宣言していない。
  RecordProcessRequests の間は u4 を使う段が無いので、その間だけ m_externalViews[0] = 層の置き場(RecordLayerReconcile と同じ「結んで戻す」)。ルート署名の語は変わらない。
- 退けた案: u6 の後ろ(写しの後ろ g_overflowBase + 枠 × 108)。溢れを使う世界では u6 の後ろが溢れの領域 + 作業場(大きさがルート定数に無い m_spaceSpecies 等で決まる)なので、
  層の始まりを渡す語がもう 1 つ要る(64/64 語で空きが無い)。層の置き場は大きさが別(頁 × 12288 語)で、u6 に混ぜると読み戻し・写す像も u6 の並びに絡む。
- 置かれ方の変種(MR_REFINE_PLACE)は写しを u6 の後ろのまま使う(T-1189)。層は u4。両方を同じ段で読める。

### 2. 層の置き場の並びを変える(T-1221 の見出し 2 語 → 頁の索引 + 空きのスタック)
- 今: 見出し [層の頁] = (セルの頁・持ち主の枠)。CPU が写す時に詰めるだけで、GPU は新しく取れない・頁を配り直すと「持ち主が手放した」で 0 にする。
- 新: [セルの頁ごとの層の頁の番号(PageCount 語。無ければ MR_LAYER_NO_PAGE)][層の頁ごとのセルの頁(capacity 語)][空きのスタック(capacity 語)+ 数 1 語]
  [一様の枠の層(下の 4)][層の頁 × MR_LAYER_PAGE_WORDS]。持ち主の枠の語は要らなくなる(頁を返す段が層の頁も返す = 「頁を配ったら層は空」がそのまま成り立つ)。
- 層の頁の番号は CPU に無い(CPU は頁ごとに vector)ので、番号の取り方は CPU とのビット一致に効かない(像は頁の番号で比べる)。ただし決定性のため取り方は要求の順のプレフィックス和(TreeAllocate と同じ)。
- 返す: TreeRelease(粗くした子の頁)・TreeFold(畳んだ頁)の後に「層の頁を返す段」(1 グループ = 返した頁 1 つ。中身を 0 にしてから積む = 取る側は 0 を前提にできる)。
- **【エンジン】層の頁が尽きた時(GPU だけ。CPU は上限が無い)→ 仮: 細かくする / 広げる要求を断る(MR_STATUS_NO_SPACE と同じ扱い・計器に数える)**。
  作り手から見た違い: 層の頁の数(GpuMultiresOptions::layerPages)を小さくすると、層のある所が細かくならない(層は捨てない・量は変わらない)。
  もう 1 つの案「層を残りへ混ぜて続ける」は結果を変える(D-428)ので取らない。尽きると CPU とずれるので、試験は尽きない数で回し、尽きる場面は「断って数える」を別に確かめる。

### 3. 要求の処理の段ごとの配り(CPU の関数と同じ式)
| 段 | CPU(engine/src/sim) | GPU でやること |
|---|---|---|
| 細かくする・頁の親 | multires_tree.cpp RefineRequestLevel → nest_detail::CopyCellToPage(層ごと) | RefineLevel でセルを写す時に層の頁を取り(親の八分の一に層があれば)、子のセル t ← 親のセルの層 |
| 細かくする・置かれ方 | multires_refine_cells.cpp MaterialsOf(層の分 = 沈む側)・PlaceTreeCell | PlaceRefineChildren が層を「層の分・残り」の 2 物質に分けて配る。PlaceFullnessAt・写しの段 HaloFullness も層を含める(CPU の CellFullness と同じ) |
| 細かくする・一様の親 | uniformLayers.Set(child, parent) | 一様の枠の層を子の枠へ写す(下の 4) |
| 影(観察の枠) | multires_nest.cpp RefineShadowLevel → CopyCellToPage | RefineLevel の影の道も同じ(影の頁の古い層は残さない) |
| 粗くする | multires_tree.cpp CoarsenLayers(成分ごとに層の和 ÷ 8 の切り捨て。物質で結ぶ) | CoarsenRequest で子 8 つの層を物質で結んで和 ÷ 8。一様の親は CoarsenKeepsUniformLayers が偽なら頁に広げる(TreeResolve の pageNeed) |
| 畳む | multires_activity.cpp FoldExact → FoldLayeredPage(層まで一様な時だけ・舞う粒が残れば畳まない)・FoldNear(層のある頁は畳まない) | TreeFoldCheck に層の比べを足し、畳んだら一様の枠の層を書く・層の頁を返す |
| 広げる | FillPageFromUniform(一様の枠の層を頁へ)・ClearUniformLayers | TreeExpand・粗くする時の親を広げる所で写す |
| 解放 | ReleaseAll → ClearPageLayers・ClearUniformLayers(返した枠) | 層の頁を返す段・返した枠の一様の層を消す |
- HW の Work Graph の止まり(T-0397・alloca)に注意: 層を物質で結ぶ回しは決まった数(RX_MAX_CELL_SPECIES)の FX_UNROLL で、読んだ構造体の配列を動的な添字で読まない(dxil_alloca_check が見張る)。
  置かれ方の段は別プログラム(ADR-0499)。層の読みでさらに重くなるので、HW で止まったら層の配りを Compute の段(RefineLevel の後・同じ要求の鎖を読む)に出す(退く道)。

### 4. 一様の枠の層(T-1238 の形)
- CPU: MultiresUniformLayerStore = entryOfSlot[枠](4 B)+ 層のある枠だけの組。GPU も同じ: [枠ごとの組の番号][組 × (物質・層の下位・上位) × RX_MAX_CELL_SPECIES][組の空きのスタック]。
  溢れを使わない世界は 1 枠の物質 ≤ 8 なので組は固定の大きさ。溢れを使う世界の一様の値は 8 を超えうる → 組の大きさを RX_GPU_WIDE_SPECIES にするか、2 段目(溢れの層)と一緒に塊の置き場に置く(5 で決める)。

### 5. 溢れを使う世界の溢れの成分の層(段を分ける: T-1240 の 2 回目)
- CPU は MultiresOverflowArea::layers(species と同じ並び)。GPU の溢れの面は塊で並びが毎回変わる(読んだ列で並びが変わる)ので、層は**物質で結ぶ別の置き場**(層の頁と同じ形を溢れの成分の数まで)にし、
  溢れの段・WideRetry(刻みの段を足したら RecordWideRetry)の後に層を合わせる段を回す。1 回目(溢れを使わない世界)の後に。

### 6. T-1252 段 2(GPU の配り手・集めて書く段)との並べ方(T-1252 段 1 はまだ main に無い。2026-10-29 確かめた)
- 同じ木の段(RefineLevel・CoarsenRequest・TreeFold・TreeExpand)に触る。RefineLevel の中の順: セルを写す → 層を写す → 置かれ方(層を読む)→ 持ち主の欄の配り(T-1252。置かれ方の後 = ADR-0499 の後)。
  粗くする: セルの和 → 層の和 ÷ 8 → 持ち主の欄の配りの口。T-1252 の「集めて書く段」の頁の取り方(枠の順のプレフィックス和)は層の頁の取り方(要求の順のプレフィックス和)と別の空きのスタックなので干渉しない。
- 試験は同じ器(gpu_multires_layers に要求のある場面を足す)に T-1252 の試験用の持ち主の欄を後で載せられる形にする。

### 7. 進め方(この順。各段で CPU の試験を先に・GPU は最後にまとめて)
1. 置き場の並び(2)と写す像・戻す(gpu_multires_layers.cpp。CPU だけの往復の試験)→ 層を合わせる段を新しい並びに
2. 細かくする(頁・一様・影)と解放 → 試験: gpu_multires_layers に「本物の鎖で細かくする」場面(CPU の RequestRefine と比べる)
3. 粗くする・畳む・広げる → 試験: 粗くする・畳む・広げるの場面
4. 置かれ方の世界の層(PlaceRefineChildren・写しの段)→ gpu_tree_refine_placement に層のある場面
5. 溢れの世界(T-1240 の 2 回目)
- 完了の確かめ: gpu_multires_layers〔HW・WARP〕・gpu_multires・gpu_tree_refine_placement〔HW・WARP〕・gpu_lab_box・dxil_alloca_check・release 警告 0・archmap --check。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29(wt3・T-1221 の作業役): T-1221 から分けた。
- 2026-10-29 wt2(作業役): 設計の詰め(上の節)。u4 に結ぶ・頁の索引 + 空きのスタック・段ごとの配りの表・尽きた時は断る(仮)・T-1252 段 2 との順。コードは未着手(t-1189 の仕上げに時間を使った)。
- 2026-10-29 wt2(作業役 2): 7 の 1 を実装(c8b0b91a)。置き場の並びを [頁の索引][層の頁の持ち主][空きのスタック][空きの数][足りなかった数][要求ごとの割り当て][層の頁] に(common/multires_layers.hlsli)。
  道具は新しい sim/multires_layer_access.hlsli(u4 = g_layers・g_external0 = 層の頁の数・g_external1 = セルの頁の数)。TreeRelease が返す頁の層の頁を返す(中身を 0 にして積む)。
  TreeFoldCheck は層の頁を持つ頁を畳まない(仮。層ごと畳むのは 7 の 3)。GpuMultires::BindLayers / RestoreExternal で要求の処理・畳むの間は u4 = 層の置き場。
  層を合わせる段は層の頁の全部に投げ、空いた層の頁は飛ばす。release ビルド警告 0・dxil_alloca_check・multires_layers 合格。GPU の組(下の引き継ぎ)は流した。

- 2026-10-30 wt2(作業役 3): 7 の 2 を実装(t-1240-refine-layers)。TreeAllocate が細かくする要求ごとに層の頁を要求の順のプレフィックス和で取る
  (親の頁が層の頁を持つ時だけ・鎖の段の数。MrLayerRequestAddress)。尽きたら MR_STATUS_NO_SPACE・MrLayerShortageAddress(仮【エンジン】)。
  RefineLevel: 子の頁に層の頁を結び、子のセル t ← 親のセル MrParentCellOfChild の組 8 つ(CopyLayerCell)・覆った親のセルの層は 0。
  影の鎖: 影の頁の層の頁を使い回すか空きの上から取る(尽きたら数えて層なし: CPU が決めた鎖は断れない)。RecordRefineShadow も u4 を結ぶ・RecordPullBack の後に層を合わせる。
  置かれ方の世界に層がある時は RecordUpload が断る(7 の 4 まで)。試験: gpu_multires_layers に本物の鎖・影の鎖(9 段・毎刻み)・層の頁が尽きる場面。

## 引き継ぎメモ(2026-10-30 wt2 作業役 3)
- 済み: 7 の 2(細かくする: 要求の鎖・影の鎖・覆った親の層)。一様な親の子(一様の枠の層)は 4 の一様の枠の層と一緒に(今は写す像が一様の枠の層を断る)。
- 次(7 の 3): 粗くする(CoarsenRequest。子 8 つの層を物質で結んで和 ÷ 8 の切り捨て = CPU の CoarsenLayers)。親が層の頁を持たなければ TreeAllocate で取る
  (今の RefineLayerNeed と同じ累積和に粗くする要求の分を足す: 子が層の頁を持ち親が持たない時 1)。子の層の頁は TreeRelease が返す(済み)。
  畳む: TreeFoldCheck の「層の頁を持つ頁は畳まない」を、CPU の FoldExact → FoldLayeredPage(層まで一様・舞う粒が残れば畳まない)に。一様の枠の層(4)が要る。
  広げる: TreeExpand で一様の枠の層を頁へ。3 と 4 は一緒に進める方が早い(一様の枠の層が無いと畳む・広げるの層の行き先が無い)。
- 影の頁は RemoveShadow で層の頁を返さない(CPU の RemoveShadowChain は頁の層を残すが観察の枠の頁なので次の RefineShadow で書き直す)。影の鎖の分の層の頁を
  layerPages に持たせる約束(尽きると CPU とずれる)は gpu_multires.h のコメントに書いた。

## 引き継ぎメモ(2026-10-29 wt2 作業役 2)
- 済み: 7 の 1(並び・像・層を合わせる段・頁を返す時に層の頁を返す)。**細かくする・粗くする・畳む・広げるはまだ層を配らない**(前と同じ「層が消える」側に倒れる。前は持ち主の印で 0 にしていた)。
- 次(7 の 2・細かくする): TreeAllocate で、許可した細かくする要求ごとに「層の頁が要る段の数」を層の空きのスタックから要求の順のプレフィックス和で取る
  (MrLayerRequestAddress に始まりを書く。親の頁が層の頁を持つ時だけ・鎖の段はどれも親から写すので最初の段が層を持てば全部の段が要る)。足りなければ要求を MR_STATUS_NO_SPACE に・
  MrLayerShortageAddress に数える(仮【エンジン】。TreeAllocate は 1 グループなので頁・枠の許可と同じ累積和に 4 つ目の値を足す。InclusiveScan は 3 つなので groupshared を 1 本足す)。
  RefineLevel(multires_graph.hlsl): 子の頁の層の頁 = 取った番号・頁の索引と持ち主を書き、スレッド t が親の層の頁のセル MrParentCellOfChild の組 8 つを子のセル t へ写す(CopyLayerEntry)。
  影の道(RecordRefineShadow)も同じ形(影の頁の古い層は残さない = 影の頁が前に層の頁を持っていれば先に返す)。CPU は CopyCellToPage(multires_cell_store.cpp)。
  Work Graph の止まり(T-0397・alloca)に注意: 組は g_layers から直に読んで直に書く(構造体の配列に持たない)。
- 試験は gpu_multires_layers に「本物の鎖で細かくする」場面(test::MultiresRequestsAt の Real・根に層)を足す。CPU は test::StepMultiresScene。
- GPU の組(release・RTX): gpu_multires・gpu_multires_warp・gpu_multires_quiet・gpu_multires_layers・gpu_multires_layers_warp・gpu_multires_near_fold・gpu_lab_box・gpu_tree_refine_placement の 8 本合格(2026-10-29)。archmap --check OK。
- t-1189・T-1252 段 1 は main に入った(2026-10-29 確かめた)。このブランチは rebase main 済み。T-1252 段 2(GPU)はまだ始めていない。
