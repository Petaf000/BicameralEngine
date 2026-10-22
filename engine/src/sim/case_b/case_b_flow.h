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
//
// 引きずり(R2・T-0869。FlowOptions::drag。21 §2.3 B-a・§2.4): 粒(置き場 ④ の粒の置き場)も動かし、面ごとに
//   流体 ↔ 粒(K)・流体 ↔ 据わる固体の孔(K_x。permeability_um2)の引きずりを、陰の圧力と同じ解きの中で消去する(部分消去):
//     [m_f + K + K_x, −K; −K, m_s + K] [v_f; v_s] = [M_f + A_f J; M_s + A_s J](J = Δt Δy。A_f・A_s は流体と粒の面積)
//   粒を消去すると r = K ÷ b(b = m_s + K)・a' = m_f + K_x + K m_s ÷ b・c' = A_f + r A_s で
//     体積の流れ = Δt (c' (M_f + r M_s + c' J) ÷ a' + A_s (M_s + A_s J) ÷ b)
//   になり、R1 と同じ形の 2 項(A² ÷ m と M A ÷ m)の和で圧力の係数と予測の流れが組める。解いた後は
//   圧力の力積 → 孔の引きずり(帳簿 FixedPhase)→ 相どうしの受け渡し(MoveMomentum。和 0)で当てるので保存は構造で守る。
//   K は散らばった粒(Stokes + Newton: 18 μ ÷ d² + ¾ C₂ ρ_f |Δv| ÷ d。粒の体積あたり)と詰まった粒(Kozeny–Carman: 180 μ φ_s ÷ (d² φ_f))の大きい方、
//   K_x は Darcy(μ φ_f² ÷ k。全体の体積あたり)。粒の重さと浮力は重さの軸の面に陽で(浮力は流体の密度 × 粒の体積)。
//   粒が詰まった所(grainPackedQ16 ≥ 1)へ入る粒の面は止める(帳簿 Wall。押し合いは R3 の仕事・仮)。
//   粒・孔が無ければ R1 とビット単位で同じ(r = 0・K_x = 0)。気体 ↔ 液体の引きずり(しずく・泡・水面の風)は置き場 ① が要る(T-0963)。
//
// 置き場 ①(T-0963。箱の layout = PerPhase なら同じ段がこちらを通る。④ の計算は変えない):
//   気体と液体を別の面の運動量で持ち、面ごとに体積の大きい相を主の流体、もう一方を主でない流体として、粒と同じ形で消去する
//   (主でない流体・粒は主の流体とだけつながる星の形なので、消去を足し合わせられる。系の形は ④ と同じ)。
//   水面のセル(気体と液体があり、下が液体だけか閉じている)は相ごとの柱(気体が上・液体が下の層)で支え、
//   層の面の気体 ↔ 液体は境の剪断 K = ρ_g C_f |Δv| A Δt(C_f = surfaceShearPer10000。21 §2.4 の「水面のセルでは弱める」)。
//   混ざった面(しずく・泡)は、重さの軸で浮力(重さ − 押しのけた混ざりの重さ)を相どうしで受け渡し(和 0。柱は混ざりを支える)、
//   しずく・泡の抗力(粒と同じ式。大きさ drop_size_um・形の定数 dropShapeDragPermille)× 主でない流体の体積でつなぐ。
//   運びは相ごとに、風上のセルのその相の割合を移す(主は解いた体積の流れ・主でない流体は面の速さ × Δt ÷ Δx)。
//
// 粒の降伏(R3・T-0996。FlowOptions::grainYield。21 §2.5・B-b): 段 grainYield(陰の圧力の後・運ぶの前)。
//   支え: 粒は下が「詰まった粒(支えられている)・据わる固体・物・床」の時だけ支えられる。支えられた粒だけが、上の粒の
//         浮力を引いた重さを柱で持つ(有効応力 σ'。セルの中心の値 = 上の支えられた粒の重さ + 自分の半分)。舞う粒・落ちる粒は持たない(押し合わない)。
//   降伏(2 本目・既定 yieldPerFace): 横の面ごとに、押す力 D = (σ'_L f_L − σ'_R f_R) A Δt と上限 F = (tan φ W̄' + c_eff f̄) A Δt で
//         P' = P + D。|P'| ≤ F なら 0・超えたら F だけ減らす。向かう側が詰まった面は 0。表面の段では「段の高さ ÷ Δx > tan φ」で崩れる。
//   降伏(1 本目 yieldPerFace = false): 横の面を重さの軸に沿って積んだ「面の柱」(隣り合う 2 本のセルの柱の間)ごとに、
//         押す力 D = Σ (σ'_L − σ'_R) A Δt・摩擦の上限 F = Σ (tan φ × 面の粒の有効な重さ + c_eff A) Δt(21 §2.5 の τ_max の面の和)。
//         深さで和をとるので、崩れるのは「2 本の柱の高さの差 ÷ Δx > tan φ」の時(Δx によらない。崩れる角度は φ から出る)。
//         面の柱の運動量 P' = P + D が |P'| ≤ F なら 0(ちょうど止まる)・超えたら F だけ減らす(降伏の後の運動量は持ち越す)。
//         P' は、向かう側のセルに空きがある面(詰まっていない・据わる物が無い)だけに質量の割合で載せる(表面を流れる)。差は帳簿 Wall。
//   くっつく強さ c_eff = cohesion + suction × 4S(1−S)(S = 孔の満ち具合 = 液体 ÷ 流体の体積)。混ざった粒は体積の平均(S9)。
//   詰まり: 詰まった粒へ入る面は止める(R2 のまま。押し返しの形は接触で、共有の陰の圧力には粒そのものの体積で入る)。
//   面の開き具合(T-0937・ADR-0420): 箱の faceBlockedQ16(欄 face.blocked と同じ形)を、面の開き = min(セルの空き, 1 − 塞ぐ割合)で読む(FoFaceOpen)。
//   穴を通る口(S8): 箱の faceHoleSizeUm と面の開きを FoGrainFaceOpen に渡し、粒の開きが 0 の面は流さない(粒 ÷ 穴 > grain_pass_ratio_pct。既定 100 %)。

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

        // 引きずり(R2・T-0869): 粒を動かし、引きずりを陰の圧力と一緒に解く。false なら R1 と同じ(粒は塞ぐだけ)
        bool drag = false;
        // 粒の形の抗力の定数 C₂(‰。C_d ≈ 24/Re + C₂。自然の砂 1000・滑らかな球 400。Ferguson & Church 2004)。法則の定数(仮)
        uint32_t grainShapeDragPermille = 1000;

        // --- 置き場 ①(T-0963)---
        // しずく・泡の形の抗力の定数 C₂(‰)。2 mm の雨粒は滑らかな球に近い 400(仮。法則の定数)
        uint32_t dropShapeDragPermille = 400;
        // 水面のセルの境の剪断の係数 C_f × 10^4(30 = 0.003。21 §2.6 の面の剪断と同じ値。仮。法則の定数)
        uint32_t surfaceShearPer10000 = 30;
        // 水面の層の横の面を別の速さで解く(T-0963 の 2 本目)。true にすると水面の風は 1/120 で合格するが、
        // 連通管・閉じた空気・ダムが崩れ、1/60 の風洞が暴れた。既定 false = しずく・霧だけ分ける(R1 は ④ と同じ値。仮)
        bool splitSurfaceLayer = false;

        // --- 粒の降伏(R3・T-0996。drag が要る)---
        bool grainYield = false;
        // 降伏を面ごとに決める(2 本目・既定)。false = 面の柱(重さの軸に沿って続いた面)の和で決め、表面の面に載せる(1 本目。
        // 柱の深さ ÷ Δx に比例して表面が速くなり、山が平らになりすぎた。T-0996)
        bool yieldPerFace = true;
        // 崩れ始める角度と止まる角度の差(度。T-0997・ADR-0430。一旦の法則の定数)。書いた repose_angle_deg は山が止まる角度(D-620)で、
        // 降伏の上限(tan φ × 重さ)には「書いた値 + この差」を使う。崩れた粒は勢いで約 6° 行き過ぎるので(φ 25°・34°・40° で 5〜7°)、
        // 止まる角度が書いた値 ±1° になり、崩れ始める角度は書いた値より 4〜6° 急になる。0 = T-0996 の 2 本目(書いた値で崩れ始める)
        uint32_t reposeStartOffsetDegrees = 6;
        // 動いている粒と止まりかけの粒で降伏の上限を分ける(T-1004・ADR-0436。研究は打ち切り・既定 0 = 分けない = T-0997)。面の粒の速さ(刻みの初めの
        // 面の運動量 ÷ 面の粒の質量)がこの値以上なら「動いている」で上限は書いた値(止まる角度)、未満なら書いた値 + 差(崩れ始める角度)。
        // 状態(前の刻みから持ち越した面の運動量)だけで決まる(決定的)。法則の定数(仮)
        uint32_t grainMovingSpeedMmPerS = 0;
        // 動いているかをセルで決める(T-1004 の 2 本目)。true = セルの横の面のどれかの粒の速さが grainMovingSpeedMmPerS 以上なら、
        // そのセルの粒は動いている(粒の流れの中)で、両側のどちらかが動いているセルの面は書いた値の上限。false = 面ごと(1 本目)
        bool grainMovingByCell = false;
    };

    // 1 つのセルの状態式の結果(刻みの初めの中身から)
    struct FlowCell {
        bool closed = true;  // 空きが無い(据わる固体・物で埋まった)
        uint32_t openQ16 = 0;
        int64_t availableNl = 0;
        int64_t liquidNl = 0;  // 圧力 p での液体の体積
        int64_t gasNl = 0;
        int64_t voidNl = 0;   // 真空(液体だけで満たせない時)
        int64_t grainNl = 0;  // 粒そのものの体積(R2)
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

        // --- 引きずり(R2)---
        uint64_t dragFaces = 0;           // 相どうしの引きずりを圧力と一緒に解いた面
        uint64_t porousFaces = 0;         // 孔の引きずり(Darcy)を解いた面
        uint64_t cappedDragFaces = 0;     // 引きずりの係数を頭打ちにした面(21 §1.3 の計器)
        uint64_t grainLimitedCells = 0;   // 出る粒の体積を中の粒で縮めたセル
        uint64_t blockedGrainFaces = 0;   // 詰まった所へ入る粒の面を止めた数(R3 まで・仮)
        uint64_t vanishedGrainFaces = 0;  // 粒が消えた面の運動量を流体へ渡した数(相が消える所)

        // --- 置き場 ①(T-0963)---
        uint64_t fluidPairFaces = 0;     // 気体と液体を別の速さで解いた面
        uint64_t surfaceFaces = 0;       // そのうち層の面(境の剪断)
        uint64_t mergedFluidFaces = 0;   // 気体と液体があるが 1 つの速さで解いた面(主の流体が片側に無い)
        uint64_t stoppedFluidFaces = 0;  // 主でない流体が、その相の無いセルから出る向きなので止めた面

        // --- 粒の降伏(R3・T-0996)---
        uint64_t yieldMovingPairs = 0;         // 降伏して動いた面の柱(刻みの累計)
        uint64_t yieldDynamicFaces = 0;        // 動いている面として書いた値の上限を使った面(T-1004。刻みの累計)
        uint64_t yieldHeldPairs = 0;           // 摩擦で止めた面の柱(運動量か押す力があったもの)
        uint64_t yieldBlockedPairs = 0;        // 降伏したが向かう側に空きが無く止めた面の柱
        uint64_t holeBlockedFaces = 0;         // 穴より大きい粒で止めた面(S8 の口)
        uint64_t overpackedCells = 0;          // 詰まりを 1/16 より多く超えたセル(刻みの累計)
        uint32_t maxPackedQ16 = 0;             // 詰まりの最大(2^16 = ちょうど詰まった)
        int64_t lastMaxStressMicroPascal = 0;  // 直近の刻みの有効応力 σ' の最大(µPa)
    };

    struct FlowStages {
        Stages stages;
        std::shared_ptr<FlowStats> stats;
    };

    [[nodiscard]] FlowCell MeasureFlowCell(const Box& box, const View& view, uint32_t cell, const FlowOptions& options);

    // 置き場は ④(GrainSeparate)と ①(PerPhase。T-0963)。段は pressure・transport・apply を使う(drag なら粒も動かす)
    [[nodiscard]] FlowStages MakeFlowStages(const FlowOptions& options);

}  // namespace bicameral::sim::case_b
