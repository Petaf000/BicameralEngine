// gpu_object_heat_stages.h — 物の熱の GPU の段(shaders/sim/object_heat.hlsl)を試験から回す道具(T-1064・T-1077。gpu_object_heat_test.cpp から分けた)。
// データの流れ: CPU の物の熱の世界(object_heat_cpu.h の OhWorld)→ CreateGpuWorld・UploadWorld で既定のヒープへ → RecordTick で 1 刻みの段を記録
//   → ReadUav・CompareWorld で読み戻して CPU と比べる。使うのは tests/gpu_object_heat_test.cpp(平らなセル)と
//   tests/gpu_object_heat_nest_test.cpp(多重解像度の世界のセル。T-1077)。
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "core/log.h"
#include "gpu/com_ptr.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "object_heat_cpu.h"
#include "sim/reaction_table.h"

namespace bicameral::test::object_heat_gpu {

    namespace rx = bicameral::reaction;

    constexpr uint32_t THREADS = 64;
    constexpr uint32_t ROW_GROUPS = 1024;  // グループの数が 65535 を超える時の x の数
    constexpr uint32_t ROOT_CONSTANT_COUNT = 13;
    constexpr uint32_t SRV_COUNT = 11;
    constexpr uint32_t TIMESTAMP_COUNT = 7;  // 刻みの初め + 6 つの段の後

