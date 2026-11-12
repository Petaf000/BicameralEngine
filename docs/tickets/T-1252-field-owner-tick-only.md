# T-1252 登録に「配り手」と「寿命」・粗くする時のセルの配りの口・集めて書く段(土台。段 1 CPU / 段 2 GPU。ADR-0500 改訂)

- Status: In Progress(2026-11-13 wt6: 段 2 の 7 本目 = 保存量の配り手の欄を GPU の木の段に〔t-1252-conserved-owner〕。2026-11-12 wt6: 段 2 の 6 本目 = 集めて書く段を木の段に〔t-1252-gather-tree〕。段 2 の 5 本目 = 影の鎖・引き戻し・畳む・広げるまで〔t-1252-tree-gpu-4〕。段 2 の 3 本目 = GPU の世界の欄の区画まで〔t-1252-tree-gpu-2〕。段 2 の 1 段目 = GPU の欄の頁の置き場の並びと集めて書く段の Compute が CPU と頁の番号までビット一致〔ブランチ t-1252-registry-owner-gpu・ADR-0532〕。木の段への載せは下の「段 2 の残り」。段 1 CPU の主な形は済み・2026-10-29 本体の作業役 18 回目)
- 種類: 工学(土台の共通部分。並走の作業役は自分の都合で広げない = CLAUDE.md §1 の 9)
- PC: 必須
- 見積もり: チャット 2〜4 回分(段 1 CPU 1〜2 / 段 2 GPU 1〜2)
- マイルストーン: ROADMAP「土台の変更要求の束」の 3
- 設計: docs/design/20-transport-substrate.md §2.1・§2.4・§10.7 / 決定: ADR-0500(Proposed・一旦の決定)・ADR-0403・ADR-0329・ADR-0469 の芯 4

## 目的(1〜2 行)
登録の 1 行に配り手(SF_SCALE_OWNER + 配り手の番号)と寿命(SF_FLAG_TICK_ONLY)を足し、散らばった書き手が欄へ書く共通の「集めて書く段」と、粗くする時のセルの配りの口を CPU で作る。
段 1 は CPU・段 2 は GPU。使い手(Ec = T-1230・放射の持ち越し = T-1227・源の項 = T-1228)はまだ載せない(配り手の表は空 + 試験用の配り手 1 つ)。

## 完了条件(チェックできる形で)
- [ ] state_field.hlsli: SF_SCALE_OWNER・SfFieldDesc に配り手の番号(SF_NO_OWNER)・SF_FLAG_TICK_ONLY・SF_KIND_TICK(ハッシュ・保存点に入らない)・SF_MAX_COMPONENTS 12。
      SfDescValid: 保存量 ⇒ VOLUME か持ち主・VOLUME ⇒ 保存量・この刻みだけは真実の印(HASHED)と導かれる値の印を持たない・持ち主は配り手の番号を持つ
- [ ] nest_fields::Refine / Coarsen / RefineShadow / PullBackShadow が持ち主の欄を写さない(0 で渡す)。畳む・広げる・眠る・保存点・ハッシュは今のまま
- [ ] 粗くする時のセルの配りの口(MultiresNest に refineCells と並べて。CPU の Coarsen の後に呼ぶ)。順は「セルの中身 → 持ち主の欄」
- [ ] 配り手の表(決まった番号 → 細かくする関数・粗くする関数。HLSL は決まった数の回しで選ぶ = T-0397)
- [ ] 集めて書く段の共通の関数: 印のブロックを枠の順に回る・頁が無ければ枠の順のプレフィックス和で頁を取る・一様な枠は広げてから書く・頁が尽きたら計器の誤り(量は捨てない)
- [ ] 当てる段が「この刻みだけ」の源の項の欄を 1 回の掃きで読む口(使い手はまだ無い)・セルの真実を書く 1 つの関数で busyTick を書く(今の書き場所を寄せる。値は今と同じ)
- [ ] 試験: 新しい印の欄が無い世界は全部ビット一致(multires・multires_state・multires_sparse・field_storage・window_lab)・試験用の持ち主の欄の往復で保存・この刻みだけの欄が境目で 0・頁が尽きた時の誤り
- [ ] **段 2(GPU。段 1 の後)**: GPU の木の粗くする段からセルの配りの口・細かくする側は RefineLevel の置かれ方の段(ADR-0499)の後・集めて書く段の Compute(頁の番号まで CPU と同じ)。
      GPU の欄の頁は T-0930 の形で要る所まで。試験: 試験用の持ち主の欄と、この刻みだけの欄で CPU と GPU の頁の像がビット一致(HW・WARP)・新しい印の欄が無い世界は今とビット一致(gpu_multires・gpu_lab_box・gpu_tree_refine_placement)。ルート署名 64/64 語の中
- [ ] release のビルド(警告 0)・archmap --check

## メモ・参考
- 段 2 は T-1240(GPU の層)と同じ木の段に触る。並べる時は同じ作業役に(木の段の持ち主を 1 つに)。T-0930(GPU の疎な欄の頁)の取り方 = 枠の順のプレフィックス和と同じにする。
- 段 2 と T-1240(GPU の層)の並べ方の詰め(2026-10-29 wt2): T-1240 の「設計の詰め」6。RefineLevel の中は セルを写す → 層 → 置かれ方 → 持ち主の欄の配り、
  粗くするは セルの和 → 層の和 ÷ 8 → 配りの口。層の頁の空きのスタック(要求の順)と欄の頁(枠の順のプレフィックス和)は別のスタック。段 1 はまだ main に無い(2026-10-29)。
- 影の引き戻しが配り手の粗くする関数で ADR-0361(子の和 = 親 × 8)と揃うかを確かめる(未確認)。

## 段 1 で作ったもの(2026-10-29 本体 18 回目)
- state_field.hlsli: `SF_SCALE_OWNER`(SF_SCALE_COUNT 3)・`SfFieldDesc::owner`(SF_NO_OWNER)・`SF_OWNER_TEST`(試験用の配り手 1 つ。SF_OWNER_COUNT 1)・
  `SF_FLAG_TICK_ONLY`(256。SF_FLAG_MASK 511)・`SF_KIND_TICK`・`SF_MAX_COMPONENTS` 12・`SfMakeOwnerDesc`・`SfMakeTickDesc`・`SfOwned`・`SfTickOnly`・
  配り手の関数 `SfOwnerRefineChild`・`SfOwnerRefineUniform`・`SfOwnerCoarsen`・`SfOwnerPullBackChild`(決まった数の分かれで選ぶ)。
  SfDescValid: 保存量 ⇒ VOLUME か配り手・VOLUME ⇒ 保存量・配り手 ⇔ 番号・配り手はセル・面の真実で置かれ方の印を持たない・この刻みだけはセル・面・SAME で HASHED・導かれる値・保存量・置かれ方・内訳の印を持たない。SfSameDesc に owner。
- multires_fields: 一般の段(Refine・Coarsen)は配り手の欄を写さない。セルの配りの口 = `RefineOwners`(Refine の最後 = セルの中身の配りの後)・`CoarsenOwners`(Coarsen の一般の欄の後。保存量の余りは同じ帳簿の列へ)。
  CoarsenKeepsUniform は配り手の粗くする関数で見る。影(RefineShadow・PullBackShadow)は配り手の関数で。
  集めて書く段 `NestGatherWork`・`AddNestGatherWork`・`GatherNestField`(触った枠を枠の順に・一様な枠は MR_PAGE_WANTED → ExpandWantedPages で広げる・欄の頁は枠の順に取る・頁の予算 `SetNestFieldPageBudget` が尽きたら書かずに作業に残す = 報告の pageShortage)・`NestGatherWorkTotal`・`UsedNestFieldPages`。
  この刻みだけ: `ConsumeTickOnlyField`(当てる段の読む口: 枠・セルの順に 1 回の掃きで渡して 0 に)・`TickOnlyFieldsClear`(StepNest・StepActive の初めに FX_ASSERT)。ハッシュ・保存点は SfHashed・SfSaved が種類から外す(コードの変更なし)。
