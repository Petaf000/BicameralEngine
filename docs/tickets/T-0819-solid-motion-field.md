# T-0819 物質の欄「固体が動くか」と見立てのばらける固体の和

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ③' 段 9 の入口(T-0797 の前)(docs/plan/ROADMAP.md)
- 設計: docs/design/20 §7 / 決定: D-520・ADR-0359(Proposed。判断待ち A の答えで欄の名前を合わせる)

## 目的(1〜2 行)
物質に `solid_motion`(fixed / loose。名前は仮)・`repose_angle_deg` を書けるようにし、相の表と見立てがばらける固体を数える。流れはまだ作らない(T-0797)。

## 完了条件
- [ ] 型(data/types/bicameral.d.luau)・読み込み・ベイク・ベイクの知らせ(ADR-0359 §1 の誤り 2・警告 2)
- [ ] 相の表 CvSpeciesPhase に「動く」1 bit・安息角(tan θ の Q16)の列。CPU の sim::MakeSpeciesPhases と GPU の表が同じ
- [ ] 見立てのばらける固体の和(物質量・質量・体積)と CV_VIEW_LOOSE。cell_view_test に足す
- [ ] `solid_motion` の無い表・ブランクの世界で今とビット一致(見立て・ハッシュ)
- [ ] release 警告 0・archmap --check

## 作業ログ
