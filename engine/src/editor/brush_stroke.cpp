// brush_stroke.cpp — 筆をドラッグして塗る時の置く中心の列(T-0245)。流れと間隔の決め方は brush_stroke.h。
#include "editor/brush_stroke.h"

#include <algorithm>
#include <cstdlib>

namespace bicameral::editor {
    namespace {

        // 2 つのセルの軸ごとの差(to − from)
        std::array<int32_t, 3> Delta(const BrushCell& from, const BrushCell& to) {
            return {static_cast<int32_t>(to[0]) - static_cast<int32_t>(from[0]),
                    static_cast<int32_t>(to[1]) - static_cast<int32_t>(from[1]),
                    static_cast<int32_t>(to[2]) - static_cast<int32_t>(from[2])};
        }

        // いちばん大きく動く軸(同じなら x → y → z の先の方)
        size_t MajorAxis(const std::array<int32_t, 3>& delta) {
            size_t axis = 0;
            for (size_t i = 1; i < delta.size(); ++i) {
                if (std::abs(delta[i]) > std::abs(delta[axis]))
                    axis = i;
            }

            return axis;
        }

    }  // namespace

    uint32_t BrushStroke::Budget(size_t waitingCommands) {
        if (waitingCommands >= MAX_WAITING_COMMANDS)
            return 0;

        return static_cast<uint32_t>(std::min<size_t>(MAX_DABS_PER_FRAME, MAX_WAITING_COMMANDS - waitingCommands));
    }

    uint32_t BrushStroke::Spacing(const BrushCell& from, const BrushCell& to, const BrushExtents& extents, bool add) {
        const uint32_t extent = extents[MajorAxis(Delta(from, to))];

        return add ? (2 * extent) + 1 : std::max(1u, extent);
    }

    std::vector<BrushCell> BrushStroke::Paint(std::span<const BrushCell> pressed, std::span<const BrushCell> dragged,
                                              bool released, const BrushExtents& extents, bool add, uint32_t budget) {
        std::vector<BrushCell> dabs;

        // --- 押した: 新しい筆の運び(前の運びの残りは捨てる)。置けない時は始めない ---
        for (const BrushCell& cell : pressed) {
            if (budget == 0) {
                m_active = false;
                continue;
            }

            dabs.push_back(cell);
            budget -= 1;
            m_active = true;
            m_released = false;
            m_last = cell;
            m_target = cell;
        }

        if (!m_active)
            return dabs;

        // --- 押したまま通ったセルへ順に(置ききれなければ最後のセルを目標に残す)---
        for (const BrushCell& cell : dragged) {
            WalkToTarget(extents, add, budget, dabs);
            m_target = cell;
        }

        WalkToTarget(extents, add, budget, dabs);
        m_released = m_released || released;

        // --- 離していて、目標の手前の間隔の中まで置き終えたら終わる ---
        if (m_released && TargetReached(extents, add))
            m_active = false;

        return dabs;
    }

    // 最後に置いた中心から目標へ、いちばん大きく動く軸で間隔ずつ進んだ所に置く(ほかの軸は比で割り、0 の方向へ切る)
    void BrushStroke::WalkToTarget(const BrushExtents& extents, bool add, uint32_t& budget,
                                   std::vector<BrushCell>& dabs) {
        while (budget > 0 && !TargetReached(extents, add)) {
            const std::array<int32_t, 3> delta = Delta(m_last, m_target);
            const int32_t distance = std::abs(delta[MajorAxis(delta)]);
            const auto spacing = static_cast<int32_t>(Spacing(m_last, m_target, extents, add));
            for (size_t i = 0; i < delta.size(); ++i)
                m_last[i] = static_cast<uint32_t>(static_cast<int32_t>(m_last[i]) + (delta[i] * spacing / distance));

            dabs.push_back(m_last);
            budget -= 1;
        }
    }

    // 目標が最後に置いた中心から間隔より近い(もう置かない。同じセルなら距離 0 < 間隔 1 以上)
    bool BrushStroke::TargetReached(const BrushExtents& extents, bool add) const {
        const std::array<int32_t, 3> delta = Delta(m_last, m_target);
        const auto distance = static_cast<uint32_t>(std::abs(delta[MajorAxis(delta)]));

        return distance < Spacing(m_last, m_target, extents, add);
    }

}  // namespace bicameral::editor
