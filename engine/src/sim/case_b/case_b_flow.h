// case_b_flow.h — 案 B の試作の研究 R1(T-0868。docs/design/21 §5.2): 気体と液体を「1 つの陰の圧力」で動かす段(Stages)。
// 液体は硬い(縮みにくい。本物の水は音の速さ 1483 m/s)。気体は理想気体(温度はセルのエネルギーから。押し出す仕事はまだ数えない)。
// 圧力はセルに 1 つで、気体と液体が共有する。粒・据わる固体・物は体積を塞ぐだけ(動かすのは R3・B2)。
//
// 式(セル i・面 f。体積 nL・圧力 µPa・質量 mg・運動量 mg·2^-20 m/s・A は面の開いた面積 mm²・1 刻み Δt):
//   状態式 p(i): 液体の体積 V_l(p) = V_l0 − S (p − 大気)(S = Σ V_0 ÷ ρc²)と気体の体積 V_g(p) = G ÷ p(G = Σ nRT)の和が
//           空き(セル − 粒そのもの − 据わる固体 − 物)になる p。液体だけで満たせなければ真空(p = 0)。
//   柱 p_col(i): 上にある流体(気体 + 液体)の重さ + 自分の重さの半分(閉じたセルで切る)。重さの軸の面では
//           面の重さ (m_i + m_j)/2 × g と柱の差がちょうど打ち消すので、どちらも足さない(21 §2.3。静かな池は整数で力が 0)。横の面は柱の差の力積を足す。
//   陰の圧力: 残り y = p − p_col を刻みの終わりの値として解く。面の体積の流れ Q_f = M_f A ÷ (m_f Δt⁻¹)、圧力の力積 ΔM_f = A Δt (y_i − y_j) から
//           C_i y_i + Σ K_f (y_i − y_j) = C_i ỹ_i − Σ 出る Q*_f(C_i = S + V_g ÷ p + 真空 ÷ 大気・K_f = A² ÷ (m_f Δt⁻²))を口 PressureSolver で。
//   運ぶ: 解いた後の M の Q_f だけ、風上のセルの流体を移す(重さの軸の面は、下向きなら液体から・上向きなら気体から。横は体積の割合で)。
//           出る体積が中の流体を超えるセルは、その相の出る量を同じ割合で縮める(数える)。エネルギーは熱容量の割合で一緒に。
//   運動量: 面の運動量を、セルの中心を通る質量の流れ × 風上の面の速さで、同じ軸の隣の面へ運ぶ(横の向きの運びはまだ無い)。
// 運動量は流体(気体 + 液体)で 1 つ = 置き場 ④ の流体の置き場。① で気体と液体を分けると、引きずり(R2)が無い間は
// 水面の面の気体が液体の柱の坂を受けて約 ρ_水/ρ_空気 g で加速する(未測。T-0868 の仮)。だから R1 は ④ で測る。
//   粘り(T-0927・陰の圧力の系を組む前): 同じ軸の隣の面 F・G の間で ΔM = (μ A ÷ Δ + m ν_t ÷ Δ²) Δt (v_G − v_F)(μ は 2 つの面の小さい方・
//           ν_t = (C_s Δ)² |v_G − v_F| ÷ Δ は格子より小さい渦の粘り。受け渡しなので保存。速さの差を越えて渡さない)。
// 端数の持ち越し(21 §2.10)・押し出す仕事(Q1b)・衝撃の区は、まだ無い(T-0868 の残り)。浮動小数点は使わない。
#pragma once

#include <cstdint>
#include <memory>

#include "sim/case_b/case_b_box.h"

namespace bicameral::sim::case_b {

    inline constexpr uint32_t WATER_SOUND_SPEED_MM_PER_S = 1483000;  // 本物の水(R1 の 1 本目)

    struct FlowOptions {
        // 液体の硬さ ρc² の c(液体の段 (2) で下げる。この宇宙の水の硬さの宣言。どの細かさでも同じ値)
        uint32_t liquidSoundSpeedMmPerS = WATER_SOUND_SPEED_MM_PER_S;

        // 粘り(T-0927): 物の粘り(viscosity_upa_s)で隣の面と運動量を受け渡す
        bool materialViscosity = true;
        // 格子より小さい渦の粘り(Smagorinsky の C_s² × 10000。0 = 無し・289 = C_s 0.17)。T-0926 の仮説 2。物質の値ではなく乱れの法則の定数
        uint32_t eddyCoefficientPer10000 = 0;
    };

    // 1 つのセルの状態式の結果(刻みの初めの中身から)
    struct FlowCell {
        bool closed = true;  // 空きが無い(据わる固体・物で埋まった)
        uint32_t openQ16 = 0;
        int64_t availableNl = 0;
        int64_t liquidNl = 0;  // 圧力 p での液体の体積
        int64_t gasNl = 0;
        int64_t voidNl = 0;  // 真空(液体だけで満たせない時)
        int64_t fluidMassMg = 0;
        int64_t pressureMicroPascal = 0;  // 絶対の圧力(真空 = 0)
        uint64_t capacityQ48 = 0;         // 縮みやすさ C(nL/µPa の Q48)
    };

    // 段の数え(全部の刻みの累計と最大)
    struct FlowStats {
        uint64_t ticks = 0;
        uint32_t lastCycles = 0;
        uint32_t maxCycles = 0;
        int64_t lastErrorNl = 0;  // 陰の圧力の式の誤差(解いた後。口の値)
        int64_t maxErrorNl = 0;
        uint64_t limitedCells = 0;  // 出る体積を中の流体で縮めたセル(相ごとに数える)
        uint64_t voidCells = 0;     // 真空のあるセル
        uint64_t lightFaces = 0;    // 面の流体の質量を下限に上げた面(係数 K が上限を超えないように。ほぼ真空の面)
        uint64_t viscousPairs = 0;  // 粘りで運動量を受け渡した面の組(0 でない量)
    };

    struct FlowStages {
        Stages stages;
        std::shared_ptr<FlowStats> stats;
    };

    [[nodiscard]] FlowCell MeasureFlowCell(const Box& box, const View& view, uint32_t cell, const FlowOptions& options);

    // 置き場は ④(MomentumLayout::GrainSeparate)の箱だけ。段は pressure・transport・apply を使う
    [[nodiscard]] FlowStages MakeFlowStages(const FlowOptions& options);

}  // namespace bicameral::sim::case_b
