// support_law.cpp — 支えの法則の試作(R-B1・T-0871。何をするかは support_law.h)。
// 式(セルの一辺 dx・奥行 dx・ν = 0・ka = E × dx・b = ka ÷ 6。係数は全部 b の整数倍なので丸めは b の 1 回だけ):
//   BeamLattice の面(セル a → 隣 b、軸の向き n・横の向き t)のエネルギー(Euler–Bernoulli の梁を半分ずつ。22 §3.2 ①'):
//     ½ ka (n_b − n_a)²  +  ½ ka (t_b − t_a ∓ (φ_a + φ_b))²  +  ½ (ka/3) (φ_b − φ_a)²
//     (∓ は x の面で −・上の面で +。φ = 角 × dx ÷ 2 は反時計回りが正)
//   固定した面(半分の長さの梁で動かない壁へ): ½ 2ka n²  +  ½ 8ka (t ± φ/2)²  +  ½ (2ka/3) φ²
//   PerComponent: 面ごとに伸び縮み ka とずれ ka/2 のばね(回りなし)。固定した面は 2ka と ka。
//   段の斜めの棒(stairBraces。T-0944): 下のセル a → 斜め上のセル c(横の向き s = ±1)に ½ (ka/24) (s Δx + Δup)²
//     (斜めの長さ √2 dx・軸の硬さ ka/12 の棒。Δ = c − a)。x と上の面に ka/24・x ⇄ 上の結合は遅らせる。
// Lagged: 成分ごとの式の対角と負の非対角(C-0 が解く形)は面と固定した面に、成分の間の項と φ どうしの正の非対角は遅らせて右辺の項へ。
// Coupled(T-0919): 同じエネルギーの各項 ½ c (g · [x_a; x_b])² を 3 × 3 の面のブロック c g gᵀ にそのまま入れ(成分の間の項も面の中)、
//   成分を持つ節の C-0(equilibrium_block)で 1 つの系として解く。延ばし方は剛体の動き(RigidProlongation)。
#include "sim/support_law.h"

#include <algorithm>

