# T-0410 GPU が毎段読む表をアップロードのヒープに置いている所を洗い出して測る(T-0374 から分けた・候補)

- Status: Done(wt6・ブランチ t-0410。gpu_gas の直した後の計測だけ残り)
- 種類: 工学(測って、効くものだけ既定のヒープの写しへ)
- PC: 必須(測るのはランナーが idle の時に)
- 見積もり: 作業役 1 体(1 時間以内)
- 関係: T-0374(気体の木の形の SRV をアップロードのヒープから読んでいて 32³ で約 30 倍遅かった)・docs/perf.md 2026-10-12 の T-0374 の行

## 目的
アップロードのヒープ(CPU のメモリ)に置いたバッファをシェーダーが直接読むと、L2 に収まらない大きさ・L2 から追い出される負荷で PCIe 越しの読みになり、
セルの数に対して超線形に遅くなる(T-0374)。同じ形がほかに残っていないかを洗い出し、効くものだけ直す。

## 完了条件
- [x] `BufferKind::Upload` / `CreateUploadBuffer` / `CreateFilledUpload` で作ってルートの SRV に結んでいる所の一覧(既知の候補: gpu_multires.cpp の反応の表 m_tables〔2 か所〕・
      gpu_gas.cpp の設定・層の基準・層の外・gpu_physics.cpp の設定・初めの物体)。小さい定数(設定 1 つ)は今のままでよい
- [x] 大きい場面(gpu_multires_test --profile など)で、既定のヒープの写しにした時の差を測って docs/perf.md へ。差が測れないものは直さない
- [x] 直したものの試験(ビット一致)が今のまま通る(release: gpu_multires・_warp・_uniform・gpu_lab_box・_warp・gpu_gas・_warp・gpu_gas_tree・_warp・gpu_probe_peek・_warp の 11 本)

## 洗い出し(2026-10-12。engine/src と tests の `BufferKind::Upload` 全部)

シェーダーが SRV として**直接**読んでいたもの(=直す候補):

| どこ | 中身 | 大きさ | 読む頻度 | 扱い |
|---|---|---|---|---|
| gpu_multires.cpp `m_tables`(Create と ReplaceTable の 2 か所) | 反応の表 t0〜t3(物質・規則・索引・速度) | 速度の表が規則 × 4096 K × 8 B = **規則 1 本で 32 KiB**(規則が数十なら MB 級) | 反応の段で毎刻み・セルごとに温度で引く | **直した**(既定のヒープの写し。最初の BindRoot で写す) |
| gpu_gas.cpp `m_srvs` | t0 設定 1 個・t1 層の基準の導く値・t2 層の外 | 層の数(32³ で 32 個)× 数十 B | 毎段・セルごと | **直した**(RecordUpload で写す) |
| gpu_gas_tree.cpp `m_srvs` | 木の形 t0〜t6 | 32³ で面の一覧 6.5 MB | 毎段 | T-0374 で直し済み。共通の道具に置き換えた |
| gpu_physics.cpp `m_srvs[0]` | 設定 1 個 | 1 個 | 毎段 | そのまま(小さい定数) |
| gpu_physics.cpp `m_srvs[1]` | 初めの物体 | 物体の数 | physics_initialize の 1 回だけ | そのまま |
| probe_sim.cpp `frame.input` | 覗き窓の入力(フレームごとに CPU が書く) | PROBE_INPUT_BYTES | 毎フレーム | そのまま(毎フレーム書く小さな定数) |
| frame_loop.cpp `slot.constants`・graph_trace.cpp の絞り込み | フレームの定数・トレースの絞り込み | 小さい | 毎フレーム CPU が書く | そのまま |
| tests/gpu_reaction_test.cpp | 反応の表・初めのセル(試験だけ) | 小さい盤面 | 毎刻み | そのまま(試験。速さを測らない) |

写しの元としてだけ使っていたもの(シェーダーは読まない。直さない): gpu_multires の `m_uploads`・`m_requestUpload`・`m_activityUpload`・`m_patchUpload`、
gpu_gas の初めのセル・帳簿、gpu_gas_tree の `m_uploads`、gpu_implicit の `m_uploads`、gpu_lab_box のコマンド・付け替えの表、probe_sim の初めの表・差し替えの表(T-0194 の形で既に既定のヒープへ写している)。

共通の道具: `gpu::ResidentBuffer`・`gpu::CreateResidentBuffer`・`gpu::RecordResidentUploads`・`gpu::CreateFilledUpload`(gpu/resources.h)。
写した後は NON_PIXEL_SHADER_RESOURCE、リストの終わりで COMMON に戻り、次のリストは読む状態へ暗黙に上がる。
反応の表は GpuMultires::BindRoot が「作った・替えた後の最初の結び付け」で写す(ReplaceTable の「次の結び付けから効く」の意味は前と同じ。
返す前の表には写しの元も入れる)。

## 作業ログ
- 2026-10-12(wt6): engine/src・tests の `BufferKind::Upload` を全部洗い出した(上の表)。シェーダーが直接読む変わらない表は 3 つ(反応の表・気体の一様な箱の t0〜t2・
  気体の木の形〔T-0374 で直し済み〕)。共通の道具 `gpu::ResidentBuffer`(gpu/resources.h)を作り、3 つともそれに揃えた。
- 計測(docs/perf.md 2026-10-12 T-0410。負荷あり): measure-wide で成分の多いセルが 1 割前後速い(上限の種 1326 → 1144 ns/セル など)。30 倍のような外れは無かった。
  気体の一様な箱は直した後を測っていない(GPU の順番待ちで時間切れ。直す前 25.5〜26.6 ms〔GBV あり〕)。

## 引き継ぎメモ
- 反応の表の写しは `GpuMultires::BindRoot` が「作った・替えた後の最初の結び付け」で記録する(m_tablesPending。BindRoot は const なので mutable)。
  そのリストを走らせずに捨てると表は空のまま(今そういう使い方は無い)。ReplaceTable の返す前の表には写しの元も入る(呼ぶ側は今まで通り持つだけ)。
- 写しの組は同じリストで「読んだ後に写し直す」をしない(読む状態のまま COPY_DEST へは暗黙に上がらない)。GpuGas・GpuGasTree の RecordUpload は 1 リストに 1 回。

## 判断待ち
- なし(実装の細部)

## 分けたもの
- なし。gpu_gas の直した後の値は、次に gpu_gas_test --profile を流す時に足す(本体か次の巡目)
