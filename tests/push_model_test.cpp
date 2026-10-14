// push_model_test.cpp — 坂を上らせる構造(押す)の一般の形の模型(D-487 ⑤・T-0495。docs/design/19 §5 の段 5・§11・ADR-0228)。CPU だけの研究の模型。
//
// 坂の原理の P4「ある流れが坂を上るのは、同じ所で別の流れが坂を下り、合計のエントロピーの生成が ≥ 0 の時だけ」(19 §2.1)を、
// T-0493 の交差の引きずり(ADR-0220。結合の強さ q・L_xy = q √(L_xx L_yy))と同じ芯で書く:
//   押す構造 = 「蓄え X が自分の坂を下る流れ」と「押される物 Y の流れ」の交差を、構造が自分の係数(速さ P・1 単位を押す費用 w)で持つもの。
//   エネルギーの単位にそろえた坂(f_X = 蓄え 1 mJ が熱になる時の自由エネルギー = 1、f_Y = Y を 1 単位動かす時の自由エネルギーの下がり)で
//     J_X = P (1 + q y)、J_Y = (P ÷ w)(q + y)、y = f_Y ÷ w(負 = 坂の上へ)
//   合計の生成 T σ = J_X f_X + J_Y f_Y = P (1 + 2 q y + y²) は |q| ≤ 1 ならどの状態でも ≥ 0。効率 −J_Y f_Y ÷ J_X の最大は q だけで決まる
//   (Kedem–Caplan: η_max = q² ÷ (1 + √(1 − q²))²)。|q| > 1 は効率 > 1 になりうる値で、ベイクが警告して ±1 に押さえる。
//   q = 1 は「きっちり結合」: 1 mJ で 1 ÷ w だけ押す比の機械で、坂が費用に追いつくと(y = −1)止まり、蓄えも使わない。逆に押し返されると発電機として回る。
// 模型: 2 つの部屋(同じ体積・同じ温度の理想気体)と熱の溜め(構造と気体の熱)と蓄え(仕事として使える mJ)。ポンプが部屋 A から B へ気体を押す。
// 模型の状態を進める所は整数だけで書く(エンジンに入れる時の形の下書き)。倍精度は測る所(エントロピー・効率)だけ。結果は docs/tickets/T-0495-push-structure-general-form.md。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "common/fixed.hlsli"

namespace {

    namespace fx = bicameral::fx;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // a × b ÷ divisor を 0 の方向へ切り捨てる(128bit の途中の値。divisor > 0)
    int64_t MulDivToZero(int64_t a, int64_t b, uint64_t divisor) {
        const bool negative = (a < 0) != (b < 0);
        const uint64_t magnitude = fx::FxDivU128By64(fx::FxMulU64Full(fx::FxAbsU64(a), fx::FxAbsU64(b)), divisor)
                                       .quotient;
        return fx::FxApplySign(magnitude, negative);
    }

    // a × b ÷ divisor を +∞ の方向へ丸める(蓄えの使い方を多めに数える = 熱を多めに出す側。坂を上る向きの丸めにならない)
    int64_t MulDivUp(int64_t a, int64_t b, uint64_t divisor) {
        const bool negative = (a < 0) != (b < 0);
        const fx::FxDivResult result = fx::FxDivU128By64(fx::FxMulU64Full(fx::FxAbsU64(a), fx::FxAbsU64(b)), divisor);
        if (negative)
            return fx::FxApplySign(result.quotient, true);

        return (int64_t)(result.quotient + (result.remainder != 0 ? 1u : 0u));
    }

    constexpr int64_t ONE_Q16 = 65536;
    constexpr int64_t ONE_Q32 = (int64_t)1 << 32;

    // =============================================================================================
    // 押す構造の表の 1 行(作り手が書くもの。Luau の案は T-0495 のチケット)と、ベイク
    // =============================================================================================

