// time_step_lab_test.cpp — 研究 R-T2(刻みの長さを状態が変わる速さで選ぶ。T-0856・ADR-0390・ADR-0410)の CPU の試作の試験。
// 試作は engine/src/sim/time_step_lab.{h,cpp}・式は shaders/common/time_step.hlsli・場面は tests/time_step_lab_scene.h。確かめること:
//   1. 精度: 一定に流れる川(熱と溶けた物を運ぶ・底でゆっくり発酵)と燃え広がる火を、選んだ長い刻みで進めても、
//      短い刻み(毎刻み)と計器の精度の内で同じか(温度 1 mK・溶けた物 1 ppm・運ばれ出た量と反応の進みは 100 万分の 1)。
//      2 本の試し(進め方はどちらも後ろ向きの Euler + 反応は閉じた形。選び方を変える): 試し 1 = 規則 A(変化の量)・
//      試し 2 = 規則 B(1 回と半分 2 回の差 = 誤差の見積もり)
//   2. 細かさ: 同じ川を 0.5 m と 2 m のセルで進め(ブロックはどちらも 8 m)、同じ場所で選ばれる刻みの長さ(e)の差
//      (合格: 平均 0.5 段以下・9 割が 1 段の内。前線の通る間は打ち切りの連なり方で数段ずれることがある)
//   3. 早送り: 1 刻みずつ・一気に・ばらばらの塊で進めて、状態の要約がビット一致(火を付ける刻みも同じ)。2 回の実行でも一致
//   4. 保存: どの塊の境目でも Σ(真実 + 歩の変化)− 帳簿 が最初と同じ・隣どうしの e の差は 1 段まで
//   5. 費用: 一晩(1 日 = 現実の約 1 時間の世界の半日 = 108,000 刻み)を進めた CPU の時間と、セル 1 つあたりの解きの回数
// 合格の線(静かな夜 5 秒・火事 30 秒)は CPU の試作の 1 回の解きの時間から、線の内に進められるセルの数をログに出す(未確認)。
// 試し 2 は精度で落ちた(記録だけ。judged = false)。
// T-1169: flow [scene=0〜5] [flowbudget=n](流れ続ける所〔川・速い川・風〕で選ばれる刻みの長さ・早送りの GPU の待ちの見積もり・毎刻みとの差。
//         引数なしは今までの全部)
// T-1170: flow [flowarrival=印](これから来る変化の形を比べる。既定 4 = 境のセルに積もる分を見る)・[flowtrace=刻み](枠ごとの差)
// T-1173: place / placetime(しきい値の場面を置く場所・始める刻みでずらす。placeblock= で単位のセルの数)
// T-1174: grain(単位の形を 0.5 m・1 m・2 m のセルで比べる。D-640)・window= / slope=(滑る窓の半径・窓の外の坂。m)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "sim/time_step_lab.h"
#include "time_step_lab_scene.h"

