# T-1164 仮の世界の放射の段の「世界の外」を参照の温度にする(既定にした・GPU の 2 側も)

- Status: Doing(2026-10-26 wt6 第 2 回: 既定 = 環境の温度の物〔D-671〕・いつも流す〔D-670〕・GPU の正と負の 2 側は済み。**window_replay_play が落ちる**〔下の「落ちている試験」〕のが残り)
- 種類: 工学
- PC: 必須
- マイルストーン: 列 4(T-1155 の「世界の外の扱い」から分けた)
- 設計: docs/design/23-radiation.md §3.2・§3.3 / 決定: **ADR-0478(Accepted)**・D-670・D-671・D-676・ADR-0461・ADR-0474・D-468

## 目的
世界の外が 0 K の空なので、室温の面のブロックが毎刻み起きて眠らない(T-1154 の残り)。外を参照の温度の物にし、釣り合った世界を眠らせる。

## 形(2026-10-26 wt6)
- `ProbeCascadeOptions::params.referenceMk`(0 = 今の 0 K の空・既定)。0 でなければ ProbeReference::StepCascadeRadiation が
  StepCascadeLightRelative(正の側と負の側)を流し、セルの正味 = 正(吸った − 放った)− 負(吸った − 放った)。負の側の持ち越しは別に持つ
  (CascadeRadiationCarryNegative。状態のハッシュは面の番号 | 8 で正の側と分けて足す)。報告の escaped は 正 − 負(mod 2^64)。
- 保存: Σ セルのエネルギー × 2^16 + Σ 正の持ち越し − Σ 負の持ち越し + Σ(外へ 正 − 負)+ Σ 丸め − Σ つつき × 2^16 が毎刻み一定(probe_radiation の 6)。
- GPU の仮の世界(ProbeSim)は参照の温度の形を作る時に断る(「T-1164」の誤り)。
- RcFaceEmissionRelative(共通の hlsli)は、参照と同じ温度の面を黒体の量を求めずに 0 で抜く(結果は同じ値。釣り合った世界の CPU の費用 790 → 下の測り)。

## 既定にした形(2026-10-26 wt6 第 2 回。D-670・D-671・D-676)
- **いつも流す(D-670)**: CPU(StepCascadeRadiation)・GPU(CreateRadiation)とも ProbeTableWritesLight の条件を外した。物質の無い表(speciesLight が空)だけ流さない(流しても同じ結果)。
  ProbeTableWritesLight は残す(書き忘れの警告・試験用)。
- **外 = 環境の温度の物(D-671)**: `ProbeCascadeOptions::environmentReference`(既定 true)なら参照の温度 = **作った時の表の基準の大気の高さ 0 の温度**
  (ProbeCascadeReferenceMk。SampleAtmosphere(…, 0))。基準の大気を書かない表は 0 = 0 K の空(ADR-0478 の決定 3)。false なら params.referenceMk のまま(試験・測り)。
  表の差し替えでは参照の温度を変えない(CPU・GPU とも作った時の表。ProbeReference::m_environmentMk)。
- **GPU の 2 側**: 参照の温度が 0 でなければ GpuCascadeLight を 2 つ(m_cascade = 正・m_cascadeNegative = 負。作業と状態の並びが同じことを作る時に確かめる)。
  RadiationGrid は両側の作業へ同じ光の値と温度を書き(正の側の参照の語が 0 でない時)、段は側ごとに 1 刻み、RadiationApply は
  正(吸った − 放った)− 負(吸った − 放った)を (正.吸った + 負.放った) − (正.放った + 負.吸った) の形で 32bit の上下に分けて引き、繰り上がりも手で回して丸める
  (T-1078・T-1159 の約束)。RadiationHash は側ごとに 1 回(負の側は面 | 8。CPU と同じ)。保存点・写しの早送りは負の側の状態の頭も写す。置き場は 265 MiB × 2。
