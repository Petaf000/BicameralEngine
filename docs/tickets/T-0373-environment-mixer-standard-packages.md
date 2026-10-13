# T-0373 環境ミキサー(中身の部分): 標準パッケージと、それを世界に足す仕組み

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(UI は T-0452 に分けた)
- マイルストーン: D-471 ② 中身の器(docs/plan/ROADMAP.md)
- 設計: docs/design/14-editor-tools.md §2「環境ミキサー」・13 §2.6 / 決定: D-457・D-454・D-455・D-471 ②・D-480・D-486・ADR-0176・ADR-0182・ADR-0183

## 目的(1〜2 行)
現実の元素・物質・反応・地球の環境(重力・基準の大気)を、独立に足せる標準パッケージ(Luau)として用意し、ブランクの世界に足す仕組み
(読み込みの足す一覧・依存の解決・起動の引数 --mix)を作る。エンジンは仕組みだけを持ち、地球の値をエンジンから出す(T-0372 の引き継ぎ)。

## 分けたもの(着手前に機能単位で分けた)
- **T-0373(これ)**: 標準パッケージの中身 + 足す仕組み(読み込み・棚・依存)+ 基準の大気 + 重力の移し替え + --mix。
- **T-0452**: 環境ミキサーのパネル(エディタの UI。棚の一覧・足す/外すボタン・依存の知らせ・刻みの境界での差し替え)と窓の自動の確かめ。
- **T-0453**: 足した環境の値・初めのセル(earth_air)を動いている世界に効かせる「世界を作り直す」(今は表の法則だけ差し替わる。T-0444 と関係)。

## 完了条件(チェックできる形で)
- [x] data/standard に real_elements・real_matter・real_reactions・earth_environment・earth_air(依存つき・title つき)
- [x] 足す仕組み: ReactionTableSource::addedPackages・readPackageRoot、棚 script/standard_packages(ReadStandardCatalog・MixAdd・MixRemove・MixFolders)、
      ホットリロードの読む元の差し替え(ReactionTableHotReload::Replace)、窓の --mix
- [x] 基準の大気: 分類 atmosphere + environment の atmosphere_temperature_mk・atmosphere_surface_pressure_pa → sim::WorldAtmosphere(ADR-0182)
- [x] 重力の移し替え: sim::StandardWorldPhysics と fx::STANDARD_GRAVITY をエンジンから消し、試験は tests/standard_world_physics.h(写し)
- [x] 試験: standard_packages(新)・environment_package・reaction_package・world_layout・blank_world・reaction_hot_reload・物理と気体の CPU の試験が通る
- [x] 窓: --world blank --mix(全部)が起動して刻む・window_blank_*・--world sample の窓の試験が変わらない
- [x] release のビルドが警告 0・archmap --check

## 判断待ち(仮で進めたもの。ユーザー未確認)
1. **サンプルの世界(combustion_test)と標準パッケージの関係**(ADR-0183 の案 A〜C)。仮: **案 A 並べる**。
   作り手から見た違い: サンプルは今までどおり(試験の倍率で伝導だけでも火が広がる)。標準パッケージを足したブランクの世界は現実の値で、
   放射・気体の流れが入るまで伝導だけでは火がほとんど広がらない。同じ物質の値が 2 か所にある。試験とハッシュは変わらない。
   案 B(サンプルを標準の上に作り直す)へは後で移れる。
2. **標準パッケージの熱伝導率は現実の値**(試験の倍率を掛けない)。仮: 現実の値。作り手から見た違い: 上の 1 と同じ。
3. **基準の大気の書き方**(ADR-0182 の案 A〜C)。仮: 案 A(成分は分類 atmosphere の物質ごとの鍵・温度と圧力は environment の鍵)。
4. **足した一覧の保存先**: 今は起動の引数 --mix とエディタの間だけ(再生ファイルは表の中身を持つので再生はできる)。世界のファイルへの保存はセーブの形(15)で。

## メモ・参考
- 「現実の物理定数」のパッケージは作らない: 世界ごとに変えられる物理の値は重力と音速の基準だけで、どちらも地球の環境(D-480)。R は器。
- 液体の水・氷は相変化(T-0415)と、地面の物質は T-0414 と一緒に足す(D-471 ②: 場面に要る分だけ)。
- 反応の速さ: 熱分解だけ文献(Bradbury 1979)、ほかの 4 本は combustion_test と同じ仮の値(未確認と明記)。⑤ の場面で文献の値に。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-13(wt2・並走): 着手前に UI(T-0452)と世界を作り直す(T-0453)を分けた。data/standard に 5 つの標準パッケージ(現実の値・title・依存)、
  ReactionTableSource::addedPackages・readPackageRoot・棚 script/standard_packages・--mix、基準の大気(分類 atmosphere。ADR-0182)、
  StandardWorldPhysics と STANDARD_GRAVITY をエンジンから消して tests/standard_world_physics.h へ(ADR-0171 追記 3・ADR-0183)。
  release(警告 0): standard_packages(新)・environment_package・reaction_package・world_layout・blank_world・reaction_hot_reload・
  reaction_replay_table・luau_*・gas_reference・gas_tree・gpu_gas_tree(_warp)が通過。archmap --check OK。
  窓(release・GPU): window_mix_start・window_blank_record/play/rewind・window_hot_reload(_game)_*・window_table_edit が通過。
  司令塔がマージ後に: ROADMAP の T-0373 を Done に・T-0452(Done)・T-0453(Todo)の行・QUESTIONS に判断待ち 1〜3(ADR-0182・ADR-0183)。
