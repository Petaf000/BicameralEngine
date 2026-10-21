// gpu_sun_column.cpp — 太陽の影の柱の GPU 側(gpu_sun_column.h)。CPU リファレンスは sim/sun_column.cpp の SweepSunColumn。
#include "sim/gpu_sun_column.h"

#include <cstring>

#include "gpu/resources.h"

namespace bicameral::sim {

    namespace mr = ::bicameral::multires;

    namespace {

        constexpr uint32_t SUN_GROUP_THREADS = 64;  // shaders/sim/sun_column.hlsl の SUN_GROUP_THREADS

        // HLSL の Load<SunParams> と同じ並び(64bit の欄が先・詰めて 128 バイト)
        static_assert(sizeof(mr::SunParams) == mr::SUN_WORK_PARAMS_BYTES);
        static_assert(sizeof(reaction::LtSpeciesLight) == mr::SUN_SPECIES_LIGHT_BYTES);

        uint32_t GroupsFor(uint32_t threads) {
            return (threads + SUN_GROUP_THREADS - 1) / SUN_GROUP_THREADS;
        }

    }  // namespace

    std::expected<GpuSunColumn, std::string> GpuSunColumn::Create(ID3D12Device5* device, const GpuMultires& nest,
                                                                  const BakedReactionTable& table,
                                                                  uint32_t worldBlocks) {
        GpuSunColumn result;
        result.m_lights = table.speciesLight;
        result.m_wordCount = mr::SUN_LEDGER_WORDS + (worldBlocks * mr::MR_BLOCK_CELLS * mr::SUN_CELL_WORDS);

        // --- パイプライン(木のルート署名を借りる)---
        const auto clearShader = gpu::LoadShader("sim/sun_column_clear.cso");
        if (!clearShader)
            return std::unexpected(clearShader.error());

        const auto sweepShader = gpu::LoadShader("sim/sun_column_sweep.cso");
        if (!sweepShader)
            return std::unexpected(sweepShader.error());

        ID3D12RootSignature* root = nest.RootSignature();
        result.m_clearPipeline = gpu::CreateComputePipeline(device, root, *clearShader);
        result.m_sweepPipeline = gpu::CreateComputePipeline(device, root, *sweepShader);
        if (!result.m_clearPipeline || !result.m_sweepPipeline)
            return std::unexpected("太陽の影の柱のパイプラインを作れない");

        // --- バッファ ---
        const uint64_t workBytes = mr::SUN_WORK_PARAMS_BYTES +
                                   (uint64_t{mr::SUN_SPECIES_LIGHT_BYTES} * result.m_lights.size());
        const uint64_t outputBytes = uint64_t{result.m_wordCount} * sizeof(uint64_t);
        result.m_work = gpu::CreateBuffer(device, workBytes, gpu::BufferKind::UnorderedAccess);
        result.m_workUpload = gpu::CreateBuffer(device, workBytes, gpu::BufferKind::Upload);
        result.m_output = gpu::CreateBuffer(device, outputBytes, gpu::BufferKind::UnorderedAccess);
        result.m_readback = gpu::CreateBuffer(device, outputBytes, gpu::BufferKind::Readback);
        if (!result.m_work || !result.m_workUpload || !result.m_output || !result.m_readback)
            return std::unexpected("太陽の影の柱のバッファを作れない");

        return result;
    }

    bool GpuSunColumn::RecordWorkUpload(ID3D12GraphicsCommandList10* list, const mr::SunParams& params) {
        // --- 作業(掃き方 + 物質の光の値)を写す。シェーダーが毎回読むので既定のヒープへ(T-0374)---
        void* mapped = nullptr;
        const D3D12_RANGE noRead{.Begin = 0, .End = 0};
        if (FAILED(m_workUpload->Map(0, &noRead, &mapped)))
            return false;

        auto* bytes = static_cast<std::byte*>(mapped);
        std::memcpy(bytes, &params, sizeof(params));
        std::memcpy(bytes + mr::SUN_WORK_PARAMS_BYTES, m_lights.data(),
                    m_lights.size() * sizeof(reaction::LtSpeciesLight));
        m_workUpload->Unmap(0, nullptr);

        const uint64_t workBytes = mr::SUN_WORK_PARAMS_BYTES +
                                   (uint64_t{mr::SUN_SPECIES_LIGHT_BYTES} * m_lights.size());
        list->CopyBufferRegion(m_work.Get(), 0, m_workUpload.Get(), 0, workBytes);
        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(m_work.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
        m_rayCount = params.rayCountB * params.rayCountC;

        return true;
    }

    void GpuSunColumn::RecordSweep(ID3D12GraphicsCommandList10* list, GpuMultires& nest,
                                   D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        // 0 にして掃く(RecordExternalDispatch が後ろに UAV のバリアを入れる)
        nest.SetExternalViews(m_work->GetGPUVirtualAddress(), m_output->GetGPUVirtualAddress());
        nest.RecordExternalDispatch(list, debugRing, m_clearPipeline.Get(), GroupsFor(m_wordCount),
                                    {m_wordCount, 0, 0, 0});
        nest.RecordExternalDispatch(list, debugRing, m_sweepPipeline.Get(), GroupsFor(m_rayCount),
                                    {m_rayCount, 0, 0, 0});
        nest.SetExternalViews(0, 0);
    }

    void GpuSunColumn::RecordReadback(ID3D12GraphicsCommandList10* list) {
        gpu::RecordCopyToReadback(list, m_output.Get(), m_readback.Get());
    }

    std::vector<uint64_t> GpuSunColumn::Read() const {
        std::vector<uint64_t> words(m_wordCount);
        if (!gpu::ReadBuffer(m_readback.Get(), std::as_writable_bytes(std::span(words))))
            words.clear();

        return words;
    }

}  // namespace bicameral::sim
