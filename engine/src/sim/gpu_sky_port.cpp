// gpu_sky_port.cpp — 空との交換の口の GPU 側(gpu_sky_port.h)。CPU リファレンスは sim/sky_port.cpp の SweepSkyPort。
#include "sim/gpu_sky_port.h"

#include <array>
#include <cstring>
#include <span>

#include "gpu/resources.h"

namespace bicameral::sim {

    namespace mr = ::bicameral::multires;

    namespace {

        constexpr uint32_t SKY_GROUP_THREADS = 64;  // shaders/sim/sky_port.hlsl の SKY_GROUP_THREADS
        constexpr uint32_t DISPATCH_GROUP_LIMIT = 65535;

        static_assert(sizeof(mr::SunParams) == mr::SUN_WORK_PARAMS_BYTES);
        static_assert(sizeof(reaction::LtSpeciesLight) == mr::SUN_SPECIES_LIGHT_BYTES);
        static_assert(mr::SKY_PORT_SWEEP_LIMIT * sizeof(uint32_t) <= mr::SKY_PORT_HEADER_BYTES);

        uint32_t GroupsFor(uint32_t threads) {
            return (threads + SKY_GROUP_THREADS - 1) / SKY_GROUP_THREADS;
        }

        uint64_t WorkBytes(size_t speciesCount) {
            return mr::SKY_PORT_PARAMS_BYTES + (uint64_t{mr::SUN_SPECIES_LIGHT_BYTES} * speciesCount);
        }

    }  // namespace

    std::expected<GpuSkyPort, std::string> GpuSkyPort::Create(ID3D12Device5* device, const GpuMultires& nest,
                                                              const BakedReactionTable& table, uint32_t worldBlocks) {
        GpuSkyPort result;
        result.m_lights = table.speciesLight;
        result.m_sideWords = mr::SUN_LEDGER_WORDS + (worldBlocks * mr::MR_BLOCK_CELLS * mr::SUN_CELL_WORDS);
        if (GroupsFor(result.m_sideWords * mr::SKY_PORT_SIDES) > DISPATCH_GROUP_LIMIT)
            return std::unexpected("空との交換の口の出力が 1 回の起動で 0 にできる数を超える(世界の枠が多すぎる)");

        // --- パイプライン(木のルート署名を借りる)---
        const auto clearShader = gpu::LoadShader("sim/sky_port_clear.cso");
        if (!clearShader)
            return std::unexpected(clearShader.error());

        const auto sweepShader = gpu::LoadShader("sim/sky_port_sweep.cso");
        if (!sweepShader)
            return std::unexpected(sweepShader.error());

        ID3D12RootSignature* root = nest.RootSignature();
        result.m_clearPipeline = gpu::CreateComputePipeline(device, root, *clearShader);
        result.m_sweepPipeline = gpu::CreateComputePipeline(device, root, *sweepShader);
        if (!result.m_clearPipeline || !result.m_sweepPipeline)
            return std::unexpected("空との交換の口のパイプラインを作れない");

        // --- バッファ ---
        const uint64_t workBytes = WorkBytes(result.m_lights.size());
        const uint64_t outputBytes = uint64_t{result.m_sideWords} * mr::SKY_PORT_SIDES * sizeof(uint64_t);
        result.m_work = gpu::CreateBuffer(device, workBytes, gpu::BufferKind::UnorderedAccess);
        result.m_workUpload = gpu::CreateBuffer(device, workBytes, gpu::BufferKind::Upload);
        result.m_output = gpu::CreateBuffer(device, outputBytes, gpu::BufferKind::UnorderedAccess);
        result.m_readback = gpu::CreateBuffer(device, outputBytes, gpu::BufferKind::Readback);
        if (!result.m_work || !result.m_workUpload || !result.m_output || !result.m_readback)
            return std::unexpected("空との交換の口のバッファを作れない");

        return result;
    }

