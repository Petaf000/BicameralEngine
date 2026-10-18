# T-0768 当てる段の面の回しの費用を置き換える前に戻す(MR の CPU の伝導。T-0767 の残り)

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ROADMAP「③' 運ぶ計算の共通の土台」段 1(T-0714 の後始末)
- 設計: 20 §2.4 / 決定: ADR-0333・ADR-0334(Proposed)・D-468

## 目的(1〜2 行)
T-0767 で見立ての手間は MrCellThermal と同じに戻したが、StepConduction の時間は置き換える前(T-0714 の前)にまだ戻っていない(perf.md 2026-10-17 T-0767 の行)。
値を変えずに(ビット一致)、面の回し(印 MarkConductionWants と流れ ComputeConduction)の費用を置き換える前と同じかそれ以下にする。

## 完了条件(チェックできる形で)
- [ ] StepConduction の時間が置き換える前と同じかそれ以下(同じジョブで交互に 2 回。下の測り方)
- [ ] CPU の MR の試験 10 本・cell_view・transport_apply が通る(ビット一致)
- [ ] 当てる段・種類・世界の引数の形は変えない(並走の種類が呼んでいる。変えるなら重なりをここに)
- [ ] release 警告 0・archmap --check・perf.md

## 残りの見込み(T-0767 で測った。未確認の所は未確認)
1. **置き場の大きさ**: CellViews は 1 セル 192 バイト(CvCellView)を 513 個 / 枠で持つ(前の MrThermal は 16 バイト)。z の隣(64 セル先 = 12 KiB 先)と
   枠どうしの隣が 1 次キャッシュから外れる見込み(未確認)。案: 種類の部分だけを詰めて持つ(熱だけなら heat・heatCapacity・thermal・faceLimit の 40 バイト)、
   世界の View は値で CvCellView を組んで返す(インラインで読まない欄は消える)/ 見立ての欄の並びを熱を先頭にする。GPU は置き場を持たない(T-0766)ので CPU だけの話。
2. **自分の側の坂と係数を面ごとに作り直す**: TxFaceFlow が面ごとに kind.Slope(self)・SideCoefficient(self)・SubcycleShift(level) を呼ぶ(前はセルごとに 1 回)。
   案: TxFlowCell の中で面の前に 1 回だけ作り、面の式だけを面ごとに(TxFaceFlow の中身を分け、TxFaceFlow は今の引数のまま包む)。GPU でも ALU が減る。
3. 世界と種類を値で渡す(C++ で 64 バイトの写し。HLSL は同じ)。並走の世界のメンバー関数が const か分からないので const& にしていない。
4. (前から)印と流れで同じ面を 2 回流す(MarkCellWants と TxFlowCell)。前の形も同じなので戻す目当てには入れない。

## 測り方(T-0767 で使った。仮の計りはコミットしない)
- wt のランナーはほかの作業ツリーと並走で、同じ実行ファイルの時間が 2 倍以上揺れる。ctest の時間では比べられない。
- multires_nest.cpp の `deltas = StepConduction(...)` を steady_clock で囲んで終わりに stderr へ出す仮の計りを、比べる版ごとにビルドし、
  out\build\release\bin に `<試験>_<版>.exe` として写し、1 つの raw のジョブで版を交互に 2 回流して小さい方を取る(版の順を回ごとに逆にする)。
- 段ごとの内訳は multires_conduction.cpp の StepConduction の各段と CellViews::View(作る回数・時間・呼ぶ回数)を同じく囲む。
- 見立ての手間は cell_view_test の費用の行(同じ過程で続けて測るので揺れにくい)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
