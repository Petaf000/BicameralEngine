// gpu_gas_internal.h — 気体の GPU 版(gpu_gas.cpp の 1 レベルの箱・gpu_gas_tree.cpp の多重解像度の木・probe_sim_gas.cpp の仮の世界。
// T-0185・T-0209・T-0592)が共有する、CPU の形(sim/gas_reference.h)と GPU の置き場所の形(shaders/common/gas_gpu.hlsli)の写し。
// 成分ごとの欄の並び(T-0598): 状態は頭の並び + 成分ごとの面、外・設定・帳簿は「頭 + 欄 × 欄の数」の件。定義は gpu_gas.cpp(bicameral_sim の外からは使わない)。
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "common/gas_gpu.hlsli"
#include "sim/gas_reference.h"

namespace bicameral::sim {

    // 1 件の成分の欄の数(成分の数と、呼ぶ側が見込む数の大きい方。少なくとも 1)
    [[nodiscard]] uint32_t GasGpuSpeciesStride(uint32_t speciesCount, uint32_t reserved = 0);

    // セル → 件(頭 + 物質量 × speciesStride。開いた境界の外)。1 件 / 並べたもの
    void PackGasGpuCell(const GasCell& cell, uint32_t speciesStride, std::span<std::byte> record);
    [[nodiscard]] std::vector<std::byte> PackGasGpuCellRecords(std::span<const GasCell> cells, uint32_t speciesStride);

    // セル ↔ 頭の並び + 成分ごとの面(gas_gpu.hlsli の GasGpuPlaneAddress。状態)。戻す時は speciesCount 個の欄だけ
    [[nodiscard]] std::vector<std::byte> PackGasGpuCellPlanes(std::span<const GasCell> cells, uint32_t speciesStride);
    [[nodiscard]] std::vector<GasCell> UnpackGasGpuCellPlanes(std::span<const std::byte> bytes, uint32_t cellCount,
                                                              uint32_t speciesCount);
    [[nodiscard]] GasGpuDerived ToGasGpuDerived(const GasDerived& derived);

    // 設定と係数(t0。木ではレベル 0 の係数。レベルで変わる係数は別の表)。頭だけ / 成分 1 つ / 頭 + 成分 × speciesStride
    [[nodiscard]] GasGpuParameters MakeGasGpuParameters(const GasConfig& config, const GasCoefficients& coefficients);
    [[nodiscard]] GasGpuSpecies MakeGasGpuSpecies(const GasConfig& config, const GasCoefficients& coefficients,
                                                  uint32_t slot);
    [[nodiscard]] std::vector<std::byte> PackGasGpuParameters(const GasConfig& config,
                                                              const GasCoefficients& coefficients,
                                                              uint32_t speciesStride);

}  // namespace bicameral::sim