- 試験 `field_owner`(tests/field_owner_test.cpp): 登録の規則・配り手の関数の和・頁を持つ親の細かく → 粗くの往復(配り手の欄は配り手の値・一般の欄は今のまま・覆われた親は 0・ComputeConservedTotals が前後で同じ・親の値に戻る)・
  一様な親の往復・影の細かく/引き戻し(引き戻した影を配り手で粗くすると親・余り 0 = ADR-0361 と揃う。試験用の配り手で確かめた)・この刻みだけ(要約に入らない・読む口・保存点に入らない)・集めて書く段(順の入れ替えで頁の表と値が同じ・枠の順の頁・一様な枠を広げる・予算が尽きても欄 + 作業 = 足した量)。

## 段 1 の残り(次の作業役へ)
- [ ] **セルの真実を書く 1 つの関数で busyTick**(完了条件 6 の後半): 今の書き場所は multires_nest.cpp の RecordWaitResults・ResolvePokes・PullBackLevel・nest_detail::PokeBlock・time_step_tree.cpp。
      値を変えずに 1 つの関数(例 `nest_detail::MarkCellsChanged(nest, slot, tick)`)へ寄せる。使い手(T-1228 の当てる段)と一緒にやる方が寄せる先が決まるので、T-1228 段 2 へ回してもよい(おすすめ)。
- [ ] 当てる段の「源の項」の読みは口(ConsumeTickOnlyField)だけ。当てる段(transport_apply・RadiationApply の mJ への丸め)へつなぐのは T-1228。
- [ ] 刻みの境目の検査は今 FX_ASSERT(debug だけ)。開発ビルドで計器にする(誤りの名前)のは使い手が来た時。
- [ ] 集めて書く段は CPU の 1 関数。GPU と同じ「プレフィックス和」の形(印の数え → 和 → 書く)の 3 段に分けるのは段 2(番号は同じになる: k 番目に要る枠が空きの k 番目の頁)。
      注意: CPU の疎な頁は「返した頁を後ろから使う」(freePages)ので、頁の番号は空きの並びで決まる。GPU の T-0930 の形と頁の番号まで同じにするには、空きの頁の並び(スタック)も CPU と同じ形で持つ(段 2 で決める)。

## 段 2 に要るもの(GPU。T-1240 と同じ木の段なので wt2 で同じ作業役に)
- GPU の欄の置き場(T-0920・T-0930 の形: 欄の頁のプール + pageOf + 空きのスタック)が要る所まで。今 GPU に登録の欄の置き場は無い。
- GPU の木の粗くする段(multires_graph.hlsl の粗くする所)から `SfOwnerCoarsen`、細かくする側は RefineLevel の置かれ方の段(ADR-0499 = multires_refine_place.hlsli)の後に `SfOwnerRefineChild`/`SfOwnerRefineUniform`。配り手の選びは owner の分かれ(T-0397)。
- 集めて書く段の Compute: 印のブロックの数え → 枠の順のプレフィックス和(T-0930 と同じ)→ 頁を取る → 書く。一様な枠は TreeExpand(物の熱の OhNestMarkExpand と同じ形)。頁が尽きたら計器の誤り(数えて CPU へ)。
- 試験: field_owner の場面を GPU で(試験用の配り手の欄・この刻みだけの欄)、CPU と頁の像(pageOf まで)のビット一致(HW・WARP)。新しい印の欄が無い世界は今とビット一致(gpu_multires・gpu_lab_box・gpu_tree_refine_placement)。ルート署名 64/64 語の中。
- 登録を GPU へ定数バッファで渡すなら 1 行 32 B(owner を足した)。

## 判断待ち(仮・ユーザー未確認。作り手から見た違いは無い = 問いにしない)
- 配り手の欄の一様な親を細かくする時は「子も一様」(SfOwnerRefineUniform)を配り手の約束にした(一様の枠は頁を持たないので、子ごとに違う値を書けない)。
  光の配り(面の側の子だけ)は一様な親でも子ごとに違うので、T-1227 では「配り手の欄が 0 でない一様な親は細かくする前に頁に広げる」が要る(CoarsenKeepsUniform と同じ控えめの形)。T-1227 で足す。
- 頁の予算は欄ごと(MultiresFieldStore::pageBudget。状態に入らない。0 = 決めない = 今まで通り)。世界の作りでどう決めるかは使い手(T-1227)。

## 段 2 の 1 段目(2026-11-12 wt6 作業役。ブランチ t-1252-registry-owner-gpu。ADR-0532)
- **入った**: `shaders/common/multires_field_pages.hlsli`(欄 1 つの GPU の置き場 = [頭 12 語][pageOf × セルの頁][空きのスタック × 容量][頁 × 512 × 値の数 × 2 語]・
  作業の並び [blockOf][枠ごとの段の結果][値]・頁の番号の取り方 MfPagesAvailable / MfTakesFreePage / MfFreePosition / MfFreshPage)/
  `shaders/sim/multires_field_gather.hlsl`(GatherMark → GatherAssign〔1 グループ・枠の順の塊ごとのプレフィックス和〕→ GatherWrite)/
  `engine/src/sim/gpu_multires_fields.{h,cpp}`(CPU の置き場・作業 ⇔ GPU の像。疎の欄だけ・密は断る)/ 試験 `gpu_field_gather`・`gpu_field_gather_warp`。
- **頁の番号の決め(ADR-0532 の 2)**: 1 つの段の中で頁の要る所を決まった順に並べ、k 番目は空きのスタックの上から k 番目・その後は高さ + (k − 空きの数)。
  = CPU の EnsurePage を同じ順に呼んだ番号(CPU は変えていない)。取れる数 = min(容量, 予算)− 使っている数。CPU の freePages の並び・使った高さ・空きの頁の古い値まで像に写す。
- **試験(release・RTX 3070 Ti / WARP。各 1 秒未満)**: A 空きのスタック [1, 3] のある置き場に頁を持つ枠 1・持たない枠 4(12 値・同じセルへ 2 回)→ 書いた 5・取った 4(空きから 2・高さの上 2)/
  B 予算 = 使っている数 + 2 → 取った 2・尽きた 2(作業に残る)/ C 根 700 個(枠 708・1 スレッドの塊 2)に乱数 → 書いた 290・取った 229 /
  D 一様な枠 2 つへ足す → GPU の印(WANTED)が CPU の広げた枠と同じ。A〜C は pageOf・空きのスタック・使った高さ・頁の値・作業の残り・報告の数が CPU とビット一致。
  ほか: field_owner・dxil_alloca_check(古い phase_layer_faces.asm を _to_delete へ移してから)合格・release 警告 0・archmap --check OK。
- **新しい印の欄が無い世界**: sim/gpu_multires の段は 1 行も変えていない(この段はまだ GpuMultires から呼ばない)= gpu_multires・gpu_lab_box・gpu_tree_refine_placement は流していない(変えた道が無い)。

