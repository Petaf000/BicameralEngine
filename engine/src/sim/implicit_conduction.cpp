// implicit_conduction.cpp — 細かいレベルの熱の陰解法の試作(T-0110 研究)。何をするかは implicit_conduction.h。
// 1 刻みの順(StepImplicit):
//   1. 温度 T0 をエネルギーから(mK × 2^16。1 mK より細かい端数も持つので、温度の丸めで反復が止まらない)
//   2. 近似解 T*: 赤黒の掃き出し(Smooth)/ 木の多重格子の V サイクル(VCycle)。RKL2 は段ごとの面の流れを積算する(Rkl2Flows)
//   3. 面の流れ F = g (T*_細かい側 − T*_粗い側)。収束していなくて温度が近所の範囲から出るセルは、面を陽解法の流れに戻す(LimitFlows)
//   4. 流れを両側に足す(ApplyFlows。ADR-0017 の形なので保存はビット単位)
// 数の幅(04 R8): 面の係数は 4^k で大きくなる(レベル 20 で 2^40 倍)ので 128bit で持ち、反復は「係数 ÷ 対角」の重み(Q48)だけを使う
// (割り算は段を作る時に 1 回。T-0110 の注意「1/(C + Σc) を刻みの初めに 1 回」)。重みの積は 128bit の途中の値から戻す。
// 多重格子の段(BuildImplicitGrid): 段 ℓ の最も細かいレベルの節を親のセル(レベル − 1、座標 ÷ 2)へ縮約する = 木の覆われた親のセル。
//   親の熱容量 = 子の合計、親どうしの面の係数 = 子の面の係数の合計 ÷ 2(区分的に一定の縮約の Galerkin は 2 倍の強さになるので、
//   そのレベルで離散化し直した値に合わせる)。全部の節が「熱容量 ≥ 面の係数の合計」(陽解法でも頭打ちにならない基準のレベルの手前)に
//   なったら止める → V サイクルの深さ = 基準からのレベルの差。
#include "sim/implicit_conduction.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <tuple>

#include "common/implicit_conduction.hlsli"
#include "common/multires_conduction.hlsli"

using namespace bicameral::fx;
using namespace bicameral::multires;

namespace bicameral::sim {

    namespace {

        constexpr uint32_t WEIGHT_SHIFT = IM_WEIGHT_SHIFT;  // 式は common/implicit_conduction.hlsli(GPU と共通。T-0117)
        constexpr uint32_t TEMPERATURE_SHIFT = IM_TEMPERATURE_SHIFT;
        constexpr int64_t HALF_WEIGHT = int64_t{1} << (WEIGHT_SHIFT - 1);
        constexpr uint32_t ENERGY_BITS_PER_LEVEL = IM_ENERGY_BITS_PER_LEVEL;
        constexpr uint32_t STAGE_RATIO_SHIFT = 16;  // RKL2 の段の数を決める比の Q
        constexpr size_t MAX_GRID_LEVELS = 64;
        constexpr uint8_t COLOR_COUNT = 2;
        constexpr uint32_t CORRECTION_SCALE_SHIFT = IM_CORRECTION_SCALE_SHIFT;  // ImplicitOptions::correctionScale の Q