    bool GpuSkyPort::RecordWorkUpload(ID3D12GraphicsCommandList10* list, const SkyPortWork& work) {
        if (work.sweeps.size() > mr::SKY_PORT_SWEEP_LIMIT)
            return false;

        // --- 見出し(向きごとの光線の番号の終わり)---
        std::array<uint32_t, mr::SKY_PORT_SWEEP_LIMIT> rayEnd{};
        uint64_t rays = 0;
        for (size_t sweep = 0; sweep < work.sweeps.size(); ++sweep) {
            rays += uint64_t{work.sweeps[sweep].rayCountB} * work.sweeps[sweep].rayCountC;
            if (rays > uint64_t{DISPATCH_GROUP_LIMIT} * SKY_GROUP_THREADS)
                return false;

            rayEnd[sweep] = static_cast<uint32_t>(rays);
        }

        // --- 作業を写す。シェーダーが毎回読むので既定のヒープへ(T-0374)---
        void* mapped = nullptr;
        const D3D12_RANGE noRead{.Begin = 0, .End = 0};
        if (FAILED(m_workUpload->Map(0, &noRead, &mapped)))
            return false;

        auto* bytes = static_cast<std::byte*>(mapped);
        std::memset(bytes, 0, mr::SKY_PORT_PARAMS_BYTES);
        std::memcpy(bytes, rayEnd.data(), sizeof(rayEnd));
        if (!work.sweeps.empty())
            std::memcpy(bytes + mr::SKY_PORT_HEADER_BYTES, work.sweeps.data(),
                        work.sweeps.size() * sizeof(mr::SunParams));

        std::memcpy(bytes + mr::SKY_PORT_PARAMS_BYTES, m_lights.data(),
                    m_lights.size() * sizeof(reaction::LtSpeciesLight));
        m_workUpload->Unmap(0, nullptr);

        list->CopyBufferRegion(m_work.Get(), 0, m_workUpload.Get(), 0, WorkBytes(m_lights.size()));
        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(m_work.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);

        m_rayCount = static_cast<uint32_t>(rays);
        m_sweepCount = static_cast<uint32_t>(work.sweeps.size());
        m_sideBits = work.bandSide[0] | (work.bandSide[1] << 1);

        return true;
    }

    void GpuSkyPort::RecordSweep(ID3D12GraphicsCommandList10* list, GpuMultires& nest,
                                 D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        // 0 にして掃く(RecordExternalDispatch が後ろに UAV のバリアを入れる)
        const uint32_t allWords = m_sideWords * mr::SKY_PORT_SIDES;
        nest.SetExternalViews(m_work->GetGPUVirtualAddress(), m_output->GetGPUVirtualAddress());
        nest.RecordExternalDispatch(list, debugRing, m_clearPipeline.Get(), GroupsFor(allWords), {allWords, 0, 0, 0});
        if (m_rayCount != 0)
            nest.RecordExternalDispatch(list, debugRing, m_sweepPipeline.Get(), GroupsFor(m_rayCount),
                                        {m_rayCount, m_sweepCount, m_sideWords, m_sideBits});

        nest.SetExternalViews(0, 0);
    }

    void GpuSkyPort::RecordReadback(ID3D12GraphicsCommandList10* list) {
        gpu::RecordCopyToReadback(list, m_output.Get(), m_readback.Get());
    }

    SkyPortResult GpuSkyPort::Read() const {
        std::vector<uint64_t> words(size_t{m_sideWords} * mr::SKY_PORT_SIDES);
        SkyPortResult result;
        result.sweeps = m_sweepCount;
        if (!gpu::ReadBuffer(m_readback.Get(), std::as_writable_bytes(std::span(words))))
            return result;

        const auto side = static_cast<std::ptrdiff_t>(m_sideWords);
        result.words[0].assign(words.begin(), words.begin() + side);
        result.words[1].assign(words.begin() + side, words.end());

        return result;
    }

}  // namespace bicameral::sim
