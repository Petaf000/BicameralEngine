// time_step_tree_test.cpp — 研究 R-T2 の 2(T-0950。刻みの長さの選び方を多重解像度の木の上の熱で。ADR-0410・ADR-0423)の CPU の試験。
// 試作は engine/src/sim/time_step_tree.{h,cpp}(C-0 の SolveEquilibrium で解く・セルは MultiresCellStore・導かれる値は C-1 の欄)。
// 場面(どれも y・z に一様 = 横は断熱の 1 本の列と同じ。違うレベルの面を通すため、混ぜた木は 1 m の根 4 つの 1 つに 0.5 m の子を 1 つ):
//   冷める池(静かな夜): 乱れで混ざる水(伝わりやすさ = ρc × 1 m²/s)・地面へ熱が逃げる・底でゆっくり発酵。初めに右側が 2 K 暖かい。
//     川の流れ(運ぶ)は C-0 の対称な面に入らないので T-0984 へ分けた。ここは熱(伝導)だけ
//   火事: 燃える床(着火 600 K)に左端 1 m で火を付ける(time_step_lab と同じ値)
// 確かめること:
//   1. 精度: 選んだ長い刻みと毎刻みが計器の精度の内(温度 1 mK・A は 100 万分の 1・地面へ逃げた熱は全体の温度で 1 mK)
//   2. 細かさ: 0.5 m(根 0.5 m)と 1 m(根 1 m)で同じ場所の e(合格: 平均 0.5 段以下・9 割が 1 段の内。最大も記録)
//   3. 早送り: 1 刻みずつ・一気に・ばらばらの塊・2 回の実行で要約がビット一致・どの塊の境目でも保存が厳密
//   4. 眠った所の導かれる値は眠った時の平均のまま(T-0854 の申し送り)
//   5. 費用: 夜・火事の CPU の秒数
//   6. 歩の途中のセーブ(Q146 の一旦の決定 (A)): 木の状態 + 歩の記録を写して戻すと、続けたのとビット一致。歩の記録のバイト/セル
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "sim/multires_fields.h"
#include "sim/multires_nest.h"
#include "sim/multires_state.h"
#include "sim/time_step_tree.h"

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

    constexpr double TICK_SECONDS = 1.0 / 60.0;
    constexpr double Q16 = 65536.0;
    constexpr double Q32 = 4294967296.0;
    constexpr double LEVEL0_VOLUME = 0.125;  // m³
    constexpr int32_t MAX_EXPONENT = 14;
    constexpr uint64_t COMPARE_EVERY = 1ull << MAX_EXPONENT;
    constexpr uint64_t NIGHT_TICKS = 7 * COMPARE_EVERY;
    constexpr uint64_t FIRE_TICKS = 4 * COMPARE_EVERY;
    constexpr uint64_t FIRE_IGNITION_TICK = 3 * 4096 + 17;

    uint64_t ToQ(double value, double scale) {
        return static_cast<uint64_t>(std::llround(value * scale));
    }

    // --- 場面の値(SI。どのレベルでも同じ数になる形: 容量・地面は 0.5 m のセルの体積で・面は 0.5 m の面で)---
    struct Material {
        double heatPerCubic = 0.0;    // J/(m³·K)
        double conductivity = 0.0;    // W/(m·K)
        double groundPerCubic = 0.0;  // W/(m³·K)
        double groundKelvin = 0.0;
        uint8_t fuel = TIME_STEP_FUEL_NONE;
        double rateHalfLifeSeconds = 0.0;  // 発酵
        double burnSeconds = 0.0;          // 燃える物が 1/e になる時間
        double ignitionKelvin = 0.0;
        double joulePerMol = 0.0;
        double molPerCubic = 0.0;
    };

    Material Pond() {
        return {.heatPerCubic = 4.18e6,
                .conductivity = 4.18e6 * 1.0,
                .groundPerCubic = 2.0,
                .groundKelvin = 283.0,
                .fuel = TIME_STEP_FUEL_SLOW,
                .rateHalfLifeSeconds = 7200.0,
                .joulePerMol = 1000.0,
                .molPerCubic = 400.0};
    }

    Material Fire() {
        return {.heatPerCubic = 2.0e5,
                .conductivity = 2000.0,
                .groundPerCubic = 200.0,
                .groundKelvin = 288.0,
                .fuel = TIME_STEP_FUEL_BURNS,
                .burnSeconds = 60.0,
                .ignitionKelvin = 600.0,
                .joulePerMol = 500.0e3,
                .molPerCubic = 500.0};
    }

    // 解きの回数(引数で変えて収束を見る)
    uint32_t solverCycles = 4;
    // 予算を 1 mK ÷ 2^これ に。木の上では 1 mK で違うレベルの境の近くが 8 mK ずれた(T-0950 の作業ログ)ので仮に 1/16 mK
    uint32_t budgetShift = 4;
    bool solverGalerkin = false;

    TimeTreeSetup MakeSetup(const Material& material, TimeStepRule rule) {
        TimeTreeSetup setup;
        setup.cycles = solverCycles;
        setup.galerkin = solverGalerkin;
        setup.capacityQ32 = ToQ(material.heatPerCubic * LEVEL0_VOLUME, Q32);
        setup.conductLevel0Q32 = {0, ToQ(material.conductivity * 0.25 / 0.5 * TICK_SECONDS, Q32)};
        setup.groundQ32 = ToQ(material.groundPerCubic * LEVEL0_VOLUME * TICK_SECONDS, Q32);
        setup.groundTemperatureQ16 = static_cast<int64_t>(ToQ(material.groundKelvin * 1000.0, Q16));
        setup.fuel = material.fuel;
        if (material.rateHalfLifeSeconds > 0.0)
            setup.rateColdQ32 = ToQ(std::log(2.0) / material.rateHalfLifeSeconds * TICK_SECONDS, Q32);

        if (material.burnSeconds > 0.0)
            setup.rateHotQ32 = ToQ(TICK_SECONDS / material.burnSeconds, Q32);

        setup.ignitionTemperatureQ16 = static_cast<int64_t>(ToQ(material.ignitionKelvin * 1000.0, Q16));
        setup.heatPerAmount = std::llround(material.joulePerMol * 1e-3);
        setup.rule = rule;
        setup.fixedExponent = 0;
        setup.maxExponent = MAX_EXPONENT;
        setup.budgetQ16 = (1ull << 16) >> budgetShift;

        return setup;
    }

    // --- 木: 根 roots 個を x に並べる(レベル rootLevel)。refine なら根 1 の (x 0〜4 m・y・z 0〜4 m) を 0.5 m に ---
    struct TreeShape {
        int32_t rootLevel = -1;
        uint32_t roots = 4;
        bool refine = true;
    };

    double CellMeters(int32_t level) {
        return 0.5 * std::ldexp(1.0, -level);
    }

    using Profile = std::function<double(double)>;  // 中心の x(m)→ 温度(K)

    // 刻むセルを profile と量で埋める(セルの値はレベルの単位 = 同じ数)
    void FillCells(MultiresNest& nest, const Material& material, const Profile& profile) {
        const double capacity = material.heatPerCubic * LEVEL0_VOLUME;  // mJ/mK
        const auto amount = static_cast<uint64_t>(std::llround(material.molPerCubic * LEVEL0_VOLUME * 1e6));
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL)
                continue;

            const bool expanded = ExpandNestPage(nest, slot);
            Expect(expanded, "場面: 頁に広げられる");
            const double size = CellMeters(block.level);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (MrIsCoveredCell(block, index))
                    continue;

                const double x = (static_cast<double>(block.originX + MrCellX(index)) + 0.5) * size;
                RxCell cell = RxMakeEmptyCell(std::llround(capacity * profile(x) * 1000.0));
                cell.speciesCount = 1;
                cell.species[0] = TIME_TREE_REACTANT;
                cell.amounts[0] = amount;
                StoreNestCell(nest, slot, index, cell);
            }
        }
    }

    TimeTreeWorld MakeWorld(const TimeTreeSetup& setup, const Material& material, const TreeShape& shape,
                            const Profile& profile) {
        MultiresCapacity capacity{.worldBlocks = 16,
                                  .observerBlocks = 0,
                                  .fractions = 16,
                                  .pages = 16,
                                  .indexEntries = 32,
                                  .ledgerColumns = 4,
                                  .rootLevel = shape.rootLevel};
        MultiresNest nest = MakeMultiresNest(capacity);
        const uint32_t field = RegisterNestField(
            nest, "derived.temperature",
            state_field::SfMakeDerivedDesc(state_field::SF_PLACE_CELL, state_field::SF_TYPE_I64,
                                           state_field::SF_PHASE_NONE, false, 1));
        const std::vector<RxCell> empty(MR_BLOCK_CELLS, RxMakeEmptyCell(0));
        for (uint32_t root = 0; root < shape.roots; ++root)
            PlaceRootBlock(nest, int64_t{root} * MR_BLOCK_EDGE, 0, 0, empty);

        if (shape.refine) {
            // 根 1 の原点の近くの 0.5 m の点(1 m の根の 1 つ下 = レベル 0 の子が八分の一を覆う)
            const int32_t level = shape.rootLevel + 1;
            const int64_t x = (int64_t{MR_BLOCK_EDGE} + 1) << 1;
            const MrRequest request = MrMakeRequest(MR_REQUEST_REFINE, level, x, 2, 2);
            SubmitRequests(nest, std::span(&request, 1));
            ProcessRequests(nest);
        }

        FillCells(nest, material, profile);

        return MakeTimeTreeWorld(setup, std::move(nest), field);
    }

    Profile PondProfile(double length) {
        return [length](double x) {
            return 288.0 + 2.0 * 0.5 * (1.0 + std::tanh((x - length * 0.25) / 8.0));
        };
    }

    Profile Ambient(double kelvin) {
        return [kelvin](double) {
            return kelvin;
        };
    }

    double WorldLength(const TreeShape& shape) {
        return shape.roots * MR_BLOCK_EDGE * CellMeters(shape.rootLevel);
    }

}  // namespace

