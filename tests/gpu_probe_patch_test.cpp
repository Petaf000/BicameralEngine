// gpu_probe_patch_test.cpp — 仮の世界(ProbeSim)に魔法のパッチを置くコマンド(PROBE_COMMAND_TYPE_PATCH。T-0323・ADR-0095)が、
// GPU と CPU リファレンスで毎刻みビット一致し、再生ファイル・保存点・表の差し替えを跨いでも同じ結果になるかを確かめる。
// 値はすべて試験用の仮のもの(D-006)。
//
// 確かめること:
//   - (CPU)コマンドの語の往復(MakePatchCommand → DecodePatchCommand)・範囲が重なるブロック(格子の外は切る・重ならなければ無し)
//   - (CPU)木箱の壁に火を付け、セルロースの 2 つの規則を止めた所は、止めている間セルロースが減らない(止めない世界では減る)。
//     パッチのある世界と無い世界は違う
//   - (GPU)火・止める・速める・格子の外・角にはみ出す・規則が表に無い(断る)・列が一杯(断る)・表の差し替え(規則の ID がずれる)を
//     跨ぐパッチ・差し替えた後のパッチ・セルロースの燃焼の生成物を替える転換(釣り合うものは差し替えを跨いで効き、釣り合わないものは
//     断る。T-0359)を、(1) 1 刻みずつ + 保存点へ戻して流し直す、(2) 刻みの途中で切るばらばらの分け方、で流し、
//     刻みごとの状態の要約(セル + パッチの列)・エネルギー・湧き出し・計算したブロックの数・上限・断った数が CPU と一致する。
//     コマンドは再生ファイル(SerializeReplay → ParseReplay)から ReplayPlayer で足し、記録の要約とも突き合わせる
//   - (GPU)物質を足す・消す表の差し替えを跨ぐ転換(T-0377): 替える先の ID がずれる転換は新しい ID に付け替えて効き続け、替える先が
//     消えた転換は効かなくなり(数える)、差し替えの後に前の表の ID で作った消えた物質への転換は付け替えで 0 になって断られる。
//     同じ 2 つの分け方で CPU と毎刻み一致
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
#include "sim/species_remap.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli・probe_patch.hlsli の定数

namespace {

    constexpr uint64_t TOTAL_TICKS = 32;
    constexpr uint64_t STOP_END_TICK = 12;                     // 止めるパッチの終わり(刻み 0 から 12 刻み)
    constexpr uint32_t PROBE_CATALYST_JOULES_PER_MOL = 40000;  // 触媒が下げる活性化エネルギー(T-0288。仮)
    constexpr uint64_t SWAP_TICK = 16;                         // 規則の ID がずれる表に替える刻み
    constexpr uint64_t SWAP_VERSION = 0x50A7C4;
    constexpr uint64_t SAVE_TICK = 8;        // 保存点(パッチが効いている途中)
    constexpr uint64_t REWIND_AT_TICK = 15;  // ここまで進めてから SAVE_TICK へ戻す(差し替えの前)
    constexpr uint64_t UNKNOWN_RULE_KEY = 0x1234;
    constexpr uint64_t CONVERT_TICK = 9;  // 転換(T-0359)を置く刻み(差し替えを跨いで CONVERT_END_TICK まで)
    constexpr uint64_t CONVERT_END_TICK = 21;
    constexpr uint64_t REMAP_LATE_TICK = 20;  // 差し替えの後に、前の表の ID で消えた物質への転換を出す刻み(T-0377)

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

    uint32_t RuleIdOf(const BakedReactionTable& table, std::string_view name) {
        return static_cast<uint32_t>(std::distance(table.ruleNames.begin(), rng::find(table.ruleNames, name)));
    }

    // 規則の生成物のうち物質 species の欄(無ければ RX_MAX_PRODUCTS)
    uint32_t ProductSlotOf(const reaction::RxRule& rule, uint32_t species) {
        uint32_t slot = 0;
        while (slot < rule.productCount && rule.products[slot] != species)
            ++slot;

        return slot < rule.productCount ? slot : reaction::RX_MAX_PRODUCTS;
    }

    uint64_t AmountOf(const reaction::RxCell& cell, uint32_t species) {
        const uint32_t slot = reaction::RxFindSlot(cell, species);
        return slot == reaction::RX_NO_SLOT ? 0 : cell.amounts[slot];
    }

