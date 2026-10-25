// gpu_object_heat_test.cpp — 物の熱(物の箱のボクセルの熱と反応・セルとの対の面の 4 つの口)の GPU 版を CPU リファレンスとビット一致で確かめ、
// 燃えている物の数を増やした時の 1 刻みの GPU の時間を測る(T-1064・T-1065・T-1098・ADR-0434・ADR-0451・ADR-0459・20 §12.4)。
// 式は shaders/common/object_heat.hlsli、段は shaders/sim/object_heat.hlsl、CPU は tests/object_heat_cpu.h。
// 物は 3 種(箱 7.8 mm = レベル 6): A4 の紙(27 × 1 × 38・厚さ 0.1 mm を箱に薄めて両面が外)・小枝(2 × 2 × 38・Ø 1 cm の木を箱に薄めて)・
// 薪(10 × 10 × 51・Ø 8 cm × 40 cm を四角の箱で)。木は試験の表の cellulose を薄めた量。外に面するボクセルを 900 K から始める(燃えている物)。
//   既定: 紙 2・小枝 3(同じセル)・薪 1(別のセル)を 60 刻み、20 刻みごとにボクセル・セル・対の端数・待ち・ブロックの状態を読み戻して CPU と比べる。
//         CPU の側で、眠らせた世界と全部回す世界が毎刻み同じこと(T-1098)と、エネルギーの保存(セル × 8^6 + ボクセル + 対の端数 − 入った光)も確かめる。
//   --profile: 薪 100 本・焚き火を 600 刻み回し、60 刻みごとに段ごとの GPU の ms と起きているブロックの数を出す
//   --measure(--measure-only は CPU と比べずに測るだけ): 薪 100 本・焚き火・紙 / 小枝 / 薪 1000 個(10 個ずつ同じセル)を、全部回す / 眠らせる で、
//         暖機 4 刻みと 300 刻みの後の 1 刻みの GPU の時間(段ごとのタイムスタンプの和)を測る。
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
    constexpr int32_t BURNING_MILLIKELVIN = 900000;
    constexpr int64_t SPOT_LIGHT = 10000000;  // 物の最初のボクセルが 1 刻みに吸う光(ボクセルの単位。約 2.3 W)
    constexpr uint64_t WORLD_SEED = 0x1064;
    constexpr uint32_t COMPARE_TICKS = 60;
    constexpr uint32_t COMPARE_EVERY = 20;
    constexpr uint32_t OBJECTS_PER_CELL = 10;
    constexpr uint32_t THREADS = 64;
    constexpr uint32_t ROW_GROUPS = 1024;  // グループの数が 65535 を超える時の x の数
    constexpr uint32_t ROOT_CONSTANT_COUNT = 13;
    constexpr uint32_t SRV_COUNT = 11;
    constexpr uint32_t TIMESTAMP_COUNT = 7;  // 刻みの初め + 6 つの段の後

    // u0〜u9 / b0 / デバッグのリング / t0〜t10(object_heat.hlsl)
    constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
        .uavCount = 10, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = SRV_COUNT};

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
    void AddObject(const sim::BakedReactionTable& table, test::OhWorld& world, Kind kind, uint32_t cell, bool spot,
                   bool burning = true) {
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
                    world.voxels.push_back(exposed && burning ? hot : cold);
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
        // 伝導率は表の現実の値のまま(試験の倍率は T-1066 で表から外した。ここで割っていたのを外す)
        return test::MakeSunTable();
    }

    // 物を置いた後に呼ぶ: ブロックと眠らせ方の状態を作る
    void Finish(test::OhWorld& world, bool sleeping) {
        test::FinishObjectHeatWorld(world);
        world.sleeping = sleeping;
    }

    // --- GPU ---

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

    std::expected<Pipelines, std::string> CreatePipelines(ID3D12Device5* device) {
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
        result.blockCount = static_cast<uint32_t>(world.blocks.size());
        result.sleeping = world.sleeping;
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
    bool UploadWorld(gpu::ImmediateQueue& queue, const GpuWorld& world) {
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

    Grid GroupsFor(uint32_t groups) {
        if (groups <= 65535)
            return {std::max(groups, 1u), 1};

        return {ROW_GROUPS, (groups + ROW_GROUPS - 1) / ROW_GROUPS};
    }

    Grid ThreadsFor(uint32_t threads) {
        return GroupsFor((threads + THREADS - 1) / THREADS);
    }

    void Barrier(ID3D12GraphicsCommandList10* list) {
        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
    }

    // 段の番号(CPU の world.pass と同じ: 刻み t の小刻み s は (t − 1) × 段の数 + s + 1、反応は t × 段の数)
    uint32_t PassOf(const GpuWorld& world, uint64_t tick, uint32_t substep, bool react) {
        const auto base = static_cast<uint32_t>((tick - 1) * world.passesPerTick);

        return react ? base + world.passesPerTick : base + substep + 1;
    }

    void SetConstants(ID3D12GraphicsCommandList10* list, const GpuWorld& world, uint32_t substep, bool lightOn,
                      uint64_t tick, uint32_t rowThreads, uint32_t pass) {
        const std::array<uint32_t, ROOT_CONSTANT_COUNT> constants = {world.voxelCount,
                                                                     world.objectCount,
                                                                     world.rangeCount,
                                                                     substep,
                                                                     lightOn ? 1u : 0u,
                                                                     static_cast<uint32_t>(tick),
                                                                     static_cast<uint32_t>(tick >> 32),
                                                                     static_cast<uint32_t>(WORLD_SEED),
                                                                     static_cast<uint32_t>(WORLD_SEED >> 32),
                                                                     rowThreads,
                                                                     world.blockCount,
                                                                     pass,
                                                                     world.sleeping ? 1u : 0u};
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, constants.data(), 0);
    }

    // 1 段を記録する(後に UAV のバリア)
    void RecordStage(ID3D12GraphicsCommandList10* list, const Pipelines& pipelines, const GpuWorld& world, Stage stage,
                     bool lightOn, uint64_t tick, uint32_t substep) {
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
    void RecordTick(ID3D12GraphicsCommandList10* list, const Pipelines& pipelines, const GpuWorld& world, bool lightOn,
                    uint64_t tick, bool stamps) {
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

    // 刻み [first, first + count) を 1 本のリストで流す。stamps なら段ごとのタイムスタンプ(ms)を返す(1 刻みの時だけ意味がある)
    std::expected<std::array<double, STAGE_COUNT>, std::string> RunTicks(gpu::ImmediateQueue& queue,
                                                                         const Pipelines& pipelines,
                                                                         const GpuWorld& world,
                                                                         gpu::DebugRing& debugRing, uint64_t first,
                                                                         uint32_t count, bool lightOn, bool stamps) {
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

    double Sum(const std::array<double, STAGE_COUNT>& stages) {
        double total = 0.0;
        for (const double value : stages)
            total += value;

        return total;
    }

    // UAV の中身を読み戻す(前のリストの終わりで COMMON に戻っているので、UAV に上げてから写す)
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

        if (different != 0)
            Log(Channel::Gpu, Level::Error, "{}: 食い違い {} / {}", label, different, cpu.size());

        return different;
    }

    // GPU の世界の真実(ボクセル・セル・対の端数・待ち・ブロックの状態)を CPU と比べる。食い違いの数
    std::expected<size_t, std::string> CompareWorld(ID3D12Device5* device, gpu::ImmediateQueue& queue,
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

    bool SameTruth(const test::OhWorld& a, const test::OhWorld& b) {
        const auto same = [](const auto& x, const auto& y) {
            return x.size() == y.size() && std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0;
        };

        return same(a.voxels, b.voxels) && same(a.cells, b.cells) && same(a.carries, b.carries) &&
               same(a.waits, b.waits) && a.sunIn == b.sunIn;
    }

    struct Context {
        ID3D12Device5* device;
        D3D12_COMMAND_LIST_TYPE queueType;
        const sim::BakedReactionTable& table;
        const test::OhTables& tables;
    };

    // 比べる場面: 紙 2・小枝 3(同じセル)・薪 1(別のセル)。最初の物は光を吸う
    test::OhWorld CompareScene(const sim::BakedReactionTable& table, bool sleeping) {
        test::OhWorld world;
        AddAirCells(table, world, 2);
        AddObject(table, world, Kind::Paper, 0, true);
        AddObject(table, world, Kind::Paper, 0, false);
        AddObject(table, world, Kind::Twig, 0, true);
        AddObject(table, world, Kind::Twig, 0, false);
        AddObject(table, world, Kind::Twig, 0, false);
        AddObject(table, world, Kind::Log, 1, true);
        Finish(world, sleeping);

        return world;
    }

    // 眠らせても眠らせなくても同じ(CPU で毎刻み)・GPU と CPU がビット一致(20 刻みごと)
    std::expected<uint64_t, std::string> RunCompare(const Context& context) {
        test::OhWorld world = CompareScene(context.table, true);
        test::OhWorld always = CompareScene(context.table, false);
        auto queue = gpu::ImmediateQueue::Create(context.device, context.queueType);
        auto debugRing = gpu::DebugRing::Create(context.device);
        auto pipelines = CreatePipelines(context.device);
        auto gpuWorld = CreateGpuWorld(context.device, context.table, context.tables, world);
        if (!queue || !debugRing || !pipelines || !gpuWorld || !UploadWorld(*queue, *gpuWorld))
            return std::unexpected("GPU の準備に失敗");

        const test::OhCpuTable cpuTable{.view = context.table.View(), .objectSpecies = context.tables.species};
        const int64_t initialEnergy = TotalEnergy(world);
        for (uint32_t tick = 1; tick <= COMPARE_TICKS; tick += COMPARE_EVERY) {
            const auto ran = RunTicks(*queue, *pipelines, *gpuWorld, *debugRing, tick, COMPARE_EVERY, true, false);
            if (!ran)
                return std::unexpected(ran.error());

            for (uint32_t t = tick; t < tick + COMPARE_EVERY; ++t) {
                test::StepObjectHeat(cpuTable, context.tables.gases, world, true, WORLD_SEED, t);
                test::StepObjectHeat(cpuTable, context.tables.gases, always, true, WORLD_SEED, t);
                if (TotalEnergy(world) != initialEnergy || world.failed)
                    return std::unexpected(std::format("CPU の保存が崩れた(刻み {})", t));

                if (!SameTruth(world, always))
                    return std::unexpected(std::format("眠らせると結果が変わった(刻み {})", t));
            }

            const auto different = CompareWorld(context.device, *queue, *gpuWorld, world);
            if (!different || *different != 0)
                return std::unexpected(std::format("刻み {} で GPU と CPU が食い違った", tick + COMPARE_EVERY - 1));
        }

        uint64_t digest = 0;
        for (const rx::RxCell& voxel : world.voxels)
            digest = fx::FxHashCombine(digest, sim::HashReactionCell(voxel));

        for (const rx::RxCell& cell : world.cells)
            digest = fx::FxHashCombine(digest, sim::HashReactionCell(cell));

        Log(Channel::Gpu, Level::Info,
            "ボクセル {}・ブロック {}・物 {}・セル {}・{} 刻み: GPU と CPU "
            "がビット一致・眠らせても同じ・エネルギーの保存 0"
            "(回したブロック 受け渡し {} / {}・反応 {} / {})",
            world.voxels.size(), world.blocks.size(), world.objects.size(), world.cells.size(), COMPARE_TICKS,
            world.awakeExchangeBlocks, always.awakeExchangeBlocks, world.awakeReactBlocks, always.awakeReactBlocks);

        return digest;
    }

    // --- 費用(T-1065・T-1098)---

    struct Scene {
        const char* name;
        std::vector<std::pair<Kind, uint32_t>> parts;  // 物の種類と数(10 個ずつ同じセル)
        uint32_t burning = UINT32_MAX;                 // 初めの何個が燃えているか(残りは冷えた物)
    };

    test::OhWorld BuildScene(const sim::BakedReactionTable& table, const Scene& scene, bool sleeping) {
        test::OhWorld world;
        uint32_t total = 0;
        for (const auto& [kind, count] : scene.parts)
            total += count;

        AddAirCells(table, world, (total + OBJECTS_PER_CELL - 1) / OBJECTS_PER_CELL);
        uint32_t index = 0;
        for (const auto& [kind, count] : scene.parts) {
            for (uint32_t i = 0; i < count; ++i, ++index)
                AddObject(table, world, kind, index / OBJECTS_PER_CELL, false, index < scene.burning);
        }

        Finish(world, sleeping);

        return world;
    }

    // 暖機 warm 刻みの後、1 刻みずつ 8 回測る(最小・中央)。起きているブロックの数(最後の段)も
    std::expected<void, std::string> MeasureOne(const Context& context, const Scene& scene, bool sleeping,
                                                uint32_t warm) {
        const test::OhWorld world = BuildScene(context.table, scene, sleeping);
        auto queue = gpu::ImmediateQueue::Create(context.device, context.queueType);
        auto debugRing = gpu::DebugRing::Create(context.device);
        auto pipelines = CreatePipelines(context.device);
        auto gpuWorld = CreateGpuWorld(context.device, context.table, context.tables, world);
        if (!queue || !debugRing || !pipelines || !gpuWorld || !UploadWorld(*queue, *gpuWorld))
            return std::unexpected("GPU の準備に失敗");

        uint64_t tick = 1;
        for (uint32_t done = 0; done < warm; done += 30) {
            const uint32_t count = std::min(30u, warm - done);
            if (auto ran = RunTicks(*queue, *pipelines, *gpuWorld, *debugRing, tick, count, false, false); !ran)
                return std::unexpected(ran.error());

            tick += count;
        }

        std::vector<double> times;
        for (uint32_t i = 0; i < 8; ++i) {
            auto ran = RunTicks(*queue, *pipelines, *gpuWorld, *debugRing, tick++, 1, false, true);
            if (!ran)
                return std::unexpected(ran.error());

            times.push_back(Sum(*ran));
        }

        rng::sort(times);
        const auto states = ReadUav<rx::OhBlockState>(context.device, *queue, gpuWorld->uavs[UavBlockStates].Get(),
                                                      world.blockStates.size());
        const auto failures = ReadUav<uint32_t>(context.device, *queue, gpuWorld->uavs[UavFailures].Get(), 4);
        if (!states || !failures)
            return std::unexpected("読み戻せない");

        size_t awake = 0;
        for (const rx::OhBlockState& state : *states)
            awake += state.awake;

        Log(Channel::Gpu, Level::Info,
            "費用: {}(ボクセル {}・ブロック {}・{}・{} 刻み目から): 1 刻み 最小 {:.3f} ms・中央 {:.3f} ms・最大 {:.3f} "
            "ms・"
            "起きているブロック {}・GPU の失敗 {}",
            scene.name, world.voxels.size(), world.blocks.size(), sleeping ? "眠らせる" : "全部回す", tick - 8,
            times.front(), times[times.size() / 2], times.back(), awake, (*failures)[0]);

        return {};
    }

    // --- 燃え進みの切り分け(--profile。T-1065)---

    std::expected<void, std::string> Profile(const Context& context, const Scene& scene) {
        const test::OhWorld world = BuildScene(context.table, scene, true);
        auto queue = gpu::ImmediateQueue::Create(context.device, context.queueType);
        auto debugRing = gpu::DebugRing::Create(context.device);
        auto pipelines = CreatePipelines(context.device);
        auto gpuWorld = CreateGpuWorld(context.device, context.table, context.tables, world);
        if (!queue || !debugRing || !pipelines || !gpuWorld || !UploadWorld(*queue, *gpuWorld))
            return std::unexpected("GPU の準備に失敗");

        std::array<double, STAGE_COUNT> sums{};
        for (uint64_t tick = 1; tick <= 600; ++tick) {
            auto ran = RunTicks(*queue, *pipelines, *gpuWorld, *debugRing, tick, 1, false, true);
            if (!ran)
                return std::unexpected(ran.error());

            for (size_t stage = 0; stage < STAGE_COUNT; ++stage)
                sums[stage] += (*ran)[stage];

            if (tick % 60 != 0)
                continue;

            const auto states = ReadUav<rx::OhBlockState>(context.device, *queue, gpuWorld->uavs[UavBlockStates].Get(),
                                                          world.blockStates.size());
            size_t awake = 0;
            for (const rx::OhBlockState& state : states.value_or(std::vector<rx::OhBlockState>{}))
                awake += state.awake;

            Log(Channel::Gpu, Level::Info,
                "燃え進み: {} 刻み {:3}: 段の ms(60 刻みの平均)起こす {:.2f}・下ごしらえ {:.2f}・流れ {:.2f}・物ごと "
                "{:.2f}・"
                "セル {:.2f}・反応 {:.2f}・起きているブロック {} / {}",
                scene.name, tick, sums[0] / 60, sums[1] / 60, sums[2] / 60, sums[3] / 60, sums[4] / 60, sums[5] / 60,
                awake, world.blocks.size());
            sums = {};
        }

        return {};
    }

    int Run(std::span<char*> arguments) {
        std::vector<char*> rest;
        bool measure = false;
        bool compare = true;
        bool profile = false;
        for (char* argument : arguments) {
            const std::string_view text = argument;
            if (text == "--measure")
                measure = true;
            else if (text == "--profile") {
                profile = true;
                compare = false;
            } else if (text == "--measure-only") {
                measure = true;
                compare = false;
            } else
                rest.push_back(argument);
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_object_heat_test [--warp] [--queue direct|compute] [--measure | --measure-only | "
                "--profile]");
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

        const Scene logs{"薪 100 本", {{Kind::Log, 100}}};
        const Scene campfire{"焚き火(薪 10・小枝 30・紙 10)", {{Kind::Log, 10}, {Kind::Twig, 30}, {Kind::Paper, 10}}};
        if (profile) {
            for (const Scene& scene : {logs, campfire}) {
                if (auto profiled = Profile(context, scene); !profiled) {
                    Log(Channel::Gpu, Level::Error, "gpu_object_heat_test: FAILED(切り分け: {})", profiled.error());
                    return 1;
                }
            }
        }

        if (measure) {
            const std::array<Scene, 5> scenes = {
                logs, campfire, Scene{"薪の山 100 本(燃えているのは 10 本)", {{Kind::Log, 100}}, 10},
                Scene{"紙の山 1000 枚(燃えているのは 100 枚)", {{Kind::Paper, 1000}}, 100},
                Scene{"薪の山 1000 本(燃えているのは 10 本)", {{Kind::Log, 1000}}, 10}};
            for (const Scene& scene : scenes) {
                for (const bool sleeping : {false, true}) {
                    for (const uint32_t warm : {4u, 300u}) {
                        if (auto measured = MeasureOne(context, scene, sleeping, warm); !measured) {
                            Log(Channel::Gpu, Level::Error, "gpu_object_heat_test: FAILED(計測: {})", measured.error());
                            return 1;
                        }
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
