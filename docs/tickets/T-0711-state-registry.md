# T-0711 状態の登録(運ぶ計算の共通の土台の段 1 ①。MR の CPU から)

- Status: Done(CPU。GPU・観察の枠・帳簿の 1 型との合流は下の「分けたもの」)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(2 時間)
- マイルストーン: 運ぶ計算の共通の土台 段 1 ①(runner/design/substrate-decided.md §2.1・§3・§6)
- 設計: substrate-decided §2.1・§3「セルの外の欄の置き場」・survey-engine §1.2・§3 / 決定: 2026-10-17 ユーザー(土台・決定 4)/ ADR-0329

## 目的
持ち越す状態(セル + 登録した欄 + 世界の帳簿)を登録制にし、状態のハッシュ・保存点・写しが「登録を回るだけ」になる形を MR の CPU で作る。

## 完了条件
- [x] 欄の登録の形を共有 .hlsli に(shaders/common/state_field.hlsli の SfFieldDesc: 置き場〔セル / 面〔+ 側の 3 面〕/ ブロック / 世界〕・型〔I64 / I32 / U32〕・
      付く相〔なし / 気体 / 液体〕・細かく/粗くの規則〔SAME: 同じ数 / 平均、VOLUME: 同じ数 / 和 ÷ 8 + 余り〕・保存量か・ハッシュに入るか・値の数)。整数だけ・CPU と GPU が同じ関数
- [x] セルの外の欄の置き場(頁と同じ番号の「欄の頁」。並びは nest.cells と同じ [一様の値 × 枠][頁 × 512] × 値の数)。MultiresNest::fields
- [x] 状態の区画の一覧 VisitNestState(engine/src/sim/multires_state.h)。HashWholeNest・SaveNestState / RestoreNestState・CopyNestState は一覧を回るだけ
- [x] 欄の無い世界の要約が前とビット単位で同じ(multires_state_test が前の作りの写しと毎刻み比べる: 本物の鎖・影・溢れ・たくさんの要求・一様なブロック)
- [x] 試験用の欄(セル・面・ブロック・世界)が細かく/粗く・頁に広げる/畳む・ハッシュ・保存点・写しの早送りで正しく扱われる(multires_state_test)
- [x] 今の MR の CPU の試験(^multires_)が通る・release のビルドの警告 0

## 作り(要点。詳しくは ADR-0329)
- 木の操作に欄を付いて行かせる所は nest_fields の 7 つ(PlaceRoot・ExpandPage・PageFoldable・FoldPage・Refine・CoarsenKeepsUniform・Coarsen)。
  呼ぶ所: PlaceRootBlock・ExpandWantedPages・粗くする時に親を頁に広げる所・ReleaseFoldedPage / FoldExact / FoldNear・RefineRequestLevel・Settle・ApplyCoarsenOf。
- 欄が一様でない頁は畳まない(許容差つきの畳み方でも欄はビット単位で一様な時だけ。仮)。欄の値の違う一様な子を一様な親へ粗くすると親を頁に広げる。
  頁を持つ子を一様な親へ粗くする時は、欄のある世界では控えめにいつも広げる(欄の無い世界は今まで通り)。
- VOLUME の欄の粗くした余り(0〜7。子のレベルの単位)は欄ごとの帳簿 MultiresField::ledger[レベル × 値] へ(世界の帳簿の 1 型〔T-0712〕に入るまでの置き場)。
- 保存点の区画: 見出し・セル・端数・空きのスタック・帳簿・数える欄・(溢れ 3)・種(ハッシュに入らない)・索引(ハッシュに入らない。墓石の位置が次の挿し入れと数える欄を変えるので写す)・欄ごとに値と帳簿。
  戻す時は形(区画の名前・型・大きさ・登録した欄の形)を先に全部比べ、違えば何もしない。
- HashRealLeaves にもハッシュに入る欄を足した(本物の刻むセルの欄・本物のブロックの欄・世界の欄。欄が無ければ前と同じ値)。

## wt4(T-0712 世界の帳簿 1 型・ADR-0331)との合わせ方
- T-0712 の帳簿の列 = 0 エネルギー・1 + 物質 ID・その後ろに登録した欄の保存量(登録の順)。この登録の SF_FLAG_CONSERVED の欄がその列になる。
- 合流の時にやること(司令塔がマージ後に / 段 1 ③〜④): (a) MultiresCapacity::ledgerColumns を「1 + 物質 + 保存量の欄の値の数」にして登録から作る
  (登録は根を置く前なので、MakeMultiresNest の後・RegisterNestField の時に帳簿の列を足す形にする) (b) MultiresField::ledger を世界の帳簿の「粗くした端数・出」の欄の列へ移す
  (単位: 今は子のレベルの整数の余り 0〜7。世界の帳簿はそのレベルの単位 × 2^-64 なので、余り r は「1 段粗いレベルの r × 2^61」= AddLedgerBits と同じ書き方) (c) 「欄ごとの保存量の和」を
  ComputeConservedTotals の 1 関数に(本物の刻むセルの値 × 8^(最も細かい − レベル)。試験の FieldTotal が形の見本)。

## 判断待ち(仮〔ユーザー未確認〕で進めたもの。どれも取り消しやすい)
1. **欄の値はどれも 64bit の符号付きで持つ**(型は範囲の約束だけ)。作り手から見た違い: なし(エンジンの中の欄。作り手は書かない)。費用は I32 の欄で 2 倍の VRAM(段 6)。
   GPU で詰めるなら型ごとに語の数を変える(置き場の関数 1 つ)。おすすめ: 今のまま測ってから。
2. **欄が一様でない頁は畳まない・許容差つきでも欄はビット単位の時だけ**。作り手から見た違い: 気体・液体が乗った所は(運動量が 0 でない間)頁を持ち続ける = 費用。
   静止の丸め(§2.6・段 4 の研究)で欄が 0 に戻れば畳まれる。おすすめ: このまま(D-428: 結果を変えない)。
3. **面の欄の細かく/粗くはセルの欄と同じ(子は親と同じ数・和 ÷ 8)**。設計 §3 のとおり「液体の面の運動量を細かくした時の内側の新しい面」は研究(段 7)。今は形だけ。

## 分けたもの
- T-0760 GPU の欄の頁(GpuMultires の欄のバッファ・同じ nest_fields の 7 つを .hlsl の段に・保存点の区画の一覧から GPU のバッファの一覧)。段 6 で。
- T-0761 観察の枠(影・写し)の欄(RefineShadowChain・PullBackShadowChain・PlaceMirrorBlock で欄を付いて行かせる。影の欄の引き戻しは和 ÷ 8 か)。段 7 の前に。
- T-0762 欄の余りを世界の帳簿の 1 型(T-0712)へ・帳簿の列を登録から作る(上の「合わせ方」a〜c)。T-0712 のマージの後すぐ。

## 作業ログ
- 2026-10-17(wt3): state_field.hlsli・multires_fields・multires_state を足し、HashWholeNest を区画の一覧を回す形に、木の操作 7 か所に欄の関数を差した。
  multires_state_test(前の作りの写しとの比べ・欄・保存点・写しの早送り)を足した。release のビルド警告 0・^multires_ が通る。
