# T-0209 気体: 多重解像度の面を GPU で(CPU の gas_tree と毎刻みビット一致。全部のレベルが同じ小刻み)

- Status: Done(2026-10-11。σ = 0・d = 1 の木まで。2^σ は T-0356、木が変わる時の面の一覧と d ≥ 2 は T-0357、費用の計測は T-0358)
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 1 体(2 時間以内)。始める前に機能単位で分けた(下の「分けたもの」)
- マイルストーン: M3 (docs/plan/ROADMAP.md)
- 設計: docs/design/07-transport.md §2.5・17 §1・§4 / 決定: ADR-0117(追記)・ADR-0017・ADR-0043・ADR-0083・D-302・原則 4
- 関係: T-0185(1 レベルの GPU)・T-0208(木の CPU)・T-0320(2^σ の CPU)・T-0356〜T-0358(このチケットから分けた)・T-0210・T-0321

## 目的(1〜2 行)
gas_tree(T-0208)の 1 小刻みを GPU の Compute に載せる。この段は全部のレベルが同じ小刻み(σ = 0)・隣のレベルの差 1 まで(2:1)の木で、
レベルをまたぐ面の受け渡し(細かい側が計算・粗い側は整数部 + 2^-64 の端数〔GasTreeFraction〕)を含めて HW・WARP が CPU と毎刻みビット一致する。

## 分け方(2 時間の約束で、始める前に分けた)
- **このチケット**: σ = 0・d = 1 の木。面の一覧は木を作った時に CPU が一度だけ作って GPU へ写す(木は作った後に変えない。T-0208 と同じ前提)。
- **T-0356**: レベルごとの 2^σ の小刻みを GPU で(T-0320 の時間線: いちばん細かい小刻みの列・forced / next を粗い小刻みの間持つ・committedOutflow・taken・
  乱数の小刻みの通し番号はレベルごと)。
- **T-0357**: 木が変わる時の面の一覧(細かくする・粗くする)を GPU で作る・d ≥ 2 の面(T-0321 の後)。05 のセルに載せる T-0210 と一緒に考える。
- **T-0358**: 費用の計測(同じ細かさの一様な箱と、木の 1 刻みの GPU 時間。ランナーが idle の時に。docs/perf.md)。

## 完了条件(チェックできる形で)
- [x] GPU の段(`shaders/sim/gas_tree_step.hlsl`・`shaders/common/gas_tree_gpu.hlsli`・`engine/src/sim/gpu_gas_tree.{h,cpp}`)と試験(`tests/gpu_gas_tree_test.cpp`。ctest gpu_gas_tree・gpu_gas_tree_warp)
- [x] 毎刻み CPU(StepGasTree)とセル・端数・帳簿(whole + 2^-64)がビット一致(HW・WARP): 閉じた 2 レベルの箱(gas_tree_test の乱れた箱)・レベルをまたぐ静止大気・
      開いた境界と風(MC・乱数の丸め)・全部レベル 0 の木
- [x] 1 レベルの箱の GPU(gpu_gas・gpu_gas_warp)が変わらず通る(1 セルの部品を shaders/common/gas_cell.hlsli に移して共有したため)

## 作業ログ
- 2026-10-11(Claude・作業役 wt2): gas_tree_step.hlsl(9 段)・gas_tree_gpu.hlsli・gpu_gas_tree.*・gpu_gas_tree_test.cpp・`ListGasTreeFaces`(gas_tree.h)。
  1 セルの部品を gas_step.hlsl から shaders/common/gas_cell.hlsli へ移して共有(式は変えていない)。CPU と GPU の形の写しは gpu_gas_internal.h へ(gpu_gas.cpp の無名の名前空間から出した)。
  初めのビルドで通り、HW・WARP とも 5 場面が初めから毎刻みビット一致した(閉じた箱 8 刻み〔端数を持つセル 64〕・静止大気 4・開いた境界と風 8〔端数 36〕・minmod と切り捨て 6・全部レベル 0 の 6)。
- 試験の時間(6 つのランナーの並走の負荷の下): debug は gpu_gas_tree 164 秒(HW。GPU-based validation)・gpu_gas_tree_warp 31 秒。
  release は gas_tree 9 秒・gpu_gas 36 秒・gpu_gas_warp 579 秒・gpu_gas_tree 12 秒・gpu_gas_tree_warp 23 秒(全部通過。gas_cell.hlsli へ移した後の 1 レベルの箱も一致)。
- archmap --check: OK(map.yaml に GpuGasTree・gas_tree_step・gas_cell・gpu_gas_tree_test)。07 §2.5・ADR-0117 追記。

## 引き継ぎメモ
- 使い方: `GpuGasTree::Create(device, tree)`(tree = MakeGasTree の木。σ > 0 なら T-0356 と言って断る)→ `RecordUpload` → `RecordStep` → `RecordReadback` → `Read(cells, fractions, ledger)`。
  帳簿は `GasTreeTotals` と同じ並び(whole + 2^-64 × 8)。
- 形: 面の一覧(`ListGasTreeFaces` = EnumerateFaces と同じ順)を `GasTreeGpuFace`(64 バイト)で写し、セルは `GasTreeGpuPlace` の [faceBegin, faceEnd) で自分の面の並び
  (面の番号 × 2 + 右側か)を集める。外の側の番号は「層の表」(レベルごとの層をレベル 0 から続けた表)の番号。
- 粗い側への足し込みはセルの段で `AddScaled`(GPU 版。繰り上がりはビットで出す)。帳簿は `LedgerAddScaled`(下位へ原子的な足し算 → 前の値から繰り上がり → 上位へ)。
- T-0356(2^σ)へ: 今の段は σ = 0 の MicroStep と同じ。committedOutflow・taken は σ = 0 では常に 0 なので持っていない(T-0356 で足す)。
- 切り分けの道具は 1 レベルの gpu_gas_test のような --dump はまだ無い(要らなかった)。`--scene`・`--ticks` はある。

## 判断待ち
- なし(実装の細部だけ。ADR-0117 追記)

## 分けたもの
- T-0356・T-0357・T-0358(上の「分け方」)
