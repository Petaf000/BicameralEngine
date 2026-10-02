// gpu_multires.h — 多重解像度の入れ子(sim/multires_nest の CPU リファレンスと同じ形)を GPU に置き、
// 細かくする・粗くする・影を引き戻す(Work Graph の再帰。shaders/sim/multires_graph.hlsl)と反応の刻み(Compute。multires_step.hlsl)を記録する(T-0017)。
//
// 使い方(テスト。1 刻み = 細分の出来事 → 刻む → 影の引き戻し。CPU の test::StepMultiresScene と同じ順):
//   auto gpu = GpuMultires::Create(device, table, blockCapacity, fractionCapacity);
//   gpu->RecordUpload(list, nest);                 // 最初だけ
//   gpu->RecordRefine(list, ring, ...); gpu->RecordStep(list, ring, seed, tick); gpu->RecordPullBack(list, ring, ...);
//   gpu->RecordReadback(list);  → 投げて待つ →  gpu->Read(nest);
// 各操作の後に UAV のバリアを入れる(次の操作は前の書き込みを読む)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "gpu/work_graph.h"
#include "sim/multires_nest.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    class GpuMultires {
    public:
        [[nodiscard]] static std::expected<GpuMultires, std::string> Create(ID3D12Device5* device,
                                                                            const BakedReactionTable& table,
                                                                            uint32_t blockCapacity,
                                                                            uint32_t fractionCapacity);

        // CPU の入れ子(見出し・セル・端数・数える欄)を GPU へ写す。大きさは Create と同じでなければならない
        [[nodiscard]] bool RecordUpload(ID3D12GraphicsCommandList10* list, const MultiresNest& nest);

        // --- 操作(multires_nest.h の同じ名前の関数と同じ結果)---
        void RecordRefine(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint32_t parentSlot,
                          uint32_t firstChildSlot, uint32_t levelCount, uint32_t kind, const MultiresPoint& point);
        void RecordCoarsen(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint32_t deepestSlot,
                           uint32_t levelCount);
        void RecordRemoveShadow(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                uint32_t firstSlot, uint32_t levelCount);
        void RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t worldSeed,
                        uint64_t tick);
        void RecordPullBack(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                            uint32_t firstShadowSlot, uint32_t levelCount);

        // --- 読み戻し ---
        void RecordReadback(ID3D12GraphicsCommandList10* list);
        [[nodiscard]] bool Read(MultiresNest& nest) const;

        // --- 計測: index 番目のタイムスタンプを打つ。RecordReadback が打った分を写し、ReadTimestamps で読む ---
        void RecordTimestamp(ID3D12GraphicsCommandList10* list, uint32_t index);
        [[nodiscard]] std::vector<uint64_t> ReadTimestamps(uint32_t count) const;

        [[nodiscard]] uint64_t GraphBackingMemoryBytes() const { return m_graph->BackingMemoryBytes(); }

    private:
        // multires_bindings.hlsli の RootConstants と同じ並び
        struct RootConstants {
            uint32_t seedLow = 0;
            uint32_t seedHigh = 0;
            uint32_t tickLow = 0;
            uint32_t tickHigh = 0;
            std::array<uint32_t, 6> point{};  // x・y・z の下位と上位
            int32_t pointLevel = 0;
            uint32_t blockCount = 0;
        };

        static constexpr uint32_t BUFFER_COUNT = 4;  // 見出し・セル・端数・数える欄
        static constexpr uint32_t TABLE_COUNT = 4;   // 物質・規則・索引・速度
        static constexpr uint32_t MAX_TIMESTAMPS = 16;

        GpuMultires() = default;

        [[nodiscard]] std::expected<void, std::string> CreatePipelines(ID3D12Device5* device);
        [[nodiscard]] std::expected<void, std::string> CreateBuffers(ID3D12Device5* device,
                                                                     const BakedReactionTable& table);
        void BindRoot(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) const;
        void SetGraphProgram(ID3D12GraphicsCommandList10* list);
        void DispatchGraph(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint32_t entry,
                           const void* record, uint32_t recordBytes);

        // --- 大きさ ---
        uint32_t m_blockCapacity = 0;
        uint32_t m_fractionCapacity = 0;

        // --- パイプライン ---
        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_stepPipeline;
        std::unique_ptr<gpu::WorkGraph> m_graph;
        bool m_graphInitialized = false;
        std::array<uint32_t, 4> m_entries{};  // 細かくする・粗くする・引き戻す・影を捨てる
        RootConstants m_constants;

        // --- バッファ(既定のヒープ・アップロード・読み戻し。並びは BUFFER_COUNT の順)---
        std::array<ComPtr<ID3D12Resource>, BUFFER_COUNT> m_buffers;
        std::array<ComPtr<ID3D12Resource>, BUFFER_COUNT> m_uploads;
        std::array<ComPtr<ID3D12Resource>, BUFFER_COUNT> m_readbacks;
        std::array<ComPtr<ID3D12Resource>, TABLE_COUNT> m_tables;

        // --- 計測 ---
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
        uint32_t m_timestampCount = 0;
    };

}  // namespace bicameral::sim
