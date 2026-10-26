// object_shell_test.cpp — 物の箱が囲む空気の層(1 段目 T-1142: 層 1 段・流れなし / 2 段目 T-1143: 層 k 段 + 流れ〔object_flow_cpu.h〕)で、紙・草・小枝の火が自分の炎で燃え広がるかを確かめる研究の試作
// (T-1142・D-667・D-660・ADR-0469 3-4。CPU だけ。層を作る・返すの口は tests/object_shell_cpu.h、式は shaders/common/object_heat.hlsli)。
// 場面: 0.5 m(または 0.25 m)の空気のセル 1 つの中に、細長い燃える物(骨格 24 × 1 × 3 ボクセル・箱 7.8 mm)を 1 つ。端の列 x = 0 に光(虫眼鏡の代わり)を当て、
//   燃え始めてから 0.5 s で外す。列ごとの燃える固体が 90 % を切った刻みを「火が来た刻み」として、燃え広がりの速さを測る。
// 層は状態だけで作る・返す(D-465。試験の中の口): 骨格のどれかが囲むセルより 100 K 以上熱ければ作り、層の中身が囲むセルとほぼ同じで骨格が冷えたら返す。
// 表: 試験の表(標準の cellulose は「炭 + 水蒸気」だけ)に、熱分解で燃える気体を出す行を足した表(下の AddFuelGas。値と出典は T-1142)。
// 確かめる: 保存(セル + ボクセル + 対の端数 − 入った光。エネルギーと元素。ビット単位)・決定的・眠らせても同じ・層を作って返しても保存。
// 記録: 燃え広がりの速さ(mm/s)・0.5 m と 0.25 m の差・層の一番熱い温度・ボクセルの数と 1 刻みの CPU の時間。
// T-1144: 世界の流れのある場面(object_shell_nest.h)と、箱の細かさ(7.8 mm / 3.9 mm)・層の数・紙の長さを選んで 1 回だけ燃やす口(--case)。
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "object_flow_cpu.h"
#include "object_shell_nest.h"
#include "sun_column_scene.h"

namespace {

    using namespace bicameral;
    namespace rx = bicameral::reaction;
    namespace fx = bicameral::fx;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 試験の値(公開側の試験データ。ゲームの中身ではない。D-006)---
    constexpr int32_t BODY_LEVEL = 6;  // 箱 7.8 mm
    constexpr uint32_t FILM_UM = 3906;
    constexpr uint64_t AIR_OXYGEN_UNITS = 1092000;
    constexpr uint64_t AIR_NITROGEN_UNITS = 4108000;
    constexpr uint32_t DEFAULT_DIFFUSIVITY_MM2_PER_S = 20;
    constexpr uint64_t WORLD_SEED = 0x1142;
    constexpr uint32_t TICK_LIMIT = 60 * 60;
    constexpr uint32_t LENS_AFTER_LIT_TICKS = 30;   // 端の列に火が来てから 0.5 s で光を外す
    constexpr uint32_t LENS_LIMIT_TICKS = 60 * 10;  // 10 s で火が来なくても外す
    constexpr uint32_t QUIET_TICKS = 60 * 10;       // 光を外した後、どの列にも火が来ない刻みがこれだけ続いたら止める
    constexpr int32_t SHELL_MAKE_KELVIN = 100;  // 層を作る: 骨格のどれかが囲むセルより 100 K 以上熱い(仮。試験で選ぶ値)
    constexpr int32_t SHELL_KEEP_KELVIN = 50;   // 層を返す: 骨格が全部 50 K 未満の差で、層の中身がセルとほぼ同じ
    constexpr uint32_t SKELETON_X = 24;
    constexpr uint32_t SKELETON_Z = 3;

    // 燃える物(骨格のボクセル 1 つの cellulose の量はボクセルの単位 = µmol × 8^6。量の数 = 濃さ × 0.125 m³)
    struct Fuel {
        const char* name;
        uint64_t celluloseUnits;
        uint32_t exposedFaces;  // 層の無い時の外に面する面の数(薄い物は 2・細い棒は 4)
        uint32_t sizeY;
        uint32_t sizeZ;
        int64_t lensMilliwatts;  // 端の列の 1 ボクセルに当てる光
    };
    // 紙 0.1 mm(1500 kg/m³)= 1.157e9 × 0.1 / 7.8125・乾いた草 0.3 kg/m²(T-1097 と同じ σ)= 1.85 mol/m² × s² × 8^6・
    // 小枝 7.8 mm 角(木 500 kg/m³ = cellulose の固体の 1/3)
    constexpr std::array<Fuel, 3> FUELS = {Fuel{.name = "紙",
                                                .celluloseUnits = 14809600,
                                                .exposedFaces = 2,
                                                .sizeY = 1,
                                                .sizeZ = SKELETON_Z,
                                                .lensMilliwatts = 2000},
                                           Fuel{.name = "草",
                                                .celluloseUnits = 29598000,
                                                .exposedFaces = 2,
                                                .sizeY = 1,
                                                .sizeZ = SKELETON_Z,
                                                .lensMilliwatts = 2000},
                                           Fuel{.name = "小枝",
                                                .celluloseUnits = 385666667,
                                                .exposedFaces = 4,
                                                .sizeY = 1,
                                                .sizeZ = 1,
                                                .lensMilliwatts = 4000}};

    // 箱の細かさと紙の長さ(T-1144)。レベル L の箱は 7.8 mm × 2^(6−L)。紙・草の面密度・光の面あたりの強さは同じに保つ
    struct Grid {
        int32_t bodyLevel = BODY_LEVEL;
        uint32_t columns = SKELETON_X;  // 骨格の x の数
    };

    // レベル L の燃える物(薄い物は 1 ボクセルの面が 1/4^(L−6)・単位が 8^(L−6) 倍 → 量 × 2^(L−6)・高さ × 2^(L−6)。光はボクセルの面に比例)
    Fuel FuelAt(const Fuel& fuel, const Grid& grid) {
        const auto finer = static_cast<uint32_t>(grid.bodyLevel - BODY_LEVEL);
        Fuel scaled = fuel;
        scaled.sizeZ = fuel.sizeZ << finer;
        scaled.lensMilliwatts = fuel.lensMilliwatts >> (2 * finer);
        if (fuel.exposedFaces == 2) {
            scaled.celluloseUnits = fuel.celluloseUnits << finer;
        } else {
            scaled.sizeY = fuel.sizeY << finer;
        }

        return scaled;
    }

