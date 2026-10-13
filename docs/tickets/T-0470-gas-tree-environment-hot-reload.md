# T-0470 気体の多重解像度の木にも環境の値の差し替え(gas_tree・GpuGasTree)

- Status: Done(ADR-0200 の 1 はユーザーの判断待ち。仮で実装)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: D-471 ② 中身の器(docs/plan/ROADMAP.md。T-0444 から分けた)
- 設計: docs/design/07-transport.md §2.5 / 決定: ADR-0200(この仕事)・ADR-0194・ADR-0117

## 目的(1〜2 行)
表の差し替えの刻みで、気体の多重解像度の木(CPU の gas_tree・GPU の GpuGasTree)も重力・音速の基準・基準の大気を新しい値にする
(1 レベルの箱の SetGasEnvironment と同じ形)。木の小刻みの数・頭打ちの温度をどう作り直すかを決める。

## 完了条件(チェックできる形で)
- [x] CPU: CheckGasTreeEnvironment・SetGasTreeEnvironment(MakeGasTree と同じ道で作り直す。状態は保つ)
- [x] GPU: GpuGasTree::RecordEnvironment(t0〜t3 を写し直す)
- [x] 器の範囲の外は sim::CheckWorldPhysics と同じ理由の形で断る(木の上限は ν がいちばん大きいレベルから)
- [x] gas_tree(CPU)に差し替えの場面(保存則・新しく作った木と同じ・当て直し・検査)
- [x] gpu_gas_tree(HW・WARP)に差し替えの場面 3 つ(毎刻み CPU とビット一致・閉じた箱は保存則)
- [x] release のビルド(警告 0)・archmap --check

## 判断待ち(ユーザー。仮で進めたもの。ADR-0200 の 1)
- **音速の基準を、木の細かさが受けられる上限より上げるホットリロードをどうするか**(作り手から見た違い):
  - 案 A(仮で実装): 断る(古い表のまま + 理由)。小刻みの数は木の作り(世界を作る時の設定)で、値の書き換えでは変わらない。
    細かくした木ほど上限が低い(小刻み 48・分けない 2 レベルで 432 m/s)。上げたい時は小刻みの設定を替えて世界を作り直す。
  - 案 B: エンジンが小刻みを自動で増やして受ける。書き換えが通る代わりに、1 刻みの重さが黙って倍になり(D-468)、刻み方が値で変わる。
  - おすすめ: A(1 レベルの箱の扱い〔小刻みの数は替えない〕と同じ・重さが値の書き換えで変わらない・取り消しやすい)。
    世界に気体を載せる時(T-0210)に、小刻みの数を c̃ から決める作り(作る時だけ)にするかは別に決める。

## 分けたもの
- T-0479(この仕事の試験で見つけた不具合。同じブランチで直した): 分けた小刻みの木で一様な加圧が偽の流れを出す(粗い側の力の見込み。ADR-0201)。
- (T-0210 の時)ProbeSim の ApplyEnvironment から木の差し替えを呼ぶ・script::CheckHotReloadCompatible を木の上限で見る(ADR-0200 の影響)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-14(wt3・第 4 巡): MakeGasTree の環境から決まる値を BuildLevelEnvironments に分け、SetGasTreeEnvironment も同じ道
  (替えた木 = 新しい環境で作った木 + 今の状態、と毎刻み同じ)。小刻みの数は替えず、音速の基準の上限は木の形から(GasTreeSoundSpeedLimitMmPerS)、
  外は CheckGasTreeEnvironment が CheckWorldPhysics(gasFinestShift を足した)と同じ理由の形で断る。CheckGasEnvironment も CheckWorldPhysics を通す。
  GPU は GpuGasTree::RecordEnvironment が t0〜t3 を写し直す。release(警告 0)で gas_tree・gas_reference・environment_package・
  reaction_hot_reload・standard_packages・gpu_gas_flow・gpu_gas_tree・gpu_gas_tree_warp(12 場面・環境の差し替え 3)が通過。debug の gas_tree も通過。
  差し替えの試験で、分けた形の木だけ偽の流れ(約 21 m/s)が出たのを調べて T-0479 に分けて直した。archmap --check 通過。
