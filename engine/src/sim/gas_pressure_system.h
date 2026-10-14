// gas_pressure_system.h — 気体の圧力を刻みの中で解く投影の模型(T-0503 研究・07 §2.6)の、気体の箱の側の部品(gas_reference.cpp の末尾で定義)。
// 陽解法の小刻み(gas_reference の Substep)は圧力を「今の状態の p̃」で決めるので、音速 c̃ に比例して小刻みが要る(T-0226)。
// ここでは 1 回の投影(Δt = 1/60 s ÷ substeps)の終わりの圧力 x(µPa。p̃ と同じ意味)を陰的に決める(後退 Euler の音。c̃ → ∞ で非圧縮の投影):
//   面の質量の流れ F_f = F*_f + K (x_左 − x_右)、F* = 中心の流れ(今の運動量)− K × 両側の質量の差の静水圧(縦の面。07 §2.1 と同じ釣り合い)
//   K = A Δt² ÷ Δx(運動量 ρu で書くので密度によらない)
//   セルの圧力の変わり方 x_i = p̃_i + c_i² × (質量の変化)÷ V(線形にした状態方程式。c_i² = c̃² T_i ÷ T_地面)
//   → C_i x_i + Σ_f K (x_i − x_j) = E_i − Σ_f F*_f(C_i = V ÷ c_i²・E_i = V p̃_i ÷ c_箱,i²〔c̃ によらない。断熱の基準からの質量の差〕)
// これは熱の陰解法 C (T − T0) = Σ g (Tj − Ti) と同じ形なので、熱の多重格子(ImplicitGrid)で解ける(T-0494 の手がかり。解くのは gas_projection.cpp)。
// 解いた x で、面の流れ・面の圧力の力積(両側の平均。陽解法と同じ部品)・重さを当て、風上の中身を移す(G2 の部品をそのまま)。
// x が近似(V サイクルを途中で止めた)でも、保存はビット単位のまま(面ごとに 1 つの値を両側で逆向きに足し引きする形。近似が効くのは速さだけ)。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "sim/gas_reference.h"

namespace bicameral::sim {

    struct GasPressureOptions {
        uint32_t substeps = 1;          // 1 刻みを何回の投影で進めるか(音速の安定の条件は無い)
        uint32_t soundSpeedMmPerS = 0;  // 圧力の伝わる速さ c̃(mm/s)。0 = 最速(C を下限 GAS_PRESSURE_MIN_CAPACITY にする)
    };

    // 最速のときの C_i(Q48)。0 にすると閉じた箱の最も粗い段の対角が 0 になるので、小さい下限を残す
    // (2^20 は c̃ 約 5.8 km/s = 1 刻みに約 190 セル伝わる速さ。0.5 m のセル・1/60 s)
    inline constexpr uint64_t GAS_PRESSURE_MIN_CAPACITY = (uint64_t)1 << 20;

    // 箱の中のセルどうしの面(周期の面も)
    struct GasPressureLink {
        uint32_t left = 0;
        uint32_t right = 0;
    };

    struct GasPressureSystem {
        // --- 節(セルの番号の順)---
        std::vector<uint64_t> capacities;       // C_i(mg/µPa の Q48)。開いた境界の面の K も足してある(外は x = 0)
        std::vector<uint64_t> stateCapacities;  // V ÷ c_箱,i²(mg/µPa の Q48。E_i = これ × statePressures)
        std::vector<int64_t> statePressures;    // p̃_i = α(p − p_基準)(µPa。頭打ちなし)
        std::vector<int64_t> divergences;       // Σ 出ていく F*(mg)

        // --- 面 ---
        std::vector<GasPressureLink> links;
        uint64_t linkCoefficient = 0;         // K(mg/µPa の Q48)
        std::vector<int64_t> predictedFlows;  // F*(mg。面の一覧の順〔gas_reference の EnumerateFaces〕。壁は 0)
    };

    // 今の状態から圧力の式を作る
    [[nodiscard]] GasPressureSystem BeginGasPressure(const GasBox& box, const GasPressureOptions& options);

    // 解いた圧力 pressures(µPa。セルの順)で 1 回の投影を当てる。roundingTick は乱数の丸めの通し番号(刻み × substeps + 何回目)。
    // box.tick は進めない(呼ぶ側が 1 刻みの終わりに進める)
    void FinishGasPressure(GasBox& box, const GasPressureOptions& options, const GasPressureSystem& system,
                           std::span<const int64_t> pressures, uint64_t roundingTick);

}  // namespace bicameral::sim
