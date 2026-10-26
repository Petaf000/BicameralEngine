// probe_sim.cpp — 仮の刻みを単位の列として GPU で走らせる(フレームの枠ごとのリストに毎フレーム記録)と、その CPU リファレンス。
// 使い方とデータの流れは probe_sim.h、規則は shaders/common/probe_sim.hlsli。
#include "sim/probe_sim.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "common/probe_region_hash.hlsli"
#include "core/aliases.h"
#include "core/log.h"
#include "gpu/com_ptr.h"
#include "gpu/resources.h"
#include "sim/radiation_lines.h"
#include "sim/radiation_reference.h"
#include "sim/world_physics.h"

namespace bicameral::sim {
    namespace {

        // ルート署名(shaders/sim/probe_bindings.hlsli と同じ順。伝導の Work Graph もこれをグローバルのルート署名に使う):
        //   u0 セル・u1 イベントのリング・u2/u3/u5 抽出・u4 重さの捨て場・u6 ハッシュの表・u7 コマンドキュー・
        //   u8 刻みのイベントの一時置き場・u9/u10 活性の一覧・u11 予定の印・u12 熱のキャッシュ・u13 物理の物(T-0098。物理なしなら仮の置き場)
        //   ・u14 物理の熱の入口(T-0561。物理なしなら仮の置き場)・u15〜u18 気体の区画(T-0592。sim/probe_sim_gas.cpp。無ければ仮の置き場)
        //   ・u19 液体の区画(T-0613。sim/probe_sim_liquid.cpp。無ければ仮の置き場)。語の数: UAV 20 × 2 + 定数 3 + 3 × 2 + SRV 6 × 2 = 61/64
        //   → b0 単位の定数 → デバッグのリング
        //   → Work Graphs のカウンタ(u1 space1。T-0008)→ 連鎖のトレース(u2 space1。T-0087)→ t0 フレームの入力・t1〜t4 反応の表
        //   ・t5 物質の付け替えの表(T-0223)
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{.uavCount = 20,
                                                       .rootConstantCount = PROBE_ROOT_CONSTANT_COUNT,
                                                       .debugRing = true,
                                                       .graphStats = true,
                                                       .graphTrace = true,
                                                       .srvCount = 6};

        constexpr uint32_t UAV_CELLS = 0;
        constexpr uint32_t UAV_EVENTS = 1;
        constexpr uint32_t UAV_EXTRACTION0 = 2;
        constexpr uint32_t UAV_EXTRACTION1 = 3;
        constexpr uint32_t UAV_BUSY_SINK = 4;
        constexpr uint32_t UAV_EXTRACTION2 = 5;
        constexpr uint32_t UAV_HASHES = 6;
        constexpr uint32_t UAV_COMMAND_QUEUE = 7;
        constexpr uint32_t UAV_TICK_EVENTS = 8;
        constexpr uint32_t UAV_ACTIVE_LIST0 = 9;
        constexpr uint32_t UAV_ACTIVE_LIST1 = 10;
        constexpr uint32_t UAV_BLOCK_SCHEDULE = 11;
        constexpr uint32_t UAV_THERMAL = 12;
        constexpr uint32_t UAV_BODIES = 13;
        constexpr uint32_t UAV_PHYSICS_HEAT = 14;
        constexpr uint32_t UAV_GAS_STATE = 15;       // 区画のセルごとの気体の欄
        constexpr uint32_t UAV_GAS_WORK = 16;        // 気体の作業場
        constexpr uint32_t UAV_GAS_CELLS = 17;       // 気体の箱のセル(GpuGas の u0)
        constexpr uint32_t UAV_GAS_BOX_LEDGER = 18;  // 気体の箱の帳簿(GpuGas の u5)
        constexpr uint32_t UAV_LIQUID_WORK = 19;     // 液体の作業場(制御・帳簿・状態)
        constexpr uint32_t SRV_INPUT = 0;
        constexpr uint32_t SRV_REACTION_FIRST = 1;  // t1 物質・t2 規則・t3 索引・t4 速度
        constexpr uint32_t SRV_SPECIES_REMAP = 5;   // t5 付け替えの表(T-0223)

        constexpr uint32_t TIMESTAMPS_PER_SLOT = ProbeSim::MAX_UNITS_PER_FRAME + 2;  // 始め・単位ごと・終わり
        constexpr uint64_t CELL_BYTES = uint64_t{PROBE_CELL_COUNT} * sizeof(reaction::RxCell);             // 1 世代
        constexpr uint64_t THERMAL_BYTES = uint64_t{PROBE_CELL_COUNT} * sizeof(reaction::HcThermalCache);  // 1 世代
        constexpr uint32_t SLICE_BYTES = PROBE_SLICE_CELL_COUNT * 4;
        constexpr uint32_t BUSY_GROUPS = PROBE_GRID_SIZE / PROBE_GROUP_SIZE;
        constexpr uint32_t LINEAR_CELL_GROUPS = PROBE_CELL_COUNT / PROBE_LINEAR_GROUP_SIZE;
        constexpr uint32_t BLOCK_GROUPS = (PROBE_BLOCK_COUNT + PROBE_LINEAR_GROUP_SIZE - 1) / PROBE_LINEAR_GROUP_SIZE;
        constexpr uint32_t EXTRACTION_BYTES = PROBE_EXTRACTION_WORDS * 4;

        // 活性の一覧の見出しは D3D12_NODE_GPU_INPUT そのもの(DispatchGraph が GPU のメモリから読む。probe_sim.hlsli)
        static_assert(offsetof(D3D12_NODE_GPU_INPUT, EntrypointIndex) == size_t{PROBE_ACTIVE_LIST_ENTRYPOINT} * 4);
        static_assert(offsetof(D3D12_NODE_GPU_INPUT, NumRecords) == size_t{PROBE_ACTIVE_LIST_COUNT} * 4);
        static_assert(offsetof(D3D12_NODE_GPU_INPUT, Records) == size_t{PROBE_ACTIVE_LIST_ADDRESS} * 4);

        static_assert(offsetof(D3D12_NODE_GPU_INPUT, Records) +
                          offsetof(D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE, StrideInBytes) ==
                      size_t{PROBE_ACTIVE_LIST_STRIDE} * 4);

        static_assert(sizeof(D3D12_NODE_GPU_INPUT) <= PROBE_ACTIVE_LIST_HEADER_BYTES);
        constexpr uint32_t MAX_LOGGED_DEBUG_MESSAGES = 8;

        // 放射の段の状態の頭(側 1 つ)と、両側を並べた読み戻しの大きさ(T-1164)
        constexpr uint64_t RADIATION_HEADER_BYTES = uint64_t{reaction::RC_GPU_HEADER_WORDS} * 8;
        constexpr uint64_t RADIATION_HEADER_READBACK_BYTES = 2 * RADIATION_HEADER_BYTES;

        // 伝導のグラフのカウンタの番号と上限(probe_sim.hlsli の PROBE_STATS_* と同じ順。T-0008)。
        // WakeBlocks の出力は構造で 7 まで・ConductBlock は出力しない(一覧へは UAV で足す)ので、出力の「近い」は見ない。
        // 活性の一覧は全部のブロックが活性でも溢れない大きさなので、満杯のときだけ知らせる
        gpu::GraphStatsLayout MakeConductStatsLayout() {
            gpu::GraphStatsLayout layout{.name = "伝導(ProbeConduct)"};
            layout.nodes.resize(2);
            layout.nodes[PROBE_STATS_NODE_WAKE] = {.name = "WakeBlocks", .maxOutputRecords = PROBE_WAKE_MAX_RECORDS};
            layout.nodes[PROBE_STATS_NODE_CONDUCT] = {.name = "ConductBlock"};
            layout.gauges.resize(2);
            layout.gauges[PROBE_STATS_GAUGE_ACTIVE_LIST] = {
                .name = "活性の一覧", .capacity = PROBE_ACTIVE_LIST_CAPACITY, .warnPercent = 100};
            layout.gauges[PROBE_STATS_GAUGE_COMMAND_QUEUE] = {.name = "コマンドキュー",
                                                              .capacity = PROBE_COMMAND_QUEUE_CAPACITY};

            return layout;
        }

        template <typename T>
        void WriteAt(std::byte* base, uint32_t offset, const T& value) {
            std::memcpy(base + offset, &value, sizeof(T));
        }

        // アップロードのバッファを作って中身を書く(最初のフレームで既定のヒープへ写す元)。
        // 空の列(ブランクの世界の表の規則など。T-0371)は 0 の要素 1 つ分にする(大きさ 0 のバッファは作れない。数はルート定数で渡すので
        // 読まれない)
        template <typename T>
        ComPtr<ID3D12Resource> CreateFilledUpload(ID3D12Device* device, const std::vector<T>& data,
                                                  const wchar_t* name) {
            const auto bytes = std::as_bytes(std::span(data));
            const size_t bufferBytes = std::max(bytes.size(), sizeof(T));
            ComPtr<ID3D12Resource> buffer = gpu::CreateBuffer(device, bufferBytes, gpu::BufferKind::Upload);
            void* mapped = nullptr;
            const D3D12_RANGE noRead{};
            if (!buffer || FAILED(buffer->Map(0, &noRead, &mapped)))
                return nullptr;

            std::memset(mapped, 0, bufferBytes);
            if (!bytes.empty())
                std::memcpy(mapped, bytes.data(), bytes.size());
            buffer->Unmap(0, nullptr);
            buffer->SetName(name);

            return buffer;
        }

        // イベントのリングの見出し(probe_sim.hlsli の PROBE_EVENT_HEADER_*)
        struct EventRingHeader {
            uint32_t requested = 0;    // リングに書こうとした数
            uint32_t tickDropped = 0;  // 刻みの一時置き場で落とした数
        };

        ComPtr<ID3D12PipelineState> LoadComputePipeline(ID3D12Device* device, ID3D12RootSignature* rootSignature,
                                                        std::string_view shaderPath) {
            const auto bytecode = gpu::LoadShader(shaderPath);
            if (!bytecode) {
                Log(Channel::Sim, Level::Error, "{}", bytecode.error());
                return nullptr;
            }

            return gpu::CreateComputePipeline(device, rootSignature, *bytecode);
        }

