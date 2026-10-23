// object_heat_cpu.h — 物の熱(物の箱のボクセルの熱と反応・セルとの対の面の 4 つの口)の CPU リファレンス(T-1064・ADR-0434・20 §12.4)。
// 式は shaders/common/object_heat.hlsli(GPU の shaders/sim/object_heat.hlsl と同じ関数)。段の順も GPU と同じ:
//   小刻みごとに ① 下ごしらえ → ② 流れ(ボクセルに当て、セルへ出す分を記録)→ ③ 物ごとの和と対の端数 → ④ セルへ当てる、刻みの終わりに ⑤ 反応。
// 使うのは試験(tests/object_heat_test.cpp の試作とのビット一致・tests/gpu_object_heat_test.cpp の GPU とのビット一致と費用)。
// 世界のセルはここでは平らな並び(OhWorld::cells)。多重解像度のセルの口(MultiresCellStore)へつなぐのは T-1077。
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "common/object_heat.hlsli"
#include "sim/reaction_table.h"

namespace bicameral::test {

    // --- 表(reaction.hlsli の Table の約束 + 物の熱の物質の値)---

    struct OhCpuTable {
        sim::ReactionTableView view;
        std::span<const reaction::OhSpecies> objectSpecies;

        [[nodiscard]] reaction::RxSpecies Species(uint32_t id) const { return view.Species(id); }
        [[nodiscard]] reaction::RxRule Rule(uint32_t id) const { return view.Rule(id); }
        [[nodiscard]] reaction::RxRule RateRule(uint32_t id) const { return view.RateRule(id); }
        [[nodiscard]] uint32_t RuleIndex(uint32_t position) const { return view.RuleIndex(position); }
        [[nodiscard]] uint64_t Rate(uint32_t rule, uint32_t kelvin) const { return view.Rate(rule, kelvin); }
        [[nodiscard]] reaction::OhSpecies ObjectSpecies(uint32_t id) const { return objectSpecies[id]; }
    };

    // 対の面で混ざる気体(番号 → 物質 ID。0 = 使わない)
    struct OhGasList {
        std::array<uint32_t, reaction::OH_MAX_GASES> species{};

        [[nodiscard]] uint32_t GasSpecies(uint32_t gas) const { return species[gas]; }
    };

    struct OhTables {
        std::vector<reaction::OhSpecies> species;  // 添字 = 物質 ID
        OhGasList gases;
    };

    // ベイクした表から物の熱の表を作る(気体が OH_MAX_GASES を超えたら std::nullopt。T-1076)
    inline std::optional<OhTables> MakeObjectHeatTables(const sim::BakedReactionTable& table,
                                                        uint32_t defaultDiffusivityMm2PerS) {
        OhTables tables;
        tables.species.resize(table.species.size());
        uint32_t gasCount = 0;
        // 物質 0 は使わない(表の phase は Gas だが中身が無い)
        for (uint32_t id = 1; id < table.species.size(); ++id) {
            reaction::OhSpecies& values = tables.species[id];
            values.gasIndex = reaction::OH_NO_GAS;
            values.diffusivityMm2PerS = table.speciesTransport[id].diffusivityMm2PerS.value_or(
                defaultDiffusivityMm2PerS);
            if (table.speciesMolarVolume[id] > UINT32_MAX)
                return std::nullopt;

            values.molarVolumeMm3PerMol = static_cast<uint32_t>(table.speciesMolarVolume[id]);
            values.absorptionLongMm2PerMol = table.speciesLight[id].absorptionLong;
            if (table.speciesPhase[id] != sim::SpeciesPhase::Gas)
                continue;

            if (gasCount == reaction::OH_MAX_GASES)
                return std::nullopt;

            values.gasIndex = gasCount;
            tables.gases.species[gasCount++] = id;
        }

        tables.species[0].gasIndex = reaction::OH_NO_GAS;

        return tables;
    }

    // --- 世界(物・ボクセル・囲むセル・対の端数)---

    struct OhWorld {
        std::vector<reaction::OhObject> objects;    // 囲むセルの順(ranges が並びを指す)
        std::vector<reaction::OhVoxelInfo> infos;   // ボクセルごと
        std::vector<reaction::RxCell> voxels;       // 全部の物のボクセル(OhObject::voxelBegin から)
        std::vector<int64_t> light;                 // ボクセルが 1 刻みに吸う光(ボクセルの単位)
        std::vector<reaction::RxCell> cells;        // 囲むセル
        std::vector<reaction::OhCellRange> ranges;  // 物の入っているセルごとの物の範囲
        std::vector<reaction::OhFlow> carries;      // 物ごとの対の端数(真実。ハッシュとセーブに入る)
        int64_t sunIn = 0;                          // 帳簿: 入った光(ボクセルの単位)
        bool failed = false;                        // 量が負になる・成分の枠があふれた
    };

