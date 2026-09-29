# ADR-0006 ログの仕組み(CPU 側)

- Status: Accepted
- 日付: 2026-09-29
- 決めた人: ユーザー(T-0007 で Claude が案を出し、ユーザーが選んだ)

## 背景
どこかが壊れたときに「どのサブシステムのどこで」壊れたかがすぐ分かるようにしたい(T-0007)。
docs/style.md はグローバルな状態を避けるが、ログだけは例外として core/singleton.h を使う候補にしていた。

## 決定
1. **生存期間は `Singleton<Logger>`**(core/singleton.h)。最初の `Log()` で作られ、main の最後の
   `SingletonFinalizer::Finalize()` で壊れる。ログは最初に作られるので、作った順の逆の破棄で最後になる。
   - 却下: main で作って参照を渡す(全サブシステムの引数に Logger& が増える)、関数内 static(破棄順が静的破棄任せ)。
2. **HRESULT の失敗はマクロ `BICAMERAL_CHECK_HR(channel, 式)`**。式の文字列・ファイル:行・エラー名・OS の説明文を
   Error で出し、成否を bool で返す。
   - 却下: API 名を手で渡す関数(呼んだ API と書いた名前がずれうる)。
   - 失敗して当然の問い合わせ(古いランタイムでの CheckFeatureSupport など)にはマクロを使わず Warning / Info で出す。
3. **ログファイルは exe の横の `logs/`**(`bicameral-YYYYMMDD-HHMMSS.log`、新しい 20 個を残す)。`--log-dir` で変えられる。
   - 却下: リポジトリの `out/logs/` を CMake で埋め込む(PC 固有のパスがバイナリに入る)。
4. 出力先はコンソール(UTF-8・色つき)・OutputDebugString(VS の出力ウィンドウ。Warning 以上は `path(line):` で飛べる)・ファイルの 3 つ。
   書式は `std::format`(コンパイル時に検査)。チャンネル(サブシステム名)と重大度(Trace〜Fatal)を必ず持つ。
5. 依存は各サブシステム → core/log の一方向。log は他のサブシステムを知らない。

## 影響
- main は `wmain` になった(引数を UTF-16 で受け取り、日本語のパスを壊さない)。
- GPU 側のデバッグ出力(リングバッファ)は T-0003 で作り、読み戻した結果をこのログに流す。
- 毎フレームの大量ログが出るようになったら、コンソールの毎行 flush と、鍵を取る書き込みを見直す(非同期化など)。
