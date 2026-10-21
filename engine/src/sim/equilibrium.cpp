// equilibrium.cpp — 釣り合う家族の共通部品の CPU の回し方(T-0853。何をするかは equilibrium.h)。
// 1 回の解き(SolveEquilibrium):
//   1. 全部の節の対角 D = C + Σ 面の係数(粗い側は ÷ 8^差)+ Σ 固定した面の係数。閉じた領域によらない(外への面も固定した面として残る)
//   2. 右辺の項 ÷ D(源の項。system.rounding の形)。前の刻みと比べて種を決め、物理の長さの内の節を未知数に(閉じない時は全部)
//   3. 未知数だけの系(外への面は外の前の答えを値にした固定した面)から多重格子の段を作る(BuildImplicitGrid。木の座標で縮約)
//   4. 右辺 = 源の項 + 固定した面の項、初め = 前の答え、V サイクルを決まった回数
//   5. 答えを全部の節の値に書き戻し、面の値 F = g (x_細かい側 − x_相手)と残差の計器を出す
#include "sim/equilibrium.h"

#include <algorithm>
#include <array>
#include <map>
#include <tuple>

#include "common/equilibrium.hlsli"
#include "common/implicit_conduction.hlsli"
#include "sim/equilibrium_fields.h"

using namespace bicameral::fx;
using namespace bicameral::multires;

namespace bicameral::sim {

    namespace {

        constexpr uint32_t ENERGY_BITS_PER_LEVEL = IM_ENERGY_BITS_PER_LEVEL;

        // --- 1. 対角(全部の節。MakeCellLevel・FinishLevel と同じ和)---
        std::vector<FxU128> Diagonals(const EquilibriumSystem& system) {
            std::vector<FxU128> diagonals(system.nodes.size());
            for (size_t i = 0; i < system.nodes.size(); ++i)
                diagonals[i] = ImWide(system.nodes[i].capacity);

            for (const EquilibriumFixedFace& fixed : system.fixedFaces)
                diagonals[fixed.node] = ImWideAdd(diagonals[fixed.node], fixed.coefficient);

            for (const EquilibriumFace& face : system.faces) {
                const FxU128 coarseSide = ImWideShiftRight(face.coefficient, ENERGY_BITS_PER_LEVEL * face.gap);
                diagonals[face.fine] = ImWideAdd(diagonals[face.fine], face.coefficient);
                diagonals[face.coarse] = ImWideAdd(diagonals[face.coarse], coarseSide);
            }

            return diagonals;
        }

        // --- 2. 源の項 ÷ D(全部の節)---
        std::vector<int64_t> SourceRhs(const EquilibriumSystem& system, const std::vector<FxU128>& diagonals) {
            std::vector<int64_t> rhs(system.nodes.size(), 0);
            for (const EquilibriumSource& source : system.sources) {
                rhs[source.node] += EqSourceTerm(system.rounding, source.value, source.factor, diagonals[source.node]);
            }

            return rhs;
        }

        bool SameWide(FxU128 a, FxU128 b) {
            return a.hi == b.hi && a.lo == b.lo;
        }

        // 種: 前の刻みが無い・数が違う節は全部。右辺が seedTolerance より大きく変わった・対角が変わった節。法則が足す種
        std::vector<uint8_t> Seeds(const EquilibriumState& state, const std::vector<int64_t>& rhs,
                                   const std::vector<FxU128>& diagonals, const EquilibriumOptions& options) {
            const size_t count = rhs.size();
            if (state.sourceRhs.size() != count || state.diagonals.size() != count || state.values.size() != count)
                return std::vector<uint8_t>(count, 1);

            std::vector<uint8_t> seeds(count, 0);
            for (size_t i = 0; i < count; ++i) {
                const uint64_t change = FxAbsU64(rhs[i] - state.sourceRhs[i]);
                if (change > options.seedTolerance || !SameWide(diagonals[i], state.diagonals[i]))
                    seeds[i] = 1;
            }

            for (const uint32_t seed : options.extraSeeds)
                seeds[seed] = 1;

            if (options.residualSeed != 0 && state.residuals.size() == count) {
                for (size_t i = 0; i < count; ++i) {
                    if (state.residuals[i] > options.residualSeed)
                        seeds[i] = 1;
                }
            }

            return seeds;
        }

