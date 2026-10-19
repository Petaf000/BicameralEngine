# T-0819 物質の欄「固体が動くか」と見立てのばらける固体の和

- Status: Done(2026-10-19。試験の結果は作業ログ)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ③' 段 9 の入口(T-0797 の前)(docs/plan/ROADMAP.md)
- 設計: docs/design/20 §7 / 決定: D-520・D-525(2026-10-19 ユーザー。ADR-0359・0360 の A〜D は全部案 1)・ADR-0359

## 目的(1〜2 行)
物質に `solid_motion`(fixed / loose。名前は仮)・`repose_angle_deg` を書けるようにし、相の表と見立てがばらける固体を数える。流れはまだ作らない(T-0797)。

## 完了条件
- [ ] 型(data/types/bicameral.d.luau)・読み込み・ベイク・ベイクの知らせ(ADR-0359 §1 の誤り 2・警告 2)
- [ ] 相の表 CvSpeciesPhase に「動く」1 bit・安息角(tan θ の Q16)の列。CPU の sim::MakeSpeciesPhases と GPU の表が同じ
- [ ] 見立てのばらける固体の和(物質量・質量・体積)と CV_VIEW_LOOSE。cell_view_test に足す
- [ ] `solid_motion` の無い表・ブランクの世界で今とビット一致(見立て・ハッシュ)
- [ ] release 警告 0・archmap --check

## 作業ログ
- 2026-10-19(作業役 wt6): D-525 の案 1 のとおり実装(相は 3 つのまま・欄は `solid_motion` + `repose_angle_deg`)。
  - 型 `data/types/bicameral.d.luau`・読み込み `script/reaction_package.cpp` の ReadSolidMotion(`"fixed"|"loose"` の外・0〜90 の外・小数は誤り)。
  - ベイク `sim/reaction_table.cpp`: 誤り 2(固体の外の 2 つの欄〔CheckPhaseFields の表に 2 行〕・`"loose"` に密度が無い)/
    警告 2(WarnSolidMotion: `"loose"` でない固体の安息角は効かない・`"loose"` の安息角の書き忘れは 35 度〔`DEFAULT_REPOSE_ANGLE_DEGREES`〕)。
    書いた値は `SpeciesTransport` の `solidMotion`・`reposeAngleDegrees`(書かない = 無し。反応の GPU の表には入らない)。
  - 相の表 `CvSpeciesPhase` に `motion`(CV_MOTION_FIXED / LOOSE。固体だけが LOOSE)・`reposeSlopeQ16`(tan θ の Q16。90° = `CV_REPOSE_VERTICAL`)。
    作るのは `sim::MakeSpeciesPhases` の 1 か所(tan は 0〜89 度の整数の表 REPOSE_SLOPE_Q16。シミュに浮動小数点を書かないため)。表は 16 → 24 バイト。
  - 見立て(`shaders/common/cell_view.hlsli`。CPU と GPU が同じ関数): `CvViewSums`・`CvCellView` に `loose`(CvPhaseSums。物質量・質量・体積 + 熱容量)。
    `CV_VIEW_LOOSE = 8` の時だけ足す(`CV_VIEW_ALL` は 15)。`solid` は今どおり固体の全部(据わる + ばらける)。ばらける物質が無ければ `loose` は 0 で、ほかの欄は同じ式。
  - 試験: cell_view_test の 5(ばらける砂と氷の表で 3003 セル × 4 レベル: 和が手で数えた値・固体の和の一部・ばらけるを据わるに変えた表とほかの欄が全部同じ・
    部分の作り方に CV_VIEW_LOOSE)。reaction_package_test の TestSolidMotionRead・TestSolidMotionChecks(読んだ形・相の表・"fixed" と書く = 書かない〔表・相の表がバイトで同じ〕・
    標準パッケージは全部据わる・警告 2・誤り 6)。reaction_package_test に bicameral_multires をリンク(相の表を作るため)。
  - 仮(ユーザー未確認・取り消しやすい): ① ADR の「相に 1 bit」を、相の値に混ぜず `CvSpeciesPhase` の別の欄 `motion` にした(相の値 `phase == CV_PHASE_SOLID` を見る所が
    ばらける固体を取りこぼさないため。作り手から見た違いは無い)。② ばらける固体の和は 3 値でなく熱容量も入れた 4 値(固体の流れが熱の分け前を運ぶ時に読む。T-0797)。
    ③ 安息角の書き忘れの既定 35 度(ADR-0359 §1 の仮のまま)。
  - 分けたもの: なし(固体の流れは T-0797。物の重さを中身から数える C は T-0821)。
