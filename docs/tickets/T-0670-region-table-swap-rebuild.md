# T-0670 区画のある世界の表の差し替えで区画の欄を作り直す(物質の付け替え・物質の値・環境の値)

- Status: Done(2026-10-19 main へ。t-0724 の上に積んだ。ADR-0300 の T-0670 の追記)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: ROADMAP の T-0210(T-0620 から分けた)・⑤ の場面で物質の値をいじって試す時
- 設計: docs/design/07-transport.md §2.7・§3 / 決定: ADR-0300(仮)の 3・ADR-0200(T-0470)

## 目的(1〜2 行)
区画のある世界で、物質の一覧・物質の値・環境の値・基準の大気が変わる表へのホットリロードを断らずに通す(今は ProbeSim::CheckRegionTableSwap が断る)。

## 完了条件(チェックできる形で)
- [x] 物質の一覧が変わる表: 気体の帳簿(物質 ID ごと)と液体の物質ごとの値を付け替える。気体の作業場の並び(物質の数で決まる)を作り直す
      (CPU の ProbeGasField・ProbeLiquidField にも RemapSpecies を足し、ProbeReference::RemapSpecies から呼ぶ)
- [x] 物質の値が変わる表: 作業場の物質ごとの値(ProbeGasSpecies・ProbeLiquidSpecies)を書き直す。気体の欄の数(表の気体の物質の数)が変われば作り直す
- [x] 環境の値・基準の大気が変わる表: 気体の設定の型・層の基準(GpuGas::RecordEnvironment。木なら SetGasTreeEnvironment)・開いた境界の外・
      液体の重力を作り直す(T-0470 の引き継ぎ: ApplyEnvironment から。script::CheckHotReloadCompatible を木と区画の上限で見る)。CPU も同じ
- [x] 差し替えの刻みから CPU と毎刻みビット一致(gpu_probe_gas・gpu_probe_liquid に差し替えの場面)。保存点で差し替えをまたいで戻せる(gpu_probe_rewind_regions)

## メモ・参考
- 保存点は区画の作業場を写すので、作業場の大きさが差し替えで変わる時は保存点のバッファも作り直す(戻す時の表と大きさを合わせる)。
- CPU の ProbeGasField は作る時に音速・重力・層の大気を持つ(m_soundSpeedMmPerS など)。今は差し替えても変わらない = GPU と同じ。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-19(wt2・作業役): t-0724(区画をやめた後、地球の環境の世界は必ず世界いっぱいの区画を持つ)の上で入れた。GpuGas::RecordEnvironment で
  部分だけ直すのではなく、区画の作業場を新しい表で丸ごと作り直す 1 本の道にした(BuildGasWork・RebuildLiquidWork)。持ち越す欄だけを
  古い作業場から CopyBufferRegion で写す(気体の帳簿は物質の名前で付け替え。消えた物質の量は捨てる)。差し替えのあるフレームの初めに作業場を
  UAV に揃える。保存点は写し先を今の大きさに作り直し、表の違う保存点へ戻す時は作業場をその表の大きさにしてから写す。
  CheckRegionTableSwap は ProbeGasField::Create・ProbeLiquidField::Create が断る表(基準の大気が無い・音速・重力が 0 など)だけ断る。
  試験: gpu_probe_gas(石と粘りの表へ・6 種の空気の表へ)・gpu_probe_liquid(月の重力・密度・粘土を足す表へ)・gpu_probe_rewind_regions
  (差し替える表を重力・密度・物質の一覧の違う表に)・window_hot_reload_*・window_gravity_reload_*・window_play_* が release で通る。

## 残り(後のチケットへ)
- 区画の無い世界(基準の大気・重力が無い表で作った世界)に、大気や重力のある表を当てても区画は生まれない(今まで通り。世界を作り直せば流れる)。
  多重解像度の世界へ移ると区画そのものが無くなる(D-510)ので、仮の世界では足さない。
- 差し替えは区画の作業場を丸ごと作り直す(64³ の液体の作業場は約 180 MB を一時に 2 つ持つ)。ホットリロードの時だけなので測っていない(未確認)。
- debug で gpu_probe_rewind_regions を流した(2026-10-19): ハッシュの一致は全部通り(差し替え・差し替えをまたいだ巻き戻し・刻み 0 の保存点・写し)、
  debug layer のエラーも 0。ただしシェーダーの assert(fixed.hlsli:320 = FxMakeRecipU32 の割る数が 0)が出て落ちる。差し替えも保存点も無い
  基準の回し(最初の 120 刻み)から出るので T-0670 の前からのもの(未確認: main の debug で同じか確かめる)。debug の gpu_probe_liquid は
  上限 300 秒に当たった(2 場面まで一致・assert 0)。