        // --- 閉じた領域: 節の箱(位置の単位)と、種の箱を長さで広げた箱との重なり ---
        struct NodeBox {
            std::array<int64_t, 3> low = {};
            int64_t edge = 0;
        };

        NodeBox BoxOf(const EquilibriumNode& node) {
            const int64_t edge = EqCellEdgeUnits(node.level);

            return {.low = {node.x * edge, node.y * edge, node.z * edge}, .edge = edge};
        }

        bool Within(const NodeBox& a, const NodeBox& b, int64_t length) {
            for (uint32_t axis = 0; axis < 3; ++axis) {
                if (EqAxisGap(a.low[axis], a.edge, b.low[axis], b.edge) > length)
                    return false;
            }

            return true;
        }

        using Bucket = std::tuple<int64_t, int64_t, int64_t>;

        int64_t FloorDiv(int64_t value, int64_t divisor) {
            const int64_t quotient = value / divisor;

            return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
        }

        // 種の箱を、一辺 bucket の桶に入れる(箱が重なる桶の全部へ)
        std::map<Bucket, std::vector<uint32_t>> BucketSeeds(const std::vector<NodeBox>& boxes,
                                                            const std::vector<uint8_t>& seeds, int64_t bucket) {
            std::map<Bucket, std::vector<uint32_t>> buckets;
            for (uint32_t i = 0; i < seeds.size(); ++i) {
                if (seeds[i] == 0)
                    continue;

                const NodeBox& box = boxes[i];
                const int64_t lowX = FloorDiv(box.low[0], bucket);
                const int64_t lowY = FloorDiv(box.low[1], bucket);
                const int64_t lowZ = FloorDiv(box.low[2], bucket);
                const int64_t highX = FloorDiv(box.low[0] + box.edge - 1, bucket);
                const int64_t highY = FloorDiv(box.low[1] + box.edge - 1, bucket);
                const int64_t highZ = FloorDiv(box.low[2] + box.edge - 1, bucket);
                for (int64_t z = lowZ; z <= highZ; ++z) {
                    for (int64_t y = lowY; y <= highY; ++y) {
                        for (int64_t x = lowX; x <= highX; ++x)
                            buckets[{x, y, z}].push_back(i);
                    }
                }
            }

            return buckets;
        }

        // 節 box が、どれかの種から length 以内か(箱を length 広げた範囲の桶だけ見る)
        bool CloseToSeed(const NodeBox& box, const std::vector<NodeBox>& boxes,
                         const std::map<Bucket, std::vector<uint32_t>>& buckets, int64_t bucket, int64_t length) {
            std::array<int64_t, 3> low = {};
            std::array<int64_t, 3> high = {};
            for (uint32_t axis = 0; axis < 3; ++axis) {
                low[axis] = FloorDiv(box.low[axis] - length - 1,
                                     bucket);  // 種の最後の単位が low − length − 1 でも隙間は length
                high[axis] = FloorDiv(box.low[axis] + box.edge + length, bucket);
            }

            for (int64_t z = low[2]; z <= high[2]; ++z) {
                for (int64_t y = low[1]; y <= high[1]; ++y) {
                    for (int64_t x = low[0]; x <= high[0]; ++x) {
                        const auto found = buckets.find({x, y, z});
                        if (found == buckets.end())
                            continue;

                        for (const uint32_t seed : found->second) {
                            if (Within(box, boxes[seed], length))
                                return true;
                        }
                    }
                }
            }

            return false;
        }

