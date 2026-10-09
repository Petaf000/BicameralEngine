// Bicameral Engine — ランタイムの入口。
//
// 引数が無ければ窓を開いてフレームのループを回す(frame/frame_loop.h。T-0004・T-0012)。
//   bicameral --caps                 GPU の対応状況を表示して終了
//   --frames <n>                     n フレームで終える(自動の確認用。既定は窓を閉じるまで)
//   --no-vsync                       垂直同期を待たずに Present する
//   --latency <2|3>                  CPU が GPU より先に進めるフレームの数(既定 2)
//   --target-fps <f>                 重いときに描画が保つ fps(ADR-0011。30〜1000、既定 60)。予算 = 1000 / f − 描画 − 余裕
//   --sim-load <n>                   重さの試験: 刻みに結果に入らない計算を n 回足す(世界が遅くなるのを見る。D-202)
//   --sim-split <k>                  重さの試験を k 個の単位に分ける(1 フレームに予算ぶんの単位だけ投げる。既定 1)
//   --render-normal                  描画のキューの優先度を NORMAL にする(既定 HIGH。比較用)
//   --auto-click                     決まった場所を自動でクリックする(人がいない確認でイベントの流れを通す)
//   --auto-push                      60 フレーム目に積み木の 5 段目を押す(人がいない確認で押すコマンドの流れを通す。T-0098)
//   --auto-ignite                    最初のフレームに木箱の壁に 1 回だけ火をつける(最初の刻みに載るので、どの実行でも同じ世界。
//                                    --screenshot-tick の画像の比較用。T-0025)
//   --no-physics                     仮の世界に物理(積み木)を入れない(既定は入れる。窓では Shift + 左クリックで押す。T-0098)
//   --check-physics                  CPU の物理を並べて走らせ、刻みごとの物のハッシュを突き合わせる(調べる用)
//   --physics-compute                物理の広域の選別を Compute で(既定は Work Graph。比べる用)
//   --record <path>                  窓の操作(コマンド)と刻みごとのハッシュを再生ファイルに書く(終わるとき。save/replay_file)
//   --replay <path>                  再生ファイルのコマンドで世界を進め、刻みごとのハッシュを突き合わせる。
//                                    最後のハッシュまで確かめたら終わる(全部一致で 0、違えば 1)。窓のクリックは無視する
//   --view <volume|mip|slice>        最初のデバッグ表示(既定 volume。窓では 1・2・3 で切り替え。render/debug_view_controller.h)
//   --camera <yaw>,<pitch>,<距離>    最初のカメラ(度・度・セル。既定 35,25,150。0,0,80 で z = 32 の面を正面から)
//   --screenshot <path>              最後のフレームを BMP に書く(--frames と一緒に使う。render/screenshot.h)
//   --screenshot-tick <t>            刻み t の始めで世界を止め、S(t) を描いた画面を --screenshot に書いて終える(決まった画面。
//                                    画像の比較用。--frames はそこまでの上限。tools/image_compare。T-0025)
//   --peek <x,y,z>                   起動時からそのセルを覗き窓で覗く(影の鎖 k = 1〜9。窓では P。sim/probe_peek.h。T-0096)
//   --peek-depth <k>                 覗き窓で潜る段(0〜9。カメラが点に寄る。窓では PageDown・PageUp)
//   --trace <path>                   伝導の連鎖のトレースを刻みごとの木にして、終わるときに書く(sim/probe_trace.h。T-0087)
//   --trace-ticks <始め>:<終わり>    トレースする刻み [始め, 終わり)(既定: 全部)
//   --trace-cells <x,y,z>:<x,y,z>    トレースするセルの箱 [最小, 最大)(既定: 全部。そのセルを含むブロックを記録する)
//   --trace-dir <path>               窓の T で集めたトレースを書くフォルダ(既定: exe の横の traces/。T-0088)
//   --auto-trace                     30 フレーム目に T を押す(人がいない確認で T の流れを通す。--auto-click と一緒に)
//   --editor                         エディタの殻(ImGui の時間の操作・状態の表示。Space = 止める・N = 1 刻み)を重ねる(T-0023)
//   --auto-time                      決まったフレームで止める・1 刻み・速さを操作し、止まった・1 刻みずつ進んだかを確かめる(T-0023)
//   --save-points <n>                巻き戻しの保存点の数(--editor の既定は 6。0 なら巻き戻さない。1 つ約 33 MiB の VRAM。T-0143)
//   --save-interval <t>              保存点へ写す間隔(刻み。既定 120)
//   --auto-rewind                    保存点が 2 つできたら、古い方へ 1 回巻き戻す(人がいない確認。--replay と一緒にハッシュ列を確かめる)
//   --packages <path>                反応表のパッケージのフォルダ(直下のフォルダが 1 つずつパッケージ。既定: exe の横の data/packages。
//                                    読めない・検査で落ちたら起動しない。script/reaction_table_loader.h。T-0157)
//   --warp                           WARP(ソフトウェアの D3D12)で走らせる
//   --log-dir <path>                 ログファイルの置き場所(既定: exe の横の logs/。ADR-0006)
//   --log-level <trace|debug|info|warning|error|fatal>
//                                    これより軽いログを捨てる(既定: Debug ビルドは debug、Release は info)
//
// 引数は wmain で UTF-16 のまま受け取る(main の char** は ANSI コードページなので日本語のパスが壊れる)。
// 終わるときは必ず SingletonFinalizer::Finalize() を通す(ログを最後に閉じ、ファイルへ書き出すため)。
#include <array>
#include <charconv>
#include <optional>

