// gpu_probe_sim_test.cpp — T-0004 の仮の刻み(sim/probe_sim)を GPU で走らせ、CPU リファレンスとビット一致するかを確かめる。
//
// 確かめること(06「テスト」の 1 つ目の形):
//   - 同じ刻みの数とコマンドなら、バッチへの分け方(1 刻みずつ / 8 刻みずつ / ばらばら)を変えても、最後の状態が CPU と一致する
//   - 記録済みのリストを使い回し、刻みの数を ExecuteIndirect の数だけで変えても正しい(使わない枠は何もしない)
//   - つつきのイベントが、適用した刻みと場所で戻ってくる(数も合う)
//   - debug layer のエラーが 0 件
// 引数: gpu_test_options.h(--warp)。キューは compute だけ(シミュは compute キュー。06 §4)。
#include <algorithm>
#include <array>
#include <iterator>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "sim/probe_sim.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)
using Microsoft::WRL::ComPtr;

namespace {

    constexpr uint64_t TOTAL_TICKS = 40;

    // 刻み・場所。同じ刻みの同じセル(順番に依存しないか)と、格子の端・角も入れる
    struct PokeSpec {
        uint64_t tick;
        uint32_t x;
        uint32_t y;
    };
    constexpr std::array<PokeSpec, 7> POKES = {{
        {.tick = 0, .x = 10, .y = 10},
        {.tick = 0, .x = 10, .y = 10},
        {.tick = 5, .x = 0, .y = 0},
        {.tick = 5, .x = PROBE_GRID_SIZE - 1, .y = 64},
        {.tick = 13, .x = 64, .y = 64},
        {.tick = 21, .x = 65, .y = 64},
        {.tick = 39, .x = 100, .y = 3},
    }};

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition) return;
            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    std::vector<sim::ProbeCommand> MakeCommands() {
        std::vector<sim::ProbeCommand> commands;
        commands.reserve(POKES.size());
        uint32_t sequence = 0;
        for (const PokeSpec& poke : POKES) {
            commands.push_back(sim::MakePokeCommand(poke.tick, sequence++, poke.x, poke.y));
        }
        return commands;
    }

    uint64_t ReferenceHash(std::span<const sim::ProbeCommand> commands) {
        sim::ProbeReference reference;
        for (uint64_t tick = 0; tick < TOTAL_TICKS; ++tick) {
            reference.Advance(tick, commands);
        }
        return sim::HashCells(reference.State(TOTAL_TICKS));
    }

    struct RunResult {
        bool ok = false;
        uint64_t hash = 0;
        uint32_t eventCount = 0;
        bool eventsMatch = true;
    };

    // 抽出(COMMON)を読み戻す
    std::vector<uint32_t> ReadExtraction(ID3D12Device5* device, ID3D12Resource* extraction) {
        std::vector<uint32_t> cells(PROBE_CELL_COUNT);
        auto queue = gpu::ImmediateQueue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        const ComPtr<ID3D12Resource> readback =
            gpu::CreateBuffer(device, uint64_t{PROBE_CELL_COUNT} * 4, gpu::BufferKind::Readback);
        if (!queue || !readback) return {};
        ID3D12GraphicsCommandList10* list = queue->Begin();
        if (list == nullptr) return {};
        list->CopyBufferRegion(readback.Get(), 0, extraction, 0, uint64_t{PROBE_CELL_COUNT} * 4);
        if (!queue->ExecuteAndWait() || !gpu::ReadBuffer(readback.Get(), std::as_writable_bytes(std::span(cells)))) {
            return {};
        }
        return cells;
    }

    // 1 つのバッチのイベントが、コマンドの刻み・場所・バッチの範囲に合うか
    bool EventsMatch(const sim::ProbeBatchReadback& readback, std::span<const sim::ProbeCommand> commands,
                     uint64_t firstTick, uint32_t tickCount) {
        return std::ranges::all_of(readback.events, [&](const sim::ProbeEvent& event) {
            const bool known = std::ranges::any_of(commands, [&](const sim::ProbeCommand& command) {
                return command.targetTick == event.tick && command.payload[0] == event.x &&
                       command.payload[1] == event.y && event.type == PROBE_EVENT_POKE_APPLIED;
            });
            return known && event.tick >= firstTick && event.tick < firstTick + tickCount;
        });
    }

    std::vector<sim::ProbeCommand> CommandsInRange(std::span<const sim::ProbeCommand> commands, uint64_t firstTick,
                                                   uint32_t tickCount) {
        std::vector<sim::ProbeCommand> selected;
        std::ranges::copy_if(commands, std::back_inserter(selected), [&](const sim::ProbeCommand& command) {
            return command.targetTick >= firstTick && command.targetTick < firstTick + tickCount;
        });
        return selected;
    }

    // 刻みを batchSizes の分け方でバッチにして走らせる(バッチの枠を順に使い回す)
    RunResult RunSplit(ID3D12Device5* device, std::span<const uint32_t> batchSizes,
                       std::span<const sim::ProbeCommand> commands) {
        RunResult result;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"TestSim");
        auto simulation = sim::ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        if (!queue || !simulation) {
            Log(Channel::Sim, Level::Error, "作れない: {}{}", queue ? "" : queue.error(),
                simulation ? "" : simulation.error());
            return result;
        }

        uint64_t tick = 0;
        uint64_t batchNumber = 0;
        for (const uint32_t size : batchSizes) {
            ++batchNumber;
            const auto slot = static_cast<uint32_t>(batchNumber % sim::ProbeSim::BATCH_SLOT_COUNT);
            const std::vector<sim::ProbeCommand> batchCommands = CommandsInRange(commands, tick, size);
            ID3D12CommandList* list =
                simulation->PrepareBatch(slot, {.firstTick = tick,
                                                .tickCount = size,
                                                .extractionTarget = uint32_t(batchNumber % PROBE_EXTRACTION_COUNT),
                                                .commands = batchCommands});
            if (list == nullptr) return result;
            // テストは 1 つずつ終わりを待つ(フレームのループは待たない。frame/frame_loop.cpp)
            if (!queue->WaitCpu(queue->Submit(list))) return result;

            const sim::ProbeBatchReadback readback = simulation->ReadBatch(slot);
            result.eventCount += static_cast<uint32_t>(readback.events.size());
            result.eventsMatch = result.eventsMatch && EventsMatch(readback, commands, tick, size);
            tick += size;
        }
        const std::vector<uint32_t> cells =
            ReadExtraction(device, simulation->Extraction(static_cast<uint32_t>(batchNumber % PROBE_EXTRACTION_COUNT)));
        if (cells.size() != PROBE_CELL_COUNT) return result;
        result.hash = sim::HashCells(cells);
        result.ok = true;
        return result;
    }

}  // namespace

