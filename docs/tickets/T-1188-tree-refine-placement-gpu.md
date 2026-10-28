# T-1188 木を細かくする段のセルの中身の置かれ方を GPU で・既定 on(T-1176 の続き)

- Status: Doing(2026-10-29 wt2: HW の gpu_multires が DEVICE_HUNG。マージしない。下の「引き継ぎ」。隣のブロックの列・斜面の空気の速さの比べは T-1189 に分けた)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: 試作の列・土台の変更要求の束(docs/plan/ROADMAP.md)
- 設計: docs/design/20-transport-substrate.md §10.5・17 §3.2 / 決定: D-664・D-674・ADR-0469 の 2-1 / CPU リファレンス: engine/src/sim/multires_refine_cells.cpp(T-1176)

## 目的(1〜2 行)
T-1176 の CPU リファレンス(PlaceTreeCell・PlaceRefineOctantCells)と同じ答えを GPU の multires_graph.hlsl RefineLevel で出し、置かれ方を既定 on にする(D-664)。

## 完了条件
- [x] RefineLevel の「セルと端数」の後で、親の八分の一の 4³ セル + 周り 1 の 6³(ブロックの中で切る)の詰まりをグループ共有へ集め(1 スレッド = 子のセル。512 スレッド)、
      同じ親を読む 8 スレッドの組で placement_refine.hlsli を回す(PR_MATERIAL(m) = g_species[cell.species[m]] の相の印とモル体積・PR_PARENT_MASS(m) = 親の数 × 8。
      親の成分はバッファから毎回読む〔書き換えた配列を動的な添字で読まない。T-0397〕)。気体の配り直しの空きの和はグループ共有(placement_refine_check.hlsl の形)
- [x] エネルギーは熱容量の重み(CPU の ChildEnergies)・成分は量 0 を消して詰める・端数は SAME。溢れ(MR_WIDE_CELLS)・一様な親・影は SAME
- [x] HLSL で 64bit の引き算と比べを並べない(熱の配りの FxMulDivS64・エネルギーの残りの引き算の後に比べを置かない)
- [~] 隣のブロック(索引で同じレベル)の列を読むか決める → 1 本目は「ブロックの中だけ」のまま(CPU と同じ)。読むのは T-1189: 同じ刻みに別の要求が覆いうるので、読むなら刻みの前の詰まりの写し(要求ごとに先に作る)から。1 本目は「ブロックの中だけ」のままでもよい(T-1176 の仮)
- [x] gpu_multires に置かれ方の世界の場面 → 別の実行ファイル gpu_tree_refine_placement(_warp)にした(場面は tests/tree_refine_placement_scene.h を CPU の試験と共有。鎖の組の 10 分の枠を食わない)(tree_refine_placement の場面)を足し、CPU とビット一致(HW・WARP)
- [x] 既定 on: 世界の nest を作る所(GpuMultires・probe の世界)で EnableRefineCellPlacement。変わる試験を数えて理由ごとに取り直しの表をチケットに
      (気体だけ・詰まり切った・据わる固体のセルの試験は変わらないはず。水・ばらける固体の混ざるセルを細かくする試験だけが変わる)
- [ ] 結果が変わる所の比べ: 0.25 m の 25° の斜面で空気の速さ(T-1106 の 2.8 m/s → 1 mm/s)→ T-1189(案 B の場面を世界の木で作る所から)

## メモ
- 傾きの式は T-1176 で 32bit にし GPU で確かめた(gpu_placement_refine の傾きの 8192 件)。
- 粒の詰まりは Q173 → D-679(書ける・書かなければ 60 % と警告)。この行で入れた(下の作り)。

