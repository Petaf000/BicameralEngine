// equilibrium_block.cpp — 成分を持つ節の釣り合いの CPU の回し方(T-0919。何をするかは equilibrium_block.h)。
// 1. 段 0: 面の 3 つのブロックを行(自分の行・隣の列)と対角へ。固定した面は対角へ
// 2. 粗い段: 子の座標 ÷ 2 を親に。親の係数 = Σ Pᵀ(子の八分割)A P(相手の八分割)を 128bit で厳密に足してから 1 回だけ ÷ 2^32
//    (P が Q16 の 2 つ。和が厳密なので粗い係数はちょうど対称)。斜めの面も軸の面も同じ式で親の面・対角になる
// 3. V サイクル: 8 色のブロックの Gauss–Seidel(節ごとの対角のブロックを LDLᵀ で解く)→ 残差を Pᵀ で親へ → 親で直しを解く
//    → P で子へ足す → 8 色の Gauss–Seidel。色の中の節どうしは隣でないので、同じ色の中の順は結果に効かない(R2)
// 数の幅(04 R8): 係数 × 値は 128bit で足してから右へずらす(0 方向へ切り捨て)。桁あふれは FX_ASSERT。
#include "sim/equilibrium_block.h"

#include <algorithm>
#include <map>
#include <tuple>

using namespace bicameral::fx;

namespace bicameral::sim {

    namespace {

        constexpr uint32_t COLOR_COUNT = 8;
        constexpr size_t MAX_LEVELS = 32;
        constexpr uint32_t
            PIVOT_FLOOR_SHIFT = 20;  // 対角のこの分の 1 より小さい消去の後の対角は、支えの無い向きとして解かない
        constexpr uint32_t ROW_WIDTH = EQ_MAX_COMPONENTS;  // ブロックの行の幅

        // --- 符号つきの 128bit(2 の補数。和を厳密に持ってから 1 回だけずらす)---
        FxU128 WideZero() {
            return {.hi = 0, .lo = 0};
        }

        FxU128 SignedAdd(FxU128 a, FxU128 b) {
            const uint64_t lo = a.lo + b.lo;

            return {.hi = a.hi + b.hi + (lo < a.lo ? 1u : 0u), .lo = lo};
        }

        bool SignedNegative(FxU128 value) {
            return (value.hi >> 63) != 0;
        }

        FxU128 SignedNegate(FxU128 value) {
            const uint64_t lo = ~value.lo + 1;

            return {.hi = ~value.hi + (lo == 0 ? 1u : 0u), .lo = lo};
        }

        // 128bit × 64bit(どちらも符号つき)。桁あふれは assert
        FxU128 SignedMul(FxU128 a, int64_t b) {
            const bool negative = SignedNegative(a) != (b < 0);
            const FxU128 magnitude = SignedNegative(a) ? SignedNegate(a) : a;
            const uint64_t factor = FxAbsU64(b);
            const FxU128 low = FxMulU64Full(magnitude.lo, factor);
            const FxU128 high = FxMulU64Full(magnitude.hi, factor);
            FX_ASSERT(high.hi == 0);
            const FxU128 product = {.hi = low.hi + high.lo, .lo = low.lo};
            FX_ASSERT(product.hi >= low.hi && (product.hi >> 63) == 0);

            return negative ? SignedNegate(product) : product;
        }

        FxU128 SignedProduct(int64_t a, int64_t b) {
            return FxMulS64Full(a, b);
        }

        // 右へ shift ずらして int64 へ(0 方向へ切り捨て)
        int64_t SignedShift(FxU128 value, uint32_t shift) {
            const bool negative = SignedNegative(value);
            const FxU128 magnitude = negative ? SignedNegate(value) : value;

            return FxApplySign(FxShiftRightU128(magnitude, shift), negative);
        }

        int64_t Entry(const EquilibriumBlock& block, uint32_t row, uint32_t column) {
            return block[row * ROW_WIDTH + column];
        }

        EquilibriumBlock Transposed(const EquilibriumBlock& block, uint32_t k) {
            EquilibriumBlock result{};
            for (uint32_t row = 0; row < k; ++row) {
                for (uint32_t column = 0; column < k; ++column)
                    result[column * ROW_WIDTH + row] = block[row * ROW_WIDTH + column];
            }

            return result;
        }

        using WideBlock = std::array<FxU128, ROW_WIDTH * ROW_WIDTH>;

        WideBlock ZeroWideBlock() {
            WideBlock block;
            block.fill(WideZero());

            return block;
        }

