// gas_base_layers.cpp — 基準の層を粗さをまたいで整数の和が合う形で作る・粗くする・起こす(T-0953・ADR-0431。形と理由は gas_base_layers.h)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#include "sim/gas_base_layers.h"

#include <algorithm>
#include <cassert>

namespace bicameral::sim {

    namespace {

        // 1 層の数(成分ごとの量 µmol・最後にエネルギー mJ)。段の和と分けるのはこの並びで、成分とエネルギーを同じ規則で扱う
        using LayerValues = std::vector<int64_t>;

        // 切り捨ての割り算(負の値も下へ。divisor > 0)
        int64_t FloorDivide(int64_t value, int64_t divisor) {
            const int64_t quotient = value / divisor;

            return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
        }

        LayerValues ToValues(const GasConfig& config, const GasCell& cell) {
            LayerValues values;
            values.reserve(config.speciesCount + 1);
            for (uint32_t s = 0; s < config.speciesCount; ++s)
                values.push_back((int64_t)cell.amounts[s]);

            values.push_back(cell.energy);

            return values;
        }

        // 基準の層は止まっている(運動量 0)
        GasCell FromValues(const GasConfig& config, const LayerValues& values) {
            GasCell cell;
            for (uint32_t s = 0; s < config.speciesCount; ++s) {
                assert(values[s] >= 0);
                cell.amounts[s] = (uint64_t)values[s];
            }

            cell.energy = values[config.speciesCount];

            return cell;
        }

        // 段 d の理想 = 細かい層の和 ÷ 2^d の切り捨て
        int64_t Ideal(const LayerValues& sum, uint32_t component, uint32_t level) {
            return FloorDivide(sum[component], (int64_t)1 << level);
        }

        // 親の段の値を子の 2 層へ分ける: 子の理想を置き、「2 × 親 − 子の理想の和」(小さな差)を半分ずつ足す。
        // 下の層 + 上の層 = 2 × 親(整数で一致)
        std::vector<LayerValues> SplitLevel(const std::vector<LayerValues>& parents,
                                            const std::vector<LayerValues>& childSums, uint32_t childLevel) {
            std::vector<LayerValues> children(parents.size() * 2);
            for (size_t j = 0; j < parents.size(); ++j) {
                LayerValues& lower = children[2 * j];
                LayerValues& upper = children[(2 * j) + 1];
                lower.resize(parents[j].size());
                upper.resize(parents[j].size());
                for (uint32_t c = 0; c < parents[j].size(); ++c) {
                    const int64_t twice = 2 * parents[j][c];
                    const int64_t lowerIdeal = Ideal(childSums[2 * j], c, childLevel);
                    const int64_t upperIdeal = Ideal(childSums[(2 * j) + 1], c, childLevel);
                    lower[c] = lowerIdeal + FloorDivide(twice - lowerIdeal - upperIdeal, 2);
                    upper[c] = twice - lower[c];
                }
            }

            return children;
        }

        // 段ごとの細かい層の和(下の段から。段 d の層 j = 段 0 の層 j × 2^d 〜 の 2^d 層の和)
        std::vector<std::vector<LayerValues>> SumLevels(const GasConfig& config, std::span<const GasCell> finest,
                                                        uint32_t coarserLevels) {
            std::vector<std::vector<LayerValues>> sums(coarserLevels + 1);
            for (const GasCell& cell : finest)
                sums[0].push_back(ToValues(config, cell));

            for (uint32_t d = 1; d <= coarserLevels; ++d) {
                const std::vector<LayerValues>& below = sums[d - 1];
                for (size_t j = 0; j + 1 < below.size(); j += 2) {
                    LayerValues sum = below[j];
                    for (size_t c = 0; c < sum.size(); ++c)
                        sum[c] += below[j + 1][c];

                    sums[d].push_back(std::move(sum));
                }
            }

            return sums;
        }

    }  // namespace

