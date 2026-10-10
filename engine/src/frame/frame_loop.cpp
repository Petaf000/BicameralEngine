// frame_loop.cpp — 窓・2 つのキュー・予算ぶんのシミュ・待たない読み戻しでフレームを回す(T-0004・T-0012)。考え方は frame_loop.h。
//
// 1 フレームの流れ(CPU):
//   窓のメッセージ → スワップチェインの待ち(歩調)→ 経過時間を SimScheduler へ
//   → 終わったシミュのリストの読み戻し(フェンスが進んだ分だけ。待たない。単位の GPU 時間・ハッシュ・イベント)
//   → 窓の入力をカメラ・表示とクリックに分け(render/debug_view_controller。--editor なら ImGui が使っている入力は渡さない)、クリック(再生なら再生ファイル)をコマンドに → 予算ぶんの単位と、GPU のキューへ足すコマンドを記録して compute キューへ(ADR-0011)
//     (T が押されていたら、そのフレームから連鎖のトレースの範囲を変える。frame/trace_capture.h。T-0088)
//   → 描画を direct キューへ(終わっている最新の抽出を見せる。シミュを待たない)
//   → --editor ならパネル(editor/editor_overlay)を作って重ね、押された時間の操作を TimeControl へ(次のフレームから効く)→ Present
// 時間の操作(止める・1 刻み・速さ。editor/time_control)は「現実の時間から始めてよい刻みの数」だけを変え、世界には入らない(ADR-0035)。
//
// 抽出の 3 組の約束(06 §4): 抽出は 1 フレームに 1 回まで、そのフレームに投げた単位の後ろで刻みの境界の状態を写す。
//   抽出 n は組 n % 3 に書き、CPU から見て終わっている抽出が n − 2 以上のときだけ投げる(でなければそのフレームは写さない)。
//   描画は終わっている最新の抽出 d(≥ n − 2、< n)を読むので、書いている組と重ならない。
//   さらに抽出 n の前に、組 n % 3 を最後に読んだ描画のフェンスを compute キューに GPU の上で待たせる(m_lastRenderReading)。
//   描画がシミュを待つことは無い(D-201)。
#include "frame/frame_loop.h"
#include "frame/auto_reload.h"

#include <chrono>
#include <cmath>
#include <deque>
#include <expected>
#include <set>
#include <thread>

#include "core/aliases.h"
#include "core/log.h"
#include "core/unicode.h"
#include "editor/editor_overlay.h"
#include "editor/time_control.h"
#include "frame/sim_scheduler.h"
#include "frame/table_hot_reload.h"
#include "frame/trace_capture.h"
#include "gpu/com_ptr.h"
#include "gpu/queue.h"
#include "gpu/resources.h"
#include "gpu/swap_chain.h"
#include "platform/window.h"
#include "render/debug_view_controller.h"
#include "render/probe_view.h"
#include "render/screenshot.h"
#include "save/replay_session.h"
#include "script/reaction_table_loader.h"
#include "sim/physics_world.h"
#include "sim/probe_peek.h"
#include "sim/probe_sim.h"
#include "sim/probe_trace.h"
#include "sim/reaction_table.h"

namespace bicameral::frame {
    namespace {

        using Clock = chr::steady_clock;

        constexpr uint32_t FRAME_SLOT_COUNT = gpu::SwapChain::BUFFER_COUNT;
        constexpr uint32_t SIM_SLOT_COUNT = sim::ProbeSim::FRAME_SLOT_COUNT;
        constexpr uint32_t INITIAL_CLIENT_WIDTH = 1280;
        constexpr uint32_t INITIAL_CLIENT_HEIGHT = 720;
        constexpr uint32_t FRAME_WAIT_TIMEOUT_MS = 100;
        constexpr uint32_t CONSTANTS_BYTES = 256;
        constexpr uint32_t TIMESTAMPS_PER_FRAME = 2;
        constexpr uint32_t AUTO_CLICK_INTERVAL_FRAMES = 20;
        constexpr auto STATS_INTERVAL = chr::seconds(1);
        constexpr auto MINIMIZED_SLEEP = chr::milliseconds(16);

        // --- 窓の T で始める連鎖のトレース(T-0088)---
        constexpr uint64_t TRACE_KEY_TICKS = 60;        // 次の刻みから何刻み
        constexpr uint32_t TRACE_KEY_RADIUS_CELLS = 8;  // 最後につついたセルの周り ±何セル
        constexpr uint64_t AUTO_TRACE_FRAME = 30;       // --auto-trace が T を押すフレーム
        constexpr uint64_t AUTO_PUSH_FRAME = 60;        // --auto-push が押すフレーム(T-0098)

        // --auto-time(T-0023): 止める → 止まったかを確かめる → 1 刻みを 3 回 → 3 刻みだけ進んだかを確かめる → 速さを変えて動かす
        constexpr uint64_t AUTO_TIME_PAUSE_FRAME = 20;
        constexpr uint64_t AUTO_TIME_HOLD_FRAME = 36;  // 止まった刻みを控える(始めていた刻みが終わるのを待ってから)
        constexpr uint64_t AUTO_TIME_HOLD_CHECK_FRAME = 44;
        constexpr std::array<uint64_t, 3> AUTO_TIME_STEP_FRAMES = {45, 50, 55};
        constexpr uint64_t AUTO_TIME_STEP_CHECK_FRAME = 64;
        constexpr uint64_t AUTO_TIME_FASTER_FRAME = 65;
        constexpr uint64_t AUTO_TIME_RESUME_FRAME = 70;
        constexpr uint64_t AUTO_TIME_SLOWER_FRAME = 100;
        constexpr uint64_t AUTO_TIME_NORMAL_FRAME = 130;

        editor::TimeRequest AutoTimeRequest(uint64_t frame) {
            if (frame == AUTO_TIME_PAUSE_FRAME || frame == AUTO_TIME_RESUME_FRAME)
                return {.togglePause = true};

            if (rng::contains(AUTO_TIME_STEP_FRAMES, frame))
                return {.stepTicks = 1};

            if (frame == AUTO_TIME_FASTER_FRAME)
                return {.speedSteps = 1};

            if (frame == AUTO_TIME_SLOWER_FRAME)
                return {.speedSteps = -2};

            if (frame == AUTO_TIME_NORMAL_FRAME)
                return {.resetSpeed = true};

            return {};
        }

        // 押す力積(T-0098): 5000 N·s(積み木の 500 kg の箱に 10 m/s)。押された箱はその上の段の摩擦に抗って抜ける
        constexpr uint32_t PUSH_IMPULSE_MILLINEWTON_SECONDS = 5'000'000;

        double Milliseconds(Clock::duration duration) {
            return chr::duration<double, std::milli>(duration).count();
        }

        double TimestampMilliseconds(uint64_t ticks, uint64_t frequency) {
            if (frequency == 0)
                return 0.0;
            return static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency);
        }

        double TimestampMilliseconds(uint64_t begin, uint64_t end, uint64_t frequency) {
            return end < begin ? 0.0 : TimestampMilliseconds(end - begin, frequency);
        }

        // 描画のフレームごとに持つもの(バックバッファの番号 = スワップチェインの CurrentIndex)
        struct FrameSlot {
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList10> list;  // 大きさが変わったときだけ記録し直す
            ComPtr<ID3D12Resource> constants;          // アップロード(render::ProbeViewConstants)
            render::ProbeViewConstants* mappedConstants = nullptr;
            uint64_t renderFence = 0;  // このリストを最後に投げた direct のフェンスの値(0 = まだ)
        };

        // 投げたシミュのリスト(ProbeSim のフレームの枠ごと)。フェンスが fence に届いたら読む
        struct SimSubmission {
            uint64_t fence = 0;           // compute のフェンスの値
            uint64_t extraction = 0;      // このリストが書いた抽出の番号(0 = 写していない)
            uint64_t extractionTick = 0;  // その抽出が写した状態の刻み(S(この刻み))
            bool read = true;
        };

        // 描画が見せる抽出(CPU から見て終わっている最新)
        struct CompletedExtraction {
            uint64_t number = 0;  // 0 = まだ無い(組 0 は 0 のまま)
            uint64_t fence = 0;   // それを書いたシミュのリストの compute のフェンスの値
            uint64_t tick = 0;    // 写した状態の刻み(S(この刻み))
        };

        struct PendingClick {
            render::CellCoordinate cell;
            uint64_t tick = UINT64_MAX;  // 載せた刻み(まだなら UINT64_MAX)
            Clock::time_point time;
        };

        // 計測(1 秒ごとにログへ。最後に全体の要約)
        struct Stats {
            // --- CPU のフレーム ---
            uint64_t frames = 0;
            double cpuMilliseconds = 0.0;
            double cpuMaxMilliseconds = 0.0;
            double presentMilliseconds = 0.0;  // cpuMilliseconds のうち Present の中にいた時間
            uint64_t cpuWaits = 0;             // 描画の枠を使い回す前に CPU が GPU を待った回数(0 のはず)

            // --- シミュ ---
            uint64_t ticks = 0;  // 終わった刻み(読み戻したハッシュの数)
            uint64_t simSubmissions = 0;
            uint64_t units = 0;
            uint64_t events = 0;
            uint64_t skippedSubmissions = 0;  // シミュの枠が空いていないので投げなかったフレーム
            uint64_t skippedExtractions = 0;  // 抽出の 3 組の約束で写さなかったフレーム

            // --- GPU の時間 ---
            double simGpuMilliseconds = 0.0;  // シミュのリスト全体(抽出と読み戻しを含む)
            uint64_t simSubmissionsMeasured = 0;
            double renderGpuMilliseconds = 0.0;
            uint64_t renderFramesMeasured = 0;

            // 伝導のグラフのノードのカウンタ(足したもの・最大。T-0008)
            gpu::GraphStatsSnapshot conductGraph;

            // 刻みの中の単位ごとの GPU 時間の合計(番号 = 刻みの中の単位。エディタの性能のパネル。T-0143)
            std::vector<double> unitGpuMilliseconds;
        };

        // 作るもの一式(FrameLoop はこれを受け取ってから動く。作れなかったら FrameLoop を作らない)
        struct FrameLoopParts {
            // --- 窓と GPU ---
            std::unique_ptr<Window> window;
            gpu::Device device;
            gpu::Queue direct;
            gpu::Queue compute;
            gpu::SwapChain swapChain;

            // --- シミュと表示 ---
            sim::ProbeSim simulation;
            sim::ProbePeek peek;  // 覗き窓(T-0096)
            render::ProbeView view;

            // --replay のときだけ 1 つ(std::optional は tidy の警告が多いので使わない)
            std::vector<save::ReplayPlayer> replay;

            // 起動時に読んだ反応表(ホットリロードの元。T-0139)と、再生ファイルに残っていた差し替えの表(T-0193)
            std::shared_ptr<const script::LoadedReactionTable> reactionTable;
            std::vector<std::shared_ptr<const script::LoadedReactionTable>> replayTables;
        };

