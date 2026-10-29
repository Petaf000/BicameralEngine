# T-1189 木を細かくする段の置かれ方: 隣のブロックの列を読む・斜面の空気の速さの比べ(T-1188 の続き)

- Status: Done(2026-10-29 wt2・t-1189-refine-neighbors: 隣のブロックの列を刻みの前の写しから読む。CPU・GPU ビット一致。斜面の空気の速さの比べは T-1257 に分けた)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列・土台の変更要求の束(docs/plan/ROADMAP.md)
- 設計: docs/design/20-transport-substrate.md §10.5・17 §3.2 / 決定: D-664・D-674・D-679 / CPU: engine/src/sim/multires_refine_cells.cpp・GPU: shaders/sim/multires_refine_place.hlsli

## 目的(1〜2 行)
細かくする親のセルが親のブロックの端にある時、隣のブロック(索引で同じレベル)の列から傾きと上下の詰まりを読む(今は「無い」と読む = 横は傾き 0・下は詰まり切る・上は空)。
置かれ方で結果が変わる所を 1 つの場面で測って残す。

## 完了条件
- [x] 隣のブロックの列: 同じ刻みに別の要求が隣を覆いうるので、刻みの前の詰まりの写し(要求ごとに先に作る)から読む。CPU と GPU で同じ(gpu_tree_refine_placement に端の場面)
- [x] 下が木の中で無い時は粗い親のセルの詰まりを読むか決める(1 本目は詰まり切る)→ 仮: 読まない(下の「判断」)
- [ ] 結果が変わる所の比べ: 0.25 m の 25° の斜面で空気の速さ(T-1106 の 2.8 m/s → 1 mm/s)→ **T-1257 に分けた**(世界の木の上に斜面の場面を作る所から。下の「残り」)

## メモ
- T-1188 で GPU の置かれ方と既定 on(世界の箱)を入れた。ブロックの中だけで読むのは CPU も GPU も同じ(ビット一致)。

## 目的の照らし合わせ(2026-10-29 wt2)
- D-532(土台は要る現象の一覧から逆算)・D-664(置かれ方で配る 1 つの規則・隣の親を読む)と合う: 細かくする出来事の時だけ隣を読み、書く値は増えない。
  土台の共通部分(見立て・面の帳簿・当てる段・登録)は足さない。置き場は多重解像度の木の u6 の溢れの領域の場所(溢れを使わない世界では空いていた)を使う。
- 完了条件 2(下が木の中で無い時の粗い親)は **仮: 読まない(詰まり切る)のまま**(下記)。完了条件 3 の斜面の比べは、案 B の場面(case_b)が世界の木を通らない
  (case_b_scene の絵 → 箱の細かくする口は PlaceRefineCell を直接呼び、隣は絵のセルから読む = 既に隣を読んでいる)ので、世界の木では場面を作り直す仕事になる → 下の「残り」。

## 作り(2026-10-29 wt2)
- **隣の写し**(`shaders/common/multires_refine_halo.hlsli`。CPU と GPU の 1 つの源): 細かくする要求の鎖の段ごとに、親のブロックの八分の一の外を向く 3 面の 6 × 6 = 108 セルの
  詰まり(Q16。無い = −1)。置かれ方が読むのは列 (x ± 1, z)・(x, z ± 1) の段 y − 1〜y + 1 と下の段だけなので、外に出るのはこの 3 面に収まる。
  置き場の位置 = 段 d の子の枠を取った空きのスタックの位置(state.blockBase − 1 − d。許可した段ごとに別で世界の枠の数未満)。
- **刻みの前から読む**: 写すのは割り当ての後・適用の前(CPU は ProcessRequests の Allocate の後の `SnapshotRefineHalos`、GPU は TreeAllocate の後の
  Compute の段 `sim/multires_refine_halo.hlsl` の TreeRefineHalo。1 グループ = 要求 1 つ)。同じ刻みに別の要求が隣を覆っても・粗くしても答えは同じ(順番に依存しない)。
  鎖の 2 段目以降の親はこの刻みに作るので、その外の隣も刻みの前の木の同じレベルのブロック。
- 隣は索引で同じレベルの本物のブロック(一様なら一様の値)。子に覆われたセル・ブロックが無い所は無い(T-1176 の仮のまま: 横 → 傾き 0・下 → 詰まり切る・上 → 空)。
- GPU の置き場: 溢れを使わない世界の u6 の後ろ(g_overflowBase から。MR_REFINE_PLACE の変種がルート定数 g_overflowBase を宣言する。ルート署名の語は変わらない)。
  世界の枠 × 108 × 4 B(枠 1 万で 4.3 MB)。溢れを使う世界は置かれ方を使わないので写しを作らない。
- 欄(SF_FLAG_PLACE_*)の重み(refinePlacement)も同じ写しを読む(RefineRequestLevel が nest_fields::Refine の後まで写しの位置を持つ)。
- 試験: `tree_refine_placement`(端の場面: 隣を読むと端の列が変わる・保存・隣を同じ刻みに細かくする 2 つの要求のどちらの順でも 1 つずつと同じ・決定的)、
  `gpu_tree_refine_placement`(端の 5 場面: 隣あり 100 % / 60 %・隣無し・同じ刻みの 2 要求の両順。HW・WARP)。