    // 規則の ID がずれる表: 名前の順で前にある規則(boudouard)を抜く(gpu_lab_box_test の MakeShiftedTable と同じ。物質は同じ)。
    // どちらの表も転換の試験の仮の物質を足した表(T-0359)
    std::expected<BakedReactionTable, std::string> MakeShiftedTable() {
        ReactionTableDefinition definition = MakeConversionTestTable();
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

        // 刻み 5: 小さいパッチを 12 個(列は 5 + 12 = 17 → 最後の 1 個は一杯で断る)。3 刻みで終わる。
        // 最初の 1 個は右の壁(燃えている)のセルロースの燃焼の活性化エネルギーを下げる触媒(T-0288。炭の 2^10 倍と重なる)
        patch(5, "cellulose_combustion", reaction::MpMakeCatalystEffect(PROBE_CATALYST_JOULES_PER_MOL), 3,
              {.low = {34, 30, 30}, .high = {37, 34, 34}});
        for (int32_t i = 1; i < 12; ++i) {
            const int32_t x = 4 + (i * 4);
            patch(5, "carbon_partial_combustion", reaction::MpMakeScaleEffect(1), 3,
                  {.low = {x, 50, 10}, .high = {x + 1, 51, 11}});
        }

        // 刻み 9: 右の壁のセルロースの燃焼を触媒で燃やし(刻み 5 と同じ)、その CO2 を同じ元素の仮の物質に替える転換(T-0359。どちらも
        // 差し替えを跨ぐ)と、CO に替えようとする転換(元素が釣り合わないので断る)
        const uint32_t dioxide = SpeciesId(table, "carbon_dioxide");
        const reaction::RxRule& combustion = table.rules[RuleIdOf(table, "cellulose_combustion")];
        const ProbePatchRange right = {.low = {34, 30, 30}, .high = {37, 34, 34}};
        patch(CONVERT_TICK, "cellulose_combustion", reaction::MpMakeCatalystEffect(PROBE_CATALYST_JOULES_PER_MOL),
              CONVERT_END_TICK - CONVERT_TICK, right);
        patch(CONVERT_TICK, "cellulose_combustion",
              reaction::MpMakeConvertEffect(ProductSlotOf(combustion, dioxide),
                                            SpeciesId(table, "carbon_dioxide_isomer_test")),
              CONVERT_END_TICK - CONVERT_TICK, right);
        patch(CONVERT_TICK, "cellulose_combustion",
              reaction::MpMakeConvertEffect(ProductSlotOf(combustion, dioxide), SpeciesId(table, "carbon_monoxide")), 5,
              right);

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
        uint64_t isomerAtEnd = 0;    // 最後の刻みの世界の、転換で替えた仮の物質の量(T-0359)
        uint64_t dioxideAtEnd = 0;   // 同じ、CO2 の量

        // --- 物質の付け替えを跨ぐ転換(T-0377)---
        uint64_t isomerAtSwap = 0;               // S(SWAP_TICK) の異性体の量(前の表の ID)
        uint64_t lateAtSwap = 0;                 // 同じ、消える物質(carbon_dioxide_late_test)の量
        uint32_t conversionsDisabled = 0;        // 差し替えで効かなくなった転換の数
        std::vector<uint32_t> patchesAfterSwap;  // 差し替えの刻みを進めた後のパッチの列
    };

    // 全部のセルの物質 species の量
    uint64_t TotalOf(std::span<const reaction::RxCell> state, uint32_t species) {
        uint64_t total = 0;
        for (const reaction::RxCell& cell : state)
            total += AmountOf(cell, species);

        return total;
    }

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
            if (tick == SWAP_TICK) {
                result.patchesAtSwap = reference.MagicPatches()[0];
                result.isomerAtSwap = TotalOf(reference.State(tick), SpeciesId(table, "carbon_dioxide_isomer_test"));
                result.lateAtSwap = TotalOf(reference.State(tick), SpeciesId(table, "carbon_dioxide_late_test"));
            }

            reference.Advance(tick, commands, tick == SWAP_TICK ? &shifted : nullptr);
            if (tick == SWAP_TICK) {
                result.conversionsDisabled = reference.LastRemapReport().conversionsDisabled;
                result.patchesAfterSwap.assign(reference.MagicPatches().begin(), reference.MagicPatches().end());
            }

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

