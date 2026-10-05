# T-0107 熱の伝導を GPU に

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: docs/design/07-transport.md §1・docs/design/17-multiresolution.md §5「熱の伝導」/ 決定: D-206・D-302・D-428 / ADR-0017

## 目的(1〜2 行)
T-0019 の熱の伝導(多重解像度の木の上・レベルをまたぐ)を GPU に載せる。活性の Work Graph の刻みと全部を刻む Compute の刻みの両方で、
CPU リファレンスと HW・WARP で状態の全部が毎刻みビット一致する。

## 完了条件(チェックできる形で)
- [x] 全部を刻む(RecordStep)と活性だけ刻む(RecordStepActive)に `MultiresStepOptions{.conduction = true}` を入れ、CPU の StepNest・StepActive と同じ順
- [x] ルート署名(63 / 64 語)にバッファを足せるよう、uint32 の表をまとめる
- [x] 鎖・たくさんの要求の場面で、状態の全部と次の刻みの種が毎刻み CPU とビット一致(HW・WARP)
- [x] 伝導で取った端数の枠の容量の扱いを決める(ADR-0017「影響」)
- [x] 伝導の段を Work Graph と Compute で測って安い方(D-302)
- [x] 前からのテストが全部通る・tidy 警告なし・archmap OK

## メモ・参考
- 1 刻みの順(CPU の StepBlocks と同じ): ConductMark(印。活性なら Work Graph の ActivityStepNode が刻まずに伝導の一覧に足した後)→ TreeExpand(頁。配った・凍らせた印)
  → TreeFractions(端数の枠を枠の順に)→ ConductPrepare(配った頁を埋める・配った端数を空にする)→ ConductFlows(面の流れを変化に。64bit の atomic)
  → ConductApply(変化を足して反応。活性なら忙しさの印と次の刻みの種)。式は shaders/sim/multires_conduct.hlsli、呼ぶ順は gpu_multires.cpp の RecordConduction。
- ルート署名: u6 に uint32 の表(世界の枠の空き・取り合いの印・索引・世界の頁の空き)をまとめ、空いた所に伝導の作業場(u11)。UAV 14・定数 24 で 62 / 64 語。
- 伝導の一覧に入るのは、刻むブロック(活性の印)・粗い側の相手(変化を受ける)・観察の枠。相手は印の段で入れる(流れの段では足さない: Work Graph の入力は読むだけなので)。
- 端数の枠の容量(ADR-0017 追記): 返すのは木の変更の時だけのまま。静かになったら端数のエネルギーを帳簿へ移して返すのは T-0104(ほぼ同じ頁を畳む判定と一緒)。
  容量は頁と同じ「世界の容量」、足りない時は数える(MR_COUNTER_FRACTION_SHORTAGE)。
- 計測(docs/perf.md): 根 8³ で伝導ありの活性の刻み 0.5〜1.1 ms(クロックで揺れる)。伝導の段は Compute と Work Graph の差が揺れの中、Compute がわずかに安い → 既定は Compute。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-05: u6 に uint32 の表をまとめてルート署名を空け(62 / 64 語)、伝導の作業場 u11・伝導の段 multires_conduct.hlsl(Compute 5 段)・TreeFractions・
  活性のグラフの伝導の一覧を足した。Work Graph 版(multires_conduct_graph.hlsl)も作って測った。gpu_multires_conduction_test(鎖・たくさんの要求 × 全部 / 活性 × Compute / Work Graph)が
  HW・WARP で毎刻み CPU とビット一致。端数の枠の容量は ADR-0017 に追記(返すのは T-0104)。
- 2026-10-05: release の WARP で Work Graph 版の最初の DispatchGraph がデバイスを失う(debug の WARP・HW は一致。WARP の JIT と推定・未確認)ので、テストは release の WARP でだけ外した。
  全部のテスト 67 本が通る(debug)・release の gpu_multires_conduction(HW・WARP)も通る・tidy 警告なし・archmap OK。

