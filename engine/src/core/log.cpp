// log.cpp — Logger の本体と、名前の表・1 行への整形。
//
// 出力先(コンソール・デバッガ・ファイル)の実装は log_sinks.cpp。ここは Windows を知らない。
#include "core/log.h"

#include <array>
#include <iterator>

#include "core/log_sinks.h"

namespace bicameral {
    namespace {

        constexpr std::array<std::string_view, static_cast<size_t>(Channel::Count)> CHANNEL_NAMES = {
            "core", "platform", "gpu", "workgraph", "sim", "reaction", "physics", "render", "audio", "tool",
        };

        constexpr std::array<std::string_view, 6> LEVEL_NAMES = {
            "trace", "debug", "info", "warning", "error", "fatal",
        };

        constexpr std::string_view LEVEL_LETTERS = "TDIWEF";

#ifdef NDEBUG
        constexpr Level DEFAULT_MIN_LEVEL = Level::Info;
#else
        constexpr Level DEFAULT_MIN_LEVEL = Level::Debug;
#endif

    }  // namespace

    // --- 名前 ---

    std::string_view ChannelName(Channel channel) {
        const auto index = static_cast<size_t>(channel);
        if (index >= CHANNEL_NAMES.size()) return "?";
        return CHANNEL_NAMES[index];
    }

    std::string_view LevelName(Level level) {
        const auto index = static_cast<size_t>(level);
        if (index >= LEVEL_NAMES.size()) return "?";
        return LEVEL_NAMES[index];
    }

    char LevelLetter(Level level) {
        const auto index = static_cast<size_t>(level);
        if (index >= LEVEL_LETTERS.size()) return '?';
        return LEVEL_LETTERS[index];
    }

    bool ParseLevel(std::string_view text, Level& level) {
        for (size_t i = 0; i < LEVEL_NAMES.size(); ++i) {
            if (LEVEL_NAMES[i] != text) continue;
            level = static_cast<Level>(i);
            return true;
        }
        return false;
    }

    // --- 整形 ---

    std::string_view FileNameOnly(std::string_view path) {
        const size_t slash = path.find_last_of("/\\");
        return slash == std::string_view::npos ? path : path.substr(slash + 1);
    }

    std::string FormatLogLine(const LogRecord& record, bool withLocation) {
        // チャンネル名の幅は最長の "workgraph" に合わせる(縦にそろうと目で追いやすい)
        std::string line = std::format("[{:9.3f}] {} {:<9} | {}", record.elapsedSeconds, LevelLetter(record.level),
                                       ChannelName(record.channel), record.message);
        if (withLocation) {
            std::format_to(std::back_inserter(line), "  ({}:{})", FileNameOnly(record.location.file_name()),
                           record.location.line());
        }
        return line;
    }

    // --- Logger ---

    Logger::Logger() : m_minLevel(DEFAULT_MIN_LEVEL), m_startTime(std::chrono::steady_clock::now()) {
        AddSink(CreateConsoleSink());
        AddSink(CreateDebuggerSink());
    }

    Logger::~Logger() {
        Flush();
    }

    void Logger::AddSink(std::unique_ptr<LogSink> sink) {
        if (sink == nullptr) return;
        std::scoped_lock lock(m_mutex);
        m_sinks.push_back(std::move(sink));
    }

    void Logger::ClearSinks() {
        std::scoped_lock lock(m_mutex);
        m_sinks.clear();
    }

    void Logger::Write(Channel channel, Level level, std::string_view message, const std::source_location& location) {
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - m_startTime;
        const LogRecord record{
            .channel = channel,
            .level = level,
            .message = message,
            .location = location,
            .elapsedSeconds = elapsed.count(),
        };

        std::scoped_lock lock(m_mutex);
        for (const auto& sink : m_sinks) {
            sink->Write(record);
        }

        // 警告以上は、直後に落ちても残るようにすぐ書き出す
        if (level < Level::Warning) return;
        for (const auto& sink : m_sinks) {
            sink->Flush();
        }
    }

    void Logger::Flush() {
        std::scoped_lock lock(m_mutex);
        for (const auto& sink : m_sinks) {
            sink->Flush();
        }
    }

}  // namespace bicameral
