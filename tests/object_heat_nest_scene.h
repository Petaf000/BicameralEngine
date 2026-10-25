// object_heat_nest_scene.h — 物の熱を多重解像度の世界のセルにつなぐ場面と CPU の口(T-1077。object_heat_nest_test.cpp から分けた)。
// 1 刻み = 世界の刻み(StepNest)→ 囲むセルを世界から読む(LoadNestCell。変わったセルは物のブロックを起こす)→ 物の熱の刻み(StepObjectHeat)
//   → 変わったセルを世界へ書く(ExpandNestPage・StoreNestCell・PokeNestBlock)と、その差を世界の帳簿の理由「物との受け渡し」に載せる。
// 使うのは tests/object_heat_nest_test.cpp(CPU の保存と決定性)と tests/gpu_object_heat_nest_test.cpp(GPU の口とのビット一致)。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "common/world_ledger.hlsli"
#include "object_heat_cpu.h"
#include "sun_column_scene.h"

namespace bicameral::test::object_heat_nest {

    namespace rx = bicameral::reaction;
    namespace mr = bicameral::multires;

    // --- 試験の値(公開側の試験データ。ゲームの中身ではない。D-006。gpu_object_heat_test と同じ物)---
    constexpr int32_t BODY_LEVEL = 6;
    constexpr uint32_t FILM_UM = 3906;
    constexpr uint64_t FULL_CELLULOSE_UNITS = 1157000000;
    constexpr uint64_t AIR_OXYGEN_UNITS = 1092000;
    constexpr uint64_t AIR_NITROGEN_UNITS = 4108000;
    constexpr uint32_t DEFAULT_DIFFUSIVITY_MM2_PER_S = 20;
    constexpr int32_t BURNING_MILLIKELVIN = 900000;
    constexpr uint64_t WORLD_SEED = 0x1077;

    inline sim::BakedReactionTable MakeTable() {
        // 伝導率は表の現実の値のまま(試験の倍率は T-1066 で表から外した。ここで割っていたのを外す)
        return MakeSunTable();
    }

    // --- 物(箱の並び。外に面するボクセルの数を形の語に)---

    struct Shape {
        uint32_t sizeX;
        uint32_t sizeY;
        uint32_t sizeZ;
        uint64_t celluloseUnits;
        bool sheet;
    };

    constexpr Shape PAPER{27, 1, 38, (FULL_CELLULOSE_UNITS * 100 << BODY_LEVEL) / 500000, true};
    constexpr Shape TWIG{2, 2, 38, (FULL_CELLULOSE_UNITS * 128) / 1000, false};
    constexpr Shape LOG{10, 10, 51, (FULL_CELLULOSE_UNITS * 400) / 1000, false};

