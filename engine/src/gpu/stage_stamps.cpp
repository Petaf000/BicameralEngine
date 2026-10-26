// stage_stamps.cpp — 段ごとの GPU 時間を測る印(T-1161)。使い方は stage_stamps.h。
#include "gpu/stage_stamps.h"

#include <algorithm>

#include "core/aliases.h"
#include "gpu/resources.h"

namespace bicameral::gpu {

    namespace {

        constexpr uint64_t TIMESTAMP_BYTES = 8;

    }  // namespace

    std::expected<StageStamps, std::string> StageStamps::Create(ID3D12Device* device, uint32_t slots, uint32_t capacity,
                                                                uint32_t stages) {
        if (slots == 0 || capacity == 0 || stages == 0)
            return std::unexpected("段の印: フレーム・容量・段の数は 1 以上");

        StageStamps stamps;
        stamps.m_capacity = capacity;
        const D3D12_QUERY_HEAP_DESC desc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = slots * capacity};
        if (FAILED(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&stamps.m_heap))))
            return std::unexpected("段の印のタイムスタンプのヒープを作れない");

        for (uint32_t slot = 0; slot < slots; ++slot) {
            ComPtr<ID3D12Resource> readback = CreateBuffer(device, uint64_t{capacity} * TIMESTAMP_BYTES,
                                                           BufferKind::Readback);
            if (!readback)
                return std::unexpected("段の印の読み戻しを作れない");

            stamps.m_readbacks.push_back(std::move(readback));
        }

        stamps.m_stages.resize(slots);
        stamps.m_ticks.assign(stages, 0);
        stamps.m_counts.assign(stages, 0);

        return stamps;
    }

    void StageStamps::Begin(uint32_t slot) {
        m_slot = slot;
        m_stages[slot].clear();
    }

    void StageStamps::Mark(ID3D12GraphicsCommandList* list, uint8_t stage) {
        std::vector<uint8_t>& stages = m_stages[m_slot];
        if (stages.size() >= m_capacity || stage >= m_ticks.size()) {
            ++m_dropped;
            return;
        }

        list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                       (m_slot * m_capacity) + static_cast<uint32_t>(stages.size()));
        stages.push_back(stage);
    }

    void StageStamps::Resolve(ID3D12GraphicsCommandList* list) const {
        const auto count = static_cast<uint32_t>(m_stages[m_slot].size());
        if (count == 0)
            return;

        list->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_slot * m_capacity, count,
                               m_readbacks[m_slot].Get(), 0);
    }

    void StageStamps::Collect(uint32_t slot) {
        const std::vector<uint8_t>& stages = m_stages[slot];
        if (stages.empty())
            return;

        std::vector<uint64_t> stamps(stages.size());
        if (!ReadBuffer(m_readbacks[slot].Get(), std::as_writable_bytes(std::span(stamps))))
            return;

        // 隣り合う印の差を後ろの印の段へ(START の印と、その前の印との差は取らない)
        for (size_t index = 1; index < stages.size(); ++index) {
            const uint8_t stage = stages[index];
            if (stage == START || stamps[index] < stamps[index - 1])
                continue;

            m_ticks[stage] += stamps[index] - stamps[index - 1];
            ++m_counts[stage];
        }
    }

    void StageStamps::Reset() {
        rng::fill(m_ticks, 0);
        rng::fill(m_counts, 0);
        m_dropped = 0;
    }

}  // namespace bicameral::gpu
