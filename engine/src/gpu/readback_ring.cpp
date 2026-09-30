// readback_ring.cpp — 待たない読み戻し(T-0004)。使い方は readback_ring.h。
#include "gpu/readback_ring.h"

#include <algorithm>

#include "gpu/com_ptr.h"
#include "gpu/resources.h"

namespace bicameral::gpu {

    std::expected<ReadbackRing, std::string> ReadbackRing::Create(ID3D12Device* device, uint64_t bytes,
                                                                  uint64_t headerBytes, uint32_t slotCount,
                                                                  std::wstring_view name) {
        ReadbackRing ring;
        ring.m_bytes = bytes;
        ring.m_headerBytes = headerBytes;
        ring.m_buffer = CreateBuffer(device, bytes, BufferKind::UnorderedAccess);
        ring.m_zeros = CreateBuffer(device, headerBytes, BufferKind::UnorderedAccess);
        if (!ring.m_buffer || !ring.m_zeros)
            return std::unexpected("読み戻しのリングのバッファを作れない");

        ring.m_buffer->SetName(std::wstring(name).c_str());
        ring.m_zeros->SetName((std::wstring(name) + L".zeros").c_str());
        for (uint32_t slot = 0; slot < slotCount; ++slot) {
            ComPtr<ID3D12Resource> readback = CreateBuffer(device, bytes, BufferKind::Readback);
            if (!readback)
                return std::unexpected("読み戻しのリングの読み戻しのバッファを作れない");

            readback->SetName(std::format(L"{}.readback{}", name, slot).c_str());
            ring.m_readbacks.push_back(std::move(readback));
        }

        return ring;
    }

    void ReadbackRing::RecordBegin(ID3D12GraphicsCommandList* list) const {
        const D3D12_RESOURCE_BARRIER barrier = Transition(m_buffer.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &barrier);
    }

    void ReadbackRing::RecordReadbackAndReset(ID3D12GraphicsCommandList* list, uint32_t slot,
                                              uint64_t copyBytes) const {
        ID3D12Resource* buffer = m_buffer.Get();
        const D3D12_RESOURCE_BARRIER toCopySource = Transition(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &toCopySource);
        list->CopyBufferRegion(m_readbacks[slot].Get(), 0, buffer, 0, std::min(copyBytes, m_bytes));

        const D3D12_RESOURCE_BARRIER toCopyDest = Transition(buffer, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                             D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &toCopyDest);
        list->CopyBufferRegion(buffer, 0, m_zeros.Get(), 0, m_headerBytes);  // 書こうとした数を 0 に

        // COMMON に戻す: 次のリストが同じ ExecuteCommandLists の中でも RecordBegin() から始められるように
        const D3D12_RESOURCE_BARRIER toCommon = Transition(buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                           D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1, &toCommon);
    }

    bool ReadbackRing::Read(uint32_t slot, std::span<std::byte> destination) const {
        return ReadBuffer(m_readbacks[slot].Get(), destination);
    }

}  // namespace bicameral::gpu