        constexpr std::array<std::array<int64_t, 3>, 6> FACE_OFFSETS = {
            {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};

        using CellKey = std::tuple<int32_t, int64_t, int64_t, int64_t>;

        // --- 128bit の小さな道具(係数・対角。負にならない)---

        FxU128 Wide(uint64_t value) {
            return {.hi = 0, .lo = value};
        }

        bool IsZero(FxU128 value) {
            return value.hi == 0 && value.lo == 0;
        }

        FxU128 WideAdd(FxU128 a, FxU128 b) {
            FxU128 sum = {.hi = a.hi + b.hi, .lo = a.lo + b.lo};
            if (sum.lo < a.lo)
                sum.hi += 1;

            FX_ASSERT(sum.hi >= a.hi);

            return sum;
        }

        FxU128 WideShiftRight(FxU128 value, uint32_t shift) {
            if (shift == 0)
                return value;

            if (shift >= 128)
                return Wide(0);

            if (shift >= 64)
                return Wide(value.hi >> (shift - 64));

            return {.hi = value.hi >> shift, .lo = (value.lo >> shift) | (value.hi << (64 - shift))};
        }

        // 桁あふれは R8 の assert
        FxU128 WideShiftLeft(FxU128 value, uint32_t shift) {
            if (shift == 0 || IsZero(value))
                return value;

            FX_ASSERT(shift < 128);
            if (shift >= 64) {
                FX_ASSERT(value.hi == 0 && (shift == 64 || (value.lo >> (128 - shift)) == 0));
                return {.hi = value.lo << (shift - 64), .lo = 0};
            }

            FX_ASSERT((value.hi >> (64 - shift)) == 0);

            return {.hi = (value.hi << shift) | (value.lo >> (64 - shift)), .lo = value.lo << shift};
        }

        FxU128 WideMul(FxU128 a, uint64_t b) {
            const FxU128 low = FxMulU64Full(a.lo, b);
            const FxU128 high = FxMulU64Full(a.hi, b);
            FX_ASSERT(high.hi == 0);
            const FxU128 product = {.hi = low.hi + high.lo, .lo = low.lo};
            FX_ASSERT(product.hi >= low.hi);

            return product;
        }

        uint32_t WideMsb(FxU128 value) {
            return value.hi != 0 ? 64 + FxMsbU64(value.hi) : FxMsbU64(value.lo);
        }

        // numerator ÷ denominator を Q fractionBits で(商は 2^63 未満であること)。分母が 63bit を超える時は両方の下位を落とす
        // (重みの近似が少し粗くなるだけ。保存には効かない)
        int64_t WideRatio(FxU128 numerator, FxU128 denominator, uint32_t fractionBits) {
            FX_ASSERT(!IsZero(denominator));
            if (IsZero(numerator))
                return 0;

            const uint32_t msb = WideMsb(denominator);
            const uint32_t drop = msb > 62 ? msb - 62 : 0;
            const uint64_t divisor = WideShiftRight(denominator, drop).lo;
            const FxU128 scaled = WideShiftLeft(WideShiftRight(numerator, drop), fractionBits);
            FX_ASSERT(scaled.hi < divisor);
            const uint64_t quotient = FxDivU128By64(scaled, divisor).quotient;
            FX_ASSERT(quotient < (uint64_t{1} << 63));

            return static_cast<int64_t>(quotient);
        }

        int64_t Weight(FxU128 numerator, FxU128 denominator) {
            return WideRatio(numerator, denominator, WEIGHT_SHIFT);
        }

        int64_t ShiftRightSigned(int64_t value, uint32_t shift) {
            return ImShiftRightSigned(value, shift);
        }

        int64_t FaceFlow(FxU128 coefficient, int64_t difference) {
            return ImFaceFlow(coefficient, difference);
        }

        // --- 面を探す ---

        std::map<CellKey, uint32_t> MakeCellIndex(const std::vector<ImplicitCell>& cells) {
            std::map<CellKey, uint32_t> index;
            for (uint32_t i = 0; i < cells.size(); ++i) {
                const ImplicitCell& cell = cells[i];
                FX_ASSERT(cell.level >= 0 && cell.x >= 0 && cell.y >= 0 && cell.z >= 0 && cell.heatCapacity != 0);
                const bool added = index.emplace(CellKey{cell.level, cell.x, cell.y, cell.z}, i).second;
                FX_ASSERT(added);
            }

            return index;
        }

        struct Containing {
            uint32_t index = 0;
            uint32_t gap = 0;
        };

        // レベル level の座標 (x, y, z) のセルを含む、レベル level 以下のセル(細かいセルに覆われていれば無い)
        std::optional<Containing> FindContaining(const std::map<CellKey, uint32_t>& index, int32_t level, int64_t x,
                                                 int64_t y, int64_t z) {
            for (int32_t gap = 0; gap <= level; ++gap) {
                const auto found = index.find(CellKey{level - gap, x >> gap, y >> gap, z >> gap});
                if (found != index.end())
                    return Containing{.index = found->second, .gap = static_cast<uint32_t>(gap)};
            }

            return std::nullopt;
        }

        FxU128 FaceCoefficient(const ImplicitCell& fine, const ImplicitCell& coarse) {
            const uint64_t conductance = std::min(fine.conductance, coarse.conductance);

            return WideShiftLeft(Wide(conductance * HC_LIMIT_PER_CONDUCTANCE), 2 * static_cast<uint32_t>(fine.level));
        }

        void FindFaces(ImplicitGrid& grid) {
            const std::map<CellKey, uint32_t> index = MakeCellIndex(grid.cells);
            for (uint32_t i = 0; i < grid.cells.size(); ++i) {
                const ImplicitCell& cell = grid.cells[i];
                for (const auto& offset : FACE_OFFSETS) {
                    const int64_t x = cell.x + offset[0];
                    const int64_t y = cell.y + offset[1];
                    const int64_t z = cell.z + offset[2];
                    if (x < 0 || y < 0 || z < 0)
                        continue;

                    const std::optional<Containing> neighbor = FindContaining(index, cell.level, x, y, z);
                    if (!neighbor || (neighbor->gap == 0 && neighbor->index < i))
                        continue;

                    const ImplicitCell& other = grid.cells[neighbor->index];
                    grid.faces.push_back({.fine = i,
                                          .coarse = neighbor->index,
                                          .gap = neighbor->gap,
                                          .coefficient = FaceCoefficient(cell, other)});
                }
            }
        }

        // --- 多重格子の段 ---

        struct RowEntry {
            uint32_t neighbor = 0;
            FxU128 coefficient{};
        };

        using Rows = std::vector<std::vector<RowEntry>>;

        void AddToRow(std::vector<RowEntry>& row, uint32_t neighbor, FxU128 coefficient) {
            for (RowEntry& entry : row) {
                if (entry.neighbor != neighbor)
                    continue;

                entry.coefficient = WideAdd(entry.coefficient, coefficient);
                return;
            }

            row.push_back({.neighbor = neighbor, .coefficient = coefficient});
        }

        void PushNode(ImplicitGridLevel& level, const CellKey& key, FxU128 capacity) {
            const auto [nodeLevel, x, y, z] = key;
            level.levels.push_back(nodeLevel);
            level.x.push_back(x);
            level.y.push_back(y);
            level.z.push_back(z);
            level.colors.push_back(static_cast<uint8_t>((x + y + z) & 1));
            level.capacities.push_back(capacity);
        }

        // 行から対角・重み・隣の一覧を作る
        void FinishLevel(ImplicitGridLevel& level, const Rows& rows) {
            const size_t count = level.capacities.size();
            level.diagonals.resize(count);
            level.selfWeights.resize(count);
            level.rowStarts.assign(1, 0);
            for (size_t i = 0; i < count; ++i) {
                FxU128 diagonal = level.capacities[i];
                for (const RowEntry& entry : rows[i])
                    diagonal = WideAdd(diagonal, entry.coefficient);

                level.diagonals[i] = diagonal;
                level.selfWeights[i] = Weight(level.capacities[i], diagonal);
                for (const RowEntry& entry : rows[i]) {
                    level.neighbors.push_back(entry.neighbor);
                    level.weights.push_back(Weight(entry.coefficient, diagonal));
                    level.coefficients.push_back(entry.coefficient);
                }

                level.rowStarts.push_back(static_cast<uint32_t>(level.neighbors.size()));
            }
        }

        // 段 0: セルそのもの。粗い側の行の係数は細かい側の係数 ÷ 8^d(粗い側の単位)
        ImplicitGridLevel MakeCellLevel(const ImplicitGrid& grid) {
            ImplicitGridLevel level;
            for (const ImplicitCell& cell : grid.cells)
                PushNode(level, CellKey{cell.level, cell.x, cell.y, cell.z}, Wide(cell.heatCapacity));

            Rows rows(grid.cells.size());
            for (const ImplicitFace& face : grid.faces) {
                rows[face.fine].push_back({.neighbor = face.coarse, .coefficient = face.coefficient});
                rows[face.coarse].push_back(
                    {.neighbor = face.fine,
                     .coefficient = WideShiftRight(face.coefficient, ENERGY_BITS_PER_LEVEL * face.gap)});
            }

            FinishLevel(level, rows);

            return level;
        }

        bool NeedsCoarser(const ImplicitGridLevel& level) {
            return std::ranges::any_of(level.selfWeights, [](int64_t weight) { return weight < HALF_WEIGHT; });
        }

        // fine の最も細かいレベルの節を親のセルへ縮約した段を作り、fine の parents・restrictWeights を埋める
        ImplicitGridLevel Coarsen(ImplicitGridLevel& fine, bool galerkin) {
            const int32_t finest = *std::ranges::max_element(fine.levels);
            const size_t count = fine.levels.size();
            ImplicitGridLevel coarse;
            std::map<CellKey, uint32_t> index;
            std::vector<uint8_t> lowered;  // 親の節ごと: 縮約してできた節か
            fine.parents.resize(count);
            for (size_t i = 0; i < count; ++i) {
                const bool lower = fine.levels[i] == finest;
                const CellKey key = lower ? CellKey{finest - 1, fine.x[i] >> 1, fine.y[i] >> 1, fine.z[i] >> 1}
                                          : CellKey{fine.levels[i], fine.x[i], fine.y[i], fine.z[i]};
                const auto [found, added] = index.emplace(key, static_cast<uint32_t>(coarse.levels.size()));
                if (added) {
                    PushNode(coarse, key, Wide(0));
                    lowered.push_back(lower ? 1 : 0);
                }

                const uint32_t parent = found->second;
                const uint32_t shift = lower ? ENERGY_BITS_PER_LEVEL : 0;
                fine.parents[i] = parent;
                coarse.capacities[parent] = WideAdd(coarse.capacities[parent],
                                                    WideShiftRight(fine.capacities[i], shift));
            }

            Rows rows(coarse.levels.size());
            for (size_t i = 0; i < count; ++i) {
                const uint32_t parent = fine.parents[i];
                const uint32_t shift = lowered[parent] != 0 ? ENERGY_BITS_PER_LEVEL : 0;
                for (uint32_t k = fine.rowStarts[i]; k < fine.rowStarts[i + 1]; ++k) {
                    const uint32_t other = fine.parents[fine.neighbors[k]];
                    if (other != parent)
                        AddToRow(rows[parent], other, WideShiftRight(fine.coefficients[k], shift));
                }
            }

            for (size_t parent = 0; parent < rows.size(); ++parent) {
                for (RowEntry& entry : rows[parent]) {
                    if (!galerkin && (lowered[parent] != 0 || lowered[entry.neighbor] != 0))
                        entry.coefficient = WideShiftRight(entry.coefficient, 1);
                }
            }

            FinishLevel(coarse, rows);
            fine.restrictWeights.resize(count);
            for (size_t i = 0; i < count; ++i) {
                const uint32_t shift = lowered[fine.parents[i]] != 0 ? ENERGY_BITS_PER_LEVEL : 0;
                fine.restrictWeights[i] = Weight(WideShiftRight(fine.diagonals[i], shift),
                                                 coarse.diagonals[fine.parents[i]]);
            }

            return coarse;
        }

        // --- 反復 ---

        // 右辺 + Σ 隣の重み × 値
        int64_t Relax(const ImplicitGridLevel& level, const std::vector<int64_t>& rhs,
                      const std::vector<int64_t>& values, size_t node) {
            int64_t sum = rhs[node];
            for (uint32_t k = level.rowStarts[node]; k < level.rowStarts[node + 1]; ++k)
                sum += FxMulShiftS64(level.weights[k], values[level.neighbors[k]], WEIGHT_SHIFT);

            return sum;
        }

        // 1 色ぶん: 全部の節を掃き出しの初めの値から計算してから書く
        void SweepColor(const ImplicitGridLevel& level, const std::vector<int64_t>& rhs, uint8_t color,
                        std::vector<int64_t>& values, std::vector<int64_t>& next) {
            for (size_t i = 0; i < values.size(); ++i) {
                if (level.colors[i] == color)
                    next[i] = Relax(level, rhs, values, i);
            }

            for (size_t i = 0; i < values.size(); ++i) {
                if (level.colors[i] == color)
                    values[i] = next[i];
            }
        }

        // 赤黒の掃き出し。同じ色の隣(違うレベルの面ではありうる)は掃き出しの初めの値を読む(順に依存しない)
        void Smooth(const ImplicitGridLevel& level, const std::vector<int64_t>& rhs, std::vector<int64_t>& values,
                    uint32_t sweeps, ImplicitCost& cost) {
            std::vector<int64_t> next(values.size());
            for (uint32_t sweep = 0; sweep < sweeps; ++sweep) {
                for (uint8_t color = 0; color < COLOR_COUNT; ++color)
                    SweepColor(level, rhs, color, values, next);

                cost.passes += COLOR_COUNT;
                cost.cellUpdates += values.size();
            }
        }

        void VCycle(const ImplicitGrid& grid, size_t depth, const std::vector<int64_t>& rhs,
                    std::vector<int64_t>& values, const ImplicitOptions& options, ImplicitCost& cost) {
            const ImplicitGridLevel& level = grid.levels[depth];
            if (depth + 1 == grid.levels.size()) {
                Smooth(level, rhs, values, options.coarsestSweeps, cost);
                return;
            }

            Smooth(level, rhs, values, options.preSmooth, cost);

            // --- 残差を親へ(自分の D で重み付けた和 ÷ 親の D)---
            const ImplicitGridLevel& coarse = grid.levels[depth + 1];
            std::vector<int64_t> coarseRhs(coarse.levels.size(), 0);
            for (size_t i = 0; i < values.size(); ++i) {
                const int64_t residual = Relax(level, rhs, values, i) - values[i];
                coarseRhs[level.parents[i]] += FxMulShiftS64(level.restrictWeights[i], residual, WEIGHT_SHIFT);
            }

            cost.passes += 1;
            cost.cellUpdates += values.size();

            // --- 親で直しを解いて、子へそのまま足す ---
            std::vector<int64_t> correction(coarse.levels.size(), 0);
            VCycle(grid, depth + 1, coarseRhs, correction, options, cost);
            for (size_t i = 0; i < values.size(); ++i)
                values[i] += FxMulShiftS64(options.correctionScale, correction[level.parents[i]],
                                           CORRECTION_SCALE_SHIFT);

            cost.passes += 1;
            Smooth(level, rhs, values, options.postSmooth, cost);
        }

        // 新しい温度の誤差の見込み = 残差(重みで割った形。温度の単位)× D/C が、全部のセルで tolerance 以下か。
        // 流れ F = g ΔT* で足すので、T* の残差は D/C ≈ 4^Δk 倍になって新しい温度に出る(T-0110 の計測)
        bool Converged(const ImplicitGridLevel& cells, const std::vector<int64_t>& rhs,
                       const std::vector<int64_t>& values, uint32_t toleranceMillikelvin, ImplicitCost& cost) {
            bool converged = true;
            for (size_t i = 0; i < values.size(); ++i) {
                if (ImExceedsTolerance(Relax(cells, rhs, values, i) - values[i], cells.selfWeights[i],
                                       toleranceMillikelvin))
                    converged = false;
            }

            cost.passes += 1;
            cost.cellUpdates += values.size();

            return converged;
        }

        // --- 面の流れ ---

        std::vector<int64_t> Temperatures(const ImplicitGrid& grid) {
            std::vector<int64_t> temperatures(grid.cells.size());
            for (size_t i = 0; i < grid.cells.size(); ++i)
                temperatures[i] = ImplicitTemperature(grid.cells[i]);

            return temperatures;
        }

        std::vector<int64_t> Flows(const ImplicitGrid& grid, const std::vector<int64_t>& temperatures) {
            std::vector<int64_t> flows(grid.faces.size());
            for (size_t f = 0; f < grid.faces.size(); ++f) {
                const ImplicitFace& face = grid.faces[f];
                flows[f] = FaceFlow(face.coefficient, temperatures[face.fine] - temperatures[face.coarse]);
            }

            return flows;
        }

        // 流れを両側に(同じレベル: 逆向きに同じ値 / 違うレベル: 粗い側は整数部 + 端数。MrSplitCrossFlow)
        void ApplyFlows(ImplicitGrid& grid, const std::vector<int64_t>& flows) {
            for (size_t f = 0; f < grid.faces.size(); ++f) {
                const ImplicitFace& face = grid.faces[f];
                ImplicitCell& fine = grid.cells[face.fine];
                ImplicitCell& coarse = grid.cells[face.coarse];
                if (face.gap == 0) {
                    fine.energy -= flows[f];
                    coarse.energy += flows[f];
                    continue;
                }

                const MrCrossTransfer transfer = MrSplitCrossFlow(flows[f], face.gap, true);
                const MrEnergyDelta sum = MrAddEnergyDelta({.whole = coarse.energy, .fraction = coarse.fraction},
                                                           transfer.coarseDelta);
                fine.energy += transfer.fineDelta;
                coarse.energy = sum.whole;
                coarse.fraction = sum.fraction;
            }
        }

        // --- 安全網: 近似が収束していない時に、温度が刻みの初めの範囲から出るセルの面を陽解法の流れに戻す ---
        // 範囲 = 全部のセルの T0 の最小〜最大(上は 1 mK の切り上げ)。新しい最低・最高の温度(負のエネルギー・行き過ぎ)は「嘘」なので止める。
        // 近所の範囲(自分と隣)にすると、細かいレベルでは D/C ≈ 4^Δk 倍に増幅された反復の残差(0.1 mK 程度)で外に出て、
        // 安全網が連鎖して全部が陽解法(= 遅い)に戻ってしまった(T-0110 の計測)。局所の小さな揺れは精度の問題として物差しで見る。外に出たセルは、
        // 全部の面を陽解法の流れ(係数を熱容量の上限で頭打ち。T-0019 と同じ。1 面 ≤ C/8 なので 6 面でも凸結合 → 範囲の中)にする。
        // 面を戻すと隣の合計も変わるので、外に出るセルが無くなるまで繰り返す(戻したセルは二度と外に出ない → 必ず終わる)。
        // 1 回ごとに「全部のセルを見てから、全部の面を直す」ので順に依存しない。保存は面ごとに 1 つの値なのでビット単位のまま。

        int64_t AddClamped(int64_t a, int64_t b) {
            return ImAddClamped(a, b);
        }

        // 陽解法の面の係数(細かい側の単位)= min(係数, 細かい側の C/8, 粗い側の C/8 × 2^d)(ADR-0017)
        std::vector<int64_t> ExplicitFlows(const ImplicitGrid& grid, const std::vector<int64_t>& start) {
            std::vector<int64_t> flows(grid.faces.size());
            for (size_t f = 0; f < grid.faces.size(); ++f) {
                const ImplicitFace& face = grid.faces[f];
                const FxU128 coefficient = ImExplicitCoefficient(face.coefficient, grid.cells[face.fine].heatCapacity,
                                                                 grid.cells[face.coarse].heatCapacity, face.gap);
                flows[f] = FaceFlow(coefficient, start[face.fine] - start[face.coarse]);
            }

            return flows;
        }

        struct Bounds {
            std::vector<int64_t> lowest;  // そのセルの単位のエネルギー
            std::vector<int64_t> highest;
            std::vector<int64_t> slack;  // この量までは外に出てよい
        };

        // T* は範囲に入れない(多重格子の直しは単調でなく、T* が T0 の範囲の外に出ることがある)
        Bounds MakeBounds(const ImplicitGrid& grid, const std::vector<int64_t>& start, int64_t slackMillikelvin) {
            const auto [lowestStart, highestStart] = std::ranges::minmax(start);
            const int64_t lowest = lowestStart >> TEMPERATURE_SHIFT;
            const int64_t highest = (highestStart >> TEMPERATURE_SHIFT) + 1;
            Bounds bounds;
            bounds.lowest.resize(grid.cells.size());
            bounds.highest.resize(grid.cells.size());
            bounds.slack.resize(grid.cells.size());
            for (size_t i = 0; i < grid.cells.size(); ++i) {
                const uint64_t capacity = grid.cells[i].heatCapacity;
                bounds.lowest[i] = ImplicitEnergyFor(capacity, lowest);
                bounds.highest[i] = ImplicitEnergyFor(capacity, highest);
                bounds.slack[i] = ImplicitEnergyFor(capacity, slackMillikelvin);
            }

            return bounds;
        }

        // 流れを足した後のエネルギー(粗い側は整数部だけ。範囲の判定用)
        std::vector<int64_t> EnergiesAfter(const ImplicitGrid& grid, const std::vector<int64_t>& flows) {
            std::vector<int64_t> energies(grid.cells.size());
            for (size_t i = 0; i < grid.cells.size(); ++i)
                energies[i] = grid.cells[i].energy;

            for (size_t f = 0; f < grid.faces.size(); ++f) {
                const ImplicitFace& face = grid.faces[f];
                energies[face.fine] = AddClamped(energies[face.fine], -flows[f]);
                energies[face.coarse] = AddClamped(energies[face.coarse],
                                                   ShiftRightSigned(flows[f], ENERGY_BITS_PER_LEVEL * face.gap));
            }

            return energies;
        }

        // 範囲(余裕を含まない)を超えた量。中なら 0 以下
        int64_t Overrun(int64_t energy, const Bounds& bounds, size_t i) {
            return ImOverrun(energy, bounds.lowest[i], bounds.highest[i]);
        }

        // 範囲の外に出たセルに印を付ける(新しく付けたら true)。最初の回は範囲を超えた最大も記録する
        bool MarkOutside(const ImplicitGrid& grid, const Bounds& bounds, const std::vector<int64_t>& energies,
                         std::vector<uint8_t>& limited, ImplicitCost& cost) {
            bool changed = false;
            for (size_t i = 0; i < grid.cells.size(); ++i) {
                const int64_t over = Overrun(energies[i], bounds, i);
                if (cost.limitRounds == 0) {
                    const int64_t excess = ImExcessMillikelvin(over, grid.cells[i].heatCapacity);
                    cost.worstExcessMillikelvin = std::max(cost.worstExcessMillikelvin, excess);
                }

                if (limited[i] != 0 || over <= bounds.slack[i])
                    continue;

                limited[i] = 1;
                changed = true;
                cost.limitedCells += 1;
            }

            return changed;
        }

        void LimitFlows(const ImplicitGrid& grid, const std::vector<int64_t>& start, int64_t slackMillikelvin,
                        std::vector<int64_t>& flows, ImplicitCost& cost) {
            const Bounds bounds = MakeBounds(grid, start, slackMillikelvin);
            std::vector<int64_t> explicitFlows;
            std::vector<uint8_t> limited(grid.cells.size(), 0);
            while (true) {
                const bool changed = MarkOutside(grid, bounds, EnergiesAfter(grid, flows), limited, cost);
                cost.passes += 1;
                if (!changed)
                    return;

                // --- 外に出たセルの面を陽解法の流れに ---
                if (explicitFlows.empty())
                    explicitFlows = ExplicitFlows(grid, start);

                for (size_t f = 0; f < grid.faces.size(); ++f) {
                    const ImplicitFace& face = grid.faces[f];
                    if (limited[face.fine] != 0 || limited[face.coarse] != 0)
                        flows[f] = explicitFlows[f];
                }

                cost.passes += 1;
                cost.limitRounds += 1;
            }
        }

        // --- RKL2(Meyer, Balsara, Aslam 2014)。段の値 Y_j = Y_0 + 面の流れの積算 Φ_j の発散で持ち、最後の Φ_s を流れとして渡す
        //     (Y の漸化式は Y_0 からの差が L(Y_i) の一次結合なので、Φ も同じ漸化式に従う)---

        struct Rational {
            uint64_t numerator = 0;
            uint64_t denominator = 1;
        };

        // b_j = (j² + j − 2) / (2j(j + 1))(j < 2 は 1/3)
        Rational Rkl2B(uint64_t j) {
            if (j < 2)
                return {.numerator = 1, .denominator = 3};

            return {.numerator = (j * j) + j - 2, .denominator = 2 * j * (j + 1)};
        }

        struct Rkl2Stage {
            int64_t mu = 0;  // Q48
            int64_t nu = 0;
            int64_t muTilde = 0;
            int64_t gammaTilde = 0;
        };

        int64_t RationalWeight(FxU128 numerator, FxU128 denominator, bool negative) {
            const int64_t weight = Weight(numerator, denominator);

            return negative ? -weight : weight;
        }

        std::vector<Rkl2Stage> Rkl2Stages(uint32_t stageCount) {
            const uint64_t s = stageCount;
            const uint64_t span = (s * s) + s - 2;  // w1 = 4 / span
            std::vector<Rkl2Stage> stages(stageCount + 1);
            stages[1].muTilde = RationalWeight(Wide(4), Wide(3 * span), false);
            for (uint64_t j = 2; j <= s; ++j) {
                const Rational b = Rkl2B(j);
                const Rational previous = Rkl2B(j - 1);
                const Rational before = Rkl2B(j - 2);
                const FxU128 muNumerator = WideMul(WideMul(Wide((2 * j) - 1), b.numerator), previous.denominator);
                const FxU128 muDenominator = WideMul(WideMul(Wide(j), b.denominator), previous.numerator);
                Rkl2Stage& stage = stages[j];
                stage.mu = RationalWeight(muNumerator, muDenominator, false);
                stage.nu = RationalWeight(WideMul(WideMul(Wide(j - 1), b.numerator), before.denominator),
                                          WideMul(WideMul(Wide(j), b.denominator), before.numerator), true);
                stage.muTilde = RationalWeight(WideMul(muNumerator, 4), WideMul(muDenominator, span), false);

                // γ̃_j = −a_{j−1} μ̃_j(a = 1 − b)
                const FxU128 gammaNumerator = WideMul(WideMul(muNumerator, 4),
                                                      previous.denominator - previous.numerator);
                const FxU128 gammaDenominator = WideMul(WideMul(muDenominator, span), previous.denominator);
                stage.gammaTilde = RationalWeight(gammaNumerator, gammaDenominator, true);
            }

            return stages;
        }

        // 安定の条件 s² + s − 2 ≥ 4 max(Σ 面の係数 ÷ 熱容量)を満たす最小の s
        uint32_t Rkl2StageCount(const ImplicitGridLevel& cells) {
            int64_t worst = 0;
            for (size_t i = 0; i < cells.capacities.size(); ++i) {
                FxU128 sum = Wide(0);
                for (uint32_t k = cells.rowStarts[i]; k < cells.rowStarts[i + 1]; ++k)
                    sum = WideAdd(sum, cells.coefficients[k]);

                worst = std::max(worst, WideRatio(sum, cells.capacities[i], STAGE_RATIO_SHIFT));
            }

            uint64_t s = 2;
            while ((((s * s) + s - 2) << STAGE_RATIO_SHIFT) < 4 * static_cast<uint64_t>(worst))
                ++s;

            return static_cast<uint32_t>(s);
        }

        // 面の流れの積算 → 温度(Y_0 + 発散 ÷ 熱容量)
        std::vector<int64_t> StageTemperatures(const ImplicitGrid& grid, const std::vector<int64_t>& start,
                                               const std::vector<int64_t>& accumulated) {
            std::vector<int64_t> heat(grid.cells.size(), 0);  // 自分の単位 × 2^16
            for (size_t f = 0; f < grid.faces.size(); ++f) {
                const ImplicitFace& face = grid.faces[f];
                FX_ASSERT((FxAbsU64(accumulated[f]) >> (63 - TEMPERATURE_SHIFT)) == 0);
                const int64_t scaled = accumulated[f] * (int64_t{1} << TEMPERATURE_SHIFT);
                heat[face.fine] -= scaled;
                heat[face.coarse] += ShiftRightSigned(scaled, ENERGY_BITS_PER_LEVEL * face.gap);
            }

            std::vector<int64_t> temperatures(grid.cells.size());
            for (size_t i = 0; i < grid.cells.size(); ++i) {
                const auto capacity = static_cast<int64_t>(grid.cells[i].heatCapacity);
                temperatures[i] = start[i] + FxDivShiftS64(heat[i], capacity, 32);
            }

            return temperatures;
        }

        int64_t MulWeight(int64_t weight, int64_t value) {
            return FxMulShiftS64(weight, value, WEIGHT_SHIFT);
        }

        std::vector<int64_t> Rkl2Flows(const ImplicitGrid& grid, const std::vector<int64_t>& start,
                                       ImplicitCost& cost) {
            const uint32_t stageCount = Rkl2StageCount(grid.levels[0]);
            const std::vector<Rkl2Stage> stages = Rkl2Stages(stageCount);
            const std::vector<int64_t> startFlows = Flows(grid, start);
            const size_t faceCount = grid.faces.size();
            std::vector<int64_t> before(faceCount, 0);
            std::vector<int64_t> previous(faceCount);
            for (size_t f = 0; f < faceCount; ++f)
                previous[f] = MulWeight(stages[1].muTilde, startFlows[f]);

            for (uint32_t j = 2; j <= stageCount; ++j) {
                const std::vector<int64_t> stageFlows = Flows(grid, StageTemperatures(grid, start, previous));
                const Rkl2Stage& stage = stages[j];
                std::vector<int64_t> current(faceCount);
                for (size_t f = 0; f < faceCount; ++f) {
                    current[f] = MulWeight(stage.mu, previous[f]) + MulWeight(stage.nu, before[f]) +
                                 MulWeight(stage.muTilde, stageFlows[f]) + MulWeight(stage.gammaTilde, startFlows[f]);
                }

                before = std::move(previous);
                previous = std::move(current);
            }

            cost.stages = stageCount;
            cost.passes += 2 * static_cast<uint64_t>(stageCount);
            cost.cellUpdates += static_cast<uint64_t>(stageCount) * grid.cells.size();

            return previous;
        }

    }  // namespace

