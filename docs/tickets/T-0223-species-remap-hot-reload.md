# T-0223 ホットリロードで物質を足す・消す(付け替えの規則と世界〔CPU・GPU〕)

- Status: Done(2026-10-10。一部: 付け替えの規則と世界まで。main に統合 9152562。残りは T-0242・T-0243)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(作業役 2 時間以内)。覗き窓・実験室・エディタで当てる所は T-0242、通常の起動の監視は T-0243 に分けた
- マイルストーン: M2(docs/plan/ROADMAP.md。D-438)
- 設計: docs/design/13-scripting-game-model.md §2.5 / 決定: D-438・ADR-0065(新)・ADR-0047・ADR-0050・ADR-0032

## 目的(1〜2 行)
物質を足す・消す反応表の差し替えを、動いている世界にその場で当てるための規則と CPU の世界の実装。物質 ID(名前のバイト順)がずれるので
セルの物質を名前で付け替え、消えた物質は元素の単体に分けて戻す(原子を保つ)。記録と再生で同じ結果になる(付け替えは表 2 つだけから決まる)。

## 完了条件(チェックできる形で)
- [x] 付け替えの表 `sim::BuildSpeciesRemap(今の表, 新しい表)`(engine/src/sim/species_remap.*): 名前と元素の組み立てが同じ物質は ID だけ替える・
      消えた物質(組み立てが変わった同じ名前の物質も)は元素ごとの単体(一番安定な形)へ・単体の無い元素を含む物質を消す表は理由を返す
- [x] セル 1 つの付け替え `RxRemapCell`(shaders/common/species_remap.hlsli。HLSL と C++ で同じ関数): 成分は ID の昇順のまま・
      原子の数を保つ(単体の端数と成分の上限に入らない分は失い、報告)・熱を保つ(化学のエネルギーの差を mJ に切り上げて足し、報告)
- [x] CPU の世界(sim::ProbeReference::Advance)が物質の一覧の違う表への差し替えで 2 世代のセルを付け替える(LastRemapReport)
- [x] GPU の世界(sim::ProbeSim): 差し替えの刻みに付け替えの表(PackSpeciesRemap → t5)を上げ、適用の単位の前に 2 世代のセルを付け替える段
      (probe_tick.hlsl の RemapSpecies)。抽出の物質の ID もそのフレームから新しい表。gpu_probe_sim_test に物質を足す・消す差し替え(CPU と毎刻みビット一致)
- [x] ホットリロードの検査に方針(script::SpeciesChangePolicy: Reject〔今の実行時〕/ Remap〔付け替えられる世界〕)
- [x] テスト reaction_species_remap(CPU だけ): 同じ表・足す(ID がずれ名前で同じ)・消す(原子と熱)・組み立ての変化・成分の上限・単体が無い・
      検査の 2 つの方針・世界(足すだけの差し替えは同じ表への差し替えと名前で見て毎刻み同じ / 消す差し替えの前後で元素が保たれる)
- [ ] 覗き窓・実験室が物質の一覧の変わる表に追従し、エディタのホットリロードで物質を足す・消す(記録と再生・窓の確認)→ T-0242
- [ ] 通常のゲームの起動でもファイルを見る(変わったら改造の印)→ T-0243

## メモ・参考
- 規則の詳しい形と、採らなかった案は ADR-0065。決定性: 新しいコマンドの種類は要らない(差し替えの印 + 再生ファイルの表の中身〔ADR-0050〕から作り直す)。
- 付け替えは差し替えの刻みの始め(コマンドの適用の前)。その後は ADR-0047 のまま(新しい表でコマンド → 熱のキャッシュの作り直し → 全部のブロックを起こす)。
- 2 世代とも付け替える(GPU の RefreshTable が 2 世代の熱のキャッシュを作るのと同じ)。報告は刻みの始めの世代の分だけ。
- 試験の表(C++ と data/packages/combustion_test)は変えていない。テストは C++ の表の定義に水素(H2)などを足し引きして作る。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-10(作業役 wt2): 付け替えの規則を ADR-0065 に決め、species_remap.{h,cpp}・common/species_remap.hlsli・ProbeReference の付け替え・
  SpeciesChangePolicy・テスト reaction_species_remap を足した。GPU の世界(ルート署名に t5・RemapSpecies の段・抽出の物質の ID)と
  gpu_probe_sim_test の物質を足す・消す差し替えも入れた。覗き窓・実験室・エディタで当てる所は T-0242、通常の起動の監視は T-0243 に分けた。
  テスト: debug で reaction_species_remap・reaction_hot_reload・reaction_package_scene・float_check_* 8 本と gpu_probe_sim(約 400 秒)が通過。
  release で警告なし・1 つ目の束 + gpu_probe_(sim|peek|fire|rewind)・window_hot_reload_*・window_lab の 57 本が通過(約 14 分)。WARP の版は流していない。

