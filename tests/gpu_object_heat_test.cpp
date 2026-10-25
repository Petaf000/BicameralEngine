// gpu_object_heat_test.cpp — 物の熱(物の箱のボクセルの熱と反応・セルとの対の面の 4 つの口)の GPU 版を CPU リファレンスとビット一致で確かめ、
// 燃えている物の数を増やした時の 1 刻みの GPU の時間を測る(T-1064・T-1065・T-1098・ADR-0434・ADR-0451・ADR-0459・20 §12.4)。
// 式は shaders/common/object_heat.hlsli、段は shaders/sim/object_heat.hlsl、CPU は tests/object_heat_cpu.h。
// 物は 3 種(箱 7.8 mm = レベル 6): A4 の紙(27 × 1 × 38・厚さ 0.1 mm を箱に薄めて両面が外)・小枝(2 × 2 × 38・Ø 1 cm の木を箱に薄めて)・
// 薪(10 × 10 × 51・Ø 8 cm × 40 cm を四角の箱で)。木は試験の表の cellulose を薄めた量。外に面するボクセルを 900 K から始める(燃えている物)。
//   既定: 紙 2・小枝 3(同じセル)・薪 1 と小刻み 4 回の小枝 1(別のセル)を 60 刻み、20 刻みごとにボクセル・セル・対の端数・待ち・ブロックの状態を
//         読み戻して CPU と比べる(GPU は起きている物の一覧の形と、全部を Dispatch して眠っているブロックを抜ける形の 2 つ。T-1077)。
//         CPU の側で、眠らせた世界と全部回す世界が毎刻み同じこと(T-1098)と、エネルギーの保存(セル × 8^6 + ボクセル + 対の端数 − 入った光)も確かめる。
//   --profile: 薪 100 本・焚き火を 600 刻み回し、60 刻みごとに段ごとの GPU の ms と起きているブロックの数を出す
//   --measure(--measure-only は CPU と比べずに測るだけ): 薪 100 本・焚き火・紙 / 小枝 / 薪 1000 個(10 個ずつ同じセル)を、全部回す / 眠らせる /
//         起きている物の一覧 で、暖機 4 刻みと 300 刻みの後の 1 刻みの GPU の時間(段ごとのタイムスタンプの和)を測る。
//         眠っている小枝 10,000 本 + 燃える小枝 10・100・1000 本は、眠らせる / 一覧 で 300 刻みの後(T-1077)。
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
#include "gpu_object_heat_stages.h"
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
                   bool burning = true, uint32_t substeps = 1) {
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
                                 .substeps = substeps,
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

    using namespace test::object_heat_gpu;  // GPU の段を回す道具(gpu_object_heat_stages.h)

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

    // 比べる場面: 紙 2・小枝 3(同じセル)・薪 1・小刻み 4 回の小枝 1(別のセル)。最初の物は光を吸う。
    // 小刻み 4 回の小枝は、ほかの物が小刻みを飛ばす段(一覧に入れても受け渡しを回さない)を作るため(形は同じ箱で、対の面を細かい時間で回すだけ)
    test::OhWorld CompareScene(const sim::BakedReactionTable& table, bool sleeping) {
        test::OhWorld world;
        AddAirCells(table, world, 2);
        AddObject(table, world, Kind::Paper, 0, true);
        AddObject(table, world, Kind::Paper, 0, false);
        AddObject(table, world, Kind::Twig, 0, true);
        AddObject(table, world, Kind::Twig, 0, false);
        AddObject(table, world, Kind::Twig, 0, false);
        AddObject(table, world, Kind::Log, 1, true);
        AddObject(table, world, Kind::Twig, 1, false, true, 4);
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
        auto listed = CreateGpuWorld(context.device, context.table, context.tables, world, WORLD_SEED, true);
        auto blocks = CreateGpuWorld(context.device, context.table, context.tables, world, WORLD_SEED, false);
        if (!queue || !debugRing || !pipelines || !listed || !blocks || !UploadWorld(*queue, *listed) ||
            !UploadWorld(*queue, *blocks))
            return std::unexpected("GPU の準備に失敗");

        const std::array<const GpuWorld*, 2> gpuWorlds = {&*listed, &*blocks};
        const test::OhCpuTable cpuTable{.view = context.table.View(), .objectSpecies = context.tables.species};
        const int64_t initialEnergy = TotalEnergy(world);
        for (uint32_t tick = 1; tick <= COMPARE_TICKS; tick += COMPARE_EVERY) {
            for (const GpuWorld* gpuWorld : gpuWorlds) {
                const auto ran = RunTicks(*queue, *pipelines, *gpuWorld, *debugRing, tick, COMPARE_EVERY, true, false);
                if (!ran)
                    return std::unexpected(ran.error());
            }

            for (uint32_t t = tick; t < tick + COMPARE_EVERY; ++t) {
                test::StepObjectHeat(cpuTable, context.tables.gases, world, true, WORLD_SEED, t);
                test::StepObjectHeat(cpuTable, context.tables.gases, always, true, WORLD_SEED, t);
                if (TotalEnergy(world) != initialEnergy || world.failed)
                    return std::unexpected(std::format("CPU の保存が崩れた(刻み {})", t));

                if (!SameTruth(world, always))
                    return std::unexpected(std::format("眠らせると結果が変わった(刻み {})", t));
            }

            for (const GpuWorld* gpuWorld : gpuWorlds) {
                const auto different = CompareWorld(context.device, *queue, *gpuWorld, world);
                if (!different || *different != 0)
                    return std::unexpected(std::format("刻み {} で GPU({})と CPU が食い違った",
                                                       tick + COMPARE_EVERY - 1,
                                                       gpuWorld == gpuWorlds[0] ? "一覧" : "全部を Dispatch"));
            }
        }

        uint64_t digest = 0;
        for (const rx::RxCell& voxel : world.voxels)
            digest = fx::FxHashCombine(digest, sim::HashReactionCell(voxel));

        for (const rx::RxCell& cell : world.cells)
            digest = fx::FxHashCombine(digest, sim::HashReactionCell(cell));

        Log(Channel::Gpu, Level::Info,
            "ボクセル {}・ブロック {}・物 {}・セル {}・{} 刻み: GPU(一覧・全部を Dispatch)と CPU "
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

    // 測る形(全部回す・眠らせる〔全部を Dispatch〕・起きている物の一覧)
    const char* ModeName(uint32_t mode) {
        if (mode == rx::OH_SLEEP_ALL)
            return "全部回す";

        return mode == rx::OH_SLEEP_BLOCKS ? "眠らせる" : "一覧";
    }

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
    std::expected<void, std::string> MeasureOne(const Context& context, const Scene& scene, uint32_t mode,
                                                uint32_t warm) {
        const test::OhWorld world = BuildScene(context.table, scene, mode != rx::OH_SLEEP_ALL);
        auto queue = gpu::ImmediateQueue::Create(context.device, context.queueType);
        auto debugRing = gpu::DebugRing::Create(context.device);
        auto pipelines = CreatePipelines(context.device);
        auto gpuWorld = CreateGpuWorld(context.device, context.table, context.tables, world, WORLD_SEED,
                                       mode == rx::OH_SLEEP_LISTED);
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
            scene.name, world.voxels.size(), world.blocks.size(), ModeName(mode), tick - 8, times.front(),
            times[times.size() / 2], times.back(), awake, (*failures)[0]);

        return {};
    }

    // --- 燃え進みの切り分け(--profile。T-1065)---

    std::expected<void, std::string> Profile(const Context& context, const Scene& scene) {
        const test::OhWorld world = BuildScene(context.table, scene, true);
        auto queue = gpu::ImmediateQueue::Create(context.device, context.queueType);
        auto debugRing = gpu::DebugRing::Create(context.device);
        auto pipelines = CreatePipelines(context.device);
        auto gpuWorld = CreateGpuWorld(context.device, context.table, context.tables, world, WORLD_SEED);
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
                for (const uint32_t mode : {rx::OH_SLEEP_ALL, rx::OH_SLEEP_BLOCKS, rx::OH_SLEEP_LISTED}) {
                    for (const uint32_t warm : {4u, 300u}) {
                        if (auto measured = MeasureOne(context, scene, mode, warm); !measured) {
                            Log(Channel::Gpu, Level::Error, "gpu_object_heat_test: FAILED(計測: {})", measured.error());
                            return 1;
                        }
                    }
                }
            }

            // --- 眠っている物 10,000 個(冷えた小枝)の中で、燃える小枝 10・100・1000 本(T-1077)---
            for (const uint32_t burning : {10u, 100u, 1000u}) {
                const std::string name = std::format("眠っている小枝 10000 本 + 燃える小枝 {} 本", burning);
                const Scene scene{name.c_str(), {{Kind::Twig, 10000 + burning}}, burning};
                for (const uint32_t mode : {rx::OH_SLEEP_BLOCKS, rx::OH_SLEEP_LISTED}) {
                    if (auto measured = MeasureOne(context, scene, mode, 300); !measured) {
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