        // sum += Pᵀ_a × A × P_b(128bit のまま。P は Q16)
        void AddTripleProduct(WideBlock& sum, const EquilibriumBlock& left, const EquilibriumBlock& middle,
                              const EquilibriumBlock& right, uint32_t k) {
            for (uint32_t m = 0; m < k; ++m) {
                for (uint32_t n = 0; n < k; ++n) {
                    const int64_t coefficient = Entry(middle, m, n);
                    if (coefficient == 0)
                        continue;

                    for (uint32_t l = 0; l < k; ++l) {
                        const int64_t prolong = Entry(right, n, l);
                        if (prolong == 0)
                            continue;

                        const FxU128 inner = SignedProduct(coefficient, prolong);
                        for (uint32_t row = 0; row < k; ++row) {
                            const int64_t weightRow = Entry(left, m, row);
                            if (weightRow != 0)
                                sum[row * ROW_WIDTH + l] = SignedAdd(sum[row * ROW_WIDTH + l],
                                                                     SignedMul(inner, weightRow));
                        }
                    }
                }
            }
        }

        EquilibriumBlock Narrow(const WideBlock& wide, uint32_t shift, uint32_t k) {
            EquilibriumBlock block{};
            for (uint32_t row = 0; row < k; ++row) {
                for (uint32_t column = 0; column < k; ++column)
                    block[row * ROW_WIDTH + column] = SignedShift(wide[row * ROW_WIDTH + column], shift);
            }

            return block;
        }

        // --- 節の対角のブロックの LDLᵀ(L は Q30。消去の後の対角が小さすぎる向きは解かない)---
        EquilibriumBlockFactor Factor(const EquilibriumBlock& diagonal, uint32_t k) {
            EquilibriumBlockFactor factor;
            EquilibriumBlock scaled{};  // u_ij = L_ij × p_j(係数の単位)
            for (uint32_t i = 0; i < k; ++i) {
                for (uint32_t j = 0; j < i; ++j) {
                    int64_t value = Entry(diagonal, i, j);
                    for (uint32_t m = 0; m < j; ++m)
                        value -= FxMulShiftS64(factor.lower[i * ROW_WIDTH + m], scaled[j * ROW_WIDTH + m],
                                               EQ_BLOCK_FACTOR_SHIFT);

                    scaled[i * ROW_WIDTH + j] = value;
                    factor.lower[i * ROW_WIDTH + j] = factor.pivots[j] == 0 ? 0
                                                                            : FxDivShiftS64(value, factor.pivots[j],
                                                                                            EQ_BLOCK_FACTOR_SHIFT);
                }

                int64_t pivot = Entry(diagonal, i, i);
                for (uint32_t m = 0; m < i; ++m)
                    pivot -= FxMulShiftS64(factor.lower[i * ROW_WIDTH + m], scaled[i * ROW_WIDTH + m],
                                           EQ_BLOCK_FACTOR_SHIFT);

                const int64_t pivotMinimum = Entry(diagonal, i, i) >> PIVOT_FLOOR_SHIFT;
                factor.pivots[i] = pivot > 0 && pivot > pivotMinimum ? pivot : 0;
            }

            return factor;
        }

        // D δ = r を解く(r は右辺の単位・δ は値の単位)
        EquilibriumVector SolveFactor(const EquilibriumBlockFactor& factor, const EquilibriumVector& residual,
                                      uint32_t k, uint32_t coefficientShift) {
            EquilibriumVector forward{};
            for (uint32_t i = 0; i < k; ++i) {
                int64_t value = residual[i];
                for (uint32_t j = 0; j < i; ++j)
                    value -= FxMulShiftS64(factor.lower[i * ROW_WIDTH + j], forward[j], EQ_BLOCK_FACTOR_SHIFT);

                forward[i] = value;
            }

            EquilibriumVector delta{};
            for (uint32_t i = k; i-- > 0;) {
                if (factor.pivots[i] == 0)
                    continue;

                int64_t value = FxDivShiftS64(forward[i], factor.pivots[i], coefficientShift);
                for (uint32_t j = i + 1; j < k; ++j)
                    value -= FxMulShiftS64(factor.lower[j * ROW_WIDTH + i], delta[j], EQ_BLOCK_FACTOR_SHIFT);

                delta[i] = value;
            }

            return delta;
        }

        // --- 段の行 ---
        struct RowEntry {
            uint32_t neighbor = 0;
            WideBlock block{};
        };

        using Rows = std::vector<std::vector<RowEntry>>;

        WideBlock& RowBlock(std::vector<RowEntry>& row, uint32_t neighbor) {
            for (RowEntry& entry : row) {
                if (entry.neighbor == neighbor)
                    return entry.block;
            }

            row.push_back({.neighbor = neighbor, .block = ZeroWideBlock()});

            return row.back().block;
        }