namespace {

    // --- 走り(塊ごとに保存を確かめ、COMPARE_EVERY ごとに写しを取る)---

    struct Snapshot {
        std::vector<int64_t> energy;  // 刻むセルの順(最も細かいレベルの単位でなく、そのセルの単位)
        std::vector<int64_t> reactant;
        std::vector<double> x;  // セルの中心(m)
        std::vector<int32_t> level;
        int64_t groundHeat = 0;
    };

    struct Ignition {
        uint64_t tick = 0;  // 0 = 付けない
        double kelvin = 0.0;
    };

    struct RunResult {
        uint64_t hash = 0;
        std::vector<Snapshot> snapshots;
        double seconds = 0.0;
        bool conserved = true;
        uint64_t asleepChecks = 0;  // 眠っている枠を見た数
        uint64_t asleepWrong = 0;   // 導かれる値が眠った時の平均でなかった数
        TimeTreeSteps steps;
    };

    Snapshot TakeSnapshot(const TimeTreeWorld& world) {
        Snapshot snapshot;
        snapshot.groundHeat = world.steps.groundHeat;
        for (const uint32_t slot : world.layout.blocks) {
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (world.layout.stepped[slot * MR_BLOCK_CELLS + index] == 0)
                    continue;

                snapshot.energy.push_back(TimeTreeSettledEnergy(world, slot, index));
                snapshot.reactant.push_back(TimeTreeSettledReactant(world, slot, index));
                const MrBlock& block = world.nest.blocks[slot];
                snapshot.x.push_back((static_cast<double>(block.originX + MrCellX(index)) + 0.5) *
                                     CellMeters(block.level));
                snapshot.level.push_back(block.level);
            }
        }

