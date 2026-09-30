// graph_trace.cpp — 連鎖のトレースのバッファ・範囲の書き込み・読み戻し(T-0087・T-0088、16 §1.3)。使い方は graph_trace.h。
#include "gpu/graph_trace.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "core/aliases.h"
#include "gpu/resources.h"

namespace bicameral::gpu {
    namespace {

        // 見出しの範囲の部分(語 [4, 16))の中身
        std::array<uint32_t, GT_FILTER_BYTES / 4> FilterWords(const GraphTraceFilter& filter) {
            std::array<uint32_t, GT_FILTER_BYTES / 4> words{};
            const auto at = [&words](uint32_t word) -> uint32_t& {
                return words[word - GT_FILTER_OFFSET / 4];
            };

            at(GT_WORD_ENABLED) = filter.enabled ? 1 : 0;
            at(GT_WORD_CAPACITY) = filter.enabled ? filter.capacity : 0;
            at(GT_WORD_TICK_BEGIN) = static_cast<uint32_t>(filter.tickBegin);
            at(GT_WORD_TICK_BEGIN + 1) = static_cast<uint32_t>(filter.tickBegin >> 32);
            at(GT_WORD_TICK_END) = static_cast<uint32_t>(filter.tickEnd);
            at(GT_WORD_TICK_END + 1) = static_cast<uint32_t>(filter.tickEnd >> 32);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                at(GT_WORD_BOX_MIN + axis) = filter.boxMin[axis];
                at(GT_WORD_BOX_MAX + axis) = filter.boxMax[axis];
            }

            return words;
        }

        GraphTraceRecord DecodeRecord(std::span<const uint32_t, 4> words) {
            return {.tick = uint64_t{words[0]} | (uint64_t{words[1]} << 32),
                    .kind = words[2] >> GT_SUBJECT_BITS,
                    .subject = words[2] & GT_SUBJECT_MASK,
                    .object = words[3]};
        }

    }  // namespace

    void SortGraphTrace(std::vector<GraphTraceRecord>& records) {
        rng::sort(records);
    }

    std::expected<GraphTrace, std::string> GraphTrace::Create(ID3D12Device* device, const GraphTraceFilter& filter,
                                                              uint32_t slotCount, uint32_t capacity) {
        capacity = std::max(capacity, filter.enabled ? filter.capacity : 0u);
        const uint64_t bytes = GT_HEADER_BYTES + uint64_t{capacity} * GT_RECORD_BYTES;

        // 毎フレーム 0 に戻すのは数(先頭の GT_RESET_BYTES)だけ。範囲は残す
        auto ring = ReadbackRing::Create(device, bytes, GT_RESET_BYTES, slotCount, L"GraphTrace");
        if (!ring)
            return std::unexpected("連鎖のトレースを作れない: " + ring.error());

        // --- 範囲の写し元(slot ごと。RecordBegin が範囲の変わったフレームに書いて GPU のバッファへ写す)---
        ComPtr<ID3D12Resource> upload = CreateBuffer(device, uint64_t{GT_FILTER_BYTES} * slotCount, BufferKind::Upload);
        if (!upload)
            return std::unexpected("連鎖のトレースの範囲のバッファを作れない");

        void* mapped = nullptr;
        const D3D12_RANGE noRead{};
        if (FAILED(upload->Map(0, &noRead, &mapped)))
            return std::unexpected("連鎖のトレースの範囲のバッファを Map できない");

        upload->SetName(L"GraphTrace.filter");

        GraphTrace trace(std::move(*ring), std::move(upload), static_cast<std::byte*>(mapped), capacity, slotCount);
        trace.SetFilter(filter);

        return trace;
    }

    void GraphTrace::SetFilter(const GraphTraceFilter& filter) {
        m_filter = filter;
        m_filter.capacity = filter.enabled ? std::min(filter.capacity, m_capacity) : 0;
        m_filterDirty = true;
    }

    void GraphTrace::RecordBegin(ID3D12GraphicsCommandList* list, uint32_t slot) {
        if (m_filterDirty) {
            // slot の写し元は、前にその slot で記録したリストが終わっているので書いてよい(ReadbackRing の slot と同じ約束)
            const auto words = FilterWords(m_filter);
            std::memcpy(m_filterMapped + size_t{slot} * GT_FILTER_BYTES, words.data(), sizeof(words));

            // バッファは COMMON(作った時・前のリストの終わり)。範囲を写して COMMON に戻す
            // (ReadbackRing::RecordBegin が COMMON → UAV で始めるので)
            ID3D12Resource* buffer = m_ring.Buffer();
            const D3D12_RESOURCE_BARRIER toCopyDest = Transition(buffer, D3D12_RESOURCE_STATE_COMMON,
                                                                 D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &toCopyDest);
            list->CopyBufferRegion(buffer, GT_FILTER_OFFSET, m_filterUpload.Get(), uint64_t{slot} * GT_FILTER_BYTES,
                                   GT_FILTER_BYTES);

            const D3D12_RESOURCE_BARRIER toCommon = Transition(buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                               D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(1, &toCommon);
            m_filterDirty = false;
        }

        m_slotFilters[slot] = m_filter;
        m_ring.RecordBegin(list);
    }

    void GraphTrace::RecordReadbackAndReset(ID3D12GraphicsCommandList* list, uint32_t slot) const {
        // 範囲が無効のリストは何も書かないので、見出しだけを読み戻す(容量ぶんを毎フレーム写さない)
        const uint32_t capacity = m_slotFilters[slot].capacity;
        m_ring.RecordReadbackAndReset(list, slot, GT_HEADER_BYTES + uint64_t{capacity} * GT_RECORD_BYTES);
    }

    std::expected<GraphTraceFrame, std::string> GraphTrace::Read(uint32_t slot) const {
        // --- 見出し(書こうとした数)→ 書けた分の記録 ---
        std::array<uint32_t, GT_HEADER_BYTES / 4> header{};
        if (!m_ring.Read(slot, std::as_writable_bytes(std::span(header))))
            return std::unexpected(std::format("連鎖のトレースを読み戻せない(slot {})", slot));

        const uint32_t requested = header[GT_WORD_REQUESTED];
        const uint32_t written = std::min(requested, m_slotFilters[slot].capacity);
        GraphTraceFrame frame{.droppedCount = requested - written};
        if (written == 0)
            return frame;

        std::vector<uint32_t> words((GT_HEADER_BYTES + size_t{written} * GT_RECORD_BYTES) / 4);
        if (!m_ring.Read(slot, std::as_writable_bytes(std::span(words))))
            return std::unexpected(std::format("連鎖のトレースの記録を読み戻せない(slot {})", slot));

        frame.records.reserve(written);
        for (uint32_t index = 0; index < written; ++index) {
            const size_t first = (GT_HEADER_BYTES + size_t{index} * GT_RECORD_BYTES) / 4;
            frame.records.push_back(DecodeRecord(std::span(words).subspan(first).first<4>()));
        }

        return frame;
    }

}  // namespace bicameral::gpu
