// tick_cost.cpp — 単位の GPU 時間を刻みごとにまとめる(T-0413・ADR-0204)。約束は tick_cost.h。
#include "sim/tick_cost.h"

namespace bicameral::sim {

    bool TickCostAccumulator::Add(uint32_t firstUnit, std::span<const uint64_t> unitGpuTicks,
                                  std::span<const ProbeTickHash> hashes, std::vector<TickCost>& out) {
        // 前のフレームの続きでなければ(読み落とし・戻した後)、途中の刻みは数えない
        if (firstUnit != m_nextUnit)
            m_partialValid = false;

        // --- 終えた刻みの GPU の時間(数えられない刻みは UINT64_MAX)---
        std::vector<uint64_t> finished;
        for (size_t index = 0; index < unitGpuTicks.size(); ++index) {
            const auto unit = static_cast<uint32_t>((firstUnit + index) % m_unitsPerTick);
            if (unit == 0) {
                m_partial = 0;
                m_partialValid = true;
            }

            m_partial += unitGpuTicks[index];
            if (unit + 1 == m_unitsPerTick)
                finished.push_back(m_partialValid ? m_partial : UINT64_MAX);
        }

        m_nextUnit = static_cast<uint32_t>((firstUnit + unitGpuTicks.size()) % m_unitsPerTick);

        // --- 要約と組にする ---
        if (finished.size() != hashes.size())
            return false;

        for (size_t index = 0; index < finished.size(); ++index) {
            if (finished[index] == UINT64_MAX)
                continue;

            out.push_back({.tick = hashes[index].tick,
                           .gpuTimestampTicks = finished[index],
                           .scheduledBlocks = hashes[index].scheduledBlocks});
        }

        return true;
    }

    void TickCostAccumulator::Reset() {
        m_nextUnit = 0;
        m_partial = 0;
        m_partialValid = true;
    }

}  // namespace bicameral::sim
