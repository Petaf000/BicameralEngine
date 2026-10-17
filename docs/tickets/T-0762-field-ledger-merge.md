# T-0762 状態の登録と世界の帳簿の合流(欄の余りを帳簿の列へ・列を登録から・保存の検査に欄の和)

- Status: Doing
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(2 時間)
- マイルストーン: 運ぶ計算の共通の土台 段 1 ①②の合流(runner/design/substrate-decided.md §2.1・§2.4 ⑤・§6)
- 設計: T-0711「wt4 との合わせ方」a〜c・T-0712「状態の登録との合わせ方」/ 決定: ADR-0329・ADR-0331

## 目的
保存の検査を「Σ 登録した保存量 − 帳簿 = 一定」の 1 関数(ComputeConservedTotals)にそろえる。欄の余りの置き場(MultiresField::ledger)を無くし、世界の帳簿の 1 型に入れる。

## 完了条件
- [x] a. 保存量の欄(SF_FLAG_CONSERVED)は世界の帳簿に値の数だけ列を持つ(列 = 1 + 物質 + 保存量の欄の値。登録の順)。VOLUME の欄を粗くした余りは
      「粗くした端数・出」(WlSlot(WL_REASON_COARSEN_REMAINDER, WL_DIRECTION_OUT))のその列へ AddWorldLedgerUnits で。MultiresField::ledger・LoadNestFieldLedger・区画 field_ledger を消した
- [x] b. ledgerColumns は登録から: 作る時は 1 + 物質の数、RegisterNestField が保存量の欄の値の数だけ足す(帳簿はまだ空なので並べ直さない)。
      NestConservedComponents・NestFieldLedgerBase(multires_nest.h)・NestConservedIndex(multires_fields.h)
- [x] c. ComputeConservedTotals に欄の和(ConservedTotals::fields。本物の刻むセルの値 × 8^(finest − レベル) − 帳簿のその列)
- [ ] release のビルド警告 0・^multires_ 全部・multires_state・lab_box・gpu_lab_box・gpu_multires_quiet

## 作り(要点)
- 余りは切り捨て(SfCoarsenRemainder は 0〜7)なのでいつも 0 以上 = セルから減った量 = 「出」だけ。符号付きの欄の値でも余りは負にならない
  (負の値も −∞ 向きの切り捨て)。将来、符号付きの差を載せる所は WlEnergyEntry と同じく正 → 出・負 → 入。
- 丸ごとの余り r(子のレベル L の単位)は AddWorldLedgerUnits(L) = L − 21 の段に 2r・L − 22 の段に最上位の bit(T-0712 の改造と同じ書き方)。
- 欄の無い世界は帳簿の大きさ・番地・ハッシュとも前と同じ(GPU の帳簿のバッファも同じ大きさ)。

## 帳簿の書き方の約束(並走の wt5 T-0714〔面の帳簿・当てる段〕とそろえる)
- 列: 0 = エネルギー・1 + 物質 ID・NestFieldLedgerBase(nest) + NestConservedIndex(nest, 欄) + 値の番号 = 登録した保存量の欄。
  列の番号を手で数えない(物質の数の違う表で作り直す箱があるので 1 + 表の物質の数とは限らない)。
- 量は「そのレベルの単位の丸ごとの数」なら AddWorldLedgerUnits、「単位 × 2^-64 の端数」なら AddWorldLedgerBits。符号付きの量は正 → 出・負 → 入(WlEnergyEntry)。
- 改造は前の中身を全部 出 + 後の中身を全部 入(差を取らない。ADR-0331 3)。欄の改造も同じ形にする(欄の値 × その列)。
- 保存の検査は ComputeConservedTotals(nest, table, finest) の 1 つ。欄の量は totals.fields[NestConservedIndex(nest, 欄) + 値の番号]。

## 判断待ち(仮〔ユーザー未確認〕。取り消しやすい)
1. **量の欄(VOLUME)と保存量(SF_FLAG_CONSERVED)は同じもの**にした(SfDescValid が片方だけの登録を断る)。作り手から見た違い: 「和を取るが保存の検査に
   入らない欄」は作れない(粗くした余りを載せる列が無いので、作れば余りが黙って消える)。要るなら「余りを捨てる量の欄」を規則に足す(state_field.hlsli の 1 行)。
   おすすめ: このまま(余りを黙って捨てない。D-428)。

## 作業ログ
- 2026-10-17(wt3): a〜c を入れ、multires_state_test の欄の合計を ComputeConservedTotals の欄の列に替えた(面の欄も保存量に。列の数・登録を断る場合の試験を足した)。
