// gas_reference_test.cpp — 気体の流れの CPU リファレンス G1・G2(sim/gas_reference。T-0026・T-0184・07 §2.1・§2.2)を CPU だけで確かめる。合格の条件:
//   閉じた箱で成分・エネルギーがビット一致で一定、運動量は「初め + 帳簿(壁と重力の力積)」とビット一致・周期的な箱(重力なし)で運動量がビット一致で一定・
//   静止大気が 600 刻み後も速度 1 単位(2^-20 m/s)以内・熱い泡が上がる・風で煙が流れる・同じ入力なら同じハッシュ。
//   G2: MUSCL で煙のにじみ(風に沿った広がり)が 1 次の風上より狭く、煙の割合が初めの最大を超えない(振動なし)・
//   微量の成分(1 セル 5 µmol)が風で流れる(切り捨てでは動かない)。
//   T-0226(本物の音速。D-440): 細い管の圧力の段差が 340 m/s で伝わる・強い段差は衝撃波になり等温の気体の理論の速さで進み、幅が広がらない・
//   1% のでたらめな乱れが 3 次元で育たない(安定の条件)・1 セルの 5 倍の圧力や 1500 K のセルで真空のセルができず保存が保たれる。
//   T-0234(断熱の仕事 pdV・運動エネルギーの散逸を熱へ): 全エネルギー(熱 + 化学 + 運動)が毎刻み「初め + 帳簿(重さの仕事)」とビット一致・
//   弱い圧縮の波と膨張の波の温度が T ∝ p^((γ−1)/γ)・強い段差の衝撃波の速さと後ろの温度が Rankine–Hugoniot・膨張の波の温度が断熱の理論・
//   動きが止まると失った運動エネルギーが熱になる。等温の形(T-0226 まで)も設定で残し、等温の衝撃波の試験で確かめる。
//   T-0269(D-461): 熱いセルの音速の頭打ちの温度が安定の上限(ν = 0.6)の約 473 K・既定の設定(頭打ちあり)のまま衝撃波が理論どおり・
//   1500 K のセルと 1500 K の 5 倍の物質量の爆発で真空のセルができず保存が保たれる。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。測った値も表示する(07 §2.1 に書いた数字の出どころ)。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

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
        // 比熱は定積の c_v(定圧 29.124・29.378 J/(mol·K) − R。断熱では押し出す仕事が R の分を足す。T-0234)
        config.species[0] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};  // N2
        config.species[1] = {.molarMass = 31999, .heatCapacity = 21064, .formationEnergy = 0};  // O2
        config.species[2] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};  // 煙の印

        ApplyWorldPhysics(StandardWorldPhysics(), config);  // 標準の重力の世界(D-480。器の既定は 0)

        return config;
    }

    // 重力を変えた世界(T-0423・D-480)。月の重力 1.62 m/s²
    constexpr uint32_t MOON_GRAVITY = 1620000;

    GasConfig WithGravity(GasConfig config, uint32_t gravity) {
        WorldPhysics world = StandardWorldPhysics();  // 音速の基準は標準のまま、重力だけ替える
        world.gravityMicrometersPerSecond2 = gravity;
        ApplyWorldPhysics(world, config);
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
    GasBox MakeDisturbedClosedBox(uint32_t gravity = STANDARD_GRAVITY_MICROMETERS_PER_SECOND2) {
        GasBox box = MakeAirBox(WithGravity(MakeAirConfig(8, 8, 8), gravity));
        Heat(box, box.Index(3, 3, 1), AIR_TEMPERATURE + 50000);
        Heat(box, box.Index(4, 3, 1), AIR_TEMPERATURE + 50000);
        AddSmoke(box, box.Index(5, 5, 5));

        GasCell& moving = box.cells[box.Index(2, 6, 4)];
        SetGasMomentum(box, moving, {ROUGH_CELL_MASS * 2 * METER_PER_SECOND, 0, -ROUGH_CELL_MASS * METER_PER_SECOND});

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

        EXPECT(after.energy == before.energy + box.ledger.energy);  // 断熱では帳簿は重さの仕事だけ(T-0234)
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
        SetGasMomentum(box, box.cells[box.Index(5, 5, 2)],
                       {ROUGH_CELL_MASS * 3 * METER_PER_SECOND, -ROUGH_CELL_MASS * METER_PER_SECOND,
                        ROUGH_CELL_MASS * 2 * METER_PER_SECOND});
        AddSmoke(box, box.Index(6, 1, 0));

        const GasTotals before = SumGas(box);
        Run(box, 200);
        const GasTotals after = SumGas(box);

        for (uint32_t s = 0; s < config.speciesCount; ++s)
            EXPECT(after.amounts[s] == before.amounts[s]);

        EXPECT(after.energy == before.energy);
        EXPECT(box.ledger.energy == 0);
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
        EXPECT(box.ledger.energy == 0);
    }

    // 試験の空気の比熱比 γ = (c_v + R)÷ c_v(物質量で重み付けた c_v)と断熱の音速 √(γ R T ÷ M)(m/s)
    double AirGamma(const GasConfig& config) {
        const double capacity = (0.79 * config.species[0].heatCapacity) + (0.21 * config.species[1].heatCapacity);
        return (capacity + 8314.462618) / capacity;
    }

    double AirSoundSpeed(const GasConfig& config) {
        const double molarMass = ((0.79 * config.species[0].molarMass) + (0.21 * config.species[1].molarMass)) * 1e-6;
        return std::sqrt(AirGamma(config) * 8.314462618 * (AIR_TEMPERATURE / 1000.0) / molarMass);
    }

    // --- 乱した大気が落ち着く(音の波が減衰し、増えない)---
    void TestDisturbedAtmosphereSettles() {
        GasBox box = MakeAirBox(MakeAirConfig(4, 4, 16));
        // 1% 濃く、断熱の圧縮と同じだけ温める(等エントロピーの乱れ: 膨らんで周りの圧力に戻ると周りと同じ温度になり、浮力は残らない。
        // 同じ温度のまま濃くすると、膨らんで冷えた分〔約 0.8 K〕が重く沈み、成層の中で浮力の振動〔周期 約 6 分〕を続ける。本物の振る舞い)
        GasCell& cell = box.cells[box.Index(1, 2, 7)];
        std::array<uint64_t, GAS_MAX_SPECIES> amounts = cell.amounts;
        for (uint32_t s = 0; s < box.config.speciesCount; ++s)
            amounts[s] += amounts[s] / 100;

        const double heating = 1.0 + ((AirGamma(box.config) - 1.0) / 100.0);
        cell = MakeGasCell(box.config, amounts, (uint32_t)std::lround(AIR_TEMPERATURE * heating));

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

    // 半径 1.5 セル(0.75 m)の球(中心 (5, 5, 4))を同じ圧力のまま +30 K(物質量を T_前 ÷ T_後 倍。T-0234: 同じ密度のまま温めると
    // 圧力が 1 割高く、断熱では膨らんで約 9 K 冷えてしまう)
    void HeatBubble(GasBox& box) {
        constexpr uint32_t HOT = AIR_TEMPERATURE + 30000;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const int32_t dx = (int32_t)CellX(box, index) - 5;
            const int32_t dy = (int32_t)((index / box.config.sizeX) % box.config.sizeY) - 5;
            const int32_t dz = (int32_t)CellZ(box, index) - 4;
            if (dx * dx + dy * dy + dz * dz > 2)
                continue;

            std::array<uint64_t, GAS_MAX_SPECIES> amounts = box.cells[index].amounts;
            for (uint64_t& amount : amounts)
                amount = amount * AIR_TEMPERATURE / HOT;

            box.cells[index] = MakeGasCell(box.config, amounts, HOT);
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
        EXPECT(SumGas(box).energy == before.energy + box.ledger.energy);
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
    GasBox MakePressureTube(uint32_t overPercent, GasThermodynamics thermodynamics, uint32_t soundSpeedMmPerS) {
        GasConfig config = MakeAirConfig(TUBE_LENGTH, 1, 1);
        config.gravity = false;
        config.thermodynamics = thermodynamics;
        config.soundSpeedMmPerS = soundSpeedMmPerS;
        // 頭打ちは既定のまま(D-461・T-0269。約 463〜473 K から上。5 倍の段差でも衝撃波の後ろは 368 K なので掛からない)

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

    // 段差の位置からの速さ(m/s)の範囲 [from, to] の真ん中 6 割のセルの平均の温度(K)。時刻は 18 刻み目
    double TubeTemperature(const GasBox& box, double fromSpeed, double toSpeed) {
        const double seconds = 18 * TICK_SECOND;
        const double from = (TUBE_LENGTH / 2.0) + (fromSpeed * seconds / CELL_METER);
        const double to = (TUBE_LENGTH / 2.0) + (toSpeed * seconds / CELL_METER);
        const auto first = (uint32_t)std::lround(from + (0.2 * (to - from)));
        const auto last = (uint32_t)std::lround(from + (0.8 * (to - from)));
        double sum = 0;
        for (uint32_t x = first; x <= last; ++x)
            sum += DeriveGasCell(box, box.cells[x], 0).temperature / 1000.0;

        return sum / (last - first + 1);
    }

    struct TubeFront {
        double speed = 0;       // m/s(6 刻み目から 18 刻み目まで)
        double earlyWidth = 0;  // 前線の幅(段差の 10%〜90%。セル)、6 刻み目
        double lateWidth = 0;   // 18 刻み目
    };

    // 18 刻み流して前線を測る(box は 18 刻み目の状態を返す)
    TubeFront MeasureTubeFront(GasBox& box, uint32_t overPercent) {
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
        EXPECT(box.ledger.energy == 0);
        std::printf("tube +%u%% (%s): front speed %.1f m/s, width %.2f -> %.2f cells\n", overPercent,
                    box.config.thermodynamics == GasThermodynamics::Adiabatic ? "adiabatic" : "isothermal", front.speed,
                    front.earlyWidth, front.lateWidth);

        return front;
    }

    // 断熱の気体(比熱比 γ・音速 c)の衝撃波管の理論(厳密な Riemann の解。Toro 4 章)。左右は同じ温度、左の圧力が ratio 倍
    struct RiemannSolution {
        double shockSpeed = 0;           // m/s
        double contactSpeed = 0;         // 接触面(左右の気体の境)
        double tailSpeed = 0;            // 膨張の波の後ろの端
        double shockedTemperature = 0;   // 衝撃波の後ろの温度 ÷ 初めの温度(Rankine–Hugoniot)
        double expandedTemperature = 0;  // 膨張の波の後ろの温度 ÷ 初めの温度(断熱: (p* ÷ p_左)^((γ−1)/γ))
    };

    RiemannSolution SolveRiemann(double ratio, double gamma, double sound) {
        // 圧力の単位は p_右 = c² ÷ γ(密度 1)。左は密度も ratio 倍
        const double pressureRight = sound * sound / gamma;
        const double pressureLeft = pressureRight * ratio;
        auto wave = [&](double pressure, double side, double density) {
            if (pressure > side) {
                const double a = 2 / ((gamma + 1) * density);
                const double b = (gamma - 1) / (gamma + 1) * side;
                return (pressure - side) * std::sqrt(a / (pressure + b));
            }

            return 2 * sound / (gamma - 1) * (std::pow(pressure / side, (gamma - 1) / (2 * gamma)) - 1);
        };

        double low = 0;
        double high = pressureLeft;
        for (int iteration = 0; iteration < 200; ++iteration) {
            const double middle = (low + high) / 2;
            if (wave(middle, pressureLeft, ratio) + wave(middle, pressureRight, 1) > 0)
                high = middle;
            else
                low = middle;
        }

        const double star = low;
        const double shockRatio = star / pressureRight;
        RiemannSolution solution;
        solution.contactSpeed = 0.5 * (wave(star, pressureRight, 1) - wave(star, pressureLeft, ratio));
        solution.shockSpeed = sound * std::sqrt(((gamma + 1) / (2 * gamma) * shockRatio) + ((gamma - 1) / (2 * gamma)));
        const double compression = (shockRatio + ((gamma - 1) / (gamma + 1))) /
                                   (((gamma - 1) / (gamma + 1) * shockRatio) + 1);
        solution.shockedTemperature = shockRatio / compression;
        solution.expandedTemperature = std::pow(star / pressureLeft, (gamma - 1) / gamma);
        solution.tailSpeed = solution.contactSpeed - (sound * std::pow(star / pressureLeft, (gamma - 1) / (2 * gamma)));

        return solution;
    }

    // 衝撃波の後ろ(接触面〜衝撃波)と膨張の波の後ろ(膨張の波の端〜接触面)の温度を理論と比べる(差は初めとの差の割合)
    void ExpectTubeTemperatures(const GasBox& box, const RiemannSolution& theory, double tolerance) {
        const double initial = AIR_TEMPERATURE / 1000.0;
        const double shocked = TubeTemperature(box, theory.contactSpeed, theory.shockSpeed);
        const double expanded = TubeTemperature(box, theory.tailSpeed, theory.contactSpeed);
        const double shockedTheory = initial * theory.shockedTemperature;
        const double expandedTheory = initial * theory.expandedTemperature;
        std::printf("  behind shock %.3f K (theory %.3f K), behind expansion %.3f K (theory %.3f K)\n", shocked,
                    shockedTheory, expanded, expandedTheory);
        EXPECT(std::abs(shocked - shockedTheory) <= std::abs(shockedTheory - initial) * tolerance);
        EXPECT(std::abs(expanded - expandedTheory) <= std::abs(expandedTheory - initial) * tolerance);
    }

    // 圧力の弱い段差は音の波: 前線が c̃ = 340 m/s で進む(既定の設定)。断熱: 圧縮の波の後ろは温まり、膨張の波の後ろは冷える
    // (T ∝ p^((γ−1)/γ)。弱い波なので等エントロピーとほぼ同じ。T-0234)
    void TestSoundSpeed() {
        GasBox box = MakePressureTube(1, GasThermodynamics::Adiabatic, 340000);
        const TubeFront front = MeasureTubeFront(box, 1);
        EXPECT(front.speed >= 340.0 * 0.98 && front.speed <= 340.0 * 1.02);
        EXPECT(front.lateWidth > front.earlyWidth * 1.2);  // 線形の波の前線は数値の拡散で広がる(衝撃波との比べ)

        RiemannSolution theory = SolveRiemann(1.01, AirGamma(box.config), 340.0);
        ExpectTubeTemperatures(box, theory, 0.03);
    }

    // 強い段差は衝撃波になる(断熱。T-0234): 音速より速く(理論どおり)進み、前線の幅が時間で広がらない(非線形の切り立ち)。
    // 衝撃波の後ろは Rankine–Hugoniot の温度まで温まり、膨張の波の後ろは断熱の理論どおり冷える。
    // c̃ を本物の断熱の音速にして α = 1(力も本物の圧力)にし、理想気体の厳密な解と比べる。頭打ちは既定のまま(T-0269。
    // T-0234 の時は頭打ちが地面の温度からで、外さないと衝撃波が 4〜7% 遅く後ろが熱かった)
    void TestShockForms() {
        for (const uint32_t overPercent : {100u, 400u}) {
            const GasConfig air = MakeAirConfig(1, 1, 1);
            const double sound = AirSoundSpeed(air);
            GasBox box = MakePressureTube(overPercent, GasThermodynamics::Adiabatic,
                                          (uint32_t)std::lround(sound * 1000));
            const TubeFront front = MeasureTubeFront(box, overPercent);
            const RiemannSolution theory = SolveRiemann(1.0 + (overPercent / 100.0), AirGamma(air), sound);
            std::printf("  adiabatic shock theory: %.1f m/s (sound %.1f m/s)\n", theory.shockSpeed, sound);
            EXPECT(front.speed >= theory.shockSpeed * 0.98 && front.speed <= theory.shockSpeed * 1.02);
            EXPECT(front.lateWidth <= front.earlyWidth + 0.5);
            EXPECT(front.lateWidth <= 8.0);
            ExpectTubeTemperatures(box, theory, 0.05);
        }
    }

    // 既定の世界(c̃ = 340 m/s・小刻み 24・頭打ちは既定)の衝撃波(T-0269・D-461): 2 倍・5 倍の段差の前線の速さが
    // 音速 340 m/s の理想気体の理論と 2% 以内(T-0234 の頭打ち〔地面の温度から〕では 383・447.7 m/s で 4〜7% 遅かった)
    void TestDefaultWorldShock() {
        for (const uint32_t overPercent : {100u, 400u}) {
            const GasConfig air = MakeAirConfig(1, 1, 1);
            GasBox box = MakePressureTube(overPercent, GasThermodynamics::Adiabatic, air.soundSpeedMmPerS);
            const TubeFront front = MeasureTubeFront(box, overPercent);
            const RiemannSolution theory = SolveRiemann(1.0 + (overPercent / 100.0), AirGamma(air),
                                                        air.soundSpeedMmPerS / 1000.0);
            std::printf("  default world shock x%u: theory %.1f m/s\n", 1 + (overPercent / 100), theory.shockSpeed);
            EXPECT(front.speed >= theory.shockSpeed * 0.98 && front.speed <= theory.shockSpeed * 1.02);
        }
    }

    // 熱いセルの音速の頭打ちの温度(T-0269・D-461〔Q32 案 D〕): 既定(340 m/s・小刻み 24・293.15 K)で、実効の音速
    // c̃ √(T ÷ T_基準)が安定の上限 432 m/s(ν = 0.6)に届く約 473 K。c̃ が上限なら地面の温度、とても小さい c̃ なら頭打ちなし
    void TestSoundCapTemperature() {
        const GasBox box = MakeAirBox(MakeAirConfig(2, 2, 2));
        const uint32_t ground = box.referenceDerived.front().temperature;
        const uint64_t limit = GasSoundSpeedLimitMmPerS(box.config.substeps);
        const uint32_t cap = box.coefficients.soundTemperature;
        const double capSpeed = 340.0 * std::sqrt((double)cap / ground);
        const double nu = capSpeed * 1000.0 / (30000.0 * box.config.substeps);
        std::printf("sound cap: %u mK (ground %u mK), sound speed at cap %.2f m/s, nu %.4f\n", cap, ground, capSpeed,
                    nu);
        EXPECT(cap == GasSoundCapTemperature(box.config.soundSpeedMmPerS, limit, ground));
        EXPECT(cap >= 470000 && cap <= 476000);
        EXPECT(nu <= 0.6);
        EXPECT(340.0 * std::sqrt((double)(cap + 2) / ground) * 1000.0 > (double)limit);  // 切り捨て(上限のすぐ下)
        EXPECT(GasSoundCapTemperature((uint32_t)limit, limit, ground) == ground);
        EXPECT(GasSoundCapTemperature(1, limit, ground) == UINT32_MAX);
    }

    // 等温の気体(T-0226 までの形。設定で残す)の衝撃波管の理論: 中間の圧力 x = p* ÷ p_右 は
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

    void TestIsothermalShock() {
        GasBox box = MakePressureTube(100, GasThermodynamics::Isothermal, 340000);
        const TubeFront front = MeasureTubeFront(box, 100);
        const double expected = IsothermalShockSpeed(2.0);
        std::printf("  isothermal shock theory: %.1f m/s\n", expected);
        EXPECT(front.speed >= expected * 0.98 && front.speed <= expected * 1.02);
        EXPECT(front.lateWidth <= front.earlyWidth + 0.5);
    }

    // 運動エネルギーの散逸を熱へ(T-0234): 閉じた箱(重力なし)の全部のセルをでたらめな速さ(各軸 ±5 m/s)で動かすと、
    // 混ざって動きが止まり、失った運動エネルギーの分だけ平均の温度が上がる(全エネルギーは毎刻みビット単位で一定)
    void TestKineticEnergyBecomesHeat() {
        GasConfig config = MakeAirConfig(8, 8, 8);
        config.gravity = false;
        std::vector<GasCell> reference = MakeHydrostaticReference(config, AIR_FRACTIONS_Q32, AIR_TEMPERATURE,
                                                                  SURFACE_PRESSURE);
        std::fill(reference.begin() + 1, reference.end(), reference.front());
        GasBox box = MakeGasBox(config, reference);
        uint64_t state = 777;
        for (GasCell& cell : box.cells) {
            std::array<int64_t, GAS_AXES> momentum = {};
            for (int64_t& value : momentum) {
                state = (state * 6364136223846793005ull) + 1442695040888963407ull;
                value = ((int64_t)((state >> 33) % 11) - 5) * ROUGH_CELL_MASS * METER_PER_SECOND;
            }

            SetGasMomentum(box, cell, momentum);
        }

        auto kinetic = [&box]() {
            double sum = 0;
            for (const GasCell& cell : box.cells)
                sum += (double)GasKineticEnergy(box, cell) * 1e-6;  // J

            return sum;
        };

        auto meanTemperature = [&box]() {
            double sum = 0;
            for (const GasCell& cell : box.cells)
                sum += DeriveGasCell(box, cell, 0).temperature / 1000.0;

            return sum / (double)box.cells.size();
        };

        double capacity = 0;  // J/K(箱全体の定積の熱容量)
        for (const GasCell& cell : box.cells) {
            for (uint32_t s = 0; s < config.speciesCount; ++s)
                capacity += (double)cell.amounts[s] * config.species[s].heatCapacity * 1e-9;
        }

        const int64_t energy = SumGas(box).energy;
        const double kineticBefore = kinetic();
        const double temperatureBefore = meanTemperature();
        for (uint32_t tick = 0; tick < 120; ++tick) {
            StepGas(box);
            EXPECT(SumGas(box).energy == energy);  // 毎刻み
        }

        const double lost = kineticBefore - kinetic();
        const double warming = meanTemperature() - temperatureBefore;
        std::printf("kinetic energy %.1f J -> %.1f J after 2 s, mean temperature +%.4f K (lost / capacity = %.4f K)\n",
                    kineticBefore, kineticBefore - lost, warming, lost / capacity);
        EXPECT(lost >= kineticBefore * 0.5);  // 半分以上が熱になる(数値の粘性と衝撃波。本物の粘性は入れていない)
        EXPECT(std::abs(warming - (lost / capacity)) <= (lost / capacity) * 0.05);
    }

    // 全エネルギーが毎刻み「初め + 帳簿(重さの仕事)」とビット一致(重力ありの閉じた箱。T-0234)
    void TestEnergyEveryTick() {
        GasBox box = MakeDisturbedClosedBox();
        const int64_t energy = SumGas(box).energy;
        for (uint32_t tick = 0; tick < 60; ++tick) {
            StepGas(box);
            EXPECT(SumGas(box).energy == energy + box.ledger.energy);
        }

        std::printf("closed box with gravity: energy ledger (gravity work) after 1 s = %lld mJ\n",
                    (long long)box.ledger.energy);
    }

    // 3 次元の安定の条件: 重力ありの閉じた 10³ の箱の全部のセルを ±1% でたらめに乱しても、音の波が育たない
    // (素の Rusanov の拡散〔÷ 2c̃〕・小刻み 32 では 1 秒で 40 m/s・真空のセルまで育った。T-0226)
    void TestNoiseStaysSmall() {
        // 断熱の乱れ(密度 ±1%・温度はその (γ − 1) 倍。膨らんで戻ると周りと同じ温度)。同じ温度の乱れだと ±0.8 K の浮力の対流が
        // 本当に起きて 10 cm/s ほどになる(安定の条件の試験にならない。T-0234)
        GasBox box = MakeAirBox(MakeAirConfig(10, 10, 10));
        const double gammaLessOne = AirGamma(box.config) - 1.0;
        uint64_t state = 12345;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            GasCell& cell = box.cells[index];
            const uint32_t temperature = DeriveGasCell(box, cell, CellZ(box, index)).temperature;
            state = (state * 6364136223846793005ull) + 1442695040888963407ull;
            const int64_t perMille = (int64_t)((state >> 33) % 21) - 10;  // −10〜+10(1/1000)
            std::array<uint64_t, GAS_MAX_SPECIES> amounts = cell.amounts;
            for (uint32_t s = 0; s < box.config.speciesCount; ++s)
                amounts[s] = (uint64_t)((int64_t)amounts[s] + ((int64_t)amounts[s] * perMille / 1000));

            const double heating = 1.0 + (gammaLessOne * (double)perMille / 1000.0);
            cell = MakeGasCell(box.config, amounts, (uint32_t)std::lround(temperature * heating));
        }

        const GasTotals before = SumGas(box);
        Run(box, 300);
        const int64_t speed = MaxSpeed(box);
        std::printf("noise +-1%% in 10^3 box: max speed after 5 s = %lld (2^-20 m/s)\n", (long long)speed);
        EXPECT(speed <= METER_PER_SECOND / 10);
        EXPECT(SumGas(box).energy == before.energy + box.ledger.energy);
    }

    // 最も軽いセルの質量(mg)
    uint64_t MinimumMass(const GasBox& box) {
        uint64_t lightest = UINT64_MAX;
        for (uint32_t index = 0; index < box.cells.size(); ++index)
            lightest = std::min(lightest, DeriveGasCell(box, box.cells[index], CellZ(box, index)).mass);

        return lightest;
    }

    // 1 セルだけ強い圧力(5 倍の物質量・1500 K・その両方〔熱い爆発。25 倍の圧力〕)を放す: 真空のセルができず、保存が保たれ、2 秒後に落ち着く。
    // 頭打ちの温度が約 473 K になり(T-0269)、1500 K のセルの実効の音速は 340 → 432 m/s(ν 0.47 → 0.6)
    void TestStrongPressureStable() {
        for (const uint32_t kind : {0u, 1u, 2u}) {
            const bool dense = kind != 1;
            const bool hot = kind != 0;
            GasBox box = MakeAirBox(MakeAirConfig(12, 12, 12));
            const uint32_t center = box.Index(6, 6, 3);
            if (dense) {
                GasCell& cell = box.cells[center];
                for (uint32_t s = 0; s < box.config.speciesCount; ++s)
                    cell.amounts[s] *= 5;

                cell.energy *= 5;
            }

            if (hot)
                Heat(box, center, 1500000);

            const GasTotals before = SumGas(box);
            uint64_t lightest = UINT64_MAX;
            for (uint32_t tick = 0; tick < 120; ++tick) {
                StepGas(box);
                lightest = std::min(lightest, MinimumMass(box));
            }

            const GasTotals after = SumGas(box);
            const int64_t speed = MaxSpeed(box);
            const char* names[] = {"x5 amount", "1500 K", "x5 amount 1500 K"};
            std::printf("strong pressure (%s): lightest cell %llu mg, max speed after 2 s = %lld (2^-20 m/s)\n",
                        names[kind], (unsigned long long)lightest, (long long)speed);
            EXPECT(after.amounts == before.amounts);
            EXPECT(after.energy == before.energy + box.ledger.energy);
            // 真空のセルができない(不安定なら 0 まで抜けた)。熱いセルは膨らんで軽くなる(圧力が釣り合えば約 1/5 = 30000 mg。
            // 熱い爆発は膨らみ過ぎてもっと軽くなる)
            EXPECT(lightest >= (uint64_t)ROUGH_CELL_MASS / (kind == 2 ? 16 : 8));
            EXPECT(speed <= 10 * METER_PER_SECOND);
        }
    }

    // --- 重力を変えた世界(T-0423・D-480・ADR-0171)---

    // 静止大気の地面の層と一番上の層の圧力の差(µPa)。セルの物質量は µmol の整数なので、1 層の差(月で約 50 µmol 分)では
    // 丸めが 1% 近くになる。箱の高さ全部の差で比べる
    int64_t LayerPressureDrop(const GasConfig& config) {
        const GasBox box = MakeAirBox(config);
        const uint32_t top = config.sizeZ - 1;
        return DeriveGasCell(box, box.cells[box.Index(0, 0, 0)], 0).pressure -
               DeriveGasCell(box, box.cells[box.Index(0, 0, top)], top).pressure;
    }

    // 静止大気の層ごとの圧力の差は重力に比例し(無重力では 0)、どの重力でも静止大気は動かない
    void TestHydrostaticScalesWithGravity() {
        const GasConfig earth = MakeAirConfig(4, 4, 16);
        const int64_t earthDrop = LayerPressureDrop(earth);
        const int64_t moonDrop = LayerPressureDrop(WithGravity(earth, MOON_GRAVITY));
        const int64_t zeroDrop = LayerPressureDrop(WithGravity(earth, 0));
        const double ratio = (double)moonDrop / (double)earthDrop;
        const double gravityRatio = (double)MOON_GRAVITY / (double)STANDARD_GRAVITY_MICROMETERS_PER_SECOND2;
        std::printf(
            "layer pressure drop: earth %lld uPa, moon %lld uPa (ratio %.5f, gravity ratio %.5f), zero %lld uPa\n",
            (long long)earthDrop, (long long)moonDrop, ratio, gravityRatio, (long long)zeroDrop);
        EXPECT(zeroDrop == 0);
        EXPECT(std::abs(ratio - gravityRatio) < gravityRatio * 5e-3);

        for (const uint32_t gravity : {0u, MOON_GRAVITY, fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2}) {
            GasBox box = MakeAirBox(WithGravity(earth, gravity));
            const GasTotals before = SumGas(box);
            Run(box, 120);
            std::printf("static atmosphere (gravity %u um/s^2): max speed after 120 ticks = %lld\n", gravity,
                        (long long)MaxSpeed(box));
            EXPECT(MaxSpeed(box) <= 1);
            EXPECT(SumGas(box).energy == before.energy);
            EXPECT(box.ledger.energy == 0);
        }
    }

    // 乱した閉じた箱: 重力の値によらず保存則がビット単位で成り立つ(エネルギー = 初め + 帳簿の重さの仕事。無重力では帳簿が 0)
    void TestClosedBoxAnyGravity() {
        for (const uint32_t gravity : {0u, MOON_GRAVITY, fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2}) {
            GasBox box = MakeDisturbedClosedBox(gravity);
            const GasTotals before = SumGas(box);
            Run(box, 120);
            const GasTotals after = SumGas(box);

            for (uint32_t s = 0; s < box.config.speciesCount; ++s)
                EXPECT(after.amounts[s] == before.amounts[s]);

            EXPECT(after.energy == before.energy + box.ledger.energy);
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                EXPECT(after.momentum[axis] == before.momentum[axis] + box.ledger.momentum[axis]);

            if (gravity == 0)
                EXPECT(box.ledger.energy == 0);

            std::printf("closed box (gravity %u um/s^2): gravity work %lld mJ, max speed %lld\n", gravity,
                        (long long)box.ledger.energy, (long long)MaxSpeed(box));
        }
    }

    // --- 環境の値のホットリロード(T-0444・ADR-0194)---

    // 新しい環境(重力・音速の基準)で層の基準を作り直して当てる(基準の大気は同じ)
    void ChangeEnvironment(GasBox& box, uint32_t gravity, uint32_t soundSpeed) {
        GasConfig next = box.config;
        WorldPhysics world{.gravityMicrometersPerSecond2 = gravity, .soundSpeedMillimetersPerSecond = soundSpeed};
        ApplyWorldPhysics(world, next);
        EXPECT(CheckGasEnvironment(box.config, next).has_value());
        SetGasEnvironment(box, next,
                          MakeHydrostaticReference(next, AIR_FRACTIONS_Q32, AIR_TEMPERATURE, SURFACE_PRESSURE));
    }

    // 器が受けられない環境は理由を返す(音速の基準 0・安定の上限の外・重力の上限の外・箱の作りが違う)
    void TestEnvironmentCheck() {
        const GasConfig current = MakeDisturbedClosedBox().config;
        GasConfig next = current;
        next.gravityMicrometersPerSecond2 = MOON_GRAVITY;
        next.soundSpeedMmPerS = 300000;
        EXPECT(CheckGasEnvironment(current, next).has_value());

        next.soundSpeedMmPerS = 0;
        EXPECT(!CheckGasEnvironment(current, next).has_value());

        next.soundSpeedMmPerS = (uint32_t)GasSoundSpeedLimitMmPerS(current.substeps) + 1;
        const auto overLimit = CheckGasEnvironment(current, next);
        EXPECT(!overLimit.has_value());
        if (!overLimit)
            std::printf("environment check: %s\n", overLimit.error().c_str());

        next.soundSpeedMmPerS = 300000;
        next.gravityMicrometersPerSecond2 = fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2 + 1;
        EXPECT(!CheckGasEnvironment(current, next).has_value());

        GasConfig resized = current;
        resized.sizeZ += 1;
        EXPECT(!CheckGasEnvironment(current, resized).has_value());
    }

    // 同じ値を当て直しても結果は変わらない / 違う値は差し替えの刻みから効き、保存則はそのまま / 静止大気は新しい重力で動き出す
    void TestEnvironmentChange() {
        GasBox unchanged = MakeDisturbedClosedBox();
        GasBox same = unchanged;
        GasBox changed = unchanged;
        const GasTotals before = SumGas(unchanged);
        Run(unchanged, 20);
        Run(same, 20);
        Run(changed, 20);
        EXPECT(HashGas(changed) == HashGas(unchanged));

        ChangeEnvironment(same, same.config.gravityMicrometersPerSecond2, same.config.soundSpeedMmPerS);
        ChangeEnvironment(changed, MOON_GRAVITY, 300000);
        EXPECT(changed.coefficients.gravityImpulse < unchanged.coefficients.gravityImpulse);
        Run(unchanged, 20);
        Run(same, 20);
        Run(changed, 20);
        EXPECT(HashGas(same) == HashGas(unchanged) && same.ledger.energy == unchanged.ledger.energy);
        EXPECT(HashGas(changed) != HashGas(unchanged));

        const GasTotals after = SumGas(changed);
        for (uint32_t s = 0; s < changed.config.speciesCount; ++s)
            EXPECT(after.amounts[s] == before.amounts[s]);

        EXPECT(after.energy == before.energy + changed.ledger.energy);
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
            EXPECT(after.momentum[axis] == before.momentum[axis] + changed.ledger.momentum[axis]);

        // 静止大気(標準の重力で釣り合った層)を無重力にすると、層の重さの差がほどけて動き出す
        GasBox still = MakeAirBox(MakeAirConfig(4, 4, 16));
        Run(still, 10);
        const int64_t stillSpeed = MaxSpeed(still);
        ChangeEnvironment(still, 0, still.config.soundSpeedMmPerS);
        Run(still, 30);
        EXPECT(stillSpeed <= 1 && MaxSpeed(still) > 1000);
        std::printf("environment change: closed box gravity work %lld -> %lld mJ, still air max speed %lld -> %lld\n",
                    (long long)unchanged.ledger.energy, (long long)changed.ledger.energy, (long long)stillSpeed,
                    (long long)MaxSpeed(still));
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
    TestDefaultWorldShock();
    TestSoundCapTemperature();
    TestIsothermalShock();
    TestKineticEnergyBecomesHeat();
    TestEnergyEveryTick();
    TestNoiseStaysSmall();
    TestStrongPressureStable();
    TestHydrostaticScalesWithGravity();
    TestClosedBoxAnyGravity();
    TestEnvironmentCheck();
    TestEnvironmentChange();

    if (failureCount != 0) {
        std::printf("gas_reference_test: %d failure(s)\n", failureCount);
        return 1;
    }

    std::printf("gas_reference_test: OK\n");
    return 0;
}