    ImplicitGrid BuildImplicitGrid(std::vector<ImplicitCell> cells, bool galerkin) {
        ImplicitGrid grid;
        grid.cells = std::move(cells);
        FindFaces(grid);
        grid.levels.push_back(MakeCellLevel(grid));
        while (grid.levels.size() < MAX_GRID_LEVELS && NeedsCoarser(grid.levels.back())) {
            ImplicitGridLevel coarser = Coarsen(grid.levels.back(), galerkin);
            if (coarser.levels.size() == grid.levels.back().levels.size())
                break;

            grid.levels.push_back(std::move(coarser));
        }

        return grid;
    }

    ImplicitCost StepImplicit(ImplicitGrid& grid, const ImplicitOptions& options) {
        ImplicitCost cost;
        const std::vector<int64_t> start = Temperatures(grid);
        if (options.method == ImplicitMethod::Rkl2) {
            std::vector<int64_t> flows = Rkl2Flows(grid, start, cost);
            LimitFlows(grid, start, options.limitSlackMillikelvin, flows, cost);
            ApplyFlows(grid, flows);

            return cost;
        }

        // --- 段 0 の右辺 = 自分の重み × 刻みの初めの温度。初めの近似 = 刻みの初めの温度 ---
        const ImplicitGridLevel& cells = grid.levels[0];
        std::vector<int64_t> rhs(start.size());
        for (size_t i = 0; i < start.size(); ++i)
            rhs[i] = MulWeight(cells.selfWeights[i], start[i]);

        std::vector<int64_t> solution = start;
        if (options.method == ImplicitMethod::RedBlack)
            Smooth(cells, rhs, solution, options.sweeps, cost);
        else {
            for (uint32_t cycle = 0; cycle < options.cycles; ++cycle) {
                VCycle(grid, 0, rhs, solution, options, cost);
                cost.cycles += 1;
                if (options.toleranceMillikelvin != 0 &&
                    Converged(cells, rhs, solution, options.toleranceMillikelvin, cost))
                    break;
            }
        }

        std::vector<int64_t> flows = Flows(grid, solution);
        LimitFlows(grid, start, options.limitSlackMillikelvin, flows, cost);
        ApplyFlows(grid, flows);
        cost.passes += 2;

        return cost;
    }

