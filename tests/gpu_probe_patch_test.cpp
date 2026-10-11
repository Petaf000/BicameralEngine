// gpu_probe_patch_test.cpp — 仮の世界(ProbeSim)に魔法のパッチを置くコマンド(PROBE_COMMAND_TYPE_PATCH。T-0323・ADR-0095)が、
// GPU と CPU リファレンスで毎刻みビット一致し、再生ファイル・保存点・表の差し替えを跨いでも同じ結果になるかを確かめる。
// 値はすべて試験用の仮のもの(D-006)。
//
// 確かめること:
//   - (CPU)コマンドの語の往復(MakePatchCommand → DecodePatchCommand)・範囲が重なるブロック(格子の外は切る・重ならなければ無し)
//   - (CPU)木箱の壁に火を付け、セルロースの 2 つの規則を止めた所は、止めている間セルロースが減らない(止めない世界では減る)。
//     パッチのある世界と無い世界は違う
//   - (GPU)火・止める・速める・格子の外・角にはみ出す・規則が表に無い(断る)・列が一杯(断る)・表の差し替え(規則の ID がずれる)を
//     跨ぐパッチ・差し替えた後のパッチを、(1) 1 刻みずつ + 保存点へ戻して流し直す、(2) 刻みの途中で切るばらばらの分け方、で流し、
//     刻みごとの状態の要約(セル + パッチの列)・エネルギー・湧き出し・計算したブロックの数・上限・断った数が CPU と一致する。
//     コマンドは再生ファイル(SerializeReplay → ParseReplay)から ReplayPlayer で足し、記録の要約とも突き合わせる
//   - debug layer のエラーが 0 件
// 引数: gpu_test_options.h(--warp)。キューは compute だけ。
#include <algorithm>
#include <array>
#include <format>
#include <tuple>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu/queue.h"
#include "gpu_test_options.h"
#include "reaction_test_table.h"
#include "save/replay_file.h"
#include "save/replay_session.h"
#include "sim/probe_sim.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli・probe_patch.hlsli の定数

namespace {

    constexpr uint64_t TOTAL_TICKS = 32;
    constexpr uint64_t STOP_END_TICK = 12;  // 止めるパッチの終わり(刻み 0 から 12 刻み)
    constexpr uint64_t SWAP_TICK = 16;      // 規則の ID がずれる表に替える刻み
    constexpr uint64_t SWAP_VERSION = 0x50A7C4;
    constexpr uint64_t SAVE_TICK = 8;        // 保存点(パッチが効いている途中)
    constexpr uint64_t REWIND_AT_TICK = 15;  // ここまで進めてから SAVE_TICK へ戻す(差し替えの前)
    constexpr uint64_t UNKNOWN_RULE_KEY = 0x1234;

