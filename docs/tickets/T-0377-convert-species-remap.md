# T-0377 物質を足す・消す反応表の差し替えで、転換のパッチの物質 ID を付け替える(T-0359 から分けた)

- Status: Doing
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: T-0359 と同じ(docs/plan/ROADMAP.md)
- 設計: docs/design/02-reaction-system.md §5.1 / ADR-0095(追記 4)・ADR-0065。関係: T-0223・T-0242・T-0266・T-0359・D-438・D-428

## 目的(1〜2 行)
物質の ID は名前のバイト順なので、物質を足す・消す表の差し替えで ID がずれる。魔法の転換(T-0359)の「替える先の物質 ID」を
新しい表の ID へ名前で付け替え、替える先が消えた転換は黙って捨てずに効かなくする(数える)。**値はすべて試験用の仮のもの(D-006)。**

## 完了条件(チェックできる形で)
- [x] 列の付け替え `MpRemapPatchList`・効き方の語の付け替え `MpRemapEffect`(common/magic_patch.hlsli。HLSL と C++ で同じ関数)。
      替える先が消えた(と、今の表の範囲の外の)転換は替える先 0 = 効かない(MpConvertedCoefficient が 0・MpEffectValid が断る)
- [x] GPU: 実験室の RemapLabSpecies・仮の世界の RemapSpecies のスレッド 0 が、セルの付け替えと同じ段で列を書き換える
- [x] CPU リファレンス: RemapLabBox・ProbeReference::RemapSpecies。効かなくなった数は SpeciesRemapReport::conversionsDisabled
      (この刻みに終わるパッチと、もう替える先の無い転換は数えない)
- [x] まだ当てていないパッチのコマンドも付け替える(実験室 RemapLabCommand・仮の世界 RemapPlaceCommand = 窓の筆の付け替え)。
      消えた物質への転換は当てる時に断る(仮の世界は断った数・イベントに出る)
- [ ] 試験: lab_box_test の TestMagicConvertRemap(CPU)/ gpu_lab_box_test の RunConvertRemap / gpu_probe_patch_test の TestRemapScene
      (どちらも毎刻み GPU と CPU がビット一致。仮の世界は保存点へ戻す・ばらばらの分け方・再生ファイル)
- [x] 02 §5.1・ADR-0095 追記 4

## メモ・参考
- 試験の表: `MakeConversionRemapBeforeTestTable`(転換の試験の表 + CO2 と同じ元素の仮の物質 carbon_dioxide_late_test)→
  `MakeConversionRemapAfterTestTable`(転換の試験の表 + 名前の順で一番前の仮の物質 a_marker_test。late は無い)。
  異性体の ID が 1 つ後ろへずれ、前の表の異性体の ID は後の表では CO2 を指す(付け替えないと「CO2 → CO2」になり異性体ができなくなる)。
- 仮の世界の GPU は、効かなくなった数をハッシュの表に出していない(付け替えの段は刻みの適用の単位〔BeginTick が欄を 0 にする〕の前)。
  効かなくなったことは列そのもの(替える先 0)に残り、状態の要約(MpListHash)で CPU と突き合わせる。数は CPU リファレンスの報告。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
