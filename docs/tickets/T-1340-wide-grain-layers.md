# T-1340 GPU: 溢れを使う世界で反応が粒を生む表の層(T-1323 の溢れの世界)

- Status: Todo(2026-11-12 wt2: 設計の詰めと引き継ぎメモだけ。コードはまだ)
- 種類: 工学
- PC: 必須(GPU)
- 見積もり: チャット 1〜2 回分
- マイルストーン: 土台の変更要求の束(T-1240 の残り)
- 設計: ADR-0489 の 3 (B)・docs/design/20 §10.5 / 決定: D-687 / 前: T-1323(溢れを使わない世界)・T-1240 の 7 の 5(溢れの成分の層)

## 目的(1〜2 行)
層を使う世界で反応が粒を生む表(T-1322 の CPU の規則 `nest_detail::GrowGrainLayers`)を、溢れを使う世界(wideCells)の GPU でも CPU とビット一致させる。
今は GpuMultires::RecordUpload が「溢れを使う世界で粒を生む表」を断る(gpu_multires.cpp の T-1340 の Log)。

## 完了条件(チェックできる形で)
- [ ] 刻む前のセルの写しが溢れの成分(頁の今の面・一様の値の溢れ)まで持ち、刻みの後の ReconcileGrownLayers の溢れの変種が
      溢れの成分の粒の増えも層に足す(物質で結ぶ。T-1240 の LayerTail の道具)
- [ ] RecordUpload の断りを外す・gpu_multires_layers の粒を生む場面(RunGrainBornScenes)を溢れの世界でも〔HW・WARP〕
- [ ] 溢れを使わない世界・溢れを使う世界で粒を生まない表は今とビット単位で同じ・release 警告 0・dxil_alloca_check・archmap --check

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

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-11-12 wt2: T-1341 の後に設計の詰めだけ書いた(上)。コードは未変更。次の一手: ① u5 を溢れの変種で raw にする形を dxil_alloca_check と一緒に試す
  ② SnapshotGrainCells の溢れの変種 ③ ReconcileGrownLayers の溢れの変種 ④ 断りを外して RunGrainBornScenes を溢れの世界で。
