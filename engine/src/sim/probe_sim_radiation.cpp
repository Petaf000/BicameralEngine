// probe_sim_radiation.cpp — 仮の世界(ProbeSim)の GPU の刻みに熱の放射の段を載せる(T-1154・D-646・D-658・D-670・D-671)。
//
// CPU リファレンスは sim/probe_reference_radiation.cpp の ProbeReference::StepCascadeRadiation(ProbeReference::Advance の (1d))。
// GPU は適用の単位の中(コマンド → 物理の熱 → 表の作り直し → 気体 → 液体 → **放射** → 起こす刻みの来たブロックを一覧へ)で、
// shaders/sim/probe_radiation.hlsl の 2 段と、その間の距離ごとの段の 1 刻み(sim/gpu_cascade_light の自分のルート署名)を投げる
// (記録は sim/probe_sim.cpp の RecordRadiation)。距離ごとの段は世界の箱 64³(低い角 0)で、放つ面の組などの形は ProbeCascadeOptions。
// 世界の外が環境の温度の物の時(参照の温度が 0 でない。ProbeCascadeReferenceMk)は、段を正の側と負の側の 2 つ作り(作業と状態の並びは同じ)、
// 当てる段が 正(吸った − 放った)− 負(吸った − 放った)を足す(CPU の StepCascadeLightRelative と同じ)。
// 作る時に CPU が決めるのは、段の形・参照の温度・置き場の大きさ・物質の光の値(表ごと)だけ。持ち越しは空から(刻み 0 の世界)。
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

        // 置き場の大きさ(溢れたら RadiationHash の assert と ProbeFrameReadback::radiationOverflow)。放つ面は 1 刻みに組 1 つ分
        // (D-658 の 15 組)。持ち越し(照り返しと組を待つ光)は鍵で引く密な表なので溢れない(T-1164・ADR-0484)。
        // carry は初めの持ち越しを写す数の上限で、仮の世界は空から始めるので小さくてよい
        constexpr GpuCascadeCapacity RADIATION_CAPACITY{.faces = 1u << 17, .bags = 1u << 20, .carry = 1u << 10};

    }  // namespace

    // 段を作る(放射を切った時・物質の無い表では何もしない。D-670: 光の値を書かない表でも作る)
    std::expected<void, std::string> ProbeSim::CreateRadiation(ID3D12Device5* device, const BakedReactionTable& table) {
        if (!m_options.cascadeRadiation.enabled || table.speciesLight.empty())
            return {};

        if (m_options.cascadeRadiation.params.negativePart)
            return std::unexpected(
                "熱の放射の段: negativePart は側の印(StepCascadeLightRelative が付ける)なので渡せない");

        CascadeParams params = m_options.cascadeRadiation.params;
        params.referenceMk = ProbeCascadeReferenceMk(m_options.cascadeRadiation, table);
        CascadeGrid grid;
        grid.size = {PROBE_GRID_SIZE, PROBE_GRID_SIZE, PROBE_GRID_SIZE};
        grid.lights.resize(PROBE_CELL_COUNT);
        grid.temperatureMk.resize(PROBE_CELL_COUNT);
        auto cascade = GpuCascadeLight::CreateStandalone(device, grid, params, RADIATION_CAPACITY);
        if (!cascade)
            return std::unexpected("熱の放射の段を作れない: " + cascade.error());

        m_cascade.emplace(std::move(*cascade));
        m_cascade->SetIndirectLevels(m_options.radiationIndirectLevels);
        if (params.referenceMk != 0) {
            CascadeParams negativeParams = params;
            negativeParams.negativePart = true;
            auto negative = GpuCascadeLight::CreateStandalone(device, grid, negativeParams, RADIATION_CAPACITY);
            if (!negative)
                return std::unexpected("熱の放射の段の負の側を作れない: " + negative.error());

            // 当てる段は正の側の作業の番地で負の側の状態を読む(同じ大きさの世界と置き場なので同じ並び)
            const reaction::RcGpuParams& positiveLayout = m_cascade->Params();
            const reaction::RcGpuParams& negativeLayout = negative->Params();
            if (positiveLayout.stateEmitted != negativeLayout.stateEmitted ||
                positiveLayout.stateAbsorbed != negativeLayout.stateAbsorbed ||
                positiveLayout.workLights != negativeLayout.workLights ||
                positiveLayout.workTemperatures != negativeLayout.workTemperatures)
                return std::unexpected("熱の放射の段: 正の側と負の側の作業・状態の並びが違う");

            m_cascadeNegative.emplace(std::move(*negative));
            m_cascadeNegative->SetIndirectLevels(m_options.radiationIndirectLevels);
        }
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

        // 段ごとの GPU 時間の印(測りの時だけ。1 刻み 42 個ほど × フレームの刻み)
        if (m_options.radiationStageTiming) {
            constexpr uint32_t STAMP_CAPACITY = 1u << 13;
            auto stamps = gpu::StageStamps::Create(device, FRAME_SLOT_COUNT, STAMP_CAPACITY,
                                                   static_cast<uint32_t>(ProbeRadiationStage::Count));
            if (!stamps)
                return std::unexpected(stamps.error());

            m_radiationStamps.emplace(std::move(*stamps));
        }

        const uint64_t vramBytes = m_cascade->VramBytes() + (m_cascadeNegative ? m_cascadeNegative->VramBytes() : 0);
        Log(Channel::Sim, Level::Info, "熱の放射の段: 64³・組 {}・外 {}・置き場 {} MiB", params.emitSlots,
            params.referenceMk == 0 ? std::string("0 K の空") : std::format("参照の温度 {} mK", params.referenceMk),
            vramBytes >> 20);

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

        if (m_cascadeNegative && !m_cascadeNegative->RecordUpload(list, m_debugRing.GpuAddress(), {}))
            Log(Channel::Sim, Level::Error, "熱の放射の段の負の側の作業を写せない");

        RecordRadiationLights(list);
    }

    // 表の差し替え(と表の違う保存点へ戻す時): 光の値を新しい表のものにする。参照の温度は作った時の表のまま(CPU リファレンスと同じ)
    bool ProbeSim::RecordRadiationTableSwap(ID3D12GraphicsCommandList10* list, FrameSlot& frame,
                                            const BakedReactionTable& table) {
        if (!m_cascade)
            return true;

        if (table.speciesLight.empty())
            Log(Channel::Sim, Level::Warning,
                "差し替えた表に物質が無いが、熱の放射の段は作った時の表のまま流す(CPU リファレンスと違う)");

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
