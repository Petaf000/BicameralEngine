# T-1340 GPU: 溢れを使う世界で反応が粒を生む表の層(T-1323 の溢れの世界)

- Status: Done(2026-11-13 wt2 t-1340-wide-grain-layers-2。9 種目の層の扱いは仮【エンジン】・下の判断待ち)
- 種類: 工学
- PC: 必須(GPU)
- 見積もり: チャット 1〜2 回分
- マイルストーン: 土台の変更要求の束(T-1240 の残り)
- 設計: ADR-0489 の 3 (B)・ADR-0533(写し方)・docs/design/20 §10.5 / 決定: D-687 / 前: T-1323(溢れを使わない世界)・T-1240 の 7 の 5(溢れの成分の層)

## 目的(1〜2 行)
層を使う世界で反応が粒を生む表(T-1322 の CPU の規則 `nest_detail::GrowGrainLayers`)を、溢れを使う世界(wideCells)の GPU でも CPU とビット一致させる。
今は GpuMultires::RecordUpload が「溢れを使う世界で粒を生む表」を断る(gpu_multires.cpp の T-1340 の Log)。

## 完了条件(チェックできる形で)
- [x] 刻む前のセルの写しが溢れの成分(頁の今の面・一様の値の溢れ)まで持ち、刻みの後の ReconcileGrownLayers の溢れの変種が
      溢れの成分の粒の増えも層に足す(物質で結ぶ。T-1240 の LayerTail の道具)
- [x] RecordUpload の断りを外す・gpu_multires_layers の粒を生む場面を溢れの世界でも〔HW・WARP〕(RunWideGrainBornScenes。下)
- [x] 溢れを使わない世界・溢れを使う世界で粒を生まない表は今とビット単位で同じ・release 警告 0・dxil_alloca_check・archmap --check

## 設計の詰め(2026-11-12 wt2。T-1341 の後の残り時間。コードはまだ)
- **退ける案: 「刻む前の溢れ = 頁のもう片方の面」を読む(写さない)。** 刻みの段は 1 − 今の面に書いて面を入れ替えるので、刻みの後の
  もう片方の面は刻む前の溢れに見える。しかし同じ刻みの中で同じ頁をもう一度書く段がある(WideDeferredPass は伝導の段が回したセルを
  消してから ② で書き直す・WideRetry・WideBigPass。multires_wide_access / multires_wide_step.hlsli の要求の語のビット 9・11・12)。
  2 回目の書き込みは入れ替えた後の 1 − 今の面 = 刻む前の面に書くので、刻む前の溢れが消える。
- **芯(おすすめ): 刻む前の写しに、溢れの領域(頁・端数の枠・一様の値の見出しと塊の置き場)を同じ並びのまま丸ごと写す。**
  溢れの成分の在り処は「見出しの面・塊の表 → 塊の番号 → MrWideChunkWord」でたどり、塊の番号は置き場の中の相対の番号なので、
  領域を同じ並びで写せば、読む語の番地に写しの始まりを足すだけで刻む前の溢れが読める(WideRange・WideWord・LayerTailAmountOf と同じ道)。
  - 置き場: 今の u5(g_grainBefore。RxCell の構造体の配列 = u1 と同じ並び)の後ろに溢れの領域の語を足す。溢れの変種では u5 を
    RWByteAddressBuffer にして、セルは Load<RxCell>(番地)・溢れは Load(番地)で読む(ルート署名は 64/64 語なので新しい口は足せない。
    u5 はルートの UAV なので raw にできる。SM 6.2 の templated Load。**未確認: dxc が Load<RxCell> を alloca にしないか** → dxil_alloca_check で見る)。
  - 写す段: SnapshotGrainCells の溢れの変種が、セルに続けて溢れの領域(gpu_multires.cpp の OverflowBaseWord から MrWideAreaWords の語)を写す
    (または CopyBufferRegion で g_treeWords の区間を u5 の後ろへ。**未確認: 同じ刻みのリストで UAV の後の写しの障壁**)。費用は置き場の大きさ
    (既定 170 塊 ≒ 0.7 MiB / 刻み)。要る世界(層を使う・粒を生む表・溢れを使う)だけ。
  - 読む道具: LayerTail に「どこから読むか」(g_treeWords か写し)を足すより、刻む前を読む関数を別に 1 つ(GrainBeforeTailAmountOf: 写しの番地で
    WideRange と同じ計算)。今の面は写しの見出しの面(刻む前の面)を読む。
- 比べる段(ReconcileGrownLayers の溢れの変種): GrainGrew・WriteGrownLayers を「インライン + 溢れ」の物質ごとの量で(前 = 写し、後 = 頁の今の面)。
  組 k は今のセルの k 番目の物質(インラインの 8 + 溢れ)なので、層の組の 8 を超える物質に粒が生まれた時の扱いを CPU の GrowGrainLayers で確かめる
  (T-1240 の 7 の 5: GPU の層の組は 1 セル 8 組で物質で結ぶ。CPU で層を持つ物質が 9 種以上になる場面は RecordUpload が断る・粗くするは断って数える。
  CPU(GrowGrainLayers・StoreWidePageCellWithLayers)は RxWideCell で層の数に上限が無い(2026-11-12 読んだ)。
  **粒が生まれて 9 種目の層ができる時の GPU の扱いが決まっていない** → 【エンジン】 層の頁が尽きた時と同じく数えて舞う粒のまま、が近い(仮のおすすめ))。
