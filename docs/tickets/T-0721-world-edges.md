# T-0721 世界の端(open / wall・外のセル・帳簿の「世界の端」。MR の CPU。最初の種類は陽の伝導)

- Status: Done(CPU の陽の伝導。外の柱を基準の大気から作る・環境とのつなぎは T-0772、GPU と陰解法は T-0773)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(2 時間)
- マイルストーン: 運ぶ計算の共通の土台 段 3 ③(ROADMAP ③' の行。20 §1.1・D-514〔世界の端だけ・風は入れない〕)
- 設計: docs/design/20-transport-substrate.md §1・§2.4・§2.6 / 決定: ADR-0337(Proposed)・ADR-0331(帳簿)・T-0762(帳簿の書き方の約束)

## 目的
区画の箱をやめたので、外との境は世界の端だけ。端の面を「端の面」として面の列挙に入れ、open / wall で振る舞いを決める一般の形を作る。

## 完了条件
- [x] shaders/common/world_edge.hlsli: 世界の箱(根のレベルのセル)と端の種類(上・下・横 = WE_WALL / WE_OPEN)の WeWorld・
      面がどの端の外か(WeFaceEdge)・開いた端か(WeIsOpenFace。本物のブロックだけ)・外のセルの高さ(WeLevelZeroZ・WeOutsideZ・WeColumnRow)・
      面の隣の種類 MR_NEIGHBOR_EDGE と WeEdgeNeighbor(世界の外の面の隣)
- [x] MultiresNest::edges(MultiresWorldEdges: WeWorld + 外の柱〔レベル 0 の z ごとの外のセル〕)。世界のデータで状態に入らない。既定は全部 wall
- [x] 当てる段(transport_apply.hlsli)と陽の伝導の世界(multires_conduction.cpp の ConductionWorld): 開いた端の面は外のセルを同じレベルの相手として
      同じ式で流し、自分だけを変え、流れた分を帳簿の「世界の端」のエネルギーの列へ(WlEnergyEntry で出 / 入。AddWorldLedgerUnits(レベル))。
      一様なブロックも端の流れがあれば頁に広げる(MarkCellWants)
- [x] 試験 multires_edge_test(^multires_ に入る): 全部 wall はビット一致で前と同じ / 上だけ open で熱が逃げ、保存の検査(ComputeConservedTotals)が
      帳簿込みで毎刻み一定・帳簿は「世界の端・出」にだけ載る / 外の柱の高さの読み方 / 上の端に接して 2 段細かくした場面で保存量が一定・活性 = 全部 /
      外が真空(柱が空)なら伝わらない
- [x] release のビルド警告 0・^multires_・archmap --check

## 作り(要点)
- **一つの芯**: 開いた端の面 = 「同じレベルの面で、相手の見立てが外のセルから来る・相手は変わらない・流れは帳簿へ」。流れの種類は端の場合分けを持たず、
  外のセルを読むだけ。熱は外の温度へ、気体は外の大気と出入り、液体は外のセルに液体が無いので出ていくだけ — が各種類の同じ式から出る。
- 外のセルは**同じレベル**(端に接するセルと同じ大きさ)。レベルの単位は 8^-k なので、同じ密度・温度の外のセルはどのレベルでも同じ数 →
  外の柱はレベル 0 の 1 列(z ごと)で足りる。レベル k のセルの z は中心を含むレベル 0 の z(WeLevelZeroZ)で柱の行を選ぶ。上下の外は端の行のまま。
- 外が真空(柱が空 = 基準の大気の無い世界)の open は熱を伝えない(真空のセルは伝導率 0)。気体は出ていくだけになる見込み(T-0720 で確かめる)。
- 縦は z(MR の気体の木 gas_tree と同じ。上 = +z)。
- 帳簿: 流れ F(出ていくのが正。そのレベルの単位)を WlEnergyEntry(F, 出) で 出 / 入 に分け、AddWorldLedgerUnits(nest, WlSlot(WL_REASON_WORLD_EDGE, 向き), レベル,
  WL_COLUMN_ENERGY, 大きさ)。小刻み(4^σ)ごとに足す。物質・欄の列も同じ形(列 = 1 + 物質 ID・NestFieldLedgerBase + …。T-0762 の約束)。
- 影のブロック(観察の枠)の外は今まで通り断熱(影は自分の中だけ。世界の端に接する影も端を流さない)。

## 当てる段(T-0714。transport_apply.hlsli の TxFlowCell)への載せ方
T-0714 が main に入ったので、端は当てる段の上に載せた(種類は端の場合分けを持たない):
1. **World::Neighbor**: MrFindFaceNeighbor が MR_NEIGHBOR_NONE なら WeEdgeNeighbor(world_edge.hlsli)が、開いた端で外の柱があれば
   `MR_NEIGHBOR_EDGE`(index = 外の柱の行)、wall・影の外・木の穴・柱が空なら NONE を返す。
2. **World::Outside(行, レベル)**: 外の柱の行のセルを自分と同じレベルで見立てたもの(CellViews::Outside = MakeCellView)。
3. **TxFaceFlow**: EDGE は同じレベルの面と同じ式で、相手の見立てを Outside から。**TxLimitFaceFlow**: EDGE の相手の倍率は TxNoLimit(外は無限の溜め)。
4. **TxFlowCell**: EDGE の面は `own.whole -= flow` と **World::AddEdge(slot, index, flow)**(5 帳簿。相手には足さない)。印の回(MarkCellWants)も EDGE を
   「自分が変わる」に数える(一様なブロックを頁に広げる)。
- 気体(T-0720)・液体(T-0717)は同じ口: 種類は外の見立てを読むだけ。包み(成分ごとの量 + エネルギー + 運ぶ欄)は AddEdge を包みの列ごとに広げて帳簿へ
  (物質 = 1 + 物質 ID・欄 = NestFieldLedgerBase + …)。運動量の欄の出入りも同じ「世界の端」の理由(壁の力積 WL_REASON_WALL_IMPULSE は wall の面が受けた力積で別)。
- 真空(柱が空)は今は NONE(流さない)。気体を載せる時は「空の外のセル」と流す(出ていくだけ)に変える要がある(T-0720。空のセルの見立てが割り算で困らないか確かめる)。

## 判断待ち(仮〔ユーザー未確認〕。取り消しやすい)
1. **基準の大気の無い世界(ブランク)で open = 真空**にした。作り手から見た違い: 大気を書かずに上を open にすると、熱は逃げない(真空は伝えない。
   放射は保留 D-501)が、気体は外へ出ていく(段 3 ② 以降)。代案: 大気が無いのに open ならベイクの「注意」を出す(T-0710 の「全部 wall で大気が無い」の逆)。
   おすすめ: 真空のまま + 注意を足す(T-0772 で)。
2. **外のセルは端に接するセルと同じレベル**(細かくした所の端は細かい外のセルと流れる)。作り手から見た違いは無い見込み(同じ密度・温度なら数が同じで、
   面の係数もレベルの規則どおり)。PS(1 段の MR)の気体の開いた境界とビット一致するかは T-0720 で確かめる。

## 分けたもの
- **T-0772**(工学・CPU): 外の柱を基準の大気(WorldAtmosphere・D-494 の静水圧)から作る(PS の気体の開いた境界の外と同じ値・同じ関数にする)・
  環境の world_edges(WorldPhysics::edges)と世界の箱を nest.edges へつなぐ・表の差し替えで外が変わったら端に接するブロックを種に・
  判断待ち 1 の注意。
- **T-0773**(工学・GPU): 世界の端の GPU(multires_conduct.hlsli の伝導の段が world_edge.hlsli を呼ぶ・帳簿は AddWorldLedgerCarrying)と
  陰解法(AddImplicitConduction の系の端の面。今は陰解法で解くブロックの端は wall と同じ)。CPU とのビット一致の試験。

## 作業ログ
- 2026-10-17(wt3): world_edge.hlsli・MultiresNest::edges・伝導の端の面と帳簿・multires_edge_test を入れた。最初は前の main の CollectCellFaces に載せたが、
  rebase の時に T-0714(当てる段)が main に入っていたので、TxFlowCell の EDGE の分岐と World::Outside / AddEdge に載せ直した。
  World の約束が 2 つ増えたので、確かめるだけのシェーダー(transport_apply_check.hlsl)と transport_apply_test の FaceWorld にも足した。
  release 警告 0・^multires_ 10 本(multires_edge を含む)・transport_apply・lab_box・archmap --check。上だけ open の 60 刻みで上の層 344049 mK・
  下の層 399999 mK(外 250 K。載せ直す前と同じ値)。
