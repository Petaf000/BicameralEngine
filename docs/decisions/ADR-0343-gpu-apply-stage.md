# ADR-0343 GPU の伝導の段を当てる段に載せる(見立ての写しは種類が読む部分だけ・相の表と世界の端は表の語 t2)

- Status: Proposed(2026-10-18。T-0766・T-0773。ADR-0334〔当てる段〕・ADR-0333〔見立て〕・ADR-0337〔世界の端〕の GPU の形。細部は仮〔ユーザー未確認〕)
- 日付: 2026-10-18
- 決めた人: Claude(T-0766・T-0773 の作業役)
- 関係: docs/design/20 §2.2・§2.4・§2.7・§3・ADR-0334・ADR-0333・ADR-0337・ADR-0338・T-0111(伝導の段の写し)・T-0410(表の写し)・T-0767(作る部分)

## 背景
- 土台 §2.7: 見立て・面の式・当てる順は共有 .hlsli の 1 つの源を CPU と GPU が呼び、違うのは回し方だけ。T-0714 で CPU の MR の伝導は
  当てる段(TxFlowCell)に載ったが、GPU の伝導の段(shaders/sim/multires_conduct.hlsli)は自前の面の回し(MarkCell・AddCellFlows)のままで、
  当てる段は「HLSL でコンパイルできるか」だけを transport_apply_check.hlsl で確かめていた。世界の端(T-0721・T-0772)も CPU だけだった。
- GPU の伝導の段は T-0111 で、セルの熱を groupshared に 1 セル 16 バイトで写し(ブロック 512 + 外の面 384 セル)、面ごとの計算はそこを読む。
  見立て全部(1 セル約 200 バイト)はこの写しに入らない。
- 多重解像度のルート署名は 64 / 64 語(UAV 14 × 2・定数 26・リング 2・表 4 × 2)。相の表(ADR-0333 の 3)と世界の端(箱・外の柱。柱は最大 2^20 行)を
  渡すバッファを足す語が無い。

## 決定(仮)
1. **GPU の伝導の段は当てる段を呼ぶ**: 流れの段は `TxFlowCell(TxConduction, GpuConductionWorld, …)`、印の段は CPU の MarkCellWants と同じく
   `TxFaceFlow` で面の流れを出す(開いた端の面は同じレベルの面と同じ数え方)。世界の約束(`GpuConductionWorld`)は CPU の ConductionWorld と同じ並び:
   面の隣(木の隣 → 世界の外なら WeEdgeNeighbor。流れの回は凍らせた枠・陰解法で解く枠との面を流さない)・見立て・レベル・端数の置き場・上限なし・
   変化は 64bit の atomic の足し算(AddConductDelta)・開いた端の帳簿は `AddWorldLedgerUnits`(繰り上げは AddWorldLedgerCarrying)。
   transport_apply_check.hlsl と cell_view_check.hlsl(コンパイルだけの確かめ)は消した(本物の段が同じ関数をコンパイルする)。
2. **見立ての写しは種類が読む部分だけ**: 見立ては CPU と同じ `MakeCellViewParts(表, セル, レベル, 0, 種類の ViewParts())` で作り(溢れを持つセルは
   `CvAddSpeciesParts` で 1 成分ずつ足す。和は整数なので同じ値)、groupshared には種類が読む部分だけを写す(伝導 = セル全体の熱 MrThermal の 4 語。
   T-0111 の大きさのまま)。当てる段の引数の形(World::View(slot, index))は変えない。ブロックの外の隣は、自分のセルの外を向く面の写しから
   枠とセルが合うものを探す(同じセルなら同じ値)。ViewParts は定数なので、作らない部分(相ごとの和・割り算・相の表の読み)はインラインの後で消える。
   気体・液体の種類を GPU に載せる時は、その種類の読む部分の写しの形(語の数)を種類の側で決める(後のチケット)。
3. **相の表と世界の端は表の語(t2)**: 前の t2(反応の索引 `StructuredBuffer<uint32_t>`)を同じ型の「表の語」にし、見出し 4 語の後ろに
   反応の索引・相の表(物質ごと 4 語)・世界の端(18 語)・外の柱(行ごと 27 語)を並べる(`shaders/common/multires_table_words.hlsli`)。
   C++ が `MakeTableWords` で書き、GPU は同じ読む関数(`TwPhase`・`TwWorldEdges`・`TwColumnCell`)で読む。反応の索引の読み
   (multires_bindings.hlsli の GpuReactionTable::RuleIndex)は定数の足し算 1 つ(見出しの 4 語 `MR_TABLE_WORDS_RULE_INDEX`。C++ の static_assert)
   だけ増える。相の表と世界の端の読みは伝導の段(multires_conduct.hlsli)だけが include する: 共通の multires_bindings.hlsli に
   include を足しただけで、引き戻しの Work Graph(multires_graph.hlsl の PullBackNode)の局所の配列が 31 → 35 個に増え
   (家族の写し [64 x i32]・[64 x i64] が 1 組増えた)、HW の「たくさんの要求」の影を作る刻みで DEVICE_HUNG になった(WARP は一致。T-0124・T-0397 の仲間)。相の表は表を作る・差し替える時に、世界の端は `RecordUpload` が `MultiresNest::edges` と
   違えば作り直す(前の表の語は次の RecordUpload まで持つ。写しの元を毎回書き換えるのと同じ約束)。表の写しは表ごとに「写すのが残っているか」を持つ。
4. **陰解法の端はまだ**: 陰解法で解くブロックの外の端は CPU も GPU も wall と同じ(T-0721 から変わらない)。CPU は wt5 の T-0790 が
   共有の .hlsli の形で直し、GPU はそれに合わせて T-0784 で入れる。

## 結果
- GPU の伝導の段は CPU と同じ関数で面の流れを出す(二重書きが 1 組減る。土台 §2.7)。今の試験(gpu_multires の伝導・小刻み・陰解法の組 HW / WARP)がビット一致のまま。
- 世界の端が GPU でも流れる: gpu_multires_conduction の「端の鎖」(6 面 open・高さで温度の違う外の柱。活性 Compute / Work Graph・全部、小刻みも)が
  毎刻み CPU とビット一致(帳簿の「世界の端」・次の刻みの種も)。
- 作り手から見た違い: 結果は同じ(ビット一致)。開いた端のある世界を GPU で刻んでも CPU と同じに熱が出入りする(T-0773 の前は GPU だけ wall だった)。
  陰解法を入れた刻みでは、陰解法で解くブロックの端は CPU も GPU も wall(T-0784)。
- 費用: docs/perf.md(2026-10-18 T-0766)。

## 代案
- 物質の行(t0)を 48 バイトに広げて相の値を入れる: 相の表には自然だが、世界の端と外の柱の置き場が残る(結局 t2 か別の置き場が要る)。
- 根の定数を定数バッファ(2 語)にして SRV を足す: 段ごとに定数を書き換える今の形(刻み・小刻みの番号・外の定数)を全部の段で直す大きな変更。
- 外の柱を固定の上限(例 256 行)で伝導の作業場(u11)に置く: 柱は最大 2^20 行で、上限を超える世界が CPU と食い違う。
- 見立て全部を groupshared に写す: 1 ブロック約 180 KiB で入らない。写しをやめて面ごとにセルから作る: 外の面の重さが面のセルのスレッドに偏る
  (T-0111 の前の形)。どちらも採らなかった。
