# T-1233 反応の共通の道具で、書き換えた配列を動的な添字で読まない(DXIL の局所の配列を減らす)

- Status: Done(2026-10-29 main へ。84511ac8。残りの局所の配列は T-1234・T-1266)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: M? (docs/plan/ROADMAP.md の今の順。T-1223 の判断待ちから)
- 設計: CLAUDE.md §4(書き換えた構造体の配列を動的な添字で読まない)・T-0397 / 決定: D-546(Compute を既定・Work Graphs は測って速い所だけ)

## 目的(1〜2 行)
shaders/common/reaction.hlsli などシミュの共通のコードで、セルの成分(species・amounts)・候補(rules・extents)・規則の反応物を
「成分の数までの回し」の動的な添字で読み書きしている所を、決まった数の回し + `FX_UNROLL` で選ぶ形にし、GPU の局所の配列(DXIL の alloca)を減らす。
**結果はビット単位で同じ**(読み方の形だけを変える。順も同じ)。

## 完了条件(チェックできる形で)
- [ ] release のビルドが通る(警告 0)
- [ ] 全 GPU 試験が通る(CPU リファレンスとのビット一致・記録したハッシュが変わらない)。組に分けて流す
- [ ] `dxil_alloca_check`(ctest)が通る。上限の表 tools/dxil_alloca/limits.txt を今の数で置く(増やさない歯止め)
- [ ] 費用を測る(反応の段・多重解像度の 1 刻み)。docs/perf.md へ
- [ ] `python3 tools/archmap/archmap.py --check`

## 直した所(読み方の形だけ)
- reaction.hlsli:
  - GPU の RxCell / RxUsage の成分の読み書き(RX_SPECIES_AT・RX_AMOUNT_AT・RX_ADD_AMOUNT など)を、`RX_MAX_CELL_SPECIES` 個の決まった回しで
    「添字が一致する欄だけ」を選ぶ・書く道具(RxPickU32/U64・RxPutU32/U64・RxAddAtU64・RxSubAtU64)に。範囲の外の添字は 0 を読み何も書かない(前は未定義)。
  - 成分の回しを `for (i < RX_SPECIES_END(cell)) { if (i >= cell.speciesCount) break; … }` に(GPU の RxCell は定数回で開く。C++ は成分の数まで)。
    速い形 RxGpuMidCell・作業場の形 RxGpuSpaceCell は RxSpeciesLoopEnd が成分の数を返す(32 種を開くと命令が増えすぎる。今までと同じ形)。
  - 候補の欄(RxCandidates の rules・extents)は RX_CANDIDATE_RULE / RX_CANDIDATE_EXTENT / RX_SET_CANDIDATE(_EXTENT) で決まった数から選ぶ・書く。
  - 規則の反応物(rule.reactants・reactantCoefficients)は RX_REACTANT_AT / RX_REACTANT_COEFFICIENT_AT。
  - RxFindSlot は return で抜けず一致を選ぶ(成分の ID は重ならないので答えは同じ)。RxOverdraws は or で畳む。RxComputeRuleRate の factors[2] は 2 つの値に。
- heat_conduction.hlsli(HcCellConductivity・HcCellConductance)・object_heat.hlsli(OhPairRadiation・OhShellReturnable): 成分の回しを同じ形に。
- multires_wide_step.hlsli: 速い形・作業場の形の RxSpeciesLoopEnd と、RxGpuMidCell の成分の道具(配列をそのまま)。
- tools/dxil_alloca/dxil_alloca_check.py + limits.txt(新): release の asm(out/build/release/shaders/asm/sim)の alloca をシェーダーごとに数え、表を超えたら落とす。ctest `dxil_alloca_check`。

## 測った数(DXIL の alloca。release の dxc)
- 共通の 279 シェーダーで 1909 → 1048(wt6 の同じ頃の main 系のビルドと比べた)。大きく減った所: multires_activity_graph 171 → 9・
  multires_step_wide_wait 163 → 88・multires_step_wait 63 → 2・multires_step_wide_expanded_wait 117 → 62・probe_conduct 50 → 4・
  multires_wide_conduct_apply 44 → 1・reaction_cells 33 → 0・object_heat_react 30 → 0・object_shell_flow 24 → 0。
- 増えた所: multires_tree_settle 20 → 27(原因は未確認。multires.hlsli の畳みの道具との組み合わせか)。

## 重なり
- **wt2 T-1188 と shaders/common/reaction.hlsli が重なる**(木の細かくする段・粒の詰まり)。こちらは読み方の形(上の道具とマクロ)だけに絞った。
  マージの時は、wt2 側で増えた `cell.species[i]`・`.amounts[i]`・`candidates.rules[c]` などの直読みを上のマクロに直す。

## 前から落ちている試験(T-1233 のせいではない。2026-10-29 に直す前の土台 da710a72 を release でビルドして同じに落ちるのを確かめた)
- GPU(HW): gpu_multires_implicit(計測: 鎖を測れない)・gpu_multires_implicit_tree / _build(SegFault。GPU の前の CPU 側)・gpu_multires_implicit_conduction_blocks(_chain)(ブロックが入らない上限なのに陽解法に回したブロックが無い)・_levels(_chain)(陰解法の上限は全部の欄が要る)・gpu_multires_near_fold(場面の確認: 頁が全部畳まれる・端数の枠を返す、になっていない)。CPU: standard_packages(line 269 の上書きの検査)・radiation_reference。
- 手がかり: 最後に通ったログは wt2 の 2026-10-21 12:21(runner/logs/20261021-122121-test.log)と wt3 の 10-18。その後にこの組の C++(implicit_conduction・multires_implicit_conduction・gpu_multires_implicit・試験)を触ったのは T-0931(71930728・10-22「セルの頁を成分の数に合わせた幅で持つ・CPU。ADR-0417」)だけ。落ち方(上限を計る CPU の系が空・畳みの場面にならない)は頁の幅の変更と合う。直すのはこのチケットの外(司令塔へ)。

