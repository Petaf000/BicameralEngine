// gas_projection_test.cpp — 気体の圧力を刻みの中で解く投影の CPU 模型(sim/gas_projection。T-0503 研究・07 §2.6)を CPU だけで確かめ、測る。
// 合格の条件(研究の打ち切り条件。docs/tickets/T-0503-pressure-projection-model.md):
//   静止大気が 600 刻みで速度がちょうど 0・閉じた箱で成分がビット一致で一定、エネルギーと運動量は毎刻み「初め + 帳簿」とビット一致・
//   同じ入力なら同じハッシュ・熱い泡(gas_reference と同じ場面)の熱の重心が 2 秒で gas_reference(1.32 セル)の 1/3〜3 倍上がる。
// 測る値(表示だけ): 1 刻みの V サイクルの回数(音速 340 m/s・3400 m/s・最速)と、爆発(1 セル 5 倍の圧力・1500 K・煙の印)で
//   圧力の前線・熱い所・煙の広がりが刻みごとにどう見えるか(gas_reference の小刻み 24 と比べる。D-487 ⑥)。
// 外側の反復(T-0530): 最速で 5 倍の爆発が 1 刻み目の終わりに中心の近くの圧力の差 1 kPa 以下・外側の反復ありでも保存はビット単位・ハッシュ一致・
//   熱い泡は同じ桁。測る値: 外側の反復の回数 × V サイクル(弱い差〔泡・扉〕と強い爆発)・許容差と上限の違い。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "sim/gas_projection.h"
#include "sim/gas_reference.h"
#include "sim/world_physics.h"
#include "standard_world_physics.h"

namespace {

    using namespace bicameral::sim;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 試験の空気(gas_reference_test と同じ。N2 79% + O2 21% と煙の印)---
    constexpr uint32_t SPECIES_N2 = 0;
    constexpr uint32_t SPECIES_SMOKE = 2;
    constexpr std::array<uint32_t, 3> AIR_FRACTIONS_Q32 = {3393024163u, 901943132u, 0u};
    constexpr uint32_t AIR_TEMPERATURE = 293150;                   // mK
    constexpr uint64_t SURFACE_PRESSURE = 101325ull * 1000000ull;  // µPa
    constexpr int64_t METER_PER_SECOND = (int64_t)1 << 20;
    constexpr int64_t ROUGH_CELL_MASS = 150000;
    constexpr uint32_t REAL_SOUND = 340000;  // mm/s
    constexpr uint32_t SLOW_SOUND = 30000;   // ゲームが爆風を見せたい時の遅い速さの例(1 刻みに 1 セル)
    constexpr int64_t
        REFERENCE_BUBBLE_RISE = 1320;  // gas_reference の熱い泡の 2 秒の重心の上がり(1/1000 セル。07 §2.4)

