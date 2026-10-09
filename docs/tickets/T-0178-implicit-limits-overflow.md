# T-0178 陰解法の系が VRAM の上限を超えた刻みの扱い(CPU リファレンスに同じ判定を入れてビット一致)

- Status: Done(Q22 は未回答のまま仮で案 A。B・C は口だけ。T-0179 は時間の約束で手を付けていない)
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 2 時間以内(始める前の見積もり: 判定は「流れの段の前に、系に入れるブロックを枠の順に予算の中まで選ぶ」形にし、
  系を作る段の CountBlocks・ScanBlocks を流れの前へ動かして使う。CPU の判定・GPU の段・試験〔上限を小さくした形〕で 1.5 時間)
- マイルストーン: M2 の並走(D-432)
- 設計: docs/design/17-multiresolution.md §6 / 決定: D-428・D-434・D-436 / ADR-0019(T-0178 の追記)/ QUESTIONS Q22(未回答。仮で案 A)

## 目的(1〜2 行)
陰解法の系(未知数・セル・多重格子の節・隣)が GpuMultiresImplicitLimits(VRAM に先に取る大きさ)を超える刻みでも、GPU が CPU と
毎刻みビット一致し、熱の値が壊れない(保存も守る)ようにする。

## 完了条件(チェックできる形で)
- [x] 入りきらないブロックはその刻みだけ陽解法(頭打ち)のまま進める(Q22 の案 A。仮〔ユーザー未確認〕)。B・C は選べる口だけ
- [x] CPU リファレンス(MarkImplicitBlocks・BuildImplicitGrid の上限)に同じ判定を入れ、選ぶブロックの順は頁と同じ枠の順
- [x] 上限を小さくした試験(ブロックが入らない・多重格子の段が入らない)で、全部・活性の刻みとも CPU と HW・WARP が毎刻みビット一致し、
      保存量(ComputeConservedTotals)も一致(伝導だけの場面は刻みの初めとも一致)

## 何をしたか(形。決めたのは Claude〔実装の細部〕。ADR-0019 の T-0178 の追記)
- **上限と扱いを刻みの選択に**: MultiresStepOptions::implicitLimits(MultiresImplicitLimits = GpuMultiresImplicitLimits。欄ごとに 0 は上限なし〔CPU だけ〕)・
  implicitOverflow(ImplicitOverflow::Explicit = 案 A。Freeze〔B〕・StopRefine〔C〕は未実装で FX_ASSERT)。GPU は EnableImplicitConduction の上限を使う。
- **予算**(MakeMultiresImplicitBudget): 未知数 ≤ min(unknowns, links / 12)・セル ≤ min(cells, nodes)。段 0 の節 = セル、段 0 の隣 = 面 × 2 ≤ 未知数 × 12 なので、
  予算に入れば段 0 はいつも入る。ブロック 1 つのセルの見込みは ImBlockCellBound(未知数 + min(未知数 × 6, 384)。境のセルはブロックの外へ出る面からしか届かない)。
- **選ぶ**(流れの段の前。CPU の MarkImplicitBlocks / GPU の implicit_build.hlsl の ScanBlocks): 入れられる(頁があり凍っていない)基準より細かい世界のブロックを
  枠の順に、未知数と見込みの和が予算に入る所まで。和は枠の順に増えるので「入らなくなった所から後は全部陽解法」(GPU は接頭和で同じ判定)。
  GPU は系を作る段の Clear → CountBlocks → ScanBlocks を流れの段の前に投げ(GpuImplicitBuild::RecordAdmit)、選んだブロックに伝導の作業場の印
  CONDUCT_MARK_IMPLICIT(語 7)を付ける。流れの段(InImplicitConduction)・系を作る残りの段(IsUnknownBlock)は印を見る。
- **選ばれなかったブロックとの面**: 粗い側・細かい側の面は今までの境のセル・Δk > implicitMaxGap と同じ形のまま(細かい側が計算)。同じレベルの面だけは、
  選ばれなかった側のセルが境のセルとして陰解法の系で受け、陽解法の側はその面を計算しない(CollectCellFaces / AddCellFlows。保存はビット単位のまま)。
  上限なし(既定)では選ばれないブロックは凍った・一様なものだけなので、今までの結果と同じ。
- **多重格子の 2 段目から**: 次の段の節を足すと nodes を超えるか、隣が「今までの和 + 段 d の隣」(次の段の隣は段 d の隣以下)で links を超えるなら、
  その段を作らずに縮約を止める(CPU の BuildImplicitGrid の ImplicitGridLimits / implicit_levels.hlsl の ParentScanGroups。今までの overflow の代わり)。
  段が浅いと V サイクルの回数が増えうるが、保存と安全網は同じ。
- 試験: gpu_multires_implicit_conduction_test に「ブロックが入らない上限」(未知数を CPU の系の 1/3)・「段が入らない上限」(節 = セルの見込み・隣 = 段 0 の見込み)を
  足し、熱い点・鎖・たくさんの要求の全部・活性の刻みで、CPU と同じ上限を渡して毎刻み比べる(状態・保存量・次の刻みの種・V の回数・安全網)。
  ふつうの形の上限は全部のブロックが入る大きさ(セル ≥ 未知数 × 7・隣 ≥ 未知数 × 12)に広げた。

## 結果(2026-10-09、release、RTX 3070 Ti と WARP)
- `-Filter "^(multires|multires_implicit(_tree)?|multires_conduction|gpu_multires_implicit_conduction(_warp)?)$"` の 6 本が通過(HW 700 s・WARP 531 s。最初のビルドで通った)。
- `-Filter "^gpu_multires_(implicit_build|conduction)(_warp)?$"`: conduction の 2 本は通過。implicit_build の 2 本は、試験の隣の上限が CPU の段の和ちょうど
  (余りなし)だったので、新しい見込み(今までの和 + 段 d の隣)で GPU だけ縮約を止めて落ちた → 試験の上限に段 0 の隣(面 × 2)を足して直した(下の作業ログ)。
