# T-1238 一様の層の置き場の頭(全部の枠に 48 B)を縮める

- Status: Doing(2026-10-29 wt3。WIP: 書いた・release のビルドは未確認・試験は未)
- 種類: 工学
- PC: 必須(CPU)
- 見積もり: チャット 0.5 回分
- マイルストーン: 土台の変更要求の束(ADR-0469 の 2-2 の続き)
- 設計: docs/design/20 §10.5 / 決定: ADR-0489 の 1 / 前: T-1220

## 目的(1〜2 行)
`MultiresCellStore::uniformLayers`(枠ごとに vector 2 つ)は、層の無い世界でも全部の枠に 48 B かかる。
枠ごとの番号(4 B)+ 層のある枠だけの組の表に替え、層の無い世界の費用を 4 B × 枠にする。

## 完了条件(チェックできる形で)
- [ ] 枠ごとの番号 + 組の表(層のある枠だけ)。読む・書く・消す口(LoadUniformLayers・SetUniformLayers・ClearUniformLayers)は今の形のまま
- [ ] 要約・保存点・検査(HashUniformLayers・CountLayerMisfits)は今とビット単位で同じ(multires_layers・multires_state)
- [ ] 枠のバイトを docs/perf.md に(前後)

## メモ・参考
- 置き場: engine/src/sim/multires_nest.h の MultiresUniformLayers・engine/src/sim/multires_cell_store.cpp。
- GPU に一様の枠の層を写す時(T-1240)の並びもこの形に合わせると写しが簡単になる。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29(wt3・T-1221 の作業役): T-1220 の「分けたもの 3」からファイルを作った。
- 2026-10-29(wt3・T-1241 の作業役。入れ物の再起動で途中で止めた): `MultiresUniformLayerStore`(multires_nest.h。枠ごとの番号 entryOfSlot 4 B +
  層のある枠だけの組 entries + 空いた組 freeEntries。Find / Empty / Reset / Set / Clear)に替え、使う所(multires_cell_store.cpp・multires_nest.cpp の要約と作る所・
  multires_tree.cpp の細かくする所 1 か所・gpu_multires_layers.cpp の断る検査)を直した。読む・書く・消す口(LoadUniformLayers・SetUniformLayers・ClearUniformLayers)の形は同じ。
  試験 multires_layers に TestUniformLayerStore(組の持ち方・使い回し・費用のログ)を足した。

## 引き継ぎ(2026-10-29)
- 状態: 1 回目の release ビルドは C4267(multires_nest.cpp:721 の Reset に size_t)だけで落ちた → Reset を size_t に直して 2 回目(runner/logs/20261028-232720-build)を投げたところで止めた(結果は未確認)。
- 次の一手: (1) release ビルド・警告 0 を確かめる (2) `test -Filter '^(multires_layers|multires_state|multires_sparse|tree_refine_placement|world_layout)$'`
  (要約・保存点がビット単位で同じなことは既存の試験が見る) (3) 試験のログの「一様の枠の層の置き場 層の無い枠 1 つ 4 B(前は 48 B)」を docs/perf.md に 1 行
  (4) archmap --check (5) 完了なら Status: Done・`git rebase main`。
- 組の並び(空いた組の使い回し)は要約・保存点に入らない(どれも枠の順に Find で引く)。wt2(T-1188)との重なりは multires_tree.cpp の細かくする所の 3 行だけ(wt2 は粗くする所)。
