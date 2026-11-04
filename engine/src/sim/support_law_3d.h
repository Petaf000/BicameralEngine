// support_law_3d.h — 支えの法則(docs/design/22-support-law.md §3.2。研究 R-B1)の 3 次元の格子の梁(T-0960)。
// 2 次元(sim/support_law。縦の面・奥行 1 セル・節 = 変位 2 + 回り 1)を 3 次元にしたもの: 節 = セルの中心が変位 3(x・上・奥行)+ 回り 3
// (各軸の回りの角 × セル ÷ 2。長さの単位)を持ち、面ごとに梁のばね(伸び・ずれ 2・ねじれ・曲げ 2)。段の斜めの棒は辺で接する斜めの隣
// (3 つの座標の面の段。軸の伸びと面の外の曲げ)と角で接する斜めの隣(3 歩の階段。軸の伸び)に置く(T-1293)。釣り合う家族の共通部品の成分を持つ節(equilibrium_block。k = 6)で 1 つの系として解く。CPU・浮動小数点なし(04 §4)。
// 範囲(T-0960 の 1 本目): 1 つの細かさのセル・割れていない面の残りの硬さ(Q16)・荷。違う大きさのセル(2 × 2 × 2 の対・形 A)は T-1294、
//   面の力・割れ・温めた始まりの運びは T-1295(どれも docs/tickets)。
// 単位は 2 次元と同じ: 変位 2^-16 nm(int64)・力 mN・係数 2^-32 mN/nm・右辺 2^-16 mN。帳簿の回りの力は mN × セル ÷ 2(位置はセルの半分の単位)。
// データの流れ: SupportScene3D → 項(面・段の棒・固定した面)→ EquilibriumBlockSystem → SolveEquilibriumBlocks → 変位と回り → 帳簿
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "sim/equilibrium_block.h"
#include "sim/support_law.h"

namespace bicameral::sim {

    inline constexpr uint32_t SUPPORT3_AXES = 3;   // 軸: 0 = x・1 = 上・2 = 奥行(x × 上 = 奥行の右手系)
    inline constexpr uint32_t SUPPORT3_X = 0;      // 成分: x の変位
    inline constexpr uint32_t SUPPORT3_UP = 1;     // 上の変位
    inline constexpr uint32_t SUPPORT3_DEPTH = 2;  // 奥行の変位
    inline constexpr uint32_t
        SUPPORT3_TURN = 3;  // 回りの最初(軸 a の回り = SUPPORT3_TURN + a。2 次元の φ は奥行の回り)
    inline constexpr uint32_t SUPPORT3_COMPONENTS = 6;  // 変位 3 + 回り 3

    // セルの固定した面(軸 a の −側 = bit 2a・+側 = bit 2a + 1)
    inline constexpr uint8_t Support3Clamp(uint32_t axis, int32_t side) {
        return (uint8_t)(1u << (2 * axis + (side > 0 ? 1 : 0)));
    }

    // ねじれの硬さ(面の b の分数。½ c (Δφ_n)²・c = 12 β b〔ν = 0 で G = E/2・J = β dx⁴〕)。7/4 b = β 7/48(T-1293): 正方形の断面の棒の
    // ねじれの硬さが Saint-Venant(0.1406 a⁴)の 1 × 1 で +3.7 %・2 × 2 で −4.3 %・3 × 3 で −1.7 %・4 × 4 で −0.7 %(1 × 1 だけに合わせる 27/16 b は 2 × 2 で −5.5 %)
    inline constexpr uint64_t SUPPORT3_TORSION_NUMERATOR = 7;
    inline constexpr uint64_t SUPPORT3_TORSION_DENOMINATOR = 4;

    struct SupportScene3D {
        uint32_t width = 0;   // x のセルの数
        uint32_t height = 0;  // 上のセルの数
        uint32_t depth = 0;   // 奥行のセルの数

        std::vector<uint8_t> solid;   // [x + width × (up + height × z)]
        std::vector<uint8_t> clamps;  // 同じ並び。Support3Clamp の組

        // --- 物質(1 つ)---
        uint64_t cellMm = 500;
        uint64_t stiffnessKpa = 0;    // x と奥行の向きの面・固定した面の硬さ
        uint64_t stiffnessUpKpa = 0;  // 上の向きの面と固定した面(0 = stiffnessKpa)
        uint64_t densityKgPerM3 = 0;
        uint64_t gravityUmPerS2 = 9806650;

        // 面の残りの硬さ(Q16。[軸][セル] のセルと +軸の隣の間の面。空 = 割れていない)
        std::array<std::vector<uint32_t>, SUPPORT3_AXES> intact;
        std::vector<int64_t> loadMn;  // セルに足す下向きの力(mN)。空 = 0
    };