    struct MachineEntry {
        int64_t couplingQ16 = 0;  // q(Q16)。1 = きっちり結合(滑りなし)。0 = 蓄えを熱にするだけ
        int64_t costQ16 = 0;  // w: Y を 1 単位(µmol)押すのに使う蓄え(mJ。Q16)。歯車の比。どんな正の値でも法則は破れない
        int64_t speed = 0;    // P: 負荷のない時に蓄えを使う速さ(mJ/刻み)
    };

    struct BakedMachine {
        int64_t couplingQ16 = 0;
        int64_t costQ16 = 0;
        int64_t speed = 0;
        int64_t peakEfficiencyQ16 = 0;  // η_max(q)(パネルに出す。Q16)
        std::string warning;
    };

    // ベイクの検査: |q| > 1 は生成の行列が半正定でなくなる(効率 > 1 の状態がある = 永久機関)。警告して ±1 に押さえる(D-487 ②)。
    // 費用 w ≤ 0・速さ < 0 は形が成り立たないので、警告して止めた構造にする。
    // 効率の上限 η_max = (1 − s) ÷ (1 + s)、s = √(1 − q²)(Q16 の整数で。パネルの表示)
    BakedMachine BakeMachine(const MachineEntry& entry) {
        BakedMachine baked{
            .couplingQ16 = entry.couplingQ16, .costQ16 = entry.costQ16, .speed = entry.speed, .warning = {}};
        if (fx::FxAbsU64(entry.couplingQ16) > (uint64_t)ONE_Q16) {
            baked.couplingQ16 = entry.couplingQ16 < 0 ? -ONE_Q16 : ONE_Q16;
            baked.warning = "coupling |q| > 1 allows efficiency > 1 (perpetual motion); clamped to 1. ";
        }

        if (entry.costQ16 <= 0 || entry.speed < 0) {
            baked.costQ16 = ONE_Q16;
            baked.speed = 0;
            baked.warning += "cost must be > 0 and speed >= 0; machine disabled. ";
        }

        const uint64_t slipQ32 = (uint64_t)(ONE_Q32 - baked.couplingQ16 * baked.couplingQ16);
        const int64_t rootQ16 = (int64_t)fx::FxSqrtU64(slipQ32);
        baked.peakEfficiencyQ16 = (ONE_Q16 - rootQ16) * ONE_Q16 / (ONE_Q16 + rootQ16);
        return baked;
    }

    // =============================================================================================
    // 模型: 2 つの部屋の気体・熱の溜め・蓄え
    // =============================================================================================

    // 熱の溜めの熱容量 1000 J/K なので、エネルギー(mJ)÷ 1000 = 温度(mK)
    constexpr int64_t BATH_HEAT_CAPACITY = 1000000;   // mJ/K(構造と両方の部屋の気体の熱をまとめたもの)
    constexpr int64_t GAS_CONSTANT_NUMERATOR = 8314;  // R = 8314 ÷ 10^6 mJ/(K·µmol)
    constexpr double GAS_CONSTANT = 8.314e-3;         // 測る所だけ
    constexpr int64_t START_BATH = 300000000;         // 300 K
    constexpr int64_t START_AMOUNT = 1000000;         // 1 mol ずつ
    constexpr int64_t OVERSHOOT_DIVISOR = 64;  // 1 刻みに動かす気体は少ない側の部屋の 1/64 まで(行き過ぎの歯止め)

    struct PumpState {
        int64_t store = 0;       // 蓄え(mJ。仕事としてそのまま使える)
        int64_t bath = 0;        // 熱(mJ)
        int64_t amountLow = 0;   // 部屋 A(押す前の側。µmol)
        int64_t amountHigh = 0;  // 部屋 B(押し込む側。µmol)
    };

    struct PumpFlow {
        int64_t store = 0;   // J_X: 使った蓄え(mJ。+ = 蓄え → 熱。− = 押し返されて蓄えが増える)
        int64_t amount = 0;  // J_Y: A → B へ動いた気体(µmol)
    };

