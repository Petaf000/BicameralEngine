// gpu_probe_place_test.cpp — 世界に物を置く筆のコマンド(PROBE_COMMAND_TYPE_PLACE。T-0222・D-449)が、仮の世界の GPU と
// CPU リファレンスで毎刻みビット一致するかを確かめる。
//
// 確かめること:
//   - (CPU)1 セルに当てる関数(common/probe_place.hlsli): 置き換えは sim::MakeReactionCell と同じセル・足すは量の和と
//     持ち込むエネルギーの和・球の形(半径 1 = 7 セル・2 = 33 セル)・当てられない値(格子の外・半径・物質・量・温度・置き方)は飛ばす
//   - (CPU)置く刻みのエネルギーの合計は、湧き出しの欄に数えた分だけ変わる(伝導と反応では変わらない)
//   - (GPU)木の球を置いて同じ刻みに火を付ける・木炭を足す・格子からはみ出す・大きく足す・当てられない値・木箱の壁を窒素に置き換える、
//     を 1 刻みずつ / 刻みの途中で切るばらばらの分け方で流し、刻みごとのハッシュ・エネルギー・湧き出し・計算したブロックの数が
//     CPU と一致する。イベントは当たった置くコマンドとつつきの分だけ
//   - debug layer のエラーが 0 件
// 引数: gpu_test_options.h(--warp)。キューは compute だけ。
#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu/queue.h"
#include "gpu_test_options.h"
#include "save/replay_session.h"
#include "sim/lab_box.h"
#include "sim/probe_sim.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli・probe_place.hlsli の定数

namespace {

