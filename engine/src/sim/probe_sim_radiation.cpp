// probe_sim_radiation.cpp — 仮の世界(ProbeSim)の GPU の刻みに熱の放射の段を載せる(T-1154・D-646・D-658)。
//
// CPU リファレンスは sim/probe_reference_radiation.cpp の ProbeReference::StepCascadeRadiation(ProbeReference::Advance の (1d))。
// GPU は適用の単位の中(コマンド → 物理の熱 → 表の作り直し → 気体 → 液体 → **放射** → 起こす刻みの来たブロックを一覧へ)で、
// shaders/sim/probe_radiation.hlsl の 2 段と、その間の距離ごとの段の 1 刻み(sim/gpu_cascade_light の自分のルート署名)を投げる
// (記録は sim/probe_sim.cpp の RecordRadiation)。距離ごとの段は世界の箱 64³(低い角 0)で、放つ面の組などの形は ProbeCascadeOptions。
// 作る時に CPU が決めるのは、段の形・置き場の大きさ・物質の光の値(表ごと)だけ。持ち越しは空から(刻み 0 の世界)。
// 保存点・写しの早送りは段の状態のバッファの頭(持ち越しの表)を写し、状態のハッシュは RadiationHash が持ち越しの寄与を足す(D-619)。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#include <array>
#include <cstring>
#include <format>

#include "core/log.h"
#include "gpu/resources.h"
#include "sim/probe_sim.h"

namespace bicameral::sim {

    namespace {

        // shaders/CMakeLists.txt で入口ごとに作る .cso(m_radiationPipelines の並び)
        constexpr std::array<const char*, 3> RADIATION_SHADER_NAMES = {
            "sim/probe_radiation_grid.cso", "sim/probe_radiation_apply.cso", "sim/probe_radiation_hash.cso"};

        // 置き場の大きさ(溢れたら RadiationHash の assert)。放つ面は 1 刻みに組 1 つ分(D-658 の 15 組)、持ち越しは照り返しと組を待つ光
        constexpr GpuCascadeCapacity RADIATION_CAPACITY{.faces = 1u << 17, .bags = 1u << 20, .carry = 1u << 19};

    }  // namespace

    // 段を作る(放射を切った時・表が光の値を書いていない時は何もしない)
    std::expected<void, std::string> ProbeSim::CreateRadiation(ID3D12Device5* device, const BakedReactionTable& table) {
        if (!m_options.cascadeRadiation.enabled || table.speciesLight.empty() || !ProbeTableWritesLight(table))
            return {};

        if (m_options.cascadeRadiation.params.referenceMk != 0 || m_options.cascadeRadiation.params.negativePart)
            return std::unexpected(
                "熱の放射の段: 参照の温度の形は GPU の仮の世界でまだ動かない(CPU リファレンスだけ。T-1164)");

        CascadeGrid grid;
        grid.size = {PROBE_GRID_SIZE, PROBE_GRID_SIZE, PROBE_GRID_SIZE};
        grid.lights.resize(PROBE_CELL_COUNT);
        grid.temperatureMk.resize(PROBE_CELL_COUNT);
        auto cascade = GpuCascadeLight::CreateStandalone(device, grid, m_options.cascadeRadiation.params,
                                                         RADIATION_CAPACITY);
        if (!cascade)
            return std::unexpected("熱の放射の段を作れない: " + cascade.error());

        m_cascade.emplace(std::move(*cascade));
        if (!MakeRadiationLights(device, table))
            return std::unexpected("熱の放射の段の光の値のバッファを作れない");

        for (size_t pass = 0; pass < RADIATION_SHADER_NAMES.size(); ++pass) {
            const auto bytecode = gpu::LoadShader(RADIATION_SHADER_NAMES[pass]);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            m_radiationPipelines[pass] = gpu::CreateComputePipeline(device, m_rootSignature.Get(), *bytecode);
            if (!m_radiationPipelines[pass])
                return std::unexpected(std::format("パイプラインを作れない: {}", RADIATION_SHADER_NAMES[pass]));
        }

        Log(Channel::Sim, Level::Info, "熱の放射の段: 64³・組 {}・置き場 {} MiB",
            m_options.cascadeRadiation.params.emitSlots, m_cascade->VramBytes() >> 20);

        return {};
    }

    // 物質の光の値のバッファ(既定のヒープ)と写しの元を表から作る。写すのは RecordRadiationLights
    bool ProbeSim::MakeRadiationLights(ID3D12Device5* device, const BakedReactionTable& table) {
        const auto bytes = std::as_bytes(std::span(table.speciesLight));
        m_radiationLightsUpload = gpu::CreateFilledUpload(device, bytes);
        if (!m_radiationLightsUpload)
            return false;

        // 写しの元の大きさ(作る側が切り上げることがある)で作り、全部を写す
        m_radiationLights = gpu::CreateBuffer(device, m_radiationLightsUpload->GetDesc().Width,
                                              gpu::BufferKind::UnorderedAccess);
        if (!m_radiationLights)
            return false;

        m_radiationLights->SetName(L"ProbeSim.radiationLights");

        return true;
    }

    // 光の値を写し、UAV にする(作った直後の COMMON から)
    void ProbeSim::RecordRadiationLights(ID3D12GraphicsCommandList10* list) const {
        list->CopyBufferRegion(m_radiationLights.Get(), 0, m_radiationLightsUpload.Get(), 0,
                               m_radiationLightsUpload->GetDesc().Width);
        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(m_radiationLights.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
    }

    // 最初のフレーム: 段の作業を写して状態を 0 に(持ち越しは空)・光の値を写す
    void ProbeSim::RecordRadiationInitialization(ID3D12GraphicsCommandList10* list) {
        if (!m_cascade)
            return;

        if (!m_cascade->RecordUpload(list, m_debugRing.GpuAddress(), {}))
            Log(Channel::Sim, Level::Error, "熱の放射の段の作業を写せない");

        RecordRadiationLights(list);
    }

    // 表の差し替え(と表の違う保存点へ戻す時): 光の値を新しい表のものにする。流す・流さないは作った時の表のまま(ProbeSimOptions)
    bool ProbeSim::RecordRadiationTableSwap(ID3D12GraphicsCommandList10* list, FrameSlot& frame,
                                            const BakedReactionTable& table) {
        if (!m_cascade)
            return true;

        if (table.speciesLight.empty() || !ProbeTableWritesLight(table))
            Log(Channel::Sim, Level::Warning,
                "差し替えた表は光の値を書いていないが、熱の放射の段は作った時の表のまま流す(CPU リファレンスと違う)");

        ComPtr<ID3D12Device5> device;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
            return false;

        frame.keepAlive.push_back(m_radiationLights);
        frame.keepAlive.push_back(m_radiationLightsUpload);
        if (!MakeRadiationLights(device.Get(), table))
            return false;

        RecordRadiationLights(list);

        return true;
    }

}  // namespace bicameral::sim
