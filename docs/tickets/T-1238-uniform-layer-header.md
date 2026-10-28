# T-1238 一様の層の置き場の頭(全部の枠に 48 B)を縮める

- Status: Todo(T-1220 から分けた)
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
