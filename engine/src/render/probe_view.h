// probe_view.h — 仮の世界(sim/probe_sim の 64³ の熱)のデバッグ表示を描く(shaders/render/probe_view.hlsl。T-0004・T-0015)。
//
// データの流れ: シミュの抽出(3 組。compute キューが書く。全部のセル + ブロックの活性の印)
//   + フレームの定数(CPU がフレームごとに書く: どの抽出を読むか・大きさ・表示・カメラ。render/probe_view_constants.h)
//   → 画面いっぱいの三角形 1 枚(画素ごとに格子を光線で辿る)→ バックバッファ。
// Record() はフレームのループ(frame/frame_loop)がバックバッファごとに 1 度だけ呼び、そのリストを毎フレーム使い回す。
// 本物の描画(10-rendering.md)は M7 から。これは原理の確認(M1)の間、世界の中を見るための道具。
#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "core/aliases.h"
#include "gpu/com_ptr.h"
#include "render/probe_view_constants.h"

namespace bicameral::render {

    struct ProbeViewTarget {
        ID3D12Resource* backBuffer = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView{};
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct ProbeViewInputs {
        D3D12_GPU_VIRTUAL_ADDRESS extraction0 = 0;
        D3D12_GPU_VIRTUAL_ADDRESS extraction1 = 0;
        D3D12_GPU_VIRTUAL_ADDRESS extraction2 = 0;
        D3D12_GPU_VIRTUAL_ADDRESS constants = 0;  // ProbeViewConstants
    };

    class ProbeView {
    public:
        [[nodiscard]] static expected<ProbeView, std::string> Create(ID3D12Device* device,
                                                                     DXGI_FORMAT renderTargetFormat);

        // PRESENT → RENDER_TARGET → 描く → PRESENT を記録する
        void Record(ID3D12GraphicsCommandList* list, const ProbeViewTarget& target,
                    const ProbeViewInputs& inputs) const;

    private:
        ProbeView() = default;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_pipeline;
    };

}  // namespace bicameral::render
