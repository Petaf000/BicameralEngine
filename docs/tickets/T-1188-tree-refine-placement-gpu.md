# T-1188 木を細かくする段のセルの中身の置かれ方を GPU で・既定 on(T-1176 の続き)

- Status: Todo(2026-10-26 本体が T-1176 から分けた)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: 試作の列・土台の変更要求の束(docs/plan/ROADMAP.md)
- 設計: docs/design/20-transport-substrate.md §10.5・17 §3.2 / 決定: D-664・D-674・ADR-0469 の 2-1 / CPU リファレンス: engine/src/sim/multires_refine_cells.cpp(T-1176)

## 目的(1〜2 行)
T-1176 の CPU リファレンス(PlaceTreeCell・PlaceRefineOctantCells)と同じ答えを GPU の multires_graph.hlsl RefineLevel で出し、置かれ方を既定 on にする(D-664)。

## 完了条件
- [ ] RefineLevel の「セルと端数」の後で、親の八分の一の 4³ セル + 周り 1 の 6³(ブロックの中で切る)の詰まりをグループ共有へ集め(1 スレッド = 子のセル。512 スレッド)、
      同じ親を読む 8 スレッドの組で placement_refine.hlsli を回す(PR_MATERIAL(m) = g_species[cell.species[m]] の相の印とモル体積・PR_PARENT_MASS(m) = 親の数 × 8。
      親の成分はバッファから毎回読む〔書き換えた配列を動的な添字で読まない。T-0397〕)。気体の配り直しの空きの和はグループ共有(placement_refine_check.hlsl の形)
- [ ] エネルギーは熱容量の重み(CPU の ChildEnergies)・成分は量 0 を消して詰める・端数は SAME。溢れ(MR_WIDE_CELLS)・一様な親・影は SAME
- [ ] HLSL で 64bit の引き算と比べを並べない(熱の配りの FxMulDivS64・エネルギーの残りの引き算の後に比べを置かない)
- [ ] 隣のブロック(索引で同じレベル)の列を読むか決める: 同じ刻みに別の要求が覆いうるので、読むなら刻みの前の詰まりの写し(要求ごとに先に作る)から。1 本目は「ブロックの中だけ」のままでもよい(T-1176 の仮)
- [ ] gpu_multires に置かれ方の世界の場面(tree_refine_placement の場面)を足し、CPU とビット一致(HW・WARP)
- [ ] 既定 on: 世界の nest を作る所(GpuMultires・probe の世界)で EnableRefineCellPlacement。変わる試験を数えて理由ごとに取り直しの表をチケットに
      (気体だけ・詰まり切った・据わる固体のセルの試験は変わらないはず。水・ばらける固体の混ざるセルを細かくする試験だけが変わる)
- [ ] 結果が変わる所の比べ: 0.25 m の 25° の斜面で空気の速さ(T-1106 の 2.8 m/s → 1 mm/s)

## メモ
- 傾きの式は T-1176 で 32bit にし GPU で確かめた(gpu_placement_refine の傾きの 8192 件)。
- 粒の詰まりは Q173(仮 = 100 %)。

## 作業ログ
