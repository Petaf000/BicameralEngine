// log_sinks.cpp — ログの出力先 3 種の実装(コンソール・OutputDebugString・ファイル)。
//
// どのシンクも Logger の鍵の中から呼ばれる(Logger::Write)ので、シンク自身は鍵を持たない。
// core はプリコンパイルヘッダを持たないので、windows.h はここで直接 include する。
#include "core/log_sinks.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>
#include <vector>

#include "core/aliases.h"
#include "core/hresult.h"
#include "core/unicode.h"

namespace bicameral {
    namespace {

        constexpr size_t MAX_LOG_FILES = 20;
        constexpr std::wstring_view LOG_FILE_PREFIX = L"bicameral-";
        constexpr std::wstring_view LOG_FILE_EXTENSION = L".log";

        // --- コンソール ---

        // VT のエスケープシーケンスで色を付ける(Windows 10 以降のコンソールが対応)
        std::string_view ColorOf(Level level) {
            switch (level) {
                case Level::Trace:
                case Level::Debug: return "\x1b[90m";    // 灰
                case Level::Warning: return "\x1b[33m";  // 黄
                case Level::Error:
                case Level::Fatal: return "\x1b[31m";  // 赤
                default: return "";
            }
        }

        class ConsoleSink final : public LogSink {
        public:
            ConsoleSink() {
                // 日本語をそのまま出すため、コンソールの出力コードページを UTF-8 にする(終わったら戻す)
                m_previousCodePage = GetConsoleOutputCP();
                if (m_previousCodePage != 0 && m_previousCodePage != CP_UTF8)
                    SetConsoleOutputCP(CP_UTF8);

                // リダイレクトされている(ランナー・パイプ)ときは GetConsoleMode が失敗する → 色を付けない
                m_output = GetStdHandle(STD_OUTPUT_HANDLE);
                if (!GetConsoleMode(m_output, &m_previousMode))
                    return;

                m_useColor = SetConsoleMode(m_output, m_previousMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
            }

            ~ConsoleSink() override {
                std::fflush(stdout);
                if (m_useColor)
                    SetConsoleMode(m_output, m_previousMode);

                if (m_previousCodePage != 0 && m_previousCodePage != CP_UTF8)
                    SetConsoleOutputCP(m_previousCodePage);
            }

            void Write(const LogRecord& record) override {
                const std::string line = FormatLogLine(record, record.level >= Level::Warning);
                const std::string_view color = m_useColor ? ColorOf(record.level) : std::string_view{};
                if (color.empty())
                    std::fprintf(stdout, "%s\n", line.c_str());
                else
                    std::fprintf(stdout, "%.*s%s\x1b[0m\n", static_cast<int>(color.size()), color.data(), line.c_str());

                // 落ちる直前の行を失わないよう毎行書き出す(フレームごとの大量ログはまだ無い。増えたら見直す)
                std::fflush(stdout);
            }

            void Flush() override { std::fflush(stdout); }

        private:
            HANDLE m_output = nullptr;
            DWORD m_previousMode = 0;
            UINT m_previousCodePage = 0;
            bool m_useColor = false;
        };

        // --- デバッガ ---

        class DebuggerSink final : public LogSink {
        public:
            void Write(const LogRecord& record) override {
                std::string line;
                if (record.level >= Level::Warning) {
                    // VS の出力ウィンドウは "フルパス(行): " で始まる行をダブルクリックでその場所へ飛べる
                    line = std::format("{}({}): {} {} | {}\n", record.location.file_name(), record.location.line(),
                                       LevelName(record.level), ChannelName(record.channel), record.message);
                } else
                    line = FormatLogLine(record, false) + '\n';

                OutputDebugStringW(ToWide(line).c_str());
            }
        };

        // --- ファイル ---

        class FileSink final : public LogSink {
        public:
            explicit FileSink(std::ofstream stream) : m_stream(std::move(stream)) {}
            // 破棄のときの書き出しは std::ofstream のデストラクタに任せる(デストラクタから例外を出さないため)