        // 未知数にする節(節の番号の昇順)。閉じない時は全部
        std::vector<uint32_t> Unknowns(const EquilibriumSystem& system, const std::vector<uint8_t>& seeds,
                                       const EquilibriumOptions& options) {
            const auto count = (uint32_t)system.nodes.size();
            std::vector<uint32_t> unknowns;
            if (options.closeLengthMm == 0) {
                unknowns.resize(count);
                for (uint32_t i = 0; i < count; ++i)
                    unknowns[i] = i;

                return unknowns;
            }

            std::vector<NodeBox> boxes(count);
            for (uint32_t i = 0; i < count; ++i)
                boxes[i] = BoxOf(system.nodes[i]);

            const int64_t length = EqLengthUnits(options.closeLengthMm);
            const int64_t bucket = std::max<int64_t>(length, EqCellEdgeUnits(0));
            const std::map<Bucket, std::vector<uint32_t>> buckets = BucketSeeds(boxes, seeds, bucket);
            for (uint32_t i = 0; i < count; ++i) {
                if (seeds[i] != 0 || CloseToSeed(boxes[i], boxes, buckets, bucket, length))
                    unknowns.push_back(i);
            }

            return unknowns;
        }

        // --- 3. 未知数だけの系(外への面は外の前の答えを値にした固定した面)---
        struct LocalSystem {
            ImplicitGrid grid;
            std::vector<int64_t> rhs;
        };

        int64_t FixedTerm(uint32_t rounding, FxU128 coefficient, FxU128 diagonal, int64_t value) {
            return EqSourceTerm(rounding, value, coefficient, diagonal);
        }

        LocalSystem MakeLocal(const EquilibriumSystem& system, const std::vector<uint32_t>& unknowns,
                              const std::vector<int64_t>& sourceRhs, const std::vector<FxU128>& diagonals,
                              const std::vector<int64_t>& values, const EquilibriumOptions& options) {
            const size_t count = system.nodes.size();
            std::vector<uint32_t> local(count, UINT32_MAX);
            for (uint32_t k = 0; k < unknowns.size(); ++k)
                local[unknowns[k]] = k;

            std::vector<ImplicitCell> cells(unknowns.size());
            std::vector<int64_t> rhs(unknowns.size());
            for (uint32_t k = 0; k < unknowns.size(); ++k) {
                const EquilibriumNode& node = system.nodes[unknowns[k]];
                cells[k] = {.level = node.level, .x = node.x, .y = node.y, .z = node.z, .heatCapacity = node.capacity};
                rhs[k] = sourceRhs[unknowns[k]];
            }

            std::vector<ImplicitEdge> edges;
            for (const EquilibriumFixedFace& fixed : system.fixedFaces) {
                const uint32_t k = local[fixed.node];
                if (k == UINT32_MAX)
                    continue;

                edges.push_back({.cell = k, .coefficient = fixed.coefficient, .temperature = fixed.value});
                rhs[k] += FixedTerm(system.rounding, fixed.coefficient, diagonals[fixed.node], fixed.value);
            }

            std::vector<ImplicitFace> faces;
            for (const EquilibriumFace& face : system.faces) {
                const uint32_t fine = local[face.fine];
                const uint32_t coarse = local[face.coarse];
                if (fine != UINT32_MAX && coarse != UINT32_MAX) {
                    faces.push_back({.fine = fine, .coarse = coarse, .gap = face.gap, .coefficient = face.coefficient});
                    continue;
                }

                // 片側だけ未知数: 外の節の前の答えを値にした固定した面(未知数の側の単位)
                if (fine != UINT32_MAX) {
                    edges.push_back(
                        {.cell = fine, .coefficient = face.coefficient, .temperature = values[face.coarse]});
                    rhs[fine] += FixedTerm(system.rounding, face.coefficient, diagonals[face.fine],
                                           values[face.coarse]);
                } else if (coarse != UINT32_MAX) {
                    const FxU128 coarseSide = ImWideShiftRight(face.coefficient, ENERGY_BITS_PER_LEVEL * face.gap);
                    edges.push_back({.cell = coarse, .coefficient = coarseSide, .temperature = values[face.fine]});
                    rhs[coarse] += FixedTerm(system.rounding, coarseSide, diagonals[face.coarse], values[face.fine]);
                }
            }

            LocalSystem result;
            result.grid = BuildImplicitGrid(std::move(cells), std::move(faces), options.galerkinCoarse, options.limits,
                                            std::move(edges));
            result.rhs = std::move(rhs);

            return result;
        }

