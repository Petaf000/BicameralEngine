// support_law.cpp — 支えの法則の試作(R-B1・T-0871。何をするかは support_law.h)。
// 式(セルの一辺 dx・奥行 dx・ν = 0・ka = E × dx・b = ka ÷ 6。係数は全部 b の整数倍なので丸めは b の 1 回だけ):
//   BeamLattice の面(セル a → 隣 b、軸の向き n・横の向き t)のエネルギー(Euler–Bernoulli の梁を半分ずつ。22 §3.2 ①'):
//     ½ ka (n_b − n_a)²  +  ½ ka (t_b − t_a ∓ (φ_a + φ_b))²  +  ½ (ka/3) (φ_b − φ_a)²
//     (∓ は x の面で −・上の面で +。φ = 角 × dx ÷ 2 は反時計回りが正)
//   固定した面(面の点の形。T-1206): ½ 2ka n²  +  ½ 2ka (t ± φ)²  +  ½ (2ka/3) φ²
//     3 つのばねを面(壁)の中心の点に置く(セルの剛体の動きで面の点へ: 横 t ± φ)。硬さは面と同じ規則(伸びとずれ E × 面積 ÷ 中心から壁 dx/2 = 2ka・
//     曲げ E I ÷ dx/2 = 2ka/3 φ²)。T-1206 の前は半分の長さの梁(ずれ 12 E I / (dx/2)³ = 8ka を中心から dx/4 の点 t ± φ/2 に)で、
//     ずれの点が面に無く、硬さが大きさで揃わなかった(違う大きさのセルの固定した面が並ぶ所で 14 %。ADR-0491)
//   PerComponent: 面ごとに伸び縮み ka とずれ ka/2 のばね(回りなし)。固定した面は 2ka と ka。
//   段の斜めの棒(stairBraces。T-0944): 下のセル a → 斜め上のセル c(横の向き s = ±1)に ½ (ka/24) (s Δx + Δup)²
//     (斜めの長さ √2 dx・軸の硬さ ka/12 の棒。Δ = c − a)。x と上の面に ka/24・x ⇄ 上の結合は遅らせる。
//     違う大きさのセルが絡む段(T-1133・ADR-0504): 細かい格子で段を探し、端はセルの中心・係数は ka/24 × 細かい中心の間 ÷ 中心の間
//     (片方の端が粗いセル 2/3・両端が粗いセル 1/2。一様な歪みで全部細かい場面の段の角の棒と同じ力)
// Lagged: 成分ごとの式の対角と負の非対角(C-0 が解く形)は面と固定した面に、成分の間の項と φ どうしの正の非対角は遅らせて右辺の項へ。
// Coupled(T-0919): 同じエネルギーの各項 ½ c (g · [x_a; x_b])² を 3 × 3 の面のブロック c g gᵀ にそのまま入れ(成分の間の項も面の中)、
//   成分を持つ節の C-0(equilibrium_block)で 1 つの系として解く。延ばし方は剛体の動き(RigidProlongation)。
// 違う大きさのセル(T-1120): 板の厚さ D = cellMm のままなので、同じ大きさのセルどうしの面は大きさによらず ka = E × D・b = ka ÷ 6
//   (梁の式の dx が消える)。粗いセル C(一辺 2s・φ_C = 角 × s)と細かいセル F(一辺 s・φ_F = 角 × s/2)の間は、長さ L = 1.5 s・
//   面の幅 s の Euler–Bernoulli の梁(MixedBeam::FaceOffset。粗い側の端は C の中心から横へ σ s/2 の点 = 剛体の動きで n_C + ε σ φ_C / 2):
//     ½ b (2 Δn')²  +  ½ (b/9) (4 Δt + ε (3 φ_C + 6 φ_F))²  +  ½ (b/3) (s 角_high − s 角_low)²(s 角_C = φ_C・s 角_F = 2 φ_F)
//   (ε = −1 は x の面・+1 は上の面。軸 E w D / L = 4b・ずれ 12 E I / L³ = 16b/9・曲げ E I / L = b/3。I = D s³ / 12。
//   粗い面に並ぶ 2 本の梁の断面の二次モーメントの和 2 (D s³/12) + 2 (s D)(s/2)² = D (2s)³ / 12 は粗い梁と同じ)
// 面の点のばね(MixedBeam::FaceSpring。T-1191・ADR-0485。既定): 3 つのばねを細かい側の面の中心の点に置く。ずれだけが ② と違う:
//     ½ 4b (Δt + ε (φ_C + φ_F))²(面の点の横の動き: 低い側 t − ε φ・高い側 t + ε φ。φ_C = 角 × s・φ_F = 角 × s/2 はどちらも中心から面までの腕 × 角)
//   ② の梁は長さ 1.5 s の一様な梁なので、ずれの力の回りが 0 になる点が梁の真ん中(粗いセルの中心から 0.75 s)にあり、面(s)に無い。
//   一様なずれでも面に偽の曲げ(ずれの力 × s/4)が出て、粗いセルの回りの釣り合いの腕が 0.75 s(本当は面までの s)になる(T-1191)。
//   硬さは同じ大きさの面と同じ規則(伸びとずれ E × 面積 ÷ 中心の間の長さ = E s D ÷ 1.5 s = 4b・曲げ E I ÷ 1.5 s = b s²/3。
//   同じ大きさの面は 6b・6b・2b Δφ² で、梁の式と一致する)
// 対のずれ(MixedBeam::FacePair。T-1191・ADR-0485。既定): 粗いセル C は剛体なので、③ の面の点は C の歪みを持たない。③ のままだと
//   (1) 塊が縦に押し縮められると、C の面に並ぶ 2 つの細かいセル L・U の縦の縮みを C が止める偽のずれ(細かい面の縦の力の 1/3)、
//   (2) 一様なずれの歪み γ で、L・U の伸びに逆向きの偽の差 ±γ s/4(ずれの力の 1/3)が出る(つぎはぎの検査に落ちる)。
//   直し(対の平均で C につなぐ): ずれは対の平均 d̄ = (d_L + d_U)/2 だけを C が受ける
//     ½ 4b d_L² + ½ 4b d_U² − ½ K (d_U − d_L)²(K = w_L w_U ÷ (w_L + w_U)・等しければ 2b → ½ 8b d̄²。負の項は L と U の面へ)
//   伸びは対の平均のずれの歪み(d̄ = 0.75 s γ)を引いて読む: 2Δn'_k → 2Δn'_k − 側 σ_k (d_L + d_U)/3(側 = 細かいセルが +軸の側なら +1)。
//     3 つの節(C・自分・相手)の項 ½ (b/9) (3 × 2Δn'_k − 側 σ_k (d_L + d_U))²(面のブロックへは C と自分の面・C と相手の面・L と U の面に分けて入れる)
//   曲げは ③ のまま(回りは L・U の伸びの対と細かい面の曲げで運ぶ = 全部細かい塊と同じ分け方)。系の全体は正(どの項も 2 乗・負の項は平均の 2 乗の分け方)。
//   対の中の面と両方の面の残りの硬さの小さい方 β の分だけ対として読み、1 − β は ③(割れて L と U が離れると面ごと)
// 対の曲げ(T-1207・ADR-0501。FacePair): 曲げの梁では回りが面に沿って変わる(上の面なら ∂角/∂x = 曲率)。L・U の回りは C の回りから
//   ±曲率 × s/2 ずれるが、剛体の C はそれを表せないので、面ごとの曲げ ½ (b/3) p_k²(p_k = 2φ_k − φ_C)が L・U に逆向きの偽の回りを出した
//   (全部細かい塊では面の両側のセルの回りは同じで 0。上の段だけ細かい片持ち梁の中立軸の上の面で 7〜10 %)。
//   直し(ずれと同じ芯 = 対の中の形の違いは C に掛けない): −½ K_曲げ (p_L − p_U)²(K_曲げ = 直列の硬さ。等しければ b/6 → 対で ½ (2b/3) p̄²)。
//   面の縁の力の曲げは、負の項の力を曲げの項と同じ両端の割合(C と F の勾配)で足して読む(面の点の回りの内挿は同じ形)
#include "sim/support_law.h"

#include <algorithm>

#include "common/equilibrium.hlsli"
#include "common/implicit_conduction.hlsli"
#include "core/aliases.h"
#include "sim/equilibrium_fields.h"

using namespace bicameral::fx;
using namespace bicameral::multires;

namespace bicameral::sim {

    namespace {

        constexpr uint64_t PER_MILLION = 1000000;
        constexpr uint64_t PER_TRILLION = 1000000000000;
        constexpr uint32_t UNIT_SHIFT = 32;   // 係数の Q(2^-32 mN/nm)
        constexpr uint32_t FORCE_SHIFT = 48;  // 力 mN → 右辺(2^-16 nm × 2^-32 mN/nm)
        constexpr uint64_t NO_CELL = UINT32_MAX;
        constexpr uint32_t SPLIT_CELL = UINT32_MAX - 1;  // m_index: 2 × 2 の細かいセルに分けたセル(T-1120)
        constexpr uint32_t MIXED_GROUP_TERMS = 3;        // 違う大きさの面 1 つの項(伸び・ずれ・曲げ)

        constexpr uint32_t
            BLOCK_COEFFICIENT_SHIFT = 32;           // Coupled の右辺の単位 2^-16 mN(係数 2^-32 × 変位 2^-16 ÷ 2^32)
        constexpr uint32_t BLOCK_FORCE_SHIFT = 16;  // mN → 2^-16 mN

        // 項 ½ c (g_low · x_low + g_high · x_high)² の勾配(成分ごとの -1〜2)
        using Gradient = std::array<int64_t, SUPPORT_COMPONENTS>;

