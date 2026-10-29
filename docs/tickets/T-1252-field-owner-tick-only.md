# T-1252 登録に「配り手」と「寿命」・粗くする時のセルの配りの口・集めて書く段(土台。段 1 CPU / 段 2 GPU。ADR-0500 改訂)

- Status: In Progress(段 1 CPU の主な形は済み・2026-10-29 本体の作業役 18 回目。ブランチ t-1252-registry-owner。残りは下の「段 1 の残り」と段 2)
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
