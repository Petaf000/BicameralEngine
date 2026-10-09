# T-0212 活性のグラフの刻みで溢れを使う(T-0176 から分けた)

- Status: Todo(T-0211 の後)
- 種類: 工学 + 測定
- 設計: 02 §3・17(R-MULTI-4)・ADR-0052。関係: T-0176・T-0211・T-0124(HW の Work Graph のノードに反応の核を 3〜4 か所展開すると DEVICE_HUNG)

## やること
- 活性のグラフ(multires_activity_graph.hlsl)の刻むノードは、今の核(RxCell)を 2 か所展開している。上限なしの形の核をノードに足すと約束(1 ノード 2 か所まで)を破る。
- 案: 溢れるセルがあるブロックはグラフの中で刻まず一覧に足し、グラフの後に Compute(multires_step_wide_*)で刻む / 溢れを使う世界は活性も Compute にする。
  どちらも HW・WARP・CPU が毎刻みビット一致することを確かめ、費用を測る(D-302)。

## 完了条件
- 溢れを使う世界の RecordStepActive が CPU の StepActive と HW・WARP で毎刻みビット一致。HW の Work Graph が止まらない

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
