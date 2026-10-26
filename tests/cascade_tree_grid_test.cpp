// cascade_tree_grid_test.cpp — 世界の中の光(RC)の木を読む写し(sim::MakeCascadeTreeGrid・FoldCascadeToLeaves。T-1155)を確かめる。
//
// 場面: 石の地面(1 m)+ 熱い石の塊(2 m 角・900 K)+ 空気の 16 × 8 × 16 m の世界を、
//       A = 1 m の根だけ / B = 0.5 m の根だけ(A の各セルを同じ数の 0.5 m のセル 8 つに割った世界)/ C = A の一部を 0.5 m に細かくした木 で作る。
// 確かめること:
//   1. A と B の写し(光の値・温度)がビット単位で同じ(粗い葉を 0.5 m に広げる = 同じ数のセルに割った世界。T-1117 の段取り 3 の性質)
//   2. RC を 3 刻み流した A と B の結果(セルごとの吸った・放った・持ち越し)が同じで、A の葉ごとの和 = B の 8 セルの和
//   3. 葉ごとに足した和 = 升目の和(帯ごと・ビット単位)。C(粗い葉と 0.5 m の葉が混ざった木)でも
//   4. 0.5 m の根の木は前の写し(MakeCascadeGrid)と同じ。0.5 m より細かい葉の木は誤り(配る規則は土台)
#include <array>
#include <cstdio>
#include <span>
#include <vector>

