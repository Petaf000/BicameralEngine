// gpu_lab_box_test.cpp — 実験室(sim/lab_session。T-0142)を GPU で走らせる: 箱の真ん中に木を置き(刻み 0)、1 つに火を付けて
// (刻み 3)200 刻み、毎刻み GPU と CPU リファレンスの状態の全部がビット一致すること。燃えたこと(GPU の箱でセルロースが減った)。
// 記録(コマンドの列 + ハッシュの列)を読み書きして初めの箱から流し直すと、同じハッシュの列になること(記録・再生)。
// 反応表を刻みの途中で替える(T-0218・ADR-0055): 次の刻みから新しい表で続き、毎刻みビット一致・替える前の刻みは同じ・替えない時と違う・
// 記録は刻み 0 の表の版と表の印を持ち、途中で替えた記録も古い表だけの記録も再生できる・持っていない表の記録は断る。
// 同じ操作を初めの箱から最新の表で流し直す(RerunWithLatestTable。T-0194 の案 A)も毎刻みビット一致。
// 表の中身(T-0217): 記録は使った表の中身を持ち、持っていない表は中身から足せば(AddTable)再生できる。
// 範囲(T-0220): 初めに箱全体を酸素の多い 200 kPa の空気にする範囲のコマンドと、木の温度を直方体で決めるコマンドも一致する。
// 仮の魔素(T-0225。試験用の仮の値): 450 K の木は魔素が無いと燃えないが、魔素を混ぜると燃える(魔素を触媒にする規則)。
// どちらも毎刻みビット一致し、魔素の量は変わらない。
// 引数は gpu_test_options.h。
#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu_test_options.h"
#include "sim/lab_session.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;

namespace {

    constexpr uint32_t IGNITE_TICK = 3;
    constexpr uint32_t RUN_TICKS = 200;
    constexpr uint32_t IGNITE_MILLIKELVIN = 1500000;
    constexpr uint64_t FIRST_TABLE_VERSION = 0x1111;  // 試験の中だけの版(TableVersion ではない。違えばよい)
    constexpr uint64_t SWAPPED_TABLE_VERSION = 0x2222;
    constexpr uint64_t UNKNOWN_TABLE_VERSION = 0x3333;
    constexpr uint32_t SWAP_RUN_TICKS = 100;  // 表を替えてから刻む数

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

    uint64_t TotalOf(const sim::MultiresNest& nest, uint32_t species) {
        uint64_t total = 0;
        for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
            const reaction::RxCell cell = sim::LabBoxCell(nest, index);
            for (uint32_t i = 0; i < cell.speciesCount; ++i)
                total += cell.species[i] == species ? cell.amounts[i] : 0;
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

        if (auto result = RunMana(device->Get(), options->queueType, *table); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(仮の魔素: {})", result.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_lab_box_test"))
            return 1;

        Log(Channel::Gpu, Level::Info,
            "gpu_lab_box_test: OK(実験室の箱の GPU と CPU が {} "
            "刻みビット一致・記録から流し直しても同じ・刻みの途中で表を替えても一致・仮の魔素で木が低い温度で燃える)",
            RUN_TICKS);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
