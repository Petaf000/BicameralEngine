// gpu_cascade_tree_grid_test.cpp — 世界の中の光(RC)の木を読む写しの GPU(GpuCascadeLight::CreateFromTree・RecordTreeGrid。
// shaders/sim/cascade_light.hlsl の RcTreeGrid。T-1117)が CPU の MakeCascadeTreeGrid とビット単位で同じ写しを作ること。
//
// 場面: 石の地面(1 m)+ 熱い石の塊(2 m 角・900 K)+ 空気の 16 m 角ほどの世界を、根のレベルを変えて作る:
//   0.5 m の根 / 1 m の根 / 2 m の根 / 1 m の根の一部を 0.5 m に細かくした木(粗い葉と細かい葉が混ざる)。
// 確かめること:
//   1. 刻みごとに GPU が木から写しを作って RC を流した結果(セルごとの吸った・放った・帳簿・光線・持ち越し)が、
//      CPU の MakeCascadeTreeGrid の写しで StepCascadeLight を流した結果と全部の刻みでビット単位で同じ
//   2. 0.5 m より細かい葉のある木は、GPU が溢れの印 4 を立てる(CPU は誤り。細かい葉へ配る規則は土台)
//   3. 費用(HW の時だけ): CPU を挟まず続けて流した刻みの写しの段と RC の GPU 時間(docs/perf.md)
// 引数は gpu_test_options.h。
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/stage_stamps.h"
#include "gpu_test_options.h"
#include "sim/cascade_light.h"
#include "sim/gpu_cascade_light.h"
#include "sim/gpu_multires.h"
#include "sun_column_scene.h"

using namespace bicameral;
namespace mr = bicameral::multires;
namespace rx = bicameral::reaction;

namespace {

    constexpr int32_t HOT_MK = 900000;
    constexpr uint64_t TICKS = 16;  // 放つ面の組(既定 15)を一巡りする

    // 0.5 m の座標(セルの低い角)→ セル(石の地面 1 m・熱い石の塊 2 m 角・空気)
    rx::RxCell CellAt(const sim::BakedReactionTable& table, int64_t x, int64_t y, int64_t z) {
        constexpr uint64_t SOLID_MICROMOLES = 20000000000;
        const bool hot = x >= 6 && x < 10 && y >= 2 && y < 6 && z >= 6 && z < 10;
        if (y < 2 || hot) {
            const std::vector<sim::SpeciesAmount> solid = {
                {.species = table.SpeciesId("sun_test_stone"), .amount = SOLID_MICROMOLES}};

            return sim::MakeReactionCell(table, solid, hot ? HOT_MK : test::SUN_ROOM_TEMPERATURE_MK);
        }

        return test::MakeSunCell(table, test::SunMaterial::Air);
    }

    struct World {
        std::string name;
        sim::MultiresNest nest;
        bool fineLeaves = false;  // 0.5 m より細かい葉がある(GPU は溢れの印 4・CPU は誤り)
    };

    // 根のレベル level(0・−1・−2)の根を roots 個並べた世界。refine なら根 0 の低い角の八分の一を 1 段細かくする
    World MakeWorld(const sim::BakedReactionTable& table, std::string name, int32_t level,
                    std::array<uint32_t, 3> roots, bool refine) {
        const uint32_t count = roots[0] * roots[1] * roots[2];
        const uint32_t blocks = count + 8;
        World world{.name = std::move(name),
                    .nest = sim::MakeMultiresNest({.worldBlocks = blocks,
                                                   .observerBlocks = 0,
                                                   .fractions = 9,
                                                   .pages = blocks,
                                                   .indexEntries = std::bit_ceil(2 * blocks),
                                                   .ledgerColumns = 1 + static_cast<uint32_t>(table.species.size()),
                                                   .rootLevel = level}),
                    .fineLeaves = refine && level == 0};
        sim::MultiresNest& nest = world.nest;
        const auto toHalfMeters = static_cast<uint32_t>(-level);  // セルの座標 → 0.5 m の座標
        std::vector<rx::RxCell> cells(mr::MR_BLOCK_CELLS);
        for (uint32_t rootZ = 0; rootZ < roots[2]; ++rootZ) {
            for (uint32_t rootY = 0; rootY < roots[1]; ++rootY) {
                for (uint32_t rootX = 0; rootX < roots[0]; ++rootX) {
                    for (uint32_t index = 0; index < mr::MR_BLOCK_CELLS; ++index) {
                        const int64_t x = (int64_t{rootX} * 8) + mr::MrCellX(index);
                        const int64_t y = (int64_t{rootY} * 8) + mr::MrCellY(index);
                        const int64_t z = (int64_t{rootZ} * 8) + mr::MrCellZ(index);
                        cells[index] = CellAt(table, x << toHalfMeters, y << toHalfMeters, z << toHalfMeters);
                    }

                    sim::PlaceRootBlock(nest, int64_t{rootX} * 8, int64_t{rootY} * 8, int64_t{rootZ} * 8, cells);
                }
            }
        }

        nest.edges.world = {.rootLevel = level,
                            .lowX = 0,
                            .lowY = 0,
                            .lowZ = 0,
                            .highX = int64_t{roots[0]} * 8,
                            .highY = int64_t{roots[1]} * 8,
                            .highZ = int64_t{roots[2]} * 8,
                            .top = 0,
                            .bottom = 0,
                            .sides = 0};
        if (refine) {
            const mr::MrRequest request = mr::MrMakeRequest(mr::MR_REQUEST_REFINE, level + 1, 2, 2, 2);
            sim::SubmitRequests(nest, std::span(&request, 1));
            sim::ProcessRequests(nest);
        }

        return world;
    }

