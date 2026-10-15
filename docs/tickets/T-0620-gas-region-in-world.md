# T-0620 気体の区画を世界の一部に(保存点・写しの早送り・ハッシュ・表の差し替え・区画を世界のデータに書く)

- Status: Todo
- 種類: 工学 + 設計
- PC: 必須
- 見積もり: チャット 2 回分
- マイルストーン: ROADMAP の T-0210(T-0592 から分けた)
- 設計: docs/design/07-transport.md §2.7 / 決定: ADR-0256・ADR-0270

## 目的(1〜2 行)
T-0592 で仮の世界の GPU の刻みに気体の区画が載った(陽解法・4 種まで)。区画のある世界で今は断っている 表の差し替え・保存点・写しの早送りを通し、
状態のハッシュに気体の欄を入れ、区画を世界のデータに書いてアプリ(エディタ・遊ぶ)で煙が流れるようにする。

## 完了条件(チェックできる形で)
- [ ] 保存点・写しの早送り(probe_save_point.cpp の StateRegions)に気体の欄(gasState)と帳簿(gasWork の帳簿の欄)。戻した後も CPU と毎刻み一致
- [ ] 状態のハッシュ(HashCells の表)に気体の欄 + 帳簿(CPU は ProbeReference::StateHash と ProbeGasField::Hash を合わせた値)
- [ ] 表の差し替え: 物質の付け替え(帳簿を物質 ID で付け替え・物質ごとの値を作り直す)・環境の値(重力・音速)が替わったら設定の型と層の基準を作り直す
  (T-0470 の引き継ぎ: ApplyEnvironment から GpuGas::RecordEnvironment / 木なら SetGasTreeEnvironment。script::CheckHotReloadCompatible を木と区画の上限で見る)
- [ ] 区画を世界のデータに書く(T-0590 の判断待ち 4。おすすめ: 世界の環境〔environment〕か world の分類に「気体を流す範囲」。書かなければ流れない = D-455)
- [ ] 基準の大気が無い世界(T-0590 の判断待ち 5。仮 = 区画を断る。真空を基準にして流すかはユーザー)
- [ ] アプリで燃える木箱の煙が上がる画像(T-0619 の後)

## メモ・参考
- 今の断り: ProbeSim::ValidateGas(sim/probe_sim_gas.cpp)。
- 作業場(gasWork)の並びは probe_gas_gpu.hlsli。帳簿は物質 ID ごと(int64 × 物質の数 + エネルギー + 運動量 × 3〔格子の軸〕)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
