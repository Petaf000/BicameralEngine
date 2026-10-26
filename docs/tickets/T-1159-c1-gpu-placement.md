# T-1159 C-1 の改訂の残り(芯 2): 置かれ方で配るを GPU で・眠る時に捨てる印

- Status: Done(2026-10-26 本体。GPU の回しとビット一致・2-3。残りは T-1176〜T-1178 に分けた)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列・土台の変更要求の束(docs/plan/ROADMAP.md)
- 設計: docs/design/20-transport-substrate.md §10.3・§10.5・17 §3.2 / 決定: D-664・ADR-0469 の 2-1〜2-5・D-532

## 目的(1〜2 行)
T-1157 の「置かれ方で配る」を GPU で回す形にして DXC で通し、CPU リファレンスとビット一致を確かめる。芯 2 の残り(2-2・2-3・2-5)のうち同じ所で済むものを入れ、済まないものを分ける。

## 完了条件
- [x] GPU の回し shaders/sim/placement_refine.hlsli(1 スレッド = 子 1 つ・子の番号は MrChildCell の j)と入口 shaders/sim/placement_refine_check.hlsl。
      state_field.hlsli の置かれ方の式が初めて DXC(-WX)を通った
- [x] tests/gpu_placement_refine_test.cpp: 乱数の親のセル 2000 + 端の場面 9(気体だけ・詰まり切った・据わる固体・物・半分の砂・傾きの頭打ち・凝縮相がごくわずか・気体なし・小さい値の端数)×
      子の体積 3 通り(0.25 m・1 m・4 m の立方体。4 m は 32bit を超える)で sim::PlaceRefineCell・PlaceChildWeights と 1 値ずつビット一致(HW の GPU と WARP。ctest gpu_placement_refine・_warp)
- [x] 子の番号の写し替え sim::PlaceIndexOfMrChild(PlacedChildren::ChildIndex(x, z, 段)⇔ MrChildCell の j)
- [x] 2-3 眠る時に捨てる印 SF_FLAG_SLEEP_DISCARD(導かれる値だけ)・SfSleepValueOf・RegisterWarmStartField の discardOnSleep。field_storage に眠る時に 0 が残る試験
- [x] 今の世界は変わらない(印の付いた欄も口もまだ無い): release の CPU と GPU の多重解像度の組(作業ログ)・archmap --check

## メモ・参考
- **GPU の食い違い(直した)**: `SfOpenVolume`(子の体積 − 凝縮相の体積を 0 で止める)が RTX 3070 Ti だけで差の上の 32bit が 5(物質の数と同じ値)になった。
  差だけ・比べだけを書き出すと正しく、2 つを並べる(select でも印 0/1 の掛け算でも・上下 32bit に分けた select でも)と壊れる。DXIL は正しく WARP は合う(ドライバの側と見る。未確認)。
  T-1078(物の熱)の「32bit を超える定数との引き算」と同じ形の根で、定数でなくても起きる。直し: 32bit の 2 つに分けて借りを手で回す(CPU と同じ値)。
  → 司令塔へ: CLAUDE.md §4 の約束を「シミュの HLSL で、64bit の引き算と同じ値どうしの比べ(0 で止める・min・選ぶ)を並べない。並べるなら 32bit に分けて借りを回す」に広げることを勧める。
  同じ形はほかにも残る(SfSurfaceSlopeQ16 の差と符号の比べ・SfClampColumnSlopeQ16 の min。今の GPU の試験では傾きは入力なので通っていない)→ T-1176 で GPU に載せる時に試験で確かめる。
- 回しの順は CPU と同じ(寄せるか → x で分ける → z で分ける → 列を縦へ → 気体を子の空きの体積で配り直す)。気体の配り直しは子 8 つの空きの和が要るので、呼ぶ側がグループ共有で集める(入口の形)。
  物質は表から毎回読む(書き換えた配列を動的な添字で読まない。T-0397)。
- 2-3: 今の main で支えの刻みをまたぐ共役勾配の向き・残差(D-649)を欄に置いている所は無い(SupportOptions::conjugateDirection の口だけ)ので結果は変わらない。置く時は discardOnSleep = true(support_law.h に書いた)。
- **wt2 T-1134 の「支えの細かくする規則を世界の木へ寄せる時の形」で、ここで受けられた所**: (4) 子の中身 — 支えの物は据わる固体なので置かれ方では SAME の側(CPU・GPU の試験の「据わる固体」の場面で確かめた)。
  置かれ方の関数と重ならない。残り (1) 細かくする要求を「状態で細かくする」の種に・(2) 戻してよいかを家族ごとに断る印・(3) 支えの節 = 木の節・(5) Window の数え方 は T-1178 へ。

## 分けたもの
- T-1176: 木の細かくする段のセルの中身(成分・エネルギー・端数)を置かれ方で(CPU の RefineRequestLevel と GPU の RefineLevel・隣のブロック / 別のレベルの親の詰まりの読み方・GPU の欄の細かくする・観察の影を揃えるか)
- T-1177: 2-2 内訳(登録の印「内訳」と 0 ≤ 内訳 ≤ 親 の検査。形は wt3 T-1136 の 2 本目の結果の後)
- T-1178: 2-5 支えの細かくする形を世界の木へ(wt2 T-1134 の (1)(2)(3)(5))

## 判断待ち(仮で進めたもの)
- 【エンジン】眠る時に捨てる印は欄ごとの印にした(登録の 1 行。全体の切り替え SF_DERIVED_SLEEP_KEEP_COARSE はそのまま)。作り手から見た違い: 書く値は無い。
  捨てる欄は眠った所の保存点に 0 が入り(セーブがわずかに小さい)、起きた刻みは 0 から解き始める。取り消しは登録の印を外すだけ。

## 作業ログ
- 2026-10-26 本体: GPU の回しと入口・GPU の試験・PlaceIndexOfMrChild・2-3 の印。HW の GPU で SfOpenVolume の上の 32bit が壊れたのを、差・比べ・積を書き出して突き止め、32bit に分けた引き算で直した。
  2-2・2-5・木のセルの中身は T-1176〜T-1178 に分けた。
  通した試験(release・警告 0・archmap OK): placement_refine・field_storage・warm_start_save 49 s・multires 40 s・multires_state 67 s・multires_edge・multires_gas 403 s・multires_liquid 413 s・
  gpu_placement_refine・gpu_placement_refine_warp・gpu_multires・gpu_multires_uniform。