- 広げた頁(刻みの段の中で一様から頁に広げる): 写しの枠の所は一様の値(印 0)なので、溢れは写しの一様の値の溢れの領域(WideUniformArea(slot) のセル 0)を読む
  (T-1323 の GrainBeforeCell と同じ形)。

## 実装(2026-11-13 wt2。上の設計の詰めの「芯」のとおり)
- 新しいシェーダー multires_layers_grain_snapshot_wide・multires_layers_grain_reconcile_wide(multires_layers.hlsl を MR_WIDE_CELLS で)。
  u5 は溢れの変種だけ RWByteAddressBuffer(セルは Load<RxCell>/Store<RxCell>。dxc は alloca にしなかった)。写しの後ろに
  溢れの領域(g_overflowBase から MrWideAreaWords 語 = 頁の見出し・塊の置き場・空きのスタック・一様の値と端数の枠の見出し)を
  同じ並びで写す(写す段は枠の後ろのグループ。64 × 16 語 / グループ)。g_external2 = 写しの溢れの始まり(バイト)・g_external3 = 語の数
  (層の段は欄の区画を読まないので BindGrainSnapshot が上書きする)。費用は要る世界(層を使う・粒を生む表・溢れを使う)だけ。
- 刻む前の溢れは写しの中を WideWord と同じ道(見出しの面 → 塊の表 → 塊)でたどる(BeforeWideWord・BeforeTailAmountOf。見出しと
  置き場の番地は塊の数だけで決まるので WideAreaHeader をそのまま使う)。一様だった枠は写しの一様の値の溢れ(観察の枠は持たない)。
- 比べる段: 粒が増えたか・生まれた粒を足した層をインライン + 溢れの成分ごとに(CPU の GrowGrainLayers)。0 でない層を成分の順に
  8 組へ前から詰める(書き直す前の 8 組を先に読む)。粒の増えないセルは溢れの成分の量まで min(ReconcileLayers の溢れの変種と同じ)。
- 局所の配列: 一様だった枠のセルの「覆われているか」を MR_CHILD_OF(溢れの変種は children を動的な添字で読む)で見ると 1 つ出たので、
  この段だけ 8 つから選ぶ GrainUniformCell にした(溢れを使わない変種も同じ答え)。
- 試験(gpu_multires_layers の 10): 溢れの像の表の p02(インライン)と q09(q01 → q09 で 9 種目 = 溢れの成分に生まれる)を粒にして、
  12 種のセルの根(層なしの頁・層のある頁・層のある一様の枠 = 頁に広げる)で溢れの世界の鎖(2 段・12 刻み)を全部を刻む・伝導つきで
  毎刻み比べる。置き場は WideChainChunks(足りる大きさ。T-1359 は直さない)。溢れの位置の粒の層が 0 でないことも確かめる。

## 判断待ち(仮で進めたもの)
- **【エンジン】粒が生まれて 0 でない層を持つ物質が 1 セルで 9 種以上になる時の GPU(溢れを使う世界だけ)**: 仮(ユーザー未確認)=
  断って数える(CountLayerShortage = ReadLayerShortage に足す)・そのセルは生まれた粒を層に足さず舞う粒のまま(min で合わせる)。
  D-689(粗くする時に 9 種以上なら断って数える)・層の頁が尽きた時と同じ形。作り手から見た違い:
  - 仮の案(今): GPU の層の組は 1 セル 8 つのまま(層の置き場の大きさは変わらない)。9 種目の粒を作る世界では GPU の結果が
    CPU リファレンスと変わる(層の足りない数の器で分かるだけ。D-468 に合わない点は T-1359 と同じ)。
  - 代案: 層の組を溢れと同じく可変にする(層の置き場に溢れの塊を持つ)。CPU と変わらないが、層の置き場と層の段の作り直し。
  取り消し: WriteGrownLayersAt が偽を返した所(ReconcileGrownLayers の CountLayerShortage)だけ。GPU の試験はこの道を通していない
  (9 種が層を持つセルは今の像に写せないので、場面を作るには 8 種が層を持つセルに 9 種目の粒を生ませる表が要る)。
- ゲームの中身へ: なし。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-11-12 wt2: T-1341 の後に設計の詰めだけ書いた(上)。コードは未変更。次の一手: ① u5 を溢れの変種で raw にする形を dxil_alloca_check と一緒に試す
  ② SnapshotGrainCells の溢れの変種 ③ ReconcileGrownLayers の溢れの変種 ④ 断りを外して RunGrainBornScenes を溢れの世界で。
- 2026-11-13 wt2: 実装(上)。release 警告 0・dxil_alloca_check(新しい 2 つは 0)・float_check 系・archmap --check OK。
  gpu_multires_layers の結果は下の行。既定の答え・ハッシュは変えていない(溢れを使わない変種は同じ式。溢れの世界で粒を生まない表は段を通らない)。
  合格(release・RTX 3070 Ti): gpu_multires_layers〔HW 55 s。10 の 4 場面を含む〕・multires 系 17 本(CPU)。
  gpu_multires_layers_warp〔823 s。上限 900 s に近い: T-1341 の時は 288 s。ほかの作業ツリーとの重なりは未確認。新しい 10 は 4 場面 × 12 刻み〕・
  gpu_multires_wide_overflow〔50 s〕・gpu_multires_wide_uniform も合格。WARP の時間が重なりでないなら、10 を WARP で 1 場面に減らすか上限を上げる(司令塔)。
