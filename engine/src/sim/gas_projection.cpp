// gas_projection.cpp — 気体の圧力を刻みの中で解く投影の CPU 模型(T-0503 研究)。何をするかは gas_projection.h。
// 1 回の投影の順(StepGasProjection):
//   1. 圧力の式を作る(BeginGasPressure)→ セルを熱の陰解法の節に(熱容量 = C_i、面の係数 = K)して多重格子の段を作る(BuildImplicitGrid)
//   2. 右辺 = (E_i − Σ F*_i)÷ D_i(D_i = C_i + Σ K。128bit の積から 1 回の割り算)
//   3. V サイクルを、全部のセルの式の誤差(残差 × D_i)が許容差(mg)以下になるまで(上限 options.solver.cycles)
//   4. 解いた x を箱の写しに仮に当てる(FinishGasPressure)
//   5. 外側の反復(T-0530): 仮の状態の本当の質量の差 E_真 と C_i x_i の食い違い G が許容差を超えたら、右辺に G ÷ s_i ÷ D_i を足して 3 へ
//      (s_i はセルの傾き = E_真 の変化 ÷ 質量の変化の割線。最速では対角のニュートンと同じ)。
//      止まったら(上限でも)仮の状態を採る
#include "sim/gas_projection.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>

#include "common/fixed.hlsli"

namespace bicameral::sim {

    namespace {

        namespace fx = bicameral::fx;

        constexpr uint32_t CAPACITY_SHIFT = 48;  // C・K の Q(mg/µPa)
        constexpr uint32_t MISMATCH_SHIFT =
            32;  // 食い違い G を mg の Q16 で持つ(C × x の Q48 から 32 ずらす。最速の大きな x でも 64bit に入る)

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

        // V サイクル(許容差に届いたら止める。上限は options.solver.cycles)
        void Solve(const ImplicitGrid& grid, const std::vector<int64_t>& rhs, std::vector<int64_t>& values,
                   const GasProjectionOptions& options, GasProjectionCost& cost) {
            std::vector<int64_t> before;
            if (options.solveObserver != nullptr)
                before = values;

            const auto tolerance = (int64_t)GasInnerToleranceMilligrams(options);
            uint32_t cycles = 0;
            int64_t error = WorstMassError(grid, rhs, values);
            while (error > tolerance && cycles < options.solver.cycles) {
                CycleImplicitGrid(grid, rhs, values, options.solver, cost.solver);
                ++cycles;
                error = WorstMassError(grid, rhs, values);
            }

            cost.cycles += cycles;
            cost.maxCycles = std::max(cost.maxCycles, cycles);
            cost.unconverged += error > tolerance ? 1 : 0;
            cost.worstMassError = std::max(cost.worstMassError, error);
            if (options.solveObserver != nullptr) {
                (*options.solveObserver)({.grid = grid,
                                          .rhs = rhs,
                                          .before = before,
                                          .after = values,
                                          .cycles = cycles,
                                          .massError = error});
            }
        }

        // 外側の反復で比べる量(セルの順): E_真 = V p̃ ÷ c_箱²(mg の Q16。式の E_i と同じ量)と質量(mg)
        struct OuterState {
            std::vector<int64_t> excesses;
            std::vector<int64_t> masses;
        };

        OuterState MeasureState(const GasBox& box, const GasPressureSystem& system) {
            OuterState state;
            state.excesses.resize(box.cells.size());
            state.masses.resize(box.cells.size());
            const uint32_t layer = box.config.sizeX * box.config.sizeY;
            for (uint32_t index = 0; index < box.cells.size(); ++index) {
                state.excesses[index] = fx::FxMulShiftS64(system.statePressures[index],
                                                          (int64_t)system.stateCapacities[index], MISMATCH_SHIFT);
                state.masses[index] = (int64_t)DeriveGasCell(box, box.cells[index], index / layer).mass;
            }

            return state;
        }

        // 食い違い G_i = E_真,i − C_i x_i(mg の Q16)。式が仮定した刻みの終わりの圧力 x と、仮の状態の本当の圧力の差を質量で測る。
        // 最大の |G|(mg)を返す
        int64_t Mismatch(const OuterState& trial, const GasPressureSystem& system, const std::vector<int64_t>& values,
                         std::vector<int64_t>& mismatches) {
            mismatches.resize(values.size());
            uint64_t worst = 0;
            for (size_t index = 0; index < values.size(); ++index) {
                const int64_t assumed = fx::FxMulShiftS64(values[index], (int64_t)system.cellCapacities[index],
                                                          MISMATCH_SHIFT);
                mismatches[index] = trial.excesses[index] - assumed;
                worst = std::max(worst, fx::FxAbsU64(mismatches[index]) >> (CAPACITY_SHIFT - MISMATCH_SHIFT));
            }

            return (int64_t)worst;
        }

