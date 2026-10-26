// support_fracture_fine.cpp — 支えの割れを細かい格子(一辺 cellMm ÷ 2)の面ごとに持つ道(T-1067。何をするかは support_fracture.h の冒頭)。
// 違う大きさのセル(T-1120・ADR-0470)が混ざる場面で、割れの判定を面の数ではなく長さと面積で数える(ADR-0470「割れの判定の数え方」)。
// 面の種類(s = cellMm ÷ 2・D = 奥行 cellMm。w = 面の幅・h = 面の梁の長さ):
//   粗い面(分けていないセルどうし)w = 2s・h = 2s / 細かい面(細かいセルどうし)w = s・h = s / 違う大きさの面 w = s・h = 1.5s
//   面積 A = w D・割れ面のエネルギー G A・U_t = σt² A h ÷ (2E)・r = G A ÷ U_t = 2 E G ÷ (σt² h)
//   1 刻みに進む割合 = 速さ × 刻み ÷ 持ち主の区間の Σw(割れが渡る幅。1 本目・2 本目は面ごとに w。T-1135)
// 区間(3 本目): 面の続き(割れ切っていない面の並び)の両端から長さ ℓ ずつ(細かい格子の数 ℓ ÷ s)。面は近い方の端からの距離(端の側の縁まで)で
//   持ち主の区間が決まる。強さは区間の範囲の面の引き・押し・軸・ずれの和 ÷ 面積の和、進みは持ち主の区間の ΣU ÷ ΣU_t と ΣGA ÷ ΣU_t で
//   (粗い面だけなら面の数で数える形 = support_fracture.cpp の WindowStep と同じ区間。丸めの順は違う)。
// 細かくする(refine): 強さの比(縁の応力)が refineIndex 以上の面の両側のセルと、その周り refineRing のセルを 2 × 2 に分ける。分けたセルは、中の面が割れていない・
//   外の面の 2 つの細かい面の進みが同じ・中と外の面の比がみな coarsenIndex 未満の刻みが coarsenTicks 続いたら戻す。分けた・戻した刻みは割れを進めない(次の刻みに分けた後の解で)。
// データの流れ: SupportLaw の FaceForces / FineFaceForces → FineFace → 区間 → 進み(m_progressFineX / Up)→ scene の intactFine と intactX / Up
//   → 細かくする・戻す(scene.fine)→ 分かれる(scene.fineAbsent・solid)→ 次の刻みの SupportLaw(前の答えから温めて。T-1120 の CarryFrom)
#include "sim/support_fracture.h"

#include <algorithm>

#include "common/fixed.hlsli"
#include "core/aliases.h"

using namespace bicameral::fx;

namespace bicameral::sim {

    namespace {

        constexpr uint64_t ONE = SUPPORT_INTACT_FULL;  // Q16 の 1
        constexpr uint64_t SOFTENING_CAP = uint64_t{1} << 30;
        constexpr uint64_t PJ_PER_NJ = 1000;  // mJ/m² × mm² = nJ
        constexpr uint64_t MICRO = 1000000;

    }  // namespace

    // --- 細かい格子の進みと面の種類の値 ---
    void SupportFracture::InitFine() {
        const size_t fineCount = (size_t)4 * m_scene.width * m_scene.height;
        if (m_scene.fine.empty())
            m_scene.fine.assign((size_t)m_scene.width * m_scene.height, 0);

        m_scene.fineAbsent.assign(fineCount, 0);
        m_scene.intactFineX.assign(fineCount, (uint32_t)ONE);
        m_scene.intactFineUp.assign(fineCount, (uint32_t)ONE);
        m_progressFineX.assign(fineCount, 0);
        m_progressFineUp.assign(fineCount, 0);
        m_calmTicks.assign((size_t)m_scene.width * m_scene.height, 0);

        const uint64_t side = m_scene.cellMm / 2;
        const uint64_t depth = m_scene.cellMm;
        const uint64_t tensile = m_strength.tensileKpa;
        const uint64_t stiffness = std::max<uint64_t>(m_scene.stiffnessKpa, 1);
        // 面の幅(細かい格子の数)と梁の長さ h の 2 倍(s の数): 粗い面 2・4・細かい面 1・2・違う大きさの面 1・3
        constexpr std::array<uint32_t, FACE_KINDS> SLOTS = {2, 1, 1};
        constexpr std::array<uint64_t, FACE_KINDS> DOUBLE_LENGTH = {4, 2, 3};
        for (uint32_t kind = 0; kind < FACE_KINDS; ++kind) {
            FaceKind& each = m_kinds[kind];
            each.slots = SLOTS[kind];
            each.areaMm2 = each.slots * side * depth;
            each.surfacePj = m_fractureEnergy * each.areaMm2 * PJ_PER_NJ;

            // σt² A h ÷ (2E) = σt² A (2h) ÷ (4E)(kPa × mm³ = 10^6 pJ)
            const FxU128 product = FxMulU64Full(tensile * tensile * each.areaMm2, DOUBLE_LENGTH[kind] * side * MICRO);
            each.unitPj = std::max<uint64_t>(FxDivU128By64(product, 4 * stiffness).quotient, 1);
            each.softening = std::min(FxDivU128By64(FxMulU64Full(each.surfacePj, ONE), each.unitPj).quotient,
                                      SOFTENING_CAP);
        }
    }