        return snapshot;
    }

    // 左端 1 m の刻むセルに火を付ける(着火の温度 + 400 K まで)
    void Ignite(const TimeTreeSetup& setup, const Material& material, TimeTreeWorld& world, const Ignition& ignition) {
        const auto heat = std::llround(material.heatPerCubic * LEVEL0_VOLUME *
                                       (material.ignitionKelvin + 400.0 - ignition.kelvin) * 1000.0);
        for (const uint32_t slot : world.layout.blocks) {
            const MrBlock block = world.nest.blocks[slot];
            const double size = CellMeters(block.level);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const double x = (static_cast<double>(block.originX + MrCellX(index)) + 0.5) * size;
                if (world.layout.stepped[slot * MR_BLOCK_CELLS + index] != 0 && x < 1.0)
                    InjectTimeTreeHeat(setup, world, slot, index, heat);
            }
        }
    }

    // 眠っている枠の導かれる値が眠った時の平均か(T-0854 の申し送り)
    void CheckAsleep(const TimeTreeWorld& world, RunResult& result) {
        for (const uint32_t slot : world.layout.blocks) {
            if (!MrIsUniform(world.nest.blocks[slot]))
                continue;

            // 覆われていないセルで読む(覆われたセルは 0 と読む)
            uint32_t index = 0;
            while (world.layout.stepped[slot * MR_BLOCK_CELLS + index] == 0)
                ++index;

            ++result.asleepChecks;
            const int64_t kept = LoadNestField(world.nest, world.temperatureField, slot, index, 0);
            if (kept == world.steps.sleepMean[slot])
                continue;

            ++result.asleepWrong;
            Log(Channel::Sim, Level::Warning, "眠った枠 {}(刻み {}): 導かれる値 {}・眠った時の平均 {}", slot,
                world.steps.tick, kept, world.steps.sleepMean[slot]);
        }
    }

    RunResult Run(const TimeTreeSetup& setup, const Material& material, TimeTreeWorld world, uint64_t ticks,
                  uint64_t chunk, const Ignition& ignition = {}, bool snapshots = false) {
        RunResult result;
        const TimeTreeTotals first = ConservedTimeTree(world);
        const auto begin = std::chrono::steady_clock::now();
        uint64_t done = 0;
        while (done < ticks) {
            uint64_t size = std::min(chunk, ticks - done);
            if (ignition.tick > done && ignition.tick < done + size)
                size = ignition.tick - done;

            if (snapshots && (done / COMPARE_EVERY + 1) * COMPARE_EVERY < done + size)
                size = (done / COMPARE_EVERY + 1) * COMPARE_EVERY - done;

            StepTimeTree(setup, world, size);
            done += size;
            if (ignition.tick != 0 && done == ignition.tick)
                Ignite(setup, material, world, ignition);

            if (snapshots && done % COMPARE_EVERY == 0)
                result.snapshots.push_back(TakeSnapshot(world));

            CheckAsleep(world, result);
            result.conserved = result.conserved && first == ConservedTimeTree(world);
        }

        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        result.hash = HashTimeTree(world);
        result.steps = world.steps;

        return result;
    }

}  // namespace

