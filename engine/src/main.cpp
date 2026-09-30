// Bicameral Engine — ランタイムの入口。
//
// 引数が無ければ窓を開いてフレームのループを回す(frame/frame_loop.h。T-0004)。
//   bicameral --caps                 GPU の対応状況を表示して終了
//   --frames <n>                     n フレームで終える(自動の確認用。既定は窓を閉じるまで)
//   --no-vsync                       垂直同期を待たずに Present する
//   --latency <2|3>                  CPU が GPU より先に進めるフレームの数(既定 2)
//   --sim-load <n>                   重さの試験: 刻みに結果に入らない計算を n 回足す(世界が遅くなるのを見る。D-202)
//   --auto-click                     決まった場所を自動でクリックする(人がいない確認でイベントの流れを通す)
//   --warp                           WARP(ソフトウェアの D3D12)で走らせる
//   --log-dir <path>                 ログファイルの置き場所(既定: exe の横の logs/。ADR-0006)
//   --log-level <trace|debug|info|warning|error|fatal>
//                                    これより軽いログを捨てる(既定: Debug ビルドは debug、Release は info)
//
// 引数は wmain で UTF-16 のまま受け取る(main の char** は ANSI コードページなので日本語のパスが壊れる)。
// 終わるときは必ず SingletonFinalizer::Finalize() を通す(ログを最後に閉じ、ファイルへ書き出すため)。
#include <charconv>
#include <optional>

#include "common/probe_sim.hlsli"
#include "core/log.h"
#include "core/log_sinks.h"
#include "core/singleton.h"
#include "core/unicode.h"
#include "frame/frame_loop.h"
#include "platform/caps.h"

namespace {

    using namespace bicameral;

    struct Options {
        bool runCaps = false;
        frame::FrameLoopOptions frameLoop;
        std::filesystem::path logDirectory;  // 空なら DefaultLogDirectory()
        Level logLevel = Level::Info;
        bool hasLogLevel = false;
    };

    // --- コマンドライン ---

    // 10 進の整数。範囲外・数でなければ std::nullopt
    std::optional<uint32_t> ParseCount(std::wstring_view text, uint32_t maximum) {
        uint32_t value = 0;
        const std::string utf8 = ToUtf8(text);
        const auto [end, error] = std::from_chars(utf8.data(), utf8.data() + utf8.size(), value);
        if (error != std::errc{} || end != utf8.data() + utf8.size() || value > maximum) return std::nullopt;
        return value;
    }

    // フレームのループの値つきの引数(--frames・--latency・--sim-load)
    std::expected<void, std::string> ParseFrameLoopCount(std::wstring_view argument, std::wstring_view text,
                                                         frame::FrameLoopOptions& loop) {
        const uint32_t maximum = argument == L"--latency"    ? 3u
                                 : argument == L"--sim-load" ? sim::PROBE_BUSY_ITERATIONS_LIMIT
                                                             : UINT32_MAX;
        const auto value = ParseCount(text, maximum);
        if (!value || (argument == L"--latency" && *value < 2)) {
            return std::unexpected(std::format("{} の値が不正: {}", ToUtf8(argument), ToUtf8(text)));
        }
        if (argument == L"--frames") loop.frameLimit = *value;
        if (argument == L"--latency") loop.maxFrameLatency = *value;
        if (argument == L"--sim-load") loop.simLoad = *value;
        return {};
    }

    // フレームのループの値なしの引数。当てはまらなければ false
    bool ParseFrameLoopFlag(std::wstring_view argument, frame::FrameLoopOptions& loop) {
        if (argument == L"--no-vsync") {
            loop.vsync = false;
        } else if (argument == L"--auto-click") {
            loop.autoClick = true;
        } else if (argument == L"--warp") {
            loop.adapter = gpu::AdapterKind::Warp;
        } else {
            return false;
        }
        return true;
    }

    std::expected<Options, std::string> ParseOptions(std::span<wchar_t*> arguments) {
        Options options;
        for (size_t i = 1; i < arguments.size(); ++i) {
            const std::wstring_view argument = arguments[i];
            const bool hasValue = i + 1 < arguments.size();
            if (argument == L"--caps") {
                options.runCaps = true;
            } else if (ParseFrameLoopFlag(argument, options.frameLoop)) {
                continue;
            } else if ((argument == L"--frames" || argument == L"--latency" || argument == L"--sim-load") && hasValue) {
                const auto parsed = ParseFrameLoopCount(argument, arguments[++i], options.frameLoop);
                if (!parsed) return std::unexpected(parsed.error());
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
        return frame::RunFrameLoop(options->frameLoop);
    }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    bicameral::SingletonFinalizer::Finalize();  // ログは最初に作られるので最後に壊れる(作った順の逆)
    return exitCode;
}
