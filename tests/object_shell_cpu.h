// object_shell_cpu.h — 物の箱が囲む空気の層(骨格の外へ k 層。1 段目 T-1142 は k = 1・流れなし、2 段目 T-1143 は k 層 + 流れ〔object_flow_cpu.h〕)を作る・返す CPU リファレンス(D-667・ADR-0469 3-4)。
// 層のボクセルの真実は囲むセルと同じ RxCell(ADR-0434 §1。物専用の式 0)。式は shaders/common/object_heat.hlsli(OhShellVoxelOf・OhGasFlowOf・
// OhShellReturnable)、受け渡しは ③ の対の端数(OhSplitCarry)と ④(OhApplyToCell)= 帳簿の理由「物との受け渡し」と同じ口。
// データの流れ:
//   作る: 囲むセルの気体を層のボクセルの数だけ写す(同じ濃さ = 同じ数)→ その和を「セル → 物」の流れ(負)として対の端数を通してセルから引く
//   返す: 層のボクセルの和を「物 → セル」の流れとして対の端数を通してセルへ足す → 骨格だけの箱に戻す
//   どちらも物の箱の並びを作り直す(ボクセルの番号・ブロック・眠らせ方の印は FinishObjectHeatWorld で初めから。待ち〔真実〕は骨格の分を写す)。
// 形の語: 層 = OH_SHELL_BIT・外に面する面 = 箱の外の面。骨格 = 外に面する面 0・層を透かす放射の面 = 層の無い時の外に面する面の数(物理の面の広さを保つ)。
// 保存: Σ セル × 8^(L − Lc) + Σ ボクセル + Σ 対の端数 − 帳簿 = 一定(作る・返すでビット単位で変わらない)。
// 作る・返す条件は呼ぶ側(試験の中の口。状態だけで決まる。D-465)。GPU の形は後(T-1142 の作業ログ)。
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "object_heat_cpu.h"

namespace bicameral::test {

    // 箱の中の (x, y, z) のボクセルの、隣の印(箱の中の隣)と箱の外の面の数
    struct OhBoxFace {
        uint32_t neighbours;
        uint32_t outside;
    };

    inline OhBoxFace BoxFaceOf(uint32_t x, uint32_t y, uint32_t z, uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ) {
        const uint32_t neighbours = (x > 0 ? 1u : 0u) | (x + 1 < sizeX ? 2u : 0u) | (y > 0 ? 4u : 0u) |
                                    (y + 1 < sizeY ? 8u : 0u) | (z > 0 ? 16u : 0u) | (z + 1 < sizeZ ? 32u : 0u);

        return {.neighbours = neighbours, .outside = 6u - static_cast<uint32_t>(std::popcount(neighbours))};
    }

    // 物 o のボクセルの並びを置き換え、後ろの物の始まりをずらす(眠らせ方の印は初めから。待ちは真実なので写す)
    struct OhObjectVoxels {
        std::vector<reaction::RxCell> voxels;
        std::vector<reaction::OhVoxelInfo> infos;
        std::vector<int64_t> light;
        std::vector<reaction::OhVoxelWait> waits;
    };

    inline void ReplaceObjectVoxels(OhWorld& world, uint32_t o, const OhObjectVoxels& next) {
        const reaction::OhObject object = world.objects[o];
        const auto begin = static_cast<std::ptrdiff_t>(object.voxelBegin);
        const auto end = begin + static_cast<std::ptrdiff_t>(object.voxelCount);
        std::vector<reaction::OhVoxelWait> waits = world.waits;
        world.voxels.erase(world.voxels.begin() + begin, world.voxels.begin() + end);
        world.voxels.insert(world.voxels.begin() + begin, next.voxels.begin(), next.voxels.end());
        world.infos.erase(world.infos.begin() + begin, world.infos.begin() + end);
        world.infos.insert(world.infos.begin() + begin, next.infos.begin(), next.infos.end());
        world.light.erase(world.light.begin() + begin, world.light.begin() + end);
        world.light.insert(world.light.begin() + begin, next.light.begin(), next.light.end());
        waits.erase(waits.begin() + begin, waits.begin() + end);
        waits.insert(waits.begin() + begin, next.waits.begin(), next.waits.end());

        const auto grown = static_cast<int64_t>(next.voxels.size()) - static_cast<int64_t>(object.voxelCount);
        for (reaction::OhObject& later : world.objects) {
            if (later.voxelBegin > object.voxelBegin)
                later.voxelBegin = static_cast<uint32_t>(static_cast<int64_t>(later.voxelBegin) + grown);
        }

        world.objects[o].voxelCount = static_cast<uint32_t>(next.voxels.size());
        const uint32_t pass = world.pass;
        FinishObjectHeatWorld(world);
        world.waits = waits;
        world.pass = pass;
    }

