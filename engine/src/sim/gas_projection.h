// gas_projection.h — 気体の圧力を刻みの中で解く投影の CPU 模型(T-0503 研究・07 §2.6・D-467・D-487 ⑥)。
// 1 刻みを substeps 回の投影で進める。1 回 = BeginGasPressure(圧力の式)→ 熱の陰解法の多重格子(ImplicitGrid の V サイクル)で
// 圧力 x を近似に解く → FinishGasPressure(面の流れ・力積・移す)。式の意味は gas_pressure_system.h。
// 気体の箱(GasBox)と帳簿は gas_reference のまま。前の回の x を初めの近似に使う(GasProjection に持つ)。
// データの流れ: GasBox(セル)→ 圧力の式 → ImplicitGrid(C_i を熱容量・K を面の係数として)→ x → GasBox(セル・帳簿)
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <cstdint>
#include <vector>

#include "sim/gas_pressure_system.h"
#include "sim/gas_reference.h"
#include "sim/implicit_conduction.h"

namespace bicameral::sim {

    struct GasProjectionOptions {
        GasPressureOptions pressure;

        // --- 多重格子(熱の陰解法の V サイクルをそのまま。cycles は上限)---
        ImplicitOptions solver = {.method = ImplicitMethod::Multigrid, .cycles = 64};
        // 止める判定: 全部のセルで、式の誤差(残差 × 対角)= 刻みの終わりの質量のずれがこれ以下(mg。1 mg は面の速さで約 0.2 mm/s)
        uint32_t toleranceMilligrams = 1;
    };

    // 前の回の圧力(µPa。セルの順。初めの近似)
    struct GasProjection {
        std::vector<int64_t> pressures;
    };

    // 1 刻みの費用
    struct GasProjectionCost {
        uint32_t cycles = 0;         // V サイクルの数(全部の回の合計)
        uint32_t maxCycles = 0;      // 1 回の投影で最も多かった数
        uint32_t unconverged = 0;    // 上限で止めた回の数
        uint32_t gridLevels = 0;     // 多重格子の段の数(最後の回)
        int64_t worstMassError = 0;  // 止めた時の式の誤差の最大(mg)
        ImplicitCost solver;         // 節の計算の数・直列の段の数
    };

    GasProjectionCost StepGasProjection(GasBox& box, GasProjection& state, const GasProjectionOptions& options);

}  // namespace bicameral::sim