    // --- 表: 熱分解で燃える気体を出す(D-660)。値と出典は T-1142 の作業ログ ---
    // セルロースの熱分解は Bradbury–Sakai–Shafizadeh(1979)の 2 本の枝を、活性セルロースを通さず直接に(600〜900 K では開始の段が 2 本の枝より速い)。
    //   揮発分(levoglucosan): A = 1.9e16 /min = 3.17e14 /s・E = 197.9 kJ/mol / 炭 + 気体: A = 7.9e11 /min = 1.32e10 /s・E = 153.1 kJ/mol
    // levoglucosan(気体): 燃焼熱(固体)−2832 kJ/mol と昇華熱 125.1 kJ/mol から生成エンタルピー −833 kJ/mol(NIST)。比熱は 300 K の推算 171 J/(mol·K)。
    // 混ざる速さ 7 mm²/s(Fuller の式の推算)・伝導率 15 mW/(m·K)(未確認)。気体の燃焼の速さ A = 1.4e9 m³/(mol·s)・E = 125 kJ/mol は**未確認**(試験の中だけ)
    void AddFuelGas(sim::ReactionTableDefinition& definition) {
        for (sim::RuleDefinition& rule : definition.rules) {
            if (rule.name == "cellulose_pyrolysis")
                rule.rate = {.preExponentialMantissa = 132, .preExponentialExponent10 = 8, .activationEnergy = 153100};
        }

        sim::SpeciesDefinition levoglucosan{
            .name = "levoglucosan",
            .composition = {{.element = "C", .count = 6}, {.element = "H", .count = 10}, {.element = "O", .count = 5}},
            .formationEnthalpy = -833000,
            .heatCapacity = 171000,
            .thermalConductivity = 15,
            .phase = sim::SpeciesPhase::Gas};
        levoglucosan.transport.diffusivityMm2PerS = 7;
        definition.species.push_back(levoglucosan);
        definition.rules.push_back(
            {.name = "cellulose_volatilization",
             .reactants = {{.species = "cellulose", .coefficient = 1, .firstOrder = true}},
             .products = {{.species = "levoglucosan", .coefficient = 1}},
             .rate = {.preExponentialMantissa = 317, .preExponentialExponent10 = 12, .activationEnergy = 197900}});
        definition.rules.push_back(
            {.name = "levoglucosan_combustion",
             .reactants = {{.species = "levoglucosan", .coefficient = 1, .firstOrder = true},
                           {.species = "oxygen", .coefficient = 6, .firstOrder = true}},
             .products = {{.species = "carbon_dioxide", .coefficient = 6},
                          {.species = "water_vapor", .coefficient = 5}},
             .rate = {.preExponentialMantissa = 14, .preExponentialExponent10 = 8, .activationEnergy = 125000}});
    }

    // CO を作らない表(記録だけ。T-1143): 骨格の中も流れる比べでは、流れが紙の冷たいボクセルへ全部の気体を少しずつ運び、紙のボクセルの成分が
    // cellulose・炭・levoglucosan・O2・N2・CO2・水・湯気・CO の 9 になってセルの成分の枠 8(RX_MAX_CELL_SPECIES。GPU の溢れ T-0176)を超える。
    // 枠を広げるのは土台なので広げず、その比べだけ CO を作る規則(炭の不完全燃焼・Boudouard ほか)を外した試験の表で回す
    // (骨格を壁にする既定の形では CO ありの表で枠に収まる)
    void AddFuelGasWithoutMonoxide(sim::ReactionTableDefinition& definition) {
        AddFuelGas(definition);
        std::erase_if(definition.rules, [](const sim::RuleDefinition& rule) {
            return std::ranges::any_of(rule.products,
                                       [](const sim::RuleTerm& term) { return term.species == "carbon_monoxide"; });
        });
    }

    struct Tables {
        sim::BakedReactionTable table;
        test::OhTables object;
    };

    enum class TableKind : uint8_t { Plain, FuelGas, FuelGasWithoutMonoxide };

    // 速さの係数 A を 2^splitLog2 で割る(反応を流れの小刻みに分ける研究 T-1186: 1 刻みを 2^splitLog2 個に分けて回す時の、
    // 1/(60 × 2^splitLog2) s あたりの表。A = 仮数 × 10^指数 なので、仮数 × 5^k・指数 − k で割り切れる)
    void DivideRates(sim::ReactionTableDefinition& definition, uint32_t splitLog2) {
        for (sim::RuleDefinition& rule : definition.rules) {
            for (uint32_t k = 0; k < splitLog2; ++k) {
                EXPECT(rule.rate.preExponentialMantissa <= UINT64_MAX / 5);
                rule.rate.preExponentialMantissa *= 5;
                rule.rate.preExponentialExponent10 -= 1;
            }
        }
    }

    Tables MakeTables(TableKind kind, uint32_t splitLog2 = 0) {
        const auto extend = [kind, splitLog2](sim::ReactionTableDefinition& definition) {
            if (kind == TableKind::FuelGas)
                AddFuelGas(definition);
            else if (kind == TableKind::FuelGasWithoutMonoxide)
                AddFuelGasWithoutMonoxide(definition);

            DivideRates(definition, splitLog2);
        };
        sim::BakedReactionTable table = test::MakeSunTable(extend);
        const auto object = test::MakeObjectHeatTables(table, DEFAULT_DIFFUSIVITY_MM2_PER_S);
        EXPECT(object.has_value());
        if (!object)
            std::exit(1);

        return {.table = std::move(table), .object = *object};
    }