## 段 2 の残り(次の作業役へ。この順)
1. **【印】木の段の頁の取り方を CPU と揃える(ADR-0532 の 3。土台の共通部分 = nest_fields の変更なので司令塔の確かめが先)**:
   CPU は要求ごとに「親の頁を取る → 子の頁を返す」が交互(次の要求が返した頁を使う)。GPU の木の段は段ごとに全部の要求を並べるので、
   一旦の形 = CPU の nest_fields も木の 1 回の処理の中で「取る」を段の順(細かくする鎖・粗くする・広げる)に要求の順で・「返す」を処理の終わりに枠の順で。
   配り手の欄は「頁が要るか」を値を作る前に決める(細かくする子 = 親の八分の一に 0 でない値がある・粗くする親 = 子に 0 でない値がある。全部 0 の頁は眠る判定の ReleaseZeroPages)。
   値と状態の要約(ハッシュ)は変わらない(頁の番号と空きの並び・MeasureNestFields のバイトだけ)。field_owner に「頁の像が段の順の取り方と同じ」を足す。
2. **GpuMultires に置き場を持たせる**: 配り手の欄・この刻みだけの欄(疎のセルの欄)を 1 つのバッファに並べる(欄ごとの始まりの語の表を頭に)。RecordUpload / Read で MakeGpuFieldImage / ApplyGpuFieldImage。
   **結ぶ口 = u5**(木の段は u4 = 層の置き場だけを使い、u5・g_external2・g_external3 は空いている。BindLayers で u5 = 欄の置き場・g_external2 = 欄の数)。ルート署名の語は増えない(64/64 のまま)。
   一様の値(枠 × 値の数)を置き場の並びに足す(配り手の一様の配り SfOwnerRefineUniform・CoarsenKeepsUniform の配り手の比べが読む)。
3. **粗くする段のセルの配りの口**: multires_graph.hlsl の CoarsenRequest(811 行あたり)の一般の段の後に SfOwnerCoarsen(owner の分かれ = T-0397)。保存量の余りは世界の帳簿の「粗くした端数・出」の列へ(CPU の CoarsenOwners)。
   一様な親で配り手が一様のまま粗くならない時は TreeResolve の pageNeed(層の CoarsenKeepsUniformLayers と同じ所)。相の層の配り(セルを読む)は T-1231 の GPU。
4. **細かくする側**: RefineLevel の置かれ方の段(ADR-0499)の後に SfOwnerRefineChild / SfOwnerRefineUniform・親の八分の一を 0 に。影は RefineShadow・引き戻しは SfOwnerPullBackChild。
   wt2 の T-1323 が同じ木の段(RecordExpandPass・反応の段・層を合わせる段)を触るので、足す場所(RefineLevel の層の配りの後)を T-1323 のチケットと突き合わせる。
5. **集めて書く段を GpuMultires の段に**: 印の段の WANTED → TreeExpand(物の熱の OhNestMarkExpand と同じ形)で広げてから印・取る・書くを回す。書いた枠をつつく(CPU の PokeBlock)。
   枠ごとの (頁, 種類) は g_blocks から読む(今の試験の t0 の 2 語の代わり)。広げる時の欄の頁(一様の値が 0 でない配り手の欄)も同じ取り方で(CPU の ExpandWantedPages の順)。
6. 試験: field_owner の場面(配り手の往復・影・この刻みだけ)を GPU の木で・pageOf まで CPU とビット一致(HW・WARP)/ 新しい印の欄が無い世界は今とビット一致(gpu_multires・gpu_lab_box・gpu_tree_refine_placement)。
- 量の見込み: チャット 2〜3 回(未確認)。1 の決めが先(番号の揃え方が木の段の作りを決める)。

## 段 2 の 2 本目(2026-11-12 wt6 作業役。ブランチ t-1252-tree-gpu = t-1252-registry-owner-gpu の上)
- **1 が入った(CPU。司令塔の受け取り 15:10)**: 配り手の欄の疎な頁を、木の 1 回の処理の中で「取る」= 割り当ての段(Allocate の後の AllocateOwnerPages)で許可した要求の順に・
  「返す」= 解放の段(ReleaseAll)で返すセルの頁ごとに要求の順に。頁が要るかは値を作る前に(細かく = 頁を持つ親が欄の頁を持てば鎖の段の数・
  粗く = 親が頁を持つ枠になり〔広げる親も〕まだ欄の頁が無く、子が欄の頁か 0 でない一様の値を持つか広げる親の一様の値が 0 でない)。
  RefineOwners・ExpandPage は配り手の欄の頁を返さない(取った頁を使う)・Coarsen は配り手の欄の頁を返さない。一般の疎な欄は今のまま。ADR-0532 の 3 に入った形を書いた。
- **頁の番号を記録した試験・セーブの写し**: 探した(grep pageOf・freePages)。番号を見るのは field_owner・gpu_field_gather・multires_sparse(一般の欄だけ = 変わらない)。
  セーブは値の像(FieldPageValues)で頁の番号を持たない = 取り直しは無い。
- 試験: field_owner に「番号: 1 回の処理の要求の順に取る(空きの上 → 後ろに足す)・返す(要求の順)・往復の値は同じ」を足した。結果は下の作業ログ。
- **wt2 の T-1323 との突き合わせ(2026-11-12 wt2 の作業ツリーのチケットを読んだ)**: T-1323 は**刻みの段**で u5 に「刻む前のセルの写し」を結ぶ(SnapshotGrainCells・ReconcileGrownLayers。
  層を使う世界で粒を生む表だけ)。この仕事の 2 は**木の段**(BindLayers の間 = 要求の処理・畳む・TreeExpand)で u5 に欄の置き場を結ぶ予定 = 結ぶ時が違うのでぶつからない。
  ただし刻みの段の中の広げる段(RecordExpandPass・ExpandStepNode・ConductPrepareBlock)は 0 でない一様の値を持つ配り手の欄を広げる時に欄の置き場が要る(CPU の ExpandWantedPages の ExpandPage)。
  その時 u5 は T-1323 の写しなので、**欄の置き場は u4 の層の置き場の後ろに並べる**(1 つのバッファ。g_external2 = 欄の置き場の始まりの語)に変える方がよい(仮)。
  足す場所: T-1323 = 刻みの段の前後(写し・比べる段)/ この仕事 = Allocate・TreeRelease・CoarsenRequest(multires_graph.hlsl 811)・RefineLevel の層の配りの後・TreeExpand。同じ関数に足すのは RefineLevel と TreeExpand だけ(層の配りの後ろに欄の配り)。

## 段 2 の 3 本目(2026-11-12 wt6 作業役。ブランチ t-1252-tree-gpu-2 = t-1252-tree-gpu の上)
- **入った(司令塔の受け取り 15:55 の形 = u5 ではなく u4 の層の置き場の後ろ)**: GPU の世界の「欄の区画」。
  並び(common/multires_field_pages.hlsli): [欄の数 N][表 × N × 4 語(CPU の登録の番号・配り手の番号・置き場の始まり・一様の値の始まり。区画の始まりからの語)]
  [欄ごと: 置き場(ADR-0532 の 1 のまま)][一様の値 × 枠 × 値の数 × 2 語]。載せる欄 = セル・面の疎な欄のうち配り手の欄・この刻みだけの欄(GpuCarriesField)。
  GpuMultiresOptions::fieldPages(欄ごとの欄の頁の数。0 = 持たない)・fieldValues(値の数の和の上限。0 = SF_MAX_COMPONENTS)。
  バッファは層の置き場と同じ 1 本(m_layerBuffer。層か欄を持つ世界だけ作る)= 層の置き場 [0, LayerStoreWords) の後ろに区画(層を持たない世界は区画が 0 語目から)。
  BindLayers: g_external2 = 区画の始まりの語(FieldRegionStart)・g_external3 = 載せた欄の数(0 = 区画を読まない)。ルート署名は変わらない(64/64)。
  RecordUpload が区画を写す(MakeGpuFieldRegion)・Read が戻す(ApplyGpuFieldRegion。読み戻し先の nest が同じ欄を登録している時だけ)。
  **区画を持たない GPU(fieldPages = 0)は載せる欄のある世界を写さない**(RecordUpload が false。D-428)。今の main で載せる欄を GPU に写す道は無い
  (相の層の Ec = EnablePhaseLayerEnergy は CPU の試験だけ。grep 済み)= 既存の GPU の試験は影響なし。