- **空との交換の口**: 仮の世界にはつないでいない。空の値(天候の層)を書く口がまだ無く、空 = 参照の温度なので口は何もしない(ADR-0478 の決定 2)。
  口の向きは毎刻み 4 方向すべて(D-676): SkyPortParams::rotateDirections の既定は false のまま(sky_port.h で確かめた。sky_port の試験が通る)。
- **試験の表の環境は 288.15 K**(combustion_test → earth_environment の ISA)、初めの世界は 300 K。なので既定の試験の世界は「環境より 11.85 K 暖かい世界」になり、
  何もしない世界も外へ冷え続けて眠らない(正しい振る舞い)。300 K の環境にした表では何もしない世界が全部眠る(probe_radiation の 7)。

## 測り(2026-10-26 wt6・CPU・release。`probe_radiation_test --measure-outside 300`。300 刻みの平均)
| 場面 | 外 | 予定したブロック(平均・最後) | 放射で変わったセル/刻み | 光線/刻み | CPU ms/刻み |
|---|---|---|---|---|---|
| 何もしない 300 K の世界(空気の中の木箱) | 0 K の空 | 32.0・32 | 296 | 21,826 | 206 |
| 同 | 参照の温度 300 K | **0・0** | 0 | 0 | 790(同じ温度を抜く前。抜いた後は下の追記) |
| 木箱の壁に火 | 0 K の空 | 75.9・80 | 296 | 22,146 | 177 |
| 同 | 参照の温度 300 K | 75.5・80 | 217 | 21,056 | 1,450(同上) |
| 何もしない世界・放射なし | — | 0・0 | — | — | 13.5 |
- 同じ温度の面を先に抜いた後(120 刻みの平均・同じ日): 何もしない世界 0 K の空 187 ms・**参照の温度 52 ms**(放射なし 13.7 ms)/ 火の世界 0 K の空 177 ms・**参照の温度 87 ms**。
  参照の温度の形の方が CPU では安い(光源が参照と違う所だけ)。GPU は未測(残りの 1 つ目)。
- 火の世界の起きたブロックは火そのもの(気体・反応)で決まり、外の扱いではほぼ変わらない。変わるのは「何もしない所」。

## 残り
- [x] GPU の仮の世界の 2 側(上の「既定にした形」)。負の側の置き場を小さくする(参照より冷たい物だけ)のは測ってから。
- [x] 既定にする(D-671)。
- [ ] 空との交換の口を仮の世界につなぐ(口は木〔MultiresNest〕を読むので、仮の世界では 64³ の写しを読む形が要る。空の値を書く口〔天候の層・05〕ができた時に。
  空 = 参照なら何もしないので今の既定の世界では不要)。
- [ ] 夜の地面(空が参照より冷たい)の測り: 木の世界(T-1155)で口とつなぐ時に。
- 土台に足りないもの: 口と RC の吸った分を当てる段・帳簿の行(ADR-0469 の芯 4。T-1161)。今は仮の世界の段が直接足す(T-1113 と同じ)。

## 判断待ち(仮で進めたもの・司令塔へ)
- 【エンジン】**参照の温度を基準の大気の「高さ 0」で取る**(仮): 仮の世界(32 m の高さ)は 1 つの参照の温度しか持たないので、床 = 高さ 0 の温度にした
  (ISA なら箱の上と下で 0.2 K 違う)。作り手から見て: 高い所に置いた世界(山の上)でも外は海面の温度。気体の区画の floorHeightMillimeters に合わせる形もある(値 1 つ)。
- 【エンジン】**基準の大気を書かない世界は 0 K の空のまま**(ADR-0478 の決定 3 のとおり): C++ の試験の表など。作り手から見て: 環境を書き忘れた世界は外へ冷え続ける
  (書けば眠る)。書き忘れをベイクで警告する形は足していない。
