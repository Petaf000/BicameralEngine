// gpu_equilibrium.h — 釣り合う家族の共通部品(T-0853・20 §9・§13.1)の GPU 版(Compute。T-0915)。CPU リファレンスは sim/equilibrium の SolveEquilibrium。
// 部品が持つもの: 多重格子の段(GpuImplicit の節・隣・子の一覧。形は CPU の BuildImplicitGrid の系を写す)・作業場(値 A・B・右辺)・
//   作業用の 128bit の語(容量・対角・面の係数・残差の計器。並びは common/equilibrium.hlsli の EqGpu*Word)・粗い段の段・残差の計器の段。
// 法則が持つもの: 段 0 を仕上げる段(同じスレッドで equilibrium_core.hlsli の EqGpuFinishNode・EqGpuSetRhs を呼ぶ。結び付けは
//   NodesBuffer・LinksBuffer・ListsBuffer・WorkBuffer・ScratchBuffer と上限 MaxNodes・MaxLinks)と、解いた値(作業場の値 A)を当てる段。
// 1 回の解き = 法則の段 0 → RecordLevels(粗い段の重み)→ RecordSolve(V サイクルをちょうど solver.cycles 回)→ RecordMeter(残差の計器。任意)。
// 今は節がどれも同じレベルの系(気体の箱)と、閉じない解き(全部の節が未知数)だけ。閉じた領域の GPU 版は T-0939・
// 違うレベルの節(木のセル)と GPU で段を作るのは T-0918 の作業場の詰めと一緒に。
// データの流れ: 法則の状態(VRAM)→ 法則の段 0 → この部品の段 → 作業場の値 A → 法則の当てる段。CPU は記録だけ(D-107)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "sim/gpu_implicit.h"
#include "sim/implicit_conduction.h"

namespace bicameral::sim {

    struct GpuEquilibriumOptions {
        ImplicitOptions solver = {.method = ImplicitMethod::Multigrid, .cycles = 2};  // V サイクルの回数(ちょうど)
        GpuImplicitRecordShape shape = {.fixedCycles = true};  // V の記録の形(値は同じ。fixedCycles は必ず立てる)
        // 段の形は刻みで変わりうる(段の数・粗い段の隣)ので、上限を「最初の形 × headroomPercent ÷ 100 + 64」にする(T-0845 の 2 倍。詰めは T-0918)
        uint32_t headroomPercent = 200;
        bool meterResiduals = false;  // 残差の計器の段を積む(試験と記録。既定は積まない)
    };

    class GpuEquilibrium {
    public:
        // 部品の GPU 版が解ける系か(どの段も節が同じレベル・粗い段の行は ROW_MAX まで)
        [[nodiscard]] static bool Supports(const ImplicitGrid& grid);

        [[nodiscard]] static std::expected<GpuEquilibrium, std::string> Create(ID3D12Device5* device,
                                                                               const ImplicitGrid& grid,
                                                                               const GpuEquilibriumOptions& options);

        // 段の形を写す(陰解法のバッファが COMMON の時。作業場〔前の答え〕は消えない)。zeroValues なら前の答えを 0 に(最初だけ)
        [[nodiscard]] bool RecordUpload(ID3D12GraphicsCommandList* list, const ImplicitGrid& grid, bool zeroValues);
        // リストの初めで、形を写さない時(バッファを UAV に戻すだけ)
        void RecordBegin(ID3D12GraphicsCommandList* list) { m_implicit.RecordReset(list); }
        // リストの終わり(バッファを COMMON へ。読み戻しの後に)
        void RecordEnd(ID3D12GraphicsCommandList* list) { m_implicit.RecordRelease(list); }

        // 粗い段(段 1 から 1 段 1 Dispatch)→ V サイクル → 残差の計器(meterResiduals の時)。法則の段 0 の後に
        void RecordLevels(ID3D12GraphicsCommandList* list, uint64_t debugRing) const;
        [[nodiscard]] bool RecordSolve(ID3D12GraphicsCommandList* list, uint64_t debugRing);
        void RecordMeter(ID3D12GraphicsCommandList* list, uint64_t debugRing) const;

        // 残差の計器の最大(式の単位)を読み戻す(RecordMeter の後。RecordEnd の前)
        void RecordResidualReadback(ID3D12GraphicsCommandList* list);
        [[nodiscard]] bool ReadWorstResidual(uint64_t& worst) const;

        void SetSolveShape(GpuImplicitRecordShape shape) {
            shape.fixedCycles = true;
            m_options.shape = shape;
        }

        void RecordCostReadback(ID3D12GraphicsCommandList* list) { m_implicit.RecordCostReadback(list); }
        [[nodiscard]] bool ReadCost(GpuImplicitCost& cost) const { return m_implicit.ReadCost(cost); }

        // --- 法則の段 0 の結び付け ---
        [[nodiscard]] ID3D12Resource* NodesBuffer() const { return m_implicit.NodesBuffer(); }
        [[nodiscard]] ID3D12Resource* LinksBuffer() const { return m_implicit.LinksBuffer(); }
        [[nodiscard]] ID3D12Resource* ListsBuffer() const { return m_implicit.ListsBuffer(); }
        [[nodiscard]] ID3D12Resource* WorkBuffer() const { return m_implicit.WorkBuffer(); }
        [[nodiscard]] ID3D12Resource* ScratchBuffer() const { return m_scratch.Get(); }
        [[nodiscard]] uint32_t MaxNodes() const { return m_implicit.Limits().nodes; }
        [[nodiscard]] uint32_t MaxLinks() const { return m_implicit.Limits().links; }
        [[nodiscard]] uint32_t Cells() const { return m_cells; }

        // VRAM(既定のヒープのバッファの合計・アップロードと読み戻しを含む。T-0918 の測り)
        [[nodiscard]] uint64_t DeviceBytes() const;

        static constexpr uint32_t ROW_MAX = 6;  // 粗い段の行の上限(equilibrium_core.hlsli の EQ_GPU_ROW_MAX)

    private:
        enum class Pass : uint8_t { Level, Meter, Count };

        // b0(equilibrium.hlsl の EquilibriumConstants と同じ順)
        struct Constants {
            uint32_t maxNodes = 0;
            uint32_t maxLinks = 0;
            uint32_t levelStart = 0;
            uint32_t levelCount = 0;
            uint32_t cellCount = 0;
        };

        explicit GpuEquilibrium(GpuImplicit implicit) : m_implicit(std::move(implicit)) {}

        void RecordPass(ID3D12GraphicsCommandList* list, uint64_t debugRing, Pass pass, const Constants& constants,
                        uint32_t threads) const;

        GpuImplicit m_implicit;
        GpuEquilibriumOptions m_options;
        uint32_t m_cells = 0;
        std::vector<uint32_t> m_levelOffsets;  // 段ごとの節の始まり(段の数 + 1。今の段の形)

        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::array<ComPtr<ID3D12PipelineState>, (size_t)Pass::Count> m_pipelines;
        ComPtr<ID3D12Resource> m_scratch;   // 作業用の 128bit の語と残差の計器
        ComPtr<ID3D12Resource> m_readback;  // 残差の計器の最大(1 語)
    };

}  // namespace bicameral::sim