        class FrameLoop {
        public:
            FrameLoop(const FrameLoopOptions& options, FrameLoopParts&& parts)
                : m_options(options),
                  m_window(std::move(parts.window)),
                  m_device(std::move(parts.device)),
                  m_direct(std::move(parts.direct)),
                  m_compute(std::move(parts.compute)),
                  m_swapChain(std::move(parts.swapChain)),
                  m_sim(std::move(parts.simulation)),
                  m_peek(std::move(parts.peek)),
                  m_view(std::move(parts.view)),
                  m_viewController(sim::PROBE_GRID_SIZE, options.view, options.camera),
                  m_replay(std::move(parts.replay)),
                  m_tableReload({.packageRoot = options.packageRoot}, std::move(parts.reactionTable),
                                options.editor && options.replayPath.empty()),
                  m_scheduler(m_sim.UnitsPerTick(), {.targetFps = static_cast<double>(options.targetFps),
                                                     .maxUnitsPerFrame = sim::ProbeSim::MAX_UNITS_PER_FRAME}),
                  m_computeFrequency(m_compute.TimestampFrequency()),
                  m_directFrequency(m_direct.TimestampFrequency()) {
                // 再生ファイルの差し替えの表は、再生が印の版から探す(T-0193)
                for (const auto& table : parts.replayTables)
                    m_tableReload.AddKnown(table);

                // --screenshot-tick: その刻みの始めで世界を止める(写すのは S(その刻み)。T-0025)
                if (options.screenshotTick)
                    m_scheduler.SetStopTick(*options.screenshotTick);

                // --trace: 起動時の範囲(ProbeSim を作った時に GPU へ渡した)を集める
                if (!options.tracePath.empty())
                    m_traceCapture.emplace_back(options.tracePath, options.trace);

                // --peek: 起動時から覗く
                if (options.peek)
                    m_viewController.Peek(options.peekCell, options.peekDepth);

                // --check-physics: 窓と同じ場面の CPU の物理
                if (options.physics && options.checkPhysics) {
                    m_physicsCheck = std::make_unique<sim::PhysicsWorld>(sim::MakeProbeStackScene(),
                                                                         physics::PxDefaultParameters());
                }
            }

            [[nodiscard]] bool CreateFrameSlots();
            [[nodiscard]] bool CreateEditor();
            [[nodiscard]] bool CreateSavePoints();
            [[nodiscard]] int Run();

        private:
            // --- 描画 ---
            bool RecordRenderLists();
            bool HandleResize();
            void SubmitRender();
            [[nodiscard]] bool WantsScreenshot() const;
            [[nodiscard]] bool SubmitScreenshot();
            [[nodiscard]] bool WriteScreenshot();
            [[nodiscard]] bool NeedsStopExtraction() const;

            // --- シミュの読み戻し ---
            void CollectSimSubmissions();
            void ReportSimReadback(const sim::ProbeFrameReadback& readback);
            void ReportEvents(const sim::ProbeFrameReadback& readback);
            void CheckPhysics(const sim::ProbeTickHash& tickHash);
            void CollectFrameSlot(uint32_t slotIndex);

            // --- コマンド(クリックと再生)---
            void QueueClicks();
            void ApplyPeekChange();
            void QueuePushes();
            [[nodiscard]] std::vector<sim::ProbeCommand> TakeCommands(SimCursor start);
            [[nodiscard]] std::vector<sim::ProbeCommand> TakeClickCommands(uint64_t applyTick, uint32_t limit);
            void TakeTableSwaps(SimCursor start, uint32_t limit, std::vector<sim::ProbeCommand>& commands);
            [[nodiscard]] bool FinishReplay();
            bool SubmitSim();

            // --- 連鎖のトレース(--trace・T。frame/trace_capture.h)---
            void RequestTrace();
            void StartRequestedTrace(SimCursor start);
            void CollectTrace(const sim::ProbeFrameReadback& readback);
            [[nodiscard]] bool FinishTrace();

            // --- フレームの流れ ---
            bool RunFrame(Clock::time_point frameStart);
            int Finish(bool failed, Clock::time_point start);
            [[nodiscard]] bool IsDeviceLost() const;

            // --- エディタと時間の操作(T-0023)---
            void BuildEditor();
            [[nodiscard]] bool SubmitEditor();
            void ApplyTimeRequest(const editor::TimeRequest& request);
            void CheckAutoTime();
            void FilterEditorInput(std::vector<InputEvent>& events) const;
            [[nodiscard]] editor::EditorStatus MakeEditorStatus() const;
            [[nodiscard]] editor::GraphPanelStatus MakeGraphPanelStatus() const;

            // --- 巻き戻し(保存点 + 再生。T-0143・ADR-0036)---
            [[nodiscard]] std::string RewindUnavailableReason() const;
            [[nodiscard]] std::vector<uint64_t> SavePointTicks() const;
            void RequestRewind(uint64_t tick);
            [[nodiscard]] uint32_t PrepareRewind();
            [[nodiscard]] uint32_t SaveSlotFor(SimCursor start, uint32_t unitCount, uint32_t restoreFrom) const;
            [[nodiscard]] std::vector<sim::ProbeCommand> TakeLiveCommands(uint64_t applyTick, uint64_t endApplyTick,
                                                                          uint32_t limit);
            void CheckAutoRewind();
            [[nodiscard]] bool CreateAutoReload();

            // --- 計測 ---
            void LogStats(const Stats& stats, Clock::duration elapsed, std::string_view label) const;
            void AddStats(const Stats& frame);

            // --- 窓と GPU ---
            FrameLoopOptions m_options;
            std::unique_ptr<Window> m_window;
            gpu::Device m_device;
            gpu::Queue m_direct;
            gpu::Queue m_compute;
            gpu::SwapChain m_swapChain;

            // --- シミュと表示 ---
            sim::ProbeSim m_sim;
            sim::ProbePeek m_peek;
            render::ProbeView m_view;
            render::DebugViewController m_viewController;

            // --- 記録・再生・画面の保存 ---
            std::vector<save::ReplayPlayer> m_replay;  // 再生中なら 1 つ
            save::ReplayRecorder m_recorder;           // --record のときだけ使う
            TableHotReload
                m_tableReload;  // 反応表のホットリロード(--editor でファイルを見る。再生は印の表を当てる。T-0139)
            std::vector<sim::ProbeTableSwap> m_frameTableSwaps;   // 次に記録するフレームで当てる差し替え
            bool m_tableSwapFailed = false;                       // 再生の印の表を持っていない(止まる)
            std::vector<render::ScreenshotCapture> m_screenshot;  // --screenshot で写したら 1 つ

            // --- 連鎖のトレース ---
            std::vector<TraceCapture> m_traceCapture;  // 集めているトレース(0 か 1 つ。GPU の範囲は 1 つだけなので)
            bool m_traceRequested = false;             // T が押された(次に投げるフレームから始める)
            bool m_traceWriteFailed = false;           // 途中で書けなかった(終わるときに失敗にする)
            render::CellCoordinate m_lastPokedCell{.x = sim::PROBE_GRID_SIZE / 2,
                                                   .y = sim::PROBE_GRID_SIZE / 2,
                                                   .z = sim::PROBE_GRID_SIZE / 2};  // T の中心(まだなら格子の真ん中)

            // --- 描画の枠 ---
            std::array<FrameSlot, FRAME_SLOT_COUNT> m_frames;
            ComPtr<ID3D12QueryHeap> m_renderTimestamps;
            ComPtr<ID3D12Resource> m_renderTimestampReadback;
            uint64_t m_frameNumber = 0;  // 描いたフレームの数

            // --- シミュの投入と抽出 ---
            SimScheduler m_scheduler;
            std::array<SimSubmission, SIM_SLOT_COUNT> m_simSubmissions;
            uint64_t m_simSubmissionCount = 0;
            uint64_t m_lastExtraction = 0;               // 最後に投げた抽出の番号
            uint64_t m_lastExtractionTick = UINT64_MAX;  // 最後に投げた抽出が写す状態の刻み(まだなら UINT64_MAX)
            CompletedExtraction m_completedExtraction;

            // 抽出の組ごとに、最後にそれを読んだ描画のフェンスの値
            std::array<uint64_t, sim::PROBE_EXTRACTION_COUNT> m_lastRenderReading{};
            sim::ProbeTickHash m_latestHash;  // 最後に読み戻した刻みの状態のハッシュ

            // --- CPU の物理との突き合わせ(--check-physics。T-0098)---
            std::unique_ptr<sim::PhysicsWorld> m_physicsCheck;
            std::vector<sim::ProbeCommand> m_physicsCheckPushes;  // 投げた押すコマンド(刻みの順)
            uint64_t m_physicsCheckMismatches = 0;

            // --- コマンド ---
            std::vector<sim::ProbeCommand> m_pendingCommands;
            std::vector<PendingClick> m_clicks;
            uint32_t m_nextSequence = 0;

            // --- エディタと時間の操作(T-0023)---
            editor::TimeControl m_timeControl;
            std::unique_ptr<editor::EditorOverlay> m_editor;  // --editor のときだけ(窓より先に壊す)
            SimCursor m_autoTimeHold;                         // --auto-time: 止めた後に控えたカーソル
            bool m_autoTimeFailed = false;
            std::optional<AutoReload> m_autoReload;  // --auto-reload(T-0195)

            // --- 巻き戻し(T-0143)---
            uint32_t m_savePointCount = 0;
            uint64_t m_rewindRequest = UINT64_MAX;  // 次に投げるフレームで戻す保存点の刻み
            uint32_t m_rewindCount = 0;
            bool m_extractAfterRewind = false;  // 止めている間に戻したら、戻した状態を抽出だけのリストで見せる
            // 今の流れで GPU のキューへ足したコマンド(刻みの順。再生ファイルを流していない時)と、巻き戻した後に足し直すコマンド
            std::vector<sim::ProbeCommand> m_commandHistory;
            std::deque<sim::ProbeCommand> m_rewindFeed;

            // --- 計測 ---
            uint64_t m_computeFrequency = 0;
            uint64_t m_directFrequency = 0;
            Stats m_interval;      // この 1 秒
            Stats m_lastInterval;  // 前の 1 秒(エディタの表示)
            double m_lastIntervalSeconds = 0.0;
            Stats m_total;  // 全体
        };

        // --- 作る ---

        std::expected<std::unique_ptr<Window>, std::string> CreateWindowForLoop() {
            auto window = Window::Create(L"Bicameral Engine", INITIAL_CLIENT_WIDTH, INITIAL_CLIENT_HEIGHT);
            if (!window)
                return std::unexpected(window.error());

            (*window)->PumpMessages();
            (void)(*window)->TakeResized();  // 最初の WM_SIZE。スワップチェインはこの大きさで作る

            return window;
        }

        // デバイスの検証の設定。物理のシェーダーは GPU-based validation の計装が最初の実行で数分かかる(debug。gpu_physics_test と同じ)ので、
        // 物理を入れるときは切る(debug layer は残す。T-0098)
        gpu::DeviceOptions LoopDeviceOptions(const FrameLoopOptions& options) {
            gpu::DeviceOptions deviceOptions = gpu::DefaultDeviceOptions();
            if (options.physics && deviceOptions.gpuBasedValidation) {
                deviceOptions.gpuBasedValidation = false;
                Log(Channel::Gpu, Level::Info, "物理を入れるので GPU-based validation を切る(--no-physics なら有効)");
            }

            return deviceOptions;
        }

