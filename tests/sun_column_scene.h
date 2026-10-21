// sun_column_scene.h — 太陽の影の柱の試験(tests/sun_column_test.cpp・gpu_sun_column_test.cpp)が同じ場面を作る道具(T-0910)。
// 表は試験の表(combustion_test)に、光の値を書かない石(灰色で通さない・照り返し 0.5。D-610)と、短い帯を通し長い帯を吸うガラスを足したもの。
// 場面は 0.25 m(レベル 1)の座標で書き、レベル 0 の世界はセルの低い角の 0.25 m のセルで決める(どの場面も 0.5 m のセルの中で一様)。
// 公開側に置く試験データで、ゲームの中身ではない(D-006)。
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <span>
#include <vector>

#include "reaction_test_table.h"
#include "sim/multires_nest.h"
#include "sim/sun_column.h"

namespace bicameral::test {

    enum class SunMaterial : uint8_t {
        Air,
        Stone,
        Glass,
    };

    // 場面: 0.25 m のセルの座標 (x, y, z)(上は +y)→ 物質
    using SunSceneShape = std::function<SunMaterial(int64_t x, int64_t y, int64_t z)>;

    constexpr int32_t SUN_ROOM_TEMPERATURE_MK = 293150;
    constexpr uint32_t SUN_IRRADIANCE_SHORT = 600000;  // mW/m²(試験の値。晴れた日の見える光の目安)
    constexpr uint32_t SUN_IRRADIANCE_LONG = 400000;

    inline sim::BakedReactionTable MakeSunTable() {
        sim::ReactionTableDefinition definition = sim::MakeCombustionTestTable();
        definition.species.push_back({.name = "sun_test_stone",
                                      .composition = {{.element = "C", .count = 1}},
                                      .formationEnthalpy = 0,
                                      .heatCapacity = 8500,
                                      .thermalConductivity = 2000,
                                      .phase = sim::SpeciesPhase::Solid,
                                      .density = 2000});
        sim::SpeciesDefinition glass{.name = "sun_test_glass",
                                     .composition = {{.element = "C", .count = 1}},
                                     .formationEnthalpy = 0,
                                     .heatCapacity = 8500,
                                     .thermalConductivity = 1000,
                                     .phase = sim::SpeciesPhase::Solid,
                                     .density = 2500};
        glass.lightAbsorptionShort = 1;           // 短い帯: 0.5 m で τ ≈ 0.08(ほぼ通す)
        glass.lightAbsorptionLong = 4000000000u;  // 長い帯: 通さない
        glass.lightReflectanceShortPermille = 0;
        glass.lightReflectanceLongPermille = 0;
        definition.species.push_back(glass);

        auto baked = sim::BakeReactionTable(definition);
        if (!baked) {
            std::printf("表を作れない: %s\n", baked.error().c_str());
            std::exit(1);
        }

        return std::move(*baked);
    }

    inline reaction::RxCell MakeSunCell(const sim::BakedReactionTable& table, SunMaterial material) {
        // 石・ガラス(0.125 m³ に 2 万 mol。レベルによらず同じ数 = 同じ密度)
        constexpr uint64_t SOLID_MICROMOLES = 20000000000;
        if (material != SunMaterial::Air) {
            const char* name = material == SunMaterial::Stone ? "sun_test_stone" : "sun_test_glass";
            const std::vector<sim::SpeciesAmount> solid = {
                {.species = table.SpeciesId(name), .amount = SOLID_MICROMOLES}};

            return sim::MakeReactionCell(table, solid, SUN_ROOM_TEMPERATURE_MK);
        }

        // 空気(0.125 m³ に約 5.2 mol。気体は光の値を書かないので透明)
        const std::vector<sim::SpeciesAmount> air = {{.species = table.SpeciesId("oxygen"), .amount = 1092000},
                                                     {.species = table.SpeciesId("nitrogen"), .amount = 4108000}};

        return sim::MakeReactionCell(table, air, SUN_ROOM_TEMPERATURE_MK);
    }

    // 根を rootsX × rootsY × rootsZ 並べた世界(根のレベル level は 0 か 1)。refineBlocks は細かくする要求のための空きの枠
    inline sim::MultiresNest MakeSunWorld(const sim::BakedReactionTable& table, int32_t level, uint32_t rootsX,
                                          uint32_t rootsY, uint32_t rootsZ, const SunSceneShape& shape,
                                          uint32_t refineBlocks = 0) {
        const uint32_t roots = rootsX * rootsY * rootsZ;
        const uint32_t blocks = roots + refineBlocks;
        sim::MultiresNest nest = sim::MakeMultiresNest(
            {.worldBlocks = blocks,
             .observerBlocks = 0,
             .fractions = 1 + refineBlocks,
             .pages = blocks,
             .indexEntries = std::bit_ceil(2 * blocks),
             .ledgerColumns = 1 + static_cast<uint32_t>(table.species.size()),
             .rootLevel = level});
        const auto toQuarter = static_cast<uint32_t>(1 - level);  // 0.25 m のセルへのずらし
        const std::array<reaction::RxCell, 3> materials = {MakeSunCell(table, SunMaterial::Air),
                                                           MakeSunCell(table, SunMaterial::Stone),
                                                           MakeSunCell(table, SunMaterial::Glass)};
        std::vector<reaction::RxCell> cells(multires::MR_BLOCK_CELLS);
        for (uint32_t rootZ = 0; rootZ < rootsZ; ++rootZ) {
            for (uint32_t rootY = 0; rootY < rootsY; ++rootY) {
                for (uint32_t rootX = 0; rootX < rootsX; ++rootX) {
                    for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
                        const int64_t x = (int64_t{rootX} * 8) + multires::MrCellX(index);
                        const int64_t y = (int64_t{rootY} * 8) + multires::MrCellY(index);
                        const int64_t z = (int64_t{rootZ} * 8) + multires::MrCellZ(index);
                        cells[index] = materials[static_cast<size_t>(
                            shape(x << toQuarter, y << toQuarter, z << toQuarter))];
                    }

                    sim::PlaceRootBlock(nest, int64_t{rootX} * 8, int64_t{rootY} * 8, int64_t{rootZ} * 8, cells);
                }
            }
        }