    // セル ⇄ 物の流れ(ボクセル → セルが正)を、物 o の対の端数を通してセルへ当てる
    inline bool ExchangeThroughCarry(const OhGasList& gases, OhWorld& world, uint32_t o, const reaction::OhFlow& flow) {
        const reaction::OhCarryStep step = reaction::OhSplitCarry(world.objects[o], flow, world.carries[o]);
        world.carries[o] = step.carry;
        const uint32_t cell = world.objects[o].cell;
        const reaction::OhAmountChange change = reaction::OhApplyToCell(gases, world.cells[cell], step.coarse);
        world.cells[cell] = change.cell;
        world.cellChangedPasses[cell] = world.pass + 1;  // 次の段で外に面するブロックを起こす

        return change.ok != 0;
    }

    // --- 作る: 物 o の箱を骨格の外へ layers 層広げる(tick = 今の刻み。層のボクセルは「受け渡しで変わった」扱いで反応を評価する)---
    inline bool AddObjectShell(const OhCpuTable& table, const OhGasList& gases, OhWorld& world, uint32_t o,
                               uint64_t tick, uint32_t layers = 1) {
        namespace rx = bicameral::reaction;
        const rx::OhObject object = world.objects[o];
        if (object.shellLayers != 0 || layers == 0)
            return false;

        const rx::RxCell air = world.cells[object.cell];
        const rx::OhShellFill fill = rx::OhShellVoxelOf(table, gases, air,
                                                        rx::RxComputeThermal(table, air).temperature);
        if (fill.ok == 0)
            return false;

        const uint32_t sizeX = object.sizeX;
        const uint32_t sizeY = object.sizeY;
        const uint32_t sizeZ = object.voxelCount / (sizeX * sizeY);
        const uint32_t outerX = sizeX + (2 * layers);
        const uint32_t outerY = sizeY + (2 * layers);
        const uint32_t outerZ = sizeZ + (2 * layers);
        const size_t count = static_cast<size_t>(outerX) * outerY * outerZ;
        OhObjectVoxels next{.voxels = std::vector<rx::RxCell>(count),
                            .infos = std::vector<rx::OhVoxelInfo>(count),
                            .light = std::vector<int64_t>(count, 0),
                            .waits = std::vector<rx::OhVoxelWait>(count, rx::OhVoxelWait{.changedTick = tick - 1})};
        const rx::OhFlow perLayer = rx::OhGasFlowOf(gases, fill.cell);
        rx::OhFlow taken = rx::OhZeroFlow();
        for (uint32_t z = 0; z < outerZ; ++z) {
            for (uint32_t y = 0; y < outerY; ++y) {
                for (uint32_t x = 0; x < outerX; ++x) {
                    const size_t index = x + (static_cast<size_t>(outerX) * (y + (static_cast<size_t>(outerY) * z)));
                    const OhBoxFace face = BoxFaceOf(x, y, z, outerX, outerY, outerZ);
                    const bool skeleton = x >= layers && x < layers + sizeX && y >= layers && y < layers + sizeY &&
                                          z >= layers && z < layers + sizeZ;
                    if (!skeleton) {
                        next.voxels[index] = fill.cell;
                        next.infos[index] = {
                            .object = o,
                            .shape = face.neighbours | (face.outside << rx::OH_EXPOSED_SHIFT) | rx::OH_SHELL_BIT};
                        taken = rx::OhAddFlow(taken, perLayer);
                        continue;
                    }

                    // 骨格: 外に面する面の数を「層を透かして放射する面」へ移す
                    const uint32_t old = object.voxelBegin + (x - layers) +
                                         (sizeX * ((y - layers) + (sizeY * (z - layers))));
                    const uint32_t exposed = (world.infos[old].shape >> rx::OH_EXPOSED_SHIFT) & rx::OH_EXPOSED_MASK;
                    next.voxels[index] = world.voxels[old];
                    next.infos[index] = {.object = o, .shape = face.neighbours | (exposed << rx::OH_THROUGH_SHIFT)};
                    next.light[index] = world.light[old];
                    next.waits[index] = world.waits[old];
                }
            }
        }

        // セル → 層(流れは負)。層の分だけセルの気体が減る(層が占める体積の分。ADR-0469 2-4 の (a)・無い頁 = 0 のまま)
        rx::OhFlow toCell = rx::OhZeroFlow();
        toCell.energy = -taken.energy;
        for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g)
            toCell.amounts[g] = -taken.amounts[g];

