// log_test.cpp — core/log・core/hresult・core/log_sinks を確かめる(CPU だけで走る)。
// 書式・呼んだ場所(ファイル:行)・重大度での間引き・HRESULT の説明・ファイルへの出力と古いファイルの削除・UTF-8 の往復。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/hresult.h"
#include "core/log.h"
#include "core/log_sinks.h"
#include "core/singleton.h"
#include "core/unicode.h"

namespace {

    using namespace bicameral;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition) return;
        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

    // シンクが受け取ったものを写しておく(LogRecord の message は呼び出しの間しか有効でないため)
    struct CapturedRecord {
        Channel channel;
        Level level;
        std::string message;
        std::string fileName;
        unsigned line;
    };

    std::vector<CapturedRecord> captured;

    class MemorySink final : public LogSink {
    public:
        void Write(const LogRecord& record) override {
            captured.push_back({.channel = record.channel,
                                .level = record.level,
                                .message = std::string(record.message),
                                .fileName = std::string(FileNameOnly(record.location.file_name())),
                                .line = record.location.line()});
        }
    };

    HResult ReturnResult(HResult result) {
        return result;
    }

    bool Contains(std::string_view text, std::string_view part) {
        return text.find(part) != std::string_view::npos;
    }

}  // namespace

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

// --- 書式と呼んだ場所 ---
void TestFormatAndLocation(Logger& logger) {
    captured.clear();
    logger.SetMinLevel(Level::Trace);
    Log(Channel::Reaction, Level::Info, "x={} y={}", 1, "a");
    const unsigned expectedLine = __LINE__ - 1;

    EXPECT(captured.size() == 1);
    if (captured.empty()) return;
    EXPECT(captured[0].message == "x=1 y=a");
    EXPECT(captured[0].channel == Channel::Reaction);
    EXPECT(captured[0].level == Level::Info);
    EXPECT(captured[0].fileName == "log_test.cpp");
    EXPECT(captured[0].line == expectedLine);  // Log の中ではなく、呼んだ行になっている
}

// --- 1 行への整形 ---
void TestFormatLogLine() {
    const LogRecord record{
        .channel = Channel::Gpu,
        .level = Level::Warning,
        .message = "m",
        .location = std::source_location::current(),
        .elapsedSeconds = 1.5,
    };
    EXPECT(FormatLogLine(record, false) == "[    1.500] W gpu       | m");
    EXPECT(Contains(FormatLogLine(record, true), "  (log_test.cpp:"));
    EXPECT(ChannelName(Channel::WorkGraph) == "workgraph");

    Level level = Level::Trace;
    EXPECT(ParseLevel("warning", level) && level == Level::Warning);
    EXPECT(!ParseLevel("nope", level));
}

// --- 重大度での間引き ---
void TestLevelFilter(Logger& logger) {
    captured.clear();
    logger.SetMinLevel(Level::Warning);
    Log(Channel::Core, Level::Info, "捨てられる");
    Log(Channel::Core, Level::Error, "残る");
    logger.SetMinLevel(Level::Trace);

    EXPECT(captured.size() == 1);
    if (captured.empty()) return;
    EXPECT(captured[0].message == "残る");
}

// --- HRESULT ---
void TestHresult() {
    captured.clear();
    EXPECT(BICAMERAL_CHECK_HR(Channel::Gpu, ReturnResult(S_OK)));
    EXPECT(captured.empty());

    const bool succeeded = BICAMERAL_CHECK_HR(Channel::Gpu, ReturnResult(E_INVALIDARG));
    const unsigned expectedLine = __LINE__ - 1;
    EXPECT(!succeeded);
    EXPECT(captured.size() == 1);
    if (captured.empty()) return;
    EXPECT(captured[0].level == Level::Error);
    EXPECT(captured[0].channel == Channel::Gpu);
    EXPECT(Contains(captured[0].message, "ReturnResult(E_INVALIDARG) が失敗"));
    EXPECT(Contains(captured[0].message, "E_INVALIDARG (0x80070057): "));  // 名前・コード・OS の説明文
    EXPECT(captured[0].line == expectedLine);

    EXPECT(Contains(DescribeHresult(DXGI_ERROR_DEVICE_REMOVED), "DXGI_ERROR_DEVICE_REMOVED (0x887A0005)"));
    EXPECT(DescribeHresult(static_cast<HResult>(0x80041234)).starts_with("0x80041234"));  // 名前を知らないコード
}

// --- ファイルへの出力と、古いファイルの削除 ---
void TestFileSink(Logger& logger, const std::filesystem::path& directory) {
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    for (int i = 0; i < 25; ++i) {  // 名前順で今の日時より古くなる偽のログ
        std::ofstream(directory / std::format("bicameral-00000000-0000{:02}.log", i)) << "old\n";
    }
    std::ofstream(directory / "other.txt") << "消されない\n";

    const auto path = OpenLogFile(logger, directory);
    EXPECT(path.has_value());
    if (!path) return;
    Log(Channel::Tool, Level::Info, "ファイルへ 日本語");
    logger.Flush();

    std::stringstream content;
    content << std::ifstream(*path, std::ios::binary).rdbuf();
    EXPECT(Contains(content.str(), "I tool      | ファイルへ 日本語  (log_test.cpp:"));

    size_t logFileCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".log") ++logFileCount;
    }
    EXPECT(logFileCount == 20);  // 古い 19 個 + 新しい 1 個
    EXPECT(std::filesystem::exists(directory / "other.txt"));
}

int main() {
    Logger& logger = GetLogger();
    logger.ClearSinks();  // コンソールに出さず、写しだけを取る
    logger.AddSink(std::make_unique<MemorySink>());

    TestFormatAndLocation(logger);
    TestFormatLogLine();
    TestLevelFilter(logger);
    TestHresult();
    EXPECT(ToWide(ToUtf8(L"反応の連鎖")) == L"反応の連鎖");

    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "bicameral_log_test";
    TestFileSink(logger, directory);
    SingletonFinalizer::Finalize();  // ファイルを閉じてから消す
    std::filesystem::remove_all(directory);

    if (failureCount == 0) std::printf("log ok\n");
    return failureCount == 0 ? 0 : 1;
}
