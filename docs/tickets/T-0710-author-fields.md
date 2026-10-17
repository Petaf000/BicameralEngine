# T-0710 作り手の欄の型(土台の段 0)

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 土台(runner/design/substrate-decided.md の段 0)
- 設計: substrate-decided.md ★ 決定 3・5・§1.1・§1.4 / 決定: D-509〜D-515・ADR-0326(本体が書く)

## 目的(1〜2 行)
作り手が Luau に書く新しい欄(物質の `phase` と動き方の 4 つ・環境の `world_edges`)を、型・読み手・ベイクの検査に入れる。
中身(流れ)はまだ動かさない(段 1 以降)。`condensed` は廃止(読み替えて警告)。

## 相の表の型(並走の T-0713 セルの見立てが読む。先に決める)
- `sim::SpeciesPhase`(engine/src/sim/reaction_table.h): `enum class SpeciesPhase : uint8_t { Gas = 0, Liquid = 1, Solid = 2 }`。
- 定義: `SpeciesDefinition::phase`(既定 Gas)が相のただ一つの源。前の `bool condensed`・`bool liquid` は消した
  (C++ で書く表は `.phase = sim::SpeciesPhase::Solid` のように書く)。
- ベイク: `BakedReactionTable::speciesPhase`(`std::vector<SpeciesPhase>`。添字 = 物質 ID。ID 0 = Gas)。
  前の `speciesCondensed`(phase != Gas)・`speciesLiquid`(phase == Liquid)は speciesPhase から作る写しとして残す
  (読み手が 10 か所ほどあるので段 0 では動かさない。段 1 で読み手を speciesPhase へ移して消す)。
- 動き方の値: `SpeciesDefinition::transport` / `BakedReactionTable::speciesTransport`(`sim::SpeciesTransport`。添字 = 物質 ID)。
  欄はどれも `std::optional`(書かない = 今と同じ動き。0 と書いたのとは違う):
  `viscosityMicroPascalSeconds`(uint64。液体だけ)・`diffusivityMm2PerS`(uint32。気体だけ)・
  `permeabilityNm2`(uint64。固体だけ。パッケージは µm² で書き、1e-6 µm² = 1 nm² の整数で持つ)・`suctionPascals`(uint32。固体だけ)。
  GPU の表には入らない(表とハッシュは変わらない)。

## 完了条件(チェックできる形で)
- [x] 型(data/types/bicameral.d.luau)・読み手(reaction_package・environment_package)・ベイクの検査(reaction_table)
- [x] `condensed` を読み替えて警告・`condensed` と `phase` の食い違いは誤り・標準パッケージと場面を `phase` だけに
- [x] `condensed` で書いた表と `phase` で書いた表のベイクの結果が同じ(GPU の表・相の表)
- [x] ベイクの知らせ(§1.4)
- [x] 試験(archmap --check は Linux 側で毎回途中で切れて確かめられていない。map.yaml の 3 つの記号は名前も場所も変えていない): luau_type_check・reaction_package・standard_packages・environment_package・world_layout が通る・release のビルドの警告 0・archmap --check

## メモ・参考
- 決めたこと(仮・ユーザー未確認。名前と単位は設計の仮のまま):
  - 孔は µm² で書き、1e-6 µm²(= 1 nm²)刻みの整数で持つ(上限 1e7 µm²)。小さい値は 10 進の文字列("1e-6"・"0.001")。
  - 粘りは 1 以上(0 は書けない。書かない = 粘りで減らない)・混ざる速さと吸い上げは 0 以上。
  - 「液体があるのに重力 0」は、世界の初めのセルに液体を置いた時だけ(物質の表に液体があるだけの部品のパッケージでは言わない)。
  - 「端が全部 wall で大気が無い(注意)」は、セルを置いた世界で、基準の大気もセルの気体も無い時だけ(閉じた真空の箱)。
    earth_air だけの組(空気で満たして基準の大気なし)は言わない。ブランクの世界も言わない。
  - 標準の earth_environment には world_edges を書いていない(既定の全部 wall のまま。世界ごとに書く物と見た)。
  - 見本の値: real_matter の水の粘り 890・湯気の混ざる速さ 25。砂は real_matter に無いので、場面 grass_fire_river の silica に
    孔 10 µm²・吸い上げ 5000 Pa(どれも出典はファイルの注。未確認)。
- T-0713(wt5)の相の表 `CvSpeciesPhase.phase` の並び(gas 0 / liquid 1 / solid 2)は `sim::SpeciesPhase` と同じ。マージ後は
  `MakeSpeciesPhases` が `speciesPhase` を写すだけにできる。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-17 作業役(wt2): 相を `SpeciesPhase` 1 本に(定義の condensed・liquid を消し、試験の C++ の表も直した)・動き方の 4 欄・
  world_edges を型・読み手・ベイクの検査へ。condensed は読み替えて警告。標準と場面を phase だけに。ベイクの知らせ(§1.4)。
  試験: reaction_package に condensed と phase の同じ表・標準の相の表・4 欄の形と誤り、environment_package に端と知らせ。
  main(t-0622・t-0713 を含む)へ rebase: RX_SPECIES_GAS の印を phase から作る・T-0713 の MakeSpeciesPhases を speciesPhase の写しに
  (並びが同じことを static_assert)・main で増えた試験の表(cell_view_test・reaction_limits_table.h)も phase へ。