## 残り(このチケットでは触らない。T-1234 の候補)
- multires.hlsli(MrFraction・MrFold の stats・MrSplit の parent など)・cell_view.hlsli・light.hlsli・probe_place.hlsli・object_heat.hlsli の気体の欄(flow.amounts[g] など)
  にも動的な添字の読み書きが残る。physics_*(83・65・62 など)は反応ではなく物理の側。

## メモ・参考
- 前例: ADR-0052 追記 14(開いたら DXIL が増えた)。速い形 RxGpuMidCell を開かないのはそのため。
- `[unroll(N)]` は N 回で打ち切るので使わない(成分の数で抜ける回しを定数回にする)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-27 wt5(前の作業役): reaction.hlsli ほかの読み方を直し、dxil_alloca_check.py を足した。release のビルドは通った。コミット前に区切れた。
- 2026-10-29 wt5: 途中の作業を読み、heat_conduction の行末コメントの折り返しを直し、上限の表 limits.txt を作ってチケットとともに WIP でコミット。
- 2026-10-29 wt5(続き): release のビルド OK(警告 0)。CPU の試験(gpu_ 以外 129 本)を流し始めた: 40 本目まで radiation_reference だけ落ちる
  (FAILED line 394 allZero = 試験の表の物質の放射の吸収の欄。シェーダーと関係ない C++ のパッケージの焼き。wt3・wt4 のログでも同じく落ちている = 前からの失敗)。
  GPU の試験(HW 99 本を 9 本ずつ 10 組。runner/t1233/groups.txt の 1〜10 行目)と費用の 3 本(gpu_object_heat_test --measure-only・
  gpu_multires_test --measure-wide・gpu_object_shell_flow_test --profile)を wt5 のランナーに積んだまま 2 時間で区切った。
- 2026-10-29 wt5(3 人目): HW の GPU 試験 10 組を全部見た(落ちたのは上の 8 本だけ・ほかは全部通った)。da710a72 を release でビルドし 8 本 + standard_packages を流して 全部同じに落ちる = 前からの失敗と確かめた。費用の前後を docs/perf.md へ(上限の種のセル・伝導なしで +45%・反応の段 +11%。T-1234 へ)。

## 引き継ぎメモ(次の作業役・司令塔へ)
- 結果は wt5 の runner/logs の z- で始まる test / run のログ(積んだ順)。全部 Passed なら完了条件の「全 GPU 試験」は HW の分を満たす。
  WARP の 34 本(groups.txt の 11〜15 行目)はまだ積んでいない。
- 費用の比べ: 前の値は runner/t1233/before_object_heat.log・before_multires_wide.log・before_shell_profile.log(同じ作業ツリーで直す前に測った)。
  後の値と並べて docs/perf.md に 1 行(反応の段の ms・多重解像度の ns/セル)。
- **GPU の試験の途中の結果(HW、2026-10-29)**: 組 1(gpu_fixed・gpu_reaction ほか 9 本)・組 4(activity・quiet・sun_column・cascade_light(_wide)・object_heat(_nest)ほか 9 本)は全部通った。
  **陰解法の組が落ちた**: gpu_multires_implicit(計測: 鎖を測れない)・gpu_multires_implicit_tree / _build(CPU 側で SegFault。GPU を使う前)・
  gpu_multires_implicit_conduction_blocks(_chain)/ _levels(_chain)(「ブロックが入らない上限なのに陽解法に回したブロックが無い」「陰解法の上限は全部の欄が要る(T-0178)」)。
  同じ組の _stress・_chain・cheapest は通っている。落ち方は試験の設定・CPU 側に見え、読み方の直しとの関係は未確認(最後に通ったログは wt2 の 2026-10-21)。
  **次にやること**: 同じ試験を直す前のビルド(main か da710a72)で流し、前から落ちているかを確かめる。前からなら本体へ(このチケットの外)、この直しのせいなら heat_conduction.hlsli の 2 か所を戻して切り分ける。
- CPU の試験は --timeout 3600 で 41 本目(probe_gas)で打ち切られた(40 本: radiation_reference・standard_packages の 2 本が落ちる。どちらも C++ のパッケージの焼きで、
  シェーダーと関係ない。standard_packages は「real_matter → combustion_test の上書き」の検査)。残りの 89 本は --timeout を長くして流し直す。
- 残り: `git rebase main`(main に t-1202・t-1223 が入った後)・wt2 T-1188 との reaction.hlsli の重なりの解消(上の「重なり」)・Status を Done に。
- **2026-10-29(3 人目)の引き継ぎ**: t-1233 に戻して release をビルドし直した(警告 0)。wt5 のキューに CPU の残り 86 本(runner/t1233/cpu_groups.txt の 4 組。--timeout 5400)と WARP の 5 組(groups.txt の 11〜15 行目)を積んだ(20261028-201913〜201923 の test)。結果は wt5 の runner/logs のそのログ。 前からの失敗(上の節)を除いて全部通ったら、`git rebase main` → release のビルドと代表の試験(組 1・組 4)→ push。CPU の multires_implicit・multires_implicit_tree(組 4)が落ちたら、上の陰解法の失敗と同じ根(T-0931)と見て前からの失敗に足す(da710a72 で確かめる)。
