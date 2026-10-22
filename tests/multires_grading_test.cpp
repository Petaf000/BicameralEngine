// multires_grading_test.cpp — 細かい所の半径と移り目を世界のデータの長さで選ぶ規則(sim/multires_grading。T-0954・ADR-0431)の CPU の試験。
// 1. 規則: 半径の内は 0 段・半径の外は距離が 2 倍ごとに段の数ずつ・遠くの葉で頭打ち・いちばん細かいセルを変えても細かい所の広さと遠くの葉は同じ長さ
// 2. 20 km 四方の地表の 4 分木(種 = 真ん中の点): 値を変えると葉の数が変わる・同じ値なら同じ木(葉の並びのハッシュが一致)
// 3. 既定の値と T-0900 の表の行の見込みのセル数を出す(docs/perf.md へ。地表の葉 × 縦の L 層と上下の粗い層 ≒ 5/3 × L)
#include <array>
#include <cstdint>
#include <string_view>

#include "core/log.h"
#include "core/singleton.h"
#include "sim/multires_grading.h"
#include "sim/world_physics.h"

using namespace bicameral;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    constexpr uint64_t FINE_CELL_MM = 500;
    constexpr uint64_t WORLD_SIDE_MM = 20000000;  // 20 km
    constexpr uint32_t ROOT_COARSENESS = 16;      // 0.5 m × 2^16 = 32.8 km(20 km を覆う)
    constexpr uint64_t SURFACE_LAYERS = 2;        // T-0900 の L(地表の葉の層)

    // --- 1. 規則 ---
    void TestRule() {
        const sim::FineRegion region;  // 128 m・32 m・1 段
        Expect(sim::GradedCoarseness(region, FINE_CELL_MM, 0) == 0, "規則: 種の上が細かくない");
        Expect(sim::GradedCoarseness(region, FINE_CELL_MM, 128000) == 0, "規則: 半径ちょうどが細かくない");
        Expect(sim::GradedCoarseness(region, FINE_CELL_MM, 128001) == 1, "規則: 半径の外が 1 段でない");
        Expect(sim::GradedCoarseness(region, FINE_CELL_MM, 256000) == 1, "規則: 半径の 2 倍が 1 段でない");
        Expect(sim::GradedCoarseness(region, FINE_CELL_MM, 256001) == 2, "規則: 半径の 2 倍の外が 2 段でない");
        Expect(sim::GradedCoarseness(region, FINE_CELL_MM, 100000000) == 6,
               "規則: 遠くの葉 32 m(0.5 m から 6 段)で止まらない");

        sim::FineRegion steep = region;
        steep.levelsPerDoubling = 2;
        Expect(sim::GradedCoarseness(steep, FINE_CELL_MM, 256001) == 4, "規則: 段の数 2 で 2 倍ごとに 2 段でない");

        // いちばん細かいセルを 0.25 m にしても、細かい所の広さと遠くの葉の大きさ(32 m = 0.25 m から 7 段)は同じ長さ
        Expect(sim::GradedCoarseness(region, 250, 128000) == 0, "規則: 0.25 m で半径の内が細かくない");
        Expect((250ull << sim::GradedCoarseness(region, 250, 100000000)) == 32000,
               "規則: 0.25 m で遠くの葉が 32 m でない");
    }

    sim::GradedLeafCount Count(const sim::FineRegion& region) {
        return sim::CountGradedSurfaceLeaves(region, FINE_CELL_MM, ROOT_COARSENESS,
                                             {WORLD_SIDE_MM / 2, WORLD_SIDE_MM / 2}, WORLD_SIDE_MM);
    }

    void Report(std::string_view name, const sim::FineRegion& region, const sim::GradedLeafCount& count) {
        // 縦: 地表に L 層、上下はさらに粗い層(面積あたり L × (1/4 + 1/16 + …)× 上下 2 = L × 2/3)
        const uint64_t cells = count.leaves * SURFACE_LAYERS * 5 / 3;
        Log(Channel::Sim, Level::Info,
            "{}: 半径 {} m・遠くの葉 {} m・段 {}: 地表の葉 {}(0.5 m の葉 {})・見込みのセル {}(L = {})", name,
            region.radiusMm / 1000, region.farLeafMm / 1000, region.levelsPerDoubling, count.leaves,
            count.byCoarseness[0], cells, SURFACE_LAYERS);
    }

    // --- 2・3. 20 km の地表の木 ---
    void TestTree() {
        const sim::FineRegion defaults;
        const sim::GradedLeafCount first = Count(defaults);
        const sim::GradedLeafCount second = Count(defaults);
        Expect(first.leaves == second.leaves && first.hash == second.hash, "木: 同じ値で違う木になった");
        Report("既定", defaults, first);

        sim::FineRegion wide = defaults;
        wide.radiusMm = 256000;
        sim::FineRegion fineFar = wide;
        fineFar.farLeafMm = 16000;
        sim::FineRegion steep = defaults;
        steep.levelsPerDoubling = 2;
        const sim::GradedLeafCount wideCount = Count(wide);
        const sim::GradedLeafCount fineFarCount = Count(fineFar);
        const sim::GradedLeafCount steepCount = Count(steep);
        Expect(wideCount.leaves > first.leaves, "木: 半径を広げても葉が増えない");
        Expect(fineFarCount.leaves > wideCount.leaves, "木: 遠くの葉を細かくしても葉が増えない");
        Expect(steepCount.leaves < first.leaves, "木: 移り目を急にしても葉が減らない");
        Expect(wideCount.hash != first.hash, "木: 値を変えても同じ木");
        Report("半径 256 m", wide, wideCount);
        Report("半径 256 m・遠くの葉 16 m", fineFar, fineFarCount);
        Report("2 倍ごとに 2 段", steep, steepCount);
    }

    int Run() {
        TestRule();
        TestTree();
        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_grading_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_grading_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
