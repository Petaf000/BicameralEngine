# T-0455 ctest から起動したプロセスがドライバのシェーダーのキャッシュに当たらない原因を調べる(T-0427 から分けた)

- Status: Done
- 種類: 研究
- PC: 必須
- 見積もり: チャット 1 回分
- 設計: D-460(GPU の順番待ち)・D-468(負荷で遅れることは不具合)。関係: T-0427・T-0426

## 目的
T-0427 で、ctest から起動した GPU の試験のプロセスは NVIDIA のドライバのシェーダーのキャッシュ(%LOCALAPPDATA%\NVIDIA\DXCache)に
当たらず、パイプラインを作るたびに全部コンパイルし直すことが分かった(debug の多重解像度 1 回 約 65 秒。`job.py run` で起動すると
2 回目から 1.6 秒)。作業ディレクトリ・環境変数(LOCALAPPDATA)・対話型・セッションは同じ。原因を突き止め、ctest でもキャッシュに当てる。
ゲームの本番の起動(エディタ・exe)では当たっているかも確かめる(当たらないなら起動のたびに数十秒〜分の待ちになる)。

## 試すこと(順に)
1. ctest の代わりに、同じ exe を PowerShell の Start-Process・cmd /c・`cmake -E env` から起動して比べる(どの起動の仕方で外れるか)。
2. ctest(libuv)が子プロセスに付けるもの(ジョブオブジェクト・CREATE_NO_WINDOW などのフラグ・SetErrorMode・標準入出力)を 1 つずつ真似る
   小さい起動器を raw で作り、どれで外れるかを見る。
3. ドライバのキャッシュの設定(NVIDIA コントロールパネルの「シェーダーキャッシュのサイズ」。23 GB 使っている)を確かめる(ユーザーに頼む)。

## 打ち切り条件
1〜3 で分からなければ、多重解像度の試験(uniform・quiet・conduction・activity・implicit)にも gpu::PipelineCache を足して終える
(T-0427 の gpu_multires_test と同じ。比べ方は変えない)。

## 測る候補と打ち切り条件(2026-10-14 の作業役。着手前に書いた)
同じ release の exe・同じ引数(`gpu_multires_test --queue compute --part wide-uniform --no-gbv`)で、起動の仕方だけを変えて
「Compute のパイプラインの作成」の秒(当たり 1〜2 秒・外れ 50 秒前後)を比べる。1 本 1 分以内・GPU の順番待ちに入って流す。
- 候補 1: 環境変数。test.ps1 だけが VS の開発者環境(_vsenv.ps1 の Enter-VsDevShell。PATH・INCLUDE・LIB など)を読み込み、run.ps1 は読み込まない。
  「VS の環境 + exe を直接」と「VS の環境なし + ctest」を測り、効くのが環境なら変数を半分ずつ戻して 1 つに絞る。
- 候補 2: ctest(libuv)の子プロセスの起動(ジョブオブジェクト・CREATE_NO_WINDOW・標準入出力がパイプ)。Start-Process・cmd /c・`cmake -E env` と比べる。
- 候補 3: 読み込まれる DLL の違い(PATH で D3D12 の SDK レイヤー・dxil などが別の場所から読まれる)。exe の中から読み込んだモジュールの一覧を出して比べる。
- 候補 4: D3D12 の ShaderCache の API(ID3D12Device9::ShaderCacheControl・アプリのキャッシュのセッション)で、外れる起動でもドライバのキャッシュを有効にできるか。
- 打ち切り: 候補 1〜4 を 1 回ずつ測って(GPU の試験 8 本・2 時間まで)原因の起動の仕方が分からなければ、アプリ側で逃げる
  (多重解像度の試験の uniform・quiet・conduction・activity・implicit にも gpu::PipelineCache を足す。CPU リファレンスとの比べ方は変えない)。
  原因が分かっても、ドライバの中の判断で直せないなら、試験の起動の側(test.ps1 など)で避ける。

