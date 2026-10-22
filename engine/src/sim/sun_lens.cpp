// sun_lens.cpp — 日の光を物で集める CPU リファレンス(sun_lens.h)。光線 1 本の本体は shaders/common/sun_lens.hlsli。
#include "sim/sun_lens.h"

#include <algorithm>
#include <climits>
#include <format>

namespace bicameral::sim {

    namespace mr = ::bicameral::multires;
    namespace rx = ::bicameral::reaction;

    namespace {

        constexpr int32_t ROOM_TEMPERATURE_MK = 293150;
        constexpr int64_t CELL0_UM = mr::LN_CELL0_UM;
        constexpr uint64_t MILLIJOULE_TICKS_TO_WATTS_PER_M2 = 60000000000;  // mJ/刻み ÷ µm² → W/m²: × 60 ÷ 1000 × 10^12

        // --- 光線の本体に渡す道具(sun_column.hlsli・sun_lens.hlsli の約束)---

        struct CpuTree {
            const MultiresNest* nest = nullptr;

            [[nodiscard]] mr::MrBlock Block(uint32_t slot) const { return nest->blocks[slot]; }

            [[nodiscard]] uint32_t Lookup(int32_t level, int64_t originX, int64_t originY, int64_t originZ) const {
                return LookupBlock(*nest, level, originX, originY, originZ);
            }
        };

        struct CpuCells {
            const MultiresNest* nest = nullptr;

            [[nodiscard]] rx::RxCell Cell(uint32_t slot, uint32_t index) const {
                return LoadNestCell(*nest, slot, index);
            }
        };

        struct CpuLights {
            const std::vector<rx::LtSpeciesLight>* lights = nullptr;

            [[nodiscard]] rx::LtSpeciesLight Light(uint32_t species) const { return (*lights)[species]; }
        };

        struct CpuSink {
            std::vector<uint64_t>* words = nullptr;

            void Deposit(uint32_t slot, uint32_t index, uint32_t band, uint64_t absorbed, uint64_t reflected) const {
                (*words)[mr::SunCellWord(slot, index, band, mr::SUN_KIND_ABSORBED)] += absorbed;
                (*words)[mr::SunCellWord(slot, index, band, mr::SUN_KIND_REFLECTED)] += reflected;
            }
        };

        // 当たった面の升: 足し算だけ(順によらない)
        struct CpuHits {
            std::map<SunLensSpot, uint64_t>* hits = nullptr;

            void Add(uint32_t slot, uint32_t index, uint32_t axis, int64_t spotU, int64_t spotV,
                     uint64_t energy) const {
                if (energy != 0)
                    (*hits)[{static_cast<int64_t>(SunLensHitKey(slot, index, axis)), spotU, spotV}] += energy;
            }
        };

        int32_t FinestRealLevel(const MultiresNest& nest) {
            int32_t finest = nest.capacity.rootLevel;
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                const mr::MrBlock& block = nest.blocks[slot];
                if (block.kind == mr::MR_BLOCK_REAL && block.level > finest)
                    finest = block.level;
            }

            return finest;
        }

        // 球面の膨らみ(口の半径 a の所で、頂点からの深さ)= R − √(R² − a²)(切り上げ側に 1 µm)
        int64_t Sagitta(int64_t radiusUm, int64_t apertureUm) {
            const auto squared = static_cast<uint64_t>((radiusUm * radiusUm) - (apertureUm * apertureUm));

            return radiusUm - static_cast<int64_t>(fx::FxSqrtU64(squared)) + 1;
        }

        // --- レンズの口の格子(物の細かさ)と、影の柱の光線との対応 ---
        // 光線 i(箱の低い端からの番号)はレンズの面の上で [low + i × セル + ずれ, + セル) を受け持ち、その中の格子の点は m × m 個。
        // 点 j の位置 = low + ずれ + ⌊(2j + 1) × 0.5 m ÷ 2N⌋(N = 2^lensLevel。光線の細かさによらず同じ点)
        struct LensGrid {
            int64_t lowBUm = 0;  // 箱の低い端 + レンズの面での横のずれ(B・C)
            int64_t lowCUm = 0;
            int64_t centreB = 0;
            int64_t centreC = 0;
            int64_t aperture = 0;
            int64_t perRay = 0;  // m = N ÷ 2^rayLevel
            int64_t firstRayB = 0;
            int64_t firstRayC = 0;
            int32_t slab = -1;
            uint32_t shift = 0;  // m² = 2^shift
            int32_t lensLevel = 0;