- 上限の形ごとの様子(HW・WARP とも全部・活性の刻みで毎刻み一致・保存量も一致):
  | 場面 | ブロックが入らない上限(未知数 = CPU の系の 1/3) | 段が入らない上限(節 = セルの見込み・隣 = 段 0 の見込み) |
  |---|---|---|
  | 熱い点(1 ブロック) | 12 刻みとも全部陽解法(系 0 セル) | 段は浅くならない(V 2 回のまま) |
  | 鎖 | 36 刻みとも一部のブロックを陽解法(系 499 セル・V 1 回) | 35 刻みで段が浅い。V 4 → 16 回 |
  | たくさんの要求 | 8 刻みとも一部を陽解法(系 6828 セル・V 11 回) | 8 刻みとも段が浅い。V 15 → **64 回(上限)** = 安全網が陽解法に戻す |
- 計測(負荷あり: 本体・wt3 のランナーが試験中。docs/perf.md には入れていない): 鎖 4.22(全部)/ 4.36(活性)ms の後の平均で T-0132(4.2〜4.3)とほぼ同じ。
  選ぶ段(Clear・CountBlocks・ScanBlocks)は流れの前へ動かしただけで、Dispatch の数は変わらない。
- 段が入らない上限では反復が大きく増える(たくさんの要求で V 64 回に当たる)。D-436(解き切る)とぶつかるので、節・隣の上限は「段 0 の見込み × 2」程度より
  小さくしない方がよい(既定の大きさを決める時の材料。下の「判断待ち」の補足)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役、wt2・約 1.3 時間): Q22 を仮で案 A に。流れの段の前に系に入れるブロックを枠の順に予算の中まで選ぶ形(CPU の MarkImplicitBlocks・GPU は
  系を作る段の CountBlocks・ScanBlocks を前へ動かして印を付ける)と、多重格子の段が入らなければ縮約を止める形を入れた。最初のビルドで release の HW・WARP とも
  上限を小さくした 2 つの形(ブロック・段)で CPU と毎刻みビット一致・保存量も一致。T-0179 は時間の約束で手を付けていない。debug・tidy は流していない。
- 同じ日(続き・約 0.3 時間): 関係する試験を広げたら gpu_multires_implicit_build(_warp)が落ちた(試験の隣の上限が CPU の段の和ちょうどで、新しい見込みでは
  GPU だけ縮約を止める)。試験の上限に段 0 の隣を足して直した。main は動いていなかったので rebase は何もしない。時間の約束で gpu_multires(_warp)・
  gpu_multires_implicit(_tree)(_warp)は流していない(マージ前に流すこと)。

## 引き継ぎメモ(HANDOFF に載せるもの。司令塔がマージ後に反映)
- 状態: T-0178 完了(Q22 は仮で案 A)。陰解法の系が上限(GpuMultiresImplicitLimits = MultiresImplicitLimits)を超える刻みも CPU と毎刻みビット一致。
  入らないブロックはその刻み陽解法、多重格子の段が入らなければ縮約を止める。CPU と比べる時は options.implicitLimits に同じ上限を渡す。
- 動いているもの: `-Filter "^gpu_multires_implicit_conduction(_warp)?$"`(release の HW 約 12 分〔計測込み・負荷あり〕・WARP 約 9 分。上限の 3 つの形 × 3 場面 × 全部・活性)。
- 壊れているもの: なし。debug・tidy は未実行。
- 決めたこと(Claude・実装の細部): ADR-0019 追記(T-0178)。
- 判断待ち: Q22 は未回答のまま(仮で A)。
- 注意: GPU の EnableImplicitConduction は上限の全部の欄が要る(0 は失敗)。バッファは予算(未知数 ≤ 隣 / 12・セル ≤ 節)の大きさで取る。
  伝導の作業場の印の語 7 を CONDUCT_MARK_IMPLICIT に使った(空きは無くなった)。

## 判断待ち
- **Q22(未回答)を仮で案 A にした**(取り消しやすい: implicitOverflow を変えるだけ。B・C を選ぶなら実装が要る)。案と遊びへの影響は T-0132 の「判断待ち」と同じ:
  - 案 A(仮で選んだ・おすすめ): 入りきらないブロックはその刻みだけ陽解法(頭打ち)。熱はいつも流れ、保存も守る。入りきらない所だけ、その刻みは細かい所の熱が
    本当より遅く伝わる(たくさんの細かい所が一度に熱くなった時だけ。D-428 の「嘘」は小さく一時的)。
  - 案 B: その刻み凍らせる。その所の熱がその刻み止まる(見えると不自然)。
  - 案 C: 細かくする要求を止める。熱は正しいが、近くで見ても細かくならない所が出る。
  - 補足: どの枠から陽解法に回すかは「枠の順で後ろから」(プレイヤーの近くを先にする形ではない)。近い所を優先するなら枠の順を変える別チケットが要る。

  - 補足(Claude が決める細部だが、既定の大きさを決める時に要る): 多重格子の段が上限に入らないと反復が増え、V の上限(64)に当たると安全網が陽解法に戻す
    (= D-436 の「解き切る」が崩れる)。上限の既定は「段 0 の見込みの 2 倍」以上を勧める(ゲームの VRAM の予算を決める時。未計測)。

## 分けたもの(新しいチケット。ROADMAP には司令塔が足す)
- なし(T-0179 はそのまま残る)
