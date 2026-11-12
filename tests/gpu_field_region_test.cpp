// gpu_field_region_test.cpp — GPU の世界(sim::GpuMultires)の欄の区画(T-1252 段 2 の 3 本目。common/multires_field_pages.hlsli の
// 「欄の区画」= u4 の層の置き場の後ろ。始まりの語 g_external2・欄の数 g_external3)を確かめる:
//   1. 写して戻す: 配り手の欄(頁を持つ根の頁の値・一様な根の一様の値)とこの刻みだけの欄を写し、何もせずに読み戻すと
//      置き場(pageOf・空きのスタック・頁の値・一様の値)と状態の要約が CPU と同じ。
//   2. 刻む: 同じ世界を 8 刻み(要求なし)CPU の StepNest と GPU の RecordStep で進め、毎刻み要約と欄の置き場がビット一致
//      (刻みの段は欄に触らない。区画を結んでも刻みが変わらない)。
//   3. 欄の無い世界: 区画を持つ GPU(fieldPages > 0)で本物の鎖の場面(細かくする・粗くする要求)を流し、毎刻み CPU とビット一致
//      (u4 に区画のバッファ・g_external2/3 を結んでも木の段の結果は変わらない)。層を持つ GPU でも同じ(層の置き場の後ろに区画)。
//   4. 区画を持たない GPU(fieldPages = 0)は配り手の欄のある世界を写さない(RecordUpload が false。D-428)。
//   5. 木の段(T-1252 段 2 の 4 本目): 配り手の欄を持つ世界で、頁を持つ根の 2 段の鎖・一様な根の 1 段を細かくし、1 段ずつ粗く戻す。
//      毎回の要求の処理の後に、置き場(pageOf・空きのスタック・頁の値・一様の値)と要約が CPU とビット一致(割り当ての段が要求の順に取る・
//      細かくする段の配り手の配り・親を覆う・粗くする段の配り手の和・解放の段が要求の順に返す)。
//   6. 広げる: 一様な根の一様な子の配り手の値を変えてから写し、粗くすると親が頁に広がる(TreeSettle の判定・広げる親の欄の頁)。
//      この刻みだけの欄の 0 の頁を持つ子を粗くして、その頁を解放の段が返す。
// 影の鎖・畳む・広げる段(TreeExpand)・集めて書く段が区画を読むのは T-1252 の段 2 の残り。引数は gpu_test_options.h。
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "multires_activity_scene.h"
#include "reaction_test_table.h"
#include "sim/gpu_multires.h"
#include "sim/gpu_multires_fields.h"
#include "sim/multires_fields.h"
#include "sim/multires_state.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::state_field;

namespace {

    constexpr uint32_t FIELD_PAGES = 8;   // 欄ごとの欄の頁の数(区画の容量)
    constexpr uint32_t LAYER_PAGES = 16;  // 層を持つ GPU の層の頁の数(場面 3)
    constexpr uint32_t OWNER_COMPONENTS = 3;
    constexpr uint64_t STEP_TICKS = 8;

    struct FieldWorld {
        sim::MultiresNest nest;
        uint32_t owned = sim::MULTIRES_NO_FIELD;
        uint32_t tick = sim::MULTIRES_NO_FIELD;
    };

    int64_t OwnerValue(uint32_t index, uint32_t component) {
        return (static_cast<int64_t>(index) * 37) - (static_cast<int64_t>(component) * 1001) + 5;
    }

