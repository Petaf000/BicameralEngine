// equilibrium.h — 釣り合う家族の共通部品(docs/design/20-transport-substrate.md §9・§13.1。T-0853・ADR-0397)の CPU の回し方。
// 陰の圧力・支え・電位・磁場・熱の陰解法(広がる家族の陰)が、同じ形の直線の式をこの部品で解く。法則が渡すのは 3 つだけ:
//   節ごとの容量 C_i と右辺の項・面ごとの係数 g_f・閉じる長さ(mm)。多重格子の段は部品が節の木の座標から作る(BuildImplicitGrid の形)。
// 解き方: 決まった回数の V サイクル(早く止める口は持たない。20 §9.2)・前の答えから始める(温めた始まり。§10.4)・
//   出力は節の値と面の値 F = g (x_細かい側 − x_相手)(当てる段が両側に足すので、解けきらなくても保存は厳密。§9.1)。
// 閉じた領域(§9.3): 変わった節(右辺か対角が丸めの幅を超えて変わった節 + 法則が足す種)から物理の長さの内の節だけを未知数にし、
//   外は前の答えの値を固定した境(面は値を固定した面になる)。長さ 0 = 閉じない(全部が未知数)。
// データの流れ: 法則の状態 → EquilibriumSystem(節・面・固定した面・右辺の項)→ SolveEquilibrium
//   → 節の値(EquilibriumState::values。次の刻みの初め)・面の値(EquilibriumResult)→ 法則の当てる段
// 式は shaders/common/equilibrium.hlsli と implicit_conduction.hlsli(GPU と共通。GPU の回し方は T-0915)。浮動小数点は使わない。
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "common/fixed.hlsli"
#include "sim/implicit_conduction.h"

namespace bicameral::sim {

    struct MultiresNest;

    // 温めた始まりを置く欄(T-0922・ADR-0427。C-1 の温めた始まりの印 SF_FLAG_WARM_START の欄。sim/equilibrium_fields.h)。
    // nest が null でなければ、解く前に前の答えを欄の値 component〜(値の数ぶん)から読み、解いた後に書く(法則の控え state.values は写し)。
    // 欄に置くと、ハッシュと保存点に入るか(D-608 (A)・SF_WARM_START_IN_STATE)・眠る時の平均が印から決まる。null は前のまま(控えだけ)
    struct EquilibriumWarmStart {
        MultiresNest* nest = nullptr;
        uint32_t field = UINT32_MAX;
        uint32_t component = 0;  // 欄の中の最初の値(成分ごとに解く法則が 1 つの欄を分けて使う)
    };

    // 節(法則の未知数 1 つ。木のセル 1 つ。物の箱でも同じ)
    struct EquilibriumNode {
        int32_t level = 0;  // 木のレベル(0 = 0.5 m・細かいほど大きい。0 以上)
        int64_t x = 0;      // そのレベルのセルの単位の座標(多重格子の段を作る・閉じる距離に使う。負でもよい)
        int64_t y = 0;
        int64_t z = 0;
        uint64_t capacity = 0;  // 自分だけの係数 C_i(法則の単位。0 より大きい)
    };

    // 面(2 つの節のつながり)。細かい側 fine の単位の係数。粗い側の行には係数 ÷ 8^gap が入る(ADR-0017 と同じ)
    struct EquilibriumFace {
        uint32_t fine = 0;
        uint32_t coarse = 0;
        uint32_t gap = 0;  // レベルの差
        fx::FxU128 coefficient{};
    };

    // 値を固定した面(開いた世界の端・閉じた領域の外。外の値 value は未知数でない)。係数は節 node の単位
    struct EquilibriumFixedFace {
        uint32_t node = 0;
        fx::FxU128 coefficient{};
        int64_t value = 0;
    };

    // 右辺の項: value × factor(÷ D は部品が rounding の形で)。1 つの節に何本あってもよい(足す順は結果に効かない)
    struct EquilibriumSource {
        uint32_t node = 0;
        int64_t value = 0;
        fx::FxU128 factor{};
    };

    struct EquilibriumSystem {
        // 右辺の項と固定した面の項の割り方(equilibrium.hlsli の EQ_ROUNDING_EXACT〔値 × 係数 ÷ D〕/ EQ_ROUNDING_WEIGHT〔重み × 値〕)。法則ごとに 1 つ
        uint32_t rounding = 0;