        // セルの傾き s_i = ΔE_真 ÷ Δ質量(Q16。前の状態と今の仮の状態の割線)。式は s = 1(質量の変化がそのまま E を変える)と仮定するが、
        // 熱い気体を受けるセル・断熱で膨らむセルでは違う。質量があまり動かないセルと、外れた値は 1 にする
        int64_t SecantSlope(const OuterState& before, const OuterState& after, size_t index) {
            constexpr int64_t ONE = (int64_t)1 << 16;
            constexpr int64_t MIN_MASS_CHANGE = 16;  // mg
            const int64_t massChange = after.masses[index] - before.masses[index];
            if (std::abs(massChange) < MIN_MASS_CHANGE)
                return ONE;

            const int64_t slope = (after.excesses[index] - before.excesses[index]) / massChange;

            return std::clamp(slope, ONE / 4, ONE * 4);
        }

        // 右辺に G ÷ s ÷ D_i を足す(右辺は値の単位 µPa。PressureRhs と同じ形)
        void CorrectRhs(const ImplicitGrid& grid, const std::vector<int64_t>& mismatches, const OuterState* before,
                        const OuterState& after, const GasProjectionOptions& options, std::vector<int64_t>& rhs) {
            const ImplicitGridLevel& cells = grid.levels[0];
            for (size_t index = 0; index < rhs.size(); ++index) {
                int64_t relaxed = mismatches[index] * (int64_t)options.outerRelaxationPercent / 100;
                if (options.outerSecant && before != nullptr)
                    relaxed = relaxed * ((int64_t)1 << 16) / SecantSlope(*before, after, index);

                rhs[index] += MulDivSigned(relaxed, (uint64_t)1 << MISMATCH_SHIFT, cells.diagonals[index].lo);
            }
        }

        // 1 回の投影(外側の反復を含む)
        void Project(GasBox& box, GasProjection& state, const GasProjectionOptions& options, uint64_t roundingTick,
                     GasProjectionCost& cost) {
            const GasPressureSystem system = BeginGasPressure(box, options.pressure);
            const ImplicitGrid grid = MakePressureGrid(box, system);
            std::vector<int64_t> rhs = PressureRhs(grid, system);
            state.pressures.resize(box.cells.size(), 0);
            cost.gridLevels = (uint32_t)grid.levels.size();

            std::vector<int64_t>& values = state.pressures;
            std::vector<int64_t> mismatches;
            OuterState previous = MeasureState(box, system);
            const uint32_t limit = std::max<uint32_t>(options.outerIterations, 1);
            for (uint32_t outer = 1;; ++outer) {
                Solve(grid, rhs, values, options, cost);

                // --- 仮に当てて、食い違いを測る ---
                GasBox trial = box;
                FinishGasPressure(trial, options.pressure, system, values, roundingTick);
                OuterState measured = MeasureState(trial, BeginGasPressure(trial, options.pressure));
                const int64_t mismatch = Mismatch(measured, system, values, mismatches);
                if (outer == 1)
                    cost.firstMismatch = std::max(cost.firstMismatch, mismatch);

                const bool settled = mismatch <= (int64_t)options.outerToleranceMilligrams;
                if (settled || outer >= limit) {
                    cost.outerIterations += outer;
                    cost.maxOuterIterations = std::max(cost.maxOuterIterations, outer);
                    cost.outerUnconverged += settled ? 0 : 1;
                    cost.worstMismatch = std::max(cost.worstMismatch, mismatch);
                    box = std::move(trial);  // セルと帳簿(FinishGasPressure が両方を書く)
                    return;
                }

                CorrectRhs(grid, mismatches, &previous, measured, options, rhs);
                previous = std::move(measured);
            }
        }

    }  // namespace

    uint32_t GasInnerToleranceMilligrams(const GasProjectionOptions& options) {
        if (options.toleranceMilligrams != 0)
            return options.toleranceMilligrams;

        return std::max(options.outerToleranceMilligrams / INNER_TOLERANCE_DIVISOR, 1u);
    }

    std::vector<int64_t> GasPressureThresholds(const ImplicitGrid& grid, uint32_t toleranceMilligrams) {
        const ImplicitGridLevel& cells = grid.levels[0];
        const uint64_t allowed = (uint64_t)toleranceMilligrams + 1;
        const fx::FxU128 numerator = {.hi = allowed >> (64 - CAPACITY_SHIFT), .lo = allowed << CAPACITY_SHIFT};
        std::vector<int64_t> thresholds(cells.diagonals.size());
        for (size_t index = 0; index < thresholds.size(); ++index) {
            assert(cells.diagonals[index].hi == 0);
            const fx::FxDivResult division = fx::FxDivU128By64(numerator, cells.diagonals[index].lo);
            const uint64_t ceiling = division.quotient + (division.remainder != 0 ? 1 : 0);
            thresholds[index] = (int64_t)std::min<uint64_t>(ceiling, (uint64_t)INT64_MAX);
        }

        return thresholds;
    }

    GasProjectionCost StepGasProjection(GasBox& box, GasProjection& state, const GasProjectionOptions& options) {
        assert(box.openQ16.empty());  // セルの空き(T-0593)は陽解法だけ。投影は T-0617
        GasProjectionCost cost;
        const uint32_t substeps = options.pressure.substeps;
        for (uint32_t substep = 0; substep < substeps; ++substep)
            Project(box, state, options, (box.tick * substeps) + substep, cost);

        ++box.tick;

        return cost;
    }

}  // namespace bicameral::sim