using namespace bicameral;
using namespace bicameral::sim;
using namespace bicameral::test;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    constexpr int32_t MAX_EXPONENT = 14;                      // 2^14 刻み = 約 4.6 分
    constexpr uint64_t COMPARE_EVERY = 1ull << MAX_EXPONENT;  // どの歩もこの倍数の刻みで揃う(比べる所)
    constexpr uint64_t NIGHT_TICKS = 7 *
                                     COMPARE_EVERY;  // 一晩: 半日 108,000 刻み(1 日 = 現実の約 1 時間。D-450)を切り上げ
    constexpr uint64_t BUDGET = 1;                   // 1 歩の変化・誤差の許す大きさ(精度の単位 = 1 mK・1 ppm)

    // judged = 合格を試験で確かめる(試し 2 は精度で落ちたので記録だけ。T-0856 の作業ログ)
    struct Trial {
        std::string_view name;
        TimeStepRule rule = TimeStepRule::Change;
        bool judged = true;
    };

    constexpr Trial REFERENCE = {"毎刻み", TimeStepRule::Fixed, true};
    constexpr Trial TRIALS[] = {
        {"試し 1(変化の量)", TimeStepRule::Change, true},
        {"試し 2(誤差の見積もり)", TimeStepRule::Estimate, false},
    };

    uint32_t trialArrival = 4;  // arrival=: 試し 1 の arrivalLook(T-1170・T-1173。既定 4 = 面で積もる分を見る)

    TimeStepLabSetup WithTrial(TimeStepLabSetup setup, const Trial& trial) {
        setup.rule = trial.rule;
        setup.arrivalLook = trial.rule == TimeStepRule::Change ? trialArrival : 0;
        setup.fixedExponent = 0;
        setup.maxExponent = MAX_EXPONENT;
        setup.budget = BUDGET;

        return setup;
    }

    double windowMeters = 0.0;  // window=: 滑る窓の半径(m。T-1174。0 = 外す)
    double slopeMeters = 0.0;   // slope=: 窓の外の坂(1 段あたりの長さ m。0 = 枠ごとに 1 段 = 今)
    double pileMeters = 0.0;    // pilemeters=: 面で積もる分を広げて見る長さ(m。0 = 境のセル 1 つ = 今)
    // T-1175: spread=1(積もる分を長い側の続きのセルへ広げる)・spreadcells=n・meanmeters=(望みを ±m の平均の変化で)・
    // slack=n(窓の打ち切りを歩の残りが上限の 2^n 倍を超える時だけ)
    bool spreadPile = false;
    uint32_t spreadCells = 64;
    uint32_t spreadShift = 0;
    double meanMeters = 0.0;
    uint32_t truncateSlack = 0;

    std::string T1175Tag() {
        return std::format("spread={}/{} mean={} slack={}", spreadPile ? 1 : 0, spreadShift, meanMeters, truncateSlack);
    }

    TimeStepLabSetup WithWindow(TimeStepLabSetup setup, double cellMeters) {
        const double blockMeters = cellMeters * setup.blockCells;
        setup.windowBlocks = static_cast<uint32_t>(std::llround(windowMeters / blockMeters));
        setup.slopeBlocks = std::max<uint32_t>(1, static_cast<uint32_t>(std::llround(slopeMeters / blockMeters)));
        setup.pileCells = std::max<uint32_t>(1, static_cast<uint32_t>(std::llround(pileMeters / cellMeters)));
        setup.spreadPile = spreadPile;
        setup.spreadCells = spreadCells;
        setup.spreadShift = spreadShift;
        setup.meanCells = static_cast<uint32_t>(std::llround(meanMeters / blockMeters));
        setup.truncateSlack = truncateSlack;

        return setup;
    }

    // --- 走り(塊ごとに保存を確かめ、COMPARE_EVERY ごとに写しを取る)---

    struct Snapshot {
        std::vector<int64_t> settled;  // [量 × cellCount + セル]
        TimeStepLabLedger ledger;
    };

    struct Ignition {
        uint64_t tick = 0;  // 0 = 付けない
        int64_t heat = 0;
        uint32_t cells = 0;
    };

    struct RunResult {
        TimeStepLabState state;
        std::vector<Snapshot> snapshots;
        double seconds = 0.0;
        bool conserved = true;
    };

    bool SameTotals(const TimeStepLabTotals& a, const TimeStepLabTotals& b) {
        return a.heat == b.heat && a.dissolved == b.dissolved && a.reactant == b.reactant;
    }

    Snapshot TakeSnapshot(const TimeStepLabSetup& setup, const TimeStepLabState& state) {
        Snapshot snapshot;
        snapshot.ledger = state.ledger;
        for (uint32_t q = 0; q < TIME_STEP_QUANTITIES; ++q) {
            for (uint32_t cell = 0; cell < setup.cellCount; ++cell)
                snapshot.settled.push_back(TimeStepLabSettled(setup, state, q, cell));
        }

        return snapshot;
    }

    RunResult Run(const TimeStepLabSetup& setup, TimeStepLabState state, uint64_t ticks, uint64_t chunk,
                  const Ignition& ignition = {}, bool snapshots = false) {
        RunResult result;
        const TimeStepLabTotals first = ConservedTimeStepLab(setup, state);
        const auto begin = std::chrono::steady_clock::now();
        uint64_t done = 0;
        while (done < ticks) {
            uint64_t size = std::min(chunk, ticks - done);
            if (ignition.tick > done && ignition.tick < done + size)
                size = ignition.tick - done;

            if (snapshots && (done / COMPARE_EVERY + 1) * COMPARE_EVERY < done + size)
                size = (done / COMPARE_EVERY + 1) * COMPARE_EVERY - done;

            StepTimeStepLab(setup, state, size);
            done += size;
            if (ignition.tick != 0 && done == ignition.tick) {
                for (uint32_t cell = 0; cell < ignition.cells; ++cell)
                    InjectTimeStepLabHeat(setup, state, cell, ignition.heat);
            }

            if (snapshots && done % COMPARE_EVERY == 0)
                result.snapshots.push_back(TakeSnapshot(setup, state));

            result.conserved = result.conserved && SameTotals(first, ConservedTimeStepLab(setup, state));
        }

        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        result.state = std::move(state);

        return result;
    }

    // --- 比べる(計器の精度の単位)---

    struct Difference {
        double temperatureMk = 0.0;      // セルの温度の差の最大
        double dissolvedPpm = 0.0;       // セルの D の差の最大
        double reactantMillionth = 0.0;  // セルの A の差の最大(referenceAmount の 100 万分の 1)
        double carriedMk = 0.0;          // 流れ出した熱の差 ÷ 流れ出した水の容量(流れ出た水の平均の温度の差)
        double carriedPpm = 0.0;         // 流れ出した D の差 ÷ 流れ出した水(平均の濃さの差)
        double reactedMillionth = 0.0;   // 反応の進みの差 ÷ 初めの A の合計(100 万分の 1)
    };

    double Gap(int64_t a, int64_t b, double scale) {
        return scale <= 0.0 ? 0.0 : std::abs(static_cast<double>(a - b)) / scale;
    }

    // ticks 刻みの比べ。流れ出た水の量(y の単位の容量)は流れの係数 × 刻み
    Difference Compare(const TimeStepLabSetup& setup, const Snapshot& trial, const Snapshot& reference,
                       uint64_t ticks) {
        Difference difference;
        const uint32_t cells = setup.cellCount;
        for (uint32_t cell = 0; cell < cells; ++cell) {
            for (uint32_t q = 0; q < TIME_STEP_CARRIED; ++q) {
                const double capacity = static_cast<double>(setup.carried[q].capacityQ16[cell]) / Q16;
                const size_t index = q * cells + cell;
                double& worst = q == TIME_STEP_HEAT ? difference.temperatureMk : difference.dissolvedPpm;
                worst = std::max(worst, Gap(trial.settled[index], reference.settled[index], capacity));
            }

            const size_t reactant = TIME_STEP_REACTANT * cells + cell;
            const double amount = Gap(trial.settled[reactant], reference.settled[reactant],
                                      static_cast<double>(setup.referenceAmount));
            difference.reactantMillionth = std::max(difference.reactantMillionth, amount * 1e6);
        }

        const double heatWater = static_cast<double>(setup.carried[0].outflowQ16) / Q16 * static_cast<double>(ticks);
        const double dissolvedWater = static_cast<double>(setup.carried[1].outflowQ16) / Q16 *
                                      static_cast<double>(ticks);
        difference.carriedMk = Gap(trial.ledger.outflow[0], reference.ledger.outflow[0], heatWater);
        difference.carriedPpm = Gap(trial.ledger.outflow[1], reference.ledger.outflow[1], dissolvedWater);
        const double initial = static_cast<double>(setup.referenceAmount) * setup.cellCount;
        difference.reactedMillionth = Gap(trial.ledger.reacted, reference.ledger.reacted, initial) * 1e6;

        return difference;
    }

    Difference Worst(const Difference& a, const Difference& b) {
        Difference worst;
        worst.temperatureMk = std::max(a.temperatureMk, b.temperatureMk);
        worst.dissolvedPpm = std::max(a.dissolvedPpm, b.dissolvedPpm);
        worst.reactantMillionth = std::max(a.reactantMillionth, b.reactantMillionth);
        worst.carriedMk = std::max(a.carriedMk, b.carriedMk);
        worst.carriedPpm = std::max(a.carriedPpm, b.carriedPpm);
        worst.reactedMillionth = std::max(a.reactedMillionth, b.reactedMillionth);

        return worst;
    }

    bool WithinPrecision(const Difference& difference) {
        return difference.temperatureMk <= 1.0 && difference.dissolvedPpm <= 1.0 &&
               difference.reactantMillionth <= 1.0 && difference.carriedMk <= 1.0 && difference.carriedPpm <= 1.0 &&
               difference.reactedMillionth <= 1.0;
    }

}  // namespace

namespace {

    // --- 1・5. 精度と費用: 川(一晩)---

    RiverParameters RiverAt(double cellMeters) {
        RiverParameters river;
        river.cellMeters = cellMeters;

        return river;
    }

    TimeStepLabState FilledRiver(const RiverParameters& river, const TimeStepLabSetup& setup) {
        TimeStepLabState state = MakeTimeStepLabState(setup);
        FillRiver(river, setup, state);

        return state;
    }

    // 川の発酵は温度によらないので、セルの A の本当の値は閉じた形。毎刻みの答えも端数の丸め(決まった乱数)で
    // 本当の値の周りに揺れる(1 晩で 100 万分の 1 ほど)ので、A は毎刻みとではなく本当の値と比べる(100 万分の 1 の最大)
    double ReactantAgainstExact(const RiverParameters& river, const TimeStepLabSetup& setup,
                                const TimeStepLabState& state, uint64_t ticks) {
        const double start = static_cast<double>(std::llround(river.fermentMolPerCubic * river.cellMeters * 1e6));
        const double rate = static_cast<double>(setup.rateColdQ32) / Q32;
        const double exact = start * std::exp(-rate * static_cast<double>(ticks));
        double worst = 0.0;
        for (uint32_t cell = 0; cell < setup.cellCount; ++cell) {
            const double value = static_cast<double>(TimeStepLabSettled(setup, state, TIME_STEP_REACTANT, cell));
            worst = std::max(worst, std::abs(value - exact) / static_cast<double>(setup.referenceAmount) * 1e6);
        }

        return worst;
    }

    void LogDifference(std::string_view scene, std::string_view trial, const Difference& d) {
        Log(Channel::Sim, Level::Info,
            "{} {}: セルの温度 {:.3f} mK・D {:.3f} ppm・A {:.3f}/100 万・運ばれ出た熱 {:.3f} mK・D {:.3f} "
            "ppm・反応の進み {:.3f}/100 万",
            scene, trial, d.temperatureMk, d.dissolvedPpm, d.reactantMillionth, d.carriedMk, d.carriedPpm,
            d.reactedMillionth);
    }

    void LogCost(std::string_view scene, std::string_view trial, const TimeStepLabSetup& setup, const RunResult& run,
                 uint64_t ticks) {
        const double perCellTick = static_cast<double>(run.state.cellSteps) /
                                   (static_cast<double>(setup.cellCount) * static_cast<double>(ticks));
        Log(Channel::Sim, Level::Info,
            "{} {}: {:.3f} 秒(CPU・{} セル・{} 刻み)・解き {:.5f} 回/セル/刻み・出来事 {}・打ち切り {}・隣の差の最大 "
            "{}・負 {}",
            scene, trial, run.seconds, setup.cellCount, ticks, perCellTick, run.state.events, run.state.truncations,
            run.state.widestNeighborGap, run.state.negativeValues);
    }