    // 小刻み 1 回(①〜④)
    inline void StepObjectHeatSubstep(const OhCpuTable& table, const OhGasList& gases, OhWorld& world, bool lightOn,
                                      uint32_t substep) {
        namespace rx = bicameral::reaction;
        const size_t count = world.voxels.size();

        // --- ① 下ごしらえ ---
        std::vector<rx::OhVoxelHeat> heats(count);
        for (size_t i = 0; i < count; ++i)
            heats[i] = rx::OhPrepareVoxel(table, world.voxels[i]);

        // --- ② 流れ(ボクセルごと。読むのは ① の作業場と囲むセルだけ)---
        std::vector<rx::OhFlow> flows(count, rx::OhZeroFlow());
        for (size_t i = 0; i < count; ++i) {
            const rx::OhVoxelInfo info = world.infos[i];
            const rx::OhObject object = world.objects[info.object];
            if (substep >= object.substeps)
                continue;

            const rx::RxCell air = world.cells[object.cell];
            const rx::RxThermal airThermal = rx::RxComputeThermal(table, air);
            const uint64_t airConductivity = uint64_t{rx::HcCellConductance(table, air)} << rx::HC_CONDUCTANCE_SHIFT;
            int64_t inner = 0;
            for (uint32_t direction = 0; direction < 6; ++direction) {
                if ((info.shape & (1u << direction)) == 0)
                    continue;

                const auto other = static_cast<size_t>(static_cast<int64_t>(i) +
                                                       rx::OhNeighbourOffset(object, direction));
                inner -= rx::OhInnerConduction(object, heats[i], heats[other]);
            }

            const int64_t sunlight = lightOn ? rx::OhLightShare(world.light[i], substep, object.substeps) : 0;
            const rx::OhVoxelStep step = rx::OhApplyVoxel(table, gases, object, info.shape, world.voxels[i], heats[i],
                                                          air, airThermal, airConductivity, inner, sunlight);
            world.sunIn += sunlight;
            world.voxels[i] = step.cell;
            flows[i] = step.toCell;
            world.failed = world.failed || step.ok == 0;
        }

        // --- ③ 物ごとの和と対の端数 ---
        std::vector<rx::OhFlow> coarse(world.objects.size());
        for (size_t o = 0; o < world.objects.size(); ++o) {
            const rx::OhObject object = world.objects[o];
            rx::OhFlow total = rx::OhZeroFlow();
            for (uint32_t v = 0; v < object.voxelCount; ++v)
                total = rx::OhAddFlow(total, flows[object.voxelBegin + v]);

            const rx::OhCarryStep step = rx::OhSplitCarry(object, total, world.carries[o]);
            world.carries[o] = step.carry;
            coarse[o] = step.coarse;
        }

        // --- ④ セルへ当てる ---
        for (const rx::OhCellRange& range : world.ranges) {
            rx::OhFlow sum = rx::OhZeroFlow();
            for (uint32_t o = 0; o < range.objectCount; ++o)
                sum = rx::OhAddFlow(sum, coarse[range.objectBegin + o]);

            const rx::OhAmountChange change = rx::OhApplyToCell(gases, world.cells[range.cell], sum);
            world.cells[range.cell] = change.cell;
            world.failed = world.failed || change.ok == 0;
        }
    }

    // ⑤ 反応(セルと同じ関数。受け渡しで毎刻み変わるので、変わった刻みは前の刻み。セルの ID = ボクセルの通し番号)
    inline void StepObjectHeatReactions(const OhCpuTable& table, OhWorld& world, uint64_t worldSeed, uint64_t tick) {
        for (size_t i = 0; i < world.voxels.size(); ++i)
            world.voxels[i] = reaction::RxStepCellWait(table, world.voxels[i], worldSeed, tick, tick - 1, i).cell;
    }

    // 1 刻み = 対の面の小刻み(物の中で一番多い数)+ 反応
    inline void StepObjectHeat(const OhCpuTable& table, const OhGasList& gases, OhWorld& world, bool lightOn,
                               uint64_t worldSeed, uint64_t tick) {
        uint32_t substeps = 1;
        for (const reaction::OhObject& object : world.objects)
            substeps = std::max(substeps, object.substeps);

        for (uint32_t sub = 0; sub < substeps; ++sub)
            StepObjectHeatSubstep(table, gases, world, lightOn, sub);

        StepObjectHeatReactions(table, world, worldSeed, tick);
    }

}  // namespace bicameral::test