int main(int argc, char** argv) {
    const auto options = test::ParseGpuTestOptions(std::span(argv, static_cast<size_t>(argc)));
    if (!options) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_probe_sim_test [--warp]");
        bicameral::SingletonFinalizer::Finalize();
        return 2;
    }
    auto device = gpu::Device::Create(options->adapter);
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();
        return 1;
    }

    Failures failures;
    const std::vector<sim::ProbeCommand> commands = MakeCommands();
    const uint64_t expected = ReferenceHash(commands);
    Log(Channel::Sim, Level::Info, "CPU リファレンス: 刻み {} の後の要約 {:016x}", TOTAL_TICKS, expected);

    // 分け方: 8 ずつ / 1 ずつ / ばらばら(合計はどれも 40)
    const std::vector<uint32_t> byEight(TOTAL_TICKS / 8, 8);
    const std::vector<uint32_t> byOne(TOTAL_TICKS, 1);
    const std::vector<uint32_t> mixed = {3, 5, 7, 1, 8, 2, 6, 8};
    for (const auto& [name, sizes] : {std::pair{"8 ずつ", std::span<const uint32_t>(byEight)},
                                      std::pair{"1 ずつ", std::span<const uint32_t>(byOne)},
                                      std::pair{"ばらばら", std::span<const uint32_t>(mixed)}}) {
        const RunResult result = RunSplit(device->Get(), sizes, commands);
        Log(Channel::Sim, Level::Info, "GPU({}、{} バッチ): 要約 {:016x}  イベント {}", name, sizes.size(), result.hash,
            result.eventCount);
        failures.Check(result.ok, std::format("{}: 走らせられた", name));
        failures.Check(result.hash == expected, std::format("{}: CPU と一致", name));
        failures.Check(result.eventCount == POKES.size(), std::format("{}: イベントの数 {}", name, result.eventCount));
        failures.Check(result.eventsMatch, std::format("{}: イベントの刻みと場所", name));
    }

    const bool passesValidation = test::PassesValidation(*device, "gpu_probe_sim_test");
    const bool passed = failures.count == 0 && passesValidation;
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_sim_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();
    return passed ? 0 : 1;
}