        world.objects[o].sizeX = outerX;
        world.objects[o].sizeY = outerY;
        world.objects[o].shellLayers = layers;
        ReplaceObjectVoxels(world, o, next);

        return ExchangeThroughCarry(gases, world, o, toCell);
    }

    // --- 返す: 物 o の層の中身をセルへ返し、骨格だけの箱に戻す(層に気体でない物があれば返さない)---
    inline bool RemoveObjectShell(const OhCpuTable& table, const OhGasList& gases, OhWorld& world, uint32_t o) {
        namespace rx = bicameral::reaction;
        const rx::OhObject object = world.objects[o];
        const uint32_t layers = object.shellLayers;
        if (layers == 0)
            return false;

        const uint32_t outerX = object.sizeX;
        const uint32_t outerY = object.sizeY;
        const uint32_t outerZ = object.voxelCount / (outerX * outerY);
        const uint32_t sizeX = outerX - (2 * layers);
        const uint32_t sizeY = outerY - (2 * layers);
        const uint32_t sizeZ = outerZ - (2 * layers);
        const size_t count = static_cast<size_t>(sizeX) * sizeY * sizeZ;
        OhObjectVoxels next{.voxels = std::vector<rx::RxCell>(count),
                            .infos = std::vector<rx::OhVoxelInfo>(count),
                            .light = std::vector<int64_t>(count, 0),
                            .waits = std::vector<rx::OhVoxelWait>(count)};
        rx::OhFlow returned = rx::OhZeroFlow();
        for (uint32_t z = 0; z < outerZ; ++z) {
            for (uint32_t y = 0; y < outerY; ++y) {
                for (uint32_t x = 0; x < outerX; ++x) {
                    const uint32_t old = object.voxelBegin + x + (outerX * (y + (outerY * z)));
                    const uint32_t shape = world.infos[old].shape;
                    if ((shape & rx::OH_SHELL_BIT) != 0) {
                        if (!rx::OhShellReturnable(table, world.voxels[old]))
                            return false;

                        returned = rx::OhAddFlow(returned, rx::OhGasFlowOf(gases, world.voxels[old]));
                        continue;
                    }

                    const size_t index = (x - layers) +
                                         (static_cast<size_t>(sizeX) * ((y - layers) + (sizeY * (z - layers))));
                    const OhBoxFace face = BoxFaceOf(x - layers, y - layers, z - layers, sizeX, sizeY, sizeZ);
                    const uint32_t through = (shape >> rx::OH_THROUGH_SHIFT) & rx::OH_EXPOSED_MASK;
                    next.voxels[index] = world.voxels[old];
                    next.infos[index] = {.object = o, .shape = face.neighbours | (through << rx::OH_EXPOSED_SHIFT)};
                    next.light[index] = world.light[old];
                    next.waits[index] = world.waits[old];
                }
            }
        }

        world.objects[o].sizeX = sizeX;
        world.objects[o].sizeY = sizeY;
        world.objects[o].shellLayers = 0;
        ReplaceObjectVoxels(world, o, next);

        return ExchangeThroughCarry(gases, world, o, returned);
    }

}  // namespace bicameral::test
