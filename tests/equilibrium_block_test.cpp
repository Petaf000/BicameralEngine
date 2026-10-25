// equilibrium_block_test.cpp — 釣り合う家族の成分を持つ節(sim/equilibrium_block)の CPU の試験: 違うレベルの節が混ざる系(T-0969・ADR-0462)。
// 場面(equilibrium_block_scenes.h。gpu_equilibrium_block_test と同じ): 一辺 8 のレベル 0 の箱(下の面を固定)のうち、真ん中の 2 × 2 × 2 セルをレベル 1(4 × 4 × 4)に細かくした系(k = 1・3)。
//   面の係数はエネルギーの形(両側に同じブロック)。細かい節どうしは粗い節どうしの 1/2、細かい節と粗い節は 1/3(面積 ÷ 距離)。延ばし方は区分的に一定。
// 確かめ:
//   1. 解ける: 刻みを続けると残差が丸めの床まで下がる(40 刻みで同じレベルの箱の 40 刻みの残差の 4 倍以内)。V 2 回と共役勾配(V 4 回 + lineSearch)で
//   2. 順によらない: 節と面の並びを逆にした系で、座標ごとの答えがビット単位で同じ(16 色の Gauss–Seidel の色の中の順が効かない)
//   3. 段: 細かい節を先にまとめ、段の数が同じレベルの箱より 1 つ多い
//   4. 閉じた領域(T-1087): 世界を覆う長さは閉じない解とビット一致・1 m で閉じた時の外の値は前の刻みのまま・刻みをまたぐ向きは外で 0
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
#include <string_view>
#include <tuple>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "equilibrium_block_scenes.h"
#include "sim/equilibrium_block.h"