            if (tick + 1 == ticks) {
                const BakedReactionTable& now = tick >= SWAP_TICK ? shifted : table;  // 差し替えの後は新しい表の ID
                result.isomerAtEnd = TotalOf(state, SpeciesId(now, "carbon_dioxide_isomer_test"));
                result.dioxideAtEnd = TotalOf(state, SpeciesId(now, "carbon_dioxide"));
            }
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

    // 再生ファイルを作り、GPU を 2 つの分け方で流して、CPU の要約と毎刻み突き合わせる。refusedCount = 断るパッチのコマンドの数
    void CheckGpuRuns(ID3D12Device5* device, const BakedReactionTable& table, const BakedReactionTable& shifted,
                      std::span<const ProbeCommand> commands, const Reference& expected, int64_t refusedCount,
                      Failures& failures) {
        // --- 再生ファイル(CPU の要約を記録として持つ)---
        save::ReplayFile recording;
        recording.commands.assign(commands.begin(), commands.end());
        for (const ProbeTickHash& tick : expected.ticks) {
            if (tick.tick > 0)
                recording.tickHashes.push_back({.tick = tick.tick, .hash = tick.WorldHash()});
        }

        const auto bytes = save::SerializeReplay(recording);
        const auto parsed = bytes ? save::ParseReplay(*bytes)
                                  : std::expected<save::ReplayFile, std::string>(std::unexpect, bytes.error());
        failures.Check(parsed.has_value() && rng::equal(parsed->commands, commands),
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
            failures.Check(patchEvents == PatchCommandCount(commands) && refusedEvents == refusedCount,
                           std::format("{}: パッチのイベントはコマンドの数だけ(断ったものを含む)", name));
        }
    }

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
        failures.Check(expected.ticks[3].patchesRefused == 1 && expected.ticks[6].patchesRefused == 1 &&
                           expected.ticks[CONVERT_TICK + 1].patchesRefused == 1,
                       "表に無い規則(刻み 2)と一杯の列(刻み 5)と釣り合わない転換(刻み 9)を 1 つずつ断る");
        failures.Check(
            expected.patchesAtSwap == 3,
            "差し替えの刻みに残っているパッチは差し替えを跨ぐ速める・触媒・転換の 3 つだけ(ほかは終わって除いた)");
        const Reference plainToEnd = RunReference(table, shifted, plainCommands, TOTAL_TICKS);
        failures.Check(
            expected.isomerAtEnd > 0 && plainToEnd.isomerAtEnd == 0,
            std::format("転換で替えた物質ができる(転換あり {}・なし {} µmol。CO2 は {}・{} µmol)", expected.isomerAtEnd,
                        plainToEnd.isomerAtEnd, expected.dioxideAtEnd, plainToEnd.dioxideAtEnd));