        uint8_t ColorOf(int64_t x, int64_t y, int64_t z) {
            return (uint8_t)((x & 1) + 2 * (y & 1) + 4 * (z & 1));
        }

        void PushNode(EquilibriumBlockLevel& level, int64_t x, int64_t y, int64_t z) {
            level.x.push_back(x);
            level.y.push_back(y);
            level.z.push_back(z);
            level.colors.push_back(ColorOf(x, y, z));
        }

        // 行と対角(128bit)から段を仕上げる(÷ 2^shift・対角の分解)
        void FinishLevel(EquilibriumBlockLevel& level, const std::vector<WideBlock>& diagonals, const Rows& rows,
                         uint32_t shift, uint32_t k) {
            const size_t count = diagonals.size();
            level.diagonals.resize(count);
            level.factors.resize(count);
            level.rowStarts.assign(1, 0);
            for (size_t i = 0; i < count; ++i) {
                level.diagonals[i] = Narrow(diagonals[i], shift, k);
                level.factors[i] = Factor(level.diagonals[i], k);
                for (const RowEntry& entry : rows[i]) {
                    level.neighbors.push_back(entry.neighbor);
                    level.blocks.push_back(Narrow(entry.block, shift, k));
                }

                level.rowStarts.push_back((uint32_t)level.neighbors.size());
            }
        }

        WideBlock Widen(const EquilibriumBlock& block, uint32_t k) {
            WideBlock wide = ZeroWideBlock();
            for (uint32_t row = 0; row < k; ++row) {
                for (uint32_t column = 0; column < k; ++column) {
                    const int64_t value = block[row * ROW_WIDTH + column];
                    wide[row * ROW_WIDTH + column] = SignedProduct(value, 1);
                }
            }

            return wide;
        }

        void AddWide(WideBlock& sum, const EquilibriumBlock& block, uint32_t k) {
            const WideBlock wide = Widen(block, k);
            for (uint32_t i = 0; i < ROW_WIDTH * ROW_WIDTH; ++i)
                sum[i] = SignedAdd(sum[i], wide[i]);
        }

        // 段 0: 面と固定した面から
        EquilibriumBlockLevel MakeNodeLevel(const EquilibriumBlockSystem& system) {
            const uint32_t k = system.components;
            EquilibriumBlockLevel level;
            for (const EquilibriumNode& node : system.nodes)
                PushNode(level, node.x, node.y, node.z);

            std::vector<WideBlock> diagonals(system.nodes.size(), ZeroWideBlock());
            Rows rows(system.nodes.size());
            for (const EquilibriumBlockFace& face : system.faces) {
                FX_ASSERT(face.low != face.high);
                AddWide(diagonals[face.low], face.lowLow, k);
                AddWide(diagonals[face.high], face.highHigh, k);
                const WideBlock lowHigh = Widen(face.lowHigh, k);
                const WideBlock highLow = Widen(Transposed(face.lowHigh, k), k);
                WideBlock& lowRow = RowBlock(rows[face.low], face.high);
                WideBlock& highRow = RowBlock(rows[face.high], face.low);
                for (uint32_t i = 0; i < ROW_WIDTH * ROW_WIDTH; ++i) {
                    lowRow[i] = SignedAdd(lowRow[i], lowHigh[i]);
                    highRow[i] = SignedAdd(highRow[i], highLow[i]);
                }
            }

            for (const EquilibriumBlockFixed& fixed : system.fixedFaces)
                AddWide(diagonals[fixed.node], fixed.block, k);

            FinishLevel(level, diagonals, rows, 0, k);

            return level;
        }