#include "common/equilibrium.hlsli"
#include "common/implicit_conduction.hlsli"
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
        constexpr uint32_t
            BLOCK_COEFFICIENT_SHIFT = 32;           // Coupled の右辺の単位 2^-16 mN(係数 2^-32 × 変位 2^-16 ÷ 2^32)
        constexpr uint32_t BLOCK_FORCE_SHIFT = 16;  // mN → 2^-16 mN

        // 項 ½ c (g_low · x_low + g_high · x_high)² の勾配(成分ごとの -1〜2)
        using Gradient = std::array<int64_t, SUPPORT_COMPONENTS>;

        void AddTerm(EquilibriumBlockFace& face, uint64_t coefficient, const Gradient& low, const Gradient& high) {
            const auto c = (int64_t)coefficient;
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

        // セル 1 つの重さ ρ g dx³(kg/m³ × µm/s² × mm³ = 10^-12 mN)
        int64_t CellWeight(const SupportScene& scene) {
            const uint64_t volume = scene.cellMm * scene.cellMm * scene.cellMm;
            const FxU128 product = FxMulU64Full(scene.densityKgPerM3 * scene.gravityUmPerS2, volume);

            return (int64_t)FxDivU128By64(product, PER_TRILLION).quotient;
        }

        int64_t Flow(uint64_t coefficient, int64_t difference) {
            return ImFaceFlow(ImWide(coefficient), difference);
        }

    }  // namespace

    SupportLaw::SupportLaw(const SupportScene& scene, const SupportOptions& options)
        : m_scene(scene), m_options(options) {
        m_options.equilibrium.faceValues = false;
        m_options.blocks.warmStart = m_options.warmStart;
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
        m_weight = CellWeight(m_scene);

        m_index.assign((size_t)width * height, (uint32_t)NO_CELL);
        for (uint32_t up = 0; up < height; ++up) {
            for (uint32_t x = 0; x < width; ++x) {
                if (m_scene.solid[x + (size_t)width * up] == 0)
                    continue;

                m_index[x + (size_t)width * up] = (uint32_t)m_cells.size();
                m_cells.push_back({x, up});
            }
        }

        for (uint32_t c = 0; c < ComponentCount(); ++c) {
            m_systems[c].rounding = EQ_ROUNDING_EXACT;
            for (const auto& [x, up] : m_cells)
                m_systems[c].nodes.push_back({.level = 0, .x = x, .y = up, .z = 0, .capacity = 1});
        }

        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            const auto [x, up] = m_cells[cell];
            if (x + 1 < width && m_index[x + 1 + (size_t)width * up] != NO_CELL)
                AddBond({.low = cell, .high = m_index[x + 1 + (size_t)width * up], .axis = 0});

            if (up + 1 < height && m_index[x + (size_t)width * (up + 1)] != NO_CELL)
                AddBond({.low = cell, .high = m_index[x + (size_t)width * (up + 1)], .axis = 1});

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
                const bool side = m_index[(size_t)sideX + (size_t)width * up] != NO_CELL;
                const bool above = m_index[x + (size_t)width * (up + 1)] != NO_CELL;
                if (diagonal != NO_CELL && side != above)
                    AddBrace({.low = cell, .high = diagonal, .across = across});
            }
        }
    }

    // --- 成分を持つ節の系(Coupled。T-0919): Lagged と同じエネルギーの項を 3 × 3 の面のブロックへ ---
    void SupportLaw::BuildBlocks() {
        EquilibriumBlockSystem& system = m_blockSystem;
        system.components = SUPPORT_COMPONENTS;
        system.coefficientShift = BLOCK_COEFFICIENT_SHIFT;
        system.prolongation = RigidProlongation();
        for (const auto& [x, up] : m_cells)
            system.nodes.push_back({.level = 0, .x = x, .y = up, .z = 0});

        // 面: ½ 6b Δn² + ½ 6b (Δt ∓ (φ_a + φ_b))² + ½ 2b Δφ²(∓ は x の面で −・上の面で +)
        for (const Bond& bond : m_bonds) {
            const uint64_t b = bond.axis == 0 ? m_bondX : m_bondUp;
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
            AddTerm(face, m_brace, {-brace.across, -1, 0}, {brace.across, 1, 0});
            system.faces.push_back(face);
        }

        // 固定した面: ½ 12b n² + ½ 12b (2t + s φ)² + ½ 4b φ²(s は Lagged の AddClamp と同じ)
        for (const Clamp& clamp : m_clamps) {
            const uint64_t b = clamp.axis == 0 ? m_bondX : m_bondUp;
            const uint32_t along = clamp.axis == 0 ? SUPPORT_X : SUPPORT_UP;
            const uint32_t across = clamp.axis == 0 ? SUPPORT_UP : SUPPORT_X;
            EquilibriumBlockFixed fixed{.node = clamp.cell};
            Gradient gradient{};
            gradient[along] = 1;
            AddFixedTerm(fixed, 12 * b, gradient);

            gradient = {};
            gradient[across] = 2;
            gradient[SUPPORT_TURN] = clamp.axis == 0 ? clamp.side : -clamp.side;
            AddFixedTerm(fixed, 12 * b, gradient);

            AddFixedTerm(fixed, 4 * b, {0, 0, 1});
            system.fixedFaces.push_back(fixed);
        }

        for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
            system.sources.push_back(
                {.node = cell, .component = SUPPORT_UP, .value = -(m_weight << BLOCK_FORCE_SHIFT)});
        }

        m_blockGrid = BuildEquilibriumBlockGrid(system);
    }

    void SupportLaw::AddBond(const Bond& bond) {
        m_bonds.push_back(bond);
        const uint64_t b = bond.axis == 0 ? m_bondX : m_bondUp;
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
            {.node = clamp.cell, .coefficient = ImWide(beam ? 48 * b : 6 * b), .value = 0});
        if (!beam)
            return;

        // 8ka (t ± φ/2)²: φ の対角 2ka は容量・曲げ 2ka/3 は値 0 の固定した面・t ⇄ φ の 4ka は遅らせる
        // ±: 左の面 −・右の面 +・下の面 +(x の面の t は上・上の面の t は x で、回りの向きが逆になる)
        m_systems[SUPPORT_TURN].nodes[clamp.cell].capacity += 12 * b;
        m_systems[SUPPORT_TURN].fixedFaces.push_back({.node = clamp.cell, .coefficient = ImWide(4 * b), .value = 0});
        const int64_t sign = clamp.axis == 0 ? clamp.side : -clamp.side;
        AddLag(across, clamp.cell, SUPPORT_TURN, clamp.cell, sign, 24 * b);
        AddLag(SUPPORT_TURN, clamp.cell, across, clamp.cell, sign, 24 * b);
    }

    // ½ q (s Δx + Δup)²: x と上の面に q・x ⇄ 上の項(同じセルに +s q・相手のセルに −s q)は遅らせる
    void SupportLaw::AddBrace(const Brace& brace) {
        m_braces.push_back(brace);
        const uint64_t q = m_brace;
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
            m_blockState.values.assign(m_cells.size() * SUPPORT_COMPONENTS, 0);
            for (uint32_t cell = 0; cell < m_cells.size(); ++cell) {
                for (uint32_t c = 0; c < SUPPORT_COMPONENTS; ++c) {
                    m_blockState.values[cell * SUPPORT_COMPONENTS + c] = before.Value(m_cells[cell][0],
                                                                                      m_cells[cell][1], c);
                }
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
                        {.node = cell, .value = -m_weight, .factor = ImWide((uint64_t)1 << FORCE_SHIFT)});
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

    // --- 読む ---
    int64_t SupportLaw::Value(uint32_t x, uint32_t up, uint32_t component) const {
        if (x >= m_scene.width || up >= m_scene.height || component >= ComponentCount())
            return 0;

        const uint32_t cell = m_index[x + (size_t)m_scene.width * up];
        if (Coupled()) {
            const size_t at = (size_t)cell * SUPPORT_COMPONENTS + component;
            return cell == NO_CELL || at >= m_blockState.values.size() ? 0 : m_blockState.values[at];
        }

        const std::vector<int64_t>& values = m_states[component].values;
        if (cell == NO_CELL || cell >= values.size())
            return 0;

        return values[cell];
    }

    int64_t SupportLaw::UpForceOnBond(const Bond& bond) const {
        const auto [lowX, lowUp] = m_cells[bond.low];
        const auto [highX, highUp] = m_cells[bond.high];
        const int64_t difference = Value(highX, highUp, SUPPORT_UP) - Value(lowX, lowUp, SUPPORT_UP);
        if (bond.axis == 1)
            return -Flow(6 * m_bondUp, difference);

        if (m_options.form == SupportForm::PerComponent)
            return -Flow(3 * m_bondX, difference);

        const int64_t turns = Value(lowX, lowUp, SUPPORT_TURN) + Value(highX, highUp, SUPPORT_TURN);

        return -Flow(6 * m_bondX, difference - turns);
    }

    int64_t SupportLaw::UpForceOnBrace(const Brace& brace) const {
        const auto [lowX, lowUp] = m_cells[brace.low];
        const auto [highX, highUp] = m_cells[brace.high];
        const int64_t slide = Value(highX, highUp, SUPPORT_X) - Value(lowX, lowUp, SUPPORT_X);
        const int64_t lift = Value(highX, highUp, SUPPORT_UP) - Value(lowX, lowUp, SUPPORT_UP);

        return -Flow(m_brace, brace.across * slide + lift);
    }

    int64_t SupportLaw::UpForceOnClamp(const Clamp& clamp) const {
        const auto [x, up] = m_cells[clamp.cell];
        const int64_t lift = Value(x, up, SUPPORT_UP);
        if (clamp.axis == 1)
            return -Flow(12 * m_bondUp, lift);

        if (m_options.form == SupportForm::PerComponent)
            return -Flow(6 * m_bondX, lift);

        // 8ka (t ± φ/2) = 24b (2t ± φ)
        return -Flow(24 * m_bondX, 2 * lift + clamp.side * Value(x, up, SUPPORT_TURN));
    }

    // 面の力(梁の切り口。dx = セル・ka = 6b): N = ka Δn・ずれ = ka (Δt ∓ (φ_a + φ_b))・6M/dx = ka (φ_b − φ_a)。
    // 縁の力 = N ± 6M/dx(回り φ は反時計回りが正なので、x の面は下の縁が +・上の面は +x の縁が +)
    SupportFaceForces SupportLaw::FaceForces(uint32_t x, uint32_t up, uint32_t axis) const {
        const uint32_t highX = axis == 0 ? x + 1 : x;
        const uint32_t highUp = axis == 0 ? up : up + 1;
        if (m_options.form != SupportForm::BeamLattice || highX >= m_scene.width || highUp >= m_scene.height)
            return {};

        if (m_index[x + (size_t)m_scene.width * up] == NO_CELL ||
            m_index[highX + (size_t)m_scene.width * highUp] == NO_CELL)
            return {};

        const uint64_t b = axis == 0 ? m_bondX : m_bondUp;
        const uint32_t along = axis == 0 ? SUPPORT_X : SUPPORT_UP;
        const uint32_t across = axis == 0 ? SUPPORT_UP : SUPPORT_X;
        const int64_t turnLow = Value(x, up, SUPPORT_TURN);
        const int64_t turnHigh = Value(highX, highUp, SUPPORT_TURN);
        const int64_t stretch = Value(highX, highUp, along) - Value(x, up, along);
        const int64_t slide = Value(highX, highUp, across) - Value(x, up, across);
        const int64_t turns = axis == 0 ? -(turnLow + turnHigh) : turnLow + turnHigh;

        SupportFaceForces forces;
        forces.axialMn = Flow(6 * b, stretch);
        forces.shearMn = Flow(6 * b, slide + turns);
        const int64_t bend = Flow(6 * b, turnHigh - turnLow);
        forces.lowSideMn = axis == 0 ? forces.axialMn + bend : forces.axialMn - bend;
        forces.highSideMn = axis == 0 ? forces.axialMn - bend : forces.axialMn + bend;

        return forces;
    }

    SupportLedger SupportLaw::Ledger() const {
        SupportLedger ledger;
        std::vector<int64_t> net(m_cells.size(), -m_weight);
        ledger.weightMn = m_weight * (int64_t)m_cells.size();
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
                for (const auto& [x, up] : m_cells) {
                    hash ^= (uint64_t)Value(x, up, c);
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