    inline void AddObject(const sim::BakedReactionTable& table, OhWorld& world, const Shape& shape, uint32_t cell,
                          int32_t cellLevel, bool burning) {
        const auto objectIndex = static_cast<uint32_t>(world.objects.size());
        world.objects.push_back({.voxelBegin = static_cast<uint32_t>(world.voxels.size()),
                                 .voxelCount = shape.sizeX * shape.sizeY * shape.sizeZ,
                                 .sizeX = shape.sizeX,
                                 .sizeY = shape.sizeY,
                                 .cell = cell,
                                 .bodyLevel = BODY_LEVEL,
                                 .cellLevel = cellLevel,
                                 .substeps = 1,
                                 .filmUm = FILM_UM});
        world.carries.push_back(rx::OhZeroFlow());
        const std::vector<sim::SpeciesAmount> amounts = {
            {.species = table.SpeciesId("cellulose"), .amount = shape.celluloseUnits},
            {.species = table.SpeciesId("oxygen"), .amount = AIR_OXYGEN_UNITS},
            {.species = table.SpeciesId("nitrogen"), .amount = AIR_NITROGEN_UNITS}};
        const rx::RxCell hot = sim::MakeReactionCell(table, amounts, BURNING_MILLIKELVIN);
        const rx::RxCell cold = sim::MakeReactionCell(table, amounts, SUN_ROOM_TEMPERATURE_MK);
        for (uint32_t z = 0; z < shape.sizeZ; ++z) {
            for (uint32_t y = 0; y < shape.sizeY; ++y) {
                for (uint32_t x = 0; x < shape.sizeX; ++x) {
                    const std::array<bool, 6> inside = {x > 0, x + 1 < shape.sizeX, y > 0, y + 1 < shape.sizeY,
                                                        z > 0, z + 1 < shape.sizeZ};
                    uint32_t neighbours = 0;
                    uint32_t exposed = 0;
                    for (uint32_t direction = 0; direction < 6; ++direction) {
                        if (inside[direction])
                            neighbours |= 1u << direction;
                        else if (!shape.sheet || direction == 2 || direction == 3)
                            ++exposed;
                    }

                    world.infos.push_back(
                        {.object = objectIndex, .shape = neighbours | (exposed << rx::OH_EXPOSED_SHIFT)});
                    world.voxels.push_back(exposed != 0 && burning ? hot : cold);
                    world.light.push_back(0);
                }
            }
        }

        if (world.ranges.empty() || world.ranges.back().cell != cell)
            world.ranges.push_back({.cell = cell, .objectBegin = objectIndex, .objectCount = 0});

        ++world.ranges.back().objectCount;
    }

    // --- 世界のセルの口(T-1077 の最初の形)---

    struct NestCellRef {
        uint32_t slot;
        uint32_t index;
    };

    // 0.25 m の座標(x, y, z)を含むレベル level のセル(根の枠だけの世界。枠は z・y・x の順に置いた)
    inline NestCellRef CellAt(int32_t level, uint32_t roots, uint32_t quarterX, uint32_t quarterY, uint32_t quarterZ) {
        const auto shift = static_cast<uint32_t>(1 - level);
        const uint32_t x = quarterX >> shift;
        const uint32_t y = quarterY >> shift;
        const uint32_t z = quarterZ >> shift;

        return {.slot = (x / 8) + (roots * ((y / 8) + (roots * (z / 8)))),
                .index = mr::MrCellIndex(x % 8, y % 8, z % 8)};
    }

    // 世界から囲むセルを読む。世界の刻みで変わったセルは、物の外に面するブロックを起こす(変わった段 = 今の段)
    inline void LoadCells(const sim::MultiresNest& nest, std::span<const NestCellRef> refs, OhWorld& world) {
        for (size_t c = 0; c < refs.size(); ++c) {
            const rx::RxCell cell = sim::LoadNestCell(nest, refs[c].slot, refs[c].index);
            if (!rx::RxSameCell(cell, world.cells[c]))
                world.cellChangedPasses[c] = world.pass;

            world.cells[c] = cell;
        }
    }

    // 量の差 after − before を、帳簿の理由「物との受け渡し」の列 column に載せる(増えた = 入・減った = 出)
    inline void AddLedgerDelta(sim::MultiresNest& nest, int32_t level, uint32_t column, int64_t delta) {
        if (delta == 0)
            return;

        const uint32_t direction = delta > 0 ? mr::WL_DIRECTION_IN : mr::WL_DIRECTION_OUT;
        sim::AddWorldLedgerUnits(nest, mr::WlSlot(mr::WL_REASON_OBJECT_EXCHANGE, direction), level, column,
                                 static_cast<uint64_t>(delta > 0 ? delta : -delta));
    }

