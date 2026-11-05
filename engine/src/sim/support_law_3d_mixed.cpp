// support_law_3d_mixed.cpp — 3 次元の格子の梁(sim/support_law_3d)の違う大きさのセル(T-1294。何をするかは support_law_3d.h)。
// 式(粗いセル C の一辺 2s・細かいセル F の一辺 s・φ = 角 × そのセルの半分〔φ_C = 角 × s・φ_F = 角 × s/2〕・b = C の面の b = E × 2s ÷ 6):
//   細かいセルどうし: 同じ形の面・固定した面・段の棒(辺と角)を細かいセルの b(一辺 s の ka ÷ 6)で(support_law_3d.cpp の冒頭の式のまま)
//   違う大きさの面(C の面の法線 n・側 ν = ±1〔F が C の +n の側なら +1〕・面の中の軸 p < q・面に並ぶ 4 つの F_k の位置 σ_p, σ_q = ±1):
//     面の点(F の面の中心)の相対の動きの 2 倍 G_k = 2 (u_F + φ_F × ρ_F) − 2 (u_C + φ_C × ρ_C)
//       (ρ = 面の点の位置 ÷ セルの半分: ρ_F = −ν e_n・ρ_C = ν e_n + (σ_p e_p + σ_q e_q) ÷ 2。C の側は回り φ_C,n の分〔面の中のねじれ ω_n r_q〕を持つ)
//     ずれ(t = p, q): ½ (b/8) (Σ_k G_k,t)²(4 つの平均のずれだけを C が受ける。面の点のばね 2b = E s² ÷ 1.5 s の 4 つの和)
//     伸び(k ごと):   ½ (b/288) (12 G_k,n − ν (σ_p Σ_l G_l,p + σ_q Σ_l G_l,q))²(4 つの平均のずれの歪みを引いて読む = 2 次元の 3 つの節の項)
//     回り(軸 i):     ½ (c_i/4) (Σ_k (2 φ_F,k,i − φ_C,i))²(c = 曲げ b/6 = E I ÷ 1.5 s・ねじれ 7b/48 = G J ÷ 1.5 s。4 つの平均の回りの差だけ = ADR-0501)
//   4 つの平均は、2 次元の「対の平均で C につなぐ」(ADR-0485・ADR-0501)の 3 次元(ADR-0519): 面ごとの正の項 + 対 6 組(辺の 4 組 + 対角の 2 組)の
//   −½ (k/4) (G_k − G_l)² と同じ。辺の 4 組だけに 2 次元の直列の硬さ k/2 を置くと、4 つの σ_p σ_q の形(面の中のねじれ)を 2 回引いて負になる
//   (系が正でなくなる)。奥行が平らな動き(2 次元の場面を奥行 2 つの細かいセルで)では 2 次元の対と同じ式になる。
//   違う大きさのセルが絡む段の棒(ADR-0504 ①): 細かい格子で段を探す(分けていないセルは自分の 8 つの場所を持つ)。粗いセルの端は段の角の側の
//     場所の時だけ。端は中心(粗いセルは、細かいセルの中心と同じ第 3 の軸の高さ = 中心から ± s/2 の点。剛体の動き)・硬さ = 細かいセルの棒 ×
//     細かい中心の間 ÷ 中心の間(片方の端が粗いセル 2/3・両端 1/2)。面の外の曲げ(T-1293)も同じ比で sθ(= 2 φ_F・φ_C)の差に
//   形 A(SupportOptions::mixedPairMaterial = NeighborStrain。ADR-0509・T-1279 §2。既定は None): 4 つの辺の対(L・U が面の中の軸 p に並ぶ・
//     もう 1 つの軸 q の位置 σ_q)ごとに、面の点の p の相対の動き P と、C の p の向きの隣 N(両側。固定した面の側は壁)から読む C の面の高さ
//     (r = ν s e_n + σ_q s/2 e_q)の繊維 f = u_p + (ω × r)_p の歪み m = d (f_N − f_C) ÷ 2(壁は −d f_C)の差に ½ (K_q ÷ 読める側の数) (P − m)²。
//     K_q = 対の外した硬さ K(4 つの平均の形で辺の対が外す分 = 面の点のばね 2b の直列 b)と C の材料 E s = 3 b_p の直列。
//     ねじれの分(ω_n r_q)も同じ r で読むので、一様な歪み・回り・一様な曲げ・ねじれで P = m(つぎはぎの検査を壊さない)。曲げの形 B は 3 次元に入れない
//   違う大きさのセルが絡む角の斜めの棒(3 歩の階段)は置かない(T-1294 の残り。細かいセルどうしと粗いセルどうしだけ)
// 剛体の動きでは全部の項が 0(ρ は面の点の位置そのもの)。一様な歪みでは違う大きさの面の力が全部細かい場面と同じ(support_3d_test)。
#include <algorithm>
#include <map>

