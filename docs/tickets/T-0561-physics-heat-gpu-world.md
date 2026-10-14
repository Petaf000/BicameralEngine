# T-0561 物理の散逸の熱を GPU の世界のセルへ足す・窓で確かめる

- Status: Done
- 種類: 工学
- PC: 必須(ビルドと GPU の試験・窓)
- 見積もり: チャット 1〜2 回分
- マイルストーン: ③(docs/plan/ROADMAP.md の「今の順」。坂の原理 19 の段 1 の残り。T-0542 から分けた)
- 設計: docs/design/19-slope-principle-research.md §11 / 08 §4 / 決定: D-465・D-466・ADR-0232・ADR-0242

## 目的(1〜2 行)
T-0542 で CPU の世界(ProbeReference::Advance の heat)まで通した物理の熱を、GPU の仮の世界(ProbeSim の刻み)でも同じ刻み t + 1 にセルへ足し、
CPU とビット一致させる。重い物を落とすと床のセルが温まるのを窓で見る。

## 完了条件(チェックできる形で)
- [x] GPU の世界: 物理の刻み t の熱の入口(GpuPhysics の u14)を刻み t + 1 の適用の単位(コマンドの後)でセルへ足し、熱のキャッシュとブロックの印を作り直す
- [x] gpu_probe_physics 系: 刻みごとのセル(ハッシュ・エネルギー・つつき・物理の熱)が ProbeReference(heat あり)とビット一致・
      GPU の世界のエネルギーの増え = つつき + 物理の熱(毎刻み)・GPU の物理の帳簿(Σ 散逸 = 返した熱 + 残り・返した熱 = セルの熱 + 足していない分)
- [x] 世界のハッシュが変わる試験の理由と期待値
- [x] 窓(--editor --play)で重い物を落とした床のセルが温まるか(画像)
- [x] release のビルド(警告 0)・archmap --check

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
- 2026-10-15(wt2): 適用の単位に AddPhysicsHeat・FinishPhysicsHeat、ルート署名に u14、ハッシュの表の [14,15]、ProbeTickHash::physicsHeatEnergy。
  gpu_probe_physics_test にセルの比べ(ProbeReference + 熱)・エネルギーの釣り合い・GPU の帳簿を足した。gpu_world_copy_test の CPU リファレンスに
  PhysicsWorld(押す・熱)を足した(写しのセルのハッシュが CPU と比べるため。熱を足すと物理ありの世界のセルが変わる)。

## 結果(2026-10-15・release・RTX 3070 Ti)
- gpu_probe_physics・_warp・_compute・_gravity_swap: セル(ハッシュ・エネルギー・つつき・物理の熱)が毎刻み CPU とビット一致・
  エネルギーの増え = つつき + 物理の熱・帳簿が合い CPU と同じ。セルへ返した熱 157.8 kJ(240 刻み。2 回押して崩す)・残り 0.015 J。
- 世界のハッシュが変わった試験: gpu_world_copy(写しのセルのハッシュを CPU リファレンスと比べる。CPU に熱が無かった → 足して通った)。
  期待値をハードコードした試験は無かった。gpu_probe_rewind・gpu_play_metric・gpu_probe_blank・gpu_probe_place は変えずに通った
  (2 回の実行・巻き戻しの比べは同じ GPU の道どうし)。
- 窓: `--world sample --auto-push --screenshot-tick 300 --view mip` で、押した方だけ積み木の落ちた床のセルが赤く(300 K より上)写る
  (out/build/release/bin/t0561_push_view.png と t0561_still_view.png。何度上がったかは未測。色は対数で 300〜2000 K)。
  `--editor --play` でも同じ流れが動き、S(300) のエネルギーの和が押さない世界より 192.6 kJ 多い(エディタの窓は枠が床を隠すので画像は `--editor` なし)。
- 足す段の重さ: 窓の「コマンドの適用」0.009 → 0.013 ms/刻み(docs/perf.md)。

## 判断待ち(仮で進めたもの。ユーザー未確認)
- ADR-0242 の 2 つ(返す元を世界で 1 つ・両側の分を接触点のセルへ)は T-0542 のまま。
- 物質の無いセル(熱容量 0)に入った熱は温度 0 のまま動かない(帳簿は合う)。床が物理の物だけでセルが空の世界(ブランクの世界に物を置いた時)では
  熱が見えない。作り手から見た違い: 「物理だけ置いた世界で物を落とすと、熱は帳簿には載るが温度にも伝導にも出ない」。おすすめ(仮)は今のまま
  (物が温度を持つ時〔08 §1〕に、物の側へ返す分で解く)。
