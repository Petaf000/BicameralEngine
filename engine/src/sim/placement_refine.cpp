// placement_refine.cpp — 細かくする時の「置かれ方で配る」の CPU リファレンス(T-1157)。何をするかは placement_refine.h。
#include "sim/placement_refine.h"

#include "common/fixed.hlsli"
#include "core/aliases.h"

namespace bicameral::sim {

    using namespace state_field;

    namespace {

        // 列の和を子へ同じ数で(端数は一番下の子へ。wt3 の SpreadEvenly)
        void SpreadEvenly(int64_t total, uint32_t index, std::vector<std::vector<int64_t>>& children) {
            const auto count = (int64_t)children.size();
            for (std::vector<int64_t>& child : children)
                child[index] = total / count;

            children.front()[index] += total % count;
        }

        // 量の束 masses を、詰まり fullness・傾き slope で n 列へ分ける(列 j の重み = 2n r + g (2j + 1 − n)。端数は最後の列へ)
        std::vector<std::vector<int64_t>> SplitColumns(std::span<const int64_t> masses, int64_t fullnessQ16,
                                                       int64_t slopeQ16, uint32_t columns) {
            std::vector<std::vector<int64_t>> split(columns, std::vector<int64_t>(masses.size(), 0));
            if (columns == 1) {
                split[0].assign(masses.begin(), masses.end());
                return split;
            }

            const int64_t slope = SfClampColumnSlopeQ16(fullnessQ16, slopeQ16, columns);
            std::vector<int64_t> weight(columns, 0);
            int64_t weightSum = 0;
            for (uint32_t j = 0; j < columns; ++j) {
                weight[j] = SfColumnWeight(fullnessQ16, slope, columns, j);
                weightSum += weight[j];
            }

            // 詰まりが Q16 の 1 目盛りに届かない(凝縮相がごくわずか)なら同じ重み(wt3 の場面では起きない形)
            if (weightSum <= 0) {
                weight.assign(columns, 1);
                weightSum = columns;
            }

            for (size_t index = 0; index < masses.size(); ++index) {
                int64_t given = 0;
                for (uint32_t j = 0; j + 1 < columns; ++j) {
                    split[j][index] = fx::FxMulDivS64(masses[index], (uint64_t)weight[j], (uint64_t)weightSum);
                    given += split[j][index];
                }

                split[columns - 1][index] = masses[index] - given;
            }

            return split;
        }

        // 量の束の詰まり(器 = 子の体積 × 子の数)
        int64_t FullnessOf(std::span<const PlaceMaterial> materials, std::span<const int64_t> masses,
                           uint64_t capacityMm3) {
            const SfSettleSum sum = PlaceSettleSum(materials, masses);

            return SfSettleFullnessQ16(SfSettledVolume(sum), capacityMm3, sum.anchored);
        }

        // 気体を子の空きの体積で配り直す(凝縮相を配った後)
        void SpreadGasToOpen(std::span<const PlaceMaterial> materials, const PlaceCell& cell, uint64_t childVolumeMm3,
                             PlacedChildren& children) {
            const PlaceWeights weights = PlaceChildWeights(materials, children, childVolumeMm3);
            const auto count = (uint32_t)children.massMg.size();
            for (uint32_t index = 0; index < materials.size(); ++index) {
                if (materials[index].kind != SF_SETTLE_GAS)
                    continue;

                uint64_t weightSum = 0;
                for (const uint64_t weight : weights.open)
                    weightSum += weight;

                if (weightSum == 0)
                    continue;  // 空きが無い(気体の量は同じ数のまま)

                const int64_t total = cell.massMg[index];
                int64_t given = 0;
                for (uint32_t child = 0; child + 1 < count; ++child) {
                    children.massMg[child][index] = fx::FxMulDivS64(total, weights.open[child], weightSum);
                    given += children.massMg[child][index];
                }

                children.massMg[count - 1][index] = total - given;
            }
        }

    }  // namespace

    SfSettleSum PlaceSettleSum(std::span<const PlaceMaterial> materials, std::span<const int64_t> massMg) {
        SfSettleSum sum = SfMakeSettleSum();
        for (size_t index = 0; index < materials.size(); ++index) {
            const PlaceMaterial& material = materials[index];
            sum = SfAddSettleOf(sum, material.kind, material.densityKgPerM3, material.packedFractionPct,
                                material.molarVolumeMm3PerMol, massMg[index]);
        }

        return sum;
    }

