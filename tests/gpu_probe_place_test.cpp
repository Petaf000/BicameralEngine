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
//   - (CPU)球以外の形(直方体・円柱。T-0245)のセルの数・格子の端・当てられない大きさ・T-0222 の記録は球のまま
//   - (GPU)直方体・円柱を置く・足す・はみ出す、ドラッグで塗る(同じ刻みに重なる球 12 個・間を空けた足す 6 個)も CPU と一致
//   - (GPU)筆の「面に沿って」の物の印(sim/probe_surface。T-0332): 抽出の後ろで作って読み戻した S(t) の印が、CPU リファレンス
//     (BuildProbeSurfaceMask)と毎回ビット一致(木箱の壁と置いた木が物になる)
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
#include "reaction_test_table.h"
#include "save/replay_session.h"
#include "sim/lab_box.h"
#include "sim/probe_sim.h"
#include "sim/probe_surface.h"

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
        for (uint32_t z = ProbePlaceBegin(place.z, place.extentZ); z < ProbePlaceEnd(place.z, place.extentZ); ++z) {
            for (uint32_t y = ProbePlaceBegin(place.y, place.extentY); y < ProbePlaceEnd(place.y, place.extentY); ++y) {
                for (uint32_t x = ProbePlaceBegin(place.x, place.extentX); x < ProbePlaceEnd(place.x, place.extentX);
                     ++x)
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

    // 球以外の形(T-0245・ADR-0089): 直方体・円柱のセルの数・格子の端・当てられない大きさ。形の欄が 0 の(T-0222 の)コマンドは球のまま
    void TestShapes(const BakedReactionTable& table, Failures& failures) {
        const std::vector<LabMaterial> materials = MakeLabMaterials(table);
        const std::vector<SpeciesAmount>& wood = Material(materials, "木").contents;
        const auto decode = [&](const ProbePlaceShape& shape) {
            return Decode(MakePlaceCommand(0, 0, shape, wood), table);
        };

        const ProbePlace box = decode(
            {.x = 10, .y = 10, .z = 10, .radius = 1, .kind = PROBE_PLACE_SHAPE_BOX, .size2 = 2, .size3 = 0});
        failures.Check(box.valid == 1 && CoveredCells(box) == 15, "直方体: 半幅 (1, 2, 0) は 3 × 5 × 1 = 15 セル");

        const ProbePlace boxCorner = decode(
            {.x = 0, .y = 63, .z = 0, .radius = 2, .kind = PROBE_PLACE_SHAPE_BOX, .size2 = 2, .size3 = 2});
        failures.Check(boxCorner.valid == 1 && CoveredCells(boxCorner) == 27, "直方体: 格子の角で 3 × 3 × 3 に切る");

        const ProbePlace upright = decode(
            {.x = 10, .y = 10, .z = 10, .radius = 2, .kind = PROBE_PLACE_SHAPE_CYLINDER, .size2 = 1, .size3 = 1});
        failures.Check(
            upright.valid == 1 && CoveredCells(upright) == 39 && upright.extentY == 1 && upright.extentX == 2,
            "円柱(y の軸): 半径 2 の円 13 セル × 高さ 3 = 39 セル");

        const ProbePlace disc = decode(
            {.x = 10, .y = 10, .z = 10, .radius = 1, .kind = PROBE_PLACE_SHAPE_CYLINDER, .size2 = 0, .size3 = 2});
        failures.Check(disc.valid == 1 && CoveredCells(disc) == 5 && disc.extentZ == 0,
                       "円柱(z の軸): 半径 1・厚さ 1 は 5 セル");

        const ProbePlace largest = decode({.x = 32,
                                           .y = 32,
                                           .z = 32,
                                           .radius = PROBE_PLACE_MAX_RADIUS,
                                           .kind = PROBE_PLACE_SHAPE_BOX,
                                           .size2 = PROBE_PLACE_MAX_RADIUS,
                                           .size3 = PROBE_PLACE_MAX_RADIUS});
        failures.Check(largest.valid == 1 && CoveredCells(largest) == 17 * 17 * 17,
                       "直方体: 1 コマンドの上限は 17³ セル");

        const std::array<ProbePlaceShape, 5> rejected = {{
            {.kind = 3},
            {.radius = 1, .kind = PROBE_PLACE_SHAPE_SPHERE, .size2 = 1},
            {.radius = 1, .kind = PROBE_PLACE_SHAPE_BOX, .size2 = PROBE_PLACE_MAX_RADIUS + 1},
            {.radius = 1, .kind = PROBE_PLACE_SHAPE_CYLINDER, .size2 = 1, .size3 = 3},
            {.radius = PROBE_PLACE_MAX_RADIUS + 1, .kind = PROBE_PLACE_SHAPE_BOX},
        }};
        failures.Check(rng::none_of(rejected, [&](const ProbePlaceShape& shape) { return decode(shape).valid; }),
                       "当てられない形(知らない形・球の余分な大きさ・半幅の上限・円柱の軸・大きさ 1 の上限)は飛ばす");

        // --- T-0222 の記録(形の欄が 0)は球のまま・格子の外の古い印(中心の語の上の 8 ビット)は当てない ---
        ProbePlacePayload legacy{};
        rng::copy(MakePlaceCommand(0, 0, {.x = 10, .y = 10, .z = 10, .radius = 2}, wood).payload,
                  std::begin(legacy.words));
        const ProbePlace legacySphere = ProbeDecodePlace(legacy, static_cast<uint32_t>(table.species.size()));
        legacy.words[PROBE_PLACE_WORD_CENTER] = 0xFF000000u;
        const ProbePlace legacyOutside = ProbeDecodePlace(legacy, static_cast<uint32_t>(table.species.size()));
        failures.Check(legacySphere.valid == 1 && legacySphere.shape == PROBE_PLACE_SHAPE_SPHERE &&
                           CoveredCells(legacySphere) == 33 && legacyOutside.valid == 0,
                       "T-0222 の形: 球のまま・格子の外の印は当てない");
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

        // 球以外の形(T-0245): 木の直方体の壁・木炭の円柱(縦)・格子の端からはみ出す空気の円柱(z の軸)を足す
        place(13, {.x = 12, .y = 40, .z = 32, .radius = 1, .kind = PROBE_PLACE_SHAPE_BOX, .size2 = 6, .size3 = 4},
              wood);
        place(13,
              {.x = 12,
               .y = 40,
               .z = 32,
               .radius = 2,
               .replace = false,
               .kind = PROBE_PLACE_SHAPE_CYLINDER,
               .size2 = 3,
               .size3 = 1},
              Material(materials, "木炭").contents);
        place(14,
              {.x = 2,
               .y = 2,
               .z = 60,
               .radius = 5,
               .replace = false,
               .kind = PROBE_PLACE_SHAPE_CYLINDER,
               .size2 = 8,
               .size3 = 2},
              Material(materials, "空気").contents);
        commands.push_back(MakePokeCommand(15, sequence++, 12, 40, 32));

        // ドラッグで塗る(T-0245): 同じ刻みに重なる球を 1 セルずつずらして 12 個(置き換え)と、重ならない間隔の足すを 6 個
        for (uint32_t i = 0; i < 12; ++i)
            place(17, {.x = 40 + i, .y = 20, .z = 32, .radius = 3}, wood);

        for (uint32_t i = 0; i < 6; ++i)
            place(18, {.x = 36 + (i * 5), .y = 24, .z = 32, .radius = 2, .replace = false},
                  Material(materials, "木炭").contents);

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
        std::vector<ProbeTickHash> ticks;             // [t] が S(t)
        bool energyBalanced = true;                   // エネルギーの合計が湧き出しの欄の分だけ変わった
        bool placedChangedEnergy = false;             // 置く刻みに湧き出しが 0 でなかった
        std::vector<std::vector<uint32_t>> surfaces;  // [t] が S(t) の物の印(T-0332)
    };

    Reference RunReference(const BakedReactionTable& table, std::span<const ProbeCommand> commands) {
        ProbeReference reference(table);
        Reference result;
        result.ticks.push_back(
            {.tick = 0, .hash = ProbeStateHash(reference.State(0)), .energy = ProbeEnergySum(reference.State(0))});
        result.surfaces.push_back(BuildProbeSurfaceMask(reference.State(0)));

        for (uint64_t tick = 0; tick < TOTAL_TICKS; ++tick) {
            reference.Advance(tick, commands);
            const std::span<const reaction::RxCell> state = reference.State(tick + 1);
            result.ticks.push_back({.tick = tick + 1,
                                    .hash = ProbeStateHash(state),
                                    .energy = ProbeEnergySum(state),
                                    .sourceEnergy = reference.SourceEnergy(),
                                    .scheduledBlocks = reference.ScheduledBlocks(),
                                    .limits = reference.Limits()});
            result.surfaces.push_back(BuildProbeSurfaceMask(state));

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
        // 抽出の後ろで読み戻した物の印(T-0332)。刻みの境界で終わったフレームの分だけ(刻みの途中で終わると、抽出の
        // S(t) にはその刻みの適用の単位が当てた置くコマンドが入っていて、CPU リファレンスの S(t) と違う)
        std::vector<ProbeSurfaceMask> surfaces;
        size_t boundaryFrames = 0;
    };

    // unitsPerFrame の列でフレームにして走らせる(コマンドは再生と同じく ReplayPlayer が数刻み先まで先に足す)
    RunResult RunGpu(ID3D12Device5* device, const BakedReactionTable& table, std::span<const ProbeCommand> commands,
                     std::span<const uint32_t> unitsPerFrame) {
        RunResult result;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"TestPlace");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table, {});
        auto surface = ProbeSurface::Create(device);
        if (!queue || !simulation || !surface) {
            Log(Channel::Sim, Level::Error, "作れない: {}{}{}", queue ? "" : queue.error(),
                simulation ? "" : simulation.error(), surface ? "" : surface.error());
            return result;
        }

        save::ReplayFile replay;
        replay.commands.assign(commands.begin(), commands.end());
        save::ReplayPlayer player(std::move(replay));

        surface->SetEnabled(true);
        const uint32_t unitsPerTick = simulation->UnitsPerTick();
        uint64_t unitPosition = 0;
        for (size_t frame = 0; frame < unitsPerFrame.size(); ++frame) {
            const uint64_t firstTick = unitPosition / unitsPerTick;
            const auto firstUnit = static_cast<uint32_t>(unitPosition % unitsPerTick);
            const auto slot = static_cast<uint32_t>(frame % ProbeSim::FRAME_SLOT_COUNT);
            const std::vector<ProbeCommand> frameCommands = player.TakeCommands(
                ProbeSim::NextApplyTick(firstTick, firstUnit),
                std::min(simulation->FreeCommandSlots(), PROBE_MAX_COMMANDS));

            ID3D12CommandList* list = simulation->RecordFrame(slot,
                                                              {.firstTick = firstTick,
                                                               .firstUnit = firstUnit,
                                                               .unitCount = unitsPerFrame[frame],
                                                               .extract = true,
                                                               .commands = frameCommands,
                                                               .afterExtract = [&](auto* simList, const auto& context) {
                                                                   surface->RecordAfterExtract(simList, context);
                                                               }});
            if (list == nullptr || !queue->WaitCpu(queue->Submit(list)))
                return result;

            const ProbeFrameReadback readback = simulation->ReadFrame(slot);
            result.hashes.insert(result.hashes.end(), readback.hashes.begin(), readback.hashes.end());
            result.events.insert(result.events.end(), readback.events.begin(), readback.events.end());
            result.droppedEventCount += readback.droppedEventCount;
            const bool boundary = (unitPosition + unitsPerFrame[frame]) % unitsPerTick == 0;
            auto mask = surface->Read(slot);
            if (mask && boundary)
                result.surfaces.push_back(std::move(*mask));

            result.boundaryFrames += boundary ? 1 : 0;

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
                gpu.sourceEnergy != cpu.sourceEnergy || gpu.scheduledBlocks != cpu.scheduledBlocks ||
                gpu.limits != cpu.limits) {
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

    // 物の印(T-0332): 刻みの境界で終わったフレームごとに 1 つ・S(t) の CPU リファレンスと同じ・物のセルがある
    bool SurfacesMatch(std::span<const ProbeSurfaceMask> surfaces, const Reference& expected, size_t frames) {
        if (surfaces.size() != frames)
            return false;

        for (const ProbeSurfaceMask& mask : surfaces) {
            if (mask.tick >= expected.surfaces.size() || mask.words != expected.surfaces[mask.tick] ||
                std::ranges::all_of(mask.words, [](uint32_t word) { return word == 0; })) {
                Log(Channel::Sim, Level::Error, "  物の印 S({}) が CPU と違う(か空)", mask.tick);
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
            failures.Check(result.boundaryFrames > 0 && SurfacesMatch(result.surfaces, expected, result.boundaryFrames),
                           std::format("{}: 物の印(T-0332)が毎回 CPU リファレンスとビット一致", name));
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
    TestShapes(*table, failures);
    TestScene(device->Get(), *table, failures);

    const bool passesValidation = test::PassesValidation(*device, "gpu_probe_place_test");
    const bool passed = failures.count == 0 && passesValidation;
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_place_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
