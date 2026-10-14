// gas_projection.cpp — 気体の圧力を刻みの中で解く投影の CPU 模型(T-0503 研究)。何をするかは gas_projection.h。
// 1 回の投影の順(StepGasProjection):
//   1. 圧力の式を作る(BeginGasPressure)→ セルを熱の陰解法の節に(熱容量 = C_i、面の係数 = K)して多重格子の段を作る(BuildImplicitGrid)
//   2. 右辺 = (E_i − Σ F*_i)÷ D_i(D_i = C_i + Σ K。128bit の積から 1 回の割り算)
//   3. V サイクルを、全部のセルの式の誤差(残差 × D_i)が許容差(mg)以下になるまで(上限 options.solver.cycles)
//   4. 解いた x で当てる(FinishGasPressure)
#include "sim/gas_projection.h"

#include <algorithm>
#include <cassert>

#include "common/fixed.hlsli"

namespace bicameral::sim {

    namespace {

        namespace fx = bicameral::fx;

        constexpr uint32_t CAPACITY_SHIFT = 48;  // C・K の Q(mg/µPa)

        // 符号付きの (|a| × b)÷ d(128bit の積。0 方向へ切り捨て)
        int64_t MulDivSigned(int64_t a, uint64_t b, uint64_t d) {
            const fx::FxU128 product = fx::FxMulU64Full(fx::FxAbsU64(a), b);
            const uint64_t quotient = fx::FxDivU128By64(product, d).quotient;

            return fx::FxApplySign(quotient, a < 0);
        }

        // セルを節に、面をつながりにした多重格子
        ImplicitGrid MakePressureGrid(const GasBox& box, const GasPressureSystem& system) {
            const uint32_t sizeX = box.config.sizeX;
            const uint32_t sizeY = box.config.sizeY;
            std::vector<ImplicitCell> cells(box.cells.size());
            for (uint32_t index = 0; index < cells.size(); ++index) {
                ImplicitCell& cell = cells[index];
                cell.x = index % sizeX;
                cell.y = (index / sizeX) % sizeY;
                cell.z = index / (sizeX * sizeY);
                cell.heatCapacity = system.capacities[index];
            }

            std::vector<ImplicitFace> faces;
            faces.reserve(system.links.size());
            for (const GasPressureLink& link : system.links) {
                ImplicitFace face;
                face.fine = std::min(link.left, link.right);
                face.coarse = std::max(link.left, link.right);
                face.coefficient = {.hi = 0, .lo = system.linkCoefficient};
                faces.push_back(face);
            }

            return BuildImplicitGrid(std::move(cells), std::move(faces));
        }

        // 右辺(値の単位 µPa)=(V p̃ ÷ c_箱² − Σ F* × 2^48)÷ D
        std::vector<int64_t> PressureRhs(const ImplicitGrid& grid, const GasPressureSystem& system) {
            const ImplicitGridLevel& cells = grid.levels[0];
            std::vector<int64_t> rhs(system.capacities.size());
            for (size_t index = 0; index < rhs.size(); ++index) {
                assert(cells.diagonals[index].hi == 0);
                const uint64_t diagonal = cells.diagonals[index].lo;
                const int64_t state = MulDivSigned(system.statePressures[index], system.stateCapacities[index],
                                                   diagonal);
                const int64_t outflow = MulDivSigned(system.divergences[index], (uint64_t)1 << CAPACITY_SHIFT,
                                                     diagonal);
                rhs[index] = state - outflow;
            }

            return rhs;
        }

        // 式の誤差の最大(mg)= max |残差 × D_i| ÷ 2^48
        int64_t WorstMassError(const ImplicitGrid& grid, const std::vector<int64_t>& rhs,
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

        // 1 回の投影
        void Project(GasBox& box, GasProjection& state, const GasProjectionOptions& options, uint64_t roundingTick,
                     GasProjectionCost& cost) {
            const GasPressureSystem system = BeginGasPressure(box, options.pressure);
            const ImplicitGrid grid = MakePressureGrid(box, system);
            const std::vector<int64_t> rhs = PressureRhs(grid, system);
            state.pressures.resize(box.cells.size(), 0);

            // --- V サイクル(許容差に届いたら止める。上限は options.solver.cycles)---
            std::vector<int64_t>& values = state.pressures;
            uint32_t cycles = 0;
            int64_t error = WorstMassError(grid, rhs, values);
            while (error > (int64_t)options.toleranceMilligrams && cycles < options.solver.cycles) {
                CycleImplicitGrid(grid, rhs, values, options.solver, cost.solver);
                ++cycles;
                error = WorstMassError(grid, rhs, values);
            }

            cost.cycles += cycles;
            cost.maxCycles = std::max(cost.maxCycles, cycles);
            cost.unconverged += error > (int64_t)options.toleranceMilligrams ? 1 : 0;
            cost.gridLevels = (uint32_t)grid.levels.size();
            cost.worstMassError = std::max(cost.worstMassError, error);

            FinishGasPressure(box, options.pressure, system, values, roundingTick);
        }

    }  // namespace

    GasProjectionCost StepGasProjection(GasBox& box, GasProjection& state, const GasProjectionOptions& options) {
        GasProjectionCost cost;
        const uint32_t substeps = options.pressure.substeps;
        for (uint32_t substep = 0; substep < substeps; ++substep)
            Project(box, state, options, (box.tick * substeps) + substep, cost);

        ++box.tick;

        return cost;
    }

}  // namespace bicameral::sim
