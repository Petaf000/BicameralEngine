# T-1154 仮の世界の GPU(ProbeSim)の刻みに熱の放射の段(RC)を載せる

- Status: Todo
- 種類: 工学
- PC: 必須
- マイルストーン: 列 4(T-1113 から分けた)
- 設計: docs/design/23-radiation.md §3.3・§3.5 / 決定: D-646・D-658・ADR-0432

## 目的
CPU リファレンス(ProbeReference の (1d)。sim/probe_reference_radiation.cpp)と毎刻みビット一致する放射の段を GPU の仮の世界に入れ、既定にする。

## 完了条件
- [ ] 刻みの初めのセル → 光の値・温度(CascadeGrid の写し)を GPU で作る段・RC の段(cascade_light.hlsl)・吸った − 放ったを最も近い mJ で当てる段(端数と外へ出た分は表へ)・持ち越しをハッシュへ
- [ ] gpu_probe_* 8 本・gpu_world_copy・遊びの指標の CPU リファレンス(play_metric_measure.cpp)の `SetCascadeRadiation({.enabled = false})` を外して一致
- [ ] 費用(RTX 3070 Ti・64³。仮の線 1 ms)を docs/perf.md に

## メモ
- GpuCascadeLight は多重解像度のルート署名(u4・u5)を借りている。仮の世界のルート署名は 61/64 語(UAV 20)なので、載せ方(別のルート署名で同じリストに記録・既存の u を共有)を先に決める。
- 変わったセルのブロックを予定の種にし、変わった印を「刻みの直前」にする(CPU と同じ)。
