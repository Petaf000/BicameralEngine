# T-0670 区画のある世界の表の差し替えで区画の欄を作り直す(物質の付け替え・物質の値・環境の値)

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: ROADMAP の T-0210(T-0620 から分けた)・⑤ の場面で物質の値をいじって試す時
- 設計: docs/design/07-transport.md §2.7・§3 / 決定: ADR-0300(仮)の 3・ADR-0200(T-0470)

## 目的(1〜2 行)
区画のある世界で、物質の一覧・物質の値・環境の値・基準の大気が変わる表へのホットリロードを断らずに通す(今は ProbeSim::CheckRegionTableSwap が断る)。

## 完了条件(チェックできる形で)
- [ ] 物質の一覧が変わる表: 気体の帳簿(物質 ID ごと)と液体の物質ごとの値を付け替える。気体の作業場の並び(物質の数で決まる)を作り直す
      (CPU の ProbeGasField・ProbeLiquidField にも RemapSpecies を足し、ProbeReference::RemapSpecies から呼ぶ)
- [ ] 物質の値が変わる表: 作業場の物質ごとの値(ProbeGasSpecies・ProbeLiquidSpecies)を書き直す。気体の欄の数(表の気体の物質の数)が変われば作り直す
- [ ] 環境の値・基準の大気が変わる表: 気体の設定の型・層の基準(GpuGas::RecordEnvironment。木なら SetGasTreeEnvironment)・開いた境界の外・
      液体の重力を作り直す(T-0470 の引き継ぎ: ApplyEnvironment から。script::CheckHotReloadCompatible を木と区画の上限で見る)。CPU も同じ
- [ ] 差し替えの刻みから CPU と毎刻みビット一致(gpu_probe_gas・gpu_probe_liquid に差し替えの場面)。保存点で差し替えをまたいで戻せる(gpu_probe_rewind_regions)

## メモ・参考
- 保存点は区画の作業場を写すので、作業場の大きさが差し替えで変わる時は保存点のバッファも作り直す(戻す時の表と大きさを合わせる)。
- CPU の ProbeGasField は作る時に音速・重力・層の大気を持つ(m_soundSpeedMmPerS など)。今は差し替えても変わらない = GPU と同じ。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