## 判断(仮・ユーザー未確認)
- 【エンジン】下の隣が木の中で同じレベルに無い時(粗いブロックしか無い)に粗い親のセルの詰まりを読むか → **仮: 読まない(詰まり切る)**。
  作り手から見た違い: 粗い所との境の上の段を細かくすると、下は詰まり切ったものとして配る(粒は下の子から詰まる)。粗い親を読む形は、粗い親の詰まりが子 8 つ分の平均なので
  端の段が少しずれる。どちらも保存はビット単位。取り消しやすい(HaloFullness で 1 段粗い索引を引く形に足せる)。横の隣も同じ(粗い所との境は傾き 0)。

## 作業ログ
- 2026-10-29 wt2(作業役): 隣の写し(CPU の SnapshotRefineHalos・GPU の TreeRefineHalo)を作り、置かれ方が親のブロックの外の列を写しから読む形にした。
  release のビルドは警告 0(1 回目は link の Access is denied で落ち、流し直して通った)。tree_refine_placement・placement_refine・multires・
  gpu_tree_refine_placement(HW・WARP。端の 5 場面を含む 10 場面がビット一致)が合格(20261029-074240・075019)。費用は docs/perf.md(写しの段 +0.004〜0.007 ms)。
  斜面の空気の速さの比べは T-1257 に分けた。
  引き継ぎの組 14 本(reaction_package・gpu_multires・stress・wide_many・warp・activity・quiet・layers・uniform・gpu_lab_box〔HW・WARP〕・gpu_placement_refine〔HW・WARP〕・gpu_phase_layer)も合格(20261029-075222。u6 の後ろを溢れを使わない世界でも使うようにしたので全部の非溢れの世界に当たる)。`archmap --check` OK。

- 2026-10-29 wt2(作業役 2 回目・仕上げ): rebase main(4d60f487。perf.md の行だけ衝突 → 両方残した)。dxil_alloca_check(T-1233 で main に入った検査)が
  multires_refine_halo の 1 個で落ちた → 隣の写しの段で子をバッファから直に読む形に直して 0(e8b7feda)。ほかに落ちた 6 本(implicit_levels_tail_o0 など)は
  今のビルドが作らない古い asm(10-08・10-09)の残りで、`_to_delete/stale-asm-20261029/` へ移した。release 警告 0。CPU の組(multires・placement_refine・
  tree_refine_placement・multires_layers・float_check 5 本・dxil_alloca_check)と GPU の組(gpu_multires・gpu_multires_layers〔HW・WARP〕・gpu_placement_refine〔HW・WARP〕・
  gpu_tree_refine_placement〔HW・WARP〕。20261029-115310)が合格。`archmap --check` OK。

## 残り(T-1257 へ)
- 0.25 m の 25° の斜面の空気の速さの比べ(T-1106 の 2.8 m/s → 1 mm/s): 案 B の場面(case_b)は世界の木を通らず、絵 → 箱の細かくする口が PlaceRefineCell を直接呼び
  隣は絵のセルから読む(= もう隣を読む形。T-1106 の 1 mm/s はこの形の値)。世界の木で同じ比べをするには、斜面を世界の木の複数の根に描き、
  前線で細かくして(要求)から流体の段を回す場面が要る(世界の木の上で空気が動く試験の器 = gpu_lab_box 系か multires_gas。大きい)。
  比べる 3 行: 隣を読まない(T-1188 の形)・隣を読む(この枝)・箱の細かさで描いた斜面。

## T-1240 の着手に要るもの(司令塔へ。この巡目は着手しない)
- T-1188(t-1188-refine-gpu-2)と T-1238 が main に入っていること。この枝(T-1189)も multires_refine_place.hlsli・multires_graph.hlsl・gpu_multires.cpp の
  RecordProcessRequests を触るので、T-1240 はこの枝の後か、この枝の上から始める。
- **u6 の後ろ(g_overflowBase から)は、溢れを使わない世界ではこの枝の隣の写しが使う**(世界の枠 × 108 語)。T-1240 が層の置き場を「ほかのバッファの後ろ(溢れと同じ)」に置くなら、
  写しの後ろ(g_overflowBase + 世界の枠 × MR_HALO_CELLS)からにする。木の段(multires_graph.hlsl・multires_tree.hlsl・multires_refine_halo.hlsl)は u4・u5 を
  宣言していない(2026-10-29 grep で確かめた)ので、層の置き場を u4 に結ぶ形(T-1221 の multires_layers.hlsl と同じ)も取れる。
- 置かれ方の段の層: CPU は MaterialsOf が層を「層の分(沈む側)・残り」の 2 つの物質に分けて配り、隣の詰まり(CellFullness)も層まで含めて読む。
  GPU の PlaceFullnessAt・PlaceRefineChildren は層を読まない(層の無い世界だけ一致)。GPU の写しの段(HaloFullness)も同じく層を読まないので、T-1240 で層を足す時は
  写しの段の詰まりにも層を入れる(CPU と同じ式)。
- HW の Work Graph の止まり(T-0397・alloca)に注意: 置かれ方の段は別のプログラム(ADR-0499)。層の読みを足すとさらに重くなるので、層の物質を分ける回しは
  書き換えた構造体の配列を動的な添字で読まない形(t-1233 の後なら共通の HLSL のローカルの配列は消えている)。