    // 頁を持つ根(本物の場面の根のセル)と一様な根(反応しない空気)の 2 つ。配り手の欄は根の頁の全部のセルと一様な根の一様の値に書く
    FieldWorld MakeFieldWorld(const sim::BakedReactionTable& table) {
        FieldWorld world{
            .nest = sim::MakeMultiresNest(test::MakeMultiresCapacity(
                table, test::MULTIRES_WORLD_BLOCKS, test::MULTIRES_LEVELS, test::MULTIRES_FRACTION_CAPACITY))};
        sim::MultiresNest& nest = world.nest;
        world.owned = sim::RegisterNestField(nest, "owned",
                                             SfMakeOwnerDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_FLAG_HASHED,
                                                             OWNER_COMPONENTS, SF_OWNER_TEST));
        world.tick = sim::RegisterNestField(nest, "source",
                                            SfMakeTickDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, 2));

        const uint32_t paged = sim::PlaceRootBlock(nest, 0, 0, 0, test::MakeMultiresRootCells(table));
        // 一様な根は反応しない空気(刻みで広がらない。広げる段はまだ区画を読まない = 段 2 の残り)
        const std::vector<RxCell> air(MR_BLOCK_CELLS, test::MakeAirCell(table));
        const uint32_t uniform = sim::PlaceRootBlock(nest, 8, 0, 0, air);
        if (!MrIsUniform(nest.blocks[paged])) {
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                for (uint32_t c = 0; c < OWNER_COMPONENTS; ++c)
                    sim::StoreNestField(nest, world.owned, paged, index, c, OwnerValue(index, c));
            }
        }

        if (MrIsUniform(nest.blocks[uniform])) {
            for (uint32_t c = 0; c < OWNER_COMPONENTS; ++c)
                sim::StoreNestFieldUniform(nest, world.owned, uniform, c, OwnerValue(7, c));
        }

        return world;
    }

    // 載せる欄の置き場が同じか(頁の番号・空きのスタック・頁の値・一様の値まで)
    std::expected<void, std::string> CompareFields(const sim::MultiresNest& cpu, const sim::MultiresNest& read,
                                                   std::string_view where) {
        if (sim::HashWholeNest(cpu) != sim::HashWholeNest(read)) {
            const bool cellsSame = cpu.blocks.size() == read.blocks.size() &&
                                   std::memcmp(cpu.blocks.data(), read.blocks.data(),
                                               cpu.blocks.size() * sizeof(MrBlock)) == 0;
            return std::unexpected(
                std::format("{}: CPU と GPU の状態の要約が違う(枠の見出し {})", where, cellsSame ? "同じ" : "違う"));
        }

        for (uint32_t field = 0; field < cpu.fields.size(); ++field) {
            if (!sim::GpuCarriesField(cpu.fields[field]))
                continue;

            const sim::MultiresFieldStore& a = cpu.fields[field].store;
            const sim::MultiresFieldStore& b = read.fields[field].store;
            if (a.pageOf != b.pageOf || a.freePages != b.freePages || a.pages != b.pages || a.uniform != b.uniform)
                return std::unexpected(std::format("{}: 欄 {} の置き場が違う", where, cpu.fields[field].name));
        }

        return {};
    }

    // record を 1 本のリストで走らせ、target(登録した欄を持つ写し)へ読み戻す
    template <typename Record>
    std::expected<void, std::string> Execute(gpu::ImmediateQueue& queue, gpu::DebugRing& ring, sim::GpuMultires& gpu,
                                             const Record& record, sim::MultiresNest& target) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        ring.RecordBegin(list);
        const bool recorded = record(list);
        gpu.RecordReadback(list);
        ring.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        if (!recorded)
            return std::unexpected("記録できない");

        if (const gpu::DebugRingContents output = ring.Drain(); output.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", output.assertCount));

        if (!gpu.Read(target))
            return std::unexpected("読み戻せない");

        return {};
    }

    sim::GpuMultiresOptions RegionOptions(uint32_t layerPages) {
        sim::GpuMultiresOptions options;
        options.fieldPages = FIELD_PAGES;
        options.layerPages = layerPages;

        return options;
    }

    // --- 1・2. 写して戻す・刻む ---
    std::expected<void, std::string> RunCarried(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                const sim::BakedReactionTable& table) {
        FieldWorld world = MakeFieldWorld(table);
        const sim::MultiresFieldStore& owned = world.nest.fields[world.owned].store;
        if (sim::UsedNestFieldPages(world.nest, world.owned) == 0 || owned.uniform.empty())
            return std::unexpected("場面の確かめ: 配り手の欄が頁も一様の値も持たない");

        auto gpu = sim::GpuMultires::Create(device, table, world.nest.capacity, RegionOptions(0));
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read = world.nest;
        const auto upload = [&](ID3D12GraphicsCommandList10* list) {
            return gpu->RecordUpload(list, world.nest);
        };
        if (auto ran = Execute(queue, ring, *gpu, upload, read); !ran)
            return std::unexpected(std::format("写して戻す: {}", ran.error()));

        if (auto compared = CompareFields(world.nest, read, "写して戻す"); !compared)
            return compared;

        for (uint64_t tick = 0; tick < STEP_TICKS; ++tick) {
            const auto step = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, world.nest))
                    return false;

                gpu->RecordStep(list, ring.GpuAddress(), test::MULTIRES_TEST_SEED, tick);
                return true;
            };
            if (auto ran = Execute(queue, ring, *gpu, step, read); !ran)
                return std::unexpected(std::format("刻み {}: {}", tick, ran.error()));

            sim::StepNest(world.nest, table, test::MULTIRES_TEST_SEED, tick);
            if (auto compared = CompareFields(world.nest, read, std::format("刻み {}", tick)); !compared)
                return compared;
        }

        Log(Channel::Gpu, Level::Info, "写して戻す・{} 刻み: 一致(配り手の欄の頁 {}・欄 {} 個)", STEP_TICKS,
            sim::UsedNestFieldPages(world.nest, world.owned), sim::GpuCarriedFieldCount(world.nest));

        return {};
    }

    // --- 3. 欄の無い世界(本物の鎖の場面)---
    std::expected<void, std::string> RunPlain(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                              const sim::BakedReactionTable& table, uint32_t layerPages) {
        using test::MultiresScenario;
        sim::MultiresNest cpu = test::MakeMultiresNestForTest(table);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, RegionOptions(layerPages));
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read = cpu;
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                if (!gpu->RecordRequests(list, test::MultiresRequestsAt(MultiresScenario::Real, tick)))
                    return false;

                gpu->RecordProcessRequests(list, ring.GpuAddress());
                gpu->RecordStep(list, ring.GpuAddress(), test::MULTIRES_TEST_SEED, tick);
                return true;
            };
            if (auto ran = Execute(queue, ring, *gpu, record, read); !ran)
                return std::unexpected(std::format("欄の無い世界 刻み {}: {}", tick, ran.error()));

            test::StepMultiresScene(cpu, table, MultiresScenario::Real, tick);
            if (sim::HashWholeNest(cpu) != sim::HashWholeNest(read))
                return std::unexpected(
                    std::format("欄の無い世界(層の頁 {})刻み {}: CPU と GPU が違う", layerPages, tick));
        }

        Log(Channel::Gpu, Level::Info, "欄の無い世界(区画あり・層の頁 {}): 本物の鎖 {} 刻み毎回一致", layerPages,
            test::MULTIRES_END_TICK);

        return {};
    }

    // --- 5・6. 木の段の配り手の欄 ---

    // 頁を持つ根(原点 0)のセル (3, 4, 5) の中の 2 段目の点・一様な根(原点 8)のセル (9, 2, 3) の中の 1 段目の点
    constexpr int64_t CHAIN_X = (3 * 4) + 1;
    constexpr int64_t CHAIN_Y = (4 * 4) + 2;
    constexpr int64_t CHAIN_Z = (5 * 4) + 3;
    constexpr int64_t UNIFORM_X = (9 * 2) + 1;
    constexpr int64_t UNIFORM_Y = 2 * 2;
    constexpr int64_t UNIFORM_Z = (3 * 2) + 1;

    MrRequest ChainRequest(uint32_t op, int32_t level) {
        const auto shift = static_cast<uint32_t>(2 - level);

        return MrMakeRequest(op, level, CHAIN_X >> shift, CHAIN_Y >> shift, CHAIN_Z >> shift);
    }

    MrRequest UniformRequest(uint32_t op) {
        return MrMakeRequest(op, 1, UNIFORM_X, UNIFORM_Y, UNIFORM_Z);
    }

    // 回 round の要求(0: 細かく・1: 2 段目と一様な子を粗く・2: 1 段目を粗く)
    std::vector<MrRequest> OwnerTreeRequests(uint32_t round) {
        if (round == 0)
            return {ChainRequest(MR_REQUEST_REFINE, 2), UniformRequest(MR_REQUEST_REFINE)};

        if (round == 1)
            return {ChainRequest(MR_REQUEST_COARSEN, 2), UniformRequest(MR_REQUEST_COARSEN)};

        return {ChainRequest(MR_REQUEST_COARSEN, 1)};
    }

    // CPU と GPU で要求を処理して比べる(GPU は前の回の結果のまま続ける。upload なら先に写す)
    std::expected<void, std::string> ProcessBoth(gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                 sim::GpuMultires& gpu, sim::MultiresNest& cpu, sim::MultiresNest& read,
                                                 std::span<const MrRequest> requests, bool upload,
                                                 std::string_view where) {
        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            if (upload && !gpu.RecordUpload(list, cpu))
                return false;

            if (!gpu.RecordRequests(list, requests))
                return false;

            gpu.RecordProcessRequests(list, ring.GpuAddress());
            return true;
        };
        if (auto ran = Execute(queue, ring, gpu, record, read); !ran)
            return std::unexpected(std::format("{}: {}", where, ran.error()));

        sim::SubmitRequests(cpu, requests);
        sim::ProcessRequests(cpu);

        return CompareFields(cpu, read, where);
    }

    // 世界の中の、レベル level の本物の枠で一様か頁を持つか uniform のもの(無ければ MR_NO_BLOCK)
    uint32_t FindRealBlock(const sim::MultiresNest& nest, int32_t level, bool uniform) {
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind == MR_BLOCK_REAL && block.level == level && MrIsUniform(block) == uniform)
                return slot;
        }

        return MR_NO_BLOCK;
    }

    std::expected<void, std::string> RunOwnerTree(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                  gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        FieldWorld world = MakeFieldWorld(table);
        auto gpu = sim::GpuMultires::Create(device, table, world.nest.capacity, RegionOptions(0));
        if (!gpu)
            return std::unexpected(gpu.error());

        // --- 5. 細かく → 粗く ---
        sim::MultiresNest read = world.nest;
        const uint32_t pagesBefore = sim::UsedNestFieldPages(world.nest, world.owned);
        for (uint32_t round = 0; round < 3; ++round) {
            const std::vector<MrRequest> requests = OwnerTreeRequests(round);
            if (auto both = ProcessBoth(queue, ring, *gpu, world.nest, read, requests, round == 0,
                                        std::format("木の段 {} 回目", round));
                !both)
                return both;

            if (round == 0 && sim::UsedNestFieldPages(world.nest, world.owned) != pagesBefore + 2)
                return std::unexpected(std::format("場面の確かめ: 細かくした鎖の子が欄の頁を持たない(頁 {} → {})",
                                                   pagesBefore, sim::UsedNestFieldPages(world.nest, world.owned)));
        }

        if (sim::UsedNestFieldPages(world.nest, world.owned) != pagesBefore)
            return std::unexpected("場面の確かめ: 粗く戻した後に欄の頁が残る");

        Log(Channel::Gpu, Level::Info, "木の段: 配り手の欄の細かく(鎖 2 段・一様 1 段)→ 粗く 3 回とも一致(空きの頁 {})",
            world.nest.fields[world.owned].store.freePages.size());

        // --- 6. 広げる・この刻みだけの欄の頁を返す ---
        FieldWorld expand = MakeFieldWorld(table);
        sim::SubmitRequests(expand.nest, OwnerTreeRequests(0));
        sim::ProcessRequests(expand.nest);
        const uint32_t uniformChild = FindRealBlock(expand.nest, 1, true);
        const uint32_t chainChild = FindRealBlock(expand.nest, 2, false);
        if (uniformChild == MR_NO_BLOCK || chainChild == MR_NO_BLOCK)
            return std::unexpected("場面の確かめ: 細かくした子が見つからない");

        const uint32_t uniformRoot = expand.nest.blocks[uniformChild].parent;
        for (uint32_t c = 0; c < OWNER_COMPONENTS; ++c)
            sim::StoreNestFieldUniform(expand.nest, expand.owned, uniformChild, c, OwnerValue(11, c) * 3);

        sim::StoreNestField(expand.nest, expand.tick, chainChild, 9, 1, 5);  // この刻みだけの欄の 0 の頁を作る
        sim::StoreNestField(expand.nest, expand.tick, chainChild, 9, 1, 0);

        auto expandGpu = sim::GpuMultires::Create(device, table, expand.nest.capacity, RegionOptions(0));
        if (!expandGpu)
            return std::unexpected(expandGpu.error());

        sim::MultiresNest expandRead = expand.nest;
        if (auto both = ProcessBoth(queue, ring, *expandGpu, expand.nest, expandRead, OwnerTreeRequests(1), true,
                                    "広げる");
            !both)
            return both;

        if (MrIsUniform(expand.nest.blocks[uniformRoot]) || sim::UsedNestFieldPages(expand.nest, expand.tick) != 0)
            return std::unexpected("場面の確かめ: 一様な根が広がらない・この刻みだけの欄の頁が返らない");

        Log(Channel::Gpu, Level::Info, "木の段: 一様な親を広げる・この刻みだけの欄の頁を返す で一致");

        return {};
    }

    // --- 4. 区画を持たない GPU は断る ---
    std::expected<void, std::string> RunRefusal(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                const sim::BakedReactionTable& table) {
        FieldWorld world = MakeFieldWorld(table);
        auto gpu = sim::GpuMultires::Create(device, table, world.nest.capacity, sim::GpuMultiresOptions{});
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read = world.nest;
        const auto upload = [&](ID3D12GraphicsCommandList10* list) {
            return gpu->RecordUpload(list, world.nest);
        };
        if (auto ran = Execute(queue, ring, *gpu, upload, read); ran || ran.error() != "記録できない")
            return std::unexpected("区画を持たない GPU が配り手の欄のある世界を写した");

        Log(Channel::Gpu, Level::Info, "区画を持たない GPU は配り手の欄のある世界を写さない(期待どおり)");

        return {};
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_field_region_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_field_region_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_field_region_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_field_region_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        std::expected<void, std::string> ran = RunCarried(device->Get(), *queue, *ring, *table);
        if (ran)
            ran = RunPlain(device->Get(), *queue, *ring, *table, 0);
        if (ran)
            ran = RunPlain(device->Get(), *queue, *ring, *table, LAYER_PAGES);
        if (ran)
            ran = RunRefusal(device->Get(), *queue, *ring, *table);
        if (ran)
            ran = RunOwnerTree(device->Get(), *queue, *ring, *table);
        if (!ran) {
            Log(Channel::Gpu, Level::Error, "gpu_field_region_test: FAILED ({})", ran.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_field_region_test"))
            return 1;

        Log(Channel::Gpu, Level::Info,
            "gpu_field_region_test: OK(欄の区画を写して戻す・刻む・欄の無い世界・断り・木の段の配り手の欄で GPU と CPU "
            "がビット一致)");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
