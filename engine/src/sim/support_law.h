// support_law.h — 支えの法則(docs/design/22-support-law.md §3.2。研究 R-B1・T-0871)の試作: 固い物が硬さでたわんで釣り合う式を、
// 釣り合う家族の共通部品(sim/equilibrium。C-0・ADR-0397)の使い手として解く。CPU だけ・浮動小数点なし(04 §4)。
// 範囲(1 チャット目): 縦の面(x と上)の平面・奥行 1 セル・1 つの細かさのセル(セルの大きさ cellMm で 0.25 / 0.5 / 2 m を作る)・
//   横の縮み方 0(ν = 0。22 §3.2 ① の「この宇宙の法則として宣言」の仮)。3 次元(変位 3 + 回り 3)は T-0944。
// 2 つの形(どちらも節 = セルの中心):
//   - PerComponent: 変位の成分ごとに独立に解く(今の C-0 の口のまま。面ごとに伸び縮みとずれのばね。22 §3.2 ①)
//   - BeamLattice: 節が変位 2 成分と回り φ(= 角 × セルの大きさ ÷ 2。長さの単位)を持ち、面ごとに梁のばね(伸び・ずれ・曲げ)を置く。
//     解き方は 2 つ(SupportSolve):
//       - Coupled(既定。T-0919): 節が 3 成分を持つ C-0 の口(equilibrium_block)で 1 つの系として解く。面の係数は 3 × 3(ずれ ⇄ 回り・
//         段の斜めの棒の x ⇄ 上の非対角も面の中)。延ばし方は剛体の動き(粗い節の変位 + 回り × 位置の差)
//       - Lagged(T-0871。比べ用): 成分ごとに C-0(スカラー)で解き、成分の間の結合は前の値を右辺の項へ遅らせる
//         (刻みごとに x → 上 → 回り の順に 1 回ずつ。釣り合いに着くまで刻みが要る)
// 単位: 変位 2^-16 nm(int64)・力 mN・係数 2^-32 mN/nm(係数 × 変位 ÷ 2^48 = mN。ImFaceFlow と同じ桁)。
// データの流れ: SupportScene(セル・固定した面・硬さ・密度)→ 成分ごとの EquilibriumSystem(節・面・固定した面)と刻みごとの右辺の項
//   (重さ・遅らせた結合)→ SolveEquilibrium → 変位(次の刻みの温めた始まり)→ 帳簿(重さ・固定した面が支える力・セルに残る力)
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "sim/equilibrium.h"
#include "sim/equilibrium_block.h"

namespace bicameral::sim {

    // セルの固定した面(地面・壁に固定した面。その向こうは動かない)
    inline constexpr uint8_t SUPPORT_CLAMP_LEFT = 1;    // −x
    inline constexpr uint8_t SUPPORT_CLAMP_RIGHT = 2;   // +x
    inline constexpr uint8_t SUPPORT_CLAMP_BOTTOM = 4;  // 下

    inline constexpr uint32_t SUPPORT_X = 0;     // 成分: 横の変位
    inline constexpr uint32_t SUPPORT_UP = 1;    // 成分: 上の変位
    inline constexpr uint32_t SUPPORT_TURN = 2;  // 成分: 回り φ(BeamLattice だけ)
    inline constexpr uint32_t SUPPORT_COMPONENTS = 3;

    enum class SupportForm : uint8_t {
        PerComponent,  // 成分ごと(結合なし)
        BeamLattice,   // 格子の梁(回りを持つ。結合は遅らせる)
    };

    enum class SupportSolve : uint8_t {
        Coupled,  // 成分を持つ節で 1 つの系(T-0919。BeamLattice だけ。PerComponent は結合が無いので Lagged と同じ)
        Lagged,   // 成分ごと + 結合を遅らせる(T-0871)
    };

    struct SupportScene {
        uint32_t width = 0;   // x のセルの数
        uint32_t height = 0;  // 上のセルの数

        std::vector<uint8_t> solid;   // [x + width × up]
        std::vector<uint8_t> clamps;  // 同じ並び。SUPPORT_CLAMP_* の組

