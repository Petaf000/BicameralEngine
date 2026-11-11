# T-1322 層を使う世界で、反応で生まれた粒・層を渡さずに置いた粒を層にする(Q175 (B) の残り)

- Status: Done(ブランチ t-1322-grain-layers-born。CPU の規則 + GPU はそれまで断る。GPU の同じ規則は T-1323 に分けた = 下の「GPU」。マージは本体)
- 種類: 工学(土台の共通の規則の変更。司令塔が docs/design/21 §3 と照らして受け取った形 = 2026-11-12 司令塔の照らし)
- PC: 必須
- 見積もり: チャット 1 回分(CPU)+ 1 回分(GPU。T-1323)
- マイルストーン: 試作の列 2(docs/plan/ROADMAP.md)
- 設計: ADR-0489 の 3(Accepted 2026-11-12・(B))・docs/design/20 §10.5 / 決定: D-687(Q175)/ 前: T-1338 の「土台に足りないもの」1・2

## 目的(1〜2 行)
ユーザーの答え Q175 (B)「層を使う世界では置いた粒は全部を層として読む。舞う粒は崩れ・吹き上げの出来事からだけ」に、組み立て(world_cells)の外で
粒が現れる所も合わせる: ① 反応で生まれた粒(灰・析出など)は層に足す ② 層を渡さない置き口(PlaceRootBlock・実験室の命令)でも粒は全部が層。

## 完了条件(チェックできる形で)
- [x] ① CPU: 刻みの反応が書く所で、粒の物質の増えた分を層に足す(層 = 前の層 + 増えた量。減った分は舞う側から = 層 = min(前の層, 量)のまま)
- [x] ② CPU: 層を使う世界の `PlaceRootBlock(nest, …, cells)`(層を渡さない置き口)は粒を全部層に・実験室の命令(NestCells::Apply)も ① と同じ道
- [x] 層を使わない世界はビット単位で今と同じ(道は `cellLayers.enabled` の時だけ・粒の増えないセルは前の道で書く)
- [x] CPU の試験(multires_layers の 2 つ・層の試験の組)・release 警告 0・archmap --check
- [x] GPU はそれまで断る(層を使う世界で粒を生む表は写さない。gpu_multires_layers の 9)
- [ ] ① ② の GPU(T-1323)と CPU とのビット一致(gpu_multires_layers に層を使う世界の燃える場面)= 分けた

## メモ・参考
- **作り手から見た違い**: 層を使う世界で、燃えかす・析出した粒は生まれた所に積もった側から始まる(細かくしても空中に広がらない・その頁は眠れる)。
  舞わせたい時は、崩れ・吹き上げの法則(層を動かす法則 = StoreNestCellWithLayers)が層から残りへ移す。根を層なしで置いても砂は積もる。
- **規則(1 つの関数 `nest_detail::GrowGrainLayers`。multires_cell_store.cpp)**: 書く前のセルと書いた後のセルを物質で結び、粒の物質(`cellLayers.grain`)で
  量が増えたら 層 = 前の層 + 増えた量(前の層 ≤ 前の量なので 層 ≤ 量)、ほかは 層 = min(前の層, 量)(今の付け替えと同じ)。
  増えた粒が 1 つも無ければ何も返さず、呼ぶ側は前の書き方(StoreCellAt・StoreWidePageCell)で書く = 粒の増えないセル・層を使わない世界は今と同じ道。
  書く所は `nest_detail::StoreGrownGrainCell`(反応の 2 つの道〔インライン・溢れ〕と実験室の命令)。
- **当てない所(わざと)**: 細かくする・粗くする・GPU から読んだ列・観察の枠(影)の引き戻し・伝導(量が変わらない)は今の付け替えのまま
  (どれも「生まれた」粒ではない。粗くする時の和 ÷ 8 の切り捨ての余り〔0〜7 µmol〕は層から残りへ移る = 下の印 1)。
- **②の約束**: 層を使う世界か(`cellLayers.enabled`)は置く時に見る。`EnableCellLayers`(`ApplyCellLayersChoice`)は根を置く前に呼ぶ(ADR-0489 の「層を書く前・刻む前に呼ぶ」)。
  先に置いてから層を使う世界にした根の粒は層 0 = 舞う粒(試験の場面 tree_place の `FloatingOnly` はこの順で舞う粒の場面を作っている。変えていない)。
