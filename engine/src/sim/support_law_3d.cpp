// support_law_3d.cpp — 支えの法則の 3 次元の格子の梁(T-0960。何をするかは support_law_3d.h)。
// 式(セルの一辺 dx・ν = 0・ka = E × dx・b = ka ÷ 6・φ_k = 軸 k の回りの角 × dx ÷ 2。2 次元の support_law.cpp の冒頭と同じ規則を 3 軸に):
//   面(セル a → +軸 n の隣 b。横の向き t は n 以外の 2 つ):
//     伸び   ½ 6b (Δu_n)²
//     ずれ   ½ 6b (Δu_t − [(φ_a + φ_b) × n]_t)²(t ごと。面の点の動き = 中心の動き + 回り × 中心から面まで。ε は Levi-Civita で
//            [(φ) × n]_t = Σ_k ε(k, n, t) φ_k。2 次元の「x の面で −・上の面で +」はこの式の奥行の回りの分)
//     曲げ   ½ 2b (Δφ_k)²(k ≠ n の 2 つ。I = dx⁴/12)
//     ねじれ ½ (7/4) b (Δφ_n)²(G = E/2・J = 7/48 dx⁴。SUPPORT3_TORSION_*。T-1293)
//   固定した面(軸 n・側 s。面の点の形 ADR-0491): 伸び ½ 12b u_n²・ずれ ½ 12b (u_t + s Σ_k ε(k, n, t) φ_k)²・曲げ ½ 4b φ_k²・ねじれ ½ (7/2) b φ_n²
//   段の斜めの棒(辺で接する斜めの隣。面 (a, b) の中で c → c + e_b + s e_a、間の 2 セルの片方だけがある所):
//     ½ q (s Δu_a + Δu_b)² + ½ q (w · (φ_c+ − φ_c))²(w = e_a − s e_b = 段の面の中で棒に直交する軸。q = (b_a + b_b) × 3 × 分子 ÷ (2 × 分母)。
//     伸びは 2 次元の段の棒〔ADR-0415〕を 3 つの座標の面に。面の外の曲げ〔回りの差〕は T-1293: 段の継ぎ目の面の外の曲げを面の梁の
//     ねじれと曲げだけで運ぶと、段の面の外へ曲がる斜めの梁が柔らかすぎた〔(1,0,1) の梁 +11 %・(1,1,1) +21 %〕)
//   角の斜めの棒(T-1293。c → c + (s_x, 1, s_z) が、間の 6 セルのうち 2 つだけを通る 3 歩の階段でつながる所): ½ q_c (s · Δu)²
//     (q_c = (b_x + b_上 + b_奥行) × 2 × 分子 ÷ (3 × 分母)。軸の硬さ 3 q_c は辺の斜めの棒 2 q と同じ ka/12)
// 剛体の動き(並進 3・回り 3)では全部の項が 0(回り ω' = 角 × dx/2 で u = ω' × p・φ = ω'。p = 中心のセルの半分の単位の座標)。
// 一様な歪み(対称 6 つ)では面ごとの力が同じ(つぎはぎの検査。support_3d_test)。延ばし方は 3 次元の剛体の動き(Rigid3DProlongation)。
#include "sim/support_law_3d.h"

#include <algorithm>

#include "common/implicit_conduction.hlsli"
#include "core/aliases.h"

using namespace bicameral::fx;
using namespace bicameral::multires;

namespace bicameral::sim {

    namespace {

        constexpr uint64_t PER_MILLION = 1000000;
        constexpr uint64_t PER_TRILLION = 1000000000000;
        constexpr uint32_t UNIT_SHIFT = 32;         // 係数の Q(2^-32 mN/nm)
        constexpr uint32_t BLOCK_SHIFT = 32;        // 右辺の単位 2^-16 mN(係数 2^-32 × 変位 2^-16 ÷ 2^32)
        constexpr uint32_t BLOCK_FORCE_SHIFT = 16;  // mN → 2^-16 mN
        constexpr uint32_t NO_CELL = UINT32_MAX;

