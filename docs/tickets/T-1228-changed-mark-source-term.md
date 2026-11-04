# T-1228 変わったブロックの印を費用の読み手へ・吸った分を当てる段の源の項に(ADR-0469 の 4-4・4-1。ADR-0500 改訂)

- Status: Doing(段 1 の RadiationGrid は 2026-11-05 wt4 で済み・RcSources の一覧が残り。2026-10-27 wt6 が T-1161 から分けた・2026-10-29 本体 16 回目〔設計〕が ADR-0500 で形を決めた)
- 種類: 工学
- PC: 必須(GPU)
- 見積もり: チャット 2 回分(段 1 / 段 2)
- マイルストーン: ROADMAP「土台の変更要求の束」の 4
- 設計: docs/design/20 §2.4・§10.7 / 決定: **ADR-0500**(決定 2・3・4)・ADR-0469 の 4-1・4-4 / 親: T-1161

## 目的(1〜2 行)
変わったブロックの印(= 今の busyTick。仮の世界は PROBE_SCHEDULE_CHANGED_WORD)を、費用だけに効く読み手(放射の写し・放つセルの一覧・光の掃き直し・物の囲むセルの変わった検出)が使う。
まっすぐ運ぶ家族の吸った − 放ったを「この刻みだけ」の源の項の欄にし、当てる段が 1 か所でセルのエネルギーへ足す(受けのあった葉も変わった印に)。

## 形(ADR-0500 の決定 4)
- 印は新しく作らない。セルの真実を書く全部の道は 1 つの関数で印を書く(T-1252)。
- 費用だけの読み手は「最後に読んだ刻み」より後に変わったブロックだけ読み直す。多めに拾ってよい・落とさない。読み込んだ直後・起きた直後は全部。
- 源の項: 集めて書く段が欄へ(粗い葉は升目の 2^L 角の和 = T-1117 の (1))・当てる段が mJ へ丸めて足す(丸めの規則と端数の行き先は今の RadiationApply のまま 1 か所へ移す)。

## 完了条件(チェックできる形で)
- [ ] **段 1(今すぐ・仮の世界・土台に触らない)**: RadiationGrid が変わったブロックだけ両側の作業へ写す・RcSources の放つセルの一覧を印で直す。
      **確かめ**: 印を全部「変わった」にした時と刻みごとのハッシュがビット一致(gpu_radiation_cost に場面)。費用を perf.md に(Grid 83 + Sources 固定 72 µs がどれだけ減るか)
  - [x] RadiationGrid(2026-11-05 wt4・ブランチ t-1228-skip-grid): 前の刻みの写しの後で変わった印のブロックだけ写す。CPU の約束 `sim::ProbeRadiationBlockStale`・
        CPU の確かめ `ProbeReference::CheckCascadeGridMarks`(probe_radiation の 8)・GPU は `ProbeSimOptions::radiationCopyAll` と比べる(gpu_radiation_cost)
  - [ ] RcSources の放つセルの一覧(下の「段 1 の残り」)
- [x] **受けのあった葉を印の相手として数える(2026-10-29 wt4・T-1227 と同じブランチ)**: `sim::CascadeTouchedSlots`(吸った・放ったが 0 でない升目の葉の枠。昇順・落とさない)。
      cascade_tree_grid の 6 で升目から数え直した枠と一致(0.5 m・1 m の根・混ざった木)。印(busyTick)を書くのは T-1252 の 1 つの関数なので、ここは数えるだけ
- [ ] **段 2(T-1252 段 2 の後・T-1155 の残りと一緒)**: 多重解像度の世界で源の項を当てる段へ(4-1)。受けのあった葉が起きる・CPU と GPU のビット一致・保存の検査
- [ ] release のビルド(警告 0)・archmap --check

## メモ・参考
- 物の囲むセルの変わった検出(wt5 OhNestGather)・R-T2・T-1147 案 3 も同じ読み手の約束で載せる(各チケットで)。
- T-1229 の「負の側の Grid +23 µs」は段 1 に乗る。

