// phase_layer_test.cpp — セルより小さい層の 2 つのエネルギー(shaders/common/phase_layer.hlsli。D-665・ADR-0486。T-1160)の CPU の試験と測り。
// 1 列のセル(0.5 m 角・横は断熱)を、今の 1 つのエネルギー(heat_conduction.hlsli の HcFaceFlow)と、混ざったセルだけ層と気体の
// 2 つのエネルギーにした形で同じ刻み・同じ反応の核(RxStepCellWait)で回して比べる。反応と混ざり方はセルの段の形(PlBeginReact・PlEndReact・PlRemixCondensedEnergy。T-1211)で呼ぶ。
//
// 確かめること:
//   1. 気体だけ・凝縮相だけのセルの列は、2 つのエネルギーの形でも今とビット単位で同じ(混ざったセルが無ければ道が同じ)
//   2. 保存: 毎刻み、列のエネルギーの和がビット単位で一定。層と気体の熱はどちらも負にならない
//   3. 露: 冷たい石の上のわずかな凝縮相のセルで、層は石の温度へ寄り、セルの空気は 1 つのエネルギーの時より冷えない(T-1110 の外れ)
//   4. 層の温度と気体の温度が分かれる(氷の上の空気は暖かいまま)
//   測り(試験ではなく表示): 氷(400 mol・台の石 500 K)と水たまり(300 mol・石 700 K)が 20 秒に受ける熱と溶けた量を、
//   試験の石(伝導率 2000 W/(m·K))・現実の石(2.8 W/(m·K))・層の側の道(層の高さ / 無し)で並べる(T-1160 のチケットの表)。
// 表とセルの作り方は tests/phase_layer_scene.h(GPU の試験と共有)。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>
#include <vector>

#include "phase_layer_scene.h"

