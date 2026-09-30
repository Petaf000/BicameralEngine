// probe_view.h — T-0004 の仮の世界(sim/probe_sim)を描く(shaders/render/probe_view.hlsl)。
//
// データの流れ: シミュの抽出(3 組。compute キューが書く)+ フレームの定数(CPU がフレームごとに書く: どちらの抽出を読むか・大きさ)
//   → 画面いっぱいの三角形 1 枚 → バックバッファ。
// Record() はフレームのループ(frame/frame_loop)がバックバッファごとに 1 度だけ呼び、そのリストを毎フレーム使い回す。
// 本物の描画(10-rendering.md)は M7 から。これはフレームの形を確かめるための仮のもの。
#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace bicameral::render {

    // フレームの定数(アップロードのバッファ。probe_view.hlsl の frame と同じ並び)
    struct ProbeViewConstants {
        uint32_t extractionIndex = 0;  // 読む抽出(0〜2)
        uint32_t width = 0;            // 描く大きさ(px)
        uint32_t height = 0;
        uint32_t reserved = 0;
    };

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
        [[nodiscard]] static std::expected<ProbeView, std::string> Create(ID3D12Device* device,
                                                                          DXGI_FORMAT renderTargetFormat);

        // PRESENT → RENDER_TARGET → 描く → PRESENT を記録する
        void Record(ID3D12GraphicsCommandList* list, const ProbeViewTarget& target,
                    const ProbeViewInputs& inputs) const;

    private:
        ProbeView() = default;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
    };

}  // namespace bicameral::render