    // 細かい格子の場所の中身: 0 = 無い・1 = 分けていないセル・2 = 細かいセル
    uint32_t SupportFracture::SlotState(uint32_t fx, uint32_t fup) const {
        const size_t at = fx / 2 + (size_t)m_scene.width * (fup / 2);
        if (m_scene.solid[at] == 0)
            return 0;

        if (m_scene.fine[at] == 0)
            return 1;

        return m_scene.fineAbsent[fx + (size_t)2 * m_scene.width * fup] != 0 ? 0 : 2;
    }

    // --- 面の一覧(線ごと・線に沿った順。粗い面は偶数の場所で 1 つ)---
    std::vector<SupportFracture::FineFace> SupportFracture::FineFaces(bool read) const {
        const uint32_t fineWidth = 2 * m_scene.width;
        const uint32_t fineHeight = 2 * m_scene.height;
        std::vector<FineFace> faces;
        for (uint32_t axis = 0; axis < 2; ++axis) {
            const uint32_t lines = axis == 0 ? fineWidth - 1 : fineHeight - 1;
            const uint32_t length = axis == 0 ? fineHeight : fineWidth;
            for (uint32_t line = 0; line < lines; ++line) {
                for (uint32_t along = 0; along < length; ++along) {
                    const uint32_t fx = axis == 0 ? line : along;
                    const uint32_t fup = axis == 0 ? along : line;
                    FineFace face;
                    if (!FaceAt(fx, fup, axis, face) || face.start != along)
                        continue;  // 面が無い・粗い面の 2 つ目の場所

                    face.line = line;
                    if (read)
                        ReadFineFace(face);

                    faces.push_back(face);
                }
            }
        }

        return faces;
    }

    // 細かい座標 (fx, fup) と +軸の隣の間の面(粗い面なら 1 つ目の場所の面。start は線に沿った位置)。無ければ false
    bool SupportFracture::FaceAt(uint32_t fx, uint32_t fup, uint32_t axis, FineFace& face) const {
        const uint32_t fineWidth = 2 * m_scene.width;
        const uint32_t nextX = axis == 0 ? fx + 1 : fx;
        const uint32_t nextUp = axis == 0 ? fup : fup + 1;
        if (fx >= fineWidth || fup >= 2 * m_scene.height || nextX >= fineWidth || nextUp >= 2 * m_scene.height)
            return false;

        const uint32_t low = SlotState(fx, fup);
        const uint32_t high = SlotState(nextX, nextUp);
        if (low == 0 || high == 0)
            return false;

        const uint32_t along = axis == 0 ? fup : fx;
        face = FineFace{.axis = axis, .key = fx + fineWidth * fup, .start = along};
        face.pairKey = face.key;
        face.cells = {fx / 2 + m_scene.width * (fup / 2), nextX / 2 + m_scene.width * (nextUp / 2)};
        if (low == 2 || high == 2) {
            face.kind = low == 2 && high == 2 ? FACE_FINE : FACE_MIXED;
            return true;
        }

        if (face.cells[0] == face.cells[1])
            return false;  // 分けていないセルの中

        // 粗い面: 1 つ目の場所(線に沿って偶数)に寄せる
        face.kind = FACE_COARSE;
        face.start = along & ~1u;
        face.key = axis == 0 ? fx + fineWidth * (fup & ~1u) : (fx & ~1u) + fineWidth * fup;
        face.pairKey = axis == 0 ? face.key + fineWidth : face.key + 1;

        return true;
    }