using namespace bicameral;
using namespace bicameral::sim;
namespace rx = bicameral::reaction;
namespace fx = bicameral::fx;

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr uint64_t TICKS = 1200;  // 20 秒
    constexpr uint64_t WORLD_SEED = 11;
    constexpr uint64_t STONE = test::PL_STONE_MICROMOL;
    constexpr uint64_t AIR_NITROGEN = test::PL_AIR_NITROGEN_MICROMOL;
    constexpr uint64_t AIR_OXYGEN = test::PL_AIR_OXYGEN_MICROMOL;
    using Amount = test::PlAmount;

    BakedReactionTable MakeTable() {
        return test::MakePhaseLayerTable();
    }

    int64_t PartEnergyMicrojoules(const BakedReactionTable& table, const std::vector<Amount>& amounts,
                                  uint32_t temperatureMk) {
        return test::PlPartEnergyMicrojoules(table, amounts, temperatureMk);
    }

    rx::RxCell MakeCell(const BakedReactionTable& table, std::vector<Amount> amounts, int64_t energyMj) {
        return test::MakePhaseLayerCell(table, std::move(amounts), energyMj);
    }

    // 列の 1 セル: 凝縮相の部分と気体の部分をそれぞれの温度で(1 つのエネルギーの形では和だけを使う)
    struct CellSpec {
        std::vector<Amount> condensed;
        uint32_t condensedMk = 300000;
        std::vector<Amount> gas;
        uint32_t gasMk = 300000;
    };

    struct ColumnCell {
        rx::RxCell cell;
        int64_t condensedEnergy = 0;  // mJ。混ざったセルの時だけ意味がある
        bool mixed = false;
    };

    struct Column {
        std::vector<ColumnCell> cells;  // 下から
        bool twoEnergies = false;
        uint32_t layerPathQ16 = 0;  // 0 = 層の高さ(既定)。ほかは層の下の面の道の高さ(Q16)
    };

    Column MakeColumn(const BakedReactionTable& table, const std::vector<CellSpec>& specs, bool twoEnergies,
                      uint32_t layerPathQ16) {
        Column column;
        column.twoEnergies = twoEnergies;
        column.layerPathQ16 = layerPathQ16;
        for (const CellSpec& spec : specs) {
            const int64_t condensedUj = PartEnergyMicrojoules(table, spec.condensed, spec.condensedMk);
            const int64_t gasUj = PartEnergyMicrojoules(table, spec.gas, spec.gasMk);
            std::vector<Amount> all = spec.condensed;
            all.insert(all.end(), spec.gas.begin(), spec.gas.end());
            const int64_t condensedMj = condensedUj / 1000;
            const int64_t gasMj = gasUj / 1000;

            ColumnCell cell;
            cell.cell = MakeCell(table, all, condensedMj + gasMj);
            const rx::PlLayer layer = rx::PlLayerOf(table.View(), cell.cell);
            cell.mixed = twoEnergies && layer.mixed != 0;
            cell.condensedEnergy = cell.mixed ? condensedMj : 0;
            column.cells.push_back(cell);
        }

        return column;
    }

    // 面の 2 つの片側(下の面・上の面)
    struct Sides {
        rx::HcThermalCache bottom;
        rx::HcThermalCache top;
    };

    Sides SidesOf(const BakedReactionTable& table, const Column& column, const ColumnCell& cell) {
        if (!cell.mixed) {
            const rx::RxThermal thermal = rx::RxComputeThermal(table.View(), cell.cell);
            const rx::HcThermalCache whole = rx::HcMakeCache(
                thermal, rx::HcCellConductance(table.View(), cell.cell, rx::HcCacheTemperature(thermal)));

            return {.bottom = whole, .top = whole};
        }

        const rx::PlLayer layer = rx::PlLayerOf(table.View(), cell.cell);
        const rx::PlParts parts = rx::PlPartsOf(table.View(), cell.cell, cell.condensedEnergy);
        const uint32_t path = column.layerPathQ16 == 0 ? layer.heightQ16 : column.layerPathQ16;

        return {.bottom = rx::PlBottomSide(parts, path), .top = rx::PlTopSide(parts, layer)};
    }

    // 反応: 混ざったセルは層の温度で(凝縮相の項を持つ規則。この試験の表では気体だけの規則は進まない)
    void React(const BakedReactionTable& table, ColumnCell& cell, uint64_t tick, uint64_t cellId) {
        if (!cell.mixed) {
            cell.cell = rx::RxStepCellWait(table.View(), cell.cell, WORLD_SEED, tick, tick - 1, cellId).cell;
            return;
        }

        // セルの段の形(T-1211。世界の段と同じ関数): 見るセル → 反応の核 → 気体の項の出入りを気体の側へ
        const auto begin = rx::PlBeginReact(table.View(), cell.cell, cell.condensedEnergy);
        const auto step = rx::RxStepCellWait(table.View(), begin.view, WORLD_SEED, tick, tick - 1, cellId);
        const int64_t energy = cell.cell.energy;
        cell.condensedEnergy = rx::PlEndReact(table.View(), begin, step.cell, step.thermal, energy);
        cell.cell = step.cell;
        cell.cell.energy = energy;
    }

    // 混ざり方が変わったセル: 混ざったら温度が揃う配り、混ざらなくなったら 1 つへ
    void Remix(const BakedReactionTable& table, const Column& column, ColumnCell& cell, const rx::RxCell& before) {
        if (!column.twoEnergies)
            return;

        cell.condensedEnergy = rx::PlRemixCondensedEnergy(table.View(), before, cell.cell, cell.condensedEnergy);
        cell.mixed = rx::PlLayerOf(table.View(), cell.cell).mixed != 0;
    }

    bool PartsNonNegative(const BakedReactionTable& table, const ColumnCell& cell) {
        if (!cell.mixed)
            return rx::RxComputeThermal(table.View(), cell.cell).heat >= 0;

        const rx::PlParts parts = rx::PlPartsOf(table.View(), cell.cell, cell.condensedEnergy);

        return parts.condensed.heat >= 0 && parts.gas.heat >= 0;
    }

    // 1 刻み: 伝導(面 → 層の上の面)→ 反応。conserved = 和が変わらなかったか
    bool Step(const BakedReactionTable& table, Column& column, uint64_t tick) {
        const size_t count = column.cells.size();
        int64_t before = 0;
        for (const ColumnCell& cell : column.cells)
            before += cell.cell.energy;

        // --- 面(下のセルの上の側 ⇄ 上のセルの下の側)---
        std::vector<Sides> sides;
        for (const ColumnCell& cell : column.cells)
            sides.push_back(SidesOf(table, column, cell));

        std::vector<int64_t> delta(count, 0);
        std::vector<int64_t> condensedDelta(count, 0);
        for (size_t i = 0; i + 1 < count; ++i) {
            const int64_t flow = rx::HcFaceFlow(sides[i].top, sides[i + 1].bottom);
            delta[i] -= flow;
            delta[i + 1] += flow;
            if (column.cells[i + 1].mixed)
                condensedDelta[i + 1] += flow;
        }

        // --- 層の上の面(層 → 気体)---
        for (size_t i = 0; i < count; ++i) {
            const ColumnCell& cell = column.cells[i];
            if (cell.mixed)
                condensedDelta[i] -= rx::PlInnerFlowOf(table.View(), cell.cell, cell.condensedEnergy);
        }

        for (size_t i = 0; i < count; ++i) {
            column.cells[i].cell.energy += delta[i];
            column.cells[i].condensedEnergy += condensedDelta[i];
        }

        // --- 反応 ---
        bool nonNegative = true;
        for (size_t i = 0; i < count; ++i) {
            const rx::RxCell unreacted = column.cells[i].cell;
            React(table, column.cells[i], tick, i);
            Remix(table, column, column.cells[i], unreacted);
            nonNegative = nonNegative && PartsNonNegative(table, column.cells[i]);
        }

        int64_t after = 0;
        for (const ColumnCell& cell : column.cells)
            after += cell.cell.energy;

        return before == after && nonNegative;
    }

    uint64_t AmountOf(const BakedReactionTable& table, const rx::RxCell& cell, std::string_view name) {
        const uint32_t id = table.SpeciesId(name);
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            if (cell.species[i] == id)
                return cell.amounts[i];
        }

        return 0;
    }

    // 部分の温度(mK。混ざらないセルは全体の温度)
    std::pair<int32_t, int32_t> PartTemperatures(const BakedReactionTable& table, const ColumnCell& cell) {
        if (!cell.mixed) {
            const int32_t whole = rx::RxComputeThermal(table.View(), cell.cell).temperature;
            return {whole, whole};
        }

        const rx::PlParts parts = rx::PlPartsOf(table.View(), cell.cell, cell.condensedEnergy);

        return {parts.condensed.temperature, parts.gas.temperature};
    }

    std::vector<Amount> Air() {
        return {{"nitrogen", AIR_NITROGEN}, {"oxygen", AIR_OXYGEN}};
    }

    // 気体の体積の割合だけ空気を減らす(凝縮相の体積 ÷ セル)
    std::vector<Amount> AirAbove(const BakedReactionTable& table, const std::vector<Amount>& condensed) {
        uint64_t volume = 0;
        for (const Amount& entry : condensed)
            volume += entry.amount * table.View().Species(table.SpeciesId(entry.name)).molarVolume;

        const uint64_t cellVolume = 125000000000000ull;
        const uint64_t open = volume < cellVolume ? cellVolume - volume : 0;
        std::vector<Amount> air = Air();
        for (Amount& entry : air)
            entry.amount = (uint64_t)fx::FxMulDivS64((int64_t)entry.amount, open, cellVolume);

        return air;
    }

    // 石 3 段 + 層のセル + 空気 3 段
    std::vector<CellSpec> LayerScene(const BakedReactionTable& table, std::string_view stone, uint32_t stoneMk,
                                     std::vector<Amount> condensed, uint32_t condensedMk) {
        std::vector<CellSpec> specs;
        for (int i = 0; i < 3; ++i)
            specs.push_back({.condensed = {{stone, STONE}}, .condensedMk = stoneMk});

        specs.push_back(
            {.condensed = condensed, .condensedMk = condensedMk, .gas = AirAbove(table, condensed), .gasMk = 300000});
        for (int i = 0; i < 3; ++i)
            specs.push_back({.gas = Air()});

        return specs;
    }

    struct Outcome {
        bool conserved = true;
        int64_t layerHeatKj = 0;  // 層のセルが下の面から受けた熱(kJ)
        uint64_t remainingMicromol = 0;
        int32_t condensedMk = 0;
        int32_t gasMk = 0;
        int32_t airAboveMk = 0;
    };

    Outcome Run(const BakedReactionTable& table, const std::vector<CellSpec>& specs, bool twoEnergies,
                uint32_t layerPathQ16, std::string_view watched) {
        Column column = MakeColumn(table, specs, twoEnergies, layerPathQ16);
        Outcome outcome;
        int64_t stoneBefore = 0;
        for (size_t i = 0; i < 3; ++i)
            stoneBefore += column.cells[i].cell.energy;

        for (uint64_t tick = 1; tick <= TICKS; ++tick)
            outcome.conserved = Step(table, column, tick) && outcome.conserved;

        int64_t stoneAfter = 0;
        for (size_t i = 0; i < 3; ++i)
            stoneAfter += column.cells[i].cell.energy;

        outcome.layerHeatKj = (stoneBefore - stoneAfter) / 1000000;
        outcome.remainingMicromol = AmountOf(table, column.cells[3].cell, watched);
        const auto [condensedMk, gasMk] = PartTemperatures(table, column.cells[3]);
        outcome.condensedMk = condensedMk;
        outcome.gasMk = gasMk;
        outcome.airAboveMk = PartTemperatures(table, column.cells[4]).second;

        return outcome;
    }

    void Print(const char* label, const Outcome& outcome, uint64_t before) {
        std::printf(
            "  %-34s 石から %6lld kJ・残り %5.1f %%・層 %6.2f K・セルの気体 %6.2f K・上の空気 %6.2f K・保存 %s\n",
            label, (long long)outcome.layerHeatKj, 100.0 * (double)outcome.remainingMicromol / (double)before,
            outcome.condensedMk * 1e-3, outcome.gasMk * 1e-3, outcome.airAboveMk * 1e-3,
            outcome.conserved ? "ok" : "NG");
    }

    // --- 1: 混ざったセルが無ければ今とビット単位で同じ ---
    void TestUnmixedSame(const BakedReactionTable& table) {
        std::printf("混ざったセルの無い列(石 + 空気)\n");
        std::vector<CellSpec> specs;
        for (int i = 0; i < 3; ++i)
            specs.push_back({.condensed = {{"stone_real", STONE}}, .condensedMk = 400000});

        for (int i = 0; i < 4; ++i)
            specs.push_back({.gas = Air(), .gasMk = 280000});

        Column one = MakeColumn(table, specs, false, 0);
        Column two = MakeColumn(table, specs, true, 0);
        bool same = true;
        for (uint64_t tick = 1; tick <= 300; ++tick) {
            EXPECT(Step(table, one, tick));
            EXPECT(Step(table, two, tick));
            for (size_t i = 0; i < one.cells.size(); ++i)
                same = same && rx::RxSameCell(one.cells[i].cell, two.cells[i].cell);
        }

        EXPECT(same);
    }

    // --- 2〜4: 層の場面 ---
    void TestScenes(const BakedReactionTable& table) {
        const std::vector<Amount> ice = {{"ice", 400000000}};
        const std::vector<Amount> puddle = {{"water", 300000000}};
        const std::vector<Amount> dew = {{"water", 1000000}};  // 1 mol(18 g。0.5 m 角に 0.07 mm)

        std::printf("氷 400 mol(270 K)・台の石 500 K・20 秒\n");
        for (const char* stone : {"stone", "stone_real"}) {
            const auto specs = LayerScene(table, stone, 500000, ice, 270000);
            const Outcome one = Run(table, specs, false, 0, "ice");
            const Outcome two = Run(table, specs, true, 0, "ice");
            const Outcome open = Run(table, specs, true, rx::PL_MIN_HEIGHT_Q16, "ice");
            std::printf(" %s\n", stone);
            Print("1 つのエネルギー", one, 400000000);
            Print("2 つ・層の側の道 = 層の高さ", two, 400000000);
            Print("2 つ・層の側の道なし(上の端)", open, 400000000);
            EXPECT(one.conserved && two.conserved && open.conserved);
            EXPECT(two.remainingMicromol <= one.remainingMicromol);
            EXPECT(open.remainingMicromol <= two.remainingMicromol);
            // 4: 層は溶ける温度に留まり、氷の上のセルの空気は暖かいまま(1 つのエネルギーでは氷と同じ温度)
            EXPECT(two.gasMk > two.condensedMk + 10000);
        }

        std::printf("水たまり 300 mol(360 K)・石 700 K・20 秒\n");
        for (const char* stone : {"stone", "stone_real"}) {
            const auto specs = LayerScene(table, stone, 700000, puddle, 360000);
            const Outcome one = Run(table, specs, false, 0, "water");
            const Outcome two = Run(table, specs, true, 0, "water");
            const Outcome open = Run(table, specs, true, rx::PL_MIN_HEIGHT_Q16, "water");
            std::printf(" %s\n", stone);
            Print("1 つのエネルギー", one, 300000000);
            Print("2 つ・層の側の道 = 層の高さ", two, 300000000);
            Print("2 つ・層の側の道なし(上の端)", open, 300000000);
            EXPECT(one.conserved && two.conserved && open.conserved);
            EXPECT(two.layerHeatKj >= one.layerHeatKj);
        }

        // 3: 露(冷たい石 260 K の上の 1 mol の水・空気 300 K)。層は石へ寄り、セルの空気は 1 つの時より冷えない
        std::printf("露 1 mol(300 K)・冷たい石 260 K・20 秒\n");
        const auto specs = LayerScene(table, "stone_real", 260000, dew, 300000);
        const Outcome one = Run(table, specs, false, 0, "water");
        const Outcome two = Run(table, specs, true, 0, "water");
        Print("1 つのエネルギー", one, 1000000);
        Print("2 つのエネルギー", two, 1000000);
        EXPECT(one.conserved && two.conserved);
        EXPECT(two.condensedMk < one.condensedMk);  // 層は石の温度へ速く寄る
        EXPECT(two.gasMk >= one.gasMk);             // セルの空気は空気ごと石に冷やされない
    }

}  // namespace

int main() {
    const BakedReactionTable table = MakeTable();
    TestUnmixedSame(table);
    TestScenes(table);
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("すべて通過\n");
    return 0;
}
