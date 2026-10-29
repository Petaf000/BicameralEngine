# T-1234 シミュの共通のコードの残りの局所の配列を減らす(T-1233 の続き)

- Status: Doing
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
- **T-1233 で遅くなった所(2026-10-29 測った。docs/perf.md)**: `gpu_multires_test --measure-wide` の「上限の種のセル・溢れ」伝導なし 932 → 1347 ns/セル(+45%)・伝導あり 139 → 155(+11%)、`gpu_object_heat_test --measure-only` の反応の段 0.81 → 0.90 ms(+11%)。成分の多いセルで、決まった数の回しの中で毎回 RX_MAX_CELL_SPECIES 個から選ぶ形が重いと見る(未確認)。選ぶ回数を減らす(回しの外で 1 度だけ局所の値に写す・同じ i の読みをまとめる)形を先に見る。負荷で遅れるのは不具合(D-468)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29 wt5(t-1234-sim-no-local-array-rest。t-1233 の上): 遅くなった所の見立て: (1) RxFindSlot が一致で抜けず全部回す形になった(32 種の速い形で回しが約 2 倍)・
  (2) RxReplaceLastCandidate(候補が 16 を超える時の置き換え)が動的な添字 last で候補の欄を選び直し、1 回の置き換えで 16 × 16 の選び・
  (3) 候補の回しの中で同じ c の欄を何度も選ぶ・RxRevokeRoundUps が pick で選び直す。直し(reaction.hlsli だけ・読み方の形だけ・順も答えも同じ):
  (1) 一致したら break・(2) 回しを FX_UNROLL で開き最も後の候補の規則を値で持つ・(3) 候補の規則と進行度を 1 回ずつ選んで値で持つ(RxSumUsage・RxShrinkToDemand・RxApplyExtentsChecked・RxRevokeRoundUps)。
- 2026-10-29 wt5(続き): 上の直しを ec2b01ec でコミット・push(release のビルドは未確認・試験も費用もまだ)。t-1233 の WARP の組 11 は 2 本だけ落ちた
  (gpu_multires_implicit_tree_warp・_build_warp。HW と同じ前からの陰解法の失敗の根〔T-0931〕)。CPU の組 2 は support_fracture_settle で --timeout 5400 に当たって打ち切り(HLSL だけの変更なので見なくてよい)。入れ物の再起動で区切った。

## 引き継ぎメモ(2026-10-29 wt5・T-1234)
- ブランチ t-1234-sim-no-local-array-rest(t-1233 の上。ec2b01ec の後に このメモのコミット)。直しは reaction.hlsli だけ(作業ログの (1)〜(3))。**ビルド・試験・費用はまだ**。
- wt5 のキューに積んだまま(名前の順に流れる): t-1233 の WARP 201922・201923 の後に、20261028-224435-build(release)→ 224621-test(dxil_alloca_check・float_check_*)→
  224728-run(gpu_object_heat_test --no-gbv --measure-only)→ 224810-run(gpu_multires_test --no-gbv --measure-wide)→ 224832-run(gpu_object_shell_flow_test --profile)→
  HW の組 1・2・4・5(224933・224954・225053・225149)→ WARP の組 11(225301。groups は runner/t1233/groups.txt)。
- 次の一手: 上のログで (a) ビルドが警告 0・(b) alloca が limits.txt を超えない(減ったら --update で下げる)・(c) 費用を runner/t1233/before_*.log と比べ、
  上限の種のセル 伝導なし 932・あり 139 ns/セル、反応の段 0.809 ms を超えないか(D-468)・(d) HW の組がハッシュを変えずに通る(前からの 8 本を除く)。
- まだ遅い時の候補: RxCollectCandidatesWait の RX_SET_CANDIDATE(動的な count への書き込みで 16 × 3 の選び。候補 1 つごと)・RxCompactCell と RxTryAddSpecies の
  動的な位置への RX_SET_ENTRY(RxCell 8 種。物の熱の段に効く見立て)。候補の回しを FX_UNROLL で開く案は命令が 16 倍になるので測ってから(ADR-0052 追記 14)。
- 残り(遅さを直した後): multires_tree_settle 27 の alloca は [8 x i32/i64] 17・[64 x i32/i64] 8・[16 x i32] 2(multires.hlsli の畳み・MrFraction の成分の動的な添字と見る。未確認)。