        // 単位の定数(b0): 刻みの下位・上位・引数
        void SetUnitConstants(ID3D12GraphicsCommandList10* list, uint64_t tick, uint32_t argument) {
            const std::array<uint32_t, PROBE_ROOT_CONSTANT_COUNT> constants = {
                static_cast<uint32_t>(tick), static_cast<uint32_t>(tick >> 32), argument};
            list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), PROBE_ROOT_CONSTANT_COUNT,
                                               constants.data(), 0);
        }

    }  // namespace

    ProbeCommand MakePokeCommand(uint64_t targetTick, uint32_t sequence, uint32_t x, uint32_t y, uint32_t z) {
        ProbeCommand command{.targetTick = targetTick,
                             .sequence = sequence,
                             .type = static_cast<uint16_t>(PROBE_COMMAND_TYPE_POKE),
                             .size = 12};

        command.payload[0] = x;
        command.payload[1] = y;
        command.payload[2] = z;

        return command;
    }

    ProbeCommand MakePushCommand(uint64_t targetTick, uint32_t sequence, const std::array<int64_t, 3>& origin,
                                 const std::array<int32_t, 3>& direction, uint32_t impulseMillinewtonSeconds) {
        ProbeCommand command{.targetTick = targetTick,
                             .sequence = sequence,
                             .type = static_cast<uint16_t>(PROBE_COMMAND_TYPE_PUSH),
                             .size = 40};

        for (size_t axis = 0; axis < 3; ++axis) {
            const auto value = static_cast<uint64_t>(origin[axis]);
            command.payload[axis * 2] = static_cast<uint32_t>(value);
            command.payload[(axis * 2) + 1] = static_cast<uint32_t>(value >> 32);
            command.payload[6 + axis] = static_cast<uint32_t>(direction[axis]);
        }

        command.payload[9] = impulseMillinewtonSeconds;

        return command;
    }

    ProbeCommand MakePlaceCommand(uint64_t targetTick, uint32_t sequence, const ProbePlaceShape& shape,
                                  std::span<const SpeciesAmount> contents) {
        ProbeCommand command{.targetTick = targetTick,
                             .sequence = sequence,
                             .type = static_cast<uint16_t>(PROBE_COMMAND_TYPE_PLACE),
                             .size = PROBE_PLACE_PAYLOAD_WORDS * 4};

        // 範囲の外の値は、当てないコマンドになる値にして書く(ProbeDecodePlace が飛ばす)
        const uint32_t mode = shape.replace ? PROBE_PLACE_MODE_REPLACE : PROBE_PLACE_MODE_ADD;
        const auto count = static_cast<uint32_t>(contents.size());
        const bool inside = shape.x < PROBE_GRID_SIZE && shape.y < PROBE_GRID_SIZE && shape.z < PROBE_GRID_SIZE;
        // 形の大きさ 2・3 は 4 ビット(上限を超える値は 0xF にして当てないコマンドに)。格子の外は x = 0xFF
        const uint32_t sizes = (std::min(shape.size2, 0xFu) << 24) | (std::min(shape.size3, 0xFu) << 28);
        command.payload[PROBE_PLACE_WORD_CENTER] = inside ? shape.x | (shape.y << 8) | (shape.z << 16) | sizes : 0xFFu;
        command.payload[PROBE_PLACE_WORD_SHAPE] = std::min(shape.radius, 0xFFu) | (mode << 8) |
                                                  (std::min(count, 0xFFu) << 16) | (std::min(shape.kind, 0xFFu) << 24);
        command.payload[PROBE_PLACE_WORD_TEMPERATURE] = shape.temperatureMilliKelvin;

        const uint32_t written = std::min(count, PROBE_PLACE_MAX_SPECIES);
        for (uint32_t i = 0; i < written; ++i) {
            const uint32_t word = PROBE_PLACE_WORD_ENTRIES + (PROBE_PLACE_WORDS_PER_ENTRY * i);
            command.payload[word] = contents[i].species;
            command.payload[word + 1] = static_cast<uint32_t>(contents[i].amount);
            command.payload[word + 2] = static_cast<uint32_t>(contents[i].amount >> 32);
        }

        return command;
    }

    ProbePlace DecodePlaceCommand(const ProbeCommand& command, uint32_t speciesCount) {
        ProbePlacePayload payload{};
        rng::copy(command.payload, std::begin(payload.words));

        return ProbeDecodePlace(payload, speciesCount);
    }

    std::vector<uint32_t> ProbePlaceBlocks(const ProbePlace& place) {
        const uint32_t beginX = ProbePlaceBegin(place.x, place.extentX) / PROBE_BLOCK_SIZE;
        const uint32_t beginY = ProbePlaceBegin(place.y, place.extentY) / PROBE_BLOCK_SIZE;
        const uint32_t beginZ = ProbePlaceBegin(place.z, place.extentZ) / PROBE_BLOCK_SIZE;
        const uint32_t endX = (ProbePlaceEnd(place.x, place.extentX) - 1) / PROBE_BLOCK_SIZE;
        const uint32_t endY = (ProbePlaceEnd(place.y, place.extentY) - 1) / PROBE_BLOCK_SIZE;
        const uint32_t endZ = (ProbePlaceEnd(place.z, place.extentZ) - 1) / PROBE_BLOCK_SIZE;

        std::vector<uint32_t> blocks;
        for (uint32_t bz = beginZ; bz <= endZ; ++bz) {
            for (uint32_t by = beginY; by <= endY; ++by) {
                for (uint32_t bx = beginX; bx <= endX; ++bx)
                    blocks.push_back(ProbeBlockIndex(bx, by, bz));
            }
        }

        return blocks;
    }

    ProbeCommand RemapPlaceCommand(const ProbeCommand& command, const SpeciesRemap& remap) {
        if (remap.identity)
            return command;

        if (command.type != PROBE_COMMAND_TYPE_PLACE)
            return command;

        // --- 残る物質の材料だけ、新しい ID で詰め直す(当てられない数のコマンドは当てられないまま)---
        const uint32_t shape = command.payload[PROBE_PLACE_WORD_SHAPE];
        const uint32_t count = (shape >> 16) & 0xFFu;
        if (count > PROBE_PLACE_MAX_SPECIES)
            return command;

        ProbeCommand remapped = command;
        uint32_t kept = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t from = PROBE_PLACE_WORD_ENTRIES + (PROBE_PLACE_WORDS_PER_ENTRY * i);
            const uint32_t oldId = command.payload[from];
            const uint32_t newId = oldId < remap.newIds.size() ? remap.newIds[oldId] : 0;
            if (newId == 0)
                continue;

            const uint32_t to = PROBE_PLACE_WORD_ENTRIES + (PROBE_PLACE_WORDS_PER_ENTRY * kept);
            remapped.payload[to] = newId;
            remapped.payload[to + 1] = command.payload[from + 1];
            remapped.payload[to + 2] = command.payload[from + 2];
            kept += 1;
        }

        for (uint32_t word = PROBE_PLACE_WORD_ENTRIES + (PROBE_PLACE_WORDS_PER_ENTRY * kept);
             word < PROBE_PLACE_PAYLOAD_WORDS; ++word)
            remapped.payload[word] = 0;

        remapped.payload[PROBE_PLACE_WORD_SHAPE] = (shape & 0xFF00FFFFu) | (kept << 16);

        return remapped;
    }

    ProbeCommand MakeTableCommand(uint64_t targetTick, uint32_t sequence, uint64_t version) {
        ProbeCommand command{.targetTick = targetTick,
                             .sequence = sequence,
                             .type = static_cast<uint16_t>(PROBE_COMMAND_TYPE_TABLE),
                             .size = 8};

        command.payload[0] = static_cast<uint32_t>(version);
        command.payload[1] = static_cast<uint32_t>(version >> 32);

        return command;
    }

    // --- 作る ---

    std::expected<ProbeSim, std::string> ProbeSim::Create(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType,
                                                          const BakedReactionTable& table,
                                                          const ProbeSimOptions& options) {
        if (options.busyPieces == 0 || options.busyPieces > PROBE_MAX_BUSY_PIECES ||
            options.busyIterations > PROBE_BUSY_ITERATIONS_LIMIT) {
            return std::unexpected(std::format("重さの試験の値が範囲外: 繰り返し {} 分ける数 {}",
                                               options.busyIterations, options.busyPieces));
        }

        auto events = gpu::ReadbackRing::Create(device, PROBE_EVENT_BYTES, PROBE_EVENT_HEADER_BYTES, FRAME_SLOT_COUNT,
                                                L"ProbeSim.events");
        if (!events)
            return std::unexpected(events.error());

        auto debugRing = gpu::DebugRing::Create(device, FRAME_SLOT_COUNT);
        if (!debugRing)
            return std::unexpected(debugRing.error());

        auto graphStats = gpu::WorkGraphStats::Create(device, MakeConductStatsLayout(), FRAME_SLOT_COUNT);
        if (!graphStats)
            return std::unexpected(graphStats.error());

        auto graphTrace = gpu::GraphTrace::Create(device, options.trace, FRAME_SLOT_COUNT, options.traceCapacity);
        if (!graphTrace)
            return std::unexpected(graphTrace.error());

        ProbeSim sim(options, std::move(*events), std::move(*debugRing), std::move(*graphStats),
                     std::move(*graphTrace));
        if (!sim.CreatePipelines(device))
            return std::unexpected("仮の刻みのパイプラインを作れない");

        if (!sim.CreateBuffers(device))
            return std::unexpected("仮の刻みのバッファを作れない");

        // 初めの世界は作る時だけ使う(呼んだ側が持つ)
        sim.m_options.initialWorld = {};
        if (!sim.CreateWorld(device, table, options.initialWorld))
            return std::unexpected("仮の世界(反応の表・初めのセル)を作れない");

        if (!sim.CreateFrameSlots(device, listType))
            return std::unexpected("仮の刻みのフレームの枠を作れない");

        if (auto physics = sim.CreatePhysics(device); !physics)
            return std::unexpected(physics.error());

        if (auto gas = sim.CreateGas(device, table); !gas)
            return std::unexpected(gas.error());

        if (auto radiation = sim.CreateRadiation(device, table); !radiation)
            return std::unexpected(radiation.error());

        if (auto liquid = sim.CreateLiquid(device, table); !liquid)
            return std::unexpected(liquid.error());

        return sim;
    }

    bool ProbeSim::CreatePipelines(ID3D12Device5* device) {
        m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!m_rootSignature)
            return false;

        ID3D12RootSignature* root = m_rootSignature.Get();
        m_enqueuePipeline = LoadComputePipeline(device, root, "sim/probe_tick_enqueue.cso");
        m_applyPipeline = LoadComputePipeline(device, root, "sim/probe_tick_apply.cso");
        m_wakeDuePipeline = LoadComputePipeline(device, root, "sim/probe_tick_wake_due.cso");
        m_addPhysicsHeatPipeline = LoadComputePipeline(device, root, "sim/probe_tick_add_physics_heat.cso");
        m_finishPhysicsHeatPipeline = LoadComputePipeline(device, root, "sim/probe_tick_finish_physics_heat.cso");
        m_refreshTablePipeline = LoadComputePipeline(device, root, "sim/probe_tick_refresh_table.cso");
        m_remapSpeciesPipeline = LoadComputePipeline(device, root, "sim/probe_tick_remap_species.cso");
        m_busyPipeline = LoadComputePipeline(device, root, "sim/probe_tick_busy.cso");
        m_hashCellsPipeline = LoadComputePipeline(device, root, "sim/probe_tick_hash_cells.cso");
        m_flushEventsPipeline = LoadComputePipeline(device, root, "sim/probe_tick_flush_events.cso");
        m_extractPipeline = LoadComputePipeline(device, root, "sim/probe_tick_extract.cso");

        return m_enqueuePipeline && m_applyPipeline && m_wakeDuePipeline && m_addPhysicsHeatPipeline &&
               m_finishPhysicsHeatPipeline && m_refreshTablePipeline && m_remapSpeciesPipeline && m_busyPipeline &&
               m_hashCellsPipeline && m_flushEventsPipeline && m_extractPipeline && CreateConductGraph(device);
    }

    // 伝導の Work Graph(WakeBlocks → ConductBlock)。compute と同じルート署名をグローバルのルート署名にする
    bool ProbeSim::CreateConductGraph(ID3D12Device5* device) {
        const auto library = gpu::LoadShader("sim/probe_conduct.cso");
        if (!library) {
            Log(Channel::Sim, Level::Error, "{}", library.error());
            return false;
        }

        auto graph = gpu::WorkGraph::Create(device, m_rootSignature.Get(), *library, L"ProbeConduct");
        if (!graph) {
            Log(Channel::Sim, Level::Error, "{}", graph.error());
            return false;
        }

        m_conductGraph = std::make_unique<gpu::WorkGraph>(std::move(*graph));
        m_conductEntrypoint = m_conductGraph->EntrypointIndex(L"WakeBlocks");

        return m_conductEntrypoint != UINT32_MAX;
    }

    bool ProbeSim::CreateBuffers(ID3D12Device5* device) {
        m_cells = gpu::CreateBuffer(device, CELL_BYTES * 2, gpu::BufferKind::UnorderedAccess);
        m_thermal = gpu::CreateBuffer(device, THERMAL_BYTES * 2, gpu::BufferKind::UnorderedAccess);
        m_busySink = gpu::CreateBuffer(device, SLICE_BYTES, gpu::BufferKind::UnorderedAccess);
        m_hashes = gpu::CreateBuffer(device, PROBE_HASH_BYTES, gpu::BufferKind::UnorderedAccess);
        m_commandQueue = gpu::CreateBuffer(device, PROBE_COMMAND_QUEUE_BYTES, gpu::BufferKind::UnorderedAccess);
        m_tickEvents = gpu::CreateBuffer(device, PROBE_TICK_EVENT_BYTES, gpu::BufferKind::UnorderedAccess);
        m_blockSchedule = gpu::CreateBuffer(device, PROBE_SCHEDULE_BYTES, gpu::BufferKind::UnorderedAccess);
        // 0 の見出し(保存点から戻す・写しを取る時にコマンドキューを空にする。既定のヒープは 0 で初期化される)
        m_zeroQueueHeader = gpu::CreateBuffer(device, PROBE_COMMAND_QUEUE_HEADER_BYTES,
                                              gpu::BufferKind::UnorderedAccess);
        if (!m_cells || !m_thermal || !m_busySink || !m_hashes || !m_commandQueue || !m_tickEvents ||
            !m_blockSchedule || !m_zeroQueueHeader)
            return false;

        m_zeroQueueHeader->SetName(L"ProbeSim.zeroQueueHeader");

        m_blockSchedule->SetName(
            L"ProbeSim.blockSchedule");  // 最初のフレームで初めの中身を写す(ProbeInitialScheduleWords)
        // 活性の一覧: 作った時は 0(空。見出しは刻みの適用の単位が毎刻み書く)
        for (uint32_t parity = 0; parity < 2; ++parity) {
            m_activeLists[parity] = gpu::CreateBuffer(device, PROBE_ACTIVE_LIST_BYTES,
                                                      gpu::BufferKind::UnorderedAccess);
            if (!m_activeLists[parity])
                return false;

            m_activeLists[parity]->SetName(std::format(L"ProbeSim.activeList{}", parity).c_str());
        }

        m_cells->SetName(L"ProbeSim.cells");
        m_thermal->SetName(L"ProbeSim.thermal");
        m_busySink->SetName(L"ProbeSim.busySink");
        m_hashes->SetName(L"ProbeSim.hashes");
        m_commandQueue->SetName(L"ProbeSim.commandQueue");  // 作った時は 0(末尾 = 先頭 = 0 の空のキュー)
        m_tickEvents->SetName(L"ProbeSim.tickEvents");

        for (uint32_t index = 0; index < PROBE_EXTRACTION_COUNT; ++index) {
            m_extractions[index] = gpu::CreateBuffer(device, EXTRACTION_BYTES, gpu::BufferKind::UnorderedAccess);
            if (!m_extractions[index])
                return false;

            m_extractions[index]->SetName(std::format(L"ProbeSim.extraction{}", index).c_str());
        }

        const D3D12_QUERY_HEAP_DESC queryDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
                                              .Count = FRAME_SLOT_COUNT * TIMESTAMPS_PER_SLOT};

        return SUCCEEDED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestamps)));
    }

    // 物理(T-0098): 場面があれば物の 1 刻みを作る。場面は作った後は持たない
    std::expected<void, std::string> ProbeSim::CreatePhysics(ID3D12Device5* device) {
        const PhysicsScene* scene = m_options.physicsScene;
        m_options.physicsScene = nullptr;
        if (scene == nullptr)
            return {};

        auto physics = GpuPhysics::Create(device, *scene, m_options.physicsParameters, m_options.physicsOptions);
        if (!physics)
            return std::unexpected("仮の世界の物理を作れない: " + physics.error());

        m_physics = std::make_unique<GpuPhysics>(std::move(*physics));
        m_physicsRate = physics::PxStepRate(m_options.physicsParameters);

        return {};
    }

    // 反応の表(既定のヒープ)と、表・初めの世界のアップロード。写すのは最初のフレーム(RecordInitialization)
    bool ProbeSim::CreateWorld(ID3D12Device5* device, const BakedReactionTable& table,
                               std::span<const reaction::RxCell> initialWorld) {
        FX_ASSERT(initialWorld.empty() || initialWorld.size() == PROBE_CELL_COUNT);
        const std::vector<reaction::RxCell> cells = initialWorld.empty()
                                                        ? MakeProbeInitialWorld(table)
                                                        : std::vector<reaction::RxCell>(initialWorld.begin(),
                                                                                        initialWorld.end());
        std::vector<reaction::HcThermalCache> caches;
        caches.reserve(cells.size());
        for (const reaction::RxCell& cell : cells)
            caches.push_back(ProbeMakeCache(table.View(), cell));

        m_initialUploads = {
            CreateFilledUpload(device, table.species, L"ProbeSim.upload.species"),
            CreateFilledUpload(device, table.rules, L"ProbeSim.upload.rules"),
            CreateFilledUpload(device, table.ruleIndex, L"ProbeSim.upload.ruleIndex"),
            CreateFilledUpload(device, table.rates, L"ProbeSim.upload.rates"),
            CreateFilledUpload(device, cells, L"ProbeSim.upload.cells"),
            CreateFilledUpload(device, caches, L"ProbeSim.upload.thermal"),
            CreateFilledUpload(device, ProbeInitialScheduleWords(table, cells), L"ProbeSim.upload.blockSchedule")};

        constexpr std::array<const wchar_t*, 4> TABLE_NAMES = {L"ProbeSim.species", L"ProbeSim.rules",
                                                               L"ProbeSim.ruleIndex", L"ProbeSim.rates"};
        for (size_t index = 0; index < m_reactionTable.size(); ++index) {
            if (!m_initialUploads[index])
                return false;

            m_reactionTable[index] = gpu::CreateBuffer(device, m_initialUploads[index]->GetDesc().Width,
                                                       gpu::BufferKind::UnorderedAccess);
            if (!m_reactionTable[index])
                return false;

            m_reactionTable[index]->SetName(TABLE_NAMES[index]);
        }

        m_viewSpecies = ProbeViewSpecies(table);
        m_tableCopy = table;
        m_speciesCount = static_cast<uint32_t>(table.species.size());

        return m_initialUploads[4] && m_initialUploads[5] && m_initialUploads[6];
    }

    // 最初のフレームの始め: 表と初めの世界(2 世代とも同じ S(0))を既定のヒープへ写す。
    // 既定のバッファは COMMON なので、写すときに COPY_DEST へ暗黙に昇格する。写した後は使う状態へ明示的に移す
    // (フレームの終わりに COMMON へ戻る。バッファは ExecuteCommandLists の終わりで COMMON に落ちる)
    void ProbeSim::RecordInitialization(ID3D12GraphicsCommandList10* list) {
        for (size_t index = 0; index < m_reactionTable.size(); ++index) {
            list->CopyBufferRegion(m_reactionTable[index].Get(), 0, m_initialUploads[index].Get(), 0,
                                   m_initialUploads[index]->GetDesc().Width);
        }

        for (uint64_t generation = 0; generation < 2; ++generation) {
            list->CopyBufferRegion(m_cells.Get(), generation * CELL_BYTES, m_initialUploads[4].Get(), 0, CELL_BYTES);
            list->CopyBufferRegion(m_thermal.Get(), generation * THERMAL_BYTES, m_initialUploads[5].Get(), 0,
                                   THERMAL_BYTES);
        }

        list->CopyBufferRegion(m_blockSchedule.Get(), 0, m_initialUploads[6].Get(), 0, PROBE_SCHEDULE_BYTES);

        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        barriers.reserve(m_reactionTable.size() + 3);
        for (const ComPtr<ID3D12Resource>& buffer : m_reactionTable) {
            barriers.push_back(gpu::Transition(buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        }

        barriers.push_back(
            gpu::Transition(m_cells.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        barriers.push_back(
            gpu::Transition(m_thermal.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        barriers.push_back(gpu::Transition(m_blockSchedule.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        RecordGasInitialization(list);
        RecordLiquidInitialization(list);
        RecordRadiationInitialization(list);
        m_initialized = true;
    }

    // フレームの枠ごとに: リスト(毎フレーム記録し直す)・入力のアップロード・読み戻し
    bool ProbeSim::CreateFrameSlots(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType) {
        for (FrameSlot& frame : m_slots) {
            if (FAILED(device->CreateCommandAllocator(listType, IID_PPV_ARGS(&frame.allocator))) ||
                FAILED(
                    device->CreateCommandList1(0, listType, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&frame.list)))) {
                return false;
            }

            frame.input = gpu::CreateBuffer(device, PROBE_INPUT_BYTES, gpu::BufferKind::Upload);
            frame.timestampReadback = gpu::CreateBuffer(device, uint64_t{TIMESTAMPS_PER_SLOT} * 8,
                                                        gpu::BufferKind::Readback);
            frame.hashReadback = gpu::CreateBuffer(device, PROBE_HASH_BYTES, gpu::BufferKind::Readback);
            frame.radiationHeaderReadback = gpu::CreateBuffer(device, RADIATION_HEADER_READBACK_BYTES,
                                                              gpu::BufferKind::Readback);
            if (!frame.input || !frame.timestampReadback || !frame.hashReadback || !frame.radiationHeaderReadback)
                return false;

            void* mapped = nullptr;
            const D3D12_RANGE noRead{};
            if (FAILED(frame.input->Map(0, &noRead, &mapped)))
                return false;

            frame.mappedInput = static_cast<std::byte*>(mapped);
        }

        return true;
    }

    // --- フレームごとに記録する ---

    bool ProbeSim::ValidateInput(uint32_t slot, const ProbeFrameInput& input) const {
        const bool valid = slot < FRAME_SLOT_COUNT && input.firstUnit < UnitsPerTick() &&
                           input.unitCount <= MAX_UNITS_PER_FRAME && input.commands.size() <= PROBE_MAX_COMMANDS &&
                           input.extractionTarget < PROBE_EXTRACTION_COUNT;

        if (!valid) {
            Log(Channel::Sim, Level::Error, "フレームの入力が範囲外: slot {} 単位 {}+{} コマンド {} 抽出 {}", slot,
                input.firstUnit, input.unitCount, input.commands.size(), input.extractionTarget);
            return false;
        }

        return ValidateSavePoints(input) && ValidateCommands(input) && ValidateTableSwap(input) &&
               ValidateRegions(input);
    }

    // 区画のある世界の表の差し替え(T-0620・T-0670): 区画の作業場は新しい表で作り直すので、新しい表で区画を作れない時だけ断る
    std::expected<void, std::string> ProbeSim::CheckRegionTableSwap(const BakedReactionTable& table) const {
        if (!m_gas && !m_liquidWork)
            return {};

        if (const auto remap = BuildSpeciesRemap(m_tableCopy, table); !remap)
            return std::unexpected("物質を付け替えられない表: " + remap.error());

        if (auto gas = CheckGasTableSwap(table); !gas)
            return gas;

        return CheckLiquidTableSwap(table);
    }

    // 区画のある世界の約束: 表の差し替えは CheckRegionTableSwap が通す表だけ(新しい表で区画を作れるかは表だけで決まるので、
    // 前の差し替えの表と比べなくてよい)。写しの早送りの写す元は同じ区画(見出しが同じ = 写す欄の並びと大きさが同じ)
    bool ProbeSim::ValidateRegions(const ProbeFrameInput& input) const {
        for (const ProbeTableSwap& swap : input.tableSwaps) {
            if (const auto compatible = CheckRegionTableSwap(*swap.table); !compatible) {
                Log(Channel::Sim, Level::Error, "刻み {} の表の差し替えを断る: {}", swap.tick, compatible.error());
                return false;
            }
        }

        if (input.adoptFrom == nullptr)
            return true;

        const ProbeSim& source = *input.adoptFrom;
        const bool sameGas = (m_gas == nullptr) == (source.m_gas == nullptr) &&
                             std::memcmp(&m_gasHeader, &source.m_gasHeader, sizeof(ProbeGasHeader)) == 0;
        const bool sameLiquid = (m_liquidWork == nullptr) == (source.m_liquidWork == nullptr) &&
                                std::memcmp(&m_liquidHeader, &source.m_liquidHeader, sizeof(ProbeLiquidHeader)) == 0;
        const bool sameRadiation = m_cascade.has_value() == source.m_cascade.has_value() &&
                                   m_cascadeNegative.has_value() == source.m_cascadeNegative.has_value() &&
                                   (!m_cascade ||
                                    m_cascade->PersistentStateBytes() == source.m_cascade->PersistentStateBytes());
        if (!sameGas || !sameLiquid || !sameRadiation)
            Log(Channel::Sim, Level::Error, "写しの早送りの写す元と区画(気体 {}・液体 {}・放射 {})が違う",
                sameGas ? "同じ" : "違う", sameLiquid ? "同じ" : "違う", sameRadiation ? "同じ" : "違う");

        return sameGas && sameLiquid && sameRadiation;
    }

    // 表の差し替えの約束: (tick, 適用の単位) がこのフレームの単位の中にあり、表がある(T-0139)
    bool ProbeSim::ValidateTableSwap(const ProbeFrameInput& input) const {
        const uint64_t first = (input.firstTick * UnitsPerTick()) + input.firstUnit;
        for (const ProbeTableSwap& swap : input.tableSwaps) {
            const uint64_t swapUnit = swap.tick * UnitsPerTick();
            if (swap.table != nullptr && swapUnit >= first && swapUnit < first + input.unitCount)
                continue;

            Log(Channel::Sim, Level::Error,
                "表の差し替えの刻み {} の適用の単位が、このフレーム(刻み {} の単位 {} から {} 個)に無い", swap.tick,
                input.firstTick, input.firstUnit, input.unitCount);
            return false;
        }

        return true;
    }

    // コマンドの約束(ファイルの先頭): キューの空き・並び・適用に間に合う刻み。破ると GPU で捨てられるか、キューが壊れる
    bool ProbeSim::ValidateCommands(const ProbeFrameInput& input) const {
        // 保存点から戻すフレームは、足す前にキューが空になる(T-0143)
        const bool restoring = input.restoreFrom != NO_SAVE_POINT;
        const uint32_t freeSlots = restoring ? PROBE_COMMAND_QUEUE_CAPACITY : FreeCommandSlots();
        if (input.commands.size() > freeSlots) {
            Log(Channel::Sim, Level::Error, "コマンドキューの空きが足りない: 足す {} 空き {}", input.commands.size(),
                freeSlots);
            return false;
        }

        const uint64_t nextApplyTick = NextApplyTick(input.firstTick, input.firstUnit);
        const ProbeCommand* previous = m_hasEnqueued && !restoring ? &m_lastEnqueued : nullptr;
        for (const ProbeCommand& command : input.commands) {
            if (command.targetTick < nextApplyTick || (previous != nullptr && !CommandPrecedes(*previous, command))) {
                Log(Channel::Sim, Level::Error,
                    "コマンドの刻みか並びが不正: 刻み {} 番号 {}(適用に間に合う最初の刻み {}、前のコマンド {} / {})",
                    command.targetTick, command.sequence, nextApplyTick, previous != nullptr ? previous->targetTick : 0,
                    previous != nullptr ? previous->sequence : 0);
                return false;
            }

            previous = &command;
        }

        return true;
    }

    // 見出し(probe_sim.hlsli の PROBE_HEADER_*)とコマンド
    void ProbeSim::WriteInput(FrameSlot& frame, const ProbeFrameInput& input) const {
        const auto commandCount = static_cast<uint32_t>(input.commands.size());

        // 重さは 1 個あたりの回数にする(分けても 1 刻みの合計がほぼ同じになるように)
        const uint32_t busyPerPiece = m_options.busyIterations == 0
                                          ? 0
                                          : std::max(1u, m_options.busyIterations / m_options.busyPieces);

        const D3D12_GPU_VIRTUAL_ADDRESS list0 = m_activeLists[0]->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS list1 = m_activeLists[1]->GetGPUVirtualAddress();

        const std::array<uint32_t, PROBE_HEADER_WORDS> header = {commandCount,
                                                                 busyPerPiece,
                                                                 m_commandTail,
                                                                 m_conductEntrypoint,
                                                                 static_cast<uint32_t>(list0),
                                                                 static_cast<uint32_t>(list0 >> 32),
                                                                 static_cast<uint32_t>(list1),
                                                                 static_cast<uint32_t>(list1 >> 32),
                                                                 m_viewSpecies[0],
                                                                 m_viewSpecies[1],
                                                                 m_viewSpecies[2],
                                                                 m_physics ? m_physics->BodyCount() : 0,
                                                                 static_cast<uint32_t>(m_physicsRate)};

        WriteAt(frame.mappedInput, PROBE_INPUT_HEADER_OFFSET, header);

        if (commandCount > 0) {
            std::memcpy(frame.mappedInput + PROBE_INPUT_COMMANDS_OFFSET, input.commands.data(),
                        input.commands.size_bytes());
        }
    }

    ID3D12CommandList* ProbeSim::RecordFrame(uint32_t slot, const ProbeFrameInput& input) {
        if (!ValidateInput(slot, input))
            return nullptr;

        // 保存点から戻す・写しを取るフレームは、コマンドキューが空になってから足す(足す場所を入力に書く前に CPU の控えを空に。
        // T-0143・T-0412)
        if (input.restoreFrom != NO_SAVE_POINT || input.adoptFrom != nullptr)
            ResetCommandMirror();

        FrameSlot& frame = m_slots[slot];
        frame.keepAlive.clear();  // この枠の前のリストは GPU が終えている(呼ぶ側の約束)
        frame.retiredGas.clear();

        // 差し替えがあれば、フレームの終わりの抽出は最後の表の ID で物質を読む(物質の一覧が変わると ID がずれる。T-0223)。
        // 表の違う保存点へ戻すなら、まずその表の ID で(T-0548)
        if (input.restoreFrom != NO_SAVE_POINT && SavePointChangesTable(input.restoreFrom))
            m_viewSpecies = ProbeViewSpecies(m_savePoints[input.restoreFrom].table);
        if (!input.tableSwaps.empty())
            m_viewSpecies = ProbeViewSpecies(*rng::max(input.tableSwaps, {}, &ProbeTableSwap::tick).table);
        WriteInput(frame, input);
        if (FAILED(frame.allocator->Reset()) || FAILED(frame.list->Reset(frame.allocator.Get(), nullptr))) {
            Log(Channel::Sim, Level::Error, "フレームのリストを記録し直せない(slot {})", slot);
            return nullptr;
        }

        ID3D12GraphicsCommandList10* list = frame.list.Get();
        const uint32_t firstQuery = slot * TIMESTAMPS_PER_SLOT;
        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery);
        if (m_radiationStamps)
            m_radiationStamps->Begin(slot);

        const bool initializing = !m_initialized;
        if (initializing)
            RecordInitialization(list);

        // 巻き戻し(T-0143): 刻みの境界から始まるフレームの先頭で、保存点へ写すか保存点から戻す(コマンドを足す前)
        // 写しの早送り(T-0412): 別の世界の S(firstTick) をこの世界へ(戻すのと同じ所)
        if (input.adoptFrom != nullptr)
            RecordAdopt(list, *input.adoptFrom, input.firstTick);
        else if (input.restoreFrom != NO_SAVE_POINT && !RecordRestore(list, frame, input.restoreFrom))
            return nullptr;
        else if (input.saveTo != NO_SAVE_POINT && !RecordSave(list, frame, input.saveTo, input.firstTick))
            return nullptr;

        // 表の差し替えのあるフレーム(T-0670): 区画の作業場を UAV に揃える(差し替えで作業場を作り直す時、古い作業場から持ち越す欄を
        // 写すので、その時の状態を決めておく。ここまではどのバッファも COMMON。初期化のフレームは初期化が UAV にしてある)
        if (!initializing && !input.tableSwaps.empty())
            RecordRegionWorkStates(list, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        BindRootArguments(list, frame.input.Get());
        m_events.RecordBegin(list);
        m_debugRing.RecordBegin(list);
        m_graphStats.RecordBegin(list);
        m_graphTrace.RecordBegin(list, slot);
        RecordPhysicsInitialization(list, frame.input.Get());

        const D3D12_RESOURCE_BARRIER hashesToUav = gpu::Transition(m_hashes.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &hashesToUav);
        // 活性の一覧はフレームの中では UAV(伝導の間だけ入力の組を GPU の入力の状態に)。フレームの終わりに COMMON へ戻す
        RecordActiveListStates(list, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        RecordEnqueue(list, static_cast<uint32_t>(input.commands.size()));

        // --- 単位を順に。刻みの終わりをまたいだら次の刻みへ(単位ごとに終わりのタイムスタンプ)---
        uint64_t tick = input.firstTick;
        uint32_t unit = input.firstUnit;
        bool hasHash = false;

        for (uint32_t index = 0; index < input.unitCount; ++index) {
            // 表の差し替え(T-0139): その刻みの適用の単位の前に写す(適用の後の RefreshTable は RecordUnit)
            const auto swap = rng::find(input.tableSwaps, tick, &ProbeTableSwap::tick);
            const bool tableSwapped = unit == PROBE_UNIT_APPLY && swap != input.tableSwaps.end();
            if (tableSwapped && !RecordTableSwap(list, frame, *swap->table, tick)) {
                Log(Channel::Sim, Level::Error, "反応表の差し替えを記録できない(刻み {})", tick);
                return nullptr;
            }

            RecordUnit(list, frame.input.Get(), tick, unit, tableSwapped);
            list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery + 1 + index);
            hasHash = hasHash || unit == HashUnit();

            if (++unit == UnitsPerTick()) {
                unit = 0;
                ++tick;
            }
        }

        // --- 刻みの境界の状態 S(tick) を抽出へ(刻みの途中で終わっても、途中の刻みは別の世代に書いているので S(tick) は揃っている)---
        if (input.extract)
            RecordExtractAndHook(list, frame, tick, input);

        RecordActiveListStates(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (input.readPhysics && m_physics)
            m_physics->RecordReadback(list);

        RecordReadbacks(list, slot, hasHash);
        const uint32_t lastQuery = firstQuery + 1 + input.unitCount;
        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, lastQuery);
        if (m_radiationStamps)
            m_radiationStamps->Resolve(list);

        list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery, input.unitCount + 2,
                               frame.timestampReadback.Get(), 0);
        if (const HRESULT closed = list->Close(); FAILED(closed)) {
            Log(Channel::Sim, Level::Error, "フレームのリストを閉じられない(slot {}・{:#x})", slot,
                static_cast<uint32_t>(closed));
            return nullptr;
        }

        frame.firstTick = input.firstTick;
        frame.firstUnit = input.firstUnit;
        frame.unitCount = input.unitCount;
        TrackCommands(input.commands, NextApplyTick(tick, unit));

        // 表を差し替えても保存点は捨てない。保存点はその境界の表も持ち、戻すと表も戻る(T-0548・ADR-0246。ADR-0047 §6 を置き換え)

        return list;
    }

    // 抽出と、その後ろのフック(覗き窓。T-0096)。フックは書き終えた抽出を読めるように UAV のバリアの後
    void ProbeSim::RecordExtractAndHook(ID3D12GraphicsCommandList10* list, FrameSlot& frame, uint64_t tick,
                                        const ProbeFrameInput& input) {
        RecordExtract(list, tick, input.extractionTarget);
        if (!input.afterExtract)
            return;

        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &allUavs);
        input.afterExtract(list, {.tick = tick,
                                  .slot = static_cast<uint32_t>(&frame - m_slots.data()),
                                  .cells = m_cells.Get(),
                                  .extraction = m_extractions[input.extractionTarget].Get(),
                                  .debugRing = m_debugRing.GpuAddress(),
                                  .keepAlive = &frame.keepAlive});
    }

    // 新しいコマンドを GPU のキューの末尾へ(フレームのリストの先頭。単位より前)
    void ProbeSim::RecordEnqueue(ID3D12GraphicsCommandList10* list, uint32_t commandCount) const {
        if (commandCount == 0)
            return;

        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        SetUnitConstants(list, 0, 0);
        list->SetPipelineState(m_enqueuePipeline.Get());
        list->Dispatch((commandCount + PROBE_LINEAR_GROUP_SIZE - 1) / PROBE_LINEAR_GROUP_SIZE, 1, 1);
        list->ResourceBarrier(1, &allUavs);
    }

    // CPU 側の控え: 足したコマンドを数え、このフレームで適用の単位を記録した刻み(nextApplyTick より前)の分を取り出し済みにする
    void ProbeSim::TrackCommands(std::span<const ProbeCommand> commands, uint64_t nextApplyTick) {
        for (const ProbeCommand& command : commands) {
            if (m_queuedTicks.empty() || m_queuedTicks.back().targetTick != command.targetTick)
                m_queuedTicks.push_back({.targetTick = command.targetTick});

            ++m_queuedTicks.back().count;
        }

        m_queuedCommandCount += static_cast<uint32_t>(commands.size());
        m_commandTail += static_cast<uint32_t>(commands.size());
        if (!commands.empty()) {
            m_hasEnqueued = true;
            m_lastEnqueued = commands.back();
        }

        const auto applied = rng::find_if(m_queuedTicks,
                                          [&](const QueuedTick& queued) { return queued.targetTick >= nextApplyTick; });
        for (auto queued = m_queuedTicks.begin(); queued != applied; ++queued)
            m_queuedCommandCount -= queued->count;

        m_queuedTicks.erase(m_queuedTicks.begin(), applied);
    }

    void ProbeSim::BindRootArguments(ID3D12GraphicsCommandList10* list, ID3D12Resource* input) const {
        list->SetComputeRootSignature(m_rootSignature.Get());
        BindRootViews(list, input);
    }

    // ルートの引数: ROOT_LAYOUT の順(u0〜u12・デバッグのリング・Work Graphs のカウンタ・t0 フレームの入力・t1〜t4 反応の表)
    void ProbeSim::BindRootViews(ID3D12GraphicsCommandList10* list, ID3D12Resource* input) const {
        list->SetComputeRootUnorderedAccessView(UAV_CELLS, m_cells->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_THERMAL, m_thermal->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EVENTS, m_events.GpuAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EXTRACTION0, m_extractions[0]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EXTRACTION1, m_extractions[1]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EXTRACTION2, m_extractions[2]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_BUSY_SINK, m_busySink->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_HASHES, m_hashes->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_COMMAND_QUEUE, m_commandQueue->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_TICK_EVENTS, m_tickEvents->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_ACTIVE_LIST0, m_activeLists[0]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_ACTIVE_LIST1, m_activeLists[1]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_BLOCK_SCHEDULE, m_blockSchedule->GetGPUVirtualAddress());
        // 物理が無ければ、シェーダーは物の数 0 で読まないので、仮に重さの捨て場を結ぶ
        list->SetComputeRootUnorderedAccessView(
            UAV_BODIES, m_physics ? m_physics->Bodies()->GetGPUVirtualAddress() : m_busySink->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_PHYSICS_HEAT, m_physics
                                                                      ? m_physics->HeatEntries()->GetGPUVirtualAddress()
                                                                      : m_busySink->GetGPUVirtualAddress());
        // 気体の区画(T-0592)。無ければシェーダーは読まないので、仮に重さの捨て場を結ぶ
        const D3D12_GPU_VIRTUAL_ADDRESS sink = m_busySink->GetGPUVirtualAddress();
        list->SetComputeRootUnorderedAccessView(UAV_GAS_STATE, m_gas ? m_gasState->GetGPUVirtualAddress() : sink);
        list->SetComputeRootUnorderedAccessView(UAV_GAS_WORK, m_gas ? m_gasWork->GetGPUVirtualAddress() : sink);
        list->SetComputeRootUnorderedAccessView(UAV_GAS_CELLS,
                                                m_gas ? m_gas->CellBuffer()->GetGPUVirtualAddress() : sink);
        list->SetComputeRootUnorderedAccessView(UAV_GAS_BOX_LEDGER,
                                                m_gas ? m_gas->LedgerBuffer()->GetGPUVirtualAddress() : sink);
        list->SetComputeRootUnorderedAccessView(UAV_LIQUID_WORK,
                                                m_liquidWork ? m_liquidWork->GetGPUVirtualAddress() : sink);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), m_debugRing.GpuAddress());
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.GraphStatsIndex(), m_graphStats.GpuAddress());
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.GraphTraceIndex(), m_graphTrace.GpuAddress());
        list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(SRV_INPUT), input->GetGPUVirtualAddress());
        // 付け替えの表は差し替えの刻みの段だけが読む。まだ無ければ入力を仮に結ぶ(読まない)
        list->SetComputeRootShaderResourceView(
            ROOT_LAYOUT.SrvIndex(SRV_SPECIES_REMAP),
            m_speciesRemap ? m_speciesRemap->GetGPUVirtualAddress() : input->GetGPUVirtualAddress());
        for (uint32_t index = 0; index < m_reactionTable.size(); ++index) {
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(SRV_REACTION_FIRST + index),
                                                   m_reactionTable[index]->GetGPUVirtualAddress());
        }
    }

    // 1 つの単位(probe_sim.hlsli の単位の表)。最後に UAV バリアで、次の単位が結果を読めるようにする
    void ProbeSim::RecordUnit(ID3D12GraphicsCommandList10* list, ID3D12Resource* input, uint64_t tick, uint32_t unit,
                              bool tableSwapped) {
        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        // 適用: 置くコマンドの検査(物質の数。T-0222)
        SetUnitConstants(list, tick, unit == PROBE_UNIT_APPLY ? m_speciesCount : 0);
        if (unit == PROBE_UNIT_APPLY) {
            // コマンドの適用は 1 グループがキューの先頭から番号順に(置くコマンドのセルだけをグループで分ける。probe_tick.hlsl・ADR-0125)。刻みの一覧と表の欄の用意も
            list->SetPipelineState(m_applyPipeline.Get());
            list->Dispatch(1, 1, 1);
            RecordPhysicsHeat(list, tick);

            // 表を差し替えた刻み: 熱のキャッシュ(2 世代)を新しい表で作り直し、全部のブロックの起こす刻みを今にする(T-0139)
            if (tableSwapped) {
                list->ResourceBarrier(1, &allUavs);
                list->SetPipelineState(m_refreshTablePipeline.Get());
                list->Dispatch(2 * LINEAR_CELL_GROUPS, 1, 1);
            }

            // 気体(区画があるときだけ。T-0592・ADR-0256): 伝導と反応の前に流す。変わったセルのブロックは物理の熱と同じく起こす
            if (m_gas) {
                RecordGasGather(list);
                RecordGasStep(list, tick);
                BindRootArguments(list, input);
                SetUnitConstants(list, tick, m_speciesCount);
                RecordGasScatter(list);
            }

            // 液体(区画があるときだけ。T-0613・ADR-0266): 気体の後(CPU の (1c))。変わったセルのブロックは気体と同じく起こす
            if (m_liquidWork) {
                list->ResourceBarrier(1, &allUavs);
                RecordLiquid(list);
            }

            // 放射(いつも。T-1154・D-670): 液体の後(CPU の (1d))。変わったセルのブロックは気体と同じく起こす
            if (m_cascade) {
                list->ResourceBarrier(1, &allUavs);
                RecordRadiation(list, input, tick);
            }

            // 起こす刻みの来たブロックを同じ一覧へ(適用が一覧の見出しを整えた後。待ちの丸め。T-0122)
            list->ResourceBarrier(1, &allUavs);
            list->SetPipelineState(m_wakeDuePipeline.Get());
            list->Dispatch(BLOCK_GROUPS, 1, 1);
        } else if (unit == PROBE_UNIT_CONDUCT)
            RecordConduct(list, input, tick);
        else if (m_physics && unit == PROBE_UNIT_PHYSICS)
            RecordPhysics(list, input, tick);
        else if (unit == HashUnit()) {
            // 表の欄は適用の単位が用意してある(刻み・0)
            list->SetPipelineState(m_hashCellsPipeline.Get());
            list->Dispatch(LINEAR_CELL_GROUPS, 1, 1);
            // 区画の持ち越す欄(気体の運動量・液体の面の運動量など)と帳簿も状態に入れる(同じ欄への atomic の和。T-0620)
            if (m_gas)
                RecordGasHash(list);

            if (m_liquidWork)
                RecordLiquidHash(list);

            if (m_cascade)
                RecordRadiationHash(list, input, tick);

            // 刻みのイベントを並べてリングへ(ハッシュとは別のバッファなので間のバリアは要らない)
            list->SetPipelineState(m_flushEventsPipeline.Get());
            list->Dispatch(1, 1, 1);
        } else {
            list->SetPipelineState(m_busyPipeline.Get());
            list->Dispatch(BUSY_GROUPS, BUSY_GROUPS, 1);
        }

        list->ResourceBarrier(1, &allUavs);
    }

    // --- 熱の放射の段(T-1154。作るのと表は sim/probe_sim_radiation.cpp)---

    // 段の間だけ u15〜u17(気体の区画の結び先)を借りる。段の後で BindRootViews が結び直す
    void ProbeSim::BindRadiationViews(ID3D12GraphicsCommandList10* list, ID3D12Resource* first, ID3D12Resource* second,
                                      ID3D12Resource* third) const {
        list->SetComputeRootUnorderedAccessView(UAV_GAS_STATE, first->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_GAS_WORK, second->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_GAS_CELLS, third->GetGPUVirtualAddress());
    }

    // 写し(セル → 光の値・温度。参照の温度の形では両側の作業へ)→ 距離ごとの段の 1 刻み(側ごと。自分のルート署名)
    // → セルへ当てる(このルート署名に戻して。正 − 負)
    void ProbeSim::RecordRadiation(ID3D12GraphicsCommandList10* list, ID3D12Resource* input, uint64_t tick) {
        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        const GpuCascadeLight& negative = m_cascadeNegative ? *m_cascadeNegative : *m_cascade;
        MarkRadiation(list, ProbeRadiationStage::Start);
        BindRadiationViews(list, m_cascade->Work(), negative.Work(), m_radiationLights.Get());
        list->SetPipelineState(m_radiationPipelines[0].Get());
        list->Dispatch(LINEAR_CELL_GROUPS, 1, 1);
        list->ResourceBarrier(1, &allUavs);
        MarkRadiation(list, ProbeRadiationStage::Grid);

        gpu::StageStamps* stamps = m_radiationStamps ? &*m_radiationStamps : nullptr;
        m_cascade->RecordTick(list, m_debugRing.GpuAddress(), tick, stamps,
                              static_cast<uint8_t>(ProbeRadiationStage::PositiveFirst));
        if (m_cascadeNegative)
            RecordRadiationNegative(list, tick, stamps);

        BindRootArguments(list, input);
        SetUnitConstants(list, tick, m_speciesCount);
        BindRadiationViews(list, m_cascade->Work(), m_cascade->State(), negative.State());
        list->SetPipelineState(m_radiationPipelines[1].Get());
        list->Dispatch(LINEAR_CELL_GROUPS, 1, 1);
        list->ResourceBarrier(1, &allUavs);
        MarkRadiation(list, ProbeRadiationStage::Apply);
        BindRootViews(list, input);
    }

    // 負の側の 1 刻み。負の側が空の刻み(放つセルが無く、前に流した刻みが何も放たず受けず、入る持ち越しも空)は
    // GPU が起動の予告で全部の段を飛ばす(T-1229。CPU は読み戻さない。飛ばしても結果は流した時と同じ)
    void ProbeSim::RecordRadiationNegative(ID3D12GraphicsCommandList10* list, uint64_t tick, gpu::StageStamps* stamps) {
        const auto first = static_cast<uint8_t>(ProbeRadiationStage::NegativeFirst);
        if (!m_options.radiationSkipIdle) {
            m_cascadeNegative->RecordTick(list, m_debugRing.GpuAddress(), tick, stamps, first);
            return;
        }

        m_cascadeNegative->RecordPlan(list, m_debugRing.GpuAddress(), tick, stamps, first);
        m_cascadeNegative->BeginSkipIfIdle(list);
        m_cascadeNegative->RecordTick(list, m_debugRing.GpuAddress(), tick, stamps, first);
        m_cascadeNegative->EndSkipIfIdle(list);
    }

    // 段ごとの GPU 時間の印(測りの時だけ)
    void ProbeSim::MarkRadiation(ID3D12GraphicsCommandList10* list, ProbeRadiationStage stage) {
        if (m_radiationStamps)
            m_radiationStamps->Mark(list, static_cast<uint8_t>(stage));
    }

    // 状態のハッシュの単位: 次の刻みへの持ち越しの寄与を足す(側ごと。単位の引数 = 歩幅のスレッドの数)
    void ProbeSim::RecordRadiationHash(ID3D12GraphicsCommandList10* list, ID3D12Resource* input, uint64_t tick) {
        constexpr uint32_t HASH_GROUPS = 2048;  // 持ち越しの表の全部の鍵(セル × 12)を掃く(T-1164)
        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        SetUnitConstants(list, tick, HASH_GROUPS * PROBE_LINEAR_GROUP_SIZE);
        list->SetPipelineState(m_radiationPipelines[2].Get());
        MarkRadiation(list, ProbeRadiationStage::Start);
        for (const std::optional<GpuCascadeLight>* side : {&m_cascade, &m_cascadeNegative}) {
            if (!side->has_value())
                continue;

            BindRadiationViews(list, (*side)->Work(), (*side)->State(), m_radiationLights.Get());
            // 負の側が空の刻みは持ち越しの表も空(寄与 0)なので、同じ刻みの予告で飛ばす(T-1229)
            const bool skippable = side == &m_cascadeNegative && m_options.radiationSkipIdle;
            if (skippable)
                (*side)->BeginSkipIfIdle(list);

            list->Dispatch(HASH_GROUPS, 1, 1);
            if (skippable)
                (*side)->EndSkipIfIdle(list);

            // 測りの時だけ側ごとに待つ(ハッシュは atomic の和なので順は結果に入らない)
            if (!m_radiationStamps)
                continue;

            list->ResourceBarrier(1, &allUavs);
            MarkRadiation(list,
                          side == &m_cascade ? ProbeRadiationStage::HashPositive : ProbeRadiationStage::HashNegative);
        }

        SetUnitConstants(list, tick, 0);
        BindRootViews(list, input);
    }

    // 物理の熱をセルへ(T-0561・ADR-0242。物理を入れた世界だけ): 前の刻みの物理の単位が分けた熱の入口(GpuPhysics の u14)を、
    // コマンドの後に入口の位置のセルへ足し(AddPhysicsHeat)、足し終わったセルの熱のキャッシュとブロックの印を作り直す(FinishPhysicsHeat)。
    // CPU の ProbeReference::Advance の heat と同じ。刻み 0 の入口は物理を初めに作った時の 0
    void ProbeSim::RecordPhysicsHeat(ID3D12GraphicsCommandList10* list, uint64_t tick) const {
        if (!m_physics)
            return;

        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        const uint32_t entryCount = m_physics->HeatEntryCount();
        const uint32_t groups = (entryCount + PROBE_LINEAR_GROUP_SIZE - 1) / PROBE_LINEAR_GROUP_SIZE;
        if (groups == 0)
            return;

        list->ResourceBarrier(1, &allUavs);
        SetUnitConstants(list, tick, entryCount);
        list->SetPipelineState(m_addPhysicsHeatPipeline.Get());
        list->Dispatch(groups, 1, 1);

        list->ResourceBarrier(1, &allUavs);
        list->SetPipelineState(m_finishPhysicsHeatPipeline.Get());
        list->Dispatch(groups, 1, 1);
        SetUnitConstants(list, tick, m_speciesCount);  // 適用の単位の残りの段の定数に戻す
    }

    // 反応表の差し替え(T-0139・ADR-0047): 新しい表を新しい既定のバッファに写し、ルートの結び先を替える。差し替える前の表は
    // このフレームの前の単位と、まだ GPU にある前のフレームが読むので、このリストが終わるまで持つ(状態を追わないよう、毎回新しく作る)
    // 環境の値のホットリロード(T-0444・ADR-0194): 新しい表の重力が今の表と違えば、この刻みの物理の単位から新しい重力で刻む
    // (物理の単位は同じ刻みの適用の後ろ。記録の順 = 刻みの順)。同じなら触らない(作った時の physicsParameters の重力のまま)。
    // 気体・液体の区画の環境の値(設定の型・層の基準・液体の重力)は、区画の作業場を作り直して合わせる(RecordRegionTableSwap。T-0670)
    void ProbeSim::ApplyEnvironment(const WorldPhysics& environment, uint64_t tick) {
        const WorldPhysics& current = m_tableCopy.environment;
        if (!m_physics || environment.gravityMicrometersPerSecond2 == current.gravityMicrometersPerSecond2)
            return;

        Log(Channel::Sim, Level::Info, "刻み {} から重力を {} µm/s² にする(表の環境の値)", tick,
            environment.gravityMicrometersPerSecond2);
        m_physics->SetGravity(PhysicsGravityFor(environment));
    }

    bool ProbeSim::RecordTableSwap(ID3D12GraphicsCommandList10* list, FrameSlot& frame, const BakedReactionTable& table,
                                   uint64_t tick) {
        const auto remap = BuildSpeciesRemap(m_tableCopy, table);
        if (!remap) {
            Log(Channel::Sim, Level::Error, "物質を付け替えられない表: {}", remap.error());
            return false;
        }

        // 単体の無い元素の原子は捨てて当てる(D-484)。GPU の世界は量を数えないので、何を捨てるかだけ知らせる(量は CPU リファレンスの報告)
        if (!remap->dropped.empty())
            Log(Channel::Sim, Level::Warning,
                "刻み {} の表の差し替えで、単体が新しい表に無い元素の原子を捨てる(改造による出入り): {}", tick,
                remap->dropped);

        // 区画の作業場は今の表(m_tableCopy)と比べて作り直すので、表を写す前に
        if (!RecordRegionTableSwap(list, frame, table, *remap) || !RecordTableBuffers(list, frame, table, tick) ||
            !RecordRadiationTableSwap(list, frame, table))
            return false;

        BindRootViews(list, frame.input.Get());

        return remap->identity || RecordSpeciesRemap(list, frame, *remap, tick);
    }

    // 表の差し替えで区画(気体・液体)の作業場を作り直す(T-0670)。区画の段はルートの結び先を読むので、呼ぶ側が後で結び直す
    bool ProbeSim::RecordRegionTableSwap(ID3D12GraphicsCommandList10* list, FrameSlot& frame,
                                         const BakedReactionTable& table, const SpeciesRemap& remap) {
        return RecordGasTableSwap(list, frame, table, remap) && RecordLiquidTableSwap(list, frame, table, remap);
    }

    // 区画の作業場(気体・液体)の状態を明示に移す(表の差し替えのあるフレームの初め。T-0670)
    void ProbeSim::RecordRegionWorkStates(ID3D12GraphicsCommandList10* list, D3D12_RESOURCE_STATES before,
                                          D3D12_RESOURCE_STATES after) const {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (ID3D12Resource* work : {m_gas ? m_gasWork.Get() : nullptr, m_liquidWork.Get()}) {
            if (work != nullptr)
                barriers.push_back(gpu::Transition(work, before, after));
        }

        if (!barriers.empty())
            list->ResourceBarrier((uint32_t)barriers.size(), barriers.data());
    }

    // 表 4 つを写して結び直し、環境の値(重力)を合わせる(差し替えと、表の違う保存点へ戻す時。世界のセルには触れない。T-0548)
    bool ProbeSim::RecordTableBuffers(ID3D12GraphicsCommandList10* list, FrameSlot& frame,
                                      const BakedReactionTable& table, uint64_t tick) {
        ComPtr<ID3D12Device5> device;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
            return false;

        const std::array<ComPtr<ID3D12Resource>, 4> uploads = {
            CreateFilledUpload(device.Get(), table.species, L"ProbeSim.swap.species"),
            CreateFilledUpload(device.Get(), table.rules, L"ProbeSim.swap.rules"),
            CreateFilledUpload(device.Get(), table.ruleIndex, L"ProbeSim.swap.ruleIndex"),
            CreateFilledUpload(device.Get(), table.rates, L"ProbeSim.swap.rates")};

        std::array<D3D12_RESOURCE_BARRIER, 4> barriers{};
        for (size_t index = 0; index < m_reactionTable.size(); ++index) {
            if (!uploads[index])
                return false;

            const uint64_t bytes = uploads[index]->GetDesc().Width;
            ComPtr<ID3D12Resource> buffer = gpu::CreateBuffer(device.Get(), bytes, gpu::BufferKind::UnorderedAccess);
            if (!buffer)
                return false;

            // 作ったバッファは COMMON(写すときに COPY_DEST へ暗黙に昇格する。RecordInitialization と同じ)
            list->CopyBufferRegion(buffer.Get(), 0, uploads[index].Get(), 0, bytes);
            barriers[index] = gpu::Transition(buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            frame.keepAlive.push_back(uploads[index]);
            frame.keepAlive.push_back(std::move(m_reactionTable[index]));
            m_reactionTable[index] = std::move(buffer);
        }

        m_speciesCount = static_cast<uint32_t>(table.species.size());

        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        ApplyEnvironment(table.environment, tick);
        m_tableCopy = table;
        m_tableSerial = ++m_lastTableSerial;

        return true;
    }

    // 物質の一覧が変わる差し替え(T-0223・ADR-0065): 付け替えの表を写して t5 に結び、2 世代のセルを付け替える(適用の単位の前)。
    // 新しい表はもう結んである(RxRemapCell は単体の h0 を新しい表から読む)
    bool ProbeSim::RecordSpeciesRemap(ID3D12GraphicsCommandList10* list, FrameSlot& frame, const SpeciesRemap& remap,
                                      uint64_t tick) {
        ComPtr<ID3D12Device5> device;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
            return false;

        const std::vector<uint32_t> words = PackSpeciesRemap(remap);
        ComPtr<ID3D12Resource> upload = CreateFilledUpload(device.Get(), words, L"ProbeSim.swap.speciesRemap");
        if (!upload)
            return false;

        const uint64_t bytes = upload->GetDesc().Width;
        ComPtr<ID3D12Resource> buffer = gpu::CreateBuffer(device.Get(), bytes, gpu::BufferKind::UnorderedAccess);
        if (!buffer)
            return false;

        list->CopyBufferRegion(buffer.Get(), 0, upload.Get(), 0, bytes);
        const D3D12_RESOURCE_BARRIER toRead = gpu::Transition(buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &toRead);
        frame.keepAlive.push_back(std::move(upload));
        if (m_speciesRemap)
            frame.keepAlive.push_back(std::move(m_speciesRemap));
        m_speciesRemap = std::move(buffer);
        BindRootViews(list, frame.input.Get());
        SetUnitConstants(list, tick, 0);

        list->SetPipelineState(m_remapSpeciesPipeline.Get());
        list->Dispatch(2 * LINEAR_CELL_GROUPS, 1, 1);
        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &allUavs);

        return true;
    }

    // 伝導: 刻み t の活性の一覧を GPU の入力にして DispatchGraph(何ブロック計算するかは GPU だけが知っている)。
    // 一覧は GPU の入力の間だけ読む状態(仕様: NON_PIXEL_SHADER_RESOURCE か COMMON)。グラフは次の刻みの一覧(もう一方の組)に UAV で書く
    void ProbeSim::RecordConduct(ID3D12GraphicsCommandList10* list, ID3D12Resource* input, uint64_t tick) {
        ID3D12Resource* activeList = m_activeLists[tick & 1].Get();
        const D3D12_RESOURCE_BARRIER toInput = gpu::Transition(activeList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &toInput);

        // 裏のメモリの初期化は最初の 1 回だけ(リストは記録した順に実行される)。グラフを設定してからルートの引数を結び直す
        m_conductGraph->SetProgram(list, !m_conductInitialized);
        m_conductInitialized = true;
        BindRootViews(list, input);
        SetUnitConstants(list, tick, 0);
        gpu::WorkGraph::DispatchFromGpu(list, activeList->GetGPUVirtualAddress());

        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(activeList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
    }

    // 最初のフレームだけ: 物の初めの状態(物理のルート署名で記録するので、仮の刻みのルートを結び直す)
    void ProbeSim::RecordPhysicsInitialization(ID3D12GraphicsCommandList10* list, ID3D12Resource* input) {
        if (!m_physics || m_physicsInitialized)
            return;

        m_physics->RecordInitialize(list, m_debugRing.GpuAddress());
        m_physicsInitialized = true;
        BindRootArguments(list, input);
    }

    // 物理: 物の 1 刻み(sim/gpu_physics。広域と接触の Work Graph・色ごとの Compute)。物理のルート署名で記録するので、終わったら結び直す。
    // 物理の刻みは世界の刻みと同じ番号で進む(1 刻みに 1 回、刻みの順に記録する)
    void ProbeSim::RecordPhysics(ID3D12GraphicsCommandList10* list, ID3D12Resource* input, uint64_t tick) {
        if (m_physics->Tick() != tick)
            Log(Channel::Sim, Level::Error, "物理の刻みが世界の刻みとずれた: 物理 {} 世界 {}", m_physics->Tick(), tick);

        m_physics->RecordStep(list, m_debugRing.GpuAddress());
        BindRootArguments(list, input);
    }

    void ProbeSim::RecordActiveListStates(ID3D12GraphicsCommandList10* list, D3D12_RESOURCE_STATES before,
                                          D3D12_RESOURCE_STATES after) const {
        const std::array<D3D12_RESOURCE_BARRIER, 2> barriers = {gpu::Transition(m_activeLists[0].Get(), before, after),
                                                                gpu::Transition(m_activeLists[1].Get(), before, after)};
        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }

    void ProbeSim::RecordExtract(ID3D12GraphicsCommandList10* list, uint64_t tick, uint32_t target) const {
        SetUnitConstants(list, tick, target);
        list->SetPipelineState(m_extractPipeline.Get());
        list->Dispatch(LINEAR_CELL_GROUPS, 1, 1);
    }

    // イベント・デバッグの出力・ノードのカウンタ・連鎖のトレース・(ハッシュの単位があれば)ハッシュの表を、slot の読み戻しのバッファへ
    void ProbeSim::RecordReadbacks(ID3D12GraphicsCommandList10* list, uint32_t slot, bool hasHash) const {
        m_events.RecordReadbackAndReset(list, slot);
        m_debugRing.RecordReadbackAndReset(list, slot);
        m_graphStats.RecordReadbackAndReset(list, slot);
        m_graphTrace.RecordReadbackAndReset(list, slot);
        RecordRadiationHeaderReadback(list, slot);

        if (hasHash) {
            gpu::RecordCopyToReadback(list, m_hashes.Get(), m_slots[slot].hashReadback.Get());
            const D3D12_RESOURCE_BARRIER toCommon = gpu::Transition(m_hashes.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                                    D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(1, &toCommon);

            return;
        }

        const D3D12_RESOURCE_BARRIER toCommon = gpu::Transition(m_hashes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1, &toCommon);
    }

    // --- 読み戻し ---

    ProbeFrameReadback ProbeSim::ReadFrame(uint32_t slot) {
        ProbeFrameReadback result;
        const FrameSlot& frame = m_slots[slot];

        // イベント: 見出しを先に読み、書かれた分だけを読む(並びは GPU が刻みの最後に並べた (刻み, 種類, 場所) の順)
        EventRingHeader header;
        const bool headerRead = m_events.Read(slot, std::as_writable_bytes(std::span(&header, 1)));
        const uint32_t stored = std::min(header.requested, PROBE_EVENT_CAPACITY);
        std::vector<uint32_t> words(PROBE_EVENT_HEADER_BYTES / 4 + size_t{stored} * PROBE_EVENT_WORDS);

        if (headerRead && (stored == 0 || m_events.Read(slot, std::as_writable_bytes(std::span(words))))) {
            result.droppedEventCount = header.requested - stored + header.tickDropped;
            result.events.reserve(stored);
            for (uint32_t index = 0; index < stored; ++index) {
                const uint32_t* record = words.data() + PROBE_EVENT_HEADER_BYTES / 4 +
                                         size_t{index} * PROBE_EVENT_WORDS;
                result.events.push_back(
                    {.tick = record[0] | (uint64_t{record[1]} << 32), .type = record[2], .place = record[3]});
            }
        }

        // タイムスタンプ: [0] 始め、[1 + i] 単位 i の終わり、[1 + 数] 全体の終わり
        std::vector<uint64_t> timestamps(size_t{frame.unitCount} + 2);
        if (gpu::ReadBuffer(frame.timestampReadback.Get(), std::as_writable_bytes(std::span(timestamps)))) {
            result.gpuBeginTimestamp = timestamps.front();
            result.gpuEndTimestamp = timestamps.back();
            result.unitGpuTicks.reserve(frame.unitCount);

            for (uint32_t index = 0; index < frame.unitCount; ++index) {
                const uint64_t begin = timestamps[index];
                const uint64_t end = timestamps[size_t{index} + 1];
                result.unitGpuTicks.push_back(end >= begin ? end - begin : 0);
            }
        }

        result.firstUnit = frame.firstUnit;
        result.hashes = ReadHashes(frame);
        result.debugAssertCount = m_debugRing.Drain(MAX_LOGGED_DEBUG_MESSAGES, slot).assertCount;
        ReadRadiationHeader(frame, result);
        if (m_radiationStamps)
            m_radiationStamps->Collect(slot);

        // ノードのカウンタ: 要約は Trace、上限に当たった・近づいたものは Warning(gpu/work_graph_stats.h)
        auto graphStats = m_graphStats.Read(slot);
        if (graphStats) {
            result.graphFindingCount = static_cast<uint32_t>(m_graphStats.Report(*graphStats).size());
            result.graphStats = std::move(*graphStats);
        } else
            Log(Channel::WorkGraph, Level::Warning, "{}", graphStats.error());

        // 連鎖のトレース(範囲が無効なら空)
        auto trace = m_graphTrace.Read(slot);
        if (trace) {
            result.trace = std::move(trace->records);
            result.droppedTraceCount = trace->droppedCount;
        } else
            Log(Channel::WorkGraph, Level::Warning, "{}", trace.error());

        return result;
    }

    // 放射の段の状態の頭(側ごと)を slot の読み戻しへ(溢れの印・放つ面の数・袋の最大。T-1164)
    void ProbeSim::RecordRadiationHeaderReadback(ID3D12GraphicsCommandList10* list, uint32_t slot) const {
        uint64_t offset = 0;
        for (const std::optional<GpuCascadeLight>* side : {&m_cascade, &m_cascadeNegative}) {
            if (!side->has_value())
                continue;

            ID3D12Resource* state = (*side)->State();
            const D3D12_RESOURCE_BARRIER toCopy = gpu::Transition(state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &toCopy);
            list->CopyBufferRegion(m_slots[slot].radiationHeaderReadback.Get(), offset, state, 0,
                                   RADIATION_HEADER_BYTES);
            const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(state, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &toUav);
            offset += RADIATION_HEADER_BYTES;
        }
    }

    // 溢れは release でも Error に出す(GPU の assert は debug だけ。溢れると結果が順番で変わり、決定性が崩れる)
    void ProbeSim::ReadRadiationHeader(const FrameSlot& frame, ProbeFrameReadback& result) {
        if (!m_cascade)
            return;

        std::array<uint64_t, 2 * reaction::RC_GPU_HEADER_WORDS> words{};
        if (!gpu::ReadBuffer(frame.radiationHeaderReadback.Get(), std::as_writable_bytes(std::span(words))))
            return;

        const size_t sides = m_cascadeNegative ? 2 : 1;
        for (size_t side = 0; side < sides; ++side) {
            const uint64_t* header = words.data() + (side * reaction::RC_GPU_HEADER_WORDS);
            result.radiationOverflow |= static_cast<uint32_t>(header[reaction::RC_GPU_OVERFLOW]);
            result.radiationFaces = std::max(result.radiationFaces,
                                             static_cast<uint32_t>(header[reaction::RC_GPU_FACE_COUNT]));
            result.radiationBagPeak = std::max(result.radiationBagPeak,
                                               static_cast<uint32_t>(header[reaction::RC_GPU_BAG_PEAK]));
            result.radiationSkippedTicks += header[reaction::RC_GPU_SKIPPED];
            for (uint32_t level = 0; level < reaction::RC_GPU_STEPS - reaction::RC_GPU_RAYS; ++level)
                result.radiationRays += header[reaction::RC_GPU_RAYS + level];
        }

        if (result.radiationOverflow == 0 || m_radiationOverflowLogged)
            return;

        m_radiationOverflowLogged = true;
        Log(Channel::Sim, Level::Error,
            "熱の放射の段の置き場が溢れた(印 {}。1 = 放つ面・2 = 袋・4 = 持ち越し。放つ面 {}・袋の最大 {})。"
            "溢れた刻みから結果が順番で変わる",
            result.radiationOverflow, result.radiationFaces, result.radiationBagPeak);
    }

    // このフレームで終えた刻み t ごとに、表の (t + 1) 番目の S(t + 1) のハッシュ
    std::vector<ProbeTickHash> ProbeSim::ReadHashes(const FrameSlot& frame) const {
        std::vector<ProbeTickHash> hashes;
        const uint64_t unitsBefore = frame.firstUnit;
        const uint64_t unitsTotal = unitsBefore + frame.unitCount;
        // firstTick から数えて終えた刻み(途中から始めた刻みを含む)
        const uint64_t completedTicks = unitsTotal / UnitsPerTick();
        if (completedTicks == 0)
            return hashes;

        std::vector<uint32_t> table(PROBE_HASH_BYTES / 4);
        if (!gpu::ReadBuffer(frame.hashReadback.Get(), std::as_writable_bytes(std::span(table))))
            return hashes;

        hashes.reserve(completedTicks);
        for (uint64_t offset = 1; offset <= completedTicks; ++offset) {
            const uint64_t stateTick = frame.firstTick + offset;
            const uint32_t* entry = table.data() + (stateTick % PROBE_HASH_CAPACITY) * (PROBE_HASH_ENTRY_BYTES / 4);
            const uint64_t storedTick = entry[0] | (uint64_t{entry[1]} << 32);

            if (storedTick != stateTick) {
                Log(Channel::Sim, Level::Error, "ハッシュの表の刻みが合わない: 期待 {} 実際 {}", stateTick, storedTick);
                continue;
            }

            hashes.push_back({.tick = stateTick,
                              .hash = entry[2] | (uint64_t{entry[3]} << 32),
                              .energy = entry[4] | (uint64_t{entry[5]} << 32),
                              .sourceEnergy = entry[8] | (uint64_t{entry[9]} << 32),
                              .scheduledBlocks = entry[6],
                              .bodyHash = entry[10] | (uint64_t{entry[11]} << 32),
                              .limits = {.productsHeld = entry[12], .candidatesLimited = entry[13]},
                              .physicsHeatEnergy = static_cast<int64_t>(entry[14] | (uint64_t{entry[15]} << 32))});
        }

        return hashes;
    }

    // --- 初めの世界(T-0089。中身はパッケージの world_cells。T-0398)---

    std::vector<reaction::RxCell> MakeProbeInitialWorld(const BakedReactionTable& table) {
        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        for (const WorldCellRegion& region : table.worldLayout.cellRegions) {
            for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
                const uint32_t x = index % PROBE_GRID_SIZE;
                const uint32_t y = (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE;
                const uint32_t z = index / PROBE_SLICE_CELL_COUNT;
                if (WorldLayout::Contains(region, x, y, z))
                    cells[index] = region.cell;
            }
        }

        return cells;
    }

    std::vector<reaction::RxCell> MakeBlankProbeWorld() {
        return std::vector<reaction::RxCell>(PROBE_CELL_COUNT, reaction::RxMakeEmptyCell(0));
    }

    void SetLayoutRegions(const WorldLayout& layout, ProbeSimOptions& options) {
        options.gas = {};
        options.liquid = {};
        if (layout.gasRegion) {
            options.gas.minCell = layout.gasRegion->minCell;
            options.gas.sizeCells = layout.gasRegion->sizeCells;
            options.gas.boundary = layout.gasRegion->boundary;
        }

        if (layout.liquidRegion) {
            options.liquid.minCell = layout.liquidRegion->minCell;
            options.liquid.sizeCells = layout.liquidRegion->sizeCells;
            options.liquid.advection = layout.liquidRegion->advection;
        }
    }

    std::array<uint32_t, PROBE_VIEW_SPECIES_COUNT> ProbeViewSpecies(const BakedReactionTable& table) {
        static_assert(PROBE_VIEW_SPECIES_COUNT == VIEW_QUANTITY_SLOT_COUNT);
        const auto& slots = table.templates.viewQuantities;

        return {slots[0].species, slots[1].species, slots[2].species};
    }

    // --- CPU リファレンス ---

    namespace {

        // ブロック block と 6 面の隣(格子の中)に印を付ける
        void MarkWithNeighbors(std::vector<uint8_t>& scheduled, uint32_t block) {
            constexpr auto AXIS = static_cast<int32_t>(PROBE_BLOCKS_PER_AXIS);
            constexpr std::array<std::array<int32_t, 3>, 7> OFFSETS = {
                {{0, 0, 0}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}}};
            const auto x = static_cast<int32_t>(block % PROBE_BLOCKS_PER_AXIS);
            const auto y = static_cast<int32_t>((block / PROBE_BLOCKS_PER_AXIS) % PROBE_BLOCKS_PER_AXIS);
            const auto z = static_cast<int32_t>(block / (PROBE_BLOCKS_PER_AXIS * PROBE_BLOCKS_PER_AXIS));

            for (const auto& offset : OFFSETS) {
                const int32_t nx = x + offset[0];
                const int32_t ny = y + offset[1];
                const int32_t nz = z + offset[2];
                if (nx < 0 || ny < 0 || nz < 0 || nx >= AXIS || ny >= AXIS || nz >= AXIS)
                    continue;

                scheduled[ProbeBlockIndex(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny),
                                          static_cast<uint32_t>(nz))] = 1;
            }
        }

        // seeds(ブロックごとの 0/1)のブロックと 6 面の隣(GPU の WakeBlocks が予定するブロック)
        std::vector<uint8_t> ScheduleBlocks(std::span<const uint8_t> seeds) {
            std::vector<uint8_t> scheduled(PROBE_BLOCK_COUNT, 0);
            for (uint32_t block = 0; block < PROBE_BLOCK_COUNT; ++block) {
                if (seeds[block] != 0)
                    MarkWithNeighbors(scheduled, block);
            }

            return scheduled;
        }

        // 前の刻みに計算しなかった(GPU では眠っていた)のに、この刻みに評価が要るブロック = GPU の WakeDueBlocks が起こすブロックの数
        uint32_t CountWokenBlocks(std::span<const uint8_t> flags, std::span<const uint8_t> previousScheduled) {
            uint32_t woken = 0;
            for (uint32_t block = 0; block < PROBE_BLOCK_COUNT; ++block) {
                if ((flags[block] & PROBE_BLOCK_FLAG_POSSIBLE) != 0 && previousScheduled[block] == 0)
                    ++woken;
            }

            return woken;
        }

        // 1 世代のセルとキャッシュ
        struct ReferenceGeneration {
            reaction::RxCell* cells;
            reaction::HcThermalCache* caches;
        };

        // 置くコマンド(T-0222)を current に当てる(probe_tick.hlsl の ApplyPlace と同じ順と範囲)。変わったエネルギーを返す。
        // 触ったブロック(形を囲む箱が重なるブロック全部)は予定の種にし、「刻みの直前に変わった」にする
        uint64_t ApplyPlace(const ReactionTableView& table, uint32_t speciesCount, ReferenceGeneration current,
                            uint64_t tick, const ProbeCommand& command, std::vector<uint8_t>& seeds,
                            std::vector<uint64_t>& changedMarks) {
            const ProbePlace place = DecodePlaceCommand(command, speciesCount);
            if (place.valid == 0)
                return 0;

            // --- セル ---
            int64_t source = 0;
            for (uint32_t z = ProbePlaceBegin(place.z, place.extentZ); z < ProbePlaceEnd(place.z, place.extentZ); ++z) {
                for (uint32_t y = ProbePlaceBegin(place.y, place.extentY); y < ProbePlaceEnd(place.y, place.extentY);
                     ++y) {
                    for (uint32_t x = ProbePlaceBegin(place.x, place.extentX);
                         x < ProbePlaceEnd(place.x, place.extentX); ++x) {
                        if (!ProbePlaceCovers(place, x, y, z))
                            continue;

                        const uint32_t index = ProbeCellIndex(x, y, z);
                        const ProbePlaced placed = ProbePlaceCell(table, current.cells[index], place);
                        if (placed.applied == 0)
                            continue;

                        source += placed.cell.energy - current.cells[index].energy;
                        current.cells[index] = placed.cell;
                        current.caches[index] = ProbeMakeCache(table, placed.cell);
                    }
                }
            }

            // --- ブロック ---
            for (const uint32_t block : ProbePlaceBlocks(place)) {
                seeds[block] = 1;
                changedMarks[block] = ProbeChangeMark(tick) - 1;
            }

            return static_cast<uint64_t>(source);
        }

        // 刻み tick のつつきと置くコマンドを current に適用し、触ったブロックに印を付ける(GPU と同じく並びの順に)。
        // 足したエネルギーを返す。つついたブロックは「刻みの直前に変わった」(tc = 刻み tick の印 − 1。probe_tick.hlsl の ApplyCommand と同じ)
        uint64_t ApplyPokes(const ReactionTableView& table, uint32_t speciesCount, ReferenceGeneration current,
                            uint64_t tick, std::span<const ProbeCommand> commands, std::vector<uint8_t>& seeds,
                            std::vector<uint64_t>& changedMarks) {
            uint64_t source = 0;
            for (const ProbeCommand& command : commands) {
                if (command.targetTick == tick && command.type == PROBE_COMMAND_TYPE_PLACE)
                    source += ApplyPlace(table, speciesCount, current, tick, command, seeds, changedMarks);

                if (command.targetTick != tick || command.type != PROBE_COMMAND_TYPE_POKE)
                    continue;

                const uint32_t x = command.payload[0];
                const uint32_t y = command.payload[1];
                const uint32_t z = command.payload[2];
                if (x >= PROBE_GRID_SIZE || y >= PROBE_GRID_SIZE || z >= PROBE_GRID_SIZE)
                    continue;

                const uint32_t index = ProbeCellIndex(x, y, z);
                const int64_t energy = ProbePokeEnergy(table, current.cells[index]);
                current.cells[index].energy += energy;
                current.caches[index] = ProbeMakeCache(table, current.cells[index]);
                source += static_cast<uint64_t>(energy);
                seeds[ProbeBlockOfCell(x, y, z)] = 1;
                changedMarks[ProbeBlockOfCell(x, y, z)] = ProbeChangeMark(tick) - 1;
            }

            return source;
        }

        // 1 セルの伝導と反応(GPU の ConductBlock と同じ。格子の外の面は自分を渡す = 断熱)
        ProbeCellStep ReferenceStepCell(const ReactionTableView& table, ReferenceGeneration current, uint32_t index,
                                        uint64_t tick, uint64_t changedMark) {
            const uint32_t x = index % PROBE_GRID_SIZE;
            const uint32_t y = (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE;
            const uint32_t z = index / PROBE_SLICE_CELL_COUNT;
            const reaction::HcThermalCache self = current.caches[index];
            const auto at = [&](bool inside, uint32_t nx, uint32_t ny, uint32_t nz) {
                return inside ? current.caches[ProbeCellIndex(nx, ny, nz)] : self;
            };

            constexpr uint32_t LAST = PROBE_GRID_SIZE - 1;

            return ProbeStepCell(table, current.cells[index], self, at(x > 0, x - 1, y, z), at(x < LAST, x + 1, y, z),
                                 at(y > 0, x, y - 1, z), at(y < LAST, x, y + 1, z), at(z > 0, x, y, z - 1),
                                 at(z < LAST, x, y, z + 1), tick, changedMark, index);
        }

        // 上限の印(RxWaitStep::limits)を数える(GPU の ConductBlock の CountLimits と同じ)
        void AddLimits(ProbeLimitCounts& counts, uint32_t limits) {
            counts.productsHeld += (limits & reaction::RX_LIMIT_PRODUCTS) != 0 ? 1 : 0;
            counts.candidatesLimited += (limits & reaction::RX_LIMIT_CANDIDATES) != 0 ? 1 : 0;
        }

        uint32_t BlockOfCellIndex(uint32_t index) {
            return ProbeBlockOfCell(index % PROBE_GRID_SIZE, (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE,
                                    index / PROBE_SLICE_CELL_COUNT);
        }

        // 物理の熱の入口(T-0542・common/physics_heat.hlsli)を、入口の位置のセルのエネルギーへ足す。つついたセルと同じく、
        // ブロックを予定の種にして「刻みの直前に変わった」にする(足す順番に依らない: 整数の和と、足した後のセルから作るキャッシュ)
        int64_t ApplyPhysicsHeat(const ReactionTableView& table, ReferenceGeneration current, uint64_t tick,
                                 std::span<const physics::PxHeatEntry> heat, std::vector<uint8_t>& seeds,
                                 std::vector<uint64_t>& changedMarks) {
            int64_t added = 0;
            for (const physics::PxHeatEntry& entry : heat) {
                if (entry.milliJoules <= 0)
                    continue;

                const uint32_t index = ProbeCellOfPhysicsPoint(entry.position.x, entry.position.y, entry.position.z);
                current.cells[index].energy += entry.milliJoules;
                current.caches[index] = ProbeMakeCache(table, current.cells[index]);
                added += entry.milliJoules;

                const uint32_t block = BlockOfCellIndex(index);
                seeds[block] = 1;
                changedMarks[block] = ProbeChangeMark(tick) - 1;
            }

            return added;
        }

    }  // namespace

    std::vector<uint64_t> ProbeInitialBlockWakes(const BakedReactionTable& table,
                                                 std::span<const reaction::RxCell> cells) {
        FX_ASSERT(cells.size() == PROBE_CELL_COUNT);

        // 刻み 0 を tc = 0 で全部計算してみる(結果は捨てる)。変わるセルがあるブロックは刻み 0 に計算が要る(印 = 刻み 0 の印)。
        // 変わらないブロックは、刻み 0 を計算しなくても同じなので、セルが返す次に評価の要る刻みの最小
        const ReactionTableView view = table.View();
        std::vector<reaction::RxCell> trialCells(cells.begin(), cells.end());
        std::vector<reaction::HcThermalCache> caches;
        caches.reserve(cells.size());
        for (const reaction::RxCell& cell : cells)
            caches.push_back(ProbeMakeCache(view, cell));

        const ReferenceGeneration initial{.cells = trialCells.data(), .caches = caches.data()};
        std::vector<uint64_t> wakes(PROBE_BLOCK_COUNT, reaction::RX_WAIT_NEVER);
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            const ProbeCellStep step = ReferenceStepCell(view, initial, index, 0, 0);
            uint64_t& wake = wakes[BlockOfCellIndex(index)];
            wake = std::min(wake, step.changed != 0 ? ProbeChangeMark(0) : step.wakeTick);
        }

        return wakes;
    }

    std::vector<uint32_t> ProbeInitialScheduleWords(const BakedReactionTable& table,
                                                    std::span<const reaction::RxCell> cells) {
        std::vector<uint32_t> words(PROBE_SCHEDULE_BYTES / 4, 0);
        const std::vector<uint64_t> wakes = ProbeInitialBlockWakes(table, cells);
        for (uint32_t block = 0; block < PROBE_BLOCK_COUNT; ++block) {
            const uint32_t word = PROBE_SCHEDULE_WAKE_WORD + (block * 2);
            words[word] = static_cast<uint32_t>(wakes[block]);
            words[word + 1] = static_cast<uint32_t>(wakes[block] >> 32);
        }

        return words;
    }

    ProbeReference::ProbeReference(const BakedReactionTable& table, std::span<const reaction::RxCell> initialWorld)
        : m_table(&table),
          m_blockFlags(PROBE_BLOCK_COUNT, 0),
          m_changedMarks(PROBE_BLOCK_COUNT, 0),
          m_scheduled(PROBE_BLOCK_COUNT, 0) {
        FX_ASSERT(initialWorld.empty() || initialWorld.size() == PROBE_CELL_COUNT);
        const std::vector<reaction::RxCell> initial = initialWorld.empty()
                                                          ? MakeProbeInitialWorld(table)
                                                          : std::vector<reaction::RxCell>(initialWorld.begin(),
                                                                                          initialWorld.end());

        // 刻み 0 の予定の種: 初めの起こす刻みが刻み 0 の印以下のブロック(GPU の WakeDueBlocks と同じ)
        const std::vector<uint64_t> wakes = ProbeInitialBlockWakes(table, initial);
        for (uint32_t block = 0; block < PROBE_BLOCK_COUNT; ++block) {
            if (wakes[block] <= ProbeChangeMark(0))
                m_blockFlags[block] = static_cast<uint8_t>(PROBE_BLOCK_FLAG_POSSIBLE);
        }

        m_cells.reserve(size_t{PROBE_CELL_COUNT} * 2);
        m_cells.insert(m_cells.end(), initial.begin(), initial.end());
        m_cells.insert(m_cells.end(), initial.begin(), initial.end());

        m_caches.reserve(m_cells.size());
        for (const reaction::RxCell& cell : m_cells)
            m_caches.push_back(ProbeMakeCache(table.View(), cell));

        m_environmentMk = ProbeCascadeReferenceMk({}, table);  // 放射の段の外の温度(D-671。作った時の表)
    }

    void ProbeReference::Advance(uint64_t tick, std::span<const ProbeCommand> commands,
                                 const BakedReactionTable* newTable, std::span<const physics::PxHeatEntry> heat) {
        if (newTable != nullptr) {
            RemapSpecies(tick, *newTable);
            m_table = newTable;
        }

        const ReactionTableView table = m_table->View();
        const size_t currentBase = static_cast<size_t>(tick & 1) * PROBE_CELL_COUNT;
        const size_t nextBase = static_cast<size_t>((tick + 1) & 1) * PROBE_CELL_COUNT;
        const ReferenceGeneration current{.cells = m_cells.data() + currentBase,
                                          .caches = m_caches.data() + currentBase};

        // (1) コマンドの適用。つついたブロックは、前の刻みで変わった・次の刻みに評価の要るブロックと同じく予定の種になる
        std::vector<uint8_t> seeds = m_blockFlags;
        const auto speciesCount = static_cast<uint32_t>(m_table->species.size());
        m_sourceEnergy = ApplyPokes(table, speciesCount, current, tick, commands, seeds, m_changedMarks);
        m_physicsHeatEnergy = ApplyPhysicsHeat(table, current, tick, heat, seeds, m_changedMarks);

        // 表を差し替えた刻み(GPU の RefreshTable と同じ。T-0139): 2 世代の熱のキャッシュを新しい表で作り直し、
        // 全部のブロックを「刻みの直前に変わった」にして起こす(速さ f が変わるので待ちを引き直す。ADR-0018)
        if (newTable != nullptr) {
            for (size_t index = 0; index < m_cells.size(); ++index)
                m_caches[index] = ProbeMakeCache(table, m_cells[index]);

            rng::fill(m_changedMarks, ProbeChangeMark(tick) - 1);
            rng::fill(seeds, uint8_t{1});
        }
        // (1b) 気体(区画があるときだけ。T-0590・ADR-0256): 伝導と反応の前に流す(06 §2 の段 3 → 段 4)。
        //      値が変わったセルは熱のキャッシュを作り直し、そのブロックを予定の種にする(つついたセルと同じ)
        if (m_gas) {
            std::vector<uint32_t> gasChanged;
            m_gasReport = m_gas->Step(*m_table, std::span(m_cells).subspan(currentBase, PROBE_CELL_COUNT), tick,
                                      gasChanged);
            // 最速 × 小さい空き(T-0788・D-519): 新しく見つかった組を作り手へ警告する(値はそのまま流す。D-487 ②)
            for (const std::string& warning : m_gasReport.warnings)
                Log(Channel::Sim, Level::Warning, "刻み {}: {}", tick, warning);

            // 熱の列で縮めた(T-0787・ADR-0355。D-508 の安全網が効いた = 1 小刻みに熱を持ち出しきるほど強い膨張)
            if (m_gasReport.energyLimitedFaces != 0)
                Log(Channel::Sim, Level::Warning,
                    "刻み {} の気体: 風上の熱を超えて持ち出す面を {} 面縮めた(陽解法の安全網。強い爆発の目安)", tick,
                    m_gasReport.energyLimitedFaces);
            for (const uint32_t index : gasChanged) {
                m_caches[currentBase + index] = ProbeMakeCache(table, m_cells[currentBase + index]);
                const uint32_t block = BlockOfCellIndex(index);
                seeds[block] = 1;
                m_changedMarks[block] = ProbeChangeMark(tick) - 1;
            }
        }

        // (1c) 液体(区画があるときだけ。T-0609・ADR-0266): 気体の後・伝導と反応の前。変わったセルの扱いは気体と同じ
        if (m_liquid) {
            std::vector<uint32_t> liquidChanged;
            m_liquidReport = m_liquid->Step(*m_table, std::span(m_cells).subspan(currentBase, PROBE_CELL_COUNT), tick,
                                            liquidChanged);
            for (const uint32_t index : liquidChanged) {
                m_caches[currentBase + index] = ProbeMakeCache(table, m_cells[currentBase + index]);
                const uint32_t block = BlockOfCellIndex(index);
                seeds[block] = 1;
                m_changedMarks[block] = ProbeChangeMark(tick) - 1;
            }
        }

        // (1d) 放射(段を入れた時だけ。T-0585・ADR-0263): 液体の後・伝導と反応の前(段の順: コマンド → 物理の熱 → 気体 → 液体 → 放射 → 伝導と反応)
        m_cascadeReport = {};
        if (m_radiation)
            StepRadiation(tick, currentBase, seeds);
        else if (m_cascade.enabled)
            StepCascadeRadiation(tick, currentBase, seeds);

        m_wokenBlocks = CountWokenBlocks(m_blockFlags, m_scheduled);
        m_scheduled = ScheduleBlocks(seeds);
        m_scheduledBlocks = static_cast<uint32_t>(rng::count(m_scheduled, uint8_t{1}));

        // (2) 伝導と反応(全部のセル。tc は刻みの初めの値)と、ブロックごとの変わったか・次に評価の要る刻みの最小
        //     上限の印は GPU が計算するブロックのセルだけ数える(眠っているブロックは GPU が評価しない。T-0177)
        std::vector<uint8_t> changed(PROBE_BLOCK_COUNT, 0);
        std::vector<uint64_t> wakes(PROBE_BLOCK_COUNT, reaction::RX_WAIT_NEVER);
        m_limits = {};
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            const uint32_t block = BlockOfCellIndex(index);
            const ProbeCellStep step = ReferenceStepCell(table, current, index, tick, m_changedMarks[block]);
            m_cells[nextBase + index] = step.cell;
            m_caches[nextBase + index] = step.cache;

            changed[block] |= static_cast<uint8_t>(step.changed);
            wakes[block] = std::min(wakes[block], step.wakeTick);
            if (m_scheduled[block] != 0)
                AddLimits(m_limits, step.limits);
        }

        // (3) 次の刻みの予定の種(GPU の ConductBlock と同じ。眠っているブロックの起こす刻みは、計算し直しても同じ値になる)
        for (uint32_t block = 0; block < PROBE_BLOCK_COUNT; ++block) {
            if (changed[block] != 0)
                m_changedMarks[block] = ProbeChangeMark(tick);

            const bool possible = wakes[block] <= ProbeChangeMark(tick + 1);
            m_blockFlags[block] = static_cast<uint8_t>((changed[block] != 0 ? PROBE_BLOCK_FLAG_CHANGED : 0) |
                                                       (possible ? PROBE_BLOCK_FLAG_POSSIBLE : 0));
        }
    }

    std::expected<void, std::string> ProbeReference::EnableGas(const ProbeGasOptions& options) {
        auto field = ProbeGasField::Create(*m_table, options);
        if (!field)
            return std::unexpected(field.error());

        m_gas = std::move(*field);

        return {};
    }

    // 放射の 1 刻み(T-0585・ADR-0263)。セルの吸収率 → 線の束で対(sim/radiation_lines)→ 対ごとの整数の交換(sim/radiation_reference)。
    // 温度は刻みの初め(熱のキャッシュ)なので、セルの並びと対の並びによらない。変わったセルは熱のキャッシュを作り直す
    void ProbeReference::StepRadiation(uint64_t tick, size_t currentBase, std::vector<uint8_t>& seeds) {
        m_radiationReport = {};
        if (rng::none_of(m_table->speciesRadiationAbsorption, [](uint32_t value) { return value != 0; }))
            return;

        const ReactionTableView table = m_table->View();
        std::vector<uint32_t> absorptance(PROBE_CELL_COUNT, 0);
        std::vector<RadiationSurface> surfaces(PROBE_CELL_COUNT);
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            const reaction::RxCell& cell = m_cells[currentBase + index];
            absorptance[index] = CellAbsorptanceQ16(*m_table, cell);
            if (absorptance[index] == 0)
                continue;

            ++m_radiationReport.absorbingCells;
            surfaces[index] = {.temperatureMk = m_caches[currentBase + index].temperature,
                               .heatCapacityNanojoulesPerKelvin = reaction::RxHeatCapacity(table, cell),
                               .absorptanceQ16 = absorptance[index]};
        }

        if (m_radiationReport.absorbingCells == 0)
            return;

        const RadiationGrid grid{.sizeX = PROBE_GRID_SIZE, .sizeY = PROBE_GRID_SIZE, .sizeZ = PROBE_GRID_SIZE};
        const std::vector<RadiationPair> pairs = FindRadiationPairs(grid, absorptance, tick, m_radiation->seed);
        m_radiationReport.pairs = pairs.size();

        const std::vector<int64_t> changes = ExchangeRadiation(surfaces, pairs);
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            if (changes[index] == 0)
                continue;

            reaction::RxCell& cell = m_cells[currentBase + index];
            cell.energy += changes[index];
            m_caches[currentBase + index] = ProbeMakeCache(table, cell);
            ++m_radiationReport.changedCells;
            if (changes[index] < 0)
                m_radiationReport.movedMilliJoules += static_cast<uint64_t>(-changes[index]);

            const uint32_t block = BlockOfCellIndex(index);
            seeds[block] = 1;
            m_changedMarks[block] = ProbeChangeMark(tick) - 1;
        }
    }

    std::expected<void, std::string> ProbeReference::EnableLiquid(const ProbeLiquidOptions& options) {
        auto field = ProbeLiquidField::Create(*m_table, options);
        if (!field)
            return std::unexpected(field.error());

        m_liquid = std::move(*field);

        return {};
    }

    // 表の差し替え: 区画の欄を新しい表に合わせ(T-0670)、物質の一覧が変わるなら(T-0223・ADR-0065)2 世代のセルを名前で付け替える
    // (熱のキャッシュは Advance が新しい表で作り直す)。
    // 報告は刻み tick の始めの世代(この刻みが進める世代)の分だけ数える
    void ProbeReference::RemapSpecies(uint64_t tick, const BakedReactionTable& newTable) {
        m_remapReport = {};
        const auto remap = BuildSpeciesRemap(*m_table, newTable);
        FX_ASSERT(remap.has_value());  // 呼ぶ側の約束(script::CheckHotReloadCompatible が通した表)
        if (!remap)
            return;

        // 区画の欄を新しい表に合わせる(T-0670。GPU の RecordRegionTableSwap と同じ)
        if (m_gas)
            m_gas->ApplyTable(newTable, *remap);

        if (m_liquid)
            m_liquid->ApplyTable(newTable);

        if (remap->identity)
            return;

        const size_t currentBase = static_cast<size_t>(tick & 1) * PROBE_CELL_COUNT;
        for (size_t index = 0; index < m_cells.size(); ++index) {
            const SpeciesRemapReport report = RemapReactionCell(*remap, newTable, m_cells[index]);
            if (index >= currentBase && index < currentBase + PROBE_CELL_COUNT)
                m_remapReport.Add(report);
        }
    }

    std::span<const reaction::RxCell> ProbeReference::State(uint64_t tick) const {
        return std::span(m_cells).subspan(static_cast<size_t>(tick & 1) * PROBE_CELL_COUNT, PROBE_CELL_COUNT);
    }

    // 区画の欄は 1 組だけ持つので、最後の Advance の次の刻みの値(GPU のハッシュの単位と同じ。T-0620)
    uint64_t ProbeReference::StateHash(uint64_t tick) const {
        uint64_t hash = ProbeStateHash(State(tick));
        if (m_gas)
            hash += m_gas->HashTerm();

        if (m_liquid)
            hash += m_liquid->HashTerm();

        // 放射の持ち越し(D-619)。(セル・面・帯)ごとの寄与の和なので、GPU(RadiationHash)が順不同に足しても同じ値。
        // 持ち越しが無い世界(放射の段の無い世界)は今までと同じ値
        for (const CascadeCarryEntry& entry : m_cascadeCarry.entries) {
            if (entry.amount != 0)
                hash += ProbeCascadeCarryHashTerm(entry.cell, entry.face, entry.band, entry.amount);
        }

        // 負の側(参照の温度の形だけ。面の番号に 8 を足して正の側と分ける)
        for (const CascadeCarryEntry& entry : m_cascadeCarryNegative.entries) {
            if (entry.amount != 0)
                hash += ProbeCascadeCarryHashTerm(entry.cell, entry.face | 8u, entry.band, entry.amount);
        }

        return hash;
    }

    uint64_t ProbeStateHash(std::span<const reaction::RxCell> cells) {
        uint64_t hash = 0;
        for (uint32_t index = 0; index < cells.size(); ++index)
            hash += ProbeCellHash(index, cells[index]);

        return hash;
    }

    std::span<const reaction::HcThermalCache> ProbeReference::Caches(uint64_t tick) const {
        return std::span(m_caches).subspan(static_cast<size_t>(tick & 1) * PROBE_CELL_COUNT, PROBE_CELL_COUNT);
    }

    std::vector<uint32_t> MakeProbeExtractionCells(std::span<const reaction::RxCell> cells,
                                                   std::span<const reaction::HcThermalCache> caches,
                                                   const std::array<uint32_t, PROBE_VIEW_SPECIES_COUNT>& viewSpecies) {
        std::vector<uint32_t> words;
        words.reserve(cells.size() * PROBE_EXTRACTION_CELL_WORDS);
        for (size_t index = 0; index < cells.size(); ++index) {
            words.push_back(caches[index].temperature);
            for (const uint32_t species : viewSpecies)
                words.push_back(ProbeViewAmount(cells[index], species));
        }

        return words;
    }

    uint64_t ProbeExtractionHash(std::span<const uint32_t> words) {
        uint64_t hash = 0;
        for (const uint32_t word : words)
            hash = fx::FxHashCombine(hash, word);

        return hash;
    }

    uint64_t ProbeEnergySum(std::span<const reaction::RxCell> cells) {
        uint64_t energy = 0;
        for (const reaction::RxCell& cell : cells)
            energy += static_cast<uint64_t>(cell.energy);

        return energy;
    }

}  // namespace bicameral::sim
