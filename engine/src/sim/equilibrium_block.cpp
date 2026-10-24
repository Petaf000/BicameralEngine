// equilibrium_block.cpp — 成分を持つ節の釣り合いの CPU の回し方(T-0919。何をするかは equilibrium_block.h)。
// 1. 段 0: 面の 3 つのブロックを行(自分の行・隣の列)と対角へ。固定した面は対角へ
// 2. 粗い段: 子の座標 ÷ 2 を親に。親の係数 = Σ Pᵀ(子の八分割)A P(相手の八分割)を 128bit で厳密に足してから 1 回だけ ÷ 2^32
//    (P が Q16 の 2 つ。和が厳密なので粗い係数はちょうど対称)。斜めの面も軸の面も同じ式で親の面・対角になる
// 3. V サイクル: 8 色のブロックの Gauss–Seidel(節ごとの対角のブロックを LDLᵀ で解く)→ 残差を Pᵀ で親へ → 親で直しを解く
//    → P で子へ足す → 8 色の Gauss–Seidel。色の中の節どうしは隣でないので、同じ色の中の順は結果に効かない(R2)
// 数の幅(04 R8): 係数 × 値は 128bit で足してから右へずらす(0 方向へ切り捨て)。桁あふれは FX_ASSERT。
// 節の LDLᵀ・解き・ブロックの残差・延ばし方と縮約・固定した面の右辺は shaders/common/equilibrium_block.hlsli(GPU 版と同じ関数。T-0970)。
#include "sim/equilibrium_block.h"

#include <algorithm>
#include <map>
#include <tuple>

#include "core/aliases.h"
#include "sim/equilibrium_fields.h"

using namespace bicameral::fx;
using namespace bicameral::equilibrium_block;

namespace bicameral::sim {

    namespace {

        constexpr size_t MAX_LEVELS = 32;
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

