// equilibrium_block.h — 釣り合う家族の共通部品(C-0。20 §9・§13.1・ADR-0397)の「成分を持つ節」(T-0919・ADR-0418)の CPU の回し方。
// 節が k 成分(2 次元の支えは 3 = 変位 2 + 回り。3 次元は 6 = 変位 3 + 回り 3)を持ち、面の係数が k × k のブロックになる系を、
// 決まった回数の V サイクルで解く。成分の間の結合(ずれ ⇄ 回り・段の斜めの棒の x ⇄ 上)を右辺へ遅らせずに 1 つの系で解く(T-0871 §3 の口)。
// 成分 1 の法則(陰の圧力・熱の陰解法)は今の SolveEquilibrium(equilibrium.h)のまま使う(k = 1 はその道。ビット一致)。
// 解き方:
//   - 平滑化: 節ごとに k × k の対角のブロックを LDLᵀ で解く Gauss–Seidel。色は (x, y, z) の奇偶の 8 色。同じレベルの斜めの節を結ぶ面や
//     粗い段の 27 点の隣も違う色になるので、色の中は順に依存しない(決定的。GPU では 8 回の Dispatch)
//   - 延ばし方: 法則が渡す「子の八分割ごとの k × k の行列」P(剛体の動き = 粗い節の変位 + 回り × 位置の差)。縮約はその転置 Pᵀ
//   - 粗い段: Galerkin(Pᵀ A P)。斜めの面も軸の面も、子の面の和として粗い段の面になる。段の木は節の木の座標から(親 = 座標 ÷ 2)
//   - 決まった回数・前の答えから始める(温めた始まり)。閉じた領域・違うレベルの節が混ざる系は持たない(T-0969)
// 単位: 値 x と右辺は法則の単位。係数 A は「A × x ÷ 2^coefficientShift = 右辺の単位」になる整数
// 式(LDLᵀ・ブロックの残差・延ばし方と縮約・固定した面の右辺)は shaders/common/equilibrium_block.hlsli で GPU 版(gpu_equilibrium_block。T-0970)と共有。
// データの流れ: 法則 → EquilibriumBlockSystem → BuildEquilibriumBlockGrid(段。系の形が変わった時だけ)→ SolveEquilibriumBlocks
//   → 節の値(state.values。節 × k)→ 法則が面の力を作る(当てる段・帳簿)。浮動小数点は使わない(04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "common/equilibrium_block.hlsli"
#include "sim/equilibrium.h"

namespace bicameral::sim {

    inline constexpr uint32_t
        EQ_MAX_COMPONENTS = equilibrium_block::EQB_MAX_COMPONENTS;  // 3 次元の支え(変位 3 + 回り 3)まで
    inline constexpr uint32_t EQ_PROLONG_SHIFT = equilibrium_block::EQB_PROLONG_SHIFT;      // 延ばし方の行列の Q
    inline constexpr uint32_t EQ_OCTANTS = equilibrium_block::EQB_OCTANTS;                  // 子の八分割
    inline constexpr uint32_t EQ_BLOCK_FACTOR_SHIFT = equilibrium_block::EQB_FACTOR_SHIFT;  // LDLᵀ の L の Q
    // 違うレベルの節が混ざる系(T-0969・ADR-0462)で、粗くしない節(その段でいちばん細かくない節)の「延ばし方」= 単位の行列の置き場
    inline constexpr uint32_t EQ_PASS_OCTANT = EQ_OCTANTS;

    // k × k の係数([行 × EQ_MAX_COMPONENTS + 列]。k を超える所は 0)と k 成分の値(GPU と共有する式の型)
    using EquilibriumBlock = equilibrium_block::EqbBlock;
    using EquilibriumVector = equilibrium_block::EqbVector;

    // 面(2 つの節のつながり)。エネルギー ½ [x_low; x_high]ᵀ [[lowLow, lowHigh], [lowHighᵀ, highHigh]] [x_low; x_high]。
    // 同じレベルの節どうし(軸の隣・斜めの隣のどちらでも。各軸の座標の差は 1 以内)か、レベルの差 1 の節どうし(T-0969。係数は法則が
    // エネルギーの形で渡す = 両側とも同じブロックの転置。スカラーの EquilibriumFace の「粗い側の行 ÷ 8^差」は体積あたりの式の形で、ここでは要らない)
    struct EquilibriumBlockFace {
        uint32_t low = 0;
        uint32_t high = 0;
        EquilibriumBlock lowLow{};
        EquilibriumBlock lowHigh{};  // low の行・high の列(high の行・low の列はこの転置)
        EquilibriumBlock highHigh{};
    };

    // 値を固定した面(地面に固定した面など)。エネルギー ½ (x − value)ᵀ block (x − value)
    struct EquilibriumBlockFixed {
        uint32_t node = 0;
        EquilibriumBlock block{};
        EquilibriumVector value{};
    };

    // 右辺の項(右辺の単位。重さなど)
    struct EquilibriumBlockSource {
        uint32_t node = 0;
        uint32_t component = 0;
        int64_t value = 0;
    };

