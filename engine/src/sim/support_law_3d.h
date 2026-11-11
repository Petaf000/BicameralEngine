// support_law_3d.h — 支えの法則(docs/design/22-support-law.md §3.2。研究 R-B1)の 3 次元の格子の梁(T-0960)。
// 2 次元(sim/support_law。縦の面・奥行 1 セル・節 = 変位 2 + 回り 1)を 3 次元にしたもの: 節 = セルの中心が変位 3(x・上・奥行)+ 回り 3
// (各軸の回りの角 × セル ÷ 2。長さの単位)を持ち、面ごとに梁のばね(伸び・ずれ 2・ねじれ・曲げ 2)。段の斜めの棒は辺で接する斜めの隣
// (3 つの座標の面の段。軸の伸びと面の外の曲げ)と角で接する斜めの隣(3 歩の階段。軸の伸び)に置く(T-1293)。釣り合う家族の共通部品の成分を持つ節(equilibrium_block。k = 6)で 1 つの系として解く。CPU・浮動小数点なし(04 §4)。
// 範囲: 面の残りの硬さ(Q16)・荷・違う大きさのセル(T-1294: 印のセルを一辺 cellMm ÷ 2 の 2 × 2 × 2 に分ける。粗いセルの面に並ぶ
//   4 つの細かいセルを 1 つの組としてつなぐ = 対の平均の 3 次元〔ADR-0485・ADR-0501・ADR-0519〕。細かいセルはレベル 1。式は support_law_3d_mixed.cpp)。
//   面の力(FaceForces。割れの法則が読む縁・角の力とたわみのエネルギー)・掘った後の温めた始まりの運び(CarryFrom)は T-1295。
//   違う大きさの面の切り口の力は未着手(FineFaceForces が 0 を返す。T-1295 の作業ログ)。
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

        // --- 違う大きさのセル(T-1294)---
        // 同じ並び。1 = このセルを一辺 cellMm ÷ 2 の 2 × 2 × 2 の細かいセルに分ける。空 = 分けない(今までとビット一致)。
        // 荷は 8 つに分ける。固定した面は向きの側の細かいセルへ。分けたセルの中の面は割れていない・分けたセルの間と違う大きさの面は
        // 元のセルの面の値(intact)
        std::vector<uint8_t> fine;
    };

    // 帳簿(力は mN・回りの力は原点の回りの mN × セル ÷ 2。違う大きさのセルが混ざる場面は mN × セル ÷ 4)。面・棒の項は両側に同じ値の逆を当てるので
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

    // 面の力(面 1 つ。セルと +軸 n の隣の間。mN。T-1295)。2 次元の SupportFaceForces の 3 次元: 横の向き t_0 < t_1 は n 以外の 2 つの軸。
    // 縁の力 = N ± 6M/dx(t_i の縁を開く曲げ = もう 1 つの横の軸 t_j の回り。向きは ε(n, t_j, t_i))、角の力 = N + 2 つの曲げの分の和
    // (面の中の直線の分布の最大と最小は角にある)。表面の引きの応力(割れの判定が読む)は縁・角の力 ÷ 面積 dx²。
    // 奥行 1 セルの x と上の面は 2 次元の FaceForces と同じ(shearMn[0] = shearMn・edgeMn[0] = lowSideMn・highSideMn。support_3d_test)
    struct SupportFaceForces3D {
        uint32_t axis = 0;    // 面の法線 n
        int64_t axialMn = 0;  // 軸の力 N(引きが正)
        std::array<int64_t, 2>
            shearMn{};  // t_i のずれの力(ずれの項 Δu_t − [(φ_a + φ_b) × n]_t が正なら正。2 次元の shearMn と同じ向き)
        std::array<int64_t, 2> bendMn{};  // t_i の回りの曲げの力(曲げの回りの力 ÷ (dx ÷ 2))
        int64_t torsionMn = 0;            // ねじれの力(n の回りの回りの力 ÷ (dx ÷ 2))

        // --- 縁と角(割れの判定が読む)---
        std::array<std::array<int64_t, 2>, 2> edgeMn{};  // [i][s]: t_i の −側(s = 0)・+側(s = 1)の縁の力
        std::array<int64_t, 4> cornerMn{};               // [s_0 + 2 s_1]: t_0 の側 s_0・t_1 の側 s_1 の角の力

        int64_t energyPj = 0;  // 面の梁に蓄えたたわみのエネルギー(pJ。伸び・ずれ 2・曲げ 2・ねじれの和)
        uint32_t intact = 0;   // 面の残りの硬さ(Q16。力とエネルギーはこの硬さでの値)
    };

    class SupportLaw3D {
    public:
        // options のうち使うのは stairBraces・braceNumerator / braceDenominator・blocks・warmStart・conjugateDirection / conjugateResidual
        // (形は格子の梁・解き方は Coupled だけ。2 次元の口と同じ値を読むので、既定の切り替えは support_law.h の 1 か所)
        SupportLaw3D(const SupportScene3D& scene, const SupportOptions& options);

        // 掘った後など: before と同じ座標のセルの値を温めた始まりにする(無いセルは 0。分けたセルは 8 つの平均・分けたセルへは
        // 粗いセルの剛体の動きで配る)。刻みをまたぐ共役勾配の向きは捨てる(D-649。2 次元の CarryFrom と同じ。T-1295)
        void CarryFrom(const SupportLaw3D& before);

        void Step();

        // --- 読む ---
        int64_t Value(uint32_t x, uint32_t up, uint32_t z, uint32_t component) const;  // セルが無い・分けたセルなら 0
        // 細かい座標(一辺 cellMm ÷ 2 の格子)のセルの値: 分けたセルの中なら細かいセルの値、分けていないセルならそのセルの値
        // (回りはそのセルの単位)。無ければ 0
        int64_t FineValue(uint32_t fx, uint32_t fup, uint32_t fz, uint32_t component) const;
        // 細かい座標のセルが細かいセルか(分けたセルの中)
        bool IsFine(uint32_t fx, uint32_t fup, uint32_t fz) const;
        bool Mixed() const { return m_mixed; }
        // セル (x, 上, 奥行) と +軸の隣の間の面の力(同じ大きさのセルどうし)。面が無い・割れ切った・分けたセルが絡むなら全部 0
        SupportFaceForces3D FaceForces(uint32_t x, uint32_t up, uint32_t z, uint32_t axis) const;
        // 細かい座標のセルと +軸の隣の間の面の力: 両方が細かいセルなら細かい面(幅 cellMm ÷ 2)・両方が分けていない違うセルなら
        // 粗い面(FaceForces と同じ)・同じ分けていないセルの中・面が無い・違う大きさの面(未着手)なら全部 0
        SupportFaceForces3D FineFaceForces(uint32_t fx, uint32_t fup, uint32_t fz, uint32_t axis) const;
        uint32_t MixedFaceCount() const {
            return m_mixedFaceCount;
        }  // 違う大きさの面(粗いセルの面 1 つ = 細かいセル 4 つの組)
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

        // 節が 3〜5 つの項 ½ c (Σ g_i · x_i)²(違う大きさの面の 4 つの組。T-1294。nodes[0] は粗いセル)
        static constexpr uint32_t MULTI_NODES = 5;
        struct MultiTerm {
            std::array<uint32_t, MULTI_NODES> nodes{};
            std::array<Gradient, MULTI_NODES> gradients{};
            uint32_t count = 0;
            uint64_t coefficient = 0;
        };

        // 同じ大きさのセルどうしの面(AddBond が m_terms に並べる 6 つの項の最初。順は 伸び・ずれ 2〔横の軸の小さい順〕・回り 3〔軸の順〕)
        struct Bond {
            uint32_t firstTerm = 0;
            uint32_t axis = 0;
            uint32_t intact = 0;
        };

        void BuildCells();
        void BuildTerms();
        // unit = b(残りの硬さ intact を掛けた後)
        void AddBond(uint32_t low, uint32_t high, uint32_t axis, uint64_t unit, uint32_t intact);
        SupportFaceForces3D BondForces(const Bond& bond) const;
        int64_t TermProjection(const Term& term) const;
        int64_t CarriedValue(const SupportLaw3D& before, uint32_t cell, uint32_t component) const;
        void AddClamp(uint32_t cell, uint32_t axis, int32_t side, uint64_t unit);
        void AddBraces(uint32_t cell, uint32_t x, uint32_t up, uint32_t z);
        // 角の斜めの棒(fineGrid = 細かい格子で細かいセルどうし。T-1294)
        void AddCornerBraces(uint32_t cell, const std::array<int64_t, SUPPORT3_AXES>& at, bool fineGrid);
        uint32_t IntactAt(const std::array<int64_t, SUPPORT3_AXES>& p, uint32_t axis) const;  // p と +軸の隣の間の面
        // 格子の場所のセル(fineGrid: 細かい格子。分けていないセルは自分の 8 つの場所を持つ)。無い・外なら UINT32_MAX・
        // 粗い格子の分けたセルは SPLIT_CELL
        uint32_t Occupant(const std::array<int64_t, SUPPORT3_AXES>& p, bool fineGrid) const;
        uint32_t FaceIntactAt(const std::array<int64_t, SUPPORT3_AXES>& p, uint32_t axis, bool fineGrid) const;

        // --- 違う大きさのセル(support_law_3d_mixed.cpp。T-1294)---
        void BuildMixedCells();
        void BuildMixedTerms();
        void AddFineBonds(uint32_t cell);
        void AddMixedFace(uint32_t coarse, const std::array<uint32_t, SUPPORT3_AXES>& split, uint32_t axis,
                          int32_t side, uint32_t intact);
        // 形 A(MixedPairMaterial::NeighborStrain。T-1279 §2): 対 lower・upper(across の向きに並ぶ・もう 1 つの面の中の軸の位置 height)
        void AddStrainTerms(uint32_t coarse, uint32_t lower, uint32_t upper, uint32_t axis, int32_t side,
                            uint32_t across, int64_t height, uint64_t pairStiffness);
        void AddFineBraces();
        void AddFineBrace(const std::array<int64_t, SUPPORT3_AXES>& at, uint32_t a, uint32_t b, int32_t across);
        void AddMultiTerm(const MultiTerm& term);
        void BuildMultiBlocks();
        int64_t MultiProjection(const MultiTerm& term) const;
        void BuildBlocks();
        uint32_t Index(int64_t x, int64_t up, int64_t z) const;  // 無い・外なら UINT32_MAX
        uint32_t Intact(uint32_t cell, uint32_t axis) const;
        uint64_t AxisUnit(uint32_t axis) const;
        int64_t CellValue(uint32_t cell, uint32_t component) const;
        std::array<int64_t, SUPPORT3_AXES> Position(uint32_t cell) const;  // 中心(セルの半分の単位)

        SupportScene3D m_scene;
        SupportOptions m_options;
        uint64_t m_bondSide = 0;      // b = ka ÷ 6(x と奥行の面)
        uint64_t m_bondUp = 0;        // 上の面
        uint64_t m_fineBondSide = 0;  // 細かいセルどうしの b(一辺 cellMm ÷ 2。T-1294)
        uint64_t m_fineBondUp = 0;
        int64_t m_weight = 0;      // セル 1 つの重さ(mN)
        int64_t m_fineWeight = 0;  // 細かいセル 1 つの重さ
        bool m_mixed = false;      // 違う大きさのセルが混ざる(T-1294)

        std::vector<std::array<uint32_t, SUPPORT3_AXES>> m_cells;  // セルの (x, 上, 奥行)(そのセルの大きさの座標)
        std::vector<uint8_t> m_cellFine;    // セルが細かいか(混ざる場面だけ。空 = 全部同じ大きさ)
        std::vector<uint32_t> m_index;      // [x + width × (up + height × z)] → セル(分けたセルは SPLIT_CELL)
        std::vector<uint32_t> m_fineIndex;  // 細かい格子 → 細かいセル(混ざる場面だけ)
        std::vector<int64_t> m_loads;       // 重さ + 荷(mN。下向きが正)
        std::vector<Term> m_terms;
        std::vector<Bond> m_bonds;
        std::vector<uint32_t> m_bondAt;  // [セル × 3 + 軸] → 面(無い・割れ切ったなら UINT32_MAX)
        std::vector<FixedTerm> m_fixedTerms;
        std::vector<MultiTerm> m_multiTerms;
        uint32_t m_mixedFaceCount = 0;
        uint32_t m_braceCount = 0;        // 辺の斜めの棒
        uint32_t m_cornerBraceCount = 0;  // 角の斜めの棒

        EquilibriumBlockSystem m_blockSystem;
        EquilibriumBlockGrid m_blockGrid;
        EquilibriumBlockState m_blockState;
        uint64_t m_lastNodeUpdates = 0;
    };

}  // namespace bicameral::sim
