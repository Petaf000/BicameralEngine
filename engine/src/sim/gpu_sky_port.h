// gpu_sky_port.h — 空との交換の口を GPU で(Compute。shaders/sim/sky_port.hlsl。T-1147・ADR-0474)。CPU リファレンスは sim/sky_port.h の SweepSkyPort。
// 多重解像度の木(sim/gpu_multires)のルート署名と外のバッファ u4・u5 を借りる(太陽の影の柱と同じ形)。木のセルは読むだけ。
//
// データの流れ: CPU の MakeSkyPortWork(向きごとの SunParams・帯ごとの側)+ 物質の光の値(表から。作る時に 1 度)→ 作業のバッファ(u4。既定のヒープへ写す)
//   → SkyPortClear(両方の側を 0 に)・SkyPortSweep(向きを全部 1 回の起動で)→ 出力(u5)→ 読み戻し(SkyPortResult。CPU と同じ並び)
// 太陽の影の柱を掃き 1 本ずつ起動する形(T-1145)より軽い: 0 にする段が 1 回・帯の正と負を 1 本の光線にまとめる・光線の数が GPU を埋める。
//
// 使い方(作業の写しは 1 本のリストに 1 回まで。前のリストが終わってから次を記録する。掃くのは何回でも):
//   auto port = GpuSkyPort::Create(device, gpu, table, nest.capacity.worldBlocks);
//   port->RecordWorkUpload(list, work);  port->RecordSweep(list, gpu, ring);  port->RecordReadback(list);  → 投げて待つ → port->Read()
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "sim/gpu_multires.h"
#include "sim/sky_port.h"

namespace bicameral::sim {

    class GpuSkyPort {
    public:
        // 作る(パイプラインは nest のルート署名で。出力は世界の枠 worldBlocks の数から)
        [[nodiscard]] static std::expected<GpuSkyPort, std::string> Create(ID3D12Device5* device,
                                                                           const GpuMultires& nest,
                                                                           const BakedReactionTable& table,
                                                                           uint32_t worldBlocks);

        // 作業(MakeSkyPortWork)を写す。1 本のリストに 1 回
        [[nodiscard]] bool RecordWorkUpload(ID3D12GraphicsCommandList10* list, const SkyPortWork& work);

        // 出力を 0 にして掃く(最後に写した作業で。掃きが 0 本なら 0 にするだけ)
        void RecordSweep(ID3D12GraphicsCommandList10* list, GpuMultires& nest, D3D12_GPU_VIRTUAL_ADDRESS debugRing);

        void RecordReadback(ID3D12GraphicsCommandList10* list);

        // 読み戻した出力(CPU の SweepSkyPort と同じ並び・数。読めなければ語が空)
        [[nodiscard]] SkyPortResult Read() const;

    private:
        GpuSkyPort() = default;

        ComPtr<ID3D12PipelineState> m_clearPipeline;
        ComPtr<ID3D12PipelineState> m_sweepPipeline;
        ComPtr<ID3D12Resource>
            m_work;  // u4: [見出し][SunParams × SKY_PORT_SWEEP_LIMIT][LtSpeciesLight × 物質](既定のヒープ)
        ComPtr<ID3D12Resource> m_workUpload;  // その写しの元(CPU が書く)
        ComPtr<ID3D12Resource> m_output;      // u5: [側 0 の語][側 1 の語]
        ComPtr<ID3D12Resource> m_readback;
        std::vector<reaction::LtSpeciesLight> m_lights;
        uint32_t m_sideWords = 0;  // 1 つの側の語の数(太陽の影の柱と同じ)

        // --- 最後に写した作業 ---
        uint32_t m_rayCount = 0;  // 向きの光線の数の和
        uint32_t m_sweepCount = 0;
        uint32_t m_sideBits = 0;  // bit 0 = 短い帯・bit 1 = 長い帯(1 = 負の側)
    };

}  // namespace bicameral::sim