#include "common/probe_sim.hlsli"
#include "core/aliases.h"
#include "core/log.h"
#include "core/log_sinks.h"
#include "core/paths.h"
#include "core/singleton.h"
#include "core/unicode.h"
#include "frame/frame_loop.h"
#include "platform/caps.h"
#include "sim/probe_trace.h"

namespace {

    using namespace bicameral;

    struct Options {
        bool runCaps = false;
        frame::FrameLoopOptions frameLoop;
        fs::path logDirectory;  // 空なら DefaultLogDirectory()
        Level logLevel = Level::Info;
        bool hasLogLevel = false;

        // --- 連鎖のトレースの範囲(--trace-ticks・--trace-cells。ParseOptions の最後に frameLoop.trace にする)---
        std::array<uint64_t, 2> traceTicks = {0, UINT64_MAX};
        std::array<uint64_t, 6> traceCells = {
            0, 0, 0, sim::PROBE_GRID_SIZE, sim::PROBE_GRID_SIZE, sim::PROBE_GRID_SIZE};
    };

    // --- コマンドライン ---

    // 10 進の整数。範囲外・数でなければ std::nullopt
    std::optional<uint32_t> ParseCount(std::wstring_view text, uint32_t maximum) {
        uint32_t value = 0;
        const std::string utf8 = ToUtf8(text);
        const auto [end, error] = std::from_chars(utf8.data(), utf8.data() + utf8.size(), value);
        if (error != std::errc{} || end != utf8.data() + utf8.size() || value > maximum)
            return std::nullopt;

        return value;
    }

    // "a:b" や "x,y,z:x,y,z" のような 10 進の整数の並び(区切りは , か :)。数が N 個でなければ std::nullopt
    template <size_t N>
    std::optional<std::array<uint64_t, N>> ParseNumbers(std::wstring_view text) {
        const std::string utf8 = ToUtf8(text);
        std::array<uint64_t, N> values{};
        const char* cursor = utf8.data();
        const char* end = utf8.data() + utf8.size();

        for (size_t index = 0; index < N; ++index) {
            const auto [next, error] = std::from_chars(cursor, end, values[index]);
            if (error != std::errc{})
                return std::nullopt;

            const bool last = index + 1 == N;
            if (last ? next != end : (next == end || (*next != ',' && *next != ':')))
                return std::nullopt;

            cursor = next + 1;
        }

        return values;
    }

    // トレースの範囲の引数(--trace-ticks・--trace-cells)
    std::expected<void, std::string> ParseTraceRange(std::wstring_view argument, std::wstring_view text,
                                                     Options& options) {
        if (argument == L"--trace-ticks") {
            const auto ticks = ParseNumbers<2>(text);
            if (!ticks || (*ticks)[0] >= (*ticks)[1])
                return std::unexpected(std::format("--trace-ticks の値が不正: {}(例: 10:20)", ToUtf8(text)));

            options.traceTicks = *ticks;

            return {};
        }

        const auto cells = ParseNumbers<6>(text);
        const bool ordered = cells && (*cells)[0] < (*cells)[3] && (*cells)[1] < (*cells)[4] &&
                             (*cells)[2] < (*cells)[5];
        if (!ordered)
            return std::unexpected(std::format("--trace-cells の値が不正: {}(例: 0,0,28:64,64,36)", ToUtf8(text)));

        options.traceCells = *cells;

        return {};
    }

    // --trace のときだけ、範囲をフレームのループのトレースの範囲にする(セルの箱 → ブロックの座標の箱)
    void ApplyTraceRange(Options& options) {
        if (options.frameLoop.tracePath.empty())
            return;

        const auto cell = [&options](size_t index) {
            return static_cast<uint32_t>(std::min<uint64_t>(options.traceCells[index], sim::PROBE_GRID_SIZE));
        };

        options.frameLoop.trace = sim::ProbeTraceFilterForCells(
            options.traceTicks[0], options.traceTicks[1], {cell(0), cell(1), cell(2)}, {cell(3), cell(4), cell(5)},
            frame::TRACE_CAPACITY_PER_FRAME);
    }