## 作り(2026-10-27 本体の作業役 11 回目)
- **GPU**: `shaders/sim/multires_refine_place.hlsli`(新)の `PlaceRefineChildren` を `multires_graph.hlsl` の RefineLevel が「セルと端数」の後に呼ぶ(溢れを使わない変種だけ。
  要求の本物の細かくする・一様でない親・印 `MR_GRAPH_ENTRY_PLACE`)。1 スレッド = 親のセル t の詰まり → グループ共有(子に覆われたセルは −1)→ 1 スレッド = 子のセル t の
  傾き・配り方・空きの体積 → 熱容量 → エネルギー(最後の重みのある子が残り)→ 子の頁へ直接書く(成分は量 0 を消して詰める)。物質は毎回 g_cells・g_species から読む(T-0397)。
  グループ共有 6 KiB(2 つの欄を段ごとに使い回す。2026-10-29 wt2 で 12 → 6 KiB)。
- **入口**: 置かれ方の段は別の入口のノード `RefinePlaceNode`(RefineNode と同じ中身 `MR_REFINE_NODE_BODY(place)` で、place だけ true。溢れを使わない変種だけ)。
  `GpuMultires::RecordUpload` が、`nest.refineCells.species` が空でなく溢れを使わない世界なら `g_graphEntries` の下位 16bit にこの入口を書く
  (= CPU の ProcessRequests と同じ条件)。ほかの世界は今まで通り軽い RefineNode(2026-10-29 wt2。前の版の bit 31 の印は外した。下の作業ログ)。
- **64bit の差**: `placement_refine.hlsli` の差(列の分け・縦の残り・気体の残り)と熱の差・エネルギーの残りを `PrSubS64`(32bit の 2 つに分けて借りを手で回す)に。
  子の量を 0 と比べるため(T-1078・T-1159 の形)。値は同じ。
- **既定 on(D-664)**: 世界の箱 `MakeLabBoxNest` が `EnableRefineCellPlacement`。表の差し替え(`RemapLabBox`)は新しい表で持ち直す。`MakeMultiresNest` だけの木
  (試験の器)は SAME のまま(作り手は世界の箱を通るので、作り手から見た既定は置かれ方)。
- **D-679**: パッケージの `packing_percent`(1〜100 の整数・ばらける固体だけ)→ `SpeciesTransport::packingPercent` → `RxSpecies::flags` のビット 8〜14
  (`RX_SPECIES_PACKING_SHIFT`・`RxSpeciesPackingPercent`。語は増えない)。書かないばらける固体は 60 % と警告・据わる固体に書くと効かないと警告。
  手で印を作る表の 0 は 100 %(今まで通り)。CPU の MaterialOf・GPU の PlaceMaterialOf が同じビットを読む。型の定義 `data/types/bicameral.d.luau` にも。
- 試験: `gpu_tree_refine_placement`(新。SAME・置かれ方 1 段・2 段の鎖 × 詰まり 100 % / 60 % の 5 場面で状態の全部の要約が CPU と同じ。HW の RTX・WARP)・
  `reaction_package`(詰まりの読み・印・警告 2 つ・誤り 3 つ)。

## 変わる試験(既定 on の取り直し)
| 試験 | 前 | 後 | 理由 |
|---|---|---|---|
| (無し) | | | 既定 on は世界の箱(MakeLabBoxNest)だけ。箱を使う試験(gpu_lab_box・window_lab 系・time_step_lab)は CPU と GPU が同じに変わるか、凝縮相の混ざるセルを細かくしないので、期待値の書き換えは要らなかった(下の作業ログ)。lab_box は前から止めてある(T-1155 待ち) |

## 作業ログ
- 2026-10-29 wt2(作業役): WIP(28e6611a)の release のビルドは通った(警告 0)。CPU の tree_refine_placement・placement_refine・reaction_package・multires 系 5 本、
  GPU の gpu_tree_refine_placement(HW・WARP)・gpu_placement_refine・gpu_lab_box(HW・WARP)は合格。**ただし HW の gpu_multires(chains-real)が DEVICE_HUNG**(WARP は合格)。
  main(同じ日の release)の gpu_multires は合格。置かれ方の段の呼び出しを `&& false` で外すと合格 → 置かれ方を使わない木でも、段が同じノードにあるだけで RTX の HW の Work Graph が止まる。
  グループ共有を 12 → 6 KiB(欄の使い回し)にしても止まった = コードの重さ(512 スレッドのノードのレジスタ。T-0124・T-0397 と同じ種類と推定)。
