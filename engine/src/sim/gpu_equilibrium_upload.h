// gpu_equilibrium_upload.h — 釣り合う家族の部品の GPU 版(GpuEquilibrium)を、CPU が組んだ系(EquilibriumSystem)で回す小さな口(T-1111)。
// 時間を飛ばす試作(sim/time_step_tree)は列の系を CPU で組むので、GPU の費用と CPU とのビット一致はこの口で確かめる。
// 持つもの: 部品(段の形・作業場)と、段 0 の写し(節ごとの自分だけの係数・6 面の係数と隣・右辺の項。既定のヒープ)と、
//   それを部品の段 0 にする法則の段(shaders/sim/equilibrium_upload.hlsl)。部品(GpuEquilibrium・GpuImplicit)は変えない。
// 1 回の解き = 段 0(写しから)→ 粗い段の重み(部品の RecordLevels)→ V サイクル × cycles(部品の RecordSolve を 1 回ずつ)。
// 初めの値は 0(試作の列は差 δ = 0 から解く)。今の部品の範囲: 節がどれも同じレベルの系・閉じない解き(T-0939)。
// データの流れ: CPU の系 → 段 0 の写し(アップロード → 既定のヒープ)→ この段 → 部品の段 → 作業場の値 A → 読み戻し(試験)。
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "gpu/resources.h"
#include "sim/equilibrium.h"
#include "sim/gpu_equilibrium.h"

namespace bicameral::sim {

    struct GpuEquilibriumUploadOptions {
        // 部品を作る時の V の回数(RecordSolve の cycles はこの倍数。部品の RecordSolve を cycles ÷ これ 回積む。値は同じ。T-1141 の測り)
        uint32_t cyclesPerRecord = 1;
        // 段 0 の段の測りの変種(0 = 本物。1 = 右辺の項を割らない・2 = 段 0 を仕上げず読むだけ。値は壊れる = 段 0 の時間だけを測る。T-1141)
        uint32_t stageVariant = 0;
    };

    class GpuEquilibriumUpload {
    public:
        static constexpr uint32_t MAX_TIMESTAMPS = 256;

        // grid は system を SolveEquilibrium が組んだ段(閉じない道。節の順は system と同じ)
        [[nodiscard]] static std::expected<GpuEquilibriumUpload, std::string> Create(
            ID3D12Device5* device, const EquilibriumSystem& system, const ImplicitGrid& grid,
            const GpuEquilibriumUploadOptions& options = {});

        // 段の形・段 0 の写しを写し、前の答えを 0 に(リストの初め。バッファは COMMON の時)
        [[nodiscard]] bool RecordUpload(ID3D12GraphicsCommandList* list, const ImplicitGrid& grid);

        // 1 回の解き。reweight = false なら段 0・粗い段の重みを積まない(前の解きの重みを使い回す形の費用。値は同じ系の時だけ同じ)
        [[nodiscard]] bool RecordSolve(ID3D12GraphicsCommandList* list, uint64_t debugRing, uint32_t cycles,
                                       bool reweight = true);

        // 解いた値(節の順)の読み戻し(RecordSolve の後。RecordEnd の前)
        void RecordValuesReadback(ID3D12GraphicsCommandList* list);
        [[nodiscard]] bool ReadValues(std::vector<int64_t>& values) const;

        // V の記録の形(値は同じ。前の解きの数から GpuImplicit::ShapeFrom で選ぶ)
        void SetSolveShape(GpuImplicitRecordShape shape) { m_equilibrium.SetSolveShape(shape); }
        void RecordCostReadback(ID3D12GraphicsCommandList* list) { m_equilibrium.RecordCostReadback(list); }
        [[nodiscard]] bool ReadCost(GpuImplicitCost& cost) const { return m_equilibrium.ReadCost(cost); }

        void RecordEnd(ID3D12GraphicsCommandList* list) { m_equilibrium.RecordEnd(list); }
        void ReleaseStaging() { m_equilibrium.ReleaseStaging(); }

        // --- 時刻印(GPU の時計) ---
        void RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index) const;
        void RecordTimestampReadback(ID3D12GraphicsCommandList* list, uint32_t count) const;
        [[nodiscard]] std::vector<uint64_t> ReadTimestamps(uint32_t count) const;

        [[nodiscard]] uint32_t Cells() const { return m_cells; }
        [[nodiscard]] uint32_t Terms() const { return m_terms; }
        [[nodiscard]] uint64_t DeviceBytes() const;

        // 段 0 の写し(64bit の語。equilibrium_upload.hlsl の並び)。系が部品の段 0 の形に入らなければ空
        [[nodiscard]] static std::vector<uint64_t> StageWords(const EquilibriumSystem& system, uint32_t& terms);

    private:
        explicit GpuEquilibriumUpload(GpuEquilibrium equilibrium) : m_equilibrium(std::move(equilibrium)) {}

        void RecordStage(ID3D12GraphicsCommandList* list, uint64_t debugRing) const;

        GpuEquilibrium m_equilibrium;
        uint32_t m_cells = 0;
        uint32_t m_terms = 0;
        uint32_t m_rounding = 0;
        uint32_t m_cyclesPerRecord = 1;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_pipeline;
        gpu::ResidentBuffer m_stage;  // 段 0 の写し
        ComPtr<ID3D12Resource> m_valuesReadback;
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
    };

}  // namespace bicameral::sim
