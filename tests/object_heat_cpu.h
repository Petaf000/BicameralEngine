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

    // --- 世界(物・ボクセル・囲むセル・対の端数・物の中のブロック)---

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

        // --- 眠らせ方(T-1098。FinishObjectHeatWorld が作る)---
        std::vector<reaction::OhBlock> blocks;
        std::vector<reaction::OhBlockState> blockStates;
        std::vector<reaction::OhVoxelWait> waits;     // ボクセルごと(反応の待ち。真実)
        std::vector<uint32_t> cellChangedPasses;      // 囲むセルごと: 最後に変わった段
        std::vector<reaction::OhVoxelHeat> heats;     // ① の作業場(眠っているブロックは前の値のまま = 今の値)
        std::vector<reaction::OhVoxelGas> gasStarts;  // ① の作業場の気体(層に面する面の混ざりが読む。T-1142)
        std::vector<reaction::OhFlow> flows;          // ② の記録(眠っているブロックは 0 のまま)
        uint32_t pass = 0;                            // 段の通し番号
        bool sleeping = true;                         // false = 全部のブロックを毎段回す(眠らせ方の厳密さの比べ)
        uint64_t awakeExchangeBlocks = 0;             // 数えるだけ: 回した受け渡しのブロック(段の和)
        uint64_t awakeReactBlocks = 0;
    };

    // ブロックの形を物の形で選ぶ(積は 64 以下。薄い紙は 8 × 1 × 8、細い小枝は 2 × 2 × 16)
    inline void ChooseBlockShape(reaction::OhObject& object) {
        const uint32_t sizeZ = object.voxelCount / (object.sizeX * object.sizeY);
        object.blockEdgeX = std::min(object.sizeX, 4u);
        object.blockEdgeY = std::min(object.sizeY, 4u);
        object.blockEdgeZ = reaction::OH_BLOCK_VOXELS / (object.blockEdgeX * object.blockEdgeY);
        if (object.blockEdgeZ > 4 && object.blockEdgeY == 1 && object.sizeX >= 8) {
            object.blockEdgeX = 8;
            object.blockEdgeZ = 8;
        }

        object.blocksX = (object.sizeX + object.blockEdgeX - 1) / object.blockEdgeX;
        object.blocksY = (object.sizeY + object.blockEdgeY - 1) / object.blockEdgeY;
        object.blocksZ = (sizeZ + object.blockEdgeZ - 1) / object.blockEdgeZ;
    }

    // 物・ボクセル・光・セルを置いた後に、ブロックと眠らせ方の状態を作る
    inline void FinishObjectHeatWorld(OhWorld& world) {
        namespace rx = bicameral::reaction;
        world.blocks.clear();
        for (uint32_t o = 0; o < world.objects.size(); ++o) {
            rx::OhObject& object = world.objects[o];
            ChooseBlockShape(object);
            object.blockBegin = static_cast<uint32_t>(world.blocks.size());
            for (uint32_t z = 0; z < object.blocksZ; ++z) {
                for (uint32_t y = 0; y < object.blocksY; ++y) {
                    for (uint32_t x = 0; x < object.blocksX; ++x)
                        world.blocks.push_back({.object = o, .x = x, .y = y, .z = z});
                }
            }
        }

        world.blockStates.assign(world.blocks.size(), rx::OhBlockState{});
        for (size_t b = 0; b < world.blocks.size(); ++b) {
            const rx::OhObject object = world.objects[world.blocks[b].object];
            for (uint32_t local = 0; local < rx::OH_BLOCK_VOXELS; ++local) {
                const uint32_t voxel = rx::OhBlockVoxel(object, world.blocks[b], local);
                if (voxel == rx::OH_NO_BLOCK)
                    continue;

                // 外に面する = 囲むセルを読む面がある(膜の口か、層を透かす放射。T-1142)
                const uint32_t shape = world.infos[voxel].shape;
                const bool exposed = ((shape >> rx::OH_EXPOSED_SHIFT) & rx::OH_EXPOSED_MASK) != 0 ||
                                     ((shape >> rx::OH_THROUGH_SHIFT) & rx::OH_EXPOSED_MASK) != 0;
                world.blockStates[b].flags |= (exposed ? rx::OH_BLOCK_EXPOSED : 0u) |
                                              (world.light[voxel] != 0 ? rx::OH_BLOCK_LIT : 0u);
            }
        }

        world.waits.assign(world.voxels.size(), rx::OhVoxelWait{});
        world.cellChangedPasses.assign(world.cells.size(), 0);
        world.heats.assign(world.voxels.size(), rx::OhVoxelHeat{});
        world.gasStarts.assign(world.voxels.size(), rx::OhVoxelGas{});
        world.flows.assign(world.voxels.size(), rx::OhZeroFlow());
        world.pass = 0;
    }

    // 段の初め: この受け渡しの段で回すブロックを決める
    inline void WakeObjectHeatBlocks(OhWorld& world, bool lightOn) {
        namespace rx = bicameral::reaction;
        for (size_t b = 0; b < world.blocks.size(); ++b) {
            rx::OhBlockState& state = world.blockStates[b];
            const rx::OhObject object = world.objects[world.blocks[b].object];
            uint32_t newest = state.changedPass;
            for (uint32_t direction = 0; direction < 6; ++direction) {
                const uint32_t other = rx::OhNeighbourBlock(object, world.blocks[b], direction);
                if (other != rx::OH_NO_BLOCK)
                    newest = std::max(newest, world.blockStates[other].facePass[direction ^ 1u]);
            }

            const bool awake = rx::OhExchangeAwake(state, newest, world.cellChangedPasses[object.cell], lightOn);
            state.awake = (!world.sleeping || awake) ? 1u : 0u;
            world.awakeExchangeBlocks += state.awake;
        }
    }

    // ① 下ごしらえ(起きているブロックのボクセルだけ)
    inline void PrepareObjectHeat(const OhCpuTable& table, const OhGasList& gases, OhWorld& world) {
        for (size_t b = 0; b < world.blocks.size(); ++b) {
            if (world.blockStates[b].awake == 0)
                continue;

            const reaction::OhObject object = world.objects[world.blocks[b].object];
            for (uint32_t local = 0; local < reaction::OH_BLOCK_VOXELS; ++local) {
                const uint32_t voxel = reaction::OhBlockVoxel(object, world.blocks[b], local);
                if (voxel == reaction::OH_NO_BLOCK)
                    continue;

                world.heats[voxel] = reaction::OhPrepareVoxel(table, world.voxels[voxel]);
                world.gasStarts[voxel] = reaction::OhPrepareGas(gases, world.voxels[voxel]);
            }
        }
    }

    // ② 1 つのボクセルの流れ(当てて、セルへ出す分を world.flows に)。戻り値: ビット 0 = 変わった・ビット 1 = 静かでない
    inline uint32_t FlowObjectHeatVoxel(const OhCpuTable& table, const OhGasList& gases, OhWorld& world, uint32_t voxel,
                                        bool lightOn, uint32_t substep, uint64_t tick) {
        namespace rx = bicameral::reaction;
        const rx::OhVoxelInfo info = world.infos[voxel];
        const rx::OhObject object = world.objects[info.object];
        const rx::RxCell air = world.cells[object.cell];
        const rx::RxThermal airThermal = rx::RxComputeThermal(table, air);
        const uint64_t airConductivity = uint64_t{rx::HcCellConductance(table, air, rx::HC_ROOM_TEMPERATURE_MK)}
                                         << rx::HC_CONDUCTANCE_SHIFT;  // 室温の値(T-1066)
        rx::OhFlow inner = rx::OhZeroFlow();
        bool faceFlow = false;
        for (uint32_t direction = 0; direction < 6; ++direction) {
            if ((info.shape & (1u << direction)) == 0)
                continue;

            const auto other = static_cast<uint32_t>(static_cast<int64_t>(voxel) +
                                                     rx::OhNeighbourOffset(object, direction));
            const int64_t heat = rx::OhInnerConduction(object, world.heats[voxel], world.heats[other]);
            inner.energy -= heat;
            faceFlow = faceFlow || heat != 0;

            // 層に面する面は気体も混ざる(T-1142)
            if (((info.shape | world.infos[other].shape) & rx::OH_SHELL_BIT) == 0)
                continue;

            const rx::OhFlow gas = rx::OhInnerGas(table, gases, object, world.gasStarts[voxel], world.gasStarts[other],
                                                  world.heats[voxel], world.heats[other]);
            inner.energy -= gas.energy;
            for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g)
                inner.amounts[g] -= gas.amounts[g];

            faceFlow = faceFlow || !rx::OhFlowIsZero(gas);
        }

        const int64_t sunlight = lightOn ? rx::OhLightShare(world.light[voxel], substep, object.substeps) : 0;
        const rx::OhVoxelStep step = rx::OhApplyVoxelInner(table, gases, object, info.shape, world.voxels[voxel],
                                                           world.heats[voxel], air, airThermal, airConductivity, inner,
                                                           sunlight);
        const bool changed = !rx::RxSameCell(step.cell, world.voxels[voxel]);
        world.sunIn += sunlight;
        world.voxels[voxel] = step.cell;
        world.flows[voxel] = step.toCell;
        world.failed = world.failed || step.ok == 0;
        if (changed)
            world.waits[voxel].changedTick = tick - 1;

        return (changed ? 1u : 0u) | ((faceFlow || step.flowsZero == 0) ? 2u : 0u);
    }

    // ② 流れ(起きているブロックだけ。読むのは ① の作業場と囲むセル)
    inline void FlowObjectHeat(const OhCpuTable& table, const OhGasList& gases, OhWorld& world, bool lightOn,
                               uint32_t substep, uint64_t tick) {
        namespace rx = bicameral::reaction;
        for (size_t b = 0; b < world.blocks.size(); ++b) {
            rx::OhBlockState& state = world.blockStates[b];
            if (state.awake == 0)
                continue;

            const rx::OhObject object = world.objects[world.blocks[b].object];
            uint32_t marks = 0;
            for (uint32_t local = 0; local < rx::OH_BLOCK_VOXELS; ++local) {
                const uint32_t voxel = rx::OhBlockVoxel(object, world.blocks[b], local);
                if (voxel == rx::OH_NO_BLOCK)
                    continue;

                if (substep >= object.substeps) {
                    world.flows[voxel] = rx::OhZeroFlow();
                    continue;
                }

                const uint32_t mark = FlowObjectHeatVoxel(table, gases, world, voxel, lightOn, substep, tick);
                marks |= mark | ((mark & 1u) != 0 ? rx::OhVoxelFaces(object, local) << 2 : 0u);
            }

            if (substep >= object.substeps)
                continue;

            state.changedPass = (marks & 1u) != 0 ? world.pass : state.changedPass;
            state.quiet = marks == 0 ? 1u : 0u;
            state.exchangePass = world.pass;
            for (uint32_t face = 0; face < 6; ++face)
                state.facePass[face] = (marks & (4u << face)) != 0 ? world.pass : state.facePass[face];
        }
    }

    // ③ 物ごとの和と対の端数。物ごとのセルへ渡す量(セルの単位)を返す
    inline std::vector<reaction::OhFlow> CarryObjectHeat(OhWorld& world) {
        std::vector<reaction::OhFlow> coarse(world.objects.size());
        for (size_t o = 0; o < world.objects.size(); ++o) {
            const reaction::OhObject object = world.objects[o];
            reaction::OhFlow total = reaction::OhZeroFlow();
            for (uint32_t v = 0; v < object.voxelCount; ++v)
                total = reaction::OhAddFlow(total, world.flows[object.voxelBegin + v]);

            const reaction::OhCarryStep step = reaction::OhSplitCarry(object, total, world.carries[o]);
            world.carries[o] = step.carry;
            coarse[o] = step.coarse;
        }

        return coarse;
    }

    // ④ セルへ当てる(そのセルの物の粗い量の和)。変わったセルは段の番号を記録する
    inline void ApplyObjectHeatToCells(const OhGasList& gases, OhWorld& world,
                                       std::span<const reaction::OhFlow> coarse) {
        for (const reaction::OhCellRange& range : world.ranges) {
            reaction::OhFlow sum = reaction::OhZeroFlow();
            for (uint32_t o = 0; o < range.objectCount; ++o)
                sum = reaction::OhAddFlow(sum, coarse[range.objectBegin + o]);

            const reaction::OhAmountChange change = reaction::OhApplyToCell(gases, world.cells[range.cell], sum);
            world.cells[range.cell] = change.cell;
            world.failed = world.failed || change.ok == 0;
            if (!reaction::OhFlowIsZero(sum))
                world.cellChangedPasses[range.cell] = world.pass;
        }
    }

    // 小刻み 1 回(段 1 つ: 起こす → ①〜④)
    inline void StepObjectHeatSubstep(const OhCpuTable& table, const OhGasList& gases, OhWorld& world, bool lightOn,
                                      uint32_t substep, uint64_t tick) {
        ++world.pass;
        WakeObjectHeatBlocks(world, lightOn);
        PrepareObjectHeat(table, gases, world);
        FlowObjectHeat(table, gases, world, lightOn, substep, tick);
        const std::vector<reaction::OhFlow> coarse = CarryObjectHeat(world);
        ApplyObjectHeatToCells(gases, world, coarse);
    }

    // ⑤ 反応(セルと同じ関数。段 1 つ。ボクセルの changedTick と起こす刻みで待ちの丸め。セルの ID = ボクセルの通し番号)
    inline void StepObjectHeatReactions(const OhCpuTable& table, OhWorld& world, uint64_t worldSeed, uint64_t tick) {
        namespace rx = bicameral::reaction;
        ++world.pass;
        for (size_t b = 0; b < world.blocks.size(); ++b) {
            rx::OhBlockState& state = world.blockStates[b];
            if (world.sleeping && !rx::OhReactAwake(state, tick))
                continue;

            ++world.awakeReactBlocks;
            const rx::OhObject object = world.objects[world.blocks[b].object];
            bool changed = false;
            uint32_t faces = 0;
            uint64_t minWake = rx::RX_WAIT_NEVER;
            for (uint32_t local = 0; local < rx::OH_BLOCK_VOXELS; ++local) {
                const uint32_t voxel = rx::OhBlockVoxel(object, world.blocks[b], local);
                if (voxel == rx::OH_NO_BLOCK)
                    continue;

                rx::OhVoxelWait& wait = world.waits[voxel];
                const rx::RxWaitStep step = rx::RxStepCellWait(table, world.voxels[voxel], worldSeed, tick,
                                                               wait.changedTick, voxel);
                if (!rx::RxSameCell(step.cell, world.voxels[voxel])) {
                    world.voxels[voxel] = step.cell;
                    wait.changedTick = tick;
                    changed = true;
                    faces |= rx::OhVoxelFaces(object, local);
                }

                wait.wakeTick = step.wakeTick;
                minWake = std::min(minWake, step.wakeTick);
            }

            state.minWakeTick = minWake;
            state.reactPass = world.pass;
            state.changedPass = changed ? world.pass : state.changedPass;
            for (uint32_t face = 0; face < 6; ++face)
                state.facePass[face] = (faces & (1u << face)) != 0 ? world.pass : state.facePass[face];
        }
    }

    // 1 刻み = 対の面の小刻み(物の中で一番多い数)+ 反応
    inline void StepObjectHeat(const OhCpuTable& table, const OhGasList& gases, OhWorld& world, bool lightOn,
                               uint64_t worldSeed, uint64_t tick) {
        uint32_t substeps = 1;
        for (const reaction::OhObject& object : world.objects)
            substeps = std::max(substeps, object.substeps);

        for (uint32_t sub = 0; sub < substeps; ++sub)
            StepObjectHeatSubstep(table, gases, world, lightOn, sub, tick);

        StepObjectHeatReactions(table, world, worldSeed, tick);
    }

}  // namespace bicameral::test