    // --- 場面 ---
    test::OhWorld MakeWorld(const sim::BakedReactionTable& table, const Fuel& base, int32_t cellLevel,
                            const Grid& grid) {
        test::OhWorld world;
        const Fuel fuel = FuelAt(base, grid);
        const uint32_t columns = grid.columns;
        const uint32_t count = columns * fuel.sizeY * fuel.sizeZ;
        world.objects.push_back({.voxelBegin = 0,
                                 .voxelCount = count,
                                 .sizeX = columns,
                                 .sizeY = fuel.sizeY,
                                 .cell = 0,
                                 .bodyLevel = grid.bodyLevel,
                                 .cellLevel = cellLevel,
                                 .substeps = 1u << static_cast<uint32_t>(2 * (grid.bodyLevel - BODY_LEVEL)),
                                 .filmUm = FILM_UM,
                                 .shellLayers = 0});
        const std::vector<sim::SpeciesAmount> solid = {
            {.species = table.SpeciesId("cellulose"), .amount = fuel.celluloseUnits},
            {.species = table.SpeciesId("oxygen"), .amount = AIR_OXYGEN_UNITS},
            {.species = table.SpeciesId("nitrogen"), .amount = AIR_NITROGEN_UNITS}};
        const rx::RxCell voxel = sim::MakeReactionCell(table, solid, test::SUN_ROOM_TEMPERATURE_MK);
        // 1 W = 1000 / 60 mJ / 刻み × 8^6(ボクセルの単位)
        const int64_t lens = (fuel.lensMilliwatts * (int64_t{1} << (3 * grid.bodyLevel))) / 60;
        for (uint32_t z = 0; z < fuel.sizeZ; ++z) {
            for (uint32_t y = 0; y < fuel.sizeY; ++y) {
                for (uint32_t x = 0; x < columns; ++x) {
                    const test::OhBoxFace face = test::BoxFaceOf(x, y, z, columns, fuel.sizeY, fuel.sizeZ);
                    world.infos.push_back(
                        {.object = 0, .shape = face.neighbours | (fuel.exposedFaces << rx::OH_EXPOSED_SHIFT)});
                    world.voxels.push_back(voxel);
                    world.light.push_back(x == 0 ? lens : 0);
                }
            }
        }

        const std::vector<sim::SpeciesAmount> air = {
            {.species = table.SpeciesId("oxygen"), .amount = AIR_OXYGEN_UNITS},
            {.species = table.SpeciesId("nitrogen"), .amount = AIR_NITROGEN_UNITS}};
        world.cells = {sim::MakeReactionCell(table, air, test::SUN_ROOM_TEMPERATURE_MK)};
        world.ranges = {{.cell = 0, .objectBegin = 0, .objectCount = 1}};
        world.carries = {rx::OhZeroFlow()};
        test::FinishObjectHeatWorld(world);

        return world;
    }

    // 保存の和: [0] = エネルギー − 入った光、[1..] = 元素(ボクセルの単位)
    std::vector<int64_t> Totals(const sim::BakedReactionTable& table, const test::OhTables& tables,
                                const test::OhWorld& world) {
        const size_t elementCount = table.elementNames.size();
        const int64_t scaleShift = 3 * (world.objects[0].bodyLevel - world.objects[0].cellLevel);
        std::vector<int64_t> totals(1 + elementCount, 0);
        totals[0] = (world.cells[0].energy << scaleShift) - world.sunIn;
        const std::vector<uint64_t> cellCounts = sim::CountElements(table, world.cells[0]);
        for (size_t e = 0; e < elementCount; ++e)
            totals[1 + e] += static_cast<int64_t>(cellCounts[e]) << scaleShift;

        for (const rx::RxCell& voxel : world.voxels) {
            totals[0] += voxel.energy;
            const std::vector<uint64_t> counts = sim::CountElements(table, voxel);
            for (size_t e = 0; e < elementCount; ++e)
                totals[1 + e] += static_cast<int64_t>(counts[e]);
        }

        for (const rx::OhFlow& carry : world.carries) {
            totals[0] += carry.energy;
            for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g) {
                const uint32_t species = tables.gases.species[g];
                for (size_t e = 0; species != 0 && e < elementCount; ++e)
                    totals[1 + e] += carry.amounts[g] * table.speciesElements[(species * elementCount) + e];
            }
        }