    struct EquilibriumBlockSystem {
        uint32_t components = 1;        // k(1〜EQ_MAX_COMPONENTS)
        uint32_t coefficientShift = 0;  // A × x ÷ 2^この値 = 右辺の単位

        std::vector<EquilibriumNode>
            nodes;  // レベルと座標だけ使う(capacity は見ない。対角は面と固定した面から)。隣どうしのレベルの差は 1 まで(T-0969)
        std::vector<EquilibriumBlockFace> faces;
        std::vector<EquilibriumBlockFixed> fixedFaces;
        std::vector<EquilibriumBlockSource> sources;

        // 延ばし方: 子の値 = P[子の八分割] × 親の値 ÷ 2^EQ_PROLONG_SHIFT(法則が決める。剛体の動きを入れる)
        std::array<EquilibriumBlock, EQ_OCTANTS> prolongation{};
    };

    struct EquilibriumBlockOptions {
        uint32_t cycles = 2;          // V サイクルの回数(法則ごとの定数。判定で早く止めない)
        uint32_t preSmooth = 2;       // 下る前の掃き出し(8 色で 1 回)
        uint32_t postSmooth = 2;      // 上った後の掃き出し
        uint32_t coarsestSweeps = 8;  // 最も粗い段の掃き出し
        // V の直しを前処理とする共役勾配で進める(刻みの中だけ。V ごとに歩幅をエネルギー最小に)(T-1068・ADR-0455。研究中の比べ。既定 false = 今とビット一致。
        // GPU 版〔gpu_equilibrium_block〕にはまだ無い)
        bool lineSearch = false;
        // 共役勾配の前の向きを刻みをまたいで持つ(T-1086 の研究の比べ。既定 false)
        bool conjugateAcrossTicks = false;
        // 温めた始まりを欄に置く(T-0922。欄の値の数 ≥ component + k)。既定は置かない(state.values だけ)
        EquilibriumWarmStart warmStart;
    };

    // 節の対角のブロックの LDLᵀ(L の対角は 1。pivots ≤ 0 の成分は解かない = 支えの無い向き。lower は Q30)
    using EquilibriumBlockFactor = equilibrium_block::EqbFactor;

    // 多重格子の 1 段
    struct EquilibriumBlockLevel {
        // --- 節 ---
        std::vector<int64_t> x;
        std::vector<int64_t> y;
        std::vector<int64_t> z;
        std::vector<int32_t> depths;  // 木のレベル(段 0 は節のレベル。粗くした節は 1 小さい)
        std::vector<uint8_t> colors;  // 8 色(同じレベル)/ 16 色(違うレベルが混ざる段: 座標の奇偶 + 8 × レベルの奇偶)
        uint32_t colorCount = 8;

        // --- 式(係数の単位)---
        std::vector<EquilibriumBlock> diagonals;
        std::vector<EquilibriumBlockFactor> factors;
        std::vector<uint32_t> rowStarts;  // 隣の一覧の始まり(節の数 + 1)
        std::vector<uint32_t> neighbors;
        std::vector<EquilibriumBlock> blocks;  // 自分の行・隣の列

        // --- 次の段へ ---
        std::vector<uint32_t> parents;
        std::vector<uint8_t> octants;
    };

    struct EquilibriumBlockGrid {
        uint32_t components = 1;
        uint32_t coefficientShift = 0;
        // [0, 8) = 法則の延ばし方・[EQ_PASS_OCTANT] = 単位の行列(粗くしない節)
        std::array<EquilibriumBlock, EQ_OCTANTS + 1> prolongation{};
        std::array<EquilibriumBlock, EQ_OCTANTS + 1> restriction{};  // Pᵀ
        std::vector<EquilibriumBlockLevel> levels;
    };

    // 温めた始まり(節 × k の値。数が変わったら 0 から)
    struct EquilibriumBlockState {
        std::vector<int64_t> values;

        // --- 共役勾配の向きを刻みをまたいで持つ時だけ(conjugateAcrossTicks。T-1086 の研究。セーブ・ハッシュにはまだ入れていない)---
        std::vector<int64_t> conjugateDirection;
        std::vector<int64_t> conjugateResidual;
        uint64_t conjugateZrHigh = 0;
        uint64_t conjugateZrLow = 0;
        bool conjugateValid = false;
    };

    struct EquilibriumBlockResult {
        uint64_t worstResidual = 0;  // 解いた後の |右辺 − A x| の最大(右辺の単位。残差の計器)
        uint64_t nodeUpdates = 0;    // 節の値を 1 回解いた数(全部の段)
        uint32_t gridLevels = 0;
    };

    // 段を作る(係数・延ばし方が変わった時だけ。右辺の項は見ない)
    EquilibriumBlockGrid BuildEquilibriumBlockGrid(const EquilibriumBlockSystem& system);

    // 解く。state.values は節 × k の答えになる
    EquilibriumBlockResult SolveEquilibriumBlocks(const EquilibriumBlockSystem& system,
                                                  const EquilibriumBlockGrid& grid,
                                                  const EquilibriumBlockOptions& options, EquilibriumBlockState& state);

}  // namespace bicameral::sim