namespace {

    // --- 比べる(計器の精度の単位)---

    struct Difference {
        double temperatureMk = 0.0;      // セルの温度の差の最大
        double reactantMillionth = 0.0;  // セルの A の差の最大(初めの量の 100 万分の 1)
        double groundMk = 0.0;           // 地面へ逃げた熱の差 ÷ 全体の熱容量(全体の温度で)
        double worstX = 0.0;             // 温度の差の最大のセルの中心の x(m)
        int32_t worstLevel = 0;
    };

    Difference Compare(const TimeTreeWorld& world, const Material& material, const Snapshot& trial,
                       const Snapshot& reference) {
        Difference difference;
        const double capacity = material.heatPerCubic * LEVEL0_VOLUME;
        const double amount = material.molPerCubic * LEVEL0_VOLUME * 1e6;
        for (size_t i = 0; i < trial.energy.size(); ++i) {
            const double energy = std::abs(static_cast<double>(trial.energy[i] - reference.energy[i]));
            const double reactant = std::abs(static_cast<double>(trial.reactant[i] - reference.reactant[i]));
            if (energy / capacity > difference.temperatureMk) {
                difference.temperatureMk = energy / capacity;
                difference.worstX = trial.x[i];
                difference.worstLevel = trial.level[i];
            }

            difference.reactantMillionth = std::max(difference.reactantMillionth, reactant / amount * 1e6);
        }

        // 全体の熱容量(最も細かいレベルの単位)
        double total = 0.0;
        for (const uint32_t slot : world.layout.blocks) {
            const double scale = std::ldexp(1.0, 3 * (world.layout.finestLevel - world.nest.blocks[slot].level));
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                total += world.layout.stepped[slot * MR_BLOCK_CELLS + index] != 0 ? capacity * scale : 0.0;
        }

        difference.groundMk = std::abs(static_cast<double>(trial.groundHeat - reference.groundHeat)) / total;

        return difference;
    }