    // 気体の坂 f_Y = R T ln(n_A ÷ n_B)(mJ/µmol。Q32)。同じ体積・同じ温度なので圧力の比 = 物質量の比。B が多いと負(押すのは坂の上へ)
    int64_t GasSlopeQ32(int64_t amountLow, int64_t amountHigh, int64_t bath) {
        const int64_t lnRatioQ32 = fx::FxLnU64((uint64_t)amountLow) - fx::FxLnU64((uint64_t)amountHigh);
        const int64_t temperatureMk = bath / 1000;
        return MulDivToZero(lnRatioQ32, temperatureMk * GAS_CONSTANT_NUMERATOR, 1000000000);
    }

    // 押す構造の流れ(一つの芯。前の世代の値だけから。Jacobi 型)。坂はエネルギーの単位にそろえた値(Q32):
    //   storeSlope f_X = 蓄え 1 単位が坂を下る時の自由エネルギー(仕事の蓄えなら 1、熱の流れなら 1 − T_低 ÷ T_高)
    //   pushedSlope f_Y = Y を 1 単位動かす時の自由エネルギーの下がり(負 = 坂の上へ)。y = f_Y ÷ w
    //   J_X = P (f_X + q y)(+∞ 方向に丸める)、J_Y = (P ÷ w)(q f_X + y)(0 方向に切り捨て)
    //   = 係数 L_XX = P・L_YY = P ÷ w²・L_XY = q √(L_XX L_YY) の交差(ADR-0220 と同じ形)
    // 蓄えが足りない時(available 未満)は、蓄えの流れを残り s₀ に固定して Y の流れを同じ行列で解き直す(シューアの補):
    //   J_Y = q s₀ ÷ w + P (1 − q²) y ÷ w。q = 1 なら蓄えが尽きると止まって押し返されない。q < 1 なら滑った分だけ押し返される
    PumpFlow PushStructureFlow(const BakedMachine& machine, int64_t storeSlopeQ32, int64_t pushedSlopeQ32,
                               int64_t available) {
        const int64_t yQ32 = MulDivToZero(pushedSlopeQ32, ONE_Q16, (uint64_t)machine.costQ16);
        const int64_t couplingQ32 = machine.couplingQ16 * ONE_Q16;
        const int64_t qyQ32 = MulDivToZero(machine.couplingQ16, yQ32, (uint64_t)ONE_Q16);
        const int64_t qfQ32 = MulDivToZero(machine.couplingQ16, storeSlopeQ32, (uint64_t)ONE_Q16);
        const uint64_t costDivisor = (uint64_t)machine.costQ16 * (uint64_t)ONE_Q16;

        PumpFlow flow;
        flow.store = MulDivUp(machine.speed, storeSlopeQ32 + qyQ32, (uint64_t)ONE_Q32);
        flow.amount = MulDivToZero(machine.speed, qfQ32 + yQ32, costDivisor);
        if (flow.store <= available)
            return flow;

        // --- 蓄えが足りない: 残りを使い切る形で解き直す ---
        const int64_t slipQ32 = ONE_Q32 - machine.couplingQ16 * machine.couplingQ16;
        flow.store = available;
        flow.amount = MulDivToZero(available, couplingQ32, costDivisor) +
                      MulDivToZero(machine.speed, MulDivToZero(slipQ32, yQ32, (uint64_t)ONE_Q32), costDivisor);
        return flow;
    }

    // ポンプ: 蓄えは仕事(f_X = 1)、押すのは気体(熱の溜めの温度で)
    PumpFlow MachineFlow(const BakedMachine& machine, const PumpState& state) {
        return PushStructureFlow(machine, ONE_Q32, GasSlopeQ32(state.amountLow, state.amountHigh, state.bath),
                                 state.store);
    }

