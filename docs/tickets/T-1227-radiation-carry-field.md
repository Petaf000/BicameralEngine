# T-1227 放射の持ち越しを頁の形に → 登録の持ち主の欄に(ADR-0469 の 4-2・4-3。ADR-0500 改訂)

- Status: Todo(2026-10-27 wt6 が T-1161 から分けた・2026-10-29 本体 16 回目〔設計〕が ADR-0500 で形を決めた)
- 種類: 工学
- PC: 必須(GPU)
- 見積もり: チャット 2〜3 回分(段 1 / 段 2)
- マイルストーン: ROADMAP「土台の変更要求の束」の 4
- 設計: docs/design/20 §10.7・23-radiation.md §3.3・§3.4 / 決定: **ADR-0500**・ADR-0469 の 4-2・4-3・D-619・ADR-0484 / 親: T-1161

## 目的(1〜2 行)
RC の持ち越し(今は (セル・面・帯) の鍵の密な表を刻みの偶奇で 2 枚。CarryIn + RadiationHash が全部の鍵を掃いて 2 側 約 190 µs/刻み)を、
光の当たる頁だけの形にし、多重解像度の世界では登録の持ち主の欄(光の配り・保存量・ハッシュとセーブに入る)にする。

## 形(ADR-0500 の決定 1〜3・5)
- 欄: セル・12 値(入った面 6 × 帯 2)・SF_SCALE_OWNER(光の配り: 細かく = 面 f の側の子 4 つへ同じ重み・反対側は 0 / 粗く = 面 f の側の子 4 つの和)・真実・ハッシュに入る・保存量(光の持ち越しの列)。
- **偶奇の 2 枚は登録に足さない**: 欄は 1 枚。刻みの初めに CarryIn が読んで 0 にし、光線が歩く途中の次の刻みへの足し込みは RC の作業(密な升目・刻みの中だけ)へ。刻みの終わりに集めて書く段が欄へ。
- 鍵は (葉の枠・セル・面・帯)。T-1117 の段取り 4(出どころと袋の鍵の相対化)・段取り 5(受けの置き場)はこの並び。
- 頁が無い所: 集めて書く段が枠の順のプレフィックス和で取る(GPU がその場で取らない)。尽きたら計器の誤り(量は捨てない・D-468・D-202)。
- 帳簿の 4-3(ADR-0469 の (a)): 場所のある出入り = 世界の端・場所の無い出入り = 外への出入り 1 列。

## 完了条件(チェックできる形で)
- [ ] **段 1(今すぐ・仮の世界・土台に触らない)**: 持ち越しの表を「ブロックの頁 × セル × 12」の形に(光の当たったブロックの印 + 頁の表)。CarryIn・RadiationHash は印のブロックだけ。
      ハッシュ・帳簿・読み戻しが今とビット一致(gpu_radiation_cost・cascade_light・probe_radiation)。費用を perf.md に(2 側・外 288.15 K の 190 µs がどれだけ減るか)
- [ ] **段 2(T-1252 段 2 の後・T-1117 の段取り 4・5・T-1155 の残りと一緒)**: 多重解像度の世界で持ち越しを登録の欄(光の配り)にし、集めて書く段で書く。細かく/粗くで保存(光の列)・保存点から続けてハッシュ一致・CPU と GPU のビット一致
- [x] **段 1 の CPU(2026-10-29 wt4・司令塔の完了条件)**: 持ち越しの鍵を (葉の枠・セル・面・帯) に(sim/cascade_light の CascadeLeafCarry・FoldCarryToLeaves・SpreadLeafCarry・
      StepCascadeLightOnLeaves・RcCarrySpreadShare)。全部 0.5 m の木で升目の鍵と 6 刻みビット一致・1 m の根の木と混ざった木で帳簿が合う(cascade_tree_grid の 5)。
      上の GPU の「ブロックの頁 × セル × 12」の表と CarryIn・RadiationHash を印のブロックだけにするのは**まだ**(下の「段 1 の残り」)
- [x] **段 1 の GPU の頁の印(2026-10-29 wt4 3 本目)**: 持ち越しの表に頁の印・RcCarryIn と RadiationHash は印の頁だけ(下の「段 1 の残りの GPU の形」)。
      gpu_cascade_light(_warp)・gpu_cascade_tree_grid(_warp。4 = 葉へ畳んで 0.5 m の木で CPU の葉の鍵とビット一致・混ざった木で帳簿)・
      gpu_radiation_cost・gpu_probe_rewind・probe_radiation が通った。2 側の CarryIn + Hash 約 190 → 約 90 µs(perf.md)。
      木を読む形(CreateFromTree)の GPU の葉の頁へ atomic で畳む・広げるは段 2(頁の取り方が段 2 の集めて書く段)
