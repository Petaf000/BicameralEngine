# T-1266 シミュの共通のコードの残りの局所の配列(T-1233・T-1234 の続き)

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: M? (docs/plan/ROADMAP.md。司令塔が置き場所を決める)
- 設計: CLAUDE.md §4・T-0397・T-1233・T-1234 / 決定: D-546

## 目的(1〜2 行)
T-1233・T-1234 の後も DXIL の alloca が残るシミュの共通のコードを、同じ形(決まった数の回し + `FX_UNROLL` で選ぶ・書く)で直し、
tools/dxil_alloca/limits.txt を下げる。結果はビット単位で同じ・費用は直す前を超えない(D-468)。

## 完了条件(チェックできる形で)
- [ ] release のビルド(警告 0)・float_check・`dxil_alloca_check` を --update した表で通す(下がった数をチケットに)
- [ ] 触ったシェーダーの GPU 試験(HW と WARP)が CPU リファレンスとビット一致・記録したハッシュが変わらない
- [ ] 費用(多重解像度の `gpu_multires_test --measure-wide`・物の熱の `gpu_object_heat_test --measure-only`)が直す前以下。docs/perf.md へ
- [ ] `python3 tools/archmap/archmap.py --check`

## 候補(2026-10-29 の grep・T-1234 のメモから)
- multires.hlsli: MrFraction の species・amounts(MrFractionAmountOf・次の種を探す回し)・MrSplit の parent.species[p]・
  MrFold の stats の差し込み(MrFoldInsertSpecies の詰め直し)・分けた結果の result.cell.species[result.cell.speciesCount] への書き込み
- multires_tree_settle 27(T-1233 で 20 → 27): [8 x i32/i64] 17・[64 x i32/i64] 8・[16 x i32] 2。畳み・MrFraction の動的な添字と見る(未確認)
- cell_view.hlsli(CvAddSpeciesParts の回し)・light.hlsli(吸収の回し)・probe_place.hlsli(置く成分の検査)・multires_table_words.hlsli(セルを語から作る)
- object_heat.hlsli の気体の欄(flow.amounts[g] など。OH_MAX_GASES の決まった回しなら開いてそのままでよいか asm で確かめる)
- main で入った新しいシェーダー(2026-10-29 に表へ足した): multires_graph_place 60(T-1188。multires_graph と同じ数)・phase_layer_cells 6(T-1211)・cascade_light_tree_grid 2(T-1117)
- 多い所(2026-10-29 main 4d60f487 の表): multires_step_wide_wait・multires_graph_wide 82・multires_graph 60・multires_step_wide_expanded_wait・multires_step_wide_big。
  速い形 RxGpuMidCell(32 種)は開くと命令が増えすぎるので動的な添字のまま(ADR-0052 追記 14)。physics_*(83・65・62)は物理の側で別。

## メモ・参考
- 遅くなる形に注意(T-1234 で測った): 回しの中で毎回 RX_MAX_CELL_SPECIES 個・16 個から選ぶ形は、局所の配列が減っても遅くなることがある。
  版ごとにシミュのシェーダーだけ作り直して比べる道具 runner/t1234/ab.ps1(ab_revs.txt)を使い、直す前と交互に測る。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29 wt5: T-1233・T-1234 を main へ入れた時に T-1234 の「残り」から分けた(チケットを作っただけ)。
