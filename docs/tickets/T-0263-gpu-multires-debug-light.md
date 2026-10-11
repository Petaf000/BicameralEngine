# T-0263 debug の HW の gpu_multires を軽くする

- Status: Done(2026-10-11)
- 種類: 工学
- PC: 必須
- 見積もり: 小(作業役 1 時間以内)
- マイルストーン: M2(docs/plan/ROADMAP.md)
- 設計: docs/design/16(テスト)/ 関係: T-0239・ADR-0079 / 元: 2026-10-11 の統合

## 目的(1〜2 行)
debug の HW の gpu_multires が 6 つのランナーの並走で 1200 秒の上限に届く(release は約 326 秒)。release の確かめる範囲は変えずに、
debug だけ軽くする。

## 完了条件
- [x] debug では 2 回目の実行(決定性。本物の鎖・たくさんの要求)と時間の計測(暖機 400 刻み × 2)を省く。場面と刻みの数は release と同じ
- [x] 場面ごとの壁時計の秒をログに出す(次にどこが重いか見るため)
- [x] ctest の HW を 2 本に分ける: gpu_multires(`--part chains`。GBV あり)・gpu_multires_wide(`--part wide --no-gbv`)。WARP は 1 本のまま
- [x] debug の 2 本が 1200 秒の中で通る・release の gpu_multires(_wide|_warp) は今までどおり

## 作業ログ
- 2026-10-11(作業役・本体): debug(-O3 のシミュのシェーダー・GBV あり)の場面ごとの秒(6 ランナーの並走): 本物の鎖 73・影の鎖 74・たくさんの要求 99・上限 74・上限・伝導 58、
  溢れの場面は 1 つ 210〜290 秒(上限・溢れ 276・同・伝導 214・大きい溢れ 286・同・伝導 263 で 1200 秒切れ)。GBV を切ると溢れの 7 場面で 82 秒(初めの 1 場面の 53 秒はほぼ
  パイプラインの作成)= 溢れの場面の重さは GPU-based validation。release の HW は 1 場面約 30 秒(WARP は 3 秒)。
  変えたこと: debug は 2 回目の実行と計測を省く(REPEAT_AND_MEASURE)・Run を鎖の場面と溢れの場面に分けて `--part all|chains|wide`・ctest の HW を 2 本
  (溢れの場面は `--no-gbv`。debug layer・FX_ASSERT は残る。溢れの場面の GBV は WARP では元から無い)。release の確かめる範囲は同じ。
- テスト: debug の gpu_multires(鎖)404 秒・gpu_multires_wide 345 秒(並走で重い時。軽い時 82 秒)・gpu_multires_warp 113 秒が通過。release の gpu_multires 105 秒・_wide 213 秒・_warp 57 秒が通過。
  **束の正規表現に `gpu_multires_wide` を足すこと**(`gpu_multires(_warp|_quiet|…)?` には入らない)。

