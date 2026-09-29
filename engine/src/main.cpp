// Bicameral Engine — ランタイムの入口。
//
// 今はコマンドラインの振り分けだけ。窓・スワップチェイン・フレームループは T-0004 以降。
//   bicameral --caps                 GPU の対応状況を表示して終了
//   --log-dir <path>                 ログファイルの置き場所(既定: exe の横の logs/。ADR-0006)
//   --log-level <trace|debug|info|warning|error|fatal>
//                                    これより軽いログを捨てる(既定: Debug ビルドは debug、Release は info)
//
// 引数は wmain で UTF-16 のまま受け取る(main の char** は ANSI コードページなので日本語のパスが壊れる)。
// 終わるときは必ず SingletonFinalizer::Finalize() を通す(ログを最後に閉じ、ファイルへ書き出すため)。
#include "core/log.h"
#include "core/log_sinks.h"
#include "core/singleton.h"
#include "core/unicode.h"
#include "platform/caps.h"

namespace {

    using namespace bicameral;

    struct Options {
        bool runCaps = false;
        std::filesystem::path logDirectory;  // 空なら DefaultLogDirectory()
        Level logLevel = Level::Info;
        bool hasLogLevel = false;
    };

    // --- コマンドライン ---

    std::expected<Options, std::string> ParseOptions(std::span<wchar_t*> arguments) {
        Options options;
        for (size_t i = 1; i < arguments.size(); ++i) {
            const std::wstring_view argument = arguments[i];
            const bool hasValue = i + 1 < arguments.size();
            if (argument == L"--caps") {
                options.runCaps = true;
            } else if (argument == L"--log-dir" && hasValue) {
                options.logDirectory = arguments[++i];
            } else if (argument == L"--log-level" && hasValue) {
                const std::string name = ToUtf8(arguments[++i]);
                options.hasLogLevel = ParseLevel(name, options.logLevel);
                if (!options.hasLogLevel) return std::unexpected(std::format("知らないログの重大度: {}", name));
            } else {
                return std::unexpected(std::format("知らない引数: {}", ToUtf8(argument)));
            }
        }
        return options;
    }

    // --- 本体 ---

    int Run(std::span<wchar_t*> arguments) {
        const auto options = ParseOptions(arguments);
        Logger& logger = GetLogger();
        if (!options) {
            Log(Channel::Core, Level::Error, "{}(使い方は main.cpp の先頭)", options.error());
            return 2;
        }
        if (options->hasLogLevel) logger.SetMinLevel(options->logLevel);

        const std::filesystem::path logDirectory =
            options->logDirectory.empty() ? DefaultLogDirectory() : options->logDirectory;
        if (const auto logFile = OpenLogFile(logger, logDirectory)) {
            Log(Channel::Core, Level::Info, "Bicameral Engine  ログ: {}", ToUtf8(logFile->wstring()));
        } else {
            Log(Channel::Core, Level::Warning, "ログのファイルなしで続ける: {}", logFile.error());
        }

        if (options->runCaps) return RunCapsProbe();
        Log(Channel::Core, Level::Info, "まだ骨組みだけ。--caps を試す");
        return 0;
    }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    bicameral::SingletonFinalizer::Finalize();  // ログは最初に作られるので最後に壊れる(作った順の逆)
    return exitCode;
}
