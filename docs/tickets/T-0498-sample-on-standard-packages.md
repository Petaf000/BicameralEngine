# T-0498 サンプル combustion_test を標準パッケージ「現実」の上に作り直す

- Status: Done(司令塔の確かめ待ち)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ROADMAP の T-0498 の行(D-490 ①)
- 設計: docs/design/13-scripting-game-model.md §2.2・§2.6 / 決定: D-490 ①・Q50 の B・ADR-0183(追記)

## 目的(1〜2 行)
サンプルの世界 combustion_test を、標準パッケージ(現実の元素・物質・反応・地球の環境)を読み込み、遊びのための値だけを書き直す/足す形にする。
作り手の見本になる形にする(値は標準パッケージの 1 か所)。

## 完了条件(チェックできる形で)
- [x] combustion_test が real_elements・real_matter・real_reactions・earth_environment に依存し、遊びのための値だけを書く
- [x] 依存の標準パッケージを棚から読む(data/packages に写さない)・依存の表を写して読める(`require("@dep")`)
- [x] ゲーム本体が依存の鍵を書き直しても「改造された世界」にならない(Mod は今までどおり)
- [x] release のビルドが通る(警告 0)
- [x] CPU の試験(luau・reaction・environment・standard・world_layout・blank_world・play_metric・lab_box・multires・replay・physics・gas など 48 本。release)
- [x] GPU・窓の試験(最小の組 19 本: gpu_reaction(_warp・_limits)・gpu_probe_sim(_tables・_warp)・window_table_edit・
      window_hot_reload(_game)・window_gravity_reload の記録と再生)。image_*・ほかの window_* は回していない(司令塔がマージ後の全体で)
- [x] ADR-0183 の追記・13 §2.2/§2.6・data/standard/README

## やったこと(仕組み。作り手から見た違い)
1. **依存は棚から読む**(`script::AddStandardDependencies`・`ReactionTableSource::standardRoot`〔空なら exe の横の data/standard〕・
   `LoadedReactionTable::standardRoot`)。どのパッケージ(Mod も)も標準パッケージに依存してよい。
2. **`require("@dep")` / `require("@dep/sub/util")`**: たどって依存するパッケージの表の写し(読むだけ)。殻(`ModuleResolver` に require を書いた
   チャンクを渡すようにした。依存のモジュールの中の require はそのパッケージの中で解く)と型検査(`CheckPackage` の `dependencies`)の両方。
3. **ゲーム本体は自分のたどる依存の鍵を書き直してよい(印なし・overrides には残る)**。ゲーム本体のたどる依存はゲーム本体に依存させない
   (前は全部のパッケージがゲーム本体に依存し、循環になった)。
4. **データ**: species.luau は real_matter の全部の物質を `table.clone` で写し、熱伝導率だけに倍率(固体 = 木・炭 ×20000、ほかは気体 ×5000)。
   elements / reactions は仮の魔素の分だけ。environment.luau は消した(earth_environment)。初めのセル・材料・抽出・既定・指標・積み木はそのまま。
5. **エディタの反応表のパネル**: 棚の標準パッケージの値は探すが書き戻さない(理由を出す)。ゲーム本体が写して書き直した鍵は、元のリテラルを
   書き戻さない(倍率を掛ける前の値に掛けた後の値を書いてしまうため。`LoadedReactionTable::overrides` で判断)。
   --auto-table-edit は仮の魔素の木の燃焼の速さを書き換える。--auto-reload の物質を足す所は mana_test の前、重力は init.luau に環境の鍵を足す。

## 試験のハッシュ・値が変わったもの(理由)
- 表の中身: 元素 5 → 6(Ar)・物質 8 → 9(argon。名前の順で先頭に入り、ほかの物質の ID が 1 つずれる。量はどのセルも 0)・規則 7 のまま・
  環境に基準の大気(温度・地面の圧力・成分 atmosphere)が増えた。→ 表の版・物質の ID・セルの記録のハッシュ(物質の並びを含むもの)が変わる。
  燃焼の値(熱伝導率・反応の速さ・比熱・生成エンタルピー)は前と同じなので、燃え方(温度・量)は前と同じはず(argon の量は 0)。
