# T-1192 支えの法則から値で固定する閉じた領域を外す

- Status: Done(2026-10-27。ADR-0480 は D-680 で Accepted)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(小さい)
- マイルストーン: 試作の列 3(R-B3)
- 設計: docs/design/22-support-law.md §3.1 / 決定: ADR-0480

## 目的(1〜2 行)
T-1171 の 1 本目で支えの法則に入れた値で固定する閉じた領域(SupportOptions::closeMultiplier・closeGrowth・closeResidualDivisor・SupportLaw::CarryCloseState)を外す。
既定 0 = 使っていないので、外しても答えは変わらない。C-0 の閉じた領域(T-1087)は土台の持ち物なので触らない。

## 完了条件(チェックできる形で)
- [x] 上の値と CarryCloseState・閉じた刻みの帳簿の数(LastSeeds・CloseLengthMm)を外す
- [x] support_cost_test の 1 本目の場面は「閉じない」と「粗さで閉じる」だけにする(1 本目の表は T-1171 に残る)
- [x] support_* の ctest が通る・答えはビット一致・archmap --check

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-27(wt2・t-1192-remove-value-closing。t-1206-fixed-face-points の上): SupportOptions の closeMultiplier・closeResidualDivisor・closeGrowth と
  SupportCloseGrowth・SupportLaw::CarryCloseState・LastSeeds・CloseLengthMm・閉じた領域の種の控え(m_closeSeeds・m_changedSeeds・m_closeResidual)・
  外接の箱 CellBox を外した。CarryFrom・StepCoupled は閉じない時の道筋そのまま(C-0 の extraSeeds・closeLengthMm・residualSeed は既定のまま渡る)。
  C-0 の閉じた領域(T-1087)は土台の持ち物なので触っていない。support_cost_test の 1 本目は「閉じない」(帳簿・2 回の実行のハッシュ一致)と
  「粗さで閉じる」(CoarseCase。変えていない)だけにした。--measure は閉じない解の 3 場面の費用の比だけを出す。
- 試験: 既定 0 で使っていなかった値を外しただけなので、値は変わらない(新旧の表は無い)。release ビルド警告 0・`^support_` 9 本すべて通過
  (3315 s)。ビット一致の根拠: 外したのは closeMultiplier ≠ 0 の時だけ通る枝で、既定の道筋(CarryFrom・StepCoupled が C-0 へ渡す値)は
  前と同じ(extraSeeds は空・closeLengthMm と residualSeed は 0 のまま)・support_cost の 32×16 m・0.5 m・穴 1×1 m は
  閉じない解の費用 10220(粗さで閉じるの全部細かい塊と同じ)・2 回の実行一致・archmap --check OK。