    // 1 刻みの行き過ぎの歯止め: 気体の流れが少ない側の部屋の 1/64 を超えたら、両方の流れを同じ割合で縮める
    // (流れの組を λ 倍しても生成は λ 倍なので ≥ 0 のまま。蓄えは +∞ 方向の丸め)
    PumpFlow LimitOvershoot(PumpFlow flow, const PumpState& state) {
        const int64_t limit = std::min(state.amountLow, state.amountHigh) / OVERSHOOT_DIVISOR;
        const int64_t magnitude = (int64_t)fx::FxAbsU64(flow.amount);
        if (magnitude <= limit)
            return flow;

        flow.store = MulDivUp(flow.store, limit, (uint64_t)magnitude);
        flow.amount = flow.amount < 0 ? -limit : limit;
        return flow;
    }

    // 全エントロピー(mJ/K。S = C ln T − R Σ n (ln n − 1)。定数は略す。蓄えは仕事なので 0)
    double TotalEntropy(const PumpState& state) {
        const double temperature = (double)state.bath / (double)BATH_HEAT_CAPACITY;
        double entropy = (double)BATH_HEAT_CAPACITY * std::log(temperature);
        for (const int64_t amount : {state.amountLow, state.amountHigh})
            entropy -= GAS_CONSTANT * (double)amount * (std::log((double)amount) - 1.0);

        return entropy;
    }

    struct RunReport {
        double worstSlack = 0;             // min(1 刻みの ΔS + 丸めの幅)。負なら丸めの幅を超えて坂を上った
        double minimumProduction = 1e300;  // 1 刻みの ΔS の最小(mJ/K)
        double totalProduction = 0;        // ΔS の合計(mJ/K)
        double peakEfficiency = 0;         // 1 刻みの効率 −J_Y f_Y ÷ J_X の最大(蓄えを使った刻みだけ)
        double gasWork = 0;                // Σ −J_Y f_Y(mJ。気体が得た自由エネルギー。線形の坂で)
        int64_t largestPush = 0;           // n_B − n_A の最大(µmol)
        int64_t storeMinimum = 0;
        bool conservedEveryTick = true;
        uint64_t hash = 0;
        PumpState last;
    };

    RunReport RunPump(const BakedMachine& machine, PumpState state, uint32_t ticks) {
        const int64_t energy = state.store + state.bath;
        const int64_t amount = state.amountLow + state.amountHigh;

        RunReport report;
        report.storeMinimum = state.store;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            const PumpFlow flow = LimitOvershoot(MachineFlow(machine, state), state);
            const double slope = (double)GasSlopeQ32(state.amountLow, state.amountHigh, state.bath) /
                                 (double)ONE_Q32;  // mJ/µmol
            const double temperature = (double)state.bath / (double)BATH_HEAT_CAPACITY;
            const double before = TotalEntropy(state);

            // --- 当てる(片方から引いた値をそのまま他方へ。保存はビット単位)---
            state.store -= flow.store;
            state.bath += flow.store;
            state.amountLow -= flow.amount;
            state.amountHigh += flow.amount;

            const double production = TotalEntropy(state) - before;
            const double roundingBound = 2.0 * (1.0 + std::fabs(slope)) / temperature;
            report.worstSlack = std::min(report.worstSlack, production + roundingBound);
            report.minimumProduction = std::min(report.minimumProduction, production);
            report.totalProduction += production;
            const double work = -(double)flow.amount * slope;
            report.gasWork += work;
            if (flow.store > 0 && work > 0)
                report.peakEfficiency = std::max(report.peakEfficiency, work / (double)flow.store);

            report.largestPush = std::max(report.largestPush, state.amountHigh - state.amountLow);
            report.storeMinimum = std::min(report.storeMinimum, state.store);
            if (state.store + state.bath != energy || state.amountLow + state.amountHigh != amount)
                report.conservedEveryTick = false;

            report.hash = fx::FxHashCombine(report.hash, (uint64_t)state.store);
            report.hash = fx::FxHashCombine(report.hash, (uint64_t)state.amountHigh);
        }

