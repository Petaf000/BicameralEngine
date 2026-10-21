// gas_projection.h — 気体の圧力を刻みの中で解く投影の CPU 模型(T-0503 研究・07 §2.6・D-467・D-487 ⑥)。
// 1 刻みを substeps 回の投影で進める。1 回 = BeginGasPressure(圧力の式)→ 熱の陰解法の多重格子(ImplicitGrid の V サイクル)で
// 圧力 x を近似に解く → FinishGasPressure(面の流れ・力積・移す)。式の意味は gas_pressure_system.h。
// 外側の反復(T-0530): 式は刻みの初めで状態方程式を線形にしているので、強い圧力の差(爆発)は 1 回では釣り合わない。
// x を箱の写しに仮に当て、仮の状態の本当の「基準との質量の差」E_真 と、式が仮定した C_i x_i の食い違い G を右辺に足して解き直す(ニュートン型)。
// G が許容差以下になった(または上限に届いた)仮の状態をそのまま採る。当てるのは 1 回分なので保存はビット単位のまま。
// 気体の箱(GasBox)と帳簿は gas_reference のまま。前の回の x を初めの近似に使う(GasProjection に持つ)。
// データの流れ: GasBox(セル)→ 圧力の式 → ImplicitGrid(C_i を熱容量・K を面の係数として)→ x → GasBox(セル・帳簿)
// セルの空き(GasBox::openQ16)は式が陽解法と同じ数え方で読む(T-0667・ADR-0312。gas_pressure_system.h)。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "sim/equilibrium.h"
#include "sim/gas_pressure_system.h"
#include "sim/gas_reference.h"
#include "sim/implicit_conduction.h"

namespace bicameral::sim {

    // 1 回の V サイクルの解き(Solve)を外から見る口(T-0531。GPU の試験が同じ系を GPU で解き直してビット一致を確かめ、時間を測る)。
    // before は解く前の近似、after は解いた値、cycles は回した V サイクルの数(0 もある。V の前に判定する)
    struct GasPressureSolve {
        const ImplicitGrid& grid;
        std::span<const int64_t> rhs;
        std::span<const int64_t> before;
        std::span<const int64_t> after;
        uint32_t cycles = 0;
        int64_t massError = 0;  // 止めた時の式の誤差の最大(mg)
    };

    using GasPressureSolveObserver = std::function<void(const GasPressureSolve&)>;

    struct GasProjectionOptions {
        GasPressureOptions pressure;

        // --- 多重格子(熱の陰解法の V サイクルをそのまま。cycles は 1 回の解きの上限)---
        // 内側の解きの止め方は外側の許容差から決める(ADR-0254。仮・ユーザー未確認): 外側の反復は解きの残りも食い違い G として測って直すので、
        // 外側の許容差より細かく解いても採る状態は良くならない(不正確なニュートン)。上限 8 で 1 刻みの V の最大が
        // 「外側の上限 × 8」に決まる(D-468)。GPU は記録する回が 64 → 8 になり、止めた後の回の費用が約 1/8(T-0531)
        ImplicitOptions solver = {.method = ImplicitMethod::Multigrid, .cycles = 8};
        // 止める判定: 全部のセルで、式の誤差(残差 × 対角)= 刻みの終わりの質量のずれがこれ以下(mg。1 mg は面の速さで約 0.2 mm/s)。
        // 0 なら外側の許容差 ÷ INNER_TOLERANCE_DIVISOR(GasInnerToleranceMilligrams。既定 100 mg → 10 mg)
        uint32_t toleranceMilligrams = 0;
        // 粗い段の面の係数を子の和のまま(Galerkin)にする(既定は熱と同じ ÷ 2。T-0667 の研究の比べだけ)
        bool galerkinCoarse = false;
        // V サイクルを許容差で止めず、ちょうど solver.cycles 回まわす(T-0842 研究。費用を決まった回数に固定する)。
        // 外側の反復が 1(解き直さない)のときは、仮の状態の食い違いも測らない(費用に入れない。cost の食い違いは 0)
        bool fixedCycles = false;
        // 釣り合う家族の共通部品(equilibrium.h。T-0853)の上で解く。fixedCycles と outerIterations = 1(T-0845 の形)だけ。
        // 前の道(熱の多重格子を直に呼ぶ)とビット単位で同じ(gas_projection_test --t0853)。既定は前の道(GPU の比べ T-0915 の後で替える)
        bool equilibrium = false;
        // 部品の閉じた領域の長さ(mm。0 = 閉じない。20 §9.3 の陰の圧力 = 音の速さ × 刻みの倍数。equilibrium の時だけ)
        uint64_t closeLengthMm = 0;