    // 火を付ける木箱の壁のセル(左の壁と右の壁。木箱は [28, 36) の 8³)
    constexpr std::array<uint32_t, 3> LEFT_WALL = {28, 32, 32};
    constexpr std::array<uint32_t, 3> RIGHT_WALL = {35, 32, 32};

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition)
                return;

            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    uint64_t RuleKey(const BakedReactionTable& table, std::string_view name) {
        const auto found = rng::find(table.ruleNames, name);
        FX_ASSERT(found != table.ruleNames.end());

        return table.rules[static_cast<size_t>(std::distance(table.ruleNames.begin(), found))].key;
    }

    uint32_t SpeciesId(const BakedReactionTable& table, std::string_view name) {
        return static_cast<uint32_t>(std::distance(table.speciesNames.begin(), rng::find(table.speciesNames, name)));
    }

    uint64_t AmountOf(const reaction::RxCell& cell, uint32_t species) {
        const uint32_t slot = reaction::RxFindSlot(cell, species);
        return slot == reaction::RX_NO_SLOT ? 0 : cell.amounts[slot];
    }

    // 規則の ID がずれる表: 名前の順で前にある規則(boudouard)を抜く(gpu_lab_box_test の MakeShiftedTable と同じ。物質は同じ)
    std::expected<BakedReactionTable, std::string> MakeShiftedTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        std::erase_if(definition.rules, [](const RuleDefinition& rule) { return rule.name == "boudouard"; });

        return BakeReactionTable(definition);
    }

    // --- CPU: コマンドの形と範囲 ---

    void TestCommandShape(const BakedReactionTable& table, Failures& failures) {
        const uint64_t key = RuleKey(table, "cellulose_combustion");
        const ProbeCommand command = MakePatchCommand(7, 3, key, reaction::MpMakeScaleEffect(-5), 9,
                                                      {.low = {-4, 2, 60}, .high = {3, 5, 70}});
        const reaction::MpPatch patch = DecodePatchCommand(command);
        failures.Check(command.type == PROBE_COMMAND_TYPE_PATCH && patch.ruleKey == key && patch.startTick == 7 &&
                           patch.endTick == 16 && reaction::MpEffectShift(patch.effect) == -5 && patch.lowX == -4 &&
                           patch.highZ == 70,
                       "パッチのコマンドの語が往復する");

        const ProbePatchBlocks edge = ProbePatchBlockRange(patch);
        failures.Check(edge.valid == 1 && edge.beginX == 0 && edge.endX == 0 && edge.beginY == 0 && edge.endY == 1 &&
                           edge.beginZ == 15 && edge.endZ == 15,
                       "範囲が重なるブロック: 格子の外は切る");

        const ProbeCommand outside = MakePatchCommand(0, 0, key, reaction::MpMakeStopEffect(), 1,
                                                      {.low = {-10, 0, 0}, .high = {-1, 63, 63}});
        failures.Check(ProbePatchBlockRange(DecodePatchCommand(outside)).valid == 0,
                       "格子と重ならない範囲はブロック無し");
    }

    // --- 場面 ---

    std::vector<ProbeCommand> MakeCommands(const BakedReactionTable& table, const BakedReactionTable& shifted,
                                           bool withPatches) {
        uint32_t sequence = 0;
        std::vector<ProbeCommand> commands;
        const auto patch = [&](uint64_t tick, std::string_view rule, uint32_t effect, uint32_t duration,
                               ProbePatchRange range) {
            if (withPatches)
                commands.push_back(MakePatchCommand(tick, sequence++, RuleKey(table, rule), effect, duration, range));
        };

        // 刻み 0: 左右の壁に火。左の壁のまわりはセルロースの 2 つの規則を止め、右の壁のまわりは炭の燃焼を 2^10 倍(差し替えを跨ぐ)
        commands.push_back(MakePokeCommand(0, sequence++, LEFT_WALL[0], LEFT_WALL[1], LEFT_WALL[2]));
        commands.push_back(MakePokeCommand(0, sequence++, RIGHT_WALL[0], RIGHT_WALL[1], RIGHT_WALL[2]));
        const ProbePatchRange left = {.low = {26, 30, 30}, .high = {29, 34, 34}};
        patch(0, "cellulose_combustion", reaction::MpMakeStopEffect(), STOP_END_TICK, left);
        patch(0, "cellulose_pyrolysis", reaction::MpMakeStopEffect(), STOP_END_TICK, left);
        patch(0, "carbon_combustion", reaction::MpMakeScaleEffect(10), 40, {.low = {34, 30, 30}, .high = {37, 34, 34}});

        // 刻み 2: 表に無い規則(断る)。刻み 3: 格子の外だけ(足せるがどのブロックも起こさない)。
        // 刻み 4: 角にはみ出す(眠っている空気のブロックを始まり・終わりに起こす)
        if (withPatches) {
            commands.push_back(MakePatchCommand(2, sequence++, UNKNOWN_RULE_KEY, reaction::MpMakeStopEffect(), 5,
                                                {.low = {0, 0, 0}, .high = {7, 7, 7}}));
        }

        patch(3, "cellulose_combustion", reaction::MpMakeStopEffect(), 6, {.low = {-9, -9, -9}, .high = {-2, -2, -2}});
        patch(4, "cellulose_pyrolysis", reaction::MpMakeScaleEffect(-5), 5, {.low = {-4, -4, -4}, .high = {3, 9, 3}});

        // 刻み 5: 小さいパッチを 12 個(列は 5 + 12 = 17 → 最後の 1 個は一杯で断る)。3 刻みで終わる
        for (int32_t i = 0; i < 12; ++i) {
            const int32_t x = 4 + (i * 4);
            patch(5, "carbon_partial_combustion", reaction::MpMakeScaleEffect(1), 3,
                  {.low = {x, 50, 10}, .high = {x + 1, 51, 11}});
        }

        // 刻み 16: 規則の ID がずれる表に替える印。刻み 20: 替えた表の名前の鍵で止める(右の壁)
        commands.push_back(MakeTableCommand(SWAP_TICK, sequence++, SWAP_VERSION));
        if (withPatches) {
            commands.push_back(MakePatchCommand(20, sequence++, RuleKey(shifted, "cellulose_combustion"),
                                                reaction::MpMakeStopEffect(), 6,
                                                {.low = {35, 31, 31}, .high = {35, 33, 33}}));
        }

        rng::sort(commands, CommandPrecedes);

        return commands;
    }

    uint32_t PatchCommandCount(std::span<const ProbeCommand> commands) {
        return static_cast<uint32_t>(rng::count_if(
            commands, [](const ProbeCommand& command) { return command.type == PROBE_COMMAND_TYPE_PATCH; }));
    }

    // --- CPU リファレンス ---

    struct Reference {
        std::vector<ProbeTickHash> ticks;        // [t] が S(t)
        uint64_t leftCelluloseWhileStopped = 0;  // S(STOP_END_TICK) の左の壁のセルロース
        uint64_t leftCelluloseInitial = 0;
        uint32_t patchesAtSwap = 0;  // 差し替えの刻みの初めに列にあったパッチの数
    };

    Reference RunReference(const BakedReactionTable& table, const BakedReactionTable& shifted,
                           std::span<const ProbeCommand> commands, uint64_t ticks) {
        ProbeReference reference(table);
        const uint32_t cellulose = SpeciesId(table, "cellulose");
        const uint32_t leftIndex = ProbeCellIndex(LEFT_WALL[0], LEFT_WALL[1], LEFT_WALL[2]);
        Reference result;
        result.leftCelluloseInitial = AmountOf(reference.State(0)[leftIndex], cellulose);
        result.ticks.push_back(
            {.tick = 0, .hash = reference.StateHash(0), .energy = ProbeEnergySum(reference.State(0))});

        for (uint64_t tick = 0; tick < ticks; ++tick) {
            if (tick == SWAP_TICK)
                result.patchesAtSwap = reference.MagicPatches()[0];

            reference.Advance(tick, commands, tick == SWAP_TICK ? &shifted : nullptr);
            const std::span<const reaction::RxCell> state = reference.State(tick + 1);
            result.ticks.push_back({.tick = tick + 1,
                                    .hash = reference.StateHash(tick + 1),
                                    .energy = ProbeEnergySum(state),
                                    .sourceEnergy = reference.SourceEnergy(),
                                    .scheduledBlocks = reference.ScheduledBlocks(),
                                    .limits = reference.Limits(),
                                    .patchesRefused = reference.PatchesRefused()});
            if (tick + 1 == STOP_END_TICK)
                result.leftCelluloseWhileStopped = AmountOf(state[leftIndex], cellulose);
        }

        return result;
    }

    // --- GPU ---

    struct RunResult {
        bool ok = false;
        std::vector<ProbeTickHash> hashes;  // 読み戻した順(巻き戻すと同じ刻みがもう一度来る)
        std::vector<ProbeEvent> events;
        bool replayPassed = false;
    };

    // frames の列でフレームにして走らせる。コマンドは再生ファイルから ReplayPlayer が足す。rewind なら、1 刻みずつのフレームで
    // SAVE_TICK に保存点を写し、REWIND_AT_TICK まで進めてから戻して流し直す
    RunResult RunGpu(ID3D12Device5* device, const BakedReactionTable& table, const BakedReactionTable& shifted,
                     const save::ReplayFile& replay, std::span<const uint32_t> frames, bool rewind) {
        RunResult result;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"TestPatch");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table, {});
        if (!queue || !simulation || (rewind && !simulation->CreateSavePoints(device, 1))) {
            Log(Channel::Sim, Level::Error, "作れない");
            return result;
        }

        save::ReplayPlayer player(replay);
        const uint32_t unitsPerTick = simulation->UnitsPerTick();
        const ProbeTableSwap swap = {.tick = SWAP_TICK, .table = &shifted};
        uint64_t unitPosition = 0;
        bool rewound = false;
        for (size_t frame = 0; unitPosition < TOTAL_TICKS * unitsPerTick; ++frame) {
            const uint64_t firstTick = unitPosition / unitsPerTick;
            const auto firstUnit = static_cast<uint32_t>(unitPosition % unitsPerTick);
            const bool restore = rewind && !rewound && firstTick == REWIND_AT_TICK && firstUnit == 0;
            const uint64_t startTick = restore ? SAVE_TICK : firstTick;
            if (restore) {
                player.Rewind(SAVE_TICK);
                rewound = true;
                unitPosition = SAVE_TICK * unitsPerTick;
            }

            const uint64_t remaining = (TOTAL_TICKS * unitsPerTick) - unitPosition;
            const auto count = static_cast<uint32_t>(
                std::min<uint64_t>(rewind ? unitsPerTick : frames[frame % frames.size()], remaining));
            const uint64_t swapUnit = SWAP_TICK * unitsPerTick;
            const bool swapHere = unitPosition <= swapUnit && swapUnit < unitPosition + count;
            const auto slot = static_cast<uint32_t>(frame % ProbeSim::FRAME_SLOT_COUNT);
            const std::vector<ProbeCommand> commands = player.TakeCommands(
                ProbeSim::NextApplyTick(startTick, firstUnit),
                std::min(simulation->FreeCommandSlots(), PROBE_MAX_COMMANDS));
            const ProbeFrameInput input{
                .firstTick = startTick,
                .firstUnit = restore ? 0 : firstUnit,
                .unitCount = count,
                .commands = commands,
                .saveTo = rewind && !rewound && startTick == SAVE_TICK && firstUnit == 0 ? 0 : NO_SAVE_POINT,
                .restoreFrom = restore ? 0 : NO_SAVE_POINT,
                .tableSwaps = swapHere ? std::span(&swap, 1) : std::span<const ProbeTableSwap>()};
            ID3D12CommandList* list = simulation->RecordFrame(slot, input);
            if (list == nullptr || !queue->WaitCpu(queue->Submit(list)))
                return result;

            const ProbeFrameReadback readback = simulation->ReadFrame(slot);
            for (const ProbeTickHash& hash : readback.hashes)
                player.CheckHash(hash.tick, hash.WorldHash());

            result.hashes.insert(result.hashes.end(), readback.hashes.begin(), readback.hashes.end());
            result.events.insert(result.events.end(), readback.events.begin(), readback.events.end());
            unitPosition += count;
        }

        result.ok = !rewind || rewound;
        result.replayPassed = player.Passed();

        return result;
    }

    bool HashesMatch(std::span<const ProbeTickHash> hashes, const Reference& expected) {
        for (const ProbeTickHash& gpu : hashes) {
            const ProbeTickHash& cpu = expected.ticks[gpu.tick];
            if (gpu.hash != cpu.hash || gpu.energy != cpu.energy || gpu.sourceEnergy != cpu.sourceEnergy ||
                gpu.scheduledBlocks != cpu.scheduledBlocks || gpu.limits != cpu.limits ||
                gpu.patchesRefused != cpu.patchesRefused) {
                Log(Channel::Sim, Level::Error,
                    "  S({}) = {:016x} エネルギー {} 湧き出し {} ブロック {} 断った {}(CPU {:016x} エネルギー {} "
                    "湧き出し {} "
                    "ブロック {} 断った {})",
                    gpu.tick, gpu.hash, gpu.energy, gpu.sourceEnergy, gpu.scheduledBlocks, gpu.patchesRefused, cpu.hash,
                    cpu.energy, cpu.sourceEnergy, cpu.scheduledBlocks, cpu.patchesRefused);

                return false;
            }
        }

        return !hashes.empty() && hashes.back().tick == TOTAL_TICKS;
    }

    // 刻みの途中で切れる分け方
    constexpr std::array<uint32_t, 7> MIXED_FRAMES = {1, 2, 5, 4, 7, 11, 3};

    void TestScene(ID3D12Device5* device, const BakedReactionTable& table, const BakedReactionTable& shifted,
                   Failures& failures) {
        const std::vector<ProbeCommand> commands = MakeCommands(table, shifted, true);
        const Reference expected = RunReference(table, shifted, commands, TOTAL_TICKS);
        Log(Channel::Sim, Level::Info, "パッチの場面: CPU S({}) = {:016x}", TOTAL_TICKS, expected.ticks.back().hash);

        // --- CPU: 効き目と断った数 ---
        const std::vector<ProbeCommand> plainCommands = MakeCommands(table, shifted, false);
        const Reference plain = RunReference(table, shifted, plainCommands, STOP_END_TICK);
        failures.Check(expected.leftCelluloseWhileStopped == expected.leftCelluloseInitial &&
                           plain.leftCelluloseWhileStopped < plain.leftCelluloseInitial,
                       std::format("止めている間は左の壁のセルロースが減らない(止めた {} / 初め {}・止めない {})",
                                   expected.leftCelluloseWhileStopped, expected.leftCelluloseInitial,
                                   plain.leftCelluloseWhileStopped));
        failures.Check(expected.ticks[STOP_END_TICK].hash != plain.ticks[STOP_END_TICK].hash,
                       "パッチのある世界と無い世界は違う");
        failures.Check(expected.ticks[3].patchesRefused == 1 && expected.ticks[6].patchesRefused == 1,
                       "表に無い規則(刻み 2)と一杯の列(刻み 5)を 1 つずつ断る");
        failures.Check(expected.patchesAtSwap == 1,
                       "差し替えの刻みに残っているパッチは差し替えを跨ぐ速める 1 つだけ(ほかは終わって除いた)");

        // --- 再生ファイル(CPU の要約を記録として持つ)---
        save::ReplayFile recording;
        recording.commands = commands;
        for (const ProbeTickHash& tick : expected.ticks) {
            if (tick.tick > 0)
                recording.tickHashes.push_back({.tick = tick.tick, .hash = tick.WorldHash()});
        }

        const auto bytes = save::SerializeReplay(recording);
        const auto parsed = bytes ? save::ParseReplay(*bytes)
                                  : std::expected<save::ReplayFile, std::string>(std::unexpect, bytes.error());
        failures.Check(parsed.has_value() && parsed->commands == commands,
                       "再生ファイルの往復でパッチのコマンドが変わらない");
        if (!parsed) {
            Log(Channel::Sim, Level::Error, "再生ファイル: {}", parsed.error());
            return;
        }

        // --- GPU ---
        const std::array<uint32_t, 1> perTick = {PROBE_FIXED_UNITS_PER_TICK};
        for (const auto& [name, frames, rewind] :
             {std::tuple{"1 刻みずつ + 保存点へ戻す", std::span<const uint32_t>(perTick), true},
              std::tuple{"ばらばら", std::span<const uint32_t>(MIXED_FRAMES), false}}) {
            const RunResult result = RunGpu(device, table, shifted, *parsed, frames, rewind);
            Log(Channel::Sim, Level::Info, "パッチの場面 GPU({}): S({}) = {:016x}", name,
                result.hashes.empty() ? 0 : result.hashes.back().tick,
                result.hashes.empty() ? 0 : result.hashes.back().hash);

            failures.Check(
                result.ok && HashesMatch(result.hashes, expected),
                std::format("{}: 刻みごとの要約・エネルギー・湧き出し・ブロック・断った数が CPU と一致", name));
            failures.Check(result.replayPassed, std::format("{}: 再生ファイルの要約と一致", name));
            if (rewind)
                continue;

            const auto patchEvents = rng::count_if(
                result.events, [](const ProbeEvent& event) { return event.type == PROBE_EVENT_PATCH_APPLIED; });
            const auto refusedEvents = rng::count_if(result.events, [](const ProbeEvent& event) {
                return event.type == PROBE_EVENT_PATCH_APPLIED && event.place == PROBE_PATCH_REFUSED;
            });
            failures.Check(patchEvents == PatchCommandCount(commands) && refusedEvents == 2,
                           std::format("{}: パッチのイベントはコマンドの数だけ(断った 2 つを含む)", name));
        }
    }

}  // namespace

int main(int argc, char** argv) {
    const auto options = test::ParseGpuTestOptions(std::span(argv, static_cast<size_t>(argc)));
    if (!options) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_probe_patch_test [--warp]");
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    const auto table = BakeReactionTable(MakeCombustionTestTable());
    const auto shifted = MakeShiftedTable();
    if (!table || !shifted) {
        Log(Channel::Sim, Level::Error, "試験の反応の表をベイクできない");
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    Failures failures;
    TestCommandShape(*table, failures);
    TestScene(device->Get(), *table, *shifted, failures);

    const bool passesValidation = test::PassesValidation(*device, "gpu_probe_patch_test");
    const bool passed = failures.count == 0 && passesValidation;
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_patch_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
