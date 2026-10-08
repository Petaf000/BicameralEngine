// gpu_implicit_levels.h — 細かいレベルの熱の陰解法(方式②。ADR-0019)の多重格子の段と重みを GPU で作る(T-0134)。
// 入力は GpuImplicitBuild(T-0129)が作った系(セル・面・セルの面の一覧)とセルの座標。CPU リファレンスは implicit_conduction.cpp の
// BuildImplicitGrid(段 0 = セル → 最も細かいレベルの節を親のセルへ縮約 → 重み〔128bit の割り算〕→ 重み < 1/2 の節がある間は次の段)。
// 作るもの(節 ImGpuNode・隣 ImGpuLink・子の一覧)は GpuImplicit の段の形と同じ並び・同じ番号なので、そのまま写して解ける(RecordCopyTo)。
// 段の数は値で決まる: 回は上限(limits.levels − 1 回)だけ積み、要らない回は述語(SetPredication)で飛ばす。段の中身は shaders/sim/implicit_levels.hlsl。
// 節の並び(長い行・色ごと)・ImTail の境・間接の Dispatch・GpuImplicit の大きさを上限から決めるのは T-0135(今は CPU の系の形で Create する)。
//
// 使い方(テスト):
//   auto levels = GpuImplicitLevels::Create(device, build, limits);
//   build.RecordBuild(...);  levels->RecordBuild(list, ring, build);  → levels->RecordReadback(list) / levels->RecordCopyTo(list, implicit)
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "common/implicit_conduction.hlsli"
#include "gpu/com_ptr.h"
#include "sim/gpu_implicit.h"
#include "sim/gpu_implicit_build.h"

namespace bicameral::sim {

    // 作る段の大きさの上限(バッファの大きさ。超えたら overflow が立つ)
    struct GpuImplicitLevelLimits {
        uint32_t nodes = 0;    // 全部の段の節
        uint32_t links = 0;    // 全部の段の隣
        uint32_t levels = 64;  // 段の数(CPU の MAX_GRID_LEVELS。回はこれ − 1 回積む)
    };

    // GpuImplicit の段の形の像(CPU の ImplicitGrid から。比べる用。gpu_implicit.cpp の MakeNodes と同じ)
    struct GpuImplicitLevelImages {
        std::vector<uint32_t> levelOffsets;  // 段ごとの節の始まり(段の数 + 1)
        std::vector<multires::ImGpuNode> nodes;
        std::vector<multires::ImGpuLink> links;
        std::vector<uint32_t> children;  // 子の一覧(GpuImplicit の面の一覧の後ろの部分)
    };

    [[nodiscard]] GpuImplicitLevelImages MakeGpuImplicitLevelImages(const ImplicitGrid& grid);

    // GPU が作った段(読み戻し)
    struct GpuImplicitLevelSystem {
        bool overflow = false;
        GpuImplicitLevelImages images;
        std::vector<uint32_t> linkOffsets;  // 段ごとの隣の始まり(段の数 + 1)
    };

    class GpuImplicitLevels {
    public:
        static constexpr uint32_t MAX_TIMESTAMPS = 8;

        [[nodiscard]] static std::expected<GpuImplicitLevels, std::string> Create(ID3D12Device5* device,
                                                                                  const GpuImplicitBuild& build,
                                                                                  const GpuImplicitLevelLimits& limits);

        // build が作った系から段を作る(build.RecordBuild の後)
        void RecordBuild(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                         const GpuImplicitBuild& build);

        // 作った節・隣・子の一覧を GpuImplicit へ写す(GpuImplicit の RecordUpload の後。形は GpuImplicit を作った系と同じこと)
        void RecordCopyTo(ID3D12GraphicsCommandList* list, GpuImplicit& implicit);

        void RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index);
        void RecordReadback(ID3D12GraphicsCommandList* list);
        [[nodiscard]] std::expected<GpuImplicitLevelSystem, std::string> Read() const;
        [[nodiscard]] std::vector<uint64_t> ReadTimestamps(uint32_t count) const;

        // 1 回の RecordBuild に積む Dispatch の数(計測の表に使う)
        [[nodiscard]] uint32_t DispatchCount() const;

    private:
        GpuImplicitLevels() = default;

        struct Constants {
            uint32_t maxCells = 0;
            uint32_t maxNodes = 0;
            uint32_t maxLinks = 0;
            uint32_t levelLinks = 0;
            uint32_t depth = 0;
            uint32_t maxLevels = 0;
            uint32_t nodeTableMask = 0;
            uint32_t linkTableMask = 0;
            uint32_t keysWord = 0;
            uint32_t listCellsWord = 0;
            uint32_t facesByte = 0;
            uint32_t listsByte = 0;
        };

        void Dispatch(ID3D12GraphicsCommandList* list, uint32_t pass, uint32_t groups);
        void BeginSkippable(ID3D12GraphicsCommandList* list, uint32_t coarsening);
        void EndSkippable(ID3D12GraphicsCommandList* list);

        Constants m_constants;
        uint64_t m_workBytes = 0;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::vector<ComPtr<ID3D12PipelineState>> m_pipelines;  // implicit_levels.hlsl の段の順
        ComPtr<ID3D12Resource> m_work;
        ComPtr<ID3D12Resource> m_nodes;
        ComPtr<ID3D12Resource> m_links;
        ComPtr<ID3D12Resource> m_children;
        ComPtr<ID3D12Resource> m_predicate;
        ComPtr<ID3D12Resource> m_headerReadback;
        ComPtr<ID3D12Resource> m_nodesReadback;
        ComPtr<ID3D12Resource> m_linksReadback;
        ComPtr<ID3D12Resource> m_childrenReadback;
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
        uint32_t m_timestampCount = 0;
    };

}  // namespace bicameral::sim