    // 測りの印の段(START・木を読む写し・RC の 1 刻み)
    enum Stage : uint8_t {
        StageTreeGrid = 1,
        StageTick = 2,
        StageCount = 3,
    };

    struct GpuContext {
        ID3D12Device5* device = nullptr;
        gpu::ImmediateQueue* queue = nullptr;
        gpu::DebugRing* ring = nullptr;
        gpu::StageStamps* stamps = nullptr;
    };

    // 最初の食い違い(無ければ空)
    std::string Difference(const sim::CascadeTickResult& cpu, const sim::CascadeTickResult& gpu) {
        if (cpu.ledger != gpu.ledger)
            return "帳簿";

        if (cpu.raysPerLevel != gpu.raysPerLevel || cpu.steps != gpu.steps)
            return std::format("光線の本数(CPU {}・GPU {})・通ったセル(CPU {}・GPU {})", cpu.rays, gpu.rays, cpu.steps,
                               gpu.steps);

        if (cpu.emitted != gpu.emitted)
            return "放った(セルごと)";

        if (cpu.absorbed != gpu.absorbed)
            return "吸った(セルごと)";

        if (cpu.carry != gpu.carry)
            return std::format("持ち越し(CPU {} 個・GPU {} 個)", cpu.carry.entries.size(), gpu.carry.entries.size());

        return {};
    }

    struct WorldRun {
        sim::GpuMultires nest;
        sim::GpuCascadeLight light;
    };

    std::expected<WorldRun, std::string> PrepareWorld(const GpuContext& context, const sim::BakedReactionTable& table,
                                                      const World& world) {
        auto nest = sim::GpuMultires::Create(context.device, table, world.nest.capacity);
        if (!nest)
            return std::unexpected(nest.error());

        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr || !nest->RecordUpload(list, world.nest) || !context.queue->ExecuteAndWait())
            return std::unexpected("木を写せない");

        auto light = sim::GpuCascadeLight::CreateFromTree(context.device, *nest, world.nest, table, {},
                                                          {.faces = 1u << 14, .bags = 1u << 18, .carry = 1u << 16});
        if (!light)
            return std::unexpected(light.error());

        list = context.queue->Begin();
        if (list == nullptr || !light->RecordUpload(list, *nest, context.ring->GpuAddress(), {}) ||
            !context.queue->ExecuteAndWait())
            return std::unexpected("作業を写せない");

