// probe_gas_test.cpp — 仮の世界に気体を載せる CPU リファレンス(T-0590・ADR-0256。sim/probe_gas)の試験。
//
// 確かめること:
//   1. 静かな大気: 区画を基準の大気で満たすと、何刻み進めてもセルはビット単位で変わらず、運動量は 0(陽解法・投影とも)
//   2. 燃える木箱: 閉じた区画の底の木箱が燃え、煙(CO2)と熱が上へ運ばれる。気体を流さない世界(伝導だけ)より熱の重心が高い
//   3. 保存: 毎刻み、元素の数・エネルギー(セル + 運動エネルギーのぶん − 帳簿)・運動量(− 帳簿)がビット単位で一定
//   4. 決定性: 2 回の実行で、セルと気体の欄のハッシュが一致(陽解法・投影とも)
//   5. ブランクの世界(基準の大気が無い)では区画を作れない(何も読まずに動く世界は今までどおり。D-455)
//   6. 成分の数の上限なし(T-0591): 地球の空気(N2・O2・Ar・CO2)+ 燃えてできる H2O・CO の 6 種の区画で 2〜4 が成り立つ。
//      気体の段の費用(1 刻みの ms)を 4 種と 6 種で出す
//   7. 定積の比熱(T-0591・ADR-0258): 表が気体の比熱を定圧で書くと、ベイクが c_v = c_p − R を作る(凝縮相はそのまま)
// 表は 1〜4 が試験の表(combustion_test)から CO を作る規則を除いたもの(区画の気体の物質 4 つ〔N2・O2・CO2・H2O〕。T-0590 の形)、
// 6・7 が試験の表のまま + 地球の空気 + 定圧の比熱。
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "reaction_test_table.h"
#include "sim/probe_gas.h"
#include "sim/probe_sim.h"
#include "sim/world_physics.h"
#include "standard_world_physics.h"

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 区画(格子の真ん中の柱。12 × 24 × 12 セル = 6 m × 12 m × 6 m。y が上)と木箱(底の 2 × 2 × 2)---
    constexpr std::array<uint32_t, 3> REGION_MIN = {26, 0, 26};
    constexpr std::array<uint32_t, 3> REGION_SIZE = {12, 24, 12};
    constexpr uint32_t WOOD_MIN = 31;  // x・z
    constexpr uint32_t WOOD_BOTTOM = 1;
    constexpr uint32_t WOOD_SIZE = 2;
    constexpr uint64_t WOOD_AMOUNT = 50000000;     // µmol(セルロース 50 mol ≒ 8 kg/セル)
    constexpr int32_t WOOD_TEMPERATURE = 1000000;  // mK(火がついている)
    constexpr int32_t AIR_TEMPERATURE = 300000;    // mK

    constexpr uint64_t SMOKE_TICKS = 120;  // 2 秒
    constexpr uint64_t SHORT_TICKS = 30;

    // 試験の表から CO を作る規則を除き、基準の大気(N2 79%・O2 21%・300 K・1 気圧)と地球の環境の値を足したもの
    BakedReactionTable MakeTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        std::erase_if(definition.rules, [](const RuleDefinition& rule) {
            return std::ranges::any_of(rule.products,
                                       [](const RuleTerm& term) { return term.species == "carbon_monoxide"; });
        });

        definition.environment = StandardWorldPhysics();
        definition.atmosphere = {
            .points = {{.heightMillimeters = 0,
                        .temperatureMilliKelvin = (uint32_t)AIR_TEMPERATURE,
                        .fractions = {{.species = "nitrogen", .partsPerMillion = 790000},
                                      {.species = "oxygen", .partsPerMillion = 210000}}}},
            .surfacePressurePascals = 101325,
        };

        auto baked = BakeReactionTable(definition);
        if (!baked) {
            std::printf("表を作れない: %s\n", baked.error().c_str());
            std::exit(1);
        }

        return std::move(*baked);
    }

    // 試験の表のまま(CO を作る規則も)+ 地球の乾いた空気(N2・O2・Ar・CO2。300 K・1 気圧)+ 気体の比熱は定圧(T-0591)
    BakedReactionTable MakeEarthAirTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.environment = StandardWorldPhysics();
        definition.atmosphere = {
            .points = {{.heightMillimeters = 0,
                        .temperatureMilliKelvin = (uint32_t)AIR_TEMPERATURE,
                        .fractions = {{.species = "nitrogen", .partsPerMillion = 780840},
                                      {.species = "oxygen", .partsPerMillion = 209460},
                                      {.species = "argon", .partsPerMillion = 9340},
                                      {.species = "carbon_dioxide", .partsPerMillion = 360}}}},
            .surfacePressurePascals = 101325,
        };
        definition.gasHeatCapacityAtConstantPressure = true;

        auto baked = BakeReactionTable(definition);
        if (!baked) {
            std::printf("地球の空気の表を作れない: %s\n", baked.error().c_str());
            std::exit(1);
        }

        return std::move(*baked);
    }

    ProbeGasOptions MakeOptions(ProbeGasPressure pressure) {
        return {.minCell = REGION_MIN, .sizeCells = REGION_SIZE, .pressure = pressure, .randomSeed = 12345};
    }

    bool InWood(uint32_t x, uint32_t y, uint32_t z) {
        return x >= WOOD_MIN && x < WOOD_MIN + WOOD_SIZE && z >= WOOD_MIN && z < WOOD_MIN + WOOD_SIZE &&
               y >= WOOD_BOTTOM && y < WOOD_BOTTOM + WOOD_SIZE;
    }

    // 区画を基準の大気で満たした世界(区画の外は空)。wood なら底に燃える木箱(孔の空気は同じ圧力 = 物質量 × 300 ÷ 1000)
    std::vector<reaction::RxCell> MakeWorld(const BakedReactionTable& table, const ProbeGasField& field, bool wood) {
        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const uint32_t index = field.GridIndex(region);
            const uint32_t x = index % PROBE_GRID_SIZE;
            const uint32_t y = (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE;
            const uint32_t z = index / PROBE_SLICE_CELL_COUNT;
            const reaction::RxCell air = field.ReferenceCell(table, y);
            if (!wood || !InWood(x, y, z)) {
                cells[index] = air;
                continue;
            }

            std::vector<SpeciesAmount> amounts = {{.species = table.SpeciesId("cellulose"), .amount = WOOD_AMOUNT}};
            for (uint32_t i = 0; i < air.speciesCount; ++i)
                amounts.push_back({.species = air.species[i],
                                   .amount = air.amounts[i] * (uint64_t)AIR_TEMPERATURE / (uint64_t)WOOD_TEMPERATURE});

            std::ranges::sort(amounts, {}, &SpeciesAmount::species);
            cells[index] = MakeReactionCell(table, amounts, WOOD_TEMPERATURE);
        }

        return cells;
    }

    // --- 保存の合計 ---
    struct Totals {
        std::vector<uint64_t> elements;
        int64_t energy = 0;                    // セル + 運動エネルギーのぶん − 帳簿
        std::array<int64_t, 3> momentum = {};  // 気体の欄 − 帳簿(格子の軸)

        bool operator==(const Totals&) const = default;
    };

    Totals Sum(const BakedReactionTable& table, std::span<const reaction::RxCell> cells, const ProbeGasField* gas) {
        Totals totals;
        totals.elements.assign(table.elementNames.size(), 0);
        for (const reaction::RxCell& cell : cells) {
            totals.energy += cell.energy;
            const std::vector<uint64_t> elements = CountElements(table, cell);
            for (size_t e = 0; e < elements.size(); ++e)
                totals.elements[e] += elements[e];
        }

        if (gas == nullptr)
            return totals;

        // 帳簿の物質は元素に直して引く(閉じた箱では 0)
        for (size_t species = 0; species < gas->Ledger().amounts.size(); ++species) {
            const int64_t amount = gas->Ledger().amounts[species];
            for (size_t e = 0; e < totals.elements.size(); ++e)
                totals.elements[e] -= (uint64_t)amount *
                                      table.speciesElements[(species * table.elementNames.size()) + e];
        }

        totals.energy += gas->KineticEnergySum() - gas->Ledger().energy;
        constexpr std::array<uint32_t, 3> GRID_AXIS = {0, 2, 1};  // 気体の欄の軸(x・y・上)→ 格子の軸
        for (const std::array<int64_t, 3>& momentum : gas->Momentum()) {
            for (uint32_t axis = 0; axis < 3; ++axis)
                totals.momentum[GRID_AXIS[axis]] += momentum[axis];
        }

        for (uint32_t axis = 0; axis < 3; ++axis)
            totals.momentum[axis] -= gas->Ledger().momentum[axis];

        return totals;
    }

    // --- 見るもの: 物質の重心の高さ・熱い所の重心の高さ(セル。1 K 以上熱い所を熱の差で重み付け)---
    int64_t CentroidMilliCells(std::span<const reaction::RxCell> cells, uint32_t species) {
        uint64_t weight = 0;
        uint64_t moment = 0;
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            const reaction::RxCell& cell = cells[index];
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                if (cell.species[i] != species)
                    continue;

                const uint64_t y = (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE;
                weight += cell.amounts[i] / 1000;
                moment += (cell.amounts[i] / 1000) * (y * 1000 + 500);
            }
        }

        return weight == 0 ? -1 : (int64_t)(moment / weight);
    }

    int64_t HeatCentroidMilliCells(std::span<const reaction::HcThermalCache> caches,
                                   std::span<const reaction::RxCell> cells, const ProbeGasField& field) {
        uint64_t weight = 0;
        uint64_t moment = 0;
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const uint32_t index = field.GridIndex(region);
            if (cells[index].speciesCount == 0)
                continue;

            const int64_t excess = (int64_t)caches[index].temperature - (AIR_TEMPERATURE + 1000);
            if (excess <= 0)
                continue;

            const uint64_t y = (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE;
            weight += (uint64_t)excess;
            moment += (uint64_t)excess * (y * 1000 + 500);
        }

        return weight == 0 ? -1 : (int64_t)(moment / weight);
    }

    uint64_t RunHash(const ProbeReference& reference, uint64_t tick) {
        return reference.StateHash(tick) ^ (reference.GasField() != nullptr ? reference.GasField()->Hash() : 0);
    }

    // --- 1. 静かな大気 ---
    void TestQuietAtmosphere(const BakedReactionTable& table, ProbeGasPressure pressure) {
        const auto field = ProbeGasField::Create(table, MakeOptions(pressure));
        EXPECT(field.has_value());
        if (!field)
            return;

        const std::vector<reaction::RxCell> world = MakeWorld(table, *field, false);
        ProbeReference reference(table, world);
        EXPECT(reference.EnableGas(MakeOptions(pressure)).has_value());
        for (uint64_t tick = 0; tick < 20; ++tick)
            reference.Advance(tick, {});

        const std::span<const reaction::RxCell> state = reference.State(20);
        bool same = true;
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index)
            same = same && state[index].energy == world[index].energy &&
                   HashReactionCell(state[index]) == HashReactionCell(world[index]);

        bool still = true;
        for (const std::array<int64_t, 3>& momentum : reference.GasField()->Momentum())
            still = still && momentum == std::array<int64_t, 3>{0, 0, 0};

        EXPECT(same);
        EXPECT(still);
        std::printf("静かな大気(%s): 20 刻みで変わらない %s・動かない %s\n",
                    pressure == ProbeGasPressure::Explicit ? "陽解法" : "投影", same ? "yes" : "no",
                    still ? "yes" : "no");
    }

    // --- 2〜4. 燃える木箱 ---
    struct FireResult {
        uint64_t hash = 0;
        int64_t heatCentroid = 0;   // ミリセル
        int64_t smokeCentroid = 0;  // CO2 の重心(ミリセル)
        uint64_t smokeTop = 0;      // CO2 が 1 mol 以上ある最も高い層
        uint64_t celluloseLeft = 0;
        bool conserved = true;
        uint32_t skipped = 0;
        uint32_t maxGasSpecies = 0;       // 区画の気体の物質の数の最大
        int64_t advanceMicroseconds = 0;  // 刻みを進めた時間の合計(気体・伝導・反応)
    };

    FireResult RunFire(const BakedReactionTable& table, bool flow, ProbeGasPressure pressure, uint64_t ticks) {
        const auto field = ProbeGasField::Create(table, MakeOptions(pressure));
        EXPECT(field.has_value());
        FireResult result;
        if (!field)
            return result;

        ProbeReference reference(table, MakeWorld(table, *field, true));
        if (flow)
            EXPECT(reference.EnableGas(MakeOptions(pressure)).has_value());

        const Totals initial = Sum(table, reference.State(0), reference.GasField());
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            const auto start = std::chrono::steady_clock::now();
            reference.Advance(tick, {});
            result.advanceMicroseconds += std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - start)
                                              .count();
            result.skipped += reference.LastGasReport().skippedCellOverflow ? 1 : 0;
            result.maxGasSpecies = std::max(result.maxGasSpecies, reference.LastGasReport().gasSpecies);
            if (Sum(table, reference.State(tick + 1), reference.GasField()) != initial)
                result.conserved = false;
        }

        const std::span<const reaction::RxCell> state = reference.State(ticks);
        result.hash = RunHash(reference, ticks);
        result.heatCentroid = HeatCentroidMilliCells(reference.Caches(ticks), state, *field);
        const uint32_t carbonDioxide = table.SpeciesId("carbon_dioxide");
        result.smokeCentroid = CentroidMilliCells(state, carbonDioxide);
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            for (uint32_t i = 0; i < state[index].speciesCount; ++i) {
                if (state[index].species[i] == carbonDioxide && state[index].amounts[i] >= 1000000)
                    result.smokeTop = std::max<uint64_t>(result.smokeTop, (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE);

                if (state[index].species[i] == table.SpeciesId("cellulose"))
                    result.celluloseLeft += state[index].amounts[i];
            }
        }

        return result;
    }

    void TestSmokeRises(const BakedReactionTable& table) {
        const FireResult still = RunFire(table, false, ProbeGasPressure::Explicit, SMOKE_TICKS);
        const FireResult flow = RunFire(table, true, ProbeGasPressure::Explicit, SMOKE_TICKS);
        std::printf(
            "燃える木箱 2 秒(陽解法): 熱の重心 伝導だけ %lld → 気体あり %lld ミリセル・CO2 の重心 %lld → %lld・"
            "CO2 1 mol 以上の最も高い層 %llu → %llu・残りのセルロース %llu → %llu µmol・保存 %s・流さなかった刻み %u\n",
            (long long)still.heatCentroid, (long long)flow.heatCentroid, (long long)still.smokeCentroid,
            (long long)flow.smokeCentroid, (unsigned long long)still.smokeTop, (unsigned long long)flow.smokeTop,
            (unsigned long long)still.celluloseLeft, (unsigned long long)flow.celluloseLeft,
            flow.conserved ? "yes" : "no", flow.skipped);

        EXPECT(still.conserved);
        EXPECT(flow.conserved);
        EXPECT(flow.skipped == 0);
        EXPECT(flow.heatCentroid > still.heatCentroid + 1000);          // 熱が 1 セル以上高く運ばれる
        EXPECT(flow.smokeCentroid > (WOOD_BOTTOM + WOOD_SIZE) * 1000);  // 煙の重心が木箱の上
        EXPECT(flow.smokeTop > still.smokeTop);
        std::printf("費用(気体 %u 種): 1 刻み 伝導と反応だけ %.2f ms → 気体あり %.2f ms(区画 %u セル・release)\n",
                    flow.maxGasSpecies, (double)still.advanceMicroseconds / 1000.0 / (double)SMOKE_TICKS,
                    (double)flow.advanceMicroseconds / 1000.0 / (double)SMOKE_TICKS,
                    REGION_SIZE[0] * REGION_SIZE[1] * REGION_SIZE[2]);
    }

    // --- 6. 成分の数の上限なし(T-0591)---
    void TestManySpecies(const BakedReactionTable& table) {
        const FireResult still = RunFire(table, false, ProbeGasPressure::Explicit, SMOKE_TICKS);
        const FireResult flow = RunFire(table, true, ProbeGasPressure::Explicit, SMOKE_TICKS);
        std::printf(
            "6 種の空気の燃える木箱 2 秒(陽解法): 区画の気体の物質 最大 %u 種・熱の重心 %lld → %lld ミリセル・"
            "CO2 の重心 %lld → %lld・保存 %s・流さなかった刻み %u\n",
            flow.maxGasSpecies, (long long)still.heatCentroid, (long long)flow.heatCentroid,
            (long long)still.smokeCentroid, (long long)flow.smokeCentroid, flow.conserved ? "yes" : "no", flow.skipped);
        std::printf("費用(気体 %u 種): 1 刻み 伝導と反応だけ %.2f ms → 気体あり %.2f ms\n", flow.maxGasSpecies,
                    (double)still.advanceMicroseconds / 1000.0 / (double)SMOKE_TICKS,
                    (double)flow.advanceMicroseconds / 1000.0 / (double)SMOKE_TICKS);

        EXPECT(flow.maxGasSpecies >= 5);
        EXPECT(still.conserved);
        EXPECT(flow.conserved);
        EXPECT(flow.skipped == 0);
        EXPECT(flow.heatCentroid > still.heatCentroid + 1000);
        EXPECT(flow.smokeCentroid > (WOOD_BOTTOM + WOOD_SIZE) * 1000);

        for (const ProbeGasPressure pressure : {ProbeGasPressure::Explicit, ProbeGasPressure::Projection}) {
            const FireResult first = RunFire(table, true, pressure, SHORT_TICKS);
            const FireResult second = RunFire(table, true, pressure, SHORT_TICKS);
            std::printf("6 種の決定性(%s・%llu 刻み): 最大 %u 種・ハッシュ %016llx / %016llx・保存 %s\n",
                        pressure == ProbeGasPressure::Explicit ? "陽解法" : "投影", (unsigned long long)SHORT_TICKS,
                        first.maxGasSpecies, (unsigned long long)first.hash, (unsigned long long)second.hash,
                        first.conserved ? "yes" : "no");
            EXPECT(first.maxGasSpecies >= 5);
            EXPECT(first.hash == second.hash);
            EXPECT(first.conserved);
            EXPECT(first.skipped == 0);
        }
    }

    // --- 7. 定積の比熱(T-0591・ADR-0258)---
    void TestHeatCapacityAtConstantVolume(const BakedReactionTable& constantVolume, const BakedReactionTable& earth) {
        // 書いた値(NIST-JANAF の c_p)。N2 29124 → 20810・Ar 20786 → 12472・セルロース(凝縮相)はそのまま
        const auto capacity = [](const BakedReactionTable& table, const char* name) {
            return table.species[table.SpeciesId(name)].heatCapacity;
        };

        EXPECT(capacity(constantVolume, "nitrogen") == 29124);
        EXPECT(capacity(earth, "nitrogen") == 29124 - 8314);
        EXPECT(capacity(earth, "argon") == 20786 - 8314);
        EXPECT(capacity(earth, "cellulose") == capacity(constantVolume, "cellulose"));
        EXPECT(capacity(earth, "carbon") == capacity(constantVolume, "carbon"));

        // 298.15 K の内部エネルギー = 生成エンタルピー − RT(H0 は書いた c_p から作る)
        const int64_t h0Nitrogen = earth.species[earth.SpeciesId("nitrogen")].h0;
        EXPECT(h0Nitrogen == constantVolume.species[constantVolume.SpeciesId("nitrogen")].h0);

        // 気体の c_p が R 以下なら断る(凝縮相の書き忘れの多く)
        ReactionTableDefinition broken = MakeCombustionTestTable();
        broken.gasHeatCapacityAtConstantPressure = true;
        for (SpeciesDefinition& species : broken.species) {
            if (species.name == "nitrogen")
                species.heatCapacity = 8000;
        }

        const auto refused = BakeReactionTable(broken);
        EXPECT(!refused.has_value());
        std::printf("定積の比熱: N2 %u → %u・Ar → %u mJ/(mol·K)・R 以下は断る %s\n",
                    capacity(constantVolume, "nitrogen"), capacity(earth, "nitrogen"), capacity(earth, "argon"),
                    refused ? "no" : "yes");
    }

    void TestDeterminism(const BakedReactionTable& table, ProbeGasPressure pressure) {
        const FireResult first = RunFire(table, true, pressure, SHORT_TICKS);
        const FireResult second = RunFire(table, true, pressure, SHORT_TICKS);
        std::printf("決定性(%s・%llu 刻み): ハッシュ %016llx / %016llx・保存 %s\n",
                    pressure == ProbeGasPressure::Explicit ? "陽解法" : "投影", (unsigned long long)SHORT_TICKS,
                    (unsigned long long)first.hash, (unsigned long long)second.hash, first.conserved ? "yes" : "no");
        EXPECT(first.hash == second.hash);
        EXPECT(first.conserved);
        EXPECT(first.skipped == 0);
    }

    // --- 5. ブランクの世界 ---
    void TestBlankWorldHasNoGas() {
        const auto blank = BakeReactionTable({});
        EXPECT(blank.has_value());
        if (!blank)
            return;

        EXPECT(!ProbeGasField::Create(*blank, MakeOptions(ProbeGasPressure::Explicit)).has_value());
        EXPECT(!ProbeGasField::Create(*blank, {}).has_value());  // 区画が空
    }

}  // namespace

int main() {
    const BakedReactionTable table = MakeTable();
    TestBlankWorldHasNoGas();
    TestQuietAtmosphere(table, ProbeGasPressure::Explicit);
    TestQuietAtmosphere(table, ProbeGasPressure::Projection);
    TestDeterminism(table, ProbeGasPressure::Explicit);
    TestDeterminism(table, ProbeGasPressure::Projection);
    TestSmokeRises(table);

    const BakedReactionTable earth = MakeEarthAirTable();
    TestHeatCapacityAtConstantVolume(table, earth);
    TestManySpecies(earth);

    std::printf("probe_gas_test: %s\n", failureCount == 0 ? "OK" : "FAILED");
    return failureCount == 0 ? 0 : 1;
}
