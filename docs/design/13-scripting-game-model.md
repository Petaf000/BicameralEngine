# 13 Luau とゲームの Model(L4 詳細設計)

対応する決定: D-103(BotW 型の進行)/ D-107(進行も Model = GPU)/ D-304(ホットリロード)/ D-313・D-318(ゲームを作る人が触るものは全部 Luau)/ D-320(Mod・サンドボックス)

## 目的
ゲームを作る人(ユーザー・仲間・Mod を作る人)が触るものを、全部 Luau の 1 つの言語で書けるようにする。
Luau は CPU で動く。GPU の世界に効くものは「表にベイクして渡す」か「コマンドを出す」かのどちらか。

## 1. Luau で書くものと、行き先
| 書くもの | 行き先 | 実行時に Luau が動くか |
|---|---|---|
| 元素・物質・反応則・魔法のパッチの種類(02)。**上限なしで増やせる**(D-418) | 整数の表にベイク → GPU | 動かない |
| 壊れ方の上書き(08 §3)・物質ごとの力学の値 | 表にベイク → GPU | 動かない |
| 魔法具・魔石の定義と、買える魔法具の設計(18) | 物の体積と表にベイク → GPU | 動かない |
| 生き物の種類・行動・生態(09) | 表にベイク → GPU | 動かない |
| 世界の生成手順(05 §5) | 表にベイク → GPU の生成器 | 動かない |
| 音の対応表(11)・操作の種類(12)・文字列 | CPU の表 | 動かない |
| 進行の条件(発見・目標・依頼の達成条件) | 表にベイク → GPU(§3) | 動かない |
| UI・会話・依頼の流れ | CPU の View で実行 | 動く(コマンドを出すだけ) |
| エディタの拡張・ツール(14) | CPU のエディタで実行 | 動く |

## 2. サンドボックスとホットリロード
- Luau はもともとサンドボックス向けに作られている(ファイル・OS への手段を渡さない)。ゲームとして渡す API だけを持つ環境で動かす。Mod も同じ環境(D-320)。
- 1 つのパッケージ = 1 つのフォルダ(ゲーム本体もパッケージの 1 つ)。読み込む順番と、表の合わせ方(上書き・追加)を決める。
- ホットリロード: エディタがファイルの変更を見て、そのパッケージを読み直し → ベイク → 表の新しい版を、刻みの境界でコマンドとして GPU に適用する。
  再生ファイルには表の版が記録されるので、ホットリロードをまたいでも再現できる。
- 型: Luau の型注釈を使い、ベイクの前に型検査を通す(§2.3。T-0140・ADR-0046)。

### 2.1 殻(実装。T-0020・ADR-0030)
- `script::LuauSandbox`(engine/src/script/luau_sandbox.*)。Luau は vcpkg の port(0.729)。CPU だけで、GPU もシミュの状態も知らない。
- 見せるもの(許可の一覧): base(`gcinfo`・`getfenv`・`setfenv`・`newproxy` を除く)・coroutine・table・string・math・utf8・bit32・buffer・vector
  + ホストが Seal() の前に登録した関数(1 つの表にまとめる)。os・debug・require は開かない(io・load 系は Luau にもともと無い)。
- 使い方: `Create(limits)` → `RegisterFunction("host", "emit", fn, context)` → `Seal()` → `Run(chunkName, source)`。
  Run ごとに自分用のスレッド(グローバルの書き込みはそこに入り、ほかの Run と混ざらない)。戻り値は文字列(アドレスを含めない)。
- 上限(1 回の Run ごと): 安全点(ループの折り返し・呼び出し)の数・殻全体のメモリ。超えたら止まり、理由の種類(`SafepointLimit`・`Memory`)を返す。
  時計で止めないので、止まるかどうかが PC の速さに依らない。
- 決定性: Run の始めに math の種を `randomSeed` に戻す。同じ設定・同じソース・同じ実行の順番なら、戻り値と安全点の数が同じ(luau_sandbox_test)。
- **書く側の規則(ベイクする Luau)**:
  - 関数・表をキーにした表を `pairs` で回した順番を、結果に使わない(アドレスで決まる)。文字列・数のキーの順番も、使う前に並べる。
  - 超越関数(`math.sin`・`exp`・`log`・`^`)の結果をベイクに使うかは T-0021 で決める(機種をまたぐと C ランタイム次第。未確認)。