## 段 1 の引き継ぎ(2026-10-30 wt4 4 本目・コード未着手。読んで決めた形)
- 印の値と比べ方: 刻み t の Grid より前の変化は印 ≤ t(t − 1 の伝導 = ProbeChangeMark(t − 1) = t・t の適用〔コマンド・気体・液体・物理の熱〕= ProbeChangeMark(t) − 1 = t)。
  Grid の後の RadiationApply も印 t。だから作業に「前に写した刻み」L を持ち、**印 ≥ L のブロックだけ写す**・写した後に L = t
  (印 t のブロックを 2 回写すのは多めに拾うだけ)。L = 0 なら全部(印はいつも ≥ 0)。L は 64bit を 32bit の 2 語で持ち、上 → 下の順に比べる(引き算しない)。
  置き場は作業の頭(RcGpuParams は今 196 B・RC_GPU_WORK_EMITTERS = 248 の手前に 2 語の空き)。
- 全部写す時(初めのフレーム・RecordRestore・RecordAdopt・RecordRadiationTableSwap)は GPU に書かず、CPU の印(例 m_radiationCopyAll)を立てて
  次の Grid の起動の単位の定数 argument で「全部」を渡す案が軽い(Grid は argument を使っていないか要確認。SetUnitConstants は適用の単位で m_speciesCount を入れている)。
- 注意 1 の解き方の案: 放つセルの印(RC_GPU_WORK_EMITTERS)は、写したブロックのセルだけでは立てきれない。ブロックごとの「放つ」1 語(側ごと 2 bit)を作業に持ち、
  写したブロックはその語を書き直し、RC_GPU_WORK_EMITTERS は全部のブロックの語の OR(波ごと)。置き場は放射の側の作業のバッファの後ろ(GpuCascadeLight の作業の並びに 1 区画。土台ではない)。
- 注意 3 の確かめ: 熱のキャッシュ(thermal)を書く所は全部ブロックの印も書く(probe_tick の置く・つつく・物理の熱・RefreshTable、気体・液体・放射の当てる段、伝導)。
  RemapSpecies(物質の付け替え)は印を書かないが、表の差し替えと組なので RecordRadiationTableSwap の「全部」で拾える(要確認: 付け替えだけで表の差し替えを通らない道が無いか)。
  眠っているブロックは 2 世代のセルが同じなので、写した世代と違う世代を読む刻みでも同じ値。
- RcSources の放つセルの一覧: 今は全部のセルを回し、冷たいセルでも前の刻みの放った・吸ったが 0 でなければ 0 に戻している。一覧にする時は
  「前に放ったセル」の 0 戻しを落とさない(冷えたセルが一覧から抜ける刻みに 0 に)。一覧は印で直す(写したブロックのセルだけ出し入れ)。
- 確かめ: ProbeSimOptions に「いつも全部写す」(比べる試験だけ切る)を足し、gpu_radiation_cost の SkipScenes と火の場面で刻みごとのハッシュが全部一致。
  保存点・写しの早送りの道は gpu_probe_rewind・gpu_world_copy(放射は既定で入る)。費用は perf.md(Grid 83・Sources 81 µs/側 がどれだけ減るか)。

## 段 1 の RadiationGrid の作り(2026-11-05 wt4 5 本目)
- **確かめた「RemapSpecies が印を書かない所」**: GPU の RemapSpecies(probe_tick.hlsl)を流すのは RecordSpeciesRemap だけで、呼ぶのは RecordTableSwap だけ
  (probe_sim.cpp。保存点へ戻す道は RecordTableBuffers と RecordRadiationTableSwap だけで付け替えない)。RecordTableSwap の刻みは必ず tableSwapped で、
  適用の単位の RefreshTable が全部のブロックに印 = 刻み t を付ける(Grid より前)。CPU も Advance の newTable で RemapSpecies → 全部の印を t に。
  だから付け替え・表の差し替え(光の値が変わる)は印だけで全部写す。別の「全部」の道は要らない(保存点へ戻す・写しの早送りは印そのものが戻るので要る)。
- セルの真実を書く道の照らし(全部が印を書く): 置く・つつく・物理の熱(probe_tick)・RefreshTable・気体(GasScatter)・液体(動かした後の段が changed の印で)・
  放射の当てる段・伝導(変わったブロック)。眠っているブロックは 2 世代が同じ(変わったブロックは次の刻みに必ず伝導が流れて次の世代へ写す)。
