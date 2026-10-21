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
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
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

    TimeStepLabSetup WithTrial(TimeStepLabSetup setup, const Trial& trial) {
        setup.rule = trial.rule;
        setup.fixedExponent = 0;
        setup.maxExponent = MAX_EXPONENT;
        setup.budget = BUDGET;

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

    // --- 3. 早送り = 待った(ビット一致)---

    void CheckFastForward() {
        FireParameters fire;
        fire.burningMeters = 24.0;
        for (const Trial& trial : TRIALS) {
            const TimeStepLabSetup setup = WithTrial(MakeFireSetup(fire), trial);
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

    int Run() {
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

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
