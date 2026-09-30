// gpu_probe_sim_test.cpp — 仮の刻み(sim/probe_sim)を単位の列として GPU で走らせ、CPU リファレンスとビット一致するかを確かめる(T-0004・T-0012)。
//
// 確かめること(06「テスト」の 1 つ目の形):
//   - 同じ刻みの数とコマンドなら、フレームへの単位の分け方(1 刻みずつ / 8 刻みずつ / 1 単位ずつ / 刻みの途中で切るばらばら /
//     重さの単位を足して分ける)を変えても、GPU が刻みごとに取った状態のハッシュ列が、CPU リファレンスのハッシュ列と一致する
//   - 最後の抽出(描画が読むもの)が、最後の刻みの状態と一致する
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
    constexpr uint32_t TEST_BUSY_ITERATIONS = 64;  // 重さの単位を入れる分け方で(結果に入らないことを確かめる)
    constexpr uint32_t TEST_BUSY_PIECES = 3;

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

    // CPU リファレンスの S(1)〜S(TOTAL_TICKS) のハッシュ([t] が S(t)。[0] は S(0))
    std::vector<uint64_t> ReferenceHashes(std::span<const sim::ProbeCommand> commands) {
        sim::ProbeReference reference;
        std::vector<uint64_t> hashes = {sim::ProbeStateHash(reference.State(0))};
        for (uint64_t tick = 0; tick < TOTAL_TICKS; ++tick) {
            reference.Advance(tick, commands);
            hashes.push_back(sim::ProbeStateHash(reference.State(tick + 1)));
        }
        return hashes;
    }

    // 分け方: 1 フレームに投げる単位の数の列。pattern を繰り返して合計 total にする(最後は切る)
    std::vector<uint32_t> RepeatPattern(std::span<const uint32_t> pattern, uint32_t total) {
        std::vector<uint32_t> sizes;
        uint32_t sum = 0;
        for (size_t index = 0; sum < total; ++index) {
            const uint32_t size = std::min(pattern[index % pattern.size()], total - sum);
            sizes.push_back(size);
            sum += size;
        }
        return sizes;
    }

    struct Plan {
        const char* name;
        sim::ProbeSimOptions options;
        std::vector<uint32_t> unitsPerFrame;
    };

    struct RunResult {
        bool ok = false;
        std::vector<sim::ProbeTickHash> hashes;
        uint64_t extractionHash = 0;
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

    // 刻み [firstTick, lastTick] を触るフレームのコマンド(適用の単位が targetTick で選ぶ)
    std::vector<sim::ProbeCommand> CommandsInRange(std::span<const sim::ProbeCommand> commands, uint64_t firstTick,
                                                   uint64_t lastTick) {
        std::vector<sim::ProbeCommand> selected;
        std::ranges::copy_if(commands, std::back_inserter(selected), [&](const sim::ProbeCommand& command) {
            return command.targetTick >= firstTick && command.targetTick <= lastTick;
        });
        return selected;
    }

    // 1 フレームのイベントが、コマンドの刻み・場所・フレームの刻みの範囲に合うか
    bool EventsMatch(const sim::ProbeFrameReadback& readback, std::span<const sim::ProbeCommand> commands,
                     uint64_t firstTick, uint64_t lastTick) {
        return std::ranges::all_of(readback.events, [&](const sim::ProbeEvent& event) {
            const bool known = std::ranges::any_of(commands, [&](const sim::ProbeCommand& command) {
                return command.targetTick == event.tick && command.payload[0] == event.x &&
                       command.payload[1] == event.y && event.type == PROBE_EVENT_POKE_APPLIED;
            });
            return known && event.tick >= firstTick && event.tick <= lastTick;
        });
    }

    // 単位を plan の分け方でフレームにして走らせる(フレームの枠を順に使い回す。毎フレーム抽出する)
    RunResult RunPlan(ID3D12Device5* device, const Plan& plan, std::span<const sim::ProbeCommand> commands) {
        RunResult result;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"TestSim");
        auto simulation = sim::ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, plan.options);
        if (!queue || !simulation) {
            Log(Channel::Sim, Level::Error, "作れない: {}{}", queue ? "" : queue.error(),
                simulation ? "" : simulation.error());
            return result;
        }

        const uint32_t unitsPerTick = simulation->UnitsPerTick();
        uint64_t unitPosition = 0;  // 通しの単位の番号(刻み × 1 刻みの単位の数 + 刻みの中の番号)
        uint32_t extractionTarget = 0;
        for (size_t frame = 0; frame < plan.unitsPerFrame.size(); ++frame) {
            const uint32_t unitCount = plan.unitsPerFrame[frame];
            const uint64_t firstTick = unitPosition / unitsPerTick;
            const uint64_t lastTick = (unitPosition + unitCount - 1) / unitsPerTick;
            const auto slot = static_cast<uint32_t>(frame % sim::ProbeSim::FRAME_SLOT_COUNT);
            extractionTarget = static_cast<uint32_t>(frame % PROBE_EXTRACTION_COUNT);
            const std::vector<sim::ProbeCommand> frameCommands = CommandsInRange(commands, firstTick, lastTick);
            ID3D12CommandList* list =
                simulation->RecordFrame(slot, {.firstTick = firstTick,
                                               .firstUnit = static_cast<uint32_t>(unitPosition % unitsPerTick),
                                               .unitCount = unitCount,
                                               .extract = true,
                                               .extractionTarget = extractionTarget,
                                               .commands = frameCommands});
            if (list == nullptr) return result;
            // テストは 1 つずつ終わりを待つ(フレームのループは待たない。frame/frame_loop.cpp)
            if (!queue->WaitCpu(queue->Submit(list))) return result;

            const sim::ProbeFrameReadback readback = simulation->ReadFrame(slot);
            result.hashes.insert(result.hashes.end(), readback.hashes.begin(), readback.hashes.end());
            result.eventCount += static_cast<uint32_t>(readback.events.size());
            result.eventsMatch = result.eventsMatch && EventsMatch(readback, commands, firstTick, lastTick);
            unitPosition += unitCount;
        }
        const std::vector<uint32_t> cells = ReadExtraction(device, simulation->Extraction(extractionTarget));
        if (cells.size() != PROBE_CELL_COUNT) return result;
        result.extractionHash = sim::ProbeStateHash(cells);
        result.ok = unitPosition == TOTAL_TICKS * unitsPerTick;
        return result;
    }

    // GPU のハッシュ列が S(1)〜S(TOTAL_TICKS) の順に並び、CPU と一致するか
    bool HashesMatch(std::span<const sim::ProbeTickHash> hashes, std::span<const uint64_t> expected) {
        if (hashes.size() != TOTAL_TICKS) return false;
        for (size_t index = 0; index < hashes.size(); ++index) {
            if (hashes[index].tick != index + 1 || hashes[index].hash != expected[index + 1]) {
                Log(Channel::Sim, Level::Error, "  S({}) = {:016x}(CPU S({}) = {:016x})", hashes[index].tick,
                    hashes[index].hash, index + 1, expected[index + 1]);
                return false;
            }
        }
        return true;
    }

    std::vector<Plan> MakePlans() {
        const uint32_t plainUnits = PROBE_FIXED_UNITS_PER_TICK;
        const uint32_t busyUnits = PROBE_FIXED_UNITS_PER_TICK + TEST_BUSY_PIECES;
        const auto plainTotal = static_cast<uint32_t>(TOTAL_TICKS * plainUnits);
        const auto busyTotal = static_cast<uint32_t>(TOTAL_TICKS * busyUnits);
        const sim::ProbeSimOptions busy{.busyIterations = TEST_BUSY_ITERATIONS, .busyPieces = TEST_BUSY_PIECES};
        const std::array<uint32_t, 7> mixed = {1, 2, 5, 4, 7, 11, 3};  // 刻みの途中で切れる
        const std::array<uint32_t, 4> mixedBusy = {5, 7, 2, 13};
        return {
            {.name = "1 刻みずつ", .options = {}, .unitsPerFrame = std::vector<uint32_t>(TOTAL_TICKS, plainUnits)},
            {.name = "8 刻みずつ",
             .options = {},
             .unitsPerFrame = std::vector<uint32_t>(TOTAL_TICKS / 8, plainUnits * 8)},
            {.name = "1 単位ずつ", .options = {}, .unitsPerFrame = std::vector<uint32_t>(plainTotal, 1)},
            {.name = "ばらばら", .options = {}, .unitsPerFrame = RepeatPattern(mixed, plainTotal)},
            {.name = "重さを分けてばらばら", .options = busy, .unitsPerFrame = RepeatPattern(mixedBusy, busyTotal)},
        };
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
    const std::vector<uint64_t> expected = ReferenceHashes(commands);
    Log(Channel::Sim, Level::Info, "CPU リファレンス: S({}) = {:016x}", TOTAL_TICKS, expected.back());

    for (const Plan& plan : MakePlans()) {
        const RunResult result = RunPlan(device->Get(), plan, commands);
        Log(Channel::Sim, Level::Info, "GPU({}、{} フレーム): S({}) = {:016x}  抽出 {:016x}  イベント {}", plan.name,
            plan.unitsPerFrame.size(), result.hashes.empty() ? 0 : result.hashes.back().tick,
            result.hashes.empty() ? 0 : result.hashes.back().hash, result.extractionHash, result.eventCount);
        failures.Check(result.ok, std::format("{}: 走らせられた", plan.name));
        failures.Check(HashesMatch(result.hashes, expected),
                       std::format("{}: 刻みごとのハッシュが CPU と一致", plan.name));
        failures.Check(result.extractionHash == expected.back(), std::format("{}: 最後の抽出が CPU と一致", plan.name));
        failures.Check(result.eventCount == POKES.size(),
                       std::format("{}: イベントの数 {}", plan.name, result.eventCount));
        failures.Check(result.eventsMatch, std::format("{}: イベントの刻みと場所", plan.name));
    }

    const bool passesValidation = test::PassesValidation(*device, "gpu_probe_sim_test");
    const bool passed = failures.count == 0 && passesValidation;
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_sim_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();
    return passed ? 0 : 1;
}
