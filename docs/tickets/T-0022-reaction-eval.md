# T-0022 反応の評価(Work Graphs・可変長の成分)— 最初の段: 上限を超えた時に黙って捨てる 3 か所を塞ぐ

- Status: Review(2026-10-09 作業役。最初の段だけ。残りは T-0163・T-0164 に分けた)
- 種類: 工学
- 設計: 02 §3・§6・01・04。関係: T-0021(ベイク)・T-0124(DEVICE_HUNG)・D-302・D-401・D-418・D-428

## このチケットの範囲(2026-10-09 に絞った)
上限(1 セル 8 種・同時に進む規則 16)を超えた時に黙って捨てて保存が破れる 3 か所を、「捨てない」形(D-428)で塞ぎ、印と数える器を付ける。
上限そのものを無くす(成分の二段・分布の測定)は T-0163、評価の振り分け(ノード配列・Compute との比較・R-REACT-2)は T-0164。

## 完了条件
- ① RxAddSpecies の 9 種目の生成物・② RxCollectCandidatesWait の 17 個目以降・③ MrAppendCoarsened の入りきらない成分、のどれも捨てない
- 3 つの場面(9 種目の生成物・候補 17 以上・8 種ずつ違う子を粗くする)で CPU と HW・WARP が毎刻みビット一致し、元素とエネルギーがビット単位で保存される
- 上限に当たった印と数(release でも数える)

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役): 最初に t-0136・t-0021・t-0143 を合わせた main を release で全部流して通過(直したものなし)。
  ① 生成物を全部足して入りきらなければ、セルに無い物質を作る規則をその刻みは待たせてやり直す(RxApplyExtentsHeld。RX_LIMIT_PRODUCTS)。
  ② 進む規則が 16 を超えたら、刻みとセルごとの乱数の優先度で 16 個を選ぶ(RxReplaceLastCandidate。表の並び・ID に偏らない。RX_LIMIT_CANDIDATES)。
  ③ 粗くする要求は、親の 64 セルのどれかが入りきらなければ断る(MrCoarsenFits・MR_STATUS_SPECIES_FULL・MR_COUNTER_COARSEN_FULL。静かな葉は今の忙しさの印で「粗くできない」に)。
  試験: reaction_test の TestLimits・gpu_reaction_limits(_warp)(tests/reaction_limits_table.h)・multires_test の TestCoarsenFull・gpu_multires(_warp)の RunCoarsenFull(tests/multires_limits_scene.h)。
  当座のふるまいは仮(QUESTIONS Q19)。世界の刻み(多重解像度・仮の世界)で ①② を数える器は T-0163(印 RxWaitStep::limits は全部の呼ぶ所に届いている)。

## 分けたもの
- T-0163: 成分の数と候補の数の分布を測る(研究)→ 成分の二段(インライン K + 頁の溢れ領域)で上限を無くす。世界の刻みで ①② を数える器
- T-0164: 評価をノード配列で型ごとに振り分ける・Compute の振り分けと測り比べる(D-302)・R-REACT-2(規則が数千の時の候補の引き方)

## 事前調査(2026-10-09、調べ役。読むだけ。未確認の数値は「推定」「未確認」)

### 今の実装の要点
- セルは固定長。`RxCell` = エネルギー + 成分 8 個(ID u32 + µmol u64、ID の昇順、0 になった成分は消す)で 112 B(reaction.hlsli:35,84)。規則は反応物・生成物とも 3 個まで、1 セルの候補は 16 個まで(:36-38)。
- **上限を超えた時に黙って捨てている所が 3 つある**(FX_ASSERT は debug だけで release は素通り):
  ① `RxAddSpecies`(:652)は 9 種目の生成物を足さずに返す → 反応物は引いてあるので**元素とエネルギーの保存が破れる**。
  ② `RxCollectCandidatesWait`(:452)は 17 個目以降の規則を打ち切る → ID の大きい物質の規則だけ系統的に起きない。
  ③ 粗くする時の `MrAppendCoarsened`(multires.hlsli:380)は入りきらない成分を捨てて MR_COUNTER_OVERFLOW に数えるだけ。8 個の子の和集合は 8 種を簡単に超える。
  ①③は D-401・D-418・D-428・R8 に反する。**T-0022 の最初の仕事はこの 3 つを塞ぐこと**。
- 候補の引き方: 規則は「ID が最小の反応物」の索引に 1 回だけ載る。評価 1 回ごとに log2 2 回と 128bit の割り算(待ちの丸め)。R-REACT-2(候補が平均 8 を超えたら物質の組で引く)は手つかず。
- Work Graph: ActivityStepNode(broadcasting、1 グループ = 1 ブロック 512 セル、64 スレッド × 8 セル)が StepBlockWait の中で反応の核をまるごと展開。反応の連鎖はノードの連鎖ではなく、セルの中で閉じた反応を刻みごとに回し、隣へは活性(WakeFaceNode)。
- T-0124: 核を 1 ノードに 3〜4 か所展開すると HW で DEVICE_HUNG(同数の Compute は動く。2 か所までなら動く)。原因はノードの大きさか局所配列のスクラッチと推定(未確認)。可変長にすると局所配列が増えるので直接ぶつかる。