- [ ] 帳簿の 4-3 の行
- [ ] release のビルド(警告 0)・archmap --check

## メモ・参考
- 段 1 の頁の形は段 2 の欄の並び(セル × 12 × 8 B = 48 KiB/頁)と同じにしておく(写し替えだけで載る)。
- 重なり: wt6 の T-1117(cascade_light.hlsl・gpu_cascade_light)。同じ作業役に配るのがよい。

## 段 1 の CPU の形(2026-10-29 wt4。仮の置き場は放射の側に閉じる・土台に触らない)
- 置き場: `CascadeLeafCarry`(0 でない値だけ・枠 → セル → 面 × 2 + 帯の順 = 段 2 の欄の並び)。鍵 = ((枠 × 512 + セル) × 12) + 面 × 2 + 帯。
- 畳む(升目 → 葉 = 光の配りの「粗く」): 葉のセルが覆う升目の面 f の値の**和**(割らない)。広げる(葉 → 升目 = 光の配りの「細かく」): 葉の面 f の側の升目の層
  (世界の箱の中の部分。2^−L 角の 1 枚 = 4^−L 個)へ同じ重み・最後の升目が残り(`RcCarrySpreadShare`。hlsli にあるので GPU も同じ源)。
  ADR-0500 の光の配り(木の段 1 つ = 面の側の子 4 つへ・反対側は 0)を升目まで一度にした形。段ごとに 4 つずつ配るのと一度に 4^−L へ配るのは
  床の端数の置き場が違う(和は同じ)ので、段 2 では「欄の配り手(段ごと)」と「升目への広げ(一度に)」を別の関数として持つ。
- 粗い葉の光は葉の面から出直す(升目の鍵では升目の面の位置を覚えていた)。量は保つ(畳む・広げるの和がビット単位で同じ = 試験 5)。結果は升目の鍵と違う(D-465 の性質で、
  粗い葉のセルの中の位置を持たない = 粗い葉は 1 つのセル)。
- 受けのあった葉(T-1228 段 1): `CascadeTouchedSlots`(吸った・放ったが 0 でない升目の葉の枠。昇順)。印(busyTick)を書くのは T-1252 の 1 つの関数なので、ここは数えて返すだけ。
- 費用: CPU だけ。std::map で畳む・広げる(試験の世界でだけ使う。刻みの道には載せていない)。

## 段 1 の残り(GPU。土台に触らずにできる。次の作業役)
- GPU の持ち越しの表(stateCarry0/1。cascade_light.hlsl・probe_radiation.hlsl の RadiationHash)を「光の当たったブロックの印 + 頁の表(ブロック × 512 × 12 × 8 B)」に。
  CarryIn・RadiationHash は印のブロックだけ。ハッシュの項は今の (セル・面・帯) のまま(ProbeCascadeCarryHashTerm)なので、印で飛ばしても今とビット一致の見込み。
  費用を perf.md へ(2 側・外 288.15 K の 190 µs)。試験: gpu_radiation_cost・gpu_cascade_light・probe_radiation。
- 木を読む形(CreateFromTree)で葉の鍵の持ち越し(上の CPU)を GPU に: 畳む = 升目ごとに葉の頁へ atomic の和・広げる = CarryIn が葉の面の層へ RcCarrySpreadShare。
  CPU の StepCascadeLightOnLeaves とビット一致(gpu_cascade_tree_grid に場面)。頁の取り方は段 2 の集めて書く段(枠の順のプレフィックス和)に合わせる = **段 2 で**。

