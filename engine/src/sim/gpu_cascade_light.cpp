// gpu_cascade_light.cpp — 距離ごとの段の GPU 側(gpu_cascade_light.h)。CPU リファレンスは sim/cascade_light.cpp の StepCascadeLight。
// 置き場の並びは shaders/common/cascade_light.hlsli の RcGpuParams。1 刻み = 6 + 3 × (段 − 1) 回の起動(間に UAV のバリア)。
// 世界の全部を掃くのは RcSources の読みと、RcCarryIn の持ち越しの表(鍵で引く密な表。溢れない。T-1164・ADR-0484)の読み。
#include "sim/gpu_cascade_light.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>

#include "gpu/resources.h"

namespace bicameral::sim {

    namespace rx = ::bicameral::reaction;

    namespace {

        constexpr uint32_t RC_GROUP_THREADS = 64;  // shaders/sim/cascade_light.hlsl の RC_GROUP_THREADS
        // 1 回の起動のグループの上限(どのカーネルも歩幅で回すので、GPU を埋めるだけあればよい。3070 Ti は 48 SM × 1536 スレッド)
        constexpr uint64_t MAX_GROUPS = 2048;
        constexpr uint64_t WORK_PARAMS_BYTES = 256;
        constexpr uint64_t LIGHT_BYTES = 24;
        constexpr uint64_t WORD_BYTES = 8;

        // 自分のルート署名(CreateStandalone): 木のルート署名(sim/gpu_multires.cpp の ROOT_LAYOUT)の頭と同じ並び。
        // シェーダーが読むのは u4・u5・定数 b0 の外の 4 語(並びの 12〜15 語目)・デバッグのリングだけ
        constexpr uint32_t OWN_ROOT_CONSTANTS = 26;  // multires_bindings.hlsli の RootConstants の語の数
        constexpr uint32_t OWN_EXTERNAL_FIRST = 12;  // g_external0 の位置
        constexpr gpu::RootSignatureLayout OWN_ROOT_LAYOUT{
            .uavCount = 6, .rootConstantCount = OWN_ROOT_CONSTANTS, .debugRing = true};

        static_assert(sizeof(rx::RcGpuParams) <= WORK_PARAMS_BYTES);
        static_assert(sizeof(rx::RcCellLight) == LIGHT_BYTES);

        // パイプラインの番号(shaders/CMakeLists.txt の cascade_light_*)
        enum Pipeline : uint8_t {
            InitState,
            InitBags,
            BeginTick,
            Sources,
            CarryIn,
            ClearFaces,
            Level0Prepare,
            Level0Walk,
            LevelWalk,
            ClearBags,
            ResetBags,
            PipelineCount,
        };
        static_assert(PipelineCount == GpuCascadeLight::STAGE_COUNT);

        constexpr std::array<const char*, PipelineCount> SHADER_NAMES = {
            "sim/cascade_light_init_state.cso",     "sim/cascade_light_init_bags.cso",
            "sim/cascade_light_begin_tick.cso",     "sim/cascade_light_sources.cso",
            "sim/cascade_light_carry_in.cso",       "sim/cascade_light_clear_faces.cso",
            "sim/cascade_light_level0_prepare.cso", "sim/cascade_light_level0_walk.cso",
            "sim/cascade_light_level_walk.cso",     "sim/cascade_light_clear_bags.cso",
            "sim/cascade_light_reset_bags.cso",
        };

        uint64_t AlignUp(uint64_t value) {
            return (value + WORD_BYTES - 1) & ~(WORD_BYTES - 1);
        }

        // 段 0 の放つ面ごとの升目の表(CPU の MakeFaceTexels と同じ順。face | i << 8 | j << 16)
        std::expected<std::vector<uint32_t>, std::string> MakeTexelTable(uint32_t texels, uint32_t& perFace) {
            std::vector<uint32_t> table;
            perFace = 0;
            for (uint32_t source = 0; source < rx::RC_FACES; ++source) {
                uint32_t count = 0;
                for (uint32_t face = 0; face < rx::RC_FACES; ++face) {
                    for (uint32_t j = 0; j < texels; ++j) {
                        for (uint32_t i = 0; i < texels; ++i) {
                            if (rx::RcFaceWeight(texels, face, i, j, source) == 0)
                                continue;

                            table.push_back(face | (i << 8) | (j << 16));
                            ++count;
                        }
                    }
                }

                if (source != 0 && count != perFace)
                    return std::unexpected("放つ面ごとの升目の数が揃わない");

                perFace = count;
            }

            return table;
        }

