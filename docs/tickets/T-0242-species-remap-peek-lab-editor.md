# T-0242 覗き窓・実験室とエディタのホットリロードで物質を足す・消す

- Status: Review(一部完了。作業役・wt2・ブランチ t-0242。司令塔のマージ待ち。残りは T-0254)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(作業役 2 時間以内)
- マイルストーン: M2(docs/plan/ROADMAP.md。D-438)
- 設計: docs/design/13-scripting-game-model.md §2.5 / 決定: D-438・ADR-0065(追記)・ADR-0047・ADR-0050・ADR-0054・ADR-0055

## 目的(1〜2 行)
T-0223 の付け替え(世界 = ProbeSim・ProbeReference)に続けて、覗き窓と実験室も物質の一覧が変わる表に追従させ、
エディタのホットリロードの方針を Reject から Remap にして、動いている世界に物質を足す・消す差し替えを当てる(記録と再生・窓の確認つき)。

## 完了条件(チェックできる形で)
- [x] 覗き窓: 物質の一覧が変わる表でも、付け替えた世界の写しから影の鎖を作り直し・見る物質の ID も新しい表(作りは T-0194 のまま。
      gpu_probe_peek_test に物質を足す・消す差し替えの走りを足し、毎フレーム CPU とビット一致)
- [x] 実験室: 印の刻みに GPU(shaders/sim/lab_box.hlsl の RemapLabSpecies)と CPU(sim::RemapLabBox)が箱の全部のセルを
      同じ RxRemapCell で付け替える。まだ刻んでいない置く操作の材料も新しい ID(RemapLabCommand。消えた物質の材料は落とす)。
      AddTable は物質の一覧の違う表も足せる(Replay が表から表へ付け替えられるかを流す前に確かめる)。最新の表で流し直すときは
      古い表で置いた材料を名前で付け替える。パネルの材料の行も名前で付け替える
- [x] gpu_lab_box_test: 燃えている箱を水素を足しセルロースを消した表に替える → 毎刻みビット一致・元素は報告した端数のほか保たれる・
      別の実験室で記録の表を足して再生・最新の表で流し直し
- [ ] エディタのホットリロードを既定で Remap に → **T-0254**(今は `--species-remap` を付けた時だけ Remap。frame/table_hot_reload の
      policy)。差し替えと同じフレームに前の表で作った筆のコマンド(置く)の物質 ID を新しい表へ付け替えるのは入れた
      (frame_loop の RemapLivePlaceCommands・sim::RemapPlaceCommand。物質の一覧が同じなら何もしない)
- [x] 記録と再生: reaction_replay_table_test にオゾンを足す → 筆で置く → 消す(酸素に分かれる)差し替え(表の中身だけから再生して一致)
- [ ] 窓の確認 → **T-0254**: `--auto-reload --species-remap` は速度の書き換えの後、species.luau にオゾンを足す → 当たる → 消す → 当たる、
      までは通る(段の作りは入れた)が、オゾンを足した(物質の数 7 → 8)刻みから窓の世界の GPU で reaction.hlsli:179(RxMulS64Checked。
      RxChemicalEnergy の 物質量 × h0 の桁あふれ)の assert が毎フレーム数百件出て、終わりに DEVICE_HUNG(Sim の DispatchGraph)。
      ctest にはまだ足していない(window_hot_reload_* は今までどおり --species-remap 無し)
- [ ] 通常のゲームの起動でもファイルを見る → T-0243(元から別)

## メモ・参考
- 実験室の箱は 1 レベル(端数の枠・帳簿は使わない)なので、セルだけを付け替える。帳簿の列(1 + 物質の数)は箱を作った表のまま
  (使わないので害はない。帳簿を使う箱を作る時は物質の数の上限で取る)。
