// gas_reference_test.cpp — 気体の流れの CPU リファレンス G1・G2(sim/gas_reference。T-0026・T-0184・07 §2.1・§2.2)を CPU だけで確かめる。合格の条件:
//   閉じた箱で成分・エネルギーがビット一致で一定、運動量は「初め + 帳簿(壁と重力の力積)」とビット一致・周期的な箱(重力なし)で運動量がビット一致で一定・
//   静止大気が 10^4 刻み後も速度 1 単位(2^-20 m/s)以内・熱い泡が上がる・風で煙が流れる・同じ入力なら同じハッシュ。
//   G2: MUSCL で煙のにじみ(風に沿った広がり)が 1 次の風上より狭く、煙の割合が初めの最大を超えない(振動なし)・
//   微量の成分(1 セル 5 µmol)が風で流れる(切り捨てでは動かない)。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。測った値も表示する(07 §2.1 に書いた数字の出どころ)。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "sim/gas_reference.h"

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

    // --- 試験の空気(N2 79% + O2 21%)と煙の印(N2 と同じ値の別の物質。流れには影響せず、運ばれるだけ)---
    constexpr uint32_t SPECIES_N2 = 0;
    constexpr uint32_t SPECIES_SMOKE = 2;
    constexpr std::array<uint32_t, 3> AIR_FRACTIONS_Q32 = {3393024163u, 901943132u, 0u};
    constexpr uint32_t AIR_TEMPERATURE = 293150;                   // mK
    constexpr uint64_t SURFACE_PRESSURE = 101325ull * 1000000ull;  // µPa
    constexpr int64_t METER_PER_SECOND = (int64_t)1 << 20;         // 速度の単位
    constexpr int64_t ROUGH_CELL_MASS = 150000;  // 地面の近くの 1 セルの空気の質量(mg。運動量を速さから作る目安)

    GasConfig MakeAirConfig(uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ) {
        GasConfig config;
        config.sizeX = sizeX;
        config.sizeY = sizeY;
        config.sizeZ = sizeZ;
        config.speciesCount = 3;
        config.species[0] = {.molarMass = 28013, .heatCapacity = 29124, .formationEnergy = 0};  // N2
        config.species[1] = {.molarMass = 31999, .heatCapacity = 29378, .formationEnergy = 0};  // O2
        config.species[2] = {.molarMass = 28013, .heatCapacity = 29124, .formationEnergy = 0};  // 煙の印

        return config;
    }

    GasBox MakeAirBox(const GasConfig& config) {
        return MakeGasBox(config,
                          MakeHydrostaticReference(config, AIR_FRACTIONS_Q32, AIR_TEMPERATURE, SURFACE_PRESSURE));
    }

    // セルの温度を変える(物質量はそのまま)
    void Heat(GasBox& box, uint32_t index, uint32_t temperature) {
        const GasCell old = box.cells[index];
        GasCell cell = MakeGasCell(box.config, old.amounts, temperature);
        cell.momentum = old.momentum;
        box.cells[index] = cell;
    }

    // N2 の半分を煙の印に替える
    void AddSmoke(GasBox& box, uint32_t index) {
        GasCell& cell = box.cells[index];
        const uint64_t half = cell.amounts[SPECIES_N2] / 2;
        cell.amounts[SPECIES_N2] -= half;
        cell.amounts[SPECIES_SMOKE] += half;
    }

    int64_t MaxSpeed(const GasBox& box) {
        int64_t speed = 0;
        for (const GasCell& cell : box.cells) {
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                const int64_t velocity = GasVelocity(box, cell, axis);
                speed = std::max(speed, velocity < 0 ? -velocity : velocity);
            }
        }

        return speed;
    }

    uint32_t CellX(const GasBox& box, uint32_t index) {
        return index % box.config.sizeX;
    }

    uint32_t CellZ(const GasBox& box, uint32_t index) {
        return index / (box.config.sizeX * box.config.sizeY);
    }

    void Run(GasBox& box, uint64_t ticks) {
        for (uint64_t tick = 0; tick < ticks; ++tick)
            StepGas(box);
    }

    // --- 閉じた箱: 成分・エネルギーは一定、運動量は初め + 帳簿 ---
    GasBox MakeDisturbedClosedBox() {
        GasBox box = MakeAirBox(MakeAirConfig(8, 8, 8));
        Heat(box, box.Index(3, 3, 1), AIR_TEMPERATURE + 50000);
        Heat(box, box.Index(4, 3, 1), AIR_TEMPERATURE + 50000);
        AddSmoke(box, box.Index(5, 5, 5));

        GasCell& moving = box.cells[box.Index(2, 6, 4)];
        moving.momentum[0] = ROUGH_CELL_MASS * 2 * METER_PER_SECOND;
        moving.momentum[2] = -ROUGH_CELL_MASS * METER_PER_SECOND;

        return box;
    }

    void TestClosedBoxConservation() {
        GasBox box = MakeDisturbedClosedBox();
        const GasTotals before = SumGas(box);
        Run(box, 200);
        const GasTotals after = SumGas(box);

        for (uint32_t s = 0; s < box.config.speciesCount; ++s) {
            EXPECT(after.amounts[s] == before.amounts[s]);
            EXPECT(box.ledger.amounts[s] == 0);
        }

        EXPECT(after.energy == before.energy);
        EXPECT(box.ledger.energy == 0);
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
            EXPECT(after.momentum[axis] == before.momentum[axis] + box.ledger.momentum[axis]);

        // 同じ入力なら同じ状態(決定性)
        GasBox again = MakeDisturbedClosedBox();
        Run(again, 200);
        EXPECT(HashGas(again) == HashGas(box));
        std::printf("closed box: max speed after 200 ticks = %lld (2^-20 m/s)\n", (long long)MaxSpeed(box));
    }

    // --- 周期的な箱(重力なし): 外との出入りが無いので運動量もそのまま一定 ---
    void TestPeriodicMomentum() {
        GasConfig config = MakeAirConfig(8, 8, 4);
        config.boundary = {GasBoundary::Periodic, GasBoundary::Periodic, GasBoundary::Periodic};
        config.gravity = false;

        std::vector<GasCell> reference = MakeHydrostaticReference(config, AIR_FRACTIONS_Q32, AIR_TEMPERATURE,
                                                                  SURFACE_PRESSURE);
        std::fill(reference.begin() + 1, reference.end(), reference.front());
        GasBox box = MakeGasBox(config, reference);
        Heat(box, box.Index(1, 2, 1), AIR_TEMPERATURE + 80000);
        box.cells[box.Index(5, 5, 2)].momentum = {ROUGH_CELL_MASS * 3 * METER_PER_SECOND,
                                                  -ROUGH_CELL_MASS * METER_PER_SECOND,
                                                  ROUGH_CELL_MASS * 2 * METER_PER_SECOND};
        AddSmoke(box, box.Index(6, 1, 0));

        const GasTotals before = SumGas(box);
        Run(box, 200);
        const GasTotals after = SumGas(box);

        for (uint32_t s = 0; s < config.speciesCount; ++s)
            EXPECT(after.amounts[s] == before.amounts[s]);

        EXPECT(after.energy == before.energy);
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
            EXPECT(after.momentum[axis] == before.momentum[axis]);
            EXPECT(box.ledger.momentum[axis] == 0);
        }
    }

    // --- 静止大気: 10^4 刻み後も速度 1 単位以内 ---
    void TestStaticAtmosphere() {
        GasBox box = MakeAirBox(MakeAirConfig(4, 4, 16));
        const GasTotals before = SumGas(box);
        Run(box, 10000);

        const int64_t speed = MaxSpeed(box);
        std::printf("static atmosphere: max speed after 10^4 ticks = %lld (2^-20 m/s)\n", (long long)speed);
        EXPECT(speed <= 1);
        EXPECT(SumGas(box).energy == before.energy);
    }

    // --- 乱した大気が落ち着く(音の波が減衰し、増えない)---
    void TestDisturbedAtmosphereSettles() {
        GasBox box = MakeAirBox(MakeAirConfig(4, 4, 16));
        GasCell& cell = box.cells[box.Index(1, 2, 7)];
        for (uint32_t s = 0; s < box.config.speciesCount; ++s)
            cell.amounts[s] += cell.amounts[s] / 100;  // 同じ温度のまま 1% 濃く(圧力だけの乱れ。浮力は残らない)

        cell.energy += cell.energy / 100;

        int64_t early = 0;
        for (uint32_t tick = 0; tick < 120; ++tick) {
            StepGas(box);
            early = std::max(early, MaxSpeed(box));
        }

        Run(box, 10000 - 120);
        const int64_t late = MaxSpeed(box);
        std::printf("disturbed atmosphere: max speed in first 2 s = %lld, at 10^4 ticks = %lld (2^-20 m/s)\n",
                    (long long)early, (long long)late);
        EXPECT(late <= early);                   // 育たない(重さと音の波が組んだ振動は前進・後退の順で止めた)
        EXPECT(late <= METER_PER_SECOND / 200);  // 5 mm/s 以下(渦は粘性が無いので残る。07 §2.1)
    }

    // 基準より熱い分(mK)で重みを付けた高さの平均(セルの単位 × 1000)
    int64_t HeatCentroidZ(const GasBox& box) {
        int64_t weighted = 0;
        int64_t total = 0;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const uint32_t z = CellZ(box, index);
            const int64_t excess = (int64_t)DeriveGasCell(box, box.cells[index], z).temperature -
                                   (int64_t)box.referenceDerived[z].temperature;
            if (excess <= 0)
                continue;

            weighted += excess * z;
            total += excess;
        }

        return total == 0 ? 0 : weighted * 1000 / total;
    }

    // 半径 1.5 セル(0.75 m)の球(中心 (5, 5, 4))を +30 K
    void HeatBubble(GasBox& box) {
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const int32_t dx = (int32_t)CellX(box, index) - 5;
            const int32_t dy = (int32_t)((index / box.config.sizeX) % box.config.sizeY) - 5;
            const int32_t dz = (int32_t)CellZ(box, index) - 4;
            if (dx * dx + dy * dy + dz * dz <= 2)
                Heat(box, index, AIR_TEMPERATURE + 30000);
        }
    }

    // --- 熱い泡が上がる ---
    void TestHotBubbleRises(uint32_t soundSpeedMmPerS, uint32_t substeps) {
        GasConfig config = MakeAirConfig(10, 10, 20);
        config.soundSpeedMmPerS = soundSpeedMmPerS;
        config.substeps = substeps;
        GasBox box = MakeAirBox(config);
        HeatBubble(box);

        const GasTotals before = SumGas(box);
        const int64_t start = HeatCentroidZ(box);
        Run(box, 120);
        const int64_t end = HeatCentroidZ(box);
        const int64_t upward = GasVelocity(box, box.cells[box.Index(5, 5, 6)], 2);
        std::printf(
            "hot bubble (c~ %u mm/s, %u substeps): heat centroid z %lld -> %lld (1/1000 cell) after 2 s, w = %lld\n",
            soundSpeedMmPerS, substeps, (long long)start, (long long)end, (long long)upward);
        EXPECT(end - start >= 1000);  // 2 秒で 0.5 m 以上(熱の重心。泡の後ろに残る熱も数える)
        EXPECT(upward >= METER_PER_SECOND /
                             2);  // 泡の上で 0.5 m/s 以上(半径 0.75 m・+30 K の終端速度の目安 √(g ΔT/T r) ≈ 0.85 m/s)
        EXPECT(SumGas(box).energy == before.energy);
    }

    // 煙の印の x の平均(セルの単位 × 1000)
    int64_t SmokeCentroidX(const GasBox& box) {
        uint64_t total = 0;
        uint64_t weighted = 0;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const uint64_t smoke = box.cells[index].amounts[SPECIES_SMOKE];
            weighted += smoke * CellX(box, index);
            total += smoke;
        }

        return total == 0 ? 0 : (int64_t)(weighted * 1000 / total);
    }

    // --- 風で煙が流れる(x は開いた境界で風 2 m/s、y は周期的、z は壁)---
    void TestWindCarriesSmoke() {
        GasConfig config = MakeAirConfig(32, 4, 6);
        config.boundary = {GasBoundary::Open, GasBoundary::Periodic, GasBoundary::Wall};
        config.windVelocity = {(int32_t)(2 * METER_PER_SECOND), 0, 0};
        GasBox box = MakeAirBox(config);

        for (uint32_t z = 2; z <= 3; ++z) {
            for (uint32_t y = 0; y < config.sizeY; ++y) {
                for (uint32_t x = 4; x <= 5; ++x)
                    AddSmoke(box, box.Index(x, y, z));
            }
        }

        const uint64_t smokeBefore = SumGas(box).amounts[SPECIES_SMOKE];
        const int64_t start = SmokeCentroidX(box);
        Run(box, 60);
        const int64_t end = SmokeCentroidX(box);
        std::printf("wind 2 m/s: smoke centroid x %lld -> %lld (1/1000 cell) after 1 s (expected +4000)\n",
                    (long long)start, (long long)end);
        EXPECT(end - start >= 3600 && end - start <= 4400);
        EXPECT(SumGas(box).amounts[SPECIES_SMOKE] == smokeBefore);  // まだ出口に届かない

        // 一様な風そのものは崩れない(±0.1%。煙の印の質量の丸め〔1 mg〕が作る音の波の分だけ揺れる)
        const int64_t velocity = GasVelocity(box, box.cells[box.Index(20, 1, 1)], 0);
        std::printf("wind 2 m/s: velocity downstream = %lld (2^-20 m/s)\n", (long long)velocity);
        EXPECT(velocity >= 2 * METER_PER_SECOND - 2 * METER_PER_SECOND / 1000 &&
               velocity <= 2 * METER_PER_SECOND + 2 * METER_PER_SECOND / 1000);
    }

    // --- G2: 煙のにじみ(MUSCL)と微量の成分(乱数の丸め)。T-0184・07 §2.2 ---

    // x は開いた境界で風 2 m/s、y は周期的、z は壁の箱
    GasBox MakeWindBox(uint32_t sizeX, GasReconstruction reconstruction, bool stochasticRounding) {
        GasConfig config = MakeAirConfig(sizeX, 4, 6);
        config.boundary = {GasBoundary::Open, GasBoundary::Periodic, GasBoundary::Wall};
        config.windVelocity = {(int32_t)(2 * METER_PER_SECOND), 0, 0};
        config.reconstruction = reconstruction;
        config.stochasticRounding = stochasticRounding;

        return MakeAirBox(config);
    }

    struct SmokeProfile {
        double centroid = 0;  // セル
        double spread = 0;    // 風に沿った標準偏差(セル)
        double maxRatio = 0;  // セルの煙の割合(煙 ÷ 全物質量)の最大
    };

    SmokeProfile MeasureSmoke(const GasBox& box) {
        double total = 0;
        double first = 0;
        double second = 0;
        SmokeProfile profile;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const GasCell& cell = box.cells[index];
            const auto smoke = (double)cell.amounts[SPECIES_SMOKE];
            const double x = CellX(box, index);
            total += smoke;
            first += smoke * x;
            second += smoke * x * x;

            const auto amount = (double)(cell.amounts[0] + cell.amounts[1] + cell.amounts[2]);
            profile.maxRatio = std::max(profile.maxRatio, smoke / amount);
        }

        profile.centroid = first / total;
        profile.spread = std::sqrt(std::max(0.0, (second / total) - (profile.centroid * profile.centroid)));

        return profile;
    }

    // 4 セルの幅の煙(全部の y・z = 2〜3)を 3 秒流し、風に沿った広がりを測る。広がりを返す
    double SmokeSpreadAfterWind(GasReconstruction reconstruction, const char* name) {
        GasBox box = MakeWindBox(48, reconstruction, true);
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const uint32_t x = CellX(box, index);
            const uint32_t z = CellZ(box, index);
            if (x >= 4 && x <= 7 && z >= 2 && z <= 3)
                AddSmoke(box, index);
        }

        const uint64_t smokeBefore = SumGas(box).amounts[SPECIES_SMOKE];
        const SmokeProfile start = MeasureSmoke(box);
        Run(box, 60);
        const SmokeProfile oneSecond = MeasureSmoke(box);
        Run(box, 120);
        const SmokeProfile threeSeconds = MeasureSmoke(box);
        std::printf(
            "smoke %s: spread %.3f -> %.3f (1 s) -> %.3f (3 s) cells, centroid %.3f -> %.3f, "
            "max ratio %.4f -> %.4f\n",
            name, start.spread, oneSecond.spread, threeSeconds.spread, start.centroid, threeSeconds.centroid,
            start.maxRatio, threeSeconds.maxRatio);

        EXPECT(SumGas(box).amounts[SPECIES_SMOKE] == smokeBefore);  // まだ出口に届かない
        // 3 秒で 6 m(12 セル)流れ、煙の割合は初めの最大を超えない(新しい山を作らない = 振動なし。R-TRANS-2)
        const double shift = threeSeconds.centroid - start.centroid;
        EXPECT(shift >= 11.0 && shift <= 13.0);
        EXPECT(threeSeconds.maxRatio <= start.maxRatio * 1.0001);

        return threeSeconds.spread;
    }

    void TestMusclNarrowsSmoke() {
        const double upwind = SmokeSpreadAfterWind(GasReconstruction::Upwind, "upwind");
        const double minmod = SmokeSpreadAfterWind(GasReconstruction::Minmod, "minmod");
        const double central = SmokeSpreadAfterWind(GasReconstruction::MonotonizedCentral, "MC");
        EXPECT(minmod < upwind);
        EXPECT(central <= upwind * 0.6);  // 測った値は 07 §2.2
    }

    // 1 セルに 5 µmol の微量の成分(全部の y・x = 4・z = 2〜3)を 1 秒流す。割合 × 物質量 は 1 小刻みに約 0.08 µmol で、
    // 切り捨てでは永久に 0(D-428 の嘘)。乱数の丸めなら期待値どおり風で流れる。重心の移動(1/1000 セル)を返す
    int64_t TraceShiftAfterWind(bool stochasticRounding) {
        GasBox box = MakeWindBox(32, GasReconstruction::MonotonizedCentral, stochasticRounding);
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const uint32_t z = CellZ(box, index);
            if (CellX(box, index) == 4 && z >= 2 && z <= 3)
                box.cells[index].amounts[SPECIES_SMOKE] = 5;
        }

        const uint64_t traceBefore = SumGas(box).amounts[SPECIES_SMOKE];
        const int64_t start = SmokeCentroidX(box);
        Run(box, 60);
        const int64_t shift = SmokeCentroidX(box) - start;
        std::printf("trace 5 umol/cell (%s rounding): centroid shift %lld (1/1000 cell) after 1 s (wind: +4000)\n",
                    stochasticRounding ? "stochastic" : "floor", (long long)shift);
        EXPECT(SumGas(box).amounts[SPECIES_SMOKE] == traceBefore);

        return shift;
    }

    void TestTraceSpeciesMoves() {
        const int64_t truncated = TraceShiftAfterWind(false);
        const int64_t stochastic = TraceShiftAfterWind(true);
        EXPECT(truncated == 0);  // G1 の切り捨ての嘘を再現できていること(測り方の確認)
        EXPECT(stochastic >= 3000 && stochastic <= 5000);
    }

}  // namespace

int main() {
    TestClosedBoxConservation();
    TestPeriodicMomentum();
    TestStaticAtmosphere();
    TestDisturbedAtmosphereSettles();
    TestHotBubbleRises(30000, 4);
    TestHotBubbleRises(60000, 8);  // c̃ は設定で変えられる(本物の音速の小刻みへ切り替える時の形)
    TestWindCarriesSmoke();
    TestMusclNarrowsSmoke();
    TestTraceSpeciesMoves();

    if (failureCount != 0) {
        std::printf("gas_reference_test: %d failure(s)\n", failureCount);
        return 1;
    }

    std::printf("gas_reference_test: OK\n");
    return 0;
}