    // 合格の線への見積もり(未確認): CPU の試作の 1 回の解きの時間から、線の秒数で一晩(火事は FIRE_TICKS)を進められる
    // 起きたセルの数。本物は GPU で、世界の大きさ(起きたセルの数)と最低機は未確認(Q11)
    void LogBudget(std::string_view scene, const TimeStepLabSetup& setup, const RunResult& run, uint64_t ticks,
                   double limitSeconds) {
        const double solves = static_cast<double>(run.state.cellSteps);
        const double secondsPerSolve = run.seconds / std::max(1.0, solves);
        const double perCellTick = solves / (static_cast<double>(setup.cellCount) * static_cast<double>(ticks));
        const double cells = limitSeconds / (secondsPerSolve * perCellTick * static_cast<double>(ticks));
        Log(Channel::Sim, Level::Info,
            "合格の線 {}: {:.1f} 秒の内に進められるセル {:.0f}(CPU の試作 1 スレッド・1 回の解き {:.1f} ns・"
            "解き {:.4f} 回/セル/刻み。毎刻みなら 2 回。未確認: 本物は GPU・世界の大きさと最低機は Q11 の後)",
            scene, limitSeconds, cells, secondsPerSolve * 1e9, perCellTick);
    }

    void CheckRiver() {
        const RiverParameters river = RiverAt(0.5);
        const TimeStepLabSetup base = MakeRiverSetup(river);
        const TimeStepLabSetup referenceSetup = WithTrial(base, REFERENCE);
        const RunResult reference = Run(referenceSetup, FilledRiver(river, referenceSetup), NIGHT_TICKS, NIGHT_TICKS,
                                        {}, true);
        Expect(reference.conserved, "川 毎刻み: 保存");
        LogCost("川", REFERENCE.name, referenceSetup, reference, NIGHT_TICKS);
        Log(Channel::Sim, Level::Info, "川 毎刻み: セルの A の本当の値との差 {:.3f}/100 万",
            ReactantAgainstExact(river, referenceSetup, reference.state, NIGHT_TICKS));

        for (const Trial& trial : TRIALS) {
            const TimeStepLabSetup setup = WithTrial(base, trial);
            const RunResult run = Run(setup, FilledRiver(river, setup), NIGHT_TICKS, NIGHT_TICKS, {}, true);
            Difference worst;
            for (size_t i = 0; i < run.snapshots.size(); ++i)
                worst = Worst(worst, Compare(setup, run.snapshots[i], reference.snapshots[i], (i + 1) * COMPARE_EVERY));

            LogDifference("川", trial.name, worst);
            LogCost("川", trial.name, setup, run, NIGHT_TICKS);
            const double reactant = ReactantAgainstExact(river, setup, run.state, NIGHT_TICKS);
            Log(Channel::Sim, Level::Info, "川 {}: セルの A の本当の値との差 {:.3f}/100 万", trial.name, reactant);
            worst.reactantMillionth = reactant;
            Expect(run.conserved, "川: 保存(どの塊の境目でも Σ − 帳簿 が最初と同じ)");
            Expect(run.state.widestNeighborGap <= 1, "川: 隣どうしの e の差は 1 段まで");
            if (!trial.judged)
                continue;

            Expect(WithinPrecision(worst), "川: 毎刻みと計器の精度の内で同じ(A は本当の値と)");
            Expect(run.state.negativeValues == 0, "川: 真実が負にならない");
            LogBudget("静かな夜(川)", setup, run, NIGHT_TICKS, 5.0);
        }
    }

}  // namespace

namespace {

    // --- 1・5. 精度と費用: 火事(静かな床に火を付けて燃え広がる)---

    constexpr uint64_t FIRE_IGNITION_TICK = 3 * 4096 + 17;  // 長い刻みで寝ている途中に付ける
    constexpr uint64_t FIRE_TICKS = 4 * COMPARE_EVERY;      // 約 18 分

    Ignition FireIgnition(const FireParameters& fire) {
        return {FIRE_IGNITION_TICK, IgnitionHeatPerCell(fire), CellCount(1.0, fire.cellMeters)};
    }

    TimeStepLabState FilledFire(const FireParameters& fire, const TimeStepLabSetup& setup) {
        TimeStepLabState state = MakeTimeStepLabState(setup);
        FillFire(fire, setup, state);

        return state;
    }

    // 燃え広がった先(燃える物が半分を切ったセルの数 × dx)
    double BurntMeters(const FireParameters& fire, const TimeStepLabSetup& setup, const TimeStepLabState& state) {
        uint32_t burnt = 0;
        const int64_t start = std::llround(fire.fuelMolPerCubic * fire.cellMeters * 1e6);
        for (uint32_t cell = 0; cell < setup.cellCount; ++cell) {
            if (TimeStepLabSettled(setup, state, TIME_STEP_REACTANT, cell) * 2 < start)
                ++burnt;
        }

        return burnt * fire.cellMeters;
    }

    void CheckFire() {
        FireParameters fire;
        const TimeStepLabSetup base = MakeFireSetup(fire);
        const TimeStepLabSetup referenceSetup = WithTrial(base, REFERENCE);
        const RunResult reference = Run(referenceSetup, FilledFire(fire, referenceSetup), FIRE_TICKS, FIRE_TICKS,
                                        FireIgnition(fire), true);
        Expect(reference.conserved, "火 毎刻み: 保存");
        LogCost("火", REFERENCE.name, referenceSetup, reference, FIRE_TICKS);
        Log(Channel::Sim, Level::Info, "火 毎刻み: 燃え広がった長さ {:.1f} m",
            BurntMeters(fire, base, reference.state));
        for (const Trial& trial : TRIALS) {
            const TimeStepLabSetup setup = WithTrial(base, trial);
            const RunResult run = Run(setup, FilledFire(fire, setup), FIRE_TICKS, FIRE_TICKS, FireIgnition(fire), true);
            Difference worst;
            for (size_t i = 0; i < run.snapshots.size(); ++i)
                worst = Worst(worst, Compare(setup, run.snapshots[i], reference.snapshots[i], (i + 1) * COMPARE_EVERY));

            LogDifference("火", trial.name, worst);
            LogCost("火", trial.name, setup, run, FIRE_TICKS);
            Log(Channel::Sim, Level::Info, "火 {}: 燃え広がった長さ {:.1f} m", trial.name,
                BurntMeters(fire, base, run.state));
            Expect(run.conserved, "火: 保存");
            Expect(run.state.widestNeighborGap <= 1, "火: 隣どうしの e の差は 1 段まで");
            if (!trial.judged)
                continue;

            Expect(WithinPrecision(worst), "火: 毎刻みと計器の精度の内で同じ");
            LogBudget("火事", setup, run, FIRE_TICKS, 30.0);
        }
    }

    // --- 2. 細かさ: 0.5 m と 2 m で同じ場所に選ばれる e ---