        // 状態のバッファの置き場(バイト)。読み戻すのは頭から持ち越しの表 2 つまで
        uint64_t LayOutState(rx::RcGpuParams& params, uint64_t cells, const GpuCascadeCapacity& capacity,
                             uint64_t& readbackBytes) {
            uint64_t at = uint64_t{rx::RC_GPU_HEADER_WORDS} * WORD_BYTES;
            auto place = [&at](uint32_t& field, uint64_t bytes) {
                field = static_cast<uint32_t>(at);
                at = AlignUp(at + bytes);
            };
            place(params.stateEmitted, cells * 16);
            place(params.stateAbsorbed, cells * 16);
            // 持ち越しの表 2 つ(鍵 = (セル × 6 + 面) × 2 + 帯 で引く 64bit の語。溢れない。T-1164・ADR-0484)
            place(params.stateCarry0, cells * rx::RC_FACES * rx::LT_BANDS * WORD_BYTES);
            place(params.stateCarry1, cells * rx::RC_FACES * rx::LT_BANDS * WORD_BYTES);
            readbackBytes = at;
            place(params.stateSources, cells * rx::RC_FACES * 16);
            place(params.stateFaces, uint64_t{capacity.faces} * 4);
            place(params.stateFaceFlags, cells * rx::RC_FACES * 4);
            place(params.stateFaceInfo, uint64_t{capacity.faces} * rx::RC_GPU_FACE_INFO_WORDS * WORD_BYTES);
            place(params.stateBags0, uint64_t{capacity.bags} * rx::RC_GPU_BAG_WORDS * WORD_BYTES);
            place(params.stateBags1, uint64_t{capacity.bags} * rx::RC_GPU_BAG_WORDS * WORD_BYTES);
            place(params.stateBagList0, uint64_t{capacity.bags} * 4);
            place(params.stateBagList1, uint64_t{capacity.bags} * 4);
            params.stateWords = static_cast<uint32_t>(at / WORD_BYTES);

            return at;
        }

        std::expected<void, std::string> CheckGpuShape(const CascadeGrid& grid, const CascadeParams& params,
                                                       const GpuCascadeCapacity& capacity) {
            if (auto checked = CheckCascadeParams(params); !checked)
                return checked;

            if (auto checked = CheckCascadeGrid(grid); !checked)
                return checked;

            if ((params.baseTexels << (params.levels - 1)) > rx::RC_GPU_TEXEL_LIMIT)
                return std::unexpected("GPU の升目の 1 辺(n0 × 2^(段 − 1))は 512 まで(袋の鍵に 9bit で詰める)");

            if (capacity.bags == 0 || (capacity.bags & (capacity.bags - 1)) != 0)
                return std::unexpected("袋の表の番地の数は 2 の冪");

            if (capacity.faces == 0 || capacity.carry == 0)
                return std::unexpected("置き場の大きさが 0");

            return {};
        }

    }  // namespace

    std::expected<GpuCascadeLight, std::string> GpuCascadeLight::Create(ID3D12Device5* device, const GpuMultires& nest,
                                                                        const CascadeGrid& grid,
                                                                        const CascadeParams& params,
                                                                        const GpuCascadeCapacity& capacity) {
        return CreateWith(device, nest.RootSignature(), grid, params, capacity);
    }

