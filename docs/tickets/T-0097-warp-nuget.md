# T-0097 新しい WARP(NuGet の Microsoft.Direct3D.WARP)を試す

- Status: Done(2026-10-03)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(小さい)
- マイルストーン: M1 (docs/plan/ROADMAP.md)
- 設計: ADR-0013 / 16 / 決定: 2026-10-03 ユーザー決定(T-0017 の BACKLOG から (a))

## 目的(1〜2 行)
OS の WARP では Work Graph を作る所で落ちていた仮の世界・多重解像度のテストが、NuGet の新しい WARP(1.0.21、2026-09-22)を
exe の横に置けば動くかを確かめる。動けば ADR-0013 を戻して WARP のテストを足す。

## 完了条件(チェックできる形で)
- [x] NuGet の WARP を exe の横に置く仕組み(ビルドで取ってくる・版とハッシュを固定)
- [x] 実際に読み込まれた d3d10warp.dll の場所と版をログに出す(OS の WARP と取り違えないため)
- [x] gpu_multires・gpu_probe_sim・gpu_probe_trace が WARP で CPU リファレンスとビット一致(ctest に戻す)
- [x] ADR-0013 を戻す(どの版から直ったかを書く)

## メモ・参考
- 2026-10-03 の手での確認(release): OS の WARP(10.0.26100.9278)は gpu_multires_test --warp が 3.6 s で落ちる。
  NuGet 1.0.21 を bin\ に置くと gpu_multires(8.5 s)・gpu_probe_sim(74 s)・gpu_probe_trace(26 s)が全部 OK。
- 1.0.21 の変更点: 「制御の流れの変換を作り直した」(README)。ADR-0013 の「ノードが少し複雑になると JIT が落ちる」に当たると見ている(未確認)。
- 版の一覧には 1.65535.20-preview もあるが、これは 1.0.20 からのプレビュー(2026-05-27)で 1.0.21 より古い。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-03: OS の WARP(10.0.26100.9278)で落ちる gpu_multires_test --warp が、NuGet の WARP 1.0.21 を bin\ に置くと通った。仮の世界・トレース・物理も WARP で CPU とビット一致。
  ルートの CMakeLists.txt が nupkg を版と SHA-256 で固定して取り(_deps/warp-1.0.21)、engine/CMakeLists.txt が bin/ にコピーする。
  gpu/device.cpp が読み込んだ d3d10warp.dll の場所と版をログに出す(exe の横でなければ Warning)。
  ctest に gpu_multires_warp・gpu_probe_sim_warp・gpu_probe_trace_warp を戻し、gpu_physics_stack_warp(120 刻み)を足した。debug の warp 10 件・release の全 49 件・tidy が通る。
  ADR-0013 を Superseded にし、D-427 を足した。窓のアプリも --warp で動く(1.3 fps)。
