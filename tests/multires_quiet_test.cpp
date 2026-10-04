// multires_quiet_test.cpp — 静かなブロックを粗くする(sim::SubmitQuietCoarsenRequests・multires_activity.hlsli の MrWantsQuietCoarsen)の
// CPU のテスト(T-0101)。場面は tests/multires_quiet_scene.h(木箱と静かな空気の周りを細かくし、粗くするのは静かな葉の要求だけ)。毎刻み:
//   - 粗くしたブロックは、直前の N 刻みに変わっていなかった(テストが毎刻みの写しと比べて自分で数えた刻みで確かめる)
//   - 「世界 + 帳簿」の保存量が最初とビット一致・索引で本物のブロックが全部引ける
// 終わりには根だけが残る(燃え尽きると木箱の鎖も畳まれる)。一覧が一杯の刻みには静かな葉が次へ回り、静かな兄弟は 1 つずつ要求を出す。2 回走らせて全部が一致(決定性)。
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "multires_quiet_scene.h"
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

    constexpr uint64_t NEVER = UINT64_MAX;

    struct QuietRun {
        uint64_t digest = 0;
        uint32_t coarsened = 0;  // 静かな葉を粗くした数
        uint32_t tooEarly = 0;   // 忙しかったのに粗くした数
        uint32_t conservationMismatches = 0;
        uint32_t indexMismatches = 0;
        uint64_t collapsedTick = NEVER;  // 根だけになった最初の刻み
        uint32_t maxRealBlocks = 0;
        uint32_t deferredAtStuff = 0;        // 一覧が一杯の刻みに次へ回した数
        uint32_t siblingsSkipped = 0;        // 一覧を埋めた次の刻みに、兄弟に譲って要求を出さなかった静かな葉の数
        std::vector<uint32_t> realBlocksAt;  // 刻み 20・60・100・150・200 の本物のブロックの数
    };

    // 静かな葉の数(兄弟に譲るものも数える)
    uint32_t CountQuietLeaves(const MultiresNest& nest, uint64_t tick) {
        uint32_t count = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot)
            count += MrIsQuietLeaf(nest.blocks[slot], MrActivityMark(tick)) ? 1 : 0;

        return count;
    }

    // 見出しの木の欄(活性と忙しさの印は除く)が同じか
    bool SameStructure(const MrBlock& a, const MrBlock& b) {
        return a.originX == b.originX && a.originY == b.originY && a.originZ == b.originZ && a.level == b.level &&
               a.kind == b.kind && a.parent == b.parent && a.parentOctant == b.parentOctant &&
               a.fraction == b.fraction && std::ranges::equal(a.children, b.children);
    }

    // 前の写しから見出しの木の欄かセルが変わった世界の枠を、「変わった刻み」に書く(実装の忙しさの印を使わない答え)
    void MarkChanged(const MultiresNest& before, const MultiresNest& after, uint64_t tick,
                     std::vector<uint64_t>& lastChanged) {
        for (uint32_t slot = 0; slot < after.capacity.worldBlocks; ++slot) {
            const auto first = static_cast<ptrdiff_t>(size_t{slot} * MR_BLOCK_CELLS);
            const bool cellsChanged = std::memcmp(&before.cells[first], &after.cells[first],
                                                  sizeof(RxCell) * MR_BLOCK_CELLS) != 0;
            if (cellsChanged || !SameStructure(before.blocks[slot], after.blocks[slot]))
                lastChanged[slot] = tick;
        }
    }

    // 粗くしたブロック(本物だった枠が本物でなくなった)は、直前の N 刻みに変わっていなかった
    void CheckCoarsened(const MultiresNest& before, const MultiresNest& after, uint64_t tick,
                        std::span<const uint64_t> lastChanged, QuietRun& run) {
        for (uint32_t slot = 0; slot < after.capacity.worldBlocks; ++slot) {
            const bool removed = before.blocks[slot].kind == MR_BLOCK_REAL && after.blocks[slot].kind != MR_BLOCK_REAL;
            if (!removed)
                continue;

            ++run.coarsened;
            const bool quiet = tick - lastChanged[slot] > MR_QUIET_TICKS;
            run.tooEarly += quiet ? 0 : 1;
            if (!quiet)
                Log(Channel::Sim, Level::Error, "刻み {}: 枠 {} は刻み {} に変わったのに粗くした", tick, slot,
                    lastChanged[slot]);
        }
    }

    // 索引と木の大きさ
    void RecordTree(const MultiresNest& nest, uint64_t tick, QuietRun& run) {
        run.indexMismatches += test::CountIndexMismatches(nest);
        const uint32_t real = test::CountRealBlocks(nest);
        run.maxRealBlocks = std::max(run.maxRealBlocks, real);
        if (tick == 20 || tick == 60 || tick == 100 || tick == 150 || tick == 200)
            run.realBlocksAt.push_back(real);

        if (real == test::ACTIVITY_ROOTS && run.collapsedTick == NEVER && tick > test::QUIET_REFINE_AGAIN_TICK)
            run.collapsedTick = tick;
    }

    void CheckRun(const MultiresNest& nest, const QuietRun& run) {
        Expect(run.tooEarly == 0, "変わったばかりのブロックを粗くしない");
        Expect(run.conservationMismatches == 0, "毎刻み「世界 + 帳簿」が最初とビット一致");
        Expect(run.indexMismatches == 0, "索引で本物のブロックが全部引ける");
        Expect(test::CountRealBlocks(nest) == test::ACTIVITY_ROOTS, "終わりには根だけが残る");
        Expect(run.deferredAtStuff > 0, "一覧が一杯の刻みに静かな葉が次へ回る(場面の確認)");
        Expect(run.siblingsSkipped > 0, "静かな兄弟は 1 つだけが要求を出す(場面の確認)");
        Expect(nest.counters[MR_COUNTER_CONFLICT] == 0, "静かな葉の要求どうしが同じ親を取り合わない");
        Expect(nest.counters[MR_COUNTER_QUIET_REQUESTS] >= run.coarsened, "粗くした数 ≦ 静かな葉の要求の数");
    }

    // check = false なら刻むだけ(2 回目の決定性の確認)
    QuietRun RunQuiet(const BakedReactionTable& table, bool check) {
        MultiresNest nest = test::MakeActivityNest(table);
        const ConservedTotals initial = ComputeConservedTotals(nest, table, test::QUIET_DEPTH);
        std::vector<uint64_t> lastChanged(nest.capacity.worldBlocks, 0);  // 根は刻み 0 に置いた

        QuietRun run;
        for (uint64_t tick = 0; tick < test::QUIET_TICKS; ++tick) {
            MultiresNest before;
            if (check)
                before = nest;

            // --- 一覧を埋めた刻みは静かな葉が次へ回り、その次の刻みには兄弟のうち 1 つだけが要求を出す ---
            const uint64_t releaseTick = test::QUIET_STUFF_TICK + test::QUIET_STUFF_TICKS;
            const uint32_t quietLeaves = tick == releaseTick ? CountQuietLeaves(nest, tick) : 0;
            const uint32_t deferred = nest.counters[MR_COUNTER_QUIET_DEFERRED];
            const uint32_t requested = nest.counters[MR_COUNTER_QUIET_REQUESTS];
            test::BeginQuietTick(nest, tick, test::QuietRequestsAt(tick));
            run.deferredAtStuff += nest.counters[MR_COUNTER_QUIET_DEFERRED] - deferred;
            if (tick == releaseTick)
                run.siblingsSkipped = quietLeaves - (nest.counters[MR_COUNTER_QUIET_REQUESTS] - requested);

            if (!check) {
                StepActive(nest, table, test::STRESS_SEED, tick);
                continue;
            }

            CheckCoarsened(before, nest, tick, lastChanged, run);
            MarkChanged(before, nest, tick, lastChanged);  // 木の変更
            before = nest;
            StepActive(nest, table, test::STRESS_SEED, tick);
            MarkChanged(before, nest, tick, lastChanged);  // 反応

            run.conservationMismatches += ComputeConservedTotals(nest, table, test::QUIET_DEPTH) == initial ? 0 : 1;
            RecordTree(nest, tick, run);
        }

        run.digest = HashWholeNest(nest);
        if (!check)
            return run;

        CheckRun(nest, run);

        return run;
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "multires_quiet_test: FAILED(表を作れない)");
            return 1;
        }

        const QuietRun first = RunQuiet(*table, true);
        const QuietRun second = RunQuiet(*table, false);
        Expect(first.digest == second.digest, "2 回の実行で全部が一致");

        std::string counts;
        for (const uint32_t count : first.realBlocksAt)
            counts += std::format(" {}", count);

        Log(Channel::Sim, Level::Info,
            "multires_quiet_test: {} 刻み・N = {}・粗くした {}・最大の本物のブロック {}・刻み 20/60/100/150/200 "
            "の本物のブロック{}・根だけになった刻み {}・一覧が一杯で回した {}・兄弟に譲った {}・要約 {:016x}",
            test::QUIET_TICKS, MR_QUIET_TICKS, first.coarsened, first.maxRealBlocks, counts, first.collapsedTick,
            first.deferredAtStuff, first.siblingsSkipped, first.digest);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_quiet_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_quiet_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
