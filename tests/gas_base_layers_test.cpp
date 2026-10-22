// gas_base_layers_test.cpp — 基準の層を粗さをまたいで整数の和が合う形で作る(sim/gas_base_layers。T-0953・ADR-0431)の CPU の試験。
// 10 km の空気の柱(段 0 = 0.5 m × 20000 層・粗い段 5 つ = 1〜16 m。気温は 6.5 K/km で下がる。T-0900 M-0・05 §5.1 S48):
// 1. 和: どの段のどの層も、量(成分ごと)とエネルギーが「2 × 粗い層 = 細かい 2 層の和」で整数で一致。粗い段が無ければ段 0 は
//    MakeHydrostaticReference とビット単位で同じ。段 0 の理想(静水圧の積み上げ)からのずれは段の数 + 1 単位以内。
//    導く圧力の「粗い層 − 細かい 2 層の平均」の最大(段差。温度の 1 mK の丸めの大きさ)を記録する。
// 2. 粗くする: 静かな細かい 8 セルを粗くすると、端数 0 で粗い段の基準そのもの(全部の段・全部の層)。
// 3. 静かな柱: 地表 0.5 m・上空は粗く(0〜32 m は 0.5 m、2 倍の高さごとに 1 段粗く、512 m〜10 km は 16 m)の帯ごとの箱を、粗くした状態から
//    陰の圧力(T-0845 の形)で ticks 刻み: 速さ 0・p̃ と解いた x の揺れ 0・境の両側の基準との質量の差 0・保存 0・2 回でハッシュ一致。
// 4. 起こす: 3 の後の粗い帯を 1 段細かく起こすと子の基準そのもの(Σ 子 = 8 × 親)。起こした帯を ticks 刻み: 速さ 0・揺れ 0。
//    基準からずらした親・大きく減った親(同じ数で配る道)でも Σ 子 = 8 × 親・粗くし直すと親に戻る。
// 5. 見張り(試験が効いている印): 前の作り(段ごとに別に積み上げた基準)で粗い帯を回すと動く。
// 引数: [--ticks n](既定 600)
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gas_projection_scenes.h"
#include "sim/gas_base_layers.h"
#include "sim/gas_projection.h"
#include "sim/gas_reference.h"

