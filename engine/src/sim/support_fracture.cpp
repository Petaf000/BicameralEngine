// support_fracture.cpp — 支えの法則の割れの試作(R-B2・T-0872。何をするかは support_fracture.h)。
// 式(セルの一辺 dx・奥行 dx・面の面積 A = dx²・E = 硬さ。応力は面の力 ÷ A。割れかけの面は「割れていない硬さに戻した」応力で見る):
//   強さの比 f = max(引きの縁 ÷ σt, 押しの縁 ÷ σc, |ずれ| ÷ (c + 押し合い × tan φ))。c = √(σc σt) ÷ 2・sin φ = (σc − σt) ÷ (σc + σt)(22 §3.3)。
//     ずれの比は 2 |τ| √(σc σt) ÷ (σc σt + σn (σc − σt))(σn = 押し合いが正。分母が 0 以下 = 引きの切り口の外で、引きの比が先に 1 を超える)
//   Energy の軟化: 面のエネルギー U(割れていない硬さで)と、引きの強さちょうどで一様に伸びた時の U_t = σt² dx³ ÷ (2E) から κ = √(U ÷ U_t)。
//     進み p(κ) = 1 − (r − κ) ÷ (κ (r − 1))(1 ≤ κ ≤ r)・r = G A ÷ U_t = 2 E G ÷ (σt² dx)。
//     払う割れ面のエネルギー D(p) = G A (κ(p) − 1) ÷ (r − 1)・κ(p) = r ÷ (1 + (1 − p)(r − 1))(D(1) = G A)。
//     dD/dp = U_t κ² = 変位を止めて p を上げた時に解放されるエネルギーの割合(dU/dp)なので、今の κ で決めた進みまでなら熱 ≥ 0(坂を下る)。
//     r ≤ 1(セルが E G ÷ σt² の 2 倍より粗い)は軟化できないので、強さを超え、かつ U ≥ G A の面がもろく割れ切る。
#include "sim/support_fracture.h"

#include <algorithm>

#include "common/fixed.hlsli"

using namespace bicameral::fx;

namespace bicameral::sim {

    namespace {

        constexpr uint64_t ONE = SUPPORT_INTACT_FULL;  // Q16 の 1
        constexpr uint32_t INDEX_CAP = 1u << 30;
        constexpr uint64_t SOFTENING_CAP = uint64_t{1} << 30;
        constexpr uint64_t PJ_PER_NJ = 1000;  // mJ/m² × mm² = nJ
        constexpr uint64_t MICRO = 1000000;

        uint32_t CapIndex(int64_t value) {
            if (value <= 0)
                return 0;

            return (uint32_t)std::min<int64_t>(value, INDEX_CAP);
        }

    }  // namespace