- 直し(1 回目・効かなかった): 置かれ方の段を別の入口 `RefinePlaceNode` に分け、CPU が世界ごとに入口を選ぶ(使わない世界は RefineNode)。
  → **それでも HW の gpu_multires が止まった**(gpu_multires_stress・gpu_lab_box・gpu_tree_refine_placement の HW、WARP は合格)。
  同じ Work Graph のプログラムに重いノードがあるだけで、ほかのノードも止まる(RTX はプログラム全体で 1 つの重さ〔レジスタ〕を取ると推定。未確認)。
- **残る心配(司令塔へ)**: 置かれ方の世界(既定 on の世界の箱)は重いノードを通る。小さい場面(gpu_tree_refine_placement・gpu_lab_box)の HW は合格だが、
  多くの要求の鎖が同時に走る大きい世界で止まらないかは未確認。止まるなら、細かくする段を Compute へ移す(D-546)か、置かれ方の段のレジスタを減らす(64bit の割り算の回しを 1 か所に)。
- 既定 on の取り直し: 世界の箱を使う GPU の試験(gpu_lab_box HW・WARP)は期待値の書き換えなしで合格。window_lab は main でも同じ所(「隣の根の木が温まっていない(300004 mK)」)で落ちる(前からの不具合。この枝のせいではない)。
- 前からの不具合(main でも落ちる。この枝のせいではない): radiation_reference(line 394 allZero。real_matter の物質が放射の吸い込みを書いている)・standard_packages(line 269 overrides)・window_lab。
- 【wt5 T-1233 と reaction.hlsli が重なる】この枝は reaction.hlsli に RX_SPECIES_PACKING_SHIFT・RX_SPECIES_PACKING_MASK・RxSpeciesPackingPercent(物質の印の近く 8 行)を足しただけ。成分の読み方は触っていない。

## 引き継ぎ(2026-10-29 wt2・作業役。2 時間で区切った)
- 状態: ブランチ t-1188-refine-gpu。release のビルドは通る(警告 0)。CPU と GPU のビット一致は合格。**HW の gpu_multires(chains-real・置かれ方を使わない木)だけが DEVICE_HUNG**。マージしない。
- 次の手(おすすめ順):
  1. 置かれ方の段を**別の Work Graph のプログラム**にする: shaders/CMakeLists.txt で multires_graph.hlsl を `NAME multires_graph_place DEFINES MR_REFINE_PLACE=1` でもう 1 つ作り
     (RefineNode が place = true。ほかのノードは同じ)、GpuMultires が置かれ方の世界だけそのプログラムで細かくする/粗くする(GPU の入力の見出しの入口の番号もそのプログラムの番号)。
     置かれ方を使わない世界の木は main と同じプログラムだけを持つ = gpu_multires は main と同じになるはず。プログラムを選ぶのは RecordUpload(nest で決まる)なので、
     2 つ目のプログラムは Create で作っておくか、要る時に作る(作り直しの時間に注意)。
  2. それでも置かれ方の世界の大きい場面で止まるなら、細かくする段を Compute へ(D-546。Work Graph は測って速い所だけ)。研究: 打ち切りは「gpu_multires を置かれ方の世界で回して止まる」。
- 確かめに使う組: `^(gpu_multires|gpu_multires_stress|gpu_multires_warp|gpu_lab_box|gpu_tree_refine_placement|gpu_tree_refine_placement_warp)$`(release で約 3 分 + 待ち)。
  main の比べは runner/main-gpu-base.ps1(raw。GPU の順番を取って ..\..\out の main の release の gpu_multires_test と bicameral を流す)。
- まだ: `git rebase main`(origin に push 済みの枝なので、載せ直すと force push が要る。次の作業役は main から新しい枝を作って cherry-pick するのが早い)。
