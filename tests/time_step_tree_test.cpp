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
// T-0985: 選び方の 2 通り目(select=retake)・細かさを同じ物の長さのブロックで(group=1)・毎刻みの写しを使い回す(ファイル)。
// T-1013: 退く道の 1 本目(select=jump。寝ている間だけ。e は 0 か上限)。
// T-1063(D-633): 予算を mK の整数で(budgetmk=10・100・1000)。毎刻みとの差は記録だけ(合格の判定に使わない)。
//   火事は前線(着火した・半分燃えた最も遠い x)が届いた刻みと燃えた量を毎刻みと比べる(遊びに効く結果が予算で変わるか)。
// 引数: <forward|grain|grain-pond|grain-fire|grain-fine-pond|grain-fine-fire|night|fire> [select=previous|retake|jump] [budget=予算のずらし]
//       [budgetmk=予算(mK)] [group=揃える段] [buffer=1(前線の緩衝)]
//       [zerocycles=1 刻みの V の回数(既定 1)] [unit=block(単位 4 m を外す)]
//       [bufferunits=前線の緩衝の単位の数(T-1074)] [shape=mixed|coarse|fine|finer(night・fire の木。1 m・0.5 m・0.25 m の根だけ)]
//       [cycles=V の回数] [trialcycles=試しの V の回数] [trial=fixed(試しも毎刻み = 床の測り)] [galerkin=1]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <string>
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
    uint64_t budgetMk = 100;    // 予算を mK の整数で(0 = budgetShift を使う。既定 0.1 K = D-642)
    uint32_t zeroCycles = 1;    // 短い刻みの列の V の回数(T-1090。0 = cycles と同じ)
    int32_t shortExponent = 8;  // 短い刻み = e がこれ以下(shorte=。既定 8)
    bool unitByLength = true;   // 刻みを選ぶ単位を物の長さ 4 m で(D-640 の一旦。unit=block で外す)
    bool solverGalerkin = false;
    TimeTreeSelect selectRule = TimeTreeSelect::Previous;
    uint32_t groupShift = 0;   // 細かさの比べで 0.5 m の木の枠を揃える段(1 = 8 m のブロックに)
    uint32_t trialCycles = 0;  // 0 = solverCycles と同じ
    // 試しも毎刻みに(V の回数だけ変えて、毎刻みどうしの揺れ = 床を測る)
    bool trialFixed = false;
    // 試しの初めに x(m)のセル(y = z = 0)へ熱を 1 単位だけ足す(わずかな揺らしが前線でどこまで広がるか = 着火の閾値の床。T-1014)
    double perturbMeters = -1.0;
    // 前線の緩衝(buffer=1。T-1014)
    bool frontBuffer = false;
    // select= を書いた時は forward をその選び方だけで(T-1063。予算ごとに 1 通り目 + 前線の緩衝だけ確かめる)
    bool selectGiven = false;
    uint32_t bufferUnits = 0;  // 前線の緩衝を単位(groupShift の組)の数で(T-1074。0 = buffer= の形)

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
        setup.budgetQ16 = budgetMk != 0 ? budgetMk << 16 : (1ull << 16) >> budgetShift;
        setup.select = selectRule;
        setup.frontBuffer = frontBuffer;
        setup.frontBufferUnits = bufferUnits;
        setup.cyclesAtZero = zeroCycles;
        setup.shortExponent = shortExponent;
        setup.unitByLength = unitByLength;

        return setup;
    }

    // --- 木: 根 roots 個を x に並べる(レベル rootLevel)。refine なら根 1 の (x 0〜4 m・y・z 0〜4 m) を 0.5 m に ---
    struct TreeShape {
        int32_t rootLevel = -1;
        uint32_t roots = 4;
        bool refine = true;
    };

    // night・fire の木(shape=。T-1074): mixed = 既定(1 m の根 4 つ + 0.5 m の子 1 つ)・coarse / fine / finer = 1 m・0.5 m・0.25 m の根だけ
    // (どれも x に 32 m。ブロックの物の長さ 8 m・4 m・2 m)
    TreeShape sceneShape{};
    std::string shapeName = "mixed";

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
        const uint32_t blocks = std::max(16u, 2 * shape.roots);
        MultiresCapacity capacity{.worldBlocks = blocks,
                                  .observerBlocks = 0,
                                  .fractions = 16,
                                  .pages = blocks,
                                  .indexEntries = 2 * blocks,
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

    // 火事の前線(遊びに効く結果。T-1063): 着火した(A が減った)・半分燃えた最も遠いセルの中心の x と、燃えた量の割合
    constexpr uint64_t FRONT_EVERY = 256;  // 前線を見る刻みの間隔(約 4.3 秒)

    struct FrontSample {
        uint64_t tick = 0;
        double ignitedX = -1.0;  // A が初めより減った最も遠いセルの中心(m。無ければ −1)
        double halfX = -1.0;     // A が初めの半分以下の最も遠いセルの中心(m)
        double burned = 0.0;     // 燃えた A ÷ 初めの A(体積で重み)
    };

    struct RunResult {
        uint64_t hash = 0;
        std::vector<Snapshot> snapshots;
        double seconds = 0.0;
        bool conserved = true;
        uint64_t asleepChecks = 0;  // 眠っている枠を見た数
        uint64_t asleepWrong = 0;   // 導かれる値が眠った時の平均でなかった数
        TimeTreeSteps steps;
        std::vector<FrontSample> fronts;  // FRONT_EVERY ごと(火事)
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

    FrontSample TakeFront(const TimeTreeWorld& world, const Material& material, uint64_t tick) {
        const auto start = static_cast<int64_t>(std::llround(material.molPerCubic * LEVEL0_VOLUME * 1e6));
        FrontSample sample{.tick = tick};
        double burned = 0.0;
        double total = 0.0;
        for (const uint32_t slot : world.layout.blocks) {
            const MrBlock& block = world.nest.blocks[slot];
            const double volume = std::ldexp(1.0, -3 * block.level);
            const double size = CellMeters(block.level);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (world.layout.stepped[slot * MR_BLOCK_CELLS + index] == 0)
                    continue;

                const int64_t reactant = TimeTreeSettledReactant(world, slot, index);
                const double x = (static_cast<double>(block.originX + MrCellX(index)) + 0.5) * size;
                if (reactant < start)
                    sample.ignitedX = std::max(sample.ignitedX, x);

                if (2 * reactant <= start)
                    sample.halfX = std::max(sample.halfX, x);

                burned += static_cast<double>(start - reactant) * volume;
                total += static_cast<double>(start) * volume;
            }
        }

        sample.burned = total > 0.0 ? burned / total : 0.0;

        return sample;
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
                  uint64_t chunk, const Ignition& ignition = {}, bool snapshots = false, bool fronts = false) {
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

            if (fronts && (done / FRONT_EVERY + 1) * FRONT_EVERY < done + size)
                size = (done / FRONT_EVERY + 1) * FRONT_EVERY - done;

            StepTimeTree(setup, world, size);
            done += size;
            if (ignition.tick != 0 && done == ignition.tick)
                Ignite(setup, material, world, ignition);

            if (snapshots && done % COMPARE_EVERY == 0)
                result.snapshots.push_back(TakeSnapshot(world));

            if (fronts && done % FRONT_EVERY == 0)
                result.fronts.push_back(TakeFront(world, material, done));

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

    // --- 毎刻みの写しを使い回す(場面と V の回数ごとのファイル。試しの設定だけ変えて何度も測るため)---

    template <typename T>
    void WriteVector(std::ofstream& out, const std::vector<T>& values) {
        const uint64_t count = values.size();
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        out.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(count * sizeof(T)));
    }

    template <typename T>
    bool ReadVector(std::ifstream& in, std::vector<T>& values) {
        uint64_t count = 0;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!in || count > (1u << 24))
            return false;

        values.resize(count);
        in.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(count * sizeof(T)));

        return static_cast<bool>(in);
    }

    void SaveSnapshots(const std::string& path, const std::vector<Snapshot>& snapshots) {
        std::ofstream out(path, std::ios::binary);
        const uint64_t count = snapshots.size();
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (const Snapshot& snapshot : snapshots) {
            out.write(reinterpret_cast<const char*>(&snapshot.groundHeat), sizeof(snapshot.groundHeat));
            WriteVector(out, snapshot.energy);
            WriteVector(out, snapshot.reactant);
            WriteVector(out, snapshot.x);
            WriteVector(out, snapshot.level);
        }
    }

    std::optional<std::vector<Snapshot>> LoadSnapshots(const std::string& path, size_t expected) {
        std::ifstream in(path, std::ios::binary);
        uint64_t count = 0;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!in || count != expected)
            return std::nullopt;

        std::vector<Snapshot> snapshots(count);
        for (Snapshot& snapshot : snapshots) {
            in.read(reinterpret_cast<char*>(&snapshot.groundHeat), sizeof(snapshot.groundHeat));
            const bool read = ReadVector(in, snapshot.energy) && ReadVector(in, snapshot.reactant) &&
                              ReadVector(in, snapshot.x) && ReadVector(in, snapshot.level);
            if (!read)
                return std::nullopt;
        }

        return snapshots;
    }

    std::string BudgetText() {
        if (budgetMk >= 100)
            return std::format("{:g} K", static_cast<double>(budgetMk) / 1000.0);

        if (budgetMk != 0)
            return std::format("{} mK", budgetMk);

        return budgetShift == 0 ? std::string("1 mK") : std::format("1/{} mK", 1u << budgetShift);
    }

    std::string TrialName() {
        if (trialFixed)
            return std::format("毎刻み V {}", trialCycles != 0 ? trialCycles : solverCycles);

        if (selectRule == TimeTreeSelect::Jump)
            return std::format("退く道 1 本目・予算 {}", BudgetText());

        std::string buffer = frontBuffer ? "・前線の緩衝" : "";
        if (bufferUnits > 0)
            buffer = std::format("・前線の緩衝 {} 単位", bufferUnits);

        return std::format("{} 通り目{}・予算 {}・木 {}・揃える段 {}・V {}(1 刻み {})・単位 {}",
                           selectRule == TimeTreeSelect::Retake ? 2 : 1, buffer, BudgetText(), shapeName, groupShift,
                           trialCycles != 0 ? trialCycles : solverCycles, zeroCycles,
                           unitByLength ? "4 m" : "ブロック");
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
            "{}・打ち切り {}・取り直し {}・"
            "隣の差の最大 {}・負 {}・眠る {}・起こす {}・残差の計器の最大 {}(V {} 回{})",
            scene, name, run.seconds, cells, ticks, perCellTick, run.steps.solves, run.steps.events,
            run.steps.truncations, run.steps.retakes, run.steps.widestNeighborGap, run.steps.negativeValues,
            run.steps.sleeps, run.steps.wakes, run.steps.worstResidual, solverCycles,
            solverGalerkin ? "・Galerkin" : "");
        const double cellTicks = static_cast<double>(cells) * static_cast<double>(ticks);
        Log(Channel::Sim, Level::Info,
            "{} {}: 毎刻み(e = 0)で解いたセル {:.5f} /セル/刻み・そのうち自分の望みの e が 0 でない枠 "
            "{:.5f}・解いたセル {}",
            scene, name, static_cast<double>(run.steps.zeroCellSteps) / cellTicks,
            static_cast<double>(run.steps.forcedCellSteps) / cellTicks, run.steps.cellSteps);
        Log(Channel::Sim, Level::Info,
            "{} {}: e = 0 の解き {}・枠の組が変わった {}・組は同じで温度の帯が変わった 1 K {} / 10 K {} / 100 K "
            "{}(T-1090)",
            scene, name, run.steps.zeroSolves, run.steps.zeroReshaped, run.steps.zeroRebanded[0],
            run.steps.zeroRebanded[1], run.steps.zeroRebanded[2]);
    }

    // --- 1・4・5. 精度と費用(混ぜた木)---

    // 試しの初めに perturbMeters の x を含むセル(y = z = 0)へ熱を 1 単位(T-1014 の床)
    void Perturb(const TimeTreeSetup& setup, TimeTreeWorld& world) {
        for (const uint32_t slot : world.layout.blocks) {
            const MrBlock& block = world.nest.blocks[slot];
            const double size = CellMeters(block.level);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const double x = static_cast<double>(block.originX + MrCellX(index)) * size;
                const bool inside = perturbMeters >= x && perturbMeters < x + size;
                const bool corner = block.originY + MrCellY(index) == 0 && block.originZ + MrCellZ(index) == 0;
                if (!inside || !corner || world.layout.stepped[slot * MR_BLOCK_CELLS + index] == 0)
                    continue;

                InjectTimeTreeHeat(setup, world, slot, index, 1);
                Log(Channel::Sim, Level::Info, "揺らし: x {:.2f} m のセル(レベル {})へ熱 1 単位", x, block.level);
                return;
            }
        }
    }

    std::optional<std::vector<FrontSample>> LoadFronts(const std::string& path, size_t expected) {
        std::ifstream in(path, std::ios::binary);
        std::vector<FrontSample> fronts;
        if (!ReadVector(in, fronts) || fronts.size() != expected)
            return std::nullopt;

        return fronts;
    }

    // 毎刻みの写し(ファイルにあれば読む。無ければ走らせて書く)。fronts なら前線も(別のファイル。T-1063)
    std::vector<Snapshot> ReferenceSnapshots(std::string_view scene, std::string_view tag, const Material& material,
                                             const Profile& profile, uint64_t ticks, const Ignition& ignition,
                                             bool& conserved, std::vector<FrontSample>* fronts = nullptr) {
        const std::string name = std::format("time_step_tree_reference_{}_v{}{}", tag, solverCycles,
                                             solverGalerkin ? "g" : "");
        const std::string path = name + ".bin";
        const std::string frontPath = name + "_front.bin";
        auto loaded = LoadSnapshots(path, ticks / COMPARE_EVERY);
        auto loadedFronts = fronts ? LoadFronts(frontPath, ticks / FRONT_EVERY) : std::nullopt;
        if (loaded && (fronts == nullptr || loadedFronts)) {
            Log(Channel::Sim, Level::Info, "{} 毎刻み: 写し {} を使う", scene, path);
            if (fronts)
                *fronts = *std::move(loadedFronts);

            return *std::move(loaded);
        }

        // 毎刻みは V 4 のまま(写しのファイルの名前の V の回数どおり)
        TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Fixed);
        setup.cyclesAtZero = 0;
        const TimeTreeWorld world = MakeWorld(setup, material, sceneShape, profile);
        RunResult reference = Run(setup, material, world, ticks, ticks, ignition, true, fronts != nullptr);
        LogRun(scene, "毎刻み", world, reference, ticks);
        conserved = reference.conserved;
        SaveSnapshots(path, reference.snapshots);
        if (fronts) {
            std::ofstream out(frontPath, std::ios::binary);
            WriteVector(out, reference.fronts);
            *fronts = reference.fronts;
        }

        return std::move(reference.snapshots);
    }

    // 前線が x(1 m ごと)に届いた刻み(届かなければ 0)
    std::vector<uint64_t> Arrivals(const std::vector<FrontSample>& fronts, double length, bool half) {
        std::vector<uint64_t> arrivals;
        for (double x = 2.0; x < length; x += 1.0) {
            uint64_t tick = 0;
            for (const FrontSample& sample : fronts) {
                if ((half ? sample.halfX : sample.ignitedX) >= x) {
                    tick = sample.tick;
                    break;
                }
            }

            arrivals.push_back(tick);
        }

        return arrivals;
    }

    // 遊びに効く結果(着火・燃え広がり・燃えた量)が長い刻みと毎刻みでどれだけ違うか(T-1063。記録)
    void CompareFronts(std::string_view scene, const std::vector<FrontSample>& trial,
                       const std::vector<FrontSample>& reference, double length) {
        for (const bool half : {false, true}) {
            const std::vector<uint64_t> a = Arrivals(trial, length, half);
            const std::vector<uint64_t> b = Arrivals(reference, length, half);
            double worst = 0.0;
            double sum = 0.0;
            uint32_t both = 0;
            uint32_t onlyOne = 0;
            double farthest = 0.0;
            for (size_t i = 0; i < a.size(); ++i) {
                if ((a[i] == 0) != (b[i] == 0)) {
                    ++onlyOne;
                    continue;
                }

                if (a[i] == 0)
                    continue;

                const double gap = std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])) * TICK_SECONDS;
                worst = std::max(worst, gap);
                sum += gap;
                ++both;
                farthest = 2.0 + static_cast<double>(i);
            }

            Log(Channel::Sim, Level::Info,
                "{} 前線({}): 1 m ごとに届いた刻みの差 最大 {:.1f} 秒・平均 {:.2f} 秒({} か所・最も遠い {:.0f} m)・"
                "片方だけ届いた {} か所(前線を見る間隔 {:.1f} 秒)",
                scene, half ? "半分燃えた" : "着火", worst, both > 0 ? sum / both : 0.0, both, farthest, onlyOne,
                static_cast<double>(FRONT_EVERY) * TICK_SECONDS);
        }

        double burnedWorst = 0.0;
        double halfWorst = 0.0;
        for (size_t i = 0; i < trial.size() && i < reference.size(); ++i) {
            burnedWorst = std::max(burnedWorst, std::abs(trial[i].burned - reference[i].burned));
            halfWorst = std::max(halfWorst, std::abs(trial[i].halfX - reference[i].halfX));
        }

        const FrontSample& last = trial.back();
        const FrontSample& lastReference = reference.back();
        Log(Channel::Sim, Level::Info,
            "{} 燃えた量: 終わり 試し {:.4f} % / 毎刻み {:.4f} %・差の最大 {:.4f} % 点・半分燃えた前線の位置の差の最大 "
            "{:.2f} m・終わりの前線 試し {:.2f} m / 毎刻み {:.2f} m",
            scene, last.burned * 100.0, lastReference.burned * 100.0, burnedWorst * 100.0, halfWorst, last.halfX,
            lastReference.halfX);
    }

    void CheckScene(std::string_view scene, std::string_view tag, const Material& material, const Profile& profile,
                    uint64_t ticks, const Ignition& ignition, bool reactantExact) {
        const TreeShape shape = sceneShape;
        TimeTreeSetup trialSetup = MakeSetup(material, trialFixed ? TimeStepRule::Fixed : TimeStepRule::Change);
        trialSetup.groupShift = groupShift;
        if (trialCycles != 0)
            trialSetup.cycles = trialCycles;

        const std::string fullTag = shapeName == "mixed" ? std::string(tag) : std::format("{}_{}", tag, shapeName);

        bool referenceConserved = true;
        const bool fronts = ignition.tick != 0;
        std::vector<FrontSample> referenceFronts;
        const std::vector<Snapshot> referenceSnapshots = ReferenceSnapshots(scene, fullTag, material, profile, ticks,
                                                                            ignition, referenceConserved,
                                                                            fronts ? &referenceFronts : nullptr);
        TimeTreeWorld trialWorld = MakeWorld(trialSetup, material, shape, profile);
        if (perturbMeters >= 0.0)
            Perturb(trialSetup, trialWorld);

        const RunResult trial = Run(trialSetup, material, trialWorld, ticks, ticks, ignition, true, fronts);
        LogRun(scene, TrialName(), trialWorld, trial, ticks);
        if (fronts)
            CompareFronts(scene, trial.fronts, referenceFronts, WorldLength(shape));

        Difference worst;
        for (size_t i = 0; i < trial.snapshots.size(); ++i) {
            const Difference at = Compare(trialWorld, material, trial.snapshots[i], referenceSnapshots[i]);
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
            const double referenceMean = against(referenceSnapshots.back(), referenceCell);
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
        Expect(referenceConserved && trial.conserved, "保存: どの塊の境目でも Σ − 帳簿 が最初と同じ");
        // 退く道は隣の差の決まりを使わない(0 と上限が隣り合う)
        Expect(selectRule == TimeTreeSelect::Jump || trial.steps.widestNeighborGap <= 1,
               "隣どうしの e の差は 1 段まで");
        Expect(trial.steps.negativeValues == 0, "真実が負にならない");
        // 毎刻みとの差は記録だけ(D-633: 長い刻みの結果はそれ自体がこの宇宙の真実。守るのは寝ても待っても同じ・
        // 細かさによらない一貫性・決定性。T-1063)
        Log(Channel::Sim, Level::Info, "{} 毎刻みとの差(記録・D-633。合格の判定に使わない): 予算 {}", scene,
            BudgetText());
        Expect(trial.asleepWrong == 0, "眠った所の導かれる値は眠った時の平均のまま");
    }

}  // namespace