#include "common/implicit_conduction.hlsli"
#include "core/aliases.h"
#include "sim/support_law_3d.h"

using namespace bicameral::fx;

namespace bicameral::sim {

    namespace {

        constexpr uint32_t NO_CELL = UINT32_MAX;
        constexpr uint32_t SPLIT_CELL = UINT32_MAX - 1;
        constexpr uint32_t QUAD = 4;  // 粗いセルの面に並ぶ細かいセル

        constexpr int64_t Epsilon(uint32_t i, uint32_t j, uint32_t k) {
            return ((int64_t)i - (int64_t)j) * ((int64_t)j - (int64_t)k) * ((int64_t)k - (int64_t)i) / 2;
        }

        // 2 つのばねを直列にした硬さ a b ÷ (a + b)(切り捨て。support_law.cpp と同じ)
        uint64_t SeriesCoefficient(uint64_t a, uint64_t b) {
            if (a == 0 || b == 0)
                return 0;

            return FxDivU128By64(FxMulU64Full(a, b), a + b).quotient;
        }

        uint64_t Scaled(uint64_t coefficient, uint32_t intact) {
            if (intact >= SUPPORT_INTACT_FULL)
                return coefficient;

            return FxShiftRightU128(FxMulU64Full(coefficient, intact), 16);
        }

        // ρ(面の点 ÷ セルの半分。2 倍の整数)の点の動きの 2 倍の勾配: 行 i = 2 u_i + Σ ε(i, j, k) φ_j ρ2_k
        using PointGradient = std::array<std::array<int64_t, SUPPORT3_COMPONENTS>, SUPPORT3_AXES>;
        PointGradient PointOf(const std::array<int64_t, SUPPORT3_AXES>& rho2, int64_t sign) {
            PointGradient gradient{};
            for (uint32_t i = 0; i < SUPPORT3_AXES; ++i) {
                gradient[i][i] = 2 * sign;
                for (uint32_t j = 0; j < SUPPORT3_AXES; ++j) {
                    int64_t sum = 0;
                    for (uint32_t k = 0; k < SUPPORT3_AXES; ++k)
                        sum += Epsilon(i, j, k) * rho2[k];

                    gradient[i][SUPPORT3_TURN + j] = sign * sum;
                }
            }

            return gradient;
        }

    }  // namespace

    // --- セル: 分けたセルは 2 × 2 × 2 の細かいセル(細かい格子の座標)。荷は 8 つに分け、割り切れない分は最初の細かいセルへ ---
    void SupportLaw3D::BuildMixedCells() {
        const size_t count = (size_t)m_scene.width * m_scene.height * m_scene.depth;
        const size_t fineWidth = 2 * (size_t)m_scene.width;
        const size_t fineHeight = 2 * (size_t)m_scene.height;
        m_index.assign(count, NO_CELL);
        m_fineIndex.assign(count * 8, NO_CELL);
        for (uint32_t z = 0; z < m_scene.depth; ++z) {
            for (uint32_t up = 0; up < m_scene.height; ++up) {
                for (uint32_t x = 0; x < m_scene.width; ++x) {
                    const size_t at = x + (size_t)m_scene.width * (up + (size_t)m_scene.height * z);
                    if (m_scene.solid[at] == 0)
                        continue;

                    const int64_t load = m_scene.loadMn.empty() ? 0 : m_scene.loadMn[at];
                    if (m_scene.fine[at] == 0) {
                        m_index[at] = (uint32_t)m_cells.size();
                        m_cells.push_back({x, up, z});
                        m_cellFine.push_back(0);
                        m_loads.push_back(m_weight + load);
                        continue;
                    }

                    m_index[at] = SPLIT_CELL;
                    for (uint32_t part = 0; part < 8; ++part) {
                        const uint32_t fx = 2 * x + (part & 1);
                        const uint32_t fup = 2 * up + (part >> 1 & 1);
                        const uint32_t fz = 2 * z + (part >> 2 & 1);
                        m_fineIndex[fx + fineWidth * (fup + fineHeight * fz)] = (uint32_t)m_cells.size();
                        m_cells.push_back({fx, fup, fz});
                        m_cellFine.push_back(1);
                        m_loads.push_back(m_fineWeight + (part == 0 ? load - 7 * (load / 8) : load / 8));
                    }
                }
            }
        }
    }