    Difference Worst(const Difference& a, const Difference& b) {
        Difference worst = a.temperatureMk >= b.temperatureMk ? a : b;
        worst.reactantMillionth = std::max(a.reactantMillionth, b.reactantMillionth);
        worst.groundMk = std::max(a.groundMk, b.groundMk);

        return worst;
    }

    void LogRun(std::string_view scene, std::string_view name, const TimeTreeWorld& world, const RunResult& run,
                uint64_t ticks) {
        uint64_t cells = 0;
        for (const uint8_t stepped : world.layout.stepped)
            cells += stepped;

        const double perCellTick = static_cast<double>(run.steps.cellSteps) /
                                   (static_cast<double>(cells) * static_cast<double>(ticks));
        Log(Channel::Sim, Level::Info,
            "{} {}: {:.2f} 秒(CPU 1 スレッド・{} セル・{} 刻み)・解き {:.5f} 回/セル/刻み・部品を呼んだ {}・出来事 "
            "{}・打ち切り {}・"
            "隣の差の最大 {}・負 {}・眠る {}・起こす {}・残差の計器の最大 {}(V {} 回{})",
            scene, name, run.seconds, cells, ticks, perCellTick, run.steps.solves, run.steps.events,
            run.steps.truncations, run.steps.widestNeighborGap, run.steps.negativeValues, run.steps.sleeps,
            run.steps.wakes, run.steps.worstResidual, solverCycles, solverGalerkin ? "・Galerkin" : "");
    }

    // --- 1・4・5. 精度と費用(混ぜた木)---