using namespace bicameral;
using namespace bicameral::gas_scenes;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    // --- 柱の形 ---
    constexpr uint32_t FINE_LAYERS = 20000;   // 0.5 m × 20000 = 10 km
    constexpr uint32_t COARSER_LEVELS = 5;    // 1・2・4・8・16 m
    constexpr uint32_t BAND_WIDTH = 2;        // 帯の箱の横(周期)
    constexpr uint32_t LAPSE_NUMERATOR = 13;  // 気温の下がり 6.5 K/km = 3.25 mK/層
    constexpr uint32_t LAPSE_DENOMINATOR = 4;

    // 帯(段 d の層 first から count 層)。段 0 は 0〜32 m、段 d ≥ 1 は 32 × 2^(d−1) m 〜 の倍の高さまで、いちばん粗い段は 10 km まで
    struct Band {
        uint32_t coarseness = 0;
        uint32_t first = 0;
        uint32_t count = 0;
    };

    std::vector<Band> MakeBands() {
        std::vector<Band> bands = {{.coarseness = 0, .first = 0, .count = 64}};
        for (uint32_t d = 1; d < COARSER_LEVELS; ++d)
            bands.push_back({.coarseness = d, .first = 32, .count = 32});

        bands.push_back({.coarseness = COARSER_LEVELS, .first = 32, .count = (FINE_LAYERS >> COARSER_LEVELS) - 32});

        return bands;
    }

    sim::GasConfig LevelConfig(uint32_t coarseness, uint32_t width, uint32_t layers) {
        sim::GasConfig config = MakeAirConfig(width);
        config.sizeZ = layers;
        config.cellLevel = -(int32_t)coarseness;
        config.boundary = {sim::GasBoundary::Periodic, sim::GasBoundary::Periodic, sim::GasBoundary::Wall};

        return config;
    }

    // 段 d の層 j の温度と成分(細かい層の真ん中の高さの値)
    std::vector<sim::GasReferenceLayer> MakeProfile(uint32_t coarseness) {
        std::vector<sim::GasReferenceLayer> profile;
        for (uint32_t j = 0; j < (FINE_LAYERS >> coarseness); ++j) {
            const uint32_t fine = (j << coarseness) + ((1u << coarseness) >> 1);
            sim::GasReferenceLayer layer{.temperatureMilliKelvin = AIR_TEMPERATURE -
                                                                   (fine * LAPSE_NUMERATOR / LAPSE_DENOMINATOR)};
            for (uint32_t s = 0; s < AIR_FRACTIONS_Q32.size(); ++s)
                layer.fractionsQ32[s] = AIR_FRACTIONS_Q32[s];

            profile.push_back(layer);
        }

        return profile;
    }

    bool SameCell(const sim::GasCell& a, const sim::GasCell& b) {
        return a.amounts == b.amounts && a.energy == b.energy && a.momentum == b.momentum;
    }

    int64_t Absolute(int64_t value) {
        return value < 0 ? -value : value;
    }

    sim::GasProjectionOptions T0845Options() {
        sim::GasProjectionOptions options;
        options.pressure.soundSpeedMmPerS = REAL_SOUND;
        options.fixedCycles = true;
        options.solver.cycles = 2;
        options.outerIterations = 1;

        return options;
    }

    // --- 1. 和 ---
    void TestSums(const sim::GasBaseLayers& layers) {
        const sim::GasConfig config = LevelConfig(0, 1, FINE_LAYERS);
        bool exact = true;
        int64_t worstStep = 0;
        for (uint32_t d = 1; d <= COARSER_LEVELS; ++d) {
            const std::vector<sim::GasCell>& coarse = layers.levels[d];
            const std::vector<sim::GasCell>& fine = layers.levels[d - 1];
            Expect(coarse.size() == FINE_LAYERS >> d, "和: 段の層の数");
            for (size_t j = 0; j < coarse.size(); ++j) {
                for (uint32_t s = 0; s < config.speciesCount; ++s)
                    exact = exact && 2 * coarse[j].amounts[s] == fine[2 * j].amounts[s] + fine[(2 * j) + 1].amounts[s];

                exact = exact && 2 * coarse[j].energy == fine[2 * j].energy + fine[(2 * j) + 1].energy;
            }

            // 導く圧力の段差(粗い層 − 細かい 2 層の平均)
            const sim::GasBox coarseBox = sim::MakeGasBox(LevelConfig(d, 1, (uint32_t)coarse.size()), coarse);
            const sim::GasBox fineBox = sim::MakeGasBox(LevelConfig(d - 1, 1, (uint32_t)fine.size()), fine);
            for (size_t j = 0; j < coarse.size(); ++j) {
                const int64_t mean = (fineBox.referenceDerived[2 * j].pressure +
                                      fineBox.referenceDerived[(2 * j) + 1].pressure) /
                                     2;
                worstStep = std::max(worstStep, Absolute(coarseBox.referenceDerived[j].pressure - mean));
            }
        }

        Expect(exact, "和: 2 × 粗い層 ≠ 細かい 2 層の和");
        Expect(worstStep <= 1000000, "和: 導く圧力の段差が 1 Pa を超えた");

        // --- 段 0 だけなら今の積み上げとビット単位で同じ・理想からのずれ ---
        const std::vector<sim::GasReferenceLayer> profile = MakeProfile(0);
        const std::vector<sim::GasCell> ideal = sim::MakeHydrostaticReference(config, profile, SURFACE_PRESSURE);
        const sim::GasBaseLayers single = sim::MakeGasBaseLayers(config, profile, SURFACE_PRESSURE, 0);
        bool same = single.levels.size() == 1;
        int64_t worstIdeal = 0;
        for (uint32_t z = 0; z < FINE_LAYERS; ++z) {
            same = same && SameCell(single.levels[0][z], ideal[z]);
            for (uint32_t s = 0; s < config.speciesCount; ++s)
                worstIdeal = std::max(worstIdeal,
                                      Absolute((int64_t)layers.levels[0][z].amounts[s] - (int64_t)ideal[z].amounts[s]));

            worstIdeal = std::max(worstIdeal, Absolute(layers.levels[0][z].energy - ideal[z].energy));
        }

        Expect(same, "和: 粗い段が無い時に段 0 が MakeHydrostaticReference と違う");
        Expect(worstIdeal <= COARSER_LEVELS + 1, "和: 段 0 の理想からのずれが段の数 + 1 を超えた");
        Log(Channel::Sim, Level::Info,
            "1. 和: 2 × 粗い = 細かい 2 層の和 {}・導く圧力の段差の最大 {} µPa・段 0 の理想からのずれ {} 単位",
            exact ? "一致" : "不一致", worstStep, worstIdeal);
    }

    // 横に同じ値の子 8 つ(下の層 4・上の層 4)
    std::array<sim::GasCell, 8> UniformChildren(const sim::GasCell& lower, const sim::GasCell& upper) {
        std::array<sim::GasCell, 8> children;
        for (uint32_t i = 0; i < 8; ++i)
            children[i] = i < 4 ? lower : upper;

        return children;
    }

    // --- 2. 静かな細かい柱を段ごとに粗くする(段 d の状態を返す)---
    std::vector<std::vector<sim::GasCell>> TestCoarsen(const sim::GasBaseLayers& layers) {
        const sim::GasConfig config = LevelConfig(0, 1, FINE_LAYERS);
        std::vector<std::vector<sim::GasCell>> states = {layers.levels[0]};
        bool exact = true;
        for (uint32_t d = 1; d <= COARSER_LEVELS; ++d) {
            const std::vector<sim::GasCell>& below = states[d - 1];
            std::vector<sim::GasCell> coarse;
            for (size_t j = 0; j + 1 < below.size(); j += 2) {
                const std::array<sim::GasCell, 8> children = UniformChildren(below[j], below[j + 1]);
                sim::GasCoarsenRemainder remainder;
                coarse.push_back(sim::CoarsenGasCells(config, children, remainder));
                exact = exact && SameCell(coarse.back(), layers.levels[d][j / 2]) &&
                        remainder.amounts == sim::GasCoarsenRemainder{}.amounts && remainder.energy == 0;
            }

            states.push_back(std::move(coarse));
        }

        Expect(exact, "粗くする: 静かな細かいセルを粗くしても粗い段の基準にならない(端数か値の違い)");
        Log(Channel::Sim, Level::Info, "2. 粗くする: 静かな 8 セル → 粗い段の基準 {}(端数 0)",
            exact ? "一致" : "不一致");

        return states;
    }

    // --- 3・4. 帯の箱を回す ---
    struct QuietRun {
        int64_t maxMomentum = 0;
        int64_t maxScaled = 0;  // p̃(µPa)
        int64_t maxSolved = 0;  // 解いた x(µPa)
        int64_t maxExcess = 0;  // 基準との質量の差(mg。いちばん下と上の層 = 境の両側)
        bool conserved = true;
        uint64_t hash = 0;
    };

    sim::GasBox MakeBandBox(const Band& band, std::span<const sim::GasCell> column,
                            std::vector<sim::GasCell> reference) {
        const sim::GasConfig config = LevelConfig(band.coarseness, BAND_WIDTH, band.count);
        sim::GasBox box = sim::MakeGasBox(config, std::move(reference));
        for (uint32_t index = 0; index < box.cells.size(); ++index)
            box.cells[index] = column[band.first + (index / (BAND_WIDTH * BAND_WIDTH))];

        return box;
    }

    void Watch(const sim::GasBox& box, const sim::GasProjection& state, QuietRun& run) {
        const uint32_t perLayer = BAND_WIDTH * BAND_WIDTH;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const uint32_t z = index / perLayer;
            for (const int64_t momentum : box.cells[index].momentum)
                run.maxMomentum = std::max(run.maxMomentum, Absolute(momentum));

            const sim::GasDerived derived = sim::DeriveGasCell(box, box.cells[index], z);
            run.maxScaled = std::max(run.maxScaled, Absolute(derived.scaledPressure));
            if (z == 0 || z + 1 == box.config.sizeZ) {
                const int64_t excess = (int64_t)derived.mass - (int64_t)box.referenceDerived[z].mass;
                run.maxExcess = std::max(run.maxExcess, Absolute(excess));
            }
        }

        for (const int64_t pressure : state.pressures)
            run.maxSolved = std::max(run.maxSolved, Absolute(pressure));
    }

    QuietRun StepQuiet(sim::GasBox box, uint32_t ticks) {
        const sim::GasProjectionOptions options = T0845Options();
        const sim::GasTotals before = sim::SumGas(box);
        sim::GasProjection state;
        QuietRun run;
        Watch(box, state, run);
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            (void)sim::StepGasProjection(box, state, options);
            Watch(box, state, run);
            const sim::GasTotals now = sim::SumGas(box);
            bool ok = now.amounts == before.amounts && now.energy == before.energy + box.ledger.energy;
            for (uint32_t axis = 0; axis < sim::GAS_AXES; ++axis)
                ok = ok && now.momentum[axis] == before.momentum[axis] + box.ledger.momentum[axis];

            run.conserved = run.conserved && ok;
        }

        run.hash = sim::HashGas(box);

        return run;
    }

    bool Quiet(const QuietRun& run) {
        return run.maxMomentum == 0 && run.maxScaled == 0 && run.maxSolved == 0 && run.maxExcess == 0 && run.conserved;
    }

    void TestWakeBand(const sim::GasBaseLayers& layers, const Band& band, uint32_t ticks);

    void TestQuietColumn(const sim::GasBaseLayers& layers, const std::vector<std::vector<sim::GasCell>>& states,
                         uint32_t ticks) {
        for (const Band& band : MakeBands()) {
            const int32_t level = -(int32_t)band.coarseness;
            const sim::GasBox box = MakeBandBox(band, states[band.coarseness],
                                                sim::GasBaseLevelReference(layers, level, band.first, band.count));
            Expect(SameCell(box.reference.front(), layers.levels[band.coarseness][band.first]),
                   "静かな柱: 箱の基準が基準の層と違う(GPU の箱が写す値)");

            const QuietRun first = StepQuiet(box, ticks);
            const QuietRun second = StepQuiet(box, ticks);
            Expect(Quiet(first), "静かな柱: 速さ・揺れ・境の質量の差・保存のどれかが 0 でない");
            Expect(first.hash == second.hash, "静かな柱: 2 回でハッシュが違う");
            Log(Channel::Sim, Level::Info,
                "3. 静かな柱 段 {}({} 層 × {} 刻み): 運動量の最大 {}・p̃ {} µPa・x {} µPa・境の質量の差 {} mg・保存 "
                "{}・決定性 {}",
                band.coarseness, band.count, ticks, first.maxMomentum, first.maxScaled, first.maxSolved,
                first.maxExcess, first.conserved ? "0" : "崩れ", first.hash == second.hash ? "一致" : "不一致");
            if (band.coarseness == 0)
                continue;

            TestWakeBand(layers, band, ticks);
        }
    }

    // 4. 粗い帯の全部のセルを 1 段細かく起こし、子の基準そのものか確かめて回す
    void TestWakeBand(const sim::GasBaseLayers& layers, const Band& band, uint32_t ticks) {
        const sim::GasConfig config = LevelConfig(0, 1, FINE_LAYERS);
        const std::vector<sim::GasCell>& coarse = layers.levels[band.coarseness];
        const std::vector<sim::GasCell>& fine = layers.levels[band.coarseness - 1];
        std::vector<sim::GasCell> woken(fine.size());
        bool exact = true;
        for (uint32_t j = band.first; j < band.first + band.count; ++j) {
            const std::array<sim::GasCell, 8> children = sim::RefineGasCell(config, coarse[j], coarse[j], fine[2 * j],
                                                                            fine[(2 * j) + 1]);
            exact = exact && SameCell(children[0], fine[2 * j]) && SameCell(children[7], fine[(2 * j) + 1]);
            woken[2 * j] = children[0];
            woken[(2 * j) + 1] = children[7];
        }

        const Band fineBand{.coarseness = band.coarseness - 1, .first = 2 * band.first, .count = 2 * band.count};
        const sim::GasBox box = MakeBandBox(
            fineBand, woken,
            sim::GasBaseLevelReference(layers, -(int32_t)fineBand.coarseness, fineBand.first, fineBand.count));
        const QuietRun run = StepQuiet(box, ticks);
        Expect(exact, "起こす: 静かな親から子の基準が出ない");
        Expect(Quiet(run), "起こす: 起こした帯が動いた");
        Log(Channel::Sim, Level::Info, "4. 起こす 段 {} → {}: 子 = 子の基準 {}・{} 刻みの運動量の最大 {}・p̃ {} µPa",
            band.coarseness, fineBand.coarseness, exact ? "一致" : "不一致", ticks, run.maxMomentum, run.maxScaled);
    }

    // 4. ずらした親・大きく減った親でも Σ 子 = 8 × 親・粗くし直すと親に戻る
    void TestRefineConserves(const sim::GasBaseLayers& layers) {
        const sim::GasConfig config = LevelConfig(0, 1, FINE_LAYERS);
        const uint32_t d = 3;
        const uint32_t j = 40;
        const sim::GasCell& reference = layers.levels[d][j];
        const sim::GasCell& lower = layers.levels[d - 1][2 * j];
        const sim::GasCell& upper = layers.levels[d - 1][(2 * j) + 1];

        sim::GasCell shifted = reference;
        shifted.amounts[SPECIES_N2] += 12345;
        shifted.amounts[SPECIES_SMOKE] += 777;
        shifted.energy -= 98765;
        shifted.momentum = {1001, -2002, 3003};

        sim::GasCell depleted = reference;
        depleted.amounts[SPECIES_N2] = 3;  // 子の基準より大きく減った(同じ数で配る道)

        for (const sim::GasCell& parent : {shifted, depleted}) {
            const std::array<sim::GasCell, 8> children = sim::RefineGasCell(config, parent, reference, lower, upper);
            sim::GasCoarsenRemainder remainder;
            const sim::GasCell back = sim::CoarsenGasCells(config, children, remainder);
            const bool zero = remainder.amounts == sim::GasCoarsenRemainder{}.amounts && remainder.energy == 0 &&
                              remainder.momentum == std::array<int64_t, sim::GAS_AXES>{};
            Expect(SameCell(back, parent) && zero, "起こす: Σ 子 ≠ 8 × 親(粗くし直して親に戻らない)");
        }
    }

    // --- 5. 見張り: 前の作り(段ごとに別に積み上げた基準)では粗くした静かな空気が動く ---
    void TestOldReferenceMoves(const sim::GasBaseLayers& layers, const std::vector<std::vector<sim::GasCell>>& states) {
        const uint32_t d = COARSER_LEVELS;
        const sim::GasConfig whole = LevelConfig(d, 1, FINE_LAYERS >> d);
        const std::vector<sim::GasCell> old = sim::MakeHydrostaticReference(whole, MakeProfile(d), SURFACE_PRESSURE);
        int64_t worstAmount = 0;
        for (size_t j = 0; j < old.size(); ++j)
            worstAmount = std::max(worstAmount, Absolute((int64_t)old[j].amounts[SPECIES_N2] -
                                                         (int64_t)layers.levels[d][j].amounts[SPECIES_N2]));

        const Band band = MakeBands().back();
        const std::vector<sim::GasCell> reference(old.begin() + band.first, old.begin() + band.first + band.count);
        const QuietRun run = StepQuiet(MakeBandBox(band, states[d], reference), 60);
        Expect(run.maxMomentum > 0, "見張り: 前の作りの基準でも動かない(試験が効いていない)");
        Log(Channel::Sim, Level::Info,
            "5. 見張り: 前の作り(段 {} を別に積み上げ)と粗くした値の差 最大 {} µmol・60 刻みの運動量の最大 {}・p̃ {} "
            "µPa",
            d, worstAmount, run.maxMomentum, run.maxScaled);
    }

    int Run(std::span<char*> arguments) {
        uint32_t ticks = 600;
        for (size_t i = 1; i + 1 < arguments.size(); i += 2) {
            if (std::string_view(arguments[i]) == "--ticks")
                ticks = (uint32_t)std::atoi(arguments[i + 1]);
        }

        const sim::GasBaseLayers layers = sim::MakeGasBaseLayers(LevelConfig(0, 1, FINE_LAYERS), MakeProfile(0),
                                                                 SURFACE_PRESSURE, COARSER_LEVELS);
        TestSums(layers);
        const std::vector<std::vector<sim::GasCell>> states = TestCoarsen(layers);
        TestQuietColumn(layers, states, ticks);
        TestRefineConserves(layers);
        TestOldReferenceMoves(layers, states);
        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "gas_base_layers_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "gas_base_layers_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, (size_t)argc));
    SingletonFinalizer::Finalize();

    return exitCode;
}