        return totals;
    }

    uint64_t Digest(const test::OhWorld& world) {
        uint64_t digest = sim::HashReactionCell(world.cells[0]);
        for (const rx::RxCell& voxel : world.voxels)
            digest = fx::FxHashCombine(digest, sim::HashReactionCell(voxel));

        digest = fx::FxHashCombine(digest, static_cast<uint64_t>(world.carries[0].energy));
        for (const int64_t amount : world.carries[0].amounts)
            digest = fx::FxHashCombine(digest, static_cast<uint64_t>(amount));

        return digest;
    }

    // --- 層を作る・返す条件(状態だけ。D-465)---
    int32_t HottestSkeletonKelvin(const test::OhCpuTable& shared, const test::OhWorld& world) {
        int32_t hottest = 0;
        for (size_t v = 0; v < world.voxels.size(); ++v) {
            if ((world.infos[v].shape & rx::OH_SHELL_BIT) == 0)
                hottest = std::max(hottest, rx::RxComputeThermal(shared, world.voxels[v]).temperature);
        }

        return hottest / 1000;
    }

    // 層のボクセルが全部、囲むセルと同じ(気体の量の差がセルの量の 1/1024 + 16 以内・温度の差が 1 K 以内。仮。D-430 の許容差は後で)
    bool ShellMatchesCell(const test::OhCpuTable& shared, const test::OhGasList& gases, const test::OhWorld& world) {
        const rx::RxCell air = world.cells[0];
        const int32_t airMilliKelvin = rx::RxComputeThermal(shared, air).temperature;
        const rx::OhVoxelGas cellGas = rx::OhPrepareGas(gases, air);
        for (size_t v = 0; v < world.voxels.size(); ++v) {
            if ((world.infos[v].shape & rx::OH_SHELL_BIT) == 0)
                continue;

            if (!rx::OhShellReturnable(shared, world.voxels[v]) ||
                std::abs(rx::RxComputeThermal(shared, world.voxels[v]).temperature - airMilliKelvin) > 1000)
                return false;

            const rx::OhVoxelGas gas = rx::OhPrepareGas(gases, world.voxels[v]);
            for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g) {
                const auto difference = static_cast<int64_t>(gas.amounts[g]) - static_cast<int64_t>(cellGas.amounts[g]);
                if (std::abs(difference) > static_cast<int64_t>((cellGas.amounts[g] >> 10) + 16))
                    return false;
            }
        }

        return true;
    }

    // 骨格の列 x の燃える固体(cellulose)の和
    uint64_t ColumnFuel(const sim::BakedReactionTable& table, const test::OhWorld& world, uint32_t x) {
        const rx::OhObject object = world.objects[0];
        const uint32_t layers = object.shellLayers;
        const uint32_t sizeZ = object.voxelCount / (object.sizeX * object.sizeY);
        const uint32_t cellulose = table.SpeciesId("cellulose");
        uint64_t sum = 0;
        for (uint32_t z = layers; z < sizeZ - layers; ++z) {
            for (uint32_t y = layers; y < object.sizeY - layers; ++y)
                sum += rx::OhAmountOf(world.voxels[x + layers + (object.sizeX * (y + (object.sizeY * z)))], cellulose);
        }

        return sum;
    }

    // 調べ: ボクセルの中身を書き出す
    void DumpVoxel(const sim::BakedReactionTable& table, const test::OhCpuTable& shared, const test::OhWorld& world,
                   size_t v, const char* why, uint32_t tick) {
        const rx::RxCell cell = world.voxels[v];
        std::printf("  [%s] 刻み %u・ボクセル %zu(%s)・%d mK・成分 %u・エネルギー %lld:", why, tick, v,
                    (world.infos[v].shape & rx::OH_SHELL_BIT) != 0 ? "層" : "骨格",
                    rx::RxComputeThermal(shared, cell).temperature, cell.speciesCount,
                    static_cast<long long>(cell.energy));
        for (uint32_t i = 0; i < cell.speciesCount; ++i)
            std::printf(" %s=%llu", table.speciesNames[cell.species[i]].c_str(),
                        static_cast<unsigned long long>(cell.amounts[i]));

        std::printf("\n");
    }

    // --- 流れの代わり(試験の中の口。土台には入れない): 層のボクセルが囲むセルより「量 × 温度」(= 圧力)で多ければ、多い分の気体を
    // その温度の熱と化学のエネルギーごとセルへ出す(熱い気体が膨らんで箱の外へ押し出される流れの代わり。D-667 の 2 段目〔数層と流れ〕の見込みを測るため)。
    // セルへは対の端数を通す(保存はビット単位)。
    bool RelieveShellPressure(const test::OhCpuTable& shared, const test::OhGasList& gases, test::OhWorld& world) {
        const rx::RxCell air = world.cells[0];
        const rx::OhVoxelGas cellGas = rx::OhPrepareGas(gases, air);
        uint64_t cellMoles = 0;
        for (const uint64_t amount : cellGas.amounts)
            cellMoles += amount;

        const auto cellPressure = static_cast<double>(cellMoles) * rx::RxComputeThermal(shared, air).temperature;
        rx::OhFlow out = rx::OhZeroFlow();
        bool ok = true;
        for (size_t v = 0; v < world.voxels.size(); ++v) {
            if ((world.infos[v].shape & rx::OH_SHELL_BIT) == 0)
                continue;

            rx::RxCell voxel = world.voxels[v];
            const int32_t milliKelvin = rx::RxComputeThermal(shared, voxel).temperature;
            const rx::OhVoxelGas gas = rx::OhPrepareGas(gases, voxel);
            uint64_t moles = 0;
            for (const uint64_t amount : gas.amounts)
                moles += amount;

            const double pressure = static_cast<double>(moles) * milliKelvin;
            if (pressure <= cellPressure)
                continue;

            // 出す割合(Q16。試験の中なので浮動小数点で決め、整数にしてから使う)
            const auto shareQ16 = static_cast<uint64_t>(65536.0 * (1.0 - (cellPressure / pressure)));
            for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g) {
                const uint32_t species = gases.GasSpecies(g);
                const auto moved = static_cast<int64_t>((gas.amounts[g] * shareQ16) >> 16);
                if (species == 0 || moved == 0)
                    continue;

                const rx::RxSpecies values = shared.Species(species);
                const int64_t perUnit = (values.h0 * 1000000) + (int64_t{values.heatCapacity} * milliKelvin);
                const int64_t carried = rx::OhMulDivToward0(moved, fx::FxAbsU64(perUnit), 1000000000);
                const int64_t energy = perUnit < 0 ? -carried : carried;
                const rx::OhAmountChange change = rx::OhAddAmount(voxel, species, -moved);
                voxel = change.cell;
                voxel.energy -= energy;
                ok = ok && change.ok != 0;
                out.amounts[g] += moved;
                out.energy += energy;
            }

            world.voxels[v] = voxel;
        }

        return test::ExchangeThroughCarry(gases, world, 0, out) && ok;
    }

    // --- 1 回の燃え ---
    // Flow = 層 k 段 + 流れ(T-1143。object_flow_cpu.h)
    enum class ShellMode : uint8_t { None, OnDemand, OnDemandRelieved, Flow };

    // 圧力の反復の規則(T-1201。既定 = 温めた始まり + 収まったら止める。T-1200 までの形は {24, false, false})
    struct PressureRule {
        uint32_t sweeps = 1024;  // 上限(untilQuiet = false なら決まった回数)
        bool untilQuiet = true;
        bool warmStart = true;
        uint32_t gapSweeps = 0;  // 調べるだけ: 0 から解く時のこの回数と止まった時の面の速さの差
    };

    // 圧力の反復の回数(T-1201。燃え方の場面ごと: 0 始め〔火の来た列が 1/3 まで〕・1 最中・2 消えかけ〔0.5 s 新しい列に火が来ない〕)
    struct SweepStats {
        uint64_t solves = 0;
        uint64_t sweeps = 0;
        uint32_t most = 0;
        uint64_t capped = 0;
    };

    struct Burn {
        std::vector<uint32_t> litTick;  // 列に火が来た刻み(0 = 来ない)
        int32_t bodyLevel = BODY_LEVEL;
        int32_t hottestAboveKelvin = 0;  // 世界の場面: 囲むセルの 1 つ上のセル
        bool objectConserved = true;     // 世界の場面: 物 + 帳簿の物との受け渡し
        bool worldBalanced = true;       // 世界の場面: 世界の Σ − 帳簿
        bool worldStored = true;
        uint32_t ticks = 0;
        uint32_t shellsMade = 0;
        uint32_t shellsReturned = 0;
        int32_t hottestShellKelvin = 0;
        int32_t hottestSkeletonKelvin = 0;
        int32_t hottestCellKelvin = 0;            // 囲むセル(閉じた 1 セル。燃えた熱が全部ここに溜まる)
        uint64_t voxelTicks = 0;                  // 回したボクセルの数の和(費用)
        int64_t fastestMillimetresPerSecond = 0;  // 流れ: 一番速い面
        uint64_t limitedShares = 0;               // 流れ: 出る上限で縮めた(ボクセル × 小刻み)
        std::array<SweepStats, 3> sweepStats{};   // 流れ: 圧力の反復の回数(燃え方の場面ごと。T-1201)
        int64_t gapSpeedQ16 = 0;                  // 流れ: 決まった回数と止まった時の面の速さの差の最大(T-1201)
        int64_t gapReferenceQ16 = 0;
        double cpuMilliseconds = 0;
        bool conserved = true;
        bool failed = false;
        uint64_t digest = 0;
    };

    // 反応を流れの小刻みに分けた 1 刻み(T-1186): 物の熱の小刻み → (反応 → 流れの小刻み substeps ÷ splits)× splits。
    // 刻みの番号は objectTick(= 刻み × splits)から 1 ずつ。物の表は 1 / splits の長さの表(DivideRates)
    bool StepSplit(const test::OhCpuTable& table, const test::OhGasList& gases, test::OhWorld& world,
                   test::OhShellFlow& flow, bool withFlow, bool lensOn, uint64_t objectTick, uint32_t splits) {
        uint32_t heatSubsteps = 1;
        for (const rx::OhObject& object : world.objects)
            heatSubsteps = std::max(heatSubsteps, object.substeps);

        for (uint32_t sub = 0; sub < heatSubsteps; ++sub)
            test::StepObjectHeatSubstep(table, gases, world, lensOn, sub, objectTick);

        const uint32_t substeps = flow.substeps;
        EXPECT(substeps % splits == 0);
        flow.substeps = substeps / splits;
        bool ok = true;
        for (uint32_t r = 0; r < splits; ++r) {
            test::StepObjectHeatReactions(table, world, WORLD_SEED, objectTick + r);
            if (withFlow)
                ok = test::StepShellFlow(table, gases, world, 0, flow, objectTick + r) && ok;
        }

        flow.substeps = substeps;

        return ok;
    }

    // grid: 箱の細かさと紙の長さ・inWorld: 世界の流れのある場面(object_shell_nest.h)に置く(T-1144)
    Burn RunBurn(const Tables& tables, const Fuel& fuel, int32_t cellLevel, ShellMode mode, bool sleeping = true,
                 bool probe = false, uint32_t layers = 1, bool skeletonWalls = true, const Grid& grid = {},
                 bool inWorld = false, const Tables* splitTables = nullptr, uint32_t splitLog2 = 0,
                 const PressureRule& pressure = {}) {
        const test::OhCpuTable shared{.view = tables.table.View(),
                                      .objectSpecies = tables.object.species,
                                      .molarMasses = tables.table.molarMasses};
        // 反応を分ける(T-1186): 物の刻みは 1 刻み = 2^splitLog2 個の小さい刻み(番号 tick × splits + r)。物の反応はその長さの表で
        // splits 回、流れの小刻みを splits 個に分けて反応と交互に回す。世界(囲むセル)は今の表と刻みのまま
        const uint32_t splits = 1u << splitLog2;
        const Tables& objectTables = splitTables != nullptr ? *splitTables : tables;
        const test::OhCpuTable objectShared{.view = objectTables.table.View(),
                                            .objectSpecies = objectTables.object.species,
                                            .molarMasses = objectTables.table.molarMasses};
        // 流れの小刻みは箱が細かいほど多く(1 小刻みに 1 ボクセルの上限が同じ 1.875 m/s になるように)
        test::OhShellFlow flow{.substeps = 4u << static_cast<uint32_t>(grid.bodyLevel - BODY_LEVEL),
                               .pressureSweeps = pressure.sweeps,
                               .pressureUntilQuiet = pressure.untilQuiet,
                               .pressureWarmStart = pressure.warmStart,
                               .gapSweeps = pressure.gapSweeps,
                               .skeletonWalls = skeletonWalls};
        const test::OhGasList& gases = tables.object.gases;
        test::OhWorld world = MakeWorld(tables.table, fuel, cellLevel, grid);
        world.sleeping = sleeping;
        std::unique_ptr<test::object_shell_nest::ShellNest> nest;
        if (inWorld) {
            nest = test::object_shell_nest::MakeShellNest(tables.table, cellLevel, WORLD_SEED);
            test::object_shell_nest::AttachCells(*nest, world);
            const rx::RxCell air = world.cells[0];
            std::printf("  世界の囲むセル(初め): %d mK・成分", rx::RxComputeThermal(shared, air).temperature);
            for (uint32_t i = 0; i < air.speciesCount; ++i)
                std::printf(" %s=%llu", tables.table.speciesNames[air.species[i]].c_str(),
                            static_cast<unsigned long long>(air.amounts[i]));

            std::printf("\n");
        }

        const auto totals = [&] {
            return nest ? test::object_shell_nest::ObjectTotals(tables.table, tables.object, world, *nest)
                        : Totals(tables.table, tables.object, world);
        };
        const std::vector<int64_t> start = totals();
        const uint32_t columns = grid.columns;
        std::vector<uint64_t> initial(columns, 0);
        for (uint32_t x = 0; x < columns; ++x)
            initial[x] = ColumnFuel(tables.table, world, x);

        Burn burn;
        burn.litTick.assign(columns, 0);
        burn.bodyLevel = grid.bodyLevel;
        uint32_t lastLitTick = 0;
        const auto began = std::chrono::steady_clock::now();
        for (uint32_t tick = 1; tick <= TICK_LIMIT; ++tick) {
            const bool lensOn = burn.litTick[0] == 0 ? tick < LENS_LIMIT_TICKS
                                                     : tick < burn.litTick[0] + LENS_AFTER_LIT_TICKS;
            if (nest)
                test::object_shell_nest::BeforeObject(tables.table, *nest, world, WORLD_SEED, tick);

            const uint64_t objectTick = static_cast<uint64_t>(tick) * splits;
            const uint64_t solvesBefore = flow.pressureSolves;
            const uint64_t sweepsBefore = flow.pressureSweepsUsed;
            const uint64_t cappedBefore = flow.pressureCapped;
            flow.pressureMostSweeps = 0;
            if (splits == 1) {
                test::StepObjectHeat(shared, gases, world, lensOn, WORLD_SEED, tick);
                if (mode == ShellMode::Flow)
                    burn.failed = burn.failed || !test::StepShellFlow(shared, gases, world, 0, flow, tick);
            } else {
                burn.failed = burn.failed || !StepSplit(objectShared, gases, world, flow, mode == ShellMode::Flow,
                                                        lensOn, objectTick, splits);
            }

            burn.voxelTicks += world.voxels.size();
            if (flow.pressureSolves != solvesBefore) {
                uint32_t lit = 0;
                for (const uint32_t litAt : burn.litTick)
                    lit += litAt != 0 ? 1u : 0u;

                const size_t phase = lit * 3 < columns ? 0 : (lastLitTick != 0 && tick > lastLitTick + 30 ? 2 : 1);
                SweepStats& stats = burn.sweepStats[phase];
                stats.solves += flow.pressureSolves - solvesBefore;
                stats.sweeps += flow.pressureSweepsUsed - sweepsBefore;
                stats.most = std::max(stats.most, flow.pressureMostSweeps);
                stats.capped += flow.pressureCapped - cappedBefore;
            }

            if (probe) {
                size_t hottestVoxel = 0;
                int32_t hottestMilliKelvin = 0;
                for (size_t v = 0; v < world.voxels.size(); ++v) {
                    const int32_t t = rx::RxComputeThermal(shared, world.voxels[v]).temperature;
                    if (t > hottestMilliKelvin) {
                        hottestMilliKelvin = t;
                        hottestVoxel = v;
                    }
                }

                static bool reported = false;
                if (!reported && (world.failed || hottestMilliKelvin > 3500000)) {
                    reported = true;
                    DumpVoxel(tables.table, shared, world, hottestVoxel, world.failed ? "量が負 / 枠" : "3500 K 超え",
                              tick);
                    for (size_t v = 0; v < world.voxels.size(); ++v) {
                        if (world.voxels[v].speciesCount >= rx::RX_MAX_CELL_SPECIES)
                            DumpVoxel(tables.table, shared, world, v, "成分の枠が一杯", tick);
                    }
                }

                if (tick % 10 == 0 && tick >= 140 && tick <= 260)
                    DumpVoxel(tables.table, shared, world, hottestVoxel, "一番熱い", tick);
            }

            burn.failed = burn.failed || world.failed;
            burn.ticks = tick;

            // 層を作る・返す(刻みの終わり。状態だけ)
            const int32_t airKelvin = rx::RxComputeThermal(shared, world.cells[0]).temperature / 1000;
            burn.hottestCellKelvin = std::max(burn.hottestCellKelvin, airKelvin);
            const int32_t hottest = HottestSkeletonKelvin(shared, world);
            burn.hottestSkeletonKelvin = std::max(burn.hottestSkeletonKelvin, hottest);
            if (mode == ShellMode::OnDemandRelieved && world.objects[0].shellLayers != 0)
                burn.failed = burn.failed || !RelieveShellPressure(shared, gases, world);

            if (mode != ShellMode::None && world.objects[0].shellLayers == 0 &&
                hottest >= airKelvin + SHELL_MAKE_KELVIN) {
                burn.failed = burn.failed ||
                              !test::AddObjectShell(objectShared, gases, world, 0, objectTick + splits - 1, layers);
                test::ResetShellFlow(world, 0, flow);
                ++burn.shellsMade;
            } else if (world.objects[0].shellLayers != 0 && hottest < airKelvin + SHELL_KEEP_KELVIN &&
                       ShellMatchesCell(shared, gases, world)) {
                burn.failed = burn.failed || !test::RemoveObjectShell(shared, gases, world, 0);
                ++burn.shellsReturned;
            }

            for (size_t v = 0; v < world.voxels.size(); ++v) {
                if ((world.infos[v].shape & rx::OH_SHELL_BIT) != 0)
                    burn.hottestShellKelvin = std::max(
                        burn.hottestShellKelvin, rx::RxComputeThermal(shared, world.voxels[v]).temperature / 1000);
            }

            if (nest)
                test::object_shell_nest::AfterObject(tables.table, shared, *nest, world, true, tick);

            // 列に火が来た(燃える固体が 90 % を切った)
            for (uint32_t x = 0; x < columns; ++x) {
                if (burn.litTick[x] == 0 && ColumnFuel(tables.table, world, x) * 10 < initial[x] * 9) {
                    burn.litTick[x] = tick;
                    lastLitTick = tick;
                }
            }

            if (tick % 60 == 0) {
                const bool same = totals() == start;
                if (!same && burn.objectConserved && nest) {
                    const std::vector<int64_t> now = totals();
                    std::printf("  物の保存が崩れた刻み %u:", tick);
                    for (size_t i = 0; i < now.size(); ++i)
                        std::printf(" %lld", static_cast<long long>(now[i] - start[i]));

                    std::printf("\n");
                }

                burn.objectConserved = burn.objectConserved && same;
                burn.conserved = burn.conserved && same;
            }

            if (nest && burn.worldBalanced && !nest->balanced) {
                std::printf("  世界の保存が崩れた刻み %u\n", tick);
                burn.worldBalanced = false;
            }

            if (!lensOn && tick > std::max(lastLitTick, burn.litTick[0]) + QUIET_TICKS)
                break;
        }

        // 終わりに層が残っていたら返す条件を見ずに返して保存を確かめる(返す口の保存)
        if (world.objects[0].shellLayers != 0 && test::RemoveObjectShell(shared, gases, world, 0))
            ++burn.shellsReturned;

        if (nest) {
            test::object_shell_nest::AfterObject(tables.table, shared, *nest, world, true, burn.ticks + 1);
            burn.conserved = burn.conserved && nest->balanced && nest->stored;
            burn.worldBalanced = nest->balanced;
            burn.worldStored = nest->stored;
            burn.hottestAboveKelvin = nest->hottestAboveKelvin;
        }

        // 1 小刻み・1 ボクセル の速さ = 1.875 m/s(7.8 mm・1/240 s。細かい箱も小刻みを同じ割合で増やすので同じ): q(Q16)× 1875 ÷ 65536 mm/s
        burn.fastestMillimetresPerSecond = (flow.fastestQ16 * 1875) / 65536;
        burn.limitedShares = flow.limitedShares;
        burn.gapSpeedQ16 = flow.gapSpeedQ16;
        burn.gapReferenceQ16 = flow.gapReferenceQ16;

        burn.conserved = burn.conserved && totals() == start;
        burn.cpuMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began)
                                   .count();
        burn.digest = Digest(world);

        return burn;
    }

    // 燃え広がりの速さ(列 2 → 火が来た一番遠い列。mm/s。届かなければ 0)
    struct Spread {
        uint32_t farthest = 0;
        double millimetresPerSecond = 0;
    };

    // 測り始めは端から 15.6 mm の列(7.8 mm の箱の列 2。細かい箱でも同じ位置)
    Spread SpreadOf(const Burn& burn) {
        Spread spread;
        for (uint32_t x = 0; x < burn.litTick.size(); ++x) {
            if (burn.litTick[x] != 0)
                spread.farthest = x;
        }

        const auto finer = static_cast<uint32_t>(burn.bodyLevel - BODY_LEVEL);
        const uint32_t begin = 2u << finer;
        if (spread.farthest > begin && burn.litTick[begin] != 0 &&
            burn.litTick[spread.farthest] > burn.litTick[begin]) {
            const double millimetres = (spread.farthest - begin) * 7.8125 / (1u << finer);
            const double seconds = (burn.litTick[spread.farthest] - burn.litTick[begin]) / 60.0;
            spread.millimetresPerSecond = millimetres / seconds;
        }

        return spread;
    }

    void Print(const char* fuel, const char* label, int32_t cellLevel, const Burn& burn) {
        const Spread spread = SpreadOf(burn);
        std::printf(
            "%s・%s・%.2f m: 端に火 %u 刻み・火が来た一番遠い列 %u / %u・速さ %.2f mm/s・層 作った %u 返した %u・"
            "一番熱い 骨格 %d K 層 %d K セル %d K・刻み %u・ボクセル×刻み %llu・CPU %.0f ms・保存 %s%s\n",
            fuel, label, cellLevel == 0 ? 0.5 : 0.25, burn.litTick[0], spread.farthest,
            static_cast<uint32_t>(burn.litTick.size() - 1), spread.millimetresPerSecond, burn.shellsMade,
            burn.shellsReturned, burn.hottestSkeletonKelvin, burn.hottestShellKelvin, burn.hottestCellKelvin,
            burn.ticks, static_cast<unsigned long long>(burn.voxelTicks), burn.cpuMilliseconds,
            burn.conserved ? "0" : "崩れた", burn.failed ? "・量が負 / 枠があふれた" : "");
        if (burn.hottestAboveKelvin != 0)
            std::printf("  世界: 囲むセルの 1 つ上のセルの一番熱い %d K・物 + 帳簿 %s・世界の Σ − 帳簿 %s・書けた %s\n",
                        burn.hottestAboveKelvin, burn.objectConserved ? "0" : "崩れた",
                        burn.worldBalanced ? "0" : "崩れた", burn.worldStored ? "はい" : "いいえ");

        if (burn.fastestMillimetresPerSecond != 0)
            std::printf("  流れ: 一番速い面 %lld mm/s・出る上限で縮めた %llu\n",
                        static_cast<long long>(burn.fastestMillimetresPerSecond),
                        static_cast<unsigned long long>(burn.limitedShares));

        if (burn.sweepStats[0].solves + burn.sweepStats[1].solves + burn.sweepStats[2].solves != 0) {
            std::printf("  圧力の反復(1 回に解く赤黒の平均 / 一番多い / 上限で止まった / 解いた回数):");
            const std::array<const char*, 3> names = {"始め", "最中", "消えかけ"};
            for (size_t phase = 0; phase < names.size(); ++phase) {
                const SweepStats& stats = burn.sweepStats[phase];
                std::printf(
                    " %s %.1f / %u / %llu / %llu", names[phase],
                    stats.solves == 0 ? 0.0 : static_cast<double>(stats.sweeps) / static_cast<double>(stats.solves),
                    stats.most, static_cast<unsigned long long>(stats.capped),
                    static_cast<unsigned long long>(stats.solves));
            }

            std::printf("\n");
        }

        if (burn.gapReferenceQ16 != 0)
            std::printf(
                "  決まった回数で打ち切った時の面の速さの差: 最大 %lld(止まった時の速さの最大 %lld。Q16 ボクセル / "
                "小刻み)\n",
                static_cast<long long>(burn.gapSpeedQ16), static_cast<long long>(burn.gapReferenceQ16));

        std::printf("  列に火が来た刻み:");
        for (const uint32_t tick : burn.litTick)
            std::printf(" %u", tick);

        std::printf("\n");
    }

}  // namespace

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string_view(argv[1]) == "--quick";
    const Tables plain = MakeTables(TableKind::Plain);
    const Tables fuelGas = MakeTables(TableKind::FuelGas);
    const Tables noMonoxide = MakeTables(TableKind::FuelGasWithoutMonoxide);
    // 1 回だけ燃やす(T-1144): --case <燃える物 0 紙 / 1 草> <セルのレベル 0 / 1> <層の数> <箱のレベル 6 / 7> <骨格の x の数>
    //   <場面 0 = 閉じた 1 セル / 1 = 世界の流れ> [表 0 = CO なし / 1 = CO あり]
    if (argc >= 8 && std::string_view(argv[1]) == "--case") {
        const auto fuelIndex = static_cast<size_t>(std::atoi(argv[2]));
        const int32_t cellLevel = std::atoi(argv[3]);
        const auto layers = static_cast<uint32_t>(std::atoi(argv[4]));
        const Grid grid{.bodyLevel = std::atoi(argv[5]), .columns = static_cast<uint32_t>(std::atoi(argv[6]))};
        const bool inWorld = std::atoi(argv[7]) != 0;
        const bool monoxide = argc >= 9 && std::atoi(argv[8]) != 0;
        // [反応を分ける 2 の何乗か](T-1186。0 = 今のまま 1 刻みに 1 度)
        const auto splitLog2 = argc >= 10 ? static_cast<uint32_t>(std::atoi(argv[9])) : 0u;
        // [圧力の赤黒の反復の上限](T-1186 の次の道の調べ。既定 1024)[規則 0 = 0 から決まった回数(T-1200 まで)/ 1 = 0 から収まるまで /
        //  3 = 温めた始まり + 収まるまで(既定)][調べる回数: 0 から解く時、この回数と止まった時の面の速さの差](T-1201)
        PressureRule pressure;
        if (argc >= 11)
            pressure.sweeps = static_cast<uint32_t>(std::atoi(argv[10]));

        if (argc >= 12) {
            const auto rule = static_cast<uint32_t>(std::atoi(argv[11]));
            pressure.untilQuiet = (rule & 1u) != 0;
            pressure.warmStart = (rule & 2u) != 0;
        }

        if (argc >= 13)
            pressure.gapSweeps = static_cast<uint32_t>(std::atoi(argv[12]));

        const uint32_t sweeps = pressure.sweeps;
        const TableKind kind = monoxide ? TableKind::FuelGas : TableKind::FuelGasWithoutMonoxide;
        const Tables split = MakeTables(kind, splitLog2);
        const Burn burn = RunBurn(monoxide ? fuelGas : noMonoxide, FUELS[fuelIndex], cellLevel, ShellMode::Flow, true,
                                  false, layers, true, grid, inWorld, splitLog2 != 0 ? &split : nullptr, splitLog2,
                                  pressure);
        char label[200];
        std::snprintf(label, sizeof(label), "層 %u 段・箱 %.2f mm・骨格 %u 列・%s%s・反応 %u 分け・圧力 上限 %u 回%s%s",
                      layers, 7.8125 / (1 << (grid.bodyLevel - BODY_LEVEL)), grid.columns,
                      inWorld ? "世界の流れ" : "閉じた 1 セル", monoxide ? "・CO あり" : "", 1u << splitLog2, sweeps,
                      pressure.untilQuiet ? "・収まるまで" : "・決まった回数",
                      pressure.warmStart ? "・温めた始まり" : "");
        Print(FUELS[fuelIndex].name, label, cellLevel, burn);

        return burn.conserved && !burn.failed ? 0 : 1;
    }

    // 世界だけの保存の調べ(T-1144): --world-check <セルのレベル>。物なしで 120 刻み、反応と伝導の段・気体の段の後ごとに Σ − 帳簿 を見る
    if (argc >= 3 && std::string_view(argv[1]) == "--world-check") {
        const int32_t level = std::atoi(argv[2]);
        auto nest = test::object_shell_nest::MakeShellNest(noMonoxide.table, level, WORLD_SEED);
        uint32_t brokenAfterNest = 0;
        uint32_t brokenAfterGas = 0;
        for (uint64_t tick = 1; tick <= 120; ++tick) {
            sim::ConservedTotals before = sim::ComputeConservedTotals(nest->nest, noMonoxide.table, level);
            sim::StepNest(nest->nest, noMonoxide.table, WORLD_SEED, tick, {.conduction = true});
            sim::ConservedTotals after = sim::ComputeConservedTotals(nest->nest, noMonoxide.table, level);
            brokenAfterNest += after == before ? 0u : 1u;
            std::vector<uint32_t> changedSlots;
            (void)nest->gas.Step(nest->nest, noMonoxide.table, tick, changedSlots);
            before = after;
            after = sim::ComputeConservedTotals(nest->nest, noMonoxide.table, level);
            brokenAfterGas += after == before ? 0u : 1u;
        }

        std::printf("世界だけ・レベル %d・120 刻み: Σ − 帳簿 が変わった刻み 反応と伝導の段 %u・気体の段 %u\n", level,
                    brokenAfterNest, brokenAfterGas);

        return 0;
    }

    if (argc > 1 && std::string_view(argv[1]) == "--probe") {
        Print("紙", "調べ", 0, RunBurn(noMonoxide, FUELS[0], 0, ShellMode::Flow, true, true, 2));

        return 0;
    }

    // 1. 保存・決定的・眠らせても同じ(紙・層 2 段 + 流れ・燃える気体)
    const Burn first = RunBurn(noMonoxide, FUELS[0], 0, ShellMode::Flow, true, false, 2);
    const Burn again = RunBurn(noMonoxide, FUELS[0], 0, ShellMode::Flow, true, false, 2);
    const Burn awake = RunBurn(noMonoxide, FUELS[0], 0, ShellMode::Flow, false, false, 2);
    std::printf("決定的(2 回で同じ)%s・眠らせても同じ %s・層を作った %u・返した %u\n",
                first.digest == again.digest ? "はい" : "いいえ", first.digest == awake.digest ? "はい" : "いいえ",
                first.shellsMade, first.shellsReturned);
    EXPECT(first.digest == again.digest);
    EXPECT(first.digest == awake.digest);
    EXPECT(first.shellsMade >= 1 && first.shellsReturned >= 1);

    // 2. 燃え広がり(燃える物 × 0.5 m / 0.25 m × 表と層)
    struct Variant {
        const char* label;
        const Tables* tables;
        ShellMode mode;
        bool record;  // 記録だけ(保存を確かめない。流れなしの層は熱がこもって量が負になる = 研究の結果)
        uint32_t layers = 1;
        bool skeletonWalls = true;
    };
    // 流れの比べは CO を作らない表(草は CO ありの表で紙のボクセルの成分が枠 8 を超える。AddFuelGasWithoutMonoxide)。
    // 記録だけ(--quick で飛ばす): 層 3 段(重い)・骨格の中も流れる・CO ありの表
    const std::array<Variant, 9> variants = {
        Variant{"燃える気体(CO なし)+ 層 2 段 + 流れ", &noMonoxide, ShellMode::Flow, false, 2},
        Variant{"燃える気体(CO なし)+ 層 1 段 + 流れ", &noMonoxide, ShellMode::Flow, false, 1},
        Variant{"燃える気体(CO なし)+ 層 3 段 + 流れ", &noMonoxide, ShellMode::Flow, true, 3},
        Variant{"燃える気体(CO あり)+ 層 2 段 + 流れ", &fuelGas, ShellMode::Flow, true, 2},
        Variant{"燃える気体(CO なし)+ 層 2 段 + 流れ・骨格の中も流れる", &noMonoxide, ShellMode::Flow, true, 2, false},
        Variant{"燃える気体 + 層 + 流れの代わり", &fuelGas, ShellMode::OnDemandRelieved, false},
        Variant{"燃える気体 + 層(流れなし)", &fuelGas, ShellMode::OnDemand, true},
        Variant{"燃える気体・層なし", &fuelGas, ShellMode::None, false},
        Variant{"今の表(炭 + 水蒸気)+ 層", &plain, ShellMode::OnDemand, false}};
    for (const Fuel& fuel : FUELS) {
        for (const Variant& variant : variants) {
            if (quick && variant.record)
                continue;

            const Burn coarse = RunBurn(*variant.tables, fuel, 0, variant.mode, true, false, variant.layers,
                                        variant.skeletonWalls);
            const Burn fine = RunBurn(*variant.tables, fuel, 1, variant.mode, true, false, variant.layers,
                                      variant.skeletonWalls);
            Print(fuel.name, variant.label, 0, coarse);
            Print(fuel.name, variant.label, 1, fine);
            const double a = SpreadOf(coarse).millimetresPerSecond;
            const double b = SpreadOf(fine).millimetresPerSecond;
            if (a > 0 && b > 0)
                std::printf("  速さの差(0.5 m と 0.25 m)%.1f %%\n", 100.0 * std::abs(a - b) / a);

            if (variant.record)
                continue;

            EXPECT(coarse.conserved && fine.conserved);
            EXPECT(!coarse.failed && !fine.failed);
        }
    }

    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("object_shell: 全部通った\n");

    return 0;
}