        // Levi-Civita の ε(i, j, k)(0〜2 の添字。巡回 +1・逆 −1・同じ添字 0)
        constexpr int64_t Epsilon(uint32_t i, uint32_t j, uint32_t k) {
            return ((int64_t)i - (int64_t)j) * ((int64_t)j - (int64_t)k) * ((int64_t)k - (int64_t)i) / 2;
        }

        // ka = E × dx(kPa × mm = 10^-6 mN/nm)を 2^-32 mN/nm で(support_law.cpp と同じ式)
        uint64_t BondUnit(uint64_t stiffnessKpa, uint64_t cellMm) {
            const FxU128 product = FxMulU64Full(stiffnessKpa * cellMm, (uint64_t)1 << UNIT_SHIFT);

            return FxDivU128By64(product, PER_MILLION).quotient;
        }

        // セル 1 つの重さ ρ g dx³(10^-12 mN → mN)
        int64_t CellWeight(const SupportScene3D& scene) {
            const uint64_t volume = scene.cellMm * scene.cellMm * scene.cellMm;
            const FxU128 product = FxMulU64Full(scene.densityKgPerM3 * scene.gravityUmPerS2, volume);

            return (int64_t)FxDivU128By64(product, PER_TRILLION).quotient;
        }

        // 係数 × 面の残りの硬さ(Q16。割れていない面は掛けない)
        uint64_t Scaled(uint64_t coefficient, uint32_t intact) {
            if (intact >= SUPPORT_INTACT_FULL)
                return coefficient;

            return FxShiftRightU128(FxMulU64Full(coefficient, intact), 16);
        }

        int64_t Flow(uint64_t coefficient, int64_t difference) {
            return ImFaceFlow(ImWide(coefficient), difference);
        }

        // 3 次元の剛体の動きの延ばし方(子の八分割 = x・上・奥行の bit 0・1・2。Q16): 子の変位 = 親の変位 + ω × r。
        // r = ±dx/2・親の φ = ω × dx なので u_i += ½ Σ ε(i, j, m) φ_j s_m・子の φ = 親の φ ÷ 2
        std::array<EquilibriumBlock, EQ_OCTANTS> Rigid3DProlongation() {
            constexpr int64_t PROLONG_ONE = int64_t{1} << EQ_PROLONG_SHIFT;
            constexpr int64_t PROLONG_HALF = PROLONG_ONE / 2;
            std::array<EquilibriumBlock, EQ_OCTANTS> prolongation{};
            for (uint32_t octant = 0; octant < EQ_OCTANTS; ++octant) {
                std::array<int64_t, SUPPORT3_AXES> side{};
                for (uint32_t axis = 0; axis < SUPPORT3_AXES; ++axis)
                    side[axis] = (octant >> axis & 1) != 0 ? 1 : -1;

                EquilibriumBlock& block = prolongation[octant];
                for (uint32_t i = 0; i < SUPPORT3_AXES; ++i) {
                    block[i * EQ_MAX_COMPONENTS + i] = PROLONG_ONE;
                    block[(SUPPORT3_TURN + i) * EQ_MAX_COMPONENTS + SUPPORT3_TURN + i] = PROLONG_HALF;
                    for (uint32_t j = 0; j < SUPPORT3_AXES; ++j) {
                        int64_t sum = 0;
                        for (uint32_t m = 0; m < SUPPORT3_AXES; ++m)
                            sum += Epsilon(i, j, m) * side[m];

                        block[i * EQ_MAX_COMPONENTS + SUPPORT3_TURN + j] = sum * PROLONG_HALF;
                    }
                }
            }

            return prolongation;
        }

        // ½ c (g · x)²(2 つの節)を面のブロックへ
        void AddFaceTerm(EquilibriumBlockFace& face, uint64_t coefficient,
                         const std::array<int64_t, SUPPORT3_COMPONENTS>& low,
                         const std::array<int64_t, SUPPORT3_COMPONENTS>& high) {
            const auto c = (int64_t)coefficient;
            for (uint32_t row = 0; row < SUPPORT3_COMPONENTS; ++row) {
                for (uint32_t column = 0; column < SUPPORT3_COMPONENTS; ++column) {
                    const uint32_t at = row * EQ_MAX_COMPONENTS + column;
                    face.lowLow[at] += c * low[row] * low[column];
                    face.lowHigh[at] += c * low[row] * high[column];
                    face.highHigh[at] += c * high[row] * high[column];
                }
            }
        }