    int64_t ImplicitTemperature(const ImplicitCell& cell) {
        return ImTemperature(cell.energy, cell.heatCapacity);
    }

    int64_t ImplicitEnergyFor(uint64_t heatCapacity, int64_t millikelvin) {
        return ImEnergyFor(heatCapacity, millikelvin);
    }

    FxU128 ImplicitConservedTotal(const ImplicitGrid& grid) {
        int32_t finest = 0;
        for (const ImplicitCell& cell : grid.cells)
            finest = std::max(finest, cell.level);

        // 2 の補数の 128bit で 2^128 を法として足す(負のエネルギー〔近似の行き過ぎ〕も比べられる。等しいかを見るだけなので桁は気にしない)
        FxU128 total = Wide(0);
        for (const ImplicitCell& cell : grid.cells) {
            const auto shift = static_cast<uint32_t>(finest - cell.level) * ENERGY_BITS_PER_LEVEL;
            FX_ASSERT(shift < 64);
            const auto whole = static_cast<uint64_t>(cell.energy);
            const FxU128 value = shift == 0 ? FxU128{.hi = whole, .lo = cell.fraction}
                                            : FxU128{.hi = (whole << shift) | (cell.fraction >> (64 - shift)),
                                                     .lo = cell.fraction << shift};
            const FxU128 sum = {.hi = total.hi + value.hi, .lo = total.lo + value.lo};
            total = {.hi = sum.hi + (sum.lo < total.lo ? 1u : 0u), .lo = sum.lo};
        }

        return total;
    }

}  // namespace bicameral::sim
