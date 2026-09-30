// immediate_queue.h — コマンドリストを 1 本記録して投げ、GPU が終えるまで待つキュー(T-0013)。
// テストと起動時の準備(アップロード・初期化)のための簡単な道具。フレームのループ(T-0004・T-0012)は待たない別の仕組みを使う。
//
//   auto queue = ImmediateQueue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
//   ID3D12GraphicsCommandList10* list = queue->Begin();   // ここに記録する
//   if (!queue->ExecuteAndWait()) ...                     // 投げて待つ。デバイスが失われたら false
#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "core/aliases.h"
#include "gpu/com_ptr.h"

namespace bicameral::gpu {

    class ImmediateQueue {
    public:
        [[nodiscard]] static expected<ImmediateQueue, std::string> Create(ID3D12Device5* device,
                                                                          D3D12_COMMAND_LIST_TYPE type);

        // 記録を始める(前の記録は捨てる)。失敗なら nullptr
        [[nodiscard]] ID3D12GraphicsCommandList10* Begin();

        // 記録を閉じて投げ、GPU が終えるまで待つ。記録の誤りやデバイスの喪失なら false(理由はログ)
        [[nodiscard]] bool ExecuteAndWait();

        // タイムスタンプの周波数を得るなど、キューそのものが要るとき
        [[nodiscard]] ID3D12CommandQueue* Native() const { return m_queue.Get(); }

    private:
        ImmediateQueue() = default;

        ComPtr<ID3D12CommandQueue> m_queue;
        ComPtr<ID3D12CommandAllocator> m_allocator;
        ComPtr<ID3D12GraphicsCommandList10> m_list;
        ComPtr<ID3D12Fence> m_fence;
        uint64_t m_fenceValue = 0;
    };

}  // namespace bicameral::gpu
