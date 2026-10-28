# T-1234 シミュの共通のコードの残りの局所の配列を減らす(T-1233 の続き)

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: M? (docs/plan/ROADMAP.md の今の順。司令塔が置き場所を決める)
- 設計: CLAUDE.md §4・T-0397・T-1233 / 決定: D-546

## 目的(1〜2 行)
T-1233 で反応の道具(reaction.hlsli)の読み方を直した後も、DXIL の alloca が残るシェーダーの元を、同じ形(決まった数の回し + `FX_UNROLL` で選ぶ・書く)で直す。
結果はビット単位で同じ。tools/dxil_alloca/limits.txt を下げる。

## 完了条件(チェックできる形で)
- [ ] release のビルド(警告 0)・全 GPU 試験(ハッシュが変わらない)
- [ ] `dxil_alloca_check` を --update で下げた表で通す
- [ ] 費用(docs/perf.md)

## メモ・参考(T-1233 の後に残っていた所。2026-10-29 の grep)
- multires.hlsli: MrFraction の species・amounts(MrFractionAmountOf・次の種を探す回し)・MrSplit の parent.species[p]・MrFold の stats の差し込み(MrFoldInsertSpecies の詰め直し)・
  分けた結果の result.cell.species[result.cell.speciesCount] への書き込み
- cell_view.hlsli(CvAddSpeciesParts の回し)・light.hlsli(吸収の回し)・probe_place.hlsli(置く成分の検査)・multires_table_words.hlsli(セルを語から作る)
- object_heat.hlsli の気体の欄(flow.amounts[g] など。OH_MAX_GASES の回し。決まった数ならそのままでよいか、asm で確かめる)
- alloca の多いシェーダー(T-1233 の後): multires_step_wide_wait 88・multires_graph_wide 82・multires_step_wide_expanded_wait 62・multires_graph 60・multires_step_wide_big 47
  (速い形 RxGpuMidCell は 32 種を開くと命令が増えすぎるので動的な添字のまま。ADR-0052 追記 14)。physics_*(83・65・62)は物理の側で別。
- T-1233 で増えた multires_tree_settle 20 → 27 の原因を見る。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
