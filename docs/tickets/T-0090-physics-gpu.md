# T-0090 物理を GPU に載せる(整数の AVBD の 1 刻みを Compute で・CPU とビット一致)

- Status: Done(2026-10-02。T-0016 から分けた。2026-10-01 に Work Graphs と計測を T-0092 に分けた)
- 種類: 工学
- PC: 必須
- マイルストーン: M1 (docs/plan/ROADMAP.md)
- 設計: docs/design/08-bodies-physics.md §2 / 04 §6 / ADR-0014(Proposed)

## 目的(1〜2 行)
T-0016・T-0091 で CPU で確かめた整数の AVBD(shaders/common/physics_*.hlsli)の 1 刻みを GPU の Compute で走らせ、CPU とビット一致させる。
Work Graphs 版はこれを正しい答えにして T-0092 で作り、測って比べる。

## 完了条件(チェックできる形で)
- [x] 物・接触の構造体と、物ごと・組ごとの手順を共通のヘッダ(shaders/common/physics_step.hlsli)に移し、CPU の世界(physics_world.cpp)もそれを呼ぶ。
      整数の山のハッシュ(422a771e9ae913f7)と physics_test が変わらない
- [x] GPU の Compute の 1 刻み(活性 → 広域の選別 → 接触の生成と引き継ぎ → β → 初期化 → 線形化 → 彩色 → 反復(途中の探し直し・色ごとの解・λ と硬さ)→ 速度 → 仕上げ)
- [x] CPU リファレンスとビット一致・2 回の実行でハッシュ一致(場面 A・質量比・C 小)。RTX 3070 Ti で(WARP はパイプラインの JIT が 160 s で終わらず未確認)
- [x] GPU の時間を docs/perf.md に記録する(比べる相手は T-0092 の Work Graphs 版)

## 移したもの(T-0092 へ)
- 広域の選別・接触の生成を Work Graphs で、ソルバーの反復を Work Graphs と Compute の両方で作って測り、安い方(D-302)
- Nsight で、演算と帯域のどちらで詰まっているかを 1 回記録する(04 §6 と docs/perf.md)/ 04 §6 の表の NVIDIA(SASS)の命令数(ADR-0010)

## メモ・参考
- 組は「持ち主」の物の枠に入れる(持ち主 = 片方が動かない物なら動く方、それ以外は小さい番号)。持ち主の枠の中は相手の番号の昇順。
  CPU の std::map の順とは違うが、物ごとの 6×6 への足し込み(PxAddRow)は整数の和なので順番に依存しない。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-01〜02: 手順を shaders/common/physics_step.hlsli に移し(CPU の山のハッシュ 422a771e9ae913f7 は不変)、GPU の Compute のパスの列(physics_step.hlsl・sim::GpuPhysics)を作った。
  3 場面の全部の刻みで CPU とビット一致・2 回で一致(gpu_physics_test、ctest の gpu_physics_*)。debug の GPU で FX_ASSERT 0 件。
  詰まった所: HLSL の `?:` は構造体を返せない・`linear`/`point`/`half` は予約語・組 3016 B が構造化バッファの要素の上限 2048 B を超える(見出しと点に分けた)・
  debug で GPU-based validation を有効にするとパイプラインの作成が終わらない(このテストは GBV を切った)。速さは 1 刻み 2.7〜12 ms(最適化なし。T-0092)。
