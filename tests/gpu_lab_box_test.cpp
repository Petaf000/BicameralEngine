// gpu_lab_box_test.cpp — 実験室(sim/lab_session。T-0142)を GPU で走らせる: 箱の真ん中に木を置き(刻み 0)、1 つに火を付けて
// (刻み 3)200 刻み、毎刻み GPU と CPU リファレンスの状態の全部がビット一致すること。燃えたこと(GPU の箱でセルロースが減った)。
// 記録(コマンドの列 + ハッシュの列)を読み書きして初めの箱から流し直すと、同じハッシュの列になること(記録・再生)。
// 反応表を刻みの途中で替える(T-0218・ADR-0055): 次の刻みから新しい表で続き、毎刻みビット一致・替える前の刻みは同じ・替えない時と違う・
// 記録は刻み 0 の表の版と表の印を持ち、途中で替えた記録も古い表だけの記録も再生できる・持っていない表の記録は断る。
// 同じ操作を初めの箱から最新の表で流し直す(RerunWithLatestTable。T-0194 の案 A)も毎刻みビット一致。
// 表の中身(T-0217): 記録は使った表の中身を持ち、持っていない表は中身から足せば(AddTable)再生できる。
// 範囲(T-0220): 初めに箱全体を酸素の多い 200 kPa の空気にする範囲のコマンドと、木の温度を直方体で決めるコマンドも一致する。
// 物質を足す・消す表(T-0242・ADR-0065): 燃えている箱の表を、水素を足してセルロースを消した表に替える。印の刻みに箱の全部のセルを
// 名前で付け替え(GPU と CPU が同じ RxRemapCell)、毎刻みビット一致・元素は報告した端数のほか保たれる・別の実験室で記録の表を足すと再生できる・
// まだ刻んでいない置く操作の材料も新しい ID になる(消えた物質の材料は落ちる)。
// 仮の魔素(T-0225。試験用の仮の値): 450 K の木は魔素が無いと燃えないが、魔素を混ぜると燃える(魔素を触媒にする規則)。
// どちらも毎刻みビット一致し、魔素の量は変わらない。
// 一部を細かくした箱(T-0314・ADR-0137): 根 0 の (2..5)³ を 2 段細かくし、細かいセルに木・箱のセルに炭を置いて細かいセルに火を付ける。
// 細かくする要求の組(CPU は ProcessRequests・GPU は RecordProcessRequests)から先、毎刻みビット一致・細かい所の木が燃えた・
// 同じ形の別の箱で再生できる。
// 引数は gpu_test_options.h。
#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu_test_options.h"
#include "reaction_test_table.h"
#include "sim/lab_gauge.h"
#include "sim/lab_session.h"

using namespace bicameral;

namespace {

    constexpr uint32_t IGNITE_TICK = 3;
    constexpr uint32_t RUN_TICKS = 200;
    constexpr uint32_t IGNITE_MILLIKELVIN = 1500000;
    constexpr uint64_t FIRST_TABLE_VERSION = 0x1111;  // 試験の中だけの版(TableVersion ではない。違えばよい)
    constexpr uint64_t SWAPPED_TABLE_VERSION = 0x2222;
    constexpr uint64_t UNKNOWN_TABLE_VERSION = 0x3333;
    constexpr uint32_t SWAP_RUN_TICKS = 100;            // 表を替えてから刻む数
    constexpr uint64_t SPECIES_TABLE_VERSION = 0x4444;  // 物質を足す・消す表(T-0242)
    constexpr uint32_t SPECIES_FIRE_TICKS = 30;         // 物質の一覧を替える前に燃やす刻み

    // --- 仮の魔素の実験(T-0225)---
    constexpr uint64_t MANA_TABLE_VERSION = 0x4444;
    constexpr uint32_t MANA_RUN_TICKS = 100;
    constexpr uint32_t MANA_WARM_MILLIKELVIN = 450000;             // 魔素なしの木の燃焼(約 600 K から)では燃えない温度
    constexpr uint64_t MANA_WOOD_CELLULOSE_MICROMOLES = 38600000;  // 木(sim::MakeLabMaterials の「木」と同じ量)
    constexpr uint64_t MANA_WOOD_OXYGEN_MICROMOLES = 983000;
    constexpr uint64_t MANA_MICROMOLES = 500000;
    constexpr uint32_t MANA_WOOD_CELLS = 8;

    // 木の燃焼を 100 倍速くした表(物質の一覧は同じ。gpu_probe_sim_test の差し替えと同じ形)
    std::expected<sim::BakedReactionTable, std::string> MakeSwappedTable() {
        sim::ReactionTableDefinition definition = sim::MakeCombustionTestTable();
        for (sim::RuleDefinition& rule : definition.rules) {
            if (rule.name == "cellulose_combustion")
                rule.rate.preExponentialMantissa *= 100;
        }

        return sim::BakeReactionTable(definition);
    }

    uint64_t TotalOf(const sim::MultiresNest& nest, uint32_t species,
                     lab::LabBoxShape shape = sim::LAB_SINGLE_ROOT_SHAPE) {
        uint64_t total = 0;
        for (uint32_t slot = 0; slot < lab::LabRootCount(shape); ++slot) {
            for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
                const reaction::RxCell cell = sim::LoadNestCell(nest, slot, index);
                for (uint32_t i = 0; i < cell.speciesCount; ++i)
                    total += cell.species[i] == species ? cell.amounts[i] : 0;
            }
        }