    uint32_t SupportFracture::FineFailureIndex(uint32_t fx, uint32_t fup, uint32_t axis) const {
        FineFace face;
        if (!m_fineMode || !FaceAt(fx, fup, axis, face))
            return 0;

        ReadFineFace(face);
        if (!face.present)
            return 0;

        return IndexFromArea(face.edgePull, face.edgePush, face.axial, face.shear, m_kinds[face.kind].areaMm2);
    }

    void SupportFracture::ReadFineFace(FineFace& face) const {
        const uint32_t fineWidth = 2 * m_scene.width;
        const uint32_t fx = face.key % fineWidth;
        const uint32_t fup = face.key / fineWidth;
        face.progress = (face.axis == 0 ? m_progressFineX : m_progressFineUp)[face.key];
        const SupportFaceForces forces = face.kind == FACE_COARSE ? m_law->FaceForces(fx / 2, fup / 2, face.axis)
                                                                  : m_law->FineFaceForces(fx, fup, face.axis);
        if (forces.intact == 0)
            return;

        const auto restore = [&](int64_t force) {
            return FxMulDivS64(force, ONE, forces.intact);
        };
        const int64_t low = restore(forces.lowSideMn);
        const int64_t high = restore(forces.highSideMn);
        face.pull = PositivePart(low, high);
        face.push = PositivePart(-low, -high);
        face.edgePull = std::max(low, high);
        face.edgePush = -std::min(low, high);
        face.axial = restore(forces.axialMn);
        face.shear = restore(forces.shearMn);
        face.energyPj = restore(forces.energyPj);
        face.forces = forces;
        face.present = true;
    }

    // --- 1 刻み(細かい格子): 細かくする・戻すのが先。変わらなければ割れを進める ---
    void SupportFracture::FineStep() {
        m_nextFineX = m_progressFineX;
        m_nextFineUp = m_progressFineUp;
        const std::vector<FineFace> faces = FineFaces(true);
        const bool refined = m_fracture.refine && Refine(faces);
        const bool changed = !refined && FineWindowStep(faces);
        if (!refined && !changed)
            return;

        if (changed) {
            m_progressFineX.swap(m_nextFineX);
            m_progressFineUp.swap(m_nextFineUp);
        }

        if (refined)
            ++m_ledger.refineSteps;

        ApplyFineProgress();
        SeparateFine();
        Rebuild();
    }

    // 区間を決めて進める(1 本目・2 本目は面ごとに 1 区間・縁の応力で)
    bool SupportFracture::FineWindowStep(const std::vector<FineFace>& faces) {
        const uint64_t side = m_scene.cellMm / 2;
        const auto span = (uint32_t)std::max<uint64_t>(1, m_windowMm / side);
        std::vector<uint32_t> homeOf(faces.size(), NO_WINDOW);
        std::vector<FineWindow> windows;
        if (m_fracture.rule != FractureRule::Window) {
            for (size_t i = 0; i < faces.size(); ++i) {
                const FineFace& face = faces[i];
                if (!face.present)
                    continue;

                const FaceKind& kind = m_kinds[face.kind];
                homeOf[i] = (uint32_t)windows.size();
                windows.push_back({.pull = face.edgePull,
                                   .push = face.edgePush,
                                   .axial = face.axial,
                                   .shear = face.shear,
                                   .areaMm2 = kind.areaMm2,
                                   .energyPj = face.energyPj,
                                   .unitPj = kind.unitPj,
                                   .surfacePj = kind.surfacePj,
                                   .count = 1,
                                   .progress = face.progress,
                                   .widthMm = kind.slots * side});
            }

            return AdvanceFineWindows(faces, homeOf, windows);
        }

        // 面の続き: 同じ線で、前の面の終わりから始まる割れ切っていない面
        std::vector<size_t> run;
        for (size_t i = 0; i < faces.size(); ++i) {
            const FineFace& face = faces[i];
            const bool continues = !run.empty() && face.present && faces[run.back()].axis == face.axis &&
                                   faces[run.back()].line == face.line &&
                                   faces[run.back()].start + m_kinds[faces[run.back()].kind].slots == face.start;
            if (!continues) {
                AssignFineRun(run, faces, span, homeOf, windows);
                run.clear();
            }

            if (face.present)
                run.push_back(i);
        }

        AssignFineRun(run, faces, span, homeOf, windows);

        return AdvanceFineWindows(faces, homeOf, windows);
    }

