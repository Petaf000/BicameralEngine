// gpu_gas_tree.h — 気体の多重解像度の木(sim/gas_tree.h。T-0208)の 1 刻みを GPU の Compute で走らせる(T-0209・T-0356。07 §2.5・ADR-0117)。
// 隣のレベルの差 1 まで(2:1)の木だけ(木が変わる時の面の一覧と d ≥ 2 は T-0357)。レベルごとの 2^σ の小刻み(T-0320 の時間線)も載る(T-0356)。
// いちばん細かい小刻みごとに 9 段(shaders/sim/gas_tree_step.hlsl)。CPU リファレンス(StepGasTree)とセル・端数・帳簿が毎刻みビット一致することを
// tests/gpu_gas_tree_test.cpp が確かめる。
// CPU が書くのは、木を作った時に一度だけの形(面の一覧・セルの場所・レベルごとの係数と層の基準)と初めの状態の写し、ルート定数(小刻みの通し番号)だけ。
// 木のセル・端数・帳簿は GPU が持つ(D-107)。
//
// 使い方:
//   auto gas = GpuGasTree::Create(device, tree);   // tree = MakeGasTree(...) に初めの乱れを入れたもの
//   gas->RecordUpload(list);                      // 最初だけ(tree.cells・fractions・ledger・tick から続ける)
//   gas->RecordStep(list, debugRingAddress);      // 1 刻み
//   gas->RecordReadback(list);  → 投げて待つ → gas->Read(cells, fractions, ledger)
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "sim/gas_tree.h"

namespace bicameral::sim {

    class GpuGasTree {
    public:
        [[nodiscard]] static std::expected<GpuGasTree, std::string> Create(ID3D12Device5* device, const GasTree& tree);

        // 初めの状態(Create に渡したセル・端数・帳簿)を GPU へ写す
        void RecordUpload(ID3D12GraphicsCommandList10* list);

        // 1 刻み(いちばん細かい小刻み config.substeps × 2^σmax 回 × 9 段)
        void RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing);

        // セル・端数・帳簿を読み戻しへ(同じリストで RecordStep を記録した後に)
        void RecordReadback(ID3D12GraphicsCommandList10* list) const;

        // RecordReadback を含むリストが終わってから
        [[nodiscard]] bool Read(std::vector<GasCell>& cells, std::vector<GasTreeFraction>& fractions,
                                GasTreeTotals& ledger) const;

        [[nodiscard]] uint64_t Tick() const { return m_tick; }
        [[nodiscard]] uint32_t FaceCount() const { return m_faceCount; }

    private:
        // shaders/sim/gas_tree_step.hlsl の入口(呼ぶ順。.cso の名前と同じ順)
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

        // b0 のルート定数(gas_tree_step.hlsl の RootConstants と同じ順)
        struct Constants {
            uint32_t tickLow = 0;
            uint32_t tickHigh = 0;
            uint32_t micro = 0;
            uint32_t substeps = 0;
            uint32_t maxExponent = 0;
            uint32_t cellCount = 0;
            uint32_t faceCount = 0;
            uint32_t unused = 0;
        };

        GpuGasTree() = default;

        [[nodiscard]] std::expected<void, std::string> CreatePipelines(ID3D12Device5* device);
        [[nodiscard]] std::expected<void, std::string> CreateBuffers(ID3D12Device5* device, const GasTree& tree);
        [[nodiscard]] std::expected<void, std::string> CreateLayout(ID3D12Device5* device, const GasTree& tree);
        void RecordPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Pass pass,
                        const Constants& constants) const;

        uint32_t m_cellCount = 0;
        uint32_t m_faceCount = 0;
        uint32_t m_substeps = 0;
        uint32_t m_maxExponent = 0;  // σmax(1 刻み = いちばん細かい小刻み m_substeps × 2^これ 回)
        uint64_t m_tick = 0;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::array<ComPtr<ID3D12PipelineState>, (size_t)Pass::Count> m_pipelines;
        std::array<ComPtr<ID3D12Resource>, 11> m_uavs;  // u0〜u10(gas_tree_step.hlsl の結び付け)
        std::array<ComPtr<ID3D12Resource>, 7>
            m_srvs;  // t0〜t6(設定・レベル・層の基準・層の外・面の形・セルの場所・セルの面の並び)
        std::array<ComPtr<ID3D12Resource>, 3> m_uploads;    // 初めのセル・端数・帳簿
        std::array<ComPtr<ID3D12Resource>, 3> m_readbacks;  // セル・端数・帳簿
    };

}  // namespace bicameral::sim
