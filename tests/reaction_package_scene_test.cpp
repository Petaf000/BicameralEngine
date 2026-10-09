// reaction_package_scene_test.cpp — ランタイムが読む反応表(データのフォルダのパッケージ。T-0157)で、今の場面がビット単位で
// 前と同じ結果になるかを CPU リファレンス(sim::ProbeReference。GPU とビット一致を別のテストが確かめている)で確かめる。
//
// 見るもの: exe の横の data/packages(ビルドがリポジトリの data/packages を写したもの)を、ランタイムと同じ入り口
//           (script::LoadReactionTable)で読んだ表と、C++ の試験の表(MakeCombustionTestTable。T-0157 の前にランタイムが使っていた)で、
//   - 仮の世界(空気の中の木箱。frame_loop の --auto-ignite と同じ壁に火をつける)を進め、刻みごとの状態のハッシュ・エネルギーの合計が同じ
//   - 最後の状態(全部のセルのバイト)と熱のキャッシュ・抽出(描画が読むもの)がバイトで同じ
//   - 反応が起きている(炭と CO2 ができた。燃える場面を比べている)
// 失敗すると失敗した条件を表示して 1 を返す(ctest が落ちる)。CPU だけ(GPU を使わない)。
// `--distribution [刻み] [間引き]` は比べずに、同じ場面の成分の数と進む規則の数の分布を表示する(T-0163 の研究)。
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
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

    // --- 成分の数と候補の数の分布(T-0163 の研究。`--distribution [刻み] [間引き]` の時だけ。ctest では流さない)---
    // 同じ燃える場面を進め、間引いた刻みごとに全部のセルの成分の数と、反応の規則を持つセルの「進む規則の数」(RxWaitCandidates::offered)
    // を数える。進む規則の数は刻みの初めのセル(伝導の前)と、ブロックの tc で評価した近似(伝導の分だけ温度がずれる)

    struct Distribution {
        std::array<uint64_t, reaction::RX_MAX_CELL_SPECIES + 1> species{};          // 全部のセル
        std::array<uint64_t, reaction::RX_MAX_CELL_SPECIES + 1> reactingSpecies{};  // 進む規則があるセル
        std::vector<uint64_t> offered;  // 進む規則があるセルの、進む規則の数(添字 = 数)
        uint64_t ruleCells = 0;         // 反応の規則を持つセル
        uint64_t sampledTicks = 0;
    };

    void SampleTick(const BakedReactionTable& table, const ProbeReference& reference, uint64_t tick,
                    Distribution& result) {
        const ReactionTableView view = table.View();
        const std::span<const reaction::RxCell> cells = reference.State(tick);
        const std::span<const uint64_t> marks = reference.ChangedMarks();
        const uint64_t mark = ProbeChangeMark(tick);
        result.sampledTicks += 1;
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            const reaction::RxCell& cell = cells[index];
            result.species[cell.speciesCount] += 1;
            if (!ProbeHasRules(view, cell))
                continue;

            // --- 進む規則の数(RxStepCellWait と同じ種と温度の表の引き方)---
            result.ruleCells += 1;
            const uint64_t changedMark = marks[ProbeBlockOfCell(
                index % PROBE_GRID_SIZE, (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE, index / PROBE_SLICE_CELL_COUNT)];
            const reaction::RxThermal thermal = reaction::RxComputeThermal(view, cell);
            const uint32_t kelvin = static_cast<uint32_t>(thermal.temperature) /
                                    static_cast<uint32_t>(fx::MILLIKELVIN_PER_KELVIN);
            const uint32_t tableKelvin = std::min(kelvin, reaction::RX_RATE_TABLE_KELVINS - 1);
            const reaction::RxWaitCandidates collected = reaction::RxCollectCandidatesWait(
                view, cell, tableKelvin, reaction::RxWaitSeed(ProbeWorldSeed(), changedMark, index), mark - changedMark,
                reaction::RxSelectSeed(ProbeWorldSeed(), mark, index));
            if (collected.offered == 0)
                continue;

            result.reactingSpecies[cell.speciesCount] += 1;
            if (result.offered.size() <= collected.offered)
                result.offered.resize(collected.offered + 1, 0);

            result.offered[collected.offered] += 1;
        }
    }

    void PrintHistogram(const char* title, std::span<const uint64_t> histogram) {
        uint64_t total = 0;
        for (const uint64_t count : histogram)
            total += count;

        std::printf("%s(合計 %llu)\n", title, static_cast<unsigned long long>(total));
        for (size_t value = 0; value < histogram.size(); ++value) {
            if (histogram[value] == 0)
                continue;

            std::printf("  %2zu: %12llu (%.4f%%)\n", value, static_cast<unsigned long long>(histogram[value]),
                        total == 0 ? 0.0 : 100.0 * static_cast<double>(histogram[value]) / static_cast<double>(total));
        }
    }

    int MeasureDistribution(const BakedReactionTable& table, uint64_t ticks, uint64_t every) {
        const std::vector<ProbeCommand> commands = MakeCommands();
        ProbeReference reference(table);
        Distribution result;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            if (tick % every == 0)
                SampleTick(table, reference, tick, result);

            reference.Advance(tick, commands);
        }

        std::printf(
            "成分と候補の分布: 燃える木箱 %llu 刻み(%llu 刻みごとに %llu 回)・セル %u・表の物質 %zu・規則 %zu\n",
            static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(every),
            static_cast<unsigned long long>(result.sampledTicks), PROBE_CELL_COUNT, table.speciesNames.size(),
            table.rules.size());
        PrintHistogram("全部のセルの成分の数", result.species);
        PrintHistogram("進む規則があるセルの成分の数", result.reactingSpecies);
        PrintHistogram("進む規則があるセルの、進む規則の数", result.offered);
        std::printf("反応の規則を持つセル(延べ)%llu\n", static_cast<unsigned long long>(result.ruleCells));

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<size_t>(argc));
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
    if (args.size() >= 2 && std::string_view(args[1]) == "--distribution") {
        const uint64_t ticks = args.size() >= 3 ? std::strtoull(args[2], nullptr, 10) : 600;
        const uint64_t every = args.size() >= 4 ? std::strtoull(args[3], nullptr, 10) : 10;

        return MeasureDistribution(loaded->table, ticks, every == 0 ? 1 : every);
    }

    TestSameScene(loaded->table, *cpp);

    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("すべて通過\n");

    return 0;
}
