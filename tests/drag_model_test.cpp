// drag_model_test.cpp — 引きずりの一般の形の模型(D-487 ④・T-0493。docs/design/19 §5 の段 3・§9)。CPU だけの研究の模型。
//
// 坂の原理の P3「流れは別のものを引きずる」(19 §2.1)の 2 つの形を確かめる:
//   模型 1「交差の引きずり」(散逸の流れどうしの結合。Onsager の交差の係数): 1 次元の棒で、熱の流れ(温度の坂)と成分の流れ(濃さの坂)を
//     作り手が書く**結合の強さ q**(−1〜1。Kedem–Caplan の結合度)でつなぐ。エンジンが交差の係数を L_xy = q √(L_xx L_yy) で作るので、
//     |q| ≤ 1 なら結合の行列は半正定で、どの面でも坂を上らない(エントロピーの生成 ≥ 0)。ベイクは |q| > 1 を警告して ±1 に押さえる
//     (BakeDragCoupling)。押さえずに q = 1.5 を流すと、ほぼ一様な棒から温度と濃さの差がひとりでに育つ(第二種の永久機関)ことを見せる。
//   模型 2「運ぶ引きずり」(流れる物が持つ量を一緒に運ぶ。GENERIC の可逆の部分): 気体の箱(sim::gas_reference。変えない)の中に
//     燃える木箱(反応 + 伝導。この試験の中の小さな模型)を置き、煙(熱い生成物)が熱を上へ運ぶことを、気体を流さない(伝導だけの)場合と
//     熱の重心で比べる。元素・エネルギー・運動量が毎刻み「初め + 帳簿」とビット単位で一致し、2 回の実行でハッシュが一致することも確かめる。
// 模型の状態を進める所は整数だけで書く(エンジンに入れる時の形の下書き)。倍精度は測る所(エントロピー・重心)だけ。
// 世界への組み込み(反応表の物質・伝導のキャッシュ・多重解像度の木)は T-0186・T-0210。結果は docs/tickets/T-0493-drag-general-form.md。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/fixed.hlsli"
#include "sim/gas_reference.h"
#include "sim/world_physics.h"
#include "standard_world_physics.h"

namespace {

    using namespace bicameral::sim;
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

    // =============================================================================================
    // 模型 1 交差の引きずり(1 次元の棒)
    // =============================================================================================

    // --- 引きずりの表の 1 行(作り手が書くもの。Luau の案は T-0493 のチケット)と、ベイク ---
    constexpr int64_t COUPLING_ONE = 65536;  // 結合の強さ q の 1(Q16)

    struct DragEntry {
        int64_t couplingQ16 = 0;  // q(Q16)。+ = 熱が流れる向きへ成分も引きずる
    };

    struct BakedDrag {
        int64_t couplingQ16 = 0;  // |q| ≤ 1 に押さえた値
        std::string warning;      // 押さえた時の警告(空 = なし)
    };

    // ベイクの検査: |q| > 1 は結合の行列が半正定でなくなる(ある坂の組で坂を上る = 永久機関)。警告して ±1 に押さえる(D-487 ②「警告して動かす」)
    BakedDrag BakeDragCoupling(const DragEntry& entry) {
        BakedDrag baked{.couplingQ16 = entry.couplingQ16};
        if (fx::FxAbsU64(entry.couplingQ16) <= (uint64_t)COUPLING_ONE)
            return baked;

        baked.couplingQ16 = entry.couplingQ16 < 0 ? -COUPLING_ONE : COUPLING_ONE;
        baked.warning =
            "coupling |q| > 1 makes a perpetual motion machine (Onsager matrix not positive semi-definite); clamped to "
            "1";
        return baked;
    }