            [[nodiscard]] int64_t PointB(int64_t j) const {
                return lowBUm + (((2 * j) + 1) * CELL0_UM) / (int64_t{2} << static_cast<uint32_t>(lensLevel));
            }

            [[nodiscard]] int64_t PointC(int64_t j) const {
                return lowCUm + (((2 * j) + 1) * CELL0_UM) / (int64_t{2} << static_cast<uint32_t>(lensLevel));
            }

            [[nodiscard]] bool Inside(int64_t jB, int64_t jC) const {
                const int64_t db = PointB(jB) - centreB;
                const int64_t dc = PointC(jC) - centreC;

                return (db * db) + (dc * dc) <= aperture * aperture;
            }

            // 光線(rayB・rayC)が受け持つ点の番号の始まり(B・C)
            [[nodiscard]] std::array<int64_t, 2> FirstPoint(uint32_t rayB, uint32_t rayC) const {
                return {(firstRayB + rayB) * perRay, (firstRayC + rayC) * perRay};
            }

            // 光線が受け持つ範囲が口の外接の正方形に掛かるか(掛からなければ数えない)
            [[nodiscard]] bool Touches(uint32_t rayB, uint32_t rayC) const {
                const auto first = FirstPoint(rayB, rayC);
                const bool apartB = PointB(first[0] + perRay - 1) < centreB - aperture ||
                                    PointB(first[0]) > centreB + aperture;
                const bool apartC = PointC(first[1] + perRay - 1) < centreC - aperture ||
                                    PointC(first[1]) > centreC + aperture;

                return !apartB && !apartC;
            }

            [[nodiscard]] mr::SunCatch Catch(uint32_t rayB, uint32_t rayC) const {
                mr::SunCatch caught{.slab = -1, .count = 0, .shift = shift, .padding = 0};
                if (!Touches(rayB, rayC))
                    return caught;

                const auto first = FirstPoint(rayB, rayC);
                for (int64_t kC = 0; kC < perRay; ++kC) {
                    for (int64_t kB = 0; kB < perRay; ++kB)
                        caught.count += Inside(first[0] + kB, first[1] + kC) ? 1u : 0u;
                }

                caught.slab = caught.count == 0 ? -1 : slab;

                return caught;
            }
        };

        std::expected<LensGrid, std::string> MakeLensGrid(const mr::SunParams& params, const SunLensShape& shape) {
            if (params.axisA != shape.axis || params.signA != -shape.facing)
                return std::unexpected(
                    "レンズの光の軸が太陽の向きの一番大きい軸と合わない(回る物の箱は T-0878 の申し送り)");

            if (shape.lensLevel < params.rayLevel || shape.lensLevel - params.rayLevel > 15)
                return std::unexpected("物の細かさは光線の細かさ以上・差は 15 まで");

            const int64_t cellUm = mr::LnCellUm(params.rayLevel);
            const int64_t aLens = shape.centreUm[shape.axis];
            const int64_t aCell = mr::LnFloorDivide(aLens, cellUm) - params.lowA;
            if (aCell < 0 || aCell >= params.sizeA)
                return std::unexpected("レンズが世界の箱の外");

            // レンズの面までの A の道のり(光の入る面から)と、横のずれ = 道のり × D_B ÷ D_A(床へ)
            const int64_t travelUm = params.signA > 0 ? aLens - (params.lowA * cellUm)
                                                      : ((params.lowA + params.sizeA) * cellUm) - aLens;
            const int64_t driftB = mr::LnFloorDivide(travelUm * params.stepB, params.stepA);
            const int64_t driftC = mr::LnFloorDivide(travelUm * params.stepC, params.stepA);

            LensGrid grid;
            grid.lowBUm = (params.lowB * cellUm) + driftB;
            grid.lowCUm = (params.lowC * cellUm) + driftC;
            grid.centreB = shape.centreUm[params.axisB];
            grid.centreC = shape.centreUm[params.axisC];
            grid.aperture = shape.apertureUm;
            grid.perRay = int64_t{1} << static_cast<uint32_t>(shape.lensLevel - params.rayLevel);
            grid.firstRayB = params.firstRayB;
            grid.firstRayC = params.firstRayC;
            grid.slab = static_cast<int32_t>(params.signA > 0 ? aCell : params.sizeA - 1 - aCell);
            grid.shift = 2 * static_cast<uint32_t>(shape.lensLevel - params.rayLevel);
            grid.lensLevel = shape.lensLevel;

            return grid;
        }