namespace {

    // --- 2. 細かさ: 0.5 m(根 0.5 m・8 つ)と 1 m(根 1 m・4 つ)で同じ場所に選ばれる e ---

    // pair: 0 = 0.5 m と 1 m(既定)・1 = 0.25 m と 0.5 m(どちらも単位 4 m。T-1063 の既定の確かめ)
    void CheckGranularity(std::string_view scene, const Material& material, const Profile& profile, uint64_t ticks,
                          const Ignition& ignition, int32_t pair = 0) {
        constexpr uint64_t SAMPLE_EVERY = 1024;
        const TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Change);
        // group=1 なら 0.5 m の木は x に隣り合う 2 枠(4 m × 2 = 8 m)を同じ e に揃える = 1 m の木のブロックと同じ物の長さ(T-0985)
        TimeTreeSetup fineSetup = setup;
        fineSetup.groupShift = groupShift;
        const TreeShape fineShape{.rootLevel = pair, .roots = 8u << pair, .refine = false};
        const TreeShape coarseShape{.rootLevel = pair - 1, .roots = 4u << pair, .refine = false};
        TimeTreeWorld fine = MakeWorld(fineSetup, material, fineShape, profile);
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

            StepTimeTree(fineSetup, fine, size);
            StepTimeTree(setup, coarse, size);
            done += size;
            if (ignition.tick != 0 && done == ignition.tick) {
                Ignite(fineSetup, material, fine, ignition);
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
            "細かさ {}({}・組 {}〔1 = 0.25 m と 0.5 m。e の平均の名前は 0.5 m / 1 m のまま〕・細かい方のブロック {:g} "
            "m): 同じ場所の e の差 平均 {:.2f}・1 段の内 {:.1f} %・最大 {}・e の平均 "
            "0.5 m "
            "{:.2f} / 1 m {:.2f}・解き 0.5 m {} / 1 m {}・取り直し {} / {}",
            scene, TrialName(), pair, std::ldexp(4.0, static_cast<int>(groupShift) - pair), meanGap / samples,
            share * 100.0, widest, meanFine / samples, meanCoarse / samples, fine.steps.cellSteps,
            coarse.steps.cellSteps, fine.steps.retakes, coarse.steps.retakes);
        Log(Channel::Sim, Level::Info,
            "細かさ {}(1 m の e と 0.5 m の 2 つの小さい方): 差 平均 {:.2f}・1 段の内 {:.1f} %・最大 {}", scene,
            meanPair / pairs, 100.0 * withinOnePair / pairs, widestPair);
        Expect(meanGap / samples <= 0.5 && share >= 0.9, "細かさ: 平均 0.5 段以下・9 割が 1 段の内");
    }

    // --- 3. 早送り = 待った(ビット一致)・6. 歩の途中のセーブ ---

    void CheckFastForward() {
        const Material material = Fire();
        const TimeTreeSetup setup = MakeSetup(material, TimeStepRule::Change);
        Log(Channel::Sim, Level::Info, "早送り・セーブ: {}", TrialName());
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
        for (int i = 2; i < argc; ++i) {
            const std::string_view argument = argv[i];
            const size_t equal = argument.find('=');
            const std::string_view key = argument.substr(0, equal);
            const std::string value(equal == std::string_view::npos ? "" : argument.substr(equal + 1));
            if (key == "select") {
                selectGiven = true;
                selectRule = value == "retake" ? TimeTreeSelect::Retake
                             : value == "jump" ? TimeTreeSelect::Jump
                                               : TimeTreeSelect::Previous;
            } else if (key == "budget") {
                budgetShift = static_cast<uint32_t>(std::atoi(value.c_str()));
                budgetMk = 0;
            } else if (key == "budgetmk")
                budgetMk = static_cast<uint64_t>(std::atoll(value.c_str()));
            else if (key == "zerocycles")
                zeroCycles = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "shorte")
                shortExponent = std::atoi(value.c_str());
            else if (key == "unit")
                unitByLength = value != "block";
            else if (key == "bufferunits")
                bufferUnits = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "shape") {
                shapeName = value;
                if (value == "coarse")
                    sceneShape = {.rootLevel = -1, .roots = 4, .refine = false};
                else if (value == "fine")
                    sceneShape = {.rootLevel = 0, .roots = 8, .refine = false};
                else if (value == "finer")
                    sceneShape = {.rootLevel = 1, .roots = 16, .refine = false};
                else
                    shapeName = "mixed";
            } else if (key == "group")
                groupShift = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "cycles")
                solverCycles = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "trialcycles")
                trialCycles = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "buffer")
                frontBuffer = value == "1";
            else if (key == "perturb")
                perturbMeters = std::atof(value.c_str());
            else if (key == "trial")
                trialFixed = value == "fixed";
            else if (key == "galerkin")
                solverGalerkin = value == "1";
        }

        const bool all = part == "all";
        const Material pond = Pond();
        const Material fire = Fire();
        const Ignition fireIgnition{FIRE_IGNITION_TICK, fire.groundKelvin};
        if (all || part == "forward") {
            // 1 通り目と退く道の 1 本目で(2 通り目は採らないので外した。T-1013)
            for (const TimeTreeSelect select : {TimeTreeSelect::Previous, TimeTreeSelect::Jump}) {
                if (selectGiven && select != selectRule)
                    continue;

                selectRule = select;
                CheckFastForward();
                CheckSave();
            }
        }

        if (all || part == "grain" || part == "grain-pond")
            CheckGranularity("池", pond, PondProfile(32.0), NIGHT_TICKS, {});

        if (all || part == "grain" || part == "grain-fire")
            CheckGranularity("火事", fire, Ambient(fire.groundKelvin), FIRE_TICKS, fireIgnition);

        if (part == "grain-fine-pond")
            CheckGranularity("池", pond, PondProfile(32.0), NIGHT_TICKS, {}, 1);

        if (part == "grain-fine-fire")
            CheckGranularity("火事", fire, Ambient(fire.groundKelvin), FIRE_TICKS, fireIgnition, 1);

        if (all || part == "night")
            CheckScene("池(静かな夜)", "night", pond, PondProfile(WorldLength(TreeShape{})), NIGHT_TICKS, {}, true);

        if (all || part == "fire")
            CheckScene("火事", "fire", fire, Ambient(fire.groundKelvin), FIRE_TICKS, fireIgnition, false);

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
