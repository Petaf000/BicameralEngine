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
#include "core/aliases.h"

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

    // 面の中の直線の分布(縁の力 a・b)の正の部分の平均(力。両方正なら平均・両方負なら 0・混ざれば三角形)
    int64_t SupportFracture::PositivePart(int64_t a, int64_t b) {
        const int64_t high = std::max(a, b);
        const int64_t low = std::min(a, b);
        if (low >= 0)
            return (a + b) / 2;

        if (high <= 0)
            return 0;

        return FxMulDivS64(high, (uint64_t)high, (uint64_t)(2 * (high - low)));
    }

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
        // 割れの判定の長さ ℓ = 硬さ × 割れ面のエネルギー ÷ 引きの強さ²(Irwin の長さ。D-644 = ADR-0448 の (C))。法則の下限 ℓ_min 0.5 m
        // (kPa × mJ/m² ÷ kPa² = 10^-3 m → ÷ 1000 で mm)
        const FxU128 irwin = FxMulU64Full(stiffness, m_fractureEnergy);
        const uint64_t lengthMm = FxDivU128By64(irwin, std::max<uint64_t>(tensile * tensile * 1000, 1)).quotient;
        m_windowMm = std::max(lengthMm, SUPPORT_FRACTURE_MIN_LENGTH_MM);

        // 細かい格子で持つ(T-1067): 分けたセルがあるか、割れかけの所を細かくする時
        m_fineMode = m_fracture.refine || rng::any_of(m_scene.fine, [](uint8_t fine) { return fine != 0; });
        if (m_fineMode)
            InitFine();

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
            if (!m_fineMode)
                continue;

            m_scene.fine[at] = 0;
            for (uint32_t part = 0; part < 4; ++part)
                m_scene.fineAbsent[2 * x + (part & 1) + (size_t)2 * m_scene.width * (2 * up + (part >> 1))] = 0;
        }

        Separate();
        Rebuild();
    }

    uint32_t SupportFracture::Progress(uint32_t x, uint32_t up, uint32_t axis) const {
        if (x >= m_scene.width || up >= m_scene.height)
            return 0;

        if (m_fineMode) {
            // 粗い面の 2 つの細かい面の小さい方(割れ切った = 2 つとも)
            const uint32_t fx = axis == 0 ? 2 * x + 1 : 2 * x;
            const uint32_t fup = axis == 0 ? 2 * up : 2 * up + 1;
            return std::min(FineProgress(fx, fup, axis),
                            FineProgress(axis == 0 ? fx : fx + 1, axis == 0 ? fup + 1 : fup, axis));
        }

        const std::vector<uint32_t>& progress = axis == 0 ? m_progressX : m_progressUp;

        return progress[x + (size_t)m_scene.width * up];
    }

    uint32_t SupportFracture::FineProgress(uint32_t fx, uint32_t fup, uint32_t axis) const {
        if (!m_fineMode || fx >= 2 * m_scene.width || fup >= 2 * m_scene.height)
            return 0;

        const std::vector<uint32_t>& progress = axis == 0 ? m_progressFineX : m_progressFineUp;

        return progress[fx + (size_t)2 * m_scene.width * fup];
    }

    // --- 強さの比(Q16)。面の力を割れていない硬さに戻して応力にし、強さの形の 3 つの切り口のいちばん近いもの ---
    uint32_t SupportFracture::IndexOf(const SupportFaceForces& forces) const {
        if (forces.intact == 0 || m_strength.tensileKpa == 0)
            return 0;

        const auto restore = [&](int64_t force) {
            return FxMulDivS64(force, ONE, forces.intact);
        };
        const int64_t low = restore(forces.lowSideMn);
        const int64_t high = restore(forces.highSideMn);
        const int64_t axial = restore(forces.axialMn);
        const int64_t shear = restore(forces.shearMn);

        return IndexFrom(std::max(low, high), -std::min(low, high), axial, shear);
    }

    // 引きの力 pull・押しの力 push(どちらも応力 × 面積)・軸の力・ずれの力から強さの比(Q16)
    uint32_t SupportFracture::IndexFrom(int64_t pull, int64_t push, int64_t axial, int64_t shear) const {
        return IndexFromArea(pull, push, axial, shear, m_scene.cellMm * m_scene.cellMm);
    }

    // 同じ。面積 areaMm2 の面(細かい格子の面・区間の和。T-1067)
    uint32_t SupportFracture::IndexFromArea(int64_t pull, int64_t push, int64_t axial, int64_t shear,
                                            uint64_t areaMm2) const {
        const uint64_t area = areaMm2;
        const uint64_t tensile = m_strength.tensileKpa;
        const uint64_t compressive = m_strength.compressiveKpa;
        int64_t index = 0;
        if (pull > 0)
            index = std::max(index, FxMulDivS64(pull, ONE, tensile * area));

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
        return EnergyIndexOf(undamagedPj, m_faceUnitPj);
    }

    // κ = √(U ÷ U_t)(Q16。U_t = unitPj)
    uint32_t SupportFracture::EnergyIndexOf(int64_t undamagedPj, uint64_t unitPj) const {
        if (undamagedPj <= 0)
            return 0;

        if ((uint64_t)undamagedPj / unitPj >= (uint64_t{1} << 20))
            return INDEX_CAP;

        const uint64_t ratio = FxDivU128By64(FxMulU64Full((uint64_t)undamagedPj, ONE * ONE), unitPj).quotient;

        return FxSqrtU64(ratio);
    }

    // 今の解で進んでよい所(進みは戻らない)
    uint32_t SupportFracture::TargetProgress(const SupportFaceForces& forces, uint32_t progress) const {
        return TargetFrom(IndexOf(forces), FxMulDivS64(forces.energyPj, ONE, forces.intact), progress);
    }

    // 強さの比 index(Q16)と、割れていない硬さに戻したエネルギー undamagedPj から
    uint32_t SupportFracture::TargetFrom(uint32_t index, int64_t undamagedPj, uint32_t progress) const {
        return TargetFromSums(index, undamagedPj, m_faceUnitPj, m_faceSurfacePj, progress);
    }

    // 同じ。エネルギー・U_t・割れ面のエネルギーの和から(区間の面の和でもよい: r = ΣGA ÷ ΣU_t・κ = √(ΣU ÷ ΣU_t)。T-1067)
    uint32_t SupportFracture::TargetFromSums(uint32_t index, int64_t undamagedPj, uint64_t unitPj, uint64_t surfacePj,
                                             uint32_t progress) const {
        if (index < ONE)
            return progress;  // 強さを超えていない面は割れ始めない(どの形でも)

        if (m_fracture.rule == FractureRule::Strength)
            return (uint32_t)ONE;

        const uint64_t softening = std::min(FxDivU128By64(FxMulU64Full(surfacePj, ONE), unitPj).quotient,
                                            SOFTENING_CAP);
        if (softening <= ONE)
            return undamagedPj >= (int64_t)surfacePj ? (uint32_t)ONE : progress;

        const uint64_t kappa = EnergyIndexOf(undamagedPj, unitPj);
        if (kappa <= ONE)
            return progress;

        if (kappa >= softening)
            return (uint32_t)ONE;

        // p = 1 − (r − κ) ÷ (κ (r − 1))。残りは切り上げる(進みを切り捨て、κ(p) ≤ 今の κ = 熱 ≥ 0 を丸めで破らない)
        const FxDivResult rest = FxDivU128By64(FxMulU64Full((softening - kappa) * ONE, ONE), kappa * (softening - ONE));
        const uint64_t restUp = rest.quotient + (rest.remainder != 0 ? 1 : 0);

        return std::max(progress, (uint32_t)(ONE - std::min(restUp, ONE)));
    }

    // 進み p までに払った割れ面のエネルギー(pJ)
    int64_t SupportFracture::SurfacePj(uint32_t progress) const {
        return SurfaceOf(progress, m_faceSurfacePj, m_softening);
    }

    // 同じ。面の G A = surfacePj・r = softening(細かい格子の面の種類ごと。T-1067)
    int64_t SupportFracture::SurfaceOf(uint32_t progress, uint64_t surfacePj, uint64_t softening) const {
        if (m_fracture.rule == FractureRule::Strength || softening <= ONE)
            return (int64_t)FxShiftRightU128(FxMulU64Full(surfacePj, progress), 16);

        // κ(p) = r ÷ (1 + (1 − p)(r − 1))・D = G A (κ − 1) ÷ (r − 1)
        const uint64_t denominator = ONE + (ONE - progress) * (softening - ONE) / ONE;
        const uint64_t kappa = softening * ONE / denominator;
        const uint64_t fraction = (kappa - ONE) * ONE / (softening - ONE);

        return (int64_t)FxShiftRightU128(FxMulU64Full(surfacePj, std::min(fraction, ONE)), 16);
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
        if (m_fineMode) {
            FineStep();
            return;
        }

        m_nextX = m_progressX;
        m_nextUp = m_progressUp;

        const uint32_t width = m_scene.width;
        bool changed = m_fracture.rule == FractureRule::Window && WindowStep();
        for (uint32_t up = 0; up < m_scene.height && m_fracture.rule != FractureRule::Window; ++up) {
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

    // --- 3 本目(T-1042・T-1069): 割れの面を線ごとの「面の続き」(割れ切っていない面の並び)に分け、続きの両端
    //     (物の表面・割れ切った面)から ℓ ずつの区間で判定する(世界の原点にそろえない = 置く場所によらない)。
    //     面は近い方の端から数えた区間に属し(持ち主の区間)、持ち主の区間の面は一緒に進む。
    //     強さの判定 = 区間の範囲(端から k ℓ〜(k + 1) ℓ。続きが 2ℓ より短ければ反対の半分にかかる)の応力の平均。
    //     エネルギーと進み = 持ち主の区間の面の平均(同じ進みで、その平均のエネルギーで進めるので、熱 ≥ 0 が区間ごとに成り立つ)
    bool SupportFracture::WindowStep() {
        const size_t cells = (size_t)m_scene.width * m_scene.height;
        const uint64_t dx = m_scene.cellMm;
        const auto span = (uint32_t)std::max<uint64_t>(1, m_windowMm / dx);

        // --- 面の値(割れていない硬さに戻した応力 × 面積・エネルギー)。並びは axis × cells + セル ---
        std::vector<WindowFace> faces(2 * cells);
        for (uint32_t up = 0; up < m_scene.height; ++up) {
            for (uint32_t x = 0; x < m_scene.width; ++x) {
                if (m_scene.solid[x + (size_t)m_scene.width * up] != 0) {
                    for (uint32_t axis = 0; axis < 2; ++axis)
                        faces[axis * cells + x + (size_t)m_scene.width * up] = ReadWindowFace(x, up, axis);
                }
            }
        }

        // --- 線ごとに面の続きを集め、区間を決める(x の面は上の向き・上の面は x の向きに並ぶ)---
        std::vector<uint32_t> homeOf(2 * cells, NO_WINDOW);
        std::vector<WindowSum> windows;
        std::vector<size_t> run;
        for (uint32_t axis = 0; axis < 2; ++axis) {
            const uint32_t lines = axis == 0 ? m_scene.width : m_scene.height;
            const uint32_t length = axis == 0 ? m_scene.height : m_scene.width;
            for (uint32_t line = 0; line < lines; ++line) {
                for (uint32_t along = 0; along <= length; ++along) {
                    const size_t key = axis * cells + (axis == 0 ? line + (size_t)m_scene.width * along
                                                                 : along + (size_t)m_scene.width * line);
                    if (along < length && faces[key].present) {
                        run.push_back(key);
                        continue;
                    }

                    AssignRun(run, span, faces, homeOf, windows);
                    run.clear();
                }
            }
        }

        return AdvanceWindows(faces, homeOf, windows);
    }

    SupportFracture::WindowFace SupportFracture::ReadWindowFace(uint32_t x, uint32_t up, uint32_t axis) const {
        const SupportFaceForces forces = m_law->FaceForces(x, up, axis);
        if (forces.intact == 0)
            return {};

        const auto restore = [&](int64_t force) {
            return FxMulDivS64(force, ONE, forces.intact);
        };
        const int64_t low = restore(forces.lowSideMn);
        const int64_t high = restore(forces.highSideMn);

        return {.pull = PositivePart(low, high),
                .push = PositivePart(-low, -high),
                .axial = restore(forces.axialMn),
                .shear = restore(forces.shearMn),
                .energyPj = restore(forces.energyPj),
                .forces = forces,
                .progress = Progress(x, up, axis),
                .present = true};
    }

    // 面の続き 1 つ(run。端から順)を区間に分ける。続きが ℓ 以下なら 1 区間。長ければ、面は近い方の端から数えて k = 距離 ÷ ℓ 番目の区間に属し、
    // その区間の強さは端から [k ℓ, (k + 1) ℓ) の範囲の平均で見る(真ん中をまたいでよい)
    void SupportFracture::AssignRun(const std::vector<size_t>& run, uint32_t span, const std::vector<WindowFace>& faces,
                                    std::vector<uint32_t>& homeOf, std::vector<WindowSum>& windows) {
        const auto count = (uint32_t)run.size();
        if (count == 0)
            return;

        const bool single = count <= span;
        const uint32_t perSide = single ? 1 : count / span + 1;
        const auto first = (uint32_t)windows.size();
        windows.resize(first + 2 * perSide);
        for (uint32_t side = 0; side < 2; ++side) {
            for (uint32_t k = 0; k < perSide; ++k) {
                // 強さを見る範囲(端から数えた番号 → 続きの中の番号)
                const uint32_t begin = single ? 0 : std::min(k * span, count);
                const uint32_t end = single ? count : std::min((k + 1) * span, count);
                WindowSum& window = windows[first + 2 * k + side];
                for (uint32_t d = begin; d < end && (side == 0 || !single); ++d) {
                    const WindowFace& face = faces[run[side == 0 ? d : count - 1 - d]];
                    window.pull += face.pull;
                    window.push += face.push;
                    window.axial += face.axial;
                    window.shear += face.shear;
                    ++window.rangeCount;
                }
            }
        }

        for (uint32_t j = 0; j < count; ++j) {
            const uint32_t side = single || 2 * j < count ? 0 : 1;
            const uint32_t distance = side == 0 ? j : count - 1 - j;
            const uint32_t home = first + 2 * (single ? 0 : distance / span) + side;
            const WindowFace& face = faces[run[j]];
            WindowSum& window = windows[home];
            window.energyPj += face.energyPj;
            ++window.count;
            window.progress = std::min(window.progress, face.progress);
            homeOf[run[j]] = home;
        }
    }

    // 面 1 つを reach まで進めた時の熱(解放 − 払う割れ面のエネルギー)
    int64_t SupportFracture::FaceHeat(const WindowFace& face, uint32_t reach) const {
        if (reach <= face.progress)
            return 0;

        const int64_t released = FxMulDivS64(face.forces.energyPj, reach - face.progress, face.forces.intact);

        return released - (SurfacePj(reach) - SurfacePj(face.progress));
    }

    // 持ち主の区間(first からつないだ面)を reach まで進めた時の熱の和
    int64_t SupportFracture::WindowHeat(const std::vector<WindowFace>& faces, const std::vector<size_t>& nextOf,
                                        size_t first, uint32_t reach) const {
        int64_t heat = 0;
        for (size_t key = first; key != NO_FACE; key = nextOf[key])
            heat += FaceHeat(faces[key], reach);

        return heat;
    }

    // 区間ごとの進み(区間の平均で 2 本目と同じ判定)→ 持ち主の区間の面を同じ進みまで
    bool SupportFracture::AdvanceWindows(const std::vector<WindowFace>& faces, const std::vector<uint32_t>& homeOf,
                                         std::vector<WindowSum>& windows) {
        const size_t cells = (size_t)m_scene.width * m_scene.height;
        for (WindowSum& window : windows) {
            if (window.count == 0 || window.rangeCount == 0)
                continue;

            const auto range = (int64_t)window.rangeCount;
            const uint32_t index = IndexFrom(window.pull / range, window.push / range, window.axial / range,
                                             window.shear / range);
            window.target = TargetFrom(index, window.energyPj / (int64_t)window.count, window.progress);
        }

        // 持ち主の区間ごとの面の一覧(つないだ並び。面の番号の順)
        std::vector<size_t> firstOf(windows.size(), NO_FACE);
        std::vector<size_t> lastOf(windows.size(), NO_FACE);
        std::vector<size_t> nextOf(faces.size(), NO_FACE);
        for (size_t key = 0; key < faces.size(); ++key) {
            const uint32_t home = homeOf[key];
            if (home == NO_WINDOW)
                continue;

            if (firstOf[home] == NO_FACE)
                firstOf[home] = key;
            else
                nextOf[lastOf[home]] = key;

            lastOf[home] = key;
        }

        bool changed = false;
        for (size_t w = 0; w < windows.size(); ++w) {
            WindowSum& window = windows[w];
            if (window.count == 0 || window.target <= window.progress)
                continue;

            // 共通の届く所。区間の熱が負なら(面の進みがそろっていない時など)、熱 ≥ 0 の所まで二分で下げる(T-1103。坂を下る所までしか進まない)
            uint32_t reach = window.progress + std::min(window.target - window.progress, m_speedStep);
            if (WindowHeat(faces, nextOf, firstOf[w], reach) < 0) {
                uint32_t low = window.progress;
                uint32_t high = reach;
                for (uint32_t attempt = 0; attempt < 17; ++attempt) {
                    const uint32_t middle = low + (high - low + 1) / 2;
                    if (WindowHeat(faces, nextOf, firstOf[w], middle) >= 0)
                        low = middle;
                    else
                        high = middle - 1;
                }

                reach = low;
                ++m_ledger.heatLimitedSteps;
            }

            for (size_t key = firstOf[w]; key != NO_FACE; key = nextOf[key]) {
                const WindowFace& face = faces[key];
                if (reach <= face.progress)
                    continue;

                const int64_t heat = FaceHeat(face, reach);
                m_ledger.releasedPj += FxMulDivS64(face.forces.energyPj, reach - face.progress, face.forces.intact);
                m_ledger.surfacePj += SurfacePj(reach) - SurfacePj(face.progress);
                m_ledger.heatPj += heat;
                window.heatPj += heat;
                (key < cells ? m_nextX : m_nextUp)[key % cells] = reach;
                changed = true;
            }
        }

        // 熱 ≥ 0 は区間ごと(同じ進みで区間の平均のエネルギーで進めた上に、負なら届く所を下げたので)
        for (const WindowSum& window : windows) {
            if (window.count != 0)
                m_ledger.worstStepHeatPj = std::min(m_ledger.worstStepHeatPj, window.heatPj);
        }

        return changed;
    }

    // --- 固定した面(地面・壁)から割れていない面でたどれないセルに「分かれた」印を付け、釣り合いから外す ---
    void SupportFracture::Separate() {
        if (m_fineMode) {
            SeparateFine();
            return;
        }

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

        // 細かい格子の進みと分けたセル(T-1067)
        for (size_t at = 0; at < m_progressFineX.size(); ++at) {
            hash = FxHashCombine(hash, m_progressFineX[at]);
            hash = FxHashCombine(hash, m_progressFineUp[at]);
            hash = FxHashCombine(hash, m_scene.fineAbsent[at]);
        }

        if (m_fineMode) {
            for (const uint8_t fine : m_scene.fine)
                hash = FxHashCombine(hash, fine);

            for (const uint16_t calm : m_calmTicks)
                hash = FxHashCombine(hash, calm);
        }

        return hash;
    }

}  // namespace bicameral::sim
