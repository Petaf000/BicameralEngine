// reaction_species_remap_test.cpp — 物質を足す・消す反応表の差し替えで、セルの物質 ID を名前で付け替える規則(T-0223・ADR-0065)を
// CPU だけで確かめる(GPU の世界の付け替えとのビット一致は gpu_probe_sim_test)。
//
// 確かめること:
//   - 同じ表なら何もしない・物質を足すと後ろの ID がずれ、セルは名前で同じ中身になる(並びもそのまま)
//   - 物質を消すと元素に分けて単体へ戻す: 原子の数が保たれる(単体の端数だけ失い、報告と合う)・熱は減らず 1 mJ 未満しか増えない
//   - 組み立てが変わった同じ名前の物質は「消して足した」扱い・セルの成分の上限に入らない分は報告する・単体の無い元素は当てない
//   - ホットリロードの検査(script::CheckHotReloadCompatible)の 2 つの方針
//   - 世界(ProbeReference): 足すだけの差し替えは、同じ表への差し替えと名前で見て毎刻み同じ・消す差し替えは元素を保ち、その後も進む
// 失敗すると失敗した条件を表示して 1 を返す(ctest が落ちる)。
#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/aliases.h"
#include "script/reaction_hot_reload.h"
#include "sim/probe_sim.h"
#include "sim/reaction_test_table.h"
#include "sim/species_remap.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    constexpr uint64_t SWAP_TICK = 4;  // debug の CPU リファレンスは 1 刻み約 5 秒(全部のセルを毎刻み計算する)
    constexpr uint64_t TOTAL_TICKS = 10;
    constexpr int32_t ROOM_MILLIKELVIN = 300000;
    constexpr uint32_t GAS_CONDUCTIVITY = 18 * 5000;  // 試験の倍率(reaction_test_table.cpp と同じ形)

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 表を作る ---

    SpeciesDefinition Simple(std::string name, std::string element, uint32_t atoms, int64_t formationEnthalpy) {
        return {.name = std::move(name),
                .composition = {{.element = std::move(element), .count = atoms}},
                .formationEnthalpy = formationEnthalpy,
                .heatCapacity = 20786,
                .thermalConductivity = GAS_CONDUCTIVITY};
    }

    // 試験の表 + 水素(H2。どの規則にも出ない)
    ReactionTableDefinition WithHydrogen() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.species.push_back(Simple("hydrogen", "H", 2, 0));

        return definition;
    }

    // セルロースとそれを使う規則を消す
    ReactionTableDefinition WithoutCellulose(ReactionTableDefinition definition) {
        std::erase_if(definition.species, [](const SpeciesDefinition& species) { return species.name == "cellulose"; });
        std::erase_if(definition.rules, [](const RuleDefinition& rule) {
            return rng::any_of(rule.reactants, [](const RuleTerm& term) { return term.species == "cellulose"; });
        });

        return definition;
    }

    // 希ガスを 3 つ足す(成分の上限の試験。セルに 8 種類を入れるため)
    ReactionTableDefinition WithNobleGases(ReactionTableDefinition definition) {
        definition.elements.push_back({.name = "Ar", .atomicMass = 39948});
        definition.elements.push_back({.name = "He", .atomicMass = 4003});
        definition.elements.push_back({.name = "Ne", .atomicMass = 20180});
        definition.species.push_back(Simple("argon", "Ar", 1, 0));
        definition.species.push_back(Simple("helium", "He", 1, 0));
        definition.species.push_back(Simple("neon", "Ne", 1, 0));

        return definition;
    }

    BakedReactionTable Bake(const ReactionTableDefinition& definition) {
        auto baked = BakeReactionTable(definition);
        EXPECT(baked.has_value());
        if (!baked) {
            std::printf("bake: %s\n", baked.error().c_str());
            return {};
        }

        return std::move(*baked);
    }

    // --- セルを名前で見る ---

    struct NamedCell {
        int64_t energy = 0;
        std::vector<std::pair<std::string, uint64_t>> slots;  // セルの並びのまま

        friend bool operator==(const NamedCell&, const NamedCell&) = default;
    };

    NamedCell Named(const BakedReactionTable& table, const reaction::RxCell& cell) {
        NamedCell named{.energy = cell.energy};
        for (uint32_t slot = 0; slot < cell.speciesCount; ++slot)
            named.slots.emplace_back(table.speciesNames[cell.species[slot]], cell.amounts[slot]);

        return named;
    }

    uint64_t AmountOf(const BakedReactionTable& table, const reaction::RxCell& cell, std::string_view name) {
        const uint32_t slot = reaction::RxFindSlot(cell, table.SpeciesId(name));

        return slot == reaction::RX_NO_SLOT ? 0 : cell.amounts[slot];
    }

    uint64_t AtomsOf(const std::vector<NamedElementCount>& counts, std::string_view element) {
        const auto found = rng::find(counts, element, &NamedElementCount::element);

        return found == counts.end() ? 0 : found->atomMicromoles;
    }

    int64_t HeatOf(const BakedReactionTable& table, const reaction::RxCell& cell) {
        return reaction::RxComputeThermal(table.View(), cell).heat;
    }

    reaction::RxCell MakeCell(const BakedReactionTable& table,
                              std::initializer_list<std::pair<std::string_view, uint64_t>> amounts) {
        std::vector<SpeciesAmount> list;
        for (const auto& [name, amount] : amounts)
            list.push_back({.species = table.SpeciesId(name), .amount = amount});

        return MakeReactionCell(table, list, ROOM_MILLIKELVIN);
    }

    // --- セル 1 つの規則 ---

    void TestIdentityAndAddition(const BakedReactionTable& base, const BakedReactionTable& added) {
        const auto same = BuildSpeciesRemap(base, base);
        EXPECT(same.has_value() && same->identity);

        const reaction::RxCell original = MakeCell(base, {{"water_vapor", 7}, {"oxygen", 1000}, {"nitrogen", 3000}});
        reaction::RxCell cell = original;
        if (same)
            EXPECT(RemapReactionCell(*same, base, cell).decomposedMicromoles == 0);
        EXPECT(HashReactionCell(cell) == HashReactionCell(original));

        // 水素を足すと nitrogen・oxygen・water_vapor の ID が 1 つずつ後ろへずれる
        const auto shifted = BuildSpeciesRemap(base, added);
        EXPECT(shifted.has_value() && !shifted->identity);
        EXPECT(added.SpeciesId("oxygen") == base.SpeciesId("oxygen") + 1);
        if (!shifted)
            return;

        const SpeciesRemapReport report = RemapReactionCell(*shifted, added, cell);
        EXPECT(Named(added, cell) == Named(base, original));
        EXPECT(report.decomposedMicromoles == 0 && report.energyDeltaMilliJoules == 0);
    }

    void TestRemoval(const BakedReactionTable& base, const BakedReactionTable& removed) {
        const auto remap = BuildSpeciesRemap(base, removed);
        EXPECT(remap.has_value() && remap->newIds[base.SpeciesId("cellulose")] == 0);
        if (!remap)
            return;

        // C6H10O5 が 1001 µmol: C 6006・H 10010(H2 5005)・O 5005(O2 2502 と端数 1)
        const reaction::RxCell original = MakeCell(base, {{"carbon", 3}, {"cellulose", 1001}, {"oxygen", 500}});
        reaction::RxCell cell = original;
        const SpeciesRemapReport report = RemapReactionCell(*remap, removed, cell);

        EXPECT(AmountOf(removed, cell, "carbon") == 3 + 6006);
        EXPECT(AmountOf(removed, cell, "hydrogen") == 5005);
        EXPECT(AmountOf(removed, cell, "oxygen") == 500 + 2502);
        EXPECT(report.decomposedMicromoles == 1001 && report.remainderAtomMicromoles == 1);
        EXPECT(report.overflowAtomMicromoles == 0);

        const auto before = CountNamedElements(base, original);
        const auto after = CountNamedElements(removed, cell);
        EXPECT(AtomsOf(before, "C") == AtomsOf(after, "C") && AtomsOf(before, "H") == AtomsOf(after, "H"));
        EXPECT(AtomsOf(before, "O") == AtomsOf(after, "O") + report.remainderAtomMicromoles);

        // 熱は保つ(mJ への切り上げで 1 mJ 未満だけ増える)。セルロースを元素に戻すのは強い吸熱なのでエネルギーは増える
        const int64_t heatGain = HeatOf(removed, cell) - HeatOf(base, original);
        EXPECT(heatGain >= 0 && heatGain < 1000);
        EXPECT(report.energyDeltaMilliJoules > 0 && cell.energy == original.energy + report.energyDeltaMilliJoules);
    }

    // 同じ名前で組み立てが変わった(N2 → N): 消して足した扱い。原子の数を保って新しい nitrogen が 2 倍になる
    void TestCompositionChange(const BakedReactionTable& base) {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        for (SpeciesDefinition& species : definition.species) {
            if (species.name == "nitrogen")
                species = Simple("nitrogen", "N", 1, 472680);
        }
        const BakedReactionTable atomic = Bake(definition);

        const auto remap = BuildSpeciesRemap(base, atomic);
        EXPECT(remap.has_value());
        if (!remap)
            return;

        reaction::RxCell cell = MakeCell(base, {{"nitrogen", 400}});
        const SpeciesRemapReport report = RemapReactionCell(*remap, atomic, cell);
        EXPECT(AmountOf(atomic, cell, "nitrogen") == 800 && report.decomposedMicromoles == 400);
    }

    // セルに 8 種類あるとき、分けた先の単体は ID の小さい順に空いた 1 か所へ入り、入らない分は失ったと報告する
    void TestOverflow() {
        const BakedReactionTable full = Bake(WithNobleGases(MakeCombustionTestTable()));
        const BakedReactionTable removed = Bake(WithNobleGases(WithoutCellulose(WithHydrogen())));
        const auto remap = BuildSpeciesRemap(full, removed);
        EXPECT(remap.has_value());
        if (!remap)
            return;

        reaction::RxCell cell = MakeCell(full, {{"cellulose", 100},
                                                {"carbon_dioxide", 1},
                                                {"carbon_monoxide", 1},
                                                {"nitrogen", 1},
                                                {"water_vapor", 1},
                                                {"argon", 1},
                                                {"helium", 1},
                                                {"neon", 1}});
        EXPECT(cell.speciesCount == reaction::RX_MAX_CELL_SPECIES);

        const SpeciesRemapReport report = RemapReactionCell(*remap, removed, cell);
        EXPECT(AmountOf(removed, cell, "carbon") == 600);
        EXPECT(AmountOf(removed, cell, "hydrogen") == 0 && AmountOf(removed, cell, "oxygen") == 0);
        EXPECT(report.overflowAtomMicromoles == 1000 + 500 && report.remainderAtomMicromoles == 0);
    }

    void TestMissingUnit(const BakedReactionTable& base) {
        const BakedReactionTable noHydrogen = Bake(WithoutCellulose(MakeCombustionTestTable()));
        const auto remap = BuildSpeciesRemap(base, noHydrogen);
        EXPECT(!remap.has_value());
        if (!remap)
            EXPECT(remap.error().find("cellulose の H") != std::string::npos);

        // ホットリロードの検査: Reject は物質の一覧が変われば当てない・Remap は分けて戻せる表だけ当てる
        const BakedReactionTable removed = Bake(WithoutCellulose(WithHydrogen()));
        EXPECT(!script::CheckHotReloadCompatible(base, removed).has_value());
        EXPECT(script::CheckHotReloadCompatible(base, removed, script::SpeciesChangePolicy::Remap).has_value());
        EXPECT(!script::CheckHotReloadCompatible(base, noHydrogen, script::SpeciesChangePolicy::Remap).has_value());
        EXPECT(script::CheckHotReloadCompatible(base, base, script::SpeciesChangePolicy::Remap).has_value());
    }

    // --- 世界(CPU リファレンス)---

    std::vector<ProbeCommand> FireCommands() {
        std::vector<ProbeCommand> commands = {MakePokeCommand(0, 0, 28, 32, PROBE_VIEW_Z),
                                              MakePokeCommand(0, 1, 28, 33, PROBE_VIEW_Z),
                                              MakePokeCommand(2, 2, 28, 32, PROBE_VIEW_Z)};
        rng::sort(commands, CommandPrecedes);

        return commands;
    }

    // 刻み SWAP_TICK に swapped へ差し替えて TOTAL_TICKS まで進め、刻みごとのセルを名前で返す(差し替えの後の刻みだけ)
    std::vector<std::vector<NamedCell>> RunNamed(const BakedReactionTable& base, const BakedReactionTable& swapped) {
        const std::vector<ProbeCommand> commands = FireCommands();
        ProbeReference reference(base);
        std::vector<std::vector<NamedCell>> states;
        for (uint64_t tick = 0; tick < TOTAL_TICKS; ++tick) {
            reference.Advance(tick, commands, tick == SWAP_TICK ? &swapped : nullptr);
            if (tick < SWAP_TICK)
                continue;

            std::vector<NamedCell> cells;
            for (const reaction::RxCell& cell : reference.State(tick + 1))
                cells.push_back(Named(swapped, cell));
            states.push_back(std::move(cells));
        }

        return states;
    }

    std::vector<uint64_t> WorldElements(const BakedReactionTable& table, std::span<const reaction::RxCell> cells,
                                        std::span<const std::string_view> elements) {
        std::vector<uint64_t> totals(elements.size(), 0);
        for (const reaction::RxCell& cell : cells) {
            const auto counts = CountNamedElements(table, cell);
            for (size_t index = 0; index < elements.size(); ++index)
                totals[index] += AtomsOf(counts, elements[index]);
        }

        return totals;
    }

    // 消す差し替え: 差し替えの刻みの前後で元素の数が保たれ(O は単体の端数だけ減る)、その後も新しい表で進む
    void RunRemoval(const BakedReactionTable& base, const BakedReactionTable& removed) {
        constexpr std::array<std::string_view, 4> ELEMENTS = {"C", "H", "N", "O"};
        const std::vector<ProbeCommand> commands = FireCommands();
        ProbeReference reference(base);
        for (uint64_t tick = 0; tick < TOTAL_TICKS; ++tick) {
            const bool swap = tick == SWAP_TICK;
            if (!swap) {
                reference.Advance(tick, commands);
                continue;
            }

            const std::vector<uint64_t> before = WorldElements(base, reference.State(tick), ELEMENTS);
            reference.Advance(tick, commands, &removed);

            const SpeciesRemapReport& report = reference.LastRemapReport();
            const std::vector<uint64_t> after = WorldElements(removed, reference.State(tick + 1), ELEMENTS);
            EXPECT(report.decomposedMicromoles > 0 && report.overflowAtomMicromoles == 0);
            EXPECT(before[0] == after[0] && before[1] == after[1] && before[2] == after[2]);
            EXPECT(before[3] == after[3] + report.remainderAtomMicromoles);
            std::printf("removal: decomposed %llu umol, remainder %llu umol O, energy +%lld mJ\n",
                        static_cast<unsigned long long>(report.decomposedMicromoles),
                        static_cast<unsigned long long>(report.remainderAtomMicromoles),
                        static_cast<long long>(report.energyDeltaMilliJoules));
        }
    }

    void TestWorld(const BakedReactionTable& base, const BakedReactionTable& added, const BakedReactionTable& removed) {
        // 足すだけ: 同じ中身の別の表への差し替え(付け替えなし)と、名前で見て毎刻み同じ
        const BakedReactionTable copy = Bake(MakeCombustionTestTable());
        EXPECT(RunNamed(base, copy) == RunNamed(base, added));

        RunRemoval(base, removed);
    }

}  // namespace

int main() {
    const BakedReactionTable base = Bake(MakeCombustionTestTable());
    const BakedReactionTable added = Bake(WithHydrogen());
    const BakedReactionTable removed = Bake(WithoutCellulose(WithHydrogen()));

    TestIdentityAndAddition(base, added);
    TestRemoval(base, removed);
    TestCompositionChange(base);
    TestOverflow();
    TestMissingUnit(base);
    TestWorld(base, added, removed);

    if (failureCount != 0) {
        std::printf("reaction_species_remap_test: %d failure(s)\n", failureCount);
        return 1;
    }

    std::printf("reaction_species_remap_test: OK\n");
    return 0;
}
