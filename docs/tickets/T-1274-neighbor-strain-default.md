# T-1274 支えの混ざった細かさの対の既定を形 A(NeighborStrain)にする

- Status: Doing(本体。t-1274-neighbor-strain-default)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列 3(docs/plan/ROADMAP.md)
- 設計: ADR-0509(Accepted 2026-11-12)/ 決定: D-685(Q182)

## 目的(1〜2 行)
細かさの違うセルが接する所の対で、粗いセルの側の歪みを隣の値から一様な歪みとして読む形 A を既定にする(縁の力の偽の差を減らす)。

## 完了条件(チェックできる形で)
- [ ] support_law.h の既定を MixedPairMaterial::NeighborStrain に
- [ ] ctest の support_mixed_neighbor_strain の扱い(既定と同じになるので --pair 0〔None との比べ〕にするか消す)
- [ ] ハッシュ/期待の取り直し: support 系・warm_start_save・gpu_equilibrium_block(HW・WARP)
- [ ] release ビルド警告 0・archmap --check
- [ ] CPU と GPU のビット一致は今まで通り(T-1279)

## メモ・参考
- 作り手から見た違い(D-685): 根の線の縁 4.7 → 2.6 %・洞窟の天井 3.42 → 1.23 %・両端固定 4.47 → 2.20 %。違う大きさのセルを持つ場面の値とハッシュが変わる。
  GPU の 1 回の解き 1.955 → 2.232 ms(+14 %・洞窟の天井 1288 節)。片持ち梁の下りる道 93 → 94 kN/m・洞窟の縦の割れ 7 → 5 面(T-1279 §4)。
- ADR-0509 の追記: 既定を仮に形 A にして支えの CPU の試験 11 本(support_*・warm_start_save)は合格・試験は既定に付いてくる形。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-11-12 本体: ★(claude/48)の反映でチケットを作った。
- 2026-11-12 本体: support_law.h の SupportOptions::mixedPairMaterial の既定を NeighborStrain に(2 次元・3 次元〔support_law_3d_mixed〕とも同じ欄を読む)。
  ctest support_mixed_neighbor_strain(--pair 3)は既定と同じになったので、前の既定の比べ support_mixed_pair_none(--pair 0。根の線の 2 場面は 6 % の線)に置き換えた。
  記録したハッシュを持つ試験は無かった(support 系・warm_start_save・gpu_equilibrium_block は既定に付いてくる形・gpu_equilibrium_block は None と形 A を両方流す)。
