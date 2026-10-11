// gas_tree.h — 気体の流れを多重解像度の木(レベルの違うセルが面で接する)に広げた CPU リファレンス(T-0208。07 §2.5・ADR-0117)。
// 1 レベルの式(gas_reference。07 §2.1〜§2.4・ADR-0043・ADR-0083)をそのまま使い、レベルをまたぐ面の受け渡しだけを足す。
//
// レベル k のセル: 一辺 0.5 m × 2^-k、量の単位は 8^-k(物質量 µmol・エネルギー mJ・質量 mg・運動量 mg·2^-20 m/s。17 §1)。
//   同じ濃度・温度・速度ならどのレベルのセルも同じ数(細かくする = 子は親と同じ数)。圧力・温度・速度はレベルによらない。
// 面の係数はレベルの単位にする: 質量の流れ・圧力の拡散・圧力の力積は × 2^k(面積 ÷ 体積)、静水圧の差と重さの仕事は × 2^-k(セルの高さ)。
// レベルをまたぐ面(ADR-0017 を運動量にも): 細かい側だけが計算する(細かい単位・細かい面の面積)。粗い側の量は
//   密度のような値(流れを出す運動量・基準との質量の差)は数のまま、中身(移す物質量・エネルギー・運動量)は × 8^d で細かい単位に直して使う。
//   粗い側は細かい単位の増減を、整数部はセルへ・2^-64 単位の端数はセルの端数(GasTreeFraction)へ足す(3d ≤ 64 なら正確。ADR-0015 の端数と同じ形)。
//   → 細かい側が引いた量 = 粗い側が足した量で、成分・エネルギー・運動量はビット単位で保存される。導く値(温度・圧力)は整数部だけから。
// レベルごとの小刻み(T-0320。17 §4・ADR-0117 追記): レベル k は 1 刻みに substeps × 2^σ(k) 回(σ = clamp(k − 基準, 0, Δσmax)。
//   GasTreeOptions。既定は分けない = 全部のレベルが同じ小刻み = T-0208)。レベル k の小刻みは、いちばん細かい小刻みの通し番号が
//   2^(σmax − σ) の倍数の時に始まり、その終わりにだけセルの値が変わる(流れは小刻みの初めの値から)。違う小刻みの面は細かい側が細かい
//   小刻みごとに計算し、粗い側は力積も移した塊も粗い小刻みの終わりに合計を受ける(Berger–Colella の refluxing。時間補間はしない)。
//   粗い側から細かい面が 2^g 回引く時は、出ていく割合を × 2^g で数えて粗い小刻み 1 回の合計を 1 以下に抑える(熱の C/8 ÷ 引く回数と同じ考え)。
// いまの制限: 隣のレベルの差は 1 まで(2:1。d ≥ 2 は assert)・レベルをまたぐ面の成分は 1 次の風上(MUSCL は同じレベルの 3 つ並びだけ)・
//   どのレベルも安定の条件 ν × 2^(k − σ) ≤ 0.6 を満たすこと(MakeGasTree が assert)。
//   木は作った後に変えない(細かくする・粗くするは T-0210 で 05 のセルに載せる時)。
// 全部がレベル 0 なら gas_reference と毎刻みビット一致する(面の番号・乱数・式の形が同じ)。
//
// データの流れ: GasConfig(レベル 0 の箱の大きさ・境界・物質)+ 細かくする範囲 → MakeGasTree → StepGasTree → SumGasTree・DeriveGasTreeCell。
// 浮動小数点は使わない(04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "sim/gas_reference.h"

namespace bicameral::sim {

    // 細かくする範囲(レベル level の座標で。原点と大きさは偶数 = 1 つ粗いレベルのセルにそろえる)。
    // 範囲の下の 1 つ粗いレベルのセルは覆われて空になる(17 §1)。範囲は 1 つ粗いレベルのセル(か範囲)の中だけに置ける
    struct GasTreeRegion {
        uint32_t level = 1;
        std::array<uint32_t, GAS_AXES> origin = {};
        std::array<uint32_t, GAS_AXES> size = {};
    };

    // レベルごとの小刻み(T-0320)。σ(k) = clamp(k − subcycleBaseLevel, 0, maxSubcycleGap)。
    // 既定(maxSubcycleGap = 0)は分けない = 全部のレベルが substeps 回(T-0208 と毎刻みビット一致)。熱の sim::MultiresStepOptions と同じ名前
    struct GasTreeOptions {
        uint32_t subcycleBaseLevel = 0;
        uint32_t maxSubcycleGap = 0;
    };

    // whole + fraction × 2^-64(帳簿と合計。レベル 0 の単位)
    struct GasQ64 {
        int64_t whole = 0;
        uint64_t fraction = 0;

        bool operator==(const GasQ64&) const = default;
    };