- ホットリロードは §2.4(T-0139)。型検査(Luau.Analysis)は §2.3。
  パッケージは §2.2(T-0138)。

### 2.2 パッケージ(実装。T-0138・ADR-0031)
- `script::LoadPackages`(engine/src/script/luau_package.*)。ディスクからは `ReadPackageRoot(root)`(直下のフォルダ = パッケージ)。テストは luau_package_test。
- 形: フォルダの `*.luau`。`package.luau` が `{ format = 1, depends = { "core" }, entry = "init" }` を返し、entry が「分類 → 鍵 → 値」の表を返す。
  ```lua
  -- mods/fire-plus/init.luau
  local rates = require("lib/rates")          -- 同じパッケージの lib/rates.luau だけ
  return { reactions = { smolder = { rate = rates.slow } } }
  ```
- require: パッケージの中だけ(`..`・絶対パス・ほかのパッケージは届かない)。1 回の読み込みで 1 つのモジュールは 1 回だけ走る。マニフェストでは使えない。
- 読む順番: 依存の後 → 名前(バイト順)。`basePackage`(ゲーム本体)にほかの全部が依存する。依存の欠け・循環・誤りは、そのパッケージと依存するものだけを読まない。
- 表の合わせ方(`MergeMode`。**Q8 未回答。仮で案 C**): 既定 `AddOnly`(既にある鍵を書いたパッケージは丸ごと読まない)、
  上書きを許す設定で `OverrideMarked`(上書きを記録して「改造された世界」の印 `modifiedWorld`)。案 A 用の `Override`(印なし)もある。値は丸ごと置き換え。
- 値は `ScriptValue`(nil・真偽・数・文字列・表。鍵は数 → 文字列の順に並べ直す)。関数・vector・NaN・循環する表は誤り。
- 表の版 `TableVersion` = 合わせた表の中身(分類・鍵・値)の正準なバイト列のハッシュ。誰が書いたかは入れない(同じ法則なら同じ版)。
- 決定性: 入れる順番・殻・回数を変えても、同じパッケージ群・同じ種なら同じバイト列(luau_package_test)。

### 2.3 読む前の型検査(実装。T-0140・ADR-0046)
- `script::LuauTypeChecker`(engine/src/script/luau_type_check.*)。Luau.Analysis(vcpkg の luau port に入っている)の新しい型ソルバー。
  型の定義は `data/types/bicameral.d.luau`(`PackageManifest`・`PackageEntry`・`Element`・`Species`・`ReactionRate`・`Reaction`。
  ビルドが bin/data/types に写す。エディタの定義ファイルとしても使える)。
- LoadPackages に `typeChecker` を渡すと、マニフェストを走らせる前に package.luau を、entry を走らせる前に全部のファイルを検査する。
  誤りのあるパッケージ(と依存するもの)は読まない。ランタイムの入り口(LoadReactionTable)は必ず検査する(ゲーム本体が落ちたら起動を止める)。
- 見るもの: 全部のファイルを strict(`--!nonstrict` はそのファイルだけ)・知らない名前(殻で見えない os・debug なども)・require の先・文法・
  返す表の形(欄ごとに突き合わせる: 欄が足りない・知らない欄〔綴りの誤り〕・欄の型)。整数か・範囲の中かは読み手が見る(ADR-0032)。
- 出し方: `combustion_test/species.luau:12:9: 型が合わない: species.water.formation_enthalpy_j_per_mol: Expected this to be 'number', but got 'string'`
  (ファイル:行:列: 何が違うか: 欄の道筋と Luau の説明)。require した先の表の誤りも、そのファイルの行に出る。