        std::array<int64_t, 3> Cross(const std::array<int64_t, 3>& a, const std::array<int64_t, 3>& b) {
            return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        }

    }  // namespace

    SupportLaw3D::SupportLaw3D(const SupportScene3D& scene, const SupportOptions& options)
        : m_scene(scene), m_options(options) {
        FX_ASSERT(options.form == SupportForm::BeamLattice && options.solve == SupportSolve::Coupled);
        m_options.blocks.warmStart = m_options.warmStart;
        m_options.blocks.conjugateDirection = m_options.conjugateDirection;
        m_options.blocks.conjugateResidual = m_options.conjugateResidual;
        m_bondSide = BondUnit(m_scene.stiffnessKpa, m_scene.cellMm) / 6;
        const uint64_t upKpa = m_scene.stiffnessUpKpa != 0 ? m_scene.stiffnessUpKpa : m_scene.stiffnessKpa;
        m_bondUp = BondUnit(upKpa, m_scene.cellMm) / 6;
        m_weight = CellWeight(m_scene);
        BuildCells();
        BuildTerms();
        BuildBlocks();
    }

    // --- セル(1 つの大きさ)と重さ + 荷 ---
    void SupportLaw3D::BuildCells() {
        const size_t count = (size_t)m_scene.width * m_scene.height * m_scene.depth;
        m_index.assign(count, NO_CELL);
        for (uint32_t z = 0; z < m_scene.depth; ++z) {
            for (uint32_t up = 0; up < m_scene.height; ++up) {
                for (uint32_t x = 0; x < m_scene.width; ++x) {
                    const size_t at = x + (size_t)m_scene.width * (up + (size_t)m_scene.height * z);
                    if (m_scene.solid[at] == 0)
                        continue;

                    m_index[at] = (uint32_t)m_cells.size();
                    m_cells.push_back({x, up, z});
                    m_loads.push_back(m_weight + (m_scene.loadMn.empty() ? 0 : m_scene.loadMn[at]));
                }
            }
        }
    }

    uint32_t SupportLaw3D::Index(int64_t x, int64_t up, int64_t z) const {
        if (x < 0 || up < 0 || z < 0 || x >= m_scene.width || up >= m_scene.height || z >= m_scene.depth)
            return NO_CELL;

        return m_index[(size_t)x + (size_t)m_scene.width * ((size_t)up + (size_t)m_scene.height * (size_t)z)];
    }

    uint64_t SupportLaw3D::AxisUnit(uint32_t axis) const {
        return axis == SUPPORT3_UP ? m_bondUp : m_bondSide;
    }

    uint32_t SupportLaw3D::Intact(uint32_t cell, uint32_t axis) const {
        const std::vector<uint32_t>& intact = m_scene.intact[axis];
        if (intact.empty())
            return SUPPORT_INTACT_FULL;

        const auto [x, up, z] = m_cells[cell];

        return intact[x + (size_t)m_scene.width * (up + (size_t)m_scene.height * z)];
    }