    void CheckGranularity() {
        constexpr uint64_t SAMPLE_EVERY = 1024;
        for (const Trial& trial : TRIALS) {
            const RiverParameters fine = RiverAt(0.5);
            const RiverParameters coarse = RiverAt(2.0);
            const TimeStepLabSetup fineSetup = WithTrial(MakeRiverSetup(fine), trial);
            const TimeStepLabSetup coarseSetup = WithTrial(MakeRiverSetup(coarse), trial);
            TimeStepLabState fineState = FilledRiver(fine, fineSetup);
            TimeStepLabState coarseState = FilledRiver(coarse, coarseSetup);
            int32_t widest = 0;
            double meanFine = 0.0;
            double meanCoarse = 0.0;
            double meanGap = 0.0;
            uint32_t withinOne = 0;
            uint32_t samples = 0;
            for (uint64_t done = 0; done < NIGHT_TICKS; done += SAMPLE_EVERY) {
                StepTimeStepLab(fineSetup, fineState, SAMPLE_EVERY);
                StepTimeStepLab(coarseSetup, coarseState, SAMPLE_EVERY);

                // ブロックはどちらも同じ 8 m(0.5 m で 16 セル・2 m で 4 セル)。同じ場所のブロックの e を比べる
                for (uint32_t block = 0; block < coarseState.exponent.size(); ++block) {
                    const int32_t gap = std::abs(fineState.exponent[block] - coarseState.exponent[block]);
                    widest = std::max(widest, gap);
                    meanGap += gap;
                    withinOne += gap <= 1 ? 1u : 0u;
                    meanFine += fineState.exponent[block];
                    meanCoarse += coarseState.exponent[block];
                    ++samples;
                }
            }

            const double share = static_cast<double>(withinOne) / samples;
            Log(Channel::Sim, Level::Info,
                "細かさ {}: 同じ場所の e の差 平均 {:.2f}・1 段の内 {:.1f} %・最大 {}・e の平均 0.5 m {:.2f} / 2 m "
                "{:.2f}",
                trial.name, meanGap / samples, share * 100.0, widest, meanFine / samples, meanCoarse / samples);
            if (trial.judged)
                Expect(meanGap / samples <= 0.5 && share >= 0.9,
                       "細かさ: 0.5 m と 2 m で同じ場所の e の差が平均 0.5 段以下・9 割が 1 段の内");
        }
    }

    // --- T-1169: 流れ続ける所(川・風)で選ばれる刻みの長さ・早送りの待ち・毎刻みとの差 ---
    // 流れの速さそのもの(運動量の家族)はこの試作では決まった値(面の流れの係数)で、運ばれる熱・溶けた物(風では煙)の変化の速さだけを見る
    // (運動量を長い刻みで進めるのは T-0952)。予算は flowbudget=(精度の単位。既定 100 = 0.1 K・100 ppm = D-642)、
    // 刻みを選ぶ単位(ブロック)は 4 m(D-643)。引数: flow [scene=0〜5(既定 全部)] [flowbudget=n]
    struct FlowScene {
        std::string_view name;
        double speed = 0.3;       // m/s
        double dispersion = 1.0;  // 乱れで混ざる(m²/s。熱と D に同じ)
        double heatPerCubic = WATER_HEAT;
        double dissolvedPerPpm = WATER_MICROMOL_PPM;  // µmol / (m³·ppm)
        double groundPerCubic = 2.0;                  // W/(m³·K)
        double groundKelvin = 283.0;
        double inflowKelvin = 288.0;
        double parcelKelvin = 0.0;  // 初めに下っていく暖かい所の高さ(K。0 = 流れ込みと同じ = 一定に流れる)
        double inflowPpm = 200.0;
        double parcelPpm = 0.0;
        bool ferment = true;  // 底でゆっくり発酵(川)
    };

    constexpr double AIR_HEAT = 1200.0;        // J/(m³·K)
    constexpr double AIR_MICROMOL_PPM = 41.6;  // 1 m³ の空気の 100 万分の 1(µmol)
    constexpr double FLOW_CELL_METERS = 0.5;
    constexpr double FLOW_LENGTH_METERS = 64.0;
    double flowBlockMeters = 4.0;  // flowblock=(m)
    constexpr uint64_t FLOW_SAMPLE_EVERY = 64;

    constexpr FlowScene FLOW_SCENES[] = {
        {.name = "川(一定に流れる・0.3 m/s)"},
        {.name = "川(暖かい水が下る・0.3 m/s)", .parcelKelvin = 2.0, .parcelPpm = 60.0},
        {.name = "速い川(1.5 m/s)", .speed = 1.5, .dispersion = 3.0, .parcelKelvin = 0.0},
        {.name = "速い川(暖かい水が下る・1.5 m/s)",
         .speed = 1.5,
         .dispersion = 3.0,
         .parcelKelvin = 2.0,
         .parcelPpm = 60.0},
        {.name = "風(一定に吹く・3 m/s)",
         .speed = 3.0,
         .dispersion = 5.0,
         .heatPerCubic = AIR_HEAT,
         .dissolvedPerPpm = AIR_MICROMOL_PPM,
         .groundPerCubic = 10.0,
         .inflowPpm = 0.0,
         .ferment = false},
        {.name = "風(暖かい空気と煙が流れる・3 m/s)",
         .speed = 3.0,
         .dispersion = 5.0,
         .heatPerCubic = AIR_HEAT,
         .dissolvedPerPpm = AIR_MICROMOL_PPM,
         .groundPerCubic = 10.0,
         .parcelKelvin = 2.0,
         .inflowPpm = 0.0,
         .parcelPpm = 60.0,
         .ferment = false},
    };

    uint64_t flowBudget = 100;
    bool flowLook = false;     // flowlook=1: 今の変化の速さで e に上限(rateLook)
    uint32_t flowArrival = 4;  // flowarrival=: これから来る変化で e に上限(arrivalLook の印。T-1170。既定 4)
    int flowSceneIndex = -1;

    TimeStepLabSetup MakeFlowSetup(const FlowScene& scene) {
        const double dx = FLOW_CELL_METERS;
        const uint32_t cells = CellCount(FLOW_LENGTH_METERS, dx);
        TimeStepLabSetup setup;
        setup.cellCount = cells;
        setup.blockCells = CellCount(flowBlockMeters, dx);
        setup.inflowCell = 0;
        setup.outflowCell = cells - 1;

        CarriedValues heat;
        heat.capacity = scene.heatPerCubic;
        heat.conductivity = scene.heatPerCubic * scene.dispersion;
        heat.speed = scene.speed;
        heat.ground = scene.groundPerCubic;
        heat.groundValue = scene.groundKelvin * 1000.0;
        heat.inflowValue = scene.inflowKelvin * 1000.0;
        SetCarried(setup.carried[TIME_STEP_HEAT], heat, cells, dx);
        CarriedValues dissolved;
        dissolved.capacity = scene.dissolvedPerPpm;
        dissolved.conductivity = scene.dissolvedPerPpm * scene.dispersion;
        dissolved.speed = scene.speed;
        dissolved.inflowValue = scene.inflowPpm;
        SetCarried(setup.carried[TIME_STEP_DISSOLVED], dissolved, cells, dx);

        const RiverParameters river;
        setup.fuel.assign(cells, scene.ferment ? TIME_STEP_FUEL_SLOW : TIME_STEP_FUEL_NONE);
        setup.rateColdQ32 = ToQ(std::log(2.0) / river.fermentHalfLifeSeconds * TICK_SECONDS, Q32);
        setup.heatPerAmount = static_cast<int64_t>(std::llround(river.fermentJoulePerMol * 1e-3));
        setup.referenceAmount = ToQ(river.fermentMolPerCubic * dx * 1e6, 1.0);

        return setup;
    }