    // "yaw,pitch,距離"(小数でよい)。距離は正
    std::optional<render::OrbitCameraState> ParseCamera(std::wstring_view text) {
        const std::string utf8 = ToUtf8(text);
        std::array<float, 3> values{};
        const char* cursor = utf8.data();
        const char* end = utf8.data() + utf8.size();

        for (size_t index = 0; index < values.size(); ++index) {
            const auto [next, error] = std::from_chars(cursor, end, values[index]);
            if (error != std::errc{})
                return std::nullopt;
            const bool last = index + 1 == values.size();
            if (last ? next != end : (next == end || *next != ','))
                return std::nullopt;

            cursor = next + 1;
        }

        if (!(values[2] > 0.0f))
            return std::nullopt;

        render::OrbitCameraState camera;
        camera.yawDegrees = values[0];
        camera.pitchDegrees = values[1];
        camera.distance = values[2];

        return camera;
    }

    // 覗き窓の引数(--peek・--peek-depth)
    std::expected<void, std::string> ParsePeekOption(std::wstring_view argument, std::wstring_view text,
                                                     frame::FrameLoopOptions& loop) {
        if (argument == L"--peek-depth") {
            const auto depth = ParseCount(text, render::PEEK_MAX_DEPTH);
            if (!depth)
                return std::unexpected(
                    std::format("--peek-depth の値が不正: {}(0〜{})", ToUtf8(text), render::PEEK_MAX_DEPTH));

            loop.peekDepth = *depth;

            return {};
        }

        const auto cell = ParseNumbers<3>(text);
        if (!cell || std::ranges::any_of(*cell, [](uint64_t value) { return value >= sim::PROBE_GRID_SIZE; }))
            return std::unexpected(std::format("--peek の値が不正: {}(例: 28,32,32)", ToUtf8(text)));

        loop.peek = true;
        loop.peekCell = {.x = static_cast<uint32_t>((*cell)[0]),
                         .y = static_cast<uint32_t>((*cell)[1]),
                         .z = static_cast<uint32_t>((*cell)[2])};

        return {};
    }

    // 表示の引数(--view・--camera・--screenshot・--peek・--peek-depth)
    std::expected<void, std::string> ParseViewOption(std::wstring_view argument, std::wstring_view text,
                                                     frame::FrameLoopOptions& loop) {
        if (argument == L"--screenshot") {
            loop.screenshotPath = text;
            return {};
        }

        if (argument == L"--peek" || argument == L"--peek-depth")
            return ParsePeekOption(argument, text, loop);

        if (argument == L"--view") {
            if (render::ParseDebugViewMode(ToUtf8(text), loop.view.mode))
                return {};
            return std::unexpected(std::format("--view の値が不正: {}(volume・mip・slice)", ToUtf8(text)));
        }

        const auto camera = ParseCamera(text);
        if (!camera)
            return std::unexpected(std::format("--camera の値が不正: {}(例: 35,25,150)", ToUtf8(text)));

        loop.camera = *camera;

        return {};
    }

    // 目標 fps の範囲(ADR-0011: 30 より下げない)
    constexpr uint32_t MIN_TARGET_FPS = 30;
    constexpr uint32_t MAX_TARGET_FPS = 1000;

    // フレームのループの値つきの引数(--frames・--latency・--target-fps・--sim-load・--sim-split・--screenshot-tick)
    std::expected<void, std::string> ParseFrameLoopCount(std::wstring_view argument, std::wstring_view text,
                                                         frame::FrameLoopOptions& loop) {
        const uint32_t maximum = argument == L"--latency"      ? 3u
                                 : argument == L"--target-fps" ? MAX_TARGET_FPS
                                 : argument == L"--sim-load"   ? sim::PROBE_BUSY_ITERATIONS_LIMIT
                                 : argument == L"--sim-split"  ? sim::PROBE_MAX_BUSY_PIECES
                                                               : UINT32_MAX;
        const auto value = ParseCount(text, maximum);

        if (!value || (argument == L"--latency" && *value < 2) || (argument == L"--sim-split" && *value == 0) ||
            (argument == L"--target-fps" && *value < MIN_TARGET_FPS)) {
            return std::unexpected(std::format("{} の値が不正: {}", ToUtf8(argument), ToUtf8(text)));
        }

        if (argument == L"--frames")
            loop.frameLimit = *value;

        if (argument == L"--latency")
            loop.maxFrameLatency = *value;

        if (argument == L"--target-fps")
            loop.targetFps = *value;

        if (argument == L"--sim-load")
            loop.simLoad = *value;

        if (argument == L"--sim-split")
            loop.simSplit = *value;

        if (argument == L"--screenshot-tick")
            loop.screenshotTick = *value;

        if (argument == L"--save-points")
            loop.savePoints = *value;

        if (argument == L"--save-interval")
            loop.saveIntervalTicks = std::max(*value, 1u);

        return {};
    }