    std::expected<GpuCascadeLight, std::string> GpuCascadeLight::CreateStandalone(ID3D12Device5* device,
                                                                                  const CascadeGrid& grid,
                                                                                  const CascadeParams& params,
                                                                                  const GpuCascadeCapacity& capacity) {
        ComPtr<ID3D12RootSignature> root = gpu::CreateRootSignature(device, OWN_ROOT_LAYOUT);
        if (!root)
            return std::unexpected("距離ごとの段のルート署名を作れない");

        auto result = CreateWith(device, root.Get(), grid, params, capacity);
        if (result)
            result->m_ownRoot = std::move(root);

        return result;
    }

    std::expected<GpuCascadeLight, std::string> GpuCascadeLight::CreateWith(ID3D12Device5* device,
                                                                            ID3D12RootSignature* rootSignature,
                                                                            const CascadeGrid& grid,
                                                                            const CascadeParams& params,
                                                                            const GpuCascadeCapacity& capacity) {
        if (auto checked = CheckGpuShape(grid, params, capacity); !checked)
            return std::unexpected(checked.error());

        GpuCascadeLight result;
        result.m_cellCount = grid.CellCount();

        // --- パイプライン(木のルート署名を借りるか、自分のもの)---
        for (const char* name : SHADER_NAMES) {
            const auto shader = gpu::LoadShader(name);
            if (!shader)
                return std::unexpected(shader.error());

            result.m_pipelines.push_back(gpu::CreateComputePipeline(device, rootSignature, *shader));
            if (!result.m_pipelines.back())
                return std::unexpected(std::string("距離ごとの段のパイプラインを作れない: ") + name);
        }

        // --- 作業のバッファ: [RcGpuParams][升目の表][光の値][温度][初めの持ち越し] ---
        uint32_t perFace = 0;
        const auto texels = MakeTexelTable(params.baseTexels, perFace);
        if (!texels)
            return std::unexpected(texels.error());

        const uint64_t cells = grid.CellCount();
        rx::RcGpuParams& gpu = result.m_params;
        gpu.seed = params.seed;
        gpu.lowX = grid.low[0];
        gpu.lowY = grid.low[1];
        gpu.lowZ = grid.low[2];
        gpu.sizeX = grid.size[0];
        gpu.sizeY = grid.size[1];
        gpu.sizeZ = grid.size[2];
        gpu.baseTexels = params.baseTexels;
        gpu.levels = params.levels;
        gpu.intervalBlocks = params.intervalBlocks;
        gpu.raysPerBag = params.raysPerBag;
        gpu.emitSlots = params.emitSlots;
        gpu.thermalInSlots = params.thermalInSlots ? 1u : 0u;
        gpu.quietBelowMk = params.quietBelowMk;
        gpu.referenceWord = params.referenceMk | (params.negativePart ? rx::RC_REFERENCE_NEGATIVE : 0u);
        gpu.cellCount = static_cast<uint32_t>(cells);
        gpu.texelsPerFace = perFace;
        gpu.bagCapacity = capacity.bags;
        gpu.faceCapacity = capacity.faces;
        gpu.carryKeys = static_cast<uint32_t>(cells * rx::RC_FACES * rx::LT_BANDS);
        gpu.initialCarryCapacity = capacity.carry;
        gpu.workTexels = static_cast<uint32_t>(WORK_PARAMS_BYTES);
        gpu.workLights = static_cast<uint32_t>(AlignUp(gpu.workTexels + (texels->size() * 4)));
        gpu.workTemperatures = static_cast<uint32_t>(AlignUp(gpu.workLights + (cells * LIGHT_BYTES)));
        gpu.workCarry = static_cast<uint32_t>(AlignUp(gpu.workTemperatures + (cells * 4)));
        result.m_workBytes = gpu.workCarry + (uint64_t{capacity.carry} * 16);
        result.m_stateBytes = LayOutState(gpu, cells, capacity, result.m_readbackBytes);
        if (result.m_stateBytes >= (uint64_t{1} << 32) || result.m_workBytes >= (uint64_t{1} << 32))
            return std::unexpected("距離ごとの段の置き場が 4 GB を超える(バイトの位置が 32bit)");

        std::vector<std::byte>& image = result.m_workBytesImage;
        image.assign(gpu.workCarry, std::byte{0});
        std::memcpy(image.data() + gpu.workTexels, texels->data(), texels->size() * 4);
        std::memcpy(image.data() + gpu.workLights, grid.lights.data(), cells * LIGHT_BYTES);
        std::memcpy(image.data() + gpu.workTemperatures, grid.temperatureMk.data(), cells * 4);

        result.m_work = gpu::CreateBuffer(device, result.m_workBytes, gpu::BufferKind::UnorderedAccess);
        result.m_workUpload = gpu::CreateBuffer(device, result.m_workBytes, gpu::BufferKind::Upload);
        result.m_state = gpu::CreateBuffer(device, result.m_stateBytes, gpu::BufferKind::UnorderedAccess);
        result.m_readback = gpu::CreateBuffer(device, result.m_readbackBytes, gpu::BufferKind::Readback);
        if (!result.m_work || !result.m_workUpload || !result.m_state || !result.m_readback)
            return std::unexpected("距離ごとの段のバッファを作れない");

        return result;
    }

