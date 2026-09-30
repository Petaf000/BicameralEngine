// immediate_queue.cpp — 1 本記録して投げて待つキュー(T-0013)。
// 待つのは ID3D12Fence::SetEventOnCompletion にイベントを渡さない形(終わるまで呼んだスレッドを止める)。
#include "gpu/immediate_queue.h"

#include "core/aliases.h"
#include "core/hresult.h"
#include "core/log.h"
#include "gpu/com_ptr.h"
#include "gpu/device.h"

namespace bicameral::gpu {

    expected<ImmediateQueue, std::string> ImmediateQueue::Create(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE type) {
        ImmediateQueue queue;
        const D3D12_COMMAND_QUEUE_DESC queueDesc{.Type = type};
        if (!BICAMERAL_CHECK_HR(Channel::Gpu, device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue.m_queue))) ||
            !BICAMERAL_CHECK_HR(Channel::Gpu, device->CreateCommandAllocator(type, IID_PPV_ARGS(&queue.m_allocator))) ||
            !BICAMERAL_CHECK_HR(Channel::Gpu, device->CreateCommandList1(0, type, D3D12_COMMAND_LIST_FLAG_NONE,
                                                                         IID_PPV_ARGS(&queue.m_list))) ||
            !BICAMERAL_CHECK_HR(Channel::Gpu,
                                device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue.m_fence)))) {
            return unexpected("キュー・コマンドリスト・フェンスを作れない");
        }

        // DRED と debug layer の報告に出る名前
        queue.m_queue->SetName(L"ImmediateQueue");
        queue.m_list->SetName(L"ImmediateQueue.list");

        return queue;
    }

    ID3D12GraphicsCommandList10* ImmediateQueue::Begin() {
        if (!BICAMERAL_CHECK_HR(Channel::Gpu, m_allocator->Reset()))
            return nullptr;

        if (!BICAMERAL_CHECK_HR(Channel::Gpu, m_list->Reset(m_allocator.Get(), nullptr)))
            return nullptr;

        return m_list.Get();
    }

    bool ImmediateQueue::ExecuteAndWait() {
        if (!BICAMERAL_CHECK_HR(Channel::Gpu, m_list->Close()))
            return false;  // 記録の誤りはここで分かる

        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);

        ++m_fenceValue;
        if (!BICAMERAL_CHECK_HR(Channel::Gpu, m_queue->Signal(m_fence.Get(), m_fenceValue)))
            return false;

        if (!BICAMERAL_CHECK_HR(Channel::Gpu, m_fence->SetEventOnCompletion(m_fenceValue, nullptr)))
            return false;

        // デバイスが失われるとフェンスの値は UINT64_MAX になる。理由と DRED の記録(有効なら)をログへ
        if (m_fence->GetCompletedValue() != UINT64_MAX)
            return true;

        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(m_queue->GetDevice(IID_PPV_ARGS(&device))))
            LogDeviceRemoved(device.Get());

        return false;
    }

}  // namespace bicameral::gpu
