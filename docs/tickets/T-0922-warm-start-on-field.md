# T-0922 C-1 の使い手の付け替え: C-0 の温めた始まりを C-1 の印の欄へ・セーブの大きさと費用を測る

- Status: Done(2026-10-22・wt2・1 チャット目)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列 1(C-1。T-0854 から分けた)
- 設計: docs/design/20-transport-substrate.md §10.4・§13.2 の 3・§13.3 / 決定: D-608(D-549)/ ADR-0399・ADR-0427

## 目的
C-0 の部品(陰の圧力・成分を持つ節)の温めた始まり(前の刻みの答え)を、C-1 の温めた始まりの印 `SF_FLAG_WARM_START` の欄に置く。
保存点から続けてもハッシュ一致(D-608 (A))。解き始めを入れた時のセーブの大きさと費用を測り、ユーザーに見せる(重ければ (B) に切り替えてよい)。

## 完了条件
- [x] 部品の口: `EquilibriumOptions::warmStart`・`EquilibriumBlockOptions::warmStart`(使い手は `GasProjectionOptions::warmStart`・`SupportOptions::warmStart`)。
      欄は `RegisterWarmStartField` = `SfMakeDerivedDesc(SF_PLACE_CELL, SF_TYPE_I64, phase, true, k)`。読み書きは `LoadNestField`・`StoreNestField`(sim/equilibrium_fields)
- [x] 気体の陰の圧力と支えで、欄に置いても答えは控えだけと毎刻みビット一致・保存点から続けて HashGas / SupportLaw::Hash と HashWholeNest が毎刻み一致(warm_start_save_test)
- [x] セーブの大きさ(B/セル)・保存点を取る / 戻す / 要約の時間・欄を持たない(= (B))時との差(docs/perf.md 2026-10-22)
- [x] release のビルド(警告 0)・equilibrium・support_law・gas_projection・field_storage・gpu_gas_projection_t0915・gpu_equilibrium_block が今のまま通る(下の作業ログ)

## 結果(2026-10-22・wt2・release・CPU)
### 保存点から続けてハッシュ一致(warm_start_save_test)
| 場面 | 刻み・保存点 | (A) 温めた始まりも保存点に | (B) 真実だけ(欄を 0 にして続ける) |
|---|---|---|---|
| 気体 16³ 泡・扉・爆発・孔のある壁・細孔の板(V 2) | 40・15 | 毎刻み一致 | 保存点の直後の刻みからずれ、40 刻みでもずれたまま |
| 気体 32³ 爆発 | 20・8 | 毎刻み一致 | 同上 |
| 支え 片持ち梁 16 × 4 m(1 つの系 k = 3) | 60・20 | 毎刻み一致 | 同上 |
| 支え 洞窟 0.5 m(1 つの系) | 40・12 | 毎刻み一致 | 同上 |
| 支え 片持ち梁(遅らせる形・成分ごと) | 60・20 | 毎刻み一致 | 同上 |
- (A) では法則の要約(箱・変位)も木の状態の要約(HashWholeNest。温めた始まりが入る)も毎刻み一致。(B) は「読み込んだ直後から答えがずれる」が本当に起きることの確かめ(D-608 の説明どおり)。

### セーブの大きさと費用(64³ = 26 万セルを全部起こし、全部のセルに値がある最も重い場合)
| | 欄 B/セル | 保存点を取る ns/セル(無し → 有り)| 戻す | 状態の要約 |
|---|---|---|---|---|
| k = 1(陰の圧力) | **8.02** | 1.27 → 3.65(**+2.4**) | 0.77 → 1.67(+0.9) | 25.3 → 30.5(+5.3) |
| k = 3(支え) | **24.05** | 1.24 → 8.57(**+7.3**) | 0.85 → 3.37(+2.5) | 25.2 → 41.0(+15.8) |
- 起きた所 100 万セル(T-0918 の VRAM の上限)なら: k = 1 で欄 8 MB・保存点 +2.4 ms・要約 +5.3 ms。眠った所はブロックに 1 値(8 B × k)。
- 比べ: M-0 の真実だけの 1 セル 36〜48 B(T-0931 の後の今の CPU のセルの状態は 33〜41 B)。**陰の圧力の解き始めは真実の約 1/5〜1/4 を足す**。支え(k = 3)は固い物のセルだけに乗る。
- ディスクのセーブ(圧縮・差分)はまだ無いので、ディスクの量は未確認(セーブ I/O の段で測る)。
- 見立て(仮・ユーザー未確認): **重くない → (A) のまま**(`SF_WARM_START_IN_STATE = true`)。切る時は 1 行。

## 作ったもの
- engine/src/sim/equilibrium_fields.{h,cpp}: `RegisterWarmStartField`・`StoreWarmStart`・`LoadWarmStart`・`WarmStartFieldBytes`(節 → `LookupBlock` → `MrCellIndex`)。
- equilibrium・equilibrium_block: `warmStart` が null でなければ解く前に欄から読み、解いた後に書く。null(既定)は前のまま。
- gas_projection・support_law: 設定から部品へ渡す(支えの遅らせる形は成分 c を欄の値 component + c に。結合が読む今の値も欄から)。
- tests/warm_start_save_test.cpp(ctest `warm_start_save`)。ADR-0427。map.yaml にノード 1 つ。

## 仮(ユーザー未確認)
- (A) のまま(上の見立て)。
- 閉じた領域の種の控え(`EquilibriumState` の `sourceRhs`・`diagonals`・`residuals`)は欄に入れない。閉じない今の使い手(陰の圧力の既定・支え)は答えに効かない。
  閉じる法則で保存点から続けると最初の刻みは全部が種になる → T-0993。
- 欄を持つ木は試験の中の「節 = 木のセル」を置くだけの木(本物の世界の陰の圧力・支えはまだ木の外の箱)。本物の木の上の使い手は T-0981・案 B の R1 で同じ口を使う。

## 分けたもの
- **T-0993**(工学・小): 閉じた領域(closeLengthMm・residualSeed)を使う法則で、種の控えを保存点に入れるか(入れないと保存点の直後の刻みは全部が種 = 答えが変わりうる)。
  閉じた領域の使い手が来た時に、保存点から続けてハッシュ一致を確かめ、要れば `sourceRhs`・`residuals` を導かれる値の欄(温めた始まりの印)にする。
- **T-0994**(工学): GPU の部品(GpuEquilibrium・GpuEquilibriumBlock)の前の答えを、欄が GPU に来る段(T-0920)で同じ欄から読み書きする。CPU とビット一致・保存点から続けて一致。

## 作業ログ
- 2026-10-22(wt2・1 チャット目): t-0970-block-gpu から t-0922-warmstart。部品に `warmStart` の口を足し、気体と支えで保存点から続けて毎刻み一致・(B) でずれることを確かめ、
  セーブの大きさと費用を測った(k = 1 で 8 B/セル・保存点 +2.4 ns/セル)。T-0955(V の回数を世界のデータで)も同じブランチで済ませた。
