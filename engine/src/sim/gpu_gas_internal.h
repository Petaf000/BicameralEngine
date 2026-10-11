// gpu_gas_internal.h — 気体の GPU 版(gpu_gas.cpp の 1 レベルの箱・gpu_gas_tree.cpp の多重解像度の木。T-0185・T-0209)が共有する、
// CPU の形(sim/gas_reference.h)と GPU の置き場所の形(shaders/common/gas_gpu.hlsli)の写し。定義は gpu_gas.cpp(bicameral_sim の外からは使わない)。
#pragma once

#include "common/gas_gpu.hlsli"
#include "sim/gas_reference.h"

namespace bicameral::sim {

    [[nodiscard]] GasGpuCell ToGasGpuCell(const GasCell& cell);
    [[nodiscard]] GasCell FromGasGpuCell(const GasGpuCell& cell);
    [[nodiscard]] GasGpuDerived ToGasGpuDerived(const GasDerived& derived);

    // 設定と係数(t0。木ではレベル 0 の係数。レベルで変わる係数は別の表)
    [[nodiscard]] GasGpuParameters MakeGasGpuParameters(const GasConfig& config, const GasCoefficients& coefficients);

}  // namespace bicameral::sim
