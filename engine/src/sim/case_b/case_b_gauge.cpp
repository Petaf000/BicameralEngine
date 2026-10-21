// case_b_gauge.cpp — 案 B の試作の計器(B1。T-0867)。何をするかは case_b_gauge.h。
// 範囲は x・y の四角(z は全部)。水面・高さは y を上として数える(場面は y が上。重さの軸を変える場面は計器を足す)。
#include "sim/case_b/case_b_gauge.h"

#include <algorithm>
#include <format>

#include "common/fixed.hlsli"
#include "core/aliases.h"

namespace bicameral::sim::case_b {

    namespace {

        namespace fx = bicameral::fx;

        constexpr uint32_t Q16_ONE = 1u << 16;
        constexpr uint32_t SURFACE_Q16 = Q16_ONE / 2;
        constexpr uint32_t ANGLE_LIMIT_DEGREES = 89;

        // tan(d + 0.5°) の Q16(d = 0〜88)。角度は傾きがこの境目をいくつ超えたかで決める(浮動小数点を使わない)
        constexpr std::array<uint32_t, ANGLE_LIMIT_DEGREES> TAN_HALF_DEGREE_Q16 = {
            572,    1716,   2861,   4008,   5158,   6310,   7467,   8628,   9794,    10967,   12146,  13333,  14529,
            15734,  16949,  18175,  19413,  20663,  21928,  23208,  24503,  25815,   27146,   28496,  29866,  31259,
            32675,  34116,  35583,  37078,  38604,  40161,  41751,  43377,  45042,   46746,   48494,  50288,  52130,
            54024,  55973,  57981,  60053,  62191,  64402,  66690,  69061,  71520,   74075,   76733,  79502,  82390,
            85408,  88567,  91878,  95355,  99014,  102871, 106945, 111258, 115834,  120702,  125893, 131445, 137399,
            143806, 150722, 158218, 166373, 175284, 185068, 195866, 207854, 221246,  236315,  253409, 272977, 295614,
            322120, 353601, 391628, 438512, 497795, 575202, 680617, 832714, 1071504, 1501021, 2502719};

        int64_t FractionToUm(uint64_t sumQ16, uint32_t edgeUm) {
            return (int64_t)((sumQ16 * edgeUm) >> 16);
        }

        // 範囲の列ごとの、詰まった粒の高さ(µm)
        std::vector<int64_t> GrainHeights(const Box& box, const View& view, const BoxRegion& region) {
            const uint32_t edge = CellEdgeUm(box.config.level);
            std::vector<int64_t> heights;
            for (uint32_t x = region.xBegin; x < region.xEnd; ++x) {
                uint64_t sum = 0;
                for (uint32_t z = 0; z < box.config.sizeZ; ++z) {
                    for (uint32_t y = region.yBegin; y < region.yEnd; ++y)
                        sum += std::min(view.cells[CellIndex(box, x, y, z)].grainPackedQ16, Q16_ONE);
                }

                heights.push_back(FractionToUm(sum / box.config.sizeZ, edge));
            }

            return heights;
        }

        bool IsSurface(const Box& box, const View& view, uint32_t x, uint32_t y, uint32_t z) {
            if (view.cells[CellIndex(box, x, y, z)].liquidQ16 < SURFACE_Q16)
                return false;

            return y + 1 >= box.config.sizeY || view.cells[CellIndex(box, x, y + 1, z)].liquidQ16 < SURFACE_Q16;
        }

        // 範囲の質量・水位・圧力
        void MeasureCells(const Box& box, const View& view, const BoxRegion& region, RegionSample& sample) {
            const uint32_t edge = CellEdgeUm(box.config.level);
            uint64_t liquid = 0;
            int64_t pressure = 0;
            uint32_t cells = 0;
            for (uint32_t z = 0; z < box.config.sizeZ; ++z) {
                for (uint32_t y = region.yBegin; y < region.yEnd; ++y) {
                    for (uint32_t x = region.xBegin; x < region.xEnd; ++x) {
                        const uint32_t cell = CellIndex(box, x, y, z);
                        const CellView& measured = view.cells[cell];
                        for (uint32_t phase = 0; phase < MOVING_PHASE_COUNT; ++phase)
                            sample.phaseMassMg[phase] += measured.phaseMassMg[phase];

                        liquid += measured.liquidQ16;
                        pressure += box.pressureMicroPascal[cell];
                        ++cells;
                        if (!IsSurface(box, view, x, y, z))
                            continue;

                        const int64_t value = box.pressureMicroPascal[cell];
                        sample.surfaceMinPressureMicroPascal = sample.surfaceCells == 0
                                                                   ? value
                                                                   : std::min(sample.surfaceMinPressureMicroPascal,
                                                                              value);
                        sample.surfaceMaxPressureMicroPascal = sample.surfaceCells == 0
                                                                   ? value
                                                                   : std::max(sample.surfaceMaxPressureMicroPascal,
                                                                              value);
                        ++sample.surfaceCells;
                    }
                }
            }

            const uint64_t columns = (uint64_t)(region.xEnd - region.xBegin) * box.config.sizeZ;
            sample.liquidLevelUm = columns == 0 ? 0 : FractionToUm(liquid / columns, edge);
            sample.meanPressureMicroPascal = cells == 0 ? 0 : pressure / cells;
        }

    }  // namespace