    // --- 項: 違う大きさの面(粗いセルから分けた隣を見る)・細かいセルの面と固定した面・段の棒 ---
    void SupportLaw3D::BuildMixedTerms() {
        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            if (m_cellFine[cell] != 0)
                continue;

            const auto [x, up, z] = m_cells[cell];
            const std::array<int64_t, SUPPORT3_AXES> at = {x, up, z};
            for (uint32_t axis = 0; axis < SUPPORT3_AXES; ++axis) {
                for (const int32_t side : {-1, 1}) {
                    std::array<int64_t, SUPPORT3_AXES> next = at;
                    next[axis] += side;
                    if (Index(next[0], next[1], next[2]) != SPLIT_CELL)
                        continue;

                    const uint32_t intact = side > 0 ? IntactAt(at, axis) : IntactAt(next, axis);
                    if (intact != 0)
                        AddMixedFace(cell, {(uint32_t)next[0], (uint32_t)next[1], (uint32_t)next[2]}, axis, side,
                                     intact);
                }
            }
        }

        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            if (m_cellFine[cell] != 0)
                AddFineBonds(cell);
        }

        if (!m_options.stairBraces)
            return;

        AddFineBraces();
        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            if (m_cellFine[cell] == 0)
                continue;

            const auto [fx, fup, fz] = m_cells[cell];
            AddCornerBraces(cell, {fx, fup, fz}, true);
        }
    }

    // 細かいセルの +軸の面(隣が細かいセルの時だけ。粗いセルとの間は AddMixedFace)と固定した面(向きの側の細かいセルへ)
    void SupportLaw3D::AddFineBonds(uint32_t cell) {
        const auto [fx, fup, fz] = m_cells[cell];
        const std::array<int64_t, SUPPORT3_AXES> at = {fx, fup, fz};
        const auto unitOf = [&](uint32_t axis) {
            return axis == SUPPORT3_UP ? m_fineBondUp : m_fineBondSide;
        };

        for (uint32_t axis = 0; axis < SUPPORT3_AXES; ++axis) {
            std::array<int64_t, SUPPORT3_AXES> next = at;
            ++next[axis];
            const uint32_t neighbor = Occupant(next, true);
            if (neighbor == NO_CELL || m_cellFine[neighbor] == 0)
                continue;

            const uint32_t intact = FaceIntactAt(at, axis, true);
            if (intact != 0)
                AddBond(cell, neighbor, axis, Scaled(unitOf(axis), intact));
        }

        const uint8_t
            clamps = m_scene.clamps[fx / 2 + (size_t)m_scene.width * (fup / 2 + (size_t)m_scene.height * (fz / 2))];
        for (uint32_t axis = 0; axis < SUPPORT3_AXES; ++axis) {
            for (const int32_t side : {-1, 1}) {
                const bool onSide = (at[axis] & 1) == (side > 0 ? 1 : 0);
                if ((clamps & Support3Clamp(axis, side)) != 0 && onSide)
                    AddClamp(cell, axis, side, unitOf(axis));
            }
        }
    }

    // 違う大きさの面(冒頭の式): 粗いセル coarse の −/+ 軸の側の分けたセル split に並ぶ 4 つの細かいセル
    void SupportLaw3D::AddMixedFace(uint32_t coarse, const std::array<uint32_t, SUPPORT3_AXES>& split, uint32_t axis,
                                    int32_t side, uint32_t intact) {
        const uint64_t b = Scaled(AxisUnit(axis), intact);
        const uint32_t p = axis == 0 ? 1 : 0;
        const uint32_t q = axis == 2 ? 1 : 2;
        const size_t fineWidth = 2 * (size_t)m_scene.width;
        const size_t fineHeight = 2 * (size_t)m_scene.height;

        // --- 4 つの面の点の相対の動き G_k の勾配(粗いセルの分と細かいセルの分)---
        std::array<uint32_t, QUAD> fines{};
        std::array<std::array<int64_t, 2>, QUAD> sigma{};
        std::array<PointGradient, QUAD> coarseOf{};
        std::array<PointGradient, QUAD> fineOf{};
        for (uint32_t k = 0; k < QUAD; ++k) {
            sigma[k] = {(k & 1) != 0 ? 1 : -1, (k & 2) != 0 ? 1 : -1};
            std::array<size_t, SUPPORT3_AXES> f{};
            f[axis] = 2 * (size_t)split[axis] + (side > 0 ? 0 : 1);
            f[p] = 2 * (size_t)split[p] + (sigma[k][0] > 0 ? 1 : 0);
            f[q] = 2 * (size_t)split[q] + (sigma[k][1] > 0 ? 1 : 0);
            fines[k] = m_fineIndex[f[0] + fineWidth * (f[1] + fineHeight * f[2])];
            FX_ASSERT(fines[k] != NO_CELL);

            std::array<int64_t, SUPPORT3_AXES> rhoFine{};
            rhoFine[axis] = -2 * side;
            std::array<int64_t, SUPPORT3_AXES> rhoCoarse{};
            rhoCoarse[axis] = 2 * side;
            rhoCoarse[p] = sigma[k][0];
            rhoCoarse[q] = sigma[k][1];
            fineOf[k] = PointOf(rhoFine, 1);
            coarseOf[k] = PointOf(rhoCoarse, -1);
        }

        const auto term = [&](uint64_t coefficient) {
            MultiTerm each{.count = 1 + QUAD, .coefficient = coefficient};
            each.nodes[0] = coarse;
            for (uint32_t k = 0; k < QUAD; ++k)
                each.nodes[1 + k] = fines[k];

            return each;
        };

        // --- ずれ: 4 つの和 ---
        for (const uint32_t across : {p, q}) {
            MultiTerm shear = term(b / 8);
            for (uint32_t k = 0; k < QUAD; ++k) {
                for (uint32_t c = 0; c < SUPPORT3_COMPONENTS; ++c) {
                    shear.gradients[0][c] += coarseOf[k][across][c];
                    shear.gradients[1 + k][c] = fineOf[k][across][c];
                }
            }

            AddMultiTerm(shear);
        }

        // --- 形 A: 辺の対 4 組(p の向きの対 2 つ・q の向きの対 2 つ。k = σ_p の bit 0 + σ_q の bit 1)---
        if (m_options.mixedPairMaterial == MixedPairMaterial::NeighborStrain ||
            m_options.mixedPairMaterial == MixedPairMaterial::NeighborStrainBend) {
            for (const int64_t height : {-1, 1}) {
                const uint32_t row = height > 0 ? 2 : 0;
                AddStrainTerms(coarse, fines[row], fines[row + 1], axis, side, p, height, b);
                const uint32_t column = height > 0 ? 1 : 0;
                AddStrainTerms(coarse, fines[column], fines[column + 2], axis, side, q, height, b);
            }
        }

        // --- 伸び: k ごとに 4 つの平均のずれの歪みを引く ---
        for (uint32_t k = 0; k < QUAD; ++k) {
            MultiTerm axial = term(b / 288);
            for (uint32_t c = 0; c < SUPPORT3_COMPONENTS; ++c)
                axial.gradients[0][c] = 12 * coarseOf[k][axis][c];

            axial.gradients[1 + k] = {};
            for (uint32_t c = 0; c < SUPPORT3_COMPONENTS; ++c)
                axial.gradients[1 + k][c] = 12 * fineOf[k][axis][c];

            for (uint32_t l = 0; l < QUAD; ++l) {
                for (uint32_t c = 0; c < SUPPORT3_COMPONENTS; ++c) {
                    const int64_t coarseMix = sigma[k][0] * coarseOf[l][p][c] + sigma[k][1] * coarseOf[l][q][c];
                    const int64_t fineMix = sigma[k][0] * fineOf[l][p][c] + sigma[k][1] * fineOf[l][q][c];
                    axial.gradients[0][c] -= side * coarseMix;
                    axial.gradients[1 + l][c] -= side * fineMix;
                }
            }

            AddMultiTerm(axial);
        }

        // --- 回り: 4 つの平均の回りの差(曲げ 2・ねじれ)---
        for (uint32_t turn = 0; turn < SUPPORT3_AXES; ++turn) {
            const uint64_t c = turn == axis ? b * SUPPORT3_TORSION_NUMERATOR / (SUPPORT3_TORSION_DENOMINATOR * 48)
                                            : b / 24;
            MultiTerm bend = term(c);
            bend.gradients[0][SUPPORT3_TURN + turn] = -(int64_t)QUAD;
            for (uint32_t k = 0; k < QUAD; ++k)
                bend.gradients[1 + k][SUPPORT3_TURN + turn] = 2;

            AddMultiTerm(bend);
        }

        ++m_mixedFaceCount;
    }

    // 形 A の項(冒頭)。4 倍の勾配 4 (P − m) = 2 (G_U − G_L)_p − d (F_N − F_C)(F = 繊維の 2 倍。壁は + 2d F_C)・係数 K_q ÷ (16 × 読める側の数)
    void SupportLaw3D::AddStrainTerms(uint32_t coarse, uint32_t lower, uint32_t upper, uint32_t axis, int32_t side,
                                      uint32_t across, int64_t height, uint64_t pairStiffness) {
        const uint32_t other = 3 - axis - across;
        std::array<int64_t, SUPPORT3_AXES> rhoFine{};
        rhoFine[axis] = -2 * side;
        const PointGradient fine = PointOf(rhoFine, 1);
        std::array<int64_t, SUPPORT3_AXES> rhoFiber{};
        rhoFiber[axis] = 2 * side;
        rhoFiber[other] = height;
        const PointGradient fiber = PointOf(rhoFiber, 1);
        const uint64_t keep = SeriesCoefficient(pairStiffness, 3 * AxisUnit(across));

        // --- 読める側(固定した面 → 壁・隣の粗いセル → 中心の差。分けたセル・無い・割れた面の側は読まない)---
        struct Side {
            int64_t direction = 0;
            uint32_t neighbor = NO_CELL;  // NO_CELL = 固定した面
            uint32_t intact = SUPPORT_INTACT_FULL;
        };
        std::array<Side, 2> sides{};
        uint32_t count = 0;
        const auto [x, up, z] = m_cells[coarse];
        const std::array<int64_t, SUPPORT3_AXES> at = {x, up, z};
        const uint8_t clamps = m_scene.clamps[x + (size_t)m_scene.width * (up + (size_t)m_scene.height * z)];
        for (const int32_t direction : {-1, 1}) {
            if ((clamps & Support3Clamp(across, direction)) != 0) {
                sides[count++] = {.direction = direction};
                continue;
            }

            std::array<int64_t, SUPPORT3_AXES> next = at;
            next[across] += direction;
            const uint32_t neighbor = Index(next[0], next[1], next[2]);
            if (neighbor >= SPLIT_CELL)
                continue;

            const uint32_t intact = direction > 0 ? IntactAt(at, across) : IntactAt(next, across);
            if (intact != 0)
                sides[count++] = {.direction = direction, .neighbor = neighbor, .intact = intact};
        }

        // --- 項(側ごと)---
        for (uint32_t i = 0; i < count; ++i) {
            const Side& each = sides[i];
            const bool clamped = each.neighbor == NO_CELL;
            MultiTerm term{.count = clamped ? 3u : 4u, .coefficient = Scaled(keep / (16 * count), each.intact)};
            term.nodes = {coarse, lower, upper, clamped ? coarse : each.neighbor, 0};
            for (uint32_t c = 0; c < SUPPORT3_COMPONENTS; ++c) {
                term.gradients[1][c] = -2 * fine[across][c];
                term.gradients[2][c] = 2 * fine[across][c];
                term.gradients[0][c] = (clamped ? 2 : 1) * each.direction * fiber[across][c];
                if (!clamped)
                    term.gradients[3][c] = -each.direction * fiber[across][c];
            }

            AddMultiTerm(term);
        }
    }

    void SupportLaw3D::AddMultiTerm(const MultiTerm& term) {
        if (term.coefficient != 0)
            m_multiTerms.push_back(term);
    }

    // --- 段の斜めの棒(細かい格子。冒頭)---
    void SupportLaw3D::AddFineBraces() {
        for (int64_t z = 0; z < 2 * (int64_t)m_scene.depth; ++z) {
            for (int64_t up = 0; up < 2 * (int64_t)m_scene.height; ++up) {
                for (int64_t x = 0; x < 2 * (int64_t)m_scene.width; ++x) {
                    for (uint32_t a = 0; a < SUPPORT3_AXES; ++a) {
                        for (uint32_t b = a + 1; b < SUPPORT3_AXES; ++b) {
                            AddFineBrace({x, up, z}, a, b, -1);
                            AddFineBrace({x, up, z}, a, b, 1);
                        }
                    }
                }
            }
        }
    }

    // 細かい場所 at → at + e_b + s e_a の段。細かいセルどうし・違う大きさのセルの間・粗いセルどうしで間の場所が分けたセルの所
    void SupportLaw3D::AddFineBrace(const std::array<int64_t, SUPPORT3_AXES>& at, uint32_t a, uint32_t b,
                                    int32_t across) {
        const uint32_t low = Occupant(at, true);
        if (low == NO_CELL)
            return;

        std::array<int64_t, SUPPORT3_AXES> sidePosition = at;
        sidePosition[a] += across;
        std::array<int64_t, SUPPORT3_AXES> abovePosition = at;
        ++abovePosition[b];
        std::array<int64_t, SUPPORT3_AXES> diagonalPosition = sidePosition;
        ++diagonalPosition[b];
        const uint32_t high = Occupant(diagonalPosition, true);
        const bool side = Occupant(sidePosition, true) != NO_CELL;
        if (high == NO_CELL || low == high || side == (Occupant(abovePosition, true) != NO_CELL))
            return;

        const bool coarseLow = m_cellFine[low] == 0;
        const bool coarseHigh = m_cellFine[high] == 0;
        const auto split = [&](const std::array<int64_t, SUPPORT3_AXES>& p) {
            if (p[0] < 0 || p[1] < 0 || p[2] < 0)
                return false;

            return Index(p[0] / 2, p[1] / 2, p[2] / 2) == SPLIT_CELL;
        };
        if (coarseLow && coarseHigh && !split(sidePosition) && !split(abovePosition))
            return;  // 粗いセルどうしで間も粗いセル(AddBraces)

        if ((coarseLow || coarseHigh) && m_options.mixedBrace == MixedBrace::None)
            return;

        // 角の場所: 下の粗いセルは上の段・段の側の列、上の粗いセルは下の段・段の側の列
        if (coarseLow && ((at[b] & 1) == 0 || ((at[a] & 1) != 0) != (across > 0)))
            return;

        if (coarseHigh && ((diagonalPosition[b] & 1) != 0 || ((diagonalPosition[a] & 1) == 0) != (across > 0)))
            return;

        std::array<int64_t, SUPPORT3_AXES> lowAcross = across > 0 ? at : sidePosition;
        uint32_t intact = 0;
        if (side) {
            intact = std::min(FaceIntactAt(lowAcross, a, true), FaceIntactAt(sidePosition, b, true));
        } else {
            ++lowAcross[b];
            intact = std::min(FaceIntactAt(at, b, true), FaceIntactAt(lowAcross, a, true));
        }

        if (intact == 0)
            return;

        const auto unitOf = [&](uint32_t axis) {
            return axis == SUPPORT3_UP ? m_fineBondUp : m_fineBondSide;
        };
        const uint64_t fineUnit = (unitOf(a) + unitOf(b)) * 3 * m_options.braceNumerator /
                                  (2 * m_options.braceDenominator);
        Term brace{.low = low, .high = high};
        Term bend{.low = low, .high = high};
        if (!coarseLow && !coarseHigh) {
            // 細かいセルどうし: support_law_3d.cpp の AddBraces と同じ式
            brace.coefficient = Scaled(fineUnit, intact);
            brace.gradientLow[a] = -across;
            brace.gradientLow[b] = -1;
            brace.gradientHigh[a] = across;
            brace.gradientHigh[b] = 1;
            bend.coefficient = brace.coefficient;
            bend.gradientLow[SUPPORT3_TURN + a] = -1;
            bend.gradientLow[SUPPORT3_TURN + b] = across;
            bend.gradientHigh[SUPPORT3_TURN + a] = 1;
            bend.gradientHigh[SUPPORT3_TURN + b] = -across;
        } else {
            // 違う大きさ: 2 倍の勾配(端の点の動き・sθ の差)・係数 = 細かいセルの棒 × 比 ÷ 4
            uint64_t unit = fineUnit;
            if (m_options.mixedBrace == MixedBrace::Center)
                unit = coarseLow && coarseHigh ? fineUnit / 2 : fineUnit * 2 / 3;

            brace.coefficient = Scaled(unit, intact) / 4;
            bend.coefficient = brace.coefficient;
            const uint32_t c = 3 - a - b;
            const int64_t height = (at[c] & 1) != 0 ? 1 : -1;  // 細かい場所の第 3 の軸の高さ(粗いセルの中心から ± s/2)
            const bool offset = coarseLow != coarseHigh;
            // e_c × w(w = across e_a + e_b)
            std::array<int64_t, SUPPORT3_AXES> w{};
            w[a] = across;
            w[b] = 1;
            std::array<int64_t, SUPPORT3_AXES> cross{};
            for (uint32_t i = 0; i < SUPPORT3_AXES; ++i) {
                for (uint32_t k = 0; k < SUPPORT3_AXES; ++k)
                    cross[i] += Epsilon(i, c, k) * w[k];
            }

            const auto fill = [&](Gradient& axialGradient, Gradient& bendGradient, bool coarseEnd, int64_t sign) {
                axialGradient[a] = 2 * sign * across;
                axialGradient[b] = 2 * sign;
                if (coarseEnd && offset) {
                    for (uint32_t i = 0; i < SUPPORT3_AXES; ++i)
                        axialGradient[SUPPORT3_TURN + i] = sign * height * cross[i];
                }

                const int64_t turnScale = coarseEnd ? 1 : 2;
                bendGradient[SUPPORT3_TURN + a] = sign * turnScale;
                bendGradient[SUPPORT3_TURN + b] = -sign * turnScale * across;
            };
            fill(brace.gradientLow, bend.gradientLow, coarseLow, -1);
            fill(brace.gradientHigh, bend.gradientHigh, coarseHigh, 1);
        }

        m_terms.push_back(brace);
        m_terms.push_back(bend);
        ++m_braceCount;
    }

    // --- 成分を持つ節の系へ: 節の組ごとに 1 つの面(対角は最初の節を含む面へ。同じ組は 1 つの面にまとめる)---
    void SupportLaw3D::BuildMultiBlocks() {
        std::vector<EquilibriumBlockFace>& faces = m_blockSystem.faces;
        std::map<std::pair<uint32_t, uint32_t>, size_t> faceAt;
        const auto faceOf = [&](uint32_t a, uint32_t b) -> EquilibriumBlockFace& {
            const std::pair<uint32_t, uint32_t> key = {std::min(a, b), std::max(a, b)};
            const auto [found, added] = faceAt.try_emplace(key, faces.size());
            if (added)
                faces.push_back({.low = key.first, .high = key.second});

            return faces[found->second];
        };

        for (const MultiTerm& term : m_multiTerms) {
            const auto c = (int64_t)term.coefficient;
            for (uint32_t i = 0; i < term.count; ++i) {
                for (uint32_t j = i + 1; j < term.count; ++j) {
                    EquilibriumBlockFace& face = faceOf(term.nodes[i], term.nodes[j]);
                    const bool iLow = face.low == term.nodes[i];
                    const Gradient& low = iLow ? term.gradients[i] : term.gradients[j];
                    const Gradient& high = iLow ? term.gradients[j] : term.gradients[i];
                    const bool lowDiagonal = i == 0 && (iLow ? j == 1 : true);
                    const bool highDiagonal = i == 0 && (iLow ? true : j == 1);
                    for (uint32_t row = 0; row < SUPPORT3_COMPONENTS; ++row) {
                        for (uint32_t column = 0; column < SUPPORT3_COMPONENTS; ++column) {
                            const uint32_t at = row * EQ_MAX_COMPONENTS + column;
                            face.lowHigh[at] += c * low[row] * high[column];
                            if (lowDiagonal)
                                face.lowLow[at] += c * low[row] * low[column];

                            if (highDiagonal)
                                face.highHigh[at] += c * high[row] * high[column];
                        }
                    }
                }
            }
        }
    }

    int64_t SupportLaw3D::MultiProjection(const MultiTerm& term) const {
        int64_t projection = 0;
        for (uint32_t i = 0; i < term.count; ++i) {
            for (uint32_t c = 0; c < SUPPORT3_COMPONENTS; ++c)
                projection += term.gradients[i][c] * CellValue(term.nodes[i], c);
        }

        return projection;
    }

}  // namespace bicameral::sim
