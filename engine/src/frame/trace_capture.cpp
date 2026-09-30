// trace_capture.cpp — 連鎖のトレースを集めてファイルに書く(T-0087・T-0088)。使い方は trace_capture.h。
#include "frame/trace_capture.h"

#include <algorithm>
#include <format>
#include <limits>
#include <system_error>

#include "core/log.h"
#include "core/unicode.h"
#include "sim/probe_trace.h"

namespace bicameral::frame {

    void TraceCapture::Add(std::span<const gpu::GraphTraceRecord> records, uint32_t droppedCount) {
        m_droppedCount += droppedCount;

        const size_t room = MAX_RECORDS - std::min(m_records.size(), MAX_RECORDS);
        const size_t taken = std::min(room, records.size());
        m_records.insert(m_records.end(), records.begin(), records.begin() + static_cast<ptrdiff_t>(taken));
        m_droppedCount += records.size() - taken;
    }

    bool TraceCapture::IsComplete(uint64_t latestStateTick) const {
        if (m_filter.tickEnd == std::numeric_limits<uint64_t>::max())
            return false;

        return latestStateTick >= m_filter.tickEnd;  // S(tickEnd) を読んだ = 刻み tickEnd − 1 まで終わった
    }

    std::expected<void, std::string> TraceCapture::Write() {
        const std::string pathText = ToUtf8(m_path.wstring());
        if (m_path.has_parent_path()) {
            std::error_code error;
            fs::create_directories(m_path.parent_path(), error);
            if (error)
                return std::unexpected(std::format("トレースのフォルダを作れない: {}", pathText));
        }

        const size_t recordCount = m_records.size();
        const auto written = sim::WriteProbeTraceFile(m_path, std::move(m_records), m_filter, m_droppedCount);
        m_records = {};
        if (!written)
            return std::unexpected(std::format("{}: {}", written.error(), pathText));

        Log(Channel::Sim, m_droppedCount > 0 ? Level::Warning : Level::Info, "トレース: {}(記録 {} 件{})", pathText,
            recordCount,
            m_droppedCount > 0 ? std::format("、落とした {} 件。範囲を狭めると欠けない", m_droppedCount)
                               : std::string());

        return {};
    }

}  // namespace bicameral::frame