- 形: 写しの段は単位の引数 0 = 前の刻み(t − 1)の写しの後で変わったブロック(印 ≥ t − 1。32bit の上下で比べる)だけ・1 = 全部。
  CPU(ProbeSim::RecordRadiation)が「最後に写した刻み」を持ち、前の刻みでなければ全部(作った直後・RecordOverwriteState〔保存点へ戻す・写しの早送り〕で無くす)。
  GPU に新しい語は足していない(案の「作業の頭に L」は使わなかった。L は CPU が知っている)。
- 注意 1(放つセルの印)の解き方: ブロックごとの語ではなく、**側ごとの放つセルの数**を作業の RC_GPU_WORK_EMITTERS に持つ(写したセルが前の値 → 新しい値の
  増減を波の和で足す。作った時は作業の温度 0 = 放たない・数 0)。RcPlanTick は 0 でないかだけ読み、0 に戻さない(前は写しの段が毎刻み全部のセルで印を立てていた)。
  数は「作業の今の中身で放つセル」と常に同じなので、飛ばす刻みは前と同じ(gpu_radiation_cost で飛ばした刻みの数も全部写す時と比べる)。
- 1 側の形(0 K の空)は数を持たない(飛ばすのは参照の温度の形だけ。RadiationSkips)。

## 段 1 の残り: RcSources の放つセルの一覧(次の作業役へ)
- RcSources(cascade_light.hlsl)は全部のセルを回す。参照の温度の形では温度 0 でないセルはみな「hot」なので、放たないセルも面の回しに入り、
  放った・吸ったの欄に 0 を書く(冷たいセルは前の値が 0 でなければ 0 に)。一覧にするには (a) 放たないセルの欄の 0 戻し と (b) 一覧の出し入れ の 2 つが要る。
- (a) の案: 仮の世界の当てる段(RadiationApply)は全部のセルの放った・吸ったを読んでいるので、読んだ後に 0 でなければ 0 に戻す(この刻みだけの値。
  ADR-0500 の「源の項はこの刻みだけ」と同じ向き)。そうすれば RcSources は放つセルだけ回せばよい(冷たいセルの 0 戻しも要らない)。
  欄を読むのは当てる段だけか要確認(RcPlanTick は帳簿だけ・試験の読み戻しが欄を読むか: gpu_probe_sim・gpu_play_metric)。木の世界の RcSources は今のまま。
- (b) の案: 写しの段が側ごとの「放つ」1 bit をセルごとに作業へ持つ(32 セル = 1 語。写したセルだけ InterlockedOr / And。ビットは重ならないので順番によらない)。
  RcSources は語ごとに回して立っているビットのセルだけ処理(params に「一覧の語の位置」0 = 今のまま を足す。RcGpuParams の空き: 頭の 256 B の中)。
  放つかは EmitSidesOf(写しの段の今の判定。RcCellEmissionRelative が 0 でないのと同じ。面の向こうは見ない = 多めに拾うだけ)。
- 確かめ: gpu_radiation_cost の `radiationCopyAll` と同じく「一覧を使わない」の印で全部の場面のハッシュと飛ばした刻みが一致。費用は perf.md(Sources 81 µs/側)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29(本体 16 回目・設計): チケットのファイルを作った(前は T-1161 の「分けたもの」の行だけ)。形は ADR-0500。
- 2026-10-29(wt4): 段 1 のうち「受けのあった葉」だけ(CascadeTouchedSlots。T-1227 のブランチ t-1227-rc-carry-leaf)。RadiationGrid・RcSources を
  PROBE_SCHEDULE_CHANGED_WORD で飛ばす GPU の部分と gpu_radiation_cost の「全部変わった」場面はまだ(次の作業役)。段 2 に要るもの: 当てる段の 1 つの関数が
  受けのあった枠に busyTick を書く(T-1252)・GPU では集めて書く段の印のブロック(触ったブロックの冪等の OR)がそのまま受けのあった枠になる。
- 2026-10-29(wt4 3 本目): 段 1 の RadiationGrid・RcSources を印で飛ばす分は未着手(時間切れ)。読んだ所の要点と案・注意(放つセルの印・保存点・
  表の差し替え)は T-1227 の「T-1228 段 1 の残り」に書いた。T-1227 の頁の印(t-1227-gpu-carry)の上で始めてよい。
- 2026-10-30(wt4 4 本目): ブランチ t-1228-skip-grid(main 9f620e21 の上)。入れ物の再起動で区切り。コードは未着手。読んで決めた形を上の「段 1 の引き継ぎ」に書いた。
