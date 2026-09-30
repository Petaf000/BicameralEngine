// frame_loop.cpp — 窓・2 つのキュー・予算ぶんのシミュ・待たない読み戻しでフレームを回す(T-0004・T-0012)。考え方は frame_loop.h。
//
// 1 フレームの流れ(CPU):
//   窓のメッセージ → スワップチェインの待ち(歩調)→ 経過時間を SimScheduler へ
//   → 終わったシミュのリストの読み戻し(フェンスが進んだ分だけ。待たない。単位の GPU 時間・ハッシュ・イベント)
//   → クリック(再生なら再生ファイル)をコマンドに → 予算ぶんの単位と、GPU のキューへ足すコマンドを記録して compute キューへ(ADR-0011)
//   → 描画を direct キューへ(終わっている最新の抽出を見せる。シミュを待たない)→ Present
//
// 抽出の 3 組の約束(06 §4): 抽出は 1 フレームに 1 回まで、そのフレームに投げた単位の後ろで刻みの境界の状態を写す。
//   抽出 n は組 n % 3 に書き、CPU から見て終わっている抽出が n − 2 以上のときだけ投げる(でなければそのフレームは写さない)。
//   描画は終わっている最新の抽出 d(≥ n − 2、< n)を読むので、書いている組と重ならない。
//   さらに抽出 n の前に、組 n % 3 を最後に読んだ描画のフェンスを compute キューに GPU の上で待たせる(m_lastRenderReading)。
//   描画がシミュを待つことは無い(D-201)。
#include "frame/frame_loop.h"

#include <chrono>
#include <expected>
#include <thread>

#include "core/log.h"
#include "core/unicode.h"
#include "frame/sim_scheduler.h"
#include "gpu/queue.h"
#include "gpu/resources.h"
#include "gpu/swap_chain.h"
#include "platform/window.h"
#include "render/probe_view.h"
#include "save/replay_session.h"
#include "sim/probe_sim.h"

using Microsoft::WRL::ComPtr;

namespace bicameral::frame {
    namespace {

        using Clock = std::chrono::steady_clock;

        constexpr uint32_t FRAME_SLOT_COUNT = gpu::SwapChain::BUFFER_COUNT;
        constexpr uint32_t SIM_SLOT_COUNT = sim::ProbeSim::FRAME_SLOT_COUNT;
        constexpr uint32_t INITIAL_CLIENT_WIDTH = 1280;
        constexpr uint32_t INITIAL_CLIENT_HEIGHT = 720;
        constexpr uint32_t FRAME_WAIT_TIMEOUT_MS = 100;
        constexpr uint32_t CONSTANTS_BYTES = 256;
        constexpr uint32_t TIMESTAMPS_PER_FRAME = 2;
        constexpr uint32_t AUTO_CLICK_INTERVAL_FRAMES = 20;
        constexpr auto STATS_INTERVAL = std::chrono::seconds(1);
        constexpr auto MINIMIZED_SLEEP = std::chrono::milliseconds(16);

        double Milliseconds(Clock::duration duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        }

        double TimestampMilliseconds(uint64_t ticks, uint64_t frequency) {
            if (frequency == 0) return 0.0;
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
            uint64_t fence = 0;       // compute のフェンスの値
            uint64_t extraction = 0;  // このリストが書いた抽出の番号(0 = 写していない)
            bool read = true;
        };

        // 描画が見せる抽出(CPU から見て終わっている最新)
        struct CompletedExtraction {
            uint64_t number = 0;  // 0 = まだ無い(組 0 は 0 のまま)
            uint64_t fence = 0;   // それを書いたシミュのリストの compute のフェンスの値
        };

        struct PendingClick {
            uint32_t x = 0;
            uint32_t y = 0;
            uint64_t tick = UINT64_MAX;  // 載せた刻み(まだなら UINT64_MAX)
            Clock::time_point time;
        };

