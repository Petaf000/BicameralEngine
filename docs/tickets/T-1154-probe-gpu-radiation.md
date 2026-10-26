# T-1154 仮の世界の GPU(ProbeSim)の刻みに熱の放射の段(RC)を載せる

- Status: Done(2026-10-26 wt6・ブランチ t-1113-radiation-all)
- 種類: 工学
- PC: 必須
- マイルストーン: 列 4(T-1113 から分けた)
- 設計: docs/design/23-radiation.md §3.3・§3.5 / 決定: D-646・D-658・ADR-0432

## 目的
CPU リファレンス(ProbeReference の (1d)。sim/probe_reference_radiation.cpp)と毎刻みビット一致する放射の段を GPU の仮の世界に入れ、既定にする。

## 完了条件
- [x] 刻みの初めのセル → 光の値・温度(CascadeGrid の写し)を GPU で作る段(RadiationGrid)・RC の段(cascade_light.hlsl)・吸った − 放ったを最も近い mJ で当てる段(RadiationApply)・持ち越しをハッシュへ(RadiationHash)
- [x] gpu_probe_* 8 本・gpu_world_copy・遊びの指標の CPU リファレンス(play_metric_measure.cpp)の `SetCascadeRadiation({.enabled = false})` を外して一致
  (gpu_probe_trace・gpu_probe_physics だけは GPU・CPU とも切って比べる。下の「切った試験」)
- [x] 費用(RTX 3070 Ti・64³。仮の線 1 ms)を docs/perf.md に

## 形(2026-10-26 wt6)
- **載せ方 = 別のルート署名で同じリストに記録 + 既存の u を段の間だけ借りる**(メモの 2 案を両方使う):
  - 距離ごとの段の 12 のカーネルは、同じ .cso を `GpuCascadeLight::CreateStandalone` の自分のルート署名(u0〜u5 + 定数 26 語 + デバッグのリング。
    木のルート署名の頭と同じ並び)で動かす。仮の世界のルート署名(61/64 語)は増やさない。
  - 仮の世界の側の 3 段(shaders/sim/probe_radiation.hlsl)は仮の世界のルート署名のまま、u15〜u17(気体の区画の結び先。気体の段は済んでいる)に
    段の作業・状態・物質の光の値を結び、段の後で BindRootViews が結び直す。
- 持ち越しの表は刻みの偶奇で選ぶ(刻み t は表 t & 1 を読み、表 1 − (t & 1) へ書く)ので、CPU に刻みをまたぐ状態が無い。
  保存点・写しの早送りは状態のバッファの頭(帳簿・放った・吸った・持ち越しの表 2 つ。約 12 MiB)だけを StateRegions で写す。
- **持ち越しのハッシュの形を変えた**: (セル・面・帯・量)ごとの寄与(ProbeCascadeCarryHashTerm。probe_region_hash.hlsli)の和。
  前の HashCascadeCarry(並びの順に混ぜる)は GPU の順不同の表から作れない。CPU の ProbeReference::StateHash も同じ和にした(持ち越しが空なら今までと同じ値)。
  HashCascadeCarry(多重解像度の試験が使う)はそのまま。
- 段の順は CPU と同じ(適用の単位: コマンド → 物理の熱 → 表の作り直し → 気体 → 液体 → **放射** → 起こす刻みの来たブロックを一覧へ)。
  変わったセルは熱のキャッシュを作り直し、ブロックを「刻みの直前に変わった」+ 起こす刻みを今に(気体の GasScatter と同じ)。
- 段を作るのは表が光の値を書いた時だけ(ProbeTableWritesLight。CPU と同じ仮)。ProbeSimOptions::cascadeRadiation(既定は入れる)で切れる。
  表の差し替えでは光の値を新しい表のものにするが、流す・流さないは作った時の表のまま(光の値を書かない表へ差し替えると CPU と違う。警告を出す)。
- 置き場: 放つ面 2^17・袋 2^20・持ち越し 2^19(状態 265 MiB。溢れたら RadiationHash の assert ProbeRadiationOverflow)。

## 切った試験(GPU・CPU とも放射なしで比べる)
- gpu_probe_trace: トレースの予想(sim/probe_trace の AppendExpectedProbeTrace)がつつき・置くの根だけを数え、放射が起こすブロックを数えない。
  放射を入れると室温の世界でも 0 K の空へ冷える面のブロックが毎刻み起きる(GPU のトレース 3251 件 / 予想 966 件)。予想に放射の根を足すのは残り(下)。
- gpu_probe_physics: 「GPU の世界のエネルギーの増え = つつき + 物理の熱」を見る試験(CPU の physics_heat と同じく放射を切る)。

## 費用(RTX 3070 Ti・release・64³)
- 遊びの指標の早送り(木箱の壁に火・物理なし)の 1 刻みの GPU 時間: 放射なし 152.7 µs(main)→ 放射あり(下の perf.md の行)。

## 残り(司令塔へ)
- 【エンジン】世界の外は 0 K の空(T-1113 の仮)なので、室温の地面・木箱の外の面は毎刻み少しずつ冷え、そのブロックは眠らない
  (作り手から見て: 何もしない世界でも面のブロックが毎刻み計算される。費用は眠りの効き〔D-468〕に効く)。空との交換の口(ADR-0474)か参照の温度(ADR-0461・T-1102)で
  「同じ温度なら放たない」にする形は T-1155(世界の外の扱い)で決める。
- トレースの予想に放射の根(変わったセルのブロック)を足す(ProbeReference が段ごとの根を出し、AppendExpectedProbeTrace が読む)。
- 帳簿: GPU は丸めの端数・外へ出た分を数えていない(CPU の ProbeCascadeReport だけ)。窓の計器に出す時に足す。

## 作業ログ
- 2026-10-26(wt6): GpuCascadeLight に自分のルート署名の形(CreateStandalone)を足し、仮の世界の適用の単位に放射の 3 段 + RC を載せた。
  持ち越しのハッシュを寄与の和にし、GPU と比べる試験の放射を切っていた所を外した。gpu_probe_sim・blank・place・peek・gas・liquid・world_copy・play_metric が CPU とビット一致。
  trace・physics は放射を切って比べる。
