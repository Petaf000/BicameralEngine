# T-0147 LvTail を WARP で動かして既定にする(release の WARP でデバイスが失われる原因の切り分け)

- Status: Done
- 種類: 研究(不具合の切り分け。原因が読めない)→ 直れば工学(分岐を消す)
- PC: 必須
- 見積もり: 作業役 2 時間以内(時間の約束。1.5 時間で区切りの良い所まで書いてコミット)
- マイルストーン: M2 の並走(D-432)
- 設計: docs/design/17-multiresolution.md §6 / 決定: D-428・D-432・D-436 / ADR-0019(T-0135・T-0179 の追記)

## 目的(1〜2 行)
T-0135 の LvTail(多重格子の段を作る回を 1 グループ 1024 スレッドで最後まで回す)は HW で CPU とビット一致するが、release の WARP で
デバイスが失われる。原因を切り分けて WARP でも動かし、gpu::IsSoftwareDevice の分岐(GpuMultiresImplicit::Create)を消す。

## 完了条件(チェックできる形で)
- [x] release の WARP で LvTail を積む形(全部 LvTail・回 8 + 境 1024)が CPU と毎刻みビット一致(gpu_multires_implicit_build_warp。WARP も 3 つの形を流す)
- [x] IsSoftwareDevice の分岐を消し、HW・WARP・CPU が毎刻みビット一致(gpu_multires_implicit(_conduction|_tree|_build)?(_warp)?。下の作業ログ)
- [x] 原因と最小の再現をここに残す(下の「切り分けの結果」)
- [ ] debug(HW・WARP)と tidy は時間の約束で流していない

## 研究の打ち切り条件
- 2 時間(10:06 UTC 開始 → 12:06 UTC まで)。1.5 時間(11:36 UTC)を過ぎたら、その時点の結果と残りの仮説を書いてコミット。
- 試す仮説の順(1 回のビルドで変種のシェーダーを並べて、WARP で 1 本ずつ流す):
  1. 切り分け(同じ本体の変種): -Od(= debug の WARP と同じ最適化なし)/ 回のループを無くして 1 回だけ / AddCoefficients(局所の配列 alloca を持つ唯一の本体)を抜く /
     本体の前半(節の鍵・接頭和)だけ。どれが通るかで「最適化」「バリアを含む入れ子のループ」「局所の配列 × バリア」「大きさ」を分ける。
  2. 1 で当たった形を、結果を変えずに直す(本体を同じ関数のまま、形だけ変える)。
  3. 1 で何も通らなければ: バリアの置き場所を 1 か所にした平らな形(段の番号のループ + switch)/ s_depth を引数に / 256 スレッド。
  4. 3 でも通らなければ打ち切り(今の分岐のまま。最小の再現を残す)。

## 切り分けの結果(2026-10-09、release の WARP。`gpu_multires_implicit_build_test --warp --queue compute` を全部 LvTail の形で。試した変種は消した)
試験に一時的に `--tail-shader <名前>`(LvTail の .cso を差し替える)を足し、変種を 1 回のビルドで並べて 1 本ずつ流した(1 本 4〜50 秒)。
| 変種 | 中身 | 結果 |
|---|---|---|
| そのまま(-O3) | T-0135 の LvTail | デバイスが失われる(3.6 秒。最初の Dispatch) |
| -Od | 同じソース・最適化なし(debug の WARP と同じ) | **通る**(3 場面・全部 LvTail で CPU と毎刻みビット一致。49 秒) |
| -O0 | 同じソース | **通る**(44 秒) |
| -O1 | 同じソース | 落ちる |
| 回のループなし | 1 回だけ回す | 落ちる |
| AddCoefficients なし | 局所の配列(alloca)を持つ唯一の本体を抜く | 落ちる |
| 前半だけ | 回のループ + NodeHash・NodeFirst・親の接頭和 | 落ちる |
| 接頭和だけ・1 回 | 親の接頭和(グループのバリア 20 個)だけ | 落ちない(値は違う。切り分け用) |
| NodeHash だけ・1 回 | 項目のループ + NodeHash + バリア 1 つ | **落ちる**(最小の再現) |
| NodeHash だけ・項目のループなし | `if (thread < nodes) RoundNodeHash(thread)` + バリア | 落ちない |
| NodeHash だけ・ループを印で抜ける | 表を探すループの return を done の印に | 落ちない |
- **原因(分かった)**: 本体の表を探すループの中の return。LvTail の項目のループ(`for (item = thread; item < count; item += 1024)`)に inline されると、
  内側のループの出口が外のループの次の項目(latch)へ直接つながる形になり、-O1 以上の DXC の出力のその形を WARP の JIT が扱えない。
  バリアの数・シェーダーの大きさ・1024 スレッド・`s_depth` の静的変数・alloca・早い return(関数の頭の)は原因ではなかった(T-0135 の仮説は外れ)。
  同じ形の本体は 4 つ(RoundNodeHash・RoundNodeFirst・RoundLinkHash・RoundCount)。Dispatch の入口(項目のループがない)は前から WARP で動いていた。
