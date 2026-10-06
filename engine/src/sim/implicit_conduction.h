// implicit_conduction.h — 細かいレベルの熱を陰解法で解く試作(方式②。T-0110 研究。D-432・ADR-0017・17 §4)の CPU リファレンス。
// 多重解像度の木の伝導(multires_conduction.hlsli)は陽解法で、面の係数が熱容量の上限(1/8)で頭打ちになるレベルより細かい所では
// 熱が本当より遅く伝わる(方式①の小刻みで基準 + 3 段まで直した。T-0108)。ここでは 1 刻みを後退 Euler の陰解法で進める:
//   C_i (T_i − T0_i) = Σ_f g_f (T_j − T_i)(T は刻みの終わりの温度。1 刻み = 1/60 秒)
// これを回数を固定した反復(赤黒・木の多重格子の V サイクル)で近似に解き(T*)、面の流れ F_f = g_f (T*_細かい側 − T*_粗い側) を
// ADR-0017 の形(面ごとに 1 つの値を両側で逆向き・レベル違いは細かい側が計算し粗い側は整数部 + 端数)で受け渡す。
// → 反復が収束しなくても、エネルギーはビット単位で保存される(近似が効くのは「どれだけ正しい速さか」だけ)。
// 比べる相手として RKL2(Meyer, Balsara, Aslam 2014 の super-time-stepping。段の数 s で安定な刻みが約 s² 倍の陽解法)も同じ形で持つ。
//
// データの流れ: セル(エネルギー)→ 温度 T0(mK × 2^16)→ 反復で T* → 面の流れ → セルのエネルギー(粗い側は端数にも)
// 試作の約束: セルの熱容量は一定(温度 = エネルギー × 2^32 ÷ 熱容量。相変化は無い)。木の本物のブロック・頁・活性には
// まだつながない(試作のセルの一覧を受ける)。浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <cstdint>
#include <vector>

#include "common/fixed.hlsli"

namespace bicameral::sim {

    // 試作のセル
    struct ImplicitCell {
        int32_t level = 0;  // 0 以上
        int64_t x = 0;      // そのレベルのセルの単位の座標(0 以上)
        int64_t y = 0;
        int64_t z = 0;

        // --- 物質(一定)---
        uint64_t
            heatCapacity = 0;  // C(2^-32 × そのレベルの単位 / mK。= 8 × MrThermal::capacityLimit。レベルによらない数)
        uint32_t conductance = 0;  // G(mW/K。レベル 0 の大きさのセルとして。MrThermal::conductance)

        // --- 状態 ---
        int64_t energy = 0;     // そのレベルの単位(8^-k mJ)
        uint64_t fraction = 0;  // 2^-64 単位の端数(違うレベルの面の粗い側が受ける。ADR-0017)
    };

    // 面。細かい側 fine が流れを計算する(同じレベルなら gap = 0 で、fine は番号の小さい側)
    struct ImplicitFace {
        uint32_t fine = 0;
        uint32_t coarse = 0;
        uint32_t gap = 0;  // レベルの差 d
        fx::FxU128
            coefficient{};  // 面の係数(2^-32 × 細かい側の単位 / mK)= min(G) × HC_LIMIT_PER_CONDUCTANCE × 4^kf(上限なし)
    };

    // 多重格子の 1 段(段 0 = セルそのもの。段 ℓ + 1 = 段 ℓ の最も細かいレベルの節を親のセルへ縮約したもの = 木の覆われた親のセル)
    struct ImplicitGridLevel {
        // --- 節 ---
        std::vector<int32_t> levels;
        std::vector<int64_t> x;
        std::vector<int64_t> y;
        std::vector<int64_t> z;
        std::vector<uint8_t> colors;  // 赤黒の色((x + y + z) の偶奇)