    std::vector<std::vector<int64_t>> PlaceSettleColumn(std::span<const PlaceMaterial> materials,
                                                        std::span<const int64_t> columnMassMg, uint32_t childCount,
                                                        uint64_t childVolumeMm3) {
        std::vector<std::vector<int64_t>> children(childCount, std::vector<int64_t>(materials.size(), 0));
        if (childCount == 0)
            return children;

        const SfSettleSum sum = PlaceSettleSum(materials, columnMassMg);
        const uint64_t settled = SfSettledVolume(sum);
        const bool settles = SfSettles(settled, childVolumeMm3 * childCount, sum.anchored);
        const uint32_t top = settles ? SfSettleTopChild(settled, childVolumeMm3) : 0;
        for (uint32_t index = 0; index < materials.size(); ++index) {
            const uint32_t kind = materials[index].kind;
            const int64_t total = columnMassMg[index];
            if (!settles || kind == SF_SETTLE_GAS || kind == SF_SETTLE_ANCHORED) {
                SpreadEvenly(total, index, children);
                continue;
            }

            int64_t given = 0;
            for (uint32_t child = 0; child < top; ++child) {
                children[child][index] = SfSettleBelowShare(total, childVolumeMm3, settled);
                given += children[child][index];
            }

            children[top][index] = total - given;
        }

        return children;
    }

    PlacedChildren PlaceRefineCell(std::span<const PlaceMaterial> materials, const PlaceCell& cell, uint32_t columns,
                                   uint32_t depth, uint64_t childVolumeMm3) {
        PlacedChildren placed;
        placed.columns = columns;
        placed.depth = depth;
        placed.massMg.assign((size_t)columns * columns * depth, std::vector<int64_t>(materials.size(), 0));

        // --- 寄せるか(物・据わる固体・凝縮相が無い・詰まり切ったは同じ数で)---
        const SfSettleSum sum = PlaceSettleSum(materials, cell.massMg);
        const uint64_t capacity = childVolumeMm3 * placed.massMg.size();
        placed.settled = !cell.blocked && SfSettles(SfSettledVolume(sum), capacity, sum.anchored);
        if (!placed.settled) {
            for (uint32_t index = 0; index < materials.size(); ++index) {
                const auto count = (int64_t)placed.massMg.size();
                for (std::vector<int64_t>& child : placed.massMg)
                    child[index] = cell.massMg[index] / count;

                placed.massMg.front()[index] += cell.massMg[index] % count;
            }

            return placed;
        }

        // --- 横: x で分け、(3 次元なら)z で分ける。列ごとに縦へ寄せる ---
        const int64_t fullness = SfSettleFullnessQ16(SfSettledVolume(sum), capacity, false);
        const auto slabs = SplitColumns(cell.massMg, fullness, cell.slopeXQ16, columns);
        for (uint32_t x = 0; x < columns; ++x) {
            const uint64_t slabCapacity = childVolumeMm3 * columns * depth;
            const int64_t slabFullness = FullnessOf(materials, slabs[x], slabCapacity);
            const auto lines = SplitColumns(slabs[x], slabFullness, cell.slopeZQ16, depth);
            for (uint32_t z = 0; z < depth; ++z) {
                const auto rows = PlaceSettleColumn(materials, lines[z], columns, childVolumeMm3);
                for (uint32_t row = 0; row < columns; ++row)
                    placed.massMg[placed.ChildIndex(x, z, row)] = rows[row];
            }
        }

        SpreadGasToOpen(materials, cell, childVolumeMm3, placed);

        return placed;
    }

    PlaceWeights PlaceChildWeights(std::span<const PlaceMaterial> materials, const PlacedChildren& children,
                                   uint64_t childVolumeMm3) {
        PlaceWeights weights;
        for (const std::vector<int64_t>& child : children.massMg) {
            uint64_t condensedMass = 0;
            uint64_t condensedVolume = 0;
            for (size_t index = 0; index < materials.size(); ++index) {
                const PlaceMaterial& material = materials[index];
                if (material.kind == SF_SETTLE_GAS || child[index] <= 0)
                    continue;

                condensedMass += (uint64_t)child[index];
                condensedVolume += SfMaterialVolume(child[index], material.densityKgPerM3,
                                                    material.molarVolumeMm3PerMol);
            }

            weights.condensed.push_back(condensedMass);
            weights.open.push_back(SfOpenVolume(childVolumeMm3, condensedVolume));
        }

        return weights;
    }

    std::vector<int64_t> PlaceSplitValue(int64_t parentValue, std::span<const uint64_t> weights) {
        const auto count = (uint32_t)weights.size();
        uint64_t weightSum = 0;
        for (const uint64_t weight : weights)
            weightSum += weight;

        std::vector<int64_t> children(count, parentValue);
        if (weightSum == 0 || count == 0)
            return children;

        // 残りは重みのある最後の子へ(重みの無い子に端数を入れない)
        uint32_t last = 0;
        for (uint32_t child = 0; child < count; ++child)
            last = weights[child] > 0 ? child : last;

        int64_t given = 0;
        for (uint32_t child = 0; child < count; ++child) {
            if (child == last)
                continue;

            children[child] = SfPlacedValue(parentValue, weights[child], weightSum, count);
            given += children[child];
        }

        children[last] = SfPlacedLastValue(parentValue, given, count);

        return children;
    }

}  // namespace bicameral::sim