        CheckGpuRuns(device, table, shifted, commands, expected, 3, failures);
    }

    // --- 物質を足す・消す差し替えを跨ぐ転換(T-0377)---

    // 両方の壁に火を付け、セルロースの燃焼の CO2 を、右の壁は異性体(差し替えで ID がずれる)、左の壁は消える物質に替える
    // (どちらも最後まで)。刻み SWAP_TICK に物質の一覧が変わる表へ替え、刻み REMAP_LATE_TICK に前の表の ID で消える物質への転換を
    // もう 1 つ出す(フレームの付け替え RemapPlaceCommand を通す = 窓の筆と同じ)
    std::vector<ProbeCommand> MakeRemapCommands(const BakedReactionTable& before, const BakedReactionTable& after) {
        uint32_t sequence = 0;
        std::vector<ProbeCommand> commands;
        commands.push_back(MakePokeCommand(0, sequence++, LEFT_WALL[0], LEFT_WALL[1], LEFT_WALL[2]));
        commands.push_back(MakePokeCommand(0, sequence++, RIGHT_WALL[0], RIGHT_WALL[1], RIGHT_WALL[2]));

        const reaction::RxRule& combustion = before.rules[RuleIdOf(before, "cellulose_combustion")];
        const uint32_t slot = ProductSlotOf(combustion, SpeciesId(before, "carbon_dioxide"));
        const uint64_t key = RuleKey(before, "cellulose_combustion");
        const uint32_t late = SpeciesId(before, "carbon_dioxide_late_test");
        const ProbePatchRange left = {.low = {26, 30, 30}, .high = {29, 34, 34}};
        const ProbePatchRange right = {.low = {34, 30, 30}, .high = {37, 34, 34}};
        commands.push_back(MakePatchCommand(
            0, sequence++, key, reaction::MpMakeConvertEffect(slot, SpeciesId(before, "carbon_dioxide_isomer_test")),
            TOTAL_TICKS, right));
        commands.push_back(
            MakePatchCommand(0, sequence++, key, reaction::MpMakeConvertEffect(slot, late), TOTAL_TICKS, left));
        commands.push_back(MakeTableCommand(SWAP_TICK, sequence++, SWAP_VERSION));

        const auto remap = BuildSpeciesRemap(before, after);
        FX_ASSERT(remap.has_value());
        commands.push_back(RemapPlaceCommand(
            MakePatchCommand(REMAP_LATE_TICK, sequence++, key, reaction::MpMakeConvertEffect(slot, late), 4, left),
            *remap));
        rng::sort(commands, CommandPrecedes);

        return commands;
    }

    void TestRemapScene(ID3D12Device5* device, const BakedReactionTable& before, const BakedReactionTable& after,
                        Failures& failures) {
        const std::vector<ProbeCommand> commands = MakeRemapCommands(before, after);
        const Reference expected = RunReference(before, after, commands, TOTAL_TICKS);
        Log(Channel::Sim, Level::Info,
            "付け替えを跨ぐ転換: CPU S({}) = {:016x}・異性体 {} → {} µmol・消える物質 {} µmol・効かなくなった転換 {}",
            TOTAL_TICKS, expected.ticks.back().hash, expected.isomerAtSwap, expected.isomerAtEnd, expected.lateAtSwap,
            expected.conversionsDisabled);

        // --- CPU: 付け替えた列・効き目・断った数 ---
        const reaction::RxRule& combustion = after.rules[RuleIdOf(after, "cellulose_combustion")];
        const uint32_t slot = ProductSlotOf(combustion, SpeciesId(after, "carbon_dioxide"));
        const auto effectAfterSwap = [&](uint32_t index) {
            const size_t word = reaction::MP_LIST_HEADER_WORDS + (index * reaction::MP_PATCH_WORDS) +
                                reaction::MP_WORD_EFFECT;
            return word < expected.patchesAfterSwap.size() ? expected.patchesAfterSwap[word] : 0u;
        };
        failures.Check(expected.conversionsDisabled == 1 && !expected.patchesAfterSwap.empty() &&
                           expected.patchesAfterSwap[0] == 2 &&
                           effectAfterSwap(0) ==
                               reaction::MpMakeConvertEffect(slot, SpeciesId(after, "carbon_dioxide_isomer_test")) &&
                           effectAfterSwap(1) == reaction::MpMakeConvertEffect(slot, 0),
                       "差し替えで異性体への転換は新しい ID に・消える物質への転換は替える先 0 に(1 つ数える)");
        failures.Check(
            expected.lateAtSwap > 0 && expected.isomerAtSwap > 0 && expected.isomerAtEnd > expected.isomerAtSwap,
            "差し替えの前は両方の転換が効き、後も異性体への転換は効き続ける");
        failures.Check(expected.ticks[REMAP_LATE_TICK + 1].patchesRefused == 1,
                       "差し替えの後に前の表の ID で出した消えた物質への転換は断る");

        CheckGpuRuns(device, before, after, commands, expected, 1, failures);
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

    const auto table = BakeReactionTable(MakeConversionTestTable());
    const auto shifted = MakeShiftedTable();
    if (!table || !shifted) {
        Log(Channel::Sim, Level::Error, "試験の反応の表をベイクできない");
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    const auto remapBefore = BakeReactionTable(MakeConversionRemapBeforeTestTable());
    const auto remapAfter = BakeReactionTable(MakeConversionRemapAfterTestTable());
    if (!remapBefore || !remapAfter) {
        Log(Channel::Sim, Level::Error, "付け替えの試験の反応の表をベイクできない");
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    Failures failures;
    TestCommandShape(*table, failures);
    TestScene(device->Get(), *table, *shifted, failures);
    TestRemapScene(device->Get(), *remapBefore, *remapAfter, failures);

    const bool passesValidation = test::PassesValidation(*device, "gpu_probe_patch_test");
    const bool passed = failures.count == 0 && passesValidation;
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_patch_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