        nest.edges.world = {.rootLevel = level,
                            .lowX = 0,
                            .lowY = 0,
                            .lowZ = 0,
                            .highX = int64_t{rootsX} * 8,
                            .highY = int64_t{rootsY} * 8,
                            .highZ = int64_t{rootsZ} * 8,
                            .top = 0,
                            .bottom = 0,
                            .sides = 0};

        return nest;
    }

    // --- 場面(0.25 m のセルの座標。箱は 4 m 角 = 16 セル)---

    // 平らな地面(下の 0.5 m が石)
    inline SunMaterial FlatGround(int64_t /*x*/, int64_t y, int64_t /*z*/) {
        return y < 2 ? SunMaterial::Stone : SunMaterial::Air;
    }

    // 地面 + 壁(x が 1〜1.5 m・高さ 0.5〜2.5 m・奥行き全部)
    inline SunMaterial WallScene(int64_t x, int64_t y, int64_t z) {
        if (y < 2)
            return SunMaterial::Stone;

        return x >= 4 && x < 6 && y < 10 ? FlatGround(x, 0, z) : SunMaterial::Air;
    }

    // 温室: 石の床・x と z が 0.5〜3.5 m の石の壁(高さ 0.5〜2 m)・2〜2.5 m がガラスの屋根
    inline SunMaterial GlassHouse(int64_t x, int64_t y, int64_t z) {
        if (y < 2)
            return SunMaterial::Stone;

        const bool insideOuter = x >= 2 && x < 14 && z >= 2 && z < 14;
        const bool insideInner = x >= 4 && x < 12 && z >= 4 && z < 12;
        if (!insideOuter)
            return SunMaterial::Air;

        if (y >= 8 && y < 10)
            return SunMaterial::Glass;

        return y < 8 && !insideInner ? SunMaterial::Stone : SunMaterial::Air;
    }

    // 平らな地面の上 1〜1.5 m にガラスの板(x が 0〜2 m の半分だけ)
    inline SunMaterial GroundAndPanel(int64_t x, int64_t y, int64_t z) {
        if (y < 2)
            return SunMaterial::Stone;

        return y >= 4 && y < 6 && x < 8 ? SunMaterial::Glass : FlatGround(x, y, z);
    }

    // 計測の地形(64³ のレベル 0 の世界を 0.25 m の座標で 128³): 高さが場所で変わる石の地面 + ガラスの柱
    inline SunMaterial Terrain(int64_t x, int64_t y, int64_t z) {
        const int64_t cellX = x >> 1;
        const int64_t cellZ = z >> 1;
        const auto height = static_cast<int64_t>(4 + ((cellX * 7 + cellZ * 13 + ((cellX * cellZ) % 5)) % 9));
        if ((y >> 1) < height)
            return SunMaterial::Stone;

        const bool pillar = (cellX % 16) == 8 && (cellZ % 16) == 8 && (y >> 1) < height + 10;

        return pillar ? SunMaterial::Glass : SunMaterial::Air;
    }

    // 世界の枠の本物の葉のセルを全部回す(覆われたセルは飛ばす)。visit(slot, index, block)
    template <typename Visit>
    void ForEachSunLeafCell(const sim::MultiresNest& nest, Visit visit) {
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const multires::MrBlock& block = nest.blocks[slot];
            if (block.kind != multires::MR_BLOCK_REAL)
                continue;

            for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
                if (!multires::MrIsCoveredCell(block, index))
                    visit(slot, index, block);
            }
        }
    }

    // 0.25 m のセルの座標(レベル 0 か 1 の葉のセルの低い角)
    inline std::array<int64_t, 3> SunQuarterCorner(const multires::MrBlock& block, uint32_t index) {
        const auto shift = static_cast<uint32_t>(1 - block.level);

        return {(block.originX + multires::MrCellX(index)) << shift,
                (block.originY + multires::MrCellY(index)) << shift,
                (block.originZ + multires::MrCellZ(index)) << shift};
    }

    inline uint64_t SunWord(std::span<const uint64_t> words, uint32_t slot, uint32_t index, uint32_t band,
                            uint32_t kind) {
        return words[multires::SunCellWord(slot, index, band, kind)];
    }

}  // namespace bicameral::test
