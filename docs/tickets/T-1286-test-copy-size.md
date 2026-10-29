# T-1286 物の熱の GPU 試験の写しの大きさの食い違い(T-1284 と同じ形)を直す

- Status: Done(2026-10-30 wt5)
- 種類: 工学(試験だけ。エンジンは変えない)
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 列 4(T-1284 に付く)
- 設計: CLAUDE.md §1-4 / 決定: ADR-0008・T-1284

## 目的(1〜2 行)
T-1284 で、写し元(CreateFilledUpload。256 バイト以上に切り上がる)と UAV の大きさが食い違うのに CopyResource していたため、WARP でデバイスを失っていた。
同じ形が tests/gpu_object_heat_stages.h にもあるので直す(gpu_object_heat_test・gpu_object_heat_nest_test・gpu_object_shell_flow_test の比べが使う)。

## 完了条件(チェックできる形で)
- [x] gpu_object_heat_stages.h の UploadWorld を CopyBufferRegion(UAV の大きさ)に
- [x] ReadUav も、読む数が UAV の前の部分だけの時(object_heat_nest の口の後ろの部分を足したセル・段の並び)に大きさが食い違うので、読む大きさだけ CopyBufferRegion で写す
- [x] release のビルド警告 0・gpu_object_heat / gpu_object_heat_nest / gpu_object_shell_flow(HW)と gpu_object_shell_flow_warp が通る

## メモ・参考
- tests/ の中の CopyResource と RecordCopyToReadback(中で CopyResource)を grep で全部見た(2026-10-30 wt5):
  - 直したもの: tests/gpu_object_heat_stages.h の UploadWorld(写し元は CreateFilledUpload)と ReadUav(読み戻しは count × 大きさ。UAV はそれより大きいことがある)。
  - 食い違わないもの: gpu_fixed・gpu_phase_layer・gpu_placement_refine・gpu_reaction・gpu_surface_detail・gpu_work_graph の RecordCopyToReadback は、写し元と読み戻しを同じ大きさで作っている。
- エンジンの側(engine/src/sim/gpu_gas.cpp・gpu_gas_tree.cpp・gpu_multires.cpp・probe_sim_gas.cpp・gpu/resources.cpp の RecordResidentUploads)の CopyResource は、
  写し元と写し先を同じ大きさで作っている(読んで確かめた。gpu_gas の CreateUploadBuffer は切り上げない)ので変えない。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-30 wt5(t-1286-test-copy-size。t-1285 の上 → rebase main 9f620e21): UploadWorld を CopyBufferRegion(UAV の大きさ)に・ReadUav を読む大きさだけの CopyBufferRegion に(状態は COMMON → COPY_SOURCE)。
  release 警告 0。HW の gpu_object_heat 6 秒・gpu_object_heat_nest 13 秒・gpu_object_shell_flow 30 秒が合格。gpu_object_shell_flow_warp は 1 回目がほかの作業ツリーのビルドと重なり 900 秒の上限に当たった
  (16 本目の途中まで全部ビット一致)ので上限を 1800 秒に。2 回目は 304 秒で合格。物の熱の WARP の登録は無い(足していない)。