    GasConfig MakeAirConfig(uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ) {
        GasConfig config;
        config.sizeX = sizeX;
        config.sizeY = sizeY;
        config.sizeZ = sizeZ;
        config.speciesCount = 3;
        config.species[0] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};  // N2
        config.species[1] = {.molarMass = 31999, .heatCapacity = 21064, .formationEnergy = 0};  // O2
        config.species[2] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};  // 煙の印
        ApplyWorldPhysics(StandardWorldPhysics(), config);

        return config;
    }

    GasBox MakeAirBox(const GasConfig& config) {
        return MakeGasBox(config,
                          MakeHydrostaticReference(config, AIR_FRACTIONS_Q32, AIR_TEMPERATURE, SURFACE_PRESSURE));
    }

    void Heat(GasBox& box, uint32_t index, uint32_t temperature) {
        const GasCell old = box.cells[index];
        GasCell cell = MakeGasCell(box.config, old.amounts, temperature);
        cell.momentum = old.momentum;
        box.cells[index] = cell;
    }

    void AddSmoke(GasBox& box, uint32_t index) {
        GasCell& cell = box.cells[index];
        const uint64_t moved = cell.amounts[SPECIES_N2] / 2;
        cell.amounts[SPECIES_N2] -= moved;
        cell.amounts[SPECIES_SMOKE] += moved;
    }

    std::array<uint32_t, 3> Coordinate(const GasBox& box, uint32_t index) {
        const uint32_t sizeX = box.config.sizeX;
        const uint32_t sizeY = box.config.sizeY;
        return {index % sizeX, (index / sizeX) % sizeY, index / (sizeX * sizeY)};
    }

    int64_t MaxSpeed(const GasBox& box) {
        int64_t speed = 0;
        for (const GasCell& cell : box.cells) {
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                speed = std::max(speed, std::abs(GasVelocity(box, cell, axis)));
        }

        return speed;
    }

    GasProjectionOptions MakeOptions(uint32_t soundSpeed, uint32_t substeps = 1) {
        GasProjectionOptions options;
        options.pressure.soundSpeedMmPerS = soundSpeed;
        options.pressure.substeps = substeps;
        options.outerIterations = 1;  // T-0503 の試験は線形の 1 回のまま(外側の反復は T-0530 の試験)
        return options;
    }

    const char* SpeedName(uint32_t soundSpeed) {
        if (soundSpeed == 0)
            return "fastest";

        return soundSpeed == REAL_SOUND ? "340 m/s" : (soundSpeed == REAL_SOUND * 10 ? "3400 m/s" : "30 m/s");
    }

    // 刻みごとの費用をまとめる
    struct CostSummary {
        uint64_t cycles = 0;
        uint32_t maxCycles = 0;
        uint32_t unconverged = 0;
        uint32_t levels = 0;
        int64_t worstError = 0;
        uint64_t ticks = 0;

        // --- 外側の反復(T-0530)---
        uint64_t outer = 0;
        uint32_t maxOuter = 0;
        uint32_t outerUnconverged = 0;
        int64_t firstMismatch = 0;
        int64_t worstMismatch = 0;

        void Add(const GasProjectionCost& cost) {
            cycles += cost.cycles;
            maxCycles = std::max(maxCycles, cost.maxCycles);
            unconverged += cost.unconverged;
            levels = cost.gridLevels;
            worstError = std::max(worstError, cost.worstMassError);
            ++ticks;

            outer += cost.outerIterations;
            maxOuter = std::max(maxOuter, cost.maxOuterIterations);
            outerUnconverged += cost.outerUnconverged;
            firstMismatch = std::max(firstMismatch, cost.firstMismatch);
            worstMismatch = std::max(worstMismatch, cost.worstMismatch);
        }

        void PrintOuter(const char* name) const {
            std::printf(
                "  outer %s: %.2f/tick (max %u, at limit %u), V cycles %.2f/tick (max %u), mismatch first %lld mg / "
                "kept %lld mg\n",
                name, ticks == 0 ? 0.0 : (double)outer / (double)ticks, maxOuter, outerUnconverged,
                ticks == 0 ? 0.0 : (double)cycles / (double)ticks, maxCycles, (long long)firstMismatch,
                (long long)worstMismatch);
        }

        void Print(const char* name) const {
            std::printf(
                "  cost %s: V cycles %.2f/tick (max %u, unconverged %u), grid levels %u, worst mass error %lld mg\n",
                name, ticks == 0 ? 0.0 : (double)cycles / (double)ticks, maxCycles, unconverged, levels,
                (long long)worstError);
        }
    };

    // --- 静止大気: 600 刻みで速度がちょうど 0 ---
    void TestStaticAtmosphere(uint32_t soundSpeed) {
        GasBox box = MakeAirBox(MakeAirConfig(4, 4, 16));
        const GasTotals before = SumGas(box);
        GasProjection state;
        CostSummary summary;
        for (int tick = 0; tick < 600; ++tick)
            summary.Add(StepGasProjection(box, state, MakeOptions(soundSpeed)));

        const int64_t speed = MaxSpeed(box);
        std::printf("static atmosphere (%s): max speed after 600 ticks = %lld (2^-20 m/s)\n", SpeedName(soundSpeed),
                    (long long)speed);
        summary.Print(SpeedName(soundSpeed));
        EXPECT(speed == 0);
        EXPECT(SumGas(box).energy == before.energy);
        EXPECT(box.ledger.energy == 0);
    }

    // --- 閉じた箱の保存(毎刻み)と決定性 ---
    GasBox MakeDisturbedClosedBox() {
        GasBox box = MakeAirBox(MakeAirConfig(8, 8, 8));
        Heat(box, box.Index(3, 3, 1), AIR_TEMPERATURE + 50000);
        Heat(box, box.Index(4, 3, 1), AIR_TEMPERATURE + 50000);
        Heat(box, box.Index(5, 4, 3), AIR_TEMPERATURE * 5);
        AddSmoke(box, box.Index(5, 5, 5));

        GasCell& moving = box.cells[box.Index(2, 6, 4)];
        SetGasMomentum(box, moving, {ROUGH_CELL_MASS * 2 * METER_PER_SECOND, 0, -ROUGH_CELL_MASS * METER_PER_SECOND});

        return box;
    }

    bool ConservedAgainst(const GasBox& box, const GasTotals& before) {
        const GasTotals now = SumGas(box);
        bool ok = now.amounts == before.amounts && now.energy == before.energy + box.ledger.energy;
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
            ok = ok && now.momentum[axis] == before.momentum[axis] + box.ledger.momentum[axis];

        return ok;
    }

    uint64_t RunDisturbed(uint32_t soundSpeed, uint32_t substeps, bool& conserved) {
        GasBox box = MakeDisturbedClosedBox();
        const GasTotals before = SumGas(box);
        GasProjection state;
        conserved = true;
        for (int tick = 0; tick < 60; ++tick) {
            StepGasProjection(box, state, MakeOptions(soundSpeed, substeps));
            conserved = conserved && ConservedAgainst(box, before);
        }

        return HashGas(box);
    }

    void TestConservationAndDeterminism(uint32_t soundSpeed, uint32_t substeps) {
        bool first = false;
        bool second = false;
        const uint64_t hashA = RunDisturbed(soundSpeed, substeps, first);
        const uint64_t hashB = RunDisturbed(soundSpeed, substeps, second);
        std::printf("closed box (%s, %u substeps): conserved every tick = %d, hash %016llx / %016llx\n",
                    SpeedName(soundSpeed), substeps, first ? 1 : 0, (unsigned long long)hashA,
                    (unsigned long long)hashB);
        EXPECT(first && second);
        EXPECT(hashA == hashB);
    }

    // --- 熱い泡(gas_reference_test と同じ場面: 10 × 10 × 20・中心 (5, 5, 4)・半径 1.5 セル・同じ圧力で +30 K)---
    int64_t HeatCentroidZ(const GasBox& box) {
        int64_t weighted = 0;
        int64_t total = 0;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const uint32_t z = Coordinate(box, index)[2];
            const int64_t excess = (int64_t)DeriveGasCell(box, box.cells[index], z).temperature -
                                   (int64_t)box.referenceDerived[z].temperature;
            if (excess <= 0)
                continue;

            weighted += excess * z;
            total += excess;
        }

        return total == 0 ? 0 : weighted * 1000 / total;
    }

    void HeatBubble(GasBox& box) {
        constexpr uint32_t HOT = AIR_TEMPERATURE + 30000;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const std::array<uint32_t, 3> c = Coordinate(box, index);
            const int32_t dx = (int32_t)c[0] - 5;
            const int32_t dy = (int32_t)c[1] - 5;
            const int32_t dz = (int32_t)c[2] - 4;
            if (dx * dx + dy * dy + dz * dz > 2)
                continue;

            std::array<uint64_t, GAS_MAX_SPECIES> amounts = box.cells[index].amounts;
            for (uint64_t& amount : amounts)
                amount = amount * AIR_TEMPERATURE / HOT;

            box.cells[index] = MakeGasCell(box.config, amounts, HOT);
        }
    }

    void TestHotBubble(uint32_t soundSpeed, uint32_t substeps) {
        GasBox box = MakeAirBox(MakeAirConfig(10, 10, 20));
        HeatBubble(box);
        const GasTotals before = SumGas(box);
        const int64_t start = HeatCentroidZ(box);
        GasProjection state;
        CostSummary summary;
        bool conserved = true;
        for (int tick = 0; tick < 120; ++tick) {
            summary.Add(StepGasProjection(box, state, MakeOptions(soundSpeed, substeps)));
            conserved = conserved && ConservedAgainst(box, before);
        }

        const int64_t rise = HeatCentroidZ(box) - start;
        const int64_t upward = GasVelocity(box, box.cells[box.Index(5, 5, 6)], 2);
        std::printf(
            "hot bubble (%s, %u substeps): heat centroid rise %lld (1/1000 cell; gas_reference %lld), w = %.3f m/s, "
            "max speed %.3f m/s\n",
            SpeedName(soundSpeed), substeps, (long long)rise, (long long)REFERENCE_BUBBLE_RISE,
            (double)upward / (double)METER_PER_SECOND, (double)MaxSpeed(box) / (double)METER_PER_SECOND);
        summary.Print(SpeedName(soundSpeed));
        EXPECT(conserved);
        EXPECT(rise * 3 >= REFERENCE_BUBBLE_RISE &&
               rise <= REFERENCE_BUBBLE_RISE * 3);  // 打ち切り条件: 同じ向き・同じ桁
    }

    // --- 爆発: 圧力の前線・熱い所・煙の広がり(D-487 ⑥)---
    constexpr uint32_t BLAST_SIZE = 16;
    constexpr uint32_t BLAST_CENTER = 8;
    constexpr uint32_t BLAST_TEMPERATURE = 1500000;  // mK
    constexpr int BLAST_TICKS = 10;

    GasBox MakeBlastBox() {
        GasBox box = MakeAirBox(MakeAirConfig(BLAST_SIZE, BLAST_SIZE, BLAST_SIZE));
        const uint32_t index = box.Index(BLAST_CENTER, BLAST_CENTER, BLAST_CENTER);
        std::array<uint64_t, GAS_MAX_SPECIES> amounts = box.cells[index].amounts;
        for (uint64_t& amount : amounts)
            amount = amount * 5 * AIR_TEMPERATURE / BLAST_TEMPERATURE;  // 5 倍の圧力・1500 K

        box.cells[index] = MakeGasCell(box.config, amounts, BLAST_TEMPERATURE);
        AddSmoke(box, index);

        return box;
    }

    struct BlastView {
        double nearPressure = 0;  // 中心から 3 セル以内の |p − p_基準| の最大(Pa)
        double farPressure = 0;   // 中心から 5 セル以上の最大(Pa。前線が届いたか)
        int hotCells = 0;         // 基準より 10 K 以上熱いセル
        double hotRadius = 0;     // 熱(基準との差)の重み付けの平均の距離(セル)
        double smokeRadius = 0;   // 煙の印の物質量で重み付けた平均の距離(セル)
    };

    BlastView ViewBlast(const GasBox& box) {
        BlastView view;
        double heat = 0;
        double heatDistance = 0;
        double smoke = 0;
        double smokeDistance = 0;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const std::array<uint32_t, 3> c = Coordinate(box, index);
            const double dx = (double)c[0] - BLAST_CENTER;
            const double dy = (double)c[1] - BLAST_CENTER;
            const double dz = (double)c[2] - BLAST_CENTER;
            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const GasDerived derived = DeriveGasCell(box, box.cells[index], c[2]);
            const GasDerived& reference = box.referenceDerived[c[2]];
            const double pressure = std::abs((double)(derived.pressure - reference.pressure)) * 1e-6;
            if (distance <= 3)
                view.nearPressure = std::max(view.nearPressure, pressure);

            if (distance >= 5)
                view.farPressure = std::max(view.farPressure, pressure);

            const double excess = ((double)derived.temperature - (double)reference.temperature) * 1e-3;
            if (excess >= 10)
                ++view.hotCells;

            if (excess > 0) {
                heat += excess;
                heatDistance += excess * distance;
            }

            const double amount = (double)box.cells[index].amounts[SPECIES_SMOKE];
            smoke += amount;
            smokeDistance += amount * distance;
        }

        view.hotRadius = heat == 0 ? 0 : heatDistance / heat;
        view.smokeRadius = smoke == 0 ? 0 : smokeDistance / smoke;
        return view;
    }

    void PrintBlast(const char* name, int tick, const BlastView& view) {
        std::printf(
            "  %-22s tick %2d: near %9.0f Pa, far %9.0f Pa, hot cells %4d, hot radius %.2f, smoke radius %.2f\n", name,
            tick, view.nearPressure, view.farPressure, view.hotCells, view.hotRadius, view.smokeRadius);
    }

    void TestBlastVisibility() {
        std::printf("blast (16^3 closed box, 1 cell 5x pressure at 1500 K with smoke):\n");
        {
            GasBox box = MakeBlastBox();
            PrintBlast("gas_reference 24", 0, ViewBlast(box));
            for (int tick = 1; tick <= BLAST_TICKS; ++tick) {
                StepGas(box);
                PrintBlast("gas_reference 24", tick, ViewBlast(box));
            }
        }

        const std::array<uint32_t, 4> speeds = {SLOW_SOUND, REAL_SOUND, REAL_SOUND * 10, 0};
        for (const uint32_t speed : speeds) {
            GasBox box = MakeBlastBox();
            const GasTotals before = SumGas(box);
            GasProjection state;
            CostSummary summary;
            std::vector<BlastView> views;
            bool conserved = true;
            for (int tick = 1; tick <= BLAST_TICKS; ++tick) {
                summary.Add(StepGasProjection(box, state, MakeOptions(speed)));
                conserved = conserved && ConservedAgainst(box, before);
                views.push_back(ViewBlast(box));
                PrintBlast(SpeedName(speed), tick, views.back());
            }

            summary.Print(SpeedName(speed));
            EXPECT(conserved);
            // 煙と熱は数刻みかけて広がる(1 刻めで止まらない。D-487 ⑥ の「爆発そのものは見える」)
            EXPECT(views.back().smokeRadius > views.front().smokeRadius);
        }
    }

    // 爆発の後を長く見る(中心の近くの圧力が育たないか・動きが収まるか)
    void TestBlastSettles(uint32_t speed) {
        GasBox box = MakeBlastBox();
        GasProjection state;
        CostSummary summary;
        for (int tick = 1; tick <= 300; ++tick) {
            summary.Add(StepGasProjection(box, state, MakeOptions(speed)));
            if (tick % 50 != 0)
                continue;

            const BlastView view = ViewBlast(box);
            std::printf("  blast long (%s) tick %3d: near %8.0f Pa, far %6.0f Pa, max speed %.3f m/s, hot cells %d\n",
                        SpeedName(speed), tick, view.nearPressure, view.farPressure,
                        (double)MaxSpeed(box) / (double)METER_PER_SECOND, view.hotCells);
        }

        summary.Print(SpeedName(speed));
    }

    // --- 外側の反復(T-0530): 強い圧力の差を同じ刻みで釣り合わせる ---
    uint32_t outerRelaxation = 100;  // 試す倍率(%)
    bool outerSecant = true;         // 試す: セルの傾きで割る(既定と同じ)

    GasProjectionOptions MakeOuterOptions(uint32_t soundSpeed, uint32_t outer, uint32_t toleranceMg,
                                          uint32_t cycles = 64) {
        GasProjectionOptions options = MakeOptions(soundSpeed);
        options.outerRelaxationPercent = outerRelaxation;
        options.outerSecant = outerSecant;
        options.outerIterations = outer;
        options.outerToleranceMilligrams = toleranceMg;
        options.solver.cycles = cycles;
        return options;
    }

    // 全部のセルの |p − p_基準| の最大と最小の差(Pa。閉じた箱では全体が少し上がるので差で見る)
    double PressureSpread(const GasBox& box) {
        double low = 1e30;
        double high = -1e30;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const uint32_t z = Coordinate(box, index)[2];
            const double pressure = (double)(DeriveGasCell(box, box.cells[index], z).pressure -
                                             box.referenceDerived[z].pressure) *
                                    1e-6;
            low = std::min(low, pressure);
            high = std::max(high, pressure);
        }

        return high - low;
    }

    // 爆発(TestBlastVisibility と同じ場面)を外側の反復ありで。1 刻み目の終わりの中心の近くの圧力の差(Pa)を返す
    double RunOuterBlast(uint32_t speed, uint32_t outer, uint32_t toleranceMg, uint32_t cycles, bool print) {
        GasBox box = MakeBlastBox();
        const GasTotals before = SumGas(box);
        GasProjection state;
        CostSummary summary;
        std::vector<BlastView> views;
        bool conserved = true;
        char name[64];
        std::snprintf(name, sizeof(name), "%s outer %u tol %u V %u w %u%%%s", SpeedName(speed), outer, toleranceMg,
                      cycles, outerRelaxation, outerSecant ? " secant" : "");
        for (int tick = 1; tick <= BLAST_TICKS; ++tick) {
            const GasProjectionCost cost = StepGasProjection(box, state,
                                                             MakeOuterOptions(speed, outer, toleranceMg, cycles));
            summary.Add(cost);
            conserved = conserved && ConservedAgainst(box, before);
            views.push_back(ViewBlast(box));
            if (print && (tick <= 3 || tick == BLAST_TICKS)) {
                PrintBlast(name, tick, views.back());
                std::printf("      spread %.0f Pa, outer %u, V %u, mismatch first %lld / kept %lld mg\n",
                            PressureSpread(box), cost.outerIterations, cost.cycles, (long long)cost.firstMismatch,
                            (long long)cost.worstMismatch);
            }
        }

        std::printf("  blast %s: tick 1 near %.0f Pa, tick 2 near %.0f Pa\n", name, views[0].nearPressure,
                    views[1].nearPressure);
        summary.PrintOuter(name);
        EXPECT(conserved);
        EXPECT(views.back().smokeRadius > views.front().smokeRadius);  // 煙は数刻みかけて広がる(D-487 ⑥)

        return views[0].nearPressure;
    }

    // 既定の止め方(上限 4 回・許容差 100 mg・セルの傾きで割る)で、最速の強い爆発が同じ刻みで釣り合う
    void TestOuterBlast() {
        std::printf("outer iteration, blast (16^3 closed box, 1 cell 5x pressure at 1500 K):\n");
        const GasProjectionOptions defaults;
        const uint32_t cycles = defaults.solver.cycles;
        const double linear = RunOuterBlast(0, 1, defaults.outerToleranceMilligrams, cycles, true);
        const double settled = RunOuterBlast(0, defaults.outerIterations, defaults.outerToleranceMilligrams, cycles,
                                             true);
        EXPECT(settled <= 1000);  // 最速なら強い爆発も同じ刻みで釣り合う(D-467)
        EXPECT(settled < linear);
        RunOuterBlast(REAL_SOUND, defaults.outerIterations, defaults.outerToleranceMilligrams, cycles, false);
    }

    // 止め方の違いを測る(--sweep のときだけ。07 §2.6 の表)
    void SweepOuterBlast() {
        outerSecant = false;
        for (const uint32_t outer : {2u, 3u, 4u, 8u})
            RunOuterBlast(0, outer, 1, 64, false);

        RunOuterBlast(0, 8, 10, 64, false);
        RunOuterBlast(0, 8, 100, 64, false);
        RunOuterBlast(0, 8, 1, 1, false);
        RunOuterBlast(0, 16, 1, 1, false);
        RunOuterBlast(REAL_SOUND, 8, 1, 64, true);
        RunOuterBlast(SLOW_SOUND, 8, 1, 64, true);

        // 一律の倍率(速く縮むが、回数を増やすと揺れる)
        outerRelaxation = 150;
        for (const uint32_t outer : {2u, 3u, 8u})
            RunOuterBlast(0, outer, 10, 64, false);

        outerRelaxation = 100;

        // セルの傾きで割る(対角のニュートン)
        outerSecant = true;
        for (const uint32_t outer : {2u, 3u, 4u, 8u})
            RunOuterBlast(0, outer, 10, 64, outer == 4);

        RunOuterBlast(0, 4, 100, 64, false);
        RunOuterBlast(0, 8, 100, 64, false);
        RunOuterBlast(0, 8, 10, 1, false);
        RunOuterBlast(REAL_SOUND, 4, 10, 64, false);
        RunOuterBlast(REAL_SOUND, 8, 10, 64, false);
    }

    // 爆発の後を長く見る(外側の反復あり。炎・熱・煙の広がりが数刻みかけて見えるか。D-487 ⑥)
    void PrintLongBlast(const char* name, int tick, const GasBox& box) {
        const BlastView view = ViewBlast(box);
        std::printf("  long %-20s tick %3d: hot cells %4d, hot radius %.2f, smoke radius %.2f, max speed %.2f m/s\n",
                    name, tick, view.hotCells, view.hotRadius, view.smokeRadius,
                    (double)MaxSpeed(box) / (double)METER_PER_SECOND);
    }

    bool IsLongBlastTick(int tick) {
        return tick == 1 || tick == 3 || tick == 10 || tick == 30 || tick == 60 || tick == 120;
    }

    void TestOuterBlastLong() {
        {
            GasBox box = MakeBlastBox();
            for (int tick = 1; tick <= 120; ++tick) {
                StepGas(box);
                if (IsLongBlastTick(tick))
                    PrintLongBlast("gas_reference 24", tick, box);
            }
        }

        for (const uint32_t outer : {1u, 8u}) {
            GasBox box = MakeBlastBox();
            GasProjection state;
            for (int tick = 1; tick <= 120; ++tick) {
                StepGasProjection(box, state, MakeOuterOptions(0, outer, 10));
                if (IsLongBlastTick(tick))
                    PrintLongBlast(outer == 1 ? "fastest linear" : "fastest outer 8", tick, box);
            }
        }
    }

    // 外側の反復ありでも、保存は毎刻みビット単位・2 回の実行でハッシュ一致
    uint64_t RunOuterDisturbed(uint32_t speed, uint32_t outer, uint32_t toleranceMg, bool& conserved) {
        GasBox box = MakeBlastBox();
        Heat(box, box.Index(3, 3, 1), AIR_TEMPERATURE + 50000);
        const GasTotals before = SumGas(box);
        GasProjection state;
        conserved = true;
        for (int tick = 0; tick < 20; ++tick) {
            StepGasProjection(box, state, MakeOuterOptions(speed, outer, toleranceMg));
            conserved = conserved && ConservedAgainst(box, before);
        }

        return HashGas(box);
    }

    void TestOuterDeterminism(uint32_t speed, uint32_t outer, uint32_t toleranceMg) {
        bool first = false;
        bool second = false;
        const uint64_t hashA = RunOuterDisturbed(speed, outer, toleranceMg, first);
        const uint64_t hashB = RunOuterDisturbed(speed, outer, toleranceMg, second);
        std::printf(
            "outer iteration closed box (%s, outer %u, tol %u mg): conserved every tick = %d, hash %016llx / %016llx\n",
            SpeedName(speed), outer, toleranceMg, first ? 1 : 0, (unsigned long long)hashA, (unsigned long long)hashB);
        EXPECT(first && second);
        EXPECT(hashA == hashB);
    }

    // 弱い差: 熱い泡(外側の反復は 1 回で止まるか・上がり方は同じ桁か)
    void TestOuterBubble(uint32_t speed, uint32_t outer, uint32_t toleranceMg) {
        GasBox box = MakeAirBox(MakeAirConfig(10, 10, 20));
        HeatBubble(box);
        const int64_t start = HeatCentroidZ(box);
        GasProjection state;
        CostSummary summary;
        for (int tick = 0; tick < 120; ++tick)
            summary.Add(StepGasProjection(box, state, MakeOuterOptions(speed, outer, toleranceMg)));

        const int64_t rise = HeatCentroidZ(box) - start;
        std::printf("outer iteration hot bubble (%s, outer %u, tol %u mg%s): heat centroid rise %lld (1/1000 cell)\n",
                    SpeedName(speed), outer, toleranceMg, outerSecant ? ", secant" : "", (long long)rise);
        summary.PrintOuter(SpeedName(speed));
        EXPECT(rise * 3 >= REFERENCE_BUBBLE_RISE && rise <= REFERENCE_BUBBLE_RISE * 3);
    }

    // 弱い差: 扉(閉じた箱の左半分が 2% 高い圧力・同じ温度。仕切りを外した刻みから)
    void TestOuterDoor(uint32_t speed, uint32_t outer, uint32_t toleranceMg) {
        GasBox box = MakeAirBox(MakeAirConfig(16, 8, 8));
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            if (Coordinate(box, index)[0] >= 8)
                continue;

            std::array<uint64_t, GAS_MAX_SPECIES> amounts = box.cells[index].amounts;
            for (uint64_t& amount : amounts)
                amount = amount * 102 / 100;

            box.cells[index] = MakeGasCell(box.config, amounts, AIR_TEMPERATURE);
        }

        const double initial = PressureSpread(box);
        GasProjection state;
        CostSummary summary;
        std::array<double, 3> spreads = {};
        for (int tick = 1; tick <= 30; ++tick) {
            summary.Add(StepGasProjection(box, state, MakeOuterOptions(speed, outer, toleranceMg)));
            if (tick <= 3)
                spreads[tick - 1] = PressureSpread(box);
        }

        std::printf("door (%s, outer %u, tol %u mg%s): spread %.0f -> %.0f, %.0f, %.0f Pa\n", SpeedName(speed), outer,
                    toleranceMg, outerSecant ? ", secant" : "", initial, spreads[0], spreads[1], spreads[2]);
        summary.PrintOuter(SpeedName(speed));
    }

}  // namespace

