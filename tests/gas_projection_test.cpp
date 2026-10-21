// gas_projection_test.cpp — 気体の圧力を刻みの中で解く投影の CPU 模型(sim/gas_projection。T-0503 研究・07 §2.6)を CPU だけで確かめ、測る。
// 合格の条件(研究の打ち切り条件。docs/tickets/T-0503-pressure-projection-model.md):
//   静止大気が 600 刻みで速度がちょうど 0・閉じた箱で成分がビット一致で一定、エネルギーと運動量は毎刻み「初め + 帳簿」とビット一致・
//   同じ入力なら同じハッシュ・熱い泡(gas_reference と同じ場面)の熱の重心が 2 秒で gas_reference(1.32 セル)の 1/3〜3 倍上がる。
// 測る値(表示だけ): 1 刻みの V サイクルの回数(音速 340 m/s・3400 m/s・最速)と、爆発(1 セル 5 倍の圧力・1500 K・煙の印)で
//   圧力の前線・熱い所・煙の広がりが刻みごとにどう見えるか(gas_reference の小刻み 24 と比べる。D-487 ⑥)。
// 外側の反復(T-0530): 最速で 5 倍の爆発が 1 刻み目の終わりに中心の近くの圧力の差 1 kPa 以下・外側の反復ありでも保存はビット単位・ハッシュ一致・
//   熱い泡は同じ桁。測る値: 外側の反復の回数 × V サイクル(弱い差〔泡・扉〕と強い爆発)・許容差と上限の違い。
// セルの空き(T-0667): 塞がった壁の向こうへ 0 µmol・孔の大きさの順に渡る量が減る(陽解法と同じ向き)・保存・ハッシュ一致。
//   測る値: 空き 1/256 のセルを含む 12 × 24 × 12 の箱で内側の解きが上限 8 の V に入るか(研究の打ち切り条件)。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "sim/gas_fastest_check.h"
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

            GasPerSpecies<uint64_t> amounts = box.cells[index].amounts;
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
        GasPerSpecies<uint64_t> amounts = box.cells[index].amounts;
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

            GasPerSpecies<uint64_t> amounts = box.cells[index].amounts;
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

    // --- セルの空き(T-0667・07 §2.8): 投影も陽解法と同じ数え方で、塞がった所を気体が通らない ---
    // 壁の場面: 12 × 4 × 8 の閉じた箱の x = 6 の板を塞ぎ(空き 0)、真ん中 2 × 2 を孔(空き share)に。左が 2% 高い圧力
    constexpr uint32_t WALL_X = 6;
    constexpr uint32_t SHARE_NONE = 0xFFFFFFFFu;  // 壁なし

    // セルの空きを share にし、気体を基準 × share に減らす(同じ温度。固体が占めた分の空気は無い)
    void Occupy(GasBox& box, uint32_t index, uint32_t share) {
        if (box.openQ16.empty())
            box.openQ16.assign(box.cells.size(), GAS_OPEN_FULL);

        box.openQ16[index] = share;
        GasPerSpecies<uint64_t> amounts = box.cells[index].amounts;
        for (uint64_t& amount : amounts)
            amount = amount * share / GAS_OPEN_FULL;

        box.cells[index] = MakeGasCell(box.config, amounts, AIR_TEMPERATURE);
    }

    void RaiseLeft(GasBox& box, uint32_t limitX) {
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            if (Coordinate(box, index)[0] >= limitX)
                continue;

            GasPerSpecies<uint64_t> amounts = box.cells[index].amounts;
            for (uint64_t& amount : amounts)
                amount = amount * 102 / 100;

            box.cells[index] = MakeGasCell(box.config, amounts, AIR_TEMPERATURE);
        }
    }

    GasBox MakeWallBox(uint32_t holeShare) {
        GasBox box = MakeAirBox(MakeAirConfig(12, 4, 8));
        RaiseLeft(box, WALL_X);
        if (holeShare == SHARE_NONE)
            return box;

        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const auto [x, y, z] = Coordinate(box, index);
            if (x != WALL_X)
                continue;

            const bool hole = (y == 1 || y == 2) && (z == 3 || z == 4);
            Occupy(box, index, hole ? holeShare : 0);
        }

        return box;
    }

    // 壁の右(x > WALL_X)の物質量の合計(µmol)
    uint64_t RightAmount(const GasBox& box) {
        uint64_t total = 0;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            if (Coordinate(box, index)[0] <= WALL_X)
                continue;

            for (const uint64_t amount : box.cells[index].amounts)
                total += amount;
        }

        return total;
    }

    // 30 刻みで右へ渡った物質量(µmol)。projection = false は陽解法(StepGas)。保存とハッシュも返す
    struct WallRun {
        int64_t crossed = 0;
        bool conserved = true;
        uint64_t hash = 0;
        CostSummary cost;
    };

    WallRun RunWall(uint32_t holeShare, bool projection, uint32_t speed) {
        GasBox box = MakeWallBox(holeShare);
        const GasTotals before = SumGas(box);
        const uint64_t start = RightAmount(box);
        GasProjection state;
        GasProjectionOptions options;
        options.pressure.soundSpeedMmPerS = speed;
        WallRun run;
        for (int tick = 0; tick < 30; ++tick) {
            if (projection)
                run.cost.Add(StepGasProjection(box, state, options));
            else
                StepGas(box);

            run.conserved = run.conserved && ConservedAgainst(box, before);
        }

        run.crossed = (int64_t)RightAmount(box) - (int64_t)start;
        run.hash = HashGas(box);
        return run;
    }

    const char* ShareName(uint32_t share) {
        if (share == SHARE_NONE)
            return "no wall";

        if (share == 0)
            return "closed wall";

        return share == GAS_OPEN_FULL / 2 ? "hole 1/2" : "hole 1/256";
    }

    void TestWall() {
        constexpr std::array<uint32_t, 4> SHARES = {SHARE_NONE, GAS_OPEN_FULL / 2, GAS_OPEN_FULL / 256, 0};
        for (const int method : {0, 1, 2}) {
            const bool projection = method != 0;
            const uint32_t speed = method == 2 ? 0 : REAL_SOUND;
            const char* name = projection ? (speed == 0 ? "projection fastest" : "projection 340 m/s") : "explicit";
            std::array<int64_t, 4> crossed = {};
            for (size_t s = 0; s < SHARES.size(); ++s) {
                const WallRun first = RunWall(SHARES[s], projection, speed);
                const WallRun second = RunWall(SHARES[s], projection, speed);
                crossed[s] = first.crossed;
                std::printf("wall (%s, %s): crossed %lld umol in 30 ticks, conserved %d, hash %016llx / %016llx\n",
                            name, ShareName(SHARES[s]), (long long)first.crossed, first.conserved ? 1 : 0,
                            (unsigned long long)first.hash, (unsigned long long)second.hash);
                if (projection)
                    first.cost.PrintOuter(ShareName(SHARES[s]));

                EXPECT(first.conserved && second.conserved);
                EXPECT(first.hash == second.hash);
            }

            // 塞がった壁の向こうはビット単位で動かない・孔は渡す。陽解法と音速を選んだ投影は 孔 1/2 > 孔 1/256(同じ向き)。
            // 最速は刻みの中で釣り合うまで流す(D-467)ので、孔の大きさによらず 1 刻み目で釣り合う(30 刻み目は揺れの位相の差だけ。比べない)。
            // 壁なしとは比べない(壁の列〔空き 0〕の分だけ箱の体積が減り、釣り合いで右へ渡る量が 12/11 倍になる)
            if (!projection || speed != 0)
                EXPECT(crossed[1] > crossed[2]);

            EXPECT(crossed[2] > 0);
            EXPECT(crossed[3] == 0);
        }
    }

    // 研究(T-0667 の打ち切り条件): 空き 1/256 のセルを含む 12 × 24 × 12 の箱で内側の解き(V の上限 8)が許容差に入るか。
    // 板 = z = 6 の層を全部 1/256(細い孔の板。下が 2% 高い圧力)・点 = 7 セルに 1 つを 1/256(散らばった小さな空き)
    void RunPorous(bool plate, uint32_t speed, bool galerkin) {
        GasBox box = MakeAirBox(MakeAirConfig(12, 24, 12));
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            if (Coordinate(box, index)[2] >= 6)
                continue;

            GasPerSpecies<uint64_t> amounts = box.cells[index].amounts;
            for (uint64_t& amount : amounts)
                amount = amount * 102 / 100;

            box.cells[index] = MakeGasCell(box.config, amounts, AIR_TEMPERATURE);
        }

        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const bool porous = plate ? Coordinate(box, index)[2] == 6 : index % 7 == 3;
            if (porous)
                Occupy(box, index, GAS_OPEN_FULL / 256);
        }

        const GasTotals before = SumGas(box);
        GasProjection state;
        GasProjectionOptions options;
        options.pressure.soundSpeedMmPerS = speed;
        options.galerkinCoarse = galerkin;
        CostSummary summary;
        bool conserved = true;
        for (int tick = 0; tick < 10; ++tick) {
            summary.Add(StepGasProjection(box, state, options));
            conserved = conserved && ConservedAgainst(box, before);
        }

        std::printf(
            "porous %s (%s%s, 12x24x12, 10 ticks): inner unconverged %u, worst inner error %lld mg, conserved %d\n",
            plate ? "plate" : "points", SpeedName(speed), galerkin ? ", galerkin" : "", summary.unconverged,
            (long long)summary.worstError, conserved ? 1 : 0);
        summary.PrintOuter(SpeedName(speed));
        EXPECT(conserved);
    }

    // T-0722(T-0689 の再現。ADR-0346): gpu_gas_projection の細孔の板(16³・z = 4 の層を全部 1/256・下が 2% 高い)。
    // 音速(340 m/s)は外側の反復が上限の中で収まる。最速はまだ発散する(研究の打ち切り: 通り抜け・切った流れを除く・力積の上限の
    // 3 つを試して収まらなかった。最速 × 細孔は既定にしない〔D-515〕。答えは ADR-0346)。
    // 前は最速で、出ていく量の上限で切られた孔の流れを式が K で数え続け、外側の反復が流れない分を足し続けて、
    // 3 刻みで |x| が 5.2 × 10^18 µPa(int64 の上限 9.2 × 10^18 の手前)まで育った(T-0667 で見つけた)。
    // |x| の上限(µPa): 釣り合った式は |x| ≤ max |右辺の質量| ÷ C_i(C_i の最小 = 最速の下限 2^20 × 空き 1/256 = 2^12 の Q48。
    // 右辺の質量はセル数個分の質量 = 10^6 mg 程度)なので 10^6 × 2^36 ≈ 7 × 10^16。それより 1 桁上を上限にする
    constexpr int64_t PLATE16_PRESSURE_BOUND = (int64_t)1 << 59;

    struct Plate16Run {
        int64_t largest = 0;        // 全部の刻みの |x| の最大(µPa)
        int64_t worstMismatch = 0;  // 採った状態の食い違いの最大(mg)
        uint32_t outerUnconverged = 0;
        int64_t maxSpeed = 0;
        bool conserved = true;
        uint64_t hash = 0;
    };

    // 板の層(z = 4)のセルの形(T-0788 の調べ)。All = 全部を share・ClosedBetween = every セルに 1 つを share・残りを塞ぐ(孔のある壁)・
    // OpenBetween = every セルに 1 つを開けたまま・残りを share(大きな孔のある多孔の板)
    enum class PlateHoles : uint8_t { All, ClosedBetween, OpenBetween };

    struct PlateShape {
        uint32_t share = GAS_OPEN_FULL / 256;
        PlateHoles holes = PlateHoles::All;
        uint32_t every = 1;
        uint32_t raisePercent = 102;  // 板の下の気体の量(%)
    };

    GasBox MakePlate(const PlateShape& shape) {
        GasBox box = MakeAirBox(MakeAirConfig(16, 16, 16));
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            if (Coordinate(box, index)[2] >= 4)
                continue;

            GasPerSpecies<uint64_t> amounts = box.cells[index].amounts;
            for (uint64_t& amount : amounts)
                amount = amount * shape.raisePercent / 100;

            box.cells[index] = MakeGasCell(box.config, amounts, AIR_TEMPERATURE);
        }

        uint32_t layerIndex = 0;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            if (Coordinate(box, index)[2] != 4)
                continue;

            const bool marked = layerIndex++ % shape.every == 0;
            if (shape.holes == PlateHoles::All || (shape.holes == PlateHoles::ClosedBetween && marked))
                Occupy(box, index, shape.share);
            else if (shape.holes == PlateHoles::ClosedBetween)
                Occupy(box, index, 0);
            else if (!marked)
                Occupy(box, index, shape.share);
        }

        return box;
    }

    GasBox MakePlate16() {
        return MakePlate({});
    }

    Plate16Run RunPlate(GasBox box, uint32_t speed, int ticks, bool print, bool exclude) {
        const GasTotals before = SumGas(box);
        GasProjection state;
        GasProjectionOptions options;
        options.pressure.soundSpeedMmPerS = speed;
        options.pressure.excludeCutFlows = exclude;
        Plate16Run run;
        for (int tick = 0; tick < ticks; ++tick) {
            const GasProjectionCost cost = StepGasProjection(box, state, options);
            run.conserved = run.conserved && ConservedAgainst(box, before);
            run.worstMismatch = std::max(run.worstMismatch, cost.worstMismatch);
            run.outerUnconverged += cost.outerUnconverged;
            run.maxSpeed = std::max(run.maxSpeed, MaxSpeed(box));
            int64_t largest = 0;
            for (uint32_t index = 0; index < state.pressures.size(); ++index) {
                // 塞がったセル(空き 0)は式の面を持たず、x は値に効かない(T-0788 の調べで、孔のある壁の塞がったセルの x だけが育った)
                if (!box.openQ16.empty() && box.openQ16[index] == 0)
                    continue;

                const int64_t value = state.pressures[index];
                largest = std::max(largest, value == INT64_MIN ? INT64_MAX : std::abs(value));
            }

            run.largest = std::max(run.largest, largest);
            if (print && (tick < 4 || tick % 10 == 9)) {
                std::printf(
                    "plate16 %s%s tick %d: outer %u, mismatch first %lld / kept %lld mg, max |x| %lld uPa, "
                    "max speed %lld\n",
                    SpeedName(speed), exclude ? "" : " (cut not excluded)", tick, cost.outerIterations,
                    (long long)cost.firstMismatch, (long long)cost.worstMismatch, (long long)largest,
                    (long long)MaxSpeed(box));
            }
        }

        run.hash = HashGas(box);
        return run;
    }

    Plate16Run RunPlate16(uint32_t speed, int ticks, bool print, bool exclude = GasPressureOptions{}.excludeCutFlows) {
        return RunPlate(MakePlate16(), speed, ticks, print, exclude);
    }

    // T-0788 の調べ(--plates): 板の空き・形・圧力の差を変えて最速で流し、外側の反復が |x| の上限の中で収まるかを表にする
    void SweepPlates() {
        const auto report = [](const char* label, const PlateShape& shape) {
            const GasBox box = MakePlate(shape);
            const size_t joints = FindGasSmallOpenJoints(box.config, box.openQ16).size();
            const Plate16Run run = RunPlate(box, 0, 40, false, GasPressureOptions{}.excludeCutFlows);
            // 警告の閾値の根拠: 壊れた板は必ず警告する(警告しない板は壊れない)
            EXPECT(run.conserved);
            EXPECT(run.largest < PLATE16_PRESSURE_BOUND || joints != 0);
            std::printf(
                "plates %s share 1/%u every %u raise %u%%: warned %d, max |x| %lld uPa, kept mismatch %lld mg, "
                "outer at limit %u, max speed %lld, conserved %d, %s\n",
                label, shape.share == 0 ? 0 : GAS_OPEN_FULL / shape.share, shape.every, shape.raisePercent,
                joints != 0 ? 1 : 0, (long long)run.largest, (long long)run.worstMismatch, run.outerUnconverged,
                (long long)run.maxSpeed, run.conserved ? 1 : 0,
                run.largest < PLATE16_PRESSURE_BOUND ? "bounded" : "DIVERGED");
        };

        for (const uint32_t divisor : {2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u}) {
            report("all", {.share = GAS_OPEN_FULL / divisor});
            report("all", {.share = GAS_OPEN_FULL / divisor, .raisePercent = 120});
        }

        for (const uint32_t every : {2u, 4u, 16u, 64u}) {
            report("closed-between", {.holes = PlateHoles::ClosedBetween, .every = every});
            report("open-between", {.holes = PlateHoles::OpenBetween, .every = every});
        }

        for (const uint32_t raise : {150u, 200u, 500u}) {
            report("none", {.share = GAS_OPEN_FULL, .raisePercent = raise});
            for (const uint32_t divisor : {2u, 4u, 8u, 16u})
                report("all", {.share = GAS_OPEN_FULL / divisor, .raisePercent = raise});
        }

        report("closed", {.share = 0});
        report("closed-between", {.share = GAS_OPEN_FULL / 2, .holes = PlateHoles::ClosedBetween, .every = 4});
        for (const uint32_t divisor : {16u, 32u, 64u}) {
            report("closed-between",
                   {.share = GAS_OPEN_FULL / divisor, .holes = PlateHoles::ClosedBetween, .every = 4});
            report("open-between", {.share = GAS_OPEN_FULL / divisor, .holes = PlateHoles::OpenBetween, .every = 16});
        }
    }

    void TestPlate16() {
        for (const uint32_t speed : {0u, REAL_SOUND}) {
            const Plate16Run first = RunPlate16(speed, 30, true);
            const Plate16Run second = RunPlate16(speed, 30, false);
            std::printf(
                "plate16 %s (30 ticks): max |x| %lld uPa, worst kept mismatch %lld mg, outer at limit %u, "
                "max speed %lld, conserved %d, hash %016llx / %016llx\n",
                SpeedName(speed), (long long)first.largest, (long long)first.worstMismatch, first.outerUnconverged,
                (long long)first.maxSpeed, first.conserved ? 1 : 0, (unsigned long long)first.hash,
                (unsigned long long)second.hash);
            EXPECT(first.conserved && second.conserved);
            EXPECT(first.hash == second.hash);
            // 最速はまだ発散する(T-0722 の研究の答え。ADR-0346: 打ち切り。最速 × 細孔は既定にしない〔D-515〕)。音速は上限の中
            if (speed != 0)
                EXPECT(first.largest < PLATE16_PRESSURE_BOUND);
        }
    }

    // T-0788(D-519): 最速 × 小さい空きの検査。小さい空き(1/2 未満)のセルだけでつながった気体の部分の組を見つけるか
    size_t JointsOf(const GasBox& box) {
        return FindGasSmallOpenJoints(box.config, box.openQ16).size();
    }

    GasBox MakeLayerBox(uint32_t axis, uint32_t layer, uint32_t share, GasBoundary boundary) {
        GasBox box = MakeAirBox(MakeAirConfig(8, 8, 8));
        box.config.boundary[axis] = boundary;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            if (Coordinate(box, index)[axis] == layer)
                Occupy(box, index, share);
        }

        return box;
    }

    void TestFastestCheck() {
        // 16³ の板(z = 4 の層を全部 1/256): 下の 4 層と上の 11 層を 256 セルの孔だけでつなぐ
        const GasBox plate = MakePlate16();
        const std::vector<GasSmallOpenJoint> joints = FindGasSmallOpenJoints(plate.config, plate.openQ16);
        EXPECT(joints.size() == 1);
        if (joints.size() == 1) {
            const GasSmallOpenJoint& joint = joints[0];
            EXPECT(joint.parts == 2 && !joint.outside && joint.smallCells == 256);
            EXPECT(joint.smallestOpenQ16 == GAS_OPEN_FULL / 256 && joint.smallestPartCells == 16 * 16 * 4);
            EXPECT((joint.minCell == std::array<uint32_t, 3>{0, 0, 4}));
            EXPECT((joint.maxCell == std::array<uint32_t, 3>{15, 15, 4}));
            const std::string warning = GasSmallOpenJointWarning(joint, "箱", "z = 4");
            std::printf("fastest check: %s\n", warning.c_str());
            EXPECT(warning.starts_with(GAS_FASTEST_SMALL_OPEN_WARNING_PREFIX));
            EXPECT(warning.find("0.39%") != std::string::npos && warning.find("2% 高いだけで") != std::string::npos);
        }

        EXPECT(JointsOf(MakePlate({.share = GAS_OPEN_FULL / 2})) == 0);  // 1/2 は小さくない(測った 5 倍の差でも収まる)
        EXPECT(JointsOf(MakePlate({.share = (GAS_OPEN_FULL / 2) - 1})) == 1);
        EXPECT(JointsOf(MakePlate({.share = 0})) == 0);  // 塞がった板は分けるだけ(つながない)
        EXPECT(JointsOf(MakePlate({.holes = PlateHoles::OpenBetween, .every = 64})) ==
               0);  // 開いた孔が 4 つあれば部分は 1 つ
        // 孔のある壁: 64 の孔(それぞれ塞がったセルに囲まれる)は同じ 2 つの部分をつなぐので 1 つの組
        const GasBox holes = MakePlate({.share = GAS_OPEN_FULL / 16, .holes = PlateHoles::ClosedBetween, .every = 4});
        const std::vector<GasSmallOpenJoint> holeJoints = FindGasSmallOpenJoints(holes.config, holes.openQ16);
        EXPECT(holeJoints.size() == 1 && holeJoints[0].smallCells == 64);
        EXPECT(JointsOf(MakeWallBox(GAS_OPEN_FULL / 256)) == 1);
        EXPECT(JointsOf(MakeWallBox(GAS_OPEN_FULL / 2)) == 0);
        EXPECT(JointsOf(MakeAirBox(MakeAirConfig(8, 8, 8))) == 0);  // 空きが全部(openQ16 が空)

        // 端の層: 閉じた床は部分を 1 つにしか接しない。開いた天井は箱の中と外をつなぐ
        EXPECT(JointsOf(MakeLayerBox(2, 0, GAS_OPEN_FULL / 16, GasBoundary::Wall)) == 0);
        GasBox roof = MakeLayerBox(2, 7, GAS_OPEN_FULL / 16, GasBoundary::Open);  // 上と下が開く。下の層は塞ぐ
        for (uint32_t index = 0; index < roof.cells.size(); ++index) {
            if (Coordinate(roof, index)[2] == 0)
                Occupy(roof, index, 0);
        }

        const std::vector<GasSmallOpenJoint> roofJoints = FindGasSmallOpenJoints(roof.config, roof.openQ16);
        EXPECT(roofJoints.size() == 1 && roofJoints[0].outside && roofJoints[0].parts == 2);
        // 周期の境界: 真ん中の層は、壁なら 2 つに分けるが、周期なら端を回ってつながる
        EXPECT(JointsOf(MakeLayerBox(0, 3, GAS_OPEN_FULL / 16, GasBoundary::Wall)) == 1);
        EXPECT(JointsOf(MakeLayerBox(0, 3, GAS_OPEN_FULL / 16, GasBoundary::Periodic)) == 0);
    }

    void TestPorous() {
        for (const bool galerkin : {false, true}) {
            for (const bool plate : {true, false}) {
                RunPorous(plate, 0, galerkin);
                RunPorous(plate, REAL_SOUND, galerkin);
            }
        }
    }

    // --- T-0842(研究): 本物の音速の陰解法を 1 刻みに 1 回・V 固定 2・外側 0(と 1)で解く賭け ---
    // 版 1 = 音速 340 m/s(C = V/c²)・V サイクル 2 回ちょうど・excludeCutFlows。版 2 = 版 1 + 運ぶ側の局所の小刻み(上限 8)+ 切った面の運動量を縮める。
    // 合格: 300 刻みで |x| ≤ 初めの差 × 10・最大の速さ ≤ 2√(2Δp/ρ)・保存がビット一致・2 回の実行でハッシュ一致・泡の重心が陽解法と ±10 %
    struct BetVariant {
        const char* name = "";
        uint32_t outer = 1;  // 外側の反復の上限(1 = 解き直さない = 外側 0)
        bool local = false;  // 版 2
    };

    constexpr std::array<BetVariant, 4> BET_VARIANTS = {{{.name = "v1 outer0", .outer = 1, .local = false},
                                                         {.name = "v1 outer1", .outer = 2, .local = false},
                                                         {.name = "v2 outer0", .outer = 1, .local = true},
                                                         {.name = "v2 outer1", .outer = 2, .local = true}}};
    constexpr double AIR_DENSITY = 1.204;  // kg/m³(293 K・1 気圧)
    constexpr int64_t BET_OVERFLOW = (int64_t)1 << 59;

    GasProjectionOptions MakeBetOptions(const BetVariant& variant) {
        GasProjectionOptions options;
        options.pressure.soundSpeedMmPerS = REAL_SOUND;
        options.pressure.excludeCutFlows = true;
        options.outerIterations = variant.outer;
        options.solver.cycles = 2;
        options.fixedCycles = true;
        if (variant.local) {
            options.pressure.localSubsteps = 8;
            options.pressure.scaleCutMomentum = true;
        }

        return options;
    }

    // 初めの差(µPa。x と同じ p̃)= 空きのあるセルの p̃ の最大 − 最小
    int64_t InitialSpread(const GasBox& box) {
        const GasPressureSystem system = BeginGasPressure(box, {});
        int64_t low = INT64_MAX;
        int64_t high = INT64_MIN;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            if (!box.openQ16.empty() && box.openQ16[index] == 0)
                continue;

            low = std::min(low, system.statePressures[index]);
            high = std::max(high, system.statePressures[index]);
        }

        return high - low;
    }

    struct BetRun {
        int64_t largest = 0;  // |x| の最大(µPa。塞がったセルを除く)
        int64_t maxSpeed = 0;
        int overTick = -1;  // |x| が上限を超えた最初の刻み
        bool conserved = true;
        uint64_t hash = 0;
        double seconds = 0;  // 刻みの呼び出しだけの時間
        int ticks = 0;
        int64_t crossedEarly = 0;  // 30 刻みで下流へ渡った物質量(µmol)
        int64_t crossed = 0;       // 終わりまでに下流へ渡った物質量(µmol)
    };

    // 下流(軸 axis の座標が from 以上)の物質量の合計(µmol)。axis が GAS_AXES なら 0
    struct BetRegion {
        uint32_t axis = GAS_AXES;
        uint32_t from = 0;
    };

    int64_t DownstreamAmount(const GasBox& box, BetRegion region) {
        if (region.axis >= GAS_AXES)
            return 0;

        int64_t total = 0;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            if (Coordinate(box, index)[region.axis] < region.from)
                continue;

            for (const uint64_t amount : box.cells[index].amounts)
                total += (int64_t)amount;
        }

        return total;
    }

    // options が null なら陽解法(gas_reference の小刻み)
    BetRun RunBet(GasBox box, const GasProjectionOptions* options, int ticks, int64_t bound, BetRegion region) {
        const GasTotals before = SumGas(box);
        const int64_t startAmount = DownstreamAmount(box, region);
        GasProjection state;
        BetRun run;
        for (int tick = 0; tick < ticks; ++tick) {
            const auto start = std::chrono::steady_clock::now();
            if (options != nullptr)
                (void)StepGasProjection(box, state, *options);
            else
                StepGas(box);

            run.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            run.ticks = tick + 1;
            run.conserved = run.conserved && ConservedAgainst(box, before);
            run.maxSpeed = std::max(run.maxSpeed, MaxSpeed(box));
            for (uint32_t index = 0; index < state.pressures.size(); ++index) {
                if (!box.openQ16.empty() && box.openQ16[index] == 0)
                    continue;

                const int64_t value = state.pressures[index];
                run.largest = std::max(run.largest, value == INT64_MIN ? INT64_MAX : std::abs(value));
            }

            if (run.overTick < 0 && run.largest > bound)
                run.overTick = tick;

            if (tick + 1 == 30)
                run.crossedEarly = DownstreamAmount(box, region) - startAmount;

            if (run.largest > BET_OVERFLOW)  // int64 の手前で止める(発散)
                break;
        }

        run.hash = HashGas(box);
        run.crossed = DownstreamAmount(box, region) - startAmount;
        return run;
    }

    struct BetScene {
        std::string name;
        GasBox box;
        bool still = false;  // 静止大気(速さがちょうど 0)
        BetRegion region;    // 渡った量を数える下流
    };

    GasBox MakeDoorBox() {
        GasBox box = MakeAirBox(MakeAirConfig(16, 8, 8));
        RaiseLeft(box, 8);
        return box;
    }

    std::vector<BetScene> MakeBetScenes() {
        std::vector<BetScene> scenes;
        for (const uint32_t divisor : {256u, 16u, 4u}) {
            for (const uint32_t raise : {102u, 120u}) {
                scenes.push_back({.name = std::format("plate 1/{} x{}.{:02}", divisor, raise / 100, raise % 100),
                                  .box = MakePlate({.share = GAS_OPEN_FULL / divisor, .raisePercent = raise}),
                                  .region = {.axis = 2, .from = 5}});
            }
        }

        scenes.push_back({.name = "wall hole 1/256",
                          .box = MakeWallBox(GAS_OPEN_FULL / 256),
                          .region = {.axis = 0, .from = WALL_X + 1}});
        scenes.push_back(
            {.name = "wall 64 holes 1/16",
             .box = MakePlate({.share = GAS_OPEN_FULL / 16, .holes = PlateHoles::ClosedBetween, .every = 4}),
             .region = {.axis = 2, .from = 5}});
        scenes.push_back({.name = "door x1.02", .box = MakeDoorBox(), .region = {.axis = 0, .from = 8}});
        scenes.push_back({.name = "hot column (blast)", .box = MakeBlastBox()});
        scenes.push_back({.name = "static", .box = MakeAirBox(MakeAirConfig(4, 4, 16)), .still = true});
        return scenes;
    }

    double NanosecondsPerCellTick(const BetRun& run, size_t cells) {
        return run.ticks == 0 ? 0.0 : run.seconds * 1e9 / ((double)run.ticks * (double)cells);
    }

    // 1 つの場面・1 つの版。合格なら true
    bool ReportBet(const BetScene& scene, const BetVariant& variant, int ticks, const BetRun& reference) {
        const double explicitNs = NanosecondsPerCellTick(reference, scene.box.cells.size());
        const int64_t spread = InitialSpread(scene.box);
        const int64_t bound = scene.still ? 0 : spread * 10;
        const double deltaPascal = (double)spread * 1e-6;
        const double speedBound = 2.0 * std::sqrt(2.0 * deltaPascal / AIR_DENSITY);
        const GasProjectionOptions options = MakeBetOptions(variant);
        const BetRun first = RunBet(scene.box, &options, ticks, bound, scene.region);
        const BetRun second = RunBet(scene.box, &options, ticks, bound, scene.region);
        const double speed = (double)first.maxSpeed / (double)METER_PER_SECOND;
        const bool bounded = first.ticks == ticks && (scene.still ? first.maxSpeed == 0 : first.largest <= bound);
        const bool slow = scene.still ? first.maxSpeed == 0 : speed <= speedBound;
        const bool same = first.hash == second.hash;
        const bool pass = bounded && slow && first.conserved && second.conserved && same;
        std::printf(
            "bet | %-20s | %-9s | dp %8.1f Pa | max|x|/dp %8.2f (over at %d) | speed %8.2f / bound %7.2f m/s | "
            "conserved %d | hash %d | %7.0f ns/cell/tick (explicit %7.0f) | ticks %d | crossed 30: %lld (explicit "
            "%lld) end: %lld (explicit %lld) | %s\n",
            scene.name.c_str(), variant.name, deltaPascal, spread == 0 ? 0.0 : (double)first.largest / (double)spread,
            first.overTick, speed, speedBound, first.conserved && second.conserved ? 1 : 0, same ? 1 : 0,
            NanosecondsPerCellTick(first, scene.box.cells.size()), explicitNs, first.ticks,
            (long long)first.crossedEarly, (long long)reference.crossedEarly, (long long)first.crossed,
            (long long)reference.crossed, pass ? "PASS" : "FAIL");
        return pass;
    }

    // 泡: 2 秒(120 刻み)の熱の重心の上がりを陽解法と比べる(±10 %)
    bool ReportBetBubble(const BetVariant& variant, int64_t explicitRise) {
        GasBox box = MakeAirBox(MakeAirConfig(10, 10, 20));
        HeatBubble(box);
        const int64_t start = HeatCentroidZ(box);
        const GasTotals before = SumGas(box);
        const GasProjectionOptions options = MakeBetOptions(variant);
        GasProjection state;
        bool conserved = true;
        for (int tick = 0; tick < 120; ++tick) {
            (void)StepGasProjection(box, state, options);
            conserved = conserved && ConservedAgainst(box, before);
        }

        const int64_t rise = HeatCentroidZ(box) - start;
        const bool pass = conserved && rise * 10 >= explicitRise * 9 && rise * 10 <= explicitRise * 11;
        std::printf("bet | bubble 2 s | %-9s | rise %lld (explicit %lld, %.1f%%) | conserved %d | %s\n", variant.name,
                    (long long)rise, (long long)explicitRise,
                    explicitRise == 0 ? 0.0 : 100.0 * (double)(rise - explicitRise) / (double)explicitRise,
                    conserved ? 1 : 0, pass ? "PASS" : "FAIL");
        return pass;
    }

    void RunBetStudy(int ticks, int explicitTicks) {
        GasBox bubble = MakeAirBox(MakeAirConfig(10, 10, 20));
        HeatBubble(bubble);
        const int64_t bubbleStart = HeatCentroidZ(bubble);
        for (int tick = 0; tick < 120; ++tick)
            StepGas(bubble);

        const int64_t explicitRise = HeatCentroidZ(bubble) - bubbleStart;
        int failures = 0;
        for (const BetVariant& variant : BET_VARIANTS)
            failures += ReportBetBubble(variant, explicitRise) ? 0 : 1;

        for (const BetScene& scene : MakeBetScenes()) {
            const BetRun reference = RunBet(scene.box, nullptr, explicitTicks, INT64_MAX, scene.region);
            std::printf(
                "bet | %-20s | explicit  | max speed %.2f m/s in %d ticks | conserved %d | %.0f ns/cell/tick | "
                "crossed 30: %lld end: %lld\n",
                scene.name.c_str(), (double)reference.maxSpeed / (double)METER_PER_SECOND, reference.ticks,
                reference.conserved ? 1 : 0, NanosecondsPerCellTick(reference, scene.box.cells.size()),
                (long long)reference.crossedEarly, (long long)reference.crossed);
            for (const BetVariant& variant : BET_VARIANTS)
                failures += ReportBet(scene, variant, ticks, reference) ? 0 : 1;

            // 対照(ADR-0346 の形): 最速・外側 4(既定)。細孔の板だけ
            if (scene.name.starts_with("plate 1/256")) {
                GasProjectionOptions fastest;
                const BetRun control = RunBet(scene.box, &fastest, std::min(ticks, 40), InitialSpread(scene.box) * 10,
                                              scene.region);
                std::printf(
                    "bet | %-20s | control fastest outer 4 (ADR-0346) | max|x|/dp %.2e (over at %d) | ticks %d\n",
                    scene.name.c_str(), (double)control.largest / (double)InitialSpread(scene.box), control.overTick,
                    control.ticks);
            }
        }

        std::printf("bet: %d case(s) not passed (research; not counted as test failures)\n", failures);
    }

}  // namespace