    // 面の続き 1 つを長さで区間に分ける(support_fracture.cpp の AssignRun を細かい格子の長さで)
    void SupportFracture::AssignFineRun(const std::vector<size_t>& run, const std::vector<FineFace>& faces,
                                        uint32_t span, std::vector<uint32_t>& homeOf,
                                        std::vector<FineWindow>& windows) const {
        if (run.empty())
            return;

        std::vector<uint32_t> start(run.size());
        uint32_t total = 0;
        for (size_t j = 0; j < run.size(); ++j) {
            start[j] = total;
            total += m_kinds[faces[run[j]].kind].slots;
        }

        const bool single = total <= span;
        const uint32_t perSide = single ? 1 : total / span + 1;
        const auto first = (uint32_t)windows.size();
        windows.resize(first + 2 * perSide);
        for (size_t j = 0; j < run.size(); ++j) {
            const FineFace& face = faces[run[j]];
            const FaceKind& kind = m_kinds[face.kind];
            const uint32_t fromLow = start[j];
            const uint32_t fromHigh = total - start[j] - kind.slots;

            // --- 強さを見る範囲(端から [k ℓ, (k + 1) ℓ) に端の側の縁がある面)---
            const auto addRange = [&](FineWindow& window) {
                window.pull += face.pull;
                window.push += face.push;
                window.axial += face.axial;
                window.shear += face.shear;
                window.areaMm2 += kind.areaMm2;
            };
            if (single)
                addRange(windows[first]);
            else {
                addRange(windows[first + 2 * (fromLow / span)]);
                addRange(windows[first + 2 * (fromHigh / span) + 1]);
            }

            // --- 持ち主の区間(近い方の端から)---
            const uint32_t sideIndex = single || 2 * start[j] + kind.slots <= total ? 0 : 1;
            const uint32_t distance = sideIndex == 0 ? fromLow : fromHigh;
            const uint32_t home = first + 2 * (single ? 0 : distance / span) + sideIndex;
            FineWindow& window = windows[home];
            window.energyPj += face.energyPj;
            window.unitPj += kind.unitPj;
            window.surfacePj += kind.surfacePj;
            ++window.count;
            window.progress = std::min(window.progress, face.progress);
            window.widthMm += kind.slots * (m_scene.cellMm / 2);
            homeOf[run[j]] = home;
        }
    }

    // 面 1 つを reach まで進めた時の熱(解放 − 払う割れ面のエネルギー。面の種類の G A と r で)
    int64_t SupportFracture::FineFaceHeat(const FineFace& face, uint32_t reach) const {
        if (reach <= face.progress)
            return 0;

        const FaceKind& kind = m_kinds[face.kind];
        const int64_t released = FxMulDivS64(face.forces.energyPj, reach - face.progress, face.forces.intact);
        const int64_t surface = SurfaceOf(reach, kind.surfacePj, kind.softening) -
                                SurfaceOf(face.progress, kind.surfacePj, kind.softening);

        return released - surface;
    }