    // --- 項: 面・固定した面・段の斜めの棒 ---
    void SupportLaw3D::BuildTerms() {
        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            const auto [x, up, z] = m_cells[cell];
            const std::array<int64_t, SUPPORT3_AXES> at = {x, up, z};
            for (uint32_t axis = 0; axis < SUPPORT3_AXES; ++axis) {
                std::array<int64_t, SUPPORT3_AXES> next = at;
                ++next[axis];
                const uint32_t neighbor = Index(next[0], next[1], next[2]);
                if (neighbor != NO_CELL)
                    AddBond(cell, neighbor, axis, Intact(cell, axis));
            }

            const uint8_t clamps = m_scene.clamps[x + (size_t)m_scene.width * (up + (size_t)m_scene.height * z)];
            for (uint32_t axis = 0; axis < SUPPORT3_AXES; ++axis) {
                for (const int32_t side : {-1, 1}) {
                    if ((clamps & Support3Clamp(axis, side)) != 0)
                        AddClamp(cell, axis, side);
                }
            }

            if (m_options.stairBraces)
                AddBraces(cell, x, up, z);
        }
    }

    // 面の 6 つの項(伸び・ずれ 2・曲げ 2・ねじれ)
    void SupportLaw3D::AddBond(uint32_t low, uint32_t high, uint32_t axis, uint32_t intact) {
        if (intact == 0)
            return;

        const uint64_t b = Scaled(AxisUnit(axis), intact);
        Term axial{.low = low, .high = high, .coefficient = 6 * b};
        axial.gradientLow[axis] = -1;
        axial.gradientHigh[axis] = 1;
        m_terms.push_back(axial);

        for (uint32_t across = 0; across < SUPPORT3_AXES; ++across) {
            if (across == axis)
                continue;

            Term shear{.low = low, .high = high, .coefficient = 6 * b};
            shear.gradientLow[across] = -1;
            shear.gradientHigh[across] = 1;
            for (uint32_t turn = 0; turn < SUPPORT3_AXES; ++turn) {
                shear.gradientLow[SUPPORT3_TURN + turn] = -Epsilon(turn, axis, across);
                shear.gradientHigh[SUPPORT3_TURN + turn] = -Epsilon(turn, axis, across);
            }

            m_terms.push_back(shear);
        }

        for (uint32_t turn = 0; turn < SUPPORT3_AXES; ++turn) {
            const uint64_t c = turn == axis ? b * SUPPORT3_TORSION_NUMERATOR / SUPPORT3_TORSION_DENOMINATOR : 2 * b;
            Term bend{.low = low, .high = high, .coefficient = c};
            bend.gradientLow[SUPPORT3_TURN + turn] = -1;
            bend.gradientHigh[SUPPORT3_TURN + turn] = 1;
            m_terms.push_back(bend);
        }
    }

    // 固定した面の 6 つの項(面の点の形。硬さは面の 2 倍 = 中心から壁まで dx/2)
    void SupportLaw3D::AddClamp(uint32_t cell, uint32_t axis, int32_t side) {
        const uint64_t b = AxisUnit(axis);
        FixedTerm axial{.cell = cell, .coefficient = 12 * b};
        axial.gradient[axis] = 1;
        m_fixedTerms.push_back(axial);

        for (uint32_t across = 0; across < SUPPORT3_AXES; ++across) {
            if (across == axis)
                continue;

            FixedTerm shear{.cell = cell, .coefficient = 12 * b};
            shear.gradient[across] = 1;
            for (uint32_t turn = 0; turn < SUPPORT3_AXES; ++turn)
                shear.gradient[SUPPORT3_TURN + turn] = side * Epsilon(turn, axis, across);

            m_fixedTerms.push_back(shear);
        }

        for (uint32_t turn = 0; turn < SUPPORT3_AXES; ++turn) {
            const uint64_t c = turn == axis ? 2 * b * SUPPORT3_TORSION_NUMERATOR / SUPPORT3_TORSION_DENOMINATOR : 4 * b;
            FixedTerm bend{.cell = cell, .coefficient = c};
            bend.gradient[SUPPORT3_TURN + turn] = 1;
            m_fixedTerms.push_back(bend);
        }
    }

    // 段の斜めの棒: 面 (a, b)(a < b)ごとに c → c + e_b + s e_a。間の 2 セル(横 = c + s e_a・上 = c + e_b)の片方だけがある所
    void SupportLaw3D::AddBraces(uint32_t cell, uint32_t x, uint32_t up, uint32_t z) {
        const std::array<int64_t, SUPPORT3_AXES> at = {x, up, z};
        const auto find = [&](std::array<int64_t, SUPPORT3_AXES> p) {
            return Index(p[0], p[1], p[2]);
        };

        for (uint32_t a = 0; a < SUPPORT3_AXES; ++a) {
            for (uint32_t b = a + 1; b < SUPPORT3_AXES; ++b) {
                for (const int32_t across : {-1, 1}) {
                    std::array<int64_t, SUPPORT3_AXES> sidePosition = at;
                    sidePosition[a] += across;
                    std::array<int64_t, SUPPORT3_AXES> abovePosition = at;
                    ++abovePosition[b];
                    std::array<int64_t, SUPPORT3_AXES> diagonalPosition = sidePosition;
                    ++diagonalPosition[b];
                    const uint32_t diagonal = find(diagonalPosition);
                    const bool side = find(sidePosition) != NO_CELL;
                    const bool above = find(abovePosition) != NO_CELL;
                    if (diagonal == NO_CELL || side == above)
                        continue;

                    // 間にあるセルを回る 2 つの面の残りの硬さの小さい方(2 次元の BraceIntact と同じ)
                    std::array<int64_t, SUPPORT3_AXES> lowAcross = across > 0 ? at : sidePosition;
                    uint32_t intact = 0;
                    if (side) {
                        intact = std::min(IntactAt(lowAcross, a), IntactAt(sidePosition, b));
                    } else {
                        ++lowAcross[b];
                        intact = std::min(IntactAt(at, b), IntactAt(lowAcross, a));
                    }

                    if (intact == 0)
                        continue;

                    const uint64_t unit = (AxisUnit(a) + AxisUnit(b)) * 3 * m_options.braceNumerator /
                                          (2 * m_options.braceDenominator);
                    Term brace{.low = cell, .high = diagonal, .coefficient = Scaled(unit, intact)};
                    brace.gradientLow[a] = -across;
                    brace.gradientLow[b] = -1;
                    brace.gradientHigh[a] = across;
                    brace.gradientHigh[b] = 1;
                    m_terms.push_back(brace);

                    // 面の外の曲げ: w = e_a − s e_b の回りの回りの差
                    Term bend{.low = cell, .high = diagonal, .coefficient = brace.coefficient};
                    bend.gradientLow[SUPPORT3_TURN + a] = -1;
                    bend.gradientLow[SUPPORT3_TURN + b] = across;
                    bend.gradientHigh[SUPPORT3_TURN + a] = 1;
                    bend.gradientHigh[SUPPORT3_TURN + b] = -across;
                    m_terms.push_back(bend);
                    ++m_braceCount;
                }
            }
        }

        AddCornerBraces(cell, at);
    }

    // 角の斜めの棒: c → c + (s_x, 1, s_z) が、間の 6 セルのうち 2 つだけを通る 3 歩の階段(軸の順 o0 → o1 → o2)でつながる所
    void SupportLaw3D::AddCornerBraces(uint32_t cell, const std::array<int64_t, SUPPORT3_AXES>& at) {
        constexpr std::array<std::array<uint32_t, 3>, 6> ORDERS = {
            {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};
        for (const int32_t acrossX : {-1, 1}) {
            for (const int32_t acrossZ : {-1, 1}) {
                const std::array<int64_t, SUPPORT3_AXES> step = {acrossX, 1, acrossZ};
                const uint32_t diagonal = Index(at[0] + step[0], at[1] + step[1], at[2] + step[2]);
                if (diagonal == NO_CELL)
                    continue;

                // 間の 6 セルのうちあるものの数(2 つだけ)
                uint32_t present = 0;
                for (uint32_t mask = 1; mask < 7; ++mask) {
                    present += Index(at[0] + ((mask & 1) != 0 ? step[0] : 0), at[1] + ((mask & 2) != 0 ? step[1] : 0),
                                     at[2] + ((mask & 4) != 0 ? step[2] : 0)) != NO_CELL
                                   ? 1
                                   : 0;
                }

                if (present != 2)
                    continue;

                for (const std::array<uint32_t, 3>& order : ORDERS) {
                    std::array<int64_t, SUPPORT3_AXES> first = at;
                    first[order[0]] += step[order[0]];
                    std::array<int64_t, SUPPORT3_AXES> second = first;
                    second[order[1]] += step[order[1]];
                    if (Index(first[0], first[1], first[2]) == NO_CELL ||
                        Index(second[0], second[1], second[2]) == NO_CELL)
                        continue;

                    // 道の 3 つの面の残りの硬さの小さい方
                    const auto faceIntact = [&](std::array<int64_t, SUPPORT3_AXES> from, uint32_t axis) {
                        if (step[axis] < 0)
                            from[axis] -= 1;

                        return IntactAt(from, axis);
                    };
                    const uint32_t intact = std::min(
                        {faceIntact(at, order[0]), faceIntact(first, order[1]), faceIntact(second, order[2])});
                    if (intact == 0)
                        break;

                    const uint64_t unit = (AxisUnit(0) + AxisUnit(1) + AxisUnit(2)) * 2 * m_options.braceNumerator /
                                          (3 * m_options.braceDenominator);
                    Term brace{.low = cell, .high = diagonal, .coefficient = Scaled(unit, intact)};
                    for (uint32_t axis = 0; axis < SUPPORT3_AXES; ++axis) {
                        brace.gradientLow[axis] = -step[axis];
                        brace.gradientHigh[axis] = step[axis];
                    }

                    m_terms.push_back(brace);
                    ++m_cornerBraceCount;
                    break;
                }
            }
        }
    }

    // p と +軸の隣の間の面の残りの硬さ(p の位置で読む)
    uint32_t SupportLaw3D::IntactAt(const std::array<int64_t, SUPPORT3_AXES>& p, uint32_t axis) const {
        const std::vector<uint32_t>& intact = m_scene.intact[axis];
        if (intact.empty())
            return SUPPORT_INTACT_FULL;

        return intact[(size_t)p[0] + (size_t)m_scene.width * ((size_t)p[1] + (size_t)m_scene.height * (size_t)p[2])];
    }

    // --- 成分を持つ節の系(k = 6): 同じ 2 節の続いた項は 1 つの面へ ---
    void SupportLaw3D::BuildBlocks() {
        EquilibriumBlockSystem& system = m_blockSystem;
        system.components = SUPPORT3_COMPONENTS;
        system.coefficientShift = BLOCK_SHIFT;
        system.prolongation = Rigid3DProlongation();
        for (const auto& [x, up, z] : m_cells)
            system.nodes.push_back({.level = 0, .x = x, .y = up, .z = z});

        for (const Term& term : m_terms) {
            const bool same = !system.faces.empty() && system.faces.back().low == term.low &&
                              system.faces.back().high == term.high;
            if (!same)
                system.faces.push_back({.low = term.low, .high = term.high});

            AddFaceTerm(system.faces.back(), term.coefficient, term.gradientLow, term.gradientHigh);
        }

        for (const FixedTerm& term : m_fixedTerms) {
            const bool same = !system.fixedFaces.empty() && system.fixedFaces.back().node == term.cell;
            if (!same)
                system.fixedFaces.push_back({.node = term.cell});

            EquilibriumBlockFixed& fixed = system.fixedFaces.back();
            const auto c = (int64_t)term.coefficient;
            for (uint32_t row = 0; row < SUPPORT3_COMPONENTS; ++row) {
                for (uint32_t column = 0; column < SUPPORT3_COMPONENTS; ++column)
                    fixed.block[row * EQ_MAX_COMPONENTS + column] += c * term.gradient[row] * term.gradient[column];
            }
        }

        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            system.sources.push_back(
                {.node = cell, .component = SUPPORT3_UP, .value = -(m_loads[cell] << BLOCK_FORCE_SHIFT)});
        }

        m_blockGrid = BuildEquilibriumBlockGrid(system);
    }

    void SupportLaw3D::Step() {
        const EquilibriumBlockResult result = SolveEquilibriumBlocks(m_blockSystem, m_blockGrid, m_options.blocks,
                                                                     m_blockState);
        m_lastNodeUpdates = result.nodeUpdates;
    }

    // --- 読む ---
    int64_t SupportLaw3D::CellValue(uint32_t cell, uint32_t component) const {
        const size_t at = (size_t)cell * SUPPORT3_COMPONENTS + component;

        return at >= m_blockState.values.size() ? 0 : m_blockState.values[at];
    }

    int64_t SupportLaw3D::Value(uint32_t x, uint32_t up, uint32_t z, uint32_t component) const {
        const uint32_t cell = Index(x, up, z);

        return cell == NO_CELL || component >= SUPPORT3_COMPONENTS ? 0 : CellValue(cell, component);
    }

    std::array<int64_t, SUPPORT3_AXES> SupportLaw3D::Position(uint32_t cell) const {
        const auto [x, up, z] = m_cells[cell];

        return {2 * (int64_t)x + 1, 2 * (int64_t)up + 1, 2 * (int64_t)z + 1};
    }

    // 帳簿: 項ごとの力 f = c (g · x) を節 i の成分 k へ −f g_ik(変位の成分 = 力・回りの成分 = 回りの力 ÷ dx/2)。
    // 原点の回りの回りの力は p × 力 + 回りの力(p はセルの半分の単位)。剛体の回りで変わらない項なので、項ごとの和は整数で 0
    SupportLedger3D SupportLaw3D::Ledger() const {
        SupportLedger3D ledger;
        std::vector<std::array<int64_t, SUPPORT3_COMPONENTS>> net(m_cells.size());
        const auto turnOf = [&](uint32_t cell, const std::array<int64_t, SUPPORT3_COMPONENTS>& part) {
            const std::array<int64_t, 3> force = {part[0], part[1], part[2]};
            std::array<int64_t, 3> turn = Cross(Position(cell), force);
            for (uint32_t axis = 0; axis < SUPPORT3_AXES; ++axis)
                turn[axis] += part[SUPPORT3_TURN + axis];

            return turn;
        };

        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            std::array<int64_t, SUPPORT3_COMPONENTS> load{};
            load[SUPPORT3_UP] = -m_loads[cell];
            net[cell][SUPPORT3_UP] = load[SUPPORT3_UP];
            ledger.loadMn[SUPPORT3_UP] += load[SUPPORT3_UP];
            const std::array<int64_t, 3> turn = turnOf(cell, load);
            for (uint32_t axis = 0; axis < SUPPORT3_AXES; ++axis)
                ledger.loadTurn[axis] += turn[axis];
        }

        for (const Term& term : m_terms) {
            int64_t projection = 0;
            for (uint32_t c = 0; c < SUPPORT3_COMPONENTS; ++c)
                projection += term.gradientLow[c] * CellValue(term.low, c) +
                              term.gradientHigh[c] * CellValue(term.high, c);

            const int64_t force = Flow(term.coefficient, projection);
            for (uint32_t c = 0; c < SUPPORT3_COMPONENTS; ++c) {
                net[term.low][c] -= force * term.gradientLow[c];
                net[term.high][c] -= force * term.gradientHigh[c];
            }
        }

        for (const FixedTerm& term : m_fixedTerms) {
            int64_t projection = 0;
            for (uint32_t c = 0; c < SUPPORT3_COMPONENTS; ++c)
                projection += term.gradient[c] * CellValue(term.cell, c);

            const int64_t force = Flow(term.coefficient, projection);
            std::array<int64_t, SUPPORT3_COMPONENTS> part{};
            for (uint32_t c = 0; c < SUPPORT3_COMPONENTS; ++c) {
                part[c] = -force * term.gradient[c];
                net[term.cell][c] += part[c];
            }

            for (uint32_t axis = 0; axis < SUPPORT3_AXES; ++axis)
                ledger.reactionMn[axis] += part[axis];

            const std::array<int64_t, 3> turn = turnOf(term.cell, part);
            for (uint32_t axis = 0; axis < SUPPORT3_AXES; ++axis)
                ledger.reactionTurn[axis] += turn[axis];
        }

        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            const std::array<int64_t, 3> turn = turnOf(cell, net[cell]);
            for (uint32_t axis = 0; axis < SUPPORT3_AXES; ++axis) {
                ledger.netMn[axis] += net[cell][axis];
                ledger.netTurn[axis] += turn[axis];
                ledger.worstCellMn = std::max(ledger.worstCellMn, FxAbsU64(net[cell][axis]));
                ledger.worstTurnMn = std::max(ledger.worstTurnMn, FxAbsU64(net[cell][SUPPORT3_TURN + axis]));
            }
        }

        return ledger;
    }

    uint64_t SupportLaw3D::Hash() const {
        uint64_t hash = 14695981039346656037ull;
        for (const std::vector<int64_t>* values :
             {&m_blockState.values, &m_blockState.conjugateDirection, &m_blockState.conjugateResidual}) {
            for (const int64_t value : *values) {
                hash ^= (uint64_t)value;
                hash *= 1099511628211ull;
            }
        }

        return hash;
    }

}  // namespace bicameral::sim