using namespace bicameral;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    using test::Key;
    using test::MixedSystem;

    // 座標ごとの答え
    std::map<Key, std::vector<int64_t>> ByKey(const sim::EquilibriumBlockSystem& system,
                                              const std::vector<int64_t>& values) {
        std::map<Key, std::vector<int64_t>> result;
        const uint32_t k = system.components;
        for (uint32_t i = 0; i < system.nodes.size(); ++i) {
            const sim::EquilibriumNode& node = system.nodes[i];
            result[{node.level, node.x, node.y, node.z}] = std::vector<int64_t>(values.begin() + (i * k),
                                                                                values.begin() + ((i + 1) * k));
        }

        return result;
    }

    struct Run {
        std::vector<uint64_t> residuals;  // 刻みごとの残差の最大
        std::vector<int64_t> values;
        uint32_t levels = 0;
    };

    Run Solve(const sim::EquilibriumBlockSystem& system, const sim::EquilibriumBlockOptions& options, uint32_t ticks) {
        const sim::EquilibriumBlockGrid grid = sim::BuildEquilibriumBlockGrid(system);
        sim::EquilibriumBlockState state;
        Run run;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            const sim::EquilibriumBlockResult result = sim::SolveEquilibriumBlocks(system, grid, options, state);
            run.residuals.push_back(result.worstResidual);
            run.levels = result.gridLevels;
        }

        run.values = state.values;

        return run;
    }

    void TestMixed(uint32_t k, const sim::EquilibriumBlockOptions& options, std::string_view name) {
        constexpr uint32_t TICKS = 40;
        const sim::EquilibriumBlockSystem mixed = MixedSystem(k, true, false);
        const Run forward = Solve(mixed, options, TICKS);
        const Run backward = Solve(MixedSystem(k, true, true), options, TICKS);
        const Run plain = Solve(MixedSystem(k, false, false), options, TICKS);
        const auto r = [&](const Run& run, uint32_t tick) {
            return (double)run.residuals[tick - 1];
        };
        Log(Channel::Sim, Level::Info,
            "違うレベル k = {}・{}: 節 {}・段 {}(同じレベルの箱 {})・残差 1 刻み {:.3e}・5 刻み {:.3e}・10 刻み "
            "{:.3e}・40 刻み {:.3e}"
            "(同じレベルの箱: {:.3e} → {:.3e})",
            k, name, mixed.nodes.size(), forward.levels, plain.levels, r(forward, 1), r(forward, 5), r(forward, 10),
            r(forward, 40), r(plain, 1), r(plain, 40));

        Expect(forward.levels == plain.levels + 1, std::format("k = {}・{}: 段が 1 つ多い", k, name));
        // 40 刻みで丸めの床(同じレベルの箱の 40 刻みの残差)の 4 倍以内まで下がる
        Expect(forward.residuals.back() <= 4 * std::max<uint64_t>(plain.residuals.back(), 1),
               std::format("k = {}・{}: 40 刻みで残差が同じレベルの箱の丸めの床の 4 倍以内", k, name));
        Expect(ByKey(mixed, forward.values) == ByKey(MixedSystem(k, true, true), backward.values),
               std::format("k = {}・{}: 節と面を逆の順にしても座標ごとの答えがビット単位で同じ", k, name));
    }

    // 閉じた領域(T-1087): 刻みごとに隅の 1 節の荷を変え(種はその節だけ)、
    //   - 長さが世界を覆えば(1 km)閉じない解と毎刻みビット一致(値も刻みをまたぐ向きも)
    //   - 長さ 1 m なら未知数は種から 1 m の内の節だけで、外の値は前の刻みとビット一致・刻みをまたぐ向きは外で 0
    void TestClosed(uint32_t k, const sim::EquilibriumBlockOptions& base, std::string_view name) {
        constexpr uint32_t TICKS = 12;
        sim::EquilibriumBlockSystem system = MixedSystem(k, true, false);
        const sim::EquilibriumBlockGrid grid = sim::BuildEquilibriumBlockGrid(system);
        const auto count = (uint32_t)system.nodes.size();
        uint32_t corner = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const sim::EquilibriumNode& node = system.nodes[i];
            if (node.level == 0 && node.x == 0 && node.y == 0 && node.z == test::MIXED_EDGE - 1)
                corner = i;
        }

        sim::EquilibriumBlockOptions wide = base;
        wide.closeLengthMm = 1000000;
        sim::EquilibriumBlockOptions closeBy = base;
        closeBy.closeLengthMm = 1000;
        sim::EquilibriumBlockState openState;
        sim::EquilibriumBlockState wideState;
        sim::EquilibriumBlockState closeState;
        bool wideSame = true;
        bool outsideSame = true;
        bool directionOutsideZero = true;
        bool regionMatches = true;
        uint32_t closeUnknowns = 0;
        for (uint32_t tick = 0; tick < TICKS; ++tick) {
            for (sim::EquilibriumBlockSource& source : system.sources) {
                if (source.node == corner && source.component == 0)
                    source.value -= (int64_t)(tick + 1) << 16;
            }

            const std::vector<int64_t> before = closeState.values;
            sim::SolveEquilibriumBlocks(system, grid, base, openState);
            sim::SolveEquilibriumBlocks(system, grid, wide, wideState);
            const sim::EquilibriumBlockResult closeResult = sim::SolveEquilibriumBlocks(system, grid, closeBy,
                                                                                        closeState);
            wideSame = wideSame && openState.values == wideState.values &&
                       openState.conjugateDirection == wideState.conjugateDirection;
            if (tick == 0)
                continue;

            std::vector<uint8_t> seeds(count, 0);
            seeds[corner] = 1;
            const std::vector<uint8_t> inside = sim::EquilibriumCloseRegion(system.nodes, seeds, closeBy.closeLengthMm);
            regionMatches = regionMatches && closeResult.seeds == 1 &&
                            closeResult.unknowns == (uint32_t)std::ranges::count(inside, (uint8_t)1);
            closeUnknowns = closeResult.unknowns;
            for (uint32_t i = 0; i < count; ++i) {
                if (inside[i] != 0)
                    continue;

                for (uint32_t c = 0; c < k; ++c) {
                    outsideSame = outsideSame && closeState.values[i * k + c] == before[i * k + c];
                    if (!closeState.conjugateDirection.empty())
                        directionOutsideZero = directionOutsideZero && closeState.conjugateDirection[i * k + c] == 0;
                }
            }
        }

        Log(Channel::Sim, Level::Info, "閉じた領域 k = {}・{}: 節 {}・1 m の未知数 {}", k, name, count, closeUnknowns);
        Expect(wideSame, std::format("k = {}・{}: 世界を覆う長さは閉じない解と毎刻みビット一致", k, name));
        Expect(regionMatches, std::format("k = {}・{}: 種は 1 節・未知数は 1 m の内の節", k, name));
        Expect(closeUnknowns > 1 && closeUnknowns < count, std::format("k = {}・{}: 1 m で閉じる", k, name));
        Expect(outsideSame, std::format("k = {}・{}: 閉じた領域の外の値は前の刻みとビット一致", k, name));
        Expect(directionOutsideZero, std::format("k = {}・{}: 刻みをまたぐ向きは閉じた領域の外で 0", k, name));
    }

    int RunAll() {
        for (const uint32_t k : {1u, 3u}) {
            TestMixed(k, {}, "V 2 回");
            TestMixed(k, {.cycles = 4, .lineSearch = true}, "V 4 回 + 共役勾配");
            TestClosed(k, {}, "V 2 回");
            TestClosed(k, {.cycles = 2, .lineSearch = true, .conjugateAcrossTicks = true}, "刻みをまたぐ共役勾配");
        }

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "equilibrium_block_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "equilibrium_block_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = RunAll();
    SingletonFinalizer::Finalize();

    return exitCode;
}