- 書く人は型を付けなくてよい。付ければ(`local species: { [string]: Species } = { ... }`)、その場で Luau が直接見る。
- **読み手の欄を変えたら型の定義も変える**(変えないと新しい欄が「知らない欄」になり、ゲーム本体のパッケージが落ちる)。
- 決定性: 検査のたびにモジュールの状態を捨て、診断を並べて重複を除く(luau_type_check_test: 2 回・間にほかのパッケージ・作り直しで同じ列)。

### 2.4 ホットリロード(実装。T-0139・ADR-0047)
- `script::ReactionTableHotReload`(engine/src/script/reaction_hot_reload.*)がパッケージのフォルダと型の定義の中身の指紋を見て、
  変わったら(2 回続けて同じ指紋を見てから)読み直す。物質の一覧が同じ時だけ当てる。落ちたら今の表のまま誤りを返す。
- `frame::TableHotReload`(engine/src/frame/table_hot_reload.*)が `--editor` のときだけ数フレームごとに見て、次の適用の単位の刻みに
  印のコマンド(PROBE_COMMAND_TYPE_TABLE・版)を足し、`sim::ProbeFrameInput::tableSwaps` で表を渡す。ProbeSim はその刻みの適用の前に表を写し、
  適用の後に RefreshTable(熱のキャッシュを作り直して全部のブロックを起こす)。CPU リファレンスも同じ(gpu_probe_sim_test で毎刻みビット一致)。
- 再生は印の版の表を知っている表から探す。表の中身を再生ファイルに残すのは T-0193。表を差し替えたら保存点は捨てる。

### 2.5 物質を足す・消す(T-0223・ADR-0065・D-438)
- 物質 ID は名前のバイト順なので、物質を足す・消すと後ろの ID がずれる。差し替えの刻みの始め(コマンドの適用の前)に、2 世代のセルを名前で付け替える。
  付け替えは今の表と新しい表だけから決まる(`sim::BuildSpeciesRemap`。engine/src/sim/species_remap.*)ので、再生は記録した表の中身から同じ付け替えを作る。
- 名前と元素の組み立てが同じ物質は ID だけ替える。消えた物質(と組み立てが変わった物質)は、元素ごとに新しい表の単体
  (その元素だけでできた一番安定な物質)へ原子の数を保って分ける。単体の端数・セルの成分の上限に入らない分は失い、量を報告する。
  熱は保つ(化学のエネルギーの差はエネルギーの出入りとして報告)。単体の無い元素を含む物質を消す表は当てない。
- セル 1 つの付け替えは shaders/common/species_remap.hlsli の `RxRemapCell`(CPU と GPU で同じ関数)。
- 世界: CPU リファレンス(`sim::ProbeReference`)と GPU(`sim::ProbeSim`。差し替えの刻みに付け替えの表を t5 に上げ、適用の単位の前に
  probe_tick.hlsl の RemapSpecies)が付け替える(gpu_probe_sim_test で毎刻みビット一致)。
- 段階: ホットリロードは `script::SpeciesChangePolicy` で、世界の側が付け替えられる時だけ物質の一覧が変わる表を当てる。
  ホットリロード(`frame::TableHotReload`。エディタも通常の起動も)は既定で Remap(T-0266・D-438。T-0242 では `--species-remap` の時だけだった。debug の窓のずれは -Od のせいだった。ADR-0079): 覗き窓は付け替えた世界の写しから影の鎖を作り直す(見る物質の ID も新しい表。刻みの途中で終わったフレームの S(s) は付け替えた後なので、その抽出から新しい表で読む)。
  実験室は印の刻みに箱の全部のセルを付け替える(GPU は shaders/sim/lab_box.hlsl の RemapLabSpecies、CPU は `sim::RemapLabBox`。同じ RxRemapCell)。
  まだ刻んでいない置く操作(実験室の材料・同じフレームの筆のコマンド)の物質 ID も名前で新しい表へ(消えた物質の材料は落とす)。
  記録と再生は表の中身から同じ付け替えを作り直す(再生ファイル・BLAB。新しいコマンドの種類は要らない)。
  通常のゲームの起動でもファイルを見る(再生でなければ。読み直した表は「改造された世界」の印付き。T-0243・ADR-0047 改訂)。

