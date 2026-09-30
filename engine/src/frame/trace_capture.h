// trace_capture.h — 伝導の連鎖のトレースを 1 回ぶん集めて、刻みごとの木のファイルに書く(T-0087 の --trace・T-0088 の窓のキー)。
//
// データの流れ: フレームのループが範囲を ProbeSim::SetTraceFilter で GPU に渡し、同じ範囲でこれを作る
//   → 読み戻したフレームごとに Add(記録・落とした数)→ 範囲の最後の刻みの状態のハッシュを読んだら IsComplete
//   → Write(sim::WriteProbeTraceFile。並べて木にする)。
// GPU への範囲の受け渡しは持たない(フレームのループの仕事)。ここは CPU の View の側の入れ物。
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "core/aliases.h"
#include "gpu/graph_trace.h"

namespace bicameral::frame {

    class TraceCapture {
    public:
        static constexpr size_t MAX_RECORDS = size_t{1} << 22;  // 集める記録の上限(1 件 24 バイト。約 100 MB)

        // path: 書くファイル。filter: 集める範囲(ファイルの見出しと、終わったかどうかの判定に使う)
        TraceCapture(fs::path path, const gpu::GraphTraceFilter& filter) : m_path(std::move(path)), m_filter(filter) {}

        // 読み戻した 1 フレームぶん。上限を越えた分は落とした数に数える
        void Add(std::span<const gpu::GraphTraceRecord> records, uint32_t droppedCount);

        // 範囲の刻みを全部読み戻したか。latestStateTick = 最後に読み戻したハッシュの状態 S(t) の t(刻み t − 1 まで終わった)。
        // 範囲の終わりが無限なら終わらない(終わるときに書く)
        [[nodiscard]] bool IsComplete(uint64_t latestStateTick) const;

        // 刻みごとの木にしてファイルへ書き(フォルダが無ければ作る)、ログに場所と件数を出す。集めた記録は手放す
        [[nodiscard]] std::expected<void, std::string> Write();

        [[nodiscard]] const fs::path& Path() const { return m_path; }
        [[nodiscard]] const gpu::GraphTraceFilter& Filter() const { return m_filter; }

    private:
        fs::path m_path;
        gpu::GraphTraceFilter m_filter;
        std::vector<gpu::GraphTraceRecord> m_records;  // 順不同(atomic の順)
        uint64_t m_droppedCount = 0;                   // GPU の容量・ここの上限で落とした数
    };

}  // namespace bicameral::frame
