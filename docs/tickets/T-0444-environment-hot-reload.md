# T-0444 環境の値のホットリロード(表の差し替えの刻みで物理の重力・気体の設定を差し替える)

- Status: Doing
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: D-471 ② 中身の器(docs/plan/ROADMAP.md。T-0400 から分けた)
- 設計: docs/design/13-scripting-game-model.md §2.6・14 §2 / 決定: ADR-0176・ADR-0047(反応表のホットリロード)・ADR-0194(この仕事)
- 関係: T-0453(wt2。ミキサーで足した重力・基準の大気を今の世界に効かせる)・T-0398(共有の決め事: 分類 environment は反応表と同じ読み込み・同じ表の版)

## 目的(1〜2 行)
分類 environment(重力・音速の基準・基準の大気)を書き換えた表がホットリロードで当たった時、表の差し替えと同じ刻みの境目から
物理の重力・気体の設定(重力・音速の基準・層の基準)も新しい値で刻む。今は世界を作り直すまで変わらない。

## 完了条件(チェックできる形で)
- [x] 物理: 表の差し替えの刻みから重力が替わる(ProbeSim の差し替えの道。CPU の PhysicsWorld とビット一致・2 回の実行で一致)
- [x] 気体: 刻みの境目で環境(重力・音速の基準・層の基準)を差し替えられる(GpuGas と StepGas がビット一致・保存則)
- [x] 範囲の外・器が受けられない値(音速の基準が小刻みの安定の上限を超える・気体の箱で音速の基準を 0 にする 等)は検査で理由を返して当てない
- [x] 窓の --check-physics(CPU の物理の突き合わせ)も差し替えの刻みで重力を替える
- [x] 再生で同じになる(差し替えは表の版の印で記録に残り、同じ道を通る)

## wt2(T-0453)との分担
- wt3(この仕事): 環境の値を今の世界に当てる道。表の差し替え(ProbeTableSwap)の刻みで物理の重力を差し替える・気体の箱の環境を
  刻みの境目で差し替える部品(CPU と GPU)・ホットリロードの検査(器が受けられる値か)。
- wt2(T-0453): ミキサー側の配線と、大気をセルに入れるか(世界を作り直す)などの振る舞い。ミキサーの差し替えは
  ReactionTableHotReload::Replace → 同じ差し替えの道を通るので、重力はこの仕事で今の世界に効く。

## 判断待ち(ユーザー。仮で進めたもの。ADR-0194 の 1)
- **環境の値(重力など)を差し替えた時、今の世界の状態をどうするか**(作り手から見た違い):
  - 案 A(仮で実装): 状態はそのまま、差し替えの刻みから新しい法則で刻む。飛んでいる物はその刻みから新しい重力で落ち、
    空気(気体を世界に載せた後)は前の重力で釣り合った層のまま、新しい重力で流れて落ち着く。保存量に触れないので帳簿に何も載らない。
  - 案 B: 差し替えの時に大気を新しい重力の釣り合いに並べ直す(空気が瞬間的に動く。保存量を書き換えるので改造の出入りとして帳簿へ)。
  - おすすめ: A(法則だけが変わる・取り消しやすい)。並べ直したい時は「世界を作り直す」(T-0453)を使う。

## 分けたもの
- T-0470(候補): 気体の多重解像度の木(gas_tree・GpuGasTree)にも環境の差し替え(SetGasEnvironment と同じ形)。気体を世界に載せる段(T-0210)の前に。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-13(wt3・第 3 巡): 物理は重力をルート定数にし(GpuPhysics::SetGravity・PhysicsWorld::SetGravity・保存点のカーソル)、
  ProbeSim の表の差し替え(RecordTableSwap → ApplyEnvironment)で表の環境の重力が変わったら同じ刻みの物理の単位から新しい重力。窓の
  --check-physics も同じ刻みで CPU の重力を替える(NotePhysicsCheckGravity)。気体は世界にまだ無いので部品(CheckGasEnvironment・
  SetGasEnvironment・GpuGas::RecordEnvironment)。CheckHotReloadCompatible が sim::CheckWorldPhysics を通す。--auto-reload-environment(窓の確かめ)。
  release(警告 0)で environment_package・gas_reference・reaction_species_remap・reaction_hot_reload・standard_packages・luau_type_check・
  physics_gravity・gpu_physics_stack・gpu_physics_pile_gravity_change・gpu_probe_physics・gpu_probe_physics_gravity_swap・
  window_gravity_reload_record/play(刻み 38 から月の重力・--check-physics の食い違い 0・再生 70/70 一致)が通過。archmap --check も通過。
  続けて gpu_gas・gpu_gas_flow(closed_environment_change 60 刻み・open_environment_change 180 刻みが毎刻みビット一致)・gpu_gas_tree・
  gpu_probe_rewind・window_hot_reload_game_*・gpu_physics_pile・gpu_physics_stack_warp・gpu_physics_stack_alternative(色ごとの解の
  Work Graph もルート定数 16 語で通る)・gpu_physics_stack_zero_gravity・gpu_probe_physics_compute も通過(release・HW)。
  Status は Doing のまま(ADR-0194 の 1 がユーザーの判断待ち)。