            void Write(const LogRecord& record) override { m_stream << FormatLogLine(record, true) << '\n'; }
            void Flush() override { m_stream.flush(); }

        private:
            std::ofstream m_stream;
        };

        // std::error_code の説明を UTF-8 で。MSVC の system_category().message() は ANSI コードページで返すので使わない
        std::string DescribeErrorCode(const std::error_code& error) {
            if (error.category() != std::system_category())
                return error.message();
            return DescribeHresult(HRESULT_FROM_WIN32(static_cast<DWORD>(error.value())));
        }

        std::tm LocalNow() {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
            localtime_s(&local, &now);

            return local;
        }

        bool IsLogFileName(const fs::path& path) {
            const std::wstring name = path.filename().wstring();
            return name.starts_with(LOG_FILE_PREFIX) && name.ends_with(LOG_FILE_EXTENSION);
        }

        // 新しいファイルを 1 つ足しても MAX_LOG_FILES 個に収まるように、古いものから消す。
        // 名前に日時が入っているので、名前順 = 古い順。消せなくても続ける(ログが無いよりはまし)
        void RemoveOldLogFiles(const fs::path& directory) {
            std::error_code error;
            std::vector<fs::path> files;
            for (const auto& entry : fs::directory_iterator(directory, error)) {
                if (entry.is_regular_file(error) && IsLogFileName(entry.path()))
                    files.push_back(entry.path());
            }

            if (files.size() < MAX_LOG_FILES)
                return;

            rng::sort(files);
            const size_t removeCount = files.size() - (MAX_LOG_FILES - 1);
            for (size_t i = 0; i < removeCount; ++i)
                fs::remove(files[i], error);
        }

    }  // namespace

    std::unique_ptr<LogSink> CreateConsoleSink() {
        return std::make_unique<ConsoleSink>();
    }

    std::unique_ptr<LogSink> CreateDebuggerSink() {
        return std::make_unique<DebuggerSink>();
    }

    std::expected<fs::path, std::string> OpenLogFile(Logger& logger, const fs::path& directory) {
        std::error_code error;
        fs::create_directories(directory, error);
        if (error) {
            return std::unexpected(
                std::format("ログのフォルダを作れない: {}: {}", ToUtf8(directory.wstring()), DescribeErrorCode(error)));
        }

        RemoveOldLogFiles(directory);

        // --- ファイルを開く ---
        const std::tm now = LocalNow();

        const std::wstring fileName = std::format(L"{}{:04}{:02}{:02}-{:02}{:02}{:02}{}", LOG_FILE_PREFIX,
                                                  now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour,
                                                  now.tm_min, now.tm_sec, LOG_FILE_EXTENSION);

        fs::path path = directory / fileName;                          // const にしない(return で move させる)
        std::ofstream stream(path, std::ios::binary | std::ios::app);  // 同じ秒に 2 回起動したら追記になる
        if (!stream)
            return std::unexpected(std::format("ログのファイルを開けない: {}", ToUtf8(path.wstring())));

        stream << std::format("# Bicameral Engine log  {:04}-{:02}-{:02} {:02}:{:02}:{:02}\n", now.tm_year + 1900,
                              now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec);
        stream << "# [経過秒] 重大度 チャンネル | 本文  (ファイル:行)\n";
        logger.AddSink(std::make_unique<FileSink>(std::move(stream)));

        return path;
    }

    fs::path DefaultLogDirectory() {
        // MAX_PATH を超えるパスもあるので、足りなければ広げて取り直す
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;) {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
                return fs::path(L"logs");

            if (length < buffer.size()) {
                buffer.resize(length);
                break;
            }

            buffer.resize(buffer.size() * 2);
        }

        return fs::path(buffer).parent_path() / L"logs";
    }

}  // namespace bicameral
