# T-1233 反応の共通の道具で、書き換えた配列を動的な添字で読まない(DXIL の局所の配列を減らす)

- Status: Doing
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

## 残り(このチケットでは触らない。T-1234 の候補)
- multires.hlsli(MrFraction・MrFold の stats・MrSplit の parent など)・cell_view.hlsli・light.hlsli・probe_place.hlsli・object_heat.hlsli の気体の欄(flow.amounts[g] など)
  にも動的な添字の読み書きが残る。physics_*(83・65・62 など)は反応ではなく物理の側。

## メモ・参考
- 前例: ADR-0052 追記 14(開いたら DXIL が増えた)。速い形 RxGpuMidCell を開かないのはそのため。
- `[unroll(N)]` は N 回で打ち切るので使わない(成分の数で抜ける回しを定数回にする)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-27 wt5(前の作業役): reaction.hlsli ほかの読み方を直し、dxil_alloca_check.py を足した。release のビルドは通った。コミット前に区切れた。
- 2026-10-29 wt5: 途中の作業を読み、heat_conduction の行末コメントの折り返しを直し、上限の表 limits.txt を作ってチケットとともに WIP でコミット。