// 引数 --sweep で、外側の反復の止め方の違いを測る(遅い。ctest では回さない)
int main(int argc, char** argv) {
    const bool sweep = argc > 1 && std::string_view(argv[1]) == "--sweep";
    if (argc > 1 && std::string_view(argv[1]) == "--plates") {  // T-0788 の調べ(最速 × 小さい空きの板の形と空き)
        SweepPlates();
        std::printf("plates: %d failure(s)\n", failureCount);
        return failureCount == 0 ? 0 : 1;
    }

    if (argc > 1 && std::string_view(argv[1]) == "--t0842") {  // T-0842 の賭け(本物の音速の陰解法・V 固定 2・外側 0/1)
        const int ticks = argc > 2 ? std::atoi(argv[2]) : 300;
        const int explicitTicks = argc > 3 ? std::atoi(argv[3]) : 30;
        RunBetStudy(ticks, explicitTicks);
        return 0;
    }

    if (argc > 1 && std::string_view(argv[1]) == "--plate16") {  // T-0722 の調べ(細孔の板と孔の壁だけ)
        (void)RunPlate16(0, 4, true, false);                     // 前(切った流れを外側の反復の食い違いから除かない)

        TestPlate16();
        TestWall();
        std::printf("plate16: %d failure(s)\n", failureCount);
        return failureCount == 0 ? 0 : 1;
    }

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
        TestWall();
        TestPorous();
        TestPlate16();
        TestFastestCheck();
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