## 分かったこと(2026-10-14。RTX 3070 Ti / 32.0.15.9597。release の `gpu_multires_test --queue compute --part wide-uniform --no-gbv`)
- **原因は ctest ではなく、test.ps1 が読み込む VS の開発者環境**(_vsenv.ps1 の Enter-VsDevShell)。run.ps1 は読み込まない。
  Enter-VsDevShell は cmd で VsDevCmd.bat を動かした後の環境を写す時に、**ASCII の外の文字を含む値を化けさせる**
  (この PC では USERPROFILE・LOCALAPPDATA・APPDATA・USERNAME・HOMEPATH・OneDrive・PSModulePath などと、Path の 74 個のうち 23 個)。
  化けた環境から起動したプロセスでは、ドライバがシェーダーのキャッシュを読みも書きもしない(同じプロセスの 2 回目も作り直す)。
  | 起動 | Compute のパイプラインの作成(1 場面目) |
  |---|---|
  | exe を直接(VS の環境なし。= run) | 0.27〜0.30 s |
  | ctest(VS の環境なし) | 0.27 s(**ctest でも当たる**) |
  | exe を直接(Enter-VsDevShell の後) | 約 60 s(外れ。続けてもう 1 回でも外れ) |
  | ctest(Enter-VsDevShell の後。= 今までの test.ps1) | 約 60 s(外れ) |
  | Enter-VsDevShell の後に LOCALAPPDATA だけ戻す | 外れ(LOCALAPPDATA だけでは足りない) |
  | Enter-VsDevShell の後に Path だけ戻す | 外れ(50 s) |
  | Enter-VsDevShell の後に化けた変数(Path 以外の 9 個)を戻す | 0.27 s(当たる) |
  | **新しい _vsenv.ps1(VsDevCmd.bat を cmd /u で動かし UTF-16 で読む)+ ctest** | **0.27 s(当たる)。化けた変数 0・Path 74/74・cl.exe あり** |
  9 個のどれがドライバに効くか(USERPROFILE か APPDATA か USERNAME か)は未確認(直し方には関係しないので測っていない)。
- 候補 2(ctest の子プロセスの起動の仕方)・候補 3(読み込む DLL。Path)は原因ではなかった。候補 4(ShaderCache の API)は要らなくなったので試していない。
- ゲームの本番の起動(エクスプローラーや普通のシェルからの exe・run)は化けた環境を通らないので当たる(run で確かめ済み)。
  **VS の「Developer PowerShell」から起動すると同じ Enter-VsDevShell を通るので外れるはず**(未確認。同じモジュール)。

## やったこと
- scripts/jobs/_vsenv.ps1: Enter-VsDevShell をやめ、VsDevCmd.bat を `cmd /d /u /c` で動かして `set` を UTF-16 のファイルに書かせ、
  その値で環境を作る(バッチは ASCII だけ。パスは環境変数で渡す)。build・test・tidy・env の 4 つのジョブがこれを使う。
- 打ち切り条件の逃げ道(ほかの多重解像度の試験にも gpu::PipelineCache を足す。T-0427 の判断待ち)は要らなくなった:
  ドライバのキャッシュに当たるので、試験は 2 回目の起動から 1 場面目も速い。T-0427 の判断待ちは「足さない」で閉じてよい(取り消しやすい。足すのは 1 行)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-14(wt4・作業役): 同じ exe を起動の仕方だけ変えて測り、原因を ctest ではなく test.ps1 の Enter-VsDevShell が化けさせた環境変数
  (ASCII の外の文字を含む USERPROFILE・APPDATA・USERNAME など)と突き止めた。_vsenv.ps1 を VsDevCmd.bat の環境を cmd /u で UTF-16 で読む形にした。
  `job.py test -Preset release` で gpu_multires_wide_* 6 本(3〜9 s。前は 1 本ごとに約 50 s のコンパイル)と window_rebuild_record/play
  (作り直しの止まり 0.57 s。前は約 19 s)が通過。どの 1 変数が効くかは未確認(直し方に関係しない)。perf.md に 2 行。