    // --- 棒の値(試験の値)---
    // セルの熱容量 1000 mJ/K なので、エネルギー(mJ)の数がそのまま温度(mK)。成分は希薄な溶けた物(理想の混ざり)
    constexpr uint32_t BAR_CELLS = 8;
    constexpr int64_t BAR_HEAT_CAPACITY = 1000;        // mJ/K
    constexpr int64_t BAR_CONDUCTANCE = 100;           // G: 面の熱の流れ(mJ/刻み)= G × ΔT(K)
    constexpr int64_t BAR_DIFFUSION_DENOMINATOR = 10;  // D = 1/10: 面の成分の流れ(µmol/刻み)= Δn ÷ 10
    constexpr int64_t GAS_CONSTANT_NUMERATOR = 8314;   // R = 8314 ÷ 10^6 mJ/(K·µmol)
    constexpr int64_t GAS_CONSTANT_DENOMINATOR = 1000000;
    constexpr double GAS_CONSTANT = 8.314e-3;  // 測る所だけ

    struct BarCell {
        int64_t energy = 0;  // mJ(= 温度 mK)
        int64_t amount = 0;  // µmol
    };

    struct BarFlow {
        int64_t heat = 0;    // mJ(左 → 右を正)
        int64_t amount = 0;  // µmol(左 → 右を正)
    };

    // 交差の係数の大きさ K = |q| √(G D R n̄)(mJ/K の Q16)。√ の切り捨てと |q| の切り捨てで K² ≤ q² G D R n̄(半正定のまま)
    int64_t CrossCoefficientQ16(int64_t couplingQ16, int64_t meanAmount) {
        // G D R n̄ × 2^32 = 100 × 8314 × n̄ × 2^32 ÷ (10 × 10^6)
        const uint64_t scaled = fx::FxDivU128By64(
                                    fx::FxMulU64Full(
                                        (uint64_t)(BAR_CONDUCTANCE * GAS_CONSTANT_NUMERATOR) * (uint64_t)meanAmount,
                                        (uint64_t)1 << 32),
                                    (uint64_t)(BAR_DIFFUSION_DENOMINATOR * GAS_CONSTANT_DENOMINATOR))
                                    .quotient;
        const int64_t root = fx::FxSqrtU64(scaled);  // √(G D R n̄) の Q16
        return fx::FxMulShiftS64(root, couplingQ16, 16);
    }

    // 1 つの面の流れ(前の世代の値だけから。Jacobi 型)。線形の坂: X_q = ΔT ÷ T̄²、X_n = R Δn ÷ n̄
    //   J_q = G ΔT + K T̄ Δn ÷ n̄、J_n = D Δn + K (ΔT ÷ T̄) ÷ R(Onsager の相反: 交差の係数は両方向で同じ L_xy = K T̄)
    BarFlow FaceFlow(const BarCell& left, const BarCell& right, int64_t couplingQ16) {
        const int64_t deltaT = left.energy - right.energy;  // mK
        const int64_t deltaN = left.amount - right.amount;
        const int64_t meanT = (left.energy + right.energy) / 2;  // mK
        const int64_t meanN = (left.amount + right.amount) / 2;

        BarFlow flow;
        flow.heat = deltaT * BAR_CONDUCTANCE / 1000;
        flow.amount = deltaN / BAR_DIFFUSION_DENOMINATOR;
        if (couplingQ16 == 0 || meanN <= 0)
            return flow;

        const int64_t crossQ16 = CrossCoefficientQ16(couplingQ16, meanN);
        // K T̄ Δn ÷ n̄(mJ): K(Q16)× T̄(mK)× Δn ÷ (n̄ × 1000 × 2^16)
        flow.heat += MulDivToZero(crossQ16 * meanT / 1000, deltaN, (uint64_t)meanN * 65536);
        // K ΔT ÷ (T̄ R)(µmol): K(Q16)× 10^6 × ΔT ÷ (T̄ × 8314 × 2^16)
        flow.amount += MulDivToZero(crossQ16 * GAS_CONSTANT_DENOMINATOR, deltaT,
                                    (uint64_t)meanT * GAS_CONSTANT_NUMERATOR * 65536);
        return flow;
    }

    struct BarStepReport {
        double production = 0;     // Σ 面 (J_q X_q + J_n X_n)(mJ/K。線形の坂で数えた 1 刻みのエントロピーの生成)
        double roundingBound = 0;  // 整数の丸め(面ごとに J_q・J_n それぞれ 2 単位まで)が動かしうる幅
    };

