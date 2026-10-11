// probe_surface.cpp — 物のセルの印を GPU で作って読み戻す(T-0332。説明は probe_surface.h)。
#include "sim/probe_surface.h"

#include <algorithm>

#include "core/log.h"
#include "gpu/resources.h"

namespace bicameral::sim {

    namespace {

        // ルート署名: u0 = 世界のセル(読むだけ)・u1 = 印・b0 = 読む世代の先頭(shaders/sim/probe_surface.hlsl)
        constexpr gpu::RootSignatureLayout SURFACE_ROOT_LAYOUT{.uavCount = 2, .rootConstantCount = 1};
        constexpr uint32_t SURFACE_GROUP_THREADS = 64;  // probe_surface.hlsl の SURFACE_GROUP_THREADS
        constexpr uint64_t SURFACE_BYTES = uint64_t{PROBE_SURFACE_WORDS} * 4;

    }  // namespace

    std::vector<uint32_t> BuildProbeSurfaceMask(std::span<const reaction::RxCell> cells) {
        std::vector<uint32_t> words(PROBE_SURFACE_WORDS, 0);
        const size_t count = std::min<size_t>(cells.size(), PROBE_CELL_COUNT);
        for (size_t index = 0; index < count; ++index) {
            if (ProbeIsSolidCell(cells[index], PROBE_SURFACE_THRESHOLD_MICROMOLES))
                words[index / 32] |= 1u << (index % 32);
        }

        return words;
    }

    // --- GPU ---

    std::expected<ProbeSurface, std::string> ProbeSurface::Create(ID3D12Device* device) {
        ProbeSurface result;
        result.m_rootSignature = gpu::CreateRootSignature(device, SURFACE_ROOT_LAYOUT);
        if (!result.m_rootSignature)
            return std::unexpected("物の印のルート署名を作れない");

        const auto shader = gpu::LoadShader("sim/probe_surface_mask.cso");
        if (!shader)
            return std::unexpected(shader.error());

        result.m_pipeline = gpu::CreateComputePipeline(device, result.m_rootSignature.Get(), *shader);
        result.m_mask = gpu::CreateBuffer(device, SURFACE_BYTES, gpu::BufferKind::UnorderedAccess);
        if (!result.m_pipeline || !result.m_mask)
            return std::unexpected("物の印のパイプラインかバッファを作れない");

        result.m_mask->SetName(L"ProbeSurface.mask");
        for (Slot& slot : result.m_slots) {
            slot.readback = gpu::CreateBuffer(device, SURFACE_BYTES, gpu::BufferKind::Readback);
            if (!slot.readback)
                return std::unexpected("物の印の読み戻しを作れない");
        }

        return result;
    }

    void ProbeSurface::RecordAfterExtract(ID3D12GraphicsCommandList10* list, const ProbeExtractContext& context) {
        if (!m_enabled || context.slot >= m_slots.size())
            return;

        // --- 印を作る(世界のセルは UAV のまま読むだけ)---
        const uint32_t generationBase = static_cast<uint32_t>(context.tick & 1) * PROBE_CELL_COUNT;
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_pipeline.Get());
        list->SetComputeRootUnorderedAccessView(0, context.cells->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1, m_mask->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(SURFACE_ROOT_LAYOUT.RootConstantIndex(), 1, &generationBase, 0);
        list->Dispatch((PROBE_SURFACE_WORDS + SURFACE_GROUP_THREADS - 1) / SURFACE_GROUP_THREADS, 1, 1);

        // --- 読み戻しへ(印は COMMON に戻す。次のフレームの最初の書き込みで UAV に昇格する)---
        Slot& slot = m_slots[context.slot];
        gpu::RecordCopyToReadback(list, m_mask.Get(), slot.readback.Get());
        const D3D12_RESOURCE_BARRIER toCommon = gpu::Transition(m_mask.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                                D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1, &toCommon);
        slot.tick = context.tick;
        slot.recorded = true;
    }

    std::optional<ProbeSurfaceMask> ProbeSurface::Read(uint32_t slot) {
        if (slot >= m_slots.size() || !m_slots[slot].recorded)
            return std::nullopt;

        Slot& frame = m_slots[slot];
        frame.recorded = false;
        ProbeSurfaceMask mask{.tick = frame.tick, .words = std::vector<uint32_t>(PROBE_SURFACE_WORDS)};
        if (!gpu::ReadBuffer(frame.readback.Get(), std::as_writable_bytes(std::span(mask.words)))) {
            Log(Channel::Gpu, Level::Warning, "物の印を読み戻せない(枠 {})", slot);
            return std::nullopt;
        }

        return mask;
    }

}  // namespace bicameral::sim
