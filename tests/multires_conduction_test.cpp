// multires_conduction_test.cpp — 多重解像度の木の上の熱の伝導(shaders/common/multires_conduction.hlsli・sim::StepNest / StepActive の
// options.conduction)の CPU のテスト(T-0019)。確かめること:
//   - 面の隣: 刻むセルの 6 面の先が、面の中心のすぐ外の点を含む刻むセル(同じレベルか粗い)・細かい側に覆われたセル・世界の外のどれかで、
//     粗い側とのレベルの差が合う(たくさんの要求の場面。総当たりの幾何で確かめる)
//   - 閉じた箱(根 1 つ)の熱い 1 セルの熱が広がり、エネルギーの合計が毎刻み一定、温度の幅が縮む
//   - 鎖の場面(根 2×2×2 の角に 6 段の鎖・いちばん細かいブロックが熱い): 熱がレベルをまたいで根まで届き、「世界 + 帳簿」の
//     保存量が最初とビット一致(4 刻みごと)、活性だけ刻んでも全部刻んだ時とビット一致。端数の枠が無いと整数の単位の倍数だけ送る(数える)
//   - たくさんの要求の場面(深さ 26 段まで細かく/粗く・木箱が燃える・影)に伝導を入れても、活性 = 全部・保存量が一致・2 回の実行で一致
// 計測(ログ): 面の係数が熱容量の上限(1/8)で止まるレベル(T-0108 の材料)・静かになった後に残る頁と温度の幅(T-0104 の材料)。
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string_view>
#include <vector>

