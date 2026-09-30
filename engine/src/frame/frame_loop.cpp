// frame_loop.cpp — 窓・2 つのキュー・記録済みのリスト・待たない読み戻しでフレームを回す(T-0004)。考え方は frame_loop.h。
//
// 1 フレームの流れ(CPU):
//   窓のメッセージ → スワップチェインの待ち(歩調)→ 経過時間を TickPacer へ
//   → 終わったバッチ・フレームの読み戻し(フェンスが進んだ分だけ。待たない)
//   → クリックをコマンドに → シミュのバッチを compute キューへ(描画が読み終えるまで GPU の上で待たせる)
//   → 描画を direct キューへ(終わっている最新のバッチを見せる。シミュを待たない)→ Present
//
// 抽出の 3 組の約束(06 §4): バッチ b は抽出 b % 3 に書く。描画は「見せるバッチ」d(CPU から見て終わっている最新)の
//   抽出 d % 3 を読む。次に抽出 d % 3 に書くのはバッチ d + 3 で、それは描画のフェンスを GPU の上で待ってから走る
//   (m_lastRenderReading)。バッチは 2 つまでしか重ねないので、終わっている最新 d ≥ 最後に投げたバッチ − 2
//   → d + 3 はまだ投げていない(必ずこの待ちを通る)。描画がシミュを待つことは無い(D-201)。
#include "frame/frame_loop.h"

#include <chrono>
#include <expected>
#include <thread>

#include "core/log.h"
#include "frame/tick_pacer.h"
#include "gpu/queue.h"
#include "gpu/resources.h"
#include "gpu/swap_chain.h"
#include "platform/window.h"
#include "render/probe_view.h"
#include "sim/probe_sim.h"

using Microsoft::WRL::ComPtr;

namespace bicameral::frame {
    namespace {

        using Clock = std::chrono::steady_clock;

        constexpr uint32_t FRAME_SLOT_COUNT = gpu::SwapChain::BUFFER_COUNT;
        constexpr uint32_t BATCH_SLOT_COUNT = sim::ProbeSim::BATCH_SLOT_COUNT;
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

        double TimestampMilliseconds(uint64_t begin, uint64_t end, uint64_t frequency) {
            if (frequency == 0 || end < begin) return 0.0;
            return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frequency);
        }

        // 描画のフレームごとに持つもの(バックバッファの番号 = スワップチェインの CurrentIndex)
        struct FrameSlot {
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList10> list;  // 大きさが変わったときだけ記録し直す
            ComPtr<ID3D12Resource> constants;          // アップロード(render::ProbeViewConstants)
            render::ProbeViewConstants* mappedConstants = nullptr;
            uint64_t renderFence = 0;  // このリストを最後に投げた direct のフェンスの値(0 = まだ)
        };

        // 投げたバッチ(バッチの枠ごと)。フェンスが number に届いたら読む
        struct BatchRecord {
            uint64_t number = 0;  // compute のフェンスの値 = バッチの通し番号(1 から)
            uint64_t firstTick = 0;
            uint32_t tickCount = 0;
            bool read = true;
        };

        // 重さの試験を別々の投入にして、何フレームかに分けて投げている途中のバッチ(R-LOOP-2、T-0085)。list が nullptr なら無し
        struct PendingBatch {
            ID3D12CommandList* list = nullptr;
            uint32_t slot = 0;
            uint32_t extractionTarget = 0;
            uint32_t piecesLeft = 0;  // まだ投げていない重さの個数
            uint32_t piecesDone = 0;
        };

        struct PendingClick {
            uint32_t x = 0;
            uint32_t y = 0;
            uint64_t tick = UINT64_MAX;  // 載せたバッチの刻み(まだなら UINT64_MAX)
            Clock::time_point time;
        };

        // 計測(1 秒ごとにログへ。最後に全体の要約)
        struct Stats {
            uint64_t frames = 0;
            double cpuMilliseconds = 0.0;
            double cpuMaxMilliseconds = 0.0;
            double presentMilliseconds = 0.0;  // cpuMilliseconds のうち Present の中にいた時間
            uint64_t ticks = 0;
            uint64_t batches = 0;
            double simGpuMilliseconds = 0.0;
            uint64_t simBatchesMeasured = 0;
            double renderGpuMilliseconds = 0.0;
            uint64_t renderFramesMeasured = 0;
            uint64_t cpuWaits = 0;        // 描画の枠を使い回す前に CPU が GPU を待った回数(0 のはず)
            uint64_t skippedBatches = 0;  // シミュが終わっていないので投げなかったフレーム
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
                  m_computeFrequency(m_compute.TimestampFrequency()),
                  m_directFrequency(m_direct.TimestampFrequency()) {}

