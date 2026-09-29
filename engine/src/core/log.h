// log.h — サブシステム名つき・重大度つきのログ(T-0007)。
//
// 使い方:
//   Log(Channel::Platform, Level::Info, "Adapter {}: {}", index, name);
//   → [    0.004] I platform | Adapter 0: NVIDIA GeForce RTX 3070 Ti
// 書式は std::format(書式の誤りはコンパイル時に分かる)。呼んだ場所(ファイル:行)は自動で付く。
//
// データの流れ: Log() → Singleton<Logger>(core/singleton.h)→ 登録された LogSink すべて
//   既定のシンクはコンソールと OutputDebugString(VS の出力ウィンドウ)。ファイルは OpenLogFile()(core/log_sinks.h)で足す。
// 依存の向き: 各サブシステム → log の一方向だけ。log は他のサブシステムを知らない(Channel は名前の一覧にすぎない)。
// 生存期間: 最初の Log() で作られ、main の最後の SingletonFinalizer::Finalize() で最後に壊れる
//   (ADR-0006。ログを使う物より後に作られることはないので、作った順の逆の破棄で最後になる)。
// HRESULT の失敗は core/hresult.h の BICAMERAL_CHECK_HR を使う。
#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/singleton.h"

namespace bicameral {

    // --- 種類 ---

    // どのサブシステムが出したか。足すときは log.cpp の CHANNEL_NAMES にも名前を足す
    enum class Channel : uint8_t {
        Core,
        Platform,   // 窓・入力・OS・アダプタの列挙
        Gpu,        // デバイス・キュー・リソース・同期
        WorkGraph,  // Work Graphs の構築と投入
        Reaction,   // 反応の連鎖
        Physics,    // AVBD
        Render,     // 描画
        Audio,
        Tool,  // エディタ・ベイク
        Count,
    };

    // 重大度。Warning 以上はすぐにファイルへ書き出し(Flush)、呼んだ場所も表示する
    enum class Level : uint8_t {
        Trace,
        Debug,
        Info,
        Warning,
        Error,
        Fatal,  // この後すぐ終了する。ログは Flush 済みになる(止めるのは呼んだ側)
    };

    [[nodiscard]] std::string_view ChannelName(Channel channel);  // "platform"
    [[nodiscard]] std::string_view LevelName(Level level);        // "warning"
    [[nodiscard]] char LevelLetter(Level level);                  // 'W'

    // 文字列からの変換(コマンドラインの --log-level 用)。知らない名前なら false
    [[nodiscard]] bool ParseLevel(std::string_view text, Level& level);

    // 1 件のログ。シンクに渡す間だけ有効(message は呼び出し側の文字列を指す)
    struct LogRecord {
        Channel channel;
        Level level;
        std::string_view message;
        std::source_location location;
        double elapsedSeconds;  // Logger を作ってからの経過時間
    };

    // 1 行に整形する: "[    0.004] I platform  | message"。withLocation なら末尾に "  (caps.cpp:88)"
    [[nodiscard]] std::string FormatLogLine(const LogRecord& record, bool withLocation);

    // ファイル名だけを取り出す(source_location はフルパスを返すため)
    [[nodiscard]] std::string_view FileNameOnly(std::string_view path);

    // --- 出力先 ---

    class LogSink {
    public:
        virtual ~LogSink() = default;
        virtual void Write(const LogRecord& record) = 0;
        virtual void Flush() {}
    };

    // --- 本体 ---

    class Logger {
    public:
        Logger();  // 既定のシンク(コンソール・デバッガ)を登録する
        ~Logger();
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        void AddSink(std::unique_ptr<LogSink> sink);
        void ClearSinks();  // テスト用。既定のシンクも外れる

        void SetMinLevel(Level level) { m_minLevel.store(level, std::memory_order_relaxed); }
        [[nodiscard]] Level MinLevel() const { return m_minLevel.load(std::memory_order_relaxed); }
        [[nodiscard]] bool IsEnabled(Level level) const { return level >= MinLevel(); }

        // 整形済みの 1 件を全シンクへ。複数スレッドから呼んでよい(行が混ざらない)
        void Write(Channel channel, Level level, std::string_view message, const std::source_location& location);
        void Flush();

    private:
        std::mutex m_mutex;
        std::vector<std::unique_ptr<LogSink>> m_sinks;
        std::atomic<Level> m_minLevel;
        std::chrono::steady_clock::time_point m_startTime;
    };

    [[nodiscard]] inline Logger& GetLogger() {
        return Singleton<Logger>::GetInstance();
    }

    // --- 呼ぶ側 ---

    // 書式文字列と呼んだ場所をいっしょに受け取るための型。
    // 可変長引数の後ろに source_location の既定引数を置けないので、書式文字列の側に持たせる
    template <typename... Args>
    struct FormatWithLocation {
        std::format_string<Args...> text;
        std::source_location location;

        template <typename Text>
            requires std::convertible_to<const Text&, std::string_view>
        consteval FormatWithLocation(const Text& format, std::source_location where = std::source_location::current())
            : text(format), location(where) {}
    };

    template <typename... Args>
    void Log(Channel channel, Level level, FormatWithLocation<std::type_identity_t<Args>...> format, Args&&... args) {
        Logger& logger = GetLogger();
        if (!logger.IsEnabled(level)) return;  // 捨てるログは整形もしない
        logger.Write(channel, level, std::format(format.text, std::forward<Args>(args)...), format.location);
    }

}  // namespace bicameral