        // --- 外側の反復(T-0530。上限に届いたら、その時の仮の状態を採り、残りの食い違いは次の刻みへ持ち越す〔回数は決まった上限まで。D-468〕)---
        // 上限(1 = 解き直さない〔T-0503 の線形の 1 回〕)。既定の 4・100 mg・傾きで割るは仮(ユーザー未確認。ADR-0234)
        uint32_t outerIterations = 4;
        // 全部のセルで |E_真 − C_i x_i| がこれ以下なら止める(mg。地面の温度で 1 mg は約 0.9 Pa の圧力の食い違い。熱いほど T ÷ T_地面 倍)
        uint32_t outerToleranceMilligrams = 100;
        // 右辺に足す食い違いの倍率(%。100 = そのまま)
        uint32_t outerRelaxationPercent = 100;
        // 食い違いをセルの傾き s_i(E_真 の変化 ÷ 質量の変化。前の状態との割線)で割ってから足す(対角のニュートン)
        bool outerSecant = true;

        // 解きを見る口(null なら呼ばない。試験と計測だけ)
        const GasPressureSolveObserver* solveObserver = nullptr;
    };

    // 前の回の圧力(µPa。セルの順。初めの近似)
    struct GasProjection {
        std::vector<int64_t> pressures;
        EquilibriumState equilibrium;  // 部品の上で解く時の温めた始まり(T-0853。pressures はその値の写し)
    };

    // 1 刻みの費用
    struct GasProjectionCost {
        uint32_t cycles = 0;         // V サイクルの数(全部の回の合計)
        uint32_t maxCycles = 0;      // 1 回の投影で最も多かった数
        uint32_t unconverged = 0;    // 上限で止めた回の数
        uint32_t gridLevels = 0;     // 多重格子の段の数(最後の回)
        int64_t worstMassError = 0;  // 止めた時の式の誤差の最大(mg)

        // --- 外側の反復(T-0530)---
        uint32_t outerIterations = 0;     // 仮に当てた回数(全部の回の合計。1 回の投影につき 1 以上)
        uint32_t maxOuterIterations = 0;  // 1 回の投影で最も多かった数
        uint32_t outerUnconverged = 0;    // 上限で止めた回の数(最速の約束〔D-467〕を守れなかった回)
        int64_t worstMismatch = 0;        // 採った状態の食い違い |E_真 − C_i x_i| の最大(mg)
        int64_t firstMismatch = 0;        // 1 回目(解き直す前)の食い違いの最大(mg。線形にした誤差の大きさ)
        ImplicitCost solver;              // 節の計算の数・直列の段の数

        // --- 部品の上で解いた時(T-0853)---
        uint32_t unknowns = 0;       // 未知数にしたセル(閉じた領域)
        uint64_t worstResidual = 0;  // 残差の計器(mg。刻みの終わりの質量のずれの最大)
    };

    GasProjectionCost StepGasProjection(GasBox& box, GasProjection& state, const GasProjectionOptions& options);

    // 圧力の式を釣り合う家族の部品の系にする(T-0853。節 = セル・面 = 箱の中の面・開いた境界 = 外の x = 0 を固定した面・
    // 右辺の項 = E_i と −Σ F*〔値 × 係数 ÷ D〕)。options.equilibrium の道と試験・GPU の比べ(T-0915)が使う
    [[nodiscard]] EquilibriumSystem MakeGasEquilibriumSystem(const GasBox& box, const GasPressureSystem& system);

    // セルごとの止める閾値(T-0531。GPU の止める判定): |残差| がこれ以上のセルが 1 つでもあれば、式の誤差 floor(|残差| × D_i ÷ 2^48)が
    // 許容差を超える(⌈(許容差 + 1)× 2^48 ÷ D_i⌉。CPU の判定と整数で同じ)
    // 内側の解きの許容差の既定 = 外側の許容差 ÷ これ(ADR-0254。T-0531 の計測で ÷ 2・÷ 4・÷ 10 を比べ、外側が上限に届く刻みが 1 mg と同じ程度の ÷ 10)
    constexpr uint32_t INNER_TOLERANCE_DIVISOR = 10;

    // 内側の解きの許容差(mg。options.toleranceMilligrams が 0 なら外側の許容差から。1 以上)
    [[nodiscard]] uint32_t GasInnerToleranceMilligrams(const GasProjectionOptions& options);

    [[nodiscard]] std::vector<int64_t> GasPressureThresholds(const ImplicitGrid& grid, uint32_t toleranceMilligrams);

}  // namespace bicameral::sim
