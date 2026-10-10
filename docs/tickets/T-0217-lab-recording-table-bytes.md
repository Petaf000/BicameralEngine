# T-0217 実験室の記録に表の中身を残す

- Status: Done(2026-10-10)
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 1 体・小さい(統合の確認と T-0220 と合わせて 2 時間の約束の中)
- マイルストーン: M2 (docs/plan/ROADMAP.md。T-0194 から分けた。T-0218 の後)
- 設計: docs/design/14-editor-tools.md §2「実装(T-0217)」/ 決定: D-439・ADR-0037・ADR-0050・ADR-0055

## 目的(1〜2 行)
実験室の記録(BLAB)に、使った反応表(刻み 0 と途中の印の表)の中身を残し、別の起動でも途中で表を替えた記録を再生できるようにする。

## 完了条件(チェックできる形で)
- [x] BLAB 版 4: ハッシュの列の後ろに表の中身(版 + script::TableBytes のバイト列)。版 1〜3 も読める・壊れた中身は断る
- [x] LabSession が表ごとに中身を持ち、Recording() が使った表の中身を入れる。AddTable で記録の表を足せる(物質の一覧が違えば断る)
- [x] パネルの「読んで再生」は、持っていない表を中身から作り直して(script::RebuildReactionTable)足してから再生する
- [x] lab_box・gpu_lab_box(HW・WARP)・window_lab(`--auto-lab` が本物の別の表に途中で替え、新しい箱で記録から再生)

## メモ・参考
- 実装: sim/lab_box(LabTableContent・LabRecording::tables・BLAB 版 4)・sim/lab_session(m_tableBytes・AddTable・HasTable)・
  editor/lab_panel(UseTable が LoadedReactionTable を受ける・AddRecordedTables・ReplayInNewSession・MakeAutoSwappedTable)・frame/frame_loop。
- sim は表の中身を読まない(script に依らない)。作り直すのはエディタ(再生ファイルと同じ確かめ方)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-10(作業役・main): BLAB 版 4・LabSession の表の中身・AddTable・パネルの作り直しと `--auto-lab` の新しい箱での再生を足した。
  debug のビルドは警告なし・lab_box・gpu_lab_box(80 秒)・window_lab(104 秒)が 1 回目で通過。gpu_lab_box_warp は T-0220 の後に流す。
