// grass_wake_test.cpp — セルに混ぜた小さい燃える物(草)を、当たった熱が集まった所だけ物として起こして物の熱で燃やす研究の試作(T-1097・D-645。CPU だけ)。
// 試験の中の仮の口(土台は変えない。CLAUDE.md §1.9)。物の熱は tests/object_heat_cpu.h(共有の式)。
// 場面: 地面のすぐ上の空気のセルに、乾いた草を面密度 σ(0.3 kg/m² の cellulose)で混ぜる(セルの量 = σ × セルの上の面の広さ。セルの細かさで濃さは同じ)。
//   虫眼鏡の焦点の代わりに、2 mm 角の升 3 × 3 に 4 W(真ん中 2.4・辺 0.3 × 4・角 0.1 × 4)を当てる(レンズの升は世界の µm の格子で、セルの細かさによらない)。
// 1 本目(当たる熱の流れで起こす): 升の熱の流れ(W/m²)が Q_WAKE(着火の温度で薄い草が両面から失う熱の流れの目安 10 kW/m²)以上の升を覆う
//   箱 7.8 mm のボクセル(薄い物・両面が外)を、物として起こす。量 = ボクセルの面の広さ × σ(物理量。セルの細かさによらない)、
//   空気はセルと同じ濃さ、温度はセルの温度。セルからは同じ量を引き、粗い側の 1 単位に満たない分は対の端数へ(保存はビット単位)。
// 燃え広がり(T-1097 の続き): 起きた升は 1 ボクセルの物。床の面(升どうしの横の面)の口は物の中のボクセルどうしと同じ式(伝導 + 放射)で、
//   まだセルの中の升は「セルの草の濃さ・セルの温度の升」として同じ式で受ける(受けた熱はセルへ)。升が受ける熱の流れが Q_WAKE 以上になったら起こす
//   (焦点と同じ判定。一つの芯)。燃え始めてから 0.5 s でレンズを外し、火だけで広がるかを見る。
// 確かめる: 起きる・着火する・0.5 m と 0.25 m のセルで着火の時刻と燃え広がりの速さが揃う・保存。比べ: 床の面を伝導だけにする・起こさずにセルへ当てる。
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "object_heat_cpu.h"
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
    constexpr int64_t CELL_UM = 500000;
    constexpr uint32_t FILM_UM = 3906;
    constexpr uint64_t GRASS_MICROMOLES_PER_M2 = 1850000;  // 乾いた草 0.3 kg/m²(cellulose 162 g/mol)
    constexpr uint64_t AIR_OXYGEN_UNITS = 1092000;
    constexpr uint64_t AIR_NITROGEN_UNITS = 4108000;
    constexpr uint32_t DEFAULT_DIFFUSIVITY_MM2_PER_S = 20;
    constexpr uint64_t WORLD_SEED = 0x1097;
    constexpr uint32_t TICKS = 60 * 40;
    constexpr uint64_t Q_WAKE_WATTS_PER_M2 = 10000;
    constexpr int64_t SPOT_UM = 2000;  // レンズの升(LN_SPOT_UM と同じ 2 mm)
    // 升 3 × 3 の力(mW。真ん中・辺・角)
    constexpr std::array<int64_t, 9> SPOT_MILLIWATTS = {100, 300, 100, 300, 2400, 300, 100, 300, 100};
    constexpr int64_t IGNITION_MILLIWATTS = 500;
    constexpr int64_t BURNING_MILLIWATTS = 100;

    sim::BakedReactionTable MakeTable() {
        // 伝導率は表の現実の値のまま(試験の倍率は T-1066 で表から外した。ここで割っていたのを外す)
        return test::MakeSunTable();
    }

    // セルの量(レベル cellLevel の単位 = µmol × 8^cellLevel)の草と空気のセル
    rx::RxCell MakeGrassCell(const sim::BakedReactionTable& table, int32_t cellLevel) {
        const int64_t cellUm = CELL_UM >> cellLevel;
        // σ × セルの上の面(µm² → m² は ÷ 10^12)× 8^L
        const uint64_t grass = (GRASS_MICROMOLES_PER_M2 * static_cast<uint64_t>(cellUm * cellUm) / 1000000000000ull)
                               << (3 * cellLevel);
        const std::vector<sim::SpeciesAmount> amounts = {
            {.species = table.SpeciesId("cellulose"), .amount = grass},
            {.species = table.SpeciesId("oxygen"), .amount = AIR_OXYGEN_UNITS},
            {.species = table.SpeciesId("nitrogen"), .amount = AIR_NITROGEN_UNITS}};

        return sim::MakeReactionCell(table, amounts, test::SUN_ROOM_TEMPERATURE_MK);
    }

    // --- 床(草の層)の升: 物の箱のボクセルと同じ大きさの升の格子。焦点の升 = (0, 0) ---
    // 起きた升は 1 ボクセルの物(試験の中の仮の形。物を育てる口が土台に無いため。T-1097「土台に足りないもの」)。
    // 床の面(升どうしの横の面 s²)の口は、物の中のボクセルどうしと同じ式: 伝導(OhInnerConduction)+ 放射(放射の家族の対の式
    // α_A·α_B·σ(T_A⁴ − T_B⁴)·s²。RdPairCoefficient を s² の面に)。まだセルの中の升は「セルの草の濃さ・セルの温度の升」として同じ式で受け、
    // 受けた熱はセルへ入る(対の端数を通す)。升が受ける熱の流れ(W/m²)が Q_WAKE 以上になったら、その升を起こす(焦点の光と同じ判定)。

    constexpr int32_t BED_RADIUS = 12;  // 升の格子の半分の幅(± 12 升 = ± 9.4 cm。0.25 m のセルの中に収まる)
    constexpr int32_t BED_SIDE = (2 * BED_RADIUS) + 1;
    constexpr uint32_t BED_SHARE_Q16 = 16384;  // 床の面 1 つの放射の行き過ぎの分け前(1/4。4 面で 1)

    struct Bed {
        test::OhWorld world;
        std::vector<int32_t> objects;         // 升 → 物の番号(−1 = まだセルの中)
        std::vector<uint32_t> ignitionTicks;  // 升 → 燃え始めた刻み(0 = まだ)
        std::vector<int64_t> arriving;        // 升 → この刻みに床の面から受けた熱(ボクセルの単位。まだセルの中の升だけ)
        int64_t scaleShift = 0;
        uint64_t movedGrass = 0;  // セルから物へ移した草(セルの単位)
        uint32_t wokenSpots = 0;  // 焦点で Q_WAKE 以上の升(レンズの 2 mm の升)
        bool moved = true;
    };

    size_t BedIndex(int32_t x, int32_t z) {
        return static_cast<size_t>((x + BED_RADIUS) + (BED_SIDE * (z + BED_RADIUS)));
    }

    bool InBed(int32_t x, int32_t z) {
        return std::abs(x) <= BED_RADIUS && std::abs(z) <= BED_RADIUS;
    }

    // 升 (i, j)(0〜2。焦点の真ん中が升 (1, 1) の真ん中)の真ん中の µm(焦点の真ん中 = 0)
    int64_t SpotCentreUm(uint32_t i) {
        return (static_cast<int64_t>(i) - 1) * SPOT_UM;
    }

    // µm(焦点の真ん中 = 0)→ 床の升の座標(焦点の真ん中を升の真ん中に置く)
    int32_t PatchOf(int64_t um) {
        const int64_t voxelUm = CELL_UM >> BODY_LEVEL;

        return static_cast<int32_t>(((um + (voxelUm / 2) + (voxelUm * 64)) / voxelUm) - 64);
    }

    // 床の升 1 つの中身: 草 = σ × 升の面の広さ × 8^6(セルの粗い単位の倍数に切る)・空気はセルと同じ濃さ・温度はセルの温度
    // (まだセルの中の升の値もこれ。セルの残りの草は残りの面に σ で均してある)
    rx::RxCell MakePatchVoxel(const sim::BakedReactionTable& table, const rx::RxCell& cell, int64_t scaleShift) {
        const int64_t voxelUm = CELL_UM >> BODY_LEVEL;
        const uint64_t grass = ((GRASS_MICROMOLES_PER_M2 * static_cast<uint64_t>(voxelUm * voxelUm) / 1000000000000ull)
                                    << (3 * BODY_LEVEL) >>
                                scaleShift)
                               << scaleShift;
        const std::vector<sim::SpeciesAmount> amounts = {
            {.species = table.SpeciesId("cellulose"), .amount = grass},
            {.species = table.SpeciesId("oxygen"), .amount = rx::OhAmountOf(cell, table.SpeciesId("oxygen"))},
            {.species = table.SpeciesId("nitrogen"), .amount = rx::OhAmountOf(cell, table.SpeciesId("nitrogen"))}};

        return sim::MakeReactionCell(table, amounts, rx::RxComputeThermal(table.View(), cell).temperature);
    }

    // 升を起こす: セルから中身を引き(粗い単位。1 単位に満たない分は物の対の端数を負に)、1 ボクセルの物として足す
    void WakePatch(const sim::BakedReactionTable& table, const test::OhTables& tables, Bed& bed, int32_t x, int32_t z,
                   int32_t cellLevel, uint64_t tick) {
        test::OhWorld& world = bed.world;
        rx::RxCell& cell = world.cells[0];
        const rx::RxCell voxel = MakePatchVoxel(table, cell, bed.scaleShift);
        const uint32_t cellulose = table.SpeciesId("cellulose");

        // --- セルから引く(保存: セル × 8^Δ + ボクセル + 端数 = 一定)---
        rx::OhFlow carry = rx::OhZeroFlow();
        const uint64_t grass = rx::OhAmountOf(voxel, cellulose) >> bed.scaleShift;
        bed.movedGrass += grass;
        rx::OhAmountChange change = rx::OhAddAmount(cell, cellulose, -static_cast<int64_t>(grass));
        bed.moved = bed.moved && change.ok != 0;
        cell = change.cell;
        for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g) {
            const uint32_t species = tables.gases.species[g];
            const auto total = static_cast<int64_t>(rx::OhAmountOf(voxel, species));
            if (species == 0 || total == 0)
                continue;

            const int64_t coarse = total >> bed.scaleShift;
            change = rx::OhAddAmount(cell, species, -coarse);
            bed.moved = bed.moved && change.ok != 0;
            cell = change.cell;
            carry.amounts[g] = -(total - (coarse << bed.scaleShift));
        }

        const int64_t energyCoarse = voxel.energy >> bed.scaleShift;
        cell.energy -= energyCoarse;
        carry.energy = -(voxel.energy - (energyCoarse << bed.scaleShift));

        // --- 1 ボクセルの物として足す(上下の 2 面が囲むセルに面する。横の 4 面は床の面の口)---
        const auto index = static_cast<uint32_t>(world.objects.size());
        rx::OhObject object{.voxelBegin = static_cast<uint32_t>(world.voxels.size()),
                            .voxelCount = 1,
                            .sizeX = 1,
                            .sizeY = 1,
                            .cell = 0,
                            .bodyLevel = BODY_LEVEL,
                            .cellLevel = cellLevel,
                            .substeps = 1,
                            .filmUm = FILM_UM};
        test::ChooseBlockShape(object);
        object.blockBegin = static_cast<uint32_t>(world.blocks.size());
        world.objects.push_back(object);
        world.infos.push_back({.object = index, .shape = 2u << rx::OH_EXPOSED_SHIFT});
        world.voxels.push_back(voxel);
        world.light.push_back(0);
        world.carries.push_back(carry);
        world.blocks.push_back({.object = index, .x = 0, .y = 0, .z = 0});
        world.blockStates.push_back({.flags = rx::OH_BLOCK_EXPOSED});
        world.waits.push_back({.changedTick = tick, .wakeTick = 0});
        world.heats.push_back(rx::OhVoxelHeat{});
        world.flows.push_back(rx::OhZeroFlow());
        world.ranges[0].objectCount += 1;
        bed.objects[BedIndex(x, z)] = static_cast<int32_t>(index);
    }

    // 1 本目の始まり: 焦点の升の熱の流れ(W/m²。mW ÷ 4 mm² = mW × 250。真ん中 600・辺 75・角 25 kW/m²)が Q_WAKE 以上の升を覆う床の升を起こし、
    // 焦点の光をその升に当てる(草が短い帯を吸う割合で。1 刻み = 1/60 s、mW × 8^6 / 60 = ボクセルの単位の mJ)
    // line = 線で火を付ける比べ: z の向きの升の列 x = 0 の全部に同じ光を当てる(燃え広がりの前線を 1 次元にして、横の升へ逃げる熱を除く)
    Bed StartBed(const sim::BakedReactionTable& table, const test::OhTables& tables, int32_t cellLevel, bool line) {
        Bed bed;
        bed.scaleShift = 3 * (BODY_LEVEL - cellLevel);
        bed.objects.assign(BED_SIDE * BED_SIDE, -1);
        bed.ignitionTicks.assign(BED_SIDE * BED_SIDE, 0);
        bed.arriving.assign(BED_SIDE * BED_SIDE, 0);
        test::OhWorld& world = bed.world;
        world.cells = {MakeGrassCell(table, cellLevel)};
        world.ranges = {{.cell = 0, .objectBegin = 0, .objectCount = 0}};
        world.cellChangedPasses = {0};
        world.sleeping = false;  // 床の面の口が試験の中から当てるので、眠らせ方は使わない

        const uint32_t cellulose = table.SpeciesId("cellulose");
        const uint64_t absorbQ16 = 65536 - table.speciesLight[cellulose].reflectanceShortQ16;
        for (uint32_t s = 0; s < SPOT_MILLIWATTS.size(); ++s) {
            if (static_cast<uint64_t>(SPOT_MILLIWATTS[s]) * 250 < Q_WAKE_WATTS_PER_M2)
                continue;

            ++bed.wokenSpots;
            const int32_t x = PatchOf(SpotCentreUm(s % 3));
            const int32_t focusZ = PatchOf(SpotCentreUm(s / 3));
            for (int32_t z = line ? -BED_RADIUS : focusZ; z <= (line ? BED_RADIUS : focusZ); ++z) {
                if (bed.objects[BedIndex(x, z)] < 0)
                    WakePatch(table, tables, bed, x, z, cellLevel, 0);

                const int64_t energy = (SPOT_MILLIWATTS[s] << (3 * BODY_LEVEL)) / 60;
                const auto object = static_cast<size_t>(bed.objects[BedIndex(x, z)]);
                world.light[world.objects[object].voxelBegin] += static_cast<int64_t>(
                    (static_cast<uint64_t>(energy) * absorbQ16) >> 16);
            }
        }

        return bed;
    }

    // --- 床の面の口(刻みの初めの値から。A → B が正。ボクセルの単位)---

    // 長い帯の吸い込み(s² の面。OhPairRadiation と同じ OhAbsorptanceQ16)
    uint32_t PatchAbsorptance(const test::OhTables& tables, const rx::RxCell& cell) {
        fx::FxU128 sum{.hi = 0, .lo = 0};
        for (uint32_t i = 0; i < cell.speciesCount; ++i)
            sum = rx::RdAddAbsorption(sum, tables.species[cell.species[i]].absorptionLongMm2PerMol, cell.amounts[i]);

        return rx::OhAbsorptanceQ16(sum, BODY_LEVEL);
    }

    struct PatchHeat {
        rx::OhVoxelHeat heat;
        uint32_t absorptance = 0;
    };

    int64_t BedFaceFlow(const rx::OhObject& object, const PatchHeat& a, const PatchHeat& b, bool radiation) {
        int64_t flow = rx::OhInnerConduction(object, a.heat, b.heat);
        if (!radiation)
            return flow;

        const uint64_t weight = rx::RdPairWeightQ32(a.absorptance, b.absorptance, rx::RD_ONE_Q16);
        const uint64_t coefficient = rx::RdPairCoefficient(static_cast<uint32_t>(a.heat.temperature),
                                                           static_cast<uint32_t>(b.heat.temperature), weight)
                                     << BODY_LEVEL;
        const uint64_t limit = rx::RdPairLimit(a.heat.heatCapacity, b.heat.heatCapacity, BED_SHARE_Q16);

        return flow + rx::RdPairFlow(static_cast<uint32_t>(a.heat.temperature),
                                     static_cast<uint32_t>(b.heat.temperature), std::min(coefficient, limit));
    }

    // 1 刻みの床の面の受け渡し(StepObjectHeat の前)。起きた升どうしは両側へ、まだセルの中の升へはセルへ(対の端数)
    void ExchangeBed(const test::OhCpuTable& shared, const sim::BakedReactionTable& table, const test::OhTables& tables,
                     Bed& bed, bool radiation, uint64_t tick) {
        test::OhWorld& world = bed.world;
        const rx::RxCell patch = MakePatchVoxel(table, world.cells[0], bed.scaleShift);
        const PatchHeat cellPatch{.heat = rx::OhPrepareVoxel(shared, patch),
                                  .absorptance = PatchAbsorptance(tables, patch)};
        std::vector<PatchHeat> heats(world.voxels.size());
        for (size_t v = 0; v < world.voxels.size(); ++v)
            heats[v] = {.heat = rx::OhPrepareVoxel(shared, world.voxels[v]),
                        .absorptance = PatchAbsorptance(tables, world.voxels[v])};

        std::vector<int64_t> change(world.voxels.size(), 0);
        std::fill(bed.arriving.begin(), bed.arriving.end(), 0);
        constexpr std::array<std::array<int32_t, 2>, 4> DIRECTIONS = {{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};
        for (int32_t z = -BED_RADIUS; z <= BED_RADIUS; ++z) {
            for (int32_t x = -BED_RADIUS; x <= BED_RADIUS; ++x) {
                const int32_t object = bed.objects[BedIndex(x, z)];
                if (object < 0)
                    continue;

                const rx::OhObject& header = world.objects[static_cast<size_t>(object)];
                const uint32_t voxel = header.voxelBegin;
                for (uint32_t d = 0; d < DIRECTIONS.size(); ++d) {
                    const int32_t nx = x + DIRECTIONS[d][0];
                    const int32_t nz = z + DIRECTIONS[d][1];
                    const bool inside = InBed(nx, nz);
                    const int32_t other = inside ? bed.objects[BedIndex(nx, nz)] : -1;
                    if (other >= 0) {
                        // 起きた升どうしは +x・+z の向きだけで数える(対を 1 回)
                        if (d >= 2)
                            continue;

                        const uint32_t otherVoxel = world.objects[static_cast<size_t>(other)].voxelBegin;
                        const int64_t flow = BedFaceFlow(header, heats[voxel], heats[otherVoxel], radiation);
                        change[voxel] -= flow;
                        change[otherVoxel] += flow;
                        continue;
                    }

                    const int64_t flow = BedFaceFlow(header, heats[voxel], cellPatch, radiation);
                    change[voxel] -= flow;
                    world.carries[static_cast<size_t>(object)].energy += flow;
                    if (inside)
                        bed.arriving[BedIndex(nx, nz)] += flow;
                }
            }
        }

        for (size_t v = 0; v < world.voxels.size(); ++v) {
            if (change[v] == 0)
                continue;

            world.voxels[v].energy += change[v];
            world.waits[v].changedTick = tick - 1;
        }
    }

    // 受けた熱の流れが Q_WAKE 以上の升を起こす(刻みの終わり。mW = 単位 × 60 ÷ 8^6。mW × 10^9 ≥ W/m² × µm²)
    uint32_t WakeHeatedPatches(const sim::BakedReactionTable& table, const test::OhTables& tables, Bed& bed,
                               int32_t cellLevel, uint64_t tick) {
        const int64_t voxelUm = CELL_UM >> BODY_LEVEL;
        const auto threshold = static_cast<int64_t>(Q_WAKE_WATTS_PER_M2) * voxelUm * voxelUm;
        uint32_t woken = 0;
        for (int32_t z = -BED_RADIUS; z <= BED_RADIUS; ++z) {
            for (int32_t x = -BED_RADIUS; x <= BED_RADIUS; ++x) {
                const size_t index = BedIndex(x, z);
                const int64_t milliwatts = (bed.arriving[index] * 60) >> (3 * BODY_LEVEL);
                if (bed.objects[index] >= 0 || milliwatts * 1000000000 < threshold)
                    continue;

                WakePatch(table, tables, bed, x, z, cellLevel, tick);
                ++woken;
            }
        }

        return woken;
    }

    // 保存の合計(ボクセルの単位): セル × 8^Δ + ボクセル + 端数 − 入った光(エネルギー)、元素
    std::vector<int64_t> Totals(const sim::BakedReactionTable& table, const test::OhTables& tables,
                                const test::OhWorld& world, int64_t scaleShift) {
        const size_t elementCount = table.elementNames.size();
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

    struct Burn {
        uint32_t wokenSpots = 0;
        uint32_t ignitionTick = 0;             // 焦点の升が燃え始めた刻み
        uint32_t lastBurningTick = 0;          // どれかの升が燃えていた最後の刻み
        uint32_t wokenPatches = 0;             // 起きた升(焦点を含む)
        uint32_t ignitedPatches = 0;           // 燃え始めた升
        std::array<uint32_t, 3> frontTicks{};  // +x の向きに 4・8・12 升目が燃え始めた刻み(0 = 届かない)
        int32_t peakMilliKelvin = 0;
        bool conserved = true;
        bool moved = true;
        double movedGrams = 0.0;
    };

    constexpr bool TRACE = false;                       // 調べる時だけ
    constexpr uint32_t LENS_AFTER_IGNITION_TICKS = 30;  // 燃え始めてから 0.5 s でレンズを外す(燃え広がりは火だけで)

    Burn RunBurn(const sim::BakedReactionTable& table, const test::OhTables& tables, int32_t cellLevel, bool radiation,
                 bool line = false) {
        Bed bed = StartBed(table, tables, cellLevel, line);
        test::OhWorld& world = bed.world;
        const test::OhCpuTable shared{.view = table.View(), .objectSpecies = tables.species};
        const sim::ReactionTableView view = table.View();
        const std::vector<int64_t> initial = Totals(table, tables, world, bed.scaleShift);
        Burn burn{.wokenSpots = bed.wokenSpots};
        for (uint32_t tick = 1; tick <= TICKS; ++tick) {
            std::vector<int64_t> before(world.voxels.size());
            for (size_t i = 0; i < world.voxels.size(); ++i)
                before[i] = rx::RxChemicalEnergy(view, world.voxels[i]);

            const bool lensOn = burn.ignitionTick == 0 || tick <= burn.ignitionTick + LENS_AFTER_IGNITION_TICKS;
            ExchangeBed(shared, table, tables, bed, radiation, tick);
            test::StepObjectHeat(shared, tables.gases, world, lensOn, WORLD_SEED, tick);
            for (int32_t z = -BED_RADIUS; z <= BED_RADIUS; ++z) {
                for (int32_t x = -BED_RADIUS; x <= BED_RADIUS; ++x) {
                    const size_t index = BedIndex(x, z);
                    const int32_t object = bed.objects[index];
                    if (object < 0)
                        continue;

                    const uint32_t voxel = world.objects[static_cast<size_t>(object)].voxelBegin;
                    const int64_t released = before[voxel] - rx::RxChemicalEnergy(view, world.voxels[voxel]);
                    const int64_t milliwatts = (released * 60 / 1000) >> (3 * BODY_LEVEL);
                    if (bed.ignitionTicks[index] == 0 && milliwatts >= IGNITION_MILLIWATTS)
                        bed.ignitionTicks[index] = tick;

                    if (milliwatts >= BURNING_MILLIWATTS)
                        burn.lastBurningTick = tick;

                    burn.peakMilliKelvin = std::max(burn.peakMilliKelvin,
                                                    rx::RxComputeThermal(view, world.voxels[voxel]).temperature);
                }
            }

            burn.ignitionTick = bed.ignitionTicks[BedIndex(0, 0)];
            if (TRACE && cellLevel == 0 && radiation && tick % 50 == 0 && tick >= 200 && tick <= 600) {
                std::printf("  trace %u:", tick);
                for (const int32_t x : {0, 1, 2}) {
                    const int32_t object = bed.objects[BedIndex(x, 0)];
                    if (object < 0)
                        continue;

                    const rx::RxCell& cell = world.voxels[world.objects[static_cast<size_t>(object)].voxelBegin];
                    std::printf(
                        " [%d] %.0f K セルロース %llu 炭 %llu O2 %llu", x,
                        rx::RxComputeThermal(view, cell).temperature / 1000.0,
                        static_cast<unsigned long long>(rx::OhAmountOf(cell, table.SpeciesId("cellulose")) >> 18),
                        static_cast<unsigned long long>(rx::OhAmountOf(cell, table.SpeciesId("carbon")) >> 18),
                        static_cast<unsigned long long>(rx::OhAmountOf(cell, table.SpeciesId("oxygen")) >> 18));
                }

                std::printf(" セル %.2f K\n", rx::RxComputeThermal(view, world.cells[0]).temperature / 1000.0);
                const int32_t focus = bed.objects[BedIndex(0, 0)];
                const int32_t side = bed.objects[BedIndex(1, 0)];
                if (focus >= 0 && side >= 0) {
                    const rx::OhObject& header = world.objects[static_cast<size_t>(focus)];
                    const rx::RxCell& a = world.voxels[header.voxelBegin];
                    const rx::RxCell& b = world.voxels[world.objects[static_cast<size_t>(side)].voxelBegin];
                    const PatchHeat ha{.heat = rx::OhPrepareVoxel(shared, a),
                                       .absorptance = PatchAbsorptance(tables, a)};
                    const PatchHeat hb{.heat = rx::OhPrepareVoxel(shared, b),
                                       .absorptance = PatchAbsorptance(tables, b)};
                    const uint64_t weight = rx::RdPairWeightQ32(ha.absorptance, hb.absorptance, rx::RD_ONE_Q16);
                    const uint64_t coefficient = rx::RdPairCoefficient(static_cast<uint32_t>(ha.heat.temperature),
                                                                       static_cast<uint32_t>(hb.heat.temperature),
                                                                       weight)
                                                 << BODY_LEVEL;
                    const uint64_t limit = rx::RdPairLimit(ha.heat.heatCapacity, hb.heat.heatCapacity, BED_SHARE_Q16);
                    std::printf("    α %u %u w %llu 係数 %llu 上限 %llu 伝導 %lld 全部 %lld C %llu %llu k %llu %llu\n",
                                ha.absorptance, hb.absorptance, static_cast<unsigned long long>(weight),
                                static_cast<unsigned long long>(coefficient), static_cast<unsigned long long>(limit),
                                static_cast<long long>(rx::OhInnerConduction(header, ha.heat, hb.heat)),
                                static_cast<long long>(BedFaceFlow(header, ha, hb, true)),
                                static_cast<unsigned long long>(ha.heat.heatCapacity),
                                static_cast<unsigned long long>(hb.heat.heatCapacity),
                                static_cast<unsigned long long>(ha.heat.innerConductivity),
                                static_cast<unsigned long long>(hb.heat.innerConductivity));
                }
            }

            WakeHeatedPatches(table, tables, bed, cellLevel, tick);
            burn.conserved = burn.conserved && !world.failed && Totals(table, tables, world, bed.scaleShift) == initial;
        }

        for (size_t index = 0; index < bed.objects.size(); ++index) {
            burn.wokenPatches += bed.objects[index] >= 0 ? 1u : 0u;
            burn.ignitedPatches += bed.ignitionTicks[index] != 0 ? 1u : 0u;
        }

        for (uint32_t f = 0; f < burn.frontTicks.size(); ++f)
            burn.frontTicks[f] = bed.ignitionTicks[BedIndex(static_cast<int32_t>(4 * (f + 1)), 0)];

        burn.moved = bed.moved;
        burn.movedGrams = static_cast<double>(bed.movedGrass) * 162.0 / 1.0e6 /
                          static_cast<double>(int64_t{1} << (3 * cellLevel));

        return burn;
    }

    // 燃え広がりの速さ(mm/s): 4 升目から 8 升目まで(4 升 × 7.8 mm)。届かなければ 0
    double SpreadMillimetresPerSecond(const Burn& burn) {
        if (burn.frontTicks[0] == 0 || burn.frontTicks[1] <= burn.frontTicks[0])
            return 0.0;

        return 4.0 * 7.8125 * 60.0 / static_cast<double>(burn.frontTicks[1] - burn.frontTicks[0]);
    }

    void Print(const char* label, const Burn& burn) {
        std::printf(
            "%s: 焦点の升 %u・着火 %u 刻み(%.2f s)・起きた升 %u・燃え始めた升 %u・+x の 4/8/12 升目の着火 %u/%u/%u "
            "刻み・"
            "広がる速さ %.2f mm/s・最後に燃えていた %u 刻み・一番熱い %.0f K・移した草 %.3f g・保存 %s・移す %s\n",
            label, burn.wokenSpots, burn.ignitionTick, burn.ignitionTick / 60.0, burn.wokenPatches, burn.ignitedPatches,
            burn.frontTicks[0], burn.frontTicks[1], burn.frontTicks[2], SpreadMillimetresPerSecond(burn),
            burn.lastBurningTick, burn.peakMilliKelvin / 1000.0, burn.movedGrams, burn.conserved ? "0" : "崩れた",
            burn.moved ? "OK" : "失敗");
    }

    // 比べ: 起こさずに、同じ 4 W を草のセルへ均す(30 s。燃え始めない)
    void PrintMixed(const sim::BakedReactionTable& table, int32_t cellLevel) {
        rx::RxCell cell = MakeGrassCell(table, cellLevel);
        const sim::ReactionTableView view = table.View();
        const int32_t start = rx::RxComputeThermal(view, cell).temperature;
        int64_t total = 0;
        for (const int64_t milliwatts : SPOT_MILLIWATTS)
            total += milliwatts;

        int64_t carry = 0;
        for (uint32_t tick = 1; tick <= TICKS; ++tick) {
            const int64_t energy = (total << (3 * cellLevel)) + carry;
            cell.energy += energy / 60;
            carry = energy % 60;
            cell = sim::StepReactionCellWait(table, cell, WORLD_SEED, tick, tick - 1, 0).cell;
        }

        std::printf("比べ(起こさずにセルへ均す・%s): %.3f K → %.3f K\n", cellLevel == 0 ? "0.5 m" : "0.25 m",
                    start / 1000.0, rx::RxComputeThermal(view, cell).temperature / 1000.0);
    }

}  // namespace

int main() {
    const sim::BakedReactionTable table = MakeTable();
    const auto tables = test::MakeObjectHeatTables(table, DEFAULT_DIFFUSIVITY_MM2_PER_S);
    EXPECT(tables.has_value());
    if (!tables)
        return 1;

    const Burn coarse = RunBurn(table, *tables, 0, true);
    const Burn fine = RunBurn(table, *tables, 1, true);
    const Burn coarseConduction = RunBurn(table, *tables, 0, false);
    const Burn fineConduction = RunBurn(table, *tables, 1, false);
    const Burn coarseLine = RunBurn(table, *tables, 0, true, true);
    const Burn fineLine = RunBurn(table, *tables, 1, true, true);
    Print("1 本目(熱の流れで起こす・床の面 = 伝導 + 放射)0.5 m の世界", coarse);
    Print("1 本目(熱の流れで起こす・床の面 = 伝導 + 放射)0.25 m の世界", fine);
    Print("比べ(床の面 = 伝導だけ)0.5 m の世界", coarseConduction);
    Print("比べ(床の面 = 伝導だけ)0.25 m の世界", fineConduction);
    Print("比べ(線で火を付ける・伝導 + 放射)0.5 m の世界", coarseLine);
    Print("比べ(線で火を付ける・伝導 + 放射)0.25 m の世界", fineLine);
    PrintMixed(table, 0);
    PrintMixed(table, 1);
    EXPECT(coarse.conserved && fine.conserved && coarse.moved && fine.moved);
    EXPECT(coarseConduction.conserved && fineConduction.conserved && coarseLine.conserved && fineLine.conserved);
    EXPECT(coarse.ignitionTick != 0 && fine.ignitionTick != 0);

    // 研究の記録(合否にしない。T-1097 作業ログ): 着火の時刻の差・燃え広がり(8 升目まで届くか・速さが 0.5 m と 0.25 m で ±10 % か)
    const uint32_t gap = coarse.ignitionTick > fine.ignitionTick ? coarse.ignitionTick - fine.ignitionTick
                                                                 : fine.ignitionTick - coarse.ignitionTick;
    std::printf("着火の時刻の差 %.2f %%\n", 100.0 * gap / std::max(coarse.ignitionTick, 1u));
    std::printf("燃え広がりの速さ(焦点)%.2f / %.2f mm/s・(線)%.2f / %.2f mm/s\n", SpreadMillimetresPerSecond(coarse),
                SpreadMillimetresPerSecond(fine), SpreadMillimetresPerSecond(coarseLine),
                SpreadMillimetresPerSecond(fineLine));
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("grass_wake: 全部通った\n");

    return 0;
}
