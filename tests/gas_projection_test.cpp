// gas_projection_test.cpp — 気体の圧力を刻みの中で解く投影の CPU 模型(sim/gas_projection。T-0503 研究・07 §2.6)を CPU だけで確かめ、測る。
// 合格の条件(研究の打ち切り条件。docs/tickets/T-0503-pressure-projection-model.md):
//   静止大気が 600 刻みで速度がちょうど 0・閉じた箱で成分がビット一致で一定、エネルギーと運動量は毎刻み「初め + 帳簿」とビット一致・
//   同じ入力なら同じハッシュ・熱い泡(gas_reference と同じ場面)の熱の重心が 2 秒で gas_reference(1.32 セル)の 1/3〜3 倍上がる。
// 測る値(表示だけ): 1 刻みの V サイクルの回数(音速 340 m/s・3400 m/s・最速)と、爆発(1 セル 5 倍の圧力・1500 K・煙の印)で
//   圧力の前線・熱い所・煙の広がりが刻みごとにどう見えるか(gas_reference の小刻み 24 と比べる。D-487 ⑥)。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

        void Add(const GasProjectionCost& cost) {
            cycles += cost.cycles;
            maxCycles = std::max(maxCycles, cost.maxCycles);
            unconverged += cost.unconverged;
            levels = cost.gridLevels;
            worstError = std::max(worstError, cost.worstMassError);
            ++ticks;
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

}  // namespace

int main() {
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

    if (failureCount != 0) {
        std::printf("%d failure(s)\n", failureCount);
        return 1;
    }

    std::printf("all passed\n");
    return 0;
}