    TimeStepLabState FilledFlow(const FlowScene& scene, const TimeStepLabSetup& setup) {
        TimeStepLabState state = MakeTimeStepLabState(setup);
        const RiverParameters river;
        const double dx = FLOW_CELL_METERS;
        const uint32_t cells = setup.cellCount;
        for (uint32_t cell = 0; cell < cells; ++cell) {
            const double x = (cell + 0.5) * dx;
            const double shape = 0.5 * (1.0 + std::tanh((x - FLOW_LENGTH_METERS * 0.25) / river.frontMeters));
            const double temperature = scene.inflowKelvin + scene.parcelKelvin * shape;
            const double ppm = scene.inflowPpm + scene.parcelPpm * shape;
            state.values[cell] = std::llround(scene.heatPerCubic * dx * temperature * 1000.0);
            state.values[cells + cell] = std::llround(scene.dissolvedPerPpm * dx * ppm);
            state.values[2 * cells + cell] = scene.ferment ? std::llround(river.fermentMolPerCubic * dx * 1e6) : 0;
        }

        return state;
    }

    struct FlowRun {
        RunResult run;
        std::vector<double> meanExponent;  // 1/7 晩(COMPARE_EVERY 刻み)ごとのブロックの e の平均
        std::vector<int32_t> lowestExponent;
    };

    FlowRun RunFlow(const TimeStepLabSetup& setup, TimeStepLabState state) {
        FlowRun flow;
        const TimeStepLabTotals first = ConservedTimeStepLab(setup, state);
        const auto begin = std::chrono::steady_clock::now();
        double sum = 0.0;
        uint64_t samples = 0;
        int32_t lowest = MAX_EXPONENT;
        for (uint64_t done = 0; done < NIGHT_TICKS; done += FLOW_SAMPLE_EVERY) {
            StepTimeStepLab(setup, state, FLOW_SAMPLE_EVERY);
            for (const int32_t exponent : state.exponent) {
                sum += exponent;
                lowest = std::min(lowest, exponent);
                ++samples;
            }

            if ((done + FLOW_SAMPLE_EVERY) % COMPARE_EVERY != 0)
                continue;

            flow.run.snapshots.push_back(TakeSnapshot(setup, state));
            flow.run.conserved = flow.run.conserved && SameTotals(first, ConservedTimeStepLab(setup, state));
            flow.meanExponent.push_back(sum / static_cast<double>(samples));
            flow.lowestExponent.push_back(lowest);
            sum = 0.0;
            samples = 0;
            lowest = MAX_EXPONENT;
        }

        flow.run.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        flow.run.state = std::move(state);

        return flow;
    }

    // 早送りの GPU の待ちの見積もり(T-1152 の費用の式・RTX 3070 Ti の既定〔続けて積む形〕。1 出来事 = 段 1・V 1 の組 1 つと見る)。
    // この列は断面 1 m² なので、川幅・風の幅 4 m × 深さ 4 m(0.5 m のセルで 8 × 8)の流れとして 64 倍のセルで数える(見積もり)
    double FlowGpuSeconds(const TimeStepLabState& state) {
        constexpr double EVENT_NS = 19000.0 + 20000.0;
        constexpr double CELL_NS = 0.86 + 2.57;
        constexpr double CROSS_SECTION_CELLS = 64.0;

        return (static_cast<double>(state.events) * EVENT_NS +
                static_cast<double>(state.cellSteps) * CROSS_SECTION_CELLS * CELL_NS) *
               1e-9;
    }

    void CheckFlowScene(const FlowScene& scene) {
        const TimeStepLabSetup base = MakeFlowSetup(scene);
        TimeStepLabSetup reference = WithTrial(base, REFERENCE);
        TimeStepLabSetup trial = WithTrial(base, TRIALS[0]);
        trial.budget = flowBudget;
        trial.rateLook = flowLook;
        trial.arrivalLook = flowArrival;
        trial = WithWindow(trial, FLOW_CELL_METERS);
        const FlowRun fixed = RunFlow(reference, FilledFlow(scene, reference));
        const FlowRun chosen = RunFlow(trial, FilledFlow(scene, trial));
        Difference worst;
        for (size_t i = 0; i < chosen.run.snapshots.size(); ++i)
            worst = Worst(worst,
                          Compare(trial, chosen.run.snapshots[i], fixed.run.snapshots[i], (i + 1) * COMPARE_EVERY));

        LogDifference(scene.name, std::format("予算 {}", flowBudget), worst);

        // --- 温度の差がどこで・いつ大きいか(写しごと)---
        const double capacity = static_cast<double>(trial.carried[TIME_STEP_HEAT].capacityQ16[0]) / Q16;
        for (size_t i = 0; i < chosen.run.snapshots.size(); ++i) {
            double largest = 0.0;
            uint32_t where = 0;
            for (uint32_t cell = 0; cell < trial.cellCount; ++cell) {
                const double gap = Gap(chosen.run.snapshots[i].settled[cell], fixed.run.snapshots[i].settled[cell],
                                       capacity);
                if (gap > largest) {
                    largest = gap;
                    where = cell;
                }
            }

            Log(Channel::Sim, Level::Info, "{} 予算 {}: 刻み {} の温度の差の最大 {:.3f} mK(x {:.2f} m)", scene.name,
                flowBudget, (i + 1) * COMPARE_EVERY, largest, (where + 0.5) * FLOW_CELL_METERS);
        }
        LogCost(scene.name, "毎刻み", reference, fixed.run, NIGHT_TICKS);
        LogCost(scene.name, std::format("予算 {}", flowBudget), trial, chosen.run, NIGHT_TICKS);
        std::string exponents;
        for (size_t i = 0; i < chosen.meanExponent.size(); ++i)
            exponents += std::format("{}{:.2f}(最小 {})", i == 0 ? "" : "・", chosen.meanExponent[i],
                                     chosen.lowestExponent[i]);

        Log(Channel::Sim, Level::Info, "{} 予算 {}: e の平均(1/7 晩ごと){}", scene.name, flowBudget, exponents);
        Log(Channel::Sim, Level::Info,
            "{} 予算 {}: 一晩の早送りの GPU の待ち(見積もり・4 m × 4 m の流れ){:.3f} 秒・毎刻みなら {:.3f} 秒",
            scene.name, flowBudget, FlowGpuSeconds(chosen.run.state), FlowGpuSeconds(fixed.run.state));
        Log(Channel::Sim, Level::Info,
            "FLOW scene=\"{}\" block={} window={} slope={} look={} arrival={} budget={} solves={:.6f} events={} "
            "fixedEvents={} gpu={:.4f} "
            "fixedGpu={:.4f} "
            "tempMk={:.3f} "
            "ppm={:.3f} carriedMk={:.3f} meanE={} {} spreads={}",
            scene.name, flowBlockMeters, windowMeters, slopeMeters, flowLook ? 1 : 0, flowArrival, flowBudget,
            static_cast<double>(chosen.run.state.cellSteps) / (static_cast<double>(trial.cellCount) * NIGHT_TICKS),
            chosen.run.state.events, fixed.run.state.events, FlowGpuSeconds(chosen.run.state),
            FlowGpuSeconds(fixed.run.state), worst.temperatureMk, worst.dissolvedPpm, worst.carriedMk, exponents,
            T1175Tag(), chosen.run.state.spreads);
        Expect(fixed.run.conserved && chosen.run.conserved, "流れ: 保存");
        Expect(chosen.run.state.widestNeighborGap <= 1, "流れ: 隣どうしの e の差は 1 段まで");
    }

    // 枠ごとの e と毎刻みとの温度の差(歩の変化も足した値・枠の中の最大)を flowtrace= 刻みごとに出す(T-1170 の見立て)
    uint64_t flowTrace = 0;

