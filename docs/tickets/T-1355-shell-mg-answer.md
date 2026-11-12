# T-1355 層の流れの圧力の答えを「多重格子を残差の線まで回して丸めた値」にする(CPU 既定 + GPU 作り直し)

- Status: Doing(wt3。2026-11-12。ブランチ t-1355-shell-mg-answer)
- 種類: 工学(ユーザー決定 2026-11-12: Q188 → 案 A を GPU へ。研究は T-1343 で済み)
- PC: 必須
- 見積もり: チャット 1 回(終わらなければ段に分ける)
- マイルストーン: 試作の列 1(docs/plan/ROADMAP.md)
- 設計: T-1343(候補の比べ)・T-1321(分かった根)・T-1349(上限に当たる)・T-1350(段ごとの Dispatch)/ 決定: Q188 → 案 A(D-677 の「収まるまで解く」を置き換える。D の番号は本体)・D-428

## 目的(1〜2 行)
物のまわりの空気の層の流れ(object_shell_flow)の圧力の答えを、多重格子の V 字を残差が Ψ の半分以下(線 7)になるまで回して決まった規則で丸めた値にする。
整数の赤黒の動かない点へ寄せる仕上げをやめる(決まった回数で止める形には戻さない)。CPU リファレンスを既定にし、GPU を同じ答えに作り直す。

## 完了条件(チェックできる形で)
- [ ] CPU リファレンス(tests/object_flow_cpu.h の T-1343 の口 = pressureFinish = 1)を既定の答えに
- [ ] GPU の MgSolveStart(shaders/sim/object_shell_flow.hlsl)を同じ答えに作り直す(MgSolveToLine)
- [ ] CPU と GPU が HW と WARP でビット一致(gpu_object_shell_flow・gpu_object_shell_flow_warp・gpu_object_shell_flow_large)
- [ ] 時間の測り(今の約半分の見込み・箱の大きさと層の数で増えない・上限に当たらない。docs/perf.md に日付と GPU)
- [ ] 層の流れのある場面のハッシュの取り直し(理由を書く)
- [ ] 丸めの偏りの確かめ(燃え広がりの速さの変化と、偏りが系統的か = 丸めの向き)
- [ ] release 警告 0・object_shell・float_check 系・dxil_alloca_check・archmap --check
- [ ] T-1350 の Dispatch を分ける閾値(2^25)が要るかを測って書く

## メモ・参考
- 決まった答え: 0 から細かい単位(Ψ × 2^8)の V 字を「どのボクセルの残差 |右辺 + 隣の和 − 重みの和 × 値| も 重みの和 × 2^7 以下」まで(上限 32 回・当たったら数える)→ 0 から遠い方へ四捨五入。
  回数は状態だけで決まる(残差の比べは全部のボクセルの OR)。CPU の PressureMultigridToLine と GPU の MgSolveToLine が同じ式。
- 数え上げ: 線までの答えでは「赤黒の和・一番多い・上限で止まった」の欄に V 字の回数・一番多い回数・上限 32 に当たった数を入れる(CPU と GPU で同じ。比べの試験が見る)。
- 比べ・調べるだけに残したもの: 整数の赤黒の規則(0 から決まった回数・0 から収まるまで・温めた始まり。CPU の pressureFinish = 0、GPU の mode 1・2・3)。
  外したもの: GPU の「多重格子の解き始め + 赤黒を収まるまで」(mode 5。MgSolveStart・MgCenterRhs)と T-1321 の調べの口(g_pressureMode の 8〜11 bit・--mg-extra)。
  CPU の PressureMultigrid(①②③)・pressureMultigridCycles・(i')(ii) は object_shell_test --case の調べの口として残した(要らなければ外してよい)。
- object_shell_test --case の 18 番目の引数: 省くと既定(線 7)。今までの「今の規則」を流すには 18 番目に 0 を書く(T-1343 の測りの書き方は 0 = 今だった)。
- object_flow_cpu.h の冒頭の説明(上限に依る書き方)はこのブランチで書き直した(T-1357 の分。本体の T-1357 の行はこのブランチで済み)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