### 結論(推奨)
1. **成分は二段**: インラインに K 個 + **ブロックの頁ごとの溢れ領域**(1 グループの中でセル番号順のプレフィックス和で配る = 決定的)。頁の溢れ領域も足りなければ「大きい頁が要る」印 → 刻みの中で配り直して**そのブロックを同じ刻みでやり直す**(「印 → TreeExpand → ExpandStepNode」と同じ型)。CPU は上限なしの可変長でビット一致。K は 4〜8 を測って決める(R-MULTI-4)。
2. **評価は「1 ブロック = 1 グループ」のまま、ノード配列で型ごとに振り分ける**: 見出しに前の刻みの「最大の成分数と候補数の段」→ ReactBlock[段]((a) 候補なし〔空気〕/(b) K 以内・候補 16 以内 /(c) 溢れ・候補が多い汎用)。各ノードに核は 1 か所だけ(T-0124 の回避)。規則ごとにノードを分ける案は採らない(取り合いがセルの全規則の要求を一緒に見る必要がある・レコードが膨らむ・小さいノードは固定費が勝つ)。
3. D-302 に従い Compute 版(段で分ける → プレフィックス和で詰める → ExecuteIndirect)と測り比べる。

### 方式の比較
| 方式 | 決定性・ビット一致 | 保存 | メモリ・帯域 | 多重解像度(粗くする) |
|---|---|---|---|---|
| ① 固定 K + セルごとの溢れの連鎖(大域プール) | △ 大域 atomic だと満杯時の溢れ方が実行順で変わる(R4) | ○ | 空気でも K 個読む。連鎖の読みがばらける | △ 子の和集合はほぼ毎回溢れる |
| ② ブロック単位の CSR | ○ グループ内のプレフィックス和 | ○ | 最小・まとめて読める。毎回組み直し。groupshared 32KB に全部は載らない(試算) | ○ |
| ③ 物質ごとの密な配列 | ○ | ○ | × 数千種は不可能(燃焼の GPU コードでも 53〜129 種でレジスタが溢れる。KinetiX) | ○ |
| ④ 二段(主要は列 + 微量はブロックの表)= 推奨の一般形 | ○ | ○ | 空気のセルを安く持てる | ○ |

### 試す順番
1. 工学: ①〜③ の黙って捨てる箇所を、溢れの印・数える器・CPU の上限なしに置き換える。試験: 9 種目の生成物・候補 17 以上・8 種ずつ違う子を粗くする、で CPU/GPU ビット一致と保存。
2. 研究(分布を測る): T-0021 の試験の表で、場面ごとに「セルの成分数」と「候補数」の分布を取り、K と段の境目を決める。基準: VRAM 1 ページあたりの量と ns/セルが今以下。
3. 工学: 頁ごとの溢れ領域と同じ刻みのやり直し。
4. 工学 + 測定: ノード配列の振り分けと Compute の振り分けを比べる(D-302)。先に核 1 か所のノードで T-0124 が出ないことを確かめる。
5. 研究(R-REACT-2): 規則を数千に増やした合成の表で候補の引き方(物質の組の索引 → セルの中身の型ごとの候補キャッシュ)。T-0124 の原因追跡は「核 1 か所のノードで止まらなければ追わない」。

### ユーザーに聞く点 → QUESTIONS Q18

### リスクと未確認
- ノード配列で T-0124 が避けられるかは未確認。ドライバのノードごとのスクラッチ上限も未確認。
- Work Graph の振り分けが 3070 Ti で Compute に勝つかは未確認(NVIDIA の事例は 4090 で 0.8〜0.95 ms 対 uber shader 0.98 ms と差が小さい)。
- 刻みの中でブロックをやり直すには元の値が要る。今の StepBlockWait が g_cells をその場で書き換えるかは未確認。
- 仕様の制約(確認済み): broadcasting の MaxRecords ≤ 256、出力 ≤ 32KB、groupshared ≤ 32KB、出力 + groupshared + 8 × レコード数 ≤ 48KB、深さ ≤ 32、1 入力を共有するノード ≤ 256。メモリの数値(12B/成分・K=4 で約 32KB)は試算。

### 出典
- D3D12 Work Graphs 仕様: https://microsoft.github.io/DirectX-Specs/d3d/WorkGraphs.html
- NVIDIA Deferred Shading の事例: https://developer.nvidia.com/blog/work-graphs-in-direct3d-12-a-case-study-of-deferred-shading
- AMD GPUOpen Work Graphs の Tips: https://gpuopen.com/learn/gpu-work-graphs/gpu-work-graphs-part3
- KinetiX: https://arxiv.org/html/2411.02640v2 / pyJac: https://ar5iv.arxiv.org/html/1605.03262
- Noita の反応: https://noita.wiki.gg/wiki/Documentation:_Reaction / BotW: https://www.thumbsticks.com/gdc-17-breath-of-the-wild-science-lies
