// equilibrium_test.cpp — 釣り合う家族の共通部品(sim/equilibrium。T-0853・20 §13.1)の CPU の試験。
// 1. 陰の圧力を部品の上へ移した道(GasProjectionOptions::equilibrium)が、前の道(T-0845 の CPU の模型。熱の多重格子を直に呼ぶ)と
//    T-0845 の 5 場面 × 300 刻み(既定 32³・音速 340 m/s・V 2 回固定・外側 0)で毎刻みビット一致(箱のハッシュ・帳簿・圧力・残差の計器)
// 2. 保存: 部品の道で毎刻み Σ 保存量 − 帳簿 が一定。V の回数 1・2・4 のどれでも(解けきらなくても帳簿は合う。20 §9.1)
// 3. 決定性: 部品の道を 2 回走らせてハッシュ一致
// 4. 閉じた領域(20 §9.3): 長さが世界を覆えば閉じない解とビット一致・小さい長さでは外の節の値が前の刻みとビット一致・
//    未知数の数が長さから決まる・閉じても気体の保存は厳密
// 引数: [--edge n](既定 32)[--ticks n](既定 300)
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/equilibrium.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "gas_projection_scenes.h"
#include "sim/equilibrium.h"
#include "sim/gas_projection.h"
#include "sim/gas_reference.h"