        return total;
    }

    // 範囲(両端を含む)のセルの物質 species の合計
    uint64_t RegionTotalOf(const sim::MultiresNest& nest, sim::LabCellRange range, uint32_t species,
                           lab::LabBoxShape shape = sim::LAB_SINGLE_ROOT_SHAPE) {
        uint64_t total = 0;
        for (uint32_t z = range.low.z; z <= range.high.z; ++z) {
            for (uint32_t y = range.low.y; y <= range.high.y; ++y) {
                for (uint32_t x = range.low.x; x <= range.high.x; ++x) {
                    const reaction::RxCell cell = sim::LabBoxCell(nest, lab::LabCellIndex(x, y, z), shape);
                    for (uint32_t i = 0; i < cell.speciesCount; ++i)
                        total += cell.species[i] == species ? cell.amounts[i] : 0;
                }
            }
        }

        return total;
    }

    std::expected<void, std::string> CheckNoMismatch(const sim::LabSession& session) {
        if (const auto& mismatch = session.Mismatch(); mismatch)
            return std::unexpected(std::format("CPU と GPU が食い違う: {}", mismatch->what));

        return {};
    }

    // 木を置いて火を付ける実験
    std::expected<void, std::string> RunFire(sim::LabSession& session) {
        const std::vector<sim::LabMaterial> materials = sim::MakeLabMaterials(session.Table());
        const auto wood = std::ranges::find_if(materials, [](const sim::LabMaterial& m) { return m.name == "木"; });
        if (wood == materials.end())
            return std::unexpected("木の材料が無い");

        // --- 箱全体を酸素の多い 200 kPa の空気に(範囲・分圧の換算。T-0220)---
        const std::array<sim::SpeciesAmount, 2> atmosphere = {
            sim::SpeciesAmount{.species = session.Table().SpeciesId("oxygen"),
                               .amount = sim::LabGasMicromoles(60000, 300000)},
            sim::SpeciesAmount{.species = session.Table().SpeciesId("nitrogen"),
                               .amount = sim::LabGasMicromoles(140000, 300000)}};
        if (!session.PlaceRegion(sim::LabWholeBox(), atmosphere, 300000))
            return std::unexpected("箱全体に置けない");

        for (uint32_t i = 0; i < 8; ++i) {
            const sim::LabCellPosition cell{.x = 3 + (i & 1u), .y = 3 + ((i >> 1) & 1u), .z = 3 + (i >> 2)};
            if (!session.Place(cell, wood->contents, 300000))
                return std::unexpected("置けない");
        }

        if (!session.SetTemperatureRegion({.low = {.x = 3, .y = 3, .z = 3}, .high = {.x = 4, .y = 4, .z = 4}}, 320000))
            return std::unexpected("範囲の温度を決められない");

        if (auto stepped = session.Step(IGNITE_TICK); !stepped)
            return stepped;

        if (!session.SetTemperature({.x = 3, .y = 3, .z = 3}, IGNITE_MILLIKELVIN))
            return std::unexpected("温度を決められない");

        if (auto stepped = session.Step(RUN_TICKS - IGNITE_TICK); !stepped)
            return stepped;

        if (auto checked = CheckNoMismatch(session); !checked)
            return checked;

        const uint32_t cellulose = session.Table().SpeciesId("cellulose");
        const uint64_t placed = 8 * wood->contents[0].amount;
        const uint64_t left = TotalOf(session.Gpu(), cellulose);
        Log(Channel::Gpu, Level::Info, "gpu_lab_box_test: {} 刻みでセルロース {} → {} µmol(GPU)", RUN_TICKS, placed,
            left);
        if (left >= placed)
            return std::unexpected("燃えていない");

        return {};
    }

    std::expected<void, std::string> RunReplay(sim::LabSession& session) {
        const sim::LabRecording recorded = session.Recording();
        const auto parsed = sim::ParseLabRecording(sim::SerializeLabRecording(recorded));
        if (!parsed)
            return std::unexpected(parsed.error());

        if (auto replayed = session.Replay(*parsed); !replayed)
            return replayed;

        if (auto checked = CheckNoMismatch(session); !checked)
            return checked;

        if (session.ReplayDivergence())
            return std::unexpected(std::format("再生が記録と刻み {} で違う", *session.ReplayDivergence()));

        const sim::LabRecording again = session.Recording();
        if (again.tickCount != recorded.tickCount || again.hashes != recorded.hashes ||
            again.commands != recorded.commands)
            return std::unexpected("再生した記録が元と違う");

        return {};
    }

    // 刻み 0〜RUN_TICKS − 1 を表 FIRST で、コマンドの列(表の印を除く)を CPU だけで流した最後のハッシュ(替えない時の比べる相手)
    uint64_t CpuHashWithoutSwap(const sim::BakedReactionTable& table, const std::vector<sim::Command>& commands,
                                uint64_t tickCount) {
        sim::MultiresNest nest = sim::MakeLabBoxNest(table);
        for (uint64_t tick = 0; tick < tickCount; ++tick) {
            std::vector<sim::Command> now;
            for (const sim::Command& command : commands) {
                if (command.targetTick == tick && !sim::LabTableVersionOf(command))
                    now.push_back(command);
            }

            sim::StepLabBox(nest, table, tick, now);
        }

        return sim::HashWholeNest(nest);
    }

    // 刻みの途中で表を替える(T-0218・ADR-0055): 次の刻みから新しい表で続き、毎刻みビット一致・替える前の刻みは同じ・記録の印・再生
    std::expected<void, std::string> RunMidTableChange(sim::LabSession& session, const sim::BakedReactionTable& first,
                                                       const sim::BakedReactionTable& swapped) {
        const sim::LabRecording before = session.Recording();
        if (auto changed = session.ChangeTable(swapped, SWAPPED_TABLE_VERSION, "swapped"); !changed)
            return changed;

        if (!session.TableChangePending() || session.TableVersion() != FIRST_TABLE_VERSION)
            return std::unexpected("表を替えた印が置かれていない(か、刻む前に替わった)");

        if (auto stepped = session.Step(SWAP_RUN_TICKS); !stepped)
            return stepped;

        if (auto checked = CheckNoMismatch(session); !checked)
            return checked;

        const sim::LabRecording after = session.Recording();
        const uint64_t unswapped = CpuHashWithoutSwap(first, after.commands, after.tickCount);
        Log(Channel::Gpu, Level::Info,
            "gpu_lab_box_test: 刻み {} で表を替えて {} 刻み: 最後のハッシュ {:016x}(替えない時 {:016x})",
            before.tickCount, SWAP_RUN_TICKS, after.hashes.back(), unswapped);
        const bool samePrefix = std::ranges::equal(std::span(after.hashes).first(before.hashes.size()), before.hashes);
        const auto marks = std::ranges::count_if(after.commands, [&](const sim::Command& command) {
            return sim::LabTableVersionOf(command) == SWAPPED_TABLE_VERSION && command.targetTick == before.tickCount;
        });
        if (!samePrefix || marks != 1 || after.tableVersion != FIRST_TABLE_VERSION ||
            session.TableVersion() != SWAPPED_TABLE_VERSION)
            return std::unexpected("替える前の刻みが変わった・印が無い・記録の刻み 0 の表の版が違う");

        if (after.hashes.back() == unswapped)
            return std::unexpected("表を替えても箱が変わらない");

        // --- 記録は刻み 0 と印の表の中身を持つ(T-0217)---
        const bool contents = after.tables.size() == 2 && after.tables[0].version == FIRST_TABLE_VERSION &&
                              after.tables[0].bytes == "first" && after.tables[1].version == SWAPPED_TABLE_VERSION &&
                              after.tables[1].bytes == "swapped";
        if (!contents || before.tables.size() != 1)
            return std::unexpected("記録に使った表の中身が無い");

        // --- 途中で表を替えた記録をファイルの形を通して再生(両方の表をこの実験室が持っている)---
        if (auto replayed = RunReplay(session); !replayed)
            return replayed;

        // --- 替える前の記録(古い表だけ)も再生でき、流し終えると最新の表に戻す印が置かれる ---
        if (auto replayed = session.Replay(before); !replayed)
            return replayed;

        if (session.Recording().hashes != before.hashes || session.ReplayDivergence() || !session.TableChangePending())
            return std::unexpected("古い表の記録の再生が元と違う(か、最新の表に戻す印が無い)");

        // --- 持っていない表の記録は流さずに断る ---
        sim::LabRecording unknown = before;
        unknown.tableVersion = UNKNOWN_TABLE_VERSION;
        if (session.Replay(unknown).has_value())
            return std::unexpected("持っていない表の記録を再生できてしまった");

        // --- 記録の中身から表を足せば再生できる(別の起動の形。中身から表を作り直すのは呼ぶ側。T-0217)---
        if (auto added = session.AddTable(first, UNKNOWN_TABLE_VERSION, "unknown"); !added)
            return added;

        if (auto replayed = session.Replay(unknown); !replayed)
            return replayed;

        if (session.Recording().hashes != before.hashes || session.ReplayDivergence())
            return std::unexpected("足した表で再生した記録が元と違う");

        return {};
    }

    // 案 A(T-0194。RerunWithLatestTable): 同じ操作を初めの箱から最新の表で流し直す
    std::expected<void, std::string> RunRerun(sim::LabSession& session) {
        const sim::LabRecording
            before = session.Recording();  // 古い表だけの記録を再生した後(最新の表に戻す印が待っている)
        if (auto rerun = session.RerunWithLatestTable(); !rerun)
            return rerun;

        if (auto checked = CheckNoMismatch(session); !checked)
            return checked;

        const sim::LabRecording after = session.Recording();
        if (after.tickCount != before.tickCount || after.commands != before.commands ||
            after.tableVersion != SWAPPED_TABLE_VERSION || after.hashes.back() == before.hashes.back())
            return std::unexpected("最新の表で流し直した刻み・操作・表の版が違う(か、箱が変わらない)");

        return {};
    }

    std::expected<void, std::string> RunTableChange(sim::LabSession& session, const sim::BakedReactionTable& first) {
        const auto swapped = MakeSwappedTable();
        if (!swapped)
            return std::unexpected(swapped.error());

        if (auto result = RunMidTableChange(session, first, *swapped); !result)
            return std::unexpected(std::format("途中で替える: {}", result.error()));

        if (auto result = RunRerun(session); !result)
            return std::unexpected(std::format("初めから流し直す: {}", result.error()));

        return {};
    }

    // --- 物質を足す・消す表(T-0242)---

    // 元素の名前ごとの原子の数(CPU の箱の全部のセル)
    std::vector<sim::NamedElementCount> CountBoxElements(const sim::LabSession& session) {
        std::vector<sim::NamedElementCount> totals;
        const lab::LabBoxShape shape = session.Shape();
        for (uint32_t index = 0; index < lab::LAB_CELL_INDEX_LIMIT; ++index) {
            if (!lab::LabCellInBox(shape, index))
                continue;

            for (const sim::NamedElementCount& count :
                 sim::CountNamedElements(session.Table(), sim::LabBoxCell(session.Cpu(), index, shape))) {
                const auto found = std::ranges::find(totals, count.element, &sim::NamedElementCount::element);
                if (found == totals.end())
                    totals.push_back(count);
                else
                    found->atomMicromoles += count.atomMicromoles;
            }
        }

        std::ranges::sort(totals, {}, &sim::NamedElementCount::element);

        return totals;
    }

    uint64_t SumAtoms(std::span<const sim::NamedElementCount> totals) {
        uint64_t sum = 0;
        for (const sim::NamedElementCount& count : totals)
            sum += count.atomMicromoles;

        return sum;
    }

    // まだ刻んでいない置く操作の材料の付け替え(CPU だけ): [セルロース・酸素・窒素] → [酸素・窒素](新しい ID)
    std::expected<void, std::string> CheckPendingRemap(const sim::BakedReactionTable& first,
                                                       const sim::BakedReactionTable& changed) {
        const auto remap = sim::BuildSpeciesRemap(first, changed);
        if (!remap)
            return std::unexpected(remap.error());

        const std::array<sim::SpeciesAmount, 3> contents = {
            sim::SpeciesAmount{.species = first.SpeciesId("cellulose"), .amount = 100},
            sim::SpeciesAmount{.species = first.SpeciesId("nitrogen"), .amount = 200},
            sim::SpeciesAmount{.species = first.SpeciesId("oxygen"), .amount = 0x1'0000'0300}};
        const sim::Command command = sim::MakeLabFillCommand(5, 0, {.x = 1, .y = 2, .z = 3}, contents, 400000);
        const sim::Command expected = sim::MakeLabFillCommand(
            5, 0, {.x = 1, .y = 2, .z = 3},
            std::array{sim::SpeciesAmount{.species = changed.SpeciesId("nitrogen"), .amount = 200},
                       sim::SpeciesAmount{.species = changed.SpeciesId("oxygen"), .amount = 0x1'0000'0300}},
            400000);
        if (sim::RemapLabCommand(command, *remap) != expected)
            return std::unexpected("置く操作の材料が新しい表の ID にならない");

        return {};
    }

    // 450 K の木(セルロースと孔の酸素。withMana なら魔素も)を 8 セル置いて刻み、燃えたセルロースの量(GPU の箱)
    std::expected<uint64_t, std::string> BurnWarmWood(sim::LabSession& session, bool withMana) {
        if (auto reset = session.Reset(); !reset)
            return std::unexpected(reset.error());

        const sim::BakedReactionTable& table = session.Table();
        const uint32_t cellulose = table.SpeciesId("cellulose");
        const uint32_t manaTest = table.SpeciesId("mana_test");
        if (cellulose == 0 || manaTest == 0)
            return std::unexpected("表にセルロースか仮の魔素が無い");

        std::vector<sim::SpeciesAmount> contents = {
            {.species = cellulose, .amount = MANA_WOOD_CELLULOSE_MICROMOLES},
            {.species = table.SpeciesId("oxygen"), .amount = MANA_WOOD_OXYGEN_MICROMOLES}};
        if (withMana)
            contents.push_back({.species = manaTest, .amount = MANA_MICROMOLES});

        std::ranges::sort(contents, {}, &sim::SpeciesAmount::species);
        for (uint32_t i = 0; i < MANA_WOOD_CELLS; ++i) {
            const sim::LabCellPosition cell{.x = 3 + (i & 1u), .y = 3 + ((i >> 1) & 1u), .z = 3 + (i >> 2)};
            if (!session.Place(cell, contents, MANA_WARM_MILLIKELVIN))
                return std::unexpected("置けない");
        }

        if (auto stepped = session.Step(MANA_RUN_TICKS); !stepped)
            return std::unexpected(stepped.error());

        if (auto checked = CheckNoMismatch(session); !checked)
            return std::unexpected(checked.error());

        const uint64_t expectedMana = withMana ? MANA_WOOD_CELLS * MANA_MICROMOLES : 0;
        if (TotalOf(session.Gpu(), manaTest) != expectedMana)
            return std::unexpected("触媒の魔素の量が変わった");

        const uint64_t placed = MANA_WOOD_CELLS * MANA_WOOD_CELLULOSE_MICROMOLES;
        const uint64_t left = TotalOf(session.Gpu(), cellulose);
        if (left > placed)
            return std::unexpected("セルロースが増えた");

        return placed - left;
    }

    std::expected<void, std::string> RunMana(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType,
                                             const sim::BakedReactionTable& table) {
        auto session = sim::LabSession::Create(device, queueType, table, MANA_TABLE_VERSION, "mana");
        if (!session)
            return std::unexpected(session.error());

        const auto withMana = BurnWarmWood(*session, true);
        if (!withMana)
            return std::unexpected(std::format("魔素あり: {}", withMana.error()));

        const auto withoutMana = BurnWarmWood(*session, false);
        if (!withoutMana)
            return std::unexpected(std::format("魔素なし: {}", withoutMana.error()));

        Log(Channel::Gpu, Level::Info,
            "gpu_lab_box_test: 450 K の木 {} 刻みで燃えたセルロース 魔素あり {}・なし {} µmol", MANA_RUN_TICKS,
            *withMana, *withoutMana);
        if (*withMana <= 10 * (*withoutMana + 1))
            return std::unexpected("魔素を混ぜても木の燃え方が変わらない");

        return {};
    }

    std::expected<void, std::string> RunSpeciesChange(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType,
                                                      const sim::BakedReactionTable& first) {
        const auto changed = sim::BakeReactionTable(sim::MakeSpeciesChangedTestTable());
        if (!changed)
            return std::unexpected(changed.error());

        if (auto pending = CheckPendingRemap(first, *changed); !pending)
            return pending;

        auto session = sim::LabSession::Create(device, queueType, first, FIRST_TABLE_VERSION, "first");
        if (!session)
            return std::unexpected(session.error());

        // --- 300 K の空気の箱に木を置いて燃やし、燃えている途中で物質の一覧を替える ---
        const std::vector<sim::LabMaterial> materials = sim::MakeLabMaterials(first);
        const auto wood = std::ranges::find_if(materials, [](const sim::LabMaterial& m) { return m.name == "木"; });
        if (wood == materials.end())
            return std::unexpected("木の材料が無い");

        for (uint32_t i = 0; i < 8; ++i)
            static_cast<void>(session->Place({.x = 3 + (i & 1u), .y = 3 + ((i >> 1) & 1u), .z = 3 + (i >> 2)},
                                             wood->contents, 300000));

        static_cast<void>(session->Step(IGNITE_TICK));
        static_cast<void>(session->SetTemperature({.x = 3, .y = 3, .z = 3}, IGNITE_MILLIKELVIN));
        if (auto stepped = session->Step(SPECIES_FIRE_TICKS); !stepped)
            return stepped;

        const std::vector<sim::NamedElementCount> before = CountBoxElements(*session);
        if (auto change = session->ChangeTable(*changed, SPECIES_TABLE_VERSION, "species"); !change)
            return change;

        if (auto stepped = session->Step(1); !stepped)
            return stepped;

        // --- 印の刻み: 付け替えた(セルロースを分けた)・元素は失った端数のほか保たれる・毎刻み一致 ---
        const std::vector<sim::NamedElementCount> after = CountBoxElements(*session);
        const auto& report = session->LastRemapReport();
        const uint64_t lost = report ? report->remainderAtomMicromoles + report->overflowAtomMicromoles : 0;
        Log(Channel::Gpu, Level::Info,
            "gpu_lab_box_test: 物質の一覧を替えた: 分けた物質 {} µmol・失った原子 {} µmol・足したエネルギー {} "
            "mJ・水素 {} µmol",
            report ? report->decomposedMicromoles : 0, lost, report ? report->energyDeltaMilliJoules : 0,
            TotalOf(session->Gpu(), changed->SpeciesId("hydrogen")));
        if (!report || report->decomposedMicromoles == 0 || session->TableVersion() != SPECIES_TABLE_VERSION)
            return std::unexpected("物質の付け替えが箱に当たっていない");

        if (SumAtoms(before) != SumAtoms(after) + lost || TotalOf(session->Gpu(), changed->SpeciesId("hydrogen")) == 0)
            return std::unexpected(std::format("元素が保たれない(前 {} µmol・後 {} µmol・失った {} µmol)",
                                               SumAtoms(before), SumAtoms(after), lost));

        if (auto stepped = session->Step(SWAP_RUN_TICKS); !stepped)
            return stepped;

        if (auto checked = CheckNoMismatch(*session); !checked)
            return checked;

        // --- 別の実験室(最初の表だけを持つ)で、記録の表を足して再生する(別の起動の形)---
        const sim::LabRecording recorded = session->Recording();
        const auto parsed = sim::ParseLabRecording(sim::SerializeLabRecording(recorded));
        auto other = sim::LabSession::Create(device, queueType, first, FIRST_TABLE_VERSION, "first");
        if (!parsed || !other)
            return std::unexpected("記録を読めない・実験室を作れない");

        if (auto added = other->AddTable(*changed, SPECIES_TABLE_VERSION, "species"); !added)
            return added;

        if (auto replayed = other->Replay(*parsed); !replayed)
            return replayed;

        if (auto checked = CheckNoMismatch(*other); !checked)
            return checked;

        if (other->ReplayDivergence() || other->Recording().hashes != recorded.hashes)
            return std::unexpected("物質の一覧を替えた記録の再生が元と違う");

        // --- 最新の表で初めから流し直す: 古い表で置いた材料(木 = セルロース)は落ちて、毎刻み一致 ---
        if (auto rerun = session->RerunWithLatestTable(); !rerun)
            return rerun;

        return CheckNoMismatch(*session);
    }

    // --- 根を並べた箱(T-0173)---
    constexpr lab::LabBoxShape LARGE_SHAPE = {
        .rootsX = 4, .rootsY = 1, .rootsZ = 2};  // x に 4・z に 2(枠の順を確かめる)
    constexpr uint32_t LARGE_RUN_TICKS = 100;
    constexpr uint32_t LARGE_SWAP_TICKS = 20;

    // 根の境(x と z の 7 | 8)を挟んで木を 2×2×2 に置き、(7, 3, 7) に火を付けて流す → 表を替えて流す → 同じ形の別の箱で再生。
    // 毎刻みビット一致・燃えた・隣の根の木のエネルギーが増えた(根の境を熱が通る)・違う形の記録は断る
    std::expected<void, std::string> RunLargeBox(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType,
                                                 const sim::BakedReactionTable& table) {
        auto session = sim::LabSession::Create(device, queueType, table, FIRST_TABLE_VERSION, "first", LARGE_SHAPE);
        if (!session)
            return std::unexpected(session.error());

        const std::vector<sim::LabMaterial> materials = sim::MakeLabMaterials(table);
        const auto wood = std::ranges::find_if(materials, [](const sim::LabMaterial& m) { return m.name == "木"; });
        if (wood == materials.end())
            return std::unexpected("木の材料が無い");

        // --- 箱全体の空気 → 根の境に木 → 火 ---
        const std::array<sim::SpeciesAmount, 2> atmosphere = {
            sim::SpeciesAmount{.species = table.SpeciesId("oxygen"), .amount = sim::LabGasMicromoles(60000, 300000)},
            sim::SpeciesAmount{.species = table.SpeciesId("nitrogen"),
                               .amount = sim::LabGasMicromoles(140000, 300000)}};
        if (!session->PlaceRegion(sim::LabWholeBox(LARGE_SHAPE), atmosphere, 300000))
            return std::unexpected("箱全体に置けない");

        constexpr uint32_t BORDER = sim::LAB_BOX_EDGE - 1;
        for (uint32_t i = 0; i < 8; ++i) {
            const sim::LabCellPosition cell{.x = BORDER + (i & 1u), .y = 3 + ((i >> 1) & 1u), .z = BORDER + (i >> 2)};
            if (!session->Place(cell, wood->contents, 300000))
                return std::unexpected("置けない");
        }

        if (auto stepped = session->Step(IGNITE_TICK); !stepped)
            return stepped;

        const uint32_t neighbor = sim::LabCellPosition{.x = BORDER + 1, .y = 3, .z = BORDER}.Index();
        const int64_t neighborBefore = sim::LabBoxCell(session->Gpu(), neighbor, LARGE_SHAPE).energy;
        if (!session->SetTemperature({.x = BORDER, .y = 3, .z = BORDER}, IGNITE_MILLIKELVIN))
            return std::unexpected("温度を決められない");

        if (auto stepped = session->Step(LARGE_RUN_TICKS - IGNITE_TICK); !stepped)
            return stepped;

        if (auto checked = CheckNoMismatch(*session); !checked)
            return checked;

        const uint32_t cellulose = table.SpeciesId("cellulose");
        const uint64_t placed = 8 * wood->contents[0].amount;
        const uint64_t left = TotalOf(session->Gpu(), cellulose, LARGE_SHAPE);
        const int64_t neighborAfter = sim::LabBoxCell(session->Gpu(), neighbor, LARGE_SHAPE).energy;
        Log(Channel::Gpu, Level::Info,
            "gpu_lab_box_test: 4×1×2 の根の箱で {} 刻み: セルロース {} → {} µmol・隣の根の木 {} → {} mJ(GPU)",
            LARGE_RUN_TICKS, placed, left, neighborBefore, neighborAfter);
        if (left >= placed)
            return std::unexpected("燃えていない");

        if (neighborAfter <= neighborBefore)
            return std::unexpected("根の境の向こうの木が温まっていない");

        // --- 刻みの途中で表を替える(印は全部の根をつつく)---
        const auto swapped = MakeSwappedTable();
        if (!swapped)
            return std::unexpected(swapped.error());

        if (auto changed = session->ChangeTable(*swapped, SWAPPED_TABLE_VERSION, "swapped"); !changed)
            return changed;

        if (auto stepped = session->Step(LARGE_SWAP_TICKS); !stepped)
            return stepped;

        if (auto checked = CheckNoMismatch(*session); !checked)
            return checked;

        // --- 同じ形の別の箱で再生(ファイルの形を通す)・違う形の記録は断る ---
        const sim::LabRecording recorded = session->Recording();
        const auto parsed = sim::ParseLabRecording(sim::SerializeLabRecording(recorded));
        if (!parsed || !sim::SameLabShape(parsed->shape, LARGE_SHAPE))
            return std::unexpected("記録の箱の形が残らない");

        auto other = sim::LabSession::Create(device, queueType, table, FIRST_TABLE_VERSION, "first", LARGE_SHAPE);
        if (!other)
            return std::unexpected(other.error());

        if (auto added = other->AddTable(*swapped, SWAPPED_TABLE_VERSION, "swapped"); !added)
            return added;

        if (auto replayed = other->Replay(*parsed); !replayed)
            return replayed;

        if (auto checked = CheckNoMismatch(*other); !checked)
            return checked;

        if (other->ReplayDivergence() || other->Recording().hashes != recorded.hashes)
            return std::unexpected("別の箱での再生が記録と違う");

        sim::LabRecording single = recorded;
        single.shape = sim::LAB_SINGLE_ROOT_SHAPE;
        if (other->Replay(single).has_value())
            return std::unexpected("違う形の記録を再生できてしまった");

        return {};
    }

    // --- 一部を細かくした箱(T-0314)---
    constexpr lab::LabBoxShape REFINED_SHAPE = {
        .rootsX = 2, .rootsY = 1, .rootsZ = 1, .refineBlocks = sim::LAB_DEFAULT_REFINE_BLOCKS};
    constexpr uint32_t REFINED_RUN_TICKS = 60;

    std::expected<void, std::string> RunRefinedBox(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType,
                                                   const sim::BakedReactionTable& table) {
        auto session = sim::LabSession::Create(device, queueType, table, FIRST_TABLE_VERSION, "first", REFINED_SHAPE);
        if (!session)
            return std::unexpected(session.error());

        const std::vector<sim::LabMaterial> materials = sim::MakeLabMaterials(table);
        const auto find = [&](std::string_view name) {
            return std::ranges::find_if(materials, [&](const sim::LabMaterial& m) { return m.name == name; });
        };
        const auto wood = find("木");
        const auto charcoal = find("木炭");
        if (wood == materials.end() || charcoal == materials.end())
            return std::unexpected("木か木炭の材料が無い");

        // --- 刻み 0: 細かくする → 刻み 1: 細かいセル 2³ に木・粗い葉(レベル 1)と根 1(レベル 0)に炭 ---
        if (!session->Refine({.low = {.x = 2, .y = 2, .z = 2}, .high = {.x = 5, .y = 5, .z = 5}}, 2))
            return std::unexpected("細かくできない");

        if (auto stepped = session->Step(1); !stepped)
            return stepped;

        if (sim::LabRefinedBlockCount(session->Gpu()) != 16)
            return std::unexpected(
                std::format("細かくしたブロックが 16 でない({})", sim::LabRefinedBlockCount(session->Gpu())));

        for (uint32_t i = 0; i < 8; ++i) {
            const sim::LabCellPosition cell{.x = 8 + (i & 1u), .y = 8 + ((i >> 1) & 1u), .z = 8 + (i >> 2), .level = 2};
            if (!session->Place(cell, wood->contents, 300000))
                return std::unexpected("細かいセルに置けない");
        }

        if (!session->Place({.x = 6, .y = 3, .z = 3}, charcoal->contents, 300000) ||
            !session->Place({.x = 9, .y = 3, .z = 3}, charcoal->contents, 300000))
            return std::unexpected("箱のセルに置けない");

        if (auto stepped = session->Step(IGNITE_TICK - 1); !stepped)
            return stepped;

        // --- 細かいセル 1 つに火を付けて流す(毎刻み CPU と比べる)---
        if (!session->SetTemperature({.x = 8, .y = 8, .z = 8, .level = 2}, IGNITE_MILLIKELVIN))
            return std::unexpected("温度を決められない");

        if (auto stepped = session->Step(REFINED_RUN_TICKS - IGNITE_TICK); !stepped)
            return stepped;

        if (auto checked = CheckNoMismatch(*session); !checked)
            return checked;

        const uint32_t cellulose = table.SpeciesId("cellulose");
        uint64_t left = 0;
        for (uint32_t i = 0; i < 8; ++i) {
            const auto cell = sim::LabBoxFineCell(
                session->Gpu(), {.x = 8 + (i & 1u), .y = 8 + ((i >> 1) & 1u), .z = 8 + (i >> 2), .level = 2});
            if (!cell)
                return std::unexpected("細かいセルが無い");

            for (uint32_t s = 0; s < cell->speciesCount; ++s)
                left += cell->species[s] == cellulose ? cell->amounts[s] : 0;
        }

        const uint64_t placed = 8 * wood->contents[0].amount;
        Log(Channel::Gpu, Level::Info,
            "gpu_lab_box_test: 2 段細かくした箱で {} 刻み: 細かいセルのセルロース {} → {}(8^-2 µmol。GPU)",
            REFINED_RUN_TICKS, placed, left);
        if (left >= placed)
            return std::unexpected("細かい所の木が燃えていない");

        // --- 同じ形の別の箱で再生(ファイルの形を通す)---
        const sim::LabRecording recorded = session->Recording();
        const auto parsed = sim::ParseLabRecording(sim::SerializeLabRecording(recorded));
        if (!parsed || !sim::SameLabShape(parsed->shape, REFINED_SHAPE))
            return std::unexpected("記録の箱の形(細かくする枠)が残らない");

        auto other = sim::LabSession::Create(device, queueType, table, FIRST_TABLE_VERSION, "first", REFINED_SHAPE);
        if (!other)
            return std::unexpected(other.error());

        if (auto replayed = other->Replay(*parsed); !replayed)
            return replayed;

        if (auto checked = CheckNoMismatch(*other); !checked)
            return checked;

        if (other->ReplayDivergence() || other->Recording().hashes != recorded.hashes)
            return std::unexpected("別の箱での再生が記録と違う");

        return {};
    }

    // --- 魔法のパッチ(T-0024・ADR-0095)。値は試験用の仮のもの(D-006)---

    uint32_t RuleIdOf(const sim::BakedReactionTable& table, std::string_view name) {
        const auto found = std::ranges::find(table.ruleNames, name);
        return found == table.ruleNames.end() ? UINT32_MAX
                                              : static_cast<uint32_t>(std::distance(table.ruleNames.begin(), found));
    }

    // 4 か所の木(どれも 2×2×2 セル)
    struct PatchGroups {
        sim::LabCellRange stopped{.low = {.x = 1, .y = 1, .z = 1},
                                  .high = {.x = 2, .y = 2, .z = 2}};  // 魔素あり・止める
        sim::LabCellRange catalyzed{.low = {.x = 5, .y = 1, .z = 1},
                                    .high = {.x = 6, .y = 2, .z = 2}};  // 魔素あり・そのまま
        sim::LabCellRange amplified{.low = {.x = 1, .y = 5, .z = 5},
                                    .high = {.x = 2, .y = 6, .z = 6}};  // 魔素なし・速める
        sim::LabCellRange plain{.low = {.x = 5, .y = 5, .z = 5},
                                .high = {.x = 6, .y = 6, .z = 6}};  // 魔素なし・そのまま
    };

    // 根を並べた箱(T-0173)の 4 か所: 止める所は x と z の根の境、速める所は x の根の境をまたぐ
    constexpr PatchGroups LARGE_PATCH_GROUPS = {
        .stopped = {.low = {.x = 7, .y = 1, .z = 7}, .high = {.x = 8, .y = 2, .z = 8}},
        .catalyzed = {.low = {.x = 15, .y = 1, .z = 1}, .high = {.x = 16, .y = 2, .z = 2}},
        .amplified = {.low = {.x = 23, .y = 5, .z = 3}, .high = {.x = 24, .y = 6, .z = 4}},
        .plain = {.low = {.x = 29, .y = 5, .z = 12}, .high = {.x = 30, .y = 6, .z = 13}}};

    // 表を替える刻みを跨ぐパッチ(T-0287): パッチの途中(刻み PATCH_SWAP_TICK)で table に替える。版は試験の中だけ
    struct PatchSwap {
        const sim::BakedReactionTable* table = nullptr;
        uint64_t version = 0;
    };

    constexpr uint32_t PATCH_SWAP_TICK = 20;
    constexpr uint64_t PATCH_SAVE_POINT_TICK = 30;  // 保存点(パッチの途中・表を替えた後で切った記録)
    constexpr uint64_t SHIFTED_TABLE_VERSION = 0x5555;

    // 規則の ID がずれる表: 名前の順で前にある規則(boudouard)を抜く(ベイクは規則を名前の順に並べるので、定義の並びを替えても ID は変わらない)。
    // 物質は同じ。パッチの規則(燃焼 2 つ)は残り、ID が 1 つずつ前へずれる
    std::expected<sim::BakedReactionTable, std::string> MakeShiftedTable() {
        sim::ReactionTableDefinition definition = sim::MakeCombustionTestTable();
        std::erase_if(definition.rules, [](const sim::RuleDefinition& rule) { return rule.name == "boudouard"; });

        return sim::BakeReactionTable(definition);
    }

    // 保存点(パッチの途中で切った記録)と記録全体を、別の箱で初めから再生する → 毎刻みビット一致・ハッシュが記録と同じ
    // (パッチは記録のコマンドで、効いているパッチの語は状態の要約に入る。T-0287)
    std::expected<void, std::string> ReplayPatchRecording(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType,
                                                          const sim::BakedReactionTable& table, const PatchSwap& swap,
                                                          lab::LabBoxShape shape, const sim::LabRecording& recorded) {
        auto other = sim::LabSession::Create(device, queueType, table, MANA_TABLE_VERSION, "patch", shape);
        if (!other)
            return std::unexpected(other.error());

        if (auto added = other->AddTable(*swap.table, swap.version, "shifted"); !added)
            return added;

        const sim::LabRecording savePoint = sim::CutLabRecording(recorded, PATCH_SAVE_POINT_TICK);
        for (const sim::LabRecording* recording : {&savePoint, &recorded}) {
            if (auto replayed = other->Replay(*recording); !replayed)
                return replayed;

            if (auto checked = CheckNoMismatch(*other); !checked)
                return checked;

            if (other->ReplayDivergence() || other->Recording().hashes != recording->hashes)
                return std::unexpected(std::format("パッチの記録({} 刻み)の再生が元と違う", recording->tickCount));
        }

        return {};
    }

    constexpr int32_t PATCH_SHIFT = 10;
    constexpr uint32_t PATCH_CATALYST_JOULES_PER_MOL = 30000;  // 触媒(T-0288)。450 K で約 3000 倍(仮)

    // 450 K の木を 4 か所に置き、2 か所にパッチ(魔素の触媒の規則を止める・魔素なしの燃焼を速める〔amplifyEffect。既定は
    // 2^PATCH_SHIFT 倍。T-0288 で活性化エネルギーを下げる触媒も〕)を PATCH_TICKS 刻み。
    // 毎刻み CPU と GPU がビット一致・元素は保存・止めた所は燃えず、終わると燃え始める・速めた所はそのままの所よりずっと燃える
    // shape = 箱の形(大きい箱では範囲は箱の座標。パッチは範囲が重なる根だけをつつく)。swap.table があれば、パッチの途中で
    // その表に替え(パッチは名前の鍵で同じ規則に効き続ける)、終わった後で保存点と記録全体を別の箱で再生する(T-0287)
    std::expected<void, std::string> RunPatch(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType,
                                              const sim::BakedReactionTable& table,
                                              lab::LabBoxShape shape = sim::LAB_SINGLE_ROOT_SHAPE,
                                              const PatchGroups& groups = {}, const PatchSwap& swap = {},
                                              uint32_t amplifyEffect = reaction::MpMakeScaleEffect(PATCH_SHIFT)) {
        constexpr uint32_t PATCH_TICKS = 50;
        auto session = sim::LabSession::Create(device, queueType, table, MANA_TABLE_VERSION, "patch", shape);
        if (!session)
            return std::unexpected(session.error());

        const uint32_t cellulose = table.SpeciesId("cellulose");
        const uint32_t oxygen = table.SpeciesId("oxygen");
        const uint32_t manaTest = table.SpeciesId("mana_test");
        const uint32_t catalyzedRule = RuleIdOf(table, "mana_test_catalyzed_cellulose_combustion");
        const uint32_t plainRule = RuleIdOf(table, "cellulose_combustion");
        if (cellulose == 0 || manaTest == 0 || catalyzedRule == UINT32_MAX || plainRule == UINT32_MAX)
            return std::unexpected("表に試験の物質か規則が無い");

        // --- 置く(魔素ありの 2 か所・なしの 2 か所)とパッチ 2 つ(同じ刻み 0 のコマンド)---
        std::vector<sim::SpeciesAmount> wood = {{.species = cellulose, .amount = MANA_WOOD_CELLULOSE_MICROMOLES},
                                                {.species = oxygen, .amount = MANA_WOOD_OXYGEN_MICROMOLES}};
        std::vector<sim::SpeciesAmount> manaWood = wood;
        manaWood.push_back({.species = manaTest, .amount = MANA_MICROMOLES});
        std::ranges::sort(wood, {}, &sim::SpeciesAmount::species);
        std::ranges::sort(manaWood, {}, &sim::SpeciesAmount::species);
        const bool queued = session->PlaceRegion(groups.stopped, manaWood, MANA_WARM_MILLIKELVIN) &&
                            session->PlaceRegion(groups.catalyzed, manaWood, MANA_WARM_MILLIKELVIN) &&
                            session->PlaceRegion(groups.amplified, wood, MANA_WARM_MILLIKELVIN) &&
                            session->PlaceRegion(groups.plain, wood, MANA_WARM_MILLIKELVIN) &&
                            session->ApplyPatch(catalyzedRule, reaction::MpMakeStopEffect(), PATCH_TICKS,
                                                groups.stopped) &&
                            session->ApplyPatch(plainRule, amplifyEffect, PATCH_TICKS, groups.amplified);
        if (!queued)
            return std::unexpected("置けない・パッチを出せない");

        if (auto stepped = session->Step(1); !stepped)
            return std::unexpected(stepped.error());

        const std::vector<sim::NamedElementCount> elementsBefore = CountBoxElements(*session);
        if (swap.table != nullptr) {
            if (auto stepped = session->Step(PATCH_SWAP_TICK - 1); !stepped)
                return std::unexpected(stepped.error());

            if (auto changed = session->ChangeTable(*swap.table, swap.version, "shifted"); !changed)
                return changed;

            if (auto stepped = session->Step(PATCH_TICKS - PATCH_SWAP_TICK); !stepped)
                return std::unexpected(stepped.error());
        } else if (auto stepped = session->Step(PATCH_TICKS - 1); !stepped)
            return std::unexpected(stepped.error());

        if (auto checked = CheckNoMismatch(*session); !checked)
            return checked;

        // --- パッチの間: 止めた所はほとんど燃えない・速めた所はずっと燃える ---
        const uint64_t placed = 8 * MANA_WOOD_CELLULOSE_MICROMOLES;
        const auto burned = [&](sim::LabCellRange range) {
            return placed - RegionTotalOf(session->Gpu(), range, cellulose, shape);
        };
        const uint64_t stoppedDuring = burned(groups.stopped);
        const uint64_t catalyzedDuring = burned(groups.catalyzed);
        const uint64_t amplifiedDuring = burned(groups.amplified);
        const uint64_t plainDuring = burned(groups.plain);
        if (auto stepped = session->Step(PATCH_TICKS); !stepped)
            return std::unexpected(stepped.error());

        if (auto checked = CheckNoMismatch(*session); !checked)
            return checked;

        const uint64_t stoppedAfter = burned(groups.stopped);
        Log(Channel::Gpu, Level::Info,
            "gpu_lab_box_test: 根 {} つの箱でパッチ {} 刻み 燃えたセルロース 止めた {}(終わった後 {})・触媒のまま {}・"
            "速めた(効き方 {:#x}){}・そのまま {} µmol",
            lab::LabRootCount(shape), PATCH_TICKS, stoppedDuring, stoppedAfter, catalyzedDuring, amplifyEffect,
            amplifiedDuring, plainDuring);
        if (CountBoxElements(*session) != elementsBefore)
            return std::unexpected("パッチで元素の数が変わった");

        if (session->Cpu().magicPatches[0] != 0 || session->Gpu().magicPatches[0] != 0)
            return std::unexpected("終わったパッチが列に残る");

        if (catalyzedDuring <= 10 * (stoppedDuring + 1))
            return std::unexpected("止めるパッチが効かない");

        if (stoppedAfter <= 10 * (stoppedDuring + 1))
            return std::unexpected("止めるパッチが終わっても燃え始めない");

        if (amplifiedDuring <= 10 * (plainDuring + 1))
            return std::unexpected("速めるパッチが効かない");

        if (swap.table != nullptr)
            return ReplayPatchRecording(device, queueType, table, swap, shape, session->Recording());

        return {};
    }

    // 転換(T-0359): 450 K の魔素ありの木を 3 か所に置き、1 か所は魔素の燃焼の CO2 を同じ元素の仮の物質(異性体の役)に替え、
    // 1 か所は CO に替えようとする(元素が釣り合わないので CPU も GPU も断る)。毎刻み CPU と GPU がビット一致・元素は保存・
    // 替えた所だけに異性体ができる・終わると列から除く。表は試験の表に仮の物質を足したもの(値は仮。D-006)
    std::expected<void, std::string> RunConvert(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType) {
        constexpr uint32_t PATCH_TICKS = 50;
        const auto table = sim::BakeReactionTable(sim::MakeConversionTestTable());
        if (!table)
            return std::unexpected(table.error());

        auto session = sim::LabSession::Create(device, queueType, *table, MANA_TABLE_VERSION, "convert",
                                               sim::LAB_SINGLE_ROOT_SHAPE);
        if (!session)
            return std::unexpected(session.error());

        const uint32_t cellulose = table->SpeciesId("cellulose");
        const uint32_t dioxide = table->SpeciesId("carbon_dioxide");
        const uint32_t isomer = table->SpeciesId("carbon_dioxide_isomer_test");
        const uint32_t monoxide = table->SpeciesId("carbon_monoxide");
        const uint32_t rule = RuleIdOf(*table, "mana_test_catalyzed_cellulose_combustion");
        if (cellulose == 0 || isomer == 0 || rule == UINT32_MAX)
            return std::unexpected("表に試験の物質か規則が無い");

        uint32_t slot = 0;
        while (slot < table->rules[rule].productCount && table->rules[rule].products[slot] != dioxide)
            ++slot;

        // --- 置く(3 か所とも魔素あり)と転換 2 つ(釣り合う・釣り合わない)---
        std::vector<sim::SpeciesAmount> manaWood = {
            {.species = cellulose, .amount = MANA_WOOD_CELLULOSE_MICROMOLES},
            {.species = table->SpeciesId("oxygen"), .amount = MANA_WOOD_OXYGEN_MICROMOLES},
            {.species = table->SpeciesId("mana_test"), .amount = MANA_MICROMOLES}};
        std::ranges::sort(manaWood, {}, &sim::SpeciesAmount::species);
        const PatchGroups groups;
        const sim::LabCellRange converted = groups.catalyzed;
        const sim::LabCellRange refused = groups.stopped;
        const sim::LabCellRange plain = groups.plain;
        const bool queued = session->PlaceRegion(converted, manaWood, MANA_WARM_MILLIKELVIN) &&
                            session->PlaceRegion(refused, manaWood, MANA_WARM_MILLIKELVIN) &&
                            session->PlaceRegion(plain, manaWood, MANA_WARM_MILLIKELVIN) &&
                            session->ApplyPatch(rule, reaction::MpMakeConvertEffect(slot, isomer), PATCH_TICKS,
                                                converted) &&
                            session->ApplyPatch(rule, reaction::MpMakeConvertEffect(slot, monoxide), PATCH_TICKS,
                                                refused);
        if (!queued)
            return std::unexpected("置けない・パッチを出せない");

        if (auto stepped = session->Step(1); !stepped)
            return std::unexpected(stepped.error());

        if (session->Cpu().magicPatches[0] != 1 || session->Gpu().magicPatches[0] != 1)
            return std::unexpected("釣り合う転換だけが列に入る(釣り合わない転換は断る)");

        const std::vector<sim::NamedElementCount> elementsBefore = CountBoxElements(*session);
        if (auto stepped = session->Step(PATCH_TICKS - 1); !stepped)
            return std::unexpected(stepped.error());

        if (auto checked = CheckNoMismatch(*session); !checked)
            return checked;

        const auto total = [&](sim::LabCellRange range, uint32_t species) {
            return RegionTotalOf(session->Gpu(), range, species, sim::LAB_SINGLE_ROOT_SHAPE);
        };
        const uint64_t convertedIsomer = total(converted, isomer);
        const uint64_t refusedIsomer = total(refused, isomer);
        const uint64_t plainIsomer = total(plain, isomer);
        const uint64_t plainDioxide = total(plain, dioxide);
        if (auto stepped = session->Step(PATCH_TICKS); !stepped)
            return std::unexpected(stepped.error());

        if (auto checked = CheckNoMismatch(*session); !checked)
            return checked;

        Log(Channel::Gpu, Level::Info,
            "gpu_lab_box_test: 転換 {} 刻み 異性体(仮)替えた所 {}・断った所 {}・そのまま {} µmol(そのままの所の CO2 {} "
            "µmol)",
            PATCH_TICKS, convertedIsomer, refusedIsomer, plainIsomer, plainDioxide);
        if (CountBoxElements(*session) != elementsBefore)
            return std::unexpected("転換で元素の数が変わった");

        if (session->Cpu().magicPatches[0] != 0 || session->Gpu().magicPatches[0] != 0)
            return std::unexpected("終わった転換が列に残る");

        if (convertedIsomer == 0 || refusedIsomer != 0 || plainIsomer != 0 || plainDioxide == 0)
            return std::unexpected("転換が替えた所だけに効かない");

        return {};
    }

    // 物質を足す・消す表の差し替えを跨ぐ転換(T-0377): 魔素ありの木を 2 か所に置き、魔素の燃焼の CO2 を 1 か所は異性体(差し替えで
    // ID がずれる)、もう 1 か所は消える物質に替える。燃えている途中で物質の一覧が変わる表に替え、同じ刻みに前の表の ID で
    // 消える物質への転換をもう 1 つ出す。替える先の付け替え(列とまだ当てていないコマンド)が CPU と GPU で毎刻み一致し、
    // 異性体への転換は効き続け、消える物質への転換は効かなくなり(1 つ数える)、後から出した転換は断る
    std::expected<void, std::string> RunConvertRemap(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType) {
        constexpr uint32_t PATCH_TICKS = 80;
        constexpr uint32_t BEFORE_SWAP_TICKS = 30;
        constexpr uint64_t REMAP_TABLE_VERSION = 0x6666;
        const auto before = sim::BakeReactionTable(sim::MakeConversionRemapBeforeTestTable());
        const auto after = sim::BakeReactionTable(sim::MakeConversionRemapAfterTestTable());
        if (!before || !after)
            return std::unexpected(!before ? before.error() : after.error());

        auto session = sim::LabSession::Create(device, queueType, *before, MANA_TABLE_VERSION, "remap",
                                               sim::LAB_SINGLE_ROOT_SHAPE);
        if (!session)
            return std::unexpected(session.error());

        const uint32_t dioxide = before->SpeciesId("carbon_dioxide");
        const uint32_t late = before->SpeciesId("carbon_dioxide_late_test");
        const uint32_t rule = RuleIdOf(*before, "mana_test_catalyzed_cellulose_combustion");
        if (late == 0 || rule == UINT32_MAX)
            return std::unexpected("表に試験の物質か規則が無い");

        uint32_t slot = 0;
        while (slot < before->rules[rule].productCount && before->rules[rule].products[slot] != dioxide)
            ++slot;

        // --- 置く(2 か所とも魔素あり)と転換 2 つ ---
        std::vector<sim::SpeciesAmount> manaWood = {
            {.species = before->SpeciesId("cellulose"), .amount = MANA_WOOD_CELLULOSE_MICROMOLES},
            {.species = before->SpeciesId("oxygen"), .amount = MANA_WOOD_OXYGEN_MICROMOLES},
            {.species = before->SpeciesId("mana_test"), .amount = MANA_MICROMOLES}};
        std::ranges::sort(manaWood, {}, &sim::SpeciesAmount::species);
        const PatchGroups groups;
        const sim::LabCellRange kept = groups.catalyzed;
        const sim::LabCellRange lost = groups.stopped;
        const uint32_t toIsomer = reaction::MpMakeConvertEffect(slot, before->SpeciesId("carbon_dioxide_isomer_test"));
        const uint32_t toLate = reaction::MpMakeConvertEffect(slot, late);
        const bool queued = session->PlaceRegion(kept, manaWood, MANA_WARM_MILLIKELVIN) &&
                            session->PlaceRegion(lost, manaWood, MANA_WARM_MILLIKELVIN) &&
                            session->ApplyPatch(rule, toIsomer, PATCH_TICKS, kept) &&
                            session->ApplyPatch(rule, toLate, PATCH_TICKS, lost);
        if (!queued)
            return std::unexpected("置けない・パッチを出せない");

        if (auto stepped = session->Step(BEFORE_SWAP_TICKS); !stepped)
            return std::unexpected(stepped.error());

        const auto total = [&](sim::LabCellRange range, uint32_t species) {
            return RegionTotalOf(session->Gpu(), range, species, sim::LAB_SINGLE_ROOT_SHAPE);
        };
        const uint64_t lateBefore = total(lost, late);
        const uint64_t isomerBefore = total(kept, before->SpeciesId("carbon_dioxide_isomer_test"));

        // --- 差し替え(同じ刻みに前の表の ID で消える物質への転換をもう 1 つ)---
        const std::vector<sim::NamedElementCount> elementsBefore = CountBoxElements(*session);
        const bool late2 = session->ApplyPatch(rule, toLate, 5, lost);
        if (auto change = session->ChangeTable(*after, REMAP_TABLE_VERSION, "remap"); !late2 || !change)
            return std::unexpected(!late2 ? "パッチを出せない" : change.error());

        if (auto stepped = session->Step(1); !stepped)
            return std::unexpected(stepped.error());

        if (auto checked = CheckNoMismatch(*session); !checked)
            return checked;

        const auto& report = session->LastRemapReport();
        const uint64_t lostAtoms = report ? report->remainderAtomMicromoles + report->overflowAtomMicromoles : 0;
        const auto effectOf = [&](const sim::MultiresNest& nest, uint32_t index) {
            return nest.magicPatches[reaction::MP_LIST_HEADER_WORDS + (index * reaction::MP_PATCH_WORDS) +
                                     reaction::MP_WORD_EFFECT];
        };
        const uint32_t isomerAfter = after->SpeciesId("carbon_dioxide_isomer_test");
        const uint32_t afterSlot = [&] {
            const uint32_t afterRule = RuleIdOf(*after, "mana_test_catalyzed_cellulose_combustion");
            uint32_t found = 0;
            while (found < after->rules[afterRule].productCount &&
                   after->rules[afterRule].products[found] != after->SpeciesId("carbon_dioxide"))
                ++found;

            return found;
        }();
        Log(Channel::Gpu, Level::Info,
            "gpu_lab_box_test: 付け替えを跨ぐ転換: 効かなくなった転換 {}・列 {}(異性体 {} µmol・消える物質 {} µmol)",
            report ? report->conversionsDisabled : 0, session->Gpu().magicPatches[0], isomerBefore, lateBefore);
        if (!report || report->conversionsDisabled != 1 || session->Gpu().magicPatches[0] != 2)
            return std::unexpected("消える物質への転換を 1 つ数え、後から出した転換は断る");

        if (effectOf(session->Gpu(), 0) != reaction::MpMakeConvertEffect(afterSlot, isomerAfter) ||
            effectOf(session->Gpu(), 1) != reaction::MpMakeConvertEffect(afterSlot, 0))
            return std::unexpected("列の転換の替える先が新しい表の ID にならない");

        if (SumAtoms(elementsBefore) != SumAtoms(CountBoxElements(*session)) + lostAtoms)
            return std::unexpected("付け替えで元素が保たれない");

        // --- 異性体への転換の所に新しい表の ID で木を置き直して燃やす(前の木は燃え尽きている)。替える先を付け替えていなければ
        //     前の表の異性体の ID は CO2 を指すので異性体はできない。元素は保たれ、毎刻み一致 ---
        std::vector<sim::SpeciesAmount> manaWoodAfter = {
            {.species = after->SpeciesId("cellulose"), .amount = MANA_WOOD_CELLULOSE_MICROMOLES},
            {.species = after->SpeciesId("oxygen"), .amount = MANA_WOOD_OXYGEN_MICROMOLES},
            {.species = after->SpeciesId("mana_test"), .amount = MANA_MICROMOLES}};
        std::ranges::sort(manaWoodAfter, {}, &sim::SpeciesAmount::species);
        if (!session->PlaceRegion(kept, manaWoodAfter, MANA_WARM_MILLIKELVIN))
            return std::unexpected("置き直せない");

        if (auto stepped = session->Step(1); !stepped)
            return std::unexpected(stepped.error());

        const uint64_t isomerReplaced = total(kept, isomerAfter);
        const std::vector<sim::NamedElementCount> elementsAfter = CountBoxElements(*session);
        if (auto stepped = session->Step(PATCH_TICKS - BEFORE_SWAP_TICKS - 2); !stepped)
            return std::unexpected(stepped.error());

        if (auto checked = CheckNoMismatch(*session); !checked)
            return checked;

        const uint64_t isomerEnd = total(kept, isomerAfter);
        Log(Channel::Gpu, Level::Info, "gpu_lab_box_test: 付け替えを跨ぐ転換: 置き直した後の異性体 {} → {} µmol",
            isomerReplaced, isomerEnd);
        // 置く操作は中身を置き換える(前の異性体は消える)。魔素ありの 450 K の木は 1 刻みで燃え尽きるので、置き直した刻みの異性体が
        // 付け替えた転換の効き目(付け替えないと 0)
        if (lateBefore == 0 || isomerBefore == 0 || isomerReplaced == 0 || isomerEnd < isomerReplaced)
            return std::unexpected("差し替えの前は両方の転換が効き、後も異性体への転換が効き続ける");

        if (CountBoxElements(*session) != elementsAfter)
            return std::unexpected("転換で元素の数が変わった");

        return {};
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_lab_box_test [--warp] [--queue direct|compute]");
            return 2;
        }

        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto session = sim::LabSession::Create(device->Get(), options->queueType, *table, FIRST_TABLE_VERSION, "first");
        if (!session) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED({})", session.error());
            return 1;
        }

        if (auto result = RunFire(*session); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(実験: {})", result.error());
            return 1;
        }

        if (auto result = RunReplay(*session); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(再生: {})", result.error());
            return 1;
        }

        if (auto result = RunTableChange(*session, *table); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(表の差し替え: {})", result.error());
            return 1;
        }

        if (auto result = RunSpeciesChange(device->Get(), options->queueType, *table); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(物質を足す・消す表: {})", result.error());
            return 1;
        }

        if (auto result = RunMana(device->Get(), options->queueType, *table); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(仮の魔素: {})", result.error());
            return 1;
        }

        if (auto result = RunLargeBox(device->Get(), options->queueType, *table); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(根を並べた箱: {})", result.error());
            return 1;
        }

        if (auto result = RunRefinedBox(device->Get(), options->queueType, *table); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(一部を細かくした箱: {})", result.error());
            return 1;
        }

        if (auto result = RunPatch(device->Get(), options->queueType, *table); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(魔法のパッチ: {})", result.error());
            return 1;
        }

        if (auto result = RunPatch(device->Get(), options->queueType, *table, LARGE_SHAPE, LARGE_PATCH_GROUPS);
            !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(根を並べた箱の魔法のパッチ: {})", result.error());
            return 1;
        }

        // --- 活性化エネルギーを下げる触媒(T-0288): 速める所を 2^shift 倍の代わりに触媒で ---
        const uint32_t catalyst = reaction::MpMakeCatalystEffect(PATCH_CATALYST_JOULES_PER_MOL);
        if (auto result = RunPatch(device->Get(), options->queueType, *table, sim::LAB_SINGLE_ROOT_SHAPE, {}, {},
                                   catalyst);
            !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(活性化エネルギーを下げる触媒: {})",
                result.error());
            return 1;
        }

        // --- 生成物を替える転換(T-0359)---
        if (auto result = RunConvert(device->Get(), options->queueType); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(転換: {})", result.error());
            return 1;
        }

        // --- 物質を足す・消す表の差し替えを跨ぐ転換(T-0377)---
        if (auto result = RunConvertRemap(device->Get(), options->queueType); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(付け替えを跨ぐ転換: {})", result.error());
            return 1;
        }

        // --- 表を替える刻みを跨ぐパッチ・保存点(T-0287): 規則の ID がずれる表に途中で替える ---
        const auto shifted = MakeShiftedTable();
        const bool moved = shifted &&
                           RuleIdOf(*shifted, "cellulose_combustion") != RuleIdOf(*table, "cellulose_combustion") &&
                           RuleIdOf(*shifted, "mana_test_catalyzed_cellulose_combustion") !=
                               RuleIdOf(*table, "mana_test_catalyzed_cellulose_combustion");
        if (!moved) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(規則の ID がずれる表を作れない)");
            return 1;
        }

        const PatchSwap swap = {.table = &*shifted, .version = SHIFTED_TABLE_VERSION};
        if (auto result = RunPatch(device->Get(), options->queueType, *table, sim::LAB_SINGLE_ROOT_SHAPE, {}, swap);
            !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(表を替える刻みを跨ぐパッチ: {})", result.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_lab_box_test"))
            return 1;

        Log(Channel::Gpu, Level::Info,
            "gpu_lab_box_test: OK(実験室の箱の GPU と CPU が {} "
            "刻みビット一致・記録から流し直しても同じ・刻みの途中で表を替えても一致・仮の魔素で木が低い温度で燃える・"
            "根を並べた箱でも一致・魔法のパッチで止める / "
            "速める〔根を並べた箱・表を替える刻みを跨いでも。保存点から再生しても同じ〕・活性化エネルギーを下げる触媒・"
            "生成物を替える転換)",
            RUN_TICKS);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