- **試験のパッケージ combustion_test の初めの世界(300 K)と環境(earth_environment の ISA 288.15 K)が合っていない**: 窓の既定の世界(サンプルの世界)は
  何もしなくても木箱・地面が 288.15 K の外へ冷え続け、眠らない(放射の段の費用が毎刻みかかる。D-468)。どちらかを揃えるのはパッケージの値(公開の試験データ。D-006 の外)。
  おすすめ: combustion_test で地面の点を 300 K に set_point する(気体の区画の基準も 300 K になる。probe_gas などは表を自分で作っているので影響は小さい見込み・未確認)。この作業役では変えていない。
- 置き場: GPU の仮の世界の放射の段が 265 MiB × 2 側 = 531 MiB(負の側は参照より冷たい物だけなので小さくできる見込み。測ってから)。
- 費用: gpu_play_metric が 41 秒(T-1113 の前)→ 203 秒、gpu_probe_gas は 300 秒の上限に当たった(CPU リファレンスが 2 側を流す・試験の表の環境 300 K)。gpu_probe_gas の上限を 900 秒にした。
  GPU の 1 刻みの費用(2 側)は未測(perf.md の T-1154 の行 0.55 ms は 1 側)。

## 落ちている試験(2026-10-26 wt6・引き継ぎ)
- **window_replay_play**(窓の記録 → 重さを分けた単位〔--sim-load 400000 --sim-split 3 --target-fps 30〕で再生): 2 回とも **S(33) から**ハッシュが記録と違う(一致 32 / 不一致 80)。
  S(1)〜S(32) は一致。記録のハッシュは回ごとに違う(クリックがフレームで入るため)が、ずれ始めは同じ刻み。window_replay_record・window_blank_record/play は通る。
  - main(T-1154 で GPU に放射を入れた後)で通るかは**未確認**(10-23〜26 のどの作業ツリーのログにも window_replay_play を流した跡が無い)。
    司令塔: main で `window_replay_record|window_replay_play` を流して、T-1154 からか T-1164(2 側)からかを分けてほしい。
  - **GPU の非決定**: 同じ記録を 3 回再生(window_replay_play・window_replay_rewind・window_replay_peek)すると、S(33) のハッシュが 3 回とも互いに違う
    (8e226e7b…・7f0d1e4a…・94611c61…)。CPU と比べる gpu_probe_* は通るので、窓のサンプルの世界(物理の積み木・自動クリック)だけで出る競合。
    release では GPU の DEBUG_ASSERT が消えるので、放射の置き場の溢れ(ProbeRadiationOverflow。溢れると原子的な取り合いの順で結果が変わる)が見えない。疑いの 1 番目。
  - 見立て(未確認): 2 つ目の自動クリック(フレーム 20)のつつきが入る刻みの辺り。サンプルの世界には物理(積み木)があり、物理 + 放射を一緒に GPU と CPU で比べる試験が無い
    (gpu_probe_physics は放射を切って比べる)。重さを分けた単位でフレームの境が刻みの途中に来る時の、放射の段の結び直し(u15〜u17)か保存点の写しの順を疑う。
  - gpu_probe_sim・gpu_probe_sim_tables・gpu_probe_rewind(_regions)・gpu_world_copy・gpu_probe_place・gpu_probe_peek・gpu_probe_gas・gpu_probe_liquid・gpu_play_metric は CPU とビット一致(2 側の形)。