        report.last = state;
        return report;
    }

    // 試験の構造: 1 µmol を押すのに 3 mJ(300 K で q = 1 なら圧力の比 e^(3 ÷ 2.494) ≈ 3.3 で止まる)・負荷なしで 50 J/刻み
    MachineEntry PumpEntry(double coupling) {
        return {.couplingQ16 = (int64_t)std::llround(coupling * 65536.0), .costQ16 = 3 * ONE_Q16, .speed = 50000};
    }

    PumpState EqualRooms(int64_t store) {
        return {.store = store, .bath = START_BATH, .amountLow = START_AMOUNT, .amountHigh = START_AMOUNT};
    }

    double PeakEfficiency(double coupling) {
        const double root = std::sqrt(1.0 - coupling * coupling);
        return (1.0 - root) / (1.0 + root);
    }

    // --- |q| ≤ 1: 坂の上へ押し、止まる所まで行く。保存はビット単位・毎刻み生成 ≥ 0・効率の最大は η_max(q) に届いて超えない ---
    void TestPumpWithinBound(double coupling) {
        const BakedMachine machine = BakeMachine(PumpEntry(coupling));
        EXPECT(machine.warning.empty());

        const PumpState start = EqualRooms(1000000000);
        const RunReport run = RunPump(machine, start, 4000);
        const RunReport again = RunPump(machine, start, 4000);
        const double bound = PeakEfficiency(coupling);
        const double ratio = (double)run.last.amountHigh / (double)run.last.amountLow;
        const double temperature = (double)run.last.bath / (double)BATH_HEAT_CAPACITY;
        const double stallRatio = std::exp(coupling * 3.0 / (GAS_CONSTANT * temperature));
        std::printf(
            "pump q = %.2f: pressure ratio %.4f (stall %.4f), store spent %lld mJ, gas gained %.0f mJ, "
            "peak efficiency %.5f (bound %.5f, baked %.5f), min production %.3e mJ/K/tick, total %.3e mJ/K\n",
            coupling, ratio, stallRatio, (long long)(start.store - run.last.store), run.gasWork, run.peakEfficiency,
            bound, (double)machine.peakEfficiencyQ16 / 65536.0, run.minimumProduction, run.totalProduction);

        EXPECT(run.conservedEveryTick);
        EXPECT(run.hash == again.hash);
        EXPECT(run.worstSlack >= 0);                                 // 坂を上るのは丸めの幅の中だけ
        EXPECT(run.peakEfficiency <= bound + 1e-3);                  // 効率の上限は q だけで決まる
        EXPECT(run.peakEfficiency >= 0.97 * bound);                  // 押し上がる途中で上限の近くを通る
        EXPECT(std::fabs(ratio - stallRatio) <= 0.01 * stallRatio);  // 坂が費用に追いつく所で止まる
        EXPECT(std::fabs((double)machine.peakEfficiencyQ16 / 65536.0 - bound) <= 1e-4);
    }

    // --- |q| > 1(効率 > 1 になる値)はベイクが押さえる。押さえずに流すと、止まった所で熱から蓄えを作り続ける(永久機関)---
    void TestOverCouplingIsPerpetualMotion() {
        const MachineEntry written = PumpEntry(1.2);
        const BakedMachine baked = BakeMachine(written);
        EXPECT(!baked.warning.empty());
        EXPECT(baked.couplingQ16 == ONE_Q16);
        EXPECT(BakeMachine({.couplingQ16 = -98304, .costQ16 = ONE_Q16, .speed = 1}).couplingQ16 == -ONE_Q16);
        EXPECT(!BakeMachine({.couplingQ16 = ONE_Q16, .costQ16 = 0, .speed = 1}).warning.empty());

        BakedMachine unclamped = baked;
        unclamped.couplingQ16 = written.couplingQ16;  // ベイクを通さずに流す
        const PumpState start = EqualRooms(1000000000);
        const RunReport over = RunPump(unclamped, start, 4000);
        const RunReport fixed = RunPump(baked, start, 4000);
        std::printf(
            "over-coupled q = 1.20 unclamped: store %+lld mJ from its minimum (heat %+.1f K), total production %.3e "
            "mJ/K, "
            "min %.3e / clamped q = 1: store %+lld mJ, total %.3e mJ/K\n",
            (long long)(over.last.store - over.storeMinimum),
            (double)(over.last.bath - start.bath) / (double)BATH_HEAT_CAPACITY, over.totalProduction,
            over.minimumProduction, (long long)(fixed.last.store - fixed.storeMinimum), fixed.totalProduction);

        EXPECT(over.conservedEveryTick);                         // 保存は守っている(帳簿では見つからない)
        EXPECT(over.worstSlack < 0);                             // 坂を上る(丸めの幅を超えて生成が負)
        EXPECT(over.last.store > over.storeMinimum + 10000000);  // 熱から蓄え(仕事)を 10 kJ 以上作った
        EXPECT(fixed.worstSlack >= 0);
        EXPECT(fixed.last.store == fixed.storeMinimum);  // 押さえた後は蓄えが増えない
    }

    // --- 蓄えが尽きた後: q = 1 は押し上げた差を保つ(滑りなし)。q < 1 は滑りの分だけ押し返されて一様へ戻る ---
    void TestStoreRunsOut() {
        for (const double coupling : {1.0, 0.9}) {
            const BakedMachine machine = BakeMachine(PumpEntry(coupling));
            const RunReport run = RunPump(machine, EqualRooms(300000), 3000);
            const int64_t heldPush = run.last.amountHigh - run.last.amountLow;
            std::printf(
                "store runs out q = %.2f: largest push %lld umol, held at the end %lld umol, min production %.3e\n",
                coupling, (long long)run.largestPush, (long long)heldPush, run.minimumProduction);

            EXPECT(run.conservedEveryTick);
            EXPECT(run.worstSlack >= 0);
            EXPECT(run.last.store == 0);
            EXPECT(run.largestPush >= 50000);
            if (coupling == 1.0)
                EXPECT(heldPush == run.largestPush);
            else
                EXPECT(heldPush <= run.largestPush / 100);
        }
    }

    // --- 押し返される(坂が費用より急): 同じ形が逆に回り、気体が下る流れで蓄えを作る(発電機)。作った蓄え ≤ 気体が失った自由エネルギー ---
    void TestRunsBackwardsAsGenerator() {
        const BakedMachine machine = BakeMachine(PumpEntry(1.0));
        const PumpState start{.store = 0, .bath = START_BATH, .amountLow = 200000, .amountHigh = 1800000};
        const RunReport run = RunPump(machine, start, 4000);
        const double gasLost = -run.gasWork;  // 気体が失った自由エネルギー(mJ)
        const double ratio = (double)run.last.amountHigh / (double)run.last.amountLow;
        std::printf(
            "generator q = 1.00: store made %lld mJ from gas free energy %.0f mJ (%.4f), ratio 9 -> %.4f, "
            "min production %.3e\n",
            (long long)run.last.store, gasLost, (double)run.last.store / gasLost, ratio, run.minimumProduction);

        EXPECT(run.conservedEveryTick);
        EXPECT(run.worstSlack >= 0);
        EXPECT(run.last.store > 0);
        EXPECT((double)run.last.store <= gasLost);
        EXPECT(ratio < 3.5);
    }

    // --- 同じ形で X を熱の流れにする(熱機関): 熱い溜めから冷たい溜めへの熱の流れで気体を押す。
    //     f_X = 1 − T_冷 ÷ T_熱 なので、熱から得る効率は Carnot × η_max(q) を超えない(Carnot を形が自分で守る)---
    struct EngineState {
        int64_t hot = 0;   // 熱い溜め(mJ。熱容量は BATH_HEAT_CAPACITY)
        int64_t cold = 0;  // 冷たい溜め(気体の熱を含む。気体の坂はこの温度で)
        int64_t amountLow = 0;
        int64_t amountHigh = 0;
    };

    double EngineEntropy(const EngineState& state) {
        const double capacity = (double)BATH_HEAT_CAPACITY;
        double entropy = capacity * std::log((double)state.hot / capacity) +
                         capacity * std::log((double)state.cold / capacity);
        for (const int64_t amount : {state.amountLow, state.amountHigh})
            entropy -= GAS_CONSTANT * (double)amount * (std::log((double)amount) - 1.0);

        return entropy;
    }

    void TestHeatEngineKeepsCarnot(double coupling) {
        const BakedMachine machine = BakeMachine(
            {.couplingQ16 = (int64_t)std::llround(coupling * 65536.0), .costQ16 = 6 * ONE_Q16, .speed = 50000});
        EngineState state{
            .hot = 2 * START_BATH, .cold = START_BATH, .amountLow = START_AMOUNT, .amountHigh = START_AMOUNT};
        const int64_t energy = state.hot + state.cold;
        const int64_t amount = state.amountLow + state.amountHigh;
        bool conserved = true;
        double worstSlack = 0;
        double peakEfficiency = 0;  // 熱から気体の自由エネルギーへの効率
        double peakSecondLaw = 0;   // 効率 ÷ Carnot(η_max(q) を超えないはず)
        for (uint32_t tick = 0; tick < 4000; ++tick) {
            const int64_t carnotQ32 = MulDivToZero(state.hot - state.cold, ONE_Q32, (uint64_t)state.hot);
            const int64_t slopeQ32 = GasSlopeQ32(state.amountLow, state.amountHigh, state.cold);
            const PumpFlow flow = PushStructureFlow(machine, carnotQ32, slopeQ32, state.hot);
            const int64_t limit = std::min(state.amountLow, state.amountHigh) / OVERSHOOT_DIVISOR;
            EXPECT(fx::FxAbsU64(flow.amount) <= (uint64_t)limit);  // この値では歯止めに当たらない

            const double before = EngineEntropy(state);
            state.hot -= flow.store;
            state.cold += flow.store;
            state.amountLow -= flow.amount;
            state.amountHigh += flow.amount;

            const double slope = (double)slopeQ32 / (double)ONE_Q32;
            const double coldTemperature = (double)state.cold / (double)BATH_HEAT_CAPACITY;
            const double roundingBound = 2.0 * (1.0 + std::fabs(slope)) / coldTemperature;
            worstSlack = std::min(worstSlack, EngineEntropy(state) - before + roundingBound);
            const double work = -(double)flow.amount * slope;
            const double carnot = (double)carnotQ32 / (double)ONE_Q32;
            if (flow.store > 0 && work > 0) {
                peakEfficiency = std::max(peakEfficiency, work / (double)flow.store);
                peakSecondLaw = std::max(peakSecondLaw, work / (double)flow.store / carnot);
            }

            if (state.hot + state.cold != energy || state.amountLow + state.amountHigh != amount)
                conserved = false;
        }

        std::printf(
            "heat engine q = %.2f (600 K -> 300 K, Carnot 0.5): peak efficiency %.5f, peak / Carnot %.5f "
            "(bound %.5f), pressure ratio %.4f\n",
            coupling, peakEfficiency, peakSecondLaw, PeakEfficiency(coupling),
            (double)state.amountHigh / (double)state.amountLow);
        EXPECT(conserved);
        EXPECT(worstSlack >= 0);
        EXPECT(peakEfficiency <= 0.5 + 1e-3);                      // Carnot を超えない
        EXPECT(peakSecondLaw <= PeakEfficiency(coupling) + 1e-3);  // Carnot × η_max(q)
        EXPECT(peakSecondLaw >= 0.97 * PeakEfficiency(coupling));
    }

}  // namespace

int main() {
    TestPumpWithinBound(1.0);
    TestPumpWithinBound(0.9);
    TestPumpWithinBound(0.5);
    TestOverCouplingIsPerpetualMotion();
    TestStoreRunsOut();
    TestRunsBackwardsAsGenerator();
    TestHeatEngineKeepsCarnot(1.0);
    TestHeatEngineKeepsCarnot(0.8);

    if (failureCount != 0) {
        std::printf("push_model_test: %d failure(s)\n", failureCount);
        return 1;
    }

    std::printf("push_model_test: OK\n");
    return 0;
}