    void TraceFlowScene(const FlowScene& scene) {
        const TimeStepLabSetup base = MakeFlowSetup(scene);
        TimeStepLabSetup reference = WithTrial(base, REFERENCE);
        TimeStepLabSetup trial = WithTrial(base, TRIALS[0]);
        trial.budget = flowBudget;
        trial.arrivalLook = flowArrival;
        TimeStepLabState fixed = FilledFlow(scene, reference);
        TimeStepLabState chosen = FilledFlow(scene, trial);
        const double capacity = static_cast<double>(trial.carried[TIME_STEP_HEAT].capacityQ16[0]) / Q16;
        for (uint64_t done = flowTrace; done <= 2 * COMPARE_EVERY; done += flowTrace) {
            StepTimeStepLab(reference, fixed, flowTrace);
            StepTimeStepLab(trial, chosen, flowTrace);
            std::string exponents;
            std::string gaps;
            std::string temperatures;
            for (uint32_t b = 0; b < trial.cellCount / trial.blockCells; ++b) {
                double largest = 0.0;
                for (uint32_t c = b * trial.blockCells; c < (b + 1) * trial.blockCells; ++c) {
                    const double gap = static_cast<double>(TimeStepLabSettled(trial, chosen, TIME_STEP_HEAT, c) -
                                                           TimeStepLabSettled(reference, fixed, TIME_STEP_HEAT, c)) /
                                       capacity;
                    if (std::abs(gap) > std::abs(largest))
                        largest = gap;
                }

                const uint32_t middle = b * trial.blockCells + trial.blockCells / 2;
                const double kelvin = static_cast<double>(
                                          TimeStepLabSettled(reference, fixed, TIME_STEP_HEAT, middle)) /
                                      capacity * 1e-3;
                exponents += std::format("{:>4}", chosen.exponent[b]);
                gaps += std::format(" {:>6.1f}", largest);
                temperatures += std::format(" {:>6.3f}", kelvin - scene.inflowKelvin);
            }

            Log(Channel::Sim, Level::Info, "TRACE {:>6} e  {}", done, exponents);
            Log(Channel::Sim, Level::Info, "TRACE {:>6} mK {}", done, gaps);
            Log(Channel::Sim, Level::Info, "TRACE {:>6} K  {}", done, temperatures);
        }
    }

    void CheckFlow() {
        for (size_t i = 0; i < std::size(FLOW_SCENES); ++i) {
            if (flowSceneIndex < 0 || static_cast<size_t>(flowSceneIndex) == i)
                CheckFlowScene(FLOW_SCENES[i]);
        }
    }

    // --- T-1173: 置く場所・始める刻みによる違い(しきい値の近く。T-1153 の場面を 1 本の列で)---
    // 燃えない熱い物(幅 2 m・placeheaterk=)を x = 12 + o m に置き、0.5 m あけた床の初めのセル(x = 14.5 + o m)の真実
    // (歩の境目ごとの値 = 反応が読む値)の山を、毎刻みと長い刻み(予算 0.1 K・単位 placeblock= セル)で比べる。物の値は火
    // (FireParameters)・列 32 m。燃える物は置かない(着火の前は反応が 0 なので、真実の山が 600 K に届くかで着火が決まる。T-1153)。
    // 引数: place [placeblock=セル] [placearrival=0〜2] [placeheaterk=K] [placestart=刻み] [placeticks=n]
    //       placetime(ずらし 0 m で、始める刻みを 0・1・2・3・5・8・13・1000 にずらす)
    double placeHeaterKelvin = 1570.48;
    uint32_t placeBlockCells = 8;
    uint32_t placeArrival = 4;
    uint64_t placeStartTick = 0;
    uint64_t placeTicks = 8192;
    constexpr double PLACE_LENGTH_METERS = 32.0;
    constexpr double PLACE_IGNITION_MK = 600000.0;

    struct PlaceResult {
        double peakMk = 0.0;
        int64_t peakValue = 0;   // 真実の山(mJ。ビット一致を見る)
        uint64_t reachTick = 0;  // 600 K に届いた刻み(始めてから。0 = 届かない)
        uint64_t events = 0;
        double solves = 0.0;  // 回 / セル / 刻み
    };

    TimeStepLabSetup MakePlaceSetup(bool fixed) {
        FireParameters fire;
        fire.lengthMeters = PLACE_LENGTH_METERS;
        TimeStepLabSetup setup = MakeFireSetup(fire);
        setup.fuel.assign(setup.cellCount, TIME_STEP_FUEL_NONE);
        setup.blockCells = placeBlockCells;
        setup = WithTrial(setup, fixed ? REFERENCE : TRIALS[0]);
        setup.budget = 100;
        setup.arrivalLook = fixed ? 0 : placeArrival;
        if (!fixed)
            setup = WithWindow(setup, fire.cellMeters);

        return setup;
    }

    PlaceResult RunPlace(const TimeStepLabSetup& setup, uint32_t offsetCells, uint64_t startTick) {
        FireParameters fire;
        fire.lengthMeters = PLACE_LENGTH_METERS;
        TimeStepLabState state = MakeTimeStepLabState(setup);
        FillFire(fire, setup, state);
        const double capacity = fire.heatPerCubic * fire.cellMeters;  // mJ / mK
        const uint32_t heaterFirst = CellCount(12.0, fire.cellMeters) + offsetCells;
        const uint32_t heaterCells = CellCount(2.0, fire.cellMeters);
        for (uint32_t cell = heaterFirst; cell < heaterFirst + heaterCells; ++cell)
            state.values[cell] = std::llround(capacity * placeHeaterKelvin * 1000.0);

        state.tick = startTick;
        for (uint64_t& end : state.end)
            end = startTick;

        const uint32_t floor = CellCount(14.5, fire.cellMeters) + offsetCells;
        PlaceResult result;
        for (uint64_t done = 1; done <= placeTicks; ++done) {
            StepTimeStepLab(setup, state, 1);
            const double mk = static_cast<double>(state.values[floor]) / capacity;
            result.peakMk = std::max(result.peakMk, mk);
            result.peakValue = std::max(result.peakValue, state.values[floor]);
            if (result.reachTick == 0 && mk >= PLACE_IGNITION_MK)
                result.reachTick = done;
        }

        result.events = state.events;
        result.solves = static_cast<double>(state.cellSteps) /
                        (static_cast<double>(setup.cellCount) * static_cast<double>(placeTicks));

        return result;
    }

    void LogPlace(std::string_view label, const PlaceResult& fixed, const PlaceResult& chosen) {
        Log(Channel::Sim, Level::Info,
            "PLACE {} block={} window={} slope={} arrival={} heaterK={:.3f} fixedPeakMk={:.4f} diffMk={:.4f} "
            "fixedReach={} reach={} "
            "events={} fixedEvents={} solves={:.4f} fixedPeakValue={} peakValue={}",
            label, placeBlockCells, windowMeters, slopeMeters, placeArrival, placeHeaterKelvin, fixed.peakMk,
            chosen.peakMk - fixed.peakMk, fixed.reachTick, chosen.reachTick, chosen.events, fixed.events, chosen.solves,
            fixed.peakValue, chosen.peakValue);
    }

    void CheckPlace(bool shiftTime) {
        const TimeStepLabSetup fixedSetup = MakePlaceSetup(true);
        const TimeStepLabSetup chosenSetup = MakePlaceSetup(false);
        double lowest = 1e30;
        double highest = -1e30;
        double fixedLowest = 1e30;
        double fixedHighest = -1e30;
        constexpr uint64_t START_TICKS[] = {0, 1, 2, 3, 5, 8, 13, 1000};
        const size_t runs = shiftTime ? std::size(START_TICKS) : 7;
        for (size_t i = 0; i < runs; ++i) {
            const uint32_t offset = shiftTime ? 0 : static_cast<uint32_t>(i);
            const uint64_t start = shiftTime ? START_TICKS[i] : placeStartTick;
            const PlaceResult fixed = RunPlace(fixedSetup, offset, start);
            const PlaceResult chosen = RunPlace(chosenSetup, offset, start);
            LogPlace(std::format("offset={:.1f}m start={}", offset * 0.5, start), fixed, chosen);
            const double gap = chosen.peakMk - fixed.peakMk;
            lowest = std::min(lowest, gap);
            highest = std::max(highest, gap);
            fixedLowest = std::min(fixedLowest, fixed.peakMk);
            fixedHighest = std::max(fixedHighest, fixed.peakMk);
        }

        Log(Channel::Sim, Level::Info,
            "PLACEBAND {} block={} window={} slope={} arrival={} band(長い刻み − 毎刻み の幅)={:.4f} "
            "mK({:.4f}〜{:.4f})・毎刻みの山の幅 "
            "{:.6f} mK",
            shiftTime ? "time" : "space", placeBlockCells, windowMeters, slopeMeters, placeArrival, highest - lowest,
            lowest, highest, fixedHighest - fixedLowest);
    }

