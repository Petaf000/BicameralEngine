# T-0084 128÷64 の割り算を速くする

- Status: Done(2026-09-30)
- 種類: 工学(速くなる幅は未確認。打ち切り条件つき)
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: M1 (docs/plan/ROADMAP.md)
- 設計: docs/design/04-numerics-determinism.md §3・§6 / 決定: ADR-0010・D-322(BACKLOG 2026-09-30 から)

## 目的(1〜2 行)
`FxDivU128By64`(1 ビットずつ 64 回、fmul の約 1900 回分)を速い方法に替え、`FxDivShiftS64`・`FxMakeRecipU64` も速くする。
結果は今と**ビット単位で同じ**(商と余りが厳密)。

## 完了条件(チェックできる形で)
- [x] `FxDivU128By64` を新しい方法に替える。関数の約束(numerator.hi < divisor、商と余りが厳密)は変えない
- [x] fixed_test: MSVC の `_udiv128` と、ランダム + 境界(除数が 1・2^63・2^64−1、hi = divisor−1、2 のべき)でビット一致
- [x] GPU(3070 Ti・WARP)で自己テストが CPU とビット一致(`job.py test` が全部通る)
- [x] `gpu_fixed_bench` で測り直し、04 §6 の表の該当行(128÷64・FxDivShiftS64・FxMakeRecipU64)を更新。DXIL の命令数も
- [x] build(debug / release)・tidy が警告なし、clang-format・archmap --check が通る

## 方法の候補
- A: 除数を正規化(最上位ビットを立てる)し、逆数 v = floor((2^128 − 1) / d) − 2^64 を 32bit の割り算 1 回 + Newton 法(掛け算だけ)で作り、
  Möller & Granlund(2011)"Improved division by invariant integers" の 2/1 の割り算(掛け算 1 回 + 直し 1〜2 回)で商を出す。
- B: Knuth の算法 D(基数 2^32。商の 32bit ずつを 64bit の割り算で見積もって直す)。ただし GPU の 64bit の割り算自体が fmul の 113 回分。

## 打ち切り条件
- A・B とも今の 1/2 より速くならなければ、今の方法のまま残し、測った値を 04 §6 に書いて Done にする。

## 作業ログ
- 2026-09-30: 方法 A(Möller & Granlund 2011 の RECIPROCAL_WORD + DIV_2_BY_1)を採用。最初の見積もりは論文の表の代わりに 32bit の割り算 1 回。
  先に Python で 64bit の剰余演算を真似て、40 万件 + 境界で商と余りの一致を確かめてから移した。新しい関数 `FxReciprocalNormalizedU64`。
- fixed_test に境界(除数 1・2 のべき ± 1・2^64 − 1、上位 = 除数 − 1 など)と偶数の除数・逆数の比較を足した。自己テストの要約 605bc2e41947d188 は変わらず(GPU・WARP でビット一致)。
- 費用(3070 Ti、fmul = 1): 128÷64 1925 → 247、FxDivShiftS64 1829 → 247、FxMakeRecipU64 1494 → 301(約 1/7.8)。打ち切り条件(1/2)を満たす。
  ベンチの div128・divshift64 は除数が同じだと逆数を作る所がループの外へ出る(75 になる)ので、除数を毎回変えるように直した。
