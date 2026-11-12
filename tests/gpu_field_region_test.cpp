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
//   7. 影の鎖(T-1252 段 2 の 5 本目): 頁を持つ根の 2 段・一様な根の 1 段(同じ影の枠 = 前の欄の頁を返して取り直す)を影に細かくし、
//      影の欄の値を変え・親のこの刻みだけの欄に 0 でない値を書いてから引き戻す(影が欄の頁を持たない所は新しく取る)。毎回 CPU とビット一致。
//   8. 畳む: 頁を持つ根(セルが一様でない)と、頁に広げた一様な根(セルも配り手の欄も一様)を、ちょうど静かになった刻みで畳む。
//      広げた根は畳まれて欄の頁を返し、頁を持つ根は畳まれずに全部 0 のこの刻みだけの欄の頁だけを返す(CPU の ReleaseZeroPages)。
//   9. 刻みの広げる段と畳む段: 一様な木箱の根(配り手の欄の一様の値を持つ)が刻みで頁に広がり(TreeExpand が欄の頁を取って一様の値で埋める)、
//      燃え尽きて畳まれる(欄の頁を返す)までの活性の刻み(multires_uniform_scene の場面)を毎刻み CPU とビット一致。
//   10. 集めて書く段(T-1252 段 2 の 6 本目): 配り手の欄へ頁を持つ根・一様な根(広げてから)に足し、この刻みだけの欄へ予算 1 で足す
//      (1 つは頁を取って書き、1 つは作業に残る)。置き場・要約・報告・作業の残り・種(書いた枠をつつく)が CPU の GatherNestField とビット一致。
//   11. 保存量の配り手の欄(T-1252 段 2 の 7 本目): 5 の木の段を保存量の配り手の欄で。細かくした後に鎖の子の値を 8 で割り切れない値に変えて写し、
//      粗くする余りが世界の帳簿の「粗くした端数・出」の列(NestFieldLedgerBase + NestConservedIndex)に CPU と同じ段・同じ列で載る。
//      帳簿・置き場・要約が CPU とビット一致・保存の検査(ComputeConservedTotals)が粗くする前後で同じ。
// 引数は gpu_test_options.h。
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "multires_activity_scene.h"
#include "multires_uniform_scene.h"
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
    FieldWorld MakeFieldWorld(const sim::BakedReactionTable& table, uint32_t ownerFlags = SF_FLAG_HASHED) {
        FieldWorld world{
            .nest = sim::MakeMultiresNest(test::MakeMultiresCapacity(
                table, test::MULTIRES_WORLD_BLOCKS, test::MULTIRES_LEVELS, test::MULTIRES_FRACTION_CAPACITY))};
        sim::MultiresNest& nest = world.nest;
        world.owned = sim::RegisterNestField(
            nest, "owned",
            SfMakeOwnerDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, ownerFlags, OWNER_COMPONENTS, SF_OWNER_TEST));
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

    // --- 7. 影の鎖 ---

    // 影の鎖を CPU と GPU で細かくして比べる
    std::expected<void, std::string> ShadowBoth(gpu::ImmediateQueue& queue, gpu::DebugRing& ring, sim::GpuMultires& gpu,
                                                sim::MultiresNest& cpu, sim::MultiresNest& read, uint32_t parentSlot,
                                                uint32_t levels, const sim::MultiresPoint& point, bool upload,
                                                std::string_view where) {
        const uint32_t shadowSlot = cpu.capacity.worldBlocks;
        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            if (upload && !gpu.RecordUpload(list, cpu))
                return false;

            gpu.RecordRefineShadow(list, ring.GpuAddress(), parentSlot, shadowSlot, levels, point);
            return true;
        };
        if (auto ran = Execute(queue, ring, gpu, record, read); !ran)
            return std::unexpected(std::format("{}: {}", where, ran.error()));

        sim::RefineShadowChain(cpu, parentSlot, shadowSlot, levels, point);

        return CompareFields(cpu, read, where);
    }

    std::expected<void, std::string> RunShadow(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                               const sim::BakedReactionTable& table) {
        FieldWorld world = MakeFieldWorld(table);
        sim::MultiresNest& cpu = world.nest;
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, RegionOptions(0));
        if (!gpu)
            return std::unexpected(gpu.error());

        const uint32_t paged = FindRealBlock(cpu, 0, false);
        const uint32_t uniform = FindRealBlock(cpu, 0, true);
        const sim::MultiresPoint chainPoint{.x = CHAIN_X, .y = CHAIN_Y, .z = CHAIN_Z, .level = 2};
        const sim::MultiresPoint uniformPoint{.x = UNIFORM_X, .y = UNIFORM_Y, .z = UNIFORM_Z, .level = 1};
        const uint32_t shadowSlot = cpu.capacity.worldBlocks;
        sim::MultiresNest read = cpu;
        if (auto both = ShadowBoth(queue, ring, *gpu, cpu, read, paged, 2, chainPoint, true, "影 頁を持つ根の 2 段");
            !both)
            return both;

        const uint32_t shadowPages = sim::UsedNestFieldPages(cpu, world.owned);
        if (auto both = ShadowBoth(queue, ring, *gpu, cpu, read, uniform, 1, uniformPoint, false, "影 一様な根の 1 段");
            !both)
            return both;

        if (auto both = ShadowBoth(queue, ring, *gpu, cpu, read, paged, 2, chainPoint, false, "影 もう一度 2 段");
            !both)
            return both;

        // --- 引き戻す: 影の配り手の欄を変え、親(頁を持つ根)のこの刻みだけの欄に 0 でない値(影はこの刻みだけの欄の頁を持たない)---
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; index += 7) {
            for (uint32_t c = 0; c < OWNER_COMPONENTS; ++c)
                sim::StoreNestField(cpu, world.owned, shadowSlot + 1, index, c, OwnerValue(index, c) * -5);
        }

        const uint32_t octantCell = MrOctantCell(cpu.blocks[shadowSlot].parentOctant, 3);
        sim::StoreNestField(cpu, world.tick, paged, octantCell, 1, 4001);
        const uint32_t tickPagesBefore = sim::UsedNestFieldPages(cpu, world.tick);
        const auto pull = [&](ID3D12GraphicsCommandList10* list) {
            if (!gpu->RecordUpload(list, cpu))
                return false;

            gpu->RecordPullBack(list, ring.GpuAddress(), shadowSlot, 2);
            return true;
        };
        if (auto ran = Execute(queue, ring, *gpu, pull, read); !ran)
            return std::unexpected(std::format("影の引き戻し: {}", ran.error()));

        sim::PullBackShadowChain(cpu, table, shadowSlot, 2);
        if (auto compared = CompareFields(cpu, read, "影の引き戻し"); !compared)
            return compared;

        if (shadowPages < 3 || sim::UsedNestFieldPages(cpu, world.tick) <= tickPagesBefore)
            return std::unexpected(
                std::format("場面の確かめ: 影が欄の頁を持たない(配り手 {})・引き戻しで影が頁を取らない", shadowPages));

        Log(Channel::Gpu, Level::Info,
            "影の鎖: 細かく 3 回(取り直しを含む)・引き戻し(新しい頁を含む)で一致(配り手の欄の頁 {})",
            sim::UsedNestFieldPages(cpu, world.owned));

        return {};
    }

    // --- 8. 畳む ---

    constexpr uint64_t FOLD_TICK = 30;

    // ちょうど静かになった刻みが tick になるように、枠 slot の忙しさの印を書く(MrWantsFoldCheck)
    void MarkQuietAt(sim::MultiresNest& nest, uint32_t slot, uint64_t tick) {
        nest.blocks[slot].busyTick = MrChangeMark(tick) - (MR_QUIET_TICKS + 1);
    }

    std::expected<void, std::string> RunFold(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                             const sim::BakedReactionTable& table) {
        FieldWorld world = MakeFieldWorld(table);
        sim::MultiresNest& cpu = world.nest;
        const uint32_t paged = FindRealBlock(cpu, 0, false);
        const uint32_t uniform = FindRealBlock(cpu, 0, true);
        if (!sim::ExpandNestPage(cpu, uniform))
            return std::unexpected("場面の確かめ: 一様な根を頁に広げられない");

        // この刻みだけの欄の全部 0 の頁を両方の根に
        for (const uint32_t slot : {paged, uniform}) {
            sim::StoreNestField(cpu, world.tick, slot, 9, 1, 5);
            sim::StoreNestField(cpu, world.tick, slot, 9, 1, 0);
            MarkQuietAt(cpu, slot, FOLD_TICK);
        }

        const uint32_t ownedBefore = sim::UsedNestFieldPages(cpu, world.owned);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, RegionOptions(0));
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read = cpu;
        const auto fold = [&](ID3D12GraphicsCommandList10* list) {
            if (!gpu->RecordUpload(list, cpu))
                return false;

            gpu->RecordFoldPages(list, ring.GpuAddress(), FOLD_TICK);
            return true;
        };
        if (auto ran = Execute(queue, ring, *gpu, fold, read); !ran)
            return std::unexpected(std::format("畳む: {}", ran.error()));

        sim::FoldQuietPages(cpu, FOLD_TICK);
        if (auto compared = CompareFields(cpu, read, "畳む"); !compared)
            return compared;

        if (ownedBefore != 2 || !MrIsUniform(cpu.blocks[uniform]) || MrIsUniform(cpu.blocks[paged]) ||
            sim::UsedNestFieldPages(cpu, world.owned) != 1 || sim::UsedNestFieldPages(cpu, world.tick) != 0)
            return std::unexpected(std::format(
                "場面の確かめ: 広げた根だけが畳まれ・全部 0 の欄の頁が返る、になっていない(配り手の頁 {} → "
                "{}・この刻みだけ {})",
                ownedBefore, sim::UsedNestFieldPages(cpu, world.owned), sim::UsedNestFieldPages(cpu, world.tick)));

        Log(Channel::Gpu, Level::Info, "畳む: 一様な頁を畳んで欄の頁を返す・畳めない頁の全部 0 の欄の頁を返す で一致");

        return {};
    }

    // --- 9. 刻みの広げる段と畳む段(multires_uniform_scene)---
    std::expected<void, std::string> RunUniformTicks(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                     gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = test::MakeUniformNest(table, test::UNIFORM_WORLD_BLOCKS);
        const uint32_t owned = sim::RegisterNestField(cpu, "owned",
                                                      SfMakeOwnerDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE,
                                                                      SF_FLAG_HASHED, OWNER_COMPONENTS, SF_OWNER_TEST));
        static_cast<void>(
            sim::RegisterNestField(cpu, "source", SfMakeTickDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, 2)));
        for (uint32_t slot = 0; slot < test::UNIFORM_ROOTS; ++slot) {
            for (uint32_t c = 0; c < OWNER_COMPONENTS; ++c)
                sim::StoreNestFieldUniform(cpu, owned, slot, c, OwnerValue(slot + 3, c));
        }

        sim::GpuMultiresOptions options = RegionOptions(0);
        options.activity = true;
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, options);
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read = cpu;
        uint32_t mostPages = 0;
        for (uint64_t tick = 0; tick < test::UNIFORM_TICKS; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                if (!gpu->RecordRequests(list, test::UniformRequestsAt(tick)))
                    return false;

                gpu->RecordFoldPages(list, ring.GpuAddress(), tick);
                gpu->RecordQuietRequests(list, ring.GpuAddress(), tick);
                gpu->RecordProcessRequests(list, ring.GpuAddress());
                return gpu->RecordStepActive(list, ring.GpuAddress(), test::STRESS_SEED, tick);
            };
            if (auto ran = Execute(queue, ring, *gpu, record, read); !ran)
                return std::unexpected(std::format("一様な根の刻み {}: {}", tick, ran.error()));

            test::BeginUniformTick(cpu, tick);
            sim::StepActive(cpu, table, test::STRESS_SEED, tick);
            if (auto compared = CompareFields(cpu, read, std::format("一様な根の刻み {}", tick)); !compared)
                return compared;

            mostPages = std::max(mostPages, sim::UsedNestFieldPages(cpu, owned));
        }

        if (cpu.counters[MR_COUNTER_EXPANDED] == 0 || cpu.counters[MR_COUNTER_FOLDED] == 0 || mostPages == 0 ||
            sim::UsedNestFieldPages(cpu, owned) != 0)
            return std::unexpected(
                std::format("場面の確かめ: 広げて欄の頁を取り・畳んで返す、になっていない(広げた {}・畳んだ {}・"
                            "欄の頁 最多 {} → {})",
                            cpu.counters[MR_COUNTER_EXPANDED], cpu.counters[MR_COUNTER_FOLDED], mostPages,
                            sim::UsedNestFieldPages(cpu, owned)));

        Log(Channel::Gpu, Level::Info, "刻みの広げる・畳む: {} 刻み毎回一致(広げた {}・畳んだ {}・欄の頁 最多 {})",
            test::UNIFORM_TICKS, cpu.counters[MR_COUNTER_EXPANDED], cpu.counters[MR_COUNTER_FOLDED], mostPages);

        return {};
    }

    // --- 10. 集めて書く段(T-1252 段 2 の 6 本目。GpuMultires::RecordGather)---

    // 書き手の作業(GPU の作業の並び)を写す・読み戻すバッファ
    struct GatherWorkBuffer {
        ComPtr<ID3D12Resource> upload;
        ComPtr<ID3D12Resource> buffer;
        ComPtr<ID3D12Resource> readback;
        size_t bytes = 0;
    };

    GatherWorkBuffer CreateGatherWork(ID3D12Device* device, std::span<const uint32_t> words) {
        const auto bytes = std::as_bytes(words);
        return {.upload = gpu::CreateFilledUpload(device, bytes),
                .buffer = gpu::CreateBuffer(device, bytes.size(), gpu::BufferKind::UnorderedAccess),
                .readback = gpu::CreateBuffer(device, bytes.size(), gpu::BufferKind::Readback),
                .bytes = bytes.size()};
    }

    // 作業へ足す値(枠ごと・セルごとに違う。負の値も)
    int64_t GatherValue(uint32_t slot, uint32_t index, uint32_t component) {
        return (static_cast<int64_t>(index) * 11) - (static_cast<int64_t>(slot) * 3) +
               (static_cast<int64_t>(component) * 100003) - 2500;
    }

    // 欄 field の作業: 一様な根を先に足す(作業のブロックの番号の順 ≠ 枠の順)
    sim::NestGatherWork MakeRootsWork(const sim::MultiresNest& nest, uint32_t field, uint32_t paged, uint32_t uniform) {
        sim::NestGatherWork work = sim::MakeNestGatherWork(nest, field);
        for (const uint32_t slot : {uniform, paged}) {
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; index += 5) {
                for (uint32_t c = 0; c < work.components; ++c)
                    sim::AddNestGatherWork(work, slot, index, c, GatherValue(slot, index, c));
            }
        }

        return work;
    }

    bool SameReport(const sim::NestGatherReport& a, const sim::NestGatherReport& b) {
        return a.blocks == b.blocks && a.pagesTaken == b.pagesTaken && a.expanded == b.expanded &&
               a.pageShortage == b.pageShortage && a.cellPageShortage == b.cellPageShortage;
    }

    std::string ReportText(const sim::NestGatherReport& report) {
        return std::format("書いた {}・取った {}・広げた {}・欄の頁の不足 {}・セルの頁の不足 {}", report.blocks,
                           report.pagesTaken, report.expanded, report.pageShortage, report.cellPageShortage);
    }

    // CPU の GatherNestField と GPU の RecordGather を 1 回ずつ。first なら写してから
    std::expected<sim::NestGatherReport, std::string> GatherBoth(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                                 gpu::DebugRing& ring, sim::GpuMultires& gpu,
                                                                 sim::MultiresNest& cpu, sim::MultiresNest& read,
                                                                 sim::NestGatherWork work, bool first,
                                                                 std::string_view where) {
        sim::NestGatherWork gpuWork = work;
        const std::vector<uint32_t> image = sim::MakeGpuGatherWorkImage(work);
        const GatherWorkBuffer buffer = CreateGatherWork(device, image);
        if (!buffer.upload || !buffer.buffer || !buffer.readback)
            return std::unexpected(std::format("{}: 作業のバッファを作れない", where));

        const sim::MultiresNest before = cpu;
        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            if (first && !gpu.RecordUpload(list, before))
                return false;

            list->CopyBufferRegion(buffer.buffer.Get(), 0, buffer.upload.Get(), 0, buffer.bytes);
            const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(buffer.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &toUav);
            if (!gpu.RecordGather(list, ring.GpuAddress(), work.field, buffer.buffer->GetGPUVirtualAddress()))
                return false;

            gpu::RecordCopyToReadback(list, buffer.buffer.Get(), buffer.readback.Get());
            return true;
        };
        if (auto ran = Execute(queue, ring, gpu, record, read); !ran)
            return std::unexpected(std::format("{}: {}", where, ran.error()));

        const sim::NestGatherReport cpuReport = sim::GatherNestField(cpu, work);
        if (auto compared = CompareFields(cpu, read, where); !compared)
            return std::unexpected(compared.error());

        // --- 報告・作業の残り・種(書いた枠をつつく)---
        const std::optional<sim::NestGatherReport> gpuReport = gpu.ReadGatherReport(work.field);
        if (!gpuReport || !SameReport(cpuReport, *gpuReport))
            return std::unexpected(std::format("{}: 報告が違う(CPU {} / GPU {})", where, ReportText(cpuReport),
                                               gpuReport ? ReportText(*gpuReport) : "読めない"));

        std::vector<uint32_t> words(image.size());
        std::vector<uint32_t> results;
        if (!gpu::ReadBuffer(buffer.readback.Get(), std::as_writable_bytes(std::span(words))))
            return std::unexpected(std::format("{}: 作業を読み戻せない", where));

        sim::ApplyGpuGatherWorkImage(gpuWork, words, results);
        if (gpuWork.values != work.values)
            return std::unexpected(std::format("{}: 作業の残りが違う", where));

        const std::optional<sim::GpuMultires::ActivitySeeds> seeds = gpu.ReadSeeds();
        std::vector<uint32_t> cpuSeeds;
        for (uint32_t slot = 0; slot < cpu.capacity.worldBlocks; ++slot) {
            if (cpu.seeds[slot] != 0)
                cpuSeeds.push_back(slot);
        }

        if (!seeds || seeds->slots != cpuSeeds)
            return std::unexpected(std::format("{}: 種(書いた枠をつつく)が違う", where));

        return cpuReport;
    }

    std::expected<void, std::string> RunGather(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                               const sim::BakedReactionTable& table) {
        FieldWorld world = MakeFieldWorld(table);
        sim::MultiresNest& cpu = world.nest;
        const uint32_t paged = FindRealBlock(cpu, 0, false);
        const uint32_t uniform = FindRealBlock(cpu, 0, true);
        // この刻みだけの欄の頁の予算 = 1(2 つ目の根は書かずに作業に残る = 計器の誤り)
        sim::SetNestFieldPageBudget(cpu, world.tick, 1);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, RegionOptions(0));
        if (!gpu)
            return std::unexpected(gpu.error());

        // --- 配り手の欄: 頁を持つ根へ足す・一様な根は広げて(欄の頁を一様の値で埋める)から足す ---
        sim::MultiresNest read = cpu;
        auto owned = GatherBoth(device, queue, ring, *gpu, cpu, read, MakeRootsWork(cpu, world.owned, paged, uniform),
                                true, "集めて書く(配り手の欄)");
        if (!owned)
            return std::unexpected(owned.error());

        // --- この刻みだけの欄: 両方の根が欄の頁を取る所で予算が尽きる ---
        auto tick = GatherBoth(device, queue, ring, *gpu, cpu, read, MakeRootsWork(cpu, world.tick, paged, uniform),
                               false, "集めて書く(この刻みだけの欄)");
        if (!tick)
            return std::unexpected(tick.error());

        if (owned->blocks != 2 || owned->expanded != 1 || owned->pagesTaken != 0 || tick->blocks != 1 ||
            tick->pagesTaken != 1 || tick->pageShortage != 1 || MrIsUniform(cpu.blocks[uniform]))
            return std::unexpected(
                std::format("場面の確かめ: 広げる・頁を取る・予算が尽きる、になっていない(配り手 {} / この刻みだけ {})",
                            ReportText(*owned), ReportText(*tick)));

        Log(Channel::Gpu, Level::Info, "集めて書く: 配り手の欄({})・この刻みだけの欄({})で一致", ReportText(*owned),
            ReportText(*tick));

        return {};
    }

    // --- 11. 保存量の配り手の欄: 粗くする余りを世界の帳簿の列へ ---

    constexpr int32_t CONSERVED_FINEST_LEVEL = 2;  // 鎖の最も細かいレベル(保存の検査の単位)

    std::expected<void, std::string> RunConservedOwner(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                       gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        FieldWorld world = MakeFieldWorld(table, SF_FLAG_CONSERVED | SF_FLAG_HASHED);
        sim::MultiresNest& cpu = world.nest;
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, RegionOptions(0));
        if (!gpu)
            return std::unexpected(gpu.error());

        // --- 細かくする(配り手の配りは割り切れる = 帳簿は動かない)---
        sim::MultiresNest read = cpu;
        if (auto both = ProcessBoth(queue, ring, *gpu, cpu, read, OwnerTreeRequests(0), true, "保存量 細かく"); !both)
            return both;

        // --- 鎖の子の値を 8 で割り切れない値に(子 8 つの和の端数が親の 1 単位に満たない = 余り)---
        const uint32_t chainChild = FindRealBlock(cpu, 2, false);
        if (chainChild == MR_NO_BLOCK)
            return std::unexpected("場面の確かめ: 細かくした鎖の子が見つからない");

        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            for (uint32_t c = 0; c < OWNER_COMPONENTS; ++c)
                sim::StoreNestField(cpu, world.owned, chainChild, index, c,
                                    (OwnerValue(index, c) * 3) + static_cast<int64_t>(index % 5));
        }

        const sim::ConservedTotals before = sim::ComputeConservedTotals(cpu, table, CONSERVED_FINEST_LEVEL);
        const std::vector<uint64_t> ledgerBefore = cpu.ledger;
        for (uint32_t round = 1; round < 3; ++round) {
            if (auto both = ProcessBoth(queue, ring, *gpu, cpu, read, OwnerTreeRequests(round), round == 1,
                                        std::format("保存量 粗く {} 回目", round));
                !both)
                return both;

            if (cpu.ledger != read.ledger)
                return std::unexpected(std::format("保存量 粗く {} 回目: 世界の帳簿が CPU と違う", round));
        }

        if (cpu.ledger == ledgerBefore)
            return std::unexpected("場面の確かめ: 粗くする余りが帳簿に載らない");

        if (sim::ComputeConservedTotals(cpu, table, CONSERVED_FINEST_LEVEL) != before ||
            sim::ComputeConservedTotals(read, table, CONSERVED_FINEST_LEVEL) != before)
            return std::unexpected("保存の検査: 粗くする前後で保存量(配り手の欄の列)が変わる");

        Log(Channel::Gpu, Level::Info, "保存量の配り手の欄: 細かく → 粗く 2 回で帳簿の余りの列・置き場・要約が一致");

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
        if (ran)
            ran = RunShadow(device->Get(), *queue, *ring, *table);
        if (ran)
            ran = RunFold(device->Get(), *queue, *ring, *table);
        if (ran)
            ran = RunUniformTicks(device->Get(), *queue, *ring, *table);
        if (ran)
            ran = RunGather(device->Get(), *queue, *ring, *table);
        if (ran)
            ran = RunConservedOwner(device->Get(), *queue, *ring, *table);
        if (!ran) {
            Log(Channel::Gpu, Level::Error, "gpu_field_region_test: FAILED ({})", ran.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_field_region_test"))
            return 1;

        Log(Channel::Gpu, Level::Info,
            "gpu_field_region_test: "
            "OK("
            "欄の区画を写して戻す・刻む・欄の無い世界・断り・木の段・影の鎖・畳む・広げる・集めて書く・保存量の配り手の"
            "欄で "
            "GPU と CPU がビット一致)");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