    BarStepReport StepBar(std::vector<BarCell>& cells, int64_t couplingQ16) {
        std::vector<BarFlow> flows(cells.size() - 1);
        for (uint32_t face = 0; face + 1 < cells.size(); ++face)
            flows[face] = FaceFlow(cells[face], cells[face + 1], couplingQ16);

        BarStepReport report;
        for (uint32_t face = 0; face + 1 < cells.size(); ++face) {
            const BarCell& left = cells[face];
            const BarCell& right = cells[face + 1];
            const double meanT = (double)(left.energy + right.energy) / 2000.0;  // K
            const double meanN = (double)(left.amount + right.amount) / 2.0;
            const double forceHeat = ((double)(left.energy - right.energy) / 1000.0) / (meanT * meanT);
            const double forceAmount = GAS_CONSTANT * (double)(left.amount - right.amount) / meanN;
            report.production += ((double)flows[face].heat * forceHeat) + ((double)flows[face].amount * forceAmount);
            report.roundingBound += 2.0 * (std::fabs(forceHeat) + std::fabs(forceAmount));
        }

        // --- 当てる(片方から引いた値をそのまま他方へ。保存はビット単位)---
        for (uint32_t face = 0; face + 1 < cells.size(); ++face) {
            cells[face].energy -= flows[face].heat;
            cells[face + 1].energy += flows[face].heat;
            cells[face].amount -= flows[face].amount;
            cells[face + 1].amount += flows[face].amount;
        }

        return report;
    }

    // 棒の全エントロピー(mJ/K。S = Σ C ln T − R n (ln n − 1)。定数は略す)
    double BarEntropy(const std::vector<BarCell>& cells) {
        double entropy = 0;
        for (const BarCell& cell : cells) {
            entropy += (double)BAR_HEAT_CAPACITY * std::log((double)cell.energy / 1000.0);
            const double amount = (double)cell.amount;
            entropy -= GAS_CONSTANT * amount * (std::log(amount) - 1.0);
        }

        return entropy;
    }

    struct BarTotals {
        int64_t energy = 0;
        int64_t amount = 0;
    };

    BarTotals SumBar(const std::vector<BarCell>& cells) {
        BarTotals totals;
        for (const BarCell& cell : cells) {
            totals.energy += cell.energy;
            totals.amount += cell.amount;
        }

        return totals;
    }

    constexpr int64_t BAR_TEMPERATURE = 300000;  // mK
    constexpr int64_t BAR_AMOUNT = 1000000;      // µmol

    // 左半分を +100 K にした棒(成分は一様)
    std::vector<BarCell> MakeHotLeftBar() {
        std::vector<BarCell> cells(BAR_CELLS, BarCell{.energy = BAR_TEMPERATURE, .amount = BAR_AMOUNT});
        for (uint32_t i = 0; i < BAR_CELLS / 2; ++i)
            cells[i].energy += 100000;

        return cells;
    }

