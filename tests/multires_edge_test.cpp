// multires_edge_test.cpp — 世界の端(shaders/common/world_edge.hlsli・sim::MultiresNest::edges。T-0721・ADR-0337)の CPU のテスト。
// 流れの種類は熱の伝導(陽解法)。確かめること:
//   - 端が全部 wall(既定)なら、箱と外の柱を入れても入れなくても刻んだ結果がビット一致(端を流さない = T-0721 より前と同じ)
//   - 上だけ open の根 1 つ(400 K)と、上の外の行(レベル 0 の z = 8)だけ 250 K の外の柱: 熱が上の端から逃げ、上の層が下の層より冷たい。
//     保存の検査(ComputeConservedTotals)が帳簿込みで毎刻み一定・帳簿は「世界の端・出」にだけ載る
//   - 外の柱の高さの読み方: 冷たい行が世界の中の高さ(z = 7)なら上の外(z = 8 の行 = 400 K)とは釣り合っていて流れない
//   - 上の端に接して 2 段細かくした場面(端の面がレベル 0〜2): 保存量が毎刻み一定・活性だけ刻んでも全部刻んだ時とビット一致
//   - 横が open で外の柱が空(真空): 熱は伝わらず、全部 wall と同じ
#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "common/world_edge.hlsli"
#include "common/world_ledger.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "multires_conduction_scene.h"
#include "multires_test_scene.h"
#include "reaction_test_table.h"
#include "sim/multires_nest.h"

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

    constexpr MultiresStepOptions CONDUCTION = {.conduction = true};
    constexpr uint64_t EDGE_SEED = 20261017;
    constexpr int32_t WORLD_MK = 400000;  // 世界の空気の温度
    constexpr int32_t COLD_MK = 250000;   // 冷たい外の行の温度
    constexpr uint32_t COLUMN_ROWS = 10;  // 外の柱の行(レベル 0 の z = 0〜9。世界は 0〜7・上の外は 8)
    constexpr int64_t TOP_OUTSIDE_ROW = 8;
    constexpr int32_t REFINED_LEVELS = 2;

    // --- 場面の道具 ---

    RxCell MakeAir(const BakedReactionTable& table, int32_t millikelvin) {
        return test::MakeConductionAir(table, millikelvin);
    }

    // 根 1 つ(原点。一様な 400 K・hot なら (3, 4, 5) だけ 1500 K)。世界の箱 = 根の箱・端は全部 wall
    MultiresNest MakeRootNest(const BakedReactionTable& table, uint32_t worldBlocks, uint32_t fractions, bool hot) {
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(table, worldBlocks, 0, fractions));
        std::vector<RxCell> cells(MR_BLOCK_CELLS, MakeAir(table, WORLD_MK));
        if (hot)
            cells[MrCellIndex(3, 4, 5)] = MakeAir(table, 1500000);

        PlaceRootBlock(nest, 0, 0, 0, cells);
        nest.edges.world = {.rootLevel = 0,
                            .lowX = 0,
                            .lowY = 0,
                            .lowZ = 0,
                            .highX = MR_BLOCK_EDGE,
                            .highY = MR_BLOCK_EDGE,
                            .highZ = MR_BLOCK_EDGE,
                            .top = WE_WALL,
                            .bottom = WE_WALL,
                            .sides = WE_WALL};

        return nest;
    }

    // 外の柱(行 coldRow だけ 250 K・ほかは世界と同じ 400 K)
    void SetColumn(MultiresNest& nest, const BakedReactionTable& table, int64_t coldRow) {
        nest.edges.columnBaseZ = 0;
        nest.edges.column.assign(COLUMN_ROWS, MakeAir(table, WORLD_MK));
        nest.edges.column[static_cast<size_t>(coldRow)] = MakeAir(table, COLD_MK);
    }

    // 帳簿の欄 slot(WlSlot)に 0 でない語があるか
    bool LedgerSlotUsed(const MultiresNest& nest, uint32_t slot) {
        const size_t words = size_t{MR_LEDGER_LEVELS} * nest.capacity.ledgerColumns;
        const auto first = nest.ledger.begin() + static_cast<std::ptrdiff_t>(slot * words);

        return std::any_of(first, first + static_cast<std::ptrdiff_t>(words), [](uint64_t word) { return word != 0; });
    }

    // 「世界の端・出」のほかの欄が全部 0 か
    bool OnlyEdgeOutUsed(const MultiresNest& nest) {
        const uint32_t edgeOut = WlSlot(WL_REASON_WORLD_EDGE, WL_DIRECTION_OUT);
        for (uint32_t slot = 0; slot < WL_SLOT_COUNT; ++slot) {
            if (slot != edgeOut && LedgerSlotUsed(nest, slot))
                return false;
        }

        return LedgerSlotUsed(nest, edgeOut);
    }

    // 根の層 z(レベル 0)の平均の温度(mK)
    int64_t LayerTemperature(const MultiresNest& nest, const ReactionTableView& view, uint32_t z) {
        const uint32_t root = LookupBlock(nest, 0, 0, 0, 0);
        int64_t sum = 0;
        for (uint32_t y = 0; y < MR_BLOCK_EDGE; ++y) {
            for (uint32_t x = 0; x < MR_BLOCK_EDGE; ++x)
                sum += MrCellThermal(view, LoadNestCell(nest, root, MrCellIndex(x, y, z))).temperature;
        }

        return sum / (MR_BLOCK_EDGE * MR_BLOCK_EDGE);
    }

    // ticks 刻み全部刻み、保存量(帳簿込み)が動いた刻みの数を返す
    uint32_t StepAndCountDrifts(MultiresNest& nest, const BakedReactionTable& table, uint64_t ticks, int32_t finest) {
        const ConservedTotals initial = ComputeConservedTotals(nest, table, finest);
        uint32_t drifts = 0;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            StepNest(nest, table, EDGE_SEED, tick, CONDUCTION);
            drifts += ComputeConservedTotals(nest, table, finest) == initial ? 0 : 1;
        }

        return drifts;
    }

    // --- 全部 wall は前と同じ ---
    void CheckWallsUnchanged(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 40;
        MultiresNest before = MakeRootNest(table, 1, 0, true);
        before.edges = {};
        MultiresNest walls = MakeRootNest(table, 1, 0, true);
        SetColumn(walls, table, TOP_OUTSIDE_ROW);
        for (uint64_t tick = 0; tick < TICKS; ++tick) {
            StepNest(before, table, EDGE_SEED, tick, CONDUCTION);
            StepNest(walls, table, EDGE_SEED, tick, CONDUCTION);
        }

        Expect(HashWholeNest(before) == HashWholeNest(walls), "全部 wall: 端の無い世界とビット一致");
        Expect(!LedgerSlotUsed(walls, WlSlot(WL_REASON_WORLD_EDGE, WL_DIRECTION_OUT)), "全部 wall: 帳簿の世界の端は 0");
    }

    // --- 上だけ open: 熱が上の端から逃げる ---
    void CheckTopOpen(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 60;
        MultiresNest nest = MakeRootNest(table, 1, 0, false);
        nest.edges.world.top = WE_OPEN;
        SetColumn(nest, table, TOP_OUTSIDE_ROW);

        const ReactionTableView view = table.View();
        const uint32_t drifts = StepAndCountDrifts(nest, table, TICKS, 0);
        const int64_t top = LayerTemperature(nest, view, MR_BLOCK_EDGE - 1);
        const int64_t bottom = LayerTemperature(nest, view, 0);

        Expect(drifts == 0, "上だけ open: 保存量(帳簿込み)が毎刻み一定");
        Expect(top < bottom && top < WORLD_MK, "上だけ open: 上の層が冷えて下の層より冷たい");
        Expect(OnlyEdgeOutUsed(nest), "上だけ open: 帳簿は世界の端・出にだけ載る");
        Log(Channel::Sim, Level::Info, "上だけ open: {} 刻みで上の層 {} mK・下の層 {} mK", TICKS, top, bottom);
    }

    // --- 外の柱の高さの読み方: 冷たい行が世界の中の高さなら上の外とは釣り合う ---
    void CheckColumnHeight(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 8;
        MultiresNest nest = MakeRootNest(table, 1, 0, false);
        nest.edges.world.top = WE_OPEN;
        SetColumn(nest, table, TOP_OUTSIDE_ROW - 1);
        const uint64_t start = HashRealLeaves(nest);
        for (uint64_t tick = 0; tick < TICKS; ++tick)
            StepNest(nest, table, EDGE_SEED, tick, CONDUCTION);

        Expect(HashRealLeaves(nest) == start, "外の柱の高さ: 上の外は z = 8 の行(400 K)を読み、流れない");
        Expect(!LedgerSlotUsed(nest, WlSlot(WL_REASON_WORLD_EDGE, WL_DIRECTION_OUT)), "外の柱の高さ: 帳簿は 0");
    }

    // 上の端に接して 2 段細かくした場面(上の端の面がレベル 0・1・2 にある)
    MultiresNest MakeRefinedNest(const BakedReactionTable& table) {
        MultiresNest nest = MakeRootNest(table, 8, 8, false);
        nest.edges.world.top = WE_OPEN;
        SetColumn(nest, table, TOP_OUTSIDE_ROW);
        const int64_t topCell = (int64_t{MR_BLOCK_EDGE} << REFINED_LEVELS) - 1;
        const MrRequest request = MrMakeRequest(MR_REQUEST_REFINE, REFINED_LEVELS, 13, 14, topCell);
        SubmitRequests(nest, std::span(&request, 1));
        ProcessRequests(nest);
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot)
            nest.seeds[slot] = nest.blocks[slot].kind == MR_BLOCK_REAL ? 1 : 0;

        return nest;
    }

    // --- 細かくした場面: 保存量・活性 = 全部 ---
    void CheckRefined(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 40;
        MultiresNest full = MakeRefinedNest(table);
        MultiresNest active = MakeRefinedNest(table);
        Expect(LookupBlock(full, REFINED_LEVELS, 8, 8, 24) != MR_NO_BLOCK,
               "細かくした場面: レベル 2 のブロックが上の端に接する");

        const ConservedTotals initial = ComputeConservedTotals(full, table, REFINED_LEVELS);
        uint32_t drifts = 0;
        uint32_t differences = 0;
        for (uint64_t tick = 0; tick < TICKS; ++tick) {
            StepNest(full, table, EDGE_SEED, tick, CONDUCTION);
            StepActive(active, table, EDGE_SEED, tick, CONDUCTION);
            drifts += ComputeConservedTotals(full, table, REFINED_LEVELS) == initial ? 0 : 1;
            differences += test::SameWorld(full, active) ? 0 : 1;
        }

        Expect(drifts == 0, "細かくした場面: 保存量(帳簿込み)が毎刻み一定");
        Expect(differences == 0, "細かくした場面: 活性だけ刻んでも全部刻んだ時とビット一致");
        Expect(OnlyEdgeOutUsed(full), "細かくした場面: 帳簿は世界の端・出にだけ載る");
    }

    // --- 横が open で外が真空(柱が空): 全部 wall と同じ ---
    void CheckVacuum(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 20;
        MultiresNest walls = MakeRootNest(table, 1, 0, true);
        MultiresNest vacuum = MakeRootNest(table, 1, 0, true);
        vacuum.edges.world.sides = WE_OPEN;
        vacuum.edges.world.top = WE_OPEN;
        for (uint64_t tick = 0; tick < TICKS; ++tick) {
            StepNest(walls, table, EDGE_SEED, tick, CONDUCTION);
            StepNest(vacuum, table, EDGE_SEED, tick, CONDUCTION);
        }

        Expect(HashWholeNest(walls) == HashWholeNest(vacuum), "真空の外: 熱は伝わらず全部 wall とビット一致");
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "multires_edge_test: FAILED(表を作れない)");
            return 1;
        }

        CheckWallsUnchanged(*table);
        CheckTopOpen(*table);
        CheckColumnHeight(*table);
        CheckRefined(*table);
        CheckVacuum(*table);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_edge_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_edge_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