- 再生の後に最新の表へ戻す印は、付け替えられる時だけ置く(単体の無い元素の物質を消す向きなら記録の表のまま。ログに出す)。
- 試験の表(data/packages/combustion_test と C++ の表)は変えていない。C++ の試験用に `sim::MakeSpeciesChangedTestTable`
  (水素を足しセルロースを消す。gpu_probe_sim_test の表と同じ中身)を足した。Luau の試験はテストの中で写しの species.luau に
  オゾン(O3)を足す(値は文献値を丸めた試験の値。未確認)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-10(作業役 wt2・約 1 時間 50 分): 実験室の付け替え(lab_box.hlsl の RemapLabSpecies・RemapLabBox・RemapLabCommand・LabSession)・
  覗き窓の試験・Luau の記録と再生の試験(オゾン)・筆のコマンドの付け替え・`--species-remap`(Remap の方針と --auto-reload の物質の段)を入れた。
  窓で物質の数が変わる差し替えを当てると GPU の assert が出る(覗き窓・世界の GPU 試験では出ない)ので、既定の切り替えと窓の ctest は T-0254 に分けた。
  テスト(debug): gpu_probe_peek(7 → 8 物質の表で)・reaction_replay_table・reaction_hot_reload・window_lab・float_check_* が通過。
  gpu_lab_box は帳簿の大きさを直した後に流し直す(下の引き継ぎメモ)。

## 引き継ぎメモ(HANDOFF に載せる状態)
- 実験室: `LabSession` は物質の一覧の違う表に ChangeTable・AddTable できる(印の刻みに `GpuLabBox::RecordSpeciesRemap` と `sim::RemapLabBox`。
  付け替えの報告は `LastRemapReport`)。箱の帳簿の列は作った時の表のまま(`m_ledgerColumns`。物質の数の違う表で Reset しても GPU の箱の大きさに合わせる)。
  再生の後に最新の表へ戻す印は付け替えられる時だけ。最新の表で流し直す時は古い表で置いた材料を名前で付け替える(消えた物質の材料は落ちる)。
- 覗き窓は変更なし(T-0194 の作り直しで物質の数が変わる表にも追従する。gpu_probe_peek_test に 7 → 8 物質の表の走りを足した)。
- `sim::MakeSpeciesChangedTestTable()`(水素とオゾンを足しセルロースを消す。7 → 8 物質)。gpu_probe_sim_test はまだ自分の表(7 → 7)。
- `--species-remap`: エディタのホットリロードを Remap にする(T-0254 で窓の assert を直したら既定にして、--auto-reload の物質の段を ctest に)。
- テスト: `-Filter "^(gpu_lab_box|gpu_probe_peek|reaction_replay_table|reaction_hot_reload|window_lab|window_hot_reload.*)$"`。

## 判断待ち
- T-0223 の判断待ち 1〜4(熱を保つ・端数・単体の無い元素・成分の上限)は仮のまま(このチケットで変えていない)。

## 分けたもの(司令塔が ROADMAP に足すか決める)
- **T-0254 窓の世界で物質の数が変わる差し替えの GPU の assert を直し、エディタのホットリロードを既定で Remap に**:
  再現 `bicameral --editor --auto-reload --species-remap --auto-ignite --packages <写し> --peek 28,32,32 --peek-depth 9 --frames 3000 --record x`。
  オゾンを足した刻み(物質 7 → 8)から reaction.hlsli:179 の assert が毎フレーム 64〜192 件、物質を消して 7 に戻ると少しして止まり、
  終わりに元の中身に戻した後 DEVICE_HUNG(Sim の DispatchGraph)。gpu_probe_peek_test(世界 + 覗き窓、7 → 8)と gpu_probe_sim_test では出ない
  ので、窓だけにあるもの(物理・保存点・描画の抽出・GPU のコマンドの検査の物質の数など)を先に疑う。T-0239 の不安定さ(同じ窓で
  fixed.hlsli:156・:68、physics_math.hlsli:45 の assert が main でも出る)と同じ根かもしれない。直したら table_hot_reload の既定を Remap にし、
  window_hot_reload_record に --species-remap を付け、play の正規表現を差し替え 3 回にする。