    // u0〜u9 / b0 / デバッグのリング / t0〜t10(object_heat.hlsl)
    constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
        .uavCount = 10, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = SRV_COUNT};

    enum Stage : uint8_t {
        StageWake,
        StagePrepare,
        StageFlow,
        StageReduce,
        StageApplyCells,
        StageReact,
        STAGE_COUNT,
    };

    struct Pipelines {
        ComPtr<ID3D12RootSignature> rootSignature;
        std::array<ComPtr<ID3D12PipelineState>, STAGE_COUNT> stages;
    };

    inline std::expected<Pipelines, std::string> CreatePipelines(ID3D12Device5* device) {
        Pipelines result;
        result.rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!result.rootSignature)
            return std::unexpected("ルート署名を作れない");

        const std::array<const char*, STAGE_COUNT> names = {
            "sim/object_heat_wake.cso",   "sim/object_heat_prepare.cso",     "sim/object_heat_flow.cso",
            "sim/object_heat_reduce.cso", "sim/object_heat_apply_cells.cso", "sim/object_heat_react.cso"};
        for (size_t i = 0; i < names.size(); ++i) {
            const auto bytecode = gpu::LoadShader(names[i]);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            result.stages[i] = gpu::CreateComputePipeline(device, result.rootSignature.Get(), *bytecode);
            if (!result.stages[i])
                return std::unexpected(std::format("パイプラインを作れない: {}", names[i]));
        }

        return result;
    }

    // GPU の世界(既定のヒープ)。u の番号は object_heat.hlsl の並び
    enum Uav : uint8_t {
        UavVoxels,
        UavHeats,
        UavFlows,
        UavCarries,
        UavCoarse,
        UavCells,
        UavFailures,
        UavBlockStates,
        UavWaits,
        UavCellChanged,
        UAV_COUNT,
    };

    struct GpuWorld {
        std::array<ComPtr<ID3D12Resource>, UAV_COUNT> uavs;
        std::array<ComPtr<ID3D12Resource>, UAV_COUNT> uavUploads;  // 写しの元(初めの値)
        std::array<gpu::ResidentBuffer, SRV_COUNT> tables;
        ComPtr<ID3D12QueryHeap> timestamps;
        ComPtr<ID3D12Resource> timestampReadback;
        uint32_t voxelCount = 0;
        uint32_t objectCount = 0;
        uint32_t rangeCount = 0;
        uint32_t blockCount = 0;
        uint32_t passesPerTick = 2;  // 小刻みの段 + 反応の段(CPU の world.pass と同じ数え方)
        bool sleeping = true;
        uint64_t worldSeed = 0;  // 世界の種(反応の乱数)
    };

    template <typename T>
    inline std::span<const std::byte> BytesOf(const std::vector<T>& values) {
        return std::as_bytes(std::span(values));
    }

    inline std::expected<GpuWorld, std::string> CreateGpuWorld(ID3D12Device5* device,
                                                               const sim::BakedReactionTable& table,
                                                               const test::OhTables& tables, const test::OhWorld& world,
                                                               uint64_t worldSeed) {
        GpuWorld result;
        result.voxelCount = static_cast<uint32_t>(world.voxels.size());
        result.objectCount = static_cast<uint32_t>(world.objects.size());
        result.rangeCount = static_cast<uint32_t>(world.ranges.size());
        result.blockCount = static_cast<uint32_t>(world.blocks.size());
        result.sleeping = world.sleeping;
        result.worldSeed = worldSeed;
        for (const rx::OhObject& object : world.objects)
            result.passesPerTick = std::max(result.passesPerTick, object.substeps + 1);

        const std::vector<rx::OhFlow> coarse(world.objects.size(), rx::OhZeroFlow());
        const std::vector<uint32_t> failures(4, 0);
        const std::array<std::span<const std::byte>, UAV_COUNT> initial = {
            BytesOf(world.voxels), BytesOf(world.heats),
            BytesOf(world.flows),  BytesOf(world.carries),
            BytesOf(coarse),       BytesOf(world.cells),
            BytesOf(failures),     BytesOf(world.blockStates),
            BytesOf(world.waits),  BytesOf(world.cellChangedPasses)};
        for (size_t i = 0; i < initial.size(); ++i) {
            result.uavs[i] = gpu::CreateBuffer(device, initial[i].size(), gpu::BufferKind::UnorderedAccess);
            result.uavUploads[i] = gpu::CreateFilledUpload(device, initial[i]);
            if (!result.uavs[i] || !result.uavUploads[i])
                return std::unexpected("世界のバッファを作れない");
        }

        const std::vector<uint32_t> gasSpecies(tables.gases.species.begin(), tables.gases.species.end());
        const std::array<std::span<const std::byte>, SRV_COUNT> srvs = {
            BytesOf(table.species),  BytesOf(table.rules),   BytesOf(table.ruleIndex), BytesOf(table.rates),
            BytesOf(tables.species), BytesOf(world.objects), BytesOf(world.infos),     BytesOf(world.light),
            BytesOf(world.ranges),   BytesOf(gasSpecies),    BytesOf(world.blocks)};
        for (size_t i = 0; i < srvs.size(); ++i) {
            result.tables[i] = gpu::CreateResidentBuffer(device, srvs[i]);
            if (!result.tables[i].Valid())
                return std::unexpected("表のバッファを作れない");
        }

        result.timestampReadback = gpu::CreateBuffer(device, TIMESTAMP_COUNT * sizeof(uint64_t),
                                                     gpu::BufferKind::Readback);
        const D3D12_QUERY_HEAP_DESC heapDesc{
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = TIMESTAMP_COUNT, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&result.timestamps))) || !result.timestampReadback)
            return std::unexpected("タイムスタンプを作れない");

        return result;
    }

    // 初めの値を既定のヒープへ写す(リストの終わりで COMMON に戻り、次のリストで UAV / SRV へ暗黙に上がる)
    inline bool UploadWorld(gpu::ImmediateQueue& queue, const GpuWorld& world) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return false;

        for (size_t i = 0; i < world.uavs.size(); ++i)
            list->CopyResource(world.uavs[i].Get(), world.uavUploads[i].Get());

        gpu::RecordResidentUploads(list, world.tables);

        return queue.ExecuteAndWait();
    }

    // スレッドかグループの並び(65535 を超える時は y に折る)
    struct Grid {
        uint32_t groupsX;
        uint32_t groupsY;
    };

    inline Grid GroupsFor(uint32_t groups) {
        if (groups <= 65535)
            return {std::max(groups, 1u), 1};

        return {ROW_GROUPS, (groups + ROW_GROUPS - 1) / ROW_GROUPS};
    }

    inline Grid ThreadsFor(uint32_t threads) {
        return GroupsFor((threads + THREADS - 1) / THREADS);
    }

    inline void Barrier(ID3D12GraphicsCommandList10* list) {
        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
    }

    // 段の番号(CPU の world.pass と同じ: 刻み t の小刻み s は (t − 1) × 段の数 + s + 1、反応は t × 段の数)
    inline uint32_t PassOf(const GpuWorld& world, uint64_t tick, uint32_t substep, bool react) {
        const auto base = static_cast<uint32_t>((tick - 1) * world.passesPerTick);

        return react ? base + world.passesPerTick : base + substep + 1;
    }

    inline void SetConstants(ID3D12GraphicsCommandList10* list, const GpuWorld& world, uint32_t substep, bool lightOn,
                             uint64_t tick, uint32_t rowThreads, uint32_t pass) {
        const std::array<uint32_t, ROOT_CONSTANT_COUNT> constants = {world.voxelCount,
                                                                     world.objectCount,
                                                                     world.rangeCount,
                                                                     substep,
                                                                     lightOn ? 1u : 0u,
                                                                     static_cast<uint32_t>(tick),
                                                                     static_cast<uint32_t>(tick >> 32),
                                                                     static_cast<uint32_t>(world.worldSeed),
                                                                     static_cast<uint32_t>(world.worldSeed >> 32),
                                                                     rowThreads,
                                                                     world.blockCount,
                                                                     pass,
                                                                     world.sleeping ? 1u : 0u};
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, constants.data(), 0);
    }

    // 1 段を記録する(後に UAV のバリア)
    inline void RecordStage(ID3D12GraphicsCommandList10* list, const Pipelines& pipelines, const GpuWorld& world,
                            Stage stage, bool lightOn, uint64_t tick, uint32_t substep) {
        Grid grid = GroupsFor(world.blockCount);
        if (stage == StageWake)
            grid = ThreadsFor(world.blockCount);
        else if (stage == StageReduce)
            grid = Grid{world.objectCount, 1};
        else if (stage == StageApplyCells)
            grid = ThreadsFor(world.rangeCount);

        SetConstants(list, world, substep, lightOn, tick, grid.groupsX * THREADS,
                     PassOf(world, tick, substep, stage == StageReact));
        list->SetPipelineState(pipelines.stages[stage].Get());
        list->Dispatch(grid.groupsX, grid.groupsY, 1);
        Barrier(list);
    }

    // 1 刻み(小刻みごとに 起こす・①〜④、最後に ⑤)を記録する。stamps なら段ごとにタイムスタンプ(1 + 段の番号。小刻み 1 の時だけ)
    inline void RecordTick(ID3D12GraphicsCommandList10* list, const Pipelines& pipelines, const GpuWorld& world,
                           bool lightOn, uint64_t tick, bool stamps) {
        for (uint32_t sub = 0; sub + 1 < world.passesPerTick; ++sub) {
            for (const Stage stage : {StageWake, StagePrepare, StageFlow, StageReduce, StageApplyCells}) {
                RecordStage(list, pipelines, world, stage, lightOn, tick, sub);
                if (stamps)
                    list->EndQuery(world.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1 + stage);
            }
        }

        RecordStage(list, pipelines, world, StageReact, lightOn, tick, 0);
        if (stamps)
            list->EndQuery(world.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1 + StageReact);
    }

    inline void BindRoot(ID3D12GraphicsCommandList10* list, const Pipelines& pipelines, const GpuWorld& world,
                         gpu::DebugRing& debugRing) {
        list->SetComputeRootSignature(pipelines.rootSignature.Get());
        for (uint32_t i = 0; i < world.uavs.size(); ++i)
            list->SetComputeRootUnorderedAccessView(i, world.uavs[i]->GetGPUVirtualAddress());

        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing.GpuAddress());
        for (uint32_t i = 0; i < world.tables.size(); ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i),
                                                   world.tables[i].buffer->GetGPUVirtualAddress());
    }

    // 刻み [first, first + count) を 1 本のリストで流す。stamps なら段ごとのタイムスタンプ(ms)を返す(1 刻みの時だけ意味がある)
    inline std::expected<std::array<double, STAGE_COUNT>, std::string> RunTicks(
        gpu::ImmediateQueue& queue, const Pipelines& pipelines, const GpuWorld& world, gpu::DebugRing& debugRing,
        uint64_t first, uint32_t count, bool lightOn, bool stamps) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        debugRing.RecordBegin(list);
        BindRoot(list, pipelines, world, debugRing);
        if (stamps)
            list->EndQuery(world.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

        for (uint32_t i = 0; i < count; ++i)
            RecordTick(list, pipelines, world, lightOn, first + i, stamps);

        if (stamps)
            list->ResolveQueryData(world.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, TIMESTAMP_COUNT,
                                   world.timestampReadback.Get(), 0);

        debugRing.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = debugRing.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        std::array<double, STAGE_COUNT> stages{};
        if (!stamps)
            return stages;

        std::array<uint64_t, TIMESTAMP_COUNT> times{};
        uint64_t frequency = 0;
        if (!gpu::ReadBuffer(world.timestampReadback.Get(), std::as_writable_bytes(std::span(times))) ||
            FAILED(queue.Native()->GetTimestampFrequency(&frequency)) || frequency == 0)
            return std::unexpected("タイムスタンプを読めない");

        for (size_t stage = 0; stage < STAGE_COUNT; ++stage)
            stages[stage] = static_cast<double>(times[stage + 1] - times[stage]) * 1000.0 /
                            static_cast<double>(frequency);

        return stages;
    }

    inline double Sum(const std::array<double, STAGE_COUNT>& stages) {
        double total = 0.0;
        for (const double value : stages)
            total += value;

        return total;
    }

    // UAV の中身を読み戻す(前のリストの終わりで COMMON に戻っているので、UAV に上げてから写す)
    template <typename T>
    inline std::expected<std::vector<T>, std::string> ReadUav(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                              ID3D12Resource* resource, size_t count) {
        const ComPtr<ID3D12Resource> readback = gpu::CreateBuffer(device, count * sizeof(T), gpu::BufferKind::Readback);
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (!readback || list == nullptr)
            return std::unexpected("読み戻しを作れない");

        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(resource, D3D12_RESOURCE_STATE_COMMON,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
        gpu::RecordCopyToReadback(list, resource, readback.Get());
        std::vector<T> values(count);
        if (!queue.ExecuteAndWait() || !gpu::ReadBuffer(readback.Get(), std::as_writable_bytes(std::span(values))))
            return std::unexpected("読み戻せない");

        return values;
    }

    template <typename T>
    inline size_t CountDifferent(std::span<const T> cpu, std::span<const T> gpuValues, const char* label) {
        size_t different = 0;
        for (size_t i = 0; i < cpu.size(); ++i) {
            if (std::memcmp(&cpu[i], &gpuValues[i], sizeof(T)) == 0)
                continue;

            if (different++ < 3) {
                std::array<int64_t, sizeof(T) / 8> a{};
                std::array<int64_t, sizeof(T) / 8> b{};
                std::memcpy(a.data(), &cpu[i], sizeof(a));
                std::memcpy(b.data(), &gpuValues[i], sizeof(b));
                Log(Channel::Gpu, Level::Error, "{} [{}]: cpu {} / gpu {}", label, i, a, b);
            }
        }

        if (different != 0)
            Log(Channel::Gpu, Level::Error, "{}: 食い違い {} / {}", label, different, cpu.size());

        return different;
    }

    // GPU の世界の真実(ボクセル・セル・対の端数・待ち・ブロックの状態)を CPU と比べる。食い違いの数
    inline std::expected<size_t, std::string> CompareWorld(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                           const GpuWorld& gpuWorld, const test::OhWorld& cpu) {
        const auto voxels = ReadUav<rx::RxCell>(device, queue, gpuWorld.uavs[UavVoxels].Get(), cpu.voxels.size());
        const auto cells = ReadUav<rx::RxCell>(device, queue, gpuWorld.uavs[UavCells].Get(), cpu.cells.size());
        const auto carries = ReadUav<rx::OhFlow>(device, queue, gpuWorld.uavs[UavCarries].Get(), cpu.carries.size());
        const auto waits = ReadUav<rx::OhVoxelWait>(device, queue, gpuWorld.uavs[UavWaits].Get(), cpu.waits.size());
        const auto states = ReadUav<rx::OhBlockState>(device, queue, gpuWorld.uavs[UavBlockStates].Get(),
                                                      cpu.blockStates.size());
        const auto failures = ReadUav<uint32_t>(device, queue, gpuWorld.uavs[UavFailures].Get(), 4);
        if (!voxels || !cells || !carries || !waits || !states || !failures)
            return std::unexpected("読み戻せない");

        return CountDifferent<rx::RxCell>(cpu.voxels, *voxels, "ボクセル") +
               CountDifferent<rx::RxCell>(cpu.cells, *cells, "セル") +
               CountDifferent<rx::OhFlow>(cpu.carries, *carries, "対の端数") +
               CountDifferent<rx::OhVoxelWait>(cpu.waits, *waits, "待ち") +
               CountDifferent<rx::OhBlockState>(cpu.blockStates, *states, "ブロックの状態") + (*failures)[0];
    }

}  // namespace bicameral::test::object_heat_gpu