#include "sim/cascade_light.h"
#include "sun_column_scene.h"

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    namespace mr = bicameral::multires;
    namespace rx = bicameral::reaction;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr int32_t HOT_MK = 900000;

    // 1 m の座標 → セル(石の地面・熱い石の塊・空気)
    rx::RxCell CellAt(const BakedReactionTable& table, int64_t x, int64_t y, int64_t z) {
        constexpr uint64_t SOLID_MICROMOLES = 20000000000;
        const bool hot = x >= 3 && x < 5 && y >= 1 && y < 3 && z >= 3 && z < 5;
        if (y < 1 || hot) {
            const std::vector<SpeciesAmount> solid = {
                {.species = table.SpeciesId("sun_test_stone"), .amount = SOLID_MICROMOLES}};

            return MakeReactionCell(table, solid, hot ? HOT_MK : test::SUN_ROOM_TEMPERATURE_MK);
        }

        return test::MakeSunCell(table, test::SunMaterial::Air);
    }

    // 根のレベル level(−1 か 0)の根を並べた世界。refine なら根 0 の低い角の八分の一を 0.5 m に細かくする(level −1 の時だけ)
    MultiresNest MakeWorld(const BakedReactionTable& table, int32_t level, std::array<uint32_t, 3> roots,
                           bool refine = false) {
        const uint32_t count = roots[0] * roots[1] * roots[2];
        const uint32_t blocks = count + 8;
        MultiresNest nest = MakeMultiresNest({.worldBlocks = blocks,
                                              .observerBlocks = 0,
                                              .fractions = 9,
                                              .pages = blocks,
                                              .indexEntries = std::bit_ceil(2 * blocks),
                                              .ledgerColumns = 1 + static_cast<uint32_t>(table.species.size()),
                                              .rootLevel = level});
        const auto toMeters = static_cast<uint32_t>(level + 1);  // セルの座標 → 1 m の座標
        std::vector<rx::RxCell> cells(mr::MR_BLOCK_CELLS);
        for (uint32_t rootZ = 0; rootZ < roots[2]; ++rootZ) {
            for (uint32_t rootY = 0; rootY < roots[1]; ++rootY) {
                for (uint32_t rootX = 0; rootX < roots[0]; ++rootX) {
                    for (uint32_t index = 0; index < mr::MR_BLOCK_CELLS; ++index) {
                        const int64_t x = (int64_t{rootX} * 8) + mr::MrCellX(index);
                        const int64_t y = (int64_t{rootY} * 8) + mr::MrCellY(index);
                        const int64_t z = (int64_t{rootZ} * 8) + mr::MrCellZ(index);
                        cells[index] = CellAt(table, x >> toMeters, y >> toMeters, z >> toMeters);
                    }

                    PlaceRootBlock(nest, int64_t{rootX} * 8, int64_t{rootY} * 8, int64_t{rootZ} * 8, cells);
                }
            }
        }

        nest.edges.world = {.rootLevel = level,
                            .lowX = 0,
                            .lowY = 0,
                            .lowZ = 0,
                            .highX = int64_t{roots[0]} * 8,
                            .highY = int64_t{roots[1]} * 8,
                            .highZ = int64_t{roots[2]} * 8,
                            .top = 0,
                            .bottom = 0,
                            .sides = 0};
        if (refine) {
            const mr::MrRequest request = mr::MrMakeRequest(mr::MR_REQUEST_REFINE, level + 1, 2, 2, 2);
            SubmitRequests(nest, std::span(&request, 1));
            ProcessRequests(nest);
        }

        return nest;
    }

    bool SameGrid(const CascadeGrid& a, const CascadeGrid& b) {
        if (a.low != b.low || a.size != b.size || a.temperatureMk != b.temperatureMk ||
            a.lights.size() != b.lights.size())
            return false;

        for (size_t at = 0; at < a.lights.size(); ++at) {
            const rx::RcCellLight& left = a.lights[at];
            const rx::RcCellLight& right = b.lights[at];
            if (left.depthShortQ32 != right.depthShortQ32 || left.depthLongQ32 != right.depthLongQ32 ||
                left.reflectanceShortQ16 != right.reflectanceShortQ16 ||
                left.reflectanceLongQ16 != right.reflectanceLongQ16)
                return false;
        }

        return true;
    }

    struct Ticks {
        std::vector<CascadeTickResult> results;
    };

    Ticks RunTicks(const CascadeGrid& grid, uint64_t ticks) {
        Ticks run;
        CascadeCarry carry;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            run.results.push_back(StepCascadeLight(grid, carry, CascadeParams{}, tick));
            carry = run.results.back().carry;
        }

        return run;
    }

    // 葉ごとの和 = 升目の和(帯ごと)
    bool FoldConserves(const CascadeTreeGrid& tree, const CascadeTickResult& result) {
        std::array<uint64_t, rx::LT_BANDS> absorbed{};
        std::array<uint64_t, rx::LT_BANDS> emitted{};
        for (const CascadeLeafNet& leaf : FoldCascadeToLeaves(tree, result)) {
            for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
                absorbed[band] += leaf.absorbed[band];
                emitted[band] += leaf.emitted[band];
            }
        }

        for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
            if (absorbed[band] != result.ledger[band].absorbed || emitted[band] != result.ledger[band].emitted)
                return false;
        }

        return true;
    }

    // A の葉(1 m のセル)ごとの和 = B の升目の 2 × 2 × 2 の和
    bool CoarseLeavesMatchFine(const MultiresNest& coarse, const CascadeTreeGrid& coarseTree,
                               const CascadeTickResult& coarseResult, const CascadeGrid& fineGrid,
                               const CascadeTickResult& fineResult) {
        for (const CascadeLeafNet& leaf : FoldCascadeToLeaves(coarseTree, coarseResult)) {
            const mr::MrBlock& block = coarse.blocks[leaf.slot];
            const int64_t x0 = (block.originX + mr::MrCellX(leaf.index)) << 1;
            const int64_t y0 = (block.originY + mr::MrCellY(leaf.index)) << 1;
            const int64_t z0 = (block.originZ + mr::MrCellZ(leaf.index)) << 1;
            std::array<uint64_t, rx::LT_BANDS> absorbed{};
            std::array<uint64_t, rx::LT_BANDS> emitted{};
            for (int64_t z = z0; z < z0 + 2; ++z) {
                for (int64_t y = y0; y < y0 + 2; ++y) {
                    for (int64_t x = x0; x < x0 + 2; ++x) {
                        const size_t at = fineGrid.IndexOf(x, y, z);
                        for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
                            absorbed[band] += fineResult.absorbed[(at * rx::LT_BANDS) + band];
                            emitted[band] += fineResult.emitted[(at * rx::LT_BANDS) + band];
                        }
                    }
                }
            }

            if (absorbed != leaf.absorbed || emitted != leaf.emitted)
                return false;
        }

        return true;
    }

    void TestCoarseEqualsSplit(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 3;
        const MultiresNest coarse = MakeWorld(table, -1, {2, 1, 2});
        const MultiresNest fine = MakeWorld(table, 0, {4, 2, 4});
        const auto coarseTree = MakeCascadeTreeGrid(coarse, table);
        const auto fineTree = MakeCascadeTreeGrid(fine, table);
        EXPECT(coarseTree.has_value());
        EXPECT(fineTree.has_value());
        if (!coarseTree || !fineTree)
            return;

        EXPECT(SameGrid(coarseTree->grid, fineTree->grid));
        const auto oldGrid = MakeCascadeGrid(fine, table);
        EXPECT(oldGrid.has_value() && SameGrid(*oldGrid, fineTree->grid));

        const Ticks coarseRun = RunTicks(coarseTree->grid, TICKS);
        const Ticks fineRun = RunTicks(fineTree->grid, TICKS);
        uint64_t emitted = 0;
        for (uint64_t tick = 0; tick < TICKS; ++tick) {
            const CascadeTickResult& left = coarseRun.results[tick];
            const CascadeTickResult& right = fineRun.results[tick];
            EXPECT(left.absorbed == right.absorbed && left.emitted == right.emitted && left.carry == right.carry);
            EXPECT(FoldConserves(*coarseTree, left));
            EXPECT(CoarseLeavesMatchFine(coarse, *coarseTree, left, fineTree->grid, right));
            for (const CascadeBandLedger& band : left.ledger)
                emitted += band.emitted;
        }

        EXPECT(emitted > 0);
        std::printf("1 m の根 / 0.5 m の根: 升目 %zu・葉 %zu / %zu(1 刻み目)・%llu 刻みに放った %llu\n",
                    coarseTree->grid.CellCount(), FoldCascadeToLeaves(*coarseTree, coarseRun.results[0]).size(),
                    FoldCascadeToLeaves(*fineTree, fineRun.results[0]).size(), static_cast<unsigned long long>(TICKS),
                    static_cast<unsigned long long>(emitted));
    }

    void TestMixedTree(const BakedReactionTable& table) {
        const MultiresNest coarse = MakeWorld(table, -1, {2, 1, 2});
        const MultiresNest mixed = MakeWorld(table, -1, {2, 1, 2}, true);
        uint32_t fineBlocks = 0;
        for (uint32_t slot = 0; slot < mixed.capacity.worldBlocks; ++slot)
            fineBlocks += mixed.blocks[slot].kind == mr::MR_BLOCK_REAL && mixed.blocks[slot].level == 0 ? 1u : 0u;

        EXPECT(fineBlocks > 0);
        const auto mixedTree = MakeCascadeTreeGrid(mixed, table);
        const auto coarseTree = MakeCascadeTreeGrid(coarse, table);
        EXPECT(mixedTree.has_value() && coarseTree.has_value());
        if (!mixedTree || !coarseTree)
            return;

        const Ticks run = RunTicks(mixedTree->grid, 2);
        EXPECT(FoldConserves(*mixedTree, run.results[1]));
        std::printf("混ざった木(0.5 m のブロック %u): 写しが 1 m の根だけの木と%s\n", fineBlocks,
                    SameGrid(mixedTree->grid, coarseTree->grid) ? "同じ" : "違う(細かくする時の割り方)");
    }

    void TestRefusesFinerLeaves(const BakedReactionTable& table) {
        const MultiresNest finer = test::MakeSunWorld(table, 1, 1, 1, 1, test::FlatGround);
        EXPECT(!MakeCascadeTreeGrid(finer, table).has_value());
    }

}  // namespace

int main() {
    const BakedReactionTable table = test::MakeSunTable();
    TestCoarseEqualsSplit(table);
    TestMixedTree(table);
    TestRefusesFinerLeaves(table);
    if (failureCount != 0)
        return 1;

    std::printf("cascade_tree_grid: OK\n");

    return 0;
}