## 取り直しの表(2026-10-26 wt6 第 2 回。既定を変えて値の変わった試験)
| 試験 | 前の基準 | 新しい基準 | なぜ変わったか |
|---|---|---|---|
| probe_radiation の 1(TestNoLight) | 光の値を書かない表では段が流れず、段を切った世界と毎刻み同じハッシュ | 段が毎刻み流れ・保存が一定・0 K 以下のセル 0・段を切った世界とハッシュが違う | D-670(いつも流す。書かない木箱は D-610 の灰色で放つ) |
| probe_radiation の 1〜5(TestDefault) | 外 = 0 K の空 | 外 = 試験の表の環境 288.15 K(関係の判定〔保存・決定的・向かいの壁が熱い〕は同じ。値の判定は無い) | D-671 |
| probe_radiation の 6(TestOutside) | 既定の値(0 K の空)と referenceMk = 300 K | 決めた値として渡す(environmentReference = false)。判定は同じ | 既定が環境の温度になったので、決めた値の形を明示 |
| probe_radiation の 7(新) | — | 試験の表の参照の温度 = 基準の大気の高さ 0(288.15 K)・書かない表は 0・**環境 300 K の表で何もしない 300 K の世界は既定のまま全部眠る**(変わったセル 0・光線 0・予定したブロックが放射なしと毎刻み同じ)・火の世界の保存と決定性・決めた値 300 K とハッシュが同じ | D-671 の確かめ |
| probe_gas の 3(燃える木箱の保存。8 か所) | 放射の段を切らずに比べていた(T-1113 で段が既定になってから、段の出入り〔外へ・持ち越し〕がこの和に入らず落ちる。**main でも落ちる見込み・未確認**: T-1113 の合格の一覧に probe_gas が無い) | 燃える木箱の世界(RunFire・断面の表示)だけ放射の段を切る(physics_heat と同じ。放射の保存は probe_radiation が見る) | T-1113(既定にした時の取り残し)。今回 D-670 で全部の世界に効くので直した |
- **値を書いた基準で変わったものは無い**: reaction_package_scene・reaction_replay_table・blank_world・physics_heat・probe_liquid・rest_return・probe_phase_porous_stone・play_metric・slope_check・reaction_species_remap は直さずに通った
  (試験の表は前から光の値を書いていて段が流れていた。外の温度だけが 0 K → 288.15 K に変わり、判定は関係か段を切った比べ)。
- GPU と CPU を毎刻み比べる試験(gpu_probe_* など)は、既定が両方で同じに変わるので基準は無い(2 側の形のビット一致の確かめになる)。

## 作業ログ
- 2026-10-26(wt6 第 2 回): いつも流す(D-670)・外 = 環境の温度の物(D-671)・GPU の 2 側。CPU: probe_radiation(7 を足した)・reaction_package_scene・reaction_replay_table・blank_world・physics_heat・
  probe_liquid・rest_return・probe_phase_porous_stone・play_metric・slope_check・reaction_species_remap・replay_file・cascade_light・cascade_light_swing・sky_port・cascade_tree_grid が通った。
  probe_gas は保存の取り残しを直した(上の表)。GPU: gpu_probe_sim・gpu_probe_place・gpu_probe_rewind_regions・gpu_world_copy・gpu_cascade_light・gpu_probe_blank が CPU とビット一致
  (試験の表の参照の温度 288.15 K = 2 側の形)。release・警告 0・archmap --check OK。
- 2026-10-26(wt6 第 2 回の続き): gpu_probe_sim_tables・gpu_probe_rewind・gpu_play_metric(203 秒)・gpu_probe_peek・gpu_probe_liquid・gpu_probe_gas(上限を 900 秒に。294 秒)・probe_gas が通った。
  main へ rebase(82453cf5)して probe_radiation・gpu_probe_sim・gpu_probe_gas・window_replay_record・window_blank_record/play が通り、**window_replay_play が S(33) から落ちる**(上)。
  T-1161(芯 4)は手を付けていない(時間切れ)。
- 2026-10-26(wt6): 試験 cascade_tree_grid・probe_radiation・cascade_light・cascade_light_swing・sky_port・gpu_cascade_light・gpu_sun_column・gpu_probe_sim が通った(release・警告 0)。
- 2026-10-26(wt6): CPU リファレンスに参照の温度の形・probe_radiation の 6(眠る・保存・決定的)・--measure-outside。ADR-0478(Proposed)。