    // フレームのループの値なしの引数。当てはまらなければ false
    bool ParseFrameLoopFlag(std::wstring_view argument, frame::FrameLoopOptions& loop) {
        if (argument == L"--no-vsync")
            loop.vsync = false;
        else if (argument == L"--auto-click")
            loop.autoClick = true;
        else if (argument == L"--auto-trace")
            loop.autoTrace = true;
        else if (argument == L"--auto-push")
            loop.autoPush = true;
        else if (argument == L"--auto-ignite")
            loop.autoIgnite = true;
        else if (argument == L"--no-physics")
            loop.physics = false;
        else if (argument == L"--check-physics")
            loop.checkPhysics = true;
        else if (argument == L"--physics-compute")
            loop.physicsComputeBroadphase = true;
        else if (argument == L"--warp")
            loop.adapter = gpu::AdapterKind::Warp;
        else if (argument == L"--render-normal")
            loop.renderHighPriority = false;
        else if (argument == L"--editor")
            loop.editor = true;
        else if (argument == L"--auto-time")
            loop.autoTime = true;
        else if (argument == L"--auto-rewind")
            loop.autoRewind = true;
        else
            return false;

        return true;
    }

    std::expected<Options, std::string> ParseOptions(std::span<wchar_t*> arguments) {
        Options options;
        for (size_t i = 1; i < arguments.size(); ++i) {
            const std::wstring_view argument = arguments[i];
            const bool hasValue = i + 1 < arguments.size();
            if (argument == L"--caps")
                options.runCaps = true;
            else if (ParseFrameLoopFlag(argument, options.frameLoop))
                continue;
            else if ((argument == L"--frames" || argument == L"--latency" || argument == L"--target-fps" ||
                      argument == L"--sim-load" || argument == L"--sim-split" || argument == L"--screenshot-tick" ||
                      argument == L"--save-points" || argument == L"--save-interval") &&
                     hasValue) {
                const auto parsed = ParseFrameLoopCount(argument, arguments[++i], options.frameLoop);
                if (!parsed)
                    return std::unexpected(parsed.error());
            } else if ((argument == L"--view" || argument == L"--camera" || argument == L"--screenshot" ||
                        argument == L"--peek" || argument == L"--peek-depth") &&
                       hasValue) {
                const auto parsed = ParseViewOption(argument, arguments[++i], options.frameLoop);
                if (!parsed)
                    return std::unexpected(parsed.error());
            } else if ((argument == L"--trace-ticks" || argument == L"--trace-cells") && hasValue) {
                const auto parsed = ParseTraceRange(argument, arguments[++i], options);
                if (!parsed)
                    return std::unexpected(parsed.error());
            } else if (argument == L"--trace" && hasValue)
                options.frameLoop.tracePath = arguments[++i];
            else if (argument == L"--trace-dir" && hasValue)
                options.frameLoop.traceDirectory = arguments[++i];
            else if (argument == L"--record" && hasValue)
                options.frameLoop.recordPath = arguments[++i];
            else if (argument == L"--replay" && hasValue)
                options.frameLoop.replayPath = arguments[++i];
            else if (argument == L"--packages" && hasValue)
                options.frameLoop.packageRoot = arguments[++i];
            else if (argument == L"--log-dir" && hasValue)
                options.logDirectory = arguments[++i];
            else if (argument == L"--log-level" && hasValue) {
                const std::string name = ToUtf8(arguments[++i]);
                options.hasLogLevel = ParseLevel(name, options.logLevel);
                if (!options.hasLogLevel)
                    return std::unexpected(std::format("知らないログの重大度: {}", name));
            } else
                return std::unexpected(std::format("知らない引数: {}", ToUtf8(argument)));
        }

        ApplyTraceRange(options);
        if (options.frameLoop.traceDirectory.empty())
            options.frameLoop.traceDirectory = ExecutableDirectory() / L"traces";

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

        if (options->hasLogLevel)
            logger.SetMinLevel(options->logLevel);

        const fs::path logDirectory = options->logDirectory.empty() ? DefaultLogDirectory() : options->logDirectory;
        if (const auto logFile = OpenLogFile(logger, logDirectory))
            Log(Channel::Core, Level::Info, "Bicameral Engine  ログ: {}", ToUtf8(logFile->wstring()));
        else
            Log(Channel::Core, Level::Warning, "ログのファイルなしで続ける: {}", logFile.error());

        if (options->runCaps)
            return RunCapsProbe();

        return frame::RunFrameLoop(options->frameLoop);
    }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    bicameral::SingletonFinalizer::Finalize();  // ログは最初に作られるので最後に壊れる(作った順の逆)

    return exitCode;
}
