// probe_view.cpp — T-0004 の仮の世界を描く。使い方は probe_view.h。
#include "render/probe_view.h"

#include <array>

#include "core/hresult.h"
#include "core/log.h"
#include "gpu/com_ptr.h"
#include "gpu/resources.h"

namespace bicameral::render {
    namespace {

        // t0〜t2 抽出の 3 組・t3 フレームの定数(probe_view.hlsl)
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{.srvCount = 4};
        constexpr std::array<float, 4> CLEAR_COLOR = {0.02f, 0.02f, 0.03f, 1.0f};

    }  // namespace

    std::expected<ProbeView, std::string> ProbeView::Create(ID3D12Device* device, DXGI_FORMAT renderTargetFormat) {
        ProbeView view;
        view.m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!view.m_rootSignature)
            return std::unexpected("描画のルート署名を作れない");

        const auto vertexShader = gpu::LoadShader("render/probe_view_vs.cso");
        const auto pixelShader = gpu::LoadShader("render/probe_view_ps.cso");
        if (!vertexShader)
            return std::unexpected(vertexShader.error());

        if (!pixelShader)
            return std::unexpected(pixelShader.error());

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{
            .pRootSignature = view.m_rootSignature.Get(),
            .VS = {.pShaderBytecode = vertexShader->data(), .BytecodeLength = vertexShader->size()},
            .PS = {.pShaderBytecode = pixelShader->data(), .BytecodeLength = pixelShader->size()},
            .BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT),
            .SampleMask = UINT_MAX,
            .RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT),
            .DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT),
            .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
            .NumRenderTargets = 1,
            .SampleDesc = {.Count = 1},
        };

        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.DepthStencilState.DepthEnable = FALSE;  // 深度バッファは使わない
        desc.RTVFormats[0] = renderTargetFormat;
        const HRESULT result = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&view.m_pipeline));
        if (FAILED(result))
            return std::unexpected("描画のパイプラインを作れない: " + DescribeHresult(result));

        return view;
    }

    void ProbeView::Record(ID3D12GraphicsCommandList* list, const ProbeViewTarget& target,
                           const ProbeViewInputs& inputs) const {
        const D3D12_RESOURCE_BARRIER toRenderTarget = gpu::Transition(target.backBuffer, D3D12_RESOURCE_STATE_PRESENT,
                                                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &toRenderTarget);

        // --- 描く ---
        list->OMSetRenderTargets(1, &target.renderTargetView, FALSE, nullptr);
        list->ClearRenderTargetView(target.renderTargetView, CLEAR_COLOR.data(), 0, nullptr);
        const D3D12_VIEWPORT viewport{
            .Width = static_cast<float>(target.width), .Height = static_cast<float>(target.height), .MaxDepth = 1.0f};
        const D3D12_RECT scissor{
            .left = 0, .top = 0, .right = static_cast<LONG>(target.width), .bottom = static_cast<LONG>(target.height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRootSignature(m_rootSignature.Get());
        list->SetGraphicsRootShaderResourceView(ROOT_LAYOUT.SrvIndex(0), inputs.extraction0);
        list->SetGraphicsRootShaderResourceView(ROOT_LAYOUT.SrvIndex(1), inputs.extraction1);
        list->SetGraphicsRootShaderResourceView(ROOT_LAYOUT.SrvIndex(2), inputs.extraction2);
        list->SetGraphicsRootShaderResourceView(ROOT_LAYOUT.SrvIndex(3), inputs.constants);
        list->SetPipelineState(m_pipeline.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);

        const D3D12_RESOURCE_BARRIER toPresent = gpu::Transition(target.backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                 D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &toPresent);
    }

}  // namespace bicameral::render
