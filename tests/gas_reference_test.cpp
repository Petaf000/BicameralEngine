// gas_reference_test.cpp — 気体の流れの CPU リファレンス G1・G2(sim/gas_reference。T-0026・T-0184・07 §2.1・§2.2)を CPU だけで確かめる。合格の条件:
//   閉じた箱で成分・エネルギーがビット一致で一定、運動量は「初め + 帳簿(壁と重力の力積)」とビット一致・周期的な箱(重力なし)で運動量がビット一致で一定・
//   静止大気が 600 刻み後も速度 1 単位(2^-20 m/s)以内・熱い泡が上がる・風で煙が流れる・同じ入力なら同じハッシュ。
//   G2: MUSCL で煙のにじみ(風に沿った広がり)が 1 次の風上より狭く、煙の割合が初めの最大を超えない(振動なし)・
//   微量の成分(1 セル 5 µmol)が風で流れる(切り捨てでは動かない)。
//   T-0226(本物の音速。D-440): 細い管の圧力の段差が 340 m/s で伝わる・強い段差は衝撃波になり等温の気体の理論の速さで進み、幅が広がらない・
//   1% のでたらめな乱れが 3 次元で育たない(安定の条件)・1 セルの 5 倍の圧力や 1500 K のセルで真空のセルができず保存が保たれる。
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

    // --- 静止大気: 600 刻み後も速度 1 単位以内(基準の状態では力も流れもちょうど 0 なので、刻みの数を増やしても同じ。
    //     T-0226 で小刻みが 6 倍になったので 10^4 刻みから減らした。長い時間の育ち方は乱した大気の 10^4 刻みで見る)---
    void TestStaticAtmosphere() {
        GasBox box = MakeAirBox(MakeAirConfig(4, 4, 16));
        const GasTotals before = SumGas(box);
        Run(box, 600);

        const int64_t speed = MaxSpeed(box);
        std::printf("static atmosphere: max speed after 600 ticks = %lld (2^-20 m/s)\n", (long long)speed);
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
        EXPECT(late <= early);  // 育たない(重さと音の波が組んだ振動は前進・後退の順で止めた)
        // 2.5 cm/s 以下(渦は粘性が無いので残る。07 §2.1)。本物の音速では同じ 1% の乱れの圧力の力が α 倍(落とした c̃ 30 m/s の約 128 倍)で、
        // 残る動きも大きい(測った値 約 1.4 cm/s。c̃ 30 m/s では約 2 mm/s だった。T-0226)
        EXPECT(late <= METER_PER_SECOND / 40);
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

    // --- T-0226: 本物の音速(D-440)。x に長い 1 セル角の管(壁・重力なし)で、左半分の圧力を上げた段差を放す ---

    constexpr uint32_t TUBE_LENGTH = 640;
    constexpr double CELL_METER = 0.5;
    constexpr double TICK_SECOND = 1.0 / 60.0;

    // 左半分を同じ温度のまま overPercent % 濃くする(圧力も同じ割合で上がる)
    GasBox MakePressureTube(uint32_t overPercent) {
        GasConfig config = MakeAirConfig(TUBE_LENGTH, 1, 1);
        config.gravity = false;
        GasBox box = MakeAirBox(config);
        for (uint32_t x = 0; x < TUBE_LENGTH / 2; ++x) {
            GasCell& cell = box.cells[x];
            for (uint32_t s = 0; s < config.speciesCount; ++s)
                cell.amounts[s] += cell.amounts[s] * overPercent / 100;

            cell.energy += cell.energy * overPercent / 100;
        }

        return box;
    }

    // 基準との圧力の差(µPa)
    double TubePressure(const GasBox& box, uint32_t x) {
        return (double)(DeriveGasCell(box, box.cells[x], 0).pressure - box.referenceDerived[0].pressure);
    }

    // 右へ進む波の前線で、圧力の差が level(段差の真ん中の圧力に対する割合)を横切る位置(セル。線形の内挿)
    double TubeCrossing(const GasBox& box, double level) {
        const double threshold = TubePressure(box, TUBE_LENGTH / 2) * level;
        for (uint32_t x = TUBE_LENGTH - 2; x > 0; --x) {
            const double here = TubePressure(box, x);
            if (here < threshold)
                continue;

            return x + ((here - threshold) / (here - TubePressure(box, x + 1)));
        }

        return 0;
    }

    struct TubeFront {
        double speed = 0;       // m/s(6 刻み目から 18 刻み目まで)
        double earlyWidth = 0;  // 前線の幅(段差の 10%〜90%。セル)、6 刻み目
        double lateWidth = 0;   // 18 刻み目
    };

    TubeFront MeasureTubeFront(uint32_t overPercent) {
        GasBox box = MakePressureTube(overPercent);
        const GasTotals before = SumGas(box);

        Run(box, 6);
        const double early = TubeCrossing(box, 0.5);
        TubeFront front;
        front.earlyWidth = TubeCrossing(box, 0.1) - TubeCrossing(box, 0.9);

        Run(box, 12);
        const double late = TubeCrossing(box, 0.5);
        front.lateWidth = TubeCrossing(box, 0.1) - TubeCrossing(box, 0.9);
        front.speed = (late - early) * CELL_METER / (12 * TICK_SECOND);

        const GasTotals after = SumGas(box);
        EXPECT(after.amounts == before.amounts);
        EXPECT(after.energy == before.energy);
        std::printf("tube +%u%%: front speed %.1f m/s, width %.2f -> %.2f cells\n", overPercent, front.speed,
                    front.earlyWidth, front.lateWidth);

        return front;
    }

    // 圧力の弱い段差は音の波: 前線が c̃ = 340 m/s で進む
    void TestSoundSpeed() {
        const TubeFront front = MeasureTubeFront(1);
        EXPECT(front.speed >= 340.0 * 0.98 && front.speed <= 340.0 * 1.02);
        EXPECT(front.lateWidth > front.earlyWidth * 1.2);  // 線形の波の前線は数値の拡散で広がる(衝撃波との比べ)
    }

    // 等温の気体(この気体は断熱の仕事を入れていない)の衝撃波管の理論: 中間の圧力 x = p* ÷ p_右 は
    // √x − 1/√x = ln(比 ÷ x)、衝撃波の速さ = c̃ √x(等温の Rankine–Hugoniot)
    double IsothermalShockSpeed(double pressureRatio) {
        double low = 1.0;
        double high = pressureRatio;
        for (int iteration = 0; iteration < 100; ++iteration) {
            const double middle = (low + high) / 2;
            if (std::sqrt(middle) - (1 / std::sqrt(middle)) > std::log(pressureRatio / middle))
                high = middle;
            else
                low = middle;
        }

        return 340.0 * std::sqrt(low);
    }

    // 強い段差は衝撃波になる: 音速より速く(理論どおり)進み、前線の幅が時間で広がらない(非線形の切り立ち)
    void TestShockForms() {
        for (const uint32_t overPercent : {100u, 400u}) {
            const TubeFront front = MeasureTubeFront(overPercent);
            const double expected = IsothermalShockSpeed(1.0 + (overPercent / 100.0));
            std::printf("  isothermal shock theory: %.1f m/s\n", expected);
            EXPECT(front.speed >= expected * 0.98 && front.speed <= expected * 1.02);
            EXPECT(front.lateWidth <= front.earlyWidth + 0.5);
            EXPECT(front.lateWidth <= 8.0);
        }
    }

    // 3 次元の安定の条件: 重力ありの閉じた 10³ の箱の全部のセルを ±1% でたらめに乱しても、音の波が育たない
    // (素の Rusanov の拡散〔÷ 2c̃〕・小刻み 32 では 1 秒で 40 m/s・真空のセルまで育った。T-0226)
    void TestNoiseStaysSmall() {
        GasBox box = MakeAirBox(MakeAirConfig(10, 10, 10));
        uint64_t state = 12345;
        for (GasCell& cell : box.cells) {
            state = (state * 6364136223846793005ull) + 1442695040888963407ull;
            const int64_t perMille = (int64_t)((state >> 33) % 21) - 10;  // −10〜+10(1/1000)
            for (uint32_t s = 0; s < box.config.speciesCount; ++s)
                cell.amounts[s] = (uint64_t)((int64_t)cell.amounts[s] + ((int64_t)cell.amounts[s] * perMille / 1000));

            cell.energy += cell.energy * perMille / 1000;
        }

        const GasTotals before = SumGas(box);
        Run(box, 300);
        const int64_t speed = MaxSpeed(box);
        std::printf("noise +-1%% in 10^3 box: max speed after 5 s = %lld (2^-20 m/s)\n", (long long)speed);
        EXPECT(speed <= METER_PER_SECOND / 10);
        EXPECT(SumGas(box).energy == before.energy);
    }

    // 最も軽いセルの質量(mg)
    uint64_t MinimumMass(const GasBox& box) {
        uint64_t lightest = UINT64_MAX;
        for (uint32_t index = 0; index < box.cells.size(); ++index)
            lightest = std::min(lightest, DeriveGasCell(box, box.cells[index], CellZ(box, index)).mass);

        return lightest;
    }

    // 1 セルだけ強い圧力(5 倍の物質量・1500 K)を放す: 真空のセルができず、保存が保たれ、2 秒後に落ち着く
    void TestStrongPressureStable() {
        for (const bool hot : {false, true}) {
            GasBox box = MakeAirBox(MakeAirConfig(12, 12, 12));
            const uint32_t center = box.Index(6, 6, 3);
            if (hot) {
                Heat(box, center, 1500000);
            } else {
                GasCell& cell = box.cells[center];
                for (uint32_t s = 0; s < box.config.speciesCount; ++s)
                    cell.amounts[s] *= 5;

                cell.energy *= 5;
            }

            const GasTotals before = SumGas(box);
            uint64_t lightest = UINT64_MAX;
            for (uint32_t tick = 0; tick < 120; ++tick) {
                StepGas(box);
                lightest = std::min(lightest, MinimumMass(box));
            }

            const GasTotals after = SumGas(box);
            const int64_t speed = MaxSpeed(box);
            std::printf("strong pressure (%s): lightest cell %llu mg, max speed after 2 s = %lld (2^-20 m/s)\n",
                        hot ? "1500 K" : "x5 amount", (unsigned long long)lightest, (long long)speed);
            EXPECT(after.amounts == before.amounts);
            EXPECT(after.energy == before.energy);
            // 真空のセルができない(不安定なら 0 まで抜けた)。熱いセルは膨らんで軽くなる(圧力が釣り合えば約 1/5 = 30000 mg)
            EXPECT(lightest >= (uint64_t)ROUGH_CELL_MASS / 8);
            EXPECT(speed <= 10 * METER_PER_SECOND);
        }
    }

}  // namespace

int main() {
    TestClosedBoxConservation();
    TestPeriodicMomentum();
    TestStaticAtmosphere();
    TestDisturbedAtmosphereSettles();
    TestHotBubbleRises(340000, 24);  // 既定(本物の音速。D-440)
    TestHotBubbleRises(30000, 4);    // 落とした c̃(G1 の仮の値)でもほぼ同じ速さで上がる(遅い流れは音速によらない)
    TestWindCarriesSmoke();
    TestMusclNarrowsSmoke();
    TestTraceSpeciesMoves();
    TestSoundSpeed();
    TestShockForms();
    TestNoiseStaysSmall();
    TestStrongPressureStable();

    if (failureCount != 0) {
        std::printf("gas_reference_test: %d failure(s)\n", failureCount);
        return 1;
    }

    std::printf("gas_reference_test: OK\n");
    return 0;
}
