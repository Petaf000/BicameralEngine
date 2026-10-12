# T-0362 GPU の溢れの 1 セルの上限を無くす(大きいセルを作業場に置き、核をセルの形の道具で読む。T-0327 から分けた)

- Status: Done(2026-10-12。範囲を「核を成分の道具〔マクロ〕で読み書きする・速い形に入らないセルを作業場の段で刻む・上限を表の物質の数〔177 まで〕・既定の世界の DXIL を main と同じに」に絞った。大きい形のセルが並ぶ場面を軽くするのは T-0380、177 を超えるのは T-0381。ADR-0052 追記 14)
- 種類: 工学(128 種で HW が止まる原因は未確認なので、止まらなくなるかは測って確かめる)
- PC: 必須(HW)
- 見積もり: チャット 1〜2 回分
- マイルストーン: T-0272 と同じ(docs/plan/ROADMAP.md)
- 設計: 02 §3・17(R-MULTI-4)・ADR-0052(追記 5・10・11)。関係: T-0272・T-0290・T-0326・T-0327・D-448

## 今(T-0327 の後)
- GPU の 1 セルの上限は RX_GPU_WIDE_SPECIES = 96(仮)。上限の大きさのセル(RxGpuWideCell)を持つのは 64 スレッドの WideDeferred・WideRetry だけ。
  128 にすると、その形が 1 度も使われない場面でも HW の compute キューの刻みが返らなくなった(T-0327。T-0290 と同じくスクラッチの大きさが疑わしい)。
- 見出しの大きさはもう上限に依らない(ADR-0052 追記 11)。要求の語の塊の数は 8 ビットで、上限 177 種まで入る(超えるなら要求の語も広げる)。

## やること
1. 核(shaders/common/reaction.hlsli)の Cell 型の使い方(.species[i]・.amounts[i]・.speciesCount を約 27 か所、セルの値の写し)を
   「成分を読む・書く・写す」道具の関数にする(RxCell・RxWideCell〔CPU〕・速い形は今の配列のまま。結果はビット単位で同じ)。
2. 大きいセルの形を「作業場(u6 の置き場の後ろ。ルート署名は一杯)の位置」にする。写す所(反応物を引く・RxCompactCell・RxTryAddSpecies・
   刻む前と後の比べ)は作業場の別の場所へ書く。上限は表の物質の数(作業場の大きさは 1 セルの場所 × 同時に刻むセル)。
3. 表の物質が 128 以上の試験の場面で HW・WARP が CPU の EnableWideCells の世界と毎刻みビット一致・待たせた 0・`--repeat-wide` が通る。
4. 大きい形のセルが一面に並ぶ場面(--measure-wide の「大きい形のセル」)を軽くする(1 セルを複数スレッドで・作業場で ① の結果を ③ に写す)。

## 完了条件
- [x] 表の物質 128 以上で 1 セル 97 種以上を作る場面で待たせず、CPU と HW・WARP が毎刻みビット一致(HW の compute キューで --repeat-wide も)
  (160 種の表・34 刻みで 1 セル 98 種・頁の溢れ 40453 成分・待たせた 0。HW `--repeat-wide many,many-conduct,limits,limits-conduct 3` が通った)
- [x] 既定の世界のシェーダー・結果・費用を変えない(reaction.hlsli を読む溢れを使わないシェーダー 92 本の .cso が main 1f6530c と MD5 で同じ。VRAM も同じ)
- [ ] 空気だけの溢れの世界の ns/セル・VRAM が T-0327 より悪くならない(docs/perf.md。マクロにした後の --measure-wide は未計測 → T-0380 の初めに測る)

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-11(作業役、wt4・ブランチ t-0362。約 1.7 時間): reaction.hlsli の核が成分を触る所(約 20 か所)を RxSpeciesAt・RxAmountAt・RxWithAmount・
  RxWithEntry・RxWritableCell・RxUsageAt・RxWithUsage にした(C++ の RxWideCell は参照の多重定義)。大きい形 RxGpuWideCell をやめ、作業場の形
  RxGpuSpaceCell(u6 の作業場に成分。特殊化で読み書き)を作業場の段 WideBigPass(32 グループ)だけが持つ。WideDeferred・WideRetry は速い形に入らない
  セルを見つけたら要求に MR_WIDE_REQUEST_BIG を付けて回す。上限 = 表の物質の数(RX_GPU_WIDE_SPECIES = 177 は並びの大きさ)。
  試験の many の場面を 160 種の表・34 刻みに(1 セル 98 種)。HW の compute キューで `--repeat-wide many,many-conduct,limits,limits-conduct 3` が通った
  (毎刻み CPU とビット一致・待たせた 0)。結果の数は下の引き継ぎメモ・docs/perf.md。