        // --- 式(自分の単位。重みは Q48)。T_i = 自分の重み × 右辺 + Σ 隣の重み × T_j ---
        std::vector<fx::FxU128> capacities;  // C
        std::vector<fx::FxU128> diagonals;   // D = C + Σ 面の係数
        std::vector<int64_t> selfWeights;    // C ÷ D
        std::vector<uint32_t> rowStarts;     // 隣の一覧の始まり(節の数 + 1)
        std::vector<uint32_t> neighbors;
        std::vector<int64_t> weights;          // 面の係数 ÷ D
        std::vector<fx::FxU128> coefficients;  // 面の係数(自分の単位。次の段を作る・RKL2 の段の数に使う)

        // --- 次の段への縮約 ---
        std::vector<uint32_t> parents;
        std::vector<int64_t> restrictWeights;  // 自分の D(親の単位)÷ 親の D
    };

    enum class ImplicitMethod : uint8_t {
        RedBlack,   // 赤黒の掃き出し(回数を固定)
        Multigrid,  // 木の多重格子の V サイクル(回数を固定)
        Rkl2,       // super-time-stepping(陽解法。段の数は安定の条件から決める)
    };

    struct ImplicitOptions {
        ImplicitMethod method = ImplicitMethod::Multigrid;
        uint32_t sweeps = 8;  // RedBlack: 掃き出しの回数
        uint32_t cycles = 2;  // Multigrid: V サイクルの回数
        uint32_t toleranceMillikelvin =
            0;                        // Multigrid: 0 でなければ、新しい温度の誤差(残差 × D/C)の最大がこれ以下になったら
                                      // cycles 回より前に止める(cycles は上限。判定は整数なので決定的)
        uint32_t preSmooth = 2;       // Multigrid: 下る前の掃き出し
        uint32_t postSmooth = 2;      // Multigrid: 上った後の掃き出し
        uint32_t coarsestSweeps = 4;  // Multigrid: 最も粗い段の掃き出し
        int64_t correctionScale =
            256;  // Multigrid: 親の直しを子へ足す倍率(Q8。区分的に一定の補間は直しが足りないので 1 より大きくする)
        uint32_t limitSlackMillikelvin = 1;  // 安全網: 刻みの初めの温度の範囲の外へ、これだけは出てよい
    };

    // 1 刻みの費用
    struct ImplicitCost {
        uint64_t cellUpdates = 0;            // 節の値を 1 回計算した数(全部の段の合計)
        uint64_t passes = 0;                 // 直列の段の数(GPU なら Dispatch の数に当たる)
        uint32_t stages = 0;                 // RKL2 の段の数
        uint32_t cycles = 0;                 // Multigrid: 回した V サイクルの数
        uint32_t limitedCells = 0;           // 安全網で面を陽解法の流れに戻したセルの数
        uint32_t limitRounds = 0;            // 安全網を繰り返した回数
        int64_t worstExcessMillikelvin = 0;  // 安全網の前に、刻みの初めの温度の範囲を超えた最大の量
    };

    struct ImplicitGrid {
        std::vector<ImplicitCell> cells;
        std::vector<ImplicitFace> faces;
        std::vector<ImplicitGridLevel> levels;  // 多重格子の段
    };

    // セルの一覧から面と多重格子の段を作る(セルは重ならないこと。面の隣が無い所は断熱)
    // galerkin: 親どうしの面の係数を子の合計のままにする(既定は ÷ 2 でそのレベルの離散化に合わせる。T-0110 の比べ)
    ImplicitGrid BuildImplicitGrid(std::vector<ImplicitCell> cells, bool galerkin = false);

    // 1 刻み進める
    ImplicitCost StepImplicit(ImplicitGrid& grid, const ImplicitOptions& options);

    // 温度(mK × 2^16。整数部のエネルギーだけから)と、温度からエネルギー
    int64_t ImplicitTemperature(const ImplicitCell& cell);
    int64_t ImplicitEnergyFor(uint64_t heatCapacity, int64_t millikelvin);

    // 保存量: 全部のセルのエネルギー + 端数(最も細かいレベルの単位 × 2^64。2 の補数で 2^128 を法とする。等しいかを比べる用)
    fx::FxU128 ImplicitConservedTotal(const ImplicitGrid& grid);

}  // namespace bicameral::sim
