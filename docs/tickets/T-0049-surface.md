# T-0049 表面の生成(Surface Nets・切り口・細かいレベルの表面)

- Status: Todo(2026-10-09 に司令塔が事前調査を置いた。着手はまだ)
- 設計: 10 §2・§3・17・05・ADR-0015・ADR-0016。関係: T-0046(破壊・切り口)・T-0002/M2 の相変化(相の欄・モル体積)

## 事前調査(2026-10-09、調べ役。読むだけ。未確認の数値は「推定」「未確認」)

### 今の実装の要点
- 描画は shaders/render/probe_view.hlsl(717 行、T-0015)だけ。画面いっぱいの三角形 1 枚で、仮の世界(64³)の抽出をセルごとの DDA でレイマーチ(立体・最大値・断面)。覗き窓(T-0096)の細かい段は断面にだけ。物は解析的な回った箱。メッシュ・メッシュシェーダー・メッシュレットのコードは無い。画像のテストは probe_volume・probe_slice の 2 枚。多重解像度の木を描画用に抜き出す経路は無い(覗きの鎖だけ。multires_peek.hlsl)。
- 前提の構造: 8³ ブロックの比 2 の疎な木。子は親の 4³ を覆い、覆われた親のセルは空(ADR-0015)→ 空間は「覆われていない八分の一」に隙間なく分かれ、隣のレベル差に制限は無い。一様なブロックは値 1 つ。busyTick がセルの変わった刻み。面の隣は索引をレベルの上へ引いて求める(WakeFaceNode)。
- 素材: 「相」の欄はまだ無い(02 §2)。物はボクセルの木で、手作りのメッシュの物は切れた所だけボクセルの表面に置き換える(08 §1)。

### 結論(推奨)
- 「固体の割合」の場から **Surface Nets(双対の方式)** でメッシュ。ブロックの中は 10³(周り 1 セル込み)を共有メモリに読んで作る。ブロックの境目とレベルの境目は **Ju 2002 の「最小の辺」の規則**で、索引を引いて 3〜4 個の隣を集めて面を張る。
  - 双対の方式は隣のレベル差に制限の無い木でも継ぎ目を当て直さずに閉じた面になる(Ju 2002)。Transvoxel は 2:1 が前提で合わない。跨ぐ隣の引き方は活性(WakeFaceNode)と同じ形で使い回せる。
- 鋭い角と切り口は 2 段目: Dual Contouring の QEF・法線は割合の勾配。切った時は刃の面(平面)を細かくしたセルに残し平らな断面を出す(Donkey Kong Bananza も 8³ のチャンク + DC で角の追加データを持つ)。
- **見た目と形を分ける**: 焦げ・濡れ・赤熱はマテリアルの段で画素ごとにセルを読む。メッシュを作り直すのは形(割合を量子化した値 + 素材 ID)のハッシュが変わったブロックだけ(燃えて温度が変わるだけなら作り直さない)。
- メッシュの置き場は頁と同じ考え: 固定の大きさのメッシュレットのプール + 空きのスタック(描画は決定的でなくてよいので atomic でよい)。
- Mesh nodes は使わない(1.715 の preview にしか無く、最新の retail 1.619.6〔2026-09〕に無い)。Work Graphs の compute ノードでメッシュレットを書き、ExecuteIndirect / DispatchMesh で描く。D-302 に従い Compute 版と測り比べる。

### 方式の比較
| 方式 | 継ぎ目(この木) | 見た目 | 更新の費用 | 相性 |
|---|---|---|---|---|
| Naive Surface Nets(双対) | 最小の辺の規則で継ぎ目なし(跨ぐ引き方が要る) | 滑らか・角は丸まる | ブロックの中は安い | ◎ 10 §2 の案そのもの |
| Dual Contouring(QEF) | 同上 | 鋭い角・平らな切り口 | QEF の分重い | ○ 2 段目 |
| Marching Cubes + Transvoxel | 2:1 前提で使えない(描画用に釣り合わせた木を別に作れば可) | 滑らか | 遷移セルの表 | △ |
| Occupancy-based DC(2024) | 連続な占有の関数を何度も評価する前提 | 良い | 重い | △ |
| ボクセルを直接レイマーチ(Teardown・Aokana) | 問題なし | ブロック状 | 作り直し不要 | △ VisBuffer・BLAS と別の道。覗き窓・顕微鏡向き |
| SDF + 球トレース(Dreams) | 距離の場の作り直し | 滑らか | 距離の伝播が要る | △ 魔法具の造形(D-421)の候補 |

