// graph_trace.h — Work Graphs の連鎖のトレース(shaders/common/graph_trace.hlsli)を CPU 側で持つ(T-0087・T-0088、16 §1.3)。
//
// データの流れ: 範囲(刻み・場所の箱)を SetFilter で決める(作った時の範囲が最初)→ 次の RecordBegin() がそれを GPU のバッファの見出しへ写す
//   → ノードが GtRecord などで記録を追記する → RecordReadbackAndReset() が slot の読み戻しのバッファへ写して数を 0 に戻す
//   → その slot のリストが終わった後に Read() → 記録の列(atomic の順。比べる前に SortGraphTrace で並べる)。
// 記録の種類・主・従の意味は使う側が決める(伝導は sim/probe_trace.h)。
// 容量(1 フレームに書ける記録の数 = バッファの大きさ)は作った時に決め、変えない。範囲は実行中に何度でも変えられる(T-0088)。
// 範囲が無効のフレームは、シェーダーが見出しの 1 語を読むだけで何も書かず、読み戻しも見出し(64 バイト)だけ。
//
// 1 本のコマンドリストの中での使い方:
//   trace.RecordBegin(list, slot);                                              // (範囲が変わっていれば写す)COMMON → UAV
//   list->SetComputeRootUnorderedAccessView(layout.GraphTraceIndex(), trace.GpuAddress());
//   ... DispatchGraph / Dispatch ...
//   trace.RecordReadbackAndReset(list, slot);
//   (slot のリストが終わったら)auto frame = trace.Read(slot);
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "common/graph_trace.hlsli"
#include "gpu/com_ptr.h"
#include "gpu/readback_ring.h"

namespace bicameral::gpu {

    // 記録する範囲。刻みは [tickBegin, tickEnd)、場所は箱 [boxMin, boxMax)(場所の単位は使う側が決める)
    struct GraphTraceFilter {
        bool enabled = false;
        uint32_t capacity = 0;  // 1 フレームに書ける記録の数(有効なときだけ使う。GraphTrace の容量までに切り詰める)

        // --- 範囲 ---
        uint64_t tickBegin = 0;
        uint64_t tickEnd = std::numeric_limits<uint64_t>::max();
        std::array<uint32_t, 3> boxMin = {0, 0, 0};
        std::array<uint32_t, 3> boxMax = {std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(),
                                          std::numeric_limits<uint32_t>::max()};
    };

    struct GraphTraceRecord {
        uint64_t tick = 0;
        uint32_t kind = 0;     // 種類(8bit)
        uint32_t subject = 0;  // 主(24bit)
        uint32_t object = 0;   // 従(32bit)

        // 並べる順 = (刻み, 種類, 主, 従)。atomic の順によらない比べ方の基準
        friend auto operator<=>(const GraphTraceRecord&, const GraphTraceRecord&) = default;
    };

    struct GraphTraceFrame {
        std::vector<GraphTraceRecord> records;  // 書けた分(atomic の順)
        uint32_t droppedCount = 0;              // 容量を越えて書けなかった数(> 0 ならトレースは欠けている)
    };

    // (刻み, 種類, 主, 従)の順に並べる。重なり(同じ記録が複数)は残す
    void SortGraphTrace(std::vector<GraphTraceRecord>& records);

    class GraphTrace {
    public:
        // filter: 最初の範囲。capacity: 1 フレームに書ける記録の上限(バッファの大きさ)。
        // 容量は max(capacity, 有効なら filter.capacity)。実行中に範囲を変えるなら、使いそうな上限ぶんを capacity に渡す
        [[nodiscard]] static std::expected<GraphTrace, std::string> Create(ID3D12Device* device,
                                                                           const GraphTraceFilter& filter,
                                                                           uint32_t slotCount = 1,
                                                                           uint32_t capacity = 0);

        // ルートの UAV(u2 space1)に渡すアドレス
        [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS GpuAddress() const { return m_ring.GpuAddress(); }
        [[nodiscard]] const GraphTraceFilter& Filter() const { return m_filter; }  // 次の RecordBegin から使う範囲
        [[nodiscard]] bool Enabled() const { return m_filter.enabled; }
        [[nodiscard]] uint32_t Capacity() const { return m_capacity; }

        // 範囲を変える。次の RecordBegin で GPU へ写し、そのリストから効く(容量は Capacity() までに切り詰める)
        void SetFilter(const GraphTraceFilter& filter);

        // シェーダーが書く前に: (範囲が変わっていれば見出しへ写す)COMMON → UNORDERED_ACCESS。slot は RecordReadbackAndReset と同じ
        void RecordBegin(ID3D12GraphicsCommandList* list, uint32_t slot = 0);

        // 書いた後に: slot の読み戻しのバッファへ写し(範囲が無効なら見出しだけ)、書こうとした数を 0 に戻す(範囲は残す)。
        // 最後は COMMON に戻す
        void RecordReadbackAndReset(ID3D12GraphicsCommandList* list, uint32_t slot = 0) const;

        // slot のリストを GPU が終えた後に呼ぶ(そのリストを記録した時の範囲で読む)
        [[nodiscard]] std::expected<GraphTraceFrame, std::string> Read(uint32_t slot = 0) const;

    private:
        GraphTrace(ReadbackRing&& ring, ComPtr<ID3D12Resource> filterUpload, std::byte* filterMapped, uint32_t capacity,
                   uint32_t slotCount)
            : m_ring(std::move(ring)),
              m_filterUpload(std::move(filterUpload)),
              m_filterMapped(filterMapped),
              m_capacity(capacity),
              m_slotFilters(slotCount) {}

        // --- GPU ---
        ReadbackRing m_ring;
        ComPtr<ID3D12Resource> m_filterUpload;  // 範囲の写し元(slot ごとに GT_FILTER_BYTES。Map したまま)
        std::byte* m_filterMapped = nullptr;
        uint32_t m_capacity = 0;  // 1 フレームに書ける記録の数(作った時に決める)

        // --- 範囲 ---
        GraphTraceFilter m_filter;                    // 次の RecordBegin から使う範囲(容量は切り詰め済み)
        bool m_filterDirty = true;                    // m_filter をまだ GPU のバッファへ写していない
        std::vector<GraphTraceFilter> m_slotFilters;  // slot のリストを記録した時の範囲(Read と読み戻しの大きさ)
    };

}  // namespace bicameral::gpu