### 2.6 エンジンは中身を持たない・ブランクが既定・標準パッケージ・サンプルの世界(D-454〜D-457・ADR-0151)
- **エンジンは仕組みだけ**(D-454): 元素・物質・反応・材料・環境(大気の成分・重力・周囲の温度・太陽光など)の名前と値は、すべてパッケージ(Luau)に置く。
  エンジン本体(C++・HLSL)が持つのは、表を読む・ベイクする・GPU で流す仕組みと、単位・整数の表し方・上限などの「器」の決まりだけ。
  どこまでがエンジンでどこからがパッケージかは ADR-0151。物理定数(気体定数・重力など)をデータにする範囲は T-0372 で詰める。
- **ブランクの世界が既定**(D-455): 何のパッケージも足さずに起動した世界は本当に空(元素も物理定数も無い)。何かを足すまで世界は何も起きない。
  空の世界でも刻み・記録・巻き戻し・エディタは動く(表が空でもベイク・GPU の段が壊れない)ことを試験で確かめる(T-0371)。
- **標準パッケージ**(D-457): 現実の元素・現実の物理定数・現実に則した反応と物質・地球の環境(大気・重力・周囲の温度)を、
  それぞれ独立に足せるパッケージとして用意する。足す順・依存(反応は元素に依存する など)はパッケージの読み込み順(§2.2)で表す。
  エディタの環境ミキサー(14 §2)はこれを選んで足す UI。
- **サンプルの世界**(D-456): UE のサンプルマップのように、標準パッケージと試験用の地形・物を全部入りにした別パッケージで、開けばすぐ動く。
  エンジン本体には含めない。今 C++ で組んでいる木箱の世界・空気の成分の既定・気体の音速の基準・実験室と筆の材料の既定はここへ移す(T-0371)。
- 試験: 今の試験の多くは「サンプルの世界」を前提にしているので、試験はサンプル(または標準)パッケージを明示して読む形にする(T-0371。
  記録のハッシュは表の版を含むので、移しても値が同じならハッシュが同じであることを確かめる)。
- **実装(T-0371 の一部)**: 起動の既定はブランクの世界にする(`bicameral --world blank`。今は仮で既定 sample〔GPU の止まりの疑い〕。セルは全部空〔`sim::MakeBlankProbeWorld`〕・積み木なし・
  表は `script::LoadBlankReactionTable`〔物質は使わない 0 番だけ・規則 0〕)。`--packages` を渡すと、ゲーム本体なし(全部 Mod)で読んだ表に
  なる(世界はまだ空)。今の世界は `--world sample`(空気の中の木箱・積み木・ゲーム本体 combustion_test)。再生ファイルは世界の種類
  (`save::ReplayWorld` の Sample = 1・Blank = 2。形式の版は同じ)を持ち、`--replay` はそれで起動する。試験: blank_world(CPU)・
  gpu_probe_blank(_warp)・window_blank_record/play。今の窓の試験は `--world sample` を明示する。
  サンプルの世界の中身(組み立て・材料のひな形・抽出の物質・環境の値)はパッケージにある(T-0398〜T-0400)。基準の大気は T-0373。
- **世界の組み立て(T-0398・ADR-0172)**: 初めのセルと物は、パッケージの分類 `world_cells`(領域: 物質 → µmol・温度・箱〔省けば格子の全部〕・
  殻の厚さ・重ねる順 layer)・`world_bodies`(直方体: mm・g・番号の順 order)に書く(script/world_layout_package・sim/world_layout)。
  反応表と同じ読み込みなので表の版と中身に入り、再生ファイルは表の中身だけから同じ初めの世界を作る。分類が無ければ空(ブランク)。
  窓の世界は `--world` に依らず読んだ表の組み立てから作る(`sim::MakeProbeInitialWorld`・`sim::MakeLayoutPhysicsScene`)。
  サンプルの木箱と積み木は combustion_test の world_cells.luau・world_bodies.luau(前の C++ とビット一致。world_layout_test)。
  ほかの分類(材料のひな形・環境の値)も同じ形で足す(T-0398 のチケットの「共有の決め事」)。