### 素材が混ざったセルの面・切り口
- 面の位置: 固体の割合 φ = Σ(固体の相の物質量 × モル質量 ÷ 密度)÷ セルの体積。等値 0.5 を辺の上で線形補間。液面は液体の割合で別の面。境の種類は DC の多素材版(符号の代わりに相・素材の番号。Ju 2002)。**相の欄とモル体積が反応表に要る**(それまでは「物質ごとの固体の印」を仮に置く)。
- 切り口: 物もボクセルの木なので同じ方式。手作りのメッシュは残っているボクセルで画素ごとに切り抜き(discard)、断面だけ DC で張る。0.5 m の粗さだと 1 m の箱が丸い塊になる → 物は物ごとのレベルを細かく持つか元のメッシュを使う。

### 試す順番と、次の代案へ移る条件
1. 工学: 仮の世界(64³)で単一レベルの Surface Nets(ブロックの中だけ・φ・作り直しの判定・メッシュレットのプール)。画像テストに場面を足す。メッシュの形は整数で決まるので CPU と数を比べ、頂点の位置は許容差つき。
2. 研究 R-REND-2: レベルを跨ぐ最小の辺の規則を GPU で。基準: ひび・T 字が 0(総当たりで閉じた面か確かめる)・決まった場面で予算に入る。移る条件: 跨ぐ引き方が予算超え → 描画用に 2:1 に釣り合わせた木 + Transvoxel 風 → 遠くだけスカートを許す。
3. 研究 R-REND-2: DC による角と切り口。基準: 10 §7「近くで見ても角ばりや段差が目立たない」を決まった場面の画像で。移る条件: 勾配の法線で角が丸まる → セルに面の情報(刃の面・素材の境)→ 見えている所を細かいレベルで描く。
4. 工学: 更新の費用(変わったブロックあたりの ms・1 フレームの作り直しの上限)。超えたら世界は止めず次のフレームへ持ち越す(描画は View なので遅れてよい。ユーザーに確認)。
5. 観察(B)の影の部分木も描く(覗いている所だけ細かい面に差し替え)。

### ユーザーに聞く点 → QUESTIONS Q27

### リスクと未確認
- 跨ぐ引き方の費用・レベル差が大きい時の再帰の深さ(MR_MAX_WAKE_DEPTH と同じ問題)・地形が変わるたびの DXR の BLAS の作り直しの費用・Mesh nodes が retail に入る時期は未確認。
- 事例の未確認: Astroneer・Deep Rock Galactic(マーチング系と言われるが一次資料未確認)・Enshrouded・Voxel Farm・Minecraft。

### 出典
- Ju ほか 2002 Dual Contouring of Hermite Data: https://www.cs.rice.edu/~jwarren/papers/dualcontour.pdf / Transvoxel: https://transvoxel.org/
- Mesh nodes: https://devblogs.microsoft.com/directx/d3d12-mesh-nodes-in-work-graphs/ / https://devblogs.microsoft.com/directx/directx12agility / https://gpuopen.com/learn/work_graphs_mesh_nodes
- Occupancy-Based DC: https://arxiv.org/abs/2409.13418 / GPU の等値面(Schmitz ほか): https://diglib.eg.org/items/c94e2e75-fc6a-4c55-8e77-c6ef52460288/full
- Teardown: https://acko.net/blog/teardown-frame-teardown/ / Aokana: https://arxiv.org/abs/2505.02017v1 / Donkey Kong Bananza(CEDEC 2026): https://dev.classmethod.jp/en/articles/cedec-2026-voxel/
- Nanite Foliage: https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage / Dreams: https://advances.realtimerendering.com/s2015/AlexEvans_SIGGRAPH-2015-sml.pdf / fast-surface-nets-rs: https://github.com/bonsairobo/fast-surface-nets-rs