- C++ の試験で直したもの: reaction_table_edit(数 6・9・7、棚の値は書き戻さない)・standard_packages(サンプルに同じ標準を足しても同じ版・
  読まないもの 0)・reaction_hot_reload / reaction_replay_table(書き換える所を combustion_test の中へ)・environment_package・world_layout・
  reaction_package(物質の数 10・11、読む順)・luau_type_check(依存を見せる)・reaction_species_remap(貴ガスの表は argon が無い時だけ足す)。
  tests/reaction_test_cells.h の「いろいろなセル」: 物質がインラインの数(8)より多い表では、反応しない物質(argon)を外して収める
  (外さないと反応で 9 種目ができ、「上限に当たらない場面」の試験〔reaction・multires・multires_wide〕が溢れを使う)。セルの中身(乱数の引き方)は
  物質の ID がずれたので前と違う → これを使う GPU の試験(gpu_reaction など)のセルも変わる(CPU との一致で見るので決まった値は無い)。
  新しい試験: luau_package の TestRequireDependency・TestBaseOverridesDependency。
- 記録したハッシュを持つ試験は無い(窓の試験は記録 → 再生で同じかを見る)ので、決まった値の書き換えは不要の見込み。

## 判断待ち(仮で進めたもの。ユーザー未確認)
1. **パネルで棚の標準パッケージの値を書き戻すか**(仮: 書き戻さない)。
   - 書き戻さない(仮): 作り手から見て、サンプルの世界のパネルで現実の反応の速さ(木の燃焼の A・Ea など)を直接は変えられない
     (理由が出る)。変えるならゲーム本体のパッケージで写して書き直す。棚(ほかの世界も使う現実の値)は変わらない。
   - 書き戻す: パネルで今までどおり変えられるが、exe の横の棚のファイルが書き換わり、同じインストールの全部の世界に効く。
2. **サンプルの初めの空気**(仮: 今のまま 300 K の O2・N2): earth_air(15 ℃・Ar・CO2 入り)に依存すると、燃え始め・燃え方の試験の値が変わる。
3. **ブランクの世界に `--packages data/packages` でサンプルを Mod として読む**と、標準の鍵の書き直しになるので読まれない(足すだけの設定)。
   サンプルは `--world sample`(ゲーム本体)で開く前提。

## 並走との重なり
- **wt2 T-0491(物質のエントロピーの任意の欄 `standard_entropy_mj_per_mol_k`)**: wt2 は real_matter と combustion_test/species.luau の両方に欄を
  足している。このチケットの後では combustion_test/species.luau は real_matter を `table.clone` で写すので、**エントロピーは real_matter に書けば
  サンプルにも自動で入る**。マージで species.luau がぶつかったら、このチケットの形を取り、wt2 の combustion_test 側の値は捨てる
  (mana_test には欄を足していない)。real_matter と型(bicameral.d.luau)・読み手(reaction_package)は wt2 の変更をそのまま取る。
- wt4 T-0496(パッチを外す): reactions.luau は仮の魔素の規則だけになった(このチケット)。gpu_probe_patch_test などは表を名前で引くので影響なしの見込み。

## メモ・参考
- 番号: このチケットでは新しいチケット・ADR の番号を使っていない(ADR-0183 の追記)。
- 分けたもの(候補): パネルの「書き直しを作る」操作(棚の値を変えたい時に、ゲーム本体へ写して書き直す行を作る)。T-0512 の候補(行は作っていない)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
### 2026-10-15(wt6 の作業役)
- 仕組み(依存は棚から・`@dep` の写し・ゲーム本体の書き直しは印なし)とデータの作り直し・パネルと自動の確認の書き換え所・文書。release のビルド OK(警告 0)。
- 型検査: `local real: PackageEntry = require("@real_matter")` は表のリテラルの型が PackageEntry に入らず落ちたので、any で受けて物質の表に型を付けた。
- CPU の試験 48 本が通った(1 回目は物質 9 種の前提で 6 本落ち → 直した)。archmap --check OK。
- GPU・窓の最小の組 19 本が通った(2026-10-15)。