using namespace bicameral;
using namespace bicameral::gas_scenes;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    // T-0845 の形(gpu_gas_projection_test の FullOptions と同じ)
    sim::GasProjectionOptions T0845Options(bool equilibrium, uint32_t cycles = 2) {
        sim::GasProjectionOptions options;
        options.pressure.soundSpeedMmPerS = REAL_SOUND;
        options.fixedCycles = true;
        options.solver.cycles = cycles;
        options.outerIterations = 1;
        options.equilibrium = equilibrium;

        return options;
    }

    bool Conserved(const sim::GasBox& box, const sim::GasTotals& before) {
        const sim::GasTotals now = sim::SumGas(box);
        bool ok = now.amounts == before.amounts && now.energy == before.energy + box.ledger.energy;
        for (uint32_t axis = 0; axis < sim::GAS_AXES; ++axis)
            ok = ok && now.momentum[axis] == before.momentum[axis] + box.ledger.momentum[axis];

        return ok;
    }

    bool SameLedger(const sim::GasLedger& a, const sim::GasLedger& b) {
        return a.energy == b.energy && a.momentum == b.momentum && a.amounts == b.amounts;
    }

    // --- 1・2. 前の道と部品の道を 1 刻みずつ並べて比べる ---
    struct SceneRun {
        uint64_t hash = 0;
        uint32_t ticks = 0;
        bool conserved = true;
        uint64_t worstResidual = 0;  // 残差の計器の最大(mg)
        uint32_t levels = 0;
    };

    SceneRun CompareScene(Scene scene, uint32_t edge, uint32_t ticks) {
        sim::GasBox before = MakeScene(scene, edge);
        sim::GasBox after = before;
        const sim::GasTotals totals = sim::SumGas(after);
        sim::GasProjection beforeState;
        sim::GasProjection afterState;
        const sim::GasProjectionOptions beforeOptions = T0845Options(false);
        const sim::GasProjectionOptions afterOptions = T0845Options(true);
        SceneRun run;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            const sim::GasProjectionCost beforeCost = sim::StepGasProjection(before, beforeState, beforeOptions);
            const sim::GasProjectionCost afterCost = sim::StepGasProjection(after, afterState, afterOptions);
            const bool same = sim::HashGas(before) == sim::HashGas(after) && SameLedger(before.ledger, after.ledger) &&
                              beforeState.pressures == afterState.pressures &&
                              beforeCost.worstMassError == afterCost.worstMassError &&
                              beforeCost.gridLevels == afterCost.gridLevels;
            if (!same) {
                Expect(false, std::format("{} {}³: 刻み {} で前の道と部品の道が違う", SceneName(scene), edge, tick));
                break;
            }

            run.conserved = run.conserved && Conserved(after, totals);
            run.worstResidual = std::max(run.worstResidual, afterCost.worstResidual);
            run.levels = afterCost.gridLevels;
            run.ticks = tick + 1;
        }

        run.hash = sim::HashGas(after);
        Expect(run.conserved, std::format("{} {}³: 部品の道で保存が崩れた", SceneName(scene), edge));

        return run;
    }

    uint64_t RunEquilibrium(Scene scene, uint32_t edge, uint32_t ticks, const sim::GasProjectionOptions& options,
                            bool& conserved) {
        sim::GasBox box = MakeScene(scene, edge);
        const sim::GasTotals totals = sim::SumGas(box);
        sim::GasProjection state;
        conserved = true;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            (void)sim::StepGasProjection(box, state, options);
            conserved = conserved && Conserved(box, totals);
        }

        return sim::HashGas(box);
    }

    void TestGasOnEquilibrium(uint32_t edge, uint32_t ticks) {
        for (const Scene scene : SCENES) {
            const SceneRun run = CompareScene(scene, edge, ticks);
            Log(Channel::Sim, Level::Info,
                "| T-0853 陰の圧力 | {}³ | {} | {} 刻み毎刻み前の道とビット一致 | 保存 {} | 段 {} | 残差の計器の最大 "
                "{} mg | ハッシュ {:016x} |",
                edge, SceneName(scene), run.ticks, run.conserved ? "0" : "崩れた", run.levels, run.worstResidual,
                run.hash);
        }
    }

    void TestCyclesConserve(uint32_t edge) {
        for (const uint32_t cycles : {1u, 2u, 4u}) {
            for (const Scene scene : {Scene::Blast, Scene::Wall}) {
                bool conserved = false;
                (void)RunEquilibrium(scene, edge, 60, T0845Options(true, cycles), conserved);
                Expect(conserved, std::format("{} V {} 回: 保存が崩れた", SceneName(scene), cycles));
                Log(Channel::Sim, Level::Info, "保存: {} {}³ V {} 回 60 刻み = {}", SceneName(scene), edge, cycles,
                    conserved ? "毎刻み 0" : "崩れた");
            }
        }
    }

    void TestDeterminism(uint32_t edge, uint32_t ticks) {
        for (const Scene scene : SCENES) {
            bool first = false;
            bool second = false;
            const uint64_t a = RunEquilibrium(scene, edge, ticks, T0845Options(true), first);
            const uint64_t b = RunEquilibrium(scene, edge, ticks, T0845Options(true), second);
            Expect(a == b, std::format("{} {}³: 2 回の実行でハッシュが違う", SceneName(scene), edge));
            Log(Channel::Sim, Level::Info, "決定性: {} {}³ {} 刻み {:016x} / {:016x}", SceneName(scene), edge, ticks, a,
                b);
        }
    }

    // --- 4. 閉じた領域(一般の系: 熱のような重みの形・一辺 n の立方体・レベル 0)---
    constexpr uint64_t NODE_CAPACITY = (uint64_t)1 << 40;
    constexpr uint64_t FACE_COEFFICIENT = (uint64_t)1 << 44;

    uint32_t NodeIndex(uint32_t edge, uint32_t x, uint32_t y, uint32_t z) {
        return x + (edge * (y + (edge * z)));
    }

    // 源の値 sources(節の順。右辺 = C ÷ D × 値)・x = 0 の面に値 0 を固定した面
    sim::EquilibriumSystem MakeCube(uint32_t edge, const std::vector<int64_t>& sources) {
        sim::EquilibriumSystem system;
        system.rounding = multires::EQ_ROUNDING_WEIGHT;
        for (uint32_t z = 0; z < edge; ++z) {
            for (uint32_t y = 0; y < edge; ++y) {
                for (uint32_t x = 0; x < edge; ++x) {
                    const uint32_t index = NodeIndex(edge, x, y, z);
                    system.nodes.push_back({.x = x, .y = y, .z = z, .capacity = NODE_CAPACITY});
                    system.sources.push_back(
                        {.node = index, .value = sources[index], .factor = {.hi = 0, .lo = NODE_CAPACITY}});
                    if (x == 0)
                        system.fixedFaces.push_back({.node = index, .coefficient = {.hi = 0, .lo = FACE_COEFFICIENT}});

                    const std::array<uint32_t, 3> coordinate = {x, y, z};
                    for (uint32_t axis = 0; axis < 3; ++axis) {
                        if (coordinate[axis] + 1 == edge)
                            continue;

                        const uint32_t next = axis == 0 ? index + 1 : axis == 1 ? index + edge : index + (edge * edge);
                        system.faces.push_back(
                            {.fine = index, .coarse = next, .coefficient = {.hi = 0, .lo = FACE_COEFFICIENT}});
                    }
                }
            }
        }

        return system;
    }

    void TestClosedRegion() {
        constexpr uint32_t EDGE = 12;
        constexpr int64_t HOT = (int64_t)1 << 40;
        std::vector<int64_t> sources(EDGE * EDGE * EDGE, (int64_t)1 << 36);

        // --- 長さが世界を覆う: 閉じない解とビット一致(源を刻みごとに変える)---
        sim::EquilibriumState open;
        sim::EquilibriumState wide;
        sim::EquilibriumOptions openOptions;
        sim::EquilibriumOptions wideOptions;
        wideOptions.closeLengthMm = 100000;
        bool same = true;
        for (uint32_t tick = 0; tick < 8; ++tick) {
            sources[NodeIndex(EDGE, tick, 3, 5)] += HOT;
            const sim::EquilibriumSystem system = MakeCube(EDGE, sources);
            const sim::EquilibriumResult a = sim::SolveEquilibrium(system, openOptions, open);
            const sim::EquilibriumResult b = sim::SolveEquilibrium(system, wideOptions, wide);
            same = same && open.values == wide.values && a.faceValues == b.faceValues && a.fixedValues == b.fixedValues;
        }

        Expect(same, "閉じた領域: 長さが世界を覆う時に閉じない解と違う");

        // --- 小さい長さ: 種は 1 節(右辺が変わった節)。外の節は前の刻みの値のまま ---
        sim::EquilibriumOptions closed;
        closed.closeLengthMm = 1000;  // 2 セル = 隙間 1 m 以内(Chebyshev で 3 セル先まで)
        sim::EquilibriumState state = open;
        const std::vector<int64_t> previous = state.values;
        sources[NodeIndex(EDGE, 2, 2, 2)] += HOT;
        const sim::EquilibriumResult result = sim::SolveEquilibrium(MakeCube(EDGE, sources), closed, state);
        bool outsideFixed = true;
        uint32_t inside = 0;
        for (uint32_t z = 0; z < EDGE; ++z) {
            for (uint32_t y = 0; y < EDGE; ++y) {
                for (uint32_t x = 0; x < EDGE; ++x) {
                    const uint32_t index = NodeIndex(EDGE, x, y, z);
                    const bool reached = x <= 5 && y <= 5 && z <= 5;  // |差| ≤ 3
                    inside += reached ? 1 : 0;
                    if (!reached && state.values[index] != previous[index])
                        outsideFixed = false;
                }
            }
        }

        Expect(result.seeds == 1, std::format("閉じた領域: 種の数 {}(1 のはず)", result.seeds));
        Expect(result.unknowns == inside, std::format("閉じた領域: 未知数 {}(長さの内 {})", result.unknowns, inside));
        Expect(outsideFixed, "閉じた領域: 外の節の値が前の刻みから動いた(固定した境が漏れた)");
        Expect(state.values[NodeIndex(EDGE, 2, 2, 2)] != previous[NodeIndex(EDGE, 2, 2, 2)],
               "閉じた領域: 種の節が解かれていない");
        Log(Channel::Sim, Level::Info, "閉じた領域: 種 {}・未知数 {} / {}・段 {}・残差の計器 {}", result.seeds,
            result.unknowns, EDGE * EDGE * EDGE, result.gridLevels, result.worstResidual);

        // --- 気体: 閉じても保存は厳密(長さ 3 m = 6 セル。泡)---
        sim::GasProjectionOptions gas = T0845Options(true);
        gas.closeLengthMm = 3000;
        bool conserved = false;
        (void)RunEquilibrium(Scene::Bubble, 16, 60, gas, conserved);
        Expect(conserved, "閉じた領域: 気体の泡で保存が崩れた");
    }

    int Run(std::span<char*> arguments) {
        uint32_t edge = 32;
        uint32_t ticks = 300;
        for (size_t i = 1; i + 1 < arguments.size(); i += 2) {
            const std::string_view name = arguments[i];
            const auto value = (uint32_t)std::atoi(arguments[i + 1]);
            if (name == "--edge")
                edge = value;
            else if (name == "--ticks")
                ticks = value;
        }

        TestClosedRegion();
        TestCyclesConserve(16);
        TestDeterminism(16, ticks);
        TestGasOnEquilibrium(edge, ticks);
        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "equilibrium_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "equilibrium_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, (size_t)argc));
    SingletonFinalizer::Finalize();

    return exitCode;
}