- 引き継ぎ(2026-10-29 wt4 2 本目・未着手。形の案だけ): 頁の単位は「セル 512 個の連続 = 鍵 6144 個 = 48 KiB」(仮の世界 64³ は 512 頁)。
  印は表ごとに頁 1 語(uint32。stateCarry0/1 の後ろに 2 × 頁の数)。AddCarry が 0 から増やした時に頁の印を冪等の OR で立てる(今の RC_GPU_CARRY_COUNT と同じ所)。
  RcCarryIn は「頁 × 6144」を回して印の無い頁を飛ばし、読み終えた頁の印を 0 に。RadiationHash も出る表の印の頁だけ(読むだけ・0 にしない)。
  RcInitBags の初めの持ち越しも印を立てる。保存点は PersistentStateBytes に印の語を含める(読み戻し・写しの早送りの形がそのまま)。
  ハッシュの項は (セル・面・帯) のまま = 今とビット一致の見込み。確かめ: gpu_cascade_light・gpu_cascade_light_warp・gpu_radiation_cost・probe_radiation(CPU)。
  T-1228 段 1(RadiationGrid を PROBE_SCHEDULE_CHANGED_WORD で飛ばす)は「最後に写した刻み」を作業のバッファに持ち、ブロックの印 > それのブロックだけ写す。
  読み込んだ直後・表の差し替えの後は全部(RecordRadiationInitialization・RecordRadiationTableSwap で「最後に写した刻み」を 0 に)。gpu_radiation_cost に「全部変わった」の場面。

## 段 1 の残りの GPU の形(2026-10-29 wt4 3 本目。ブランチ t-1227-gpu-carry)
- 頁 = 鍵の連続 `RC_GPU_CARRY_PAGE_KEYS`(セル 512 個 × 面 6 × 帯 2 = 6144 鍵 = 48 KiB。段 2 の欄の頁と同じ大きさ)。仮の世界 64³ は 512 頁。
  頁は升目の番号の連続(段 2 の葉の枠の頁とは鍵の並びが違う。段 2 で枠の頁に置き換える)。
- 置き場: `RcGpuParams` の末尾に `carryPages`・`stateCarryMarks0/1`(状態のバッファの持ち越しの表 2 枚の直後・頁 1 語の uint32)。
  `LayOutState`(gpu_cascade_light.cpp)が置く。読み戻しと保存点(`PersistentStateBytes` = `m_readbackBytes`)の範囲に入る = 写しの早送り・巻き戻しはそのまま。
- 立てる: `AddCarry`(cascade_light.hlsl)が 0 から増やした鍵の頁に `MarkCarryPage`(冪等の OR)。`RcInitBags` の初めの持ち越しも。
  0 でない鍵は、表が最後に読まれて 0 に戻った後に必ず 0 から増えているので、印の無い頁の鍵はいつも 0(印で飛ばしても落とさない)。
- 読む: `RcCarryIn` は印の頁の鍵だけ(波の中は同じ頁なので分かれは揃う)。`RadiationHash`(probe_radiation.hlsl)は出る表の印の頁だけ・0 にしない。
- 0 に戻す: 読み終えた表の印は次の起動の `RcLevel0Prepare` の頭(RcCarryIn と同じ起動の中で戻すと同じ頁のほかのスレッドが飛ばす)。
- 起動の数は今のまま(歩幅で全部の鍵の番号を回し、印の無い頁は印 1 語を読んで飛ばす)。もっと減らすなら「印の頁の列 + 間接の起動」(費用を見てから)。
- 試験: gpu_cascade_tree_grid の 4(GPU の持ち越しを葉へ畳む: 0.5 m の木で CPU の StepCascadeLightOnLeaves とビット一致・どの木でも帳簿・受けのあった枠)。
  gpu_cascade_light(_warp)・gpu_radiation_cost・probe_radiation は今とビット一致の確かめ。

## T-1228 段 1 の残り(RadiationGrid を変わった印で飛ばす。次の作業役。読んだ所の要点)
- RadiationGrid(probe_radiation.hlsl の頭。1 スレッド = 1 セル・`LINEAR_CELL_GROUPS` で起動。記録は probe_sim.cpp の `ProbeSim::RecordRadiation`):
  刻みの初めのセル(`cells[GenerationBase(tick) + cell]`)と熱のキャッシュの温度 → 作業の `workLights`(RcCellLight 24 B)・`workTemperatures`。
  参照の温度の形では負の側の作業(u16)へも同じ値と `RC_GPU_WORK_EMITTERS`(側の放つセルの印。RcPlanTick が読んで 0 に)。
- 変わった印: `blockSchedule` の `PROBE_SCHEDULE_CHANGED_WORD + block * 2`(64bit = `LoadBlockMark`。probe_bindings.hlsli)。値は
  「最後に変わった刻みの印」(ProbeChangeMark(t) = t + 1)。書く所: 伝導(probe_conduct.hlsl。変わったら ProbeChangeMark(tick))・
  気体/液体/放射の当てる段/コマンド(ProbeChangeMark(tick) − 1)・表の差し替え(RefreshTable が全部のブロックを ProbeChangeMark(tick) − 1)。
  ブロックは 4³(PROBE_BLOCK_SIZE)・ProbeBlockOfCell。
