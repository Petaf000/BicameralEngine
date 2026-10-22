# T-0990 日の光を物で集める: GPU 版(T-0878 から分けた)

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 列 4(R-R2 の続き)
- 設計: docs/design/23-radiation.md §5 / 決定: ADR-0425

## 目的(1〜2 行)
T-0878 の CPU リファレンス(sim/sun_lens)を Compute で走らせ、ビット一致させる。費用を GPU で測る。

## 完了条件
- [ ] 影の柱を `SunWalkRayCaught` で(Catcher = レンズの口の格子の点を数える。口の外接の正方形に掛かる光線だけ)
- [ ] レンズの光線 1 スレッド = 1 本(`LnTraceLens` + `LnWalkWorld`)。世界のセルへの足し算は 64bit の atomic
- [ ] 升(2 mm 角)の足し算: レンズごとに升の範囲の密な小さな表か、ハッシュの表(順によらない足し算だけ)
- [ ] CPU とビット一致(sun_lens の場面。HW・WARP)・費用を docs/perf.md に

## メモ
- sun_lens.hlsli は HLSL でもコンパイルできる形で書いた(まだ DXIL にしていない。LN_MAX_STEPS の上限付きの回し・return でループを抜けない)。
- T-0878 の申し送りの節を読む。