        mr::LnWorld MakeLensWorld(const MultiresNest& nest) {
            const mr::WeWorld& box = nest.edges.world;
            const int64_t cellUm = mr::LnCellUm(box.rootLevel);

            return {.lowX = box.lowX * cellUm,
                    .lowY = box.lowY * cellUm,
                    .lowZ = box.lowZ * cellUm,
                    .highX = box.highX * cellUm,
                    .highY = box.highY * cellUm,
                    .highZ = box.highZ * cellUm,
                    .finestLevel = FinestRealLevel(nest),
                    .rootLevel = nest.capacity.rootLevel};
        }

        // 影の柱の光線 1 本が取られた量を、口の中の点の光線に分ける(余りは最初の点へ)。点の並びは C → B の順
        struct LensRaySeed {
            mr::LnVec through{};
            uint64_t energyShort = 0;
            uint64_t energyLong = 0;
        };

        void SeedLensRays(const LensGrid& grid, const mr::SunParams& params, int64_t aLens, const SunCaughtRay& caught,
                          std::vector<LensRaySeed>& seeds) {
            const mr::SunCatch counted = grid.Catch(caught.rayB, caught.rayC);
            if (counted.count == 0)
                return;

            const uint64_t totalShort = caught.caughtShort << mr::LN_ENERGY_SHIFT;
            const uint64_t totalLong = caught.caughtLong << mr::LN_ENERGY_SHIFT;
            const uint64_t shareShort = totalShort / counted.count;
            const uint64_t shareLong = totalLong / counted.count;
            const auto first = grid.FirstPoint(caught.rayB, caught.rayC);
            bool firstPoint = true;
            for (int64_t kC = 0; kC < grid.perRay; ++kC) {
                for (int64_t kB = 0; kB < grid.perRay; ++kB) {
                    if (!grid.Inside(first[0] + kB, first[1] + kC))
                        continue;

                    std::array<int64_t, 3> position{};
                    position[params.axisA] = aLens;
                    position[params.axisB] = grid.PointB(first[0] + kB);
                    position[params.axisC] = grid.PointC(first[1] + kC);
                    LensRaySeed seed{.through = mr::LnMake(position[0], position[1], position[2]),
                                     .energyShort = shareShort,
                                     .energyLong = shareLong};
                    if (firstPoint) {
                        seed.energyShort += totalShort - (shareShort * counted.count);
                        seed.energyLong += totalLong - (shareLong * counted.count);
                        firstPoint = false;
                    }

                    seeds.push_back(seed);
                }
            }
        }

        void AddLensLedger(SunLensResult& result, const mr::LnRay& ray, uint64_t enteredShort, uint64_t enteredLong) {
            result.lensEntered[0] += enteredShort;
            result.lensEntered[1] += enteredLong;
            result.lensAbsorbed[0] += ray.lensAbsorbedShort;
            result.lensAbsorbed[1] += ray.lensAbsorbedLong;
            result.lensReflected[0] += ray.lensReflectedShort;
            result.lensReflected[1] += ray.lensReflectedLong;
            result.worldAbsorbed[0] += ray.absorbedShort;
            result.worldAbsorbed[1] += ray.absorbedLong;
            result.worldReflected[0] += ray.reflectedShort;
            result.worldReflected[1] += ray.reflectedLong;
            result.escaped[0] += ray.escapedShort;
            result.escaped[1] += ray.escapedLong;
            result.unfinishedRays += ray.unfinished;
            result.lensRays += 1;
        }