        void AddTerm(EquilibriumBlockFace& face, uint64_t coefficient, const Gradient& low, const Gradient& high,
                     bool negative = false) {
            const int64_t c = negative ? -(int64_t)coefficient : (int64_t)coefficient;
            for (uint32_t row = 0; row < SUPPORT_COMPONENTS; ++row) {
                for (uint32_t column = 0; column < SUPPORT_COMPONENTS; ++column) {
                    const uint32_t at = row * EQ_MAX_COMPONENTS + column;
                    face.lowLow[at] += c * low[row] * low[column];
                    face.lowHigh[at] += c * low[row] * high[column];
                    face.highHigh[at] += c * high[row] * high[column];
                }
            }
        }

        void AddFixedTerm(EquilibriumBlockFixed& fixed, uint64_t coefficient, const Gradient& gradient) {
            const auto c = (int64_t)coefficient;
            for (uint32_t row = 0; row < SUPPORT_COMPONENTS; ++row) {
                for (uint32_t column = 0; column < SUPPORT_COMPONENTS; ++column)
                    fixed.block[row * EQ_MAX_COMPONENTS + column] += c * gradient[row] * gradient[column];
            }
        }

        // 剛体の動きの延ばし方(子の八分割ごと。Q16): 子の変位 = 親の変位 + 親の回り × (−位置の差の上, 位置の差の x)。
        // 位置の差は ±dx/2、親の φ = 角 × 2dx ÷ 2 = 角 × dx なので、回りの係数は ±1/2・子の φ = 親の φ ÷ 2
        std::array<EquilibriumBlock, EQ_OCTANTS> RigidProlongation() {
            constexpr int64_t PROLONG_ONE = int64_t{1} << EQ_PROLONG_SHIFT;
            constexpr int64_t PROLONG_HALF = PROLONG_ONE / 2;
            std::array<EquilibriumBlock, EQ_OCTANTS> prolongation{};
            for (uint32_t octant = 0; octant < EQ_OCTANTS; ++octant) {
                const int64_t acrossX = (octant & 1) != 0 ? PROLONG_HALF : -PROLONG_HALF;  // 位置の差の x ÷ dx
                const int64_t acrossUp = (octant & 2) != 0 ? PROLONG_HALF : -PROLONG_HALF;
                EquilibriumBlock& block = prolongation[octant];
                block[SUPPORT_X * EQ_MAX_COMPONENTS + SUPPORT_X] = PROLONG_ONE;
                block[SUPPORT_X * EQ_MAX_COMPONENTS + SUPPORT_TURN] = -acrossUp;
                block[SUPPORT_UP * EQ_MAX_COMPONENTS + SUPPORT_UP] = PROLONG_ONE;
                block[SUPPORT_UP * EQ_MAX_COMPONENTS + SUPPORT_TURN] = acrossX;
                block[SUPPORT_TURN * EQ_MAX_COMPONENTS + SUPPORT_TURN] = PROLONG_HALF;
            }

            return prolongation;
        }

        // ka = E × dx(kPa × mm = N/m = 10^-6 mN/nm)を 2^-32 mN/nm で
        uint64_t BondUnit(uint64_t stiffnessKpa, uint64_t cellMm) {
            const FxU128 product = FxMulU64Full(stiffnessKpa * cellMm, (uint64_t)1 << UNIT_SHIFT);

            return FxDivU128By64(product, PER_MILLION).quotient;
        }

        // セル 1 つの重さ ρ g dx³(kg/m³ × µm/s² × mm³ = 10^-12 mN)。細かいセルは (dx/2)² × dx(板の厚さは dx のまま。T-1120)
        int64_t CellWeight(const SupportScene& scene, bool fine) {
            const uint64_t side = fine ? scene.cellMm / 2 : scene.cellMm;
            const uint64_t volume = side * side * scene.cellMm;
            const FxU128 product = FxMulU64Full(scene.densityKgPerM3 * scene.gravityUmPerS2, volume);

            return (int64_t)FxDivU128By64(product, PER_TRILLION).quotient;
        }

        int64_t Flow(uint64_t coefficient, int64_t difference) {
            return ImFaceFlow(ImWide(coefficient), difference);
        }

        // 2 つのばねを直列にした硬さ a b ÷ (a + b)(切り捨て = 対の面の系が正のまま。T-1191)
        uint64_t SeriesCoefficient(uint64_t a, uint64_t b) {
            if (a == 0 || b == 0)
                return 0;

            return FxDivU128By64(FxMulU64Full(a, b), a + b).quotient;
        }

        // 係数 × 面の残りの硬さ(Q16)。割れていない面は掛けない(今までとビット一致)
        uint64_t Scaled(uint64_t coefficient, uint32_t intact) {
            if (intact >= SUPPORT_INTACT_FULL)
                return coefficient;

            return FxShiftRightU128(FxMulU64Full(coefficient, intact), 16);
        }

        // ばねの仕事 ½ F d(F mN・d 2^-16 nm → pJ)。F は d と同じ符号(係数が正)
        int64_t SpringWork(int64_t forceMn, int64_t difference) {
            return (int64_t)FxShiftRightU128(FxMulU64Full(FxAbsU64(forceMn), FxAbsU64(difference)), 17);
        }

    }  // namespace

    SupportLaw::SupportLaw(const SupportScene& scene, const SupportOptions& options)
        : m_scene(scene), m_options(options) {
        m_options.equilibrium.faceValues = false;
        m_options.blocks.warmStart = m_options.warmStart;
        m_options.blocks.conjugateDirection = m_options.conjugateDirection;
        m_options.blocks.conjugateResidual = m_options.conjugateResidual;
        Build();
        if (Coupled())
            BuildBlocks();
    }

    bool SupportLaw::Coupled() const {
        return m_options.solve == SupportSolve::Coupled && m_options.form == SupportForm::BeamLattice;
    }

    uint32_t SupportLaw::ComponentCount() const {
        return m_options.form == SupportForm::BeamLattice ? SUPPORT_COMPONENTS : SUPPORT_TURN;
    }

    // --- 組み立て: セル・面・固定した面 → 成分ごとの系(右辺の項は刻みごと)---
    void SupportLaw::Build() {
        const uint32_t width = m_scene.width;
        const uint32_t height = m_scene.height;
        m_bondX = BondUnit(m_scene.stiffnessKpa, m_scene.cellMm) / 6;
        const uint64_t upKpa = m_scene.stiffnessUpKpa != 0 ? m_scene.stiffnessUpKpa : m_scene.stiffnessKpa;
        m_bondUp = BondUnit(upKpa, m_scene.cellMm) / 6;
        // 式の係数 = 棒の硬さ ÷ 2 = 6b × 分子 ÷ (2 × 分母)(既定 1/12 で (b_x + b_上) / 8)
        m_brace = (m_bondX + m_bondUp) * 3 * m_options.braceNumerator / (2 * m_options.braceDenominator);
        m_weight = CellWeight(m_scene, false);
        m_mixed = rng::any_of(m_scene.fine, [](uint8_t fine) { return fine != 0; });
        FX_ASSERT(!m_mixed || Coupled());  // 混ざる場面は Coupled の格子の梁だけ(T-1120)

        if (m_mixed)
            BuildMixedCells();
        else
            BuildCells();

        BuildLoads();
        m_bondAt.assign(m_cells.size() * 2, (uint32_t)NO_CELL);

        for (uint32_t c = 0; c < ComponentCount(); ++c) {
            m_systems[c].rounding = EQ_ROUNDING_EXACT;
            for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
                const auto [x, up] = m_cells[cell];
                const int32_t level = m_mixed && m_cellFine[cell] != 0 ? 1 : 0;
                m_systems[c].nodes.push_back({.level = level, .x = x, .y = up, .z = 0, .capacity = 1});
            }
        }

        // 分けたセルの隣(SPLIT_CELL)の面は BuildMixedBonds が細かいセルから足す
        const auto present = [&](uint32_t x, uint32_t up) {
            return m_index[x + (size_t)width * up] < SPLIT_CELL;
        };

        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            if (m_mixed && m_cellFine[cell] != 0)
                continue;

            const auto [x, up] = m_cells[cell];
            if (x + 1 < width && present(x + 1, up))
                AddBond({.low = cell, .high = m_index[x + 1 + (size_t)width * up], .axis = 0}, Intact(x, up, 0));

            if (up + 1 < height && present(x, up + 1))
                AddBond({.low = cell, .high = m_index[x + (size_t)width * (up + 1)], .axis = 1}, Intact(x, up, 1));

            const uint8_t clamps = m_scene.clamps[x + (size_t)width * up];
            if ((clamps & SUPPORT_CLAMP_LEFT) != 0)
                AddClamp({.cell = cell, .axis = 0, .side = -1});

            if ((clamps & SUPPORT_CLAMP_RIGHT) != 0)
                AddClamp({.cell = cell, .axis = 0, .side = 1});

            if ((clamps & SUPPORT_CLAMP_BOTTOM) != 0)
                AddClamp({.cell = cell, .axis = 1, .side = -1});

            if (!m_options.stairBraces || m_options.form != SupportForm::BeamLattice || up + 1 >= height)
                continue;