    void CheckScene(std::string_view scene, const Material& material, const Profile& profile, uint64_t ticks,
                    const Ignition& ignition, bool reactantExact) {
        const TreeShape shape;
        const TimeTreeSetup referenceSetup = MakeSetup(material, TimeStepRule::Fixed);
        const TimeTreeSetup trialSetup = MakeSetup(material, TimeStepRule::Change);
        const TimeTreeWorld referenceWorld = MakeWorld(referenceSetup, material, shape, profile);
        const TimeTreeWorld trialWorld = MakeWorld(trialSetup, material, shape, profile);
        const RunResult reference = Run(referenceSetup, material, referenceWorld, ticks, ticks, ignition, true);
        const RunResult trial = Run(trialSetup, material, trialWorld, ticks, ticks, ignition, true);
        LogRun(scene, "毎刻み", referenceWorld, reference, ticks);
        LogRun(scene, "試し 1", trialWorld, trial, ticks);

        Difference worst;
        for (size_t i = 0; i < trial.snapshots.size(); ++i) {
            const Difference at = Compare(trialWorld, material, trial.snapshots[i], reference.snapshots[i]);
            Log(Channel::Sim, Level::Info, "{} 刻み {}: セルの温度の差の最大 {:.3f} mK(x {:.2f} m・レベル {})", scene,
                (i + 1) * COMPARE_EVERY, at.temperatureMk, at.worstX, at.worstLevel);
            worst = Worst(worst, at);
        }

        // 発酵は温度によらないので A の本当の値は閉じた形(毎刻みも端数の丸めで揺れる。time_step_lab と同じく本当の値と比べる)
        if (reactantExact) {
            const double start = material.molPerCubic * LEVEL0_VOLUME * 1e6;
            const double rate = static_cast<double>(trialSetup.rateColdQ32) / Q32;
            const double exact = start * std::exp(-rate * static_cast<double>(ticks));
            // セルの A は決まった乱数の丸めで本当の値の周りに歩の数の平方根ほど揺れる(毎刻みも同じ。床の値として出す)。
            // 偏りは全体の A の平均で見る(合格はこちら)
            const auto against = [&](const Snapshot& snapshot, double& cellWorst) {
                double sum = 0.0;
                for (const int64_t value : snapshot.reactant) {
                    cellWorst = std::max(cellWorst, std::abs(static_cast<double>(value) - exact) / start * 1e6);
                    sum += static_cast<double>(value);
                }

                return std::abs(sum / static_cast<double>(snapshot.reactant.size()) - exact) / start * 1e6;
            };
            double trialCell = 0.0;
            double referenceCell = 0.0;
            const double trialMean = against(trial.snapshots.back(), trialCell);
            const double referenceMean = against(reference.snapshots.back(), referenceCell);
            Log(Channel::Sim, Level::Info,
                "{} A の本当の値との差(100 万分の 1): 試し 1 セルの最大 {:.3f}・平均 {:.4f} / 毎刻み セルの最大 "
                "{:.3f}・平均 {:.4f}",
                scene, trialCell, trialMean, referenceCell, referenceMean);
            worst.reactantMillionth = trialMean;
        }

        Log(Channel::Sim, Level::Info,
            "{} 試し 1 と毎刻み: セルの温度 {:.3f} mK・A {:.3f}/100 万{}・地面へ逃げた熱 {:.4f} "
            "mK(全体)・眠った枠の導かれる値 "
            "{} 回見て違い {}",
            scene, worst.temperatureMk, worst.reactantMillionth, reactantExact ? "(本当の値と)" : "", worst.groundMk,
            trial.asleepChecks, trial.asleepWrong);
        Expect(reference.conserved && trial.conserved, "保存: どの塊の境目でも Σ − 帳簿 が最初と同じ");
        Expect(trial.steps.widestNeighborGap <= 1, "隣どうしの e の差は 1 段まで");
        Expect(trial.steps.negativeValues == 0, "真実が負にならない");
        Expect(worst.temperatureMk <= 1.0 && worst.reactantMillionth <= 1.0 && worst.groundMk <= 1.0,
               "長い刻みと毎刻みが計器の精度の内");
        Expect(trial.asleepWrong == 0, "眠った所の導かれる値は眠った時の平均のまま");
    }

}  // namespace

namespace {

    // --- 2. 細かさ: 0.5 m(根 0.5 m・8 つ)と 1 m(根 1 m・4 つ)で同じ場所に選ばれる e ---