        // W/m² = 量(mJ ÷ 2^extraShift ÷ 8^rayLevel の単位。1 刻み)× 60 ÷ 1000 ÷ 面(µm² × 10^−12)
        uint64_t FluxOf(uint64_t energy, uint32_t extraShift, int32_t rayLevel, uint64_t areaUm2) {
            if (energy == 0)
                return 0;

            const uint64_t divisor = std::max<uint64_t>(areaUm2, 1)
                                     << (extraShift + (3 * static_cast<uint32_t>(rayLevel)));

            return fx::FxDivU128By64(fx::FxMulU64Full(energy, MILLIJOULE_TICKS_TO_WATTS_PER_M2), divisor).quotient;
        }

    }  // namespace

    std::expected<mr::LnLens, std::string> MakeSunLens(const BakedReactionTable& table, const SunLensShape& shape) {
        const bool shaped = shape.apertureUm > 0 && shape.frontRadiusUm > shape.apertureUm &&
                            shape.backRadiusUm > shape.apertureUm && shape.axis < 3 &&
                            (shape.facing == 1 || shape.facing == -1);
        if (!shaped)
            return std::unexpected("レンズの形: 口の半径 > 0・球面の半径 > 口の半径・軸 0〜2・向き ±1");

        if (shape.thicknessUm <
            Sagitta(shape.frontRadiusUm, shape.apertureUm) + Sagitta(shape.backRadiusUm, shape.apertureUm))
            return std::unexpected("レンズの厚さが 2 つの球面の膨らみの和より薄い(縁が無い)");

        if (shape.species == 0 || shape.species >= table.speciesLight.size())
            return std::unexpected("レンズの物質が表に無い");

        // --- 物質の値: 0.5 m の道のりの τ(帯ごと)・照り返し・屈折の値(描画と同じ値)---
        const std::vector<SpeciesAmount> amounts = {{.species = shape.species, .amount = shape.micromolesPerCell}};
        const rx::RxCell cell = MakeReactionCell(table, amounts, ROOM_TEMPERATURE_MK);
        const CpuLights lights{.lights = &table.speciesLight};
        const rx::LtCellBand shortBand = rx::LtCellBandOf(lights, cell, rx::LT_BAND_SHORT);
        const rx::LtCellBand longBand = rx::LtCellBandOf(lights, cell, rx::LT_BAND_LONG);

        return mr::LnLens{.centreX = shape.centreUm[0],
                          .centreY = shape.centreUm[1],
                          .centreZ = shape.centreUm[2],
                          .apertureUm = shape.apertureUm,
                          .frontRadiusUm = shape.frontRadiusUm,
                          .backRadiusUm = shape.backRadiusUm,
                          .thicknessUm = shape.thicknessUm,
                          .depthShortQ32 = mr::LnCellDepthQ32(shortBand),
                          .depthLongQ32 = mr::LnCellDepthQ32(longBand),
                          .reflectanceShortQ16 = shortBand.reflectanceQ16,
                          .reflectanceLongQ16 = longBand.reflectanceQ16,
                          .refractionQ16 = table.speciesRefractionQ16[shape.species],
                          .axis = shape.axis,
                          .facing = shape.facing,
                          .padding = 0};
    }

    std::expected<SunLensResult, std::string> FocusSunLight(const MultiresNest& nest, const BakedReactionTable& table,
                                                            const SunLight& sun, int32_t rayLevel,
                                                            const SunLensShape& shape, bool reverse) {
        const auto lens = MakeSunLens(table, shape);
        if (!lens)
            return std::unexpected(lens.error());

        const auto params = MakeSunParams(nest, sun, rayLevel);
        if (!params)
            return std::unexpected(params.error());

        const auto grid = MakeLensGrid(*params, shape);
        if (!grid)
            return std::unexpected(grid.error());

        // --- 影の柱: レンズの面で、口の中の点の割合を取る ---
        SunLensResult result;
        result.params = *params;
        const LensGrid catchGrid = *grid;
        result.columnWords = SweepSunColumnCaught(
            nest, table, *params, [&catchGrid](uint32_t rayB, uint32_t rayC) { return catchGrid.Catch(rayB, rayC); },
            result.caught);

        // --- レンズの光線: 取られた量を口の中の点へ分け、レンズで曲げて世界へ ---
        std::vector<LensRaySeed> seeds;
        for (const SunCaughtRay& caught : result.caught)
            SeedLensRays(*grid, *params, shape.centreUm[shape.axis], caught, seeds);

        if (reverse)
            std::reverse(seeds.begin(), seeds.end());

        result.lensWords.assign(SunColumnWordCount(nest), 0);
        const CpuTree tree{.nest = &nest};
        const CpuCells cells{.nest = &nest};
        const CpuLights lights{.lights = &table.speciesLight};
        const CpuSink sink{.words = &result.lensWords};
        const CpuHits hits{.hits = &result.hits};
        const mr::LnWorld world = MakeLensWorld(nest);
        const mr::LnVec sunDirection = mr::LnUnitOf(sun.direction[0], sun.direction[1], sun.direction[2]);
        for (const LensRaySeed& seed : seeds) {
            const mr::LnRay bent = mr::LnTraceLens(*lens, seed.through, sunDirection, seed.energyShort,
                                                   seed.energyLong);
            const mr::LnRay walked = mr::LnWalkWorld(tree, cells, lights, sink, hits, world, bent);
            AddLensLedger(result, walked, seed.energyShort, seed.energyLong);
        }

        return result;
    }

    uint64_t SunLensFluxWatts(const MultiresNest& nest, const SunLensResult& result, uint32_t slot, uint32_t index,
                              uint32_t axis) {
        // --- 日の光そのもの: セルが受けた量(吸った + 返した)÷ セルの面 ---
        const int32_t level = nest.blocks[slot].level;
        const auto cellUm = static_cast<uint64_t>(mr::LnCellUm(level));
        uint64_t direct = 0;
        for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
            direct += result.columnWords[mr::SunCellWord(slot, index, band, mr::SUN_KIND_ABSORBED)] +
                      result.columnWords[mr::SunCellWord(slot, index, band, mr::SUN_KIND_REFLECTED)];
        }

        const uint64_t directFlux = FluxOf(direct, 0, result.params.rayLevel, cellUm * cellUm);

        // --- レンズの光: 升の量の一番大きい所 ÷ 升の面 ---
        const auto key = static_cast<int64_t>(SunLensHitKey(slot, index, axis));
        uint64_t peak = 0;
        for (auto spot = result.hits.lower_bound({key, INT64_MIN, INT64_MIN});
             spot != result.hits.end() && spot->first[0] == key; ++spot)
            peak = std::max(peak, spot->second);

        const auto spotArea = static_cast<uint64_t>(mr::LN_SPOT_UM * mr::LN_SPOT_UM);

        return directFlux + FluxOf(peak, mr::LN_ENERGY_SHIFT, result.params.rayLevel, spotArea);
    }

    SunLensPeak FindSunLensPeak(const MultiresNest& nest, const SunLensResult& result) {
        SunLensPeak peak;
        uint64_t previous = UINT64_MAX;
        for (const auto& [spot, energy] : result.hits) {
            const auto key = static_cast<uint64_t>(spot[0]);
            if (key == previous)
                continue;

            previous = key;
            const auto axis = static_cast<uint32_t>(key % 3);
            const uint64_t cell = key / 3;
            const auto flux = SunLensFluxWatts(nest, result, static_cast<uint32_t>(cell / mr::MR_BLOCK_CELLS),
                                               static_cast<uint32_t>(cell % mr::MR_BLOCK_CELLS), axis);
            if (flux > peak.fluxWatts)
                peak = {.fluxWatts = flux, .key = key};
        }

        return peak;
    }

}  // namespace bicameral::sim