    // 区間ごとの進み → 持ち主の区間の面を同じ進みまで(熱が負なら二分で下げる。T-1103 と同じ)
    bool SupportFracture::AdvanceFineWindows(const std::vector<FineFace>& faces, const std::vector<uint32_t>& homeOf,
                                             std::vector<FineWindow>& windows) {
        for (FineWindow& window : windows) {
            if (window.count == 0 || window.areaMm2 == 0)
                continue;

            const uint32_t index = IndexFromArea(window.pull, window.push, window.axial, window.shear, window.areaMm2);
            window.target = TargetFromSums(index, window.energyPj, window.unitPj, window.surfacePj, window.progress);
        }

        // 持ち主の区間ごとの面の一覧(つないだ並び。面の順)
        std::vector<size_t> firstOf(windows.size(), NO_FACE);
        std::vector<size_t> lastOf(windows.size(), NO_FACE);
        std::vector<size_t> nextOf(faces.size(), NO_FACE);
        for (size_t i = 0; i < faces.size(); ++i) {
            const uint32_t home = homeOf[i];
            if (home == NO_WINDOW)
                continue;

            if (firstOf[home] == NO_FACE)
                firstOf[home] = i;
            else
                nextOf[lastOf[home]] = i;

            lastOf[home] = i;
        }

        const auto windowHeat = [&](size_t w, uint32_t reach) {
            int64_t heat = 0;
            for (size_t i = firstOf[w]; i != NO_FACE; i = nextOf[i])
                heat += FineFaceHeat(faces[i], reach);

            return heat;
        };

        bool changed = false;
        for (size_t w = 0; w < windows.size(); ++w) {
            FineWindow& window = windows[w];
            if (window.count == 0 || window.target <= window.progress)
                continue;

            const uint32_t speedStep = SpeedStepFor(window.widthMm);
            uint32_t reach = window.progress + std::min(window.target - window.progress, speedStep);
            if (windowHeat(w, reach) < 0) {
                uint32_t low = window.progress;
                uint32_t high = reach;
                for (uint32_t attempt = 0; attempt < 17; ++attempt) {
                    const uint32_t middle = low + (high - low + 1) / 2;
                    if (windowHeat(w, middle) >= 0)
                        low = middle;
                    else
                        high = middle - 1;
                }

                reach = low;
                ++m_ledger.heatLimitedSteps;
            }

            for (size_t i = firstOf[w]; i != NO_FACE; i = nextOf[i]) {
                const FineFace& face = faces[i];
                if (reach <= face.progress)
                    continue;

                const FaceKind& kind = m_kinds[face.kind];
                const int64_t heat = FineFaceHeat(face, reach);
                m_ledger.releasedPj += FxMulDivS64(face.forces.energyPj, reach - face.progress, face.forces.intact);
                m_ledger.surfacePj += SurfaceOf(reach, kind.surfacePj, kind.softening) -
                                      SurfaceOf(face.progress, kind.surfacePj, kind.softening);
                m_ledger.heatPj += heat;
                window.heatPj += heat;
                std::vector<uint32_t>& next = face.axis == 0 ? m_nextFineX : m_nextFineUp;
                next[face.key] = reach;
                next[face.pairKey] = reach;
                changed = true;
            }
        }

        for (const FineWindow& window : windows) {
            if (window.count != 0)
                m_ledger.worstStepHeatPj = std::min(m_ledger.worstStepHeatPj, window.heatPj);
        }

        return changed;
    }

    // --- 細かくする・戻す(状態だけで決まる。N1)---
    bool SupportFracture::Refine(const std::vector<FineFace>& faces) {
        std::vector<uint32_t> worst(m_scene.solid.size(), 0);
        for (const FineFace& face : faces) {
            if (!face.present)
                continue;

            const uint32_t index = IndexFromArea(face.edgePull, face.edgePush, face.axial, face.shear,
                                                 m_kinds[face.kind].areaMm2);
            for (const uint32_t cell : face.cells)
                worst[cell] = std::max(worst[cell], index);
        }

        // 分ける所: 線を超えた面の両側のセルから refineRing のセルまで(周りも細かくして、割れの面の続きと区間が細かい格子に入るように)
        std::vector<uint8_t> split(m_scene.solid.size(), 0);
        const auto ring = (int64_t)m_fracture.refineRing;
        for (uint32_t at = 0; at < worst.size(); ++at) {
            if (worst[at] < m_fracture.refineIndex)
                continue;

            const auto cx = (int64_t)(at % m_scene.width);
            const auto cy = (int64_t)(at / m_scene.width);
            for (int64_t y = std::max<int64_t>(cy - ring, 0); y <= std::min<int64_t>(cy + ring, m_scene.height - 1);
                 ++y) {
                for (int64_t x = std::max<int64_t>(cx - ring, 0); x <= std::min<int64_t>(cx + ring, m_scene.width - 1);
                     ++x)
                    split[(size_t)x + (size_t)m_scene.width * (size_t)y] = 1;
            }
        }

        // 戻さない所: 比が coarsenIndex 以上のセルから coarsenRing のセルまで(分ける所より広く・低い線で = 場所と線の両方に幅を持たせる)
        std::vector<uint8_t> hold(m_scene.solid.size(), 0);
        const auto holdRing = (int64_t)m_fracture.coarsenRing;
        for (uint32_t at = 0; at < worst.size(); ++at) {
            if (worst[at] < m_fracture.coarsenIndex)
                continue;

            const auto cx = (int64_t)(at % m_scene.width);
            const auto cy = (int64_t)(at / m_scene.width);
            for (int64_t y = std::max<int64_t>(cy - holdRing, 0);
                 y <= std::min<int64_t>(cy + holdRing, m_scene.height - 1); ++y) {
                for (int64_t x = std::max<int64_t>(cx - holdRing, 0);
                     x <= std::min<int64_t>(cx + holdRing, m_scene.width - 1); ++x)
                    hold[(size_t)x + (size_t)m_scene.width * (size_t)y] = 1;
            }
        }

        std::vector<uint8_t> next = m_scene.fine;
        bool changed = false;
        for (uint32_t up = 0; up < m_scene.height; ++up) {
            for (uint32_t x = 0; x < m_scene.width; ++x) {
                const size_t at = x + (size_t)m_scene.width * up;
                if (m_scene.solid[at] == 0)
                    continue;

                if (m_scene.fine[at] == 0) {
                    m_calmTicks[at] = 0;
                    if (split[at] != 0) {
                        next[at] = 1;
                        changed = true;
                    }

                    continue;
                }

                uint16_t& calm = m_calmTicks[at];
                const bool quiet = hold[at] == 0 && split[at] == 0;
                calm = quiet ? (uint16_t)std::min<uint32_t>(calm + 1u, UINT16_MAX) : 0;
                if (calm >= m_fracture.coarsenTicks && CanMerge(x, up)) {
                    next[at] = 0;
                    calm = 0;
                    changed = true;
                }
            }
        }

        m_scene.fine.swap(next);

        return changed;
    }