        // 仮の世界(物理の場面は積み木。T-0098。ProbeSim は作るときに読むだけ)
        std::expected<sim::ProbeSim, std::string> CreateSimulation(ID3D12Device5* device,
                                                                   const FrameLoopOptions& options,
                                                                   const sim::BakedReactionTable& reactionTable) {
            const sim::PhysicsScene physicsScene = sim::MakeProbeStackScene();

            return sim::ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, reactionTable,
                                         {.busyIterations = options.simLoad,
                                          .busyPieces = options.simSplit,
                                          .trace = options.trace,
                                          .traceCapacity = TRACE_CAPACITY_PER_FRAME,
                                          .physicsScene = options.physics ? &physicsScene : nullptr,
                                          .physicsOptions = {.broadphaseGraph = !options.physicsComputeBroadphase}});
        }

        // 世界の反応表: データのフォルダのパッケージ(既定は exe の横の data/packages)からベイクする(T-0157・ADR-0033)。
        // 読めない・形の誤り・検査で落ちたら、窓を開く前に起動を止める(半端な表で世界を動かさない)。
        // Mod が読めなかったとき・文献の反応熱と食い違うときは、警告をログに出して続ける(ADR-0031 の 3・02 §2 の 2)
        std::expected<std::shared_ptr<const script::LoadedReactionTable>, std::string> LoadWorldReactionTable(
            const FrameLoopOptions& options) {
            const auto start = chr::steady_clock::now();
            auto loaded = script::LoadReactionTable({.packageRoot = options.packageRoot});
            if (!loaded)
                return std::unexpected(loaded.error());

            const auto elapsedMs = chr::duration_cast<chr::milliseconds>(chr::steady_clock::now() - start).count();

            std::string packages;
            for (const std::string& name : loaded->loadOrder)
                packages += (packages.empty() ? "" : "・") + name;

            Log(Channel::Sim, Level::Info, "反応表: パッケージ {}(物質 {}・規則 {}・版 {:016x}{}・読んでベイク {} ms)",
                packages, loaded->table.species.size() - 1, loaded->table.rules.size(), loaded->tableVersion,
                loaded->modifiedWorld ? "・改造された世界" : "", elapsedMs);

            for (const script::PackageRejection& rejection : loaded->rejected)
                Log(Channel::Sim, Level::Warning, "パッケージ {} を読まなかった: {}", rejection.package,
                    rejection.reason);

            for (const std::string& warning : loaded->table.warnings)
                Log(Channel::Sim, Level::Warning, "反応表: {}", warning);

            return std::make_shared<const script::LoadedReactionTable>(std::move(*loaded));
        }

        // 世界の反応表の組: 起動時の表と、再生ファイルの差し替えの表(T-0193)
        struct WorldReactionTables {
            std::shared_ptr<const script::LoadedReactionTable> initial;
            std::vector<std::shared_ptr<const script::LoadedReactionTable>> swaps;
        };

        // 再生ファイルに残った表の中身から、パッケージのフォルダを読まずに表を作り直す(T-0170・T-0193・ADR-0050)
        std::expected<WorldReactionTables, std::string> RebuildReplayTables(const save::ReplayFile& replay) {
            WorldReactionTables tables;
            for (const save::ReplayTable& stored : replay.tables) {
                auto rebuilt = script::RebuildReactionTable(stored.content, stored.version);
                if (!rebuilt)
                    return std::unexpected(rebuilt.error());

                rebuilt->loadOrder = stored.loadOrder;
                rebuilt->modifiedWorld = stored.modifiedWorld;
                auto shared = std::make_shared<const script::LoadedReactionTable>(std::move(*rebuilt));
                if (stored.version == replay.tableVersion)
                    tables.initial = std::move(shared);
                else
                    tables.swaps.push_back(std::move(shared));
            }

            std::string packages;
            for (const std::string& name : tables.initial->loadOrder)
                packages += (packages.empty() ? "" : "・") + name;

            Log(Channel::Sim, Level::Info,
                "反応表: 再生ファイルの表を使う(パッケージのフォルダは読まない)。版 {:016x}(パッケージ {}{}・物質 "
                "{}・規則 {})"
                "・差し替える表 {} 個",
                tables.initial->tableVersion, packages, tables.initial->modifiedWorld ? "・改造された世界" : "",
                tables.initial->table.species.size() - 1, tables.initial->table.rules.size(), tables.swaps.size());

            return tables;
        }

        // 世界の反応表: 再生ファイルに表の中身があればそれを、無ければパッケージから読む。
        // 表の中身が無い再生ファイル(古い形式の版 1)は今のパッケージの表で再生する。版が書いてあって違えば止める(T-0170)
        std::expected<WorldReactionTables, std::string> LoadWorldReactionTables(const FrameLoopOptions& options,
                                                                                const save::ReplayFile* replay) {
            if (replay != nullptr && !replay->tables.empty())
                return RebuildReplayTables(*replay);

            auto loaded = LoadWorldReactionTable(options);
            if (!loaded)
                return std::unexpected(loaded.error());

            if (replay == nullptr)
                return WorldReactionTables{.initial = std::move(*loaded)};

            if (replay->tableVersion == 0) {
                Log(Channel::Sim, Level::Warning,
                    "再生ファイルに反応表の版と中身が無い(古い形式)。今のパッケージの表(版 {:016x})で再生する"
                    "(記録した時と表が違えば、ハッシュが合わない)",
                    (*loaded)->tableVersion);
            } else if (replay->tableVersion != (*loaded)->tableVersion) {
                return std::unexpected(
                    std::format("再生ファイルの反応表は版 {:016x} だが、今のパッケージの表は版 {:016x}"
                                "(表の中身が再生ファイルに無いので、記録した表で再生できない)",
                                replay->tableVersion, (*loaded)->tableVersion));
            }

            return WorldReactionTables{.initial = std::move(*loaded)};
        }

        std::expected<FrameLoopParts, std::string> CreateParts(const FrameLoopOptions& options) {
            // --- 再生ファイル(表の中身を持っていれば、パッケージより先に要る)と反応表 ---
            std::vector<save::ReplayPlayer> replay;
            if (!options.replayPath.empty()) {
                auto player = save::ReplayPlayer::Load(options.replayPath);
                if (!player)
                    return std::unexpected(player.error());

                replay.push_back(std::move(*player));
            }

            auto reactionTables = LoadWorldReactionTables(options, replay.empty() ? nullptr : &replay.front().File());
            if (!reactionTables)
                return std::unexpected(reactionTables.error());

            const std::shared_ptr<const script::LoadedReactionTable>& reactionTable = reactionTables->initial;

            auto window = CreateWindowForLoop();
            if (!window)
                return std::unexpected(window.error());

            gpu::DeviceOptions deviceOptions = LoopDeviceOptions(options);
            deviceOptions.presentMonitor = MonitorFromWindow((*window)->Handle(), MONITOR_DEFAULTTONEAREST);
            auto device = gpu::Device::Create(options.adapter, deviceOptions);
            if (!device)
                return std::unexpected(device.error());

            ID3D12Device5* native = device->Get();

            // 描画のキューは優先度を上げる(効果は無かったが害も無い。R-LOOP-2。docs/perf.md)
            auto direct = gpu::Queue::Create(
                native, D3D12_COMMAND_LIST_TYPE_DIRECT, L"Render",
                options.renderHighPriority ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);

            auto compute = gpu::Queue::Create(native, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"Sim");
            if (!direct || !compute)
                return std::unexpected("キューを作れない");

            auto swapChain = gpu::SwapChain::Create(native, device->Factory(), direct->Native(), (*window)->Handle(),
                                                    options.maxFrameLatency);
            if (!swapChain)
                return std::unexpected(swapChain.error());

            auto simulation = CreateSimulation(native, options, reactionTable->table);
            if (!simulation)
                return std::unexpected(simulation.error());

            auto peek = sim::ProbePeek::Create(native, reactionTable->table);
            if (!peek)
                return std::unexpected(peek.error());

            auto view = render::ProbeView::Create(native, gpu::SwapChain::FORMAT);
            if (!view)
                return std::unexpected(view.error());

            return FrameLoopParts{.window = std::move(*window),
                                  .device = std::move(*device),
                                  .direct = std::move(*direct),
                                  .compute = std::move(*compute),
                                  .swapChain = std::move(*swapChain),
                                  .simulation = std::move(*simulation),
                                  .peek = std::move(*peek),
                                  .view = std::move(*view),
                                  .replay = std::move(replay),
                                  .reactionTable = reactionTable,
                                  .replayTables = std::move(reactionTables->swaps)};
        }

        bool FrameLoop::CreateFrameSlots() {
            ID3D12Device5* device = m_device.Get();
            for (FrameSlot& slot : m_frames) {
                if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&slot.allocator))) ||
                    FAILED(device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                                      IID_PPV_ARGS(&slot.list)))) {
                    return false;
                }

                slot.constants = gpu::CreateBuffer(device, CONSTANTS_BYTES, gpu::BufferKind::Upload);
                if (!slot.constants)
                    return false;

                void* mapped = nullptr;
                const D3D12_RANGE noRead{};
                if (FAILED(slot.constants->Map(0, &noRead, &mapped)))
                    return false;

                slot.mappedConstants = static_cast<render::ProbeViewConstants*>(mapped);
            }

            const D3D12_QUERY_HEAP_DESC queryDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
                                                  .Count = FRAME_SLOT_COUNT * TIMESTAMPS_PER_FRAME};
            if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_renderTimestamps))))
                return false;

            m_renderTimestampReadback = gpu::CreateBuffer(device, uint64_t{FRAME_SLOT_COUNT} * TIMESTAMPS_PER_FRAME * 8,
                                                          gpu::BufferKind::Readback);

            return m_renderTimestampReadback != nullptr && RecordRenderLists();
        }

        // バックバッファごとに描画のリストを記録する(作るときと、大きさが変わったときだけ)
        bool FrameLoop::RecordRenderLists() {
            for (uint32_t index = 0; index < FRAME_SLOT_COUNT; ++index) {
                FrameSlot& slot = m_frames[index];
                if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr)))
                    return false;

                ID3D12GraphicsCommandList10* list = slot.list.Get();
                const uint32_t firstQuery = index * TIMESTAMPS_PER_FRAME;
                list->EndQuery(m_renderTimestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery);

                m_view.Record(list,
                              {.backBuffer = m_swapChain.BackBuffer(index),
                               .renderTargetView = m_swapChain.RenderTargetView(index),
                               .width = m_swapChain.Width(),
                               .height = m_swapChain.Height()},
                              {.extraction0 = m_sim.Extraction(0)->GetGPUVirtualAddress(),
                               .extraction1 = m_sim.Extraction(1)->GetGPUVirtualAddress(),
                               .extraction2 = m_sim.Extraction(2)->GetGPUVirtualAddress(),
                               .constants = slot.constants->GetGPUVirtualAddress()});

                list->EndQuery(m_renderTimestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery + 1);
                list->ResolveQueryData(m_renderTimestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery,
                                       TIMESTAMPS_PER_FRAME, m_renderTimestampReadback.Get(), uint64_t{firstQuery} * 8);
                if (FAILED(list->Close()))
                    return false;
            }

            return true;
        }

        bool FrameLoop::HandleResize() {
            if (m_window->IsMinimized())
                return true;

            // バックバッファを使うリストを GPU が全部終えてから(ここだけ CPU が待つ。Present の分も待つ)
            if (!m_direct.Flush() || !m_compute.Flush())
                return false;

            CollectSimSubmissions();
            for (uint32_t index = 0; index < FRAME_SLOT_COUNT; ++index)
                CollectFrameSlot(index);

            if (!m_swapChain.Resize(m_device.Get(), m_window->ClientWidth(), m_window->ClientHeight()))
                return false;

            Log(Channel::Render, Level::Info, "窓の大きさ: {}×{}", m_swapChain.Width(), m_swapChain.Height());

            return RecordRenderLists();
        }

        // --- 読み戻し(待たない)---

        // 投げた順に読む(枠は投げた順に回るので、次に使う枠がいちばん古い)。枠の番号の順に読むと、2 つが同時に終わっていたとき
        // 新しい方の刻みのハッシュが先に届き、再生の突き合わせが古い刻みを「戻ってこなかった」と数える(T-0096 で見つけた)
        void FrameLoop::CollectSimSubmissions() {
            for (uint32_t index = 0; index < SIM_SLOT_COUNT; ++index) {
                const auto slot = static_cast<uint32_t>((m_simSubmissionCount + index) % SIM_SLOT_COUNT);
                SimSubmission& submission = m_simSubmissions[slot];
                if (submission.read || !m_compute.IsComplete(submission.fence))
                    continue;

                submission.read = true;
                ReportSimReadback(m_sim.ReadFrame(slot));
                if (submission.extraction > m_completedExtraction.number) {
                    m_completedExtraction = {
                        .number = submission.extraction, .fence = submission.fence, .tick = submission.extractionTick};
                }
            }
        }

        // 単位ごとの GPU 時間を SimScheduler へ。終えた刻みのハッシュとイベント
        void FrameLoop::ReportSimReadback(const sim::ProbeFrameReadback& readback) {
            const uint32_t unitsPerTick = m_sim.UnitsPerTick();
            m_interval.unitGpuMilliseconds.resize(unitsPerTick);
            for (size_t index = 0; index < readback.unitGpuTicks.size(); ++index) {
                const auto unit = static_cast<uint32_t>((readback.firstUnit + index) % unitsPerTick);
                const double milliseconds = TimestampMilliseconds(readback.unitGpuTicks[index], m_computeFrequency);
                m_scheduler.ReportUnitTime(unit, milliseconds);
                m_interval.unitGpuMilliseconds[unit] += milliseconds;
            }

            gpu::AccumulateGraphStats(m_interval.conductGraph, readback.graphStats);
            m_interval.simGpuMilliseconds += TimestampMilliseconds(readback.gpuBeginTimestamp, readback.gpuEndTimestamp,
                                                                   m_computeFrequency);
            ++m_interval.simSubmissionsMeasured;
            m_interval.ticks += readback.hashes.size();
            if (!readback.hashes.empty())
                m_latestHash = readback.hashes.back();

            for (const sim::ProbeTickHash& tickHash : readback.hashes) {
                // 再生ファイルにはセルと物を合わせた要約を入れる(T-0098)
                if (!m_options.recordPath.empty())
                    m_recorder.AddHash(tickHash.tick, tickHash.WorldHash());

                if (!m_replay.empty())
                    m_replay.front().CheckHash(tickHash.tick, tickHash.WorldHash());

                if (m_physicsCheck)
                    CheckPhysics(tickHash);
            }

            ReportEvents(readback);
            CollectTrace(readback);
        }

        // 押すコマンド(probe_sim.hlsli の PROBE_COMMAND_TYPE_PUSH の payload)を CPU の物理へ
        void PushFromCommand(sim::PhysicsWorld& world, const sim::ProbeCommand& command) {
            const auto word64 = [&](size_t index) {
                return static_cast<int64_t>(uint64_t{command.payload[index]} |
                                            (uint64_t{command.payload[index + 1]} << 32));
            };
            const auto direction = [&](size_t index) {
                return static_cast<int64_t>(static_cast<int32_t>(command.payload[index]));
            };

            (void)world.Push(physics::PxMakeVec3(word64(0), word64(2), word64(4)),
                             physics::PxMakeVec3(direction(6), direction(7), direction(8)), command.payload[9]);
        }

        // CPU の物理を S(tickHash.tick) まで進めて、物のハッシュを比べる(押すコマンドは同じ刻みの Step の前に)
        void FrameLoop::CheckPhysics(const sim::ProbeTickHash& tickHash) {
            while (m_physicsCheck->Tick() < tickHash.tick) {
                const uint64_t tick = m_physicsCheck->Tick();
                for (const sim::ProbeCommand& command : m_physicsCheckPushes) {
                    if (command.targetTick == tick)
                        PushFromCommand(*m_physicsCheck, command);
                }

                m_physicsCheck->Step();
            }

            if (m_physicsCheck->StateHash() == tickHash.bodyHash)
                return;

            if (m_physicsCheckMismatches++ == 0) {
                Log(Channel::Sim, Level::Error, "物理の突き合わせ: S({}) の物が CPU と違う(GPU {:016x} CPU {:016x})",
                    tickHash.tick, tickHash.bodyHash, m_physicsCheck->StateHash());
            }
        }

        // つつきが適用されたことをログへ(クリックから CPU に戻るまでの時間つき)。イベントは (刻み, 種類, 場所) の順に届く
        void FrameLoop::ReportEvents(const sim::ProbeFrameReadback& readback) {
            const auto now = Clock::now();
            for (const sim::ProbeEvent& event : readback.events) {
                ++m_interval.events;
                if (event.type == sim::PROBE_EVENT_COMMAND_LATE) {
                    Log(Channel::Sim, Level::Warning, "種類 {} のコマンドが刻み {} の適用に遅れて届いた(捨てた)",
                        event.place, event.tick);
                    continue;
                }

                if (event.type == sim::PROBE_EVENT_PLACE_APPLIED) {
                    if (m_editor)
                        m_editor->Brush().NotePlaced(event);
                    else
                        Log(Channel::Sim, Level::Info, "置く: 刻み {} で ({}, {}, {}) を中心に置いた", event.tick,
                            event.PokeX(), event.PokeY(), event.PokeZ());

                    continue;
                }

                if (event.type == sim::PROBE_EVENT_BODY_PUSHED) {
                    if (event.place == sim::PROBE_PUSH_NOTHING)
                        Log(Channel::Sim, Level::Info, "押す: 刻み {} で光線の先に動く物が無かった", event.tick);
                    else
                        Log(Channel::Sim, Level::Info, "押す: 刻み {} で物 {} を押した", event.tick, event.place);

                    continue;
                }

                const render::CellCoordinate cell{.x = event.PokeX(), .y = event.PokeY(), .z = event.PokeZ()};
                const auto click = rng::find_if(m_clicks, [&](const PendingClick& pending) {
                    return pending.tick == event.tick && pending.cell == cell;
                });

                const double latency = click != m_clicks.end() ? Milliseconds(now - click->time) : 0.0;

                Log(Channel::Sim, Level::Info,
                    "つつき ({}, {}, {}) を刻み {} で適用(クリックから CPU に戻るまで {:.1f} ms)", cell.x, cell.y,
                    cell.z, event.tick, latency);

                if (click != m_clicks.end())
                    m_clicks.erase(click);
            }

            if (readback.droppedEventCount > 0) {
                Log(Channel::Sim, Level::Warning, "イベントのリングが溢れて {} 件を落とした",
                    readback.droppedEventCount);
            }
        }

        // 描画の枠を使い回す前に: 前にその枠で投げたリストの終わりを確かめ、タイムスタンプを読む
        void FrameLoop::CollectFrameSlot(uint32_t slotIndex) {
            FrameSlot& slot = m_frames[slotIndex];
            if (slot.renderFence == 0)
                return;

            if (!m_direct.IsComplete(slot.renderFence)) {
                ++m_interval.cpuWaits;  // スワップチェインの待ちがあるので、ふつうは起きない
                m_direct.WaitCpu(slot.renderFence);
            }

            std::array<uint64_t, size_t{FRAME_SLOT_COUNT} * TIMESTAMPS_PER_FRAME> timestamps{};
            if (gpu::ReadBuffer(m_renderTimestampReadback.Get(), std::as_writable_bytes(std::span(timestamps)))) {
                const size_t first = size_t{slotIndex} * TIMESTAMPS_PER_FRAME;
                const double milliseconds = TimestampMilliseconds(timestamps[first], timestamps[first + 1],
                                                                  m_directFrequency);
                m_scheduler.ReportRenderTime(milliseconds);
                m_interval.renderGpuMilliseconds += milliseconds;
                ++m_interval.renderFramesMeasured;
            }

            slot.renderFence = 0;
        }

        // --- 入力 → コマンド ---

        // 窓の入力: カメラと表示は再生中も動かせる。つつき(左クリックが断面に当たったセル)と自動のクリックはコマンドに
        void FrameLoop::QueueClicks() {
            std::vector<InputEvent> events = m_window->TakeInputEvents();
            FilterEditorInput(events);
            if (m_options.autoTrace && m_frameNumber == AUTO_TRACE_FRAME)
                events.push_back({.kind = InputKind::KeyDown, .key = 'T'});  // 人がいない確認で T の流れを通す

            std::vector<render::CellCoordinate> cells = m_viewController.HandleInput(events, m_swapChain.Width(),
                                                                                     m_swapChain.Height());

            // トレースと覗き窓は世界に入らない(View)ので、再生中も使える
            if (m_viewController.TakeTraceRequest())
                RequestTrace();

            ApplyPeekChange();

            // 再生中は窓の操作を世界に入れない(世界は再生ファイルのコマンドだけで進む)
            if (!m_replay.empty())
                return;

            // 筆を持っている間は、左クリックが当たったセルを「置く」コマンドに(T-0222。自動のクリックはつつきのまま)
            if (m_editor && m_editor->Brush().Holding()) {
                for (const render::CellCoordinate& cell : cells)
                    m_pendingCommands.push_back(
                        m_editor->Brush().MakeCommand(cell.x, cell.y, cell.z, m_nextSequence++));

                cells.clear();
            }

            if (m_options.autoPlace && m_editor)
                rng::move(m_editor->Brush().TakeAutoCommands(m_frameNumber, m_nextSequence),
                          std::back_inserter(m_pendingCommands));

            const bool autoClick = m_options.autoClick && m_frameNumber % AUTO_CLICK_INTERVAL_FRAMES == 0;
            const bool autoIgnite = m_options.autoIgnite && m_frameNumber == 0;  // 下の最初の 1 回と同じ場所
            if (autoClick || autoIgnite) {
                // z = PROBE_VIEW_Z の面の決まった場所を順に押す(人がいない確認用。表示やカメラに依らない)。
                // 最初の 1 回は木箱の壁 (28, 32)(初めの世界。sim/probe_sim.cpp)に火をつける(T-0089)
                const auto step = static_cast<uint32_t>(m_frameNumber / AUTO_CLICK_INTERVAL_FRAMES);
                const uint32_t u = step == 0 ? 28 : (step * 37 % 16 + 1) * sim::PROBE_GRID_SIZE / 18;
                const uint32_t v = step == 0 ? 32 : (step * 11 % 16 + 1) * sim::PROBE_GRID_SIZE / 18;
                cells.push_back(render::CellOnSlice(2, sim::PROBE_VIEW_Z, u, v));
            }

            QueuePushes();

            const auto now = Clock::now();
            for (const render::CellCoordinate& cell : cells) {
                m_pendingCommands.push_back(sim::MakePokeCommand(0, m_nextSequence++, cell.x, cell.y, cell.z));
                m_clicks.push_back({.cell = cell, .time = now});
                m_lastPokedCell = cell;
            }
        }

        // Shift + 左クリックの光線(と --auto-push)を押すコマンドに(T-0098)。光線は格子の座標なので物理の座標へ直す
        // (probe_sim.hlsli の対応: y を裏返し、1 セル = 0.5 m)。どの物に当たるかは GPU が決める(CPU は物の場所を知らない。D-107)
        void FrameLoop::QueuePushes() {
            std::vector<render::CameraRay> rays = m_viewController.TakePushRays();
            if (m_options.autoPush && m_frameNumber == AUTO_PUSH_FRAME) {
                // 積み木の 5 段目(物理の座標 x = 10 m・y = 5.5 m・z = 16.25 m。格子の (20, 53, 32.5))を −z の側から
                rays.push_back({.origin = {20.0f, 53.0f, 0.0f}, .direction = {0.0f, 0.0f, 1.0f}});
            }

            for (const render::CameraRay& ray : rays) {
                const float length = std::sqrt((ray.direction.x * ray.direction.x) +
                                               (ray.direction.y * ray.direction.y) +
                                               (ray.direction.z * ray.direction.z));
                if (length <= 0.0f)
                    continue;

                constexpr auto UNITS_PER_CELL = static_cast<double>(1u << sim::PROBE_PHYSICS_CELL_SHIFT);
                constexpr auto DIRECTION_ONE = static_cast<double>(1u << 30);
                const std::array<int64_t, 3> origin = {
                    std::llround(ray.origin.x * UNITS_PER_CELL),
                    std::llround((static_cast<double>(sim::PROBE_GRID_SIZE) - ray.origin.y) * UNITS_PER_CELL),
                    std::llround(ray.origin.z * UNITS_PER_CELL)};
                const std::array<int32_t, 3> direction = {
                    static_cast<int32_t>(std::lround(ray.direction.x / length * DIRECTION_ONE)),
                    static_cast<int32_t>(std::lround(-ray.direction.y / length * DIRECTION_ONE)),
                    static_cast<int32_t>(std::lround(ray.direction.z / length * DIRECTION_ONE))};

                m_pendingCommands.push_back(
                    sim::MakePushCommand(0, m_nextSequence++, origin, direction, PUSH_IMPULSE_MILLINEWTON_SECONDS));
            }
        }

        // 覗く場所が変わったら覗き窓へ(次の抽出から効く。T-0096)
        void FrameLoop::ApplyPeekChange() {
            static_assert(render::PEEK_MAX_DEPTH == sim::PEEK_LEVEL_COUNT);
            if (!m_viewController.TakePeekChange())
                return;

            const render::PeekView& peek = m_viewController.Peeking();
            if (peek.peeking)
                m_peek.Look({.x = peek.cell.x, .y = peek.cell.y, .z = peek.cell.z});
            else
                m_peek.Stop();
        }

        // このフレームに GPU のキューへ足すコマンド(クリックか再生ファイルから)。記録するならここで控える。
        // 適用する刻みは「まだ記録していない最初の適用の単位の刻み」以上(06 §3)。コマンドはそこまで GPU のキューで待つので、
        // このフレームに適用の単位が無くてもよい。キューの空きを超える分は次のフレームへ
        std::vector<sim::ProbeCommand> FrameLoop::TakeCommands(SimCursor start) {
            const uint64_t applyTick = sim::ProbeSim::NextApplyTick(start.tick, start.unit);
            const uint32_t limit = std::min(m_sim.FreeCommandSlots(), sim::PROBE_MAX_COMMANDS);

            // このフレームの単位が記録する最後の適用の次の刻み(巻き戻した後のコマンドは、ここより前の分だけ足す)
            const uint64_t endApplyTick = sim::ProbeSim::NextApplyTick(m_scheduler.Cursor().tick,
                                                                       m_scheduler.Cursor().unit);
            std::vector<sim::ProbeCommand> commands = m_replay.empty()
                                                          ? TakeLiveCommands(applyTick, endApplyTick, limit)
                                                          : m_replay.front().TakeCommands(applyTick, limit);

            TakeTableSwaps(start, limit, commands);
            if (!m_options.recordPath.empty())
                m_recorder.AddCommands(commands);

            if (m_physicsCheck) {
                rng::copy_if(commands, std::back_inserter(m_physicsCheckPushes), [](const sim::ProbeCommand& command) {
                    return command.type == sim::PROBE_COMMAND_TYPE_PUSH;
                });
            }

            return commands;
        }

        // 反応表の差し替え(T-0139・ADR-0047): 読み直した表を、このフレームの最初の適用の単位の刻みへ(印のコマンドを足すので
        // 記録にも残る)。再生は印の版の表を探す。当てる差し替えは m_frameTableSwaps(SubmitSim が RecordFrame に渡す)
        void FrameLoop::TakeTableSwaps(SimCursor start, uint32_t limit, std::vector<sim::ProbeCommand>& commands) {
            const uint32_t unitsPerTick = m_sim.UnitsPerTick();
            const uint64_t firstUnit = (start.tick * unitsPerTick) + start.unit;
            const uint64_t endUnit = (m_scheduler.Cursor().tick * unitsPerTick) + m_scheduler.Cursor().unit;
            auto swaps = m_tableReload.TakeSwaps(firstUnit, static_cast<uint32_t>(endUnit - firstUnit), unitsPerTick,
                                                 !m_replay.empty(), commands.size() < limit, m_nextSequence, commands);
            if (!swaps) {
                Log(Channel::Sim, Level::Error, "{}", swaps.error());
                m_tableSwapFailed = true;
                m_frameTableSwaps.clear();
                return;
            }

            m_frameTableSwaps = std::move(*swaps);

            // 覗き窓も同じ表へ(世界がその刻みを過ぎた境界を抽出する時に替える。T-0194)
            for (const sim::ProbeTableSwap& swap : m_frameTableSwaps) {
                if (swap.table != nullptr)
                    m_peek.QueueTableSwap(swap.tick, *swap.table);
            }
        }

        std::vector<sim::ProbeCommand> FrameLoop::TakeClickCommands(uint64_t applyTick, uint32_t limit) {
            const size_t count = std::min<size_t>(m_pendingCommands.size(), limit);
            std::vector<sim::ProbeCommand> commands(m_pendingCommands.begin(),
                                                    m_pendingCommands.begin() + static_cast<std::ptrdiff_t>(count));
            m_pendingCommands.erase(m_pendingCommands.begin(),
                                    m_pendingCommands.begin() + static_cast<std::ptrdiff_t>(count));

            for (sim::ProbeCommand& command : commands) {
                command.targetTick = applyTick;
                if (command.type != sim::PROBE_COMMAND_TYPE_POKE)
                    continue;  // クリックから戻るまでの時間を測るのはつつきだけ

                const render::CellCoordinate cell{
                    .x = command.payload[0], .y = command.payload[1], .z = command.payload[2]};
                const auto click = rng::find_if(m_clicks, [&](const PendingClick& pending) {
                    return pending.tick == UINT64_MAX && pending.cell == cell;
                });

                if (click != m_clicks.end())
                    click->tick = applyTick;
            }

            return commands;
        }

        // --- 投げる ---

        // 予算ぶんの単位を記録して compute キューへ(ADR-0011)。記録に失敗したら false(直せない誤り)
        bool FrameLoop::SubmitSim() {
            const auto slot = static_cast<uint32_t>(m_simSubmissionCount % SIM_SLOT_COUNT);
            SimSubmission& submission = m_simSubmissions[slot];
            if (!submission.read) {  // その枠のリストがまだ GPU にある(読めていない)
                ++m_interval.skippedSubmissions;

                return true;
            }

            // 巻き戻し(T-0143): GPU を待って読み戻しを全部読んでから、カーソルを保存点の刻みへ戻す。このフレームの先頭で世界を戻す
            const uint32_t restoreFrom = m_rewindRequest != UINT64_MAX ? PrepareRewind() : sim::NO_SAVE_POINT;
            const bool restoring = restoreFrom != sim::NO_SAVE_POINT;

            const SimCursor start = m_scheduler.Cursor();
            const uint32_t unitCount = m_scheduler.TakeUnits();
            // 止まる刻みに着いたのに、その状態をまだ抽出していない(最後のリストが抽出を飛ばした)か、
            // 止めたまま戻した状態をまだ見せていないなら、抽出だけのリストを投げる
            const bool extractOnly = unitCount == 0 && (NeedsStopExtraction() || m_extractAfterRewind);
            if (unitCount == 0 && !extractOnly && !restoring)
                return true;

            const uint64_t extraction = m_lastExtraction + 1;
            const bool extract = m_completedExtraction.number + 2 >= extraction;  // 抽出の 3 組の約束(ファイルの先頭)
            if (extractOnly && !extract && !restoring)
                return true;  // 描画が抽出を読み終えるのを待つ(次のフレームでもう一度)

            if (!extract)
                ++m_interval.skippedExtractions;

            const std::vector<sim::ProbeCommand> commands = TakeCommands(start);
            if (m_tableSwapFailed)
                return false;

            StartRequestedTrace(start);
            // 抽出は S(終わった後のカーソルの刻み)。刻みの途中で終わったら、その刻みの始めの状態(ProbeSim::RecordFrame)
            const uint64_t extractionTick = m_scheduler.Cursor().tick;

            const auto extractionTarget = static_cast<uint32_t>(extraction % sim::PROBE_EXTRACTION_COUNT);

            ID3D12CommandList* list = m_sim.RecordFrame(
                slot, {.firstTick = start.tick,
                       .firstUnit = start.unit,
                       .unitCount = unitCount,
                       .extract = extract,
                       .extractionTarget = extractionTarget,
                       .commands = commands,
                       .afterExtract = [this](auto* simList,
                                              const auto& context) { m_peek.RecordAfterExtract(simList, context); },
                       .saveTo = SaveSlotFor(start, unitCount, restoreFrom),
                       .restoreFrom = restoreFrom,
                       .tableSwaps = m_frameTableSwaps});

            if (list == nullptr)
                return false;

            // 表を差し替えたら、保存点(このフレームの先頭で写したものも)は前の表の世界なので捨てる(戻ると表と食い違う。ADR-0047)
            if (!m_frameTableSwaps.empty()) {
                m_sim.DiscardSavePointsAfter(0);
                m_frameTableSwaps.clear();
            }

            if (extract) {
                // 抽出の組を描画が読み終えるまで、GPU の上で待ってから走る
                m_compute.GpuWait(m_direct, m_lastRenderReading[extractionTarget]);
                m_lastExtraction = extraction;
                m_lastExtractionTick = extractionTick;
                m_extractAfterRewind = false;  // 戻した後の流れの状態を見せた
            }

            submission = {.fence = m_compute.Submit(list),
                          .extraction = extract ? extraction : 0,
                          .extractionTick = extractionTick,
                          .read = false};
            ++m_simSubmissionCount;
            ++m_interval.simSubmissions;
            m_interval.units += unitCount;

            return true;
        }

        void FrameLoop::SubmitRender() {
            const uint32_t index = m_swapChain.CurrentIndex();
            CollectFrameSlot(index);
            FrameSlot& slot = m_frames[index];

            // 見せる抽出: CPU から見て終わっている最新(シミュを待たない。抽出の 3 組の約束。ファイルの先頭)
            const auto extraction = static_cast<uint32_t>(m_completedExtraction.number % sim::PROBE_EXTRACTION_COUNT);
            *slot.mappedConstants = m_viewController.Constants(extraction, m_swapChain.Width(), m_swapChain.Height());
            m_direct.GpuWait(m_compute, m_completedExtraction.fence);
            slot.renderFence = m_direct.Submit(slot.list.Get());
            m_lastRenderReading[extraction] = slot.renderFence;
        }

        // 止まる刻み(--screenshot-tick)に着いたが、その状態 S(刻み) をまだ抽出していない
        bool FrameLoop::NeedsStopExtraction() const {
            return m_options.screenshotTick && m_scheduler.ReachedStopTick() &&
                   m_lastExtractionTick != m_scheduler.Cursor().tick;
        }

        // このフレームを写すか。--screenshot-tick なら、止まる刻みの状態 S(刻み) をこのフレームの描画が見せている時(T-0025)。
        // そうでなければ最後のフレーム(--frames。シミュの進み方は実行ごとに違うので、写る刻みは決まらない)
        bool FrameLoop::WantsScreenshot() const {
            if (m_options.screenshotPath.empty() || !m_screenshot.empty())
                return false;

            if (!m_options.screenshotTick)
                return m_frameNumber + 1 == m_options.frameLimit;

            return m_scheduler.ReachedStopTick() && m_completedExtraction.number > 0 &&
                   m_completedExtraction.tick == *m_options.screenshotTick;
        }

        // --screenshot: 写すフレームの描画の後・Present の前に、バックバッファを読み戻しへ写す
        bool FrameLoop::SubmitScreenshot() {
            if (!WantsScreenshot())
                return true;

            ID3D12Resource* backBuffer = m_swapChain.BackBuffer(m_swapChain.CurrentIndex());
            auto capture = render::ScreenshotCapture::Create(m_device.Get(), backBuffer);
            if (!capture) {
                Log(Channel::Render, Level::Error, "画像を写せない: {}", capture.error());
                return false;
            }

            m_screenshot.push_back(std::move(*capture));
            ID3D12CommandList* list = m_screenshot.front().Record(backBuffer);
            if (list == nullptr)
                return false;

            m_direct.Submit(list);

            return true;
        }

        // 写した画像をファイルへ(GPU を待った後)。写していなければ何もしない
        bool FrameLoop::WriteScreenshot() {
            if (m_screenshot.empty())
                return true;

            const auto written = m_screenshot.front().WriteBmp(m_options.screenshotPath);
            if (!written) {
                Log(Channel::Render, Level::Error, "画像を書けない: {}", written.error());
                return false;
            }

            Log(Channel::Render, Level::Info, "画像: {}{}", ToUtf8(m_options.screenshotPath.wstring()),
                m_options.screenshotTick ? std::format("(S({}))", *m_options.screenshotTick) : "");

            return true;
        }

        bool FrameLoop::IsDeviceLost() const {
            return m_direct.IsDeviceLost() || m_compute.IsDeviceLost() ||
                   FAILED(m_device.Get()->GetDeviceRemovedReason());
        }

        // --- エディタと時間の操作(T-0023)---

        bool FrameLoop::CreateEditor() {
            if (m_options.autoReload && !CreateAutoReload())
                return false;

            if (!m_options.editor)
                return true;

            auto overlay = editor::EditorOverlay::Create(*m_window, m_device.Get(), m_direct, gpu::SwapChain::FORMAT,
                                                         FRAME_SLOT_COUNT);
            if (!overlay) {
                Log(Channel::Render, Level::Error, "エディタの殻を作れない: {}", overlay.error());
                return false;
            }

            m_editor = std::move(*overlay);
            if (m_options.autoLab)
                m_editor->Lab().StartAuto();

            if (m_options.autoPlace)
                m_editor->Brush().StartAuto();

            // --auto-table-edit(T-0219): 書き換えるのは --packages で渡した写しだけ
            if (m_options.autoTableEdit) {
                if (!m_options.replayPath.empty() || m_options.packageRoot.empty()) {
                    Log(Channel::Tool, Level::Error,
                        "--auto-table-edit は --editor と --packages <写したフォルダ> と一緒に使う");
                    return false;
                }

                m_editor->ReactionTable().StartAuto();
            }

            return true;
        }

        // ImGui のパネルの上の入力はカメラとつつきに渡さない。離す・動かすは渡す(パネルの外で始めたドラッグを終わらせるため)
        void FrameLoop::FilterEditorInput(std::vector<InputEvent>& events) const {
            if (!m_editor)
                return;

            const bool pointer = m_editor->WantsPointer();
            const bool keyboard = m_editor->WantsKeyboard();
            std::erase_if(events, [&](const InputEvent& event) {
                if (event.kind == InputKind::KeyDown)
                    return keyboard;

                return pointer && (event.kind == InputKind::ButtonDown || event.kind == InputKind::Wheel);
            });
        }

        // パネルを作り、押された時間の操作を受ける(--auto-time の操作もここで入れる)
        void FrameLoop::BuildEditor() {
            editor::TimeRequest request;
            if (m_editor) {
                // 実験室は世界と同じ表(差し替えたら次の刻みから箱も替える。T-0194・T-0218)
                if (const auto loaded = m_tableReload.Find(m_tableReload.AppliedVersion()); loaded != nullptr) {
                    m_editor->Lab().UseTable(std::shared_ptr<const sim::BakedReactionTable>(loaded, &loaded->table),
                                             loaded->tableVersion);
                    m_editor->ReactionTable().UseTable(loaded);
                    m_editor->Brush().UseTable(std::shared_ptr<const sim::BakedReactionTable>(loaded, &loaded->table),
                                               loaded->tableVersion);
                }

                request = m_editor->Build(MakeEditorStatus(), m_timeControl);
            }

            if (m_options.autoTime) {
                CheckAutoTime();
                const editor::TimeRequest automatic = AutoTimeRequest(m_frameNumber);
                if (automatic.Any())
                    request = automatic;
            }

            if (request.rewindTick != UINT64_MAX)
                RequestRewind(request.rewindTick);

            CheckAutoRewind();
            ApplyTimeRequest(request);
        }

        bool FrameLoop::SubmitEditor() {
            if (!m_editor)
                return true;

            const uint32_t index = m_swapChain.CurrentIndex();

            return m_editor->Submit(m_direct, m_swapChain.BackBuffer(index), m_swapChain.RenderTargetView(index));
        }

        void FrameLoop::ApplyTimeRequest(const editor::TimeRequest& request) {
            if (!request.Any())
                return;

            m_timeControl.Apply(request, m_scheduler);
            const SimCursor cursor = m_scheduler.Cursor();
            Log(Channel::Sim, Level::Info, "時間の操作: {}  速さ ×{:g}  刻み {}(単位 {})  1 刻みずつ {}",
                m_timeControl.Paused() ? "止めている" : "動いている", m_timeControl.Speed(), cursor.tick, cursor.unit,
                m_timeControl.SteppedTicks());
        }

        // --auto-time: 止めている間は刻みが進まず、1 刻みを 3 回押したらちょうど 3 刻み進んだか
        void FrameLoop::CheckAutoTime() {
            const SimCursor cursor = m_scheduler.Cursor();
            if (m_frameNumber == AUTO_TIME_HOLD_FRAME)
                m_autoTimeHold = cursor;

            const bool holdBroken = m_frameNumber == AUTO_TIME_HOLD_CHECK_FRAME &&
                                    (cursor != m_autoTimeHold || cursor.unit != 0);
            const SimCursor stepped{.tick = m_autoTimeHold.tick + AUTO_TIME_STEP_FRAMES.size(), .unit = 0};
            const bool stepBroken = m_frameNumber == AUTO_TIME_STEP_CHECK_FRAME && cursor != stepped;
            if (!holdBroken && !stepBroken)
                return;

            m_autoTimeFailed = true;
            Log(Channel::Sim, Level::Error, "--auto-time: フレーム {} で刻み {}(単位 {})。期待は刻み {}(単位 0)",
                m_frameNumber, cursor.tick, cursor.unit, holdBroken ? m_autoTimeHold.tick : stepped.tick);
        }

        editor::EditorStatus FrameLoop::MakeEditorStatus() const {
            const SimCursor cursor = m_scheduler.Cursor();
            const double seconds = m_lastIntervalSeconds;
            const auto perSecond = [&](uint64_t count) {
                return seconds > 0.0 ? static_cast<double>(count) / seconds : 0.0;
            };

            const auto average = [](double sum, uint64_t count) {
                return count > 0 ? sum / static_cast<double>(count) : 0.0;
            };

            const Stats& last = m_lastInterval;

            return {.tick = cursor.tick,
                    .unit = cursor.unit,
                    .unitsPerTick = m_scheduler.UnitsPerTick(),
                    .pendingTicks = m_scheduler.PendingTicks(),
                    .droppedTicks = m_scheduler.DroppedTicks(),
                    .hashedTick = m_latestHash.tick,
                    .worldHash = m_latestHash.WorldHash(),
                    .energyMillijoules = m_latestHash.energy,
                    .scheduledBlocks = m_latestHash.scheduledBlocks,
                    .framesPerSecond = perSecond(last.frames),
                    .ticksPerSecond = perSecond(last.ticks),
                    .cpuMilliseconds = average(last.cpuMilliseconds, last.frames),
                    .simGpuMilliseconds = average(last.simGpuMilliseconds, last.simSubmissionsMeasured),
                    .renderGpuMilliseconds = average(last.renderGpuMilliseconds, last.renderFramesMeasured),
                    .budgetMilliseconds = m_scheduler.BudgetMilliseconds(),
                    .view = m_viewController.Describe(),
                    .replaying = !m_replay.empty(),
                    .recording = !m_options.recordPath.empty(),
                    .waitingCommands = m_pendingCommands.size(),
                    .savePointTicks = SavePointTicks(),
                    .saveIntervalTicks = m_options.saveIntervalTicks,
                    .rewindUnavailable = RewindUnavailableReason(),
                    .graph = MakeGraphPanelStatus(),
                    .reactionTable = {.version = m_tableReload.AppliedVersion(),
                                      .swaps = m_tableReload.AppliedCount(),
                                      .failures = m_tableReload.FailedCount(),
                                      .watching = m_tableReload.Watching(),
                                      .waiting = m_tableReload.Waiting(),
                                      .lastFailed = m_tableReload.LastFailed(),
                                      .message = m_tableReload.LastMessage()}};
        }

        // 性能のパネル(T-0143): 直近 1 秒の単位ごとの GPU 時間を 1 刻みあたりに、伝導のグラフのカウンタはそのまま
        editor::GraphPanelStatus FrameLoop::MakeGraphPanelStatus() const {
            const Stats& last = m_lastInterval;
            editor::GraphPanelStatus status{
                .layout = &m_sim.ConductStatsLayout(), .stats = last.conductGraph, .ticks = last.ticks};

            const uint32_t unitsPerTick = m_sim.UnitsPerTick();
            for (uint32_t unit = 0; unit < last.unitGpuMilliseconds.size(); ++unit) {
                const double perTick = last.ticks > 0 ? last.unitGpuMilliseconds[unit] / static_cast<double>(last.ticks)
                                                      : 0.0;
                const bool physics = m_sim.Physics() != nullptr && unit == sim::PROBE_UNIT_PHYSICS;
                const char* name = unit == sim::PROBE_UNIT_APPLY     ? "コマンドの適用"
                                   : unit == sim::PROBE_UNIT_CONDUCT ? "伝導と反応"
                                   : physics                         ? "物理"
                                   : unit == unitsPerTick - 1        ? "ハッシュとイベント"
                                                                     : "重さの試験";
                status.units.push_back({.name = name,
                                        .workGraph = unit == sim::PROBE_UNIT_CONDUCT || physics,
                                        .millisecondsPerTick = perTick});
            }

            return status;
        }

        // --- 巻き戻し(保存点 + 再生。T-0143・ADR-0036)---
        // 保存点: saveIntervalTicks の倍数の刻みの境界で、そのフレームの先頭に S(刻み) を VRAM の保存点へ写す(GPU → GPU)。
        //   SimScheduler がその境界でフレームを切るので、どの境界も必ずどれかのフレームの先頭になる。
        // 戻す: GPU を待って読み戻しを全部読み、カーソルを保存点の刻みへ。次のリストの先頭で世界を保存点の状態に戻し、
        //   その刻み以降のコマンド(再生ファイル、なければ今の流れで足したコマンド)を足し直す。同じコマンドなら同じハッシュ列になる。
        //   戻した後のつつきは、足し直すコマンドに混ぜて刻みの順に足す(先の流れは変わりうるので、戻した刻みより先の保存点は捨てる)。

        bool FrameLoop::CreateSavePoints() {
            const uint32_t requested = m_options.savePoints == AUTO_SAVE_POINTS
                                           ? (m_options.editor ? DEFAULT_EDITOR_SAVE_POINTS : 0u)
                                           : std::min(m_options.savePoints, MAX_SAVE_POINTS);
            if (requested == 0)
                return true;

            if (!m_sim.CreateSavePoints(m_device.Get(), requested)) {
                Log(Channel::Sim, Level::Error, "保存点を作れない({} 個)", requested);
                return false;
            }

            m_savePointCount = requested;
            m_scheduler.SetBreakInterval(m_options.saveIntervalTicks);
            Log(Channel::Sim, Level::Info, "巻き戻し: 保存点 {} 個 × {:.1f} MiB、{} 刻みごと", requested,
                static_cast<double>(m_sim.SavePointBytes()) / (1024.0 * 1024.0), m_options.saveIntervalTicks);

            return true;
        }

        // 巻き戻せない理由(空なら巻き戻せる)。記録と CPU の物理の突き合わせとトレースは、刻みが一度ずつ進む前提なので
        std::string FrameLoop::RewindUnavailableReason() const {
            if (m_savePointCount == 0)
                return "保存点なし(--save-points 0)";

            if (!m_options.recordPath.empty())
                return "記録中は巻き戻さない";

            if (m_physicsCheck)
                return "--check-physics の間は巻き戻さない";

            if (!m_traceCapture.empty())
                return "トレースを集めている間は巻き戻さない";

            return {};
        }

        std::vector<uint64_t> FrameLoop::SavePointTicks() const {
            std::vector<uint64_t> ticks;
            for (uint32_t index = 0; index < m_savePointCount; ++index) {
                const uint64_t tick = m_sim.SavePointTick(index);
                if (tick != UINT64_MAX)
                    ticks.push_back(tick);
            }

            rng::sort(ticks);

            return ticks;
        }

        void FrameLoop::RequestRewind(uint64_t tick) {
            const std::string reason = RewindUnavailableReason();
            if (!reason.empty() || !rng::contains(SavePointTicks(), tick)) {
                Log(Channel::Sim, Level::Warning, "刻み {} へ巻き戻せない: {}", tick,
                    reason.empty() ? "その保存点が無い" : reason);
                return;
            }

            m_rewindRequest = tick;
        }

        // GPU を待ち、読み戻しを全部読んでから、カーソルとコマンドを保存点の刻みへ戻す。戻す保存点の番号を返す
        uint32_t FrameLoop::PrepareRewind() {
            const uint64_t tick = std::exchange(m_rewindRequest, UINT64_MAX);
            if (!m_compute.WaitIdle())
                return sim::NO_SAVE_POINT;

            CollectSimSubmissions();
            uint32_t index = sim::NO_SAVE_POINT;
            for (uint32_t candidate = 0; candidate < m_savePointCount; ++candidate) {
                if (m_sim.SavePointTick(candidate) == tick)
                    index = candidate;
            }

            if (index == sim::NO_SAVE_POINT)
                return index;

            const SimCursor from = m_scheduler.Cursor();
            m_scheduler.Rewind(tick);
            m_sim.DiscardSavePointsAfter(tick);
            if (!m_replay.empty())
                m_replay.front().Rewind(tick);

            // 今の流れで足したコマンドのうち、戻した刻み以降のものを足し直す(まだ足していなかった分の後ろに)
            const auto kept = rng::find_if(
                m_commandHistory, [&](const sim::ProbeCommand& command) { return command.targetTick >= tick; });
            std::deque<sim::ProbeCommand> feed(kept, m_commandHistory.end());
            feed.insert(feed.end(), m_rewindFeed.begin(), m_rewindFeed.end());
            m_rewindFeed = std::move(feed);
            m_commandHistory.erase(kept, m_commandHistory.end());

            m_extractAfterRewind = true;
            ++m_rewindCount;
            Log(Channel::Sim, Level::Info, "巻き戻し: 刻み {}(単位 {})→ 保存点の刻み {}。足し直すコマンド: {}",
                from.tick, from.unit, tick,
                m_replay.empty() ? std::format("{} 個", m_rewindFeed.size()) : std::string("再生ファイルから"));

            return index;
        }

        // 保存点の刻みの境界から単位を始めるフレームなら、写す保存点の番号(刻み / 間隔 を数で回す)
        uint32_t FrameLoop::SaveSlotFor(SimCursor start, uint32_t unitCount, uint32_t restoreFrom) const {
            const uint64_t interval = m_options.saveIntervalTicks;
            const bool due = m_savePointCount > 0 && restoreFrom == sim::NO_SAVE_POINT && unitCount > 0 &&
                             start.unit == 0 && start.tick > 0 && start.tick % interval == 0 &&
                             m_simSubmissionCount > 0;  // 最初のリストは世界を初期化する(写すのはその後)
            if (!due)
                return sim::NO_SAVE_POINT;

            return static_cast<uint32_t>((start.tick / interval) % m_savePointCount);
        }

        // 窓のクリックと、巻き戻した後に足し直すコマンド(endApplyTick より前の分)を、刻み・番号の順に混ぜる
        std::vector<sim::ProbeCommand> FrameLoop::TakeLiveCommands(uint64_t applyTick, uint64_t endApplyTick,
                                                                   uint32_t limit) {
            std::vector<sim::ProbeCommand> commands = TakeClickCommands(applyTick, limit);
            // クリックを足すなら、同じ刻みの足し直しも一緒に(後のフレームで、番号の小さいものを同じ刻みに足せなくなるので)
            const uint64_t feedEnd = commands.empty() ? endApplyTick : std::max(endApplyTick, applyTick + 1);
            while (!m_rewindFeed.empty() && m_rewindFeed.front().targetTick < feedEnd && commands.size() < limit) {
                const sim::ProbeCommand command = m_rewindFeed.front();
                m_rewindFeed.pop_front();
                if (command.targetTick < applyTick) {
                    Log(Channel::Sim, Level::Error, "巻き戻し: 刻み {} のコマンドが足し直しに間に合わなかった(捨てた)",
                        command.targetTick);
                    continue;
                }

                commands.push_back(command);
            }

            rng::sort(commands, [](const sim::ProbeCommand& a, const sim::ProbeCommand& b) {
                return a.targetTick != b.targetTick ? a.targetTick < b.targetTick : a.sequence < b.sequence;
            });
            m_commandHistory.insert(m_commandHistory.end(), commands.begin(), commands.end());

            return commands;
        }

        // --auto-reload(T-0195): ファイルを見るのは --editor の生の操作だけ。書き換えるのは --packages で渡した写し
        bool FrameLoop::CreateAutoReload() {
            if (!m_options.editor || !m_options.replayPath.empty() || m_options.packageRoot.empty()) {
                Log(Channel::Tool, Level::Error,
                    "--auto-reload は --editor と --packages <写したフォルダ> と一緒に使う(--replay とは使えない)");
                return false;
            }

            auto autoReload = AutoReload::Create(m_options.packageRoot);
            if (!autoReload) {
                Log(Channel::Tool, Level::Error, "{}", autoReload.error());
                return false;
            }

            m_autoReload.emplace(std::move(*autoReload));

            return true;
        }

        // --auto-rewind: 保存点が 2 つできたら、古い方へ 1 回だけ戻す
        void FrameLoop::CheckAutoRewind() {
            if (!m_options.autoRewind || m_rewindCount > 0 || m_rewindRequest != UINT64_MAX)
                return;

            const std::vector<uint64_t> ticks = SavePointTicks();
            if (ticks.size() >= 2)
                RequestRewind(ticks.front());
        }

        // --- 計測 ---

        void FrameLoop::AddStats(const Stats& frame) {
            m_total.frames += frame.frames;
            m_total.cpuMilliseconds += frame.cpuMilliseconds;
            m_total.cpuMaxMilliseconds = std::max(m_total.cpuMaxMilliseconds, frame.cpuMaxMilliseconds);
            m_total.presentMilliseconds += frame.presentMilliseconds;
            m_total.ticks += frame.ticks;
            m_total.simSubmissions += frame.simSubmissions;
            m_total.units += frame.units;
            m_total.simGpuMilliseconds += frame.simGpuMilliseconds;
            m_total.simSubmissionsMeasured += frame.simSubmissionsMeasured;
            m_total.renderGpuMilliseconds += frame.renderGpuMilliseconds;
            m_total.renderFramesMeasured += frame.renderFramesMeasured;
            m_total.cpuWaits += frame.cpuWaits;
            m_total.skippedSubmissions += frame.skippedSubmissions;
            m_total.skippedExtractions += frame.skippedExtractions;
            m_total.events += frame.events;
            gpu::AccumulateGraphStats(m_total.conductGraph, frame.conductGraph);
        }

        void FrameLoop::LogStats(const Stats& stats, Clock::duration elapsed, std::string_view label) const {
            const double seconds = chr::duration<double>(elapsed).count();
            if (seconds <= 0.0 || stats.frames == 0)
                return;

            const auto average = [](double sum, uint64_t count) {
                return count > 0 ? sum / static_cast<double>(count) : 0.0;
            };

            const auto perSecond = [&](uint64_t count) {
                return static_cast<double>(count) / seconds;
            };

            Log(Channel::Core, Level::Info,
                "{}: {:.1f} fps  CPU {:.3f} ms/フレーム(うち Present {:.3f}、最大 {:.3f})  世界 {:.1f} 刻み/秒  "
                "シミュ {:.1f} 単位/投入・GPU {:.3f} ms/投入(予算 {:.2f})  描画 GPU {:.3f} ms/フレーム  "
                "捨てた刻み {}  見送り {}(抽出 {})  CPU の待ち {}  イベント {}  状態 S({}) = {:016x}"
                "(エネルギー {} mJ・計算したブロック {})",
                label, perSecond(stats.frames), average(stats.cpuMilliseconds, stats.frames),
                average(stats.presentMilliseconds, stats.frames), stats.cpuMaxMilliseconds, perSecond(stats.ticks),
                average(static_cast<double>(stats.units), stats.simSubmissions),
                average(stats.simGpuMilliseconds, stats.simSubmissionsMeasured), m_scheduler.BudgetMilliseconds(),
                average(stats.renderGpuMilliseconds, stats.renderFramesMeasured), m_scheduler.DroppedTicks(),
                stats.skippedSubmissions, stats.skippedExtractions, stats.cpuWaits, stats.events, m_latestHash.tick,
                m_latestHash.hash, static_cast<int64_t>(m_latestHash.energy), m_latestHash.scheduledBlocks);

            if (!stats.conductGraph.nodes.empty()) {
                Log(Channel::WorkGraph, Level::Info, "{}: {}", label,
                    gpu::FormatGraphStats(m_sim.ConductStatsLayout(), stats.conductGraph));
            }
        }

        // --- ループ ---

        // 1 フレーム分(歩調の待ちの後)。デバイスの喪失や Present の失敗なら false
        bool FrameLoop::RunFrame(Clock::time_point frameStart) {
            CollectSimSubmissions();
            QueueClicks();
            m_tableReload.Poll(m_frameNumber);
            if (m_autoReload)
                m_autoReload->Update(m_scheduler.Cursor().tick, m_tableReload);

            if (!SubmitSim())
                return false;

            SubmitRender();
            BuildEditor();
            if (!SubmitEditor() || !SubmitScreenshot())
                return false;

            const auto presentStart = Clock::now();
            const bool presented = m_swapChain.Present(m_options.vsync);
            m_interval.presentMilliseconds += Milliseconds(Clock::now() - presentStart);
            if (!presented || IsDeviceLost())
                return false;

            const double cpuMilliseconds = Milliseconds(Clock::now() - frameStart);
            m_interval.cpuMilliseconds += cpuMilliseconds;
            m_interval.cpuMaxMilliseconds = std::max(m_interval.cpuMaxMilliseconds, cpuMilliseconds);
            ++m_interval.frames;
            ++m_frameNumber;

            return true;
        }

        int FrameLoop::Run() {
            Log(Channel::Core, Level::Info,
                "フレームのループを始める(vsync {}  先行 {}  目標 {} fps  重さ {}(分けて {} 個。1 刻み {} 単位)  "
                "描画の優先度 {}  自動クリック {})",
                m_options.vsync ? "あり" : "なし", m_options.maxFrameLatency, m_options.targetFps, m_options.simLoad,
                m_options.simSplit, m_sim.UnitsPerTick(), m_options.renderHighPriority ? "HIGH" : "NORMAL",
                m_options.autoClick ? "あり" : "なし");

            const render::OrbitCameraState& camera = m_viewController.Camera().State();
            Log(Channel::Render, Level::Info, "表示: {}  カメラ 向き {:.0f}° 上下 {:.0f}° 距離 {:.0f}",
                m_viewController.Describe(), camera.yawDegrees, camera.pitchDegrees, camera.distance);
            if (!m_options.screenshotPath.empty() && m_options.frameLimit == 0 && !m_options.screenshotTick)
                Log(Channel::Render, Level::Warning, "--screenshot は --frames か --screenshot-tick と一緒に使う");

            if (!m_replay.empty()) {
                Log(Channel::Sim, Level::Info, "再生: {}(コマンド {} 個、ハッシュ {} 個、刻み {} まで)",
                    ToUtf8(m_options.replayPath.wstring()), m_replay.front().CommandCount(),
                    m_replay.front().HashCount(), m_replay.front().LastTick());
            }

            const auto start = Clock::now();
            auto lastFrame = start;
            auto intervalStart = start;

            while (m_window->PumpMessages()) {
                if (m_options.frameLimit > 0 && m_frameNumber >= m_options.frameLimit)
                    break;

                // --screenshot-tick: 止まる刻みの画面を写した
                if (m_options.screenshotTick && !m_screenshot.empty())
                    break;

                // --auto-reload: 差し替えて流し終えた・失敗した
                if (m_autoReload && (m_autoReload->Done() || m_autoReload->Failed()))
                    break;

                // --auto-table-edit: 書き戻して当て、元に戻った・失敗した
                if (m_options.autoTableEdit && m_editor && m_editor->ReactionTable().AutoFinished())
                    break;

                // 最後のハッシュまで確かめた(--screenshot-tick なら写すまで続ける)
                if (!m_replay.empty() && m_replay.front().Finished() && !m_options.screenshotTick)
                    break;

                if (m_window->TakeResized() && !HandleResize())
                    return Finish(true, start);

                if (m_window->IsMinimized()) {
                    std::this_thread::sleep_for(MINIMIZED_SLEEP);
                    lastFrame = Clock::now();  // 最小化の間は世界を進めない
                    continue;
                }

                m_swapChain.WaitForFrame(FRAME_WAIT_TIMEOUT_MS);  // フレームの歩調(CPU が GPU より先に行き過ぎない)
                const auto frameStart = Clock::now();
                m_timeControl.AdvanceRealTime(chr::duration<double>(frameStart - lastFrame).count(), m_scheduler);
                lastFrame = frameStart;
                if (!RunFrame(frameStart))
                    return Finish(true, start);

                if (frameStart - intervalStart < STATS_INTERVAL)
                    continue;

                LogStats(m_interval, frameStart - intervalStart, "1 秒");
                AddStats(m_interval);
                m_lastInterval = m_interval;
                m_lastIntervalSeconds = chr::duration<double>(frameStart - intervalStart).count();
                m_interval = {};
                intervalStart = frameStart;
            }

            return Finish(false, start);
        }

        // 終わり: GPU を待って残りを読み、全体の要約を出す
        int FrameLoop::Finish(bool failed, Clock::time_point start) {
            if (failed || IsDeviceLost()) {
                if (IsDeviceLost())
                    gpu::LogDeviceRemoved(m_device.Get());
                else {
                    m_direct.Flush();  // 解放する前に GPU を終わらせる
                    m_compute.Flush();
                }

                Log(Channel::Core, Level::Error, "フレームのループが失敗で終わった");

                return 1;
            }

            m_direct.Flush();  // Present の分も待つ(バックバッファを解放する前に)
            m_compute.Flush();
            if (!WriteScreenshot())
                return 1;

            if (m_options.screenshotTick && !m_options.screenshotPath.empty() && m_screenshot.empty()) {
                Log(Channel::Render, Level::Error,
                    "--screenshot-tick: 刻み {} の画面を写せなかった(--frames {} のうちに届かなかった)",
                    *m_options.screenshotTick, m_options.frameLimit);

                return 1;
            }

            CollectSimSubmissions();
            AddStats(m_interval);
            LogStats(m_total, Clock::now() - start, "全体");
            const bool replayPassed = FinishReplay();
            if (!FinishTrace())
                return 1;

            if (m_device.ValidationErrorCount() > 0) {
                Log(Channel::Gpu, Level::Error, "debug layer のエラーが {} 件", m_device.ValidationErrorCount());
                return 1;
            }

            if (m_autoTimeFailed)
                return 1;

            if (m_options.autoLab && (!m_editor || m_editor->Lab().AutoFailed())) {
                Log(Channel::Sim, Level::Error, "--auto-lab: 実験室の確かめが通らなかった(--editor が要る)");
                return 1;
            }

            if (m_options.autoPlace && (!m_editor || !m_editor->Brush().AutoPassed())) {
                Log(Channel::Sim, Level::Error, "--auto-place: 筆で置いたコマンドが当たらなかった({})",
                    m_editor ? m_editor->Brush().AutoSummary() : "--editor が要る");
                return 1;
            }

            if (m_options.autoPlace)
                Log(Channel::Sim, Level::Info, "--auto-place: {}。OK", m_editor->Brush().AutoSummary());

            if (m_options.autoReload && (!m_autoReload || !m_autoReload->Done())) {
                Log(Channel::Tool, Level::Error, "--auto-reload: ホットリロードの確かめが通らなかった({})",
                    m_autoReload ? m_autoReload->Failure() : "始められない");
                return 1;
            }

            if (m_options.autoTableEdit && (!m_editor || !m_editor->ReactionTable().AutoDone())) {
                Log(Channel::Tool, Level::Error,
                    "--auto-table-edit: 反応表のパネルの書き戻しの確かめが通らなかった({})",
                    m_editor ? m_editor->ReactionTable().AutoFailure() : "--editor が要る");
                return 1;
            }

            if (m_options.autoRewind && m_rewindCount == 0) {
                Log(Channel::Sim, Level::Error, "--auto-rewind: 巻き戻せなかった(保存点が 2 つできなかった)");
                return 1;
            }

            return replayPassed ? 0 : 1;
        }

        // --- 連鎖のトレース ---

        // T が押された: 次に投げるフレームから始める。集めている途中なら無視する(GPU の範囲は 1 つだけ)
        void FrameLoop::RequestTrace() {
            if (!m_traceCapture.empty()) {
                Log(Channel::Sim, Level::Warning, "トレースを集めている途中なので T を無視した({})",
                    ToUtf8(m_traceCapture.front().Path().wstring()));
                return;
            }

            m_traceRequested = true;
        }

        // 最後につついたセルの周り ±TRACE_KEY_RADIUS_CELLS を、このフレームの最初の丸ごとの刻みから TRACE_KEY_TICKS 刻み。
        // 刻みの途中から始めると、その刻みの前半(記録済みの単位)が欠けるので、まだ適用を記録していない最初の刻みから
        void FrameLoop::StartRequestedTrace(SimCursor start) {
            if (!m_traceRequested)
                return;

            m_traceRequested = false;
            const uint64_t tickBegin = sim::ProbeSim::NextApplyTick(start.tick, start.unit);
            const render::CellCoordinate center = m_lastPokedCell;
            const auto below = [](uint32_t cell) {
                return cell - std::min(cell, TRACE_KEY_RADIUS_CELLS);
            };
            const std::array<uint32_t, 3> cellMin = {below(center.x), below(center.y), below(center.z)};
            const std::array<uint32_t, 3> cellMax = {
                center.x + TRACE_KEY_RADIUS_CELLS + 1, center.y + TRACE_KEY_RADIUS_CELLS + 1,
                center.z + TRACE_KEY_RADIUS_CELLS + 1};  // 格子の外は切り詰められる

            const gpu::GraphTraceFilter filter = sim::ProbeTraceFilterForCells(
                tickBegin, tickBegin + TRACE_KEY_TICKS, cellMin, cellMax, TRACE_CAPACITY_PER_FRAME);
            const fs::path path = m_options.traceDirectory /
                                  std::format(L"trace-t{}-cell{}_{}_{}.txt", tickBegin, center.x, center.y, center.z);

            m_sim.SetTraceFilter(filter);
            m_traceCapture.emplace_back(path, filter);
            Log(Channel::Sim, Level::Info, "トレースを始める: セル ({}, {}, {}) ±{}・刻み [{}, {}) → {}", center.x,
                center.y, center.z, TRACE_KEY_RADIUS_CELLS, tickBegin, tickBegin + TRACE_KEY_TICKS,
                ToUtf8(path.wstring()));
        }

        // 読み戻した記録を足す。範囲の刻みを全部読んだら書いて、GPU の範囲を無効に戻す(使っていない間の費用は 0。docs/perf.md)
        void FrameLoop::CollectTrace(const sim::ProbeFrameReadback& readback) {
            if (m_traceCapture.empty())
                return;

            TraceCapture& capture = m_traceCapture.front();
            capture.Add(readback.trace, readback.droppedTraceCount);
            if (!capture.IsComplete(m_latestHash.tick))
                return;

            const auto written = capture.Write();
            if (!written) {
                Log(Channel::Sim, Level::Error, "{}", written.error());
                m_traceWriteFailed = true;
            }

            m_traceCapture.clear();
            m_sim.SetTraceFilter({});
        }

        // 終わるときに、集めている途中のトレース(--trace の終わりの無い範囲・終わっていない T)を書く
        bool FrameLoop::FinishTrace() {
            if (!m_traceCapture.empty()) {
                const auto written = m_traceCapture.front().Write();
                m_traceCapture.clear();
                if (!written) {
                    Log(Channel::Sim, Level::Error, "{}", written.error());
                    return false;
                }
            }

            return !m_traceWriteFailed;
        }

        // 記録に反応表を残す: 起動時の表と、記録したコマンドの差し替えの印の版の表(版の昇順。T-0170・T-0193・ADR-0050)。
        // 中身(TableBytes)があるので、再生はパッケージのフォルダが無くても同じ表で世界を進められる
        void AttachReplayTables(save::ReplayFile& replay, const TableHotReload& tables) {
            std::set<uint64_t> versions{tables.InitialVersion()};
            for (const sim::ProbeCommand& command : replay.commands) {
                if (command.type == sim::PROBE_COMMAND_TYPE_TABLE)
                    versions.insert(sim::TableCommandVersion(command));
            }

            replay.tableVersion = tables.InitialVersion();
            for (const uint64_t version : versions) {
                const auto table = tables.Find(version);
                if (!table) {
                    Log(Channel::Sim, Level::Warning, "記録: 反応表の版 {:016x} の中身を持っていない(再生で止まる)",
                        version);
                    continue;
                }

                replay.tables.push_back({.version = version,
                                         .loadOrder = table->loadOrder,
                                         .modifiedWorld = table->modifiedWorld,
                                         .content = table->tableBytes});
            }
        }

        // 記録を書き、再生の結果を出す。記録を書けない・再生が合わない(終わっていない)なら false
        bool FrameLoop::FinishReplay() {
            bool passed = true;
            if (!m_options.recordPath.empty()) {
                save::ReplayFile replay = m_recorder.Build();
                AttachReplayTables(replay, m_tableReload);
                const auto written = save::WriteReplayFile(m_options.recordPath, replay);
                if (written) {
                    Log(Channel::Sim, Level::Info, "記録: {}(コマンド {} 個、ハッシュ {} 個、反応表 {} 個)",
                        ToUtf8(m_options.recordPath.wstring()), replay.commands.size(), replay.tickHashes.size(),
                        replay.tables.size());
                } else {
                    Log(Channel::Sim, Level::Error, "記録を書けない: {}", written.error());
                    passed = false;
                }
            }

            if (m_replay.empty())
                return passed;

            const save::ReplayPlayer& player = m_replay.front();

            Log(Channel::Sim, player.Passed() ? Level::Info : Level::Error,
                "再生: {}(ハッシュ 一致 {} / 不一致 {} / 全部 {}、間に合わなかったコマンド {}{})",
                player.Passed() ? "OK" : "FAILED", player.Matches(), player.Mismatches(), player.HashCount(),
                player.LateCommands(), player.Finished() ? "" : "、最後まで進む前に終わった");

            return passed && player.Passed();
        }

    }  // namespace

    int RunFrameLoop(const FrameLoopOptions& options) {
        auto parts = CreateParts(options);
        if (!parts) {
            Log(Channel::Core, Level::Error, "フレームのループを始められない: {}", parts.error());
            return 1;
        }

        FrameLoop loop(options, std::move(*parts));
        if (!loop.CreateFrameSlots()) {
            Log(Channel::Gpu, Level::Error, "描画のフレームの枠を作れない");
            return 1;
        }

        if (!loop.CreateEditor() || !loop.CreateSavePoints())
            return 1;

        return loop.Run();
    }

}  // namespace bicameral::frame
