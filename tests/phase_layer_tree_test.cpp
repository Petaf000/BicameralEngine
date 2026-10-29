// phase_layer_tree_test.cpp — 相の層のエネルギー Ec の欄を世界の木で細かく/粗くする(T-1230 段 1・T-1231 の CPU。ADR-0512 の 1・D-674)の CPU の試験。
// 根 1 つの全部のセルを「水たまり(層 280 K)+ 空気(気体 320 K)」の混ざったセルにして、八分の一を細かくして粗くする。確かめること:
//   1. 置かれ方を使わない世界(子は親の写し): 子のエネルギーと Ec は親と同じ(今とビット単位で同じ)
//   2. 置かれ方を使う世界: 子のエネルギーの和は親 × 8(ビット単位)・混ざった子の層は親の層の温度・気体だけの子は親の気体の温度
//      (層と混ぜない = 細かくしただけで温度が変わらない)・混ざらない子の Ec は 0
//   3. 粗くすると親のセルのエネルギーは元のまま・Ec は元の値(丸めの分まで)・保存量が同じ
//   4. Ec は状態の要約に入る(変えると変わる)・保存点で戻る・2 回走らせて同じ
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "multires_test_scene.h"
#include "phase_layer_scene.h"
#include "sim/multires_fields.h"
#include "sim/multires_nest.h"
#include "sim/multires_state.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    constexpr uint32_t LAYER_MK = 280000;
    constexpr uint32_t GAS_MK = 320000;
    constexpr int32_t TOLERANCE_MK = 10;  // 子 1 つの丸め 1 mJ 以下(空気 0.6 mol の子で約 0.1 mK)

    // 水たまりの量(300 mol = 5.4 L。0.5 m のセルの約 4 %)と空気
    std::vector<test::PlAmount> Water() {
        return {{"water", 300000000}};
    }

    std::vector<test::PlAmount> Air() {
        return {{"nitrogen", test::PL_AIR_NITROGEN_MICROMOL}, {"oxygen", test::PL_AIR_OXYGEN_MICROMOL}};
    }

    struct MixedCell {
        RxCell cell;
        int64_t condensedEnergy = 0;
    };

    MixedCell MakeMixedCell(const BakedReactionTable& table) {
        const int64_t layerMicro = test::PlPartEnergyMicrojoules(table, Water(), LAYER_MK);
        const int64_t gasMicro = test::PlPartEnergyMicrojoules(table, Air(), GAS_MK);
        std::vector<test::PlAmount> all = Water();
        for (const test::PlAmount& entry : Air())
            all.push_back(entry);

        MixedCell mixed;
        mixed.condensedEnergy = layerMicro / 1000;
        mixed.cell = test::MakePhaseLayerCell(table, all, mixed.condensedEnergy + (gasMicro / 1000));

        return mixed;
    }

    struct TreeWorld {
        MultiresNest nest;
        uint32_t field = MULTIRES_NO_FIELD;
        uint32_t root = MR_NO_BLOCK;
    };

    TreeWorld MakeTreeWorld(const BakedReactionTable& table, bool placement) {
        TreeWorld world{.nest = MakeMultiresNest(test::MakeMultiresCapacity(table, 16, 4, 16))};
        MultiresNest& nest = world.nest;
        world.field = EnablePhaseLayerEnergy(nest, table);
        Expect(world.field != MULTIRES_NO_FIELD, "欄を登録できる");
        if (placement)
            EnableRefineCellPlacement(nest, table);

        const MixedCell mixed = MakeMixedCell(table);
        const std::vector<RxCell> cells(MR_BLOCK_CELLS, mixed.cell);
        world.root = PlaceRootBlock(nest, 0, 0, 0, cells);
        const bool expanded = ExpandNestPage(nest, world.root);
        Expect(expanded, "根を頁に広げられる");
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
            StoreNestField(nest, world.field, world.root, index, 0, mixed.condensedEnergy);

        return world;
    }

    void Request(MultiresNest& nest, uint32_t op) {
        const MrRequest request = MrMakeRequest(op, 1, 1, 1, 1);
        SubmitRequests(nest, std::span(&request, 1));
        ProcessRequests(nest);
    }

    uint32_t ChildOfRoot(const MultiresNest& nest) {
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            if (nest.blocks[slot].kind == MR_BLOCK_REAL && nest.blocks[slot].level == 1)
                return slot;
        }

        return MR_NO_BLOCK;
    }

    int32_t Distance(int32_t a, int32_t b) {
        return a > b ? a - b : b - a;
    }

    // --- 1 子が親の写し: 子は親と同じ ---
    void TestCopy(const BakedReactionTable& table) {
        TreeWorld world = MakeTreeWorld(table, false);
        const MixedCell mixed = MakeMixedCell(table);
        Request(world.nest, MR_REQUEST_REFINE);
        const uint32_t child = ChildOfRoot(world.nest);
        Expect(child != MR_NO_BLOCK && !MrIsUniform(world.nest.blocks[child]), "写し: 頁を持つ子ができる");
        if (child == MR_NO_BLOCK)
            return;

        uint32_t wrong = 0;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const RxCell cell = LoadNestCell(world.nest, child, index);
            wrong += cell.energy == mixed.cell.energy ? 0u : 1u;
            wrong += LoadNestPhaseLayerEnergy(world.nest, child, index) == mixed.condensedEnergy ? 0u : 1u;
        }

        Expect(wrong == 0, "写し: 子のエネルギーと Ec は親と同じ(今とビット単位で同じ)");
    }

    // --- 2・3 置かれ方で配る: 部分の温度を保つ・和はビット単位・粗くすると戻る ---
    void TestPlaced(const BakedReactionTable& table) {
        TreeWorld world = MakeTreeWorld(table, true);
        MultiresNest& nest = world.nest;
        const MixedCell mixed = MakeMixedCell(table);
        const ConservedTotals before = ComputeConservedTotals(nest, table, 2);
        const uint64_t hashBefore = HashWholeNest(nest);

        Request(nest, MR_REQUEST_REFINE);
        const uint32_t child = ChildOfRoot(nest);
        Expect(child != MR_NO_BLOCK && !MrIsUniform(nest.blocks[child]), "配り: 頁を持つ子ができる");
        if (child == MR_NO_BLOCK)
            return;

        const uint32_t octant = nest.blocks[child].parentOctant;
        uint32_t wrongSum = 0;
        uint32_t wrongLayer = 0;
        uint32_t wrongGas = 0;
        uint32_t strayEnergy = 0;
        uint32_t mixedChildren = 0;
        uint32_t gasChildren = 0;
        for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
            int64_t sum = 0;
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                const uint32_t index = MrChildCell(local, j);
                const RxCell cell = LoadNestCell(nest, child, index);
                const int64_t condensed = LoadNestPhaseLayerEnergy(nest, child, index);
                sum += cell.energy;
                const PlLayer layer = PlLayerOf(table.View(), cell);
                if (layer.mixed != 0) {
                    const PlParts parts = PlPartsOf(table.View(), cell, condensed);
                    wrongLayer += Distance(parts.condensed.temperature, (int32_t)LAYER_MK) <= TOLERANCE_MK ? 0u : 1u;
                    wrongGas += Distance(parts.gas.temperature, (int32_t)GAS_MK) <= TOLERANCE_MK ? 0u : 1u;
                    ++mixedChildren;
                    continue;
                }

                strayEnergy += condensed == 0 ? 0u : 1u;
                if (layer.condensedAmount == 0) {
                    const RxThermal thermal = RxComputeThermal(table.View(), cell);
                    wrongGas += Distance(thermal.temperature, (int32_t)GAS_MK) <= TOLERANCE_MK ? 0u : 1u;
                    ++gasChildren;
                }
            }

            wrongSum += sum == mixed.cell.energy * 8 ? 0u : 1u;
        }

        Log(Channel::Sim, Level::Info, "配り: 混ざった子 {}・気体だけの子 {}", mixedChildren, gasChildren);
        Expect(mixedChildren > 0 && gasChildren > 0, "配り: 置かれ方で混ざった子と気体だけの子に分かれる");
        Expect(wrongSum == 0, "配り: 子のエネルギーの和は親 × 8(ビット単位)");
        Expect(wrongLayer == 0, "配り: 混ざった子の層は親の層の温度(D-674 を部分ごとに)");
        Expect(wrongGas == 0, "配り: 子の気体は親の気体の温度(層と混ぜない)");
        Expect(strayEnergy == 0, "配り: 混ざらない子の Ec は 0");
        Expect(ComputeConservedTotals(nest, table, 2) == before, "配り: 細かくしても保存量が同じ");

        // --- 粗くする: セルのエネルギーは元のまま・Ec は元の値(丸めの分まで)---
        Request(nest, MR_REQUEST_COARSEN);
        Expect(ChildOfRoot(nest) == MR_NO_BLOCK, "粗く: 子が無い");
        uint32_t wrongBack = 0;
        uint32_t wrongEnergy = 0;
        for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
            const uint32_t index = MrOctantCell(octant, local);
            wrongEnergy += LoadNestCell(nest, world.root, index).energy == mixed.cell.energy ? 0u : 1u;
            const int64_t back = LoadNestPhaseLayerEnergy(nest, world.root, index);
            wrongBack += Distance((int32_t)(back - mixed.condensedEnergy), 0) <= 1 ? 0u : 1u;
        }

        Expect(wrongEnergy == 0, "粗く: 親のセルのエネルギーは元のまま");
        Expect(wrongBack == 0, "粗く: 親の Ec は元の値(1 mJ の内)");
        Expect(ComputeConservedTotals(nest, table, 2) == before, "粗く: 保存量が同じ");
        Log(Channel::Sim, Level::Info, "往復の要約: 前 {:016x} 後 {:016x}", hashBefore, HashWholeNest(nest));
    }

    // --- 4 状態の要約・保存点・2 回の実行 ---
    uint64_t RunOnce(const BakedReactionTable& table) {
        TreeWorld world = MakeTreeWorld(table, true);
        Request(world.nest, MR_REQUEST_REFINE);

        return HashWholeNest(world.nest);
    }

    void TestState(const BakedReactionTable& table) {
        Expect(RunOnce(table) == RunOnce(table), "状態: 2 回走らせて同じ");

        TreeWorld world = MakeTreeWorld(table, true);
        MultiresNest& nest = world.nest;
        const uint64_t hash = HashWholeNest(nest);
        const MultiresSaveState save = SaveNestState(nest);
        StoreNestField(nest, world.field, world.root, 0, 0, LoadNestPhaseLayerEnergy(nest, world.root, 0) + 1);
        Expect(HashWholeNest(nest) != hash, "状態: Ec は状態の要約に入る");
        Expect(RestoreNestState(nest, save) && HashWholeNest(nest) == hash, "状態: 保存点で Ec が戻る");
    }

    int Run() {
        const BakedReactionTable table = test::MakePhaseLayerTable();
        TestCopy(table);
        TestPlaced(table);
        TestState(table);
        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "phase_layer_tree: {} 件の失敗", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "phase_layer_tree: すべて合格");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
