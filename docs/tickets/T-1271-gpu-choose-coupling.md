# T-1271 刻みを選ぶ段の GPU 版に境の面の案 (c) の表(CPU とビット一致。既定は None のまま)

- Status: Doing(2026-10-29 wt6・t-1270-border-default-cost の上)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列 6(docs/plan/ROADMAP.md。司令塔が行を足す)
- 設計: docs/design/20-transport-substrate.md §14.2 / 決定: ADR-0493・ADR-0507(Proposed)

## 目的(1〜2 行)
Q181 の答えが (c) なら GPU の刻みを選ぶ段(gpu_time_tree_choose・ADR-0493)も同じ規則で選べるように、単位ごとの「隣の差 1 段を許す最も長い e」の表 1 本と
条件 1 つを足し、CPU(ChooseExponents の BorderAllowed)とビット一致を確かめる。既定(border = None)は今のまま。

## 完了条件(チェックできる形で)
- [ ] `TimeTreeChooseTables::looseExponents`(空 = 今の形)・`MakeTimeTreeChooseTables` は border = Coupling も範囲に
- [ ] time_tree_choose.hlsl の TtcSweepOnce: 隣の e + 1 が単位の loose を超えるなら隣の差 0 段(ルート定数 1 つ・SRV t6 1 本。表が無い時は読まない)
- [ ] 選ぶ段の写しのファイルに表(版 2。版 1 も読める)
- [ ] gpu_time_step_choose(今の形)と gpu_time_step_choose_coupling・gpu_time_step_choose_coupling_night(火事・池の c0)が CPU とビット一致
- [ ] release のビルド警告 0・archmap --check

## メモ・参考
- 規則が CPU と同じ不動点に落ちる理由: 単位の上限 = min(窓の上限, 隣ごとに 隣の e + (隣の e + 1 ≤ loose ? 1 : 0))。右辺は隣の e について単調に増える
  (e が 1 減ると値は 1 減るか同じ)ので、e は減るだけ = 掃く順によらず同じ最大の不動点(今の形の説明と同じ)。
- 表は法則の係数と木の形だけで決まる(状態を読まない。D-465)。1 単位 4 B。火事の fine(4,096 単位)で 16 KB。
- 64bit の算術は無い(e は 4 bit。比べは 32bit の符号なし)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