    // 分けたセルを戻してよいか: 細かいセルが全部ある・中の面が割れていない・外の面の 2 つの細かい面の進みが同じ
    bool SupportFracture::CanMerge(uint32_t x, uint32_t up) const {
        const uint32_t fx = 2 * x;
        const uint32_t fup = 2 * up;
        for (uint32_t part = 0; part < 4; ++part) {
            if (m_scene.fineAbsent[fx + (part & 1) + (size_t)2 * m_scene.width * (fup + (part >> 1))] != 0)
                return false;
        }

        if (FineProgress(fx, fup, 0) != 0 || FineProgress(fx, fup + 1, 0) != 0 || FineProgress(fx, fup, 1) != 0 ||
            FineProgress(fx + 1, fup, 1) != 0)
            return false;

        if (x > 0 && FineProgress(fx - 1, fup, 0) != FineProgress(fx - 1, fup + 1, 0))
            return false;

        if (FineProgress(fx + 1, fup, 0) != FineProgress(fx + 1, fup + 1, 0))
            return false;

        if (up > 0 && FineProgress(fx, fup - 1, 1) != FineProgress(fx + 1, fup - 1, 1))
            return false;

        return FineProgress(fx, fup + 1, 1) == FineProgress(fx + 1, fup + 1, 1);
    }

    // --- 進みを場面の面の残りの硬さへ(細かい面と粗い面)・帳簿の数 ---
    void SupportFracture::ApplyFineProgress() {
        const uint32_t fineWidth = 2 * m_scene.width;
        for (size_t at = 0; at < m_progressFineX.size(); ++at) {
            m_scene.intactFineX[at] = (uint32_t)ONE - m_progressFineX[at];
            m_scene.intactFineUp[at] = (uint32_t)ONE - m_progressFineUp[at];
        }

        // 粗い面は 1 つ目の細かい面の値(2 つはそろえて持つ)
        for (uint32_t up = 0; up < m_scene.height; ++up) {
            for (uint32_t x = 0; x < m_scene.width; ++x) {
                const size_t at = x + (size_t)m_scene.width * up;
                m_scene.intactX[at] = m_scene.intactFineX[2 * x + 1 + (size_t)fineWidth * 2 * up];
                m_scene.intactUp[at] = m_scene.intactFineUp[2 * x + (size_t)fineWidth * (2 * up + 1)];
            }
        }

        m_ledger.brokenFaces = 0;
        m_ledger.crackingFaces = 0;
        m_ledger.brokenAreaMm2 = 0;
        m_ledger.crackingAreaMm2 = 0;
        m_ledger.fineCells = 0;
        for (const FineFace& face : FineFaces(false)) {
            const uint32_t progress = (face.axis == 0 ? m_progressFineX : m_progressFineUp)[face.key];
            if (progress >= ONE) {
                ++m_ledger.brokenFaces;
                m_ledger.brokenAreaMm2 += m_kinds[face.kind].areaMm2;
            } else if (progress > 0) {
                ++m_ledger.crackingFaces;
                m_ledger.crackingAreaMm2 += m_kinds[face.kind].areaMm2;
            }
        }

        for (uint32_t fup = 0; fup < 2 * m_scene.height; ++fup) {
            for (uint32_t fx = 0; fx < fineWidth; ++fx)
                m_ledger.fineCells += SlotState(fx, fup) == 2 ? 1 : 0;
        }
    }

