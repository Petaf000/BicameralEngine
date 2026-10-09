// gpu_gas.h — 気体の流れ(sim/gas_reference.h の G1・G2)の 1 刻みを GPU の Compute で走らせる(G3 の第 1 段。T-0185・07 §2.3)。
// 1 レベルの箱だけ(多重解像度の木と 05 のセルにつなぐのは T-0208 以降)。小刻みごとに 9 段(shaders/sim/gas_step.hlsl)。
// CPU リファレンス(StepGas)と毎刻みビット一致することを tests/gpu_gas_test.cpp が確かめる。
// CPU が書くのは初めの箱の写し(RecordUpload)とルート定数(小刻みの通し番号)だけ。箱のセルと帳簿は GPU が持つ(D-107)。
//
// 使い方:
//   auto gas = GpuGas::Create(device, box);       // box = MakeGasBox(...) に初めの乱れを入れたもの
//   gas->RecordUpload(list);                     // 最初だけ(box.cells・box.ledger・box.tick から続ける)
//   gas->RecordStep(list, debugRingAddress);     // 1 刻み(何刻みでも続けて記録してよい)
//   gas->RecordReadback(list);  → 投げて待つ → gas->Read(cells, ledger)
// 計測: EnableTiming の後、RecordTimestamp(list, 0) → 刻み → RecordTimestamp(list, 1) → RecordTimingReadback → 待つ → TimingTicks。
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "sim/gas_reference.h"

namespace bicameral::sim {

    class GpuGas {
    public:
        [[nodiscard]] static std::expected<GpuGas, std::string> Create(ID3D12Device5* device, const GasBox& box);

        // 初めの箱(Create に渡したセルと帳簿)を GPU へ写す
        void RecordUpload(ID3D12GraphicsCommandList10* list);

        // 1 刻み(config.substeps 回の小刻み × 9 段)
        void RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing);

        // セルと帳簿を読み戻しへ(同じリストで RecordStep を記録した後に)
        void RecordReadback(ID3D12GraphicsCommandList10* list) const;

        // RecordReadback を含むリストが終わってから
        [[nodiscard]] bool Read(std::vector<GasCell>& cells, GasLedger& ledger) const;

        [[nodiscard]] uint64_t Tick() const { return m_tick; }

        // --- 計測(タイムスタンプ 2 つの差。単位はキューの GetTimestampFrequency)---
        [[nodiscard]] bool EnableTiming(ID3D12Device* device);
        void RecordTimestamp(ID3D12GraphicsCommandList10* list, uint32_t slot) const;  // slot 0 = 始め、1 = 終わり
        void RecordTimingReadback(ID3D12GraphicsCommandList10* list) const;            // 2 つを打った同じリストで
        [[nodiscard]] std::optional<uint64_t> TimingTicks() const;

    private:
        // shaders/sim/gas_step.hlsl の入口(呼ぶ順。.cso の名前と同じ順)
        enum class Pass : uint8_t {
            Derive,
            FaceForce,
            ApplyForce,
            FaceFlow,
            Outflow,
            FaceMoved,
            MovedSum,
            FaceFinal,
            Transfer,
            Count,
        };

        // b0 のルート定数(gas_step.hlsl の RootConstants と同じ順)
        struct Constants {
            uint32_t roundingTickLow = 0;
            uint32_t roundingTickHigh = 0;
            uint32_t cellCount = 0;
            uint32_t unused = 0;
        };

        GpuGas() = default;

        [[nodiscard]] std::expected<void, std::string> CreatePipelines(ID3D12Device5* device);
        [[nodiscard]] std::expected<void, std::string> CreateBuffers(ID3D12Device5* device, const GasBox& box);
        void RecordPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Pass pass,
                        const Constants& constants) const;

        uint32_t m_cellCount = 0;
        uint32_t m_substeps = 0;
        uint64_t m_tick = 0;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::array<ComPtr<ID3D12PipelineState>, (size_t)Pass::Count> m_pipelines;
        std::array<ComPtr<ID3D12Resource>, 6> m_uavs;  // u0〜u5(gas_step.hlsl の結び付け)
        std::array<ComPtr<ID3D12Resource>, 3> m_srvs;  // t0 パラメータ・t1 層の基準の導く値・t2 層の開いた境界の外
        ComPtr<ID3D12Resource> m_cellUpload;           // 初めのセル
        ComPtr<ID3D12Resource> m_ledgerUpload;         // 初めの帳簿
        ComPtr<ID3D12Resource> m_cellReadback;
        ComPtr<ID3D12Resource> m_ledgerReadback;
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
    };

}  // namespace bicameral::sim
