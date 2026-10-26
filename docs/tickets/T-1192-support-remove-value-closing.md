# T-1192 支えの法則から値で固定する閉じた領域を外す

- Status: Todo(ADR-0480 が Accepted になってから)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(小さい)
- マイルストーン: 試作の列 3(R-B3)
- 設計: docs/design/22-support-law.md §3.1 / 決定: ADR-0480

## 目的(1〜2 行)
T-1171 の 1 本目で支えの法則に入れた値で固定する閉じた領域(SupportOptions::closeMultiplier・closeGrowth・closeResidualDivisor・SupportLaw::CarryCloseState)を外す。
既定 0 = 使っていないので、外しても答えは変わらない。C-0 の閉じた領域(T-1087)は土台の持ち物なので触らない。

## 完了条件(チェックできる形で)
- [ ] 上の値と CarryCloseState・閉じた刻みの帳簿の数(LastSeeds・CloseLengthMm)を外す
- [ ] support_cost_test の 1 本目の場面は「閉じない」と「粗さで閉じる」だけにする(1 本目の表は T-1171 に残る)
- [ ] support_* の ctest が通る・答えはビット一致・archmap --check

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