    // 帳簿(力は mN・回りの力は原点の回りの mN × セル ÷ 2)。面・棒の項は両側に同じ値の逆を当てるので
    // net == reaction + load が力 3 成分・回り 3 成分とも解の精度によらず成り立つ
    struct SupportLedger3D {
        std::array<int64_t, SUPPORT3_AXES> loadMn{};        // 重さと荷の和(上の成分は負)
        std::array<int64_t, SUPPORT3_AXES> reactionMn{};    // 固定した面が支える力の和
        std::array<int64_t, SUPPORT3_AXES> netMn{};         // セルごとの力の和
        std::array<int64_t, SUPPORT3_AXES> loadTurn{};      // 重さと荷の回りの力
        std::array<int64_t, SUPPORT3_AXES> reactionTurn{};  // 固定した面の回りの力
        std::array<int64_t, SUPPORT3_AXES> netTurn{};       // セルごとの力と回りの力の原点の回りの和
        uint64_t worstCellMn = 0;                           // セルに残る力の成分の絶対値の最大(釣り合いの誤差)
        uint64_t worstTurnMn = 0;                           // セルに残る回りの力(÷ セルの半分 = mN)の最大
    };

    class SupportLaw3D {
    public:
        // options のうち使うのは stairBraces・braceNumerator / braceDenominator・blocks・warmStart・conjugateDirection / conjugateResidual
        // (形は格子の梁・解き方は Coupled だけ。2 次元の口と同じ値を読むので、既定の切り替えは support_law.h の 1 か所)
        SupportLaw3D(const SupportScene3D& scene, const SupportOptions& options);

        void Step();

        // --- 読む ---
        int64_t Value(uint32_t x, uint32_t up, uint32_t z, uint32_t component) const;  // セルが無ければ 0
        SupportLedger3D Ledger() const;
        uint64_t Hash() const;
        uint32_t CellCount() const { return (uint32_t)m_cells.size(); }
        uint32_t BraceCount() const { return m_braceCount; }
        uint32_t CornerBraceCount() const { return m_cornerBraceCount; }
        uint64_t LastNodeUpdates() const { return m_lastNodeUpdates; }

        // 成分を持つ節の系と段と今の値(GPU 版 gpu_equilibrium_block が同じ系を解いて比べる)
        const EquilibriumBlockSystem& BlockSystem() const { return m_blockSystem; }
        const EquilibriumBlockGrid& BlockGrid() const { return m_blockGrid; }
        const std::vector<int64_t>& BlockValues() const { return m_blockState.values; }

    private:
        using Gradient = std::array<int64_t, SUPPORT3_COMPONENTS>;

        // エネルギーの項 ½ c (g_low · x_low + g_high · x_high)²(面・段の棒)
        struct Term {
            uint32_t low = 0;
            uint32_t high = 0;
            uint64_t coefficient = 0;
            Gradient gradientLow{};
            Gradient gradientHigh{};
        };

        // 固定した面の項 ½ c (g · x)²
        struct FixedTerm {
            uint32_t cell = 0;
            uint64_t coefficient = 0;
            Gradient gradient{};
        };

        void BuildCells();
        void BuildTerms();
        void AddBond(uint32_t low, uint32_t high, uint32_t axis, uint32_t intact);
        void AddClamp(uint32_t cell, uint32_t axis, int32_t side);
        void AddBraces(uint32_t cell, uint32_t x, uint32_t up, uint32_t z);
        void AddCornerBraces(uint32_t cell, const std::array<int64_t, SUPPORT3_AXES>& at);
        uint32_t IntactAt(const std::array<int64_t, SUPPORT3_AXES>& p, uint32_t axis) const;  // p と +軸の隣の間の面
        void BuildBlocks();
        uint32_t Index(int64_t x, int64_t up, int64_t z) const;  // 無い・外なら UINT32_MAX
        uint32_t Intact(uint32_t cell, uint32_t axis) const;
        uint64_t AxisUnit(uint32_t axis) const;
        int64_t CellValue(uint32_t cell, uint32_t component) const;
        std::array<int64_t, SUPPORT3_AXES> Position(uint32_t cell) const;  // 中心(セルの半分の単位)

        SupportScene3D m_scene;
        SupportOptions m_options;
        uint64_t m_bondSide = 0;  // b = ka ÷ 6(x と奥行の面)
        uint64_t m_bondUp = 0;    // 上の面
        int64_t m_weight = 0;     // セル 1 つの重さ(mN)

        std::vector<std::array<uint32_t, SUPPORT3_AXES>> m_cells;  // セルの (x, 上, 奥行)
        std::vector<uint32_t> m_index;                             // [x + width × (up + height × z)] → セル
        std::vector<int64_t> m_loads;                              // 重さ + 荷(mN。下向きが正)
        std::vector<Term> m_terms;
        std::vector<FixedTerm> m_fixedTerms;
        uint32_t m_braceCount = 0;        // 辺の斜めの棒
        uint32_t m_cornerBraceCount = 0;  // 角の斜めの棒

        EquilibriumBlockSystem m_blockSystem;
        EquilibriumBlockGrid m_blockGrid;
        EquilibriumBlockState m_blockState;
        uint64_t m_lastNodeUpdates = 0;
    };

}  // namespace bicameral::sim