    void CheckGranularity(std::string_view scene, const Material& material, const Profile& profile, uint64_t ticks,
                          const Ignition& ignition) {
        constexpr uint64_t SAMPLE_EVERY = 1024;
        const TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Change);
        const TreeShape fineShape{.rootLevel = 0, .roots = 8, .refine = false};
        const TreeShape coarseShape{.rootLevel = -1, .roots = 4, .refine = false};
        TimeTreeWorld fine = MakeWorld(setup, material, fineShape, profile);
        TimeTreeWorld coarse = MakeWorld(setup, material, coarseShape, profile);
        // 根の枠を x の順に(0.5 m の根 i は 1 m の根 i / 2 の中)
        const auto byX = [](const TimeTreeWorld& world) {
            std::vector<uint32_t> slots = world.layout.blocks;
            std::ranges::sort(slots, [&](uint32_t a, uint32_t b) {
                return world.nest.blocks[a].originX < world.nest.blocks[b].originX;
            });

            return slots;
        };
        const std::vector<uint32_t> fineSlots = byX(fine);
        const std::vector<uint32_t> coarseSlots = byX(coarse);
        int32_t widest = 0;
        double meanGap = 0.0;
        uint32_t withinOne = 0;
        uint32_t samples = 0;
        // ブロックは 8³ セルなので、0.5 m のブロック(4 m)は 1 m のブロック(8 m)の半分の長さ。1 m のブロックは中の 0.5 m の
        // ブロック 2 つの変化の大きい方で縮むので、「同じ場所」は 1 m の e と 0.5 m の 2 つの e の小さい方とも比べる(記録)
        double meanFine = 0.0;
        double meanCoarse = 0.0;
        int32_t widestPair = 0;
        double meanPair = 0.0;
        uint32_t withinOnePair = 0;
        uint32_t pairs = 0;
        for (uint64_t done = 0; done < ticks;) {
            uint64_t size = SAMPLE_EVERY;
            if (ignition.tick > done && ignition.tick < done + size)
                size = ignition.tick - done;

            StepTimeTree(setup, fine, size);
            StepTimeTree(setup, coarse, size);
            done += size;
            if (ignition.tick != 0 && done == ignition.tick) {
                Ignite(setup, material, fine, ignition);
                Ignite(setup, material, coarse, ignition);
            }

            for (uint32_t i = 0; i < fineShape.roots; ++i) {
                const int32_t gap = std::abs(fine.steps.exponent[fineSlots[i]] -
                                             coarse.steps.exponent[coarseSlots[i / 2]]);
                widest = std::max(widest, gap);
                meanGap += gap;
                withinOne += gap <= 1 ? 1u : 0u;
                ++samples;
                meanFine += fine.steps.exponent[fineSlots[i]];
                meanCoarse += coarse.steps.exponent[coarseSlots[i / 2]];
                if ((i & 1u) == 0)
                    continue;

                const int32_t pairMin = std::min(fine.steps.exponent[fineSlots[i - 1]],
                                                 fine.steps.exponent[fineSlots[i]]);
                const int32_t pairGap = std::abs(pairMin - coarse.steps.exponent[coarseSlots[i / 2]]);
                widestPair = std::max(widestPair, pairGap);
                meanPair += pairGap;
                withinOnePair += pairGap <= 1 ? 1u : 0u;
                ++pairs;
            }
        }

