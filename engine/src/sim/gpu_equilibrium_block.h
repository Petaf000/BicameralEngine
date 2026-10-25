// gpu_equilibrium_block.h — 釣り合う家族の共通部品の「成分を持つ節」(T-0919・ADR-0418)の GPU 版(Compute。T-0970・ADR-0422)。
// CPU リファレンスは sim/equilibrium_block の BuildEquilibriumBlockGrid・SolveEquilibriumBlocks。式は common/equilibrium_block.hlsli で共有し、
// 段は shaders/sim/equilibrium_block.hlsl(成分の数 k = 1・2・3・6 ごとに別のシェーダー)。
// 部品が持つもの: 段の形(節・隣・子・色の一覧。形は CPU の BuildEquilibriumBlockGrid が作った段を写す)・係数(延ばし方・縮約・対角・面のブロック・
//   LDLᵀ・固定した面)・作業場(値・右辺・右辺の項)。粗い段の係数(Galerkin)と LDLᵀ は GPU が作る(RecordPrepare)。
// 法則が持つもの: 段 0 の係数(対角と面のブロック = CPU の段 0。固定した面)と右辺の項。今は写して渡す(RecordUpload)。
//   法則の段 0 を GPU で組む形(陰の圧力の EqGpuFinishNode に当たる所)は使い手ごと(支えの GPU は T-0946 → T-0981)。
// 1 回の解き = RecordPrepare(係数が変わった時: 段 0 の LDLᵀ と右辺 → 粗い段)→ RecordSolve(V サイクルをちょうど cycles 回)。
// 値は作業場の段 0(節 × k)で、次の解きの温めた始まりになる。CPU は記録だけ(D-107)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "gpu/resources.h"
#include "sim/equilibrium_block.h"

namespace bicameral::sim {

    struct GpuEquilibriumBlockOptions {
        EquilibriumBlockOptions solve;  // V の回数・掃き出しの数(CPU と同じ値で同じ答え)
        bool reverseOrder = false;   // 試験: 色の一覧・行・子・固定した面の順を逆に積む(順に依存しないことの確かめ。R3)
        uint32_t readbackSlots = 1;  // 値の読み戻しの置き場の数(試験が刻みごとの値を 1 本のリストで読む)
    };

    class GpuEquilibriumBlock {
    public:
        // 解ける成分の数(シェーダーを作ってある k)
        [[nodiscard]] static bool Supports(uint32_t components);

        [[nodiscard]] static std::expected<GpuEquilibriumBlock, std::string> Create(
            ID3D12Device5* device, const EquilibriumBlockSystem& system, const EquilibriumBlockGrid& grid,
            const GpuEquilibriumBlockOptions& options);

        // --- 記録(リストの初めに RecordBegin、終わりに RecordEnd)---
        void RecordBegin(ID3D12GraphicsCommandList* list);
        void RecordEnd(ID3D12GraphicsCommandList* list) const;

        // 段 0 の係数(grid の段 0)・固定した面と右辺の項(system)・温めた始まり(節 × k。空なら 0)を写す。
        // 段の形は Create の時と同じであること(違えば false)
        [[nodiscard]] bool RecordUpload(ID3D12GraphicsCommandList* list, const EquilibriumBlockSystem& system,
                                        const EquilibriumBlockGrid& grid, std::span<const int64_t> warmValues);
        // 段 0 の LDLᵀ と右辺 → 粗い段(Galerkin)の係数と LDLᵀ
        void RecordPrepare(ID3D12GraphicsCommandList* list, uint64_t debugRing) const;
        // V サイクルをちょうど solve.cycles 回
        void RecordSolve(ID3D12GraphicsCommandList* list, uint64_t debugRing) const;

        // 段 0 の値(節 × k)を読み戻しの置き場 slot へ(< readbackSlots)
        void RecordValuesReadback(ID3D12GraphicsCommandList* list, uint32_t slot = 0) const;
        [[nodiscard]] bool ReadValues(std::vector<int64_t>& values, uint32_t slot = 0) const;

        // 計測(試験)
        void RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index) const;
        void RecordTimestampReadback(ID3D12GraphicsCommandList* list, uint32_t count) const;
        [[nodiscard]] std::vector<uint64_t> ReadTimestamps(uint32_t count) const;

        // 写しの元(アップロード)を捨てる(RecordUpload を積んだリストが GPU で終わった後。次の RecordUpload が作り直す)
        void ReleaseStaging();

        // --- 大きさ ---
        [[nodiscard]] uint32_t Components() const { return m_components; }
        [[nodiscard]] uint32_t Cells() const { return m_cells; }
        [[nodiscard]] uint32_t Nodes() const { return m_nodeCapacity; }
        [[nodiscard]] uint32_t Links() const { return m_linkCapacity; }
        [[nodiscard]] uint32_t Levels() const { return (uint32_t)m_levels.size(); }
        [[nodiscard]] uint64_t DeviceBytes() const;  // 既定のヒープ(係数・作業場・節・添字)
        [[nodiscard]] uint64_t DispatchesPerSolve() const;

