# T-0397 HW の活性・伝導のグラフが最初の刻みで止まる(T-0359 の転換の後)

- Status: Done(2026-10-12。本体)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: docs/design/02-reaction-system.md §5 / 決定: ADR-0095・ADR-0002

## 目的(1〜2 行)
T-0359(魔法の転換)を main に合わせた後、HW の Work Graph の gpu_multires_activity(と、ときどき uniform・conduction)が
ワークグラフ MultiresActivity を作った直後の最初の刻みで DXGI_ERROR_DEVICE_HUNG になる(WARP は通る)のを直す。

## 完了条件(チェックできる形で)
- [x] 原因の切り分け(転換を外した変種で HW が通る)
- [x] 直す。CPU リファレンスとのビット一致を保つ(同じ式を CPU と GPU が使う)
- [x] HW の Work Graph の試験と CPU だけの組を release で流す

## メモ・参考
- **切り分け**: MpPatchedTable::Rule の転換の展開を外した(常に元の規則)だけで、gpu_multires_activity(HW)が通った。
  今の main(e6701b1)の release では uniform と conduction は通り(単独でも ctest でも)、activity だけが 3 回とも止まった。
- **原因(見立て)**: 転換した規則は「バッファから読んだ値」でなく書き換えた局所の値になる。核(reaction.hlsli)は規則の配列を
  `i < rule.reactantCount` などの動的な添字で読むので、規則を読む所ごとに `alloca [3 x i32]`(局所の配列 = GPU のスクラッチ)ができる
  (activity のグラフで 125 個。転換の前は 0 個)。DXIL の行数(27238 → 38000)より、局所の配列が HW の Work Graph のノードで止まる方が効いた見込み
  (行数は直した後も 35477 で、元の 38000 と大差ないのに通る)。
- **直し**(意味は同じ。CPU と GPU は同じ式):
  1. Table の約束に `RateRule(id)` を足した(速さ・優先度・反応物だけを読む所。ふつうの表は Rule と同じ、MpPatchedTable は転換の前の規則)。
     候補を集める・優先度・使う量・縮める・取り消す・反応物を引く はこれで読み、反応熱だけを `Rule(id).reactionEnthalpy` から読む。
     新しい Table 型を作る時は RateRule も要る(multires_bindings・multires_patches・probe_bindings・reaction_cells・reaction_table.h の 5 つに足した)。
  2. 生成物を足す・新しい物質を作るかの回しを決まった数(RX_MAX_PRODUCTS)+ `FX_UNROLL`(fixed.hlsli。HLSL は [unroll]、C++ は空)に。
     MpConvertSlot の欄の選び方も同じ(動的な添字を使わない)。
  3. 転換の反応熱は全部の物質を読み直さず、替えた欄の分だけ足し直す(MpSlotConversion::enthalpyChange)。ベイクの反応熱は
     Σ 生成物の H0 − Σ 反応物の H0 なので、整数で求め直したのとビット単位で同じ(lab_box の「替えてベイクした表と同じ規則」の試験で確かめ)。
  局所の配列 [3 x i32] は activity 125 → 0・step_wait 26 → 0。DXIL の行数は activity 37912 → 35477・step_wait 14766 → 13903。
- 道具: runner/dx/t0397*.ps1(git 管理外。4 本のシェーダーを dxc で作り、行数を出す)。`grep -c "= alloca \[3 x i32\]"` で局所の配列を数える。
- 費用(転換で大きくなった分)は T-0396 で測る。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-12(本体): 今の main で activity だけが止まるのを確かめ、転換を外した変種で通ることで原因を転換に絞った。[loop] は効かず(もともと開いていない)、
  反応熱の差分・RateRule で行数が少し減っただけ。DXIL の局所の配列(alloca [3 x i32] が 125 個)を見つけ、生成物の回しを決まった数にして 0 にしたら HW で通った。
  HW の Work Graph の試験・CPU だけの組を release で流した(結果は HANDOFF)。