- **途中の形の注意**: 木の段(粗くする・細かくする・広げる・集めて書く・畳む)はまだ区画を読み書きしない。fieldPages > 0 で載せる欄のある世界に細かく/粗くする要求・畳む・広げるを通すと配り手の欄が CPU と変わる
  (使い手が無い間だけの形。options のコメントに書いた)。
- 試験 `gpu_field_region`(HW・WARP): 1 写して戻す(配り手の欄 3 値 = 頁を持つ根の全セル + 一様な根の一様の値・この刻みだけの欄 2 値)→ 置き場(pageOf・空き・頁・一様)と要約が同じ /
  2 8 刻み(要求なし)で毎刻み CPU とビット一致 / 3 欄の無い世界で本物の鎖 70 刻み(区画あり・層の頁 0 と 16)毎刻み一致 / 4 区画を持たない GPU は断る。結果は作業ログ。

### 段 2 の残り(次の作業役へ。この順。足す場所は読んで確かめた)
1. **割り当ての段(multires_tree.hlsl の TreeAllocate・340 行あたり)**: 層の頁(RequestLayerNeed → LayerInclusiveScan → MrLayerRequestAddress に要求ごとの始まり)と同じ形で、
   載せた欄ごとに「要求の欄の頁の数」(CPU の multires_tree.cpp の AllocateOwnerPages〔354 行〕と同じ規則: 細かく = 頁を持つ親が欄の頁を持てば鎖の段の数・
   粗く = 親が頁を持つ枠になりまだ欄の頁が無く、子が欄の頁か 0 でない一様の値を持つか広げる親の一様の値が 0 でない)を要求の順のプレフィックス和で取る
   (k 番目 = 空きのスタックの上から k 番目・その後は高さ + k − 空き = MfTakesFreePage / MfFreePosition / MfFreshPage)。欄の数は g_external3 の回し(各欄 1 回のスキャン)。
   要求ごとの始まりの置き場が要る(区画に [欄 × MR_MAX_REQUESTS] を足すか、置き場の頭の後ろに。CPU は容量の上限が無い = GPU は尽きたら数える〔断らない。CPU と揃えるため容量を足りる数に〕)。
   **注意**: 64bit の値を比べる所(子の値が 0 でないか)は下位・上位の 2 語の OR で(64bit の引き算と比べを並べない)。
2. **粗くする段**: multires_graph.hlsl の CoarsenRequest(811 行あたり)の一般の段の後に SfOwnerCoarsen(owner の分かれ = T-0397。表の MF_TABLE_OWNER)。余りは帳簿の「粗くした端数・出」の列(CPU の CoarsenOwners)。
3. **細かくする段**: RefineNode の層の配り(ChildLayerPage・CopyLayerCell。multires_graph.hlsl 146〜245 行)の後ろに欄の配り(SfOwnerRefineChild / SfOwnerRefineUniform・親の八分の一を 0 に)。
   子の欄の頁は 1 の始まり − 1 − depth(層の MrLayerRequestAddress と同じ取り方)。影は RefineShadow・引き戻しは SfOwnerPullBackChild。
4. **解放の段**: TreeRelease(443 行)の ReleaseLayerPages(488 行)の後ろに、返すセルの頁の欄の頁を要求の順に空きのスタックへ(CPU の ReleaseOwnerPages〔multires_fields.cpp 1126〕)。
5. 畳む(TreeFold)・広げる(TreeExpand)・集めて書く段(multires_field_gather.hlsl を区画の置き場で)・SF_OWNER_PHASE_LAYER のセルを読む配り(T-1231)。
6. 試験: gpu_field_region に field_owner の往復(細かく → 粗く・影)を足し、pageOf まで CPU とビット一致(HW・WARP)。gpu_multires・gpu_lab_box・gpu_tree_refine_placement・gpu_multires_layers は欄の無い世界。
- wt2 の T-1323 との重なり: 1〜4 は T-1323 が触らない段(Allocate・CoarsenRequest・TreeRelease)。同じ関数は RefineNode の層の配りの後(3)と TreeExpand(5)だけ。u5 は T-1323 の写しのまま(この形は u5 を使わない)。

## 段 2 の 4 本目(2026-11-12 wt6 作業役。ブランチ t-1252-tree-gpu-3 = t-1252-tree-gpu-2 の上)
- **入った(「段 2 の残り」の 1〜4 と 6 の一部。ADR-0500 改訂・ADR-0532 の 2・3 の形のまま。新しい口は足していない)**:
  `shaders/sim/multires_field_access.hlsli`(新。木の段の区画の道具: 表・置き場の頭・pageOf・値・一様の値・LoadCarriedCell〔= CPU の LoadNestField〕・
  CoarsenKeepsUniformCarried・CarriedOwnerNeed〔= CPU の AllocateOwnerPages の規則〕)。multires_tree.hlsl・multires_graph.hlsl が include。
  - TreeSettle: 載せた欄があれば頁を持つ子は一様な親を広げる・一様な子は配り手の欄 = SfOwnerCoarsen が親の値で余り 0・この刻みだけの欄 = 同じ値(CPU の CoarsenKeepsUniform。64bit の比べは 2 語で)。
  - TreeAllocate: 欄ごとに要求の順の累積和(gs_layerScan を使い回す)で k 番目 = 空きの上から k 番目・その後は高さの上(ADR-0532 の 2)。取った頁を 0 で埋め、空きの数・高さを書き直す。
    **ticket の案(要求ごとの始まりの置き場)ではなく、割り当ての段で pageOf まで結ぶ形にした**(取るセルの頁は state.pageBase で決まっている = CPU の TakeOwnerPage と同じ時)。置き場を足さない。
    取れる数は容量だけ(CPU の木の段は予算を見ない)。尽きたら細かくする/粗くする要求を断る(GPU だけ。層の頁と同じ扱い・欄 0 の頭の MF_HEADER_TREE_SHORTAGE = 10 語目に数える)= 仮。
  - RefineLevel(要求の鎖の本物だけ): 層の配りの後に SfOwnerRefineChild / SfOwnerRefineUniform(一様な子はこの刻みだけの欄の一様の値も親の写し)・親を覆う所で親の欄の頁の八分の一を 0 に。
  - CoarsenRequest(既定と溢れの両方の変種): 広げる親は八分の一の外を一様の値(覆われたセルは 0 = CPU の ExpandPage)・八分の一は SfOwnerCoarsen(一様のままの親は書かない)。
  - TreeRelease: 返すセルの頁の載せた欄の頁(配り手の欄とこの刻みだけの欄)を欄ごとに要求の順に空きへ(CPU の ReleaseOwnerPages・Coarsen の ReleasePage)。頁の中身は消さない(CPU と同じ)。
  - RecordUpload: 保存量の配り手の欄(粗くする余りの帳簿の列が GPU に無い)・相の層の配り手の欄(セルを読む = T-1231)は写さない(D-428)。RecordRefineShadow は載せる欄があれば誤りを記録。
