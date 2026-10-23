// gpu_object_heat_test.cpp — 物の熱(物の箱のボクセルの熱と反応・セルとの対の面の 4 つの口)の GPU 版を CPU リファレンスとビット一致で確かめ、
// 燃えている物の数を増やした時の 1 刻みの GPU の時間を測る(T-1064・T-1065・ADR-0434・20 §12.4)。
// 式は shaders/common/object_heat.hlsli、段は shaders/sim/object_heat.hlsl、CPU は tests/object_heat_cpu.h。
// 物は 3 種(箱 7.8 mm = レベル 6): A4 の紙(27 × 1 × 38・厚さ 0.1 mm を箱に薄めて両面が外)・小枝(2 × 2 × 38・Ø 1 cm の木を箱に薄めて)・
// 薪(10 × 10 × 51・Ø 8 cm × 40 cm を四角の箱で)。木は試験の表の cellulose を薄めた量。外に面するボクセルを 900 K から始める(燃えている物)。
//   既定: 紙 2・小枝 3(同じセル)・薪 1(別のセル)を 60 刻み、20 刻みごとにボクセル・セル・対の端数を読み戻して CPU と比べる。
//         CPU の側でエネルギーの保存(セル × 8^6 + ボクセル + 対の端数 − 入った光)も毎刻み確かめる。
//   --measure(--measure-only は CPU と比べずに測るだけ): 紙・小枝・薪をそれぞれ 10・100・1000 個(10 個ずつ同じセル)並べ、1 刻みの GPU の時間(タイムスタンプ)を測る(CPU とは比べない)。
// 引数は gpu_test_options.h と --measure。
#include <cstring>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "object_heat_cpu.h"
#include "sim/reaction_table.h"
#include "sun_column_scene.h"

using namespace bicameral;
namespace rx = bicameral::reaction;

namespace {

    // --- 試験の値(公開側の試験データ。ゲームの中身ではない。D-006)---
    constexpr int32_t BODY_LEVEL = 6;  // 箱 7.8 mm
    constexpr uint32_t FILM_UM = 3906;
    constexpr uint64_t FULL_CELLULOSE_UNITS = 1157000000;  // cellulose で満たした箱(sun_column_scene と同じ)
    constexpr uint64_t AIR_OXYGEN_UNITS = 1092000;
    constexpr uint64_t AIR_NITROGEN_UNITS = 4108000;
    constexpr uint32_t DEFAULT_DIFFUSIVITY_MM2_PER_S = 20;
    constexpr uint32_t GAS_CONDUCTIVITY_SCALE = 5000;  // 試験の表の伝導率の倍率を外す(object_heat_test と同じ)
    constexpr uint32_t SOLID_CONDUCTIVITY_SCALE = 20000;
    constexpr int32_t BURNING_MILLIKELVIN = 900000;
    constexpr int64_t SPOT_LIGHT = 10000000;  // 物の最初のボクセルが 1 刻みに吸う光(ボクセルの単位。約 2.3 W)
    constexpr uint64_t WORLD_SEED = 0x1064;
    constexpr uint32_t COMPARE_TICKS = 60;
    constexpr uint32_t COMPARE_EVERY = 20;
    constexpr uint32_t OBJECTS_PER_CELL = 10;
    constexpr uint32_t THREADS = 64;
    constexpr uint32_t ROW_GROUPS = 1024;  // グループの数が 65535 を超える時の x の数
    constexpr uint32_t ROOT_CONSTANT_COUNT = 10;

