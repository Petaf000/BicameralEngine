# T-1284 層の流れの GPU 試験が WARP でデバイスを失う(gpu_object_shell_flow_test --warp)

- Status: Done(2026-10-30 wt5。原因は試験の写し: 写し元と UAV の大きさの食い違い)
- 種類: 工学(原因の切り分け。結果の読めない所は打ち切り条件付き)
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 列 4(T-1200〜T-1222 の層の流れの研究の形に付く。司令塔が置き場所を決める)
- 設計: CLAUDE.md §1-4(GPU カーネルは CPU リファレンスとビット一致)・T-1200・T-1222 / 決定: ADR-0008

## 目的(1〜2 行)
`gpu_object_shell_flow_test --warp` は main 4d60f487 のシェーダー(T-1222 の前)でも「デバイスが失われた(DXGI_ERROR_DRIVER_INTERNAL_ERROR)・GPU の準備に失敗」で落ちる。
原因を切り分け、WARP でも比べ(CPU とビット一致)が流れる形にする(ctest の gpu_object_shell_flow は今 HW だけの登録)。

## 完了条件(チェックできる形で)
- [x] 原因が分かる(下の切り分けのどこで落ちるか・WARP の版)
- [x] `gpu_object_shell_flow_test --warp --queue compute` が比べ 16 本(4 規則 × 2 形 × groupshared / 作業場)ビット一致で通る。直せない時は ctest の登録に理由を書いて HW だけのまま閉じる
- [x] release のビルド警告 0・HW の比べも変わらない

## 見立て(2026-10-29 wt5。未確認)
- 落ちる所: ログは 2.0 秒で「デバイスが失われた」→「GPU の準備に失敗」。メッセージの出方(tests/gpu_object_shell_flow_test.cpp の RunCompare: CreateGpu は成功・
  その後の ImmediateQueue / DebugRing / Upload のどれか)から、**最初の ExecuteAndWait(Upload = 写すだけのリスト)の待ちで失われた**と読める。写すだけで失われるので、
  原因はリストの中身ではなく、その前に作った**パイプライン(1,024 スレッドのグループ・groupshared 約 28 KB + α)を WARP が JIT する所**と見る(WARP は PSO の作成を遅らせて行い、失敗をデバイスの喪失として出すことがある。未確認)。
- 候補: (a) 1,024 スレッド × 28 KB の groupshared が WARP 1.0.21 の上限か JIT の不具合に当たる / (b) シェーダーが大きすぎる(asm 1.7 MB。内側の回しの展開)/ (c) compute のキューと WARP の組み合わせ(ほかの GPU 試験の WARP は compute で通っているので低い)。

## 切り分けの手順(ランナーの raw で 1 本にまとめて。WARP は GPU の順番を取らなくてよい)
1. `--warp --queue direct` と `--warp --queue compute`(c を外す)
2. `--warp --threads 512` / `--threads 256`(object_shell_flow_t512 / t256。グループの大きさ。groupshared は同じ) — 256 で通れば (a) のスレッドの数
3. `OH_SHELL_SHARED_VOXELS` を 1,024 にした .cso を dxc で作って差し替え(runner/t1222/ab3.ps1 と同じ作り直し。比べは作業場だけ) — 通れば (a) の groupshared
4. Upload の前にパイプラインを作らない形(試験に `--no-pipeline` のような切り分けは足さず、raw で PSO だけ作るのは難しいので、2・3 で決まらなければ):
   g_stageMask で段を外しても PSO は同じなので、シェーダーを段ごとに `#if` で外した .cso(⑤ 圧力・⑧ 運ぶ)で (b) を見る
5. WARP の版: 今 1.0.21(out/build/release/_deps/warp-1.0.21)。新しい版(NuGet Microsoft.Direct3D.WARP)で通るかを 1 回だけ
- 打ち切り条件: 1〜5 で原因が決まらなければ、ctest の登録に「WARP は T-1284 で未解決」と書いて HW だけのまま閉じ、司令塔へ。

## メモ・参考
- 前の版で落ちることは runner/t1222/warp_old.ps1(main 4d60f487 のシェーダーを入れて WARP)で確かめた(2026-10-29 wt5)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29 wt5: T-1222 から分けた(見立てと切り分けの手順)。
- 2026-10-29 wt5: 手順 1・2 を流した(runner/t1222/warp_split.ps1。T-1222 2 本目のシェーダー): `--queue direct`・`compute` × `--threads 1024 / 512 / 256` の 4 つとも 2.6〜2.8 秒で同じ「GPU の準備に失敗」。
  キューの種類と 1 グループのスレッドの数ではない((c) と (a) のスレッドの数を外した)。次は手順 3(groupshared を小さくした .cso)・4・5。
- 2026-10-30 wt5(ブランチ t-1285-multigrid-even): 手順 3・4 を流した(runner/t1284/mkvar.py → empty / shared1024 / nopressure / nomove / nowake の .hlsl・warp_cut.ps1)。
  **入口ですぐ return する空のシェーダーでも同じ「GPU の準備に失敗」**だったので、シェーダー(groupshared・大きさ・段)ではない。
  原因: 試験の Upload が `CopyResource(UAV, 写し元)` で、写し元は CreateFilledUpload が 256 バイト以上に切り上げる(MIN_RESIDENT_BUFFER_BYTES)のに、UAV は元の大きさ
  (数え上げの欄 g_stats は 2 + 2 × 物 + 4 語 = 40 バイト)で作っていた。CopyResource は同じ大きさの 2 つの間だけ(D3D12 の約束)。HW のドライバーは通し、WARP はデバイスを失った。
  直し: tests/gpu_object_shell_flow_test.cpp の Upload を `CopyBufferRegion(UAV, 0, 写し元, 0, UAV の大きさ)` に(試験だけ。シェーダーとエンジンは変えない)。
  `--warp --queue compute` は比べ 16 本が通って OK(約 275 秒)。ctest に gpu_object_shell_flow_warp(ラベル warp・上限 900 秒)を足した。
- 同じ形が tests/gpu_object_heat_stages.h の 222 行(`CopyResource(world.uavs[i], world.uavUploads[i])`。UAV は元の大きさ・写し元は 256 以上)にある。gpu_object_heat 系は WARP の登録が無いので今は表に出ていない(司令塔へ。直すなら同じ 1 行)。
