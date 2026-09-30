// log_sinks.h — ログの出力先(コンソール・デバッガ・ファイル)を作る。Windows 専用。
//
// Logger(core/log.h)は作られたときにコンソールとデバッガのシンクを自分で登録する。
// ファイルは置き場所をコマンドラインで変えられるように、main が OpenLogFile() で後から足す。
#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

#include "core/aliases.h"
#include "core/log.h"

namespace bicameral {

    // 標準出力へ UTF-8 で書く。コンソールなら色を付け、Warning 以上には呼んだ場所を付ける
    std::unique_ptr<LogSink> CreateConsoleSink();

    // OutputDebugString(VS の出力ウィンドウ)へ書く。Warning 以上は "path(line): ..." の形で、ダブルクリックで飛べる
    std::unique_ptr<LogSink> CreateDebuggerSink();

    // directory に bicameral-YYYYMMDD-HHMMSS.log を作ってファイルのシンクを logger に足す。成功ならファイルのパス。
    // 古いログは新しい方から MAX_LOG_FILES 個だけ残して消す
    std::expected<fs::path, std::string> OpenLogFile(Logger& logger, const fs::path& directory);

    // 既定のログの置き場所: exe の横の logs/(ADR-0006。PC 固有のパスを埋め込まない)
    fs::path DefaultLogDirectory();

}  // namespace bicameral