    // --- |q| ≤ 1 なら毎刻み坂を下り(丸めの幅の中)、熱の流れが成分を引きずる ---
    void TestCrossDragWithinBound(double coupling) {
        const BakedDrag baked = BakeDragCoupling({.couplingQ16 = (int64_t)std::llround(coupling * 65536.0)});
        EXPECT(baked.warning.empty());

        std::vector<BarCell> cells = MakeHotLeftBar();
        const BarTotals before = SumBar(cells);
        double worst = 0;  // 生成 + 丸めの幅 の最小(負なら丸めの幅を超えて坂を上った)
        double minimumProduction = 1e300;
        int64_t largestShift = 0;  // 成分が一様からずれた最大(µmol)
        for (uint32_t tick = 0; tick < 6000; ++tick) {
            const BarStepReport report = StepBar(cells, baked.couplingQ16);
            worst = std::min(worst, report.production + report.roundingBound);
            minimumProduction = std::min(minimumProduction, report.production);
            for (const BarCell& cell : cells)
                largestShift = std::max<int64_t>(largestShift, (int64_t)fx::FxAbsU64(cell.amount - BAR_AMOUNT));

            const BarTotals now = SumBar(cells);
            EXPECT(now.energy == before.energy && now.amount == before.amount);
        }

        int64_t spreadT = 0;
        int64_t spreadN = 0;
        for (const BarCell& cell : cells) {
            spreadT = std::max<int64_t>(spreadT, (int64_t)fx::FxAbsU64(cell.energy - cells[0].energy));
            spreadN = std::max<int64_t>(spreadN, (int64_t)fx::FxAbsU64(cell.amount - cells[0].amount));
        }

        std::printf(
            "cross drag q = %+.2f: largest amount shift %lld umol (of %lld), min production %.3e mJ/K/tick, "
            "final spread %lld mK / %lld umol\n",
            coupling, (long long)largestShift, (long long)BAR_AMOUNT, minimumProduction, (long long)spreadT,
            (long long)spreadN);
        EXPECT(worst >= 0);  // 坂を上るのは丸めの幅の中だけ
        if (std::fabs(coupling) < 1.0)
            EXPECT(spreadT <= 1000 &&
                   spreadN <=
                       1000);  // 平衡(一様)へ下りる(初めの差 100 K の 1% 以下)。|q| = 1 は結合しきった組が動かない
        if (coupling == 0)
            EXPECT(largestShift == 0);  // 結合なしなら成分は動かない
        else
            EXPECT(largestShift >= BAR_AMOUNT / 100);  // 熱の流れが成分を 1% 以上引きずる
    }

    // --- 書きすぎた結合(|q| > 1)はベイクが押さえる。押さえずに流すと、ほぼ一様な棒から差がひとりでに育つ(永久機関)---
    void TestOverCouplingIsPerpetualMotion() {
        const BakedDrag baked = BakeDragCoupling({.couplingQ16 = 98304});  // q = 1.5
        EXPECT(!baked.warning.empty());
        EXPECT(baked.couplingQ16 == COUPLING_ONE);
        EXPECT(BakeDragCoupling({.couplingQ16 = -98304}).couplingQ16 == -COUPLING_ONE);

        for (const int64_t couplingQ16 : {(int64_t)98304, baked.couplingQ16}) {
            // ほぼ一様: セルごとに ±1 K・∓1000 µmol の小さな揺れ(決まった模様。温度と濃さを逆向きに = q > 1 で育つ組)
            std::vector<BarCell> cells(BAR_CELLS);
            for (uint32_t i = 0; i < BAR_CELLS; ++i) {
                const int64_t sign = ((i * 7) % 3) == 0 ? 1 : -1;
                cells[i] = {.energy = BAR_TEMPERATURE + (sign * 1000), .amount = BAR_AMOUNT - (sign * 1000)};
            }

            const double startEntropy = BarEntropy(cells);
            int64_t spread = 0;
            uint32_t tick = 0;
            for (; tick < 400; ++tick) {
                StepBar(cells, couplingQ16);
                spread = 0;
                for (const BarCell& cell : cells)
                    spread = std::max<int64_t>(spread, (int64_t)fx::FxAbsU64(cell.energy - BAR_TEMPERATURE));

                if (spread > 50000)
                    break;  // 50 K の差が育った
            }

            const double entropyChange = BarEntropy(cells) - startEntropy;
            std::printf(
                "over-coupling q = %.2f: temperature spread %lld mK after %u ticks (start 1000), entropy change %.3e "
                "mJ/K\n",
                (double)couplingQ16 / 65536.0, (long long)spread, tick, entropyChange);
            if (couplingQ16 > COUPLING_ONE) {
                EXPECT(spread > 50000);     // 仕事なしで温度差が育つ
                EXPECT(entropyChange < 0);  // エントロピーが減る(第二法則を破る)
            } else {
                EXPECT(spread <= 2000);          // 押さえた値なら揺れは育たない(q = 1 は結合しきった組がそのまま残る)
                EXPECT(entropyChange >= -1e-3);  // 倍精度の和の誤差の分だけ
            }
        }
    }

