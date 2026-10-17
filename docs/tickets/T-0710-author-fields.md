# T-0710 作り手の欄の型(土台の段 0)

- Status: Doing
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
- [ ] 型(data/types/bicameral.d.luau)・読み手(reaction_package・environment_package)・ベイクの検査(reaction_table)
- [ ] `condensed` を読み替えて警告・`condensed` と `phase` の食い違いは誤り・標準パッケージと場面を `phase` だけに
- [ ] `condensed` で書いた表と `phase` で書いた表のベイクの結果が同じ(GPU の表・相の表)
- [ ] ベイクの知らせ(§1.4)
- [ ] 試験: luau_type_check・reaction_package・standard_packages・environment_package・world_layout が通る・release のビルドの警告 0・archmap --check

## メモ・参考

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
