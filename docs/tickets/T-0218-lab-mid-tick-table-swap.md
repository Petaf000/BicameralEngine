# T-0218 実験室で刻みの途中に表を替える

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 1 体・2 時間の約束の中(統合の確認と合わせて)
- マイルストーン: M2 (docs/plan/ROADMAP.md。T-0194 から分けた。D-439 = Q29 の案 B)
- 設計: docs/design/14-editor-tools.md §2「実装(T-0218)」/ 決定: D-439・ADR-0055(新。ADR-0054 の 3 を置き換え)・ADR-0018・ADR-0037

## 目的(1〜2 行)
ホットリロードで表が替わったら、実験室の箱は今の状態から次の刻みに新しい表で続ける(初めから流し直さない)。
途中で表を替えても、GPU と CPU の刻みごとのビット一致・記録と再生が成り立つ。

## 完了条件(チェックできる形で)
- [x] 表を替えた印のコマンド LAB_COMMAND_TYPE_TABLE(版を持つ)。印のある刻みはコマンドを当てる前に GPU と CPU の表を替え、印は箱をつつく
      (lab_box.hlsl と ApplyLabCommands が同じ。LabCommandMarksTable)
- [x] LabSession::ChangeTable は次の刻みに印を置く(刻む前の替え直しは最後の 1 つ・版 0 は断る)。見た表を版ごとに持つ。Reset は最新の表
- [x] 記録 BLAB 版 3(刻み 0 の表の版 + 印)。版 1・2 も読める。持っている表なら途中で替えた記録も再生・持たない版は流す前に断る・
      再生を終えて最新の表と違えば戻す印を置く
- [x] 初めから流し直す(T-0194 の案 A)は RerunWithLatestTable とパネルのボタン「最新の表で初めから」に残す
- [x] lab_box: 印はセルを変えずにつつく・CPU だけで刻み 100 に替える → 前は同じ・後は違う・燃え方が速い・版 3 と 2 の読み書き
- [x] gpu_lab_box(HW・WARP): 刻み 200 で替えて 100 刻み毎刻みビット一致・替える前の刻みは同じ・替えない時(CPU)と違う・記録の再生・
      古い表だけの記録の再生と戻す印・持たない表は断る・案 A も一致
- [x] window_lab: `--auto-lab` が火を付けた刻みに同じ中身の別の版へ替えて、一致と再生を通す

## メモ・参考
- 実装: common/lab_box.hlsli(LAB_COMMAND_TYPE_TABLE・LabCommandMarksTable)・sim/lab_box.hlsl・sim/lab_box(MakeLabTableCommand・
  LabTableVersionOf・BLAB 版 3)・sim/lab_session(ChangeTable・SwitchTable・ResetTo・RerunWithLatestTable・m_tables)・editor/lab_panel
  (UseTable の文・待っている差し替えの表示・ボタン・--auto-lab)。editor_overlay には触れていない。
- 入れ子の表に依る状態は wakeTick だけ(温度はエネルギーから毎回)なので、RefreshTable の段を作らずにつつくだけで足りる(ADR-0055)。
- T-0217 との境: 表の中身はメモリだけ(同じ起動の中なら再生できる)。ファイルに中身を残して別の起動で再生するのは T-0217(ROADMAP に書いた)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-10(作業役・main): 先に main(e35c72d)の統合を確かめた(release・debug のビルドは警告なし・release の 1 つ目の束 47 本とマージが触った 25 本が通過)。
  表を替えた印のコマンドで、印のある刻みの始めに GPU と CPU の表を替えて箱をつつく形にした(ADR-0055。入れ子の RefreshTable の段は要らなかった)。
  BLAB 版 3・版ごとの表・持たない版は断る・案 A はボタンに残す。lab_box_test(CPU)と gpu_lab_box_test(HW・WARP)に途中の差し替えを足し、--auto-lab も通す。
  変更の後: release・debug のビルドは警告なし。release の lab_box・gpu_lab_box(_warp)・window_lab・float_check 9 本が通過。debug の lab_box・gpu_lab_box(_warp)・window_lab 4 本が通過。 かかった時間 約 1 時間 50 分(統合の確認の待ち 約 55 分を含む)。
