# T-1188 木を細かくする段のセルの中身の置かれ方を GPU で・既定 on(T-1176 の続き)

- Status: Done(2026-10-27 本体の作業役 11 回目。隣のブロックの列・斜面の空気の速さの比べは T-1189 に分けた)
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
  グループ共有 4 × 4 KiB。
- **印**: ルート署名に語が無いので `g_graphEntries` の bit 31(下位 15bit = RefineNode・16〜30 = CoarsenRequestNode)。`GpuMultires::RecordUpload` が
  `nest.refineCells.species` が空でなく溢れを使わない世界なら立てる(= CPU の ProcessRequests と同じ条件)。
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