#include "common/multires_conduction.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "multires_activity_scene.h"
#include "multires_conduction_scene.h"
#include "sim/multires_nest.h"
#include "sim/reaction_test_table.h"

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
    constexpr uint64_t TOTALS_EVERY = 4;  // 保存量の合計(256bit。debug で重い)を確かめる刻みの間隔(最後の刻みも)
    using test::CHAIN_LEVELS;
    using test::CONDUCTION_SEED;
    using test::MakeChainNest;

    // --- 場面の道具 ---

    RxCell MakeAir(const BakedReactionTable& table, int32_t millikelvin) {
        return test::MakeConductionAir(table, millikelvin);
    }

    uint32_t MaxLevel(const MultiresNest& nest) {
        int32_t level = nest.capacity.rootLevel;
        for (const MrBlock& block : nest.blocks) {
            if (block.kind == MR_BLOCK_REAL)
                level = std::max(level, block.level);
        }

        return static_cast<uint32_t>(level);
    }

    using test::CountFractionBlocks;
    using test::RealTemperatures;
    using test::SameWorld;
    using test::TemperatureRange;

    // --- 面の隣(総当たりの幾何)---

    // multires_activity.hlsli の Tree の約束(索引で引く)
    struct TestTree {
        const MultiresNest* nest = nullptr;

        [[nodiscard]] MrBlock Block(uint32_t slot) const { return nest->blocks[slot]; }

        [[nodiscard]] uint32_t Lookup(int32_t level, int64_t originX, int64_t originY, int64_t originZ) const {
            return LookupBlock(*nest, level, originX, originY, originZ);
        }
    };

    // セルの箱(レベル finest の単位)
    struct CellBox {
        std::array<int64_t, 3> low{};
        int64_t size = 0;
    };

    CellBox BoxOf(const MrBlock& block, uint32_t index, uint32_t finest) {
        const auto shift = static_cast<uint32_t>(static_cast<int32_t>(finest) - block.level);
        const std::array<int64_t, 3> cell = {block.originX + MrCellX(index), block.originY + MrCellY(index),
                                             block.originZ + MrCellZ(index)};

        return {.low = {cell[0] << shift, cell[1] << shift, cell[2] << shift}, .size = int64_t{1} << shift};
    }

    bool Contains(const CellBox& box, const std::array<int64_t, 3>& coordinate) {
        bool inside = true;
        for (uint32_t axis = 0; axis < 3; ++axis)
            inside = inside && coordinate[axis] >= box.low[axis] && coordinate[axis] < box.low[axis] + box.size;

        return inside;
    }

    // 面の中心のすぐ外の点(finest の単位。セルの一辺は 2 以上)
    std::array<int64_t, 3> JustOutside(const CellBox& box, uint32_t face) {
        const uint32_t axis = face >> 1;
        std::array<int64_t, 3> coordinate{};
        for (uint32_t i = 0; i < 3; ++i)
            coordinate[i] = box.low[i] + (box.size / 2);

        coordinate[axis] = (face & 1u) != 0 ? box.low[axis] + box.size : box.low[axis] - 1;

        return coordinate;
    }

    // 根のブロックのどれかの中か(根の箱 = 原点から一辺 8 セル)
    bool InsideRoots(const MultiresNest& nest, const std::array<int64_t, 3>& coordinate, uint32_t finest) {
        return std::ranges::any_of(nest.blocks, [&](const MrBlock& block) {
            if (block.kind != MR_BLOCK_REAL || block.parent != MR_NO_BLOCK)
                return false;

            const CellBox corner = BoxOf(block, 0, finest);
            const CellBox box = {.low = corner.low, .size = corner.size * MR_BLOCK_EDGE};

            return Contains(box, coordinate);
        });
    }

    struct NeighborStats {
        uint32_t mismatches = 0;
        uint32_t coarser = 0;
        uint32_t finer = 0;
        uint32_t maxGap = 0;
    };

    // 面 1 つの答えが幾何と合うか
    bool NeighborMatches(const MultiresNest& nest, const MrBlock& block, const MrFaceNeighbor& neighbor,
                         const std::array<int64_t, 3>& outside, uint32_t finest) {
        if (neighbor.kind == MR_NEIGHBOR_NONE)
            return !InsideRoots(nest, outside, finest);

        const MrBlock& other = nest.blocks[neighbor.slot];
        if (other.kind != MR_BLOCK_REAL || !Contains(BoxOf(other, neighbor.index, finest), outside))
            return false;

        if (neighbor.kind == MR_NEIGHBOR_FINER)
            return other.level == block.level && MrIsCoveredCell(other, neighbor.index);

        const auto expectedGap = neighbor.kind == MR_NEIGHBOR_SAME ? 0u : neighbor.gap;

        return MrIsSteppedCell(other, neighbor.index) &&
               other.level + static_cast<int32_t>(expectedGap) == block.level &&
               (neighbor.kind == MR_NEIGHBOR_SAME || neighbor.gap > 0);
    }

    // セル 1 つの 6 面を確かめて数える
    void CheckCellNeighbors(const MultiresNest& nest, uint32_t slot, uint32_t index, uint32_t finest,
                            NeighborStats& stats) {
        const TestTree tree{.nest = &nest};
        const MrBlock& block = nest.blocks[slot];
        const CellBox box = BoxOf(block, index, finest);
        for (uint32_t face = 0; face < MR_FACES; ++face) {
            const MrFaceNeighbor neighbor = MrFindFaceNeighbor(tree, slot, block, index, face, nest.capacity.rootLevel);
            stats.mismatches += NeighborMatches(nest, block, neighbor, JustOutside(box, face), finest) ? 0 : 1;
            stats.coarser += neighbor.kind == MR_NEIGHBOR_COARSER ? 1 : 0;
            stats.finer += neighbor.kind == MR_NEIGHBOR_FINER ? 1 : 0;
            stats.maxGap = std::max(stats.maxGap, neighbor.kind == MR_NEIGHBOR_COARSER ? neighbor.gap : 0u);
        }
    }

    NeighborStats CheckNeighborsOf(const MultiresNest& nest) {
        const uint32_t finest = MaxLevel(nest) + 1;
        NeighborStats stats;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            if (nest.blocks[slot].kind != MR_BLOCK_REAL)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (MrIsSteppedCell(nest.blocks[slot], index))
                    CheckCellNeighbors(nest, slot, index, finest, stats);
            }
        }

        return stats;
    }

    // たくさんの要求の場面を数十刻み進め、木の形が色々な時に面の隣を総当たりで確かめる
    void CheckNeighbors(const BakedReactionTable& table) {
        MultiresNest nest = test::MakeActivityNest(table);
        NeighborStats total;
        for (uint64_t tick = 0; tick < 48; ++tick) {
            test::BeginActivityTick(nest, tick, test::MakeStressRequests(nest, tick));
            test::EndActivityTick(nest, table, tick, true);
            if (tick % 6 != 5)
                continue;

            const NeighborStats stats = CheckNeighborsOf(nest);
            total.mismatches += stats.mismatches;
            total.coarser += stats.coarser;
            total.finer += stats.finer;
            total.maxGap = std::max(total.maxGap, stats.maxGap);
        }

        Expect(total.mismatches == 0, "面の隣: 総当たりの幾何と一致");
        Expect(total.coarser > 0 && total.finer > 0 && total.maxGap >= 2,
               "面の隣: 粗い側・細かい側・2 段以上の差の面がある(場面の確認)");
        Log(Channel::Sim, Level::Info, "面の隣: 粗い側の面 {}・細かい側の面 {}・最大のレベルの差 {}", total.coarser,
            total.finer, total.maxGap);
    }

    // --- 閉じた箱: 根 1 つ(外は断熱)の熱い 1 セル ---
    void CheckBox(const BakedReactionTable& table) {
        constexpr uint64_t BOX_TICKS = 200;
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(table, 1, 0, 0));
        std::vector<RxCell> cells(MR_BLOCK_CELLS, MakeAir(table, 300000));
        cells[MrCellIndex(3, 4, 5)] = MakeAir(table, 1500000);
        PlaceRootBlock(nest, 0, 0, 0, cells);

        const ReactionTableView view = table.View();
        const ConservedTotals initial = ComputeConservedTotals(nest, table, 0);
        const TemperatureRange start = RealTemperatures(nest, view);
        int32_t width = start.high - start.low;
        uint32_t drifts = 0;
        uint32_t widenings = 0;
        for (uint64_t tick = 0; tick < BOX_TICKS; ++tick) {
            StepNest(nest, table, CONDUCTION_SEED, tick, CONDUCTION);
            drifts += ComputeConservedTotals(nest, table, 0) == initial ? 0 : 1;

            const TemperatureRange range = RealTemperatures(nest, view);
            widenings += range.high - range.low > width ? 1 : 0;
            width = range.high - range.low;
        }

        Expect(drifts == 0, "閉じた箱: エネルギーの合計が毎刻み一定");
        Expect(widenings == 0, "閉じた箱: 温度の幅が広がらない");
        Expect(width * 4 < start.high - start.low, "閉じた箱: 温度の幅が 1/4 より小さくなった");
        Log(Channel::Sim, Level::Info, "閉じた箱: {} 刻みで温度の幅 {} → {} mK", BOX_TICKS, start.high - start.low,
            width);
    }

    // --- 鎖の場面(tests/multires_conduction_scene.h)---

    constexpr uint64_t CHAIN_TICKS = 100;

    // 根(レベル 0)のセルの最高の温度(mK)
    int32_t HottestRootCell(const MultiresNest& nest, const ReactionTableView& view) {
        int32_t hottest = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL || block.level != 0)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (MrIsSteppedCell(block, index))
                    hottest = std::max(hottest, RxComputeThermal(view, LoadNestCell(nest, slot, index)).temperature);
            }
        }

        return hottest;
    }

    // 活性だけ刻む木と全部刻む木を並べて進める。fractions = 端数の枠の数(0 なら整数の単位の倍数だけ送る)
    void CheckChain(const BakedReactionTable& table, uint32_t fractions) {
        MultiresNest active = MakeChainNest(table, fractions);
        MultiresNest full = MakeChainNest(table, fractions);
        const ConservedTotals initial = ComputeConservedTotals(active, table, CHAIN_LEVELS);
        uint32_t drifts = 0;
        uint32_t mismatches = 0;
        for (uint64_t tick = 0; tick < CHAIN_TICKS; ++tick) {
            StepActive(active, table, CONDUCTION_SEED, tick, CONDUCTION);
            StepNest(full, table, CONDUCTION_SEED, tick, CONDUCTION);
            mismatches += SameWorld(active, full) ? 0 : 1;
            if (tick % TOTALS_EVERY == 0 || tick + 1 == CHAIN_TICKS)
                drifts += ComputeConservedTotals(active, table, CHAIN_LEVELS) == initial ? 0 : 1;
        }

        const ReactionTableView view = table.View();
        const int32_t hottestRoot = HottestRootCell(active, view);
        const uint32_t fractionBlocks = CountFractionBlocks(active);
        const uint32_t shortage = active.counters[MR_COUNTER_FRACTION_SHORTAGE];
        const std::string_view label = fractions == 0 ? "鎖(端数の枠なし)" : "鎖";
        Expect(drifts == 0, std::format("{}: 「世界 + 帳簿」の保存量が最初とビット一致(4 刻みごと)", label));
        Expect(mismatches == 0, std::format("{}: 活性だけ刻んでも全部刻んだ時とビット一致", label));
        Expect(hottestRoot > 300000, std::format("{}: 熱がレベル 0 の根のセルまで届いた", label));
        Expect(active.counters[MR_COUNTER_PAGE_SHORTAGE] == 0, std::format("{}: 頁は足りている", label));
        if (fractions == 0)
            Expect(shortage > 0 && fractionBlocks == 0, "鎖(端数の枠なし): 端数の枠が足りないことを数えた");
        else
            Expect(shortage == 0 && fractionBlocks > 0, "鎖: 粗い側のブロックが端数の枠を持った");

        const TemperatureRange range = RealTemperatures(active, view);
        Log(Channel::Sim, Level::Info,
            "{}: {} 刻みで世界の温度 {}〜{} mK・根の最高 {} mK・端数のブロック {}・端数の不足 {}・頁 {}", label,
            CHAIN_TICKS, range.low, range.high, hottestRoot, fractionBlocks, shortage, UsedWorldPages(active));
    }

    // --- たくさんの要求の場面に伝導を入れる(深さ 26 段・木箱が燃える・影)---
    uint64_t RunStress(const BakedReactionTable& table, uint64_t ticks, bool check) {
        MultiresNest active = test::MakeActivityNest(table);
        MultiresNest full = test::MakeActivityNest(table);
        const ConservedTotals initial = ComputeConservedTotals(active, table, test::STRESS_MAX_LEVEL);
        uint32_t drifts = 0;
        uint32_t mismatches = 0;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            const std::vector<MrRequest> requests = test::MakeStressRequests(active, tick);
            test::BeginActivityTick(active, tick, requests);
            test::EndActivityTick(active, table, tick, true, CONDUCTION);
            if (!check)
                continue;

            test::BeginActivityTick(full, tick, requests);
            test::EndActivityTick(full, table, tick, false, CONDUCTION);
            mismatches += SameWorld(active, full) ? 0 : 1;
            if (tick % TOTALS_EVERY == 0 || tick + 1 == ticks)
                drifts += ComputeConservedTotals(active, table, test::STRESS_MAX_LEVEL) == initial ? 0 : 1;

            if (mismatches == 1 && !SameWorld(active, full))
                Log(Channel::Sim, Level::Error, "刻み {}: 活性だけ刻んだ時と全部刻んだ時が違う", tick);
        }

        if (check) {
            Expect(drifts == 0, "たくさんの要求: 「世界 + 帳簿」の保存量が最初とビット一致(4 刻みごと)");
            Expect(mismatches == 0, "たくさんの要求: 活性だけ刻んでも全部刻んだ時とビット一致");
            Expect(active.counters[MR_COUNTER_WAKE_TOO_DEEP] == 0, "たくさんの要求: 面をたどる再帰の上限に当たらない");
            Log(Channel::Sim, Level::Info, "たくさんの要求: {} 刻み・端数のブロック {}・端数の不足 {}・頁の不足 {}",
                ticks, CountFractionBlocks(active), active.counters[MR_COUNTER_FRACTION_SHORTAGE],
                active.counters[MR_COUNTER_PAGE_SHORTAGE]);
        }

        return HashWholeNest(active);
    }

    void CheckStress(const BakedReactionTable& table) {
        constexpr uint64_t STRESS_TICKS = 20;
        const uint64_t first = RunStress(table, STRESS_TICKS, true);
        const uint64_t second = RunStress(table, STRESS_TICKS, false);
        Expect(first == second, "たくさんの要求: 2 回の実行で全部が一致");
    }

    // --- 計測 ---

    // 面の係数が熱容量の上限(1/8)で止まる最初のレベル。scale = 試験の表の伝導率の倍率(現実の値 ≒ 試験の値 ÷ scale)
    int32_t CapacityBoundLevel(const MrThermal& thermal, uint32_t scale) {
        constexpr int32_t LOWEST = -4;
        constexpr int32_t HIGHEST = 40;
        for (int32_t level = LOWEST; level <= HIGHEST; ++level) {
            if (MrConductanceLimit(thermal.conductance / scale, level) >= thermal.capacityLimit)
                return level;
        }

        return HIGHEST + 1;
    }

    void LogCapacityBound(const BakedReactionTable& table) {
        const ReactionTableView view = table.View();
        const MrThermal air = MrCellThermal(view, MakeAir(table, 300000));
        const MrThermal crate = MrCellThermal(view, test::MakeMultiresRootCells(table)[MrCellIndex(3, 4, 5)]);
        Log(Channel::Sim, Level::Info,
            "面の係数が熱容量の上限(1/8)で止まる最初のレベル: 空気 {}(現実の伝導率なら {})・木箱 {}(現実なら約 {})",
            CapacityBoundLevel(air, 1), CapacityBoundLevel(air, 5000), CapacityBoundLevel(crate, 1),
            CapacityBoundLevel(crate, 20000));
    }

    // 頁を持つ本物のブロックの数と、ブロックの中の温度・エネルギーの幅の最大
    struct PageSpread {
        uint32_t pagedBlocks = 0;
        int32_t temperature = 0;  // mK
        int64_t energy = 0;       // そのレベルの単位
    };

    PageSpread MeasurePageSpread(const MultiresNest& nest, const ReactionTableView& view) {
        PageSpread spread;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL || MrIsUniform(block))
                continue;

            TemperatureRange temperatures;
            int64_t lowEnergy = INT64_MAX;
            int64_t highEnergy = INT64_MIN;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (!MrIsSteppedCell(block, index))
                    continue;

                const RxCell cell = LoadNestCell(nest, slot, index);
                const int32_t temperature = RxComputeThermal(view, cell).temperature;
                temperatures.low = std::min(temperatures.low, temperature);
                temperatures.high = std::max(temperatures.high, temperature);
                lowEnergy = std::min(lowEnergy, cell.energy);
                highEnergy = std::max(highEnergy, cell.energy);
            }

            spread.pagedBlocks += 1;
            spread.temperature = std::max(spread.temperature, temperatures.high - temperatures.low);
            spread.energy = std::max(spread.energy, highEnergy - lowEnergy);
        }

        return spread;
    }

    // 鎖の場面を、頁を畳む・静かな葉を粗くするも入れて進め、静かになった後に残る頁と頁の中の幅を測る(T-0104 の材料)
    void MeasurePageResidue(const BakedReactionTable& table) {
        constexpr uint64_t RESIDUE_TICKS = 600;
        constexpr uint64_t REPORT_EVERY = 200;
        MultiresNest nest = MakeChainNest(table, 16);
        const ReactionTableView view = table.View();
        const ConservedTotals initial = ComputeConservedTotals(nest, table, CHAIN_LEVELS);
        for (uint64_t tick = 0; tick < RESIDUE_TICKS; ++tick) {
            FoldQuietPages(nest, tick);
            SubmitQuietCoarsenRequests(nest, tick);
            ProcessRequests(nest);
            StepActive(nest, table, CONDUCTION_SEED, tick, CONDUCTION);
            if (tick % REPORT_EVERY != REPORT_EVERY - 1)
                continue;

            uint32_t realBlocks = 0;
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot)
                realBlocks += nest.blocks[slot].kind == MR_BLOCK_REAL ? 1 : 0;

            const PageSpread spread = MeasurePageSpread(nest, view);
            Log(Channel::Sim, Level::Info,
                "頁の残り方: 刻み {}・本物のブロック {}・頁 {}・端数のブロック {}・種 {}・頁の中の幅 最大 {} mK / {} "
                "単位",
                tick + 1, realBlocks, UsedWorldPages(nest), CountFractionBlocks(nest), SeedSlots(nest).size(),
                spread.temperature, spread.energy);
        }

        Expect(ComputeConservedTotals(nest, table, CHAIN_LEVELS) == initial,
               "頁の残り方: 畳む・粗くするを入れても保存量が最初とビット一致");
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "multires_conduction_test: FAILED(表を作れない)");
            return 1;
        }

        CheckNeighbors(*table);
        CheckBox(*table);
        CheckChain(*table, 16);
        CheckChain(*table, 0);
        CheckStress(*table);
        LogCapacityBound(*table);
        MeasurePageResidue(*table);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_conduction_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_conduction_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