    // =============================================================================================
    // 模型 2 運ぶ引きずり(気体の箱の中の燃える木箱)
    // =============================================================================================

    constexpr uint32_t GAS_N2 = 0;
    constexpr uint32_t GAS_O2 = 1;
    constexpr uint32_t GAS_CO2 = 2;
    constexpr uint32_t GAS_H2O = 3;
    constexpr std::array<uint32_t, 4> AIR_FRACTIONS_Q32 = {3393024163u, 901943132u, 0u, 0u};  // N2 79%・O2 21%
    constexpr uint32_t AIR_TEMPERATURE = 293150;                                              // mK
    constexpr uint64_t SURFACE_PRESSURE = 101325ull * 1000000ull;                             // µPa

    // 木(セルロースの単位 C6H10O5)。C6H10O5 + 6 O2 → 6 CO2 + 5 H2O(反応熱 約 2607 kJ/mol。生成エンタルピー 約 −963 kJ/mol は
    // 文献の値の概数〔未確認の幅 ±5%〕。試験の模型なので燃え方の速さと量は見やすい値)
    constexpr int64_t WOOD_FORMATION_ENERGY = -963;       // mJ/µmol(= kJ/mol)
    constexpr int64_t WOOD_HEAT_CAPACITY_PER_MOL = 217;   // J/(mol·K)(約 1.34 J/(g·K))
    constexpr int64_t WOOD_FRAME_HEAT_CAPACITY = 100000;  // mJ/K(燃えない骨組み。燃え尽きても熱容量が 0 にならない)
    constexpr int64_t WOOD_AMOUNT = 2000000;              // µmol / セル
    constexpr uint32_t WOOD_START_TEMPERATURE = 800000;   // mK(火がついている)
    constexpr uint32_t WOOD_IGNITION = 600000;            // mK(これより熱いと燃える)
    constexpr uint32_t BURN_SHIFT = 10;                   // 1 刻みにセルの O2 の 1/1024 を使う(O2 で決まる燃え方)

    // 伝導の係数(mJ/(K·刻み)。試験の値。気体どうしは現実の空気の約 5000 倍 = combustion_test の倍率と同じ考え。07 §1.1)
    constexpr int64_t CONDUCT_GAS_GAS = 1000;
    constexpr int64_t CONDUCT_WOOD_GAS = 5000;
    constexpr int64_t CONDUCT_WOOD_WOOD = 10000;

    struct WoodCell {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t z = 0;
        uint32_t gasIndex = 0;  // 同じ場所の気体のセル(木は多孔質で、セルの中に気体もある)
        int64_t amount = 0;     // µmol
        int64_t energy = 0;     // mJ(熱 + 化学)
    };

    struct FireModel {
        GasBox gas;
        std::vector<WoodCell> wood;
        bool flow = true;    // false = 気体を流さない(伝導だけ)
        int64_t burned = 0;  // 燃えた木(µmol)
    };

    int64_t WoodHeatCapacity(const WoodCell& cell) {
        return (cell.amount * WOOD_HEAT_CAPACITY_PER_MOL / 1000) + WOOD_FRAME_HEAT_CAPACITY;  // mJ/K
    }

    int64_t WoodTemperature(const WoodCell& cell) {
        const int64_t heat = cell.energy - (cell.amount * WOOD_FORMATION_ENERGY);  // mJ
        return heat * 1000 / WoodHeatCapacity(cell);                               // mK
    }