- **この刻みだけの欄は木の段では全部 0 の前提**(刻みの境目で 0 = TickOnlyFieldsClear。木の段は刻みの外)。細かく・粗くは値を書かず、頁を返すだけ。0 でない値が木の段に来ると CPU とずれる。
- 試験 gpu_field_region に 5(頁を持つ根の 2 段の鎖・一様な根の 1 段を細かく → 2 段目と一様な子を粗く → 1 段目を粗く。毎回 置き場〔pageOf・空き・頁・一様〕と要約が CPU とビット一致)・
  6(一様な子の配り手の値を変えて写し、粗くすると親が頁に広がる + この刻みだけの欄の 0 の頁を持つ子を粗くして頁が返る)。結果は作業ログ。

- **wt2 の T-1240 7 の 5(t-1240-wide-layers-2)との突き合わせ(2026-11-12 wt2 のチケットの引き継ぎメモを読んだ)**: wt2 は TreeSettle に溢れの変種(multires_tree_wide_settle・SameTailLayers)・
  CoarsenRequest の溢れの変種に WriteWideCoarsenOctantLayers(WriteCoarsenTails の後)・multires_tree_wide_expand・TreeResolve の CoarsenLayersFit を足す。
  この仕事は TreeSettle の層の比べの直後に CoarsenKeepsUniformCarried の 1 か所・CoarsenRequest の「層の前(#ifndef MR_WIDE_CELLS の前)」に CoarsenCarriedFields・
  TreeAllocate・TreeRelease・RefineLevel の層の配りの後と親を覆う所。**文字の衝突の見込みは TreeSettle と CoarsenRequest の数行**(両方残せば足りる。順は結果に効かない = どちらも pageNeed を 1 にするだけ・欄と層は別の語)。

### 段 2 の残り(次の作業役へ。この順)
1. **影の鎖**: RefineLevel の影(record.request = MR_NO_BLOCK)は CPU の RefineShadow(StartPage → 0 でない値を書く時に頁を作る)。GPU は並べて取れないので、
   影の頁の欄の頁は「親が欄の頁か 0 でない一様の値を持てば取る」に CPU を揃える(ADR-0532 の 3 と同じ「値を作る前に決める」)か、影の鎖は 1 本ずつなのでスレッド 0 が順に取る(番号は CPU と同じ順)。引き戻し(PullBackNode)は SfOwnerPullBackChild。
2. **畳む(TreeFoldCheck・TreeFold)**: CPU の PageFoldable は欄の値が FoldValue と同じかを見る・畳むと欄の頁を返す(枠の順)。GPU は欄を見ずに畳むので、載せる欄のある世界で RecordFoldPages を通すと違う。
3. **広げる(TreeExpand)**: CPU の ExpandWantedPages → ExpandPage(0 でない一様の値を持つ配り手の欄は割り当て済みの頁…ではなく広げる段で取る。順は枠の順)。
4. **集めて書く段**を木の段に(multires_field_gather.hlsl を区画の置き場で。書いた枠をつつく)。
5. **保存量の配り手の欄**: 粗くする余りを世界の帳簿の列(CPU の NestFieldLedgerBase + 保存量の番号)へ。GPU の帳簿の列の数を確かめて表に列を足す(今は RecordUpload が断る)。
6. 相の層の配り(T-1231)・T-1230 段 2 ③ の残り。

## 段 2 の 5 本目(2026-11-12 wt6 作業役。ブランチ t-1252-tree-gpu-4 = t-1252-tree-gpu-3 の上)
- **入った(「段 2 の 4 本目」の残りの 1〜3。ADR-0500 改訂・ADR-0532 の 2・3 の形のまま。新しい口は足していない。ADR-0532 の 7)**:
  - **影の鎖**(multires_graph.hlsl の RefineShadowCarried・RefineLevel の影の道): CPU の RefineShadow と同じに、影の頁が前から持つ欄の頁を空きへ返し(StartPage)、
    0 でない値を書くなら 1 つ取る(StorePage の EnsurePage = 空きの上から・無ければ高さの上)。影の鎖は 1 段 = 1 グループで順なので、スレッド 0 が段の順に取る
    (番号は CPU と同じ。並べて取る形にしない = CPU の頁の取り方を変えない)。値は一般 = SfRefineValue・配り手 = SfOwnerRefineChild / 一様な親は SfOwnerRefineUniform。
  - **引き戻し**(PullBackCarried・PullBackNode): 親のセルごとに SfPullBackChild / SfOwnerPullBackChild。影が欄の頁を持たず 0 でない値を書くなら 1 つ取る(新しく取る頁は 0 と読む)。
    RecordPullBack が BindLayers する(前は結ばなかった)。RecordRefineShadow の「影の鎖は配り手の欄をまだ配らない」の誤りの記録を外した。
  - **畳む**(multires_tree.hlsl の CarriedFoldable・FoldCarriedUniform・ReleaseFoldedCarried): TreeFoldCheck が調べる刻みの頁の全部 0 の欄の頁に返す印
    (pageOf の上の bit = CARRIED_RELEASE_MARK。欄の頁の番号 < 容量 < 2^31。TreeFold が必ず消す)を付け(畳めなくても返す = CPU の FoldExact の ReleaseZeroPages)、
    欄が一様でなければ畳まない(PageFoldable。64bit は 2 語の比べ)。畳むなら一様の値 = 値のセルの値(FoldPage)。TreeFold が枠の順に、畳む頁と印の付いた頁の欄の頁を欄ごとに空きへ積む。
  - **広げる**(ExpandCarried・TreeExpand): 頁を配った一様な枠が覆われていないセルを持ち欄の一様の値に 0 でないものがあれば、欄ごとに枠の順で 1 つ取り(k 番目 = 空きの上から k 番目)、
    覆われていないセルに一様の値・覆われたセルに 0(CPU の ExpandWantedPages の ExpandPage)。一様の値は消さない(CPU と同じ)。
  - 影・広げる段で欄の頁が尽きたら頁なしで数える(MF_HEADER_TREE_SHORTAGE。断れない = CPU とずれる。層の頁と同じ扱い・仮)。
- 試験 gpu_field_region に 7(影の鎖: 頁を持つ根の 2 段 → 一様な根の 1 段〔同じ影の枠 = 返して取り直す〕→ もう一度 2 段 → 影の値を変え、親のこの刻みだけの欄に 0 でない値を書いて引き戻す〔影が新しい頁を取る〕)・
  8(畳む: 頁を持つ根は畳まれず全部 0 のこの刻みだけの欄の頁だけ返す・広げた一様な根は畳まれて欄の頁を返す)・9(multires_uniform_scene の活性の刻み 123 刻み: 木箱の根が刻みで広がり〔TreeExpand が欄の頁〕、燃え尽きて畳まれる)を足した。結果は作業ログ。

### 段 2 の残り(次の作業役へ。この順)
1. **集めて書く段を木の段に**(multires_field_gather.hlsl を区画の置き場で。WANTED → TreeExpand で広げてから印・取る・書く。書いた枠をつつく = CPU の PokeBlock)。
   使い手(T-1227 の光の持ち越し・T-1228 の源の項)が来るまで呼ぶ所が無い。作業の並び(blockOf・値)の置き場をどこに持つか(区画の後ろ)を使い手と一緒に決める。
2. **保存量の配り手の欄**: 粗くする余りを世界の帳簿の列(CPU の NestFieldLedgerBase + 保存量の番号)へ。GPU の帳簿の列の数を確かめて表に列を足す(今は RecordUpload が断る)。
3. 相の層の配り(T-1231)・T-1230 段 2 ③ の残り。
- wt2 の t-1240-wide-layers-2 との重なり: この本で足したのは RefineLevel の影の道(層の配りの後・`fromRequest && real` の else)・PullBackNode のセルの書き込みの後・
  TreeFoldCheck(`foldable` を決めた直後と一様の枠の層を書いた後)・TreeFold(FoldBlock の前)・TreeExpand(ExpandLayers の後)。wt2 の溢れの畳む・広げる(multires_tree_wide_expand)と同じ関数に数行ずつ = 文字の衝突の見込み(両方残せば足りる。欄と層・溢れは別の語)。

## 段 2 の 6 本目(2026-11-12 wt6 作業役。ブランチ t-1252-gather-tree = main bf564b48 の上)
- **入った(「段 2 の 5 本目」の残りの 1 = 集めて書く段を木の段に。ADR-0532 の 8。新しい口は足していない = 登録・見立て・欄の持ち方は今のまま)**:
  - 段の中身を `shaders/sim/multires_field_gather.hlsli` に 1 つに(置き場 1 つの試験の段 multires_field_gather.hlsl と、GPU の世界の
    multires_field_gather_tree.hlsl が含める。含める側が置き場・作業・枠のセルの頁の読み書きを定義する)。試験の段の結果は前と同じ(gpu_field_gather)。
  - `GpuMultires::RecordGather(list, ring, field, work)`: 載せた欄 field(CPU の登録の番号)へ、書き手の作業 work(u5。並びは
    multires_field_pages.hlsli の「作業」= CPU の NestGatherWork の像)を書く。CPU の GatherNestField と同じ順:
    印(作業のある一様な枠に MR_PAGE_WANTED)→ TreeExpand(BindLayers の定数。層・載せた欄も広げる)→ 埋める(一様の値で。OhNestFill と同じ)→
    印を付け直す(一様のまま = セルの頁の不足)→ 欄の頁を枠の順に取る → 足して作業を 0 に・書いた枠をつつく(PokeBlock = この刻みの種)。
    `ReadGatherReport(field)` = CPU の NestGatherReport(広げた枠は置き場の頭の 11 語目 MF_HEADER_EXPANDED)。溢れを使う世界・載せていない欄は false。
  - **作業の置き場は書き手が持つ(仮)**: 区画の後ろには置かず、書き手のバッファを u5 に結ぶ(u5 は刻みの段の間だけ T-1323 の写し。集めて書く段は刻みの段の後なのでぶつからない)。
    作業は刻みの中だけ(保存点・ハッシュに入らない)なので区画に入れる理由が無い。blockOf の付け方(書き手が作業のブロックを取る)は書き手の段の都合
    (atomic で取ってよい。結果は枠の順に並べて書くので番号によらない)。T-1227 / T-1228 の段 2 がこの形で書く。
- 試験 gpu_field_region に 10(配り手の欄へ頁を持つ根・一様な根〔広げて欄の頁を一様の値で埋めてから〕に足す / この刻みだけの欄へ予算 1 で両方の根に足す = 1 つ取って書き、1 つは作業に残る)。
  置き場(pageOf・空き・頁・一様)・状態の要約・報告の 5 つの数・作業の残り・種(書いた枠をつつく)が CPU とビット一致。結果は作業ログ。
- まだの所: セルの頁が尽きて一様のまま残る枠(cellPageShortage)の GPU の場面は試験に無い(数える道は同じ印の段)。

### 【検討・司令塔の印】GPU の器が尽きた時の約束を 1 つの芯に(欄の頁・層の頁・溢れの塊。Q189・T-1341・T-1340 と同じ形。コードは書いていない)
- 同じ形の問題: GPU の置き場は起動時に大きさが決まる器。尽きた時に **CPU が同じ規則で同じことをしない**と結果がずれる。今ずれうる器:
  欄の頁(T-1252: 影の鎖・広げる段は断れず頁なしで数える)・層の頁(T-1240: 同じ = gpu_multires.h の layerPages の注意)・
  溢れの塊(T-1341 / Q189: 頁のセルを待たせる)・溢れの世界の層の組(T-1340: 9 組目)。ずれない器 = セルの頁・端数の枠(CPU も同じ数を持ち、
  断れる所〔要求を断る・広げずに種に残す〕でだけ尽きる = 同じ規則)。
- 芯の案(器ごとに起動時の見積もりを持ち、3 つの扱いのどれかを決めて書く。RecordUpload が器の帳簿を作る):
  1. **CPU と同じ大きさ・同じ断り**(セルの頁・端数の枠。今のまま): 尽きても断れる所でだけ使われる器。
  2. **構造の上限で作る = 尽きない**: 上限が式で決まり払える器。欄の頁はセルの頁 1 つに高々 1 つ(pageOf が 1 対 1。1 回の木の処理の中でも、
     欄の頁を持つのはその時にあるセルの頁だけ)なので **欄ごとの容量 = セルの頁の数**。この刻みだけの欄は木の段で頁を取らない(全部 0)ので頁の予算(D-468 の前もって決める数)。
     層の頁も同じ見込み(セルの頁 1 つに層の頁 1 つ。未確認 = T-1240 で確かめる)。費用: 欄 = セルの頁 × 512 × 値の数 × 8 B
     (例 セルの頁 1024・12 値 = 48 MiB / 2 値・予算 256 = 2 MiB)。
  3. **上限が払えない器 = 作り手の上限 + 尽きたら結果を変えずに遅くする**(D-468・D-401 改訂): 溢れの塊(上限 = 頁 × 2 面 × 表の物質。表 64 種・頁 4096 で約 2.7 GB)・溢れの世界の層の組。
     尽きた刻みは数え(今の器)、CPU が器を広げて同じ刻みを刻み直す(Q189 の案 C)。刻み直しの道(刻みの初めの状態を持つか)は未確認 = 設計が要る。
     それまでの仮 = 尽きた刻みで止めて開発ビルドの誤りにする(黙ってずれない)。
- **作り手から見た違い**: 2 の器(欄の頁・層の頁)は大きさを指定しなくてよくなり、尽きてずれることが起きなくなる(fieldPages・layerPages は上書きの上限として残し、
  見積もりより小さい世界は RecordUpload が写さない = D-428)。指定させるのは この刻みだけの欄の頁の予算 と 3 の器の上限(wideChunks)だけ。
  3 の器は 尽きると遅くなる(案 C の後)/ 止まって誤りが出る(仮)= 黙ってずれることは無くなる。
- 費用: 見積もりは起動時の掛け算だけ(刻みの費用 0)。VRAM は 2 の器が上限まで増える(上の例)。今の試験(欄の容量 8 頁)は予算か上限の上書きに直す。
- おすすめ: 芯 = 「器ごとに 1〜3 を決めて帳簿に書く」。欄の頁と層の頁は 2(すぐできる)、溢れの塊と層の組は 3(仮 = 止めて誤り・案 C は設計チャットで)。
  退けた案: CPU にも欄の頁の容量を持たせて同じ規則で断る(影・広げるは断れず、どちらも値を落とす = 量を捨てない D-202 に反する)・今のまま(黙ってずれる)。

### 段 2 の残り(次の作業役へ。この順)
1. **保存量の配り手の欄**: 粗くする余りを世界の帳簿の列(CPU の NestFieldLedgerBase + 保存量の番号)へ。GPU の帳簿の列の数を確かめて表に列を足す(今は RecordUpload が断る)。
2. 相の層の配り(T-1231)・T-1230 段 2 ③ の残り。
3. 上の器の約束(司令塔の決めの後。2 なら RecordUpload の見積もりと試験の容量)。
4. 書き手: T-1227 段 2(RC の作業 → 葉の頁)・T-1228 段 2(源の項 → 当てる段)は RecordGather を呼ぶ。作業の並びは MfWork*(書き手のバッファ)。

## 段 2 の 7 本目(2026-11-13 wt6 作業役。ブランチ t-1252-conserved-owner = t-1252-gather-tree の上)
- **入った(「段 2 の 6 本目」の残りの 1 = 保存量の配り手の欄。ADR-0532 の 9。新しい口は足していない = 登録・見立て・欄の持ち方・帳簿の並びは今のまま)**:
  - 区画の表に 5 語目 `MF_TABLE_LEDGER`(保存量の欄の帳簿の列の始まり = CPU の NestFieldLedgerBase + NestConservedIndex。保存量でなければ MF_NO_LEDGER_COLUMN)。MF_TABLE_WORDS 4 → 5
    (区画の大きさの上限 MfRegionWordsBound は表の語の数で数えるので自動で足りる)。
  - 帳簿の列の数を確かめた: GPU の世界の帳簿は CPU の nest.capacity.ledgerColumns で作る(GpuMultires::Create・RecordUpload が大きさを比べる)。
    保存量の欄を登録すると CPU の capacity.ledgerColumns が値の数だけ増える(RegisterNestField)= 欄の列は GPU の帳簿にもう有る。列を足す必要は無かった。
  - CoarsenCarriedFields(multires_graph.hlsl。既定と溢れの両方の変種): SfOwnerCoarsen の余りを `AddWorldLedgerUnits(粗くした端数・出, 子のレベル, 列 + 値の番号, 余り)`
    (CPU の CoarsenOwners の AddFieldRemainder と同じ段・同じ列。一様のままの親でも足す = 余りは 0。atomic の足し算なので順によらない。64bit の引き算と比べは無い)。
  - RecordUpload は保存量の配り手の欄を断らない(相の層の配り手の欄 = T-1231 は今のまま断る)。
- 試験 gpu_field_region に 11(保存量の配り手の欄で 5 の木の段: 細かく → 鎖の子の値を 8 で割り切れない値に変えて写す → 粗く 2 回。
  帳簿・置き場・要約が毎回 CPU とビット一致・帳簿が動く〔余りが載る〕・ComputeConservedTotals が粗くする前後で CPU・GPU とも同じ)。結果は作業ログ。

### T-1231(相の層の配り)の GPU の設計の詰め(コードは次の作業役。読んで確かめた所と未確認を分けた)
- 足す場所(読んで確かめた):
  - **細かく**: RefineLevel(multires_graph.hlsl 284〜)。CPU は Refine の最後の RefineOwners = セルの中身の配り(置かれ方 PlaceRefineOctantCells か SAME)の**後**にセルを読む。
    GPU は今 RefineCarriedFields がセルを写した直後・`#ifdef MR_REFINE_PLACE` の PlaceRefineChildren の**前**にある。相の層の欄だけは
    PlaceRefineChildren の後・親を覆う前に `DeviceMemoryBarrierWithGroupSync` を 1 つ挟み、t < 64(親の八分の一のセル)が子 8 つ(g_cells の子の頁)・親のセル・親の Ec を読んで
    子のエネルギー(g_cells)と子の Ec(区画の欄の頁)を書く(CPU の RefinePhaseLayerOctant)。一般の配り手の欄(試験用)は今の場所のまま(セルを読まない)。
    一様な子(uniformChild)・影の鎖(RefineShadow・PullBackShadow はセルを読まない一般の配り)は今の SfOwnerRefineUniform / SfOwnerRefineChild のまま = CPU と同じ。
  - **粗く**: CoarsenRequest の CoarsenCarriedFields(1152 行あたり。セルの中身と一般の欄の後 = CPU の Coarsen の最後の CoarsenOwners と同じ順)。
    相の層の欄で `paged`(頁を持つ親か広げる親。CPU は ExpandPage の後なので広げる親も「頁を持つ親」)なら、親の八分の一のセル t の粗くしたセル(同じスレッドが書いた g_cells か要確認)と
    子 8 つのセル・子の Ec から PlCoarsenCondensedEnergy(CPU の CoarsenPhaseLayerOctant)。一様のままの親は今の SfOwnerCoarsen(一般)。保存量でない = 帳簿は無い。
  - 溢れを使う世界(MR_WIDE_CELLS)は CPU が一般の配り(`!nest.wideCells` の時だけ相の層の配り)= GPU も `#ifndef MR_WIDE_CELLS` の中だけ。
  - TreeAllocate の頁の要る/要らない(CarriedOwnerNeed)は配り手によらない規則 = そのまま。RecordUpload の断り(SF_OWNER_PHASE_LAYER && !wideCells)を外す。
- 読んで確かめた: 細かくする段の置かれ方(multires_refine_place.hlsli)が g_species(t0)を読んでいる = 木の段のグラフで物質の表は結ばれている(GpuReactionTable で包めばよい)。
  粗くする段の親の八分の一のセル t は同じスレッド t が `result.cell` を書く(CoarsenRequest 1146 行)= 相の層の粗くする配りは result.cell を引数で渡せばバリア不要。
  広げる親の八分の一の外のセルは別のスレッドが書くが、相の層の配りは八分の一のセルだけ読む。
- 未確認: 溢れの変種(MR_WIDE_CELLS)の CoarsenRequest も result.cell の形か(CPU は溢れの世界で相の層の配りを使わないので、`#ifndef MR_WIDE_CELLS` で外せば読まなくてよい)。
- 技術の注意: CPU の RefinePhaseLayerCell は RxCell の配列 children[8]・PlPartSums の配列を `j == last` で選ぶ = GPU では書き換えた構造体の配列の動的な添字(T-0397)にしない
  (子のセルは g_cells から j ごとに読み直す・FX_UNROLL の 8 回の回し・最後の子は回しの中の比べで選ぶ)。dxil_alloca_check で確かめる。
  PlRefineHeatsOf の `(parent.energy − Ec) × 8`・SplitPart の `total − given` は 64bit の引き算の後に比べが無い。PlCoarsenCondensedEnergy の床と天井は PlAtLeastS64(2 語で借りを回す形)= 今の規則の中。
  PlRefinePartEnergy の `(uint32_t)(chemicalMj × micro − childChemical)` と比べは 32bit に落としてから(RTX の上の 32bit の壊れに当たらない見込み = HW の試験で確かめる)。
- 試験の形: gpu_field_region に場面 12(EnablePhaseLayerEnergy の世界・CPU の phase_layer_tree の混ざったセルの場面で、頁を持つ根の鎖を細かく → 粗く・広げる親を含む)を HW・WARP で CPU とビット一致。
  混ざったセルの無い木は今とビット一致(gpu_multires・gpu_multires_layers・gpu_tree_refine_placement)。量の見込み: チャット 1 回(未確認)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29(本体 16 回目・設計): ADR-0500 の改訂で作った。
- 2026-10-29(本体 18 回目・段 1): 登録の印 2 つ・配り手の表(試験用 1 つ)・セルの配りの口(細かく/粗く・影)・集めて書く段・この刻みだけの読む口と境目の検査・SF_MAX_COMPONENTS 12 を CPU に。
  試験 field_owner を足した。今の欄の試験(multires・multires_state・multires_sparse・field_storage・placement_refine・window_lab ほか)は下の結果。費用は docs/perf.md(数え)。残りは busyTick を 1 つの関数へ(T-1228 と一緒がおすすめ)と段 2。
- 試験(release・警告 0・archmap --check OK・float_check〔state_field.hlsli・multires_fields・multires_nest.h〕OK): field_owner・multires・multires_state・multires_sparse・field_storage・placement_refine・tree_refine_placement・
  multires_wide・multires_layers・multires_activity・multires_quiet・multires_uniform・multires_graded・window_lab・window_lab_blank・window_lab_compare 合格(新しい印の欄が無い世界は今と同じ)。
  **warm_start_save が 1 件落ちた**: 「支え 片持ち梁 16 × 4 m 1 つの系: 向きの欄を 0 にしても答えが変わらない」(C の確かめ。A・B は合格)。この仕事は支えの法則と温めた始まりの欄の道に触っていない
  (欄は導かれる値・SAME のまま、細かく/粗くを通らない)。最後に合格したのは wt5 の 2026-10-28 20:19 で、その後 main に T-1207(support_law)・T-1133(support_law)が入った。main でも落ちるかは未確認(司令塔が main で warm_start_save を 1 本流して確かめる)。
- 2026-10-29(本体 18 回目・マージ): warm_start_save の落ちはこの仕事のせいではなかった。原因は T-1206(440ddf8b。固定した面を面の点の形に)で、その後は保存点 20 の刻みで持つ向きを捨てても答えが変わらない(試験が弱くなった)。main の 8f483c47 で試験の保存点を 10 に(2・5・10・15 では捨てるとずれる)。
  rebase main(NEXT・perf の衝突は両方残した)→ release ビルド警告 0 → field_owner・multires・multires_state・multires_sparse・field_storage・placement_refine・tree_refine_placement・multires_uniform・window_lab・warm_start_save・float_check 5 本 合格・archmap --check OK → main へ --no-ff。
  **段 1 の残り**(セルの真実を書く道を 1 つにまとめて busyTick を書く所)は T-1228 段 2 と一緒に(段 2 の GPU は wt2)。Status は段 1 の主な形が main に入った状態のまま In Progress。
- 2026-11-12(wt6 作業役・段 2 の 1 段目): GPU の欄の頁の置き場の並び・集めて書く段の Compute・CPU ⇔ GPU の像(ADR-0532)。gpu_field_gather(HW・WARP)で頁の番号・空きのスタックまで CPU とビット一致。
  木の段への載せは「段 2 の残り」(1 は印 = CPU の頁を返す順の変更の確かめが先)。GpuMultires は変えていない。
- 2026-11-12(wt6 作業役・段 2 の 2 本目): 段 2 の残りの 1(CPU の配り手の欄の頁を割り当ての段で取る・解放の段で返す)を t-1252-tree-gpu に。ADR-0532 に司令塔の受け取りと入った形。
  試験(release 警告 0): field_owner(番号の試験を足した)・multires・multires_state・multires_sparse・field_storage・placement_refine・tree_refine_placement・multires_uniform・multires_layers・
  multires_activity・window_lab・phase_layer_world・warm_start_save 合格。2(GpuMultires に置き場)以降は次の作業役へ(上の「段 2 の 2 本目」の T-1323 との突き合わせを先に読む)。
- 2026-11-12(wt6 作業役・段 2 の 3 本目): GPU の世界の欄の区画(u4 の層の置き場の後ろ・g_external2/3。ADR-0532 の 5)を t-1252-tree-gpu-2 に。rebase main(50a6cad6 = T-1323 の後。gpu_multires.h の衝突は両方残した)。
  試験(release 警告 0。rebase の後): gpu_field_region・gpu_field_region_warp(新)・gpu_multires・gpu_multires_layers(_warp)・gpu_lab_box・field_owner・dxil_alloca_check / rebase の前: gpu_tree_refine_placement(_warp)・gpu_field_gather も合格・archmap --check OK。
  初めの版は一様な根を反応する空気にして刻み 0 で食い違った(CPU は広げる時に配り手の欄の一様の値から頁を作る・GPU の TreeExpand はまだ区画を読まない = 段 2 の残りの 5)。木の段の載せは上の「段 2 の残り」1〜6。
- 2026-11-12(wt6 作業役・段 2 の 4 本目): GPU の木の段(TreeSettle・TreeAllocate・RefineLevel・CoarsenRequest・TreeRelease)が欄の区画を読み書き(ADR-0532 の 6)を t-1252-tree-gpu-3 に。
  試験(release 警告 0): gpu_field_region(HW 121 秒・新しい 5・6 を含む)・gpu_field_region_warp・field_owner・dxil_alloca_check 合格・archmap --check OK。
  欄の無い世界の GPU の組 gpu_multires・gpu_lab_box・gpu_tree_refine_placement・gpu_multires_layers(と warp。lab_box は HW だけ)7 本合格。残りは「段 2 の 4 本目」の「段 2 の残り」1〜6。
- 2026-11-12(wt6 作業役・段 2 の 5 本目): 影の鎖・引き戻し・畳む・刻みの広げる段が欄の区画を読み書き(ADR-0532 の 7)を t-1252-tree-gpu-4 に。試験の結果は次の行。
  rebase main(4af1dc44 = t-1240-wide-layers-2 の後。multires_graph.hlsl の UniformValueTail の後ろで衝突 = 両方残した)。dxil_alloca_check が 2 回落ちた(写した見出しの子の並びを動的な添字で読む:
  畳む段の MrFoldValueCell・影の親の LoadCarriedCell)→ 見出しのバッファから直に読む形(CarriedFoldValueCell・LoadCarriedSlotCell)で直した。
  試験(release 警告 0。rebase の後): gpu_field_region(HW 41 秒・7〜9 を含む)・gpu_field_region_warp(38 秒)・gpu_multires・gpu_multires_layers(_warp 564 秒)・gpu_multires_uniform・gpu_lab_box・
  gpu_tree_refine_placement・field_owner・multires・multires_sparse・float_check 5 本・dxil_alloca_check 合格・archmap --check OK。残りは「段 2 の 5 本目」の「段 2 の残り」1〜3(集めて書く段・保存量の配り手の欄・相の層)。
- 2026-11-12(wt6 作業役・段 2 の 6 本目): 集めて書く段を GPU の世界の木の段に(GpuMultires::RecordGather・段の中身を multires_field_gather.hlsli に 1 つ。ADR-0532 の 8)を t-1252-gather-tree に。
  試験(release 警告 0): gpu_field_region(HW 6 秒・新しい 10 を含む)・gpu_field_region_warp(48 秒)・gpu_field_gather(_warp)・field_owner・multires_sparse・float_check 5 本・dxil_alloca_check 合格・archmap --check OK。
  欄の無い世界: gpu_multires(_warp)・gpu_lab_box・gpu_tree_refine_placement(_warp)・gpu_multires_uniform・gpu_object_heat_nest 合格(既定の答え・ハッシュは変わらない)。
  器が尽きた時の約束を Q189(溢れの塊)と 1 つの芯にした検討を上に(司令塔の印)。残りは「段 2 の残り」1〜4。
- 2026-11-13(wt6 作業役・段 2 の 7 本目): 保存量の配り手の欄の粗くする余りを GPU の世界の帳簿の列へ(区画の表に帳簿の列・ADR-0532 の 9)を t-1252-conserved-owner に。
  試験(release 警告 0): gpu_field_region(HW 47 秒・新しい 11 を含む)・gpu_field_region_warp(46 秒)・gpu_field_gather(_warp)・field_owner・multires・multires_state・multires_sparse・
  float_check 5 本・dxil_alloca_check 合格・archmap --check OK。欄の無い世界: gpu_multires(_warp)・gpu_multires_uniform・gpu_lab_box・gpu_tree_refine_placement(_warp)合格(既定の答え・ハッシュは変わらない)。
  T-1231(相の層の配り)の GPU の設計の詰めを上に。残りは「段 2 の 6 本目」の「段 2 の残り」2〜4(相の層 = T-1231・器の約束・書き手)。
