# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-10-09 / チケット: T-0187 多重解像度の世界(CPU)のセルに溢れ — 完了(影の引き戻し・畳む・覗き窓の溢れは T-0199 に分けた)

## 状態(3 行以内)
- CPU の多重解像度の世界は `EnableWideCells(nest)` で「インライン 8 + 頁ごと・端数の枠ごとの溢れ」(MultiresOverflowArea。セルの番号の順に詰める)を持つ。既定は使わない(GPU と比べるテストは今のまま)。
- 溢れを使う世界は 9 種目の生成物を待たせず(待たせた 0・最大 9 種)、8 種ずつ違う子を粗くするのも断らない(親のセル 16 種)。保存は毎刻みビット単位。上限に当たらない本物の鎖は使わない世界と毎刻みビット一致。
- t-0139・t-0178・t-0184 を合わせた main は release の 2 束(46 本・41 本)が通過(直したものなし)。gpu_multires の束は変更の後に流した(結果は「動いているもの」)。次は T-0176(GPU)か T-0199。

## 並走で入ったもの(続きが終わるまで残す。詳しくは各チケット)
- **陰解法の熱(wt2。T-0110 → T-0117 → T-0119 → T-0120 → T-0127 → T-0129 → T-0134 → T-0135〔一部〕→ T-0136。T-0154・T-0132 済み。T-0178 済み。次は T-0179・T-0147・T-0137)**: T-0178 で系に入るブロックを枠の順に選び、入らないブロックはその刻みだけ陽解法(MultiresStepOptions::implicitLimits・implicitOverflow。仮で A)。GPU の EnableImplicitConduction は上限の全部の欄が要る(0 なら失敗)。伝導の作業場の印の空き語はもう無い。 T-0132 で伝導の段から陰解法を呼ぶ(MultiresStepOptions::implicitConduction が GPU でも効く。AddConductDelta は multires_bindings.hlsli・stepFlags のビット 3・5〜7・implicitMaxGap は 1〜8)。系が上限(GpuMultiresImplicitLimits)を超えると CPU と合わない(T-0178)。 T-0154 で記録の形を前の刻みの GPU の数から選ぶ(GpuImplicit::ShapeFrom・IM_PLAN_WANTED_TAIL。T-0132 でも使う)。 T-0136 で GpuImplicit は上限(GpuImplicitLimits)から作り、刻みの初めに GPU の ImPlanLevels・ImPlanArgs が節の並び・ImTail の境・ExecuteIndirect の引数を作る(V サイクルは dispatchLevels 既定 8 まで間接で積む。CPU は段の数を持たない)。
  CPU の方式②(engine/src/sim/implicit_conduction.*・multires_implicit_conduction.cpp。options.implicitConduction 既定 false)と GPU の解く側
  (gpu_implicit.*・implicit_conduct.hlsl)・系を作る側(gpu_implicit_build.*・implicit_build.hlsl)・多重格子の段(gpu_implicit_levels.*・implicit_levels.hlsl)。
  どれも CPU と番号まで毎刻みビット一致。決めたことは ADR-0019 の追記。動かし方: `-Filter "^(multires_implicit(_tree)?|gpu_multires_implicit(_tree|_build)?(_warp)?)$"`、
  計測は `job.py run -Preset release -Exe gpu_multires_implicit_build_test -- --queue compute --measure-only`(_tree_test・_test も同じ)。
  注意: GpuImplicit はまだ CPU の系で Create する(節の並び・ImTail も CPU から。T-0135)。GpuImplicitLevels の RecordCopyTo・RecordReadback は RecordBuild と同じリストで。
  多重格子の段の費用は上限 64 で 1.6〜2.7 ms(空の回 948 Dispatch の固定費。T-0135)。判断待ち(T-0119): 1 刻みより速く落ち着く細かいむら・基準 + 8 段より細かい所(どちらもおすすめ A このまま)。
  release の WARP でも C4189(implicit_conduction.cpp の 'added'。FX_ASSERT の中だけで使う)が出る(wt2 の範囲なので触っていない)。
- **Luau と反応表(T-0020・T-0138・T-0021〔一部〕・T-0157・T-0140・T-0139 済み → T-0169・T-0170・T-0193〜T-0195)**: T-0139 でホットリロード(`--editor` だけ・PROBE_COMMAND_TYPE_TABLE・表を差し替えると保存点は全部捨てる・持っていない表の版の記録は再生で止まる。ADR-0047)。 T-0140 で型検査(script::LuauTypeChecker・data/types/bicameral.d.luau)。**読み手(reaction_package.cpp・luau_package.cpp)の欄を変えたら bicameral.d.luau も変える**(変えないと本体のパッケージが落ちて起動が止まる)。 T-0157 で script::LoadReactionTable(engine/src/script/reaction_table_loader.*)を frame_loop が窓の前に呼ぶ。試験の表は data/packages/combustion_test(ビルドで bin/data に写る。`--packages <dir>`)。読めない・検査で落ちたら終了コード 1。gpu_* と multires_* のテストは今も C++ の表(中身はビットで同じ。T-0169)。ADR-0033。 T-0021 で engine/src/script/reaction_package.*(Luau の elements・species・reactions を整数だけで読む。A は 10 進の文字列でも誤差なし)・ベイクの検査・名前のバイト順の ID(ADR-0032)。試験の表(今は data/packages/combustion_test)は C++ の表と 2 か所にある(片方を変えたら両方。テストが食い違いを落とす)。 パッケージ(engine/src/script/luau_package.*・script_value.*。ADR-0031・13 §2.2・15 §4。合わせ方は仮で C〔QUESTIONS Q8〕)。 殻(CPU だけ。engine/src/script/luau_sandbox.*)。ADR-0030。vcpkg.json に luau、CMakePresets.json の環境に XDG_CONFIG_HOME・GIT_CONFIG_GLOBAL。
  Luau のヘッダは pch.h に入れていない。ログは Channel::Tool。次: T-0138 → T-0021 → T-0139。
- **エディタの殻(T-0023・T-0143・T-0142 済み → T-0172・T-0173)**: T-0142 で実験室(8³ の閉じた箱・FILL/TEMPERATURE のコマンド〔shaders/common/lab_box.hlsli〕・LabSession が GPU と CPU を刻みごとに比べる・BLAB の記録・`--auto-lab`。ADR-0037)。 T-0143 で保存点(sim/probe_save_point.cpp・ADR-0036。GPU → GPU のコピー)+ 巻き戻し(`--save-points`・`--save-interval`・`--auto-rewind`・ReplayPlayer::Rewind)と「Work Graphs と性能」のパネル。記録中・--check-physics・トレース中は巻き戻せない。 `bicameral --editor`(パネル「時間」「状態」。--auto-time は人がいない確認用)。ImGui は editor_overlay.cpp だけ(pch.h に入れない)。
  時間の操作は世界に入らない(ADR-0035)。4 つのランナーが同じ GPU を使っている間は debug 版の最初のフレームが 2〜4 分かかることがある。
