// graph_trace.h — Work Graphs の連鎖のトレース(shaders/common/graph_trace.hlsli)を CPU 側で持つ(T-0087、16 §1.3)。
//
// データの流れ: 作った時に範囲(刻み・場所の箱)を決める → 最初の RecordBegin() がそれを GPU のバッファの見出しへ写す
//   → ノードが GtRecord などで記録を追記する → RecordReadbackAndReset() が slot の読み戻しのバッファへ写して数を 0 に戻す
//   → その slot のリストが終わった後に Read() → 記録の列(atomic の順。比べる前に SortGraphTrace で並べる)。
// 記録の種類・主・従の意味は使う側が決める(伝導は sim/probe_trace.h)。範囲を無効にして作ると容量 0(見出しだけ)になり、
// シェーダーは見出しの 1 語を読むだけで何も書かない。
//
// 1 本のコマンドリストの中での使い方:
//   trace.RecordBegin(list);                                                     // (最初だけ範囲を写す)COMMON → UAV
//   list->SetComputeRootUnorderedAccessView(layout.GraphTraceIndex(), trace.GpuAddress());
//   ... DispatchGraph / Dispatch ...
//   trace.RecordReadbackAndReset(list, slot);
//   (slot のリストが終わったら)auto frame = trace.Read(slot);
#pragma once

#include <array>
#include <compare>
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
        uint32_t capacity = 0;  // 1 フレームに書ける記録の数(有効なときだけ使う)

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
        [[nodiscard]] static std::expected<GraphTrace, std::string> Create(ID3D12Device* device,
                                                                           const GraphTraceFilter& filter,
                                                                           uint32_t slotCount = 1);

        // ルートの UAV(u2 space1)に渡すアドレス
        [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS GpuAddress() const { return m_ring.GpuAddress(); }
        [[nodiscard]] const GraphTraceFilter& Filter() const { return m_filter; }
        [[nodiscard]] bool Enabled() const { return m_filter.enabled; }

        // シェーダーが書く前に: (最初の 1 回だけ範囲を見出しへ写す)COMMON → UNORDERED_ACCESS
        void RecordBegin(ID3D12GraphicsCommandList* list);

        // 書いた後に: slot の読み戻しのバッファへ写し、書こうとした数を 0 に戻す(範囲は残す)。最後は COMMON に戻す
        void RecordReadbackAndReset(ID3D12GraphicsCommandList* list, uint32_t slot = 0) const;

        // slot のリストを GPU が終えた後に呼ぶ
        [[nodiscard]] std::expected<GraphTraceFrame, std::string> Read(uint32_t slot = 0) const;

    private:
        GraphTrace(ReadbackRing&& ring, ComPtr<ID3D12Resource> filterUpload, const GraphTraceFilter& filter)
            : m_ring(std::move(ring)), m_filterUpload(std::move(filterUpload)), m_filter(filter) {}

        ReadbackRing m_ring;
        ComPtr<ID3D12Resource> m_filterUpload;  // 範囲の写し元(GT_FILTER_BYTES。作った時に書いたまま)
        GraphTraceFilter m_filter;
        bool m_filterWritten = false;  // 範囲を GPU のバッファへ写すコマンドを記録したか
    };

}  // namespace bicameral::gpu