            [[nodiscard]] bool CreateFrameSlots();
            [[nodiscard]] int Run();

        private:
            bool RecordRenderLists();
            bool HandleResize();

            void CollectBatches();
            void ReportEvents(const sim::ProbeBatchReadback& readback);
            void CollectFrameSlot(uint32_t slotIndex);
            void QueueClicks();
            size_t AssignCommandTicks();
            void SubmitSimBatch();
            void AdvancePendingBatch();
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
            std::array<FrameSlot, FRAME_SLOT_COUNT> m_frames;
            ComPtr<ID3D12QueryHeap> m_renderTimestamps;
            ComPtr<ID3D12Resource> m_renderTimestampReadback;

            TickPacer m_pacer;
            std::array<BatchRecord, BATCH_SLOT_COUNT> m_batches;
            uint64_t m_nextTick = 0;
            uint64_t m_lastDisplayedBatch = 0;
            PendingBatch m_pending;  // 重さを別々の投入にして、何フレームかに分けて投げている途中のバッチ(R-LOOP-2)
            // 抽出の組ごとに、最後にそれを読んだ描画のフェンスの値
            std::array<uint64_t, sim::PROBE_EXTRACTION_COUNT> m_lastRenderReading{};
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

            // 描画のキューは優先度を上げる: 重いシミュと並んでも描画が先に進みやすい(06 §4・R-LOOP-2。docs/perf.md)
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
                                      {.busyPieces = options.simSplit, .busyInSeparateSubmits = options.splitSubmit});
            if (!simulation) return std::unexpected(simulation.error());
            auto view = render::ProbeView::Create(native, gpu::SwapChain::FORMAT);
            if (!view) return std::unexpected(view.error());
            return FrameLoopParts{.window = std::move(*window),
                                  .device = std::move(*device),
                                  .direct = std::move(*direct),
                                  .compute = std::move(*compute),
                                  .swapChain = std::move(*swapChain),
                                  .simulation = std::move(*simulation),
                                  .view = std::move(*view)};
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
            CollectBatches();
            for (uint32_t index = 0; index < FRAME_SLOT_COUNT; ++index) {
                CollectFrameSlot(index);
            }
            if (!m_swapChain.Resize(m_device.Get(), m_window->ClientWidth(), m_window->ClientHeight())) return false;
            Log(Channel::Render, Level::Info, "窓の大きさ: {}×{}", m_swapChain.Width(), m_swapChain.Height());
            return RecordRenderLists();
        }

        // --- 読み戻し(待たない)---

        void FrameLoop::CollectBatches() {
            for (uint32_t slot = 0; slot < BATCH_SLOT_COUNT; ++slot) {
                BatchRecord& batch = m_batches[slot];
                if (batch.read || !m_compute.IsComplete(batch.number)) continue;
                batch.read = true;
                const sim::ProbeBatchReadback readback = m_sim.ReadBatch(slot);
                const double gpuMilliseconds =
                    TimestampMilliseconds(readback.gpuBeginTimestamp, readback.gpuEndTimestamp, m_computeFrequency);
                m_pacer.ReportGpuTime(gpuMilliseconds, batch.tickCount);
                m_interval.simGpuMilliseconds += gpuMilliseconds;
                ++m_interval.simBatchesMeasured;
                ReportEvents(readback);
            }
        }

        // つつきが適用されたことをログへ(クリックから CPU に戻るまでの時間つき)
        void FrameLoop::ReportEvents(const sim::ProbeBatchReadback& readback) {
            const auto now = Clock::now();
            for (const sim::ProbeEvent& event : readback.events) {
                ++m_interval.events;
                const auto click = std::ranges::find_if(m_clicks, [&](const PendingClick& pending) {
                    return pending.tick == event.tick && pending.x == event.x && pending.y == event.y;
                });
                const double latency = click != m_clicks.end() ? Milliseconds(now - click->time) : 0.0;
                Log(Channel::Sim, Level::Info,
                    "つつき ({}, {}) を刻み {} で適用(クリックから CPU に戻るまで {:.1f} ms)", event.x, event.y,
                    event.tick, latency);
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
                m_interval.renderGpuMilliseconds +=
                    TimestampMilliseconds(timestamps[first], timestamps[first + 1], m_directFrequency);
                ++m_interval.renderFramesMeasured;
            }
            slot.renderFence = 0;
        }

        // --- 入力 → コマンド ---

        void FrameLoop::QueueClicks() {
            std::vector<PointerEvent> pointers = m_window->TakePointerEvents();
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
                m_pendingCommands.push_back(sim::MakePokeCommand(0, m_nextSequence++, cellX, cellY));
                m_clicks.push_back({.x = cellX, .y = cellY, .time = Clock::now()});
            }
        }

        // 次のバッチの最初の刻みを、載せるコマンドに付ける(載せきれない分は次のバッチへ)。載せる数を返す
        size_t FrameLoop::AssignCommandTicks() {
            const size_t commandCount = std::min<size_t>(m_pendingCommands.size(), sim::PROBE_MAX_COMMANDS);
            for (sim::ProbeCommand& command : std::span(m_pendingCommands).first(commandCount)) {
                command.targetTick = m_nextTick;
                const auto click = std::ranges::find_if(m_clicks, [&](const PendingClick& pending) {
                    return pending.tick == UINT64_MAX && pending.x == command.payload[0] &&
                           pending.y == command.payload[1];
                });
                if (click != m_clicks.end()) click->tick = m_nextTick;
            }
            return commandCount;
        }

        // --- 投げる ---

        void FrameLoop::SubmitSimBatch() {
            if (m_pending.list != nullptr) {
                AdvancePendingBatch();
                return;
            }
            const uint64_t submitted = m_compute.LastSubmitted();
            // 2 つまで重ねる(GPU が次のバッチをすぐ始められるように)。それ以上は投げない(抽出の 3 組の約束。ファイルの先頭)
            if (submitted - m_compute.CompletedValue() >= 2) {
                ++m_interval.skippedBatches;
                return;
            }
            const uint64_t number = submitted + 1;
            const auto slot = static_cast<uint32_t>(number % BATCH_SLOT_COUNT);
            if (!m_batches[slot].read) return;  // その枠の前のバッチを読めていない(起きないはず)
            const uint32_t tickCount = m_pacer.TakeTicks();
            if (tickCount == 0) return;

            const size_t commandCount = AssignCommandTicks();
            const auto extractionTarget = static_cast<uint32_t>(number % sim::PROBE_EXTRACTION_COUNT);
            ID3D12CommandList* list =
                m_sim.PrepareBatch(slot, {.firstTick = m_nextTick,
                                          .tickCount = tickCount,
                                          .extractionTarget = extractionTarget,
                                          .busyIterations = m_options.simLoad,
                                          .commands = std::span(m_pendingCommands).first(commandCount)});
            if (list == nullptr) return;
            m_pendingCommands.erase(m_pendingCommands.begin(),
                                    m_pendingCommands.begin() + static_cast<std::ptrdiff_t>(commandCount));

            m_batches[slot] = {.number = number, .firstTick = m_nextTick, .tickCount = tickCount, .read = false};
            m_nextTick += tickCount;
            m_interval.ticks += tickCount;
            ++m_interval.batches;

            if (m_sim.BusyInSeparateSubmits()) {
                m_pending = {.list = list,
                             .slot = slot,
                             .extractionTarget = extractionTarget,
                             .piecesLeft = tickCount * m_sim.BusyPieceCount()};
                AdvancePendingBatch();
                return;
            }
            // 抽出 extractionTarget を描画が読み終えるまで、GPU の上で待ってから走る
            m_compute.GpuWait(m_direct, m_lastRenderReading[extractionTarget]);
            m_compute.Submit(list);
        }

        // 重さの試験を別々の投入にするとき(R-LOOP-2、T-0085): 重さの 1 個ずつを、フェンスを進めずに投げる。
        // --pieces-per-frame p なら 1 フレームに p 個まで(残りは次のフレーム)。compute のキューがフレームごとに空になり、
        // その後ろに投げる描画が間に入れる。全部投げたらバッチのリスト(フェンスを進める)を投げる
        void FrameLoop::AdvancePendingBatch() {
            const uint32_t perFrame = m_options.piecesPerFrame;
            const uint32_t count = perFrame == 0 ? m_pending.piecesLeft : std::min(perFrame, m_pending.piecesLeft);
            for (uint32_t piece = 0; piece < count; ++piece) {
                m_compute.Execute(m_sim.BusyPieceList(m_pending.slot, m_pending.piecesDone));
                ++m_pending.piecesDone;
                --m_pending.piecesLeft;
            }
            if (m_pending.piecesLeft > 0) return;
            m_compute.GpuWait(m_direct, m_lastRenderReading[m_pending.extractionTarget]);
            m_compute.Submit(m_pending.list);
            m_pending = {};
        }

        void FrameLoop::SubmitRender() {
            const uint32_t index = m_swapChain.CurrentIndex();
            CollectFrameSlot(index);
            FrameSlot& slot = m_frames[index];

            // 見せるバッチ: CPU から見て終わっている最新(シミュを待たない。抽出の 3 組の約束。ファイルの先頭)
            const uint64_t displayed = std::max(m_lastDisplayedBatch, m_compute.CompletedValue());
            m_lastDisplayedBatch = displayed;
            const auto extraction = static_cast<uint32_t>(displayed % sim::PROBE_EXTRACTION_COUNT);

            *slot.mappedConstants = {
                .extractionIndex = extraction, .width = m_swapChain.Width(), .height = m_swapChain.Height()};
            m_direct.GpuWait(m_compute, displayed);
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
            m_total.batches += frame.batches;
            m_total.simGpuMilliseconds += frame.simGpuMilliseconds;
            m_total.simBatchesMeasured += frame.simBatchesMeasured;
            m_total.renderGpuMilliseconds += frame.renderGpuMilliseconds;
            m_total.renderFramesMeasured += frame.renderFramesMeasured;
            m_total.cpuWaits += frame.cpuWaits;
            m_total.skippedBatches += frame.skippedBatches;
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
                "{}: {:.1f} fps  CPU {:.3f} ms/フレーム(うち Present {:.3f}、最大 {:.3f})  "
                "世界 {:.1f} 刻み/秒({:.2f} 刻み/バッチ)  シミュ GPU {:.3f} ms/バッチ  描画 GPU {:.3f} ms/フレーム  "
                "捨てた刻み {}  見送り {}  CPU の待ち {}  イベント {}",
                label, perSecond(stats.frames), average(stats.cpuMilliseconds, stats.frames),
                average(stats.presentMilliseconds, stats.frames), stats.cpuMaxMilliseconds, perSecond(stats.ticks),
                average(static_cast<double>(stats.ticks), stats.batches),
                average(stats.simGpuMilliseconds, stats.simBatchesMeasured),
                average(stats.renderGpuMilliseconds, stats.renderFramesMeasured), m_pacer.DroppedTicks(),
                stats.skippedBatches, stats.cpuWaits, stats.events);
        }

        // --- ループ ---

        // 1 フレーム分(歩調の待ちの後)。デバイスの喪失や Present の失敗なら false
        bool FrameLoop::RunFrame(Clock::time_point frameStart) {
            CollectBatches();
            QueueClicks();
            SubmitSimBatch();
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
                "フレームのループを始める(vsync {}  先行 {}  重さ {}(分けて {} 個・{})  描画の優先度 {}  自動クリック "
                "{})",
                m_options.vsync ? "あり" : "なし", m_options.maxFrameLatency, m_options.simLoad, m_options.simSplit,
                m_options.splitSubmit ? std::format("別々の投入・1 フレームに {} 個", m_options.piecesPerFrame)
                                      : std::string("リストの中"),
                m_options.renderHighPriority ? "HIGH" : "NORMAL", m_options.autoClick ? "あり" : "なし");
            const auto start = Clock::now();
            auto lastFrame = start;
            auto intervalStart = start;
            while (m_window->PumpMessages()) {
                if (m_options.frameLimit > 0 && m_frameNumber >= m_options.frameLimit) break;
                if (m_window->TakeResized() && !HandleResize()) return Finish(true, start);
                if (m_window->IsMinimized()) {
                    std::this_thread::sleep_for(MINIMIZED_SLEEP);
                    lastFrame = Clock::now();  // 最小化の間は世界を進めない
                    continue;
                }

                m_swapChain.WaitForFrame(FRAME_WAIT_TIMEOUT_MS);  // フレームの歩調(CPU が GPU より先に行き過ぎない)
                const auto frameStart = Clock::now();
                m_pacer.AddRealTime(std::chrono::duration<double>(frameStart - lastFrame).count());
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
            CollectBatches();
            AddStats(m_interval);
            LogStats(m_total, Clock::now() - start, "全体");
            if (m_device.ValidationErrorCount() > 0) {
                Log(Channel::Gpu, Level::Error, "debug layer のエラーが {} 件", m_device.ValidationErrorCount());
                return 1;
            }
            return 0;
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