- **直し方**: 4 つのループを印(done・found)とループの条件で抜ける形に(式も順も同じ)。そのままの -O3 で WARP の全部 LvTail が CPU と毎刻みビット一致(42 秒)。
- 直した後の HW の費用(`--queue compute --measure-only`、負荷あり。前 → 後、回 8・境 256): 熱い点 0.190 → 0.223 ms(最初の場面は暖機でぶれる)・
  鎖 0.460 → 0.459・たくさんの要求 2.064 → 2.070 ms。全部 LvTail も 0.093 → 0.120・0.955 → 0.972・19.5 → 19.4 ms。変わらない(perf.md には載せていない)。
- -O0 で逃げる案も測った(HW・負荷あり。回 8・境 256): 熱い点 0.190 → 0.266 ms・鎖 0.460 → 0.459・たくさんの要求 2.064 → 2.045 ms。原因が分かったので使わない。

## 何をしたか(形。決めたのは Claude〔実装の細部〕。ADR-0019 の T-0147 の追記)
- shaders/sim/implicit_levels.hlsl: RoundNodeHash・RoundNodeFirst・RoundLinkHash・RoundCount の表を探すループを、return ではなく印(done・found)と
  ループの条件で抜ける形に。LvTail の節に「本体のループを return で抜けない」決まりを書いた。
- GpuMultiresImplicit::Create: gpu::IsSoftwareDevice の分岐を消し、WARP も小さい段の回(節 ≤ 256)と残りの回を LvTail で積む。
  gpu::IsSoftwareDevice は残した(使う所は無い。WARP で落ちる形を避ける道具)。
- 試験: gpu_multires_implicit_build_test の WARP も 3 つの積み方(全部 Dispatch・回 8 + 境 1024・全部 LvTail)を CPU と毎刻み比べる。
  gpu_multires_implicit_conduction_test の「一番安い積み方」は WARP でも全部 LvTail になった(ログの「上限まで Dispatch(WARP)」はもう出ない)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役、wt2・約 1.7 時間): LvTail の変種を 1 回のビルドで並べて release の WARP で二分し、-O0/-Od なら通る・項目のループ + NodeHash だけで落ちる・
  NodeHash のループを印で抜けると通る、まで絞った。原因は本体の表を探すループの中の return(inline で 2 段抜けになる形を WARP の JIT が扱えない)。
  4 つの本体を直し、IsSoftwareDevice の分岐を消した。release の `-Filter "^gpu_multires_implicit(_conduction|_tree|_build)?(_warp)?$"` 8 本(結果は下の引き継ぎメモ)。
  debug・tidy・idle の計測は時間の約束で流していない。

## 引き継ぎメモ(HANDOFF に載せるもの。司令塔がマージ後に反映)
- 状態: T-0147 完了。LvTail は HW も WARP も積む(GpuMultiresImplicit の IsSoftwareDevice の分岐を消した)。CPU・HW・WARP は毎刻みビット一致。
- 原因(HANDOFF の「注意」に足す): **LvTail のような「1 グループが項目のループで本体を回す」シェーダーの本体では、内側のループを return で抜けない**
  (inline されると内側のループから外のループの次の項目へ直接飛ぶ形になり、release〔-O1 以上〕の WARP の JIT が最初の Dispatch でデバイスを失う。
  -O0・-Od・HW は通る)。抜ける時は印とループの条件で。
- 動いているもの: release の `-Filter "^gpu_multires_implicit(_conduction|_tree|_build)?(_warp)?$"` 8 本が最初のビルドで通過(HW: implicit 21 s・tree 443 s・
  build 375 s・conduction 929 s / WARP: implicit 7 s・tree 198 s・build 117 s〔3 つの積み方〕・conduction 760 s〔前 861 s〕。本体・wt3・wt4 のランナーが試験中の負荷あり)。
- 壊れているもの: なし。debug(HW・WARP)と tidy は未実行(シェーダーと C++ の変更は小さい。C++ はコメント・分岐の削除・試験の形だけ)。
- 決めたこと(Claude・実装の細部): ADR-0019 追記(T-0147)。HANDOFF の陰解法の節の「WARP は LvTail を積まない」「IsSoftwareDevice の分岐」の記述は消してよい。
- 判断待ち: なし。

## 判断待ち
- なし(積み方は費用だけを変え、解いた値は同じ。遊びへの影響なし)。

## 分けたもの
- なし。
