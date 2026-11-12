# T-1350 層の流れの 1 刻みを段ごとの Dispatch に分ける(1 つの Dispatch が箱 × 回数で伸びて TDR に当たる不具合)

- Status: Doing(担当 wt5。2026-11-12)
- 種類: 工学(不具合。D-468)。結果は変えない(CPU とビット一致・今の試験のハッシュが変わらない)
- PC: 必須
- 見積もり: チャット 1 回
- マイルストーン: 試作の列 1(docs/plan/ROADMAP.md)
- 設計: docs/design/20 §12.5 / 決定: D-468・D-428・D-694 / 前: T-1349 の ②(切り分け)・T-1331・T-1223

## 目的(1〜2 行)
層の流れ(物 1 つ = 1 グループ)は 1 刻みを 1 つの Dispatch の中で回すので、1 つの Dispatch の時間が「箱のボクセル × 赤黒の回数」で伸び、
12 万ボクセルの箱が収まるまでで上限に当たると約 3.6 s になって Windows の TDR(2 s)で GPU がリセットされた(T-1349 の ②)。1 つの Dispatch が箱と回数で伸びない形にする。

## 完了条件(チェックできる形で)
- [ ] 1 刻みを段ごとの Dispatch に分けた形(結果は変えない)。1 つの Dispatch の形と同じ段の関数を同じ順で呼ぶ
- [ ] CPU とビット一致(gpu_object_shell_flow HW / WARP)・分けた形のハッシュが 1 つの Dispatch の形と同じ・今の試験のハッシュが変わらない
- [ ] 12 万ボクセルの箱を 0 から決まった 1,024 回(上限)で回して止まらない試験(gpu_object_shell_flow_large。わざと TDR を起こす確かめはしない)
- [ ] 分けた分の費用を 46 個・1000 個で測って docs/perf.md とここに

## 分け方の芯(2026-11-12 wt5)
- **1 刻み = 小刻みごとに 〔⓪〜④・解き始め〕→〔赤黒を続きから最大 n 回〕× 足りる数 →〔⑥〜⑨(最後の小刻みは ⑩ と数え上げも)〕の Dispatch**(ルート定数 g_split の段・小刻み・n)。
  どの段も今の 1 つの Dispatch の形と同じ関数(SubstepPrepare・PressureSweeps・SubstepFinish)を呼ぶ。
- **段をまたぐ状態は作業場(UAV)か、段の初めに作り直せる groupshared だけ**:
  セルの見立て(⓪。g_cellViews は小刻みの ⑨ まで変わらないので段ごとに読み直す)・赤黒の「汚れた」印(全部を計算し直す = 印で飛ばすのとビット単位で同じ。T-1202)・
  圧力の値(groupshared の形なら段の初めに作業場から読み、終わりに書く。32bit から写すので値は同じ)。
  赤黒の続きの回数・収まったか・失敗の印は g_stats の数え上げの後ろに物ごとに 3 語(作業場。ルート署名は 64 語でいっぱいなので UAV を増やさない)。
- **n は 1 グループが 1 つの Dispatch で回すボクセル × 赤黒の回数を 2^25(約 0.24 s。7.1 ns/ボクセル/回)に収める**(n = 2^25 ÷ 一番大きい箱)。
  赤黒の Dispatch の数は 上限 ÷ n(収まった物はすぐ返る)。
- **どちらの形にするかは自動**: 一番大きい箱 × 上限 × 小刻み 4 が 2^25 を超えれば分ける。groupshared に載る箱(7,168 ボクセル)× 1,024 × 4 はちょうど収まる
  ので、今の場面(紙 24 列・A4 の層 4・6 段)は 1 つの Dispatch のまま(費用もハッシュも変わらない)。
- **TDR の時計が見るのは 1 つのグループの長さ**(T-1349 で、決まった回数 480 回 = 1 つの Dispatch 1.7 s は通り、収まるまでの 3.6 s で止まった。
  グループの境で横取りできる GPU では、物の数が多い Dispatch は短いグループの列なので長くても止まらない見込み。未確認)。だから n は物の数ではなく一番大きい箱で決める。
- T-1331 の共有の格子を複数のグループで回す形(1 つの箱を複数のグループに分ける)も、段ごとの Dispatch の上に載る(赤黒の Dispatch をグループの数だけ広げる)。
  ⓪〜④・⑥〜⑨ の Dispatch は今も 1 グループが箱全部を回すので箱に比例して伸びる(12 万ボクセルで 1 小刻み約 14 ms。2 s に届くのは約 1,700 万ボクセル)。それは T-1331 の形で解く。

## 測り(2026-11-12 wt5。RTX 3070 Ti・release)
(下に書く)

## メモ・参考
- 触った関数(並走の wt3 T-1321 と同じ object_shell_flow.hlsl): OhShellFlowTick を FlowWholeTick・FlowSplitStage と段の関数(SubstepPrepare・PressureSweeps・SubstepFinish・
  AddSweepStats・AddObjectStats・AddFailures)に分けた。段そのもの(PrepareVoxels〜WakeChanged・Mg*)は触っていない。T-1321 が MgSolveStart・MgCycle を変えても、呼ぶ所は SubstepPrepare の 1 か所。
- 試験: gpu_object_shell_flow(比べに分けた形 n = 7 を足した: 4 つの規則 × groupshared / 作業場 + 3 個で 1 セル。1 つの Dispatch の形とハッシュが同じ)・
  gpu_object_shell_flow_large(新・HW だけ)・測りは `--split auto|whole|split`・`--split-sweeps N`。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
