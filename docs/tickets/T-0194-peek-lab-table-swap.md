# T-0194 覗き窓と実験室の表も差し替える

- Status: Review(作業役・wt3・ブランチ t-0194。司令塔のマージ待ち)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(T-0195 と合わせて 2 時間の約束の中)
- マイルストーン: M2 (docs/plan/ROADMAP.md。T-0139 から分けた。T-0172 の中身を含む)
- 設計: docs/design/14-editor-tools.md §2「実装(T-0194・T-0195)」・13 §2 / 決定: ADR-0047・ADR-0037・ADR-0054(新)

## 目的(1〜2 行)
ホットリロードで世界の反応表を差し替えたら、覗き窓(T-0096)と実験室(T-0142)も同じ表を使う。実験室は試験の C++ の表への固定をやめ、
世界と同じパッケージの表を使う(T-0172)。世界の差し替えは今まで通り記録・再生で同じ刻みに起きる。

## 完了条件(チェックできる形で)
- [x] 表を持ち替える道具: GpuMultires::ReplaceTable(新しいアップロードのバッファ・前のバッファを返す)
- [x] 覗き窓: QueueTableSwap(s, 表) → s より後の境界を抽出する時に表を替え、影の鎖を作り直す。CPU リファレンスも同じ。
      前の表のバッファはそのフレームのリストの keepAlive(ProbeExtractContext::keepAlive を足した)。フレームのループが世界の差し替えごとに渡す
- [x] gpu_probe_peek の 3 回目: 刻み 4 の始めに表を替えて覗きながら 15 フレーム、毎フレーム入れ子と抽出が CPU とビット一致・替えない時と違う
- [x] 実験室: フレームのループが毎フレーム世界の表を渡す(LabPanel::UseTable)。替わったら LabSession::ChangeTable(操作を初めから新しい表で
      同じ刻みまで流し直す)。パネルに表の版と替えた回数
- [x] 実験室の記録 BLAB 版 2(表の版)。違う版の記録は再生しない。版 1 も読める(lab_box)
- [x] gpu_lab_box: 200 刻みの後に表を替える → 毎刻みビット一致・刻みと操作は同じ・最後のハッシュが違う・古い表の記録は断る・新しい記録は再生できる
- [x] window_hot_reload_*(T-0195)で、覗きながらの差し替えを窓で通した

## メモ・参考
- 実装: sim/gpu_multires(ReplaceTable)・sim/probe_peek(QueueTableSwap・ApplyDueTables・PeekState::TableChanged)・sim/probe_sim.h
  (ProbeExtractContext::keepAlive)・sim/gpu_lab_box(ReplaceTable)・sim/lab_session(ChangeTable・Create の tableVersion・Replay の版の確かめ)・
  sim/lab_box(BLAB 版 2)・editor/lab_panel(UseTable)・frame/frame_loop(TakeTableSwaps で覗き窓へ・BuildEditor で実験室へ)。
- 反応の核(reaction.hlsli)・反応表の GPU 用の形・読み手(reaction_package.cpp・luau_package.cpp)には触れていない(bicameral.d.luau も変えていない)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役・wt3・ブランチ t-0194): GpuMultires に表の持ち替えを足し、覗き窓は「表が替わったら影を作り直す」、実験室は
  「操作を初めから新しい表で流し直す」にした(ADR-0054)。実験室は世界の表を使う(T-0172 の内容)。debug のビルドは警告なし。
  debug で `^(reaction_hot_reload|lab_box|gpu_lab_box|window_lab|window_hot_reload_*)$` が通過。gpu_probe_peek は 3 回目(差し替え)の
  Multires のグラフの作成が並走の負荷で 170 秒かかり 300 秒の上限を超えた(覗く・覗かないの 2 回は CPU と一致)→ TIMEOUT 900 にして流し直した(下の結果)。

## 引き継ぎメモ(HANDOFF に載せる状態)
- 動いているもの: `--editor` で反応表を保存すると、世界・覗き窓(P)・実験室が同じ表になる。実験室は箱があれば同じ操作を新しい表で流し直す
  (パネル「実験室」に表の版と替えた回数・メッセージ)。確認: `-Filter "^(gpu_probe_peek|gpu_lab_box|lab_box)$"`。
- 壊れているもの: なし。
- 決めたこと(Claude・実装の細部): ADR-0054。
- 仮(ユーザー未確認): 下の「判断待ち」1。
- 注意: ProbeExtractContext に keepAlive を足した(フックが作ったバッファをそのフレームのリストが終わるまで持つ)。
  実験室はもう試験の C++ の表を使わない(lab_panel.cpp から reaction_test_table.h を外した)。T-0172 は ROADMAP で済みにしてよい。

## 判断待ち
1. **実験室で表が替わった時、どう替えるか**(仮で案 A で進めた・ユーザー未確認)
   - 案 A(今): 置いた操作を初めの箱から新しい表で同じ刻みまで流し直す。プレイヤーと遊びへの影響: 無し(エディタの道具)。反応表を書く人は
     「同じ置き方・同じ火の付け方で、法則だけ変えたらどうなるか」をすぐ比べられる。刻み t までは古い表、という実験はできない。
   - 案 B: 世界と同じく、今の刻みから新しい表で続ける(箱に表のコマンドを入れる。記録は複数の表を持つ)。影響: 「途中で法則が変わった時に
     世界がどう追いつくか」(魔法で反応則に介入した瞬間の様子に近い)を試せる。費用: 中(多重解像度の入れ子の RefreshTable〔GPU の段と CPU〕・BLAB に複数の表)。
   - 案 C: 箱を空に戻す。影響: 置き直しが要る。費用: 小。
   - おすすめ: A(取り消しやすい。LabSession::ChangeTable の 1 か所)。魔法の介入の見え方を試す段になったら B を足す(T-0218 として分けた)。

## 分けたもの(司令塔が ROADMAP に足すか決める)
- **T-0217 実験室の記録に表の中身を残す**: 今の BLAB 版 2 は表の版だけで、違う版の記録は再生を断る。再生ファイル(ADR-0050)と同じく
  TableBytes を残せば、表を書き換えた後でも前の実験を前の表で再生できる。
- **T-0218 実験室で刻みの途中に表を替える(判断待ち 1 の案 B)**: 多重解像度の入れ子の RefreshTable(GPU と CPU)・BLAB に複数の表。
