// multires_activity_test.cpp — 多重解像度の木の上の活性(sim::StepActive・shaders/common/multires_activity.hlsli)の CPU のテスト(T-0100)。
// 場面は tests/multires_activity_scene.h(根 4×4×4・木箱の周りをたくさんの要求で細かく/粗く・静かな根に観察の影)。毎刻み:
//   - 刻んだブロックの集合 = 種とその面の隣を総当たり(覆われていない八分の一の箱どうしの面の接触)で求めた集合
//   - 活性のブロックだけ刻んだ木と、全部を刻んだ木(StepNest)のセル・端数がビット一致(活性の規則が刻むべき所を落とさない)
//   - 面をたどる再帰の上限に当たらない
// 2 回走らせて全部が一致(決定性)。
#include <cstdint>
#include <string_view>

#include "core/log.h"
#include "core/singleton.h"
#include "multires_activity_scene.h"
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

    struct ActivityRun {
        uint64_t digest = 0;
        uint64_t scheduled = 0;      // 刻んだブロックの数(全部の刻みの和)
        uint64_t realBlocks = 0;     // 本物のブロックの数(全部の刻みの和)
        uint64_t crossLevel = 0;     // 種と違うレベルの面の隣を起こした数(総当たりで数える)
        uint32_t lastScheduled = 0;  // 最後の刻みに刻んだ数
    };

    // 全部のセル(観察の枠も)と、世界の要約・帳簿・端数が一致するか
    bool SameCells(const MultiresNest& active, const MultiresNest& full) {
        for (size_t i = 0; i < active.cells.size(); ++i) {
            if (HashReactionCell(active.cells[i]) != HashReactionCell(full.cells[i]))
                return false;
        }

        return HashRealLeaves(active) == HashRealLeaves(full) && active.ledger == full.ledger;
    }

    uint32_t CountReal(const MultiresNest& nest) {
        uint32_t count = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot)
            count += nest.blocks[slot].kind == MR_BLOCK_REAL ? 1 : 0;

        return count;
    }

    // 種と違うレベルの、総当たりの面の隣の数
    uint64_t CountCrossLevel(const MultiresNest& nest, std::span<const uint32_t> seeds) {
        uint64_t count = 0;
        for (const uint32_t seed : seeds) {
            if (nest.blocks[seed].kind != MR_BLOCK_REAL)
                continue;

            const std::array<uint32_t, 1> one = {seed};
            const std::vector<uint8_t> near = test::BruteForceScheduled(nest, one);
            for (uint32_t slot = 0; slot < near.size(); ++slot)
                count += near[slot] != 0 && nest.blocks[slot].level != nest.blocks[seed].level ? 1 : 0;
        }

        return count;
    }

    // check = false なら活性だけ刻む(2 回目の決定性の確認。debug で遅いので比べる相手と総当たりを省く)
    ActivityRun RunActivity(const BakedReactionTable& table, bool check) {
        MultiresNest active = test::MakeActivityNest(table);
        MultiresNest full = test::MakeActivityNest(table);
        ActivityRun run;
        uint32_t scheduledMismatches = 0;
        uint32_t cellMismatches = 0;
        for (uint64_t tick = 0; tick < test::ACTIVITY_TICKS; ++tick) {
            const std::vector<MrRequest> requests = test::MakeStressRequests(active, tick);
            test::BeginActivityTick(active, tick, requests);
            if (!check) {
                test::EndActivityTick(active, table, tick, true);
                continue;
            }

            test::BeginActivityTick(full, tick, requests);

            // --- 種(要求の処理でつつかれた分も入る)と、総当たりの答え ---
            const std::vector<uint32_t> seeds = SeedSlots(active);
            const std::vector<uint8_t> expected = test::BruteForceScheduled(active, seeds);
            if (tick % 8 == 0)
                run.crossLevel += CountCrossLevel(active, seeds);

            test::EndActivityTick(active, table, tick, true);
            test::EndActivityTick(full, table, tick, false);

            const std::vector<uint8_t> scheduled = test::ScheduledAt(active, tick);
            scheduledMismatches += scheduled == expected ? 0 : 1;
            cellMismatches += SameCells(active, full) ? 0 : 1;
            run.lastScheduled = static_cast<uint32_t>(std::ranges::count(scheduled, uint8_t{1}));
            run.scheduled += run.lastScheduled;
            run.realBlocks += CountReal(active);
            if (scheduled != expected && scheduledMismatches == 1)
                Log(Channel::Sim, Level::Error, "刻み {}: 刻んだ集合が総当たりの面の隣と違う", tick);

            if (cellMismatches == 1 && !SameCells(active, full))
                Log(Channel::Sim, Level::Error, "刻み {}: 活性だけ刻んだセルが全部刻んだ時と違う", tick);
        }

        run.digest = HashWholeNest(active);
        if (!check)
            return run;

        Expect(scheduledMismatches == 0, "刻んだ集合 = 種とその面の隣(総当たり)");
        Expect(cellMismatches == 0, "活性だけ刻んでも全部刻んだ時とビット一致");
        Expect(active.counters[MR_COUNTER_WAKE_TOO_DEEP] == 0, "面をたどる再帰の上限に当たらない");
        Expect(active.counters[MR_COUNTER_SCHEDULED] == run.scheduled, "刻んだ数を数える欄が合う");
        Expect(run.crossLevel > 0, "レベルをまたぐ面の隣がある(場面の確認)");

        return run;
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "multires_activity_test: FAILED(表を作れない)");
            return 1;
        }

        const ActivityRun first = RunActivity(*table, true);
        const ActivityRun second = RunActivity(*table, false);
        Expect(first.digest == second.digest, "2 回の実行で全部が一致");

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_activity_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info,
            "multires_activity_test: OK({} 刻み・刻んだブロック {} / 本物のブロック {}・最後の刻み {}・"
            "レベルをまたぐ面の隣 {}・要約 {:016x})",
            test::ACTIVITY_TICKS, first.scheduled, first.realBlocks, first.lastScheduled, first.crossLevel,
            first.digest);

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