    GasConfig MakeFireConfig() {
        GasConfig config;
        config.sizeX = 8;
        config.sizeY = 8;
        config.sizeZ = 16;
        config.speciesCount = 4;
        // 比熱は定積の c_v(定圧 − R。T-0234)。H0 は J/mol
        config.species[GAS_N2] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};
        config.species[GAS_O2] = {.molarMass = 31999, .heatCapacity = 21064, .formationEnergy = 0};
        config.species[GAS_CO2] = {.molarMass = 44009, .heatCapacity = 28821, .formationEnergy = -393509};
        config.species[GAS_H2O] = {.molarMass = 18015, .heatCapacity = 25270, .formationEnergy = -241826};
        ApplyWorldPhysics(StandardWorldPhysics(), config);
        return config;
    }

    FireModel MakeFireModel(bool flow) {
        const GasConfig config = MakeFireConfig();
        FireModel model{.gas = MakeGasBox(config, MakeHydrostaticReference(config, AIR_FRACTIONS_Q32, AIR_TEMPERATURE,
                                                                           SURFACE_PRESSURE)),
                        .flow = flow};
        for (uint32_t y = 3; y <= 4; ++y) {
            for (uint32_t x = 3; x <= 4; ++x) {
                WoodCell cell{.x = x, .y = y, .z = 1, .gasIndex = model.gas.Index(x, y, 1), .amount = WOOD_AMOUNT};
                cell.energy = (WoodHeatCapacity(cell) * WOOD_START_TEMPERATURE / 1000) +
                              (cell.amount * WOOD_FORMATION_ENERGY);
                model.wood.push_back(cell);
            }
        }

        return model;
    }

    // --- 反応(木のセルごと。O2 は同じ場所の気体から取り、生成物と木の化学のエネルギーを気体へ渡す)---
    void BurnWood(FireModel& model) {
        for (WoodCell& cell : model.wood) {
            if (cell.amount <= 0 || WoodTemperature(cell) < WOOD_IGNITION)
                continue;

            GasCell& gas = model.gas.cells[cell.gasIndex];
            const int64_t progress = std::min<int64_t>(cell.amount, (int64_t)((gas.amounts[GAS_O2] >> BURN_SHIFT) / 6));
            if (progress <= 0)
                continue;

            cell.amount -= progress;
            gas.amounts[GAS_O2] -= (uint64_t)(6 * progress);
            gas.amounts[GAS_CO2] += (uint64_t)(6 * progress);
            gas.amounts[GAS_H2O] += (uint64_t)(5 * progress);

            // 燃えた木の化学のエネルギーを気体へ(木の熱はそのまま。気体の熱 = 全エネルギー − 化学 − 運動 が反応熱の分だけ上がる)
            const int64_t transfer = progress * WOOD_FORMATION_ENERGY;
            cell.energy -= transfer;
            gas.energy += transfer;
            model.burned += progress;
        }
    }

    // --- 伝導(木 ⇄ 気体・木 ⇄ 木・気体 ⇄ 気体。前の世代の温度から全部の面を数えてから当てる)---
    void ConductHeat(FireModel& model) {
        GasBox& gas = model.gas;
        const GasConfig& config = gas.config;
        std::vector<int64_t> gasTemperature(gas.cells.size());
        for (uint32_t index = 0; index < gas.cells.size(); ++index)
            gasTemperature[index] = DeriveGasCell(gas, gas.cells[index], index / (config.sizeX * config.sizeY))
                                        .temperature;

        std::vector<int64_t> gasDelta(gas.cells.size(), 0);
        std::vector<int64_t> woodDelta(model.wood.size(), 0);

        // 気体 ⇄ 気体(各セルの +x・+y・+z の面。壁の外へは流さない)
        const std::array<uint32_t, 3> sizes = {config.sizeX, config.sizeY, config.sizeZ};
        for (uint32_t z = 0; z < config.sizeZ; ++z) {
            for (uint32_t y = 0; y < config.sizeY; ++y) {
                for (uint32_t x = 0; x < config.sizeX; ++x) {
                    const std::array<uint32_t, 3> at = {x, y, z};
                    const uint32_t from = gas.Index(x, y, z);
                    for (uint32_t axis = 0; axis < 3; ++axis) {
                        if (at[axis] + 1 >= sizes[axis])
                            continue;

                        std::array<uint32_t, 3> next = at;
                        ++next[axis];
                        const uint32_t to = gas.Index(next[0], next[1], next[2]);
                        const int64_t flow = CONDUCT_GAS_GAS * (gasTemperature[from] - gasTemperature[to]) / 1000;
                        gasDelta[from] -= flow;
                        gasDelta[to] += flow;
                    }
                }
            }
        }

        // 木 ⇄ 同じ場所の気体・木 ⇄ 隣の木
        for (uint32_t i = 0; i < model.wood.size(); ++i) {
            const WoodCell& cell = model.wood[i];
            const int64_t temperature = WoodTemperature(cell);
            const int64_t toGas = CONDUCT_WOOD_GAS * (temperature - gasTemperature[cell.gasIndex]) / 1000;
            woodDelta[i] -= toGas;
            gasDelta[cell.gasIndex] += toGas;
            for (uint32_t j = i + 1; j < model.wood.size(); ++j) {
                const WoodCell& other = model.wood[j];
                const uint32_t distance = (uint32_t)(std::abs((int32_t)cell.x - (int32_t)other.x) +
                                                     std::abs((int32_t)cell.y - (int32_t)other.y) +
                                                     std::abs((int32_t)cell.z - (int32_t)other.z));
                if (distance != 1)
                    continue;

                const int64_t flow = CONDUCT_WOOD_WOOD * (temperature - WoodTemperature(other)) / 1000;
                woodDelta[i] -= flow;
                woodDelta[j] += flow;
            }
        }

        // --- 当てる ---
        for (uint32_t index = 0; index < gas.cells.size(); ++index)
            gas.cells[index].energy += gasDelta[index];

        for (uint32_t i = 0; i < model.wood.size(); ++i)
            model.wood[i].energy += woodDelta[i];
    }

    void StepFire(FireModel& model) {
        BurnWood(model);
        ConductHeat(model);
        if (model.flow)
            StepGas(model.gas);
    }

    // --- 保存の検査: 元素(C・H・O・N)・エネルギー・運動量 ---
    struct FireTotals {
        std::array<int64_t, 4> elements = {};  // C・H・O・N(µmol の原子)
        int64_t energy = 0;                    // 気体 + 木(mJ)
        std::array<int64_t, GAS_AXES> momentum = {};
    };

    FireTotals SumFire(const FireModel& model) {
        const GasTotals gas = SumGas(model.gas);
        std::array<int64_t, 4> amounts = {};
        for (uint32_t s = 0; s < 4; ++s)
            amounts[s] = (int64_t)gas.amounts[s] - model.gas.ledger.amounts[s];  // 外から入った分を引く(閉じた箱では 0)

        FireTotals totals;
        totals.elements = {amounts[GAS_CO2], 2 * amounts[GAS_H2O],
                           (2 * amounts[GAS_O2]) + (2 * amounts[GAS_CO2]) + amounts[GAS_H2O], 2 * amounts[GAS_N2]};
        totals.energy = gas.energy - model.gas.ledger.energy;
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
            totals.momentum[axis] = gas.momentum[axis] - model.gas.ledger.momentum[axis];

        for (const WoodCell& cell : model.wood) {
            totals.elements[0] += 6 * cell.amount;
            totals.elements[1] += 10 * cell.amount;
            totals.elements[2] += 5 * cell.amount;
            totals.energy += cell.energy;
        }

        return totals;
    }

    bool SameTotals(const FireTotals& a, const FireTotals& b) {
        return a.elements == b.elements && a.energy == b.energy && a.momentum == b.momentum;
    }

    // 熱の重心の高さ(セル): 基準より 1 K 以上熱い分(K)× 気体の熱容量の目安(物質量)で重みを付けた z の平均。木の段は z = 1
    double HeatCentroidZ(const FireModel& model) {
        const GasBox& gas = model.gas;
        double weighted = 0;
        double total = 0;
        for (uint32_t index = 0; index < gas.cells.size(); ++index) {
            const uint32_t z = index / (gas.config.sizeX * gas.config.sizeY);
            const double excess = (double)DeriveGasCell(gas, gas.cells[index], z).temperature -
                                  (double)gas.referenceDerived[z].temperature;
            if (excess < 1000)
                continue;  // 1 K より小さい分は数えない(加熱で出る音の波の圧縮が箱の全体を mK だけ温め、重心をぼかす)

            double amount = 0;
            for (uint32_t s = 0; s < 4; ++s)
                amount += (double)gas.cells[index].amounts[s];

            weighted += excess * amount * z;
            total += excess * amount;
        }

        return total == 0 ? 0 : weighted / total;
    }

    // 基準より 5 K 以上熱いセルがある最も高い層
    uint32_t HottestReach(const FireModel& model) {
        const GasBox& gas = model.gas;
        uint32_t reach = 0;
        for (uint32_t index = 0; index < gas.cells.size(); ++index) {
            const uint32_t z = index / (gas.config.sizeX * gas.config.sizeY);
            if ((int64_t)DeriveGasCell(gas, gas.cells[index], z).temperature -
                    (int64_t)gas.referenceDerived[z].temperature >=
                5000)
                reach = std::max(reach, z);
        }

        return reach;
    }

    uint64_t HashFire(const FireModel& model) {
        uint64_t hash = HashGas(model.gas);
        for (const WoodCell& cell : model.wood) {
            hash = fx::FxHashCombine(hash, (uint64_t)cell.amount);
            hash = fx::FxHashCombine(hash, (uint64_t)cell.energy);
        }

        return hash;
    }

    struct FireResult {
        double centroidStart = 0;
        double centroidEnd = 0;
        uint32_t reach = 0;
        int64_t burned = 0;
        uint64_t hash = 0;
        bool conservedEveryTick = true;
    };

    constexpr uint32_t FIRE_TICKS = 120;  // 2 秒

    FireResult RunFire(bool flow) {
        FireModel model = MakeFireModel(flow);
        const FireTotals start = SumFire(model);
        FireResult result;
        for (uint32_t tick = 0; tick < FIRE_TICKS; ++tick) {
            StepFire(model);
            if (tick == 0)
                result.centroidStart = HeatCentroidZ(model);  // 最初の刻みの熱(木の段の上)から測る

            result.conservedEveryTick = result.conservedEveryTick && SameTotals(SumFire(model), start);
        }

        result.centroidEnd = HeatCentroidZ(model);
        result.reach = HottestReach(model);
        result.burned = model.burned;
        result.hash = HashFire(model);
        return result;
    }

    void TestSmokeCarriesHeatUp() {
        const FireResult conduction = RunFire(false);
        const FireResult smoke = RunFire(true);
        const FireResult again = RunFire(true);
        std::printf(
            "fire, conduction only: heat centroid z %.3f -> %.3f cells after 2 s, reach z %u, burned %lld umol\n",
            conduction.centroidStart, conduction.centroidEnd, conduction.reach, (long long)conduction.burned);
        std::printf(
            "fire, smoke flows:     heat centroid z %.3f -> %.3f cells after 2 s, reach z %u, burned %lld umol\n",
            smoke.centroidStart, smoke.centroidEnd, smoke.reach, (long long)smoke.burned);

        EXPECT(conduction.conservedEveryTick);
        EXPECT(smoke.conservedEveryTick);
        EXPECT(smoke.hash == again.hash);
        const double conductionRise = conduction.centroidEnd - conduction.centroidStart;
        const double smokeRise = smoke.centroidEnd - smoke.centroidStart;
        EXPECT(smokeRise >= conductionRise + 1.0);  // 煙が熱を 1 セル(0.5 m)以上高く運ぶ
        EXPECT(smokeRise >= 2.0 * std::max(conductionRise, 0.0));
        EXPECT(smoke.reach > conduction.reach);
    }

}  // namespace

int main() {
    TestCrossDragWithinBound(0.0);
    TestCrossDragWithinBound(0.5);
    TestCrossDragWithinBound(-0.9);
    TestCrossDragWithinBound(1.0);
    TestOverCouplingIsPerpetualMotion();
    TestSmokeCarriesHeatUp();

    if (failureCount != 0) {
        std::printf("drag_model_test: %d failure(s)\n", failureCount);
        return 1;
    }

    std::printf("drag_model_test: OK\n");
    return 0;
}
