# T-0359 魔法の転換(規則の生成物を元素が釣り合う別の物質に替える)(T-0288 から分けた)

- Status: Done(2026-10-11。作業役・wt3。物質を足す・消す表の差し替えでの物質 ID の付け替えは T-0377)
- 種類: 工学
- PC: 必須
- 設計: docs/design/02-reaction-system.md §5・§5.1 / ADR-0095(追記 3)。関係: T-0024・T-0287・T-0288・T-0323・D-006・D-419・D-428

## 目的
魔法 = 反応則そのものへの介入(CLAUDE.md §0)のうち「転換」を入れる: 範囲と期間に限って、規則の生成物を別の物質に替える。
元素の釣り合いが保たれる転換だけを許し(GPU と CPU が同じ判定)、保たれない転換はコマンドの時に断る。
反応熱は替えた後の生成物の生成エンタルピー(H0)で求め直す(エネルギーの保存。D-428)。**値はすべて試験用の仮のもの(D-006)。**

## 完了条件
- [x] 効き方の種類 `MP_KIND_CONVERT`(= 4)・`MpMakeConvertEffect(欄, 物質 ID)`(語の 8〜9bit = 欄・10〜31bit = 物質。コマンド・列の形は変わらない)
- [x] 釣り合いの判定 `MpConvertedCoefficient`(組成の型 RxSpecies::elementClass・公約数 elementUnits をベイクが付ける。整数だけで正確)。
      足す時(MpPatchValid → コマンドで断る)と効かせる時(MpConvertedRule)に同じ関数
- [x] 反応熱を替えた後の H0 で求め直す(MpRuleEnthalpy = ベイクの ReactionEnthalpy と同じ式)
- [x] 核には MpPatchedTable::Rule の包みで渡す(核は変えない)。止める・2^shift 倍・触媒と重なる。同じ欄は先のパッチが勝つ
- [x] 実験室(多重解像度の刻み)・仮の世界(ProbeSim)で、今パッチが効く所と同じ範囲で効く(MpPatchTable を通る道は全部)
- [x] 試験: lab_box_test の TestMagicConvert(CPU)/ gpu_lab_box_test の RunConvert / gpu_probe_patch_test に転換 2 つ(釣り合う・断る)
- [x] 02 §5・§5.1・ADR-0095 追記 3

## 作業ログ
- 2026-10-11(作業役・wt3): ブランチ t-0359 を main(8cbf977)から作った。元素の数の表(元素 × 物質)を GPU に載せる代わりに、
  物質ごとに「組成の型」(元素の数を最大公約数で割った並びの番号)と公約数の 2 語を RxSpecies に足した(24 → 32 バイト)。
  「型が同じ かつ c × 公約数 = d × 公約数」で釣り合いが正確に決まり、2 O3 → 3 O2 のような係数の求め直しもできる。
  表の物質の数は物質 0(使わない)の elementUnits に置いた(ルートの SRV に GetDimensions を使わず、範囲の外の ID を読まない。
  ルート署名の語は増えない)。windows.h の `small` マクロに当たって 1 回直した以外、ビルドは素直に通った(警告 0)。
- 試験の表は `MakeConversionTestTable`(tests/reaction_test_table.cpp。試験の表に CO2 と同じ元素の仮の物質と C2 の仮の物質を足す。値は仮)。

- 2026-10-12(作業役・wt3。前の作業役がユーザーの中断で止まった所から): 書きかけは無く、実装は 1 コミットに入っていた。main(1f6530c。
  GPU の順番待ち・T-0169 の試験の表)に rebase(衝突なし)→ ビルド(debug・警告 0)→ 関係するテスト 18 本 OK。
  実験室の転換の保存点の再生試験を足しかけたが、GPU の順番待ち(約 1 時間)で 2 時間を超えるので戻して T-0378 に分けた。
- 2026-10-12(本体の作業役・マージの確かめ): main で t-0362(核の成分のマクロ・作業場の段)と自動で合わさった形を読んだ。転換は MpPatchedTable::Rule の
  中だけで効き、核は規則を table.Rule() で読み、成分はマクロで書くので、作業場の段(multires_step_wide_big も MrStepCellWait → MpPatchTable)でも効く。
  RxSpecies 32 バイトはルートの SRV の StructuredBuffer で読み、C++ は sizeof の static_assert 1 か所(ストライドを書く所は無い)。作業場の大きさは表の物質の数で、
  MpSpeciesCount(物質 0 の elementUnits)と同じ数。DXIL(reaction.hlsli を読む既定の 92 本): t-0359 が 23 本を変えた(RxSpecies の型で +2 行の 14 本と、
  パッチを読む段 multires_step_wait 10601 → 14765 行・multires_activity_graph 27238 → 38000 行・lab_box_apply 4356 → 4446 行など 9 本。転換の分 = 正しい変化)。費用は T-0396。
  **release の HW の Work Graph の 3 本(gpu_multires_activity・gpu_multires_uniform・gpu_multires_conduction)が最初の刻みで DEVICE_HUNG**(WARP は通る。1f6530c では通っていた)。
  疑いはこのチケット(活性のグラフの DXIL 27238 → 38000 行)。直すのは T-0397。