## 引き継ぎメモ(HANDOFF に載せる状態)
- `sim::BuildSpeciesRemap` / `sim::RemapReactionCell`(engine/src/sim/species_remap.*。ライブラリ bicameral_reaction)・本体は common/species_remap.hlsli の
  `RxRemapCell`(Remap 型: NewId・PartBegin・Part・RemovedH0・UnitCount・Unit。GPU は probe_bindings.hlsli の ProbeSpeciesRemap が t5 を読む)。
  `ProbeReference::Advance`・`ProbeSim`(ProbeFrameInput::tableSwaps)は物質の一覧の違う表でも当てる(呼ぶ側の約束は
  `script::CheckHotReloadCompatible(…, SpeciesChangePolicy::Remap)` が通ること。ProbeSim は通らない表ならフレームの記録に失敗する)。
  ProbeSim は今の表の写し(m_tableCopy)を持つ。**覗き窓・実験室はまだ物質の一覧が同じ表だけ**なので、実行時のホットリロードは Reject のまま
  (frame/table_hot_reload は触っていない。T-0242 で Remap にする)。
- テスト: `-Filter "^(reaction_species_remap|gpu_probe_sim)$"`(reaction_species_remap は debug で約 1 分)。

## 判断待ち
1. **消した物質の化学のエネルギー(仮: 熱を保つ = 案 A。ユーザー未確認・取り消しやすい: RxRemapCell の最後の 2 行)**
   - 案 A(今): 温度(熱)を保ち、化学のエネルギーの差を「改造による出入り」として記録・表示する。プレイヤーと遊びへの影響: 物質を消しても
     その場の温度はほぼ変わらない(消した物質の元素が冷たい単体として残る)。世界のエネルギーの合計は改造の分だけ変わる(改造の印がある世界だけ)。
   - 案 B: エネルギーを厳密に保つ。影響: 安定な化合物(木・水など)を消すと、元素に戻す強い吸熱で周りが極端に冷える(木の壁のセルは熱が負に
     なり温度が作れない = 世界が壊れるので、そのセルは当てられない・または当てない表にする必要がある)。
   - おすすめ: A。改造は世界の反応ではない(D-428 の「嘘」に当たらない)。
2. **単体で割り切れない端数(仮: 失って報告 = 案 A。ユーザー未確認・取り消しやすい)**
   - 案 A(今): 1 セル 1 元素あたり「単体の原子の数 − 1」µmol 未満を失う(O2 なら O を 1 µmol = 16 µg まで)。影響: 精密な計器で世界全体の元素を
     数えると、物質を消した時だけ極わずかに減る。普段の遊び(研究)では見えない。
   - 案 B: セルの番号の順に端数を持ち越して世界全体で厳密に保つ(GPU で前置和が要る。費用: 小〜中)。影響: 計器でも完全に保存される。
   - おすすめ: A(D-435 の計器の精度より小さい。厳密さが要る遊びが出たら B)。
3. **単体の無い元素(仮: 当てない = 案 A。ユーザー未確認)**: 例えば今の試験の表は水素の単体(H2)が無いので、セルロースを消す変更は当てられない。
   - 案 A(今): 当てずに「単体を足すか、物質を残す」と出す。影響: 表を書く人は先に単体の物質を足す(1 回のホットリロードで両方してよい)。
   - 案 B: 元素の単体を表に自動で足す。影響: 書いていない物質が世界に現れる(ファイルと表が食い違い、版・再生の扱いが複雑になる)。
   - おすすめ: A。
4. **成分の上限に入らない単体(仮: 失って報告)**: セルの成分が 8 種類で詰まっている時、分けた先の単体のうち ID の大きい方が入らない。D-448(上限を無くす)で消える。
   影響: 8 種類が混ざったセルで物質を消した時だけ、その元素が消える(報告は出る)。おすすめ: このまま(D-448 の後に無くなる)。

## 分けたもの(司令塔が ROADMAP に足すか決める)
- **T-0242 覗き窓・実験室とエディタで物質を足す・消す**: 覗き窓(影の鎖を作り直す・見る物質 ProbeViewSpecies の ID)・実験室(材料の ID・BLAB の記録・
  LabSession の GPU と CPU の箱を RxRemapCell で付け替える)が物質の一覧の変わる表に追従。その後 frame/table_hot_reload を SpeciesChangePolicy::Remap にして、
  エディタで物質を足す・消す + 記録と再生(reaction_replay_table に物質を足す差し替え)・`--auto-reload` の窓の確認(window_hot_reload に物質を足す段)。
  反応表のパネル(T-0219)の一覧も表の差し替えで作り直されるか確かめる。保存点は今どおり捨てる。
- **T-0243 通常のゲームの起動でもファイルを見る(D-438)**: `--editor` でなくても約 0.25 秒ごとに指紋を見て当てる。プレイ中に法則が変わったら
  改造の印(LoadedReactionTable::modifiedWorld → 再生ファイルの表の印。D-437)。窓の自動確認(エディタなしの --auto-reload)。
