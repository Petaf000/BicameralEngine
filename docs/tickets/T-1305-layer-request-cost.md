# T-1305 層のある置かれ方の要求の段の費用 4〜9 倍の原因と直し(T-1240 から)

- Status: Done(コードと HW の測り。マージは t-1240-place-layers の後)
- 種類: 工学
- PC: 必須(GPU)
- 見積もり: チャット 1 回分
- マイルストーン: 土台の変更要求の束(ADR-0469 の 2-2 の続き。T-1240 と同じ)
- 設計: docs/design/20 §10.5 / 決定: D-468(負荷で遅れることは不具合)・D-428 / 前: T-1240 の 7 の 4 後半

## 目的(1〜2 行)
T-1240 の 7 の 4 後半で、層のある置かれ方の世界の要求の処理(RecordProcessRequests)が層なしの 4〜9 倍(1.2 → 11 ms)だった。
どの段が重いかを測って特定し、結果をビット単位で変えずに直す。

## 完了条件(チェックできる形で)
- [x] 要求を処理する段の段ごとの時間を出す(gpu_tree_refine_placement が段ごとの 8 回の最小をログに出す)
- [x] 重い段の特定と直し。GPU の結果は直す前と全部の場面で同じ要約(CPU とビット一致のまま)
- [x] docs/perf.md に前と後
- [x] dxil_alloca_check・release 警告 0

## メモ・参考
- 測り方: GpuMultires の段ごとのタイムスタンプ(SetStageTimestamps)を要求の処理にも使えるように、隣の写し・Work Graph の細かくする / 粗くするの
  後にも打つ(前は Work Graph と解放が 1 つの段)。RecordProcessRequests は 0 番を打たないので、試験は ResetStageTimestamps(新)で番号を戻し、
  最初の段は試験のタイムスタンプから数える。gpu_multires_test の粗くする場面の段の並びも 1 つ増えた(ログだけ)。
- 特定(直す前、RTX 3070 Ti・release・compute キュー・8 回の最小): 重いのは **Work Graph の細かくする段(置かれ方の段 PlaceRefineChildren)だけ**。
  層・1 段 60 %: 9.64 ms(全体 9.91)・層・2 段の鎖 100 %: 11.03 / 11.28・層・舞う粒 2 段: 12.23 / 12.44。同じ回の層なしは 0.43〜0.95 ms。
  隣の写し(PlaceFullnessAt を読む)も層ありで 0.43 ms(層なし 0.04〜0.06)。ほかの段(解決・確定・割り当て・粗くする・解放・索引)は 0.1 ms 未満で層に依らない。
- 原因: sim/multires_refine_place.hlsli の物質の口(PR_MATERIAL・PR_PARENT_MASS・PR_MATERIAL_MOLAR_VOLUME・PR_MATERIAL_COUNT)が、
  呼ばれるたびに成分を前から数え直し(PlacePartOf / PlaceMaterialCount)、その中で成分ごとに層の組 8 つを読む(PlaceLayerOf = 24 語)。
  置かれ方の回し(sim/placement_refine.hlsli)は物質の口を 1 セルあたり物質の数 × 十数回呼ぶので、層のあるセルは
  物質 M × 成分 S × 組 8 × 3 語の読みが十数回(層なしは物質 m = 成分 m の近道で数え直さない)。
- 直し: 物質の並びを PlaceUseCell(セルを読む用意。1 スレッドに 3 回)の時に 1 回だけ作り、32bit の語に詰めて持つ
  (成分ごとの層の組の番号 4 bit × 8・物質ごとの印 8 bit × 16〔成分の位置・分・舞う粒〕・物質の数)。層の組は 1 回だけ読む
  (同じ物質の 0 でない最後の組 = 前の PlaceLayerOf と同じ)。局所の配列は持たない(T-0397: 語を決まった選びで読む)。
  物質の口は O(1)。熱の和(PlaceChildEnergy の化学のエネルギー)は物質の数ではなく成分の数で回す(CPU の ChildEnergies と同じ。
  前は層のあるセルで成分の並びの外まで回していた: 外の量は 0 なので値は同じだったが、物質が 9 つ以上なら構造体の配列の外を読む)。
- 直した後(RTX 3070 Ti・同じ測り。この回は GPU が全体に遅く、層なしの置かれ方 1 段が 1.14〜1.19 ms): 層・1 段 60 % 1.74 ms・層・2 段の鎖 100 % 3.46 ms・
  層・舞う粒 2 段 3.54 ms・層・端 1.84 ms。**同じ回の層なしの 1.5〜1.7 倍**(前は同じ回で 17〜41 倍・perf.md の前の行では 4〜9 倍)。
  残りの差は物質が成分の倍(層の分と残り)になる分で、回しの式どおり(数え直しではない)。隣の写しは 0.12 ms(層なし 0.05)。
- 結果: 16 場面すべてで GPU の要約が直す前と同じ(CPU とビット一致)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-11-05 wt2(作業役 6): 作った。段ごとの時間を出して、Work Graph の細かくする段(置かれ方)が全部と特定。物質の並びを 1 回だけ作る形に直して
  層のある場面が 9.9〜12.4 → 1.7〜3.5 ms(同じ回の層なしの 1.5〜1.7 倍)。ブランチ t-1305-layer-request-cost(t-1240-wide-layers の上)。
- 2026-11-05 wt2(作業役 6 の締め): 合格(release・RTX 3070 Ti・t-1240-wide-layers の上): gpu_tree_refine_placement〔HW・WARP〕・gpu_multires_layers〔HW・WARP〕・
  gpu_multires・gpu_lab_box・dxil_alloca_check・tree_refine_placement・placement_refine・multires_layers・multires。release 警告 0・archmap --check OK。
  残り: rebase main(f1a4e17a = t-1240-place-layers 入り)→ ビルド・上の試験をもう 1 回 → ready の印(まだ書いていない)。