        // 計測(1 秒ごとにログへ。最後に全体の要約)
        struct Stats {
            uint64_t frames = 0;
            double cpuMilliseconds = 0.0;
            double cpuMaxMilliseconds = 0.0;
            double presentMilliseconds = 0.0;  // cpuMilliseconds のうち Present の中にいた時間
            uint64_t ticks = 0;                // 終わった刻み(読み戻したハッシュの数)
            uint64_t simSubmissions = 0;
            uint64_t units = 0;
            double simGpuMilliseconds = 0.0;  // シミュのリスト全体(抽出と読み戻しを含む)
            uint64_t simSubmissionsMeasured = 0;
            double renderGpuMilliseconds = 0.0;
            uint64_t renderFramesMeasured = 0;
            uint64_t cpuWaits = 0;            // 描画の枠を使い回す前に CPU が GPU を待った回数(0 のはず)
            uint64_t skippedSubmissions = 0;  // シミュの枠が空いていないので投げなかったフレーム
            uint64_t skippedExtractions = 0;  // 抽出の 3 組の約束で写さなかったフレーム
            uint64_t events = 0;
        };

        // 作るもの一式(FrameLoop はこれを受け取ってから動く。作れなかったら FrameLoop を作らない)
        struct FrameLoopParts {
            std::unique_ptr<Window> window;
            gpu::Device device;
            gpu::Queue direct;
            gpu::Queue compute;
            gpu::SwapChain swapChain;
            sim::ProbeSim simulation;
            render::ProbeView view;
            std::vector<save::ReplayPlayer>
                replay;  // --replay のときだけ 1 つ(std::optional は tidy の警告が多いので使わない)
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
                  m_view(std::move(parts.view)),
                  m_replay(std::move(parts.replay)),
                  m_scheduler(m_sim.UnitsPerTick(), {.targetFps = static_cast<double>(options.targetFps),
                                                     .maxUnitsPerFrame = sim::ProbeSim::MAX_UNITS_PER_FRAME}),
                  m_computeFrequency(m_compute.TimestampFrequency()),
                  m_directFrequency(m_direct.TimestampFrequency()) {}

            [[nodiscard]] bool CreateFrameSlots();
            [[nodiscard]] int Run();

        private:
            bool RecordRenderLists();
            bool HandleResize();

            void CollectSimSubmissions();
            void ReportSimReadback(const sim::ProbeFrameReadback& readback);
            void ReportEvents(const sim::ProbeFrameReadback& readback);
            void CollectFrameSlot(uint32_t slotIndex);
            void QueueClicks();
            [[nodiscard]] std::vector<sim::ProbeCommand> TakeCommands(SimCursor start);
            [[nodiscard]] std::vector<sim::ProbeCommand> TakeClickCommands(uint64_t applyTick, uint32_t limit);
            [[nodiscard]] bool FinishReplay();
            bool SubmitSim();
            void SubmitRender();
            bool RunFrame(Clock::time_point frameStart);
            int Finish(bool failed, Clock::time_point start);

            [[nodiscard]] bool IsDeviceLost() const;
            void LogStats(const Stats& stats, Clock::duration elapsed, std::string_view label) const;
            void AddStats(const Stats& frame);

            FrameLoopOptions m_options;
            std::unique_ptr<Window> m_window;
            gpu::Device m_device;
            gpu::Queue m_direct;
            gpu::Queue m_compute;
            gpu::SwapChain m_swapChain;
            sim::ProbeSim m_sim;
            render::ProbeView m_view;
            std::vector<save::ReplayPlayer> m_replay;  // 再生中なら 1 つ
            save::ReplayRecorder m_recorder;           // --record のときだけ使う
            std::array<FrameSlot, FRAME_SLOT_COUNT> m_frames;
            ComPtr<ID3D12QueryHeap> m_renderTimestamps;
            ComPtr<ID3D12Resource> m_renderTimestampReadback;

