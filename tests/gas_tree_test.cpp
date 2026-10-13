// gas_tree_test.cpp — 気体の多重解像度の木(sim/gas_tree。T-0208・07 §2.5・ADR-0117)を CPU だけで確かめる。合格の条件:
//   全部がレベル 0 なら 1 レベルの gas_reference と毎刻みビット一致(壁と重さ・開いた境界と風・周期的な箱)・
//   レベルをまたぐ静止大気(各レベルの基準)が動き出さない・閉じた箱で成分・エネルギー・運動量が毎刻み「初め + 帳簿」とビット一致(端数を含む)・
//   同じ入力なら同じ状態・圧力の波がレベル 0 → 1 → 0 の境を通る時の反射と通った波の誤差が小さい(全部レベル 0 の管と比べる)。
//   レベルごとの小刻み(T-0320): 分けなければ T-0208 の実装と毎刻みビット一致(状態の鎖のハッシュ)・分けても(レベル 1 が 2 倍の小刻み)
//   静止大気が動かない・閉じた箱の保存が毎刻みビット一致・波の反射が T-0208 と同じ程度。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。測った値も表示する(07 §2.5 に書いた数字の出どころ)。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "sim/gas_reference.h"
#include "sim/gas_tree.h"
#include "sim/world_physics.h"

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

    // --- 試験の空気(gas_reference_test と同じ)---
    constexpr uint32_t SPECIES_N2 = 0;
    constexpr uint32_t SPECIES_SMOKE = 2;
    constexpr std::array<uint32_t, 3> AIR_FRACTIONS_Q32 = {3393024163u, 901943132u, 0u};
    constexpr uint32_t AIR_TEMPERATURE = 293150;
    constexpr uint64_t SURFACE_PRESSURE = 101325ull * 1000000ull;
    constexpr int64_t METER_PER_SECOND = (int64_t)1 << 20;
    constexpr int64_t ROUGH_CELL_MASS = 150000;

    GasConfig MakeAirConfig(uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ) {
        GasConfig config;
        config.sizeX = sizeX;
        config.sizeY = sizeY;
        config.sizeZ = sizeZ;
        config.speciesCount = 3;
        config.species[0] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};
        config.species[1] = {.molarMass = 31999, .heatCapacity = 21064, .formationEnergy = 0};
        config.species[2] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};

        ApplyWorldPhysics(StandardWorldPhysics(), config);  // 標準の重力の世界(D-480。器の既定は 0)

        return config;
    }

    std::vector<GasCell> AirReference(const GasConfig& config) {
        std::vector<GasCell> reference = MakeHydrostaticReference(config, AIR_FRACTIONS_Q32, AIR_TEMPERATURE,
                                                                  SURFACE_PRESSURE);
        if (!config.gravity)
            std::fill(reference.begin(), reference.end(), reference.front());

        return reference;
    }

    GasTree MakeAirTree(const GasConfig& config, std::span<const GasTreeRegion> regions,
                        const GasTreeOptions& options = {}) {
        return MakeGasTree(config, regions, AIR_FRACTIONS_Q32, AIR_TEMPERATURE, SURFACE_PRESSURE, options);
    }

    void Heat(const GasConfig& config, GasCell& cell, uint32_t temperature) {
        GasCell heated = MakeGasCell(config, cell.amounts, temperature);
        heated.momentum = cell.momentum;
        cell = heated;
    }

    void AddSmoke(GasCell& cell) {
        const uint64_t half = cell.amounts[SPECIES_N2] / 2;
        cell.amounts[SPECIES_N2] -= half;
        cell.amounts[SPECIES_SMOKE] += half;
    }

    // --- 全部がレベル 0 なら 1 レベルの箱と毎刻みビット一致 ---

    bool SameCells(const GasBox& box, const GasTree& tree) {
        if (box.cells.size() != tree.cells.size())
            return false;

        for (size_t i = 0; i < box.cells.size(); ++i) {
            const GasCell& a = box.cells[i];
            const GasCell& b = tree.cells[i];
            if (a.amounts != b.amounts || a.energy != b.energy || a.momentum != b.momentum)
                return false;
        }

        return true;
    }

    bool SameLedger(const GasBox& box, const GasTree& tree) {
        bool same = tree.ledger.energy == GasQ64{.whole = box.ledger.energy, .fraction = 0};
        for (uint32_t s = 0; s < GAS_MAX_SPECIES; ++s)
            same = same && tree.ledger.amounts[s] == GasQ64{.whole = box.ledger.amounts[s], .fraction = 0};

        for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
            same = same && tree.ledger.momentum[axis] == GasQ64{.whole = box.ledger.momentum[axis], .fraction = 0};

        return same;
    }

    void CompareWithReference(const GasConfig& config, const char* name) {
        GasBox box = MakeGasBox(config, AirReference(config));
        Heat(config, box.cells[box.Index(1, 2, 1)], AIR_TEMPERATURE + 60000);
        Heat(config, box.cells[box.Index(4, 1, 2)], AIR_TEMPERATURE - 30000);
        AddSmoke(box.cells[box.Index(2, 3, 2)]);
        SetGasMomentum(box, box.cells[box.Index(3, 1, 1)],
                       {ROUGH_CELL_MASS * 3 * METER_PER_SECOND, -ROUGH_CELL_MASS * METER_PER_SECOND,
                        ROUGH_CELL_MASS * 2 * METER_PER_SECOND});

        GasTree tree = MakeAirTree(config, {});
        EXPECT(tree.cells.size() == box.cells.size());
        tree.cells = box.cells;

        int mismatchTick = -1;
        for (int tick = 0; tick < 12 && mismatchTick < 0; ++tick) {
            StepGas(box);
            StepGasTree(tree);
            if (!SameCells(box, tree) || !SameLedger(box, tree))
                mismatchTick = tick;
        }

        std::printf("single level vs gas_reference (%s): first mismatch tick = %d (-1 = none in 12 ticks)\n", name,
                    mismatchTick);
        EXPECT(mismatchTick < 0);
    }

    void TestSingleLevelMatchesReference() {
        GasConfig walls = MakeAirConfig(6, 5, 4);
        CompareWithReference(walls, "walls + gravity");

        GasConfig open = MakeAirConfig(6, 5, 4);
        open.boundary = {GasBoundary::Open, GasBoundary::Periodic, GasBoundary::Open};
        open.windVelocity = {2 * METER_PER_SECOND, 0, 0};
        CompareWithReference(open, "open + wind");

        GasConfig periodic = MakeAirConfig(6, 5, 4);
        periodic.boundary = {GasBoundary::Periodic, GasBoundary::Periodic, GasBoundary::Periodic};
        periodic.gravity = false;
        periodic.reconstruction = GasReconstruction::Minmod;
        CompareWithReference(periodic, "periodic, no gravity");
    }

    // --- 2 レベルの箱(8 × 4 × 8 のレベル 0 の真ん中 4 × 4 を x と z でレベル 1 に。y は全部)---

    constexpr std::array<GasTreeRegion, 1> MIDDLE_REGION = {
        GasTreeRegion{.level = 1, .origin = {4, 0, 4}, .size = {8, 8, 8}}};

    GasConfig TwoLevelConfig() {
        GasConfig config = MakeAirConfig(8, 4, 8);
        config.substeps = 48;  // 全部のレベルが同じ小刻み: レベル 1 の ν = 0.47(T-0208)
        return config;
    }

    // レベルごとの小刻み(T-0320): レベル 0 は 24 回(ν = 0.47)、レベル 1 は 24 × 2 回(ν = 0.47)
    constexpr GasTreeOptions SUBCYCLED = {.subcycleBaseLevel = 0, .maxSubcycleGap = 1};

    GasConfig SubcycledConfig() {
        GasConfig config = MakeAirConfig(8, 4, 8);
        config.substeps = 24;
        return config;
    }

    int64_t MaxMomentum(const GasTree& tree) {
        int64_t largest = 0;
        for (const GasCell& cell : tree.cells) {
            for (int64_t momentum : cell.momentum)
                largest = std::max(largest, momentum < 0 ? -momentum : momentum);
        }

        return largest;
    }

    bool StaysStill(const GasConfig& config, const GasTreeOptions& options, const char* name) {
        GasTree tree = MakeAirTree(config, MIDDLE_REGION, options);
        const std::vector<GasCell> before = tree.cells;
        for (int tick = 0; tick < 20; ++tick)
            StepGasTree(tree);

        bool unchanged = true;
        for (size_t i = 0; i < before.size(); ++i)
            unchanged = unchanged && before[i].amounts == tree.cells[i].amounts &&
                        before[i].energy == tree.cells[i].energy && before[i].momentum == tree.cells[i].momentum;

        std::printf("static atmosphere across levels (%s): max momentum after 20 ticks = %lld, unchanged = %d\n", name,
                    (long long)MaxMomentum(tree), unchanged ? 1 : 0);
        return unchanged;
    }

    void TestStaticAtmosphereAcrossLevels() {
        EXPECT(StaysStill(SubcycledConfig(), SUBCYCLED, "subcycled"));

        GasTree tree = MakeAirTree(TwoLevelConfig(), MIDDLE_REGION);
        const std::vector<GasCell> before = tree.cells;
        for (int tick = 0; tick < 20; ++tick)
            StepGasTree(tree);

        bool unchanged = true;
        for (size_t i = 0; i < before.size(); ++i)
            unchanged = unchanged && before[i].amounts == tree.cells[i].amounts &&
                        before[i].energy == tree.cells[i].energy && before[i].momentum == tree.cells[i].momentum;

        std::printf("static atmosphere across levels: max momentum after 20 ticks = %lld, unchanged = %d\n",
                    (long long)MaxMomentum(tree), unchanged ? 1 : 0);
        EXPECT(unchanged);

        // 情報: 細かいセルを親の数の写しで作ると(17 §1 の細かくし方)、親の中の静水圧の傾きが無いので少し動く(T-0210 で載せる時の目安)
        GasTree copied = MakeAirTree(TwoLevelConfig(), MIDDLE_REGION);
        for (uint32_t i = 0; i < copied.cells.size(); ++i) {
            const GasTreePlace& place = copied.places[i];
            if (place.level == 1)
                copied.cells[i] = copied.levels[0].reference[place.coordinate[2] / 2];
        }

        for (int tick = 0; tick < 20; ++tick)
            StepGasTree(copied);

        std::printf("refined by copying the parent: max speed after 20 ticks = %.4f m/s\n",
                    (double)MaxMomentum(copied) / (double)METER_PER_SECOND / (double)ROUGH_CELL_MASS);
    }

    // --- 閉じた箱: 合計(端数を含む)が毎刻み「初め + 帳簿」とビット一致 ---

    GasQ64 AddQ64(GasQ64 a, GasQ64 b) {
        GasQ64 sum;
        sum.fraction = a.fraction + b.fraction;
        sum.whole = a.whole + b.whole + (sum.fraction < a.fraction ? 1 : 0);
        return sum;
    }

    GasTree MakeDisturbedTwoLevelBox(const GasConfig& config = TwoLevelConfig(), const GasTreeOptions& options = {}) {
        GasTree tree = MakeAirTree(config, MIDDLE_REGION, options);
        Heat(config, tree.cells[tree.Find(0, 1, 1, 2)], AIR_TEMPERATURE + 60000);  // 細かい範囲の左の粗いセル
        Heat(config, tree.cells[tree.Find(1, 4, 2, 5)], AIR_TEMPERATURE + 60000);  // 境の細かいセル
        Heat(config, tree.cells[tree.Find(1, 6, 3, 4)], AIR_TEMPERATURE - 20000);
        AddSmoke(tree.cells[tree.Find(1, 5, 5, 6)]);
        AddSmoke(tree.cells[tree.Find(0, 6, 2, 6)]);

        // 速度 2 m/s の粗いセル(運動エネルギーをエネルギーへ足す: P² × 2^-41 ÷ m µJ)
        GasCell& moving = tree.cells[tree.Find(0, 6, 1, 3)];
        const int64_t momentum = -ROUGH_CELL_MASS * 2 * METER_PER_SECOND;
        moving.momentum = {momentum, 0, momentum / 2};
        const double kinetic = ((double)momentum * momentum * 1.25) / std::ldexp(1.0, 41) / (double)ROUGH_CELL_MASS;
        moving.energy += (int64_t)std::llround(kinetic / 1000.0);

        return tree;
    }

    uint32_t CountFractions(const GasTree& tree) {
        uint32_t count = 0;
        for (const GasTreeFraction& fraction : tree.fractions) {
            const bool any = fraction.energy != 0 || fraction.amounts[0] != 0 || fraction.momentum[0] != 0 ||
                             fraction.momentum[2] != 0;
            count += any ? 1 : 0;
        }

        return count;
    }

    void CheckClosedBoxConservation(const GasConfig& config, const GasTreeOptions& options, const char* name) {
        GasTree tree = MakeDisturbedTwoLevelBox(config, options);
        const GasTreeTotals before = SumGasTree(tree);
        int brokenTick = -1;
        for (int tick = 0; tick < 30; ++tick) {
            StepGasTree(tree);
            const GasTreeTotals now = SumGasTree(tree);
            bool kept = now.energy == AddQ64(before.energy, tree.ledger.energy);
            for (uint32_t s = 0; s < tree.config.speciesCount; ++s)
                kept = kept && now.amounts[s] == before.amounts[s] && tree.ledger.amounts[s] == GasQ64{};

            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                kept = kept && now.momentum[axis] == AddQ64(before.momentum[axis], tree.ledger.momentum[axis]);

            if (!kept && brokenTick < 0)
                brokenTick = tick;
        }

        std::printf(
            "closed two-level box (%s): first broken tick = %d (-1 = none in 30), cells with fractions = %u, "
            "max speed = %.3f m/s\n",
            name, brokenTick, CountFractions(tree),
            (double)MaxMomentum(tree) / (double)METER_PER_SECOND / (double)ROUGH_CELL_MASS);
        EXPECT(brokenTick < 0);
        EXPECT(CountFractions(tree) > 0);  // レベルをまたぐ面の端数を通った

        // 同じ入力なら同じ状態(決定性)
        GasTree again = MakeDisturbedTwoLevelBox(config, options);
        for (int tick = 0; tick < 30; ++tick)
            StepGasTree(again);

        bool same = true;
        for (size_t i = 0; i < tree.cells.size(); ++i)
            same = same && tree.cells[i].amounts == again.cells[i].amounts &&
                   tree.cells[i].energy == again.cells[i].energy && tree.cells[i].momentum == again.cells[i].momentum;

        EXPECT(same);
    }

    void TestClosedBoxConservationAcrossLevels() {
        CheckClosedBoxConservation(TwoLevelConfig(), {}, "same substeps");

        const GasTree subcycled = MakeDisturbedTwoLevelBox(SubcycledConfig(), SUBCYCLED);
        EXPECT(subcycled.maxSubcycleExponent == 1 && subcycled.levels[1].subcycleExponent == 1);
        CheckClosedBoxConservation(SubcycledConfig(), SUBCYCLED, "subcycled");
    }

    // --- 圧力の波がレベルの境を通る(全部レベル 0 の管と比べた差 = 境が作った反射と通った波の誤差)---

    constexpr uint32_t TUBE_LENGTH = 96;
    constexpr uint32_t PULSE_CENTER = 20;
    constexpr uint32_t PULSE_HALF_WIDTH = 6;
    constexpr uint32_t FINE_BEGIN = 40;  // レベル 0 の x。ここから 16 セルがレベル 1
    constexpr uint32_t FINE_END = 56;

    GasConfig TubeConfig(uint32_t substeps) {
        GasConfig config = MakeAirConfig(TUBE_LENGTH, 2, 2);
        config.boundary = {GasBoundary::Wall, GasBoundary::Periodic, GasBoundary::Periodic};
        config.gravity = false;
        config.substeps = substeps;
        return config;
    }

    // 右へ進む弱い圧力の山(単純波: δρ/ρ = δ ÷ γ・δT/T = (γ − 1)δ ÷ γ・u = c δ ÷ γ。δ = 1% の余弦の山)
    GasBox MakePulseTube(uint32_t substeps) {
        const GasConfig config = TubeConfig(substeps);
        GasBox box = MakeGasBox(config, AirReference(config));
        const double gamma = 1.4;
        const double sound = 340.0;  // c̃(実効の音速)
        const auto mass = (double)box.referenceDerived[0].mass;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const double offset = std::fabs((double)(index % TUBE_LENGTH) - (double)PULSE_CENTER);
            if (offset >= PULSE_HALF_WIDTH)
                continue;

            const double delta = 0.01 * 0.5 * (1.0 + std::cos(3.14159265358979 * offset / PULSE_HALF_WIDTH));
            GasCell& cell = box.cells[index];
            for (uint32_t s = 0; s < config.speciesCount; ++s)
                cell.amounts[s] = (uint64_t)std::llround((double)cell.amounts[s] * (1.0 + (delta / gamma)));

            Heat(config, cell, (uint32_t)std::llround(AIR_TEMPERATURE * (1.0 + (delta * (gamma - 1.0) / gamma))));
            const double velocity = sound * delta / gamma;
            const auto momentum = (int64_t)std::llround(mass * (1.0 + (delta / gamma)) * velocity * METER_PER_SECOND);
            SetGasMomentum(box, cell, {momentum, 0, 0});
        }

        return box;
    }

    // 箱の状態を木へ写す(細かいセルは親の数のまま = 17 §1 の細かくし方)
    GasTree CopyIntoTree(const GasBox& box, std::span<const GasTreeRegion> regions, const GasTreeOptions& options) {
        GasTree tree = MakeAirTree(box.config, regions, options);
        for (uint32_t i = 0; i < tree.cells.size(); ++i) {
            const GasTreePlace& place = tree.places[i];
            const uint32_t shift = place.level;
            tree.cells[i] = box.cells[box.Index(place.coordinate[0] >> shift, place.coordinate[1] >> shift,
                                                place.coordinate[2] >> shift)];
        }

        return tree;
    }

    // 一様な管(同じレベル 0 の小刻み)と比べる。T-0208(全部 48 回)と T-0320(レベル 0 は 24 回・レベル 1 は 48 回)
    void MeasurePressureWave(uint32_t substeps, const GasTreeOptions& options, const char* name) {
        const std::array<GasTreeRegion, 1> region = {
            GasTreeRegion{.level = 1, .origin = {FINE_BEGIN * 2, 0, 0}, .size = {(FINE_END - FINE_BEGIN) * 2, 4, 4}}};
        GasBox box = MakePulseTube(substeps);
        GasTree tree = CopyIntoTree(box, region, options);
        const double amplitude = 0.01 * (double)box.referenceDerived[0].pressure;
        const int64_t base = box.referenceDerived[0].pressure;

        double reflected = 0.0;
        double transmitted = 0.0;
        double peak = 0.0;
        for (int tick = 0; tick < 5; ++tick) {
            StepGas(box);
            StepGasTree(tree);
        }

        for (uint32_t x = 0; x < TUBE_LENGTH; ++x) {
            if (x >= FINE_BEGIN && x < FINE_END)
                continue;

            const int64_t boxPressure = DeriveGasCell(box, box.cells[box.Index(x, 0, 0)], 0).pressure - base;
            const int32_t found = tree.Find(0, x, 0, 0);
            const int64_t treePressure = DeriveGasTreeCell(tree, (uint32_t)found).pressure - base;
            const double difference = std::fabs((double)(treePressure - boxPressure)) / amplitude;
            if (x < FINE_BEGIN)
                reflected = std::max(reflected, difference);
            else
                transmitted = std::max(transmitted, difference);

            peak = std::max(peak, (double)boxPressure / amplitude);
        }

        std::printf(
            "pressure wave through levels 0 -> 1 -> 0 after 5 ticks (%s): reflected = %.4f, transmitted error = %.4f "
            "(of the 1%% pulse), peak in the uniform tube = %.4f\n",
            name, reflected, transmitted, peak);
        EXPECT(peak > 0.5);  // 波は右の粗い所まで届いている
        EXPECT(reflected < 0.05);
        EXPECT(transmitted < 0.10);
    }

    void TestPressureWaveCrossesLevels() {
        MeasurePressureWave(48, {}, "same substeps 48");
        MeasurePressureWave(24, SUBCYCLED, "level 0: 24, level 1: 48");
    }

    // --- 小刻みを分けない(σ = 0)なら T-0208 と毎刻みビット一致(T-0320)---

    // 状態(セル・端数・帳簿)の FNV-1a を刻みごとに鎖にする(毎刻みの状態が全部同じ時だけ最後の値が同じ)
    uint64_t HashMix(uint64_t hash, uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            hash ^= (value >> (8 * i)) & 0xffu;
            hash *= 1099511628211ull;
        }

        return hash;
    }

    uint64_t HashQ64(uint64_t hash, const GasQ64& value) {
        return HashMix(HashMix(hash, (uint64_t)value.whole), value.fraction);
    }

    uint64_t HashTreeState(uint64_t hash, const GasTree& tree) {
        for (size_t i = 0; i < tree.cells.size(); ++i) {
            const GasCell& cell = tree.cells[i];
            const GasTreeFraction& fraction = tree.fractions[i];
            for (uint32_t s = 0; s < GAS_MAX_SPECIES; ++s)
                hash = HashMix(HashMix(hash, cell.amounts[s]), fraction.amounts[s]);

            hash = HashMix(HashMix(hash, (uint64_t)cell.energy), fraction.energy);
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                hash = HashMix(HashMix(hash, (uint64_t)cell.momentum[axis]), fraction.momentum[axis]);
        }

        for (uint32_t s = 0; s < GAS_MAX_SPECIES; ++s)
            hash = HashQ64(hash, tree.ledger.amounts[s]);

        hash = HashQ64(hash, tree.ledger.energy);
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
            hash = HashQ64(hash, tree.ledger.momentum[axis]);

        return hash;
    }

    uint64_t HashTicks(GasTree tree, int ticks) {
        uint64_t hash = 14695981039346656037ull;
        for (int tick = 0; tick < ticks; ++tick) {
            StepGasTree(tree);
            hash = HashTreeState(hash, tree);
        }

        return hash;
    }

    // T-0208 の実装(小刻みを分ける前)で測った値(2026-10-11)
    constexpr uint64_t T0208_CLOSED_BOX_HASH = 0x968f764677303a49ull;

    void TestNoSubcycleMatchesT0208() {
        const uint64_t closedBox = HashTicks(MakeDisturbedTwoLevelBox(), 12);
        std::printf("no subcycling: closed two-level box hash chain (12 ticks) = 0x%016llx (T-0208: 0x%016llx)\n",
                    (unsigned long long)closedBox, (unsigned long long)T0208_CLOSED_BOX_HASH);
        EXPECT(closedBox == T0208_CLOSED_BOX_HASH);

        // 基準がいちばん細かいレベル以上なら σ = 0(分けない)
        const GasTreeOptions aboveFinest = {.subcycleBaseLevel = 1, .maxSubcycleGap = 3};
        EXPECT(HashTicks(MakeDisturbedTwoLevelBox(TwoLevelConfig(), aboveFinest), 12) == T0208_CLOSED_BOX_HASH);
    }

}  // namespace

int main() {
    TestSingleLevelMatchesReference();
    TestStaticAtmosphereAcrossLevels();
    TestClosedBoxConservationAcrossLevels();
    TestPressureWaveCrossesLevels();
    TestNoSubcycleMatchesT0208();

    if (failureCount != 0) {
        std::printf("gas_tree_test: %d failure(s)\n", failureCount);
        return 1;
    }

    std::printf("gas_tree_test: all passed\n");
    return 0;
}
