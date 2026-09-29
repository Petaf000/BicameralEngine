# T-0010 整数の数学ライブラリ(HLSL と C++ で共通のソース)・単位の表・単体テスト

- Status: Done(2026-09-30。追加分の SASS・RDNA3 の命令数は BACKLOG へ)
- 種類: 工学(exp・log の精度と速さの目標は研究。04「研究」)
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: M1 (docs/plan/ROADMAP.md)
- 設計: docs/design/04-numerics-determinism.md §2〜3 / 決定: D-205・D-306・D-307

## 目的(1〜2 行)
シミュの GPU カーネル・CPU リファレンス・ベイクが**同じ 1 つのソース**で整数の計算をする土台を作る。
以後のチケット(伝播・化学・物理・多重解像度)は、ここにある関数だけで計算する。

## 完了条件(チェックできる形で)
- [x] `shaders/common/fixed.hlsli` が C++(MSVC)と HLSL(DXC, cs_6_8)の両方でコンパイルできる(ビルドの中で DXC が自己テストのシェーダーを通す)
- [x] 関数: 128bit の積(符号なし・符号つき)/ Q 形式の積(0 方向の切り捨て)/ 128÷64 の割り算と Q 形式の割り算 / 整数の平方根 /
      log2・exp2・ln・exp(Q32)/ sin・cos(CORDIC、角度は 1 周 = 2^32)/ カウンタ型の乱数 `FxHash64(seed, tick, id, purpose)`
- [x] 単位の表 `shaders/common/units.hlsli`(04 §2 の表と同じ値。表に角度と Q32 の行を足す)
- [x] 単体テスト `fixed_test`: 積と割り算は MSVC の 128bit 組み込み関数とビット一致、平方根は定義どおり(r² ≤ x < (r+1)²)、
      exp・log・sin・cos は double との誤差の最大値を測って上限以内(値は perf.md ではなくこのチケットのログに残す)、乱数は既知の値と雪崩の性質
- [x] 図(docs/architecture/map.yaml)を新しい設計(06 の 1 刻みの段・02 のベイク)に合わせて直す(NEXT.md 2)
- [x] GPU で走らせて CPU とビット一致 → **T-0013 に回す**(int64 の対応を確かめるチケットで、この自己テストのシェーダーを GPU で走らせる)
- [x] (2026-09-30 追加)ルーチンごとの命令数(DXIL)と 3070 Ti での実測を表にして 04 §6 に載せる。逆数の掛け算を足してビット一致を確かめる
- [ ] 同じ表の NVIDIA(SASS)・RDNA3(ISA)の命令数 → BACKLOG(Nsight Graphics の GUI・RGA のインストールが要る。ユーザーと決める)

## 作業(2026-09-30 追加)
- 各ルーチンのマイクロベンチ: 掛け算とシフト・割り算・逆数の掛け算・平方根・指数・int64 の四則(比べるために 32bit と float も)。
  逆数の掛け算は fixed.hlsli にまだ無いので、このときに足す(ビット一致のテストも)。
- 結果の使い道: 割り算を逆数の掛け算に置き換える所と、int64 を使う範囲を決める(04 §6)。

## メモ・参考
- 1 つのソースを両方で使うための約束: 型は `int32_t`/`uint32_t`/`int64_t`/`uint64_t`(DXC も持つ)、関数は `FX_FN`、定数は `FX_CONST`、
  64bit の定数は `FX_U64(上位 32bit, 下位 32bit)`(HLSL のリテラル接尾辞に頼らない)。符号つきの桁あふれは C++ では未定義なので、
  桁あふれしうる計算は符号なしで行う。64 以上のシフトはしない(C++ では未定義、HLSL では下位 6bit に丸められる)。
- exp2・log2 は「ビットごとに決める」方法(log2 は 2 乗の繰り返し、exp2 は 2^(2^-k) の定数表)。表はベイクではなくソースに書いた定数(Python の decimal で作った)。
  速さは GPU で測る(T-0013)。遅ければ多項式に替える(04「研究」の代案)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-09-30: fixed.hlsli・units.hlsli・fixed_selftest.hlsli(共通)、sim/fixed_selftest.hlsl(DXC がビルドで通す)、tests/fixed_test.cpp を作った。
  DXC は cs_6_8・-WX で警告なし(64bit の割り算を含む)。MSVC /W4 も警告なし。debug と release で自己テストの要約が同じ(85c154e666febd92、65536 件)。
  実測: log2 ≤ 1.0 × 2^-32、ln ≤ 1.7 × 2^-32、exp2 の相対誤差 ≤ 2.2e-16、exp ≤ 1.6e-10、sin/cos ≤ 17.6 × 2^-30、乱数の雪崩 31.99 ビット・カイ 2 乗 4.89。
  `job.py test -Filter fixed -Show` でテストの表示(測定値)も見られるようにした。図を 06 の 1 刻みの段・02 のベイクに合わせて直した。
  持ち越し: GPU での実行と速さ → T-0013、レベルごとの単位の余り → T-0017(BACKLOG)。
- 2026-09-30(追加分): fixed.hlsli に逆数の掛け算(FxMakeRecip*/FxDivRecip*、Granlund-Montgomery)を足し、fixed_test(境目と乱数)と
  GPU の自己テスト(21 値、要約 605bc2e41947d188、3070 Ti・WARP)でビット一致。マイクロベンチ shaders/bench/fixed_bench.hlsl(演算ごとに DXC、
  bicameral_add_shader に NAME・DEFINES・FLAGS)と tests/gpu_fixed_bench.cpp(タイムスタンプ、ctest 外)、DXIL の数え方 tools/fixed_bench/dxil_count.py。
  結果(fmul = 1): int64 の割り算 113・逆数 33、128÷64 1925、exp2 553・log2 1190 → 04 §6 と perf.md。128÷64 の高速化と SASS/RGA は BACKLOG。