    // セルの端数(2^-64 × そのセルのレベルの単位)。細かい側から受けた量の下位。値 = セルの整数 + これ × 2^-64
    struct GasTreeFraction {
        std::array<uint64_t, GAS_MAX_SPECIES> amounts = {};
        uint64_t energy = 0;
        std::array<uint64_t, GAS_AXES> momentum = {};
    };

    // 帳簿と合計(レベル 0 の単位。外から入った量を正)
    struct GasTreeTotals {
        std::array<GasQ64, GAS_MAX_SPECIES> amounts = {};
        GasQ64 energy;
        std::array<GasQ64, GAS_AXES> momentum = {};
    };

    // レベルごとの係数・基準(層 z ごと)・開いた境界の外・座標からセルを引く表
    struct GasTreeLevel {
        std::array<uint32_t, GAS_AXES> size = {};  // レベル 0 の箱の大きさ × 2^k
        uint32_t subcycleExponent = 0;             // σ: 1 刻みに substeps × 2^σ 回の小刻み(係数はその Δt)
        GasCoefficients coefficients;
        std::vector<GasCell> reference;
        std::vector<GasDerived> referenceDerived;
        std::vector<GasCell> ghost;
        std::vector<int32_t> grid;  // セルの番号・GAS_TREE_ABSENT(このレベルに無い)・GAS_TREE_COVERED(細かいセルが覆う)
    };

    inline constexpr int32_t GAS_TREE_ABSENT = -1;
    inline constexpr int32_t GAS_TREE_COVERED = -2;

    struct GasTreePlace {
        uint32_t level = 0;
        std::array<uint32_t, GAS_AXES> coordinate = {};
    };

    struct GasTree {
        GasConfig config;  // 大きさはレベル 0 の箱
        std::vector<GasTreeLevel> levels;
        uint32_t maxSubcycleExponent = 0;  // σ の最大(1 刻み = いちばん細かい小刻み substeps × 2^これ 回)

        // --- 状態(レベル 0 から順に、各レベルは x が最も速く回る順)---
        std::vector<GasCell> cells;
        std::vector<GasTreeFraction> fractions;
        std::vector<GasTreePlace> places;
        GasTreeTotals ledger;
        uint64_t tick = 0;

        [[nodiscard]] uint32_t GridIndex(uint32_t level, uint32_t x, uint32_t y, uint32_t z) const {
            const std::array<uint32_t, GAS_AXES>& size = levels[level].size;
            return x + size[0] * (y + size[1] * z);
        }

        // セルの番号(無い・覆われているなら負)
        [[nodiscard]] int32_t Find(uint32_t level, uint32_t x, uint32_t y, uint32_t z) const {
            return levels[level].grid[GridIndex(level, x, y, z)];
        }
    };

    // 木を作る: レベルごとに静水圧の基準の層を作り(レベル 0 は MakeHydrostaticReference と同じ。重さが無ければ全部の層が地面の層)、
    // 全部のセルを層の基準 + 風で埋める
    [[nodiscard]] GasTree MakeGasTree(const GasConfig& config, std::span<const GasTreeRegion> regions,
                                      std::span<const uint32_t> fractionsQ32, uint32_t temperatureMilliKelvin,
                                      uint64_t surfacePressureMicroPascal, const GasTreeOptions& options = {});

    // セルから導く値(整数部だけ。そのレベルの層の基準を使う)
    [[nodiscard]] GasDerived DeriveGasTreeCell(const GasTree& tree, uint32_t cellIndex);

    // 1 刻み(レベル k は substeps × 2^σ(k) 回の小刻み)
    void StepGasTree(GasTree& tree);

    // 合計(レベル 0 の単位。セルの整数 × 8^-k + 端数)
    [[nodiscard]] GasTreeTotals SumGasTree(const GasTree& tree);

    // --- 面の一覧(GPU 版 GpuGasTree が木を作った時に一度だけ写す。T-0209)---

    // 面の片側の種類(Mixed: MUSCL の外側の隣がレベルの違うセル = 勾配を使わない)
    enum class GasTreeSideKind : uint8_t {
        Cell,
        Ghost,
        Wall,
        Mixed,
    };

    struct GasTreeFaceSide {
        GasTreeSideKind kind = GasTreeSideKind::Cell;
        uint32_t index = 0;  // Cell: セルの番号 / Ghost: そのレベルの層 z
        uint32_t level = 0;
    };

    // StepGasTree が使う面と同じ一覧・同じ順(同じレベルの面はマイナス側のセルが 1 回、レベルをまたぐ面は細かい側のセルが両向き)
    struct GasTreeFaceLayout {
        GasTreeFaceSide left;
        GasTreeFaceSide right;
        GasTreeFaceSide leftOuter;
        GasTreeFaceSide rightOuter;
        uint32_t axis = 0;
        uint32_t level = 0;  // 面を計算する単位(細かい側のレベル)
        uint32_t id = 0;     // 面の番号(そのレベルの座標から。乱数の丸め)
    };

    [[nodiscard]] std::vector<GasTreeFaceLayout> ListGasTreeFaces(const GasTree& tree);

}  // namespace bicameral::sim