        // --- 物質(1 つ)---
        uint64_t cellMm = 500;      // セルの一辺
        uint64_t stiffnessKpa = 0;  // 硬さ(ヤング率)。x の向きの面と左右の固定した面
        uint64_t
            stiffnessUpKpa = 0;  // 上の向きの面と下の固定した面の硬さ(0 = stiffnessKpa。向きごとの硬さ Q143 の材料)
        uint64_t densityKgPerM3 = 0;  // 重さの元
        uint64_t gravityUmPerS2 = 9806650;
    };

    struct SupportOptions {
        SupportForm form = SupportForm::BeamLattice;
        EquilibriumOptions equilibrium;  // 成分 1 つの解き(V の回数は法則の定数)
        // 段の斜めの棒(T-0944 の 2 本目。BeamLattice だけ): 角だけで接する斜めの隣のうち、間の 2 セルの片方だけがある所(階段の段)に、
        // 2 つのセルの中心を結ぶ軸の棒(係数 ka/12。斜めの向きの伸び縮みだけ)を足す。斜めの階段(アーチ・傾いた柱)が回りでしか
        // つながらず柔らかすぎる分(内径 4 m のアーチで +14 %)を埋める。真っすぐな物(段が無い)では何も変わらない。
        // 既定 true は司令塔の仮(2026-10-21・ADR-0415。係数 ka/12 は合わせ込みの値なので T-0919 の後に測り直す)
        bool stairBraces = true;
        // 棒の硬さ = ka × braceNumerator ÷ braceDenominator(既定 1/12。T-0919 で測り直す値)
        uint64_t braceNumerator = 1;
        uint64_t braceDenominator = 12;

        // --- 解き方(T-0919)---
        SupportSolve solve = SupportSolve::Coupled;
        EquilibriumBlockOptions blocks;  // Coupled の V の回数など(法則の定数)

        // 変位と回り(温めた始まり)を C-1 の温めた始まりの欄に置く(T-0922。値の数 ≥ 成分の数。節 = セル (x, 上, 0)・レベル 0)。
        // Coupled は blocks.warmStart、Lagged は成分 c を値 component + c に。既定は置かない(法則の控えだけ)
        EquilibriumWarmStart warmStart;
    };

    // 帳簿(上向きの力。mN)
    struct SupportLedger {
        int64_t weightMn = 0;    // 重さの和(下向きを正で数える)
        int64_t reactionMn = 0;  // 固定した面が支える力の和(上向きが正)
        int64_t netMn = 0;  // セルごとの上向きの力(重さ・面の力・固定した面の力)の和。面の力は両側に同じ値で当たるので
                            // netMn == reactionMn − weightMn が解の精度によらず成り立つ(帳簿)
        uint64_t worstCellMn = 0;  // セルに残る上向きの力の絶対値の最大(釣り合いの誤差)
    };

    // 面の力(面 1 つ。セル (x, up) と +軸の隣の間。mN)。表面の引きの応力(割れの判定 R-B2 が読む)は側の力 ÷ 面積 dx²
    // (梁の N/A ± 6M/(A·dx)。M = 曲げの力の回り。1 セルの厚さで梁の式と一致・T-0944)
    struct SupportFaceForces {
        int64_t axialMn = 0;     // 軸の力 N(引きが正)
        int64_t shearMn = 0;     // ずれの力(横の向きの正へ +軸の側のセルを押し返す向き)
        int64_t lowSideMn = 0;   // −横の側の縁の力(x の面 = 下の縁・上の面 = −x の縁)。N ± 6M/dx
        int64_t highSideMn = 0;  // +横の側の縁の力(x の面 = 上の縁・上の面 = +x の縁)
    };

    // 世界のデータの V の回数(環境の balance_cycles.support。T-0955)を 1 つの系(blocks)と成分ごとの解き(equilibrium)の両方へ
    inline void ApplySupportCycles(uint32_t cycles, SupportOptions& options) {
        options.blocks.cycles = cycles;
        options.equilibrium.solver.cycles = cycles;
    }

    class SupportLaw {
    public:
        SupportLaw(const SupportScene& scene, const SupportOptions& options);

        // 掘った後など: before と同じ座標のセルの値を温めた始まりにする(無いセルは 0)。温めた始まりを欄に置く時(warmStart)は
        // 欄が答えの元なので使わない(掘った所の欄は木の操作が持つ。T-0922)
        void CarryFrom(const SupportLaw& before);

        // 1 刻み(成分ごとに 1 回ずつ解く)
        void Step();

