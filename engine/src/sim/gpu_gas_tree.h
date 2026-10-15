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
// 計測(T-0358): EnableTiming の後、RecordTimestamp(list, 0) → 刻み → RecordTimestamp(list, 1) → RecordTimingReadback → 待つ → TimingTicks。
// 段ごとの計測(T-0374): EnablePassTiming の後は RecordStep が段ごとにタイムスタンプを打つ。1 つのリストに RecordStep 1 回 →
//   RecordPassTimingReadback → 待つ → PassTimingTicks(段ごとの 1 刻みの合計)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "gpu/resources.h"
#include "sim/gas_tree.h"

namespace bicameral::sim {

    class GpuGasTree {
    public:
        [[nodiscard]] static std::expected<GpuGasTree, std::string> Create(ID3D12Device5* device, const GasTree& tree);

        // 初めの状態(Create に渡したセル・端数・帳簿)と木の形を GPU へ写す(刻みより前に必ず 1 回)
        void RecordUpload(ID3D12GraphicsCommandList10* list);

        // 環境の値のホットリロード(T-0470・ADR-0200): tree(SetGasTreeEnvironment で環境を替えた後の設定・係数・層の基準)から
        // 設定・レベルごとの係数・層の基準・層の外(t0〜t3)を作り直して写す。次に記録する RecordStep から新しい値で刻む(刻みの境目で呼ぶ)。
        // 木の形(面・セルの場所。t4〜t6)はそのまま。前の t0〜t3 と写しの元は keepAlive へ(呼ぶ側がこのリストが GPU で終わるまで持つ)。
        // 木の形・小刻みが Create の時と同じであること(CheckGasTreeEnvironment)。作れなければ false
        [[nodiscard]] bool RecordEnvironment(ID3D12GraphicsCommandList10* list, const GasTree& tree,
                                             std::vector<ComPtr<ID3D12Resource>>& keepAlive);

        // 1 刻み(いちばん細かい小刻み config.substeps × 2^σmax 回 × 9 段)
        void RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing);

        // セル・端数・帳簿を読み戻しへ(同じリストで RecordStep を記録した後に)
        void RecordReadback(ID3D12GraphicsCommandList10* list) const;

        // RecordReadback を含むリストが終わってから
        [[nodiscard]] bool Read(std::vector<GasCell>& cells, std::vector<GasTreeFraction>& fractions,
                                GasTreeTotals& ledger) const;

        [[nodiscard]] uint64_t Tick() const { return m_tick; }
        [[nodiscard]] uint32_t FaceCount() const { return m_faceCount; }

        // 1 刻みの段の数(いちばん細かい小刻み substeps × 2^σmax 回 × 9 段)
        [[nodiscard]] uint32_t PassesPerTick() const { return (m_substeps << m_maxExponent) * (uint32_t)Pass::Count; }

        // --- 計測(タイムスタンプ 2 つの差。単位はキューの GetTimestampFrequency。gpu_gas.h と同じ形)---
        [[nodiscard]] bool EnableTiming(ID3D12Device* device);
        void RecordTimestamp(ID3D12GraphicsCommandList10* list, uint32_t slot) const;  // slot 0 = 始め、1 = 終わり
        void RecordTimingReadback(ID3D12GraphicsCommandList10* list) const;            // 2 つを打った同じリストで
        [[nodiscard]] std::optional<uint64_t> TimingTicks() const;

        // --- 段ごとの計測(T-0374。段の並びは gas_tree_step.hlsl の入口の順)---
        static constexpr size_t PASS_COUNT = 9;
        [[nodiscard]] bool EnablePassTiming(ID3D12Device* device);
        // RecordStep 1 回を記録した同じリストで
        void RecordPassTimingReadback(ID3D12GraphicsCommandList10* list) const;
        [[nodiscard]] std::optional<std::array<uint64_t, PASS_COUNT>> PassTimingTicks() const;

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
            uint32_t speciesStride = 0;
        };

        GpuGasTree() = default;

        [[nodiscard]] std::expected<void, std::string> CreatePipelines(ID3D12Device5* device);
        [[nodiscard]] std::expected<void, std::string> CreateBuffers(ID3D12Device5* device, const GasTree& tree);
        [[nodiscard]] std::expected<void, std::string> CreateLayout(ID3D12Device5* device, const GasTree& tree);
        void RecordPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Pass pass,
                        const Constants& constants) const;

        uint32_t m_cellCount = 0;
        uint32_t m_faceCount = 0;
        uint32_t m_speciesCount = 0;   // 木の成分の数(読み戻す欄)
        uint32_t m_speciesStride = 1;  // 1 件の成分の欄の数(gas_tree_step.hlsl の g_speciesStride。T-0598)
        uint32_t m_substeps = 0;
        uint32_t m_maxExponent = 0;  // σmax(1 刻み = いちばん細かい小刻み m_substeps × 2^これ 回)
        uint32_t m_levelCount = 0;
        uint64_t m_tick = 0;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::array<ComPtr<ID3D12PipelineState>, (size_t)Pass::Count> m_pipelines;
        std::array<ComPtr<ID3D12Resource>, 11> m_uavs;  // u0〜u10(gas_tree_step.hlsl の結び付け)
        // t0〜t6(設定・レベル・層の基準・層の外・面の形・セルの場所・セルの面の並び)。既定のヒープの写し(T-0374・T-0410)
        std::array<gpu::ResidentBuffer, 7> m_srvs;
        std::array<ComPtr<ID3D12Resource>, 3> m_uploads;    // 初めのセル・端数・帳簿
        std::array<ComPtr<ID3D12Resource>, 3> m_readbacks;  // セル・端数・帳簿
        ComPtr<ID3D12QueryHeap> m_timestamps;               // 計測(EnableTiming の後だけ)
        ComPtr<ID3D12Resource> m_timestampReadback;
        ComPtr<ID3D12QueryHeap> m_passTimestamps;  // 段ごとの計測(EnablePassTiming の後だけ。1 + 1 刻みの段の数)
        ComPtr<ID3D12Resource> m_passTimestampReadback;
    };

}  // namespace bicameral::sim