        static constexpr uint32_t MAX_TIMESTAMPS = 64;

    private:
        enum class Pass : uint8_t {
            Finish,
            Coarsen,
            Smooth,
            Restrict,
            Prolong,
            // 共役勾配(solve.lineSearch。T-1085・ADR-0455)
            ConjugateSave,
            ConjugateResidual,
            ConjugateBeta,
            ConjugateDirection,
            ConjugatePush,
            ConjugateAlpha,
            ConjugateUpdate,
            Count
        };

        // b0(equilibrium_block.hlsl の EquilibriumBlockConstants と同じ順)
        struct Constants {
            uint32_t nodeCapacity = 0;
            uint32_t linkCapacity = 0;
            uint32_t first = 0;
            uint32_t count = 0;
            uint32_t list = 0;
            uint32_t shift = 0;
        };

        struct Level {
            uint32_t first = 0;  // 節の始まり(全部の段の番号)
            uint32_t count = 0;
            // 添字のバッファの中の色の一覧(同じレベルの段は 8 色・違うレベルが混ざる段は 16 色。T-1104)
            uint32_t colorCount = equilibrium_block::EQB_OCTANTS;
            std::array<uint32_t, equilibrium_block::EQB_MAX_COLORS> colorStarts{};
            std::array<uint32_t, equilibrium_block::EQB_MAX_COLORS> colorCounts{};
        };

        GpuEquilibriumBlock() = default;

        // 段の形(節・添字の一覧)を作り、m_levels・容量・段 0 の写し方を埋める
        void BuildShape(const EquilibriumBlockSystem& system, const EquilibriumBlockGrid& grid,
                        std::vector<equilibrium_block::EqbGpuNode>& nodes, std::vector<uint32_t>& indices);
        [[nodiscard]] bool CreateBuffers(std::span<const equilibrium_block::EqbGpuNode> nodes,
                                         std::span<const uint32_t> indices);
        [[nodiscard]] std::vector<int64_t> CoefficientImage(const EquilibriumBlockSystem& system,
                                                            const EquilibriumBlockGrid& grid) const;
        [[nodiscard]] std::vector<int64_t> WorkImage(const EquilibriumBlockSystem& system,
                                                     std::span<const int64_t> warmValues) const;
        void RecordStateChange(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES before,
                               D3D12_RESOURCE_STATES after) const;

        void RecordPass(ID3D12GraphicsCommandList* list, uint64_t debugRing, Pass pass, Constants constants) const;
        void RecordSmooth(ID3D12GraphicsCommandList* list, uint64_t debugRing, const Level& level,
                          uint32_t sweeps) const;
        void RecordCycle(ID3D12GraphicsCommandList* list, uint64_t debugRing, size_t depth) const;
        void RecordConjugate(ID3D12GraphicsCommandList* list, uint64_t debugRing, Pass pass, uint32_t cycle) const;
        [[nodiscard]] Constants BaseConstants() const;

        uint32_t m_components = 0;
        uint32_t m_coefficientShift = 0;
        uint32_t m_cells = 0;
        uint32_t m_nodeCapacity = 0;
        uint32_t m_linkCapacity = 0;
        uint32_t m_fixedCapacity = 0;
        GpuEquilibriumBlockOptions m_options;
        std::vector<Level> m_levels;

        // 段 0 の写し方(試験の逆順も): 段 0 の隣の置き場 → CPU の段 0 の隣の番号・固定した面の置き場 → 系の固定した面の番号
        std::vector<uint32_t> m_linkSource;
        std::vector<uint32_t> m_fixedSource;
        uint32_t m_cellLinks = 0;  // 段 0 の隣の数

        ID3D12Device5* m_device = nullptr;
        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::array<ComPtr<ID3D12PipelineState>, (size_t)Pass::Count> m_pipelines;
        gpu::ResidentBuffer m_nodes;    // EqbGpuNode(t0)
        gpu::ResidentBuffer m_indices;  // 隣・子・色の一覧(t1)
        bool m_residentPending = true;
        // 写した直後で、刻みをまたぐ共役勾配の向きがまだ無い(RecordSolve が積んだら下ろす。記録の順の印なので const の記録でも書く)
        mutable bool m_conjugateFresh = true;
        ComPtr<ID3D12Resource> m_coefficients;  // u0
        ComPtr<ID3D12Resource> m_work;          // u1
        ComPtr<ID3D12Resource> m_coefficientUpload;
        ComPtr<ID3D12Resource> m_workUpload;
        ComPtr<ID3D12Resource> m_valuesReadback;
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
    };

}  // namespace bicameral::sim