- 層を渡す置き口(`PlaceRootBlock(…, layers)`・`PlaceLayoutRootBlock`)は今のまま(書いた層を使う。パッケージは T-1338 で Settled)。
- 費用: 層を使う世界の反応したセルだけ、書く前のセルと層を 1 回読む(上限の無い形)。使わない世界は分岐 1 つ。

## GPU(T-1323 に分けた。この作業役の 2 時間に収まらない)
今の GPU は「刻みの段の後に層を合わせる段を 1 回(層 = min(前の層, 今の量))」で、前の量を持たず、層の頁の無いセルの頁には層を作れない
(「GPU だけで層が生まれる道は無い」)。① を GPU で同じにするには:
1. 層を使う世界(MR_LAYER_FLAG_GRAIN)だけ、刻みの段の前に頁のセルを写す(反応の前のセル。CopyBufferRegion。頁 × 64 × 112 B)。影の後の合わせる段(gpu_multires.cpp の 1 つ目)は今のまま min。
2. 層を合わせる段を、層を使う世界では全部のセルの頁に投げ、粒の物質で量が増えたら層に足す。層の頁の無い頁は層の頁を取る(グループで 1 つ・空きのスタックから。
   尽きたら CountLayerShortage と同じ計器で断る)・組の空きに物質を足す。
3. 実験室の命令(lab_box.hlsl の LabCells)も同じ規則。
4. 試験: gpu_multires_layers に「層を使う世界・生成物を粒の印にした燃える場面」(この CPU の試験と同じ形)を CPU と GPU HW/WARP でビット一致。
- **それまでの GPU は断る**(D-428。結果を変えて回すより写さない): 層を使う世界(`cellLayers.enabled`)で、粒の印のある物質を表の規則が生む時は
  `GpuMultires::RecordUpload` が写さない(`ProducesGrain`。`m_producedSpecies` = 規則の生成物の印を Create で作る)。gpu_multires_layers の 9 で確かめる
  (生成物を粒の印にした世界は断る・生成物の粒の印を外した世界は写す)。real_reactions では熱分解の炭(carbon。書かなければ粒)が生まれるので、
  セルロースの燃える層を使う世界は GPU に写らない。今エンジン本体が層を使う世界を作る所は無く(ApplyCellLayersChoice を呼ぶのは試験だけ)、
  GPU の層を使う世界の試験(gpu_tree_refine_placement の層の場面。表はセルロースの規則を外した試験の表)は断られない。
- 実験室の命令の GPU(lab_box.hlsl)も同じ規則が要る(実験室の箱は層を使う世界にならない = MakeLabBoxNest は EnableCellLayers を呼ばない)。
- **GPU の難所(T-1323 の作業役へ)**: 刻みの段の中で一様な枠を頁に広げる(RecordStepWait の RecordExpandPass)ので、「反応の前のセル」は
  刻みの段の前の写しだけでは足りない(広げた頁は広げた後の値が前)。広げる段の後にもう一度写すか、広げる段が写しにも書く。

## 土台に足りないもの(印。司令塔へ)
1. 粗くする時の層の切り捨ての余り(成分ごとに 0〜7 µmol)は層から残りへ移る(ADR-0489 の 5)= 「出来事」でない舞う粒。層を使う世界では
   `min(和 ÷ 8 の切り上げ, 粗いセルの量)` にすれば舞う粒は生まれない(CPU と GPU の粗くする段の両方・層の試験のハッシュが変わる)。今は変えていない。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-11-12 wt2(2 本目): t-1338-grain-layers-default の上の t-1322-grain-layers-born で CPU の ① ② を入れた(GrowGrainLayers・StoreGrownGrainCell・
  SettledGrainLayers・PlaceRootBlock の層なしの置き口・実験室の命令)。試験 multires_layers に TestReactionBornGrainSettles・TestPlaceSettledGrain。
  GPU は T-1323 に分けた。
- 2026-11-12 wt2(2 本目): 合格(release 警告 0): multires_layers・multires・multires_sparse・multires_wide・multires_quiet・world_layout・environment_package・
  phase_layer_world・phase_layer_tree・placement_refine・tree_refine_placement・dxil_alloca_check / GPU(RTX 3070 Ti)gpu_multires_layers〔HW・WARP〕・
  gpu_tree_refine_placement〔HW・WARP〕・gpu_lab_box。archmap --check OK。記録したハッシュの取り直しは要らなかった(層を使う世界で反応を刻む試験は無かった)。
  lab_box(CPU)は ctest で Disabled のまま。rebase main d9eed9db。