    constexpr uint64_t TOTAL_TICKS = 30;
    constexpr uint32_t TEMPERATURE_MILLIKELVIN = 300000;

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition)
                return;

            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    const LabMaterial& Material(const std::vector<LabMaterial>& materials, std::string_view name) {
        return *rng::find(materials, name, &LabMaterial::name);
    }

    ProbePlace Decode(const ProbeCommand& command, const BakedReactionTable& table) {
        ProbePlacePayload payload{};
        rng::copy(command.payload, std::begin(payload.words));

        return ProbeDecodePlace(payload, static_cast<uint32_t>(table.species.size()));
    }

    // --- CPU: 1 セルに当てる関数 ---

    uint32_t CoveredCells(const ProbePlace& place) {
        uint32_t covered = 0;
        for (uint32_t z = ProbePlaceBegin(place.z, place.radius); z < ProbePlaceEnd(place.z, place.radius); ++z) {
            for (uint32_t y = ProbePlaceBegin(place.y, place.radius); y < ProbePlaceEnd(place.y, place.radius); ++y) {
                for (uint32_t x = ProbePlaceBegin(place.x, place.radius); x < ProbePlaceEnd(place.x, place.radius); ++x)
                    covered += ProbePlaceCovers(place, x, y, z) ? 1 : 0;
            }
        }

        return covered;
    }

    void TestCellRules(const BakedReactionTable& table, Failures& failures) {
        const std::vector<LabMaterial> materials = MakeLabMaterials(table);
        const LabMaterial& wood = Material(materials, "木");
        const LabMaterial& charcoal = Material(materials, "木炭");
        const ReactionTableView view = table.View();

        // --- 置き換え = MakeReactionCell ---
        const ProbeCommand replace = MakePlaceCommand(0, 0, {.x = 5, .y = 6, .z = 7, .radius = 2}, wood.contents);
        const ProbePlace replacePlace = Decode(replace, table);
        const reaction::RxCell air = MakeReactionCell(table, Material(materials, "空気").contents,
                                                      TEMPERATURE_MILLIKELVIN);
        const ProbePlaced replaced = ProbePlaceCell(view, air, replacePlace);
        failures.Check(replacePlace.valid == 1 && replaced.applied == 1 &&
                           HashReactionCell(replaced.cell) ==
                               HashReactionCell(MakeReactionCell(table, wood.contents, TEMPERATURE_MILLIKELVIN)),
                       "置き換え: 中身が MakeReactionCell(木, 300 K)と同じ");
        failures.Check(CoveredCells(replacePlace) == 33, "球の形: 半径 2 は 33 セル");

        // --- 足す: 量の和・持ち込むエネルギーの和 ---
        const ProbeCommand add = MakePlaceCommand(
            0, 0, {.x = 5, .y = 6, .z = 7, .radius = 1, .replace = false, .temperatureMilliKelvin = 900000},
            charcoal.contents);
        const ProbePlace addPlace = Decode(add, table);
        const ProbePlaced added = ProbePlaceCell(view, replaced.cell, addPlace);
        const reaction::RxCell charcoalHot = MakeReactionCell(table, charcoal.contents, 900000);
        bool amountsAdded = added.applied == 1;
        for (const SpeciesAmount& entry : charcoal.contents) {
            const uint32_t before = reaction::RxFindSlot(replaced.cell, entry.species);
            const uint32_t after = reaction::RxFindSlot(added.cell, entry.species);
            const uint64_t old = before == reaction::RX_NO_SLOT ? 0 : replaced.cell.amounts[before];
            amountsAdded = amountsAdded && after != reaction::RX_NO_SLOT &&
                           added.cell.amounts[after] == old + entry.amount;
        }

        failures.Check(amountsAdded && added.cell.energy == replaced.cell.energy + charcoalHot.energy,
                       "足す: 量は和・エネルギーは足した材料の分(900 K の木炭)だけ増える");
        failures.Check(CoveredCells(addPlace) == 7, "球の形: 半径 1 は 7 セル");

        // --- 格子の端で切る ---
        const ProbePlace corner = Decode(MakePlaceCommand(0, 0, {.x = 0, .y = 0, .z = 0, .radius = 1}, wood.contents),
                                         table);
        failures.Check(corner.valid == 1 && CoveredCells(corner) == 4, "格子の角: 半径 1 は格子の中の 4 セル");

        // --- 当てられない値 ---
        const std::vector<SpeciesAmount> unknown = {
            {.species = static_cast<uint32_t>(table.species.size()), .amount = 1}};
        const std::vector<SpeciesAmount> twice = {{.species = 1, .amount = 1}, {.species = 1, .amount = 2}};
        const std::vector<SpeciesAmount> tooMuch = {{.species = 1, .amount = PROBE_PLACE_MAX_AMOUNT + 1}};
        const std::vector<SpeciesAmount> four = {{.species = 1, .amount = 1},
                                                 {.species = 2, .amount = 1},
                                                 {.species = 3, .amount = 1},
                                                 {.species = 4, .amount = 1}};
        const std::array<ProbeCommand, 8> rejected = {
            MakePlaceCommand(0, 0, {.x = PROBE_GRID_SIZE, .y = 0, .z = 0}, wood.contents),
            MakePlaceCommand(0, 0, {.radius = PROBE_PLACE_MAX_RADIUS + 1}, wood.contents),
            MakePlaceCommand(0, 0, {.temperatureMilliKelvin = PROBE_PLACE_MAX_TEMPERATURE_MILLIKELVIN + 1},
                             wood.contents),
            MakePlaceCommand(0, 0, {}, unknown),
            MakePlaceCommand(0, 0, {}, twice),
            MakePlaceCommand(0, 0, {}, tooMuch),
            MakePlaceCommand(0, 0, {}, four),
            MakePlaceCommand(0, 0, {}, {}),
        };

        failures.Check(
            rng::none_of(rejected, [&](const ProbeCommand& command) { return Decode(command, table).valid; }),
            "当てられない値(格子の外・半径・温度・表に無い物質・重なる物質・量・物質の数 4・0)は飛ばす");
    }

    // --- 場面 ---

    std::vector<ProbeCommand> MakeCommands(const BakedReactionTable& table) {
        const std::vector<LabMaterial> materials = MakeLabMaterials(table);
        const auto& wood = Material(materials, "木").contents;
        uint32_t sequence = 0;
        std::vector<ProbeCommand> commands;
        const auto place = [&](uint64_t tick, ProbePlaceShape shape, std::span<const SpeciesAmount> contents) {
            commands.push_back(MakePlaceCommand(tick, sequence++, shape, contents));
        };

        // 木箱の左の空気に木の球を置き、同じ刻みに(後の番号で)火を付ける
        place(0, {.x = 20, .y = 32, .z = 32, .radius = 3}, wood);
        commands.push_back(MakePokeCommand(0, sequence++, 20, 32, 32));
        place(2, {.x = 20, .y = 32, .z = 32, .radius = 1, .replace = false}, Material(materials, "木炭").contents);
        place(3, {.x = 0, .y = 0, .z = 0, .temperatureMilliKelvin = 900000},
              Material(materials, "二酸化炭素").contents);
        place(3, {.x = 63, .y = 63, .z = 40, .radius = PROBE_PLACE_MAX_RADIUS, .replace = false},
              Material(materials, "空気").contents);
        place(5, {.x = 40, .y = 10, .z = 10, .radius = PROBE_PLACE_MAX_RADIUS + 1}, wood);  // 当たらない
        place(7, {.x = 28, .y = 32, .z = 32, .radius = 2}, Material(materials, "窒素").contents);
        place(9, {.x = 32, .y = 32, .z = 32, .radius = PROBE_PLACE_MAX_RADIUS, .replace = false}, wood);
        commands.push_back(MakePokeCommand(11, sequence++, 30, 32, 32));

        return commands;
    }

    // GPU が返すはずのイベント: 当たる置くコマンドとつつきを (刻み, 種類, 場所) の順に
    std::vector<ProbeEvent> ExpectedEvents(const BakedReactionTable& table, std::span<const ProbeCommand> commands) {
        std::vector<ProbeEvent> events;
        for (const ProbeCommand& command : commands) {
            if (command.type == PROBE_COMMAND_TYPE_POKE) {
                events.push_back({.tick = command.targetTick,
                                  .type = PROBE_EVENT_POKE_APPLIED,
                                  .place = ProbePokePlace(command.payload[0], command.payload[1], command.payload[2])});
            }

            const ProbePlace place = Decode(command, table);
            if (command.type == PROBE_COMMAND_TYPE_PLACE && place.valid == 1) {
                events.push_back({.tick = command.targetTick,
                                  .type = PROBE_EVENT_PLACE_APPLIED,
                                  .place = ProbePokePlace(place.x, place.y, place.z)});
            }
        }

        rng::sort(events, [](const ProbeEvent& a, const ProbeEvent& b) {
            if (a.tick != b.tick)
                return a.tick < b.tick;
            return ProbeEventKey(a.type, a.place) < ProbeEventKey(b.type, b.place);
        });

        return events;
    }

    // --- CPU リファレンス ---

    struct Reference {
        std::vector<ProbeTickHash> ticks;  // [t] が S(t)
        bool energyBalanced = true;        // エネルギーの合計が湧き出しの欄の分だけ変わった
        bool placedChangedEnergy = false;  // 置く刻みに湧き出しが 0 でなかった
    };

    Reference RunReference(const BakedReactionTable& table, std::span<const ProbeCommand> commands) {
        ProbeReference reference(table);
        Reference result;
        result.ticks.push_back(
            {.tick = 0, .hash = ProbeStateHash(reference.State(0)), .energy = ProbeEnergySum(reference.State(0))});

        for (uint64_t tick = 0; tick < TOTAL_TICKS; ++tick) {
            reference.Advance(tick, commands);
            const std::span<const reaction::RxCell> state = reference.State(tick + 1);
            result.ticks.push_back({.tick = tick + 1,
                                    .hash = ProbeStateHash(state),
                                    .energy = ProbeEnergySum(state),
                                    .sourceEnergy = reference.SourceEnergy(),
                                    .scheduledBlocks = reference.ScheduledBlocks()});

            result.energyBalanced = result.energyBalanced &&
                                    result.ticks.back().energy == result.ticks[tick].energy + reference.SourceEnergy();
            if (tick == 3 && reference.SourceEnergy() != 0)
                result.placedChangedEnergy = true;
        }

        return result;
    }

    // --- GPU ---

    struct RunResult {
        bool ok = false;
        std::vector<ProbeTickHash> hashes;
        std::vector<ProbeEvent> events;
        uint32_t droppedEventCount = 0;
    };

    // unitsPerFrame の列でフレームにして走らせる(コマンドは再生と同じく ReplayPlayer が数刻み先まで先に足す)
    RunResult RunGpu(ID3D12Device5* device, const BakedReactionTable& table, std::span<const ProbeCommand> commands,
                     std::span<const uint32_t> unitsPerFrame) {
        RunResult result;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"TestPlace");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table, {});
        if (!queue || !simulation) {
            Log(Channel::Sim, Level::Error, "作れない: {}{}", queue ? "" : queue.error(),
                simulation ? "" : simulation.error());
            return result;
        }

        save::ReplayFile replay;
        replay.commands.assign(commands.begin(), commands.end());
        save::ReplayPlayer player(std::move(replay));

        const uint32_t unitsPerTick = simulation->UnitsPerTick();
        uint64_t unitPosition = 0;
        for (size_t frame = 0; frame < unitsPerFrame.size(); ++frame) {
            const uint64_t firstTick = unitPosition / unitsPerTick;
            const auto firstUnit = static_cast<uint32_t>(unitPosition % unitsPerTick);
            const auto slot = static_cast<uint32_t>(frame % ProbeSim::FRAME_SLOT_COUNT);
            const std::vector<ProbeCommand> frameCommands = player.TakeCommands(
                ProbeSim::NextApplyTick(firstTick, firstUnit),
                std::min(simulation->FreeCommandSlots(), PROBE_MAX_COMMANDS));

            ID3D12CommandList* list = simulation->RecordFrame(slot, {.firstTick = firstTick,
                                                                     .firstUnit = firstUnit,
                                                                     .unitCount = unitsPerFrame[frame],
                                                                     .extract = false,
                                                                     .commands = frameCommands});
            if (list == nullptr || !queue->WaitCpu(queue->Submit(list)))
                return result;

            const ProbeFrameReadback readback = simulation->ReadFrame(slot);
            result.hashes.insert(result.hashes.end(), readback.hashes.begin(), readback.hashes.end());
            result.events.insert(result.events.end(), readback.events.begin(), readback.events.end());
            result.droppedEventCount += readback.droppedEventCount;
            unitPosition += unitsPerFrame[frame];
        }

        result.ok = unitPosition == TOTAL_TICKS * unitsPerTick;

        return result;
    }

    bool HashesMatch(std::span<const ProbeTickHash> hashes, const Reference& expected) {
        if (hashes.size() != TOTAL_TICKS)
            return false;

        for (size_t index = 0; index < hashes.size(); ++index) {
            const ProbeTickHash& gpu = hashes[index];
            const ProbeTickHash& cpu = expected.ticks[index + 1];
            if (gpu.tick != cpu.tick || gpu.hash != cpu.hash || gpu.energy != cpu.energy ||
                gpu.sourceEnergy != cpu.sourceEnergy || gpu.scheduledBlocks != cpu.scheduledBlocks) {
                Log(Channel::Sim, Level::Error,
                    "  S({}) = {:016x} エネルギー {} 湧き出し {} ブロック {}(CPU {:016x} エネルギー {} 湧き出し {} "
                    "ブロック {})",
                    gpu.tick, gpu.hash, gpu.energy, gpu.sourceEnergy, gpu.scheduledBlocks, cpu.hash, cpu.energy,
                    cpu.sourceEnergy, cpu.scheduledBlocks);

                return false;
            }
        }

        return true;
    }

    // 刻みの途中で切れる分け方
    std::vector<uint32_t> MixedFrames(uint32_t total) {
        constexpr std::array<uint32_t, 7> PATTERN = {1, 2, 5, 4, 7, 11, 3};
        std::vector<uint32_t> sizes;
        uint32_t sum = 0;
        for (size_t index = 0; sum < total; ++index) {
            const uint32_t size = std::min(PATTERN[index % PATTERN.size()], total - sum);
            sizes.push_back(size);
            sum += size;
        }

        return sizes;
    }

    void TestScene(ID3D12Device5* device, const BakedReactionTable& table, Failures& failures) {
        const std::vector<ProbeCommand> commands = MakeCommands(table);
        const Reference expected = RunReference(table, commands);
        Log(Channel::Sim, Level::Info, "置く場面: CPU S({}) = {:016x}", TOTAL_TICKS, expected.ticks.back().hash);
        failures.Check(expected.energyBalanced, "エネルギーの合計は湧き出しの欄の分だけ変わる(CPU)");
        failures.Check(expected.placedChangedEnergy, "900 K の二酸化炭素を置いた刻みは湧き出しが 0 でない(CPU)");

        const auto totalUnits = static_cast<uint32_t>(TOTAL_TICKS * PROBE_FIXED_UNITS_PER_TICK);
        const std::vector<uint32_t> perTick(TOTAL_TICKS, PROBE_FIXED_UNITS_PER_TICK);
        const std::vector<uint32_t> mixed = MixedFrames(totalUnits);
        const std::vector<ProbeEvent> expectedEvents = ExpectedEvents(table, commands);
        for (const auto& [name, frames] : {std::pair{"1 刻みずつ", std::span<const uint32_t>(perTick)},
                                           std::pair{"ばらばら", std::span<const uint32_t>(mixed)}}) {
            const RunResult result = RunGpu(device, table, commands, frames);
            Log(Channel::Sim, Level::Info, "置く場面 GPU({}): S({}) = {:016x}", name,
                result.hashes.empty() ? 0 : result.hashes.back().tick,
                result.hashes.empty() ? 0 : result.hashes.back().hash);

            failures.Check(
                result.ok && HashesMatch(result.hashes, expected),
                std::format("{}: 刻みごとのハッシュ・エネルギー・湧き出し・ブロックの数が CPU と一致", name));
            failures.Check(result.events == expectedEvents && result.droppedEventCount == 0,
                           std::format("{}: イベントは当たる置くコマンドとつつきの分だけ", name));
        }
    }

}  // namespace

int main(int argc, char** argv) {
    const auto options = test::ParseGpuTestOptions(std::span(argv, static_cast<size_t>(argc)));
    if (!options) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_probe_place_test [--warp]");
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
    if (!table) {
        Log(Channel::Sim, Level::Error, "試験の反応の表をベイクできない: {}", table.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    Failures failures;
    TestCellRules(*table, failures);
    TestScene(device->Get(), *table, failures);

    const bool passesValidation = test::PassesValidation(*device, "gpu_probe_place_test");
    const bool passed = failures.count == 0 && passesValidation;
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_place_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