    GasBaseLayers MakeGasBaseLayers(const GasConfig& finestConfig, std::span<const GasReferenceLayer> profile,
                                    uint64_t surfacePressureMicroPascal, uint32_t coarserLevels) {
        assert(profile.size() == finestConfig.sizeZ);
        assert(finestConfig.sizeZ % (1u << coarserLevels) == 0);
        assert(finestConfig.cellLevel - (int32_t)coarserLevels >= GAS_MIN_CELL_LEVEL);

        // --- 段 0 の理想 = 今と同じ静水圧の積み上げ ---
        const std::vector<GasCell> finest = MakeHydrostaticReference(finestConfig, profile, surfacePressureMicroPascal);
        const std::vector<std::vector<LayerValues>> sums = SumLevels(finestConfig, finest, coarserLevels);

        // --- いちばん粗い段は理想、その下は親を子の 2 層へ分ける(上から)---
        std::vector<std::vector<LayerValues>> values(coarserLevels + 1);
        for (const LayerValues& sum : sums[coarserLevels]) {
            LayerValues& top = values[coarserLevels].emplace_back(sum.size());
            for (uint32_t c = 0; c < sum.size(); ++c)
                top[c] = Ideal(sum, c, coarserLevels);
        }

        for (uint32_t d = coarserLevels; d > 0; --d)
            values[d - 1] = SplitLevel(values[d], sums[d - 1], d - 1);

        GasBaseLayers layers{.finestLevel = finestConfig.cellLevel};
        layers.levels.resize(coarserLevels + 1);
        for (uint32_t d = 0; d <= coarserLevels; ++d) {
            for (const LayerValues& layer : values[d])
                layers.levels[d].push_back(FromValues(finestConfig, layer));
        }

        return layers;
    }

    std::vector<GasCell> GasBaseLevelReference(const GasBaseLayers& layers, int32_t level, uint32_t firstLayer,
                                               uint32_t count) {
        const int32_t coarseness = layers.finestLevel - level;
        assert(coarseness >= 0 && coarseness < (int32_t)layers.levels.size());
        const std::vector<GasCell>& column = layers.levels[(size_t)coarseness];
        assert((size_t)firstLayer + count <= column.size());

        return {column.begin() + firstLayer, column.begin() + firstLayer + count};
    }

    GasCell CoarsenGasCells(const GasConfig& config, std::span<const GasCell, 8> children,
                            GasCoarsenRemainder& remainder) {
        GasCell parent;

        // --- 成分(和 ÷ 8 の切り捨て・端数は帳簿へ)---
        for (uint32_t s = 0; s < config.speciesCount; ++s) {
            uint64_t sum = 0;
            for (const GasCell& child : children)
                sum += child.amounts[s];

            parent.amounts[s] = sum / 8;
            remainder.amounts[s] = sum - (8 * parent.amounts[s]);
        }

        // --- エネルギー・運動量(負もあるので下へ切り捨て)---
        int64_t energy = 0;
        std::array<int64_t, GAS_AXES> momentum = {};
        for (const GasCell& child : children) {
            energy += child.energy;
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                momentum[axis] += child.momentum[axis];
        }

        parent.energy = FloorDivide(energy, 8);
        remainder.energy = energy - (8 * parent.energy);
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
            parent.momentum[axis] = FloorDivide(momentum[axis], 8);
            remainder.momentum[axis] = momentum[axis] - (8 * parent.momentum[axis]);
        }

        return parent;
    }

    std::array<GasCell, 8> RefineGasCell(const GasConfig& config, const GasCell& parent, const GasCell& parentReference,
                                         const GasCell& lowerReference, const GasCell& upperReference) {
        // --- 基準からのずれで配れるか(どの子の成分も負にならない)---
        bool byDeviation = true;
        for (uint32_t s = 0; s < config.speciesCount; ++s) {
            const int64_t deviation = (int64_t)parent.amounts[s] - (int64_t)parentReference.amounts[s];
            const int64_t lowest = std::min((int64_t)lowerReference.amounts[s], (int64_t)upperReference.amounts[s]);
            if (lowest + deviation < 0)
                byDeviation = false;
        }

        // --- 配る(運動量は密度なので同じ数。ずれで配る時の基準の運動量は 0)---
        std::array<GasCell, 8> children;
        for (uint32_t i = 0; i < 8; ++i) {
            GasCell& child = children[i];
            child.momentum = parent.momentum;
            if (!byDeviation) {
                child.amounts = parent.amounts;
                child.energy = parent.energy;
                continue;
            }

            const GasCell& reference = i < 4 ? lowerReference : upperReference;
            for (uint32_t s = 0; s < config.speciesCount; ++s)
                child.amounts[s] = reference.amounts[s] + parent.amounts[s] - parentReference.amounts[s];

            child.energy = reference.energy + parent.energy - parentReference.energy;
        }

        return children;
    }

}  // namespace bicameral::sim