    RegionSample MeasureRegion(const Box& box, const View& view, const BoxRegion& region) {
        RegionSample sample;
        MeasureCells(box, view, region, sample);

        const std::vector<int64_t> heights = GrainHeights(box, view, region);
        int64_t total = 0;
        for (const int64_t height : heights)
            total += height;

        sample.grainLevelUm = heights.empty() ? 0 : total / (int64_t)heights.size();
        sample.reposeAngleDegrees = SlopeAngleDegrees(heights, CellEdgeUm(box.config.level));

        return sample;
    }

    GaugeSample MeasureSample(const Box& box, std::span<const BoxRegion> regions) {
        const View view = BuildView(box);
        GaugeSample sample;
        sample.tick = box.tick;
        sample.hash = HashBox(box);
        sample.conserved = CheckConservation(box).IsZero();

        const uint32_t slots = SlotCount(box.config.layout);
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            for (uint32_t axis = 0; axis < 3; ++axis) {
                if (!Neighbor(box, cell, axis).has_value())
                    continue;

                for (uint32_t slot = 0; slot < slots; ++slot) {
                    const int64_t speed = (int64_t)fx::FxAbsU64(
                        FaceVelocity(box, view, {.axis = axis, .cell = cell, .slot = slot}));
                    sample.maxSpeed = std::max(sample.maxSpeed, speed);
                    sample.movingFaces += speed >= MOVING_SPEED ? 1u : 0u;
                    ++sample.totalFaces;
                }
            }
        }

        for (const BoxRegion& region : regions)
            sample.regions.push_back(MeasureRegion(box, view, region));

