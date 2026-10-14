# T-0561 物理の散逸の熱を GPU の世界のセルへ足す・窓で確かめる

- Status: Doing
- 種類: 工学
- PC: 必須(ビルドと GPU の試験・窓)
- 見積もり: チャット 1〜2 回分
- マイルストーン: ③(docs/plan/ROADMAP.md の「今の順」。坂の原理 19 の段 1 の残り。T-0542 から分けた)
- 設計: docs/design/19-slope-principle-research.md §11 / 08 §4 / 決定: D-465・D-466・ADR-0232・ADR-0242

## 目的(1〜2 行)
T-0542 で CPU の世界(ProbeReference::Advance の heat)まで通した物理の熱を、GPU の仮の世界(ProbeSim の刻み)でも同じ刻み t + 1 にセルへ足し、
CPU とビット一致させる。重い物を落とすと床のセルが温まるのを窓で見る。

## 完了条件(チェックできる形で)
- [ ] GPU の世界: 物理の刻み t の熱の入口(GpuPhysics の u14)を刻み t + 1 の適用の単位(コマンドの後)でセルへ足し、熱のキャッシュとブロックの印を作り直す
- [ ] gpu_probe_physics 系: 刻みごとのセル(ハッシュ・エネルギー・つつき・物理の熱)が ProbeReference(heat あり)とビット一致・
      GPU の世界のエネルギーの増え = つつき + 物理の熱(毎刻み)・GPU の物理の帳簿(Σ 散逸 = 返した熱 + 残り・返した熱 = セルの熱 + 足していない分)
- [ ] 世界のハッシュが変わる試験の理由と期待値
- [ ] 窓(--editor --play)で重い物を落とした床のセルが温まるか(画像)
- [ ] release のビルド(警告 0)・archmap --check

## 作り
- ハッシュの表の欄の [14,15](空きだった)に「その刻みの初めに物理の熱として足したエネルギー」(mJ)。ProbeTickHash::physicsHeatEnergy。
  保存則の検査は energy(t + 1) = energy(t) + つつき + 物理の熱。
- 仮の世界のルート署名に u14(物理の熱の入口。GpuPhysics::HeatEntries。物理なしなら重さの捨て場を仮に結ぶ)。UAV で結ぶ
  (物理のパスが UAV で書くバッファなので、SRV にすると状態の遷移が要る)。ルート署名は 15 + 3 UAV・6 SRV・3 定数 = 51 語(64 以内)。
- 適用の単位(ApplyCommands の後・RefreshTable / WakeDueBlocks の前。CPU の ApplyPokes → ApplyPhysicsHeat → 表の作り直しと同じ順)に 2 段:
  AddPhysicsHeat(入口ごと。セルの energy へ 64bit の原子的な足し算 + ハッシュの表の欄)→ FinishPhysicsHeat(入口ごと。足し終わったセルから
  熱のキャッシュ・ブロックの changed = 印 − 1・起こす刻み = 今の印。置くコマンドと同じで、WakeDueBlocks が一覧へ 1 回だけ足す)。
  入口の数(GpuPhysics::HeatEntryCount)は単位の定数 argument で渡す。

## 並走との重なり
- wt6 T-0210(気体を本物の世界へ)が ProbeSim の刻みの段・ルート署名(ROOT_LAYOUT)を触ると、ここで足した u14(UAV_PHYSICS_HEAT)と
  適用の単位の RecordPhysicsHeat に重なる。ROOT_LAYOUT の uavCount と番号、probe_bindings.hlsli の「u13 は物理の物」の注の所。
  気体の UAV を足すなら u15 から。

## 作業ログ