- **スクリーンショット(T-0025 済み)**: `--screenshot-tick t`(刻み t で世界を止めて写す)・`--auto-ignite`・tools/image_compare/image_compare.py(8/255 を超える画素が 0.1% 超で失敗)・基準 tests/images/*.png・置き換え方は 16 §5。画像のテストは debug で 1 本約 2 分。

- **気体(T-0026 G1・T-0184 G2 済み → T-0185・T-0186)**: G2 で成分の MUSCL(既定 MC)と移す物質量の乱数の丸め(07 §2.2・ADR-0043 §6・§7)。1 セルの物質量は 2^31 µmol 未満(assert)。 1 レベルの CPU リファレンス engine/src/sim/gas_reference.*(ライブラリ bicameral_gas)・tests/gas_reference_test.cpp(debug 74 秒)。07 §2.1・ADR-0043(Proposed)。c̃ 30 m/s は仮(Q11)。05 のセルの形はまだ変えていない(G3)。tidy の新しいファイルの約 20 件は G2 の初めに直す。

## 動いているもの(確認方法つき)
- **テスト(2026-10-09、T-0187)**: 最初に t-0139・t-0178・t-0184 を合わせた main(変更前)を release で
  `-Filter "^(smoke|singleton|log|sim_scheduler|debug_camera|replay_file|reaction|multires|fixed|physics|float_check|luau|time_control|image|lab|gas)"` 46 本(約 6 分)・
  `-Filter "^(gpu_(fixed|physics|work_graph|debug|reaction|conduct|probe|lab)|window_)"` 41 本(約 25 分)が通過(直したものなし・流し直しなし)。release のビルドは警告なし(前からの C4189 だけ)。
  変更後の release: `-Filter "^(multires.*|reaction.*|float_check|lab_box)$"` 16 本が通過(multires は試験の期待を 1 回直した: 刻み 0 の 2 つの粗くする要求は同じ親を取り合うので、
  溢れを使う世界でも適用は 3)。debug: ビルド(警告なし)・`-Filter "^(multires|reaction)$"` が通過(FX_ASSERT あり。multires 145 秒)。release の `-Filter "^gpu_multires(_conduction)?(_implicit(_tree)?)?(_warp)?$"`(t-0178 が求めた束 + 伝導。テンプレートにした MrCellThermal を使う)は下の作業ログ・コミットを参照(このコミットの時点では流している途中)
  足したテスト: multires_test の TestCoarsenFullWide(粗くして 12 刻み伝導ありで刻む)・TestLimitsStepWide(伝導なし・あり)・TestWideMatchesInline(本物の鎖を 2 つの世界で毎刻み比べる)。archmap OK(142)。
- **テスト(2026-10-09、T-0175)**: 最初に t-0132・t-0026・t-0140 を合わせた main(変更前)を release で
  `-Filter "^(smoke|singleton|log|sim_scheduler|debug_camera|replay_file|reaction|multires|fixed|physics|float_check|luau|time_control|image|lab|gas)"` 45 本(約 7 分)・
  `-Filter "^(gpu_(fixed|physics|work_graph|debug|reaction|conduct|probe|lab)|window_)"` 41 本(約 22 分)が通過(直したものなし・流し直しなし)。release・debug のビルドは警告なし(前からの C4189 だけ)。
  変更後: release のビルドで tests/reaction_wait_test.cpp の `std::ranges::equal(..., HashReactionCell, HashReactionCell)` が多重定義で通らなくなった → ラムダにした。
  変更後の release: 1 つ目の束 45 本が通過(約 7 分。reaction_test に TestWideLimits・TestWideCrowdedCell・TestWideMatchesInline)・
  核を使う GPU のテスト `-Filter "^(gpu_reaction(_warp)?|gpu_reaction_limits(_warp)?|gpu_probe_(sim|peek)(_warp)?|gpu_lab_box(_warp)?|window_lab)$"` 11 本が通過(約 7 分)・
  `-Filter "^gpu_multires"` 22 本が通過(約 49 分。合わせた状態の確かめも兼ねる。implicit_tree 501 s・subcycle_warp 431 s)。合わせて release 108 本。2 つ目の束の残り(gpu_fixed・physics・work_graph・debug・conduct・probe の trace/fire/physics/rewind・window_replay)は核を使わないか
  仮の世界の同じ核なので変更の後は流していない。
  debug: ビルド(警告なし)・`-Filter "^reaction$"` が通過(FX_ASSERT あり。約 35 秒)。tidy は流していない。archmap OK(140)。
- **テスト(2026-10-09、T-0163)**: 最初に t-0154・t-0157・t-0142 を合わせた main(変更前)を release で
  `-Filter "^(smoke|singleton|log|sim_scheduler|debug_camera|replay_file|reaction|multires|fixed|physics|float_check|luau|time_control|image|lab)"` 43 本(約 6 分)・
  `-Filter "^(gpu_(fixed|physics|work_graph|debug|reaction|conduct|probe|lab)|window_replay)"` 40 本(約 26 分)が通過(直したものなし。失敗・流し直しなし)。
  時間の約束のため `^gpu_multires`(20 本)と `^window_lab$` は変更を入れた後に流した: `-Filter "^gpu_multires"` 20 本が通過(約 55 分。並走の負荷で implicit_tree 720 s・subcycle_warp 421 s。足した RunLimitsStep は gpu_multires・_warp の中)、
  `-Filter "^(multires|reaction|reaction_package_scene|window_lab|lab_box|gpu_lab_box(_warp)?|gpu_probe_peek(_warp)?|gpu_reaction_limits(_warp)?)$"` 11 本が通過(約 2 分)。
  合わせて 104 本。release・debug のビルドは警告なし(前からの implicit_conduction.cpp の C4189 だけ)。tidy は流していない。archmap OK(135)。
  足したテスト: multires_test の TestLimitsStep(伝導なし・あり。上限の場面を 12 刻み: 待たせた 8448・選んだ 8448 (セル, 刻み)、保存は毎刻み同じ)・
  gpu_multires_test の RunLimitsStep(同じ場面を HW・WARP で。数える器も毎刻み CPU と同じ)。分布の測り方: `job.py run -Preset release -Exe reaction_package_scene_test -- --distribution 600 10`(約 1 分)。
- **テスト(2026-10-09、T-0022)**: 最初に t-0136・t-0021・t-0143 を合わせた main(変更前)で release の全部 96 本を 3 回に分けて通過(直したものなし。失敗も流し直しも無し)
  (`-Filter "^(smoke|singleton|log|sim_scheduler|debug_camera|replay_file|reaction|multires|fixed|physics|float_check|luau|time_control|image)"` 40 本・約 5 分 /
  `-Filter "^(gpu_(fixed|physics|work_graph|debug|reaction|conduct|probe)|window_replay)"` 36 本・約 22 分 / `-Filter "^gpu_multires"` 20 本・約 41 分)。
  変更後は release で `-Filter "^(reaction|reaction_contention|reaction_wait|reaction_package|multires|gpu_reaction(_warp|_limits|_limits_warp)?|gpu_multires(_warp)?|gpu_multires_activity|gpu_multires_quiet|gpu_probe_sim)$"`
  の 14 本が通過(約 9 分。multires だけ場面の作り方の誤り〔同じ親への細かくする要求を 1 回に出して取り合いになった〕で落ちた → 直して `^(multires|gpu_multires(_warp)?)$` の 3 本が通過)。
  核が大きくなったが HW の活性のグラフ(gpu_multires_activity)は止まらない(T-0124)。release・debug のビルドは警告なし(前からの implicit_conduction.cpp の C4189 だけ)。
  debug で `-Filter "^(reaction|multires|gpu_reaction_limits_warp)$"` の 3 本が通過(約 2 分。FX_ASSERT あり)。ほかの gpu_multires_*(conduction・subcycle・uniform・near_fold・implicit)・gpu_probe_peek/trace/fire・window_replay は流していない
  (候補が 16 以下・生成物が入りきる時は結果が変わらない形にしたので、今の場面の値は同じ。tidy も流していない)。
  足したテスト: reaction_test の TestLimits・gpu_reaction_limits(_warp)= `gpu_reaction_test --limits`(512 セル × 400 刻み)・multires_test の TestCoarsenFull・
  gpu_multires_test の RunCoarsenFull(上限の試験の表 tests/reaction_limits_table.h・場面 tests/multires_limits_scene.h)。archmap OK(132)。
- **テスト(2026-10-09、T-0113)**: 最初に t-0135・t-0138・t-0025 を合わせた main(変更前)で release の全部 93 本を 3 回に分けて通過(直したものなし。失敗も流し直しも無し)
  (`-Filter "^(smoke|singleton|log|sim_scheduler|debug_camera|replay_file|reaction|multires|fixed|physics|float_check|luau|time_control|image)"` 39 本・約 6.5 分 /
  `-Filter "^(gpu_(fixed|physics|work_graph|debug|reaction|conduct|probe)|window_replay)"` 34 本・約 18 分 / `-Filter "^gpu_multires"` 20 本・約 40 分)。
  変更後は release で `-Filter "^(multires|multires_quiet|multires_uniform|multires_activity|multires_conduction|gpu_multires_quiet(_warp)?|gpu_multires_uniform(_warp)?|gpu_multires_near_fold(_warp)?)$"`
  の 11 本が通過(約 9 分。線の場面: 許容差なしは葉 2 だけ刻み 17、D-435 は葉 1・2 が刻み 17・18 に粗くなり、熱い線・O2 の線は残る)。debug はビルドだけ(release・debug とも警告なし)。
  tidy は流していない。ほかの gpu_multires_*(subcycle・conduction・activity・implicit)は流していない(静かな葉を粗くする要求を使わない。見出しの欄は名前を変えただけ)。archmap OK(129)。
- **テスト(2026-10-09、T-0123)**: 最初に T-0134・T-0020・T-0023 を合わせた main(変更前)で release の全部 87 本を 3 回に分けて通過
  (`-Filter "^(smoke|singleton|log|sim_scheduler|debug_camera|replay_file|reaction|multires|fixed|physics|float_check|luau|time_control)"` 33 本・約 4 分 /
  `-Filter "^(gpu_(fixed|physics|work_graph|debug|reaction|conduct|probe)|window_replay)"` 34 本・約 18 分 / `-Filter "^gpu_multires"` 20 本・約 32 分)。
  2 回目の束で window_replay_peek が 1 回だけ全部のハッシュ不一致で落ちた(S(1) から。ほかの作業ツリーの GPU のテストと重なった時)→ `-Filter "^window_replay"` で流し直して 4 本通過。
  直したものなし。変更後は release で `-Filter "^(multires_activity|gpu_multires(_activity|_quiet|_uniform|_conduction|_near_fold)?(_warp)?|gpu_probe_(peek|sim)(_warp)?)$"` の 17 本が通過(約 13 分)
  (GPU の全部を刻む・活性・観察の枠・覗き窓の入れ子が CPU の全部を評価する側と毎刻みビット一致)。debug はビルドだけ。
  計測は `job.py run -Preset release -Exe gpu_multires_activity_test -- --queue compute` を 2〜3 回続けて(1 回目は時計が低く 5〜8 倍に出る)。
- **テスト(2026-10-09、T-0130)**: release の全部のテスト 84 本を 3 回に分けて通過(T-0122 と T-0129 を合わせた main + この変更。直したものなし):
  `-Filter "^(smoke|singleton|log|sim_scheduler|debug_camera|replay_file|reaction|multires|fixed|physics|float_check)"`(31 本・約 4 分)/
  `-Filter "^(gpu_(fixed|physics|work_graph|debug|reaction|conduct|probe)|window_replay)"`(33 本・約 20 分)/ `-Filter "^gpu_multires"`(20 本・約 40 分。
  並走の負荷で implicit_tree 343 s・uniform 270 s・subcycle_warp 425 s)。gpu_multires_*・gpu_probe_*・gpu_reaction は HW も WARP も CPU と毎刻みビット一致。
  tidy(release)警告なし(CreatePipelines の readability-function-size も、古いパイプラインを消して消えた)。archmap OK(123)。
  debug で `-Filter "^(reaction|reaction_contention|reaction_wait|gpu_reaction_warp|gpu_reaction|multires_activity|gpu_multires_warp)$"` の 7 本が通過(約 4 分。FX_ASSERT あり)。
- **テスト(2026-10-06、T-0122)**: release で `-Filter "gpu_probe|window_replay"` の 13 本(gpu_probe_sim・_trace・_physics・_physics_compute・_peek の HW と WARP・gpu_probe_fire・window_replay 3 本。約 11 分)、
  足した試験の後に `-Filter "^gpu_probe_sim(_warp)?$"` が通過。debug で `-Filter "^gpu_probe_(sim|trace|peek)_warp$"`(約 8 分。sim_warp 312 s)が通過。
  仮の世界と覗き窓のファイルだけ変えたので、多重解像度・反応の CPU のテストと gpu_multires_* は流していない(共有の reaction.hlsli・multires の関数は変えていない)。
  gpu_probe_sim の「遅い反応」: 一様な木の壁の世界を、待ちで起きるブロックが刻み 2〜39 に 4 つ以上ある最も低い温度(探して 340 K)にし、7 ブロックが待ちの来た刻みに起きて CPU と毎刻み一致。
  計測は docs/perf.md(T-0122。木箱 3600 刻みの平均 214.6 µs/刻み)。archmap OK。
- **テスト(2026-10-06、T-0125)**: release で `-Filter "^gpu_multires_(conduction|near_fold)(_warp)?$"`(4 本・約 4 分)と
  `-Filter "^(gpu_multires(_activity|_quiet|_uniform|_subcycle)?(_warp)?|gpu_probe_peek(_warp)?|multires.*|reaction.*|float_check)$"`(23 本・約 23 分。subcycle_warp 448 s)が
  通過(multires_uniform だけ落ちた → 畳みをまずビット単位にして直し、`^(multires|multires_(uniform|conduction|quiet|activity)|gpu_multires_(uniform|near_fold|quiet)(_warp)?)$` の 11 本を流し直して通過)。
  伝導・小刻み・near_fold・活性・静か・一様・覗き窓は HW も WARP も CPU と毎刻みビット一致(伝導は待ちの丸め、覗き窓は今までの丸め)。
  debug で `-Filter "^(multires_uniform|gpu_multires_conduction_warp|gpu_multires_near_fold_warp)$"` が通過(約 6 分)。debug の HW の GPU のテスト(GBV で長い)と tidy は流していない。archmap OK。
  計測は docs/perf.md(T-0125。伝導ありの全部を刻む 0.25 → 0.35 ms・鎖の 64 回は 4.8〜7.3 ms で T-0111 から悪化なし)。
- **テスト(2026-10-06、T-0124)**: release で `-Filter "^(gpu_multires(_activity|_quiet|_uniform|_conduction|_near_fold)?(_warp)?|gpu_probe_peek(_warp)?)$"` の 14 本が通過(約 11 分。
  gpu_multires・活性・静か・一様は HW も WARP も待ちの丸めで比べる。伝導・near_fold・覗き窓は今までの丸め)。debug で `-Filter "^(reaction.*|multires.*|float_check|gpu_multires(_activity)?(_warp)?)$"`: CPU の reaction 3 本・multires 7 本(subcycle 524 s)・gpu_multires_warp・gpu_multires_activity(HW・GBV あり 188 s)・_activity_warp が通過、
  **gpu_multires(HW・GBV あり)は失敗なしのまま 600 s の上限で切れた**(場面ごとに約 80 s。HW も待ちの丸めにして重くなった)→ TIMEOUT 1200 にして流し直し、通過(538 s)。
  GBV なしの debug の HW では gpu_multires・gpu_multires_activity とも待ちの丸めで通る(-Od の確認で流した)。
  subcycle の GPU(伝導だけ。反応の式を変えていない)と gpu_multires_implicit は流していない。archmap OK。tidy(release)は 1 件: gpu_multires.cpp の CreatePipelines が
  readability-function-size(このチケットでは触っていない。T-0121 でパイプラインを足した時からと推定。分けるのは次に gpu_multires.cpp を触る時)。
- **テスト(2026-10-06、T-0121)**: 最初に release の HW で gpu_multires_*(gpu_multires・_implicit・_activity・_quiet・_uniform・_conduction・_subcycle・_near_fold)8 本が通過(T-0115 の 104 バイトの見出しで壊れていない)。
  変更後は release で `-Filter "^(gpu_multires(_activity|_quiet|_uniform|_conduction)?(_warp)?|gpu_probe_peek(_warp)?)$"` の 12 本が通過(約 9 分)。debug・subcycle・near_fold・CPU の multires は流していない(CPU は reaction.hlsli の欄の順を戻しただけで変えていない)。
  gpu_multires_warp は待ちの丸め、HW と活性・静か・一様・伝導・覗き窓は今までの丸めで比べる(各テストの Rounding()。T-0124・T-0125 で外す)。
- **テスト(2026-10-06、T-0112)**: 畳む段(multires_tree.hlsl の TreeFoldCheck・TreeFold)とルート定数(25 個に)を変えた。release で
  `-Filter "^gpu_multires_(uniform|near_fold)(_warp)?$"`(約 3 分)、debug で `-Filter "^(multires_uniform|gpu_multires_(uniform|near_fold|quiet)(_warp)?)$"` が通過
  (debug の gpu_multires_uniform は活性のグラフを 3 回作るので 300 秒を超えた → TIMEOUT 900 にした。約 290 秒)。tidy(release)警告なし・archmap OK。ほかの GPU の多重解像度のテスト(gpu_multires の 8 本・conduction・subcycle・probe)は流していない
  (ルート定数を末尾に 1 語足しただけで、畳む段のほかは変えていない)。release のビルドに警告なし。
  新しいテスト gpu_multires_near_fold(_warp)= `gpu_multires_conduction_test --near-fold`(release の HW 約 25 秒・WARP 約 75 秒、debug は両方約 3 分)。
- **テスト(2026-10-06、T-0104)**: CPU の多重解像度だけ変えた(GPU のシェーダーは multires.hlsli・multires_activity.hlsli に関数を足しただけで、呼ぶ所は無い)。
  debug で `-Filter "^multires"` 7 本(途中の版)と `^multires_(uniform|conduction|quiet|activity)$` 4 本(最後の版)が通過。release の `multires_conduction_test --residue`(約 90 秒)も保存量一致。
  GPU のテスト(gpu_multires_*)は流していない(GPU の道は変えていない。release と debug のビルドで全部のシェーダーがコンパイルされることは確かめた)。
- `job.py build`(debug・release)・`python3 tools/archmap/archmap.py --check` OK(114)。
- 足したテスト gpu_multires_subcycle・gpu_multires_subcycle_warp(gpu_multires_conduction_test --subcycle。debug の HW 約 13 分・release の WARP 約 9 分。CPU の小刻み 64 回が重い)。
- **テスト(2026-10-06、T-0111)**: 伝導の段のシェーダーと計測の引数だけ変えたので、伝導を使うテストに絞った: release で gpu_multires_conduction(_warp)・gpu_multires_subcycle(_warp)の 4 本、
  debug で gpu_multires_conduction(_warp)・gpu_multires_subcycle の 3 本が通過(毎刻み CPU とビット一致)。debug の gpu_multires_subcycle_warp と、伝導を使わない gpu_multires の 8 本などは流していない。tidy(release)警告なし。
- (T-0109 の時)**テスト(2026-10-06、debug)**: 変更が触る所を全部流して 41 本通過(gpu_multires の 8 本・gpu_multires_conduction 2 本・gpu_multires_subcycle 2 本・gpu_probe と window_replay 13 本・
  multires の CPU・reaction・float_check 16 本)。release でも gpu_multires_conduction・gpu_multires_subcycle(_warp)が通る。多重解像度を使わない 29 本(smoke・fixed・physics など)は
  今回は流していない(変更が触らない。T-0105 のマージ後の reaction 5 本は最初に通ることを確かめた)。tidy 警告なし。
  `job.py test` は分けて投げる: `-Filter "^gpu_multires(_warp|_activity|_activity_warp|_quiet|_quiet_warp|_uniform|_uniform_warp)?$"`(約 14 分)/ `-Filter ^gpu_multires_conduction`(約 16 分)/
  `-Filter gpu_multires_subcycle`(debug で 30 分前後)/ `-Filter "gpu_probe|window_replay"`(約 20 分)/ 残り(`-Filter "^(smoke|singleton|log|sim_scheduler|debug_camera|replay_file|multires|fixed|physics|gpu_fixed|gpu_physics|gpu_work_graph|gpu_debug|float_check)"`。約 25 分)と `-Filter reaction`。
  `--timeout 2400` で裏で投げ、runner/logs/<job>.result.json を待つ。テストの表示した行は out/build/<preset>/Testing/Temporary/LastTest.log(走っている間は .tmp)。

## 壊れている/未確認のもの(ファイル:行 と症状)
- **上限の当座のふるまい(T-0022。仮 = QUESTIONS Q19)**: (CPU の多重解像度の世界は EnableWideCells で ①③ が起きない。T-0187。既定は使わない)8 種のセルで 9 種目を作る反応は枠が空くまで進まない・17 本以上は平均 16/N の速さ・入りきらない子は細かいまま。
  多重解像度の世界の刻みでは MR_COUNTER_LIMIT_PRODUCTS・_CANDIDATES に数える(T-0163)。仮の世界ではまだ数えていない(T-0177)。ほかに数えているのは 1 セルの反応の GPU
  (reaction_cells.hlsl の u1)と、粗くするのを断った数(MR_COUNTER_COARSEN_FULL)。粗くする要求の解決(TreeResolve)は 1 スレッドで 64 セルを MrCoarsenCell するので、要求 1 件ごとに重い(未計測)。
- **HW の Work Graph は反応の核を 1 ノードに 3〜4 か所展開すると DEVICE_HUNG**(T-0124。原因は推定・BACKLOG)。反応はどこも待ちの丸めだけ(今までの丸めは T-0130 で消した)。
- 待ちの丸めの 1 回の評価は重い(セルの規則ごとに log2 2 回と 128bit の割り算)。GPU は眠っているブロックを評価しないので費用は起きている所だけ(T-0123)。
  CPU の StepNest・StepActive(確かめる側)は全部を評価するので、debug の CPU のテストは T-0115 の前より遅いまま(multires_conduction・subcycle)。
  1 本のリストに 400 回積むと TDR になった(T-0123 の前。計測の暖機は 40 回のまま)。
- **細かいレベルの熱の小刻みの GPU は Δkmax 3 で 8.4 ms/刻み**(T-0111 で 40 から縮めた。鎖は Δkmax 1・2・3 で 0.55・2.2・8.4 ms)。小刻み 1 回 = 印 25・頁 3・端数 4・埋める 2・流れ 25・
  終わり + 起こすグラフ 53 µs(活性 Compute。一時的にタイムスタンプを打って測った)。段 6 つ + 起こすグラフの固定費 ≈ 50 µs × 64 が床。どうするかは QUESTIONS Q3(判断待ち)。
- 小刻みの数は呼ぶ側の maxSubcycleGap で決まり、GPU は最大回数を全部積む(空の小刻みも 41〜49 µs。T-0111 で 512 スレッドのグループが空で抜ける分 19 → 41 µs に増えた〔全部を刻む時〕)。ゲームの値は未定(T-0111 の後)。
- 基準 + 3 段より細かい所は今までどおり 1 段ごとに約 1/4 遅い。方式②は研究 T-0110(並走、D-432)。
- 基準(subcycleBaseLevel)は呼ぶ側が渡す値で、表から自動では決めていない(テストは tests/multires_conduction_scene.h の SubcycleTestOptions = 2)。ゲームの表を作る時(T-0021)にベイクで決める。
- 伝導で粗い側が取った端数の枠は、許容差つきの畳む段(CPU・GPU)なら静かになった時に返る。許容差なしの時は木の変更でしか返らない(たくさんの要求の小刻みで不足 9880)。
- 熱が通った所は温度差が約 1 mK 未満で流れが止まって静かになるが、ビット単位で同じに戻らない(許容差つきなら畳める。CPU・GPU)。
- 畳む 2 段は、畳むものが無い刻みで 0.0131 → 0.0197 ms(世界の枠 640。TreeFoldCheck が大きくなった分)。ほぼ同じ頁 2 つ + 端数の枠 1 つを返す刻みは 0.047 ms
  (docs/perf.md)。たくさんのブロックが同じ刻みに静かになってもグループは並列。重ければ成分の回(グループの同期 約 20 回)をまとめる(未着手)。
- 許容差なし(MrExactFoldTolerance)で呼ぶと、熱が通った葉は組の中のエネルギーがビット単位で違うので粗くならない(鎖の場面で本物 14・頁 14 のまま。T-0113。ゲームは D-435 の許容差で呼ぶ想定)。
- release のビルドで implicit_conduction.cpp(158) に C4189('added' が未使用。T-0110 の FX_ASSERT の中だけで使う変数)。並走の T-0117 の範囲なので触っていない。
- 影のブロックは自分の中だけ伝導する(外は断熱。親との受け渡しは引き戻し)。
- **release の WARP では、伝導の段の Work Graph 版(multires_conduct_graph.hlsl)の最初の DispatchGraph でデバイスが失われる**(DXGI_ERROR_DRIVER_INTERNAL_ERROR)。
  debug の WARP とハードウェア(debug・release)では CPU と一致する。WARP の JIT の不具合と推定(未確認。ADR-0013 と同じ種類)。既定の Compute 版は release の WARP でも一致。
  テストは release の WARP でだけ Work Graph 版を外している(gpu_multires_conduction_test.cpp の RunAll。--subcycle も同じ)。
- 伝導の段の Compute は活性の時も全部の枠の数だけグループを投げる(一覧の数を超えたグループはすぐ抜ける)。世界の枠が数万になったら ExecuteIndirect か Work Graph 版を測り直す。
- (前から)取り合いの丸めの残る偏り・「一様」はビット単位・頁の不足は「刻まない」だけ・反応の核のセルが変わらない種・観察の影の親も粗くなる・活性の固定費・PIX・セーブ・AMD は未確認/未着手。

## このチャットで決めたこと(ADR にしたなら番号)
- (Claude が決めた。T-0187)溢れの持ち方は「頁ごと(と端数の枠ごと)の領域に、セルの番号の順に詰める」(MultiresOverflowArea の offsets はプレフィックス和)。
  インラインには ID の小さい方から 8 個、溢れはその続き。1 セルを書くたびに頁の中を詰め直す(並びは内容だけで決まる。GPU の T-0176 のグループ内のプレフィックス和と同じ並び)。
  インラインの RxCell・MrFraction は今のまま有効なセル(8 種まで)なので、溢れを知らないコード(GPU・一様の値・畳む判定)はそのまま読める。02 §3 に追記。
- (Claude が決めた。T-0187)粗くする和集合は MrCoarsenCellOf(children, 空の結果)= 子の形と結果の形のテンプレート(MrCoarsenCell はインラインの包み)。
  形ごとの道具は MrFractionHasRoom・MrFractionWithRoom・MrEmptyFractionLike・MrLostHasRoom・MrWithLostRoom・MrUnionStepLimit(上限なし版は sim/multires_wide_cell.h)。
  MrNextSpecies・MrAmountOf・MrFractionOf・MrStepCellWait・MrCellThermal・HcCellConductance もセルの形のテンプレート(GPU の呼ぶ側は変えていない)。
- (Claude が決めた。T-0187)設定は世界ごと(MultiresNest::wideCells。EnableWideCells で作った直後に)。溢れのある頁・端数の枠は畳まない・端数を帳簿へ返さない(保存を守る。T-0199 で畳めるようにする)。
  2 時間の約束のため、影の引き戻し(溢れのある影は FX_ASSERT)・畳む・静かな葉の許容差の判定・覗き窓・実験室は T-0199 に分けた。
- (Claude が決めた。T-0175)上限なしは「核をセルの形のテンプレートにする」で作った(02 §3 に追記)。形ごとに違うのは RxHasRoomForSpecies・RxCellWithRoom・
  RxEmptyCellLike・RxEmptyUsage の 4 つ(同じ名前の関数を形ごとに。RxWideCell 版は引数依存の名前探索で見つかる)。使う量(RxUsage / RxWideUsage)は
  RxSumUsage(…, RxEmptyUsage(cell)) の引数で型を決める(HLSL に auto・decltype が無いので)。結果の構造体は RxAddedOf<Cell>・RxWaitStepOf<Cell>、
  RxAdded・RxWaitStep は RxCell の typedef(GPU・多重解像度の呼ぶ側は変えていない)。RxLoneCell はインラインだけ(試験は reaction_test の StepWideLone)。
- (Claude が決めた。T-0175)「RX_LIMIT_PRODUCTS を CPU では使わない設定」は、セルの形を RxWideCell にすること(RxHasRoomForSpecies が常に真なので待たせる道を通らない)。
  候補の上限 16(RX_LIMIT_CANDIDATES)は形に依らず残る(T-0164)。HashReactionCell・CountElements・MakeWideReactionCell・StepReactionCellWait に RxWideCell の版。
- (Claude が決めた。2 時間の約束)多重解像度の世界(MultiresNest のセルに溢れ・粗くする和集合・TestCoarsenFull / TestLimitsStep の上限なし版)は T-0187 に分けた。
- (Claude が決めた。T-0163)数える単位は「上限に当たった (セル, 刻み)」の数。頁のセルを刻んだ時だけ数え、一様なブロックの試しの評価(MrUniformWaitOf・
  EvaluateUniformWait・EvaluateUniformCells)は数えない(変わるなら頁に広げて刻み直す所で数える)。GPU は眠っているブロックを評価しないが、眠っているブロックは
  進む規則が無い(offered = 0)ので印が立たず、CPU が全部を評価しても同じ数になる。GPU はスレッドで足してから 0 でない時だけ InterlockedAdd(MrLimitTally・CountLimits)。
- (Claude が決めた。T-0163)K(インラインの成分の数)は 8 のまま(打ち切り条件。試験の表は物質 8 しかない)。二段の成分は CPU(T-0175)と GPU(T-0176)に分けた。
- (Claude が決めた。T-0163)時間の約束(2 時間)のため、合わせた状態の gpu_multires の 20 本と window_lab は、変更を入れた後のビルドで流した(この変更は数える器を足すだけ)。
- (Claude が決めた。T-0022)① 生成物を全部足して入りきらなければ、元のセルに無い物質を作る規則を全部進行度 0 にしてやり直す(RxApplyExtentsHeld。
  残りの生成物はどれも元のセルにある物質なので必ず入る)。待たせた規則の取り合いの分け前はほかに回さない(取り合いを解き直さない = 核を大きくしない)。
- (Claude が決めた。T-0022)② 優先度 = FxHashCombine(RxSelectSeed(世界, 刻み, セル), 規則の鍵)。小さい 16 個を 1 回の走査で残す(一杯の時は最も後の 1 つと比べて置き換える。
  どの順に来ても同じ集合)。16 以下の時は選ばないので、今の世界の結果は変わらない(種のハッシュ 1 回だけ増えた)。
- (Claude が決めた。T-0022)③ 粗くする要求の解決(ResolveCoarsen)で親の 64 セルを MrCoarsenCell して overflowCount を見る。断ったら見出しの quietCheck を
  「今の忙しさの印で調べた・粗くできない」にする(静かな葉が毎刻み要求し直さない)。断った要求は親の取り合いの印を取らない。
- (Claude が決めた)新しい数える欄は MR_COUNTER_COARSEN_FULL = 24(MR_COUNTER_COUNT 25)、状態は MR_STATUS_SPECIES_FULL = 6。RxLoneCell に上限の刻みの数を 2 つ足した。
- (Claude が決めた。ADR-0015 追記〔T-0113〕)判定は組 64 個ごとに MrCoarsenGroupWithin(許容差つきは畳むのと同じ MrFoldStats を 8 セルに、許容差なしはビット単位)。
  一様な葉はいつも。端数は比べない。許容差は呼ぶ側が FoldQuietPages と同じ値を渡す(SubmitQuietCoarsenRequests(nest, table, tick, tolerance) / RecordQuietRequests(..., tolerance)。
  渡さない版は許容差なし)。GPU は g_foldTolerance を共有(ルート署名は変えていない)。
- (Claude が決めた。同上)調べるのは「静かな葉で quietCheck が今の busyTick の印でない時」(== N + 1 の刻みにしなかった: 呼ぶ側が刻みを飛ばしても抜けない)。
  quietCheck = busyTick の下位 30bit << 2 | 調べた | 粗くできる。兄弟は粗くできる静かな兄弟にだけ譲る。HashWholeNest に入れた。
- (前のチャット T-0123 以前の決定は下と各 ADR 追記)
- (Claude が決めた。ADR-0018 追記〔T-0123〕)GPU の StepBlockWait は `印 < wakeTick かつ busyTick ≠ 印` のブロックを頁のブロックでも評価しない
  (全部を刻む StepWait・ActivityStepNode・ObserverStepNode・覗き窓の入れ子)。CPU は全部を評価したまま(確かめる側)。
- (Claude が決めた。同上)log2 と割り算を要る時だけにする形は採らない(GPU の仮の世界が 8〜12% 遅くなった。式は今のまま)。
- (Claude が決めた。ADR-0018 追記〔T-0130〕)1 セルの反応の試験は 1 セルだけのブロック(セル + changedTick)を待ちの丸めで進める。GPU の試験は区間の初めに
  「直前の刻みに変わった」とみなす(セルごとの changedTick をバッファに持たない。待ちは記憶が無いので偏らない)。MrSameCell は RxSameCell を呼ぶ。
  D-424 の「進まない」を確かめる試験(TestSleepCutoff)と、取り合いの試験の「刻みの乱数の丸め」の版は消した(待ちの丸めの版だけ残す)。
- (Claude が決めた。ADR-0018 追記〔T-0122〕)仮の世界の tc と起こす刻みはブロック(4³)ごとの 64bit の印(ProbeChangeMark = 刻み + 1)を予定の印のバッファ(u11)の後ろに
  (ルート署名は変えない)。つつきは tc = 印 − 1。計算したブロックは、変わったら tc = 印、変わったか次の刻みに評価が要れば次の一覧へ(起こす刻み = RX_WAIT_NEVER で
  WakeDueBlocks と重ならない)、ほかは起こす刻み(セルの最小)を書く。初めの起こす刻みは CPU が作って写す(ProbeInitialBlockWakes: 刻み 0 を tc = 0 で計算してみる)。
  PROBE_BLOCK_FLAG_POSSIBLE は「次の刻みに評価が要る」に変えた。試験用に ProbeSimOptions::initialWorld・ProbeReference の初めの世界を足した。
- (Claude が決めた。2 時間の約束)今までの丸めのコードを消す所を T-0130 に分けた(テストの書き直しと release の全部の流し直しで 2 時間を超えるため)。
- (Claude が決めた。ADR-0018 追記〔T-0125〕)GPU の伝導の段は待ちの丸めだけ(今までの丸めの反応を残さない。1 ノードの反応の核を増やさない)。
  伝導を入れる全部を刻む刻みも先に起こす段 WakeDue。一様なブロックの評価は最初の小刻みの印の段(ConductMarkBlock)、頁のブロックの見出しは
  ConductApplyBlock が CPU の RecordWaitResults と同じに書く(観察の枠も)。64bit の最小は GroupMinTick(上位 → 下位の 32bit 2 回)。次の刻みの種は書かない。
- (Claude が決めた。同上)許容差つきで畳んだら PokeBlock(CPU の FoldQuietPages・GPU の TreeFold の WRITTEN)。許容差つきでも**まずビット単位で一様か**を調べ、
  そうなら値が変わらないのでつつかずに畳む(前は覆われていないセルがあると平均の道を通った。multires_uniform の「許容差なしと全部一致」が落ちたので)。
  つつくと静かさを数え直すので、その頁の静かな葉を粗くするのが約 17 刻み遅れる。影を引き戻したら影の busyTick = MR_BUSY_POKED(種なし。値が変わったかは見ない)。
- (前のチャット T-0124 の決定は ADR-0018 追記〔T-0124〕: RxWakeTickOf・飽和する足し算の形を書かない・活性のグラフは待ちの丸めだけ・種は WakeDue)
- (前のチャット T-0121 の決定は ADR-0018 追記〔T-0121〕・チケット T-0121 の「決めたこと」)
- (Claude が決めた。ADR-0015 追記〔T-0112〕)GPU の畳む段の帳簿の足し算は**繰り上げつきの atomic**(AddLedgerCarrying。atomic の前の値で自分の桁あふれが分かる。
  足した値の和も繰り上がりの回数も順によらないので CPU の枠の順の足し算とビット一致)。端数を帳簿へ移す・平均を書く・余りを足すのは TreeFoldCheck、
  頁と端数の枠を積むのは TreeFold(枠の順の累積和)。印は取り合いの印に MR_CLAIM_FOLD_COPY / WRITTEN / RETURN(下位 3bit)。
- (Claude が決めた)許容差はルート定数 1 語に詰めた(MrPackFoldTolerance: 下位 25bit = 温度 mK〔約 33 K まで〕、上位 7bit = 64 − amountShift。0 = 完全に同じ)。
  ルート署名は 63 / 64 語(あと 1 語)。並走の作業役がルート定数を足すと 64 になる。
- (Claude が決めた)ほぼ同じかの集計はウェーブの縮約(和は 64bit を上下 32bit に分けて足す)・成分は「前より大きい最小の ID」の最小を繰り返して昇順に集める。
  最初は 1 スレッドで 512 セルを MrAddFoldCell で足したが、1 回 7.5 ms(並走の負荷あり)かかったので変えた(0.047 ms)。
- (前のチャット T-0104 の決定は ADR-0015 追記と 17 §5 にある)

## 次にやること
NEXT.md の先頭。判断待ちは無し(D-433〜D-436)。ユーザーに判断を求める時は「プレイヤーと遊びへの影響」の水準で出す(CLAUDE.md §5)。

## 注意(次の Claude がハマりそうな所)
- **世界のセルの溢れ(T-0187)**: 溢れを使う世界(nest.wideCells)で頁のセル・端数を書く所は StoreWidePageCell・StoreWideFraction を使う(nest.cells に直接書くと溢れと食い違う。
  LoadWideNestCell の FX_ASSERT が「溢れがあるならインラインは 8 種」を確かめる)。頁・端数の枠を空きに返す所は ClearPageOverflow・ClearFractionOverflow。
  新しく頁を返す道を足したら、溢れを空にするか、溢れのある頁を返さないこと(次に使う所に古い溢れが残る)。
- **核のセルの形(T-0175)**: reaction.hlsli の核の関数は `template <typename Table, typename Cell>`。セルの成分を増やす所は RxHasRoomForSpecies で確かめてから
  RxCellWithRoom で場所を用意して書く(RxWideCell は std::vector なので、場所を用意せずに species[speciesCount] へ書くと範囲外)。新しい関数で成分の位置ごとの
  作業場が要る時は RxUsage のように「形ごとの入れ物 + 引数で型を決める」にする(RX_MAX_CELL_SPECIES の配列を核に足すと RxWideCell で溢れる)。
  HashReactionCell・CountElements は多重定義になったので、関数名をそのまま std::ranges の射影に渡せない(ラムダで包む)。
- **上限の印の数える器(T-0163)**: MR_COUNTER_LIMIT_*(multires.hlsli)は CPU の StepPagedBlock と GPU の StepPagedWait・ConductApplyBlock(ApplyCell)の 3 か所。
  反応を刻む段を足す時はここにも足す(数える器は HashWholeNest に入るので、忘れると gpu_multires_* が食い違って教える)。
  分布の測定は reaction_package_scene_test の --distribution(進む規則の数は伝導の前のセルで数えた近似。ProbeReference::ChangedMarks を読む)。
- **静かな葉を粗くする判定(T-0113)**: multires_activity.hlsli の MrNeedsQuietCheck・MrIsCoarsenableQuietLeaf・MrCoarsenGroupWithin(Cells の約束: Cell(index)・Temperature(cell))。
  CPU は multires_activity.cpp の CheckQuietLeaves(SubmitQuietRequests の初め)、GPU は multires_tree.hlsl の TreeQuietCheck(PassQuietCheck。木の段は 12 個)。
  quietCheck は busyTick が変わると自動で古くなる(書き手を増やしても消す必要はない)。セルを書き換える段を足す時に busyTick を書き忘れると、古い判定のまま粗くなる。
  線の場面は tests/multires_quiet_scene.h の MakeLineNest(要求は 1 つの親に 1 回の処理で 1 つなので 1 つずつ処理する)。
- **GPU の眠り(T-0123)**: multires_wait_step.hlsli の StepBlockWait の初めで眠っているブロックを飛ばす。これは「ブロックのセル・tc・刻むセルの形を変えるものは
  必ずつつく(PokeBlock / MR_BUSY_POKED)か busyTick を書く」という約束に頼っている。セルを書き換える段を足す時につつき忘れると、GPU だけ起きずに CPU と食い違う
  (CPU は全部を評価するので、毎刻みの比べで見つかる)。GPU の計測は同じ exe を続けて 2〜3 回流し、2 回目以降を使う。
- **仮の世界の待ちの丸め(T-0122)**: probe_conduct.hlsl の ConductBlock(BlockMinTick で起こす刻みの最小 → tc・起こす刻みを書く)と probe_tick.hlsl の
  ApplyCommand(つつきの tc)・WakeDueBlocks(probe_sim.cpp の RecordUnit が適用の後に投げる)。印の場所は probe_sim.hlsli の PROBE_SCHEDULE_CHANGED_WORD・_WAKE_WORD。
  CPU は probe_sim.cpp の ProbeReference::Advance(全部を計算し、ブロックの待ちの最小 ≤ 次の刻みの印を POSSIBLE に)と ProbeInitialBlockWakes。
  元のテストの世界(木箱と空気)は熱が広がり続けて、1500 刻みでも待ちで起きるブロックが 0 だった。起こす道は gpu_probe_sim の TestSlowWake だけが通る。
- **1 セルの反応の試験(T-0130)**: reaction.hlsli の RxLoneCell・RxAdvanceLoneCell(C++ は reaction_table の AdvanceLoneReactionCell)。changedTick は「刻み」の単位
  (多重解像度は印 = 刻み + 1 を渡す。MrStepCellWait)。gpu_reaction は区間(50 刻み)の初めに「直前の刻みに変わった」とみなす(刻みは 1 から)ので、
  区間の切り方を変えると CPU 側(CompareSegment)も合わせる。reaction_test の「いろいろなセル」の要約と gpu_reaction の要約はもう同じにならない。
  stepFlags の 8 は空き(MR_STEP_CUTOFF_ROUNDING を消した)。計測の暖機(gpu_multires_conduction・_activity)は待ちの丸めで 40 回(400 回は TDR)。
- **伝導の段の待ちの丸め(T-0125)**: multires_conduct.hlsli の ConductMarkBlock(最初の小刻みに一様なブロックを EvaluatesUniform → EvaluateUniformCells →
  GroupMinTick → FinishWaitBlock か頁の印)と ConductApplyBlock(ApplyCell が MrStepCellWait、最後に FinishWaitBlock)。multires_wait_step.hlsli を含めて
  CurrentChangeMark・MinTick・FinishWaitBlock を使い回す。GroupMinTick は 1 回の段で 1 回だけ呼ぶ(2 回続けるとスレッド 0 の初期化と読みが競合する)。
  伝導の Work Graph 版(multires_conduct_graph.hlsl)の ConductApplyNode の反応の核は 1 か所(待ちの丸め)。印の段は Compute だけ。
- **GPU の待ちの丸め(T-0121・T-0124)**: gpu_multires.cpp の RecordStep → RecordStepWait(RecordWake〔WakeDue〕→ m_stepWaitPipeline → PassExpand → m_stepExpandedWaitPipeline)。
  活性は RecordStepActive の初めに RecordWake(STEP_FLAG_WAKE_SEEDS。m_activityWrite = この刻みの一覧)→ グラフ(ActivityStepNode・ObserverStepNode = StepBlockWait、ExpandStepNode = StepExpandedWait)。
  シェーダーは multires_step.hlsl の WakeDue・StepWait・StepExpandedWaitPass と multires_wait_step.hlsli(CPU の StepBlocks と RecordWaitResults の GPU 版)。
  HW の不具合を調べた手順と結果は T-0124 のチケットの「結論」。
  **活性のグラフのノードに反応の核(MrStepCell* / RxStepCell*)を足さない**(1 ノード 2 か所まで。3〜4 か所で HW が止まった)。HW を止めうる実験は wt2 のランナーが idle の時に。
  device_bash は 60〜75 秒で切れるので、テストは submit.py で投げて sleep 40 ずつ待つ。
- **ほぼ同じ頁を畳む GPU(T-0112)**: gpu_multires.cpp の RecordFoldPages(list, ring, tick, tolerance)(許容差なしの版は MrExactFoldTolerance で呼ぶ)。
  multires_tree.hlsl の TreeFoldCheck(端数を帳簿へ → FoldsExactly か FoldsNearly〔CollectFoldCells・CollectFoldSpecies〕)→ TreeFold。groupshared を使うので
  TreeFoldCheck の中の分岐はグループで一様に保つ(ウェーブの縮約もある)。テストは gpu_multires_uniform_test の RunActive(許容差つき)・RunNearFoldUnit と
  gpu_multires_conduction_test --near-fold(RunNearFoldChain)。ほぼ同じ頁の場面 MakeNearFoldUnit は tests/multires_uniform_scene.h に移した(CPU と GPU のテストで共有)。
- **ほぼ同じ頁を畳む(T-0104)**: CPU は multires_activity.cpp の FoldQuietPages(nest, table, tick, tolerance)(許容差なしなら古い FoldQuietPages(nest, tick) を呼ぶ)。
  式は multires.hlsli の末尾(MrFoldTolerance・MrFoldStats・MrAddFoldCell・MrFoldStatsWithin・MrFloorDivideEnergy・MrFoldStatsValue)と multires_activity.hlsli の MrWantsFractionReturn。
  帳簿へは AddLedgerBits(繰り上げつき)・AddFoldRemainder・ReturnFractionsToLedger。GPU は T-0112(上)。
  計測は `job.py run -Preset release -Exe multires_conduction_test -- --residue`(約 90 秒)。テストの場面の BeginUniformTick には許容差つきの版もある(T-0112)。ほかの場面(BeginQuietTick など)は許容差なし。
  map.yaml の multiresfold は FoldPage を指す(FoldQuietPages が 2 つあり archmap が引けないため)。
- **伝導の段のスレッド(T-0111)**: multires_conduct.hlsli の CONDUCT_THREADS = 512(印・流れ・足して反応・ConductBegin)と CONDUCT_LIGHT_THREADS = 64(埋める・小刻みの終わり。
  multires_conduct.hlsl と multires_conduct_graph.hlsl の numthreads も合わせる)。印と流れは CacheBlockFaces → GroupMemoryBarrierWithGroupSync の後に面ごとの計算(早く抜けるのはグループで揃う条件だけ)。
  段の初めから終わりまでセルも木も変わらない前提で groupshared を使っている。段の中でセルや木を書き換えるものを足すなら、この前提を見直す。
  計測は `job.py run -Preset release -Exe gpu_multires_conduction_test -- --queue compute --subcycle --measure-only`(約 10 秒。wt2 のランナーが idle の時に)。
- **細かいレベルの熱の刻みの GPU(T-0109)**: gpu_multires.cpp の RecordConduction(小刻みを 4^maxSubcycleGap 回積む)→ RecordConductSubstep(ConductMark → TreeExpand → TreeFractions →
  ConductPrepare → ConductFlows)→ RecordSubstepEnd(ConductEnd + 活性なら起こすグラフ)→ 最後に ConductApply。段の中で始まる・終わるは multires_conduct.hlsli の SubstepBegins / SubstepEnds
  (stepFlags から。CurrentSubstep・SubcycleGap・SubcycleBaseLevel)。CPU の StepConduction と同じ順なので片方を変えたらもう片方も。段を足すなら RecordConductStage
  (Compute / Work Graph を選ぶ)を通す。伝導の Work Graph 版の見出しは 4 つ(GRAPH_INPUT_CONDUCT_HEADERS。u12 の EXPAND_RECORDS は見出しの後ろ 160 バイトに動いた)。
  伝導の作業場の印は 16 語 / 枠(CONDUCT_MARK_BYTES・CONDUCT_MARK_WORDS)。RecordStepActive は活性のグラフの後すぐにこの刻みの種の一覧を UAV に戻す(起こす一覧に使い回すため)。
  stepFlags の下位 8bit だけが MR_STEP_* のフラグ(上は小刻みの設定)。
- **細かいレベルの熱の刻み(T-0108)**: CPU は multires_conduction.cpp の nest_detail::StepConduction(MarkSubstepBlocks → MarkConductionWants → ExpandForStep →
  ComputeConduction → ApplyEndingDeltas → WakeChangedBlocks)。StepBlocks(multires_nest.cpp)が呼び、最後の小刻みの変化を StepPagedBlock で反応と一緒に足す。
  式は multires_conduction.hlsli の MrSubcycleShift・MrSubstepBegins / Ends・MrSubstepSameLevelFlow・MrSubstepCrossLevelFlow・MrSubcycleBaseLevel(古い MrSameLevelFlow などは shift 0 の包み)。
  CellThermals は 1 刻みに 1 つ作って小刻みで使い回し、変化を足したブロックだけ Invalidate する(multires_nest_internal.h に移した)。
  StepBlocks は stepped を書き足す(span<uint8_t>)・wakeMark(活性の印。全部を刻む時は 0)。起こすのは nest_detail::WakeAround(multires_activity.cpp。種の起こし方から忙しさの印を除いたもの)。
- runner/logs は数が多く、device_bash から `ls -t` すると固まる。`ls -U ... | grep <日付> | sort | tail` で探す。
- **熱の伝導の GPU(T-0107)**: 段は shaders/sim/multires_conduct.hlsl(ConductBegin・ConductMark・ConductPrepare・ConductFlows・ConductApply。中身は multires_conduct.hlsli)と
  multires_tree.hlsl の TreeExpand(配った・凍らせた印)・TreeFractions(端数の枠を枠の順に。伝導の一覧の数を Work Graph の見出しに写す)。呼ぶ順は gpu_multires.cpp の RecordConduction。
  CPU の StepBlocks と同じ順なので、片方を変えたらもう片方も。印の種類は multires_bindings.hlsli の CONDUCT_MARK_*(刻みの印 = MrActivityMark)。
- u6 は [世界の枠の空き][取り合いの印][索引][世界の頁の空き](g_treeWords。番地は TreeClaimAddress・TreeIndexAddress・FreePageAddress)。CPU の木では別々の配列のままで、
  RecordUpload(MakeTreeWordsImage)と Read が詰め替える(取り合いの印は読み戻さない)。
- GPU の入力(u12)の見出しは 頁に広げる一覧 + 伝導の一覧 3 つ(埋める・流れ・足す。同じレコード)。伝導の一覧のレコードは頁に広げる一覧のレコードの後ろ(ConductRecordsOffset。世界の枠の数で動く)。
- 同じリストで RecordUpload を 2 回呼ばない(2 回目はバッファが UAV のまま CopyResource になる)。計測で版を並べる時は版ごとに別のリストにした。
- **熱の伝導(T-0019)**: 式は shaders/common/multires_conduction.hlsli(MrFindFaceNeighbor・MrCellThermal・MrSameLevelFlow・MrCrossLevelFlow・MrSplitCrossFlow)。
  CPU は multires_nest.cpp の nest_detail::StepBlocks(StepNest・StepActive で共有。前の StepPagedBlock / StepBlock を置き換えた)→ multires_conduction.cpp の
  MarkConductionWants(一様なブロックに MR_PAGE_WANTED・端数の印)→ ExpandWantedPages → ComputeConduction(端数の枠を配る・変化の表)→ 変化を足して反応。
  GPU(T-0107)も同じ順。粗いセルの変化は細かい側から足し込む(GPU は 64bit の atomic の足し算。端数の桁上がりも整数なので順に依存しない)。
- 数える欄は 24 個(23 MR_COUNTER_FRACTION_SHORTAGE。GPU は TreeFractions が書く)。
- CpuTree(Tree の約束)は multires_nest_internal.h の nest_detail::CpuTree に移した(活性と伝導が共有)。テストは自分の TestTree を持つ。
- multires_conduction_test は debug で約 3 分(保存量の 256bit の合計は 4 刻みごと)。たくさんの要求の場面の全部を刻む木が重い。
- **取り合いの丸め(T-0106)**: reaction.hlsli の RxSumUsage(要求・消費の合計)・RxShrink(切り捨てと丸め)・RxOverdraws・RxRevokeRoundUps・RxResolveContention(..., randomSeed)。
  反応の結果が変わったので、乱数や取り合いの式を変えると reaction_contention_test の表(期待・切り捨て・丸め)で偏りを見られる。sim のソースで `round` も変数名に使えない(`step` にした)。
- **頁を畳む(T-0103)**: 規則は multires.hlsli の末尾(MrFoldValueCell・MrFoldsCell)と multires_activity.hlsli の MrWantsFoldCheck。CPU は multires_activity.cpp の FoldQuietPages、
  GPU は multires_tree.hlsl の TreeFoldCheck(1 グループ = 世界の枠 1 つ・512 スレッド = セル)→ TreeFold(1 グループの累積和)。gpu_multires.cpp の RecordFoldPages(tick)。
  呼ぶ側の順は FoldQuietPages / RecordFoldPages → SubmitQuietCoarsenRequests / RecordQuietRequests → ProcessRequests(テストの BeginQuietTick・BeginUniformTick・各 GPU テストの RecordTick)。
  数える欄は 23 個(22 MR_COUNTER_FOLDED)。木の管理の段は 10 個(TREE_PASS_COUNT。TREE_SHADERS と static_assert)。
  **取り合いの印(g_claims / nest.claims)は要求の処理の外では全部 MR_NO_CLAIM という約束**を TreeFoldCheck が借りている。要求の処理の外で取り合いの印を使うものを足すなら、畳む段の置き場を変える。
- HLSL の `[numthreads] void A(...) {}` の直後に `[numthreads] void B(` を置くと clang-format が B を字下げする(TreeFillIndex がそう)。間に普通の関数を置くと直る(TreeFold の前の FoldBlock)。
- **一様なブロック(T-0102)**: 規則は shaders/common/multires.hlsli の末尾(MrIsUniform・MrIsCoveredCell・MrUniformCell・MrHasSteppedCell・MrUniformWouldChange・MrSameCell)と
  multires_tree.hlsli の MrCoarsenKeepsUniform・MrCoarsenPageNeed。**セルを読むときは論理のセル**(CPU は LoadNestCell、GPU は LoadBlockCell / LoadCell)。
  頁のセルの番地は CPU は nest_detail::PageCellAt / CellAt(頁を持つ枠だけ)、GPU は PageCellAddress(page, index) = 枠の数 + page × 512 + index(古い CellAddress は無い)。
  一様の値は nest.cells[枠](GPU は g_cells[枠])。頁の空きのスタックは nest.freeBlocks[世界の枠の数 + i](数は MR_COUNTER_FREE_PAGES = 19)。
  MultiresCapacity に pages(世界の頁の数)を足した。テストの MakeMultiresCapacity は pages = 世界の枠の数(不足しない)。
- 刻みの順(活性): 活性のグラフ(ActivityStepNode は一様なら印だけ)→ 観察の枠 → RecordExpandPages(TreeExpand → u12〔T-0107 前は u13〕の後ろの一覧 MR_GRAPH_INPUT_EXPAND_* を
  GPU の入力に ExpandStepNode)。全部を刻む RecordStep は Main → TreeExpand → StepExpanded(世界の枠の数だけグループ)。CPU は StepActive / StepNest の後ろの ExpandWantedPages。
  u12 の一覧の見出しの入口の番号と番地は RecordUpload が書く(MakeGraphInputImage。TreeExpand は数だけ書く)。数える欄は 22 個(19 頁の空き・20 広げた数・21 頁の不足)。
- 累積和 InclusiveScan(multires_tree.hlsl)は 3 本(枠・端数・頁)になった。MrRequestState に pageNeed・pageBase・releasePage を足した。
- **静かなブロックを粗くする(T-0101)**: 規則は shaders/common/multires_activity.hlsli の末尾(MR_QUIET_TICKS・MrIsQuietLeaf・MrWantsQuietCoarsen・MrCellChanged)。
  CPU は multires_activity.cpp の SubmitQuietCoarsenRequests、GPU は multires_tree.hlsl の TreeQuiet(gpu_multires.cpp の RecordQuietRequests。1 グループ 512 スレッド、InclusiveScan を使い回す)。
  1 刻みの順: 外からの要求(RecordRequests)→ RecordQuietRequests(tick)→ RecordProcessRequests → RecordStepActive(CPU は tests/multires_quiet_scene.h の BeginQuietTick)。
  **忙しさの印 busyTick**: つつく所は全部 PokeBlock(CPU は multires_nest_internal.h、GPU は multires_bindings.hlsli。種にして MR_BUSY_POKED を書く)。
  ActivitySeedNode / WakeSeed が POKED を刻みの印に直し、ActivityStepNode / StepActive がセルが変わった時に刻みの印を書く。木をつつく所を足したら PokeBlock を使う。
  全部を刻む StepNest だけの木では POKED のまま(粗くならない)。
- 見出し MrBlock は 88 B(activeTick・busyTick・padding)。HashWholeNest は両方の印を含む。数える欄は 17 QUIET_REQUESTS・18 QUIET_DEFERRED を足した(20 個のまま)。
- テストの場面の要求の一覧は 1 刻みに 1 つの親を 1 つの要求しか取れない: 同じ根の中を同じ刻みに細かくすると後ろは取り合いで落ちる(場面は空気の点を 1 刻みに 1 つずつ出す)。
- **木の上の活性(T-0100)**: 規則と面の隣は shaders/common/multires_activity.hlsli(MrWakeAcross・MrNearOctant・MrFindContaining。Tree の約束 = Block・Lookup)。
  CPU は engine/src/sim/multires_activity.cpp(StepActive。種 nest.seeds は世界の枠ごとの 0 / 1)、GPU は shaders/sim/multires_activity_graph.hlsl
  (ActivitySeedNode → WakeFaceNode〔スレッド起動・再帰 MR_MAX_WAKE_DEPTH 28〕→ ActivityStepNode、観察の枠は ObserverStepNode)と gpu_multires.cpp の RecordStepActive。
  **活性のグラフは `GpuMultires::Create(..., {.activity = true})` の時だけ作る**(反応の核を含んで大きく、debug の GPU-based validation で 1 回 2〜3 分。
  木の管理のグラフに入れたら gpu_probe_peek が 300 s を超えて落ちた)。
- 種の一覧(u13。T-0107 前は u14)は 2 本を刻みごとに入れ替える(m_activityCurrent)。要求の処理はこの刻みの一覧へ(細かくした親と子・粗くした親をつつく)、刻むノードは次の刻みの一覧へ書く。
  先頭は D3D12_NODE_GPU_INPUT そのもの + 予約の数 + 落とした数、レコード 0 は空(MR_NO_BLOCK)。RecordStep(全部を刻む)だけを使う所では一覧が空にされず、一杯になると落とすだけ(害はない)。
- **多重解像度のルート署名は UAV 14 個(u13 活性の一覧)・ルート定数 24 個で 62 / 64 語**(T-0107 で u6 にまとめた)。あと 2 語。
- 見出しの padding は activeTick になった(刻みの印 = 刻み + 1)。HashWholeNest は印を含み、HashRealLeaves・HashBlock は含まない。数える欄は 20 個(15 刻んだ数・16 再帰の上限で止まった数)。
- テストの場面は tests/multires_activity_scene.h(根 4×4×4・木箱の周りを T-0018 のたくさんの要求で・影 4 段)。総当たりの面の隣は同じファイルの BruteForceScheduled。
  CPU のテストの 2 回目は比べる相手と総当たりを省いている(debug で遅いので)。計測は根 8³ で、WARP では測らない(暖機が遅すぎる)。
- **多重解像度の木の管理(T-0018)**: 約束は shaders/common/multires_tree.hlsli(要求 MrRequest 40 B・途中の値 MrRequestState・索引の番地・帳簿)。
  CPU は engine/src/sim/multires_tree.cpp(ProcessRequests = Resolve → Settle → Allocate → Apply → ReleaseAll → RebuildIndex)、
  GPU は shaders/sim/multires_tree.hlsl の 6 段(.cso は multires_tree_<snake>)と multires_graph.hlsl(RefineNode は手で決めた影の鎖〔request = MR_NO_BLOCK〕と
  要求の鎖の両方・CoarsenRequestNode)。呼ぶ順は gpu_multires.cpp の RecordProcessRequests。GPU の入力は u12(見出し 2 つ + レコード。割り当ての段が書く。
  DispatchGraph の間だけ NON_PIXEL_SHADER_RESOURCE。レコード 0 件にしないため何もしないレコードを 1 件)。
- 数える欄は 20 個(MR_COUNTER_*。T-0100 で 15・16 を足した)。旧 MR_COUNTER_FRACTION_BLOCKS は無い(端数の枠の空きの数 MR_COUNTER_FREE_FRACTIONS)。要求の数 MR_COUNTER_REQUESTS は
  RecordRequests が CopyBufferRegion で書き、解放の段が 0 に戻す(一覧が空の時に送る)。RecordRequests の写しは 1 本のリストで 16 回まで(REQUEST_UPLOAD_SLOTS)。
- **GPU の時間は、CPU の重い処理の後に測ると暖機しても 4〜6 倍に出る**(GPU が長く空いてクロックが下がる)。gpu_multires_test は計測をたくさんの要求の前に置いた。
- MultiresNest は MakeMultiresNest(MultiresCapacity) で作る(世界の枠・観察の枠・端数・索引〔2 の冪〕・帳簿の列〔1 + 物質の数〕・根のレベル)。根は PlaceRootBlock(空きから取る)。
  覗き窓は世界の枠 0(観察の枠だけ)。GPU のバッファは 0 の大きさを作れないので最低 16 B。
- device_bash から `job.py` を `timeout` で切っても、ランナーのジョブは `--timeout` まで走り続ける。長いテストは `--timeout 290` で投げて、runner/logs/<job>.result.json を待つ。
- **6×6 のウェーブの解(T-0095)**: shaders/sim/physics_solve6_wave.hlsli。レーン t < 21 が下三角 (r, c)(t = r(r+1)/2 + c)、21〜26 が右辺。CPU の PxSolveSymmetric6 と
  要素ごとの式を揃えるため、共有の部品(PxDiagonalShift・PxCholeskyRadicand・PxCholeskyRoot・PxRhsTopBit。physics_math.hlsli)を両方が使う。片方を変えたらもう片方も。
  使う条件は physics_bindings.hlsli の CanSolveInWave(ウェーブ 32 以上かつ SOLVE_GROUP_THREADS〔64〕以下)。中は WaveReadLaneAt だけで、ウェーブの全部のレーンが一様に呼ぶこと
  (途中で return しない)。sim のソースで `round` も変数名に使えない(浮動小数点の検査が落とす。`digit` にした)。
- **述語(T-0094)**: `GpuPhysics` の u13(色ごとの「その色の物がいる」、uint64 × 16)は BeginSubstep が UAV で 0 にし、FinishColoring が書き、
  色ごとの解に使う間だけ PREDICATION(gpu_physics.cpp の RecordSubstep・RecordColorPredicateStates・RecordSolveColors)。リストの終わりで COMMON に戻り、
  次のリストの BeginSubstep で UAV に昇格する(だから小刻みの初めに必ず UAV で触ってから PREDICATION に移す)。述語と DispatchGraph は組み合わせない
  (述語をかける色ごとの解は Compute。SkipsEmptyColors)。`SetPredication(nullptr, ...)` で戻すのを忘れない(後ろのパスが黙って飛ぶ)。
- 物理のルート署名は UAV 14 個(u13 述語)・ルート定数 14 個で 48 / 64 語(T-0099 で詰めた)。足すときは BindRoot の static_assert(定数の大きさ・UAV の数)も合う。
- 島の方式(T-0094)は T-0099 で消した。もう一度試すなら git の 6c8dee9 から(physics_islands.hlsli・gpu_physics_islands.cpp)。
- **前のチャットのテストの exe が残っていると、build が `LNK1104: cannot open file 'bin\gpu_physics_test.exe'` で落ちる**(ctest の timeout で切れた後に残った)。
  `job.py raw` で `Get-Process gpu_*` を見て、古いものを `Stop-Process` してから build し直す。
- 前のコミットで archmap が落ちていた(map.yaml の gpu_probe_physics_test の RunGpu → `GpuRun::Execute` に直した)。
- **物理の GPU のパスで組(PxManifold 3 KB)を局所の変数に持たない**(T-0098)。持つと、別のキュー(窓の描画)が割り込んだ時だけ結果が時々変わり、
  火(伝導の Work Graph)と重なると GPU が固まった。テスト(描画なし)では出ない。gpu_probe_physics_test の 2 回目が優先度の高い direct のキューで雑音を流して確かめる。
  GPU の BuildSlot・Recollide・PrepareManifolds は CPU の PxBuildManifold・PxRecollideManifold・PxWarmStartManifold と同じ手順をバッファの上で書いた別の実装。片方を変えたらもう片方も。
- **仮の世界の物理(T-0098)**: ProbeSim の単位 [2] = ProbeSim::RecordPhysics(GpuPhysics::RecordStep の後にルートを結び直す)。物のバッファは仮の刻みの u13
  (物理なしなら重さの捨て場を仮に結ぶ。物の数はフレームの入力の見出し PROBE_HEADER_BODY_COUNT)。押すは probe_tick.hlsl の ApplyPush、
  物のハッシュは FlushEvents の中の StoreBodyHash、抽出の物の欄は StoreBodyView。描画は probe_view.hlsl の LoadBodyView・IntersectBodies・BodySliceColor。
- 窓の debug は物理のパイプラインの作成に約 60 s(window_replay_* の TIMEOUT を 240 にした)。release は初回約 20 s、以後はドライバのキャッシュで約 1 s。
- **覗き窓(T-0096)**: sim/probe_peek(ProbePeek・CPU リファレンス ProbePeekReference・状態 PeekState)。GpuMultires のルート署名に u4・u5(外のバッファ)と
  外の定数 4 語を足し、`SetExternalViews` + `RecordExternalDispatch` で shaders/sim/multires_peek.hlsl(MirrorWorld・ExtractShadow)を投げる。
  ProbeSim は `ProbeFrameInput::afterExtract` を抽出の後(UAV バリアの後)に呼ぶ。抽出の覗きの欄の並びは probe_sim.hlsli(PROBE_EXTRACTION_PEEK_*)、
  Extract が段の数 0 を書き、覗き窓が後ろで書き直す。描画は probe_view.hlsl の PeekSliceColor・OnPeekSliceFrame・PeekBoxEdges。
  潜った段は ProbeViewConstants の flags のビット 16〜19(VIEW_PEEK_DEPTH_SHIFT)。段 k のブロックは「点を含む 8 の倍数に揃ったブロック」(カメラの寄せ先。FocusOnPeek)。
  断面の位置は軸の座標をちょうど面に置き直している(細かい段では面がセルの境目に来るため)。
- 別のリストで GpuMultires の入れ子を読み戻すときは注意: `RecordReadback` は UAV → COPY_SOURCE の遷移を書くので、バッファが UAV にいるリストの中で呼ぶ
  (gpu_probe_peek_test はフックの中で呼んでいる。リストの終わりで COMMON に戻る)。
- gpu_probe_peek_test の GPU 時間の差は暖機していないので 7 倍に出る。覗きの費用は窓の要約(シミュ GPU ms/投入)で比べる。
- **WARP(T-0097)**: 使うのは exe の横の d3d10warp.dll(NuGet 1.0.21)。版を上げるときはルートの CMakeLists.txt の `BICAMERAL_WARP_VERSION` と `BICAMERAL_WARP_SHA256` を両方変える
  (取ってくるのは configure の時。`_deps/warp-<版>` が無ければ取る)。ログに Warning「WARP が exe の横の版ではない」が出たら OS の WARP を読んでいる。
  NuGet の版の一覧には `1.65535.20-preview` もあるが、1.0.21 より古いプレビュー(版の数字が大きいだけ)。
- **多重解像度(T-0017)**: 式は shaders/common/multires.hlsli(Mr 接頭辞。C++ は bicameral::multires)。CPU リファレンスは engine/src/sim/multires_nest(ライブラリ bicameral_multires)、
  GPU は sim/gpu_multires(Work Graph shaders/sim/multires_graph.hlsl の RefineNode・CoarsenNode・PullBackNode・RemoveShadowNode と Compute の multires_step.hlsl)。
  1 段の操作は CPU と GPU で同じ順(セルを全部書いてから見出し)。端数の枠は要求ごとに割り当ての段が配る(T-0018。スレッド 0 だけが見出しと返す枠を書く)。
  次のレベルが前のレベルの書き込みを読むので、ノードの RW バッファは globallycoherent・出力の前に `Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC)`。
  場面は tests/multires_test_scene.h(根 = 枠 0、本物の鎖 = 枠 1〜9、影の鎖 = 枠 10〜18、点のセル (3,4,5) は 600 K の木箱)。
- **HLSL で `point` は予約語**(multires.hlsli でも踏んだ。`coordinate` にした)。
- **GPU の時間は暖機してから測る**: 短い投入ばかりだと GPU のクロックが上がらず、全部が 6〜8 倍に出る(T-0017 で 16 ms → 2.3 ms)。gpu_multires_test は刻みを 400 回投げてから測る。
- 細かくした子は親と同じ数で始まるので、反応で子どうしが違わないと端数は出ない(900 K の木箱は O2 を使い切って止まり、端数が 0 だった)。
- **Nsight(T-0093)**: ngfx は `$env:ProgramFiles\NVIDIA Corporation\Nsight Graphics 2025.2.0\host\windows-desktop-nomad-x64\ngfx.exe`。ランナー(非管理者)から動く。
  性能カウンタは NVIDIA コントロールパネルの「すべてのユーザーに GPU パフォーマンスカウンタへのアクセスを許可」(2026-10-03 にユーザーが有効にした)。無いと「GPU Performance Counters Unavailable」。
  ngfx のヘルプ(`--help-all`)は UTF-16 で出る。書き出しの .xls は ASCII のタブ区切り(UTF-16 で読むと化ける)。1 回の記録は 7〜30 秒。
  記録は投入(ExecuteCommandLists)単位で `--start-after-submits`・`--limit-to-submits` で選ぶ(窓の無いテストでも使える)。
- **診断の作法(T-0093)**: シェーダーの一部の費用は「同じ計算をもう 1 回させて結果を捨てる(使ったことにするため、ありえない値のときだけ統計に足す)」で測った。ハッシュが変わらないことを確かめる。コミットしない。
- gpu_fixed_bench の `--iterations` を付けると調整なしで 5 回だけ投げる(投入 0〜4)。`--groups 1` は 1 グループ(8 ワープ)で、1 歩の時間 ≈ 1 本の連鎖の待ち時間。
- **物理の GPU の構成(T-0092)**: 小刻み = BeginSubstep → **Work Graph(physics_graph.hlsl の BroadphaseNode → NarrowphaseNode)** → BuildManifolds(Compute)→ PrepareManifolds →
  彩色 → 反復(探し直し・**SolveColor = 1 グループ 64 スレッド = 1 物**・UpdateDuals)→ Finish。呼ぶ順は gpu_physics.cpp の RecordSubstep / RecordIterations。
  バッファとルート定数と共有の関数(CollectCandidates・CollideGeometry・BuildSlot・SolveBodyInGroup・FindPrevious)は shaders/sim/physics_bindings.hlsli(Compute とグラフで共有)。
  比べた方は `GpuPhysicsOptions{.broadphaseGraph = false}`(Compute の Broadphase・Narrowphase)と `.solver = GpuPhysicsSolver::Graph`(SolveBodyNode。色ごとの物の一覧 u12 と GPU の入力 u11 を FinishColoring が作る)。
- **Work Graph のノードに大きい局所の変数を置かない**(約 5〜6 KB で GPU が固まる。組 PxManifold は 3 KB)。前の組は `PxBuildManifold` の `Previous` 型で点を 1 つずつ読む(CPU は PxPreviousManifold、GPU は GpuPreviousManifold)。
  固まった時の切り分けは、ノードの中身を少しずつ足して release で `--ticks 60` を走らせるのが早かった(debug の DRED は DispatchGraph で止まったとしか言わない)。
- **ブロードキャストのノードの出力は 1 グループ 256 件まで**(超えると CreateStateObject が E_INVALIDARG。広域の選別は 16 スレッド × 枠 16)。
- 計測の名前の表(gpu_physics.cpp の PASS_NAMES・SHADER_NAMES)は Pass の順。static_assert で数を確かめている(clang-format が並べ直すので、置き換えの編集は崩れやすい)。
- 色ごとの GPU の入力(u11)と物の一覧(u12)は、反復の間だけ NON_PIXEL_SHADER_RESOURCE(solver = Graph のとき。RecordColorListStates)。
- **物理の GPU(T-0090)**: パスは shaders/sim/physics_step.hlsl(入口 13 個 → `physics_<snake>.cso`。shaders/CMakeLists.txt の foreach)、呼ぶ順は gpu_physics.cpp の RecordSubstep
  (PhysicsWorld::Substep と同じ順に揃える。片方を変えたらもう片方も)。物・組・点の構造体は physics_step.hlsli で、C++ と HLSL の並びを揃えるため 64bit を 8 の倍数の位置に置き bool を使わない
  (大きさは physics_world.h の static_assert: 物 448・点 368・組 3016・パラメータ 96)。組は構造化バッファの要素の上限 2048 B を超えるので GPU では見出し(u1)と点(u9)に分けている。
- **gpu_physics_test は GPU-based validation を切っている**(debug の -Od のシェーダーの計装でパイプラインの作成が数分を超えた)。debug layer は有効。
  パイプライン 12 個の作成は debug で ctest の下だと約 60 s かかる(job.py run だとドライバのキャッシュで数秒のことがある)。パイプラインは 1 回だけ作って 2 回の実行で使い回している。
- HLSL で `point`・`half` も変数名に使えない(`point` はジオメトリシェーダーの修飾子、`half` は型。浮動小数点の検査も落とす)。physics_step.hlsli では `contactPoint`。
- 物理の GPU は区間の数(色・彩色の回数)を読み戻さずに固定の数だけ投げる。統計の overflow(gpu_physics.h)が 0 でなければ結果は信用できない。
- ランナーが止まっていたら、画面操作で起動できる: エクスプローラーで H:\BicameralEngine\.bicameral-runner\start-runner.cmd をダブルクリック(git 管理外。PowerShell の窓が開く)。
  エクスプローラーと PowerShell は「見る・左クリック」だけの許可なので、打鍵はできない。Google ドライブの窓が手前に来ると操作が止まるので、それも許可に入れる。
- **物理の試作を Linux で速く回す**(T-0091): device_bash の Linux に g++ 11 がある。tools/physics_lab の avbd_solver.cpp・box_collision.cpp と
  engine/src/sim/physics_scene.cpp(整数版なら physics_world.cpp も)を `g++ -O2 -std=c++20 -I tools/physics_lab -I engine/src -I shaders` で、小さな main と一緒にビルドできる
  (physics_lab.cpp は <format> が g++ 11 に無いので使わない)。double の山 1 種 3 s、2 コアで種 48 個を 80 s。整数版のハッシュは MSVC と一致する。
  山はカオスなので、基準の判定は種 1 個ではなく種 24〜48 個の割合で比べる(1 個だと ±数個ぶんの揺れで見誤る)。
- 物理の点は最大 8(生成 4 + 途中の探し直し 4)、法線は点ごと(`PhysicsContactPoint::normal`)。GPU に載せる時は探し直しが反復の途中のパス 1 つになる。
  探し直しは組を増やさない(増やすと彩色が変わり、同じ色の物が拘束を共有して並列に解けない)。
- **物理(T-0016)**: 式は shaders/common/physics_math.hlsli(ベクトル・四元数・128bit の平方根・6×6)・physics_collision.hlsli(直方体の接触)・
  physics_solver.hlsli(行・6×6 の組み立て・パラメータ PxDefaultParameters)。呼ぶ順は engine/src/sim/physics_world.cpp。試作 tools/physics_lab は同じ手順の double。
  **試作と整数版は手順を揃えてある**ので、片方を変えたらもう片方も変える(physics_lab の --integer で並べて比べる)。
- 物理の名前: 行列の行は `PxMatRow`(`PxRow` は拘束の行の構造体)。0 で埋めるのは `PX_ZERO(型)`(C++ は `型{}`、HLSL は `(型)0`)。
  HLSL の `half` は型名なので変数名に使えない(浮動小数点の検査が落とす)。`halfAngle` などにする。
- release で FX_ASSERT を効かせたいときは `BICAMERAL_FORCE_ASSERT=1` を定義する(fixed.hlsli。physics_checked_test がそう)。
- clang-tidy の NestingThreshold は 3(`{}` の入れ子)。ループを 3 重にするときは内側を `{}` なしにするか関数に分ける。HLSL 共通のファイルでは範囲 for が書けないので、
  添字を他にも使う形にするか PX_ZERO を使う(modernize-loop-convert が出る)。
- **仮の世界のセル**(T-0089): `cells`(u0、RxCell × 2 世代)と `thermal`(u12、HcThermalCache × 2 世代)。キャッシュはセルから決まる値という約束
  (ProbeStepCell の近道と、コンダクタンスを成分が変わった時だけ作り直すのがこれに頼る)。セルを書き換えたら必ず `ProbeMakeCache` で作り直す(つつきがそう)。
  反応の表は t1〜t4(既定のヒープ)。初めの世界と表は最初の RecordFrame で写す(`ProbeSim::RecordInitialization`)。
- **ProbeSim::Create は反応の表を取る**(`BakeReactionTable(MakeCombustionTestTable())`)。ProbeReference も表を取る。
- **WARP の Work Graph の JIT は、ノードの関数が少し複雑になると落ちる**(ADR-0013)。compute なら同じコードが動く。
- 抽出のセルの部分は `PROBE_EXTRACTION_BLOCK_OFFSET`(= セル × 4)語。CPU 側は `MakeProbeExtractionCells` で同じものを作れる。
- HLSL で `linear` は補間の修飾子(`line`・`point` と同じく変数名に使えない)。
- `shaders/common/probe_world.hlsli` は C++ では bicameral::sim の中で reaction と fx を using する。

- **反応の核**(T-0014、02 §3.1): 評価は `shaders/common/reaction.hlsli`(テンプレートの Table 型で表を読む。C++ は `sim::ReactionTableView`、
  HLSL は `reaction_cells.hlsl` の `GpuReactionTable`)。ベイクは `sim/reaction_table.cpp`、試験の表は `sim/reaction_test_table.cpp`(ライブラリ bicameral_reaction。GPU を知らない)。
  表の構造体の大きさは reaction_table.h の static_assert(RxSpecies 24・RxRule 80・RxCell 112 バイト)。HLSL の構造体を変えたら揃える。
- **HLSL の `?:` は構造体を返せない**(「conditional operator only supports results with numeric scalar...」)。if / else で。
- **WARP で GPU-based validation を有効にすると、大きなシェーダー(reaction_cells)の計装と JIT が 15 分を超えて終わらない。** gpu_reaction_test は WARP だけ GBV を切る。
  WARP は最初の Dispatch で JIT に約 8 秒かかる(以後は 20 ms)。
- 小数点のリテラル(`1e6` も)と `frac`・`floor`・`exp` などの名前はシミュのソースで禁止(浮動小数点の検査)。100 万は `1000000` と書く。
- release では FX_ASSERT が空になるので、assert の中だけで使う局所変数を作ると C4189(未使用)になる。式を FX_ASSERT の中に書く。
- **2026-09-30〜10-01 にコーディング規約を変えた(docs/style.md)。** 全コードを書き直し済み(ビルド・tidy 警告なし・テスト 28/28・archmap OK)。
  - 中身が 1 行の if / else / ループは `{}` なしで改行 + 字下げ(同じ行に書かない)。連鎖は分岐ごと(1 行の分岐だけ `{}` なし)。
    clang-format は `{}` を付け外ししない(RemoveBracesLLVM は入れ子の if を 1 文と見て外すので使わない)。
    clang-tidy の readability-braces-around-statements は外した(連鎖の混在を許さないため)。**`{}` の付け方は手で守る。**
  - 処理の区切りに空行(style.md「空行」の目安)。function-size の行数は空行も数えるので 70 にした。
  - 短い名前: `fs::` `rng::` `views::` `chr::`(core/aliases.h)・`ComPtr`(gpu/com_ptr.h)。使うファイルで include する。
    std 直下(`std::optional`・`std::span`・`std::format` など)は `std::` を付ける(10-01 に戻した)。ほかの長い名前空間はそのファイルの中だけで短くする。
  - 宣言は `=` の直後で折り返さない(.clang-format の PenaltyBreakAssignment)。行末コメントで収まらなければコメントを上の行へ。
  - 構造体・クラスの欄はまとまりごとに空行 + `// --- 〇〇 ---`。`// clang-format off` の範囲(HLSL のノード)も手で同じ見た目に揃える。
- `job.py test` を全部走らせると 3 分を超える(gpu_probe_sim が約 60 秒)。device_bash の 180 秒と `--timeout` で途中で切れるので、
  `-Filter` で分けて走らせる(例: `gpu_probe_sim` / `window_replay|gpu_debug_device` / `float_check` / 残り)。
- **同じコマンドリストを、キューのフェンスが前の実行を越える前に投げ直さない**(debug layer [553]。debug layer はその実行を捨て、release は黙って走る)。
  だからシミュのリストは ProbeSim のフレームの枠ごとに毎フレーム記録し直す(枠の前のリストが終わるまで、その枠は使わない)。`gpu::Queue::Execute` はフェンスを進めずに投げる。
- `job.py run` で窓のループが固まると、run の timeout(600 秒)まで runner が塞がる。試すときは `job.py run --timeout 60 -Preset ...` にする。
- **GPU のテストを足すとき**: tests/CMakeLists.txt の `bicameral_add_gpu_test(<名前>)` と `bicameral_add_gpu_test_case(<テスト名> <exe> <gpu|warp> <引数>)`。
  exe は bin/ に出て、bin/D3D12(Agility SDK)と bin/shaders をそのまま使う(agility_sdk.cpp を exe ごとに入れている)。
- テストの出力は printf ではなく Log(日本語をそのまま出せる)。終わりに `SingletonFinalizer::Finalize()`。
- **HLSL のノードの属性は clang-format が崩す**ので、work_graph_probe.hlsl のように属性つきの宣言を `// clang-format off/on` で囲む。
- Work Graph の起動順: SetComputeRootSignature → WorkGraph::SetProgram(最初だけ initialize=true)→ SetComputeRoot* → DispatchFromCpu。
- バッファは COMMON で作り、最初の UAV の使用で暗黙に昇格させている。読み戻しは gpu::RecordCopyToReadback(UAV → COPY_SOURCE)。
- archmap は CMake の `function(名前` も定義として見つける(T-0013 で追加)。C++ のメンバー関数は `Class::Method` で書く。
- **シミュのコードに `sqrt`・`pow`・`floor`・`lerp` などの名前の変数や関数を作らない**(ソースの検査が拒否する)。FxSqrt のように接頭辞を付ける。
- 描画だけの共通の HLSL は shaders/common ではなく shaders/render/ に置く(common は浮動小数点禁止)。
- **ベンチの演算を足すとき**: shaders/CMakeLists.txt の BICAMERAL_BENCH_OPERATIONS・fixed_bench.hlsl の BENCH_OP の分岐・
  tests/gpu_fixed_bench.cpp の BENCH_OPERATIONS・tools/fixed_bench/dxil_count.py の ORDER を同じ順で直す(番号 = 並び順)。
  定数の除数や、前の結果に依存しない連鎖はドライバがまとめて消すので、値は実行時のものにして前の結果に依存させる。
- **fixed.hlsli の約束**: 64bit の定数は `FX_U64(上位, 下位)`、関数は `FX_FN`、定数は `FX_CONST`。桁あふれしうる計算は符号なしで。
  自己テストに関数を足したら `FX_SELF_TEST_OUTPUT_COUNT` を増やす(要約 85c154e666febd92 が変わる。gpu_fixed_test も自動で追従する)。
- `job.py tidy` は debug のビルドフォルダの compile_commands.json を使う。先に `job.py build`。
- tools/dxil_float_check/ という空のフォルダが PC に残っている(削除の許可が無く消せなかった。git には入らない)。
- **公開リポジトリ。** 鍵(.bicameral-runner/)・CLAUDE.local.md・PC 固有のパスをコミットしない。ゲームの中身も公開側に入れない(D-006)。
- Linux 側の整形: `python3 -m pip install --user clang-format==23.1.1 pyyaml` → `~/.local/bin/clang-format -i`(セッションごとに入れ直し)。
- `.github/` 以下は device_commit_files では書けない(保護)。device_bash の cp や python なら書ける。
- runner の結果 JSON(runner/logs/*.result.json)を cat しない。job.py の要約か .log を grep する。
- 外部コマンドを呼ぶ .ps1 で `$ErrorActionPreference='Stop'` にしない(PS 5.1 は stderr の警告で止まる)。.ps1 は UTF-8(BOM 付き)+ CRLF。
  ただし `job.py raw` に渡す .ps1 は本文が埋め込まれるので BOM を付けない。
- Windows の Python がパイプに書く文字は CP932 になる。ビルドの中で呼ぶ Python は `sys.stdout.reconfigure(encoding="utf-8")` する。
- **fixed.hlsli の割り算を変えるとき**: 先に 64bit の剰余演算を Python で真似て突き合わせると早い(T-0084 はそうした)。fixed_test の `TestDivide128` が _udiv128 と境界まで比べる。
- **デバッグのリングを使うシェーダー**: ルート署名を `gpu::CreateRootSignature(device, {.uavCount = n, .debugRing = true})` で作り、
  `ring.RecordBegin(list)` → `SetComputeRootUnorderedAccessView(layout.DebugRingIndex(), ring.GpuAddress())` → 書く → `ring.RecordReadbackAndReset(list)` → 待つ → `ring.Drain()`。
  fixed.hlsli を使うシミュのシェーダーは Debug で FX_ASSERT がリングを使うので、必ずリングを結ぶ(結ばないと PSO / 実行が壊れる)。
- 書式を足すときは shaders/common/debug_formats.hlsli に `DEBUG_FORMAT(名前, チャンネル, 場所, 書式)` を 1 行。HLSL では `DebugFormat::名前`。
- **HLSL で `line` は予約語**(ジオメトリシェーダーの修飾子)。変数名に使うと「modifiers must appear before type」になる。
- **HLSL で `point` も予約語**(`line` と同じくジオメトリシェーダーの修飾子)。変数名に使うと「modifiers must appear before type」。
- デバッグ表示の定数は engine/src/render/probe_view_constants.h(96 バイト)と shaders/render/probe_view.hlsl の LoadConstants を揃える。
  光線の式は debug_camera.cpp の RayThroughPixel と probe_view.hlsl の MakeRay で同じにする(ずれるとクリックした所と見えている所が合わない)。
- `render/debug_camera`・`render/debug_view_controller` は bicameral_view(D3D12 を知らない)。D3D12 の型を持ち込まない(debug_camera_test が GPU なしで動くように)。
- 抽出の大きさを変えたら gpu_probe_sim_test の ReadExtraction も変える(PROBE_EXTRACTION_WORDS)。
- debug layer は、デバイスを作った後に有効にするとデバイスが失われる。`gpu::Device::Create` は必ず最初のデバイスより前に設定する(1 プロセス 1 回の想定)。
- `DebugRing` と `ReadbackRing` の読み戻しは枠(slot)ごと。`Create(device, slotCount)` → `RecordReadbackAndReset(list, slot)` → その枠のリストが終わってから `Drain(max, slot)` / `Read(slot, ...)`。
- **フレームのループ(frame/frame_loop.cpp)**: 抽出の 3 組の約束はファイルの先頭に書いた(抽出 n は、終わっている抽出が n − 2 以上のときだけ)。
  抽出の組の数を変えるときはこの条件も変える(破ると描画とシミュが同じ抽出を同時に使う)。
- **バックバッファを作り直す前・終わる前は `Queue::Flush()`**(最後の Submit の値を待つだけだと、その後ろの Present がまだ走っていて debug layer が CORRUPTION を出す)。
- 仮の刻み(sim/probe_sim・shaders/sim/probe_tick.hlsl・probe_conduct.hlsl・shaders/common/probe_sim.hlsli)の中身(64³ の熱の伝導)は段ごとに本物に置き換える前提。
  形(単位の列・単位ごとのタイムスタンプ・刻みの最後のハッシュ・64 バイトのコマンド・枠ごとの入力・イベントのリング・GPU の入力の活性の一覧)は本物にも使う。
  単位を足すときは probe_sim.hlsli の単位の表・ProbeSim::RecordUnit・UnitsPerTick を揃える。ハッシュの単位は必ず刻みの最後(次の刻みの適用より前の S(t+1) を取る)。
- ハッシュの表の読み戻しは、そのフレームにハッシュの単位があるときだけ(無いのに UAV → COPY_SOURCE を入れると状態が合わない)。ProbeSim::RecordReadbacks。
- HLSL の `[numthreads] void F(...) {}` が続くと clang-format が次の行を字下げして崩す。probe_tick.hlsl は入口の範囲をまとめて `// clang-format off/on` で囲んだ。
- HLSL の 64bit の剰余は避ける(表の番号は下位 32bit を 2 の冪でマスク)。WaveActiveSum は uint64_t で使える(DXC・RTX 3070 Ti・WARP で確認)。
- `job.py run` は窓を開く(ユーザーの画面に出る)。自動の確認は `--frames n --auto-click` で終わらせる。
- clang-tidy: `std::optional` のメンバーは unchecked-optional-access で大量に警告が出る。作れたものだけを受け取る形(ProbeSim・FrameLoopParts)にする。
- HLSL の `[numthreads(...)] void Name(` は archmap が定義として見つけない。map.yaml ではその .hlsl の普通の関数を指す。
- **コマンドを足すときの約束**(ProbeSim::RecordFrame が確かめる): (targetTick, sequence) の昇順・targetTick ≥ `NextApplyTick(カーソル)`・数 ≤ `FreeCommandSlots()` かつ 256。
  CPU の控え(ProbeSim の m_commandTail・m_queuedTicks)は「記録したら GPU で実行される」前提。記録したリストを投げなかった場合は控えがずれる(今は失敗 = 終了なので問題なし)。
- probe_sim のルート署名は UAV 12 個(u7 コマンドキュー・u8 刻みのイベントの一時置き場・u9/u10 活性の一覧・u11 予定の印)。
  バッファの結び方は shaders/sim/probe_bindings.hlsli(compute と Work Graph で共有)。イベントのリングの見出しは [0] 書こうとした数 [1] 一時置き場で落とした数。
- **GPU の入力の DispatchGraph**: 入力(見出しとレコード)は NON_PIXEL_SHADER_RESOURCE か COMMON でなければならない。活性の一覧はフレームの始めに
  COMMON → UAV、伝導の前後だけ入力の組を UAV ⇄ NON_PIXEL_SHADER_RESOURCE、フレームの終わりに UAV → COMMON(ProbeSim::RecordConduct)。
  **レコードを 0 件にしない**(WARP が固まる。一覧の先頭は必ず PROBE_NO_BLOCK)。SetProgram の後にルートの引数を結び直している(念のため)。
- **スレッド起動のノードの出力**: 局所の配列に集めて `GetThreadNodeOutputRecords(n)` で一括に出すと WARP で予定がずれた。展開したループで 0 件か 1 件ずつ出す(probe_conduct.hlsl)。
- GPU のテストの ctest の TIMEOUT は 300 秒(固まったときに runner を 25 分塞がないため)。`job.py test` を device_bash から呼ぶときは
  `timeout 170 python3 ~/job.py test --timeout 160 ...` にする(device_bash は 180 秒で切れ、ジョブは runner で走り続ける)。
- `--record` / `--replay` の相対パスは runner の作業フォルダ(bin/)から。ctest の window_replay_* は `out/build/<preset>/tests/window_replay.bcreplay` を使う。
- 窓の ctest(window_replay_*)は画面に窓が 2 回出る(約 6 秒ずつ)。CI はラベル gpu なので走らない。
- **Work Graphs のカウンタ**(T-0008、16 §1.2): ルート署名に `.graphStats = true`(u1 space1。`GraphStatsIndex()`、SRV の番号が 1 つずれる)。
  `gpu::WorkGraphStats::Create(device, layout, slotCount)` → `RecordBegin` → `SetComputeRootUnorderedAccessView(layout.GraphStatsIndex(), stats.GpuAddress())`
  → 数える → `RecordReadbackAndReset(list, slot)` → 終わったら `Read(slot)` → `Report`。HLSL のノード番号と `GraphStatsLayout` の並びを揃える
  (伝導は probe_sim.hlsli の `PROBE_STATS_*` と probe_sim.cpp の `MakeConductStatsLayout`)。ブロードキャストのノードは `WgCountLaunch` をグループの 1 スレッドだけで呼ぶ。
- ウェーブの関数(WaveActiveSum など)はスレッド起動のノードの中でも動く(HW・WARP で確認)。
- **windows.h の `near`・`far` はマクロ**。変数名に使うと意味の分からない構文エラーになる。
- job.py の build の要約に `add_custom_command(TARGET main POST_BUILD` が出るのは、vcpkg の使い方の表示(構成し直した時)。エラーではない。
- **連鎖のトレース**(T-0087、16 §1.3): ルート署名に `.graphTrace = true`(u2 space1。`GraphTraceIndex()`、SRV の番号がさらに 1 つずれる)。
  `gpu::GraphTrace::Create(device, filter, slotCount)` → `RecordBegin`(最初だけ範囲を写すので非 const)→ `SetComputeRootUnorderedAccessView(layout.GraphTraceIndex(), trace.GpuAddress())`
  → 書く → `RecordReadbackAndReset(list, slot)` → `Read(slot)` → `gpu::SortGraphTrace`。HLSL の `GtReserve` はウェーブの全部のレーンが呼ぶ(書かないレーンは 0 件)。
  伝導の記録の種類は probe_sim.hlsli の `PROBE_TRACE_*`、CPU の予想は sim/probe_trace の `AppendExpectedProbeTrace`(範囲の判定を GPU と同じにする)。
- `std::map` を持つ構造体を値で返すと clang-tidy の bugprone-exception-escape(ムーブが noexcept でない)。呼ぶ側の物に書く形にした(probe_trace.cpp の BuildTickTree)。
- python の `"""` の中に C++ の `"\n"` を書くと本物の改行になる(heredoc の python で編集するとき)。`<<'EOF'` の cat で書くか、`\\n` にする。
- gpu_conduct_bench は `--trace` を自分で取り除いてから gpu_test_options.h に渡す(知らない引数で止まるので)。
- **トレースの範囲を変えるとき**(T-0088): `GraphTrace::SetFilter` / `ProbeSim::SetTraceFilter` は次の `RecordBegin(list, slot)` から効く。
  容量は `Create` の capacity(`ProbeSimOptions::traceCapacity`)までに切り詰める。ランタイムは `frame::TRACE_CAPACITY_PER_FRAME`(65,536)をいつも確保。
  `RecordBegin` に slot を渡す(slot ごとのアップロードと、読むときの範囲)。
- exe の横のフォルダは `core/paths.h` の `ExecutableDirectory()`(ログ・シェーダー・トレースの既定の置き場所)。
- 一度だけ debug のリンクが `LNK1236: corrupt or invalid COFF sections`(graph_trace.cpp.obj)で落ち、もう一度 build したら通った(release と続けて投げた直後)。

