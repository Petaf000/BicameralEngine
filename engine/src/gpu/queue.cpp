// queue.cpp — コマンドキューとフェンス(T-0004)。使い方は queue.h。
#include "gpu/queue.h"

#include "core/hresult.h"
#include "core/log.h"

using Microsoft::WRL::ComPtr;

namespace bicameral::gpu {

    std::expected<Queue, std::string> Queue::Create(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type,
                                                    std::wstring_view name, D3D12_COMMAND_QUEUE_PRIORITY priority) {
        Queue queue;
        const D3D12_COMMAND_QUEUE_DESC desc{.Type = type, .Priority = priority};
        HRESULT result = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue.m_queue));
        if (FAILED(result)) return std::unexpected("コマンドキューを作れない: " + DescribeHresult(result));
        result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue.m_fence));
        if (FAILED(result)) return std::unexpected("フェンスを作れない: " + DescribeHresult(result));
        queue.m_queue->SetName(std::wstring(name).c_str());
        queue.m_fence->SetName((std::wstring(name) + L".fence").c_str());
        return queue;
    }

    uint64_t Queue::Submit(std::span<ID3D12CommandList* const> lists) {
        if (!lists.empty()) m_queue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
        ++m_lastSubmitted;
        (void)BICAMERAL_CHECK_HR(Channel::Gpu, m_queue->Signal(m_fence.Get(), m_lastSubmitted));
        return m_lastSubmitted;
    }

    void Queue::GpuWait(const Queue& other, uint64_t value) const {
        if (value == 0) return;
        (void)BICAMERAL_CHECK_HR(Channel::Gpu, m_queue->Wait(other.m_fence.Get(), value));
    }

    bool Queue::WaitCpu(uint64_t value) const {
        if (IsDeviceLost()) return false;
        if (IsComplete(value)) return true;
        // イベントに nullptr を渡すと、終わるまでこの呼び出しの中で待つ
        if (!BICAMERAL_CHECK_HR(Channel::Gpu, m_fence->SetEventOnCompletion(value, nullptr))) return false;
        return !IsDeviceLost();
    }

    uint64_t Queue::TimestampFrequency() const {
        uint64_t frequency = 0;
        if (FAILED(m_queue->GetTimestampFrequency(&frequency))) return 0;
        return frequency;
    }

}  // namespace bicameral::gpu