    // u0〜u6 / b0 / デバッグのリング / t0〜t9(object_heat.hlsl)
    constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
        .uavCount = 7, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 10};

    // --- 物の形 ---

    enum class Kind : uint8_t {
        Paper,
        Twig,
        Log,
    };

    struct KindShape {
        const char* name;
        uint32_t sizeX;
        uint32_t sizeY;
        uint32_t sizeZ;
        uint64_t celluloseUnits;  // 1 ボクセルの量(そのレベルの単位)
        bool sheet;               // 薄い紙: 外に面するのは両面だけ(試作と同じ)
    };

    KindShape ShapeOf(Kind kind) {
        if (kind == Kind::Paper)
            return {"A4 の紙", 27, 1, 38, (FULL_CELLULOSE_UNITS * 100 << BODY_LEVEL) / 500000, true};

        // 木 = cellulose の 40 %。小枝は Ø 1 cm の丸を 1.56 cm の四角に薄めて(32 %)
        if (kind == Kind::Twig)
            return {"小枝", 2, 2, 38, (FULL_CELLULOSE_UNITS * 128) / 1000, false};

        return {"薪", 10, 10, 51, (FULL_CELLULOSE_UNITS * 400) / 1000, false};
    }

    // ボクセルの形の語(隣のある方向と外に面する面の数)
    uint32_t VoxelShape(const KindShape& shape, uint32_t x, uint32_t y, uint32_t z) {
        const std::array<bool, 6> inside = {x > 0, x + 1 < shape.sizeX, y > 0, y + 1 < shape.sizeY,
                                            z > 0, z + 1 < shape.sizeZ};
        uint32_t neighbours = 0;
        uint32_t exposed = 0;
        for (uint32_t direction = 0; direction < 6; ++direction) {
            if (inside[direction])
                neighbours |= 1u << direction;
            else if (!shape.sheet || direction == 2 || direction == 3)
                ++exposed;
        }

        return neighbours | (exposed << rx::OH_EXPOSED_SHIFT);
    }

    // 物を 1 つ足す(セルの順に足すこと)。外に面するボクセルは燃えている温度から
    void AddObject(const sim::BakedReactionTable& table, test::OhWorld& world, Kind kind, uint32_t cell, bool spot) {
        const KindShape shape = ShapeOf(kind);
        const auto objectIndex = static_cast<uint32_t>(world.objects.size());
        const auto begin = static_cast<uint32_t>(world.voxels.size());
        world.objects.push_back({.voxelBegin = begin,
                                 .voxelCount = shape.sizeX * shape.sizeY * shape.sizeZ,
                                 .sizeX = shape.sizeX,
                                 .sizeY = shape.sizeY,
                                 .cell = cell,
                                 .bodyLevel = BODY_LEVEL,
                                 .cellLevel = 0,
                                 .substeps = 1,
                                 .filmUm = FILM_UM});
        world.carries.push_back(rx::OhZeroFlow());
        const std::vector<sim::SpeciesAmount> amounts = {
            {.species = table.SpeciesId("cellulose"), .amount = shape.celluloseUnits},
            {.species = table.SpeciesId("oxygen"), .amount = AIR_OXYGEN_UNITS},
            {.species = table.SpeciesId("nitrogen"), .amount = AIR_NITROGEN_UNITS}};
        const rx::RxCell hot = sim::MakeReactionCell(table, amounts, BURNING_MILLIKELVIN);
        const rx::RxCell cold = sim::MakeReactionCell(table, amounts, test::SUN_ROOM_TEMPERATURE_MK);
        for (uint32_t z = 0; z < shape.sizeZ; ++z) {
            for (uint32_t y = 0; y < shape.sizeY; ++y) {
                for (uint32_t x = 0; x < shape.sizeX; ++x) {
                    const uint32_t voxelShape = VoxelShape(shape, x, y, z);
                    const bool exposed = (voxelShape >> rx::OH_EXPOSED_SHIFT) != 0;
                    world.infos.push_back({.object = objectIndex, .shape = voxelShape});
                    world.voxels.push_back(exposed ? hot : cold);
                    world.light.push_back(0);
                }
            }
        }

        if (spot)
            world.light[begin] = SPOT_LIGHT;

        if (world.ranges.empty() || world.ranges.back().cell != cell)
            world.ranges.push_back({.cell = cell, .objectBegin = objectIndex, .objectCount = 0});

        ++world.ranges.back().objectCount;
    }

    void AddAirCells(const sim::BakedReactionTable& table, test::OhWorld& world, uint32_t count) {
        world.cells.assign(count, test::MakeSunCell(table, test::SunMaterial::Air));
    }

    sim::BakedReactionTable MakeTable() {
        return test::MakeSunTable([](sim::ReactionTableDefinition& definition) {
            for (sim::SpeciesDefinition& species : definition.species) {
                if (species.phase == sim::SpeciesPhase::Gas)
                    species.thermalConductivity /= GAS_CONDUCTIVITY_SCALE;
                else if (species.name == "cellulose" || species.name == "carbon")
                    species.thermalConductivity /= SOLID_CONDUCTIVITY_SCALE;
            }
        });
    }

    // --- GPU ---

    struct Pipelines {
        ComPtr<ID3D12RootSignature> rootSignature;
        std::array<ComPtr<ID3D12PipelineState>, 5> stages;  // 下ごしらえ・流れ・物ごとの和・セルへ当てる・反応
    };

    enum Stage : uint8_t {
        StagePrepare,
        StageFlow,
        StageReduce,
        StageApplyCells,
        StageReact,
    };

    std::expected<Pipelines, std::string> CreatePipelines(ID3D12Device5* device) {
        Pipelines result;
        result.rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!result.rootSignature)
            return std::unexpected("ルート署名を作れない");

        const std::array<const char*, 5> names = {"sim/object_heat_prepare.cso", "sim/object_heat_flow.cso",
                                                  "sim/object_heat_reduce.cso", "sim/object_heat_apply_cells.cso",
                                                  "sim/object_heat_react.cso"};
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

    // GPU の世界(既定のヒープ)
    struct GpuWorld {
        std::array<ComPtr<ID3D12Resource>, 7> uavs;        // u0〜u6
        std::array<gpu::ResidentBuffer, 10> tables;        // t0〜t9
        std::array<ComPtr<ID3D12Resource>, 7> uavUploads;  // 写しの元(初めの値)
        ComPtr<ID3D12Resource> voxelReadback;
        ComPtr<ID3D12Resource> cellReadback;
        ComPtr<ID3D12Resource> carryReadback;
        ComPtr<ID3D12Resource> failureReadback;
        ComPtr<ID3D12QueryHeap> timestamps;
        ComPtr<ID3D12Resource> timestampReadback;
        uint32_t voxelCount = 0;
        uint32_t objectCount = 0;
        uint32_t rangeCount = 0;
    };

    template <typename T>
    std::span<const std::byte> BytesOf(const std::vector<T>& values) {
        return std::as_bytes(std::span(values));
    }

    std::expected<GpuWorld, std::string> CreateGpuWorld(ID3D12Device5* device, const sim::BakedReactionTable& table,
                                                        const test::OhTables& tables, const test::OhWorld& world) {
        GpuWorld result;
        result.voxelCount = static_cast<uint32_t>(world.voxels.size());
        result.objectCount = static_cast<uint32_t>(world.objects.size());
        result.rangeCount = static_cast<uint32_t>(world.ranges.size());
        const std::vector<rx::OhVoxelHeat> heats(world.voxels.size());
        const std::vector<rx::OhFlow> flows(world.voxels.size(), rx::OhZeroFlow());
        const std::vector<rx::OhFlow> coarse(world.objects.size(), rx::OhZeroFlow());
        const std::vector<uint32_t> failures(4, 0);
        const std::array<std::span<const std::byte>, 7> initial = {
            BytesOf(world.voxels), BytesOf(heats),       BytesOf(flows),   BytesOf(world.carries),
            BytesOf(coarse),       BytesOf(world.cells), BytesOf(failures)};
        for (size_t i = 0; i < initial.size(); ++i) {
            result.uavs[i] = gpu::CreateBuffer(device, initial[i].size(), gpu::BufferKind::UnorderedAccess);
            result.uavUploads[i] = gpu::CreateFilledUpload(device, initial[i]);
            if (!result.uavs[i] || !result.uavUploads[i])
                return std::unexpected("世界のバッファを作れない");
        }

        const std::vector<uint32_t> gasSpecies(tables.gases.species.begin(), tables.gases.species.end());
        const std::array<std::span<const std::byte>, 10> srvs = {
            BytesOf(table.species),  BytesOf(table.rules),   BytesOf(table.ruleIndex), BytesOf(table.rates),
            BytesOf(tables.species), BytesOf(world.objects), BytesOf(world.infos),     BytesOf(world.light),
            BytesOf(world.ranges),   BytesOf(gasSpecies)};
        for (size_t i = 0; i < srvs.size(); ++i) {
            result.tables[i] = gpu::CreateResidentBuffer(device, srvs[i]);
            if (!result.tables[i].Valid())
                return std::unexpected("表のバッファを作れない");
        }

        result.voxelReadback = gpu::CreateBuffer(device, initial[0].size(), gpu::BufferKind::Readback);
        result.carryReadback = gpu::CreateBuffer(device, initial[3].size(), gpu::BufferKind::Readback);
        result.cellReadback = gpu::CreateBuffer(device, initial[5].size(), gpu::BufferKind::Readback);
        result.failureReadback = gpu::CreateBuffer(device, initial[6].size(), gpu::BufferKind::Readback);
        result.timestampReadback = gpu::CreateBuffer(device, 2 * sizeof(uint64_t), gpu::BufferKind::Readback);
        const D3D12_QUERY_HEAP_DESC heapDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 2, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&result.timestamps))) || !result.voxelReadback ||
            !result.carryReadback || !result.cellReadback || !result.failureReadback || !result.timestampReadback)
            return std::unexpected("読み戻しかタイムスタンプを作れない");

        return result;
    }

    // 初めの値を既定のヒープへ写す(リストの終わりで COMMON に戻り、次のリストで UAV / SRV へ暗黙に上がる)
    bool UploadWorld(gpu::ImmediateQueue& queue, const GpuWorld& world) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return false;

        for (size_t i = 0; i < world.uavs.size(); ++i)
            list->CopyResource(world.uavs[i].Get(), world.uavUploads[i].Get());

        gpu::RecordResidentUploads(list, world.tables);

        return queue.ExecuteAndWait();
    }

    // スレッドの並び(グループの数が 65535 を超える時は y に折る)
    struct Grid {
        uint32_t groupsX;
        uint32_t groupsY;
    };

    Grid GridFor(uint32_t threads) {
        const uint32_t groups = (threads + THREADS - 1) / THREADS;
        if (groups <= 65535)
            return {std::max(groups, 1u), 1};

        return {ROW_GROUPS, (groups + ROW_GROUPS - 1) / ROW_GROUPS};
    }

    void Barrier(ID3D12GraphicsCommandList10* list) {
        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
    }

    void SetConstants(ID3D12GraphicsCommandList10* list, const GpuWorld& world, uint32_t substep, bool lightOn,
                      uint64_t tick, uint32_t rowThreads) {
        const std::array<uint32_t, ROOT_CONSTANT_COUNT> constants = {world.voxelCount,
                                                                     world.objectCount,
                                                                     world.rangeCount,
                                                                     substep,
                                                                     lightOn ? 1u : 0u,
                                                                     static_cast<uint32_t>(tick),
                                                                     static_cast<uint32_t>(tick >> 32),
                                                                     static_cast<uint32_t>(WORLD_SEED),
                                                                     static_cast<uint32_t>(WORLD_SEED >> 32),
                                                                     rowThreads};
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, constants.data(), 0);
    }

    // 1 段を記録する(後に UAV のバリア)
    void RecordStage(ID3D12GraphicsCommandList10* list, const Pipelines& pipelines, const GpuWorld& world, Stage stage,
                     bool lightOn, uint64_t tick) {
        const Grid voxels = GridFor(world.voxelCount);
        const Grid ranges = GridFor(world.rangeCount);
        const Grid grid = stage == StageReduce ? Grid{world.objectCount, 1}
                                               : (stage == StageApplyCells ? ranges : voxels);
        SetConstants(list, world, 0, lightOn, tick, grid.groupsX * THREADS);
        list->SetPipelineState(pipelines.stages[stage].Get());
        list->Dispatch(grid.groupsX, grid.groupsY, 1);
        Barrier(list);
    }

    // 1 刻み(小刻み 1 回。全部の物がレベル 6)を記録する
    void RecordTick(ID3D12GraphicsCommandList10* list, const Pipelines& pipelines, const GpuWorld& world, bool lightOn,
                    uint64_t tick) {
        for (const Stage stage : {StagePrepare, StageFlow, StageReduce, StageApplyCells, StageReact})
            RecordStage(list, pipelines, world, stage, lightOn, tick);
    }

    void BindRoot(ID3D12GraphicsCommandList10* list, const Pipelines& pipelines, const GpuWorld& world,
                  gpu::DebugRing& debugRing) {
        list->SetComputeRootSignature(pipelines.rootSignature.Get());
        for (uint32_t i = 0; i < world.uavs.size(); ++i)
            list->SetComputeRootUnorderedAccessView(i, world.uavs[i]->GetGPUVirtualAddress());

        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing.GpuAddress());
        for (uint32_t i = 0; i < world.tables.size(); ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i),
                                                   world.tables[i].buffer->GetGPUVirtualAddress());
    }

    // 刻み [first, first + count) を 1 本のリストで流す。measure なら最初と最後にタイムスタンプを打ち、GPU の ms を返す
    std::expected<double, std::string> RunTicks(gpu::ImmediateQueue& queue, const Pipelines& pipelines,
                                                const GpuWorld& world, gpu::DebugRing& debugRing, uint64_t first,
                                                uint32_t count, bool lightOn, bool measure) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        debugRing.RecordBegin(list);
        BindRoot(list, pipelines, world, debugRing);
        if (measure)
            list->EndQuery(world.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

        for (uint32_t i = 0; i < count; ++i)
            RecordTick(list, pipelines, world, lightOn, first + i);

        if (measure) {
            list->EndQuery(world.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            list->ResolveQueryData(world.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                   world.timestampReadback.Get(), 0);
        }

        debugRing.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = debugRing.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        if (!measure)
            return 0.0;

        std::array<uint64_t, 2> stamps{};
        uint64_t frequency = 0;
        if (!gpu::ReadBuffer(world.timestampReadback.Get(), std::as_writable_bytes(std::span(stamps))) ||
            FAILED(queue.Native()->GetTimestampFrequency(&frequency)) || frequency == 0)
            return std::unexpected("タイムスタンプを読めない");

        return static_cast<double>(stamps[1] - stamps[0]) * 1000.0 / static_cast<double>(frequency);
    }

    struct Readback {
        std::vector<rx::RxCell> voxels;
        std::vector<rx::RxCell> cells;
        std::vector<rx::OhFlow> carries;
        std::array<uint32_t, 4> failures{};
    };

    std::expected<Readback, std::string> ReadWorld(gpu::ImmediateQueue& queue, const GpuWorld& world) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        // 前のリストの終わりで COMMON に戻っているので、UAV に上げてから写す(RecordCopyToReadback は UAV から)
        for (const size_t i : {size_t{0}, size_t{3}, size_t{5}, size_t{6}}) {
            const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(world.uavs[i].Get(), D3D12_RESOURCE_STATE_COMMON,
                                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &toUav);
        }

        gpu::RecordCopyToReadback(list, world.uavs[0].Get(), world.voxelReadback.Get());
        gpu::RecordCopyToReadback(list, world.uavs[3].Get(), world.carryReadback.Get());
        gpu::RecordCopyToReadback(list, world.uavs[5].Get(), world.cellReadback.Get());
        gpu::RecordCopyToReadback(list, world.uavs[6].Get(), world.failureReadback.Get());
        if (!queue.ExecuteAndWait())
            return std::unexpected("読み戻しに失敗");

        Readback result;
        result.voxels.resize(world.voxelCount);
        result.carries.resize(world.objectCount);
        const bool read = gpu::ReadBuffer(world.voxelReadback.Get(),
                                          std::as_writable_bytes(std::span(result.voxels))) &&
                          gpu::ReadBuffer(world.carryReadback.Get(),
                                          std::as_writable_bytes(std::span(result.carries))) &&
                          gpu::ReadBuffer(world.failureReadback.Get(),
                                          std::as_writable_bytes(std::span(result.failures)));
        if (!read)
            return std::unexpected("読み戻せない");

        return result;
    }

    // --- CPU の検査 ---

    // エネルギーの合計(ボクセルの単位。セル × 8^6 + ボクセル + 対の端数 − 入った光)
    int64_t TotalEnergy(const test::OhWorld& world) {
        int64_t total = -world.sunIn;
        for (const rx::RxCell& cell : world.cells)
            total += cell.energy << (3 * BODY_LEVEL);

        for (const rx::RxCell& voxel : world.voxels)
            total += voxel.energy;

        for (const rx::OhFlow& carry : world.carries)
            total += carry.energy;

        return total;
    }

    int CountMismatches(const test::OhWorld& cpu, const Readback& gpuWorld, std::span<const rx::RxCell> gpuCells,
                        uint32_t tick) {
        int mismatches = 0;
        for (size_t i = 0; i < cpu.voxels.size(); ++i) {
            if (sim::HashReactionCell(cpu.voxels[i]) == sim::HashReactionCell(gpuWorld.voxels[i]))
                continue;

            if (mismatches++ < 5)
                Log(Channel::Gpu, Level::Error, "食い違い: 刻み {} ボクセル {}: cpu エネルギー {} / gpu {}", tick, i,
                    cpu.voxels[i].energy, gpuWorld.voxels[i].energy);
        }

        for (size_t i = 0; i < cpu.cells.size(); ++i)
            mismatches += sim::HashReactionCell(cpu.cells[i]) == sim::HashReactionCell(gpuCells[i]) ? 0 : 1;

        for (size_t o = 0; o < cpu.carries.size(); ++o)
            mismatches += std::memcmp(&cpu.carries[o], &gpuWorld.carries[o], sizeof(rx::OhFlow)) == 0 ? 0 : 1;

        return mismatches;
    }

    struct Context {
        ID3D12Device5* device;
        D3D12_COMMAND_LIST_TYPE queueType;
        const sim::BakedReactionTable& table;
        const test::OhTables& tables;
    };

    // --- 段ごとの切り分け(刻み 1 を段ごとに流し、段ごとに CPU と比べる)---

    template <typename T>
    std::expected<std::vector<T>, std::string> ReadUav(ID3D12Device5* device, gpu::ImmediateQueue& queue,
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
    size_t CountDifferent(std::span<const T> cpu, std::span<const T> gpuValues, const char* label) {
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

        Log(Channel::Gpu, Level::Info, "段ごと: {} 食い違い {} / {}", label, different, cpu.size());

        return different;
    }

    std::expected<void, std::string> RunStageCheck(const Context& context, test::OhWorld& world) {
        auto queue = gpu::ImmediateQueue::Create(context.device, context.queueType);
        auto debugRing = gpu::DebugRing::Create(context.device);
        auto pipelines = CreatePipelines(context.device);
        auto gpuWorld = CreateGpuWorld(context.device, context.table, context.tables, world);
        if (!queue || !debugRing || !pipelines || !gpuWorld || !UploadWorld(*queue, *gpuWorld))
            return std::unexpected("GPU の準備に失敗");

        const test::OhCpuTable cpuTable{.view = context.table.View(), .objectSpecies = context.tables.species};
        const auto runStage = [&](Stage stage) {
            ID3D12GraphicsCommandList10* list = queue->Begin();
            BindRoot(list, *pipelines, *gpuWorld, *debugRing);
            RecordStage(list, *pipelines, *gpuWorld, stage, true, 1);
            return queue->ExecuteAndWait();
        };
        const auto read = [&]<typename T>(size_t uav, size_t count, T) {
            return ReadUav<T>(context.device, *queue, gpuWorld->uavs[uav].Get(), count);
        };

        const std::vector<rx::OhVoxelHeat> heats = test::PrepareObjectHeat(cpuTable, world);
        for (uint32_t i = 0; i < world.voxels[0].speciesCount; ++i) {
            const uint32_t id = world.voxels[0].species[i];
            const rx::OhSpecies values = context.tables.species[id];
            Log(Channel::Gpu, Level::Info,
                "ボクセル 0 の成分: {} 量 {} 伝導率 {} 気体 {} モル体積 {} 混ざる {} 吸い込み {}", id,
                world.voxels[0].amounts[i], context.table.species[id].conductivity, values.gasIndex,
                values.molarVolumeMm3PerMol, values.diffusivityMm2PerS, values.absorptionLongMm2PerMol);
        }

        runStage(StagePrepare);
        const auto gpuHeats = read(1, heats.size(), rx::OhVoxelHeat{});
        const std::vector<rx::OhFlow> flows = test::FlowObjectHeat(cpuTable, context.tables.gases, world, heats, true,
                                                                   0);
        runStage(StageFlow);
        const auto gpuFlows = read(2, flows.size(), rx::OhFlow{});
        const auto gpuVoxels = read(0, world.voxels.size(), rx::RxCell{});
        const std::vector<rx::OhFlow> coarse = test::CarryObjectHeat(world, flows);
        runStage(StageReduce);
        const auto gpuCoarse = read(4, coarse.size(), rx::OhFlow{});
        test::ApplyObjectHeatToCells(context.tables.gases, world, coarse);
        runStage(StageApplyCells);
        const auto gpuCells = read(5, world.cells.size(), rx::RxCell{});
        if (!gpuHeats || !gpuFlows || !gpuVoxels || !gpuCoarse || !gpuCells)
            return std::unexpected("段ごとの読み戻しに失敗");

        const size_t different = CountDifferent<rx::OhVoxelHeat>(heats, *gpuHeats, "① 下ごしらえ") +
                                 CountDifferent<rx::OhFlow>(flows, *gpuFlows, "② 流れ") +
                                 CountDifferent<rx::RxCell>(world.voxels, *gpuVoxels, "② ボクセル") +
                                 CountDifferent<rx::OhFlow>(coarse, *gpuCoarse, "③ 粗い量") +
                                 CountDifferent<rx::RxCell>(world.cells, *gpuCells, "④ セル");
        if (different != 0)
            return std::unexpected(std::format("段ごとに食い違い {}", different));

        return {};
    }

    std::expected<uint64_t, std::string> RunCompare(const Context& context) {
        test::OhWorld world;
        AddAirCells(context.table, world, 2);
        AddObject(context.table, world, Kind::Paper, 0, true);
        AddObject(context.table, world, Kind::Paper, 0, false);
        AddObject(context.table, world, Kind::Twig, 0, true);
        AddObject(context.table, world, Kind::Twig, 0, false);
        AddObject(context.table, world, Kind::Twig, 0, false);
        AddObject(context.table, world, Kind::Log, 1, true);

        // 刻み 1 を段ごとに流して比べる(食い違った段が分かる)
        test::OhWorld staged = world;
        if (auto checked = RunStageCheck(context, staged); !checked)
            return std::unexpected(checked.error());

        auto queue = gpu::ImmediateQueue::Create(context.device, context.queueType);
        auto debugRing = gpu::DebugRing::Create(context.device);
        auto pipelines = CreatePipelines(context.device);
        auto gpuWorld = CreateGpuWorld(context.device, context.table, context.tables, world);
        if (!queue || !debugRing || !pipelines || !gpuWorld)
            return std::unexpected("GPU の準備に失敗");

        if (!UploadWorld(*queue, *gpuWorld))
            return std::unexpected("初めの値を写せない");

        const test::OhCpuTable cpuTable{.view = context.table.View(), .objectSpecies = context.tables.species};
        const int64_t initialEnergy = TotalEnergy(world);
        for (uint32_t tick = 1; tick <= COMPARE_TICKS; tick += COMPARE_EVERY) {
            const auto ran = RunTicks(*queue, *pipelines, *gpuWorld, *debugRing, tick, COMPARE_EVERY, true, false);
            if (!ran)
                return std::unexpected(ran.error());

            for (uint32_t t = tick; t < tick + COMPARE_EVERY; ++t) {
                test::StepObjectHeat(cpuTable, context.tables.gases, world, true, WORLD_SEED, t);
                if (TotalEnergy(world) != initialEnergy || world.failed)
                    return std::unexpected(std::format("CPU の保存が崩れた(刻み {})", t));
            }

            auto read = ReadWorld(*queue, *gpuWorld);
            if (!read)
                return std::unexpected(read.error());

            std::vector<rx::RxCell> gpuCells(world.cells.size());
            if (!gpu::ReadBuffer(gpuWorld->cellReadback.Get(), std::as_writable_bytes(std::span(gpuCells))))
                return std::unexpected("セルを読み戻せない");

            const int mismatches = CountMismatches(world, *read, gpuCells, tick + COMPARE_EVERY - 1);
            if (mismatches != 0 || read->failures[0] != 0)
                return std::unexpected(std::format("刻み {} で食い違い {}・GPU の失敗 {}", tick + COMPARE_EVERY - 1,
                                                   mismatches, read->failures[0]));
        }

        uint64_t digest = 0;
        for (const rx::RxCell& voxel : world.voxels)
            digest = fx::FxHashCombine(digest, sim::HashReactionCell(voxel));

        for (const rx::RxCell& cell : world.cells)
            digest = fx::FxHashCombine(digest, sim::HashReactionCell(cell));

        Log(Channel::Gpu, Level::Info,
            "ボクセル {}・物 {}・セル {}・{} 刻み: GPU と CPU がビット一致・エネルギーの保存 0", world.voxels.size(),
            world.objects.size(), world.cells.size(), COMPARE_TICKS);

        return digest;
    }

    // --- 費用(T-1065)---

    std::expected<void, std::string> MeasureOne(const Context& context, Kind kind, uint32_t count) {
        test::OhWorld world;
        const uint32_t cellCount = (count + OBJECTS_PER_CELL - 1) / OBJECTS_PER_CELL;
        AddAirCells(context.table, world, cellCount);
        for (uint32_t i = 0; i < count; ++i)
            AddObject(context.table, world, kind, i / OBJECTS_PER_CELL, false);

        auto queue = gpu::ImmediateQueue::Create(context.device, context.queueType);
        auto debugRing = gpu::DebugRing::Create(context.device);
        auto pipelines = CreatePipelines(context.device);
        auto gpuWorld = CreateGpuWorld(context.device, context.table, context.tables, world);
        if (!queue || !debugRing || !pipelines || !gpuWorld)
            return std::unexpected("GPU の準備に失敗");

        if (!UploadWorld(*queue, *gpuWorld))
            return std::unexpected("初めの値を写せない");

        // 暖機 4 刻み(時計を上げる)の後、1 刻みずつ 8 回測る
        uint64_t tick = 1;
        for (uint32_t i = 0; i < 4; ++i) {
            if (auto ran = RunTicks(*queue, *pipelines, *gpuWorld, *debugRing, tick++, 1, false, false); !ran)
                return std::unexpected(ran.error());
        }

        std::vector<double> times;
        for (uint32_t i = 0; i < 8; ++i) {
            auto ran = RunTicks(*queue, *pipelines, *gpuWorld, *debugRing, tick++, 1, false, true);
            if (!ran)
                return std::unexpected(ran.error());

            times.push_back(*ran);
        }

        rng::sort(times);
        auto read = ReadWorld(*queue, *gpuWorld);
        if (!read)
            return std::unexpected(read.error());

        const double perVoxelNs = times.front() * 1.0e6 / static_cast<double>(world.voxels.size());
        Log(Channel::Gpu, Level::Info,
            "費用: {} × {}(ボクセル {}・セル {}): 1 刻み 最小 {:.3f} ms・中央 {:.3f} ms・最大 {:.3f} ms(ボクセルあたり "
            "{:.1f} ns)・"
            "GPU の失敗 {}",
            ShapeOf(kind).name, count, world.voxels.size(), cellCount, times.front(), times[times.size() / 2],
            times.back(), perVoxelNs, read->failures[0]);

        return {};
    }

    int Run(std::span<char*> arguments) {
        std::vector<char*> rest;
        bool measure = false;
        bool compare = true;
        for (char* argument : arguments) {
            if (std::string_view(argument) == "--measure")
                measure = true;
            else if (std::string_view(argument) == "--measure-only") {
                measure = true;
                compare = false;
            } else
                rest.push_back(argument);
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_object_heat_test [--warp] [--queue direct|compute] [--measure | --measure-only]");
            return 2;
        }

        const sim::BakedReactionTable table = MakeTable();
        const auto tables = test::MakeObjectHeatTables(table, DEFAULT_DIFFUSIVITY_MM2_PER_S);
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!tables || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_object_heat_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        const Context context{
            .device = device->Get(), .queueType = options->queueType, .table = table, .tables = *tables};
        const auto digest = compare ? RunCompare(context) : std::expected<uint64_t, std::string>(0);
        if (!digest) {
            Log(Channel::Gpu, Level::Error, "gpu_object_heat_test: FAILED({})", digest.error());
            return 1;
        }

        if (measure) {
            for (const Kind kind : {Kind::Paper, Kind::Twig, Kind::Log}) {
                for (const uint32_t count : {10u, 100u, 1000u}) {
                    if (auto measured = MeasureOne(context, kind, count); !measured) {
                        Log(Channel::Gpu, Level::Error, "gpu_object_heat_test: FAILED(計測: {})", measured.error());
                        return 1;
                    }
                }
            }
        }

        if (!test::PassesValidation(*device, "gpu_object_heat_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_object_heat_test: OK(要約 {:016x})", *digest);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