- 2026-10-12(作業役 2、wt4。約 2 時間。前の作業役がユーザーの中断で止まった続き): 書きかけを WIP でコミットして main(1f6530c)に載せ直した
  (gpu_multires.cpp の MrWideAreaWords に世界の枠の数が足されたのと合わせ、ADR-0052 の追記の番号を 13 → 14)。核の道具を値を返す関数から
  マクロ(RX_SPECIES_AT・RX_AMOUNT_AT・RX_USAGE_AT・RX_SET_ENTRY・RX_ADD_AMOUNT・RX_SUB_AMOUNT・RX_ADD_USAGE・RX_MAKE_WRITABLE)に変え、
  既定は前と同じ配列の式、RX_CELL_ACCESSORS を定義した作業場の段だけが関数に展開するようにした。reaction.hlsli を読む溢れを使わないシェーダー
  92 本の .cso が main と MD5 で同じになった(前は 25 本が違った)。C++ の RxWideCell の参照の多重定義は要らなくなったので消した。
- 2026-10-12(本体の作業役・マージの確かめ): main(c6bca01。t-0359 の転換と合わせた後)と t-0362 を合わせる前(0172a5a)の既定の 92 本の .cso の MD5 が
  92 本とも同じ(runner/dx の gen.py・cmp.py。git 管理外。runner/t362/gen.py の型を main の build.ninja に合わせたもの)。マクロは転換が入っても DXIL を変えない。

## 引き継ぎメモ
- 作業場の形と段は ADR-0052 追記 14。作業場は表の物質が 32(RX_GPU_MID_SPECIES)を超える世界だけ作る(32 × 64 スレッド × 8 語 × 上限。160 種で 10 MiB)。
  作業場の段は外の定数(g_external0 = 始まり・g_external1 = 上限)を使う(RecordWideBig が設定して戻す)。
- 既定の世界のシェーダーの DXIL は main と同じ(上)。比べ方: runner/t362/gen.py(git 管理外)が release の build.ninja から dxc の行を拾い、
  reaction.hlsli を推移的に読む溢れを使わないシェーダーだけの .ps1 を作る(runner/t362/run_m0.ps1 = main を `git archive main shaders` で
  展開した runner/t362/m0、run_cur*.ps1 = 作業ツリー)。`job.py raw` で流して(約 1 分・GPU の順番待ちなし)、ログの「名前 行数 MD5」を join して比べる。
  核に触るチケット(wt3 の T-0359 など)が合わさった後も、同じ型で「既定の世界は変わらない」を確かめられる。
- マクロの決まり: 核(reaction.hlsli)と multires_wide_step.hlsli の形に依らない道具は成分をマクロだけで触る。RX_SET_ENTRY は既定で 2 文に
  なるので {} の中だけで使う。作業場の形の読み書きは multires_wide_step.hlsli の特殊化(RX_CELL_ACCESSORS が無いと #error)。
- 作業役 2 の最後の確かめ(マクロにした後・release): HW の compute キューで `--repeat-wide many,many-conduct,limits,limits-conduct 3` が通った
  (160 種の表で 1 セル 98 種・頁の溢れ 40453 成分・待たせた 0・毎刻み CPU とビット一致。GPU の順番待ち約 25 分)。WARP と ctest 全体は
  マクロにした後に流していない(時間の約束のため。C++ はマクロが main と同じ配列の式に展開するので main と同じ。WARP の gpu_multires_warp は
  main でも「端数の枠の塊」で落ちる = T-0385)。T-0380 の初めに `job.py test` を流す。
- (以下は T-0327 からの申し送り。結果は T-0362 の計測を見る)--measure-wide の「大きい形のセル」(64 種の表・根 64 に 1 セル 33〜64 種が一面)は HW で測る途中に DEVICE_HUNG(T-0327。原因は未確認)。
  まず --diagnose-wide の段ごとのタイムスタンプで 1 刻みの時間と段を見て、TDR か本当の止まりかを分ける。

## 判断待ち
- なし(上限を表の物質の数にしたのは T-0327 の「上限を無くす」の続きで、ユーザーの D-448〔混ぜた所だけ重くてよい・壊れない〕に沿う。
  177 を超える表は今までどおり待たせる〔D-448 の仮のふるまい〕。プレイヤーと遊び: 1 か所に 97〜177 種を混ぜる実験でも反応が CPU の世界と同じに進む)。

## 分けたもの
- T-0380(工学): 大きい形のセルが一面に並ぶ場面を軽くする(1 セルを複数スレッドで・作業場で ① の結果を ③ に写す。作業場の段のグループの数)。
  初めに T-0362 の後の --measure-wide(空気だけの溢れの世界・32 種・上限の種・大きい形のセル)を GPU の順番待ちありで測り、perf.md に残す
  (前の作業役の値: 上限の種のセル 2471 / 431 ns/セル・大きい形のセル 3933 / 2902 ns/セル〔伝導なし / あり。HW・負荷あり〕・待たせた 0)。
  (既定の世界の DXIL を main と同じにするのは T-0362 で済んだ)
- T-0381(工学): 1 セルの上限 177 を超える(要求の語の塊の数を 8 → 12 ビット以上にし、面の塊の表の続き〔MR_WIDE_TABLE_SPILL〕を上限に依らなくする)。