        return sample;
    }

    Recording RunScene(Box& box, const Stages& stages, const PressureSolver& pressureSolver, uint64_t ticks,
                       std::span<const BoxRegion> regions, const std::function<void(Box&)>& afterTick) {
        Recording recording;
        recording.cellCount = CellCount(box);
        recording.samples.push_back(MeasureSample(box, regions));
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            const auto begin = chr::steady_clock::now();
            StepBox(box, stages, pressureSolver);
            const auto elapsed = chr::duration_cast<chr::nanoseconds>(chr::steady_clock::now() - begin);
            if (afterTick)
                afterTick(box);

            GaugeSample sample = MeasureSample(box, regions);
            sample.stepNanoseconds = elapsed.count();
            recording.samples.push_back(std::move(sample));
        }

        return recording;
    }

    int64_t Recording::NanosecondsPerCellTick() const {
        if (samples.size() < 2 || cellCount == 0)
            return 0;

        int64_t total = 0;
        for (size_t index = 1; index < samples.size(); ++index)
            total += samples[index].stepNanoseconds;

        return total / ((int64_t)(samples.size() - 1) * cellCount);
    }

    bool Recording::AllConserved() const {
        return rng::all_of(samples, [](const GaugeSample& sample) { return sample.conserved; });
    }

    uint64_t Recording::HashChain() const {
        uint64_t hash = 0;
        for (const GaugeSample& sample : samples)
            hash = fx::FxHashCombine(hash, sample.hash);

        return hash;
    }

    // --- 刻みの列 ---
    std::vector<int64_t> RegionSeries(const Recording& recording, uint32_t region, int64_t RegionSample::* field) {
        std::vector<int64_t> series;
        series.reserve(recording.samples.size());
        for (const GaugeSample& sample : recording.samples)
            series.push_back(sample.regions[region].*field);

        return series;
    }

    uint32_t CountReversals(std::span<const int64_t> series, int64_t threshold) {
        if (series.empty())
            return 0;

        // 最後の山か谷(extreme)から threshold を超えて戻ったら 1 回(最初の向きが決まるのは数えない)
        uint32_t reversals = 0;
        int direction = 0;
        int64_t extreme = series[0];
        for (const int64_t value : series) {
            if (direction == 0) {
                if (value - extreme > threshold || extreme - value > threshold) {
                    direction = value > extreme ? 1 : -1;
                    extreme = value;
                }

                continue;
            }

            const bool further = direction > 0 ? value > extreme : value < extreme;
            const int64_t back = direction > 0 ? extreme - value : value - extreme;
            if (further)
                extreme = value;
            else if (back > threshold) {
                ++reversals;
                direction = -direction;
                extreme = value;
            }
        }

        return reversals;
    }

    std::optional<uint64_t> SettleTick(std::span<const int64_t> series, int64_t target, int64_t tolerance) {
        std::optional<uint64_t> settled;
        for (uint64_t tick = 0; tick < series.size(); ++tick) {
            const bool inside = (int64_t)fx::FxAbsU64(series[tick] - target) <= tolerance;
            if (!inside)
                settled.reset();
            else if (!settled.has_value())
                settled = tick;
        }

        return settled;
    }

    std::optional<uint64_t> FirstTickAtLeast(std::span<const int64_t> series, int64_t threshold) {
        for (uint64_t tick = 0; tick < series.size(); ++tick) {
            if (series[tick] >= threshold)
                return tick;
        }

        return std::nullopt;
    }

    Peak PeakOf(std::span<const int64_t> series) {
        Peak peak;
        for (uint64_t tick = 0; tick < series.size(); ++tick) {
            if (tick == 0 || series[tick] > peak.value)
                peak = {.value = series[tick], .tick = tick};
        }

        return peak;
    }

    int64_t DifferencePermille(int64_t a, int64_t b) {
        const uint64_t larger = std::max(fx::FxAbsU64(a), fx::FxAbsU64(b));
        if (larger == 0)
            return 0;

        return (int64_t)fx::FxDivU128By64(fx::FxMulU64Full(fx::FxAbsU64(a - b), 1000), larger).quotient;
    }

    uint32_t AngleFromSlopeQ16(uint64_t slopeQ16) {
        uint32_t degrees = 0;
        while (degrees < ANGLE_LIMIT_DEGREES && slopeQ16 >= TAN_HALF_DEGREE_Q16[degrees])
            ++degrees;

        return degrees;
    }

    uint32_t SlopeAngleDegrees(std::span<const int64_t> heightsUm, uint32_t columnWidthUm) {
        if (heightsUm.empty())
            return 0;

        const int64_t top = *rng::max_element(heightsUm);
        if (top <= 0)
            return 0;

        // 頂上の平らな所の両端(最初と最後の最も高い列)
        const auto count = (int64_t)heightsUm.size();
        int64_t firstTop = 0;
        while (heightsUm[(size_t)firstTop] != top)
            ++firstTop;

        int64_t lastTop = count - 1;
        while (heightsUm[(size_t)lastTop] != top)
            --lastTop;

        // 片側: 頂上の端から外へ、高さが 0 になる手前の列まで(無ければ箱の端まで)の平均の傾き
        const auto side = [&](int64_t start, int64_t step) -> uint32_t {
            int64_t last = start;
            for (int64_t index = start + step; index >= 0 && index < count; index += step) {
                if (heightsUm[(size_t)index] <= 0)
                    break;

                last = index;
            }

            const auto run = (uint64_t)((last > start ? last - start : start - last) * columnWidthUm);
            if (run == 0)
                return ANGLE_LIMIT_DEGREES + 1;

            const auto drop = (uint64_t)(top - heightsUm[(size_t)last]);

            return AngleFromSlopeQ16((drop << 16) / run);
        };

        return std::max(side(firstTop, -1), side(lastTop, 1));
    }

    std::string FormatFixed(int64_t value, uint32_t decimals) {
        uint64_t scale = 1;
        for (uint32_t i = 0; i < decimals; ++i)
            scale *= 10;

        const uint64_t magnitude = fx::FxAbsU64(value);
        const char* sign = value < 0 ? "-" : "";
        if (decimals == 0)
            return std::format("{}{}", sign, magnitude);

        return std::format("{}{}.{:0{}}", sign, magnitude / scale, magnitude % scale, decimals);
    }

}  // namespace bicameral::sim::case_b
