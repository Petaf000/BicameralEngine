# T-1323 GPU: 層を使う世界で反応で生まれた粒を層に足す(T-1322 の GPU)

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: 試作の列 2(docs/plan/ROADMAP.md)
- 設計: ADR-0489 の 3 (B)・docs/design/20 §10.5 / 決定: D-687(Q175)/ 前: T-1322(CPU リファレンス `nest_detail::GrowGrainLayers`)

## 目的(1〜2 行)
T-1322 の CPU の規則(層を使う世界で、反応・実験室の命令で増えた粒の物質の量を層に足す)を GPU に入れ、CPU とビット一致させる。
それまで GPU は、層を使う世界で粒を生む表を写さずに断る(`GpuMultires::ProducesGrain`)。入れたら断りを外す。

## 完了条件(チェックできる形で)
- [ ] 刻みの段(全部を刻む・活性だけ・伝導つき・Work Graph の道)の後の層を合わせる段が、層を使う世界(MR_LAYER_FLAG_GRAIN)では
      反応の前のセルと比べて粒の物質の増えた分を層に足す(影の引き戻しの後の合わせる段は今の min のまま)
- [ ] 層の頁の無いセルの頁で粒が生まれたら層の頁を取る(尽きたら計器で数える)・組の空きに物質を足す
- [ ] 実験室の命令(lab_box.hlsl の LabCells)も同じ規則
- [ ] `ProducesGrain` の断りを外し、gpu_multires_layers の 9 を「層を使う世界の燃える場面(生成物を粒の印に)で CPU と GPU HW/WARP がビット一致」に
- [ ] 層を使わない世界は今とビット単位で同じ・release 警告 0・archmap --check

## メモ・参考(T-1322 の作業役の見立て)
- 今の層を合わせる段(shaders/sim/multires_layers.hlsl)は層の頁だけに投げ、層 = min(前の層, 今の量)。前の量を持たない。
- 案: 層を使う世界だけ、刻みの段の前に頁のセルを写し(反応の前。頁 × 64 × 112 B)、合わせる段を全部のセルの頁に投げて比べる。
  **刻みの段の中で一様な枠を頁に広げる**(RecordStepWait の RecordExpandPass)ので、広げた頁の「前」は広げた後の値 = 広げる段の後にも写すか、
  広げる段が写しにも書く。溢れを使う世界と層は GPU がまだ断る(T-1240)ので、インラインの成分だけでよい。
- ルート署名は多重解像度で 64/64 語(外の口 u4・u5。合わせる段は u4 に層の置き場を結ぶ。u5 に前のセルを結べるか確かめる)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-11-12 wt2: T-1322 から分けた(チケットだけ)。
- 2026-11-12 wt2(3 本目): 入れ物の再起動で止まった。コードは未変更(読んだだけ)。ブランチ t-1323-grain-layers-born-gpu(t-1322 c707f3a7 の上)にこのメモだけコミット。
  **引き継ぎメモ(次の一手)**: 読んだ所 = CPU の規則 `nest_detail::GrowGrainLayers`(multires_cell_store.cpp 370 行)・合わせる段 shaders/sim/multires_layers.hlsl
  (層の頁だけに投げる・MrLayerAfterWrite で min)・道具 sim/multires_layer_access.hlsli(層の頁を取るのは木の段の累積和 + 空きのスタック。
  1 グループで取る形は無い = 合わせる段で取るなら InterlockedAdd(-1) で空きの数を下げる形を足す。取る順が番号に効くだけで結果には効かない)。
  gpu_multires.cpp: 合わせる段を呼ぶ所 1362(影の後・min のまま)・1429 / 1448(刻みの段の後 = ここを「前のセルと比べる」に)・RecordLayerReconcile 1469・
  RecordExpandPass 1508 / 1635 / 1720(刻みの段の中で広げる = 写しは広げた後に取る)・断り ProducesGrain 997 / 1033(外す)・m_producedSpecies 350。
  順: ① 層を使う世界だけ「反応の前のセル」の写しのバッファ(頁 × 64 × 112 B)を広げる段の後・刻みの前に CopyBufferRegion
  ② 合わせる段の 2 つ目の入口(全部のセルの頁に投げ、粒の物質〔粒の印の表が要る。u5 か定数で〕で増えた分を層へ。層の頁が無ければ取る・尽きたら CountLayerShortage)
  ③ lab_box.hlsl の LabCells ④ gpu_multires_layers の 9 を燃える場面のビット一致に(HW/WARP)。u5 に前のセルを結べるか(64/64 語)を最初に確かめる。