        // fine を親へ縮約した段(Galerkin: 親の係数 = Σ Pᵀ A P)。fine の parents・octants を埋める
        EquilibriumBlockLevel Coarsen(EquilibriumBlockLevel& fine, const EquilibriumBlockGrid& grid) {
            const uint32_t k = grid.components;
            const size_t count = fine.x.size();
            EquilibriumBlockLevel coarse;
            std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> index;
            fine.parents.resize(count);
            fine.octants.resize(count);
            for (size_t i = 0; i < count; ++i) {
                const int64_t x = fine.x[i] >> 1;
                const int64_t y = fine.y[i] >> 1;
                const int64_t z = fine.z[i] >> 1;
                const auto [found, added] = index.emplace(std::tuple{x, y, z}, (uint32_t)coarse.x.size());
                if (added)
                    PushNode(coarse, x, y, z);

                fine.parents[i] = found->second;
                fine.octants[i] = ColorOf(fine.x[i], fine.y[i], fine.z[i]);
            }

            std::vector<WideBlock> diagonals(coarse.x.size(), ZeroWideBlock());
            Rows rows(coarse.x.size());
            for (size_t i = 0; i < count; ++i) {
                const uint32_t parent = fine.parents[i];
                const EquilibriumBlock& left = grid.prolongation[fine.octants[i]];
                AddTripleProduct(diagonals[parent], left, fine.diagonals[i], left, k);
                for (uint32_t link = fine.rowStarts[i]; link < fine.rowStarts[i + 1]; ++link) {
                    const uint32_t other = fine.neighbors[link];
                    const uint32_t otherParent = fine.parents[other];
                    const EquilibriumBlock& right = grid.prolongation[fine.octants[other]];
                    WideBlock& sum = otherParent == parent ? diagonals[parent] : RowBlock(rows[parent], otherParent);
                    AddTripleProduct(sum, left, fine.blocks[link], right, k);
                }
            }

            FinishLevel(coarse, diagonals, rows, 2 * EQ_PROLONG_SHIFT, k);

            return coarse;
        }

        // --- 反復 ---

        // 節 i の残差 r = b − A x(右辺の単位)
        EquilibriumVector NodeResidual(const EquilibriumBlockLevel& level, const std::vector<int64_t>& rhs,
                                       const std::vector<int64_t>& values, size_t i, uint32_t k, uint32_t shift) {
            std::array<FxU128, ROW_WIDTH> sums;
            sums.fill(WideZero());
            const auto addProduct = [&](const EquilibriumBlock& block, size_t node) {
                for (uint32_t row = 0; row < k; ++row) {
                    for (uint32_t column = 0; column < k; ++column) {
                        const int64_t coefficient = block[row * ROW_WIDTH + column];
                        const int64_t value = values[node * k + column];
                        if (coefficient != 0 && value != 0)
                            sums[row] = SignedAdd(sums[row], SignedProduct(coefficient, value));
                    }
                }
            };

            addProduct(level.diagonals[i], i);
            for (uint32_t link = level.rowStarts[i]; link < level.rowStarts[i + 1]; ++link)
                addProduct(level.blocks[link], level.neighbors[link]);

            EquilibriumVector residual{};
            for (uint32_t row = 0; row < k; ++row)
                residual[row] = rhs[i * k + row] - SignedShift(sums[row], shift);

            return residual;
        }

        // 8 色のブロックの Gauss–Seidel。同じ色の節は隣どうしでないので、色の中の順は結果に効かない
        void Smooth(const EquilibriumBlockLevel& level, const std::vector<int64_t>& rhs, std::vector<int64_t>& values,
                    uint32_t sweeps, uint32_t k, uint32_t shift, uint64_t& updates) {
            const size_t count = level.x.size();
            for (uint32_t sweep = 0; sweep < sweeps; ++sweep) {
                for (uint8_t color = 0; color < COLOR_COUNT; ++color) {
                    for (size_t i = 0; i < count; ++i) {
                        if (level.colors[i] != color)
                            continue;

                        const EquilibriumVector residual = NodeResidual(level, rhs, values, i, k, shift);
                        const EquilibriumVector delta = SolveFactor(level.factors[i], residual, k, shift);
                        for (uint32_t c = 0; c < k; ++c)
                            values[i * k + c] += delta[c];
                    }
                }

                updates += count;
            }
        }