## テストの結果
- 2026-10-12(main 1f6530c に rebase 後・debug): CPU 12 本(reaction_package・reaction_hot_reload・reaction_table_edit・reaction_package_scene・
  reaction_replay_table・reaction_species_remap・reaction_package_runtime_stops・reaction・reaction_contention・reaction_wait・multires・lab_box)
  と GPU 6 本(gpu_reaction・gpu_lab_box・gpu_lab_box_warp・gpu_probe_sim・gpu_probe_patch・gpu_probe_patch_warp)で 18 / 18 通過。
  GPU の順番待ちは約 60 分(本体の 40 本・wt6 の後)。

## 引き継ぎメモ
- 転換の語は「種類 4・8〜9bit = 生成物の欄・10〜31bit = 物質 ID」。古い記録の効き方(1〜3)はそのまま。
- 替える先は今の表の物質 ID。物質を足す・消す表の差し替え(ADR-0065)ではパッチの ID を付け替えていない(T-0377)。
  付け替えなくても、効かせる時に今の表で釣り合いを確かめるので保存則は破れない(ずれた ID が釣り合う別の物質なら、それに替わる)。
- RxSpecies が 32 バイトになった(GPU のバッファの stride も)。物質 0 の elementUnits = 物質の数。手で作る表(テストの上限の表など)は 0 = 転換はすべて断る。
- エディタ・ゲームから出す操作は T-0325(効き方の選び方に転換を足す。欄と物質を選ぶ)。

## 判断待ち
- **同じ規則の同じ生成物に転換が 2 つ重なった時、どちらが効くか**(仮・ユーザー未確認。取り消しやすいので仮で進めた)
  - 案 A(仮で採用): **先にかけた転換が勝つ**(後のものはその欄には効かない。期間が終われば後のものが効き始める)。
    遊びへの影響: 「先に場を押さえた魔法が優先」という分かりやすい規則。相手の転換を上書きするには、終わるのを待つか止める魔法を重ねる。
  - 案 B: 後にかけた転換が勝つ。遊びへの影響: 上書きが簡単で、魔法の打ち合いが起きやすい。先に置いた転換の罠は崩されやすい。
  - 案 C: 重なった所では転換が打ち消し合う(どちらも効かない)。遊びへの影響: 衝突が目に見える。ただ「何も起きない」は法則として作り物に近い。
  - おすすめ: A(決まった順で CPU と GPU が同じ。どれにしても語の形は変わらない)。
- **転換で速さを変えるか**(この段では変えない。おすすめ: 変えない)
  - 変えない(今): 進む量は同じで、出てくる物質と熱だけが変わる。遊びへの影響: 「同じ速さで燃えるが、できる物と温まり方が違う」ので、
    プレイヤーは生成物と温度を測って転換を見分ける。速さを変えたいなら触媒・2^shift 倍と重ねる(独立した法則の組み合わせとして発見できる)。
  - 変える: 生成物が不安定なほど遅くなる、のような法則を入れられるが、反応速度論として作り物になりやすい(D-428)。

## 分けたもの
- T-0378 実験室の転換の保存点の再生(gpu_lab_box_test の RunConvert の最後に ReplayPatchRecording(表の差し替え無し。
  `swap.table != nullptr` の時だけ AddTable にする)を足し、転換の途中の保存点と記録全体を別の箱で再生して毎刻みビット一致・ハッシュ一致)。
  仮の世界の保存点・巻き戻しは gpu_probe_patch_test で転換込みで確かめ済み。小さい(試験だけ)
- T-0377 物質を足す・消す反応表の差し替え(ADR-0065・species_remap)で、転換のパッチの物質 ID を付け替える(消えた物質への転換は効かなくする)。
  実験室・仮の世界の付け替えの段で列の語を書き換え、CPU と GPU で毎刻みビット一致 | 02 §5.1・ADR-0095 追記 3