    SupportFracture::SupportFracture(const SupportScene& scene, const SupportOptions& options,
                                     const SupportStrength& strength, const SupportFractureOptions& fracture)
        : m_scene(scene), m_options(options), m_strength(strength), m_fracture(fracture) {
        // 書かなかった方の強さはもう片方と同じ(22 §1.1)
        if (m_strength.compressiveKpa == 0)
            m_strength.compressiveKpa = m_strength.tensileKpa;

        if (m_strength.tensileKpa == 0)
            m_strength.tensileKpa = m_strength.compressiveKpa;

        const size_t count = (size_t)m_scene.width * m_scene.height;
        m_scene.intactX.assign(count, (uint32_t)ONE);
        m_scene.intactUp.assign(count, (uint32_t)ONE);
        m_progressX.assign(count, 0);
        m_progressUp.assign(count, 0);
        m_separated.assign(count, 0);

        // --- 物質と細かさから決まる値 ---
        const uint64_t dx = m_scene.cellMm;
        const uint64_t tensile = m_strength.tensileKpa;
        const uint64_t stiffness = std::max<uint64_t>(m_scene.stiffnessKpa, 1);
        m_fractureEnergy = m_strength.fractureEnergyMjPerM2;
        if (m_fractureEnergy == 0) {
            // σt² × L ÷ E(kPa × mm = J/m² → mJ/m²)
            const FxU128 product = FxMulU64Full(tensile * tensile, m_strength.fragmentMm * 1000);
            m_fractureEnergy = FxDivU128By64(product, stiffness).quotient;
        }

        m_faceSurfacePj = m_fractureEnergy * dx * dx * PJ_PER_NJ;
        // σt² dx³ ÷ (2E)(kPa × mm³ = 10^-6 J = 10^6 pJ)
        m_faceUnitPj = std::max<uint64_t>(
            FxDivU128By64(FxMulU64Full(tensile * tensile * dx, dx * dx * MICRO), 2 * stiffness).quotient, 1);
        m_softening = std::min(FxDivU128By64(FxMulU64Full(m_faceSurfacePj, ONE), m_faceUnitPj).quotient, SOFTENING_CAP);

        // 1 刻みに進む割合 = 速さ × 刻み ÷ セル(1 を超えたら 1。小刻みの道〔D-612 (a)〕は T-0943)
        const uint64_t step = FxDivU128By64(FxMulU64Full(m_strength.crackSpeedMmPerS * m_fracture.stepUs, ONE),
                                            MICRO * dx)
                                  .quotient;
        m_speedStep = (uint32_t)std::clamp<uint64_t>(step, 1, ONE);
        m_rootStrength = FxSqrtU64(m_strength.compressiveKpa * tensile);

        Separate();
        Rebuild();
    }

    void SupportFracture::Rebuild() {
        auto next = std::make_unique<SupportLaw>(m_scene, m_options);
        if (m_law != nullptr)
            next->CarryFrom(*m_law);

        m_law = std::move(next);
    }

    void SupportFracture::SetLoads(const std::vector<int64_t>& loadMn) {
        m_scene.loadMn = loadMn;
        Rebuild();
    }

    void SupportFracture::Dig(const std::vector<std::array<uint32_t, 2>>& cells) {
        for (const auto& [x, up] : cells) {
            const size_t at = x + (size_t)m_scene.width * up;
            m_scene.solid[at] = 0;
            m_scene.clamps[at] = 0;
        }

        Separate();
        Rebuild();
    }

    uint32_t SupportFracture::Progress(uint32_t x, uint32_t up, uint32_t axis) const {
        if (x >= m_scene.width || up >= m_scene.height)
            return 0;

        const std::vector<uint32_t>& progress = axis == 0 ? m_progressX : m_progressUp;

        return progress[x + (size_t)m_scene.width * up];
    }

    // --- 強さの比(Q16)。面の力を割れていない硬さに戻して応力にし、強さの形の 3 つの切り口のいちばん近いもの ---
    uint32_t SupportFracture::IndexOf(const SupportFaceForces& forces) const {
        if (forces.intact == 0 || m_strength.tensileKpa == 0)
            return 0;

        const uint64_t area = m_scene.cellMm * m_scene.cellMm;
        const uint64_t tensile = m_strength.tensileKpa;
        const uint64_t compressive = m_strength.compressiveKpa;
        const auto restore = [&](int64_t force) {
            return FxMulDivS64(force, ONE, forces.intact);
        };
        const int64_t low = restore(forces.lowSideMn);
        const int64_t high = restore(forces.highSideMn);
        const int64_t axial = restore(forces.axialMn);
        const int64_t shear = restore(forces.shearMn);

        int64_t index = 0;
        const int64_t pull = std::max(low, high);
        if (pull > 0)
            index = std::max(index, FxMulDivS64(pull, ONE, tensile * area));

        const int64_t push = -std::min(low, high);
        if (push > 0)
            index = std::max(index, FxMulDivS64(push, ONE, compressive * area));

        // ずれ: 2 |τ| √(σc σt) ÷ (σc σt + σn (σc − σt))(力で: 面積を分母と分子に掛けた形)
        const int64_t pressing = -axial;
        const int64_t denominator = (int64_t)(compressive * tensile * area) +
                                    pressing * (int64_t)(compressive - tensile);
        if (denominator > 0) {
            const int64_t slide = (int64_t)FxAbsU64(shear);
            index = std::max(index, FxMulDivS64(slide, 2 * m_rootStrength * ONE, (uint64_t)denominator));
        }

        return CapIndex(index);
    }

