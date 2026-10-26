// stage_stamps.h — 段ごとの GPU 時間を測る印(タイムスタンプ)。測り用で、結果には入らない(T-1161)。
//
// データの流れ: フレームの記録で Begin(slot) → 段の後ろで Mark(list, 段の番号)… → リストの終わりで Resolve(list)
//   → そのフレームが終わった後に Collect(slot) が、隣り合う印の差を後ろの印の段に足す(段 START の印は差を取らない = 測りの始め)。
// 段の数・意味は呼ぶ側が決める(例 sim/probe_sim の ProbeRadiationStage)。1 フレームの印が容量を超えたら超えた分は捨て、Dropped に数える。
// 時間はタイムスタンプの刻みのまま(秒にするのはキューの TimestampFrequency で呼ぶ側)。
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"

namespace bicameral::gpu {

    class StageStamps {
    public:
        static constexpr uint8_t START = 0;  // 測りの始めの印(差を取らない)

        // slots: フレームの数(読み戻しの数)。capacity: 1 フレームの印の上限。stages: 段の番号の数(START を含む)
        [[nodiscard]] static std::expected<StageStamps, std::string> Create(ID3D12Device* device, uint32_t slots,
                                                                            uint32_t capacity, uint32_t stages);

        // フレームの記録の始め(CPU)。その slot の印を空に
        void Begin(uint32_t slot);
        // 今のフレームのリストに印を書く(容量を超えたら書かない)
        void Mark(ID3D12GraphicsCommandList* list, uint8_t stage);
        // 今のフレームの印を読み戻しへ(リストの終わり)
        void Resolve(ID3D12GraphicsCommandList* list) const;
        // slot のフレームが GPU で終わった後: 差を段ごとの和へ足す
        void Collect(uint32_t slot);

        [[nodiscard]] std::span<const uint64_t> Ticks() const { return m_ticks; }    // 段ごとの時間の和
        [[nodiscard]] std::span<const uint64_t> Counts() const { return m_counts; }  // 段ごとの回数
        [[nodiscard]] uint64_t Dropped() const { return m_dropped; }
        void Reset();

    private:
        StageStamps() = default;

        ComPtr<ID3D12QueryHeap> m_heap;
        std::vector<ComPtr<ID3D12Resource>> m_readbacks;  // slot ごと
        std::vector<std::vector<uint8_t>> m_stages;       // slot ごとの印の段(書いた順)
        std::vector<uint64_t> m_ticks;
        std::vector<uint64_t> m_counts;
        uint64_t m_dropped = 0;
        uint32_t m_capacity = 0;
        uint32_t m_slot = 0;
    };

}  // namespace bicameral::gpu