    // 物との受け渡しでセルが変わった分を帳簿へ(エネルギーと、どちらかのセルにある物質ごと)
    inline void LedgerCellChange(sim::MultiresNest& nest, int32_t level, const rx::RxCell& before,
                                 const rx::RxCell& after, uint32_t speciesCount) {
        AddLedgerDelta(nest, level, mr::WL_COLUMN_ENERGY, after.energy - before.energy);
        for (uint32_t species = 1; species < speciesCount; ++species) {
            const int64_t delta = static_cast<int64_t>(rx::OhAmountOf(after, species)) -
                                  static_cast<int64_t>(rx::OhAmountOf(before, species));
            AddLedgerDelta(nest, level, mr::WL_COLUMN_SPECIES_BASE + species, delta);
        }
    }

    // 物の熱で変わったセルを世界へ書き、帳簿に載せ、ブロックをつつく(次の刻みに反応と伝導が見る)
    inline bool StoreCells(sim::MultiresNest& nest, std::span<const NestCellRef> refs, const OhWorld& world,
                           std::span<const rx::RxCell> before, int32_t level, uint32_t speciesCount) {
        bool stored = true;
        for (size_t c = 0; c < refs.size(); ++c) {
            if (rx::RxSameCell(world.cells[c], before[c]))
                continue;

            LedgerCellChange(nest, level, before[c], world.cells[c], speciesCount);

            stored = stored && sim::ExpandNestPage(nest, refs[c].slot);
            sim::StoreNestCell(nest, refs[c].slot, refs[c].index, world.cells[c]);
            sim::PokeNestBlock(nest, refs[c].slot);
        }

        return stored;
    }

    // --- 場面(0.5 m なら根 1 つ・0.25 m なら根 2 × 2 × 2 の 4 m 角の箱。下 1 m が石)---

    struct NestScene {
        sim::MultiresNest nest;
        std::array<NestCellRef, 2> refs;  // 0 = 燃える物の囲むセル・1 = 冷えた薪の囲むセル(+x の隣)
        OhWorld world;
        int32_t level = 0;
    };

    inline NestScene MakeNestScene(const sim::BakedReactionTable& table, int32_t level) {
        const uint32_t roots = level == 0 ? 1u : 2u;
        NestScene scene{.nest = MakeSunWorld(
                            table, level, roots, roots, roots,
                            [](int64_t, int64_t y, int64_t) { return y < 4 ? SunMaterial::Stone : SunMaterial::Air; }),
                        .refs = {CellAt(level, roots, 8, 5, 8), CellAt(level, roots, 10, 5, 8)},
                        .world = {},
                        .level = level};
        for (const NestCellRef& ref : scene.refs)
            scene.world.cells.push_back(sim::LoadNestCell(scene.nest, ref.slot, ref.index));

        for (uint32_t i = 0; i < 3; ++i)
            AddObject(table, scene.world, TWIG, 0, level, true);

        AddObject(table, scene.world, PAPER, 0, level, true);
        AddObject(table, scene.world, LOG, 1, level, false);
        FinishObjectHeatWorld(scene.world);

        return scene;
    }

    // 1 刻み(CPU): 世界の刻み(伝導つき)→ 囲むセルを読む → 物の熱 → 変わったセルを世界へ書く。書けなかった・物の熱が失敗したら false
    inline bool StepNestScene(const sim::BakedReactionTable& table, const OhTables& tables, NestScene& scene,
                              uint64_t tick) {
        // 世界のセルどうしの熱の伝導を入れる(StepNest の既定は伝導なし。T-1077 の 1 回目はこれを付けずに隣のセルが動かなかった)
        sim::StepNest(scene.nest, table, WORLD_SEED, tick, {.conduction = true});
        LoadCells(scene.nest, scene.refs, scene.world);

        const std::vector<reaction::RxCell> before = scene.world.cells;
        const OhCpuTable shared{.view = table.View(), .objectSpecies = tables.species};
        StepObjectHeat(shared, tables.gases, scene.world, false, WORLD_SEED, tick);

        const auto speciesCount = static_cast<uint32_t>(table.species.size());

        return StoreCells(scene.nest, scene.refs, scene.world, before, scene.level, speciesCount) &&
               !scene.world.failed;
    }

}  // namespace bicameral::test::object_heat_nest