        // --- 5. 面の値・残差 ---
        void FaceValues(const EquilibriumSystem& system, const std::vector<int64_t>& values,
                        EquilibriumResult& result) {
            result.faceValues.resize(system.faces.size());
            for (size_t f = 0; f < system.faces.size(); ++f) {
                const EquilibriumFace& face = system.faces[f];
                result.faceValues[f] = EqFaceValue(face.coefficient, values[face.fine], values[face.coarse]);
            }

            result.fixedValues.resize(system.fixedFaces.size());
            for (size_t f = 0; f < system.fixedFaces.size(); ++f) {
                const EquilibriumFixedFace& fixed = system.fixedFaces[f];
                result.fixedValues[f] = EqFaceValue(fixed.coefficient, values[fixed.node], fixed.value);
            }
        }

        // 未知数の残差(式の単位)を state.residuals へ・最大を計器へ
        void MeterResiduals(const LocalSystem& local, const std::vector<int64_t>& solution,
                            const std::vector<uint32_t>& unknowns, std::vector<uint64_t>& residuals,
                            EquilibriumResult& result) {
            const ImplicitGridLevel& cells = local.grid.levels[0];
            for (size_t k = 0; k < unknowns.size(); ++k) {
                const int64_t residual = ImplicitGridResidual(local.grid, local.rhs, solution, k);
                const uint64_t magnitude = EqResidualMagnitude(residual, cells.diagonals[k]);
                result.worstResidual = std::max(result.worstResidual, magnitude);
                residuals[unknowns[k]] = magnitude;
            }
        }

    }  // namespace

    EquilibriumResult SolveEquilibrium(const EquilibriumSystem& system, const EquilibriumOptions& options,
                                       EquilibriumState& state, const EquilibriumObserver* observer) {
        FX_ASSERT(options.solver.method == ImplicitMethod::Multigrid);
        const size_t count = system.nodes.size();
        EquilibriumResult result;

        // --- 1・2. 対角・源の項・種・未知数 ---
        std::vector<FxU128> diagonals = Diagonals(system);
        std::vector<int64_t> sourceRhs = SourceRhs(system, diagonals);
        const EquilibriumWarmStart& warm = options.warmStart;
        if (warm.nest != nullptr)
            state.values = LoadWarmStart(warm, system.nodes, 1);  // 前の答えは欄から(T-0922)

        const std::vector<uint8_t> seeds = Seeds(state, sourceRhs, diagonals, options);
        if (state.values.size() != count)
            state.values.assign(count, 0);

        if (state.residuals.size() != count)
            state.residuals.assign(count, 0);

        const std::vector<uint32_t> unknowns = Unknowns(system, seeds, options);
        result.unknowns = (uint32_t)unknowns.size();
        result.seeds = (uint32_t)std::ranges::count(seeds, (uint8_t)1);

        // --- 3・4. 未知数の系を決まった回数だけ解く ---
        if (!unknowns.empty()) {
            const LocalSystem local = MakeLocal(system, unknowns, sourceRhs, diagonals, state.values, options);
            std::vector<int64_t> solution(unknowns.size());
            for (size_t k = 0; k < unknowns.size(); ++k)
                solution[k] = state.values[unknowns[k]];

            const std::vector<int64_t> before = observer != nullptr ? solution : std::vector<int64_t>{};
            for (uint32_t cycle = 0; cycle < options.solver.cycles; ++cycle)
                CycleImplicitGrid(local.grid, local.rhs, solution, options.solver, result.cost);

            result.gridLevels = (uint32_t)local.grid.levels.size();
            MeterResiduals(local, solution, unknowns, state.residuals, result);
            if (observer != nullptr) {
                (*observer)(
                    {.grid = local.grid, .nodes = unknowns, .rhs = local.rhs, .before = before, .after = solution});
            }

            for (size_t k = 0; k < unknowns.size(); ++k)
                state.values[unknowns[k]] = solution[k];
        }

        if (warm.nest != nullptr)
            (void)StoreWarmStart(warm, system.nodes, state.values, 1);

        // --- 5. 面の値と、次の刻みの種のための控え ---
        if (options.faceValues)
            FaceValues(system, state.values, result);

        if (options.meterResiduals)
            result.residuals = state.residuals;

        state.sourceRhs = std::move(sourceRhs);
        state.diagonals = std::move(diagonals);

        return result;
    }

}  // namespace bicameral::sim