        const double share = static_cast<double>(withinOne) / samples;
        Log(Channel::Sim, Level::Info,
            "細かさ {}: 同じ場所の e の差 平均 {:.2f}・1 段の内 {:.1f} %・最大 {}・e の平均 0.5 m {:.2f} / 1 m {:.2f}",
            scene, meanGap / samples, share * 100.0, widest, meanFine / samples, meanCoarse / samples);
        Log(Channel::Sim, Level::Info,
            "細かさ {}(1 m の e と 0.5 m の 2 つの小さい方): 差 平均 {:.2f}・1 段の内 {:.1f} %・最大 {}", scene,
            meanPair / pairs, 100.0 * withinOnePair / pairs, widestPair);
        Expect(meanGap / samples <= 0.5 && share >= 0.9, "細かさ: 平均 0.5 段以下・9 割が 1 段の内");
    }

    // --- 3. 早送り = 待った(ビット一致)・6. 歩の途中のセーブ ---

    void CheckFastForward() {
        const Material material = Fire();
        const TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Change);
        const TimeTreeWorld world = MakeWorld(setup, material, TreeShape{}, Ambient(material.groundKelvin));
        const Ignition ignition{FIRE_IGNITION_TICK, material.groundKelvin};
        const uint64_t ticks = 2 * COMPARE_EVERY + 777;
        const RunResult waited = Run(setup, material, world, ticks, 1, ignition);
        const RunResult skipped = Run(setup, material, world, ticks, ticks, ignition);
        const RunResult uneven = Run(setup, material, world, ticks, 997, ignition);
        const RunResult again = Run(setup, material, world, ticks, ticks, ignition);
        Expect(waited.hash == skipped.hash, "早送り: 1 刻みずつと一気にがビット一致");
        Expect(waited.hash == uneven.hash, "早送り: ばらばらの塊とビット一致");
        Expect(waited.hash == again.hash, "2 回の実行でビット一致");
        Expect(waited.conserved && skipped.conserved && uneven.conserved, "早送り: どの塊の境目でも保存");
        Log(Channel::Sim, Level::Info, "早送り: 要約 {:016x}・1 刻みずつ {:.2f} 秒・一気に {:.2f} 秒", waited.hash,
            waited.seconds, skipped.seconds);
    }

    void CheckSave() {
        const Material material = Fire();
        const TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Change);
        const TimeTreeWorld start = MakeWorld(setup, material, TreeShape{}, Ambient(material.groundKelvin));
        const Ignition ignition{FIRE_IGNITION_TICK, material.groundKelvin};
        const uint64_t saveTick = COMPARE_EVERY + 4096 + 333;  // 長い歩の途中
        const uint64_t ticks = 2 * COMPARE_EVERY + 777;

        TimeTreeWorld world = start;
        StepTimeTree(setup, world, ignition.tick);
        Ignite(setup, material, world, ignition);
        StepTimeTree(setup, world, saveTick - ignition.tick);
        uint32_t midStep = 0;
        for (const uint32_t slot : world.layout.blocks)
            midStep += world.steps.open[slot] != 0 && world.steps.start[slot] < saveTick ? 1u : 0u;

        const MultiresSaveState save = SaveNestState(world.nest);
        const TimeTreeSteps savedSteps = world.steps;
        const TimeTreeRecordBytes bytes = MeasureTimeTreeRecords(world);
        StepTimeTree(setup, world, ticks - saveTick);

        TimeTreeWorld loaded = start;
        const bool restored = RestoreNestState(loaded.nest, save);
        loaded.steps = savedSteps;
        StepTimeTree(setup, loaded, ticks - saveTick);
        Expect(restored && midStep > 0, "セーブ: 歩の途中の枠がある刻みで写せる");
        Expect(HashTimeTree(world) == HashTimeTree(loaded), "セーブ: 歩の記録ごと戻すと続けたのとビット一致");
        Log(Channel::Sim, Level::Info,
            "セーブ(刻み {}・歩の途中の枠 {}): 歩の記録 最小 {} B({:.2f} B/セル)・歩の変化も持つと {} B({:.2f} "
            "B/セル)・{} セル",
            saveTick, midStep, bytes.minimal, static_cast<double>(bytes.minimal) / bytes.cells, bytes.held,
            static_cast<double>(bytes.held) / bytes.cells, bytes.cells);
    }

    int Run(int argc, char** argv) {
        const std::string_view part = argc > 1 ? argv[1] : "all";
        if (argc > 2)
            solverCycles = static_cast<uint32_t>(std::atoi(argv[2]));

        solverGalerkin = argc > 3 && std::string_view(argv[3]) == "galerkin";
        if (argc > 4)
            budgetShift = static_cast<uint32_t>(std::atoi(argv[4]));
        const bool all = part == "all";
        const Material pond = Pond();
        const Material fire = Fire();
        const Ignition fireIgnition{FIRE_IGNITION_TICK, fire.groundKelvin};
        if (all || part == "forward") {
            CheckFastForward();
            CheckSave();
        }

        if (all || part == "grain") {
            CheckGranularity("池", pond, PondProfile(32.0), NIGHT_TICKS, {});
            CheckGranularity("火事", fire, Ambient(fire.groundKelvin), FIRE_TICKS, fireIgnition);
        }

        if (all || part == "night")
            CheckScene("池(静かな夜)", pond, PondProfile(WorldLength(TreeShape{})), NIGHT_TICKS, {}, true);

        if (all || part == "fire")
            CheckScene("火事", fire, Ambient(fire.groundKelvin), FIRE_TICKS, fireIgnition, false);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "time_step_tree_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "time_step_tree_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(argc, argv);
    SingletonFinalizer::Finalize();

    return exitCode;
}
