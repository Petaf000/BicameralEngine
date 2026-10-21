// case_b_pressure.cpp — 案 B の試作の箱(T-0866)の陰の圧力の口。C-0(釣り合う家族の共通部品)が無い間の解き方。
// 式 C_i x_i + Σ_f K_f (x_i − x_j) = b_i を、熱の陰解法の多重格子(implicit_conduction)の一般の右辺の V サイクルで解く
// (T-0503 の気体の投影の模型 gas_projection.cpp と同じ使い方: C を熱容量・K を面の係数・右辺 = b × 2^48 ÷ D)。
// C-0 ができたら、同じ PressureSolver の形でそれを包んだ解き方に差し替える(箱と段は変えない)。
#include <algorithm>
#include <cassert>
#include <utility>

#include "common/fixed.hlsli"
#include "sim/case_b/case_b_box.h"
#include "sim/implicit_conduction.h"

namespace bicameral::sim::case_b {

    namespace {

        namespace fx = bicameral::fx;

        constexpr uint32_t CAPACITY_SHIFT = 48;  // C・K の Q(mg/µPa)

        ImplicitGrid MakeGrid(const PressureSystem& system) {
            std::vector<ImplicitCell> cells(system.capacities.size());
            for (uint32_t index = 0; index < cells.size(); ++index) {
                ImplicitCell& cell = cells[index];
                cell.x = index % system.sizeX;
                cell.y = (index / system.sizeX) % system.sizeY;
                cell.z = index / (system.sizeX * system.sizeY);
                cell.heatCapacity = system.capacities[index];
            }

            std::vector<ImplicitFace> faces;
            faces.reserve(system.links.size());
            for (const PressureLink& link : system.links) {
                ImplicitFace face;
                face.fine = std::min(link.left, link.right);
                face.coarse = std::max(link.left, link.right);
                face.coefficient = {.hi = 0, .lo = link.coefficient};
                faces.push_back(face);
            }

            return BuildImplicitGrid(std::move(cells), std::move(faces));
        }

        // 右辺(値の単位 µPa)= b × 2^48 ÷ D
        std::vector<int64_t> MakeRhs(const ImplicitGrid& grid, const PressureSystem& system) {
            const ImplicitGridLevel& cells = grid.levels[0];
            std::vector<int64_t> rhs(system.sources.size());
            for (size_t index = 0; index < rhs.size(); ++index) {
                assert(cells.diagonals[index].hi == 0);
                rhs[index] = fx::FxMulDivS64(system.sources[index], (uint64_t)1 << CAPACITY_SHIFT,
                                             cells.diagonals[index].lo);
            }

            return rhs;
        }

        int64_t WorstError(const ImplicitGrid& grid, const std::vector<int64_t>& rhs,
                           const std::vector<int64_t>& values) {
            const ImplicitGridLevel& cells = grid.levels[0];
            uint64_t worst = 0;
            for (size_t index = 0; index < values.size(); ++index) {
                const int64_t residual = ImplicitGridResidual(grid, rhs, values, index);
                const fx::FxU128 error = fx::FxMulU64Full(fx::FxAbsU64(residual), cells.diagonals[index].lo);
                worst = std::max(worst, fx::FxShiftRightU128(error, CAPACITY_SHIFT));
            }

            return (int64_t)worst;
        }

    }  // namespace

    PressureSolver MakeImplicitGridSolver(uint32_t maxCycles, int64_t toleranceMg) {
        return [maxCycles, toleranceMg](const PressureSystem& system, std::vector<int64_t>& values) {
            assert(values.size() == system.capacities.size());
            const ImplicitGrid grid = MakeGrid(system);
            const std::vector<int64_t> rhs = MakeRhs(grid, system);
            const ImplicitOptions options;
            ImplicitCost cost;
            PressureCost result;
            result.worstErrorMg = WorstError(grid, rhs, values);
            while (result.worstErrorMg > toleranceMg && result.cycles < maxCycles) {
                CycleImplicitGrid(grid, rhs, values, options, cost);
                ++result.cycles;
                result.worstErrorMg = WorstError(grid, rhs, values);
            }

            return result;
        };
    }

    int64_t PressureSystemWorstError(const PressureSystem& system, std::span<const int64_t> values) {
        // 式の誤差 |C_i x_i + Σ K (x_i − x_j) − b_i|(mg。項ごとに Q48 を落とすので ±面の数 mg の丸めを含む。試験の目安用)
        std::vector<int64_t> totals(values.size());
        for (size_t index = 0; index < values.size(); ++index)
            totals[index] = fx::FxMulShiftS64(values[index], (int64_t)system.capacities[index], CAPACITY_SHIFT);

        for (const PressureLink& link : system.links) {
            const int64_t flow = fx::FxMulShiftS64(values[link.left] - values[link.right], (int64_t)link.coefficient,
                                                   CAPACITY_SHIFT);
            totals[link.left] += flow;
            totals[link.right] -= flow;
        }

        int64_t worst = 0;
        for (size_t index = 0; index < values.size(); ++index)
            worst = std::max(worst, (int64_t)fx::FxAbsU64(totals[index] - system.sources[index]));

        return worst;
    }

}  // namespace bicameral::sim::case_b