            SimScheduler m_scheduler;
            std::array<SimSubmission, SIM_SLOT_COUNT> m_simSubmissions;
            uint64_t m_simSubmissionCount = 0;
            uint64_t m_lastExtraction = 0;  // 最後に投げた抽出の番号
            CompletedExtraction m_completedExtraction;
            // 抽出の組ごとに、最後にそれを読んだ描画のフェンスの値
            std::array<uint64_t, sim::PROBE_EXTRACTION_COUNT> m_lastRenderReading{};
            sim::ProbeTickHash m_latestHash;  // 最後に読み戻した刻みの状態のハッシュ
            std::vector<sim::ProbeCommand> m_pendingCommands;
            std::vector<PendingClick> m_clicks;
            uint32_t m_nextSequence = 0;
            uint64_t m_frameNumber = 0;  // 描いたフレームの数
            uint64_t m_computeFrequency = 0;
            uint64_t m_directFrequency = 0;

            Stats m_interval;  // この 1 秒
            Stats m_total;     // 全体
        };

        // --- 作る ---

        std::expected<std::unique_ptr<Window>, std::string> CreateWindowForLoop() {
            auto window = Window::Create(L"Bicameral Engine", INITIAL_CLIENT_WIDTH, INITIAL_CLIENT_HEIGHT);
            if (!window) return std::unexpected(window.error());
            (*window)->PumpMessages();
            (void)(*window)->TakeResized();  // 最初の WM_SIZE。スワップチェインはこの大きさで作る
            return window;
        }

