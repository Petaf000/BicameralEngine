// equilibrium_block_test.cpp — 釣り合う家族の成分を持つ節(sim/equilibrium_block)の CPU の試験: 違うレベルの節が混ざる系(T-0969・ADR-0462)。
// 場面(equilibrium_block_scenes.h。gpu_equilibrium_block_test と同じ): 一辺 8 のレベル 0 の箱(下の面を固定)のうち、真ん中の 2 × 2 × 2 セルをレベル 1(4 × 4 × 4)に細かくした系(k = 1・3)。
//   面の係数はエネルギーの形(両側に同じブロック)。細かい節どうしは粗い節どうしの 1/2、細かい節と粗い節は 1/3(面積 ÷ 距離)。延ばし方は区分的に一定。
// 確かめ:
//   1. 解ける: 刻みを続けると残差が丸めの床まで下がる(40 刻みで同じレベルの箱の 40 刻みの残差の 4 倍以内)。V 2 回と共役勾配(V 4 回 + lineSearch)で
//   2. 順によらない: 節と面の並びを逆にした系で、座標ごとの答えがビット単位で同じ(16 色の Gauss–Seidel の色の中の順が効かない)
//   3. 段: 細かい節を先にまとめ、段の数が同じレベルの箱より 1 つ多い
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

    int RunAll() {
        for (const uint32_t k : {1u, 3u}) {
            TestMixed(k, {}, "V 2 回");
            TestMixed(k, {.cycles = 4, .lineSearch = true}, "V 4 回 + 共役勾配");
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