- 案: 作業のバッファの頭に「最後に写した刻みの印」を 1 語(RcGpuParams の 256 B の中・RC_GPU_WORK_EMITTERS(248)の手前の空き)。
  写すのはブロックの印 > それのブロックだけ・写し終えたら(次の段の 1 スレッドが)今の刻みの印に。
  注意 1: 放つセルの印(RC_GPU_WORK_EMITTERS)は今は写した全部のセルで立てるので、飛ばすと「変わっていないが放つセル」で立たなくなる →
  印は飛ばしたセルでも温度・光の値(作業にある前の値)から立てる(作業を読むだけ)か、放つセルの数を作業に持つ。
  注意 2: 作業は保存点に入らない(PersistentStateBytes は状態のバッファの頭だけ)ので、巻き戻し・写しの早送り・表の差し替え・初めのフレームの後は
  「最後に写した刻み」を 0 に(RecordRadiationInitialization・RecordRadiationTableSwap・RecordRestore・RecordAdopt)。
  注意 3: 熱のキャッシュの温度だけ変わってセルが変わらない道が無いか(RefreshTable は全部変わった扱いなので可)。
  確かめ: gpu_radiation_cost に「印を全部変わったにした時」の場面(刻みごとの世界のハッシュが全部一致)・費用を perf.md に(Grid 83 µs)。
- RcSources の放つセルの一覧を印で直す分も同じ印で(T-1228 の完了条件)。

## 段 2 に要るもの(T-1252 段 2 の後)
- 登録に持ち越しの欄(セル・12 値・SF_SCALE_OWNER の光の配り・保存量の列)。SF_MAX_COMPONENTS 12(T-1252)。
- 配り手の関数 2 つ: 木の段ごとの細かく(面 f の側の子 4 つへ同じ重み・反対は 0)/ 粗く(面 f の側の子 4 つの和)。上の升目への広げ(一度に 4^−L)とは端数の置き場が違うので別の関数。
- 集めて書く段: RC の作業(升目の鍵の密な表)→ 葉の頁(枠の順のプレフィックス和で頁を取る)。今の FoldCarryToLeaves の和の順と同じ(整数の和なので順によらない)。
- CarryIn: 欄の頁から読む(SpreadLeafCarry の GPU 版)・読んで 0 にする。RadiationHash は欄のハッシュ(一般の段)へ = RC の中のハッシュの項は消える。
- 受けのあった葉: 当てる段の 1 つの関数が busyTick を書く(T-1228 段 2)。CascadeTouchedSlots は試験の確かめに残す。
- 新しい ADR は要らない見込み(ADR-0500 の決定 1・3 のまま)。上の「升目へ一度に広げる」と「段ごとの配り手」が 2 つになる点だけ、段 2 で ADR-0500 に追記するか司令塔が決める。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29(本体 16 回目・設計): チケットのファイルを作った(前は T-1161 の「分けたもの」の行だけ)。形は ADR-0500。
- 2026-10-29(wt4): 段 1 の CPU。葉の鍵の持ち越し(CascadeLeafCarry・FoldCarryToLeaves・SpreadLeafCarry・StepCascadeLightOnLeaves・RcCarrySpreadShare)と
  受けのあった枠(CascadeTouchedSlots = T-1228 段 1)。cascade_tree_grid に 5・6(0.5 m の木で 6 刻みビット一致・粗い木と混ざった木で帳簿)。GPU の頁の表は段 1 の残り。
  ブランチ t-1227-rc-carry-leaf。
- 2026-10-29(wt4 2 本目): t-1227-rc-carry-leaf を渡す形に(release 警告 0・cascade_light・cascade_light_swing・cascade_tree_grid・probe_radiation・dxil_alloca_check)。
  GPU の頁の表と印(段 1 の残り)は時間切れで未着手。案を「段 1 の残り」の引き継ぎに書いた。
- 2026-10-29(wt4 3 本目): 段 1 の残りの GPU のうち頁の印(AddCarry が立てる・RcCarryIn と RadiationHash は印の頁だけ・RcLevel0Prepare が 0 に)。
  試験は上。T-1228 段 1 の RadiationGrid を印で飛ばす分は未着手(T-1228 と下の「T-1228 段 1 の残り」に読んだ所の要点)。ブランチ t-1227-gpu-carry。