        return WorldRun{.nest = std::move(*nest), .light = std::move(*light)};
    }

    std::expected<sim::GpuCascadeTickResult, std::string> TickOnGpu(const GpuContext& context, WorldRun& run,
                                                                    uint64_t tick) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("リストを始められない");

        const D3D12_GPU_VIRTUAL_ADDRESS ring = context.ring->GpuAddress();
        context.ring->RecordBegin(list);
        context.stamps->Begin(0);
        context.stamps->Mark(list, gpu::StageStamps::START);
        if (!run.light.RecordTreeGrid(list, run.nest, ring))
            return std::unexpected("木を読む形で作っていない");

        context.stamps->Mark(list, StageTreeGrid);
        run.light.RecordTick(list, run.nest, ring, tick);
        context.stamps->Mark(list, StageTick);
        context.stamps->Resolve(list);
        run.light.RecordReadback(list);
        context.ring->RecordReadbackAndReset(list);
        if (!context.queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        context.stamps->Collect(0);
        const gpu::DebugRingContents debugOutput = context.ring->Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        return run.light.Read();
    }

    // 1. 刻みごとに CPU の写しの結果とビット一致(比べた刻みの数を返す)
    std::expected<uint64_t, std::string> CompareWorld(const GpuContext& context, const sim::BakedReactionTable& table,
                                                      const World& world) {
        const auto tree = sim::MakeCascadeTreeGrid(world.nest, table);
        if (!tree)
            return std::unexpected(tree.error());

        auto run = PrepareWorld(context, table, world);
        if (!run)
            return std::unexpected(run.error());

        sim::CascadeCarry carry;
        uint64_t emitting = 0;
        // 刻みごとの最小(GPU は刻みのたびに CPU のリファレンスを待つので、クロックが落ちた刻みの値は大きく揺れる)
        std::array<uint64_t, StageCount> fastest{};
        fastest.fill(UINT64_MAX);
        context.stamps->Reset();
        for (uint64_t tick = 0; tick < TICKS; ++tick) {
            const sim::CascadeTickResult cpu = sim::StepCascadeLight(tree->grid, carry, {}, tick);
            std::array<uint64_t, StageCount> before{};
            std::ranges::copy(context.stamps->Ticks().first(StageCount), before.begin());
            const auto gpu = TickOnGpu(context, *run, tick);
            if (!gpu)
                return std::unexpected(gpu.error());

            for (size_t stage = 0; stage < StageCount; ++stage)
                fastest[stage] = std::min(fastest[stage], context.stamps->Ticks()[stage] - before[stage]);

            if (gpu->overflow != 0)
                return std::unexpected(std::format("刻み {}: 溢れの印 {}", tick, gpu->overflow));

            if (const std::string difference = Difference(cpu, gpu->result); !difference.empty())
                return std::unexpected(std::format("刻み {}: {} が食い違う", tick, difference));

            emitting += cpu.ledger[rx::LT_BAND_LONG].emitted != 0 ? 1 : 0;
            carry = cpu.carry;
        }

        // 熱い塊が放たない場面では比べた意味が無い
        if (emitting == 0)
            return std::unexpected("どの刻みも放っていない(場面の作りの誤り)");

        // 費用(測り。HW の時だけ意味がある): 刻みの平均の µs
        uint64_t frequencyHz = 0;
        if (FAILED(context.queue->Native()->GetTimestampFrequency(&frequencyHz)))
            frequencyHz = 0;

        const auto frequency = static_cast<double>(frequencyHz);
        const auto averageUs = [&](Stage stage) {
            const std::span<const uint64_t> ticks = context.stamps->Ticks();
            return frequency > 0 ? static_cast<double>(ticks[stage]) * 1e6 / frequency / static_cast<double>(TICKS)
                                 : 0.0;
        };
        const auto fastestUs = [&](Stage stage) {
            return frequency > 0 ? static_cast<double>(fastest[stage]) * 1e6 / frequency : 0.0;
        };
        Log(Channel::Gpu, Level::Info,
            "{}: 升目 {} セル・葉の枠 {}・{} 刻み GPU の木を読む写しと CPU がビット一致(放った刻み {})。"
            "1 刻みの GPU 時間(平均 / 最小): 木を読む写し {:.1f} / {:.1f} µs・RC {:.1f} / {:.1f} µs",
            world.name, tree->grid.CellCount(), world.nest.capacity.worldBlocks, TICKS, emitting,
            averageUs(StageTreeGrid), fastestUs(StageTreeGrid), averageUs(StageTick), fastestUs(StageTick));

        return TICKS;
    }

    // 3. 費用(HW の時だけ): CPU のリファレンスを挟まず、温めの刻みの後に続けて流した刻みの GPU 時間(平均 / 最小)。
    // 1・2 は刻みのたびに CPU を待つので GPU のクロックが落ち、同じ場面でも 10 倍ほど揺れた(2026-10-29)。
    // 1 スレッド = 升目のセル 1 つ(索引で葉を引く)の写しの形もこの測りで比べ、葉ごとの形を残した(docs/perf.md)
    constexpr uint64_t WARM_TICKS = 32;
    constexpr uint64_t MEASURE_TICKS = 64;

    std::expected<void, std::string> MeasureWorld(const GpuContext& context, const sim::BakedReactionTable& table,
                                                  const World& world) {
        auto run = PrepareWorld(context, table, world);
        if (!run)
            return std::unexpected(run.error());

        std::array<uint64_t, StageCount> fastest{};
        fastest.fill(UINT64_MAX);
        for (uint64_t tick = 0; tick < WARM_TICKS + MEASURE_TICKS; ++tick) {
            if (tick == WARM_TICKS)
                context.stamps->Reset();

            std::array<uint64_t, StageCount> before{};
            std::ranges::copy(context.stamps->Ticks().first(StageCount), before.begin());
            const auto gpu = TickOnGpu(context, *run, tick);
            if (!gpu)
                return std::unexpected(gpu.error());

            if (tick < WARM_TICKS)
                continue;

            for (size_t stage = 0; stage < StageCount; ++stage)
                fastest[stage] = std::min(fastest[stage], context.stamps->Ticks()[stage] - before[stage]);
        }

        uint64_t frequencyHz = 0;
        if (FAILED(context.queue->Native()->GetTimestampFrequency(&frequencyHz)) || frequencyHz == 0)
            return std::unexpected("タイムスタンプの周波数を読めない");

        const auto frequency = static_cast<double>(frequencyHz);
        const auto averageUs = [&](Stage stage) {
            return static_cast<double>(context.stamps->Ticks()[stage]) * 1e6 / frequency /
                   static_cast<double>(MEASURE_TICKS);
        };
        const auto fastestUs = [&](Stage stage) {
            return static_cast<double>(fastest[stage]) * 1e6 / frequency;
        };
        Log(Channel::Gpu, Level::Info,
            "測り {}: 続けて {} 刻み(温め {})の GPU 時間(平均 / 最小): 木を読む写し {:.1f} / {:.1f} µs・"
            "RC {:.1f} / {:.1f} µs",
            world.name, MEASURE_TICKS, WARM_TICKS, averageUs(StageTreeGrid), fastestUs(StageTreeGrid),
            averageUs(StageTick), fastestUs(StageTick));

        return {};
    }

    // 2. 0.5 m より細かい葉: CPU は誤り・GPU は溢れの印 4
    std::expected<void, std::string> CheckFineLeaves(const GpuContext& context, const sim::BakedReactionTable& table,
                                                     const World& world) {
        if (sim::MakeCascadeTreeGrid(world.nest, table))
            return std::unexpected("CPU が細かい葉の木を誤りにしない");

        auto run = PrepareWorld(context, table, world);
        if (!run)
            return std::unexpected(run.error());

        const auto gpu = TickOnGpu(context, *run, 0);
        if (!gpu)
            return std::unexpected(gpu.error());

        if ((gpu->overflow & rx::RC_GPU_OVERFLOW_FINE_LEAF) == 0)
            return std::unexpected(std::format("GPU が細かい葉の印を立てない(溢れの印 {})", gpu->overflow));

        Log(Channel::Gpu, Level::Info, "{}: 細かい葉は CPU が誤り・GPU が溢れの印 {}", world.name, gpu->overflow);

        return {};
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_cascade_tree_grid_test [--warp] [--queue direct|compute] [--no-gbv]");
            return 2;
        }

        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_cascade_tree_grid_test: FAILED ({})", device.error());
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_cascade_tree_grid_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        auto stamps = gpu::StageStamps::Create(device->Get(), 1, 8, StageCount);
        if (!stamps) {
            Log(Channel::Gpu, Level::Error, "gpu_cascade_tree_grid_test: FAILED ({})", stamps.error());
            return 1;
        }

        const GpuContext context{.device = device->Get(), .queue = &*queue, .ring = &*ring, .stamps = &*stamps};
        const sim::BakedReactionTable table = test::MakeSunTable();
        const std::array<World, 4> worlds = {
            MakeWorld(table, "0.5 m の根", 0, {4, 2, 4}, false),
            MakeWorld(table, "1 m の根", -1, {2, 1, 2}, false),
            MakeWorld(table, "2 m の根", -2, {1, 1, 1}, false),
            MakeWorld(table, "1 m の根の一部を 0.5 m に", -1, {2, 1, 2}, true),
        };

        uint64_t compared = 0;
        const World fine = MakeWorld(table, "0.5 m の根の一部を 0.25 m に", 0, {4, 2, 4}, true);
        for (const World& world : worlds) {
            const auto ticks = CompareWorld(context, table, world);
            if (!ticks) {
                Log(Channel::Gpu, Level::Error, "gpu_cascade_tree_grid_test: FAILED({}: {})", world.name,
                    ticks.error());
                return 1;
            }

            compared += *ticks;
        }

        if (auto checked = CheckFineLeaves(context, table, fine); !checked) {
            Log(Channel::Gpu, Level::Error, "gpu_cascade_tree_grid_test: FAILED({}: {})", fine.name, checked.error());
            return 1;
        }

        if (options->adapter == gpu::AdapterKind::Hardware) {
            for (const World& world : worlds) {
                if (auto measured = MeasureWorld(context, table, world); !measured) {
                    Log(Channel::Gpu, Level::Error, "gpu_cascade_tree_grid_test: FAILED(測り {}: {})", world.name,
                        measured.error());
                    return 1;
                }
            }
        }

        if (!test::PassesValidation(*device, "gpu_cascade_tree_grid_test"))
            return 1;

        Log(Channel::Gpu, Level::Info,
            "gpu_cascade_tree_grid_test: OK({} 刻み分の場面で GPU の木を読む写しと CPU がビット一致)", compared);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