// 引数 --sweep で、外側の反復の止め方の違いを測る(遅い。ctest では回さない)
int main(int argc, char** argv) {
    const bool sweep = argc > 1 && std::string_view(argv[1]) == "--sweep";
    if (!sweep) {
        TestStaticAtmosphere(REAL_SOUND);
        TestStaticAtmosphere(0);
        TestConservationAndDeterminism(REAL_SOUND, 1);
        TestConservationAndDeterminism(0, 1);
        TestConservationAndDeterminism(REAL_SOUND, 3);
        TestHotBubble(REAL_SOUND, 1);
        TestHotBubble(REAL_SOUND * 10, 1);
        TestHotBubble(0, 1);
        TestBlastVisibility();
        TestBlastSettles(REAL_SOUND);
        TestBlastSettles(0);
    }

    // --- 外側の反復(T-0530。既定の止め方)---
    const GasProjectionOptions defaults;
    TestOuterBlast();
    TestOuterDeterminism(0, defaults.outerIterations, defaults.outerToleranceMilligrams);
    TestOuterDeterminism(REAL_SOUND, 8, 1);  // 上限に届く刻みがある止め方でも
    TestOuterBubble(0, defaults.outerIterations, defaults.outerToleranceMilligrams);
    TestOuterBubble(REAL_SOUND, defaults.outerIterations, defaults.outerToleranceMilligrams);
    TestOuterDoor(0, defaults.outerIterations, defaults.outerToleranceMilligrams);
    TestOuterDoor(REAL_SOUND, defaults.outerIterations, defaults.outerToleranceMilligrams);

    if (sweep) {
        SweepOuterBlast();
        TestOuterBlastLong();
        for (const bool secant : {false, true}) {
            outerSecant = secant;
            for (const uint32_t tolerance : {1u, 10u, 100u, 300u})
                TestOuterBubble(0, 8, tolerance);

            TestOuterDoor(0, 8, 10);
            TestOuterDoor(REAL_SOUND, 8, 10);
        }
    }

    if (failureCount != 0) {
        std::printf("%d failure(s)\n", failureCount);
        return 1;
    }

    std::printf("all passed\n");
    return 0;
}