            // 斜め上の隣があり、間の 2 セル(横の隣・上の隣)の片方だけがある = 階段の段
            for (const int32_t across : {-1, 1}) {
                const int64_t sideX = (int64_t)x + across;
                if (sideX < 0 || sideX >= (int64_t)width)
                    continue;

                const uint32_t diagonal = m_index[(size_t)sideX + (size_t)width * (up + 1)];
                const uint32_t sideCell = m_index[(size_t)sideX + (size_t)width * up];
                const uint32_t aboveCell = m_index[x + (size_t)width * (up + 1)];
                const bool side = sideCell != NO_CELL;
                const bool above = aboveCell != NO_CELL;
                if (diagonal == NO_CELL || side == above)
                    continue;

                // ここは同じ大きさのセルの間だけ(細かいセルどうしは AddFineBraces。分けたセルが絡む段は AddMixedBraces)
                if (diagonal == SPLIT_CELL || sideCell == SPLIT_CELL || aboveCell == SPLIT_CELL)
                    continue;

                const uint32_t intact = BraceIntact(x, up, (uint32_t)sideX, side);
                if (intact != 0)
                    AddBrace({.low = cell, .high = diagonal, .across = across, .unit = Scaled(m_brace, intact)});
            }
        }

        if (m_mixed)
            BuildMixedBonds();
    }

    // --- セル(1 つの大きさ)---
    void SupportLaw::BuildCells() {
        const uint32_t width = m_scene.width;
        m_index.assign((size_t)width * m_scene.height, (uint32_t)NO_CELL);
        for (uint32_t up = 0; up < m_scene.height; ++up) {
            for (uint32_t x = 0; x < width; ++x) {
                if (m_scene.solid[x + (size_t)width * up] == 0)
                    continue;

                m_index[x + (size_t)width * up] = (uint32_t)m_cells.size();
                m_cells.push_back({x, up});
            }
        }
    }

    // --- セル(違う大きさが混ざる。T-1120): 分けたセルは 2 × 2 の細かいセル(細かい格子の座標)---
    void SupportLaw::BuildMixedCells() {
        const uint32_t width = m_scene.width;
        m_index.assign((size_t)width * m_scene.height, (uint32_t)NO_CELL);
        m_fineIndex.assign((size_t)4 * width * m_scene.height, (uint32_t)NO_CELL);
        for (uint32_t up = 0; up < m_scene.height; ++up) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t at = x + (size_t)width * up;
                if (m_scene.solid[at] == 0)
                    continue;

                if (m_scene.fine[at] == 0) {
                    m_index[at] = (uint32_t)m_cells.size();
                    m_cells.push_back({x, up});
                    m_cellFine.push_back(0);
                    continue;
                }

                m_index[at] = SPLIT_CELL;
                for (uint32_t part = 0; part < 4; ++part) {
                    const uint32_t fx = 2 * x + (part & 1);
                    const uint32_t fup = 2 * up + (part >> 1);
                    if (!m_scene.fineAbsent.empty() && m_scene.fineAbsent[fx + (size_t)2 * width * fup] != 0)
                        continue;  // 割れて分かれた細かいセル(T-1067)

                    m_fineIndex[fx + (size_t)2 * width * fup] = (uint32_t)m_cells.size();
                    m_cells.push_back({fx, fup});
                    m_cellFine.push_back(1);
                }
            }
        }
    }

    // --- セルの重さ + 載せた荷(分けたセルの荷は 4 つに分け、割り切れない分は左下の細かいセルへ)---
    void SupportLaw::BuildLoads() {
        const int64_t fineWeight = m_mixed ? CellWeight(m_scene, true) : 0;
        m_loads.assign(m_cells.size(), 0);
        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            const auto [x, up] = m_cells[cell];
            const bool fine = m_mixed && m_cellFine[cell] != 0;
            const size_t at = fine ? x / 2 + (size_t)m_scene.width * (up / 2) : x + (size_t)m_scene.width * up;
            const int64_t load = m_scene.loadMn.empty() ? 0 : m_scene.loadMn[at];
            if (!fine) {
                m_loads[cell] = m_weight + load;
                continue;
            }

            const bool first = (x & 1) == 0 && (up & 1) == 0;
            m_loads[cell] = fineWeight + (first ? load - 3 * (load / 4) : load / 4);
        }
    }

    // --- 細かいセルの面(T-1120): 細かいセルどうしは同じ形の面・分けていないセルとの間は違う大きさの梁 ---
    void SupportLaw::BuildMixedBonds() {
        const uint32_t fineWidth = 2 * m_scene.width;
        const uint32_t fineHeight = 2 * m_scene.height;
        m_mixedFaceAt.assign((size_t)fineWidth * fineHeight * 2, (uint32_t)NO_CELL);
        const auto addMixed = [&](uint32_t coarse, uint32_t fine, uint32_t axis, bool coarseLow, int64_t offset,
                                  uint32_t faceX, uint32_t faceUp) {
            const auto first = (uint32_t)m_mixedTerms.size();
            AddMixedBond(coarse, fine, axis, coarseLow, offset, FineIntact(faceX, faceUp, axis),
                         PairBeta(fine, axis, faceX, faceUp));
            if (m_mixedTerms.size() != first)
                m_mixedFaceAt[(faceX + (size_t)fineWidth * faceUp) * 2 + axis] = first;
        };
        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            if (m_cellFine[cell] == 0)
                continue;

            const auto [fx, fup] = m_cells[cell];
            AddFineClamps(cell);
            AddFineBraces(cell);
            for (uint32_t axis = 0; axis < 2; ++axis) {
                const uint32_t stepX = axis == 0 ? 1 : 0;
                const uint32_t stepUp = 1 - stepX;
                const int64_t offset = ((axis == 0 ? fup : fx) & 1) != 0 ? 1 : -1;  // 粗いセルの中心から見た横の側 σ

                // + 側
                const uint32_t nextX = fx + stepX;
                const uint32_t nextUp = fup + stepUp;
                if (nextX < fineWidth && nextUp < fineHeight) {
                    const uint32_t next = m_fineIndex[nextX + (size_t)fineWidth * nextUp];
                    const uint32_t coarse = m_index[nextX / 2 + (size_t)m_scene.width * (nextUp / 2)];
                    if (next != NO_CELL)
                        AddBond({.low = cell, .high = next, .axis = axis}, FineIntact(fx, fup, axis));
                    else if (coarse < SPLIT_CELL)
                        addMixed(coarse, cell, axis, false, offset, fx, fup);
                }

                // − 側(分けていないセルとの間だけ。細かいセルどうしは − 側のセルが足す)
                if ((axis == 0 ? fx : fup) == 0)
                    continue;

                const uint32_t backX = fx - stepX;
                const uint32_t backUp = fup - stepUp;
                const uint32_t coarse = m_index[backX / 2 + (size_t)m_scene.width * (backUp / 2)];
                if (m_fineIndex[backX + (size_t)fineWidth * backUp] == NO_CELL && coarse < SPLIT_CELL)
                    addMixed(coarse, cell, axis, true, offset, backX, backUp);
            }
        }

        BuildMixedPairs();
        AddMixedBraces();
    }

    // 細かいセルどうしの段の斜めの棒(T-1067): 細かい格子で、斜め上の隣が細かいセル・間の 2 つの場所(横・上)の片方だけにセルがある所。
    // 棒の硬さは大きさによらない(軸の硬さ E A ÷ L の A ∝ 一辺 × D・L ∝ 一辺)ので粗いセルと同じ m_brace。違う大きさのセルの段は AddMixedBraces
    void SupportLaw::AddFineBraces(uint32_t cell) {
        const uint32_t fineWidth = 2 * m_scene.width;
        const auto [fx, fup] = m_cells[cell];
        if (!m_options.stairBraces || fup + 1 >= 2 * m_scene.height)
            return;

        const auto occupied = [&](uint32_t x, uint32_t up) {
            return m_fineIndex[x + (size_t)fineWidth * up] != NO_CELL ||
                   m_index[x / 2 + (size_t)m_scene.width * (up / 2)] < SPLIT_CELL;
        };
        for (const int32_t across : {-1, 1}) {
            const int64_t sideX = (int64_t)fx + across;
            if (sideX < 0 || sideX >= (int64_t)fineWidth)
                continue;

            const auto sx = (uint32_t)sideX;
            const uint32_t diagonal = m_fineIndex[sx + (size_t)fineWidth * (fup + 1)];
            const bool side = occupied(sx, fup);
            if (diagonal == NO_CELL || side == occupied(fx, fup + 1))
                continue;

            const uint32_t lowX = std::min(fx, sx);
            const uint32_t intact = side ? std::min(FineIntact(lowX, fup, 0), FineIntact(sx, fup, 1))
                                         : std::min(FineIntact(fx, fup, 1), FineIntact(lowX, fup + 1, 0));
            if (intact != 0)
                AddBrace({.low = cell, .high = diagonal, .across = across, .unit = Scaled(m_brace, intact)});
        }
    }

    // 違う大きさのセルが絡む段の斜めの棒(T-1133・ADR-0504): 細かい格子で段を探す(分けていないセルは 4 つの場所を持つ = 全部細かい
    // 場面の段と同じ場所)。端の片方が分けていないセルの所と、両端が分けていないセルで間の場所の元のセルが分けたセルの所(Build は飛ばす)
    void SupportLaw::AddMixedBraces() {
        if (!m_options.stairBraces || m_options.mixedBrace == MixedBrace::None)
            return;

        for (uint32_t fup = 0; fup + 1 < 2 * m_scene.height; ++fup) {
            for (uint32_t fx = 0; fx < 2 * m_scene.width; ++fx) {
                AddMixedBrace(fx, fup, -1);
                AddMixedBrace(fx, fup, 1);
            }
        }
    }

    // 細かい場所 (fx, fup) → 斜め上 (fx + across, fup + 1) の段。分けていないセルの端は、その場所が段の角の側の角の場所の時だけ
    // (棒が 45° = 端の中心の差が横と上で同じ。割れて細かいセルが欠けた所で角でない場所が段になる時は置かない)
    void SupportLaw::AddMixedBrace(uint32_t fx, uint32_t fup, int32_t across) {
        const uint32_t fineWidth = 2 * m_scene.width;
        const int64_t sideX = (int64_t)fx + across;
        if (sideX < 0 || sideX >= (int64_t)fineWidth)
            return;

        const auto occupant = [&](uint32_t x, uint32_t up) {
            const uint32_t fine = m_fineIndex[x + (size_t)fineWidth * up];
            if (fine != NO_CELL)
                return fine;

            const uint32_t coarse = m_index[x / 2 + (size_t)m_scene.width * (up / 2)];

            return coarse < SPLIT_CELL ? coarse : (uint32_t)NO_CELL;
        };
        const auto split = [&](uint32_t x, uint32_t up) {
            return m_index[x / 2 + (size_t)m_scene.width * (up / 2)] == SPLIT_CELL;
        };

        const auto sx = (uint32_t)sideX;
        const uint32_t low = occupant(fx, fup);
        const uint32_t high = occupant(sx, fup + 1);
        const bool side = occupant(sx, fup) != NO_CELL;
        if (low == NO_CELL || high == NO_CELL || low == high || side == (occupant(fx, fup + 1) != NO_CELL))
            return;

        const bool coarseLow = m_cellFine[low] == 0;
        const bool coarseHigh = m_cellFine[high] == 0;
        if (!coarseLow && !coarseHigh)
            return;  // 細かいセルどうし(AddFineBraces)

        if (coarseLow && coarseHigh && !split(sx, fup) && !split(fx, fup + 1))
            return;  // 粗いセルどうしで間も粗いセル(Build)

        // 角の場所: 下の粗いセルは上の段・段の側の列、上の粗いセルは下の段・段の側の列
        const bool rightColumn = across > 0;
        if (coarseLow && ((fup & 1) == 0 || ((fx & 1) != 0) != rightColumn))
            return;

        if (coarseHigh && ((fup & 1) == 0 || ((sx & 1) == 0) != rightColumn))
            return;

        const uint32_t lowX = std::min(fx, sx);
        const uint32_t intact = side ? std::min(FineIntact(lowX, fup, 0), FineIntact(sx, fup, 1))
                                     : std::min(FineIntact(fx, fup, 1), FineIntact(lowX, fup + 1, 0));
        if (intact == 0)
            return;

        // 硬さ(MixedBrace): ① 中心の間の長さは片方の端が粗いセルで細かいセルの 1.5 倍・両端で 2 倍
        uint64_t unit = m_brace;
        if (m_options.mixedBrace == MixedBrace::Center)
            unit = coarseLow && coarseHigh ? m_brace / 2 : m_brace * 2 / 3;

        AddBrace({.low = low, .high = high, .across = across, .unit = Scaled(unit, intact)});
    }

    // 分けたセルの固定した面は、向きの側の細かいセルへ(面の点の式は大きさによらず同じ = 冒頭。T-1206)
    void SupportLaw::AddFineClamps(uint32_t cell) {
        const auto [fx, fup] = m_cells[cell];
        const uint8_t clamps = m_scene.clamps[fx / 2 + (size_t)m_scene.width * (fup / 2)];
        if ((clamps & SUPPORT_CLAMP_LEFT) != 0 && (fx & 1) == 0)
            AddClamp({.cell = cell, .axis = 0, .side = -1});

        if ((clamps & SUPPORT_CLAMP_RIGHT) != 0 && (fx & 1) != 0)
            AddClamp({.cell = cell, .axis = 0, .side = 1});

        if ((clamps & SUPPORT_CLAMP_BOTTOM) != 0 && (fup & 1) == 0)
            AddClamp({.cell = cell, .axis = 1, .side = -1});
    }

    // 違う大きさの梁(冒頭の式。低い側 = −軸の側)。MixedBeam::CenterLine は粗い側の端の横のずれ σ を見ない
    void SupportLaw::AddMixedBond(uint32_t coarse, uint32_t fine, uint32_t axis, bool coarseLow, int64_t offset,
                                  uint32_t intact, uint32_t beta) {
        if (intact == 0)
            return;

        const uint64_t b = Scaled(axis == 0 ? m_bondX : m_bondUp, intact);
        const uint32_t along = axis == 0 ? SUPPORT_X : SUPPORT_UP;
        const uint32_t across = axis == 0 ? SUPPORT_UP : SUPPORT_X;
        const int64_t turnSign = axis == 0 ? -1 : 1;  // ε
        const int64_t coarseSide = coarseLow ? -1 : 1;
        const int64_t fineSide = -coarseSide;
        const bool faceOffset = m_options.mixedBeam != MixedBeam::CenterLine;
        const auto term = [&](uint64_t coefficient) {
            return MixedTerm{.low = coarseLow ? coarse : fine,
                             .high = coarseLow ? fine : coarse,
                             .coefficient = coefficient,
                             .intact = intact};
        };
        const auto gradientOf = [&](MixedTerm& each, bool ofCoarse) -> std::array<int64_t, SUPPORT_COMPONENTS>& {
            return ofCoarse == coarseLow ? each.gradientLow : each.gradientHigh;
        };

        // 軸: ½ b (2 Δn')²(2 n_C' = 2 n_C + ε σ φ_C)。対のずれは β の分を対の 3 つの節の項に(AddMixedPair。冒頭)
        const uint64_t axialPair = Scaled(b, beta);
        MixedTerm axial = term(b - axialPair);
        gradientOf(axial, true)[along] = 2 * coarseSide;
        gradientOf(axial, false)[along] = 2 * fineSide;
        gradientOf(axial, true)[SUPPORT_TURN] = faceOffset ? coarseSide * turnSign * offset : 0;
        m_mixedTerms.push_back(axial);

        // ずれ: 面の点のばね ½ 4b (Δt + ε (φ_C + φ_F))²(FaceLever は硬さだけ ② の 16b/9)・② ½ (b/9) (4 Δt + ε (3 φ_C + 6 φ_F))²
        const bool atFace = m_options.mixedBeam == MixedBeam::FacePair ||
                            m_options.mixedBeam == MixedBeam::FacePairShear ||
                            m_options.mixedBeam == MixedBeam::FaceSpring || m_options.mixedBeam == MixedBeam::FaceLever;
        MixedTerm shear = term(!atFace ? b / 9 : m_options.mixedBeam == MixedBeam::FaceLever ? 16 * b / 9 : 4 * b);
        const int64_t slideScale = atFace ? 1 : 4;
        gradientOf(shear, true)[across] = slideScale * coarseSide;
        gradientOf(shear, false)[across] = slideScale * fineSide;
        gradientOf(shear, true)[SUPPORT_TURN] = (atFace ? 1 : 3) * turnSign;
        gradientOf(shear, false)[SUPPORT_TURN] = (atFace ? 1 : 6) * turnSign;
        m_mixedTerms.push_back(shear);

        // 曲げ: ½ (b/3) (s 角_high − s 角_low)²
        MixedTerm bend = term(b / 3);
        gradientOf(bend, true)[SUPPORT_TURN] = coarseSide;
        gradientOf(bend, false)[SUPPORT_TURN] = 2 * fineSide;
        m_mixedTerms.push_back(bend);

        // 対の曲げの差を引くのは FacePair だけ(FacePairShear は 0 = T-1207 の前とビット一致)
        const uint64_t bendPair = m_options.mixedBeam == MixedBeam::FacePair ? Scaled(bend.coefficient, beta) : 0;
        m_mixedGroups.push_back(
            {.beta = beta, .axialPair = axialPair, .shearPair = Scaled(shear.coefficient, beta), .bendPair = bendPair});
    }

    // 対の面として読む割合 β(Q16。冒頭): FacePair で、対の相手の細かいセルがあれば、対の中の面と 2 つの違う大きさの面の残りの硬さの
    // 小さい方。(faceX, faceUp) は違う大きさの面の鍵(m_mixedFaceAt)。対の相手は横の座標の奇偶を入れ替えた所
    uint32_t SupportLaw::PairBeta(uint32_t fine, uint32_t axis, uint32_t faceX, uint32_t faceUp) const {
        if (m_options.mixedBeam != MixedBeam::FacePair && m_options.mixedBeam != MixedBeam::FacePairShear)
            return 0;

        const auto [fx, fup] = m_cells[fine];
        const uint32_t partnerX = axis == 0 ? fx : fx ^ 1;
        const uint32_t partnerUp = axis == 0 ? fup ^ 1 : fup;
        if (m_fineIndex[partnerX + (size_t)2 * m_scene.width * partnerUp] == NO_CELL)
            return 0;

        const uint32_t inner = axis == 0 ? FineIntact(fx, fup & ~1u, 1) : FineIntact(fx & ~1u, fup, 0);
        const uint32_t partner = axis == 0 ? FineIntact(faceX, faceUp ^ 1, axis) : FineIntact(faceX ^ 1, faceUp, axis);

        return std::min({inner, partner, FineIntact(faceX, faceUp, axis), SUPPORT_INTACT_FULL});
    }

    // 対(冒頭): L(横の座標が偶数の側)の違う大きさの面から相手の U の面を探す
    void SupportLaw::BuildMixedPairs() {
        const uint32_t fineWidth = 2 * m_scene.width;
        for (uint32_t fup = 0; fup < 2 * m_scene.height; ++fup) {
            for (uint32_t fx = 0; fx < fineWidth; ++fx) {
                for (uint32_t axis = 0; axis < 2; ++axis) {
                    const uint32_t lower = m_mixedFaceAt[(fx + (size_t)fineWidth * fup) * 2 + axis];
                    if (lower == NO_CELL || ((axis == 0 ? fup : fx) & 1) != 0 ||
                        m_mixedGroups[lower / MIXED_GROUP_TERMS].beta == 0)
                        continue;

                    const uint32_t partnerX = axis == 0 ? fx : fx + 1;
                    const uint32_t partnerUp = axis == 0 ? fup + 1 : fup;
                    const uint32_t upper = m_mixedFaceAt[(partnerX + (size_t)fineWidth * partnerUp) * 2 + axis];
                    if (upper != NO_CELL)
                        AddMixedPair(lower, upper);
                }
            }
        }
    }

    // 対のずれの負の項と、伸びの 3 つの節の項(冒頭)。組の項は 0 = 伸び・1 = ずれ・2 = 曲げ
    void SupportLaw::AddMixedPair(uint32_t lower, uint32_t upper) {
        MixedGroup& groupLower = m_mixedGroups[lower / MIXED_GROUP_TERMS];
        MixedGroup& groupUpper = m_mixedGroups[upper / MIXED_GROUP_TERMS];
        groupLower.pair = (uint32_t)m_mixedPairs.size();
        groupUpper.pair = groupLower.pair;
        groupUpper.upper = true;
        m_mixedPairs.push_back({.lower = lower,
                                .upper = upper,
                                .pairTerm = (uint32_t)m_pairTerms.size(),
                                .firstTriple = (uint32_t)m_tripleTerms.size()});

        const auto fineOf = [&](const MixedTerm& term) {
            return m_cellFine[term.high] != 0 ? term.high : term.low;
        };
        const auto coarseOf = [&](const MixedTerm& term) {
            return m_cellFine[term.high] != 0 ? term.low : term.high;
        };
        const auto gradientOf = [&](const MixedTerm& term, uint32_t cell) {
            return term.high == cell ? term.gradientHigh : term.gradientLow;
        };

        // ずれ: −½ K (d_U − d_L)²(d = 細かい側の勾配 g の分。C の分は消える)
        const MixedTerm& shearLower = m_mixedTerms[lower + 1];
        const MixedTerm& shearUpper = m_mixedTerms[upper + 1];
        const uint32_t fineLower = fineOf(shearLower);
        const uint32_t fineUpper = fineOf(shearUpper);
        const Gradient shearFine = gradientOf(shearLower, fineLower);
        Gradient negated{};
        for (uint32_t c = 0; c < SUPPORT_COMPONENTS; ++c)
            negated[c] = -shearFine[c];

        m_pairTerms.push_back({.low = fineLower,
                               .high = fineUpper,
                               .coefficient = SeriesCoefficient(groupLower.shearPair, groupUpper.shearPair),
                               .gradientLow = negated,
                               .gradientHigh = shearFine,
                               .intact = groupLower.beta,
                               .negative = true});

        // 曲げの差: −½ K_曲げ (p_L − p_U)²(p = 細かい側の曲げの勾配の分。C の分は消える。冒頭・T-1207)
        const Gradient bendFine = gradientOf(m_mixedTerms[lower + 2], fineLower);
        Gradient bendNegated{};
        for (uint32_t c = 0; c < SUPPORT_COMPONENTS; ++c)
            bendNegated[c] = -bendFine[c];

        m_pairTerms.push_back({.low = fineLower,
                               .high = fineUpper,
                               .coefficient = SeriesCoefficient(groupLower.bendPair, groupUpper.bendPair),
                               .gradientLow = bendFine,
                               .gradientHigh = bendNegated,
                               .intact = groupLower.beta,
                               .negative = true});

        // 伸び: ½ (b/9) (3 × 2Δn'_k − 側 σ_k (d_L + d_U))²(σ_L = −1・σ_U = +1。側 = 細かいセルが +軸の側なら +1)
        const uint32_t coarse = coarseOf(shearLower);
        const int64_t fineSide = m_cellFine[shearLower.high] != 0 ? 1 : -1;
        for (const uint32_t own : {lower, upper}) {
            const MixedTerm& axial = m_mixedTerms[own];
            const uint32_t ownFine = fineOf(axial);
            const uint32_t otherFine = ownFine == fineLower ? fineUpper : fineLower;
            const int64_t mix = -fineSide * (own == lower ? -1 : 1);
            TripleTerm triple{.nodes = {coarse, ownFine, otherFine},
                              .coefficient = m_mixedGroups[own / MIXED_GROUP_TERMS].axialPair / 9};
            for (uint32_t c = 0; c < SUPPORT_COMPONENTS; ++c) {
                triple.gradients[0][c] = 3 * gradientOf(axial, coarse)[c] +
                                         mix * (gradientOf(shearLower, coarse)[c] + gradientOf(shearUpper, coarse)[c]);
                triple.gradients[1][c] = 3 * gradientOf(axial, ownFine)[c] + mix * shearFine[c];
                triple.gradients[2][c] = mix * shearFine[c];
            }

            m_tripleTerms.push_back(triple);
        }
    }

    // --- 成分を持つ節の系(Coupled。T-0919): Lagged と同じエネルギーの項を 3 × 3 の面のブロックへ ---
    void SupportLaw::BuildBlocks() {
        EquilibriumBlockSystem& system = m_blockSystem;
        system.components = SUPPORT_COMPONENTS;
        system.coefficientShift = BLOCK_COEFFICIENT_SHIFT;
        system.prolongation = RigidProlongation();
        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            const auto [x, up] = m_cells[cell];
            // 混ざる場面は細かいセルがレベル 1(細かいほど大きい = EquilibriumNode の約束。座標は細かいセルの単位)。
            // T-1120 の初めは逆(粗いセルが 1)で、段の作りが粗いセルを先にまとめて細かいセルの座標と重ね、同じ色の節が隣り合った(T-1104 で見つけた)
            const int32_t level = m_mixed && m_cellFine[cell] != 0 ? 1 : 0;
            system.nodes.push_back({.level = level, .x = x, .y = up, .z = 0});
        }

        // 面: ½ 6b Δn² + ½ 6b (Δt ∓ (φ_a + φ_b))² + ½ 2b Δφ²(∓ は x の面で −・上の面で +)
        for (const Bond& bond : m_bonds) {
            const uint64_t b = bond.unit;
            const uint32_t along = bond.axis == 0 ? SUPPORT_X : SUPPORT_UP;
            const uint32_t across = bond.axis == 0 ? SUPPORT_UP : SUPPORT_X;
            const int64_t turnSign = bond.axis == 0 ? -1 : 1;
            EquilibriumBlockFace face{.low = bond.low, .high = bond.high};
            Gradient low{};
            Gradient high{};
            low[along] = -1;
            high[along] = 1;
            AddTerm(face, 6 * b, low, high);

            low = {};
            high = {};
            low[across] = -1;
            high[across] = 1;
            low[SUPPORT_TURN] = turnSign;
            high[SUPPORT_TURN] = turnSign;
            AddTerm(face, 6 * b, low, high);

            AddTerm(face, 2 * b, {0, 0, -1}, {0, 0, 1});
            system.faces.push_back(face);
        }

        // 段の斜めの棒: ½ q (s Δx + Δup)²(同じレベルの斜めの 2 節の面。x ⇄ 上の非対角も面の中)
        for (const Brace& brace : m_braces) {
            EquilibriumBlockFace face{.low = brace.low, .high = brace.high};
            AddTerm(face, brace.unit, {-brace.across, -1, 0}, {brace.across, 1, 0});
            system.faces.push_back(face);
        }

        // 違う大きさの面(T-1120・T-1191): 同じ 2 節の 3 つの項を 1 つの面へ。対は L と U の面に負の項、3 つの節の項は 3 つの面に分けて入れる
        // (面だけでは正でない所がある。系の全体は正)
        BuildMixedBlocks(system.faces);

        // 固定した面(面の点の形。T-1206): ½ 12b n² + ½ 12b (t + s φ)² + ½ 4b φ²(s は Lagged の AddClamp と同じ)
        for (const Clamp& clamp : m_clamps) {
            const uint64_t b = clamp.axis == 0 ? m_bondX : m_bondUp;
            const uint32_t along = clamp.axis == 0 ? SUPPORT_X : SUPPORT_UP;
            const uint32_t across = clamp.axis == 0 ? SUPPORT_UP : SUPPORT_X;
            EquilibriumBlockFixed fixed{.node = clamp.cell};
            Gradient gradient{};
            gradient[along] = 1;
            AddFixedTerm(fixed, 12 * b, gradient);

            gradient = {};
            gradient[across] = 1;
            gradient[SUPPORT_TURN] = clamp.axis == 0 ? clamp.side : -clamp.side;
            AddFixedTerm(fixed, 12 * b, gradient);

            AddFixedTerm(fixed, 4 * b, {0, 0, 1});
            system.fixedFaces.push_back(fixed);
        }

        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            system.sources.push_back(
                {.node = cell, .component = SUPPORT_UP, .value = -(CellLoad(cell) << BLOCK_FORCE_SHIFT)});
        }

        m_blockGrid = BuildEquilibriumBlockGrid(system);
    }

    void SupportLaw::BuildMixedBlocks(std::vector<EquilibriumBlockFace>& faces) const {
        const size_t mixedFirst = faces.size();
        for (size_t first = 0; first < m_mixedTerms.size(); first += MIXED_GROUP_TERMS) {
            EquilibriumBlockFace face{.low = m_mixedTerms[first].low, .high = m_mixedTerms[first].high};
            for (size_t i = first; i < first + MIXED_GROUP_TERMS; ++i)
                AddTerm(face, m_mixedTerms[i].coefficient, m_mixedTerms[i].gradientLow, m_mixedTerms[i].gradientHigh);

            faces.push_back(face);
        }

        for (const MixedPair& pair : m_mixedPairs) {
            const MixedTerm& term = m_pairTerms[pair.pairTerm];
            const MixedTerm& bendTerm = m_pairTerms[pair.pairTerm + 1];
            EquilibriumBlockFace pairFace{.low = term.low, .high = term.high};
            AddTerm(pairFace, term.coefficient, term.gradientLow, term.gradientHigh, true);
            AddTerm(pairFace, bendTerm.coefficient, bendTerm.gradientLow, bendTerm.gradientHigh, true);
            for (uint32_t i = 0; i < 2; ++i) {
                const TripleTerm& triple = m_tripleTerms[pair.firstTriple + i];
                EquilibriumBlockFace& own = faces[mixedFirst + (i == 0 ? pair.lower : pair.upper) / MIXED_GROUP_TERMS];
                EquilibriumBlockFace&
                    other = faces[mixedFirst + (i == 0 ? pair.upper : pair.lower) / MIXED_GROUP_TERMS];
                AddTriple(own, other, pairFace, triple);
            }

            faces.push_back(pairFace);
        }
    }

    // 3 つの節の項 c g_i g_jᵀ を面へ: 粗いセルと自分の面に 2 つの対角と間・粗いセルと相手の面に相手の対角と間・L と U の面に 2 つの細かいセルの間
    void SupportLaw::AddTriple(EquilibriumBlockFace& own, EquilibriumBlockFace& other, EquilibriumBlockFace& pairFace,
                               const TripleTerm& triple) const {
        const auto c = (int64_t)triple.coefficient;
        const auto add = [&](EquilibriumBlockFace& face, uint32_t i, uint32_t j, bool diagonals) {
            const bool aLow = face.low == triple.nodes[i];
            FX_ASSERT(aLow ? face.high == triple.nodes[j]
                           : face.high == triple.nodes[i] && face.low == triple.nodes[j]);
            const Gradient& low = aLow ? triple.gradients[i] : triple.gradients[j];
            const Gradient& high = aLow ? triple.gradients[j] : triple.gradients[i];
            for (uint32_t row = 0; row < SUPPORT_COMPONENTS; ++row) {
                for (uint32_t column = 0; column < SUPPORT_COMPONENTS; ++column) {
                    const uint32_t at = row * EQ_MAX_COMPONENTS + column;
                    face.lowHigh[at] += c * low[row] * high[column];
                    if (!diagonals)
                        continue;

                    face.lowLow[at] += c * low[row] * low[column];
                    face.highHigh[at] += c * high[row] * high[column];
                }
            }
        };

        add(own, 0, 1, true);
        add(other, 0, 2, false);
        add(pairFace, 1, 2, false);

        // 相手の対角は粗いセルと相手の面へ
        const bool otherLow = other.low == triple.nodes[2];
        for (uint32_t row = 0; row < SUPPORT_COMPONENTS; ++row) {
            for (uint32_t column = 0; column < SUPPORT_COMPONENTS; ++column) {
                const int64_t value = c * triple.gradients[2][row] * triple.gradients[2][column];
                (otherLow ? other.lowLow : other.highHigh)[row * EQ_MAX_COMPONENTS + column] += value;
            }
        }
    }

    // 面の残りの硬さ(Q16)。scene の intactX / intactUp が空なら割れていない
    uint32_t SupportLaw::Intact(uint32_t x, uint32_t up, uint32_t axis) const {
        const std::vector<uint32_t>& intact = axis == 0 ? m_scene.intactX : m_scene.intactUp;

        return intact.empty() ? SUPPORT_INTACT_FULL : intact[x + (size_t)m_scene.width * up];
    }

    // 細かい格子の面の残りの硬さ(T-1067)。細かい並びが空なら、分けたセルの中は割れていない・ほかは元のセルの面の値(T-1120 と同じ)
    uint32_t SupportLaw::FineIntact(uint32_t fx, uint32_t fup, uint32_t axis) const {
        const std::vector<uint32_t>& intact = axis == 0 ? m_scene.intactFineX : m_scene.intactFineUp;
        if (!intact.empty())
            return intact[fx + (size_t)2 * m_scene.width * fup];

        const uint32_t nextX = axis == 0 ? fx + 1 : fx;
        const uint32_t nextUp = axis == 0 ? fup : fup + 1;
        if (nextX / 2 == fx / 2 && nextUp / 2 == fup / 2)
            return SUPPORT_INTACT_FULL;

        return Intact(fx / 2, fup / 2, axis);
    }

    // 段の斜めの棒は、間にあるセルを回る 2 つの面の残りの硬さの小さい方(仮。棒の力を面の応力へ配る形は R-B2 の残り)
    uint32_t SupportLaw::BraceIntact(uint32_t x, uint32_t up, uint32_t sideX, bool side) const {
        const uint32_t lowX = std::min(x, sideX);
        if (side)
            return std::min(Intact(lowX, up, 0), Intact(sideX, up, 1));

        return std::min(Intact(x, up, 1), Intact(lowX, up + 1, 0));
    }

    int64_t SupportLaw::CellLoad(uint32_t cell) const {
        return m_loads[cell];
    }

    void SupportLaw::AddBond(const Bond& candidate, uint32_t intact) {
        if (intact == 0)
            return;

        Bond bond = candidate;
        bond.unit = Scaled(bond.axis == 0 ? m_bondX : m_bondUp, intact);
        bond.intact = intact;
        m_bondAt[(size_t)bond.low * 2 + bond.axis] = (uint32_t)m_bonds.size();
        m_bonds.push_back(bond);
        const uint64_t b = bond.unit;
        const uint32_t along = bond.axis == 0 ? SUPPORT_X : SUPPORT_UP;
        const uint32_t across = bond.axis == 0 ? SUPPORT_UP : SUPPORT_X;
        const bool beam = m_options.form == SupportForm::BeamLattice;
        m_systems[along].faces.push_back(
            {.fine = bond.low, .coarse = bond.high, .gap = 0, .coefficient = ImWide(6 * b)});
        m_systems[across].faces.push_back(
            {.fine = bond.low, .coarse = bond.high, .gap = 0, .coefficient = ImWide(beam ? 6 * b : 3 * b)});
        if (!beam)
            return;

        // 回り: 曲げ(ka/3)は面・横のずれの (φ_a + φ_b)² は対角を容量に・正の非対角を遅らせる
        EquilibriumSystem& turn = m_systems[SUPPORT_TURN];
        turn.faces.push_back({.fine = bond.low, .coarse = bond.high, .gap = 0, .coefficient = ImWide(2 * b)});
        turn.nodes[bond.low].capacity += 6 * b;
        turn.nodes[bond.high].capacity += 6 * b;
        AddLag(SUPPORT_TURN, bond.low, SUPPORT_TURN, bond.high, 1, 6 * b);
        AddLag(SUPPORT_TURN, bond.high, SUPPORT_TURN, bond.low, 1, 6 * b);

        // 横の変位 ⇄ 回り: x の面は t_b − t_a − (φ_a + φ_b)、上の面は t_b − t_a + (φ_a + φ_b)
        const int64_t sign = bond.axis == 0 ? 1 : -1;
        for (const uint32_t turnCell : {bond.low, bond.high}) {
            AddLag(across, bond.low, SUPPORT_TURN, turnCell, sign, 6 * b);
            AddLag(across, bond.high, SUPPORT_TURN, turnCell, -sign, 6 * b);
            AddLag(SUPPORT_TURN, turnCell, across, bond.low, sign, 6 * b);
            AddLag(SUPPORT_TURN, turnCell, across, bond.high, -sign, 6 * b);
        }
    }

    void SupportLaw::AddClamp(const Clamp& clamp) {
        m_clamps.push_back(clamp);
        const uint64_t b = clamp.axis == 0 ? m_bondX : m_bondUp;
        const uint32_t along = clamp.axis == 0 ? SUPPORT_X : SUPPORT_UP;
        const uint32_t across = clamp.axis == 0 ? SUPPORT_UP : SUPPORT_X;
        const bool beam = m_options.form == SupportForm::BeamLattice;
        m_systems[along].fixedFaces.push_back({.node = clamp.cell, .coefficient = ImWide(12 * b), .value = 0});
        m_systems[across].fixedFaces.push_back(
            {.node = clamp.cell, .coefficient = ImWide(beam ? 12 * b : 6 * b), .value = 0});
        if (!beam)
            return;

        // 2ka (t ± φ)²(面の点の形。T-1206): φ の対角 2ka は容量・曲げ 2ka/3 は値 0 の固定した面・t ⇄ φ の 2ka は遅らせる
        // ±: 左の面 −・右の面 +・下の面 +(x の面の t は上・上の面の t は x で、回りの向きが逆になる)
        m_systems[SUPPORT_TURN].nodes[clamp.cell].capacity += 12 * b;
        m_systems[SUPPORT_TURN].fixedFaces.push_back({.node = clamp.cell, .coefficient = ImWide(4 * b), .value = 0});
        const int64_t sign = clamp.axis == 0 ? clamp.side : -clamp.side;
        AddLag(across, clamp.cell, SUPPORT_TURN, clamp.cell, sign, 12 * b);
        AddLag(SUPPORT_TURN, clamp.cell, across, clamp.cell, sign, 12 * b);
    }

    // ½ q (s Δx + Δup)²: x と上の面に q・x ⇄ 上の項(同じセルに +s q・相手のセルに −s q)は遅らせる
    void SupportLaw::AddBrace(const Brace& brace) {
        m_braces.push_back(brace);
        const uint64_t q = brace.unit;
        for (const uint32_t c : {SUPPORT_X, SUPPORT_UP})
            m_systems[c].faces.push_back({.fine = brace.low, .coarse = brace.high, .gap = 0, .coefficient = ImWide(q)});

        for (const uint32_t row : {SUPPORT_X, SUPPORT_UP}) {
            const uint32_t column = row == SUPPORT_X ? SUPPORT_UP : SUPPORT_X;
            AddLag(row, brace.low, column, brace.low, brace.across, q);
            AddLag(row, brace.low, column, brace.high, -brace.across, q);
            AddLag(row, brace.high, column, brace.high, brace.across, q);
            AddLag(row, brace.high, column, brace.low, -brace.across, q);
        }
    }

    void SupportLaw::AddLag(uint32_t rowComponent, uint32_t row, uint32_t columnComponent, uint32_t column,
                            int64_t sign, uint64_t magnitude) {
        m_lags[rowComponent].push_back({.row = row,
                                        .column = column,
                                        .columnComponent = columnComponent,
                                        .negative = sign < 0,
                                        .magnitude = magnitude});
    }

    void SupportLaw::CarryFrom(const SupportLaw& before) {
        if (Coupled()) {
            m_blockState.conjugateDiscard = true;  // 形・荷が変わった刻みは共役勾配の向きを捨てる(D-649)
            m_blockState.values.assign(m_cells.size() * SUPPORT_COMPONENTS, 0);
            for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
                for (uint32_t c = 0; c < SUPPORT_COMPONENTS; ++c)
                    m_blockState.values[cell * SUPPORT_COMPONENTS + c] = CarriedValue(before, cell, c);
            }

            return;
        }

        for (uint32_t c = 0; c < ComponentCount(); ++c) {
            m_states[c].values.assign(m_cells.size(), 0);
            for (uint32_t cell = 0; cell < m_cells.size(); ++cell)
                m_states[c].values[cell] = before.Value(m_cells[cell][0], m_cells[cell][1], c);
        }
    }

    // --- 1 刻み(Coupled): 成分を持つ節で 1 つの系を決まった回数の V で ---
    void SupportLaw::StepCoupled() {
        const EquilibriumBlockResult result = SolveEquilibriumBlocks(m_blockSystem, m_blockGrid, m_options.blocks,
                                                                     m_blockState);
        m_lastNodeUpdates = result.nodeUpdates;
        m_lastUnknowns = result.unknowns;
    }

    // --- 1 刻み(Lagged): x → 上 → 回り の順に、ほかの成分の今の値を右辺の項へ入れて C-0 で解く ---
    void SupportLaw::Step() {
        if (Coupled()) {
            StepCoupled();
            return;
        }

        // 欄に置いた時は、成分の間の結合が読む今の値も欄から(保存点から続けた最初の刻みも同じ値。T-0922)
        const EquilibriumWarmStart& warm = m_options.warmStart;
        if (warm.nest != nullptr) {
            for (uint32_t c = 0; c < ComponentCount(); ++c) {
                const EquilibriumWarmStart part = {
                    .nest = warm.nest, .field = warm.field, .component = warm.component + c};
                m_states[c].values = LoadWarmStart(part, m_systems[c].nodes, 1);
            }
        }

        m_lastNodeUpdates = 0;
        for (uint32_t c = 0; c < ComponentCount(); ++c) {
            EquilibriumSystem& system = m_systems[c];
            system.sources.clear();
            if (c == SUPPORT_UP) {
                for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
                    system.sources.push_back(
                        {.node = cell, .value = -CellLoad(cell), .factor = ImWide((uint64_t)1 << FORCE_SHIFT)});
                }
            }

            // 右辺 −K × 値(K < 0 なら +|K| × 値)
            for (const Lag& lag : m_lags[c]) {
                const int64_t value = Value(m_cells[lag.column][0], m_cells[lag.column][1], lag.columnComponent);
                if (value == 0)
                    continue;

                system.sources.push_back(
                    {.node = lag.row, .value = lag.negative ? value : -value, .factor = ImWide(lag.magnitude)});
            }

            EquilibriumOptions solve = m_options.equilibrium;
            if (warm.nest != nullptr)
                solve.warmStart = {.nest = warm.nest, .field = warm.field, .component = warm.component + c};

            m_lastNodeUpdates += SolveEquilibrium(system, solve, m_states[c]).cost.cellUpdates;
        }
    }

    // 掘る・割れる前の答え before からこのセルの値(T-1120: 大きさが変わったセルは剛体の動きで配る・平均でまとめる)
    int64_t SupportLaw::CarriedValue(const SupportLaw& before, uint32_t cell, uint32_t component) const {
        const auto [x, up] = m_cells[cell];
        const bool fine = m_mixed && m_cellFine[cell] != 0;
        if (!fine) {
            if (!before.IsFine(2 * x, 2 * up))
                return before.Value(x, up, component);

            // 細かい 4 つの平均(回りは単位が 2 倍: φ_粗 = 角 × s = 2 φ_細)
            int64_t sum = 0;
            for (uint32_t part = 0; part < 4; ++part)
                sum += before.FineValue(2 * x + (part & 1), 2 * up + (part >> 1), component);

            return component == SUPPORT_TURN ? sum / 2 : sum / 4;
        }

        if (before.IsFine(x, up))
            return before.FineValue(x, up, component);

        // 粗いセルから剛体の動きで(RigidProlongation と同じ: 横 ∓ φ/2・上 ± φ/2・回り φ/2)
        const int64_t turn = before.Value(x / 2, up / 2, SUPPORT_TURN);
        const int64_t value = before.Value(x / 2, up / 2, component);
        if (component == SUPPORT_TURN)
            return turn / 2;

        if (component == SUPPORT_X)
            return value + ((up & 1) != 0 ? -turn / 2 : turn / 2);

        return value + ((x & 1) != 0 ? turn / 2 : -turn / 2);
    }

    // --- 読む ---
    int64_t SupportLaw::Value(uint32_t x, uint32_t up, uint32_t component) const {
        if (x >= m_scene.width || up >= m_scene.height)
            return 0;

        const uint32_t cell = m_index[x + (size_t)m_scene.width * up];

        return cell >= SPLIT_CELL ? 0 : CellValue(cell, component);
    }

    int64_t SupportLaw::CellValue(uint32_t cell, uint32_t component) const {
        if (component >= ComponentCount())
            return 0;

        if (Coupled()) {
            const size_t at = (size_t)cell * SUPPORT_COMPONENTS + component;
            return at >= m_blockState.values.size() ? 0 : m_blockState.values[at];
        }

        const std::vector<int64_t>& values = m_states[component].values;

        return cell >= values.size() ? 0 : values[cell];
    }

    bool SupportLaw::IsFine(uint32_t fx, uint32_t fup) const {
        if (!m_mixed || fx >= 2 * m_scene.width || fup >= 2 * m_scene.height)
            return false;

        return m_fineIndex[fx + (size_t)2 * m_scene.width * fup] != NO_CELL;
    }

    int64_t SupportLaw::FineValue(uint32_t fx, uint32_t fup, uint32_t component) const {
        if (IsFine(fx, fup))
            return CellValue(m_fineIndex[fx + (size_t)2 * m_scene.width * fup], component);

        return Value(fx / 2, fup / 2, component);
    }

    int64_t SupportLaw::UpForceOnBond(const Bond& bond) const {
        const int64_t difference = CellValue(bond.high, SUPPORT_UP) - CellValue(bond.low, SUPPORT_UP);
        if (bond.axis == 1)
            return -Flow(6 * bond.unit, difference);

        if (m_options.form == SupportForm::PerComponent)
            return -Flow(3 * bond.unit, difference);

        const int64_t turns = CellValue(bond.low, SUPPORT_TURN) + CellValue(bond.high, SUPPORT_TURN);

        return -Flow(6 * bond.unit, difference - turns);
    }

    int64_t SupportLaw::UpForceOnBrace(const Brace& brace) const {
        const int64_t slide = CellValue(brace.high, SUPPORT_X) - CellValue(brace.low, SUPPORT_X);
        const int64_t lift = CellValue(brace.high, SUPPORT_UP) - CellValue(brace.low, SUPPORT_UP);

        return -Flow(brace.unit, brace.across * slide + lift);
    }

    // 項 ½ c (g · x)² が high のセルへ当てる上の力 −c (g · x) g_high,上(low へは g_low,上 = −g_high,上 なので同じ値の逆)
    int64_t SupportLaw::TermProjection(const MixedTerm& term) const {
        int64_t projection = 0;
        for (uint32_t c = 0; c < SUPPORT_COMPONENTS; ++c) {
            projection += term.gradientLow[c] * CellValue(term.low, c);
            projection += term.gradientHigh[c] * CellValue(term.high, c);
        }

        return projection;
    }

    int64_t SupportLaw::TripleProjection(const TripleTerm& term) const {
        int64_t projection = 0;
        for (uint32_t i = 0; i < 3; ++i) {
            for (uint32_t c = 0; c < SUPPORT_COMPONENTS; ++c)
                projection += term.gradients[i][c] * CellValue(term.nodes[i], c);
        }

        return projection;
    }

    int64_t SupportLaw::TermForce(const MixedTerm& term) const {
        const int64_t force = Flow(term.coefficient, TermProjection(term));

        return term.negative ? -force : force;
    }

    int64_t SupportLaw::UpForceOnTerm(const MixedTerm& term) const {
        return -TermForce(term) * term.gradientHigh[SUPPORT_UP];
    }

    int64_t SupportLaw::UpForceOnClamp(const Clamp& clamp) const {
        const int64_t lift = CellValue(clamp.cell, SUPPORT_UP);
        if (clamp.axis == 1)
            return -Flow(12 * m_bondUp, lift);

        if (m_options.form == SupportForm::PerComponent)
            return -Flow(6 * m_bondX, lift);

        // 2ka (t ± φ) = 12b (t ± φ)(面の点の形。T-1206)
        return -Flow(12 * m_bondX, lift + clamp.side * CellValue(clamp.cell, SUPPORT_TURN));
    }

    // 面の力(梁の切り口。dx = セル・ka = 6b): N = ka Δn・ずれ = ka (Δt ∓ (φ_a + φ_b))・6M/dx = ka (φ_b − φ_a)。
    // 縁の力 = N ± 6M/dx(回り φ は反時計回りが正なので、x の面は下の縁が +・上の面は +x の縁が +)
    SupportFaceForces SupportLaw::FaceForces(uint32_t x, uint32_t up, uint32_t axis) const {
        const uint32_t highX = axis == 0 ? x + 1 : x;
        const uint32_t highUp = axis == 0 ? up : up + 1;
        if (m_options.form != SupportForm::BeamLattice || highX >= m_scene.width || highUp >= m_scene.height)
            return {};

        // 分けたセル(SPLIT_CELL)が絡む面は FineFaceForces(T-1067)
        const uint32_t cell = m_index[x + (size_t)m_scene.width * up];
        if (cell >= SPLIT_CELL || m_index[highX + (size_t)m_scene.width * highUp] >= SPLIT_CELL)
            return {};

        const uint32_t bondIndex = m_bondAt[(size_t)cell * 2 + axis];
        if (bondIndex == NO_CELL)
            return {};  // 割れた面

        return BondForces(m_bonds[bondIndex]);
    }

    // 同じ大きさのセルどうしの面の力(冒頭の式。大きさによらず同じ形 = ADR-0470 の 2)
    SupportFaceForces SupportLaw::BondForces(const Bond& bond) const {
        const uint64_t b = bond.unit;
        const uint32_t along = bond.axis == 0 ? SUPPORT_X : SUPPORT_UP;
        const uint32_t across = bond.axis == 0 ? SUPPORT_UP : SUPPORT_X;
        const int64_t turnLow = CellValue(bond.low, SUPPORT_TURN);
        const int64_t turnHigh = CellValue(bond.high, SUPPORT_TURN);
        const int64_t stretch = CellValue(bond.high, along) - CellValue(bond.low, along);
        const int64_t slide = CellValue(bond.high, across) - CellValue(bond.low, across);
        const int64_t turns = bond.axis == 0 ? -(turnLow + turnHigh) : turnLow + turnHigh;

        SupportFaceForces forces;
        forces.axialMn = Flow(6 * b, stretch);
        forces.shearMn = Flow(6 * b, slide + turns);
        const int64_t bend = Flow(6 * b, turnHigh - turnLow);
        forces.lowSideMn = bond.axis == 0 ? forces.axialMn + bend : forces.axialMn - bend;
        forces.highSideMn = bond.axis == 0 ? forces.axialMn - bend : forces.axialMn + bend;

        // ½ 6b Δn² + ½ 6b (Δt ∓ (φ_a + φ_b))² + ½ 2b Δφ²(BuildBlocks の面の項と同じ)
        const int64_t bendDifference = turnHigh - turnLow;
        forces.energyPj = SpringWork(forces.axialMn, stretch) + SpringWork(forces.shearMn, slide + turns) +
                          SpringWork(Flow(2 * b, bendDifference), bendDifference);
        forces.intact = bond.intact;

        return forces;
    }

    // 違う大きさの面の力(T-1067)。項 ½ c p²(p = g · x)の力 c p を勾配で配る: 軸 = Σ c p g_high,軸・ずれ = Σ c p g_high,横。
    // 切り口(細かい側の面の位置。中心の間 L = 1.5 s のうち粗い側の端から s)の曲げの力の回り M は、両端の回り(ずれと曲げの項の
    // φ の勾配 f)の直線の内挿で、どちらの向きでも 6M/s = 2 (f_high − f_low)(同じ大きさの面の 6M/dx = 1.5 (f_high − f_low) と同じ形)。
    // ずれを面の点で運ぶ形(③・④)は、ずれの項の f が両端で同じ(腕 s と s/2)なので M に入らない(面の点で回りが 0)。
    // 軸の項の φ の勾配(粗い側の端を面の位置へずらす剛体の動き)は梁の曲げではないので入れない(④ の 3 つの節の伸びの項も同じ)。
    // 対(④。T-1191): ずれの負の項は細かいセルが受ける力として足し(β が全部なら対の 2 つの面のずれの力は同じ)、伸びの 3 つの節の項は自分の分を足す
    SupportFaceForces SupportLaw::MixedForces(size_t first, uint32_t axis) const {
        const uint32_t along = axis == 0 ? SUPPORT_X : SUPPORT_UP;
        const uint32_t across = axis == 0 ? SUPPORT_UP : SUPPORT_X;
        SupportFaceForces forces;
        int64_t turnLow = 0;
        int64_t turnHigh = 0;
        const bool atFace = m_options.mixedBeam != MixedBeam::FaceOffset &&
                            m_options.mixedBeam != MixedBeam::CenterLine;
        for (size_t i = first; i < first + MIXED_GROUP_TERMS; ++i) {
            const MixedTerm& term = m_mixedTerms[i];
            const int64_t force = TermForce(term);
            forces.axialMn += force * term.gradientHigh[along];
            forces.shearMn += force * term.gradientHigh[across];
            forces.energyPj += SpringWork(force, TermProjection(term));
            if (i == first || (i == first + 1 && atFace))
                continue;  // 軸の項・面の点で運ぶずれの項(面の点で回りが 0)は曲げの力の回りに入らない

            turnLow += force * term.gradientLow[SUPPORT_TURN];
            turnHigh += force * term.gradientHigh[SUPPORT_TURN];
        }

        const MixedGroup& group = m_mixedGroups[first / MIXED_GROUP_TERMS];
        if (group.pair != NO_CELL) {
            const bool fineHigh = m_cellFine[m_mixedTerms[first].high] != 0;
            const MixedPair& pair = m_mixedPairs[group.pair];

            // ずれの負の項(細かいセルが受ける力 → 面の力。細かいセルが −軸の側なら逆)
            const MixedTerm& term = m_pairTerms[pair.pairTerm];
            const int64_t force = TermForce(term);
            const Gradient& gradient = group.upper ? term.gradientHigh : term.gradientLow;
            const int64_t toFace = fineHigh ? 1 : -1;
            forces.axialMn += toFace * force * gradient[along];
            forces.shearMn += toFace * force * gradient[across];
            forces.energyPj -= SpringWork(force, TermProjection(term)) / 2;  // 負の項のエネルギーを 2 つの面で半分ずつ

            // 曲げの差の負の項(T-1207): 自分の細かいセルの回りの力を曲げの項の力に直し(L は −p_U を含む勾配の向きが逆)、
            // 曲げの項と同じ両端の勾配で回りへ(面の点の回り = 曲げの項 + この分)
            const MixedTerm& bendTerm = m_pairTerms[pair.pairTerm + 1];
            const int64_t bendForce = TermForce(bendTerm);
            const int64_t bendShare = group.upper ? -bendForce : bendForce;
            const MixedTerm& faceBend = m_mixedTerms[first + 2];
            turnLow += bendShare * faceBend.gradientLow[SUPPORT_TURN];
            turnHigh += bendShare * faceBend.gradientHigh[SUPPORT_TURN];
            forces.energyPj -= SpringWork(bendForce, TermProjection(bendTerm)) / 2;

            // 伸びの 3 つの節の項(nodes の 0 = 粗いセル・1 = 自分の細かいセル)
            const TripleTerm& triple = m_tripleTerms[pair.firstTriple + (group.upper ? 1 : 0)];
            const int64_t projection = TripleProjection(triple);
            const int64_t tripleForce = Flow(triple.coefficient, projection);
            const Gradient& highGradient = fineHigh ? triple.gradients[1] : triple.gradients[0];
            forces.axialMn += tripleForce * highGradient[along];
            forces.shearMn += tripleForce * highGradient[across];
            forces.energyPj += SpringWork(tripleForce, projection);
        }

        const int64_t bend = 2 * (turnHigh - turnLow);
        forces.lowSideMn = axis == 0 ? forces.axialMn + bend : forces.axialMn - bend;
        forces.highSideMn = axis == 0 ? forces.axialMn - bend : forces.axialMn + bend;
        forces.intact = m_mixedTerms[first].intact;

        return forces;
    }

    SupportFaceForces SupportLaw::FineFaceForces(uint32_t fx, uint32_t fup, uint32_t axis) const {
        const uint32_t fineWidth = 2 * m_scene.width;
        const uint32_t nextX = axis == 0 ? fx + 1 : fx;
        const uint32_t nextUp = axis == 0 ? fup : fup + 1;
        if (!m_mixed || m_options.form != SupportForm::BeamLattice || nextX >= fineWidth ||
            nextUp >= 2 * m_scene.height)
            return {};

        const uint32_t low = m_fineIndex[fx + (size_t)fineWidth * fup];
        const uint32_t high = m_fineIndex[nextX + (size_t)fineWidth * nextUp];
        if (low == NO_CELL && high == NO_CELL) {
            if (nextX / 2 == fx / 2 && nextUp / 2 == fup / 2)
                return {};  // 分けていないセルの中

            return FaceForces(fx / 2, fup / 2, axis);
        }

        if (low != NO_CELL && high != NO_CELL) {
            const uint32_t bondIndex = m_bondAt[(size_t)low * 2 + axis];
            return bondIndex == NO_CELL ? SupportFaceForces{} : BondForces(m_bonds[bondIndex]);
        }

        const uint32_t first = m_mixedFaceAt[(fx + (size_t)fineWidth * fup) * 2 + axis];

        return first == NO_CELL ? SupportFaceForces{} : MixedForces(first, axis);
    }

    SupportLedger SupportLaw::Ledger() const {
        SupportLedger ledger;
        std::vector<int64_t> net(m_cells.size());
        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            net[cell] = -CellLoad(cell);
            ledger.weightMn += CellLoad(cell);
        }

        for (const Bond& bond : m_bonds) {
            const int64_t force = UpForceOnBond(bond);
            net[bond.high] += force;
            net[bond.low] -= force;
        }

        for (const Brace& brace : m_braces) {
            const int64_t force = UpForceOnBrace(brace);
            net[brace.high] += force;
            net[brace.low] -= force;
        }

        for (const std::vector<MixedTerm>* terms : {&m_mixedTerms, &m_pairTerms}) {
            for (const MixedTerm& term : *terms) {
                const int64_t force = UpForceOnTerm(term);
                net[term.high] += force;
                net[term.low] -= force;
            }
        }

        // 3 つの節の項: 節 i へ −c p g_i,上(並進で変わらない項なので 3 つの和は 0)
        for (const TripleTerm& term : m_tripleTerms) {
            const int64_t force = Flow(term.coefficient, TripleProjection(term));
            for (uint32_t i = 0; i < 3; ++i)
                net[term.nodes[i]] -= force * term.gradients[i][SUPPORT_UP];
        }

        for (const Clamp& clamp : m_clamps) {
            const int64_t force = UpForceOnClamp(clamp);
            net[clamp.cell] += force;
            ledger.reactionMn += force;
        }

        for (const int64_t force : net) {
            ledger.netMn += force;
            ledger.worstCellMn = std::max(ledger.worstCellMn, FxAbsU64(force));
        }

        return ledger;
    }

    uint64_t SupportLaw::Hash() const {
        uint64_t hash = 14695981039346656037ull;
        if (Coupled()) {
            for (uint32_t c = 0; c < SUPPORT_COMPONENTS; ++c) {
                for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
                    hash ^= (uint64_t)CellValue(cell, c);
                    hash *= 1099511628211ull;
                }
            }

            // 刻みをまたぐ共役勾配の向きと残差(D-649。持たない時は空)
            for (const std::vector<int64_t>* carried :
                 {&m_blockState.conjugateDirection, &m_blockState.conjugateResidual}) {
                for (const int64_t value : *carried) {
                    hash ^= (uint64_t)value;
                    hash *= 1099511628211ull;
                }
            }

            return hash;
        }

        for (uint32_t c = 0; c < ComponentCount(); ++c) {
            for (const int64_t value : m_states[c].values) {
                hash ^= (uint64_t)value;
                hash *= 1099511628211ull;
            }
        }

        return hash;
    }

}  // namespace bicameral::sim
