// gpu_implicit_build.h — 細かいレベルの熱の陰解法(方式②。ADR-0019)の系を GPU の木から作る(T-0129)。
// 作るもの: 未知数(基準より細かい本物のブロックのセル)・境のセル・面(係数・粗い側の端数の枠)・セルの面の一覧。
// 番号の付け方は CPU リファレンス(multires_implicit_conduction.cpp の MakeSystem と gpu_implicit.cpp の MakeCellFaces)と同じなので、
// 作った系は GpuImplicit の同じバッファの並びにそのまま写せる(RecordCopyTo)。段の中身は shaders/sim/implicit_build.hlsl。
// 多重格子の段と重みは GpuImplicitLevels(T-0134)がこの系から作る。GPU の伝導の段から呼ぶ形は GpuMultiresImplicit(T-0132)。
//
// 使い方(テスト):
//   auto build = GpuImplicitBuild::Create(device, multires, nest.capacity, limits);
//   multires.RecordUpload(list, nest);                  // 系を作る時の木(陰解法を入れる刻みの、陽解法の流れの後)
//   build->RecordBuild(list, ring, multires, options);  → build->RecordReadback(list) / build->RecordCopyTo(list, implicit)
// GpuMultires のルート署名で投げる(外のバッファ u4・u5 を自分の作業場と系に結ぶ)。Dispatch の数は上限から決める(間接の引数は T-0135)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "common/implicit_conduction.hlsli"
#include "gpu/com_ptr.h"
#include "sim/gpu_implicit.h"
#include "sim/gpu_multires.h"
#include "sim/multires_nest.h"

namespace bicameral::sim {

    // 作る系の大きさの上限(バッファの大きさ。超えたら作った系の overflow が立つ)
    struct GpuImplicitBuildLimits {
        uint32_t unknowns = 0;  // 未知数のセル(面は 6 倍まで)
        uint32_t cells = 0;     // 未知数 + 境のセル
    };

    // GPU が作った系(読み戻し。並びは GpuImplicit の BufferCells・BufferFaces・BufferLists の先頭と同じ)
    struct GpuImplicitSystem {
        uint32_t unknowns = 0;
        uint32_t boundary = 0;
        uint32_t faces = 0;
        bool overflow = false;
        std::vector<multires::ImGpuCell> cells;
        std::vector<multires::ImGpuFace> faceList;
        std::vector<uint32_t> lists;  // セルの面の一覧(面の番号 × 2 + 粗い側なら 1)
    };

    class GpuImplicitBuild {
    public:
        static constexpr uint32_t MAX_TIMESTAMPS = 32;

        [[nodiscard]] static std::expected<GpuImplicitBuild, std::string> Create(ID3D12Device5* device,
                                                                                 const GpuMultires& multires,
                                                                                 const MultiresCapacity& capacity,
                                                                                 const GpuImplicitBuildLimits& limits);

        // 系を作る。木は multires のバッファ(呼ぶ側が写すか刻んである)。凍った印(TreeExpand が付ける)を見るのは useFrozenMarks の時だけ
        // (見る時は multires の刻みの印〔SetTick〕がその刻みのものであること。T-0132)
        // afterAdmit: 同じ刻みに RecordAdmit を投げてある(Clear〜ScanBlocks を飛ばす。useFrozenMarks の時だけ)
        void RecordBuild(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, GpuMultires& multires,
                         const MultiresStepOptions& options, bool useFrozenMarks = false, bool afterAdmit = false);

        // 伝導の段の流れの前(T-0178): Clear → CountBlocks → ScanBlocks で、系に入れるブロックを枠の順に上限(作った時の limits =
        // 予算)の中まで選び、陰解法の印を付ける。凍った印を見る。同じ刻みの後で RecordBuild(…, true, true)
        void RecordAdmit(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, GpuMultires& multires,
                         const MultiresStepOptions& options);

        // GpuImplicit が解いたセル(solvedCells = GpuImplicit::CellsBuffer。UAV の状態)の「足した後 − 刻みの初め」を multires の伝導の
        // 変化の表へ足し、活性の刻みならそのブロックを伝導の一覧へ(T-0132。RecordBuild と同じ刻みの印・定数で、伝導の段の流れの後に)
        void RecordApply(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, GpuMultires& multires,
                         const MultiresStepOptions& options, ID3D12Resource* solvedCells);

        // 作った系を GpuImplicit のセル・面・面の一覧へ写す(GpuImplicit の RecordUpload の後。数は GpuImplicit を作った系と同じこと)
        void RecordCopyTo(ID3D12GraphicsCommandList* list, GpuImplicit& implicit);

        // 計測用: true なら RecordBuild が段ごとにタイムスタンプ(段 i の前に i、最後の段の後に段の数)を打つ
        void StampPasses(bool stamp) { m_stampPasses = stamp; }
        [[nodiscard]] static uint32_t StageCount();
        [[nodiscard]] static const char* StageName(uint32_t pass);

        void RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index);
        void RecordReadback(ID3D12GraphicsCommandList* list);
        [[nodiscard]] std::expected<GpuImplicitSystem, std::string> Read() const;
        [[nodiscard]] std::vector<uint64_t> ReadTimestamps(uint32_t count) const;

        // --- 多重格子の段を作る段(GpuImplicitLevels。T-0134)が読むもの ---
        [[nodiscard]] const GpuImplicitBuildLimits& Limits() const { return m_limits; }
        [[nodiscard]] ID3D12Resource* WorkBuffer() const { return m_work.Get(); }
        [[nodiscard]] ID3D12Resource* SystemBuffer() const { return m_system.Get(); }
        [[nodiscard]] uint64_t FacesOffset() const;
        [[nodiscard]] uint64_t ListsOffset() const;
        [[nodiscard]] uint32_t ListCellsWord() const;  // 作業場の「面の一覧の位置のセル」の始まり(語)
        [[nodiscard]] uint32_t CellKeysWord() const;   // 作業場のセルの座標の始まり(語。8 語 / セル)

    private:
        GpuImplicitBuild() = default;

        // implicit_build.hlsl の段 [firstPass, endPass) を投げる
        void RecordPasses(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, GpuMultires& multires,
                          const MultiresStepOptions& options, bool useFrozenMarks, uint32_t firstPass,
                          uint32_t endPass);
        [[nodiscard]] std::array<uint32_t, 4> ExternalConstants(const MultiresStepOptions& options,
                                                                bool useFrozenMarks) const;

        GpuImplicitBuildLimits m_limits;
        uint32_t m_worldBlocks = 0;
        uint64_t m_workBytes = 0;
        uint64_t m_systemBytes = 0;

        std::vector<ComPtr<ID3D12PipelineState>> m_pipelines;  // implicit_build.hlsl の段の順
        ComPtr<ID3D12PipelineState> m_applyPipeline;           // 解いた変化を伝導の表へ(BuildApply。T-0132)
        ComPtr<ID3D12Resource> m_work;                         // u4 作業場
        ComPtr<ID3D12Resource> m_system;                       // u5 系
        ComPtr<ID3D12Resource> m_workReadback;
        ComPtr<ID3D12Resource> m_systemReadback;
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
        uint32_t m_timestampCount = 0;
        bool m_stampPasses = false;
    };

}  // namespace bicameral::sim
