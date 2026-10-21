// gpu_sun_column.h — 太陽の影の柱を GPU で(Compute。shaders/sim/sun_column.hlsl。T-0910・ADR-0396)。CPU リファレンスは sim/sun_column.h。
// 多重解像度の木(sim/gpu_multires)のルート署名と外のバッファ u4・u5 を借りる(覗き窓・実験室の箱と同じ形)。木のセルは読むだけ。
//
// データの流れ: CPU の MakeSunParams(SunParams)+ 物質の光の値(表から。作る時に 1 度)→ 作業のバッファ(u4。既定のヒープへ写す)
//   → SunClear・SunSweep → 出力(u5。uint64 の語。CPU の SweepSunColumn と同じ並び)→ 読み戻し
//
// 使い方(作業の写しは 1 本のリストに 1 回まで。写しの置き場が 1 つなので、前のリストが終わってから次を記録する。掃くのは何回でも):
//   auto sun = GpuSunColumn::Create(device, gpu, table, nest.capacity.worldBlocks);
//   sun->RecordWorkUpload(list, params);  sun->RecordSweep(list, gpu, ring);  sun->RecordReadback(list);  → 投げて待つ → sun->Read()
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "sim/gpu_multires.h"
#include "sim/sun_column.h"

namespace bicameral::sim {

    class GpuSunColumn {
    public:
        // 作る(パイプラインは nest のルート署名で。出力は世界の枠 worldBlocks の数から)
        [[nodiscard]] static std::expected<GpuSunColumn, std::string> Create(ID3D12Device5* device,
                                                                             const GpuMultires& nest,
                                                                             const BakedReactionTable& table,
                                                                             uint32_t worldBlocks);

        // 作業(掃き方 params = CPU の MakeSunParams + 物質の光の値)を写す。1 本のリストに 1 回
        [[nodiscard]] bool RecordWorkUpload(ID3D12GraphicsCommandList10* list, const multires::SunParams& params);

        // 出力を 0 にして掃く(最後に写した作業で)
        void RecordSweep(ID3D12GraphicsCommandList10* list, GpuMultires& nest, D3D12_GPU_VIRTUAL_ADDRESS debugRing);

        void RecordReadback(ID3D12GraphicsCommandList10* list);

        // 読み戻した出力の語(CPU の SweepSunColumn と同じ並び・数)
        [[nodiscard]] std::vector<uint64_t> Read() const;

    private:
        GpuSunColumn() = default;

        ComPtr<ID3D12PipelineState> m_clearPipeline;
        ComPtr<ID3D12PipelineState> m_sweepPipeline;
        ComPtr<ID3D12Resource> m_work;        // u4: [SunParams][LtSpeciesLight × 物質](既定のヒープ)
        ComPtr<ID3D12Resource> m_workUpload;  // その写しの元(CPU が書く)
        ComPtr<ID3D12Resource> m_output;      // u5: 出力の語
        ComPtr<ID3D12Resource> m_readback;
        std::vector<reaction::LtSpeciesLight> m_lights;
        uint32_t m_wordCount = 0;
        uint32_t m_rayCount = 0;  // 最後に写した作業の光線の数
    };

}  // namespace bicameral::sim