        // --- 読む ---
        // セル (x, up) の成分 component の値(2^-16 nm)。セルが無ければ 0
        int64_t Value(uint32_t x, uint32_t up, uint32_t component) const;
        SupportLedger Ledger() const;
        // セル (x, up) と +軸(0 = x・1 = 上)の隣の間の面の力。面が無い(どちらかのセルが無い)・成分ごとの形なら全部 0
        SupportFaceForces FaceForces(uint32_t x, uint32_t up, uint32_t axis) const;
        uint64_t Hash() const;
        // 前の Step で節の値を解いた回数(全部の段。Lagged は成分ごとの和。費用の比べ)
        uint64_t LastNodeUpdates() const { return m_lastNodeUpdates; }
        uint32_t CellCount() const { return (uint32_t)m_cells.size(); }

        // 成分を持つ節の系と段と今の値(Coupled の時。GPU 版〔gpu_equilibrium_block。T-0970〕が同じ系を解いて比べる)
        const EquilibriumBlockSystem& BlockSystem() const { return m_blockSystem; }
        const EquilibriumBlockGrid& BlockGrid() const { return m_blockGrid; }
        const std::vector<int64_t>& BlockValues() const { return m_blockState.values; }

    private:
        struct Bond {
            uint32_t low = 0;   // −軸の側のセル
            uint32_t high = 0;  // +軸の側のセル
            uint32_t axis = 0;  // 0 = x・1 = 上
        };

        // 段の斜めの棒: low = 下のセル・high = 斜め上のセル・across = 横の向き(+1 = +x へ・−1 = −x へ)
        struct Brace {
            uint32_t low = 0;
            uint32_t high = 0;
            int32_t across = 0;
        };

        struct Clamp {
            uint32_t cell = 0;
            uint32_t axis = 0;
            int32_t side = 0;  // −1 = −軸の面・+1 = +軸の面
        };

        // 遅らせた結合: 行の成分の式に K × (列の成分の値) が入る
        struct Lag {
            uint32_t row = 0;
            uint32_t column = 0;
            uint32_t columnComponent = 0;
            bool negative = false;   // K < 0
            uint64_t magnitude = 0;  // |K|(2^-32 mN/nm)
        };

        void Build();
        void BuildBlocks();
        void StepCoupled();
        bool Coupled() const;
        void AddBond(const Bond& bond);
        void AddClamp(const Clamp& clamp);
        void AddBrace(const Brace& brace);
        int64_t UpForceOnBrace(const Brace& brace) const;  // high のセルへ(low へは同じ値の逆)
        void AddLag(uint32_t rowComponent, uint32_t row, uint32_t columnComponent, uint32_t column, int64_t sign,
                    uint64_t magnitude);
        int64_t UpForceOnClamp(const Clamp& clamp) const;
        int64_t UpForceOnBond(const Bond& bond) const;  // high のセルへ low のセルから(low へは同じ値の逆)
        uint32_t ComponentCount() const;

        SupportScene m_scene;
        SupportOptions m_options;
        uint64_t m_bondX = 0;   // b = ka ÷ 6(ka = E × セル。2^-32 mN/nm)。x の面
        uint64_t m_bondUp = 0;  // 上の面
        uint64_t m_brace = 0;   // 段の斜めの棒の式の係数(棒の硬さの半分。既定 ka/24。横と上の硬さの平均)
        int64_t m_weight = 0;   // セル 1 つの重さ(mN)

        std::vector<std::array<uint32_t, 2>> m_cells;  // セルの (x, up)
        std::vector<uint32_t> m_index;                 // [x + width × up] → セル(無ければ UINT32_MAX)
        std::vector<Bond> m_bonds;
        std::vector<Clamp> m_clamps;
        std::vector<Brace> m_braces;

        std::array<EquilibriumSystem, SUPPORT_COMPONENTS> m_systems;
        std::array<std::vector<Lag>, SUPPORT_COMPONENTS> m_lags;
        std::array<EquilibriumState, SUPPORT_COMPONENTS> m_states;

        // --- 成分を持つ節(Coupled)---
        EquilibriumBlockSystem m_blockSystem;
        EquilibriumBlockGrid m_blockGrid;
        EquilibriumBlockState m_blockState;
        uint64_t m_lastNodeUpdates = 0;
    };

}  // namespace bicameral::sim
