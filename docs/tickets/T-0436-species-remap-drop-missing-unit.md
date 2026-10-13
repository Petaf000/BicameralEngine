# T-0436 単体が表に無い元素を含む物質を消す表も当てる(原子は捨てて報告)

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分より小さい
- マイルストーン: D-471 ② 中身の器(docs/plan/ROADMAP.md)
- 設計: docs/design/02-reaction-system.md(物質を足す・消す表の差し替え)/ 決定: D-484(Q34-3 の案 C)・D-473・ADR-0065(追記 2026-10-13)

## 目的(1〜2 行)
今は単体(その元素だけでできた物質)が新しい表に無い元素を含む物質を消す表を断っていた。D-484 に従い、断らずに当て、その元素の原子は世界から捨てて
改造による出入りとして報告する(端数と同じ扱い)。環境ミキサーでパッケージを外すのが 1 回で済むようにする。

## 完了条件(チェックできる形で)
- [x] BuildSpeciesRemap が断らない(捨てる「物質 の 元素」を SpeciesRemap::dropped に返す)。分けた先の species が RX_REMAP_DROPPED(0)の元素を捨てる
- [x] RxRemapCell(CPU と GPU の共通の関数)が捨てた原子を数える(RxRemapResult::droppedAtoms → SpeciesRemapReport::droppedAtomMicromoles)。熱は保つ
- [x] 保存則の検査(帳簿込み): 元素ごとの原子が「捨てた分・端数」込みで合う(reaction_species_remap の TestMissingUnit)
- [x] GPU の世界の付け替えが CPU と毎刻みビット一致(gpu_probe_sim の「水素なし〔H を捨てる〕」の版)
- [x] 実験室の報告(ログ)に捨てた原子を出す・世界の差し替え(ProbeSim)は捨てる元素を警告で知らせる。ホットリロードの検査(Remap)が通す

## メモ・参考
- GPU のバッファの形(PackSpeciesRemap)は変えていない(分けた先の species が 0 の行が増えるだけ)。単体の一覧(units)に 0 は入らない。
- 再生は表の中身から同じ付け替えを作り直すので、記録と再生で同じ(新しいコマンドは要らない)。
- BuildSpeciesRemap は今は失敗しないが、std::expected の形は呼ぶ側の約束として残した。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-13(司令塔のチャット・本体): species_remap.hlsli に RX_REMAP_DROPPED・RxRemapDroppedAtoms、species_remap の報告に droppedAtomMicromoles・dropped。
  reaction_species_remap の TestMissingUnit を「断る」から「捨てて報告・帳簿込みで合う・熱を保つ」に書き直し、gpu_probe_sim の物質の付け替えに水素なしの版を足した。
  release: reaction_species_remap・reaction_replay_table・reaction_hot_reload・lab_box・gpu_lab_box・gpu_probe_sim が通過(警告 0)。
