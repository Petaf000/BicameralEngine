// queue.h — コマンドキューと、そのキューが進めるフェンス(T-0004)。
// フレームのループはこれで「投げる」「別のキューを GPU の上で待たせる」「終わったかを待たずに見る」だけをする(06 §4)。
// CPU が GPU を待つ WaitCpu() は、起動・終了・窓の大きさの変更のときだけ使う。
//
//   auto compute = Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"Sim");
//   const uint64_t done = compute->Submit(list);   // 終わるとフェンスが done になる
//   direct->GpuWait(*compute, done);               // direct キューは compute の done まで先へ進まない(CPU は待たない)
//   if (compute->IsComplete(done)) ...             // 待たずに見る
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace bicameral::gpu {

    class Queue {
    public:
        // priority: 描画のキューは HIGH にする(重いシミュと並ぶときに描画が先に進みやすい。docs/perf.md 2026-09-30 T-0004)
        [[nodiscard]] static std::expected<Queue, std::string> Create(
            ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, std::wstring_view name,
            D3D12_COMMAND_QUEUE_PRIORITY priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);

        [[nodiscard]] ID3D12CommandQueue* Native() const { return m_queue.Get(); }

        // リストを投げてフェンスを進める。返り値はこれらのリストが終わったときのフェンスの値
        uint64_t Submit(std::span<ID3D12CommandList* const> lists);
        uint64_t Submit(ID3D12CommandList* list) { return Submit(std::span(&list, 1)); }

        // フェンスを進めずに投げる(1 つの仕事を何回かの投入に分けるとき。最後の 1 回を Submit にすれば、その値で全部の終わりが分かる)
        void Execute(ID3D12CommandList* list) const;

        // このキューは other のフェンスが value になるまで先へ進まない(GPU の上で待つ。CPU は待たない)
        void GpuWait(const Queue& other, uint64_t value) const;

        // 最後に Submit() したときの値と、GPU が終えた値(待たない)
        [[nodiscard]] uint64_t LastSubmitted() const { return m_lastSubmitted; }
        [[nodiscard]] uint64_t CompletedValue() const { return m_fence->GetCompletedValue(); }
        [[nodiscard]] bool IsComplete(uint64_t value) const { return CompletedValue() >= value; }

        // デバイスが失われるとフェンスは UINT64_MAX を返す
        [[nodiscard]] bool IsDeviceLost() const { return CompletedValue() == UINT64_MAX; }

        // CPU で value まで待つ(起動・終了・大きさの変更のときだけ)。デバイスが失われたら false
        bool WaitCpu(uint64_t value) const;
        bool WaitIdle() const { return WaitCpu(m_lastSubmitted); }

        // Present のようにフェンスの後ろに積まれた仕事も含めて、今までに投げた全部を待つ(フェンスを 1 つ進めてから待つ)。
        // バックバッファを作り直す前・終わる前に使う
        bool Flush() { return WaitCpu(Submit(std::span<ID3D12CommandList* const>{})); }

        // タイムスタンプの 1 秒あたりの刻み。取れなければ 0
        [[nodiscard]] uint64_t TimestampFrequency() const;

    private:
        Queue() = default;

        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
        uint64_t m_lastSubmitted = 0;
    };

}  // namespace bicameral::gpu