    // --- T-1174: 刻みを選ぶ単位の形を 0.5 m・1 m・2 m のセルで比べる(D-640「0.5 m と 1 m の細かさで刻みが揃う」)---
    // 川(一晩)と火事(18 分)を、同じ単位の形(grainunit= m の格子。0 = 1 セル。window=・slope= で滑る窓)で各細かさで走らせ、
    // 0.5 m ごとの場所で選ばれた e を 1,024 刻みごとに比べる(0.5 m のセルと、1 m・2 m のセル)。毎刻みとの差(同じ細かさ)も。
    // 引数: grain [grainunit=m(既定 4)] [grainbudget=n(既定 100 = 0.1 K)] [grainscene=0 川・1 火(既定 両方)] [window=m] [slope=m]
    double grainUnitMeters = 4.0;
    uint64_t grainBudget = 100;
    int grainScene = -1;
    constexpr double GRAIN_SAMPLE_METERS = 0.5;
    constexpr uint64_t GRAIN_SAMPLE_EVERY = 1024;
    constexpr double GRAIN_CELL_METERS[] = {0.5, 1.0, 2.0};

    struct GrainRun {
        std::vector<std::vector<int32_t>> exponents;  // [標本][0.5 m ごとの場所]
        Difference worst;                             // 毎刻み(同じ細かさ)との差
        RunResult run;
    };

    std::vector<int32_t> SampleExponents(const TimeStepLabSetup& setup, const TimeStepLabState& state,
                                         double cellMeters) {
        const uint32_t places = static_cast<uint32_t>(std::llround(setup.cellCount * cellMeters / GRAIN_SAMPLE_METERS));
        std::vector<int32_t> sampled(places);
        for (uint32_t p = 0; p < places; ++p) {
            const uint32_t cell = static_cast<uint32_t>((p + 0.5) * GRAIN_SAMPLE_METERS / cellMeters);
            sampled[p] = state.exponent[cell / setup.blockCells];
        }

        return sampled;
    }

    GrainRun RunGrain(const TimeStepLabSetup& setup, TimeStepLabState state, double cellMeters, uint64_t ticks,
                      const Ignition& ignition, const RunResult& reference) {
        GrainRun grain;
        const TimeStepLabTotals first = ConservedTimeStepLab(setup, state);
        const auto begin = std::chrono::steady_clock::now();
        for (uint64_t done = 0; done < ticks;) {
            uint64_t size = GRAIN_SAMPLE_EVERY - done % GRAIN_SAMPLE_EVERY;
            if (ignition.tick > done && ignition.tick < done + size)
                size = ignition.tick - done;

            StepTimeStepLab(setup, state, size);
            done += size;
            if (ignition.tick != 0 && done == ignition.tick) {
                for (uint32_t cell = 0; cell < ignition.cells; ++cell)
                    InjectTimeStepLabHeat(setup, state, cell, ignition.heat);
            }

            if (done % GRAIN_SAMPLE_EVERY == 0)
                grain.exponents.push_back(SampleExponents(setup, state, cellMeters));

            if (done % COMPARE_EVERY == 0) {
                const Snapshot& expected = reference.snapshots[done / COMPARE_EVERY - 1];
                grain.worst = Worst(grain.worst, Compare(setup, TakeSnapshot(setup, state), expected, done));
            }

            grain.run.conserved = grain.run.conserved && SameTotals(first, ConservedTimeStepLab(setup, state));
        }

        grain.run.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        grain.run.state = std::move(state);

        return grain;
    }

    TimeStepLabSetup GrainTrial(TimeStepLabSetup setup, double cellMeters) {
        setup.blockCells = grainUnitMeters <= 0.0 ? 1 : CellCount(grainUnitMeters, cellMeters);
        setup = WithTrial(setup, TRIALS[0]);
        setup.budget = grainBudget;

        return WithWindow(setup, cellMeters);
    }

    void CheckGrainScene(bool fire) {
        const std::string_view name = fire ? "火事" : "川";
        const uint64_t ticks = fire ? FIRE_TICKS : NIGHT_TICKS;
        std::vector<GrainRun> runs;
        for (const double dx : GRAIN_CELL_METERS) {
            FireParameters fireScene;
            fireScene.cellMeters = dx;
            const RiverParameters river = RiverAt(dx);
            const TimeStepLabSetup base = fire ? MakeFireSetup(fireScene) : MakeRiverSetup(river);
            const auto filled = [&](const TimeStepLabSetup& setup) {
                return fire ? FilledFire(fireScene, setup) : FilledRiver(river, setup);
            };
            const Ignition ignition = fire ? FireIgnition(fireScene) : Ignition{};
            const TimeStepLabSetup referenceSetup = WithTrial(base, REFERENCE);
            const RunResult reference = Run(referenceSetup, filled(referenceSetup), ticks, ticks, ignition, true);
            const TimeStepLabSetup setup = GrainTrial(base, dx);
            runs.push_back(RunGrain(setup, filled(setup), dx, ticks, ignition, reference));
            const GrainRun& grain = runs.back();
            double meanExponent = 0.0;
            size_t samples = 0;
            for (const std::vector<int32_t>& row : grain.exponents) {
                for (const int32_t e : row) {
                    meanExponent += e;
                    ++samples;
                }
            }

            const double solves = static_cast<double>(grain.run.state.cellSteps) /
                                  (static_cast<double>(setup.cellCount) * static_cast<double>(ticks));
            Log(Channel::Sim, Level::Info,
                "GRAIN scene={} unit={} window={} slope={} budget={} cell={} tempMk={:.3f} ppm={:.3f} carriedMk={:.3f} "
                "reacted={:.3f} solves={:.5f} events={} truncations={} gap={} conserved={} meanE={:.3f} seconds={:.2f} "
                "{} spreads={}",
                name, grainUnitMeters, windowMeters, slopeMeters, grainBudget, dx, grain.worst.temperatureMk,
                grain.worst.dissolvedPpm, grain.worst.carriedMk, grain.worst.reactedMillionth, solves,
                grain.run.state.events, grain.run.state.truncations, grain.run.state.widestNeighborGap,
                grain.run.conserved ? 1 : 0, meanExponent / std::max<size_t>(samples, 1), grain.run.seconds, T1175Tag(),
                grain.run.state.spreads);
            Expect(grain.run.conserved, "細かさ(T-1174): 保存");
            Expect(grain.run.state.widestNeighborGap <= 1, "細かさ(T-1174): 隣どうしの e の差は 1 段まで");
        }

        // --- 0.5 m のセルと、1 m・2 m のセルで同じ場所の e ---
        for (size_t other = 1; other < runs.size(); ++other) {
            double meanGap = 0.0;
            uint64_t withinOne = 0;
            uint64_t samples = 0;
            int32_t widest = 0;
            double signedGap = 0.0;    // 細かい − 粗い(負 = 細かい方が短い)
            uint64_t nearSamples = 0;  // どちらかの e が 3 以下(前線の近く)
            uint64_t nearWithin = 0;
            for (size_t i = 0; i < runs[0].exponents.size(); ++i) {
                for (size_t p = 0; p < runs[0].exponents[i].size(); ++p) {
                    const int32_t fine = runs[0].exponents[i][p];
                    const int32_t coarse = runs[other].exponents[i][p];
                    const int32_t gap = std::abs(fine - coarse);
                    meanGap += gap;
                    signedGap += fine - coarse;
                    withinOne += gap <= 1 ? 1u : 0u;
                    widest = std::max(widest, gap);
                    ++samples;
                    if (std::min(fine, coarse) <= 3) {
                        ++nearSamples;
                        nearWithin += gap <= 1 ? 1u : 0u;
                    }
                }
            }

            // 時間で均した e の場所ごとの形(歩の始まりのずれを均す)の差
            const size_t places = runs[0].exponents.empty() ? 0 : runs[0].exponents[0].size();
            double profileGap = 0.0;
            for (size_t p = 0; p < places; ++p) {
                double fineSum = 0.0;
                double coarseSum = 0.0;
                for (size_t i = 0; i < runs[0].exponents.size(); ++i) {
                    fineSum += runs[0].exponents[i][p];
                    coarseSum += runs[other].exponents[i][p];
                }

                profileGap += std::abs(fineSum - coarseSum) / static_cast<double>(runs[0].exponents.size());
            }

            Log(Channel::Sim, Level::Info,
                "GRAINPAIR scene={} unit={} window={} slope={} pile={} budget={} arrival={} cells=0.5-{} "
                "meanGap={:.3f} "
                "withinOne={:.1f} widest={} profileGap={:.3f} signedGap={:.3f} near={:.1f}%:{:.1f} far={:.1f} {}",
                name, grainUnitMeters, windowMeters, slopeMeters, pileMeters, grainBudget, trialArrival,
                GRAIN_CELL_METERS[other], meanGap / static_cast<double>(samples),
                100.0 * static_cast<double>(withinOne) / samples, widest,
                profileGap / static_cast<double>(std::max<size_t>(places, 1)), signedGap / static_cast<double>(samples),
                100.0 * static_cast<double>(nearSamples) / static_cast<double>(samples),
                100.0 * static_cast<double>(nearWithin) / static_cast<double>(std::max<uint64_t>(nearSamples, 1)),
                100.0 * static_cast<double>(withinOne - nearWithin) /
                    static_cast<double>(std::max<uint64_t>(samples - nearSamples, 1)),
                T1175Tag());
        }
    }