    uint32_t SupportFracture::FailureIndex(uint32_t x, uint32_t up, uint32_t axis) const {
        return IndexOf(m_law->FaceForces(x, up, axis));
    }

    // κ = √(U ÷ U_t)(Q16)
    uint32_t SupportFracture::EnergyIndex(int64_t undamagedPj) const {
        if (undamagedPj <= 0)
            return 0;

        if ((uint64_t)undamagedPj / m_faceUnitPj >= (uint64_t{1} << 20))
            return INDEX_CAP;

        const uint64_t ratio = FxDivU128By64(FxMulU64Full((uint64_t)undamagedPj, ONE * ONE), m_faceUnitPj).quotient;

        return FxSqrtU64(ratio);
    }

    // 今の解で進んでよい所(進みは戻らない)
    uint32_t SupportFracture::TargetProgress(const SupportFaceForces& forces, uint32_t progress) const {
        if (IndexOf(forces) < ONE)
            return progress;  // 強さを超えていない面は割れ始めない(どちらの形でも)

        if (m_fracture.rule == FractureRule::Strength)
            return (uint32_t)ONE;

        const int64_t undamagedPj = FxMulDivS64(forces.energyPj, ONE, forces.intact);
        if (m_softening <= ONE)
            return undamagedPj >= (int64_t)m_faceSurfacePj ? (uint32_t)ONE : progress;

        const uint64_t kappa = EnergyIndex(undamagedPj);
        if (kappa <= ONE)
            return progress;

        if (kappa >= m_softening)
            return (uint32_t)ONE;

        // p = 1 − (r − κ) ÷ (κ (r − 1))。残りは切り上げる(進みを切り捨て、κ(p) ≤ 今の κ = 熱 ≥ 0 を丸めで破らない)
        const FxDivResult rest = FxDivU128By64(FxMulU64Full((m_softening - kappa) * ONE, ONE),
                                               kappa * (m_softening - ONE));
        const uint64_t restUp = rest.quotient + (rest.remainder != 0 ? 1 : 0);

        return std::max(progress, (uint32_t)(ONE - std::min(restUp, ONE)));
    }

    // 進み p までに払った割れ面のエネルギー(pJ)
    int64_t SupportFracture::SurfacePj(uint32_t progress) const {
        if (m_fracture.rule == FractureRule::Strength || m_softening <= ONE)
            return (int64_t)FxShiftRightU128(FxMulU64Full(m_faceSurfacePj, progress), 16);

        // κ(p) = r ÷ (1 + (1 − p)(r − 1))・D = G A (κ − 1) ÷ (r − 1)
        const uint64_t denominator = ONE + (ONE - progress) * (m_softening - ONE) / ONE;
        const uint64_t kappa = m_softening * ONE / denominator;
        const uint64_t fraction = (kappa - ONE) * ONE / (m_softening - ONE);

        return (int64_t)FxShiftRightU128(FxMulU64Full(m_faceSurfacePj, std::min(fraction, ONE)), 16);
    }

    // 面 1 つ: 進める分を決め、帳簿に載せる(当てるのは全部の面を決めた後)
    bool SupportFracture::Advance(uint32_t x, uint32_t up, uint32_t axis) {
        const SupportFaceForces forces = m_law->FaceForces(x, up, axis);
        if (forces.intact == 0)
            return false;

        const size_t at = x + (size_t)m_scene.width * up;
        const uint32_t progress = axis == 0 ? m_progressX[at] : m_progressUp[at];
        const uint32_t target = TargetProgress(forces, progress);
        if (target <= progress)
            return false;

        const uint32_t step = std::min(target - progress, m_speedStep);
        const uint32_t next = progress + step;
        // 変位を止めて硬さを step だけ落とした時に面から出るエネルギー = 今のエネルギー × step ÷ 残りの硬さ
        const int64_t released = FxMulDivS64(forces.energyPj, step, forces.intact);
        const int64_t surface = SurfacePj(next) - SurfacePj(progress);
        m_ledger.releasedPj += released;
        m_ledger.surfacePj += surface;
        m_ledger.heatPj += released - surface;
        m_ledger.worstStepHeatPj = std::min(m_ledger.worstStepHeatPj, released - surface);
        (axis == 0 ? m_nextX : m_nextUp)[at] = next;

        return true;
    }