        void VCycle(const EquilibriumBlockGrid& grid, size_t depth, const std::vector<int64_t>& rhs,
                    std::vector<int64_t>& values, const EquilibriumBlockOptions& options, uint64_t& updates) {
            const uint32_t k = grid.components;
            const uint32_t shift = grid.coefficientShift;
            const EquilibriumBlockLevel& level = grid.levels[depth];
            if (depth + 1 == grid.levels.size()) {
                Smooth(level, rhs, values, options.coarsestSweeps, k, shift, updates);
                return;
            }

            Smooth(level, rhs, values, options.preSmooth, k, shift, updates);

            // --- 残差を Pᵀ で親へ(128bit で足してから ÷ 2^16)---
            const EquilibriumBlockLevel& coarse = grid.levels[depth + 1];
            const size_t count = level.x.size();
            std::vector<FxU128> sums(coarse.x.size() * k, WideZero());
            for (size_t i = 0; i < count; ++i) {
                const EquilibriumVector residual = NodeResidual(level, rhs, values, i, k, shift);
                const EquilibriumBlock& restriction = grid.restriction[level.octants[i]];
                const size_t parent = level.parents[i];
                for (uint32_t row = 0; row < k; ++row) {
                    for (uint32_t column = 0; column < k; ++column) {
                        const int64_t weight = restriction[row * ROW_WIDTH + column];
                        if (weight != 0 && residual[column] != 0) {
                            sums[parent * k + row] = SignedAdd(sums[parent * k + row],
                                                               SignedProduct(weight, residual[column]));
                        }
                    }
                }
            }

            std::vector<int64_t> coarseRhs(sums.size());
            for (size_t i = 0; i < sums.size(); ++i)
                coarseRhs[i] = SignedShift(sums[i], EQ_PROLONG_SHIFT);

            // --- 親で直しを解き、P で子へ足す ---
            std::vector<int64_t> correction(coarseRhs.size(), 0);
            VCycle(grid, depth + 1, coarseRhs, correction, options, updates);
            for (size_t i = 0; i < count; ++i) {
                const EquilibriumBlock& prolong = grid.prolongation[level.octants[i]];
                const size_t parent = level.parents[i];
                for (uint32_t row = 0; row < k; ++row) {
                    FxU128 sum = WideZero();
                    for (uint32_t column = 0; column < k; ++column)
                        sum = SignedAdd(
                            sum, SignedProduct(prolong[row * ROW_WIDTH + column], correction[parent * k + column]));

                    values[i * k + row] += SignedShift(sum, EQ_PROLONG_SHIFT);
                }
            }

            Smooth(level, rhs, values, options.postSmooth, k, shift, updates);
        }

    }  // namespace

    EquilibriumBlockGrid BuildEquilibriumBlockGrid(const EquilibriumBlockSystem& system) {
        FX_ASSERT(system.components >= 1 && system.components <= EQ_MAX_COMPONENTS);
        EquilibriumBlockGrid grid;
        grid.components = system.components;
        grid.coefficientShift = system.coefficientShift;
        grid.prolongation = system.prolongation;
        for (uint32_t octant = 0; octant < EQ_OCTANTS; ++octant)
            grid.restriction[octant] = Transposed(system.prolongation[octant], system.components);

        // 違うレベルの節が混ざる系は T-0969
        [[maybe_unused]] const bool sameLevel = std::ranges::all_of(
            system.nodes, [&](const EquilibriumNode& node) { return node.level == system.nodes.front().level; });
        FX_ASSERT(sameLevel);

        grid.levels.push_back(MakeNodeLevel(system));
        while (grid.levels.size() < MAX_LEVELS && grid.levels.back().x.size() > 1) {
            EquilibriumBlockLevel coarser = Coarsen(grid.levels.back(), grid);
            if (coarser.x.size() == grid.levels.back().x.size())
                break;

            grid.levels.push_back(std::move(coarser));
        }

        return grid;
    }

    EquilibriumBlockResult SolveEquilibriumBlocks(const EquilibriumBlockSystem& system,
                                                  const EquilibriumBlockGrid& grid,
                                                  const EquilibriumBlockOptions& options,
                                                  EquilibriumBlockState& state) {
        const uint32_t k = system.components;
        const size_t count = system.nodes.size();
        EquilibriumBlockResult result;
        if (state.values.size() != count * k)
            state.values.assign(count * k, 0);

        // --- 右辺 = 右辺の項 + 固定した面の block × value ---
        std::vector<int64_t> rhs(count * k, 0);
        for (const EquilibriumBlockSource& source : system.sources)
            rhs[source.node * k + source.component] += source.value;

        for (const EquilibriumBlockFixed& fixed : system.fixedFaces) {
            for (uint32_t row = 0; row < k; ++row) {
                FxU128 sum = WideZero();
                for (uint32_t column = 0; column < k; ++column)
                    sum = SignedAdd(sum, SignedProduct(fixed.block[row * ROW_WIDTH + column], fixed.value[column]));

                rhs[fixed.node * k + row] += SignedShift(sum, system.coefficientShift);
            }
        }

        if (count == 0)
            return result;

        for (uint32_t cycle = 0; cycle < options.cycles; ++cycle)
            VCycle(grid, 0, rhs, state.values, options, result.nodeUpdates);

        // --- 残差の計器 ---
        const EquilibriumBlockLevel& cells = grid.levels[0];
        for (size_t i = 0; i < count; ++i) {
            const EquilibriumVector residual = NodeResidual(cells, rhs, state.values, i, k, system.coefficientShift);
            for (uint32_t c = 0; c < k; ++c)
                result.worstResidual = std::max(result.worstResidual, FxAbsU64(residual[c]));
        }

        result.gridLevels = (uint32_t)grid.levels.size();

        return result;
    }

}  // namespace bicameral::sim