    void CheckGrain() {
        for (int scene = 0; scene < 2; ++scene) {
            if (grainScene < 0 || grainScene == scene)
                CheckGrainScene(scene == 1);
        }
    }

    // --- 3. 早送り = 待った(ビット一致)---

    void CheckFastForward() {
        FireParameters fire;
        fire.burningMeters = 24.0;
        for (const Trial& trial : TRIALS) {
            TimeStepLabSetup setup = WithTrial(MakeFireSetup(fire), trial);
            if (windowMeters > 0.0 || slopeMeters > 0.0) {
                // ff window=・slope=: 滑る窓(1 セルの単位)でも早送りがビット一致か(T-1174)
                setup.blockCells = 1;
                setup = WithWindow(setup, fire.cellMeters);
            }

            const uint64_t ticks = 2 * COMPARE_EVERY + 777;
            const Ignition ignition = FireIgnition(fire);
            const RunResult waited = Run(setup, FilledFire(fire, setup), ticks, 1, ignition);
            const RunResult skipped = Run(setup, FilledFire(fire, setup), ticks, ticks, ignition);
            const RunResult uneven = Run(setup, FilledFire(fire, setup), ticks, 997, ignition);
            const RunResult again = Run(setup, FilledFire(fire, setup), ticks, ticks, ignition);
            const uint64_t hash = HashTimeStepLab(waited.state);
            Expect(hash == HashTimeStepLab(skipped.state), "早送り: 1 刻みずつと一気にがビット一致");
            Expect(hash == HashTimeStepLab(uneven.state), "早送り: ばらばらの塊とビット一致");
            Expect(hash == HashTimeStepLab(again.state), "2 回の実行でビット一致");
            Expect(waited.conserved && skipped.conserved && uneven.conserved, "早送り: どの塊の境目でも保存");
            Log(Channel::Sim, Level::Info, "早送り {}: 要約 {:016x}・1 刻みずつ {:.3f} 秒・一気に {:.3f} 秒",
                trial.name, hash, waited.seconds, skipped.seconds);
        }
    }

    int Run(int argc, char** argv) {
        const std::string_view part = argc > 1 ? argv[1] : "all";
        for (int i = 2; i < argc; ++i) {
            const std::string_view argument = argv[i];
            const size_t equal = argument.find('=');
            const std::string_view key = argument.substr(0, equal);
            const std::string value(equal == std::string_view::npos ? "" : argument.substr(equal + 1));
            if (key == "scene")
                flowSceneIndex = std::atoi(value.c_str());
            else if (key == "flowlook")
                flowLook = value == "1";
            else if (key == "flowblock")
                flowBlockMeters = std::atof(value.c_str());
            else if (key == "flowbudget")
                flowBudget = static_cast<uint64_t>(std::atoll(value.c_str()));
            else if (key == "arrival")
                trialArrival = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "flowtrace")
                flowTrace = static_cast<uint64_t>(std::atoll(value.c_str()));
            else if (key == "flowarrival")
                flowArrival = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "placeblock")
                placeBlockCells = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "placearrival")
                placeArrival = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "placeheaterk")
                placeHeaterKelvin = std::atof(value.c_str());
            else if (key == "placestart")
                placeStartTick = static_cast<uint64_t>(std::atoll(value.c_str()));
            else if (key == "placeticks")
                placeTicks = static_cast<uint64_t>(std::atoll(value.c_str()));
            else if (key == "window")
                windowMeters = std::atof(value.c_str());
            else if (key == "slope")
                slopeMeters = std::atof(value.c_str());
            else if (key == "pilemeters")
                pileMeters = std::atof(value.c_str());
            else if (key == "spread")
                spreadPile = std::atoi(value.c_str()) != 0;
            else if (key == "spreadshift")
                spreadShift = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "spreadcells")
                spreadCells = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "meanmeters")
                meanMeters = std::atof(value.c_str());
            else if (key == "slack")
                truncateSlack = static_cast<uint32_t>(std::atoi(value.c_str()));
            else if (key == "grainunit")
                grainUnitMeters = std::atof(value.c_str());
            else if (key == "grainbudget")
                grainBudget = static_cast<uint64_t>(std::atoll(value.c_str()));
            else if (key == "grainscene")
                grainScene = std::atoi(value.c_str());
        }

        if (part == "flow" && flowTrace != 0) {
            TraceFlowScene(FLOW_SCENES[std::max(flowSceneIndex, 0)]);

            return 0;
        }

        if (part == "flow") {
            CheckFlow();

            return failureCount == 0 ? 0 : 1;
        }

        if (part == "ff") {
            CheckFastForward();

            return failureCount == 0 ? 0 : 1;
        }

        if (part == "grain") {
            CheckGrain();

            return failureCount == 0 ? 0 : 1;
        }

        if (part == "place" || part == "placetime") {
            CheckPlace(part == "placetime");

            return failureCount == 0 ? 0 : 1;
        }

        CheckFastForward();
        CheckGranularity();
        CheckRiver();
        CheckFire();
        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "time_step_lab_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "time_step_lab_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(argc, argv);
    SingletonFinalizer::Finalize();

    return exitCode;
}
