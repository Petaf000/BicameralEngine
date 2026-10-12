# T-0378 実験室の転換の保存点の再生の試験(T-0359 から分けた)

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(小さい。試験だけ)
- マイルストーン: T-0359 と同じ(docs/plan/ROADMAP.md)
- 設計: docs/design/02-reaction-system.md §5 / ADR-0095。関係: T-0287・T-0359・T-0377

## 目的(1〜2 行)
実験室で魔法の転換(T-0359)を当てている途中の保存点と記録全体を、別の箱で初めから再生して、毎刻み GPU と CPU がビット一致し、
ハッシュが記録と同じになることを確かめる(仮の世界の保存点・巻き戻しは gpu_probe_patch_test で転換込みで確かめ済み)。

## 完了条件(チェックできる形で)
- [x] gpu_lab_box_test の RunConvert の最後に ReplayPatchRecording(表の差し替え無し。`swap.table != nullptr` の時だけ AddTable)
- [x] gpu_lab_box(HW)で、保存点(刻み 30 = 転換の途中)と記録全体の再生が毎刻みビット一致・ハッシュ一致

## メモ・参考
- 値はすべて試験用の仮のもの(D-006)。表は sim::MakeConversionTestTable。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-12(作業役・wt3): ReplayPatchRecording を表の差し替え無しでも使えるようにし(`swap.table` がある時だけ AddTable)、
  RunConvert の最後で呼ぶ形にした。ビルドは素直に通った(警告 0)。gpu_lab_box(HW・debug)通過(607 秒。足す前は 526 秒)。

## テストの結果
- 2026-10-12(debug・HW・main e6701b1 の上 + T-0377 の [527] の直し): gpu_lab_box 通過(転換の保存点と記録全体の再生が毎刻みビット一致・
  ハッシュ一致・PassesValidation 通過)。