        std::expected<FrameLoopParts, std::string> CreateParts(const FrameLoopOptions& options) {
            auto window = CreateWindowForLoop();
            if (!window) return std::unexpected(window.error());
            gpu::DeviceOptions deviceOptions = gpu::DefaultDeviceOptions();
            deviceOptions.presentMonitor = MonitorFromWindow((*window)->Handle(), MONITOR_DEFAULTTONEAREST);
            auto device = gpu::Device::Create(options.adapter, deviceOptions);
            if (!device) return std::unexpected(device.error());
            ID3D12Device5* native = device->Get();

            // 描画のキューは優先度を上げる(効果は無かったが害も無い。R-LOOP-2。docs/perf.md)
            auto direct = gpu::Queue::Create(
                native, D3D12_COMMAND_LIST_TYPE_DIRECT, L"Render",
                options.renderHighPriority ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
            auto compute = gpu::Queue::Create(native, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"Sim");
            if (!direct || !compute) return std::unexpected("キューを作れない");
            auto swapChain = gpu::SwapChain::Create(native, device->Factory(), direct->Native(), (*window)->Handle(),
                                                    options.maxFrameLatency);
            if (!swapChain) return std::unexpected(swapChain.error());
            auto simulation =
                sim::ProbeSim::Create(native, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                      {.busyIterations = options.simLoad, .busyPieces = options.simSplit});
            if (!simulation) return std::unexpected(simulation.error());
            auto view = render::ProbeView::Create(native, gpu::SwapChain::FORMAT);
            if (!view) return std::unexpected(view.error());
            std::vector<save::ReplayPlayer> replay;
            if (!options.replayPath.empty()) {
                auto player = save::ReplayPlayer::Load(options.replayPath);
                if (!player) return std::unexpected(player.error());
                replay.push_back(std::move(*player));
            }
            return FrameLoopParts{.window = std::move(*window),
                                  .device = std::move(*device),
                                  .direct = std::move(*direct),
                                  .compute = std::move(*compute),
                                  .swapChain = std::move(*swapChain),
                                  .simulation = std::move(*simulation),
                                  .view = std::move(*view),
                                  .replay = std::move(replay)};
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
                if (!slot.constants) return false;
                void* mapped = nullptr;
                const D3D12_RANGE noRead{};
                if (FAILED(slot.constants->Map(0, &noRead, &mapped))) return false;
                slot.mappedConstants = static_cast<render::ProbeViewConstants*>(mapped);
            }
            const D3D12_QUERY_HEAP_DESC queryDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
                                                  .Count = FRAME_SLOT_COUNT * TIMESTAMPS_PER_FRAME};
            if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_renderTimestamps)))) return false;
            m_renderTimestampReadback = gpu::CreateBuffer(device, uint64_t{FRAME_SLOT_COUNT} * TIMESTAMPS_PER_FRAME * 8,
                                                          gpu::BufferKind::Readback);
            return m_renderTimestampReadback != nullptr && RecordRenderLists();
        }

        // バックバッファごとに描画のリストを記録する(作るときと、大きさが変わったときだけ)
        bool FrameLoop::RecordRenderLists() {
            for (uint32_t index = 0; index < FRAME_SLOT_COUNT; ++index) {
                FrameSlot& slot = m_frames[index];
                if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr))) {
                    return false;
                }
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
                if (FAILED(list->Close())) return false;
            }
            return true;
        }

        bool FrameLoop::HandleResize() {
            if (m_window->IsMinimized()) return true;
            // バックバッファを使うリストを GPU が全部終えてから(ここだけ CPU が待つ。Present の分も待つ)
            if (!m_direct.Flush() || !m_compute.Flush()) return false;
            CollectSimSubmissions();
            for (uint32_t index = 0; index < FRAME_SLOT_COUNT; ++index) {
                CollectFrameSlot(index);
            }
            if (!m_swapChain.Resize(m_device.Get(), m_window->ClientWidth(), m_window->ClientHeight())) return false;
            Log(Channel::Render, Level::Info, "窓の大きさ: {}×{}", m_swapChain.Width(), m_swapChain.Height());
            return RecordRenderLists();
        }

        // --- 読み戻し(待たない)---

        void FrameLoop::CollectSimSubmissions() {
            for (uint32_t slot = 0; slot < SIM_SLOT_COUNT; ++slot) {
                SimSubmission& submission = m_simSubmissions[slot];
                if (submission.read || !m_compute.IsComplete(submission.fence)) continue;
                submission.read = true;
                ReportSimReadback(m_sim.ReadFrame(slot));
                if (submission.extraction > m_completedExtraction.number) {
                    m_completedExtraction = {.number = submission.extraction, .fence = submission.fence};
                }
            }
        }

        // 単位ごとの GPU 時間を SimScheduler へ。終えた刻みのハッシュとイベント
        void FrameLoop::ReportSimReadback(const sim::ProbeFrameReadback& readback) {
            const uint32_t unitsPerTick = m_sim.UnitsPerTick();
            for (size_t index = 0; index < readback.unitGpuTicks.size(); ++index) {
                const auto unit = static_cast<uint32_t>((readback.firstUnit + index) % unitsPerTick);
                m_scheduler.ReportUnitTime(unit,
                                           TimestampMilliseconds(readback.unitGpuTicks[index], m_computeFrequency));
            }
            m_interval.simGpuMilliseconds +=
                TimestampMilliseconds(readback.gpuBeginTimestamp, readback.gpuEndTimestamp, m_computeFrequency);
            ++m_interval.simSubmissionsMeasured;
            m_interval.ticks += readback.hashes.size();
            if (!readback.hashes.empty()) m_latestHash = readback.hashes.back();
            for (const sim::ProbeTickHash& tickHash : readback.hashes) {
                if (!m_options.recordPath.empty()) m_recorder.AddHash(tickHash.tick, tickHash.hash);
                if (!m_replay.empty()) m_replay.front().CheckHash(tickHash.tick, tickHash.hash);
            }
            ReportEvents(readback);
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
                const auto click = std::ranges::find_if(m_clicks, [&](const PendingClick& pending) {
                    return pending.tick == event.tick && pending.x == event.PokeX() && pending.y == event.PokeY();
                });
                const double latency = click != m_clicks.end() ? Milliseconds(now - click->time) : 0.0;
                Log(Channel::Sim, Level::Info,
                    "つつき ({}, {}) を刻み {} で適用(クリックから CPU に戻るまで {:.1f} ms)", event.PokeX(),
                    event.PokeY(), event.tick, latency);
                if (click != m_clicks.end()) m_clicks.erase(click);
            }
            if (readback.droppedEventCount > 0) {
                Log(Channel::Sim, Level::Warning, "イベントのリングが溢れて {} 件を落とした",
                    readback.droppedEventCount);
            }
        }

        // 描画の枠を使い回す前に: 前にその枠で投げたリストの終わりを確かめ、タイムスタンプを読む
        void FrameLoop::CollectFrameSlot(uint32_t slotIndex) {
            FrameSlot& slot = m_frames[slotIndex];
            if (slot.renderFence == 0) return;
            if (!m_direct.IsComplete(slot.renderFence)) {
                ++m_interval.cpuWaits;  // スワップチェインの待ちがあるので、ふつうは起きない
                m_direct.WaitCpu(slot.renderFence);
            }
            std::array<uint64_t, size_t{FRAME_SLOT_COUNT} * TIMESTAMPS_PER_FRAME> timestamps{};
            if (gpu::ReadBuffer(m_renderTimestampReadback.Get(), std::as_writable_bytes(std::span(timestamps)))) {
                const size_t first = size_t{slotIndex} * TIMESTAMPS_PER_FRAME;
                const double milliseconds =
                    TimestampMilliseconds(timestamps[first], timestamps[first + 1], m_directFrequency);
                m_scheduler.ReportRenderTime(milliseconds);
                m_interval.renderGpuMilliseconds += milliseconds;
                ++m_interval.renderFramesMeasured;
            }
            slot.renderFence = 0;
        }

        // --- 入力 → コマンド ---

        void FrameLoop::QueueClicks() {
            std::vector<PointerEvent> pointers = m_window->TakePointerEvents();
            if (!m_replay.empty()) return;  // 再生中は窓の操作を入れない(世界は再生ファイルのコマンドだけで進む)
            // probe_view.hlsl と同じ置き方: 正方形の格子を短い辺に合わせて真ん中に
            const auto width = static_cast<int32_t>(m_swapChain.Width());
            const auto height = static_cast<int32_t>(m_swapChain.Height());
            const int32_t side = std::min(width, height);
            const int32_t originX = (width - side) / 2;
            const int32_t originY = (height - side) / 2;
            if (side <= 0) return;
            if (m_options.autoClick && m_frameNumber % AUTO_CLICK_INTERVAL_FRAMES == 0) {
                // 格子の上の決まった場所を順に押す(人がいない確認用)
                const auto step = static_cast<int32_t>(m_frameNumber / AUTO_CLICK_INTERVAL_FRAMES);
                pointers.push_back(
                    {.x = originX + (step * 37 % 16 + 1) * side / 18, .y = originY + (step * 11 % 16 + 1) * side / 18});
            }
            for (const PointerEvent& pointer : pointers) {
                const int32_t localX = pointer.x - originX;
                const int32_t localY = pointer.y - originY;
                if (localX < 0 || localY < 0 || localX >= side || localY >= side) continue;
                const auto cellX = static_cast<uint32_t>(int64_t{localX} * sim::PROBE_GRID_SIZE / side);
                const auto cellY = static_cast<uint32_t>(int64_t{localY} * sim::PROBE_GRID_SIZE / side);
                m_pendingCommands.push_back(sim::MakePokeCommand(0, m_nextSequence++, cellX, cellY, sim::PROBE_VIEW_Z));
                m_clicks.push_back({.x = cellX, .y = cellY, .time = Clock::now()});
            }
        }

        // このフレームに GPU のキューへ足すコマンド(クリックか再生ファイルから)。記録するならここで控える。
        // 適用する刻みは「まだ記録していない最初の適用の単位の刻み」以上(06 §3)。コマンドはそこまで GPU のキューで待つので、
        // このフレームに適用の単位が無くてもよい。キューの空きを超える分は次のフレームへ
        std::vector<sim::ProbeCommand> FrameLoop::TakeCommands(SimCursor start) {
            const uint64_t applyTick = sim::ProbeSim::NextApplyTick(start.tick, start.unit);
            const uint32_t limit = std::min(m_sim.FreeCommandSlots(), sim::PROBE_MAX_COMMANDS);
            std::vector<sim::ProbeCommand> commands = m_replay.empty()
                                                          ? TakeClickCommands(applyTick, limit)
                                                          : m_replay.front().TakeCommands(applyTick, limit);
            if (!m_options.recordPath.empty()) m_recorder.AddCommands(commands);
            return commands;
        }

        std::vector<sim::ProbeCommand> FrameLoop::TakeClickCommands(uint64_t applyTick, uint32_t limit) {
            const size_t count = std::min<size_t>(m_pendingCommands.size(), limit);
            std::vector<sim::ProbeCommand> commands(m_pendingCommands.begin(),
                                                    m_pendingCommands.begin() + static_cast<std::ptrdiff_t>(count));
            m_pendingCommands.erase(m_pendingCommands.begin(),
                                    m_pendingCommands.begin() + static_cast<std::ptrdiff_t>(count));
            for (sim::ProbeCommand& command : commands) {
                command.targetTick = applyTick;
                const auto click = std::ranges::find_if(m_clicks, [&](const PendingClick& pending) {
                    return pending.tick == UINT64_MAX && pending.x == command.payload[0] &&
                           pending.y == command.payload[1];
                });
                if (click != m_clicks.end()) click->tick = applyTick;
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
            const SimCursor start = m_scheduler.Cursor();
            const uint32_t unitCount = m_scheduler.TakeUnits();
            if (unitCount == 0) return true;

            const std::vector<sim::ProbeCommand> commands = TakeCommands(start);
            const uint64_t extraction = m_lastExtraction + 1;
            const bool extract = m_completedExtraction.number + 2 >= extraction;  // 抽出の 3 組の約束(ファイルの先頭)
            if (!extract) ++m_interval.skippedExtractions;
            const auto extractionTarget = static_cast<uint32_t>(extraction % sim::PROBE_EXTRACTION_COUNT);
            ID3D12CommandList* list = m_sim.RecordFrame(slot, {.firstTick = start.tick,
                                                               .firstUnit = start.unit,
                                                               .unitCount = unitCount,
                                                               .extract = extract,
                                                               .extractionTarget = extractionTarget,
                                                               .commands = commands});
            if (list == nullptr) return false;

            if (extract) {
                // 抽出の組を描画が読み終えるまで、GPU の上で待ってから走る
                m_compute.GpuWait(m_direct, m_lastRenderReading[extractionTarget]);
                m_lastExtraction = extraction;
            }
            submission = {.fence = m_compute.Submit(list), .extraction = extract ? extraction : 0, .read = false};
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
            *slot.mappedConstants = {
                .extractionIndex = extraction, .width = m_swapChain.Width(), .height = m_swapChain.Height()};
            m_direct.GpuWait(m_compute, m_completedExtraction.fence);
            slot.renderFence = m_direct.Submit(slot.list.Get());
            m_lastRenderReading[extraction] = slot.renderFence;
        }

        bool FrameLoop::IsDeviceLost() const {
            return m_direct.IsDeviceLost() || m_compute.IsDeviceLost() ||
                   FAILED(m_device.Get()->GetDeviceRemovedReason());
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
        }

        void FrameLoop::LogStats(const Stats& stats, Clock::duration elapsed, std::string_view label) const {
            const double seconds = std::chrono::duration<double>(elapsed).count();
            if (seconds <= 0.0 || stats.frames == 0) return;
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
                "(熱 {}・伝導したブロック {})",
                label, perSecond(stats.frames), average(stats.cpuMilliseconds, stats.frames),
                average(stats.presentMilliseconds, stats.frames), stats.cpuMaxMilliseconds, perSecond(stats.ticks),
                average(static_cast<double>(stats.units), stats.simSubmissions),
                average(stats.simGpuMilliseconds, stats.simSubmissionsMeasured), m_scheduler.BudgetMilliseconds(),
                average(stats.renderGpuMilliseconds, stats.renderFramesMeasured), m_scheduler.DroppedTicks(),
                stats.skippedSubmissions, stats.skippedExtractions, stats.cpuWaits, stats.events, m_latestHash.tick,
                m_latestHash.hash, m_latestHash.heat, m_latestHash.scheduledBlocks);
        }

        // --- ループ ---

        // 1 フレーム分(歩調の待ちの後)。デバイスの喪失や Present の失敗なら false
        bool FrameLoop::RunFrame(Clock::time_point frameStart) {
            CollectSimSubmissions();
            QueueClicks();
            if (!SubmitSim()) return false;
            SubmitRender();
            const auto presentStart = Clock::now();
            const bool presented = m_swapChain.Present(m_options.vsync);
            m_interval.presentMilliseconds += Milliseconds(Clock::now() - presentStart);
            if (!presented || IsDeviceLost()) return false;

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
            if (!m_replay.empty()) {
                Log(Channel::Sim, Level::Info, "再生: {}(コマンド {} 個、ハッシュ {} 個、刻み {} まで)",
                    ToUtf8(m_options.replayPath.wstring()), m_replay.front().CommandCount(),
                    m_replay.front().HashCount(), m_replay.front().LastTick());
            }
            const auto start = Clock::now();
            auto lastFrame = start;
            auto intervalStart = start;
            while (m_window->PumpMessages()) {
                if (m_options.frameLimit > 0 && m_frameNumber >= m_options.frameLimit) break;
                if (!m_replay.empty() && m_replay.front().Finished()) break;  // 最後のハッシュまで確かめた
                if (m_window->TakeResized() && !HandleResize()) return Finish(true, start);
                if (m_window->IsMinimized()) {
                    std::this_thread::sleep_for(MINIMIZED_SLEEP);
                    lastFrame = Clock::now();  // 最小化の間は世界を進めない
                    continue;
                }

                m_swapChain.WaitForFrame(FRAME_WAIT_TIMEOUT_MS);  // フレームの歩調(CPU が GPU より先に行き過ぎない)
                const auto frameStart = Clock::now();
                m_scheduler.AddRealTime(std::chrono::duration<double>(frameStart - lastFrame).count());
                lastFrame = frameStart;
                if (!RunFrame(frameStart)) return Finish(true, start);
                if (frameStart - intervalStart < STATS_INTERVAL) continue;
                LogStats(m_interval, frameStart - intervalStart, "1 秒");
                AddStats(m_interval);
                m_interval = {};
                intervalStart = frameStart;
            }
            return Finish(false, start);
        }

        // 終わり: GPU を待って残りを読み、全体の要約を出す
        int FrameLoop::Finish(bool failed, Clock::time_point start) {
            if (failed || IsDeviceLost()) {
                if (IsDeviceLost()) {
                    gpu::LogDeviceRemoved(m_device.Get());
                } else {
                    m_direct.Flush();  // 解放する前に GPU を終わらせる
                    m_compute.Flush();
                }
                Log(Channel::Core, Level::Error, "フレームのループが失敗で終わった");
                return 1;
            }
            m_direct.Flush();  // Present の分も待つ(バックバッファを解放する前に)
            m_compute.Flush();
            CollectSimSubmissions();
            AddStats(m_interval);
            LogStats(m_total, Clock::now() - start, "全体");
            const bool replayPassed = FinishReplay();
            if (m_device.ValidationErrorCount() > 0) {
                Log(Channel::Gpu, Level::Error, "debug layer のエラーが {} 件", m_device.ValidationErrorCount());
                return 1;
            }
            return replayPassed ? 0 : 1;
        }

        // 記録を書き、再生の結果を出す。記録を書けない・再生が合わない(終わっていない)なら false
        bool FrameLoop::FinishReplay() {
            bool passed = true;
            if (!m_options.recordPath.empty()) {
                const save::ReplayFile replay = m_recorder.Build();
                const auto written = save::WriteReplayFile(m_options.recordPath, replay);
                if (written) {
                    Log(Channel::Sim, Level::Info, "記録: {}(コマンド {} 個、ハッシュ {} 個)",
                        ToUtf8(m_options.recordPath.wstring()), replay.commands.size(), replay.tickHashes.size());
                } else {
                    Log(Channel::Sim, Level::Error, "記録を書けない: {}", written.error());
                    passed = false;
                }
            }
            if (m_replay.empty()) return passed;
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
        return loop.Run();
    }

}  // namespace bicameral::frame