    void GpuCascadeLight::Dispatch(ID3D12GraphicsCommandList10* list, GpuMultires* nest,
                                   D3D12_GPU_VIRTUAL_ADDRESS debugRing, size_t pipeline, uint64_t items,
                                   std::array<uint32_t, 4> external) const {
        const uint64_t groups = std::clamp<uint64_t>((items + RC_GROUP_THREADS - 1) / RC_GROUP_THREADS, 1, MAX_GROUPS);
        external[2] = static_cast<uint32_t>(groups * RC_GROUP_THREADS);
        if (nest != nullptr) {
            nest->RecordExternalDispatch(list, debugRing, m_pipelines[pipeline].Get(), static_cast<uint32_t>(groups),
                                         external);
            return;
        }

        // --- 自分のルート署名(u0〜u3 は読まないので作業のバッファを仮に結ぶ)---
        std::array<uint32_t, OWN_ROOT_CONSTANTS> constants{};
        std::copy(external.begin(), external.end(), constants.begin() + OWN_EXTERNAL_FIRST);
        const D3D12_GPU_VIRTUAL_ADDRESS work = m_work->GetGPUVirtualAddress();
        list->SetComputeRootSignature(m_ownRoot.Get());
        list->SetPipelineState(m_pipelines[pipeline].Get());
        for (uint32_t view = 0; view < 4; ++view)
            list->SetComputeRootUnorderedAccessView(view, work);

        list->SetComputeRootUnorderedAccessView(4, work);
        list->SetComputeRootUnorderedAccessView(5, m_state->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(OWN_ROOT_LAYOUT.RootConstantIndex(), OWN_ROOT_CONSTANTS, constants.data(),
                                           0);
        list->SetComputeRootUnorderedAccessView(OWN_ROOT_LAYOUT.DebugRingIndex(), debugRing);
        list->Dispatch(static_cast<uint32_t>(groups), 1, 1);
        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
    }

    bool GpuCascadeLight::RecordUpload(ID3D12GraphicsCommandList10* list, GpuMultires& nest,
                                       D3D12_GPU_VIRTUAL_ADDRESS debugRing, const CascadeCarry& carry) {
        return RecordUploadWith(list, &nest, debugRing, carry);
    }

    bool GpuCascadeLight::RecordUpload(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                       const CascadeCarry& carry) {
        return m_ownRoot && RecordUploadWith(list, nullptr, debugRing, carry);
    }

    bool GpuCascadeLight::RecordUploadWith(ID3D12GraphicsCommandList10* list, GpuMultires* nest,
                                           D3D12_GPU_VIRTUAL_ADDRESS debugRing, const CascadeCarry& carry) {
        if (carry.entries.size() > m_params.initialCarryCapacity)
            return false;

        // --- 作業を写す(シェーダーが毎回読むので既定のヒープへ。T-0374)---
        m_params.initialCarryCount = static_cast<uint32_t>(carry.entries.size());
        void* mapped = nullptr;
        const D3D12_RANGE noRead{.Begin = 0, .End = 0};
        if (FAILED(m_workUpload->Map(0, &noRead, &mapped)))
            return false;

        auto* bytes = static_cast<std::byte*>(mapped);
        std::memcpy(bytes, m_workBytesImage.data(), m_workBytesImage.size());
        std::memcpy(bytes, &m_params, sizeof(m_params));
        for (size_t at = 0; at < carry.entries.size(); ++at) {
            const CascadeCarryEntry& entry = carry.entries[at];
            const uint64_t words[2] = {(uint64_t{entry.cell} * rx::RC_FACES * rx::LT_BANDS) +
                                           (uint64_t{entry.face} * rx::LT_BANDS) + entry.band,
                                       entry.amount};
            std::memcpy(bytes + m_params.workCarry + (at * 16), words, sizeof(words));
        }

        m_workUpload->Unmap(0, nullptr);
        list->CopyBufferRegion(m_work.Get(), 0, m_workUpload.Get(), 0, m_workBytes);
        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(m_work.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);

        // --- 状態を 0 に・袋の溜めを空に・初めの持ち越しを表 0 へ ---
        if (nest != nullptr)
            nest->SetExternalViews(m_work->GetGPUVirtualAddress(), m_state->GetGPUVirtualAddress());

        Dispatch(list, nest, debugRing, InitState, m_params.stateWords, {0, 0, 0, 0});
        Dispatch(list, nest, debugRing, InitBags, uint64_t{m_params.bagCapacity} * 2, {0, 0, 0, 0});
        if (nest != nullptr)
            nest->SetExternalViews(0, 0);

        m_parity = 0;

        return true;
    }

    void GpuCascadeLight::RecordTick(ID3D12GraphicsCommandList10* list, GpuMultires& nest,
                                     D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick) {
        RecordTickWith(list, &nest, debugRing, tick, m_parity);
    }

    void GpuCascadeLight::RecordTick(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                     uint64_t tick, gpu::StageStamps* stamps, uint8_t firstStage) {
        if (m_ownRoot)
            RecordTickWith(list, nullptr, debugRing, tick, static_cast<uint32_t>(tick & 1), stamps, firstStage);
    }

    std::string_view GpuCascadeLight::StageName(uint32_t stage) {
        constexpr std::array<std::string_view, PipelineCount> NAMES = {
            "InitState",     "InitBags",   "BeginTick", "Sources",   "CarryIn",  "ClearFaces",
            "Level0Prepare", "Level0Walk", "LevelWalk", "ClearBags", "ResetBags"};
        return stage < NAMES.size() ? NAMES[stage] : std::string_view("?");
    }

    void GpuCascadeLight::RecordTickWith(ID3D12GraphicsCommandList10* list, GpuMultires* nest,
                                         D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick, uint32_t in,
                                         gpu::StageStamps* stamps, uint8_t firstStage) {
        const auto tickLow = static_cast<uint32_t>(tick);  // 刻みは 2^32 未満(試験と測りの範囲)
        const uint64_t cells = m_cellCount;
        if (nest != nullptr)
            nest->SetExternalViews(m_work->GetGPUVirtualAddress(), m_state->GetGPUVirtualAddress());

        // 起動の後ろ(Dispatch がバリアを置いた後)に段の印(測りの時だけ。結果には入らない)
        const auto run = [&](Pipeline pipeline, uint64_t items, std::array<uint32_t, 4> external) {
            Dispatch(list, nest, debugRing, pipeline, items, external);
            if (stamps != nullptr)
                stamps->Mark(list, static_cast<uint8_t>(firstStage + pipeline));
        };

        // --- 段 0: 放つ面 ---
        run(BeginTick, rx::RC_GPU_HEADER_WORDS, {tickLow, in, 0, 0});
        run(Sources, cells, {tickLow, in, 0, 0});
        const uint32_t out = 1 - in;
        run(CarryIn, m_params.carryKeys, {tickLow, in, 0, 0});
        run(Level0Prepare, m_params.faceCapacity, {tickLow, in, 0, 0});
        run(Level0Walk, uint64_t{m_params.faceCapacity} * m_params.texelsPerFace, {tickLow, in, 0, out << 1});
        run(ClearFaces, m_params.faceCapacity, {tickLow, in, 0, 0});

        // --- 段 1 から: 袋の表を交互に ---
        for (uint32_t level = 1; level < m_params.levels; ++level) {
            const uint32_t table = (level - 1) & 1u;
            run(LevelWalk, uint64_t{m_params.bagCapacity} * m_params.raysPerBag,
                {tickLow, level, 0, table | (out << 1)});
            run(ClearBags, m_params.bagCapacity, {tickLow, level, 0, table});
            run(ResetBags, 1, {tickLow, level, 0, table});
        }

        // 照り返しと組を待つ光は、歩く間に表 out へ足してある(次の刻みが読む)
        if (nest != nullptr)
            nest->SetExternalViews(0, 0);

        m_parity = 1 - in;
    }

    void GpuCascadeLight::RecordReadback(ID3D12GraphicsCommandList10* list) {
        const D3D12_RESOURCE_BARRIER toCopy = gpu::Transition(m_state.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                              D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &toCopy);
        list->CopyBufferRegion(m_readback.Get(), 0, m_state.Get(), 0, m_readbackBytes);
    }

    std::expected<GpuCascadeTickResult, std::string> GpuCascadeLight::Read() const {
        std::vector<uint64_t> words(m_readbackBytes / WORD_BYTES);
        if (!gpu::ReadBuffer(m_readback.Get(), std::as_writable_bytes(std::span(words))))
            return std::unexpected("距離ごとの段の結果を読み戻せない");

        GpuCascadeTickResult read;
        CascadeTickResult& result = read.result;
        for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
            const uint64_t* ledger = words.data() + rx::RC_GPU_LEDGER + (band * rx::RC_GPU_LEDGER_KINDS);
            result.ledger[band] = {.emitted = ledger[0],
                                   .carriedIn = ledger[1],
                                   .absorbed = ledger[2],
                                   .reflected = ledger[3],
                                   .escaped = ledger[4],
                                   .held = words[rx::RC_GPU_HELD + band],
                                   .kept = words[rx::RC_GPU_KEPT + band]};
        }

        for (uint32_t level = 0; level < result.raysPerLevel.size(); ++level) {
            result.raysPerLevel[level] = words[rx::RC_GPU_RAYS + level];
            result.rays += result.raysPerLevel[level];
        }

        result.steps = words[rx::RC_GPU_STEPS];
        read.activeFaces = static_cast<uint32_t>(words[rx::RC_GPU_FACE_COUNT]);
        read.overflow = static_cast<uint32_t>(words[rx::RC_GPU_OVERFLOW]);
        read.bagPeak = static_cast<uint32_t>(words[rx::RC_GPU_BAG_PEAK]);

        // --- セルごと・持ち越し(セル・面・帯の順に並べ直す)---
        const size_t values = m_cellCount * rx::LT_BANDS;
        const auto emitted = words.begin() + (m_params.stateEmitted / WORD_BYTES);
        const auto absorbed = words.begin() + (m_params.stateAbsorbed / WORD_BYTES);
        result.emitted.assign(emitted, emitted + static_cast<std::ptrdiff_t>(values));
        result.absorbed.assign(absorbed, absorbed + static_cast<std::ptrdiff_t>(values));

        // 持ち越しの表は鍵の順(= セル・面・帯の順)に並ぶ密な表。0 でない鍵だけを拾う
        const uint32_t table = m_parity == 0 ? m_params.stateCarry0 : m_params.stateCarry1;
        for (uint64_t key = 0; key < m_params.carryKeys; ++key) {
            const uint64_t amount = words[(table / WORD_BYTES) + key];
            if (amount == 0)
                continue;

            result.carry.entries.push_back({.cell = static_cast<uint32_t>(key / (rx::RC_FACES * rx::LT_BANDS)),
                                            .face = static_cast<uint8_t>((key / rx::LT_BANDS) % rx::RC_FACES),
                                            .band = static_cast<uint8_t>(key % rx::LT_BANDS),
                                            .amount = amount});
        }

        return read;
    }

}  // namespace bicameral::sim
