# T-0007 ログの仕組み(CPU 側)

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分

## 目的
どこかが壊れたときに、どのサブシステムのどこで壊れたかがすぐ分かるようにする(ユーザーの要望 2026-09-29)。
GPU 側のデバッグ出力(リングバッファ)は T-0003。ここは CPU 側と、その出力先の共通化。

## やること(案)
- `engine/src/core/log.h`: サブシステム名つき・重大度つきのログ。`std::format` の書式。例: `Log(Channel::Reaction, Level::Error, "...", ...)`
- HRESULT の失敗を「呼んだ API・ファイル:行・HRESULT の意味」つきで出すマクロ/関数(`std::source_location`)。
- 出力先: コンソール + OutputDebugString(VS の出力ウィンドウ)+ ファイル(out/logs/)。
- caps.cpp の printf を置き換える。
- 依存の向き: 各サブシステム → log の一方向だけ。log は他のサブシステムを知らない。

## 決めること(ユーザー)
- ログの生存期間の持ち方。ユーザー提供の singleton_template を規約に合わせて `engine/src/core/singleton.h` に取り込み済み
  (作った順の逆に破棄・二重 delete しない・FinalizeClass の後は作り直す。tests/singleton_test.cpp)。これを使うのが第一案。

## 完了条件
- [x] サブシステム名つきのログが 3 つの出力先に出る
- [x] HRESULT の失敗が API 名と場所つきで出る
- [x] caps.cpp がログを使う
- [x] 図(map.yaml)にノードを足す

## 作業ログ
- 2026-09-29: ユーザーが決定(ADR-0006): Singleton<Logger>・HRESULT はマクロ BICAMERAL_CHECK_HR・ファイルは exe の横の logs/。
- core に log / log_sinks / hresult / unicode を追加。Log() は FormatWithLocation で std::format の検査と source_location を両立。
- caps.cpp の printf を全部 Log に。main は wmain にして --log-dir / --log-level を足し、最後に Finalize。
- tests/log_test.cpp(書式・呼んだ行・間引き・HRESULT・ファイルと古いログの削除・UTF-8)。debug/release とも警告 0、ctest 3/3。
- 設計メモ docs/design/03-logging.md、図に core のノード 3 つ。