    // --- 固定した面から割れ切っていない面でたどれない所を分ける(細かい格子で。分けていないセルの中はつながっている)---
    void SupportFracture::SeparateFine() {
        const uint32_t fineWidth = 2 * m_scene.width;
        const uint32_t fineHeight = 2 * m_scene.height;
        std::vector<uint8_t> reached((size_t)fineWidth * fineHeight, 0);
        std::vector<uint32_t> queue;

        // 種: 固定した面のあるセル(分けたセルは向きの側の細かいセル = SupportLaw::AddFineClamps と同じ)
        for (uint32_t fup = 0; fup < fineHeight; ++fup) {
            for (uint32_t fx = 0; fx < fineWidth; ++fx) {
                const uint32_t state = SlotState(fx, fup);
                const uint8_t clamps = m_scene.clamps[fx / 2 + (size_t)m_scene.width * (fup / 2)];
                if (state == 0 || clamps == 0)
                    continue;

                const bool seed = state == 1 || ((clamps & SUPPORT_CLAMP_LEFT) != 0 && (fx & 1) == 0) ||
                                  ((clamps & SUPPORT_CLAMP_RIGHT) != 0 && (fx & 1) != 0) ||
                                  ((clamps & SUPPORT_CLAMP_BOTTOM) != 0 && (fup & 1) == 0);
                if (!seed)
                    continue;

                reached[fx + (size_t)fineWidth * fup] = 1;
                queue.push_back(fx + fineWidth * fup);
            }
        }

        const auto visit = [&](uint32_t to, uint32_t axis, uint32_t lowKey) {
            if (reached[to] != 0 || SlotState(to % fineWidth, to / fineWidth) == 0)
                return;

            if ((axis == 0 ? m_progressFineX : m_progressFineUp)[lowKey] >= ONE)
                return;  // 割れ切った面

            reached[to] = 1;
            queue.push_back(to);
        };
        for (size_t head = 0; head < queue.size(); ++head) {
            const uint32_t at = queue[head];
            const uint32_t fx = at % fineWidth;
            const uint32_t fup = at / fineWidth;
            if (fx + 1 < fineWidth)
                visit(at + 1, 0, at);

            if (fx > 0)
                visit(at - 1, 0, at - 1);

            if (fup + 1 < fineHeight)
                visit(at + fineWidth, 1, at);

            if (fup > 0)
                visit(at - fineWidth, 1, at - fineWidth);
        }

        for (uint32_t up = 0; up < m_scene.height; ++up) {
            for (uint32_t x = 0; x < m_scene.width; ++x) {
                const size_t at = x + (size_t)m_scene.width * up;
                if (m_scene.solid[at] == 0)
                    continue;

                const size_t corner = 2 * x + (size_t)fineWidth * 2 * up;
                if (m_scene.fine[at] == 0) {
                    if (reached[corner] != 0)
                        continue;

                    m_scene.solid[at] = 0;
                    m_separated[at] = 1;
                    ++m_ledger.separatedCells;
                    continue;
                }

                uint32_t absent = 0;
                for (uint32_t part = 0; part < 4; ++part) {
                    const size_t slot = corner + (part & 1) + (size_t)fineWidth * (part >> 1);
                    if (m_scene.fineAbsent[slot] == 0 && reached[slot] == 0) {
                        m_scene.fineAbsent[slot] = 1;
                        ++m_ledger.separatedCells;
                    }

                    absent += m_scene.fineAbsent[slot] != 0 ? 1 : 0;
                }

                if (absent < 4)
                    continue;

                // 細かいセルが全部分かれた: セルごと無くす
                m_scene.solid[at] = 0;
                m_scene.fine[at] = 0;
                m_separated[at] = 1;
                for (uint32_t part = 0; part < 4; ++part)
                    m_scene.fineAbsent[corner + (part & 1) + (size_t)fineWidth * (part >> 1)] = 0;
            }
        }
    }

}  // namespace bicameral::sim