- **実装(T-0399・ADR-0174)**: 材料のひな形・抽出の物質・実験室と置く操作の既定はパッケージの分類 `material_templates`・`view_quantities`・
  `editor_defaults`(反応表と同じ読み込み・同じ表の版。読む所 script/material_templates_package・ベイク sim::BakeMaterialTemplates・
  型 data/types/bicameral.d.luau)。分類が無ければ材料は 0 個・実験室の箱は空のセル・抽出の物質は 0 番(ブランクの世界)。
  サンプルの値は data/packages/combustion_test の material_templates.luau・view_quantities.luau・editor_defaults.luau。
- **環境の値(T-0400・ADR-0176)**: 分類 `environment` に 1 つの鍵 = 1 つの値で書く(`gravity_um_per_s2`・`sound_speed_mm_per_s`。整数・単位は鍵の名前・
  書かない値は 0 = ブランク)。`script::ReadWorldEnvironment` → ベイクした表の `environment`(`sim::WorldPhysics`)→ 物理の重力・気体の設定。
  反応表と同じ読み込みなので表の版・再生ファイルの表の中身に入る。サンプルの世界は `combustion_test/environment.luau`(地球の値)。
  基準の大気(成分・温度・地面の圧力)は大気の成分と一緒に T-0373 で足す。
- **基準の大気(T-0373・ADR-0182)**: 成分は分類 `atmosphere`(物質 → ppm。足して 1000000)、温度と地面の圧力は `environment` の
  `atmosphere_temperature_mk`・`atmosphere_surface_pressure_pa`。`script::ReadWorldAtmosphere` → `sim::BakeWorldAtmosphere`(知らない物質・
  合計・温度と圧力の有無を検査)→ ベイクした表の `atmosphere`。気体を世界に載せる段(T-0210)が層の基準に使う。
- **標準パッケージ(T-0373・ADR-0183)**: `data/standard` の棚(real_elements・real_matter・real_reactions・earth_environment・earth_air)。
  値は現実のまま(熱伝導率に試験の倍率を掛けない)。足すのは環境ミキサー(14 §2)か `--mix`。読み方は `ReactionTableSource::addedPackages`
  (と、ブランクの世界で元のフォルダを読まない `readPackageRoot = false`)。サンプルの世界 combustion_test とは並べる(仮。ADR-0183 の案 A)。
  地球の重力・音速の基準の値はエンジンから出し(`sim::StandardWorldPhysics` を消した)、試験の写しは tests/standard_world_physics.h。

## 3. 進行(Model)
- 進行の状態 = GPU の小さなバッファ: フラグ(ビット列)・カウンタ(int32)・発見の記録(何を・いつ・どこで)。
- 発見の条件は、イベントとの突き合わせとして表にベイクする(例: 「反応 X が起きたのを、道具 Y で観測した」「温度が Z を超えたのを記録した」)。GPU の段 9 でイベントを照合し、満たしたら記録してイベントを出す。
- 依頼・会話の流れ(CPU の Luau)は、スナップショットの進行の状態を読み、変えたいときはコマンド(「フラグを立てる」)を出す。直接は書き換えない。
- 大きな目標(D-103)の達成条件も、進行の条件の 1 つとして書く。

## 4. 記法
- 反応則の例は 02 §7。記法の細部(キーワード・既定値・単位の書き方)は、最初の反応表を書く時にユーザーと決める。
- 単位つきの値を書けるようにする(`300 * K`、`2.5 * kJ / mol`)。ベイクで 04 の整数の単位に直す(整数の計算で)。

## テスト
- 同じ Luau のパッケージから、同じ表(バイト単位で同じ)がベイクされる(機種・OS をまたいで)。
- サンドボックス: ファイル・OS・ネットワークに触れようとすると失敗する(luau_sandbox_test。T-0020)。
- ホットリロード前後で再生ファイルが再現できる。

## 未確認
- ~~Luau の C++ への組み込み(vcpkg にあるか、自前でビルドするか)。~~ → vcpkg の port(ADR-0030)。
- 超越関数の結果が機種・C ランタイムをまたいで同じか(T-0021 で、ベイクに使うなら測るか自前の実装にする)。