        std::vector<EquilibriumNode> nodes;
        std::vector<EquilibriumFace> faces;
        std::vector<EquilibriumFixedFace> fixedFaces;
        std::vector<EquilibriumSource> sources;
    };

    struct EquilibriumOptions {
        // 多重格子(V サイクルの回数は法則ごとの定数。method は Multigrid・tolerance は見ない = 判定で早く止めない)
        ImplicitOptions solver = {.method = ImplicitMethod::Multigrid, .cycles = 2};
        bool galerkinCoarse = false;  // 粗い段の面の係数を子の和のまま(既定は ÷ 2。implicit_conduction と同じ)
        ImplicitGridLimits limits;    // 段の節・隣の上限(GPU のバッファ。0 = なし)

        // --- 閉じた領域(20 §9.3)---
        uint64_t closeLengthMm = 0;            // 物理の長さ(mm)。0 = 閉じない
        uint64_t seedTolerance = 0;            // 右辺(値の単位)がこれより大きく変わった節を種に(対角は少しでも変われば)
        std::span<const uint32_t> extraSeeds;  // 法則が足す種(節の番号)
        // 前の解きの残差(式の単位)がこれを超えた節も種に(0 = 見ない)。決まった回数で解けきらなかった所を、右辺が止まっても
        // 次の刻みで解き続ける(起こす。仮 = T-0853。20 §9.3 の「変わった所」に足した)
        uint64_t residualSeed = 0;

        bool meterResiduals = false;  // 節ごとの残差(式の単位)を結果に入れる(残差の計器。法則がブロックにまとめる)
        // 面の値を出す(法則が面の値を自分の当てる段で作る間は切ってよい。陰の圧力は FinishGasPressure が同じ K (x_左 − x_右) を作る)
        bool faceValues = true;

        // 温めた始まりを欄に置く(T-0922。既定は置かない)。閉じた領域(closeLengthMm・residualSeed)の種の控え
        // (EquilibriumState の sourceRhs・diagonals・residuals)は欄に入らない: 保存点から続けると最初の刻みは全部が種になる(T-0993)
        EquilibriumWarmStart warmStart;
    };

    // 温めた始まり(節の順。法則は刻みをまたいで節の順を保つ。数が変わったら全部が種・値 0 から)
    struct EquilibriumState {
        std::vector<int64_t> values;        // 前の答え(次の解きの初め・閉じた領域の外の値)
        std::vector<int64_t> sourceRhs;     // 前の右辺(右辺の項 ÷ D。変わった節を見つける)
        std::vector<fx::FxU128> diagonals;  // 前の対角 D
        std::vector<uint64_t> residuals;    // 前の解きの残差(式の単位。未知数でなかった節はその前の値のまま)
    };

    struct EquilibriumResult {
        std::vector<int64_t> faceValues;   // system.faces の順。F = g (x_fine − x_coarse)(fine の単位)
        std::vector<int64_t> fixedValues;  // system.fixedFaces の順。F = g (x_node − value)(外へ出るのが正)
        std::vector<uint64_t> residuals;   // meterResiduals の時: 節ごとの |残差 × D| ÷ 2^48(state.residuals の写し)

        uint32_t unknowns = 0;       // 未知数にした節の数
        uint32_t seeds = 0;          // 種の数(閉じた時)
        uint32_t gridLevels = 0;     // 多重格子の段の数
        uint64_t worstResidual = 0;  // 未知数の |残差 × D| ÷ 2^48 の最大(残差の計器)
        ImplicitCost cost;
    };

    // 解きを見る口(GPU の試験が同じ系を GPU で解き直す。T-0915)。node は grid の節の番号 → system の節の番号
    struct EquilibriumSolveView {
        const ImplicitGrid& grid;
        std::span<const uint32_t> nodes;
        std::span<const int64_t> rhs;
        std::span<const int64_t> before;
        std::span<const int64_t> after;
    };

    using EquilibriumObserver = std::function<void(const EquilibriumSolveView&)>;

    // 解く。state.values は全部の節の答えになる(外の節は前の値のまま)
    EquilibriumResult SolveEquilibrium(const EquilibriumSystem& system, const EquilibriumOptions& options,
                                       EquilibriumState& state, const EquilibriumObserver* observer = nullptr);

}  // namespace bicameral::sim
