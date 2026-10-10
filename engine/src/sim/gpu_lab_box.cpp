// gpu_lab_box.cpp — 実験室の箱の GPU 側(gpu_lab_box.h)。CPU リファレンスは sim/lab_box.cpp の StepLabBox。
#include "sim/gpu_lab_box.h"

#include <cstring>

#include "gpu/resources.h"

namespace bicameral::sim {

    std::expected<GpuLabBox, std::string> GpuLabBox::Create(ID3D12Device5* device, const BakedReactionTable& table) {
        auto nest = GpuMultires::Create(device, table, LabBoxCapacity(table));
        if (!nest)
            return std::unexpected(nest.error());

        GpuLabBox result(std::move(*nest));
        result.m_speciesCount = static_cast<uint32_t>(table.species.size());

        const auto shader = gpu::LoadShader("sim/lab_box_apply.cso");
        if (!shader)
            return std::unexpected(shader.error());

        const auto remapShader = gpu::LoadShader("sim/lab_box_remap.cso");
        if (!remapShader)
            return std::unexpected(remapShader.error());

        result.m_applyPipeline = gpu::CreateComputePipeline(device, result.m_nest.RootSignature(), *shader);
        result.m_remapPipeline = gpu::CreateComputePipeline(device, result.m_nest.RootSignature(), *remapShader);
        if (!result.m_applyPipeline || !result.m_remapPipeline)
            return std::unexpected("実験室の箱のパイプラインを作れない");

        // --- コマンドの列(u4)と写し ---
        constexpr uint64_t COMMAND_LIST_BYTES = uint64_t{LAB_MAX_COMMANDS_PER_TICK} * COMMAND_BYTES;
        result.m_commands = gpu::CreateBuffer(device, COMMAND_LIST_BYTES, gpu::BufferKind::UnorderedAccess);
        result.m_commandUpload = gpu::CreateBuffer(device, COMMAND_LIST_BYTES, gpu::BufferKind::Upload);
        if (!result.m_commands || !result.m_commandUpload)
            return std::unexpected("実験室の箱のバッファを作れない");

        return result;
    }

    bool GpuLabBox::RecordUpload(ID3D12GraphicsCommandList10* list, const MultiresNest& nest) {
        return m_nest.RecordUpload(list, nest);
    }

    bool GpuLabBox::RecordCommandUpload(ID3D12GraphicsCommandList10* list, std::span<const Command> commands) {
        void* mapped = nullptr;
        const D3D12_RANGE noRead{.Begin = 0, .End = 0};
        if (FAILED(m_commandUpload->Map(0, &noRead, &mapped)))
            return false;

        std::memcpy(mapped, commands.data(), commands.size_bytes());
        m_commandUpload->Unmap(0, nullptr);

        // 列のバッファは COMMON から COPY_DEST へ暗黙に上がる(バッファはリストの終わりに COMMON へ戻る)
        list->CopyBufferRegion(m_commands.Get(), 0, m_commandUpload.Get(), 0, commands.size_bytes());
        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(m_commands.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);

        return true;
    }

    bool GpuLabBox::RecordTick(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick,
                               std::span<const Command> commands) {
        if (commands.size() > LAB_MAX_COMMANDS_PER_TICK)
            return false;

        // --- コマンドを当てる(無ければ投げない。CPU の ApplyLabCommands も何もしない)---
        if (!commands.empty()) {
            if (!RecordCommandUpload(list, commands))
                return false;

            const D3D12_GPU_VIRTUAL_ADDRESS commandAddress = m_commands->GetGPUVirtualAddress();
            m_nest.SetExternalViews(commandAddress, commandAddress);
            m_nest.RecordExternalDispatch(list, debugRing, m_applyPipeline.Get(), 1,
                                          {static_cast<uint32_t>(commands.size()), LAB_BOX_SLOT, m_speciesCount, 0});
        }

        m_nest.RecordStep(list, debugRing, LAB_WORLD_SEED, tick, LAB_STEP_OPTIONS);

        return true;
    }

    // 物質の付け替え(T-0242): 付け替えの表を u4 に写し、箱の全部のセルを 1 スレッド 1 セルで付け替える
    bool GpuLabBox::RecordSpeciesRemap(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                       std::span<const uint32_t> remapWords, uint32_t cellCount) {
        ComPtr<ID3D12Device> device;
        if (remapWords.empty() || FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
            return false;

        const uint64_t bytes = remapWords.size_bytes();
        m_remapUpload = gpu::CreateBuffer(device.Get(), bytes, gpu::BufferKind::Upload);
        m_remap = gpu::CreateBuffer(device.Get(), bytes, gpu::BufferKind::UnorderedAccess);
        if (!m_remapUpload || !m_remap)
            return false;

        void* mapped = nullptr;
        const D3D12_RANGE noRead{.Begin = 0, .End = 0};
        if (FAILED(m_remapUpload->Map(0, &noRead, &mapped)))
            return false;

        std::memcpy(mapped, remapWords.data(), bytes);
        m_remapUpload->Unmap(0, nullptr);

        // --- 写して u4 に結び、付け替える ---
        list->CopyBufferRegion(m_remap.Get(), 0, m_remapUpload.Get(), 0, bytes);
        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(m_remap.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);

        constexpr uint32_t REMAP_GROUP_THREADS = 64;  // lab_box.hlsl の RemapLabSpecies と同じ
        const D3D12_GPU_VIRTUAL_ADDRESS remapAddress = m_remap->GetGPUVirtualAddress();
        m_nest.SetExternalViews(remapAddress, remapAddress);
        m_nest.RecordExternalDispatch(list, debugRing, m_remapPipeline.Get(),
                                      (cellCount + REMAP_GROUP_THREADS - 1) / REMAP_GROUP_THREADS,
                                      {cellCount, 0, 0, 0});

        return true;
    }

}  // namespace bicameral::sim