        // --- 節の対角のブロックの LDLᵀ と解き(common/equilibrium_block.hlsli の EqbFactorize・EqbSolveFactor)---
        EquilibriumBlockFactor Factor(const EquilibriumBlock& diagonal, uint32_t k) {
            return EqbFactorize(diagonal, k);
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

        void PushNode(EquilibriumBlockLevel& level, int32_t depth, int64_t x, int64_t y, int64_t z) {
            level.x.push_back(x);
            level.y.push_back(y);
            level.z.push_back(z);
            level.depths.push_back(depth);
        }

        // 色: 同じレベルの段は座標の奇偶の 8 色(今と同じ)。違うレベルが混ざる段は + 8 × レベルの奇偶の 16 色
        // (隣のレベルの差は 1 までなので、レベルの違う隣は違う色。同じレベルの隣〔27 点〕は座標の奇偶で違う色)
        void FinishColors(EquilibriumBlockLevel& level) {
            const bool mixed = !level.depths.empty() &&
                               rng::any_of(level.depths, [&](int32_t depth) { return depth != level.depths.front(); });
            level.colorCount = mixed ? 16 : 8;
            level.colors.resize(level.x.size());
            for (size_t i = 0; i < level.x.size(); ++i) {
                const uint8_t parity = ColorOf(level.x[i], level.y[i], level.z[i]);
                level.colors[i] = (uint8_t)(parity + (mixed ? 8 * (level.depths[i] & 1) : 0));
            }
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
                PushNode(level, node.level, node.x, node.y, node.z);

            FinishColors(level);

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
            // いちばん細かいレベルの節だけを親(座標 ÷ 2)へまとめ、ほかの節はそのまま次の段へ(延ばし方は単位の行列。T-0969)。
            // 同じレベルの系は全部がまとまる(今と同じ)。木の決まり(ある所は粗い節か子のどちらか)で、まとめた親と残した節は重ならない
            const int32_t finest = *rng::max_element(fine.depths);
            std::map<std::tuple<int32_t, int64_t, int64_t, int64_t>, uint32_t> index;
            std::vector<uint8_t> passed;  // 粗い段の節が「残した節」か(重なりの確かめ)
            fine.parents.resize(count);
            fine.octants.resize(count);
            for (size_t i = 0; i < count; ++i) {
                const bool merge = fine.depths[i] == finest;
                const int32_t depth = merge ? finest - 1 : fine.depths[i];
                const int64_t x = merge ? fine.x[i] >> 1 : fine.x[i];
                const int64_t y = merge ? fine.y[i] >> 1 : fine.y[i];
                const int64_t z = merge ? fine.z[i] >> 1 : fine.z[i];
                const auto [found, added] = index.emplace(std::tuple{depth, x, y, z}, (uint32_t)coarse.x.size());
                if (added) {
                    PushNode(coarse, depth, x, y, z);
                    passed.push_back(merge ? 0 : 1);
                }

                FX_ASSERT(added || (merge && passed[found->second] == 0));
                fine.parents[i] = found->second;
                fine.octants[i] = merge ? ColorOf(fine.x[i], fine.y[i], fine.z[i]) : (uint8_t)EQ_PASS_OCTANT;
            }

            FinishColors(coarse);

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

        EquilibriumVector NodeValues(const std::vector<int64_t>& values, size_t node, uint32_t k) {
            EquilibriumVector result{};
            for (uint32_t c = 0; c < k; ++c)
                result[c] = values[node * k + c];

            return result;
        }

        // 節 i の残差 r = b − A x(右辺の単位。EqbAddProduct・EqbResidual)
        EquilibriumVector NodeResidual(const EquilibriumBlockLevel& level, const std::vector<int64_t>& rhs,
                                       const std::vector<int64_t>& values, size_t i, uint32_t k, uint32_t shift) {
            EqbWideVector sums = EqbZeroWideVector();
            sums = EqbAddProduct(sums, level.diagonals[i], NodeValues(values, i, k), k);
            for (uint32_t link = level.rowStarts[i]; link < level.rowStarts[i + 1]; ++link)
                sums = EqbAddProduct(sums, level.blocks[link], NodeValues(values, level.neighbors[link], k), k);

            return EqbResidual(NodeValues(rhs, i, k), sums, k, shift);
        }

        // 8 色のブロックの Gauss–Seidel。同じ色の節は隣どうしでないので、色の中の順は結果に効かない
        void Smooth(const EquilibriumBlockLevel& level, const std::vector<int64_t>& rhs, std::vector<int64_t>& values,
                    uint32_t sweeps, uint32_t k, uint32_t shift, uint64_t& updates) {
            const size_t count = level.x.size();
            for (uint32_t sweep = 0; sweep < sweeps; ++sweep) {
                for (uint8_t color = 0; color < level.colorCount; ++color) {
                    for (size_t i = 0; i < count; ++i) {
                        if (level.colors[i] != color)
                            continue;

                        const EquilibriumVector residual = NodeResidual(level, rhs, values, i, k, shift);
                        const EquilibriumVector delta = EqbSolveFactor(level.factors[i], residual, k, shift);
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
            std::vector<EqbWideVector> sums(coarse.x.size(), EqbZeroWideVector());
            for (size_t i = 0; i < count; ++i) {
                const EquilibriumVector residual = NodeResidual(level, rhs, values, i, k, shift);
                const size_t parent = level.parents[i];
                sums[parent] = EqbAddProduct(sums[parent], grid.restriction[level.octants[i]], residual, k);
            }

            std::vector<int64_t> coarseRhs(coarse.x.size() * k);
            for (size_t parent = 0; parent < sums.size(); ++parent) {
                const EquilibriumVector narrowed = EqbNarrow(sums[parent], k, EQ_PROLONG_SHIFT);
                for (uint32_t c = 0; c < k; ++c)
                    coarseRhs[parent * k + c] = narrowed[c];
            }

            // --- 親で直しを解き、P で子へ足す ---
            std::vector<int64_t> correction(coarseRhs.size(), 0);
            VCycle(grid, depth + 1, coarseRhs, correction, options, updates);
            for (size_t i = 0; i < count; ++i) {
                const EquilibriumVector added = EqbProlong(grid.prolongation[level.octants[i]],
                                                           NodeValues(correction, level.parents[i], k), k);
                for (uint32_t c = 0; c < k; ++c)
                    values[i * k + c] += added[c];
            }

            Smooth(level, rhs, values, options.postSmooth, k, shift, updates);
        }

        // --- V の直しを前処理とする共役勾配(T-1068。lineSearch のときだけ)---
        // V サイクルの直し z = after − before を前処理した残差として、刻みの中で共役勾配を進める(ConjugateStep)。粗い段が運べない遅い形
        // (壁と棚の継ぎ目で回る形。1 回の V で (1 − ρ) しか進まない)は、前処理した系の孤立した小さい固有値になり、共役な向きで消える。
        // 刻みをまたいで持つものは無い(セーブ・ハッシュは今のまま)。整数: 内積は 128bit で厳密(足す順によらない = GPU でも同じ和)。
        // 歩幅と β は Q16・大きさの上限 EQB_STEP_MAX(式は common/equilibrium_block.hlsli で GPU と共有)。rᵀp ≤ 0 か pᵀAp ≤ 0 なら V のまま(その歩で覚えを捨てる)

        // 共役勾配の前の歩(刻みの中だけ)
        struct ConjugateMemory {
            std::vector<int64_t> direction;
            std::vector<int64_t> residual;
            FxU128 zr = {.hi = 0, .lo = 0};
            bool valid = false;
        };

        // 節 × k の並びどうしの内積(128bit で厳密)
        FxU128 Dot(const std::vector<int64_t>& a, const std::vector<int64_t>& b) {
            FxU128 sum = WideZero();
            for (size_t i = 0; i < a.size(); ++i)
                sum = SignedAdd(sum, SignedProduct(a[i], b[i]));

            return sum;
        }

        // 全部の節の b − A x(rhs が 0 なら −A x)
        std::vector<int64_t> Residuals(const EquilibriumBlockLevel& level, const std::vector<int64_t>& rhs,
                                       const std::vector<int64_t>& values, uint32_t k, uint32_t shift) {
            std::vector<int64_t> result(values.size());
            for (size_t i = 0; i < level.x.size(); ++i) {
                const EquilibriumVector residual = NodeResidual(level, rhs, values, i, k, shift);
                for (uint32_t c = 0; c < k; ++c)
                    result[i * k + c] = residual[c];
            }

            return result;
        }

        // V の直し z を前処理とする共役勾配の 1 歩(刻みの中だけ覚える。刻みをまたいで持つ状態は無い = セーブ・ハッシュは今のまま)。
        // p = z + β p_前(β は Polak–Ribière = zᵀ(r − r_前) ÷ z_前ᵀr_前)、歩幅 α = rᵀp ÷ pᵀAp。粗い段が運べない遅い形は、
        // 前処理した系の孤立した小さい固有値になり、共役な向きで 1〜2 歩で消える
        void ConjugateStep(const EquilibriumBlockLevel& level, const std::vector<int64_t>& rhs,
                           const std::vector<int64_t>& before, std::vector<int64_t>& values, uint32_t k, uint32_t shift,
                           ConjugateMemory& memory) {
            std::vector<int64_t> direction(values.size());
            for (size_t i = 0; i < values.size(); ++i)
                direction[i] = values[i] - before[i];

            const std::vector<int64_t> residual = Residuals(level, rhs, before, k, shift);
            const FxU128 zr = Dot(direction, residual);
            if (memory.valid) {
                std::vector<int64_t> change(residual.size());
                for (size_t i = 0; i < residual.size(); ++i)
                    change[i] = residual[i] - memory.residual[i];

                const int64_t beta = EqbSignedRatio(Dot(direction, change), memory.zr);
                for (size_t i = 0; i < direction.size(); ++i)
                    direction[i] += EqbScaleQ16(memory.direction[i], beta);
            }

            const std::vector<int64_t> zero(values.size(), 0);
            const std::vector<int64_t> pushed = Residuals(level, zero, direction, k, shift);  // −A p
            const FxU128 along = Dot(direction, residual);
            const FxU128 energy = SignedNegate(Dot(direction, pushed));
            memory.valid = false;
            if (SignedNegative(along) || SignedNegative(energy) || EqbWideBitLength(along) == 0 ||
                EqbWideBitLength(energy) == 0)
                return;

            const uint64_t alpha = EqbStepRatio(along, energy);
            for (size_t i = 0; i < values.size(); ++i)
                values[i] = before[i] + FxMulDivS64(direction[i], alpha, EQB_STEP_ONE);

            memory.valid = !SignedNegative(zr) && EqbWideBitLength(zr) != 0;
            memory.zr = zr;
            memory.direction = std::move(direction);
            memory.residual = residual;
        }

    }  // namespace

    EquilibriumBlockGrid BuildEquilibriumBlockGrid(const EquilibriumBlockSystem& system) {
        FX_ASSERT(system.components >= 1 && system.components <= EQ_MAX_COMPONENTS);
        EquilibriumBlockGrid grid;
        grid.components = system.components;
        grid.coefficientShift = system.coefficientShift;
        rng::copy(system.prolongation, grid.prolongation.begin());
        for (uint32_t c = 0; c < system.components; ++c)
            grid.prolongation[EQ_PASS_OCTANT][(c * ROW_WIDTH) + c] = int64_t{1} << EQ_PROLONG_SHIFT;

        for (uint32_t octant = 0; octant <= EQ_PASS_OCTANT; ++octant)
            grid.restriction[octant] = Transposed(grid.prolongation[octant], system.components);

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
        if (options.warmStart.nest != nullptr)
            state.values = LoadWarmStart(options.warmStart, system.nodes, k);  // 前の答えは欄から(T-0922)
        else if (state.values.size() != count * k)
            state.values.assign(count * k, 0);

        // --- 右辺 = 右辺の項 + 固定した面の block × value ---
        std::vector<int64_t> rhs(count * k, 0);
        for (const EquilibriumBlockSource& source : system.sources)
            rhs[source.node * k + source.component] += source.value;

        for (const EquilibriumBlockFixed& fixed : system.fixedFaces) {
            const EquilibriumVector added = EqbNarrow(EqbAddProduct(EqbZeroWideVector(), fixed.block, fixed.value, k),
                                                      k, system.coefficientShift);
            for (uint32_t row = 0; row < k; ++row)
                rhs[fixed.node * k + row] += added[row];
        }

        if (count == 0)
            return result;

        ConjugateMemory memory;
        const bool across = options.lineSearch && options.conjugateAcrossTicks;
        if (across && state.conjugateValid && state.conjugateDirection.size() == state.values.size()) {
            memory.direction = state.conjugateDirection;
            memory.residual = state.conjugateResidual;
            memory.zr = {.hi = state.conjugateZrHigh, .lo = state.conjugateZrLow};
            memory.valid = true;
        }

        for (uint32_t cycle = 0; cycle < options.cycles; ++cycle) {
            if (!options.lineSearch) {
                VCycle(grid, 0, rhs, state.values, options, result.nodeUpdates);
                continue;
            }

            // V の直しを前処理とする共役勾配の 1 歩(T-1068)
            const std::vector<int64_t> before = state.values;
            VCycle(grid, 0, rhs, state.values, options, result.nodeUpdates);
            ConjugateStep(grid.levels[0], rhs, before, state.values, k, system.coefficientShift, memory);
        }

        if (across) {
            state.conjugateValid = memory.valid;
            state.conjugateDirection = memory.direction;
            state.conjugateResidual = memory.residual;
            state.conjugateZrHigh = memory.zr.hi;
            state.conjugateZrLow = memory.zr.lo;
        }

        if (options.warmStart.nest != nullptr)
            (void)StoreWarmStart(options.warmStart, system.nodes, state.values, k);

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