    void SupportFracture::Step() {
        m_law->Step();
        m_nextX = m_progressX;
        m_nextUp = m_progressUp;

        const uint32_t width = m_scene.width;
        bool changed = false;
        for (uint32_t up = 0; up < m_scene.height; ++up) {
            for (uint32_t x = 0; x < width; ++x) {
                if (m_scene.solid[x + (size_t)width * up] == 0)
                    continue;

                for (uint32_t axis = 0; axis < 2; ++axis)
                    changed = Advance(x, up, axis) || changed;
            }
        }

        if (!changed)
            return;

        m_progressX.swap(m_nextX);
        m_progressUp.swap(m_nextUp);
        m_ledger.brokenFaces = 0;
        m_ledger.crackingFaces = 0;
        for (size_t at = 0; at < m_progressX.size(); ++at) {
            for (const uint32_t progress : {m_progressX[at], m_progressUp[at]}) {
                if (progress >= ONE)
                    ++m_ledger.brokenFaces;
                else if (progress > 0)
                    ++m_ledger.crackingFaces;
            }

            m_scene.intactX[at] = (uint32_t)ONE - m_progressX[at];
            m_scene.intactUp[at] = (uint32_t)ONE - m_progressUp[at];
        }

        Separate();
        Rebuild();
    }

    // --- 固定した面(地面・壁)から割れていない面でたどれないセルに「分かれた」印を付け、釣り合いから外す ---
    void SupportFracture::Separate() {
        const uint32_t width = m_scene.width;
        const uint32_t height = m_scene.height;
        std::vector<uint8_t> reached(m_scene.solid.size(), 0);
        std::vector<uint32_t> queue;
        for (uint32_t at = 0; at < m_scene.solid.size(); ++at) {
            if (m_scene.solid[at] != 0 && m_scene.clamps[at] != 0) {
                reached[at] = 1;
                queue.push_back(at);
            }
        }

        const auto visit = [&](uint32_t to, uint32_t intact) {
            if (intact == 0 || m_scene.solid[to] == 0 || reached[to] != 0)
                return;

            reached[to] = 1;
            queue.push_back(to);
        };
        for (size_t head = 0; head < queue.size(); ++head) {
            const uint32_t at = queue[head];
            const uint32_t x = at % width;
            const uint32_t up = at / width;
            if (x + 1 < width)
                visit(at + 1, m_scene.intactX[at]);

            if (x > 0)
                visit(at - 1, m_scene.intactX[at - 1]);

            if (up + 1 < height)
                visit(at + width, m_scene.intactUp[at]);

            if (up > 0)
                visit(at - width, m_scene.intactUp[at - width]);
        }

        for (uint32_t at = 0; at < m_scene.solid.size(); ++at) {
            if (m_scene.solid[at] == 0 || reached[at] != 0)
                continue;

            m_scene.solid[at] = 0;
            m_separated[at] = 1;
            ++m_ledger.separatedCells;
        }
    }

    uint64_t SupportFracture::Hash() const {
        uint64_t hash = m_law->Hash();
        for (size_t at = 0; at < m_progressX.size(); ++at) {
            hash = FxHashCombine(hash, m_progressX[at]);
            hash = FxHashCombine(hash, m_progressUp[at]);
            hash = FxHashCombine(hash, m_separated[at]);
        }

        return hash;
    }

}  // namespace bicameral::sim
