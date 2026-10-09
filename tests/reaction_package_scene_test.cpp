// reaction_package_scene_test.cpp — ランタイムが読む反応表(データのフォルダのパッケージ。T-0157)で、今の場面がビット単位で
// 前と同じ結果になるかを CPU リファレンス(sim::ProbeReference。GPU とビット一致を別のテストが確かめている)で確かめる。
//
// 見るもの: exe の横の data/packages(ビルドがリポジトリの data/packages を写したもの)を、ランタイムと同じ入り口
//           (script::LoadReactionTable)で読んだ表と、C++ の試験の表(MakeCombustionTestTable。T-0157 の前にランタイムが使っていた)で、
//   - 仮の世界(空気の中の木箱。frame_loop の --auto-ignite と同じ壁に火をつける)を進め、刻みごとの状態のハッシュ・エネルギーの合計が同じ
//   - 最後の状態(全部のセルのバイト)と熱のキャッシュ・抽出(描画が読むもの)がバイトで同じ
//   - 反応が起きている(炭と CO2 ができた。燃える場面を比べている)
// 失敗すると失敗した条件を表示して 1 を返す(ctest が落ちる)。CPU だけ(GPU を使わない)。
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "core/aliases.h"
#include "script/reaction_table_loader.h"
#include "sim/probe_sim.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    constexpr uint64_t TOTAL_TICKS = 60;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    template <typename T>
    bool SameBytes(std::span<const T> left, std::span<const T> right) {
        return left.size() == right.size() &&
               (left.empty() || std::memcmp(left.data(), right.data(), left.size_bytes()) == 0);
    }

    // frame_loop の --auto-ignite と同じ木箱の壁 (28, 32, PROBE_VIEW_Z) に火をつけ、少し後にもう一度つつく(燃え広がる場面)
    std::vector<ProbeCommand> MakeCommands() {
        return {MakePokeCommand(0, 0, 28, 32, PROBE_VIEW_Z), MakePokeCommand(0, 1, 28, 33, PROBE_VIEW_Z),
                MakePokeCommand(7, 2, 28, 32, PROBE_VIEW_Z), MakePokeCommand(19, 3, 28, 31, PROBE_VIEW_Z)};
    }

    bool HasSpecies(const BakedReactionTable& table, std::span<const reaction::RxCell> cells, const char* name) {
        const uint32_t id = table.SpeciesId(name);

        return rng::any_of(cells, [&](const reaction::RxCell& cell) { return ProbeViewAmount(cell, id) > 0; });
    }

    // --- 同じ場面を 2 つの表で進めて比べる ---

    void TestSameScene(const BakedReactionTable& package, const BakedReactionTable& cpp) {
        const std::vector<ProbeCommand> commands = MakeCommands();
        ProbeReference fromPackage(package);
        ProbeReference fromCpp(cpp);

        const uint64_t initialHash = ProbeStateHash(
            fromPackage.State(0));  // State は 2 世代を使い回すので、先に取っておく
        size_t firstDifferentTick = SIZE_MAX;
        for (uint64_t tick = 0; tick <= TOTAL_TICKS; ++tick) {
            if (tick > 0) {
                fromPackage.Advance(tick - 1, commands);
                fromCpp.Advance(tick - 1, commands);
            }

            const bool same = ProbeStateHash(fromPackage.State(tick)) == ProbeStateHash(fromCpp.State(tick)) &&
                              ProbeEnergySum(fromPackage.State(tick)) == ProbeEnergySum(fromCpp.State(tick)) &&
                              fromPackage.ScheduledBlocks() == fromCpp.ScheduledBlocks();
            if (!same && firstDifferentTick == SIZE_MAX)
                firstDifferentTick = tick;
        }

        if (firstDifferentTick != SIZE_MAX)
            std::printf("  刻み %zu で初めて食い違った\n", firstDifferentTick);

        EXPECT(firstDifferentTick == SIZE_MAX);

        // --- 最後の状態・熱のキャッシュ・抽出がバイトで同じ ---
        const std::span<const reaction::RxCell> last = fromPackage.State(TOTAL_TICKS);
        EXPECT(SameBytes(last, fromCpp.State(TOTAL_TICKS)));
        EXPECT(SameBytes(fromPackage.Caches(TOTAL_TICKS), fromCpp.Caches(TOTAL_TICKS)));

        const std::vector<uint32_t> extraction = MakeProbeExtractionCells(last, fromPackage.Caches(TOTAL_TICKS),
                                                                          ProbeViewSpecies(package));
        const std::vector<uint32_t> cppExtraction = MakeProbeExtractionCells(
            fromCpp.State(TOTAL_TICKS), fromCpp.Caches(TOTAL_TICKS), ProbeViewSpecies(cpp));
        EXPECT(SameBytes(std::span<const uint32_t>(extraction), std::span<const uint32_t>(cppExtraction)));

        // --- 燃える場面を比べている(何も起きない世界どうしで一致しても意味がない)---
        EXPECT(HasSpecies(package, last, "carbon"));
        EXPECT(HasSpecies(package, last, "carbon_dioxide"));
        EXPECT(ProbeStateHash(last) != initialHash);

        std::printf("刻み %llu まで: 状態のハッシュ %016llx(パッケージの表と C++ の表で毎刻み同じ)\n",
                    static_cast<unsigned long long>(TOTAL_TICKS),
                    static_cast<unsigned long long>(ProbeStateHash(last)));
    }

}  // namespace

int main() {
    // ランタイム(frame_loop)と同じ入り口・同じ既定のフォルダ
    const auto loaded = script::LoadReactionTable({});
    const auto cpp = BakeReactionTable(MakeCombustionTestTable());
    if (!loaded || !cpp) {
        std::printf("FAILED: 表を作れない: %s / %s\n", loaded ? "" : loaded.error().c_str(),
                    cpp ? "" : cpp.error().c_str());

        return 1;
    }

    std::printf("パッケージのフォルダ: %s\n",
                reinterpret_cast<const char*>(loaded->packageRoot.generic_u8string().c_str()));
    EXPECT(loaded->rejected.empty());
    TestSameScene(loaded->table, *cpp);

    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("すべて通過\n");

    return 0;
}
