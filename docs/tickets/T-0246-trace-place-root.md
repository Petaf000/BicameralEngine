# T-0246 連鎖のトレースの根に置くコマンドを入れる(T-0222 から分けた)

- Status: Done(2026-10-11)
- 種類: 工学(小)
- 設計: 06 §3・16 §1.3・ADR-0067。関係: T-0087(トレース)・T-0222(置くコマンド)

## やること
置くコマンド(PROBE_COMMAND_TYPE_PLACE)の適用が触ったブロックを、連鎖のトレースの根として記録する(probe_tick.hlsl の ApplyPlace で GtRecord)。
CPU リファレンスの予想(probe_trace.cpp の AppendExpectedProbeTrace)も同じ根と、その根が起こすブロックを予想する。今はトレース中に置くと予想と食い違う。

## 完了条件
- [x] GPU のトレースに「置く」(PROBE_TRACE_PLACE。主 = 触ったブロック・従 = 形の中心のセル)が出る
- [x] CPU の予想と一致する(gpu_probe_trace(_warp) に熱い炭の球を置く段を足した)
- [x] 刻みごとの木のファイルで「置く 中心 (x, y, z)」の根が読める

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-11(作業役・本体): PROBE_TRACE_PLACE(= 4)を足し、probe_tick.hlsl の ApplyPlace が触ったブロックごとに GtRecord(主 = ブロック・従 = 中心のセル)。
  CPU の予想 AppendExpectedProbeTrace は表の物質の数を受け取り(引数を足した)、置くコマンドを DecodePlaceCommand で読み、ProbePlaceBlocks
  (CPU リファレンスの適用と共通にした)を根と予定の種に足す。木のファイルは「置く 中心 (x, y, z)」。gpu_probe_trace に刻み 4 の熱い炭の球
  (半径 2・8 ブロック)を足した(コマンドは刻みの順に並べる。ProbeSim は刻みが戻るコマンドを拒む)。
