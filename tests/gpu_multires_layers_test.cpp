// gpu_multires_layers_test.cpp — セルの成分ごとの層の量(T-1221。ADR-0481 の 5・20 §10.5)を GPU の世界(sim/gpu_multires)で持ち、
// CPU リファレンス(sim::StepNest・StepActive の層の付け替え。T-1210・T-1220)と毎刻みビット一致することを確かめる。
//   1. 写す像(CPU だけ): 像にして戻すと状態の要約が同じ・層の頁が足りなければ断る
//   2. 層のある場面: 根の頁の全部のセルに層を書き、全部を刻む(RecordStep)・伝導つき・活性だけ刻む(RecordStepActive)を毎刻み
//      読み戻して比べる(状態の全部 = 層まで。読み戻しは毎回新しい入れ物へ: CPU の前の層に頼らず GPU の層だけで一致する)。
//      0 ≤ 層 ≤ 量 で、燃えて減った物質の層が GPU でも減る(試験が空でない)
//   3. まとめて刻む: 1 本のリストで写してから何刻みも刻み、最後に 1 回だけ読み戻して比べる(層が VRAM で刻みをまたいで続く)
//   4. 層の無い場面: 層の頁を持つ GPU でも、層の無い世界は今とビット単位で同じ(CPU と一致)
//   5. 細かくする場面(T-1240 の 2 段目): 層のある根の 1 点を本物の鎖・影の鎖で 9 段細かくし(test::StepMultiresScene と同じ順)、
//      毎刻み比べる(子の頁の層 = 親のセルの層・覆った親のセルの層は 0・影は引き戻しの後に層を合わせる)。粗くする前の刻みまで
//      (粗くする・畳む・広げるの層は T-1240 の 3 段目)
//   6. 層の頁が尽きる場面: 層の頁を小さくした世界では、層のある所を細かくする要求を断って数える(木と量は細かくしない世界と同じ)
//   5 は粗くする刻み(COARSEN_TICK から 1 段ずつ)まで比べる(T-1240 の 3 段目: 子の層の和 ÷ 8・子の層の頁を返す)。
//      本物の鎖は層を使う世界でも(粗くする層は和 ÷ 8 の切り上げを量で抑える。T-1324)
//   7. 一様の枠の層(T-1240 の 3・4 段目): 層まで同じ静かな頁を畳む(層ごと一様の枠の層へ)→ 一様な根を本物の鎖で細かくする(子は親と同じ
//      一様の枠の層)→ 粗くして戻す。燃える一様な根(一様の枠の層)を刻むと頁に広げる(一様の枠の層を頁へ)。毎刻み比べる
//   9. 層を使う世界で反応が粒を生む表(T-1322・T-1323): 燃える根(層なしで始める = GPU が層の頁を取る)と燃える一様な根(一様の枠の層 →
//      頁に広げて刻む)を、全部を刻む・伝導つき・活性だけで毎刻み比べる。表は燃えて出た湯気が粒の露になる規則を足したもの。
//      生まれた粒が層になる(層の和が増える)
//   8. 一様な親の影の鎖(影の頁へ一様の枠の層を今の値と結んで写す)・一様な親へ層の違う一様な子を粗くする(親を頁に広げて
//      子の層の和 ÷ 8。T-1240 の 4 段目で試していなかった道)
// 時間(WARP では測らない): 層のある場面の 1 刻み(全部を刻む)を層の頁あり・なしで測り、層を合わせる段の分を見る(docs/perf.md)。
// 場面は tests/multires_test_scene.h の根と鎖。引数は gpu_test_options.h。
#include <algorithm>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/multires_layers.hlsli"
#include "common/multires_wide.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "multires_test_scene.h"
#include "reaction_limits_table.h"
#include "reaction_test_table.h"
#include "sim/gpu_multires.h"
#include "sim/gpu_multires_layers.h"
#include "sim/multires_nest_internal.h"
#include "sim/multires_state.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace {

    constexpr uint64_t LAYER_TICKS = 24;                               // 毎刻み比べる場面の刻みの数
    constexpr uint64_t BATCH_TICKS = 12;                               // まとめて刻む場面の刻みの数
    constexpr uint32_t LAYER_PAGES = 4;                                // GPU の層の頁の数
    constexpr uint32_t CHAIN_LAYER_PAGES = 1 + test::MULTIRES_LEVELS;  // 細かくする場面: 根 + 鎖の 9 段(ちょうど足りる)
    constexpr uint64_t CHAIN_TICKS = test::MULTIRES_END_TICK;          // 細かくする場面の刻みの数(粗くして戻すまで)
    constexpr uint64_t EXPAND_TICKS = 24;                              // 一様な根を広げる場面の刻みの数
    constexpr uint32_t WARMUP_STEPS = 200;                             // 計測の前の刻み(GPU のクロックを上げる)
    constexpr uint32_t MEASURE_STEPS = 64;                             // 計測の刻み
    constexpr uint64_t GRAIN_DEW_RATE_PER_SECOND = 1000;               // 粒を生む表の湯気 → 露(粒)の速さ(9)

    enum class StepMode : uint8_t {
        Full,
        Conduction,
        Active,
    };

    std::string_view ModeName(StepMode mode) {
        switch (mode) {
            case StepMode::Full: return "全部を刻む";
            case StepMode::Conduction: return "伝導つき";
            case StepMode::Active: return "活性だけ";
        }

        return "?";
    }

    // セルの成分ごとに 量 × 3 ÷ 4 の層(multires_layers_test の QuarterLayers と同じ)
    sim::MrCellLayers ThreeQuarterLayers(const RxCell& cell) {
        sim::MrCellLayers layers;
        for (uint32_t i = 0; i < cell.speciesCount; ++i)
            layers.amounts[i] = cell.amounts[i] / 4 * 3;

        return layers;
    }

    // 試験の根(頁を持つ)。layered なら全部のセルに層を書く
    sim::MultiresNest MakeScene(const sim::BakedReactionTable& table, bool layered) {
        sim::MultiresNest nest = test::MakeMultiresNestForTest(table);
        if (!layered)
            return nest;

        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const RxCell cell = sim::LoadNestCell(nest, test::MULTIRES_ROOT_SLOT, index);
            sim::StoreNestCellWithLayers(nest, test::MULTIRES_ROOT_SLOT, index, cell, ThreeQuarterLayers(cell));
        }

        return nest;
    }

    // 全部のセルが同じ cell・同じ 3/4 の層の根(一様の枠の層。T-1240)。paged なら頁に広げる(頁の層 = 一様の枠の層)
    sim::MultiresNest MakeUniformScene(const sim::BakedReactionTable& table, const RxCell& cell, bool paged) {
        sim::MultiresNest nest = sim::MakeMultiresNest(test::MakeMultiresCapacity(
            table, test::MULTIRES_WORLD_BLOCKS, test::MULTIRES_LEVELS, test::MULTIRES_FRACTION_CAPACITY));
        const std::vector<RxCell> cells(MR_BLOCK_CELLS, cell);
        const std::vector<sim::MrCellLayers> layers(MR_BLOCK_CELLS, ThreeQuarterLayers(cell));
        const uint32_t root = sim::PlaceRootBlock(nest, 0, 0, 0, cells, layers);
        if (paged) {
            [[maybe_unused]] const bool expanded = sim::ExpandNestPage(nest, root);
            FX_ASSERT(expanded);
        }

        return nest;
    }

    // 燃えない空気(300 K)・燃え始めの木箱(600 K。multires_test_scene の点のセル)
    RxCell InertAir(const sim::BakedReactionTable& table) {
        const std::vector<sim::SpeciesAmount> air = {{.species = table.SpeciesId("oxygen"), .amount = 983000},
                                                     {.species = table.SpeciesId("nitrogen"), .amount = 3697000}};
        return sim::MakeReactionCell(table, air, 300000);
    }

    RxCell BurningCrate(const sim::BakedReactionTable& table) {
        const std::vector<sim::SpeciesAmount> crate = {{.species = table.SpeciesId("cellulose"), .amount = 38600000},
                                                       {.species = table.SpeciesId("oxygen"), .amount = 983000},
                                                       {.species = table.SpeciesId("nitrogen"), .amount = 3697000}};
        return sim::MakeReactionCell(table, crate, 600000);
    }

    // 根の 0 でない層の和(試験が空でないかを見る)
    uint64_t RootLayerSum(const sim::MultiresNest& nest) {
        uint64_t sum = 0;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const sim::MrCellLayers layers = sim::LoadNestCellLayers(nest, test::MULTIRES_ROOT_SLOT, index);
            for (const uint64_t layer : layers.amounts)
                sum += layer;
        }

        return sum;
    }

    sim::MultiresStepOptions OptionsOf(StepMode mode) {
        sim::MultiresStepOptions options;
        options.conduction = mode == StepMode::Conduction;

        return options;
    }

    void StepCpu(sim::MultiresNest& nest, const sim::BakedReactionTable& table, StepMode mode, uint64_t tick) {
        if (mode == StepMode::Active) {
            sim::StepActive(nest, table, test::MULTIRES_TEST_SEED, tick, OptionsOf(mode));
            return;
        }

        sim::StepNest(nest, table, test::MULTIRES_TEST_SEED, tick, OptionsOf(mode));
    }

    bool RecordGpuStep(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                       StepMode mode, uint64_t tick) {
        if (mode == StepMode::Active)
            return gpu.RecordStepActive(list, ring, test::MULTIRES_TEST_SEED, tick, OptionsOf(mode));

        gpu.RecordStep(list, ring, test::MULTIRES_TEST_SEED, tick, OptionsOf(mode));

        return true;
    }

    // record を 1 本のリストで走らせ、新しい入れ物に読み戻す
    template <typename Record>
    std::expected<sim::MultiresNest, std::string> Execute(gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                          sim::GpuMultires& gpu, const Record& record) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        ring.RecordBegin(list);
        if (!record(list))
            return std::unexpected("記録できない");

        gpu.RecordReadback(list);
        ring.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        if (const gpu::DebugRingContents output = ring.Drain(); output.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", output.assertCount));

        sim::MultiresNest read;
        if (!gpu.Read(read))
            return std::unexpected("読み戻せない");

        return read;
    }

    std::expected<void, std::string> Compare(const sim::MultiresNest& cpu, const sim::MultiresNest& read,
                                             std::string_view where) {
        if (sim::CountLayerMisfits(read) != 0)
            return std::unexpected(std::format("{}: GPU の層が量を超える", where));

        if (RootLayerSum(cpu) != RootLayerSum(read))
            return std::unexpected(
                std::format("{}: 層の和が違う(cpu {} / gpu {})", where, RootLayerSum(cpu), RootLayerSum(read)));

        if (sim::HashWholeNest(cpu) != sim::HashWholeNest(read))
            return std::unexpected(std::format("{}: CPU と GPU の状態の要約が違う", where));

        return {};
    }

    sim::GpuMultiresOptions GpuOptions(StepMode mode, uint32_t layerPages) {
        sim::GpuMultiresOptions options;
        options.activity = mode == StepMode::Active;
        options.layerPages = layerPages;

        return options;
    }

    // 試験の世界の層の置き場の語の数(セルの頁の数 = 観察の枠の頁 + 世界の頁)
    uint32_t LayerStoreWords(const sim::BakedReactionTable& table) {
        const sim::MultiresNest nest = MakeScene(table, false);

        return MrLayerStoreWords(static_cast<uint32_t>(nest.cells.pages.size()), LAYER_PAGES,
                                 static_cast<uint32_t>(nest.blocks.size()));
    }

    // 像の頁の索引・持ち主・空きのスタックが食い違わないか(層の頁は索引か空きのどちらか 1 つだけにある。T-1240)
    bool LayerStoreConsistent(std::span<const uint32_t> words, uint32_t pageCount, uint32_t capacity) {
        std::vector<uint32_t> seen(capacity, 0);
        for (uint32_t page = 0; page < pageCount; ++page) {
            const uint32_t layerPage = words[MrLayerIndexAddress(page)];
            if (layerPage == MR_LAYER_NO_PAGE)
                continue;

            if (layerPage >= capacity || words[MrLayerOwnerAddress(pageCount, layerPage)] != page)
                return false;

            ++seen[layerPage];
        }

        const uint32_t freeCount = words[MrLayerFreeCountAddress(pageCount, capacity)];
        if (freeCount > capacity)
            return false;

        for (uint32_t position = 0; position < freeCount; ++position) {
            const uint32_t layerPage = words[MrLayerFreeAddress(pageCount, capacity, position)];
            if (layerPage >= capacity || words[MrLayerOwnerAddress(pageCount, layerPage)] != MR_LAYER_NO_PAGE)
                return false;

            ++seen[layerPage];
        }

        return std::ranges::all_of(seen, [](uint32_t count) { return count == 1; });
    }

    // --- 1. 写す像(CPU だけ)---
    std::expected<void, std::string> TestImage(const sim::BakedReactionTable& table) {
        sim::MultiresNest nest = MakeScene(table, true);
        const uint64_t before = sim::HashWholeNest(nest);
        const auto pageCount = static_cast<uint32_t>(nest.cells.pages.size());
        auto image = sim::MakeGpuLayerImage(nest, LAYER_PAGES);
        if (!image || image->used != 1)
            return std::unexpected("像: 層のある頁 1 つを写せない");

        if (!LayerStoreConsistent(image->words, pageCount, LAYER_PAGES))
            return std::unexpected("像: 頁の索引・持ち主・空きのスタックが食い違う");

        sim::ApplyGpuLayerImage(nest, image->words, LAYER_PAGES);
        if (sim::HashWholeNest(nest) != before)
            return std::unexpected("像: 写して戻すと状態の要約が変わる");

        if (sim::MakeGpuLayerImage(nest, 0))
            return std::unexpected("像: 層の頁が 0 なのに写せてしまう");

        // --- 一様の枠の層(T-1240): 枠ごとの組に写して戻すと同じ ---
        sim::MultiresNest uniform = MakeUniformScene(table, InertAir(table), false);
        const uint64_t uniformBefore = sim::HashWholeNest(uniform);
        auto uniformImage = sim::MakeGpuLayerImage(uniform, LAYER_PAGES);
        if (!uniformImage || uniformImage->used != 0 || uniformImage->uniformSlots != 1)
            return std::unexpected("像: 一様の枠の層 1 つを写せない");

        sim::ApplyGpuLayerImage(uniform, uniformImage->words, LAYER_PAGES);
        if (sim::HashWholeNest(uniform) != uniformBefore || uniform.cells.uniformLayers.Empty())
            return std::unexpected("像: 一様の枠の層を写して戻すと状態の要約が変わる");

        return {};
    }

    // --- 溢れを使う世界の像(T-1240 の 7 の 5 の ①): 溢れの成分の層も物質で組に詰めて戻す・9 種以上が層を持てば写さない ---
    constexpr uint32_t WIDE_IMAGE_SPECIES = 12;  // 1 セルの成分(インライン 8 + 溢れ 4)

    // 物質 c01〜c08・q01〜q04 のセル(量はセルと物質で変える)
    RxWideCell WideImageCell(const sim::BakedReactionTable& table, uint32_t index) {
        std::vector<sim::SpeciesAmount> amounts;
        for (uint32_t i = 1; i <= WIDE_IMAGE_SPECIES; ++i) {
            const std::string name = i <= test::LIMITS_CROWD_SPECIES
                                         ? test::LimitsCrowdName(i)
                                         : test::LimitsHeldName(i - test::LIMITS_CROWD_SPECIES);
            const uint64_t amount = 1000000 + (uint64_t{index} * 37) + (uint64_t{i} * 11);
            amounts.push_back({.species = table.SpeciesId(name), .amount = amount});
        }

        return sim::MakeWideReactionCell(table, amounts, test::LIMITS_TEMPERATURE_MILLIKELVIN);
    }

    // 成分 i の層 = (index + i) % stride == 0 なら 量 ÷ 4 × 3(stride 3: 0 でない層は 4 種まで・溢れの成分にも。stride 1: 12 種)
    constexpr uint32_t WIDE_NO_LAYERS = 0;  // stride 0: 層なし(溢れを使う世界の層を使わない場面)

    std::vector<uint64_t> WideImageLayers(const RxWideCell& cell, uint32_t index, uint32_t stride) {
        std::vector<uint64_t> layers(cell.speciesCount, 0);
        for (uint32_t i = 0; i < cell.speciesCount && stride != WIDE_NO_LAYERS; ++i)
            layers[i] = (index + i) % stride == 0 ? cell.amounts[i] / 4 * 3 : 0;

        return layers;
    }

    // 溢れを使う世界の根。paged なら頁(セルごとに量が違う)、でなければ一様の枠(全部 WideImageCell(0))
    sim::MultiresNest MakeWideImageRoot(const sim::BakedReactionTable& table, uint32_t stride, bool paged,
                                        bool chain = false) {
        // chain: 鎖の場面の大きさ(本物の鎖 9 段・影の鎖 9 段。multires_test_scene)
        const sim::MultiresCapacity capacity = chain ? test::MakeMultiresCapacity(table, test::MULTIRES_WORLD_BLOCKS,
                                                                                  test::MULTIRES_LEVELS,
                                                                                  test::MULTIRES_FRACTION_CAPACITY)
                                                     : test::MakeMultiresCapacity(table, 16, 0, 16);
        sim::MultiresNest nest = sim::MakeMultiresNest(capacity);
        sim::EnableWideCells(nest);
        std::vector<RxCell> cells(MR_BLOCK_CELLS, RxMakeEmptyCell(0));
        if (paged)
            cells[1] = RxMakeEmptyCell(1);  // 頁を取る

        const uint32_t root = sim::PlaceRootBlock(nest, 0, 0, 0, cells);
        if (!paged) {
            const RxWideCell value = WideImageCell(table, 0);
            sim::StoreWideUniform(nest, root, value);
            sim::nest_detail::SetUniformLayers(nest, root, value, WideImageLayers(value, 0, stride));
            return nest;
        }

        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const RxWideCell cell = WideImageCell(table, index);
            sim::StoreWideNestCellWithLayers(nest, root, index, cell, WideImageLayers(cell, index, stride));
        }

        return nest;
    }

    std::expected<void, std::string> TestWideImage(const sim::BakedReactionTable& table) {
        for (const bool paged : {true, false}) {
            const std::string_view name = paged ? "頁" : "一様の枠";
            sim::MultiresNest nest = MakeWideImageRoot(table, 3, paged);
            const std::vector<uint64_t> rootLayers = sim::LoadWideNestCellLayers(nest, test::MULTIRES_ROOT_SLOT, 0);
            if (rootLayers.size() != WIDE_IMAGE_SPECIES ||
                !std::ranges::any_of(rootLayers.begin() + RX_MAX_CELL_SPECIES, rootLayers.end(),
                                     [](uint64_t layer) { return layer != 0; }))
                return std::unexpected(std::format("溢れの像({}): 場面の溢れの成分に層が無い", name));

            const uint64_t before = sim::HashWholeNest(nest);
            auto image = sim::MakeGpuLayerImage(nest, LAYER_PAGES);
            if (!image || image->used + image->uniformSlots != 1)
                return std::unexpected(std::format("溢れの像({}): 層 1 つを写せない", name));

            sim::ApplyGpuLayerImage(nest, image->words, LAYER_PAGES);
            if (sim::HashWholeNest(nest) != before)
                return std::unexpected(std::format("溢れの像({}): 写して戻すと状態の要約が変わる", name));

            if (sim::MakeGpuLayerImage(MakeWideImageRoot(table, 1, paged), LAYER_PAGES))
                return std::unexpected(std::format("溢れの像({}): 9 種以上が層を持つのに写せてしまう", name));
        }

        return {};
    }

    // --- 2・4. 毎刻み比べる ---
    std::expected<void, std::string> RunEachTick(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                 gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                 StepMode mode, bool layered) {
        sim::MultiresNest cpu = MakeScene(table, layered);
        const uint64_t firstSum = RootLayerSum(cpu);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, GpuOptions(mode, LAYER_PAGES));
        if (!gpu)
            return std::unexpected(gpu.error());

        const std::string scene = std::format("{}・{}", ModeName(mode), layered ? "層あり" : "層なし");
        uint64_t lastSum = firstSum;
        for (uint64_t tick = 0; tick < LAYER_TICKS; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordGpuStep(list, *gpu, ring.GpuAddress(), mode, tick);
            };
            auto read = Execute(queue, ring, *gpu, record);
            if (!read)
                return std::unexpected(std::format("{} 刻み {}: {}", scene, tick, read.error()));

            StepCpu(cpu, table, mode, tick);
            if (auto compared = Compare(cpu, *read, std::format("{} 刻み {}", scene, tick)); !compared)
                return compared;

            lastSum = RootLayerSum(*read);
        }

        if (layered && !(lastSum < firstSum && lastSum > 0))
            return std::unexpected(
                std::format("{}: 層が減らない・全部消えた(初め {}・終わり {})", scene, firstSum, lastSum));

        Log(Channel::Gpu, Level::Info, "{}: {} 刻み毎回一致(層の和 {} → {})", scene, LAYER_TICKS, firstSum, lastSum);

        return {};
    }

    // 1 刻みの GPU の記録(test::StepMultiresScene と同じ順。gpu_multires_test の RecordTick と同じ)
    bool RecordSceneTick(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                         test::MultiresScenario scenario, uint64_t tick) {
        using test::MultiresScenario;
        const sim::MultiresPoint point = test::MakeMultiresPoint(test::MULTIRES_LEVELS);
        if (!gpu.RecordRequests(list, test::MultiresRequestsAt(scenario, tick)))
            return false;

        gpu.RecordProcessRequests(list, ring);
        if (tick == test::MULTIRES_REFINE_TICK && scenario == MultiresScenario::Shadow)
            gpu.RecordRefineShadow(list, ring, test::MULTIRES_ROOT_SLOT, test::MULTIRES_SHADOW_SLOT,
                                   test::MULTIRES_LEVELS, point);
        else if (tick == test::MULTIRES_COARSEN_TICK && scenario == MultiresScenario::Shadow)
            gpu.RecordRemoveShadow(list, ring, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS);

        gpu.RecordStep(list, ring, test::MULTIRES_TEST_SEED, tick);
        if (test::MultiresShadowExists(scenario, tick))
            gpu.RecordPullBack(list, ring, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS);

        return true;
    }

    // 層のある頁の数(根の外へ層が配られたかを見る)
    uint32_t LayeredPages(const sim::MultiresNest& nest) {
        uint32_t count = 0;
        for (uint32_t page = 0; page < nest.cells.pages.size(); ++page)
            count += sim::nest_detail::PageHasLayers(nest, page) ? 1 : 0;

        return count;
    }

    // --- 5. 細かくする場面(本物の鎖・影の鎖)。layerWorld = 層を使う世界(粗くする層は和 ÷ 8 の切り上げを量で抑える。T-1324)---
    std::expected<void, std::string> RunChain(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                              const sim::BakedReactionTable& table, test::MultiresScenario scenario,
                                              bool layerWorld = false) {
        sim::MultiresNest cpu = MakeScene(table, true);
        if (layerWorld)
            sim::EnableCellLayers(cpu, table);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, GpuOptions(StepMode::Full, CHAIN_LAYER_PAGES));
        if (!gpu)
            return std::unexpected(gpu.error());

        const std::string scene = std::format("{}{}", scenario == test::MultiresScenario::Real ? "本物の鎖" : "影の鎖",
                                              layerWorld ? "・層を使う世界" : "");
        uint32_t layered = 0;
        uint32_t mostLayered = 0;
        for (uint64_t tick = 0; tick < CHAIN_TICKS; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordSceneTick(list, *gpu, ring.GpuAddress(), scenario, tick);
            };
            auto read = Execute(queue, ring, *gpu, record);
            if (!read)
                return std::unexpected(std::format("{} 刻み {}: {}", scene, tick, read.error()));

            test::StepMultiresScene(cpu, table, scenario, tick);
            if (sim::CountLayerMisfits(*read) != 0)
                return std::unexpected(std::format("{} 刻み {}: GPU の層が量を超える", scene, tick));

            if (sim::HashWholeNest(cpu) != sim::HashWholeNest(*read))
                return std::unexpected(
                    std::format("{} 刻み {}: CPU と GPU の状態の要約が違う(層のある頁 cpu {} / gpu {})", scene, tick,
                                LayeredPages(cpu), LayeredPages(*read)));

            if (gpu->ReadLayerShortage() != 0u)
                return std::unexpected(std::format("{} 刻み {}: 足りるはずの層の頁が尽きた", scene, tick));

            layered = LayeredPages(*read);
            mostLayered = std::max(mostLayered, layered);
        }

        // 根と鎖の 9 段の頁が層を持つ(試験が空でない)。本物の鎖は粗くして根だけに戻る(子の層の頁を返した)
        if (mostLayered != CHAIN_LAYER_PAGES)
            return std::unexpected(std::format("{}: 層のある頁が {}(期待 {})", scene, mostLayered, CHAIN_LAYER_PAGES));

        if (scenario == test::MultiresScenario::Real && layered != 1)
            return std::unexpected(std::format("{}: 粗くした後の層のある頁が {}(期待 1)", scene, layered));

        Log(Channel::Gpu, Level::Info, "{}: {} 刻み毎回一致(層のある頁 最大 {}・終わり {})", scene, CHAIN_TICKS,
            mostLayered, layered);

        return {};
    }

    // 根の様子(食い違った時に見る): 一様か・子の数・一様の枠の層・層のある頁の数
    std::string DescribeRoot(const sim::MultiresNest& nest) {
        const MrBlock& root = nest.blocks[test::MULTIRES_ROOT_SLOT];
        const auto children = std::ranges::count_if(root.children, [](uint32_t child) { return child != MR_NO_BLOCK; });

        return std::format("根 {}・子 {}・一様の枠の層 {}・層のある頁 {}・畳んだ数 {}",
                           MrIsUniform(root) ? "一様" : "頁", children,
                           nest.cells.uniformLayers.Find(test::MULTIRES_ROOT_SLOT) != nullptr ? "あり" : "なし",
                           LayeredPages(nest), nest.counters[MR_COUNTER_FOLDED]);
    }

    // --- 7. 一様の枠の層(T-1240 の 3・4 段目)---
    // 畳む → 一様な根を細かくする → 粗くする: 層まで同じ静かな頁(燃えない空気)を毎刻み畳む段に通し(CPU は FoldQuietPages)、
    // 本物の鎖の場面を進める。畳んだら根は一様の枠の層を持つ(層の頁を返す)。鎖は一様(子は親と同じ一様の枠の層)
    std::expected<void, std::string> RunUniformFold(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                    gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = MakeUniformScene(table, InertAir(table), true);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity,
                                            GpuOptions(StepMode::Active, CHAIN_LAYER_PAGES));
        if (!gpu)
            return std::unexpected(gpu.error());

        bool folded = false;
        uint32_t uniformLayered = 0;
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                // test::BeginQuietTick と同じ順(要求 → 畳む → 要求の処理 → 活性だけ刻む。眠る印は活性の刻みが付ける)
                if (!gpu->RecordRequests(list, test::MultiresRequestsAt(test::MultiresScenario::Real, tick)))
                    return false;

                gpu->RecordFoldPages(list, ring.GpuAddress(), tick);
                gpu->RecordProcessRequests(list, ring.GpuAddress());
                return gpu->RecordStepActive(list, ring.GpuAddress(), test::MULTIRES_TEST_SEED, tick);
            };
            auto read = Execute(queue, ring, *gpu, record);
            if (!read)
                return std::unexpected(std::format("畳む 刻み {}: {}", tick, read.error()));

            sim::SubmitRequests(cpu, test::MultiresRequestsAt(test::MultiresScenario::Real, tick));
            sim::FoldQuietPages(cpu, tick);
            sim::ProcessRequests(cpu);
            sim::StepActive(cpu, table, test::MULTIRES_TEST_SEED, tick);
            if (auto compared = Compare(cpu, *read, std::format("畳む 刻み {}", tick)); !compared)
                return std::unexpected(std::format("{}(cpu: {} / gpu: {}・層の頁の不足 {})", compared.error(),
                                                   DescribeRoot(cpu), DescribeRoot(*read),
                                                   gpu->ReadLayerShortage().value_or(~0u)));

            folded = folded || MrIsUniform(read->blocks[test::MULTIRES_ROOT_SLOT]);
            uint32_t count = 0;
            for (uint32_t slot = 0; slot < read->blocks.size(); ++slot)
                count += read->cells.uniformLayers.Find(slot) != nullptr ? 1 : 0;

            uniformLayered = std::max(uniformLayered, count);
        }

        // 根を層ごと畳み、鎖の 9 段も一様の枠の層を持った(試験が空でない)
        if (!folded || uniformLayered != 1 + test::MULTIRES_LEVELS)
            return std::unexpected(
                std::format("畳む: 根を層ごと畳めない・鎖に一様の枠の層が無い(一様の枠の層 最大 {})", uniformLayered));

        Log(Channel::Gpu, Level::Info, "畳む・一様な鎖: {} 刻み毎回一致(一様の枠の層 最大 {})", test::MULTIRES_END_TICK,
            uniformLayered);

        return {};
    }

    // 広げる: 燃える一様な根(一様の枠の層)を刻むと頁に広げる(層の頁を取り、一様の枠の層を頁へ写す)
    std::expected<void, std::string> RunUniformExpand(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                      gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = MakeUniformScene(table, BurningCrate(table), false);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, GpuOptions(StepMode::Full, LAYER_PAGES));
        if (!gpu)
            return std::unexpected(gpu.error());

        for (uint64_t tick = 0; tick < EXPAND_TICKS; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordSceneTick(list, *gpu, ring.GpuAddress(), test::MultiresScenario::None, tick);
            };
            auto read = Execute(queue, ring, *gpu, record);
            if (!read)
                return std::unexpected(std::format("広げる 刻み {}: {}", tick, read.error()));

            test::StepMultiresScene(cpu, table, test::MultiresScenario::None, tick);
            if (auto compared = Compare(cpu, *read, std::format("広げる 刻み {}", tick)); !compared)
                return compared;

            if (gpu->ReadLayerShortage() != 0u)
                return std::unexpected(std::format("広げる 刻み {}: 足りるはずの層の頁が尽きた", tick));
        }

        if (MrIsUniform(cpu.blocks[test::MULTIRES_ROOT_SLOT]) || RootLayerSum(cpu) == 0)
            return std::unexpected("広げる: 根が頁に広がらない・層が無い(試験が空)");

        Log(Channel::Gpu, Level::Info, "広げる: {} 刻み毎回一致(根の層の和 {})", EXPAND_TICKS, RootLayerSum(cpu));

        return {};
    }

    // --- 8. 一様な親の影の鎖: 燃えない一様な根(一様の枠の層)の影の鎖を作り、毎刻み引き戻して COARSEN_TICK に捨てる ---
    std::expected<void, std::string> RunUniformShadow(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                      gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = MakeUniformScene(table, InertAir(table), false);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, GpuOptions(StepMode::Full, CHAIN_LAYER_PAGES));
        if (!gpu)
            return std::unexpected(gpu.error());

        uint32_t mostLayered = 0;
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordSceneTick(list, *gpu, ring.GpuAddress(), test::MultiresScenario::Shadow, tick);
            };
            auto read = Execute(queue, ring, *gpu, record);
            if (!read)
                return std::unexpected(std::format("一様な親の影 刻み {}: {}", tick, read.error()));

            test::StepMultiresScene(cpu, table, test::MultiresScenario::Shadow, tick);
            if (auto compared = Compare(cpu, *read, std::format("一様な親の影 刻み {}", tick)); !compared)
                return compared;

            if (gpu->ReadLayerShortage() != 0u)
                return std::unexpected(std::format("一様な親の影 刻み {}: 足りるはずの層の頁が尽きた", tick));

            mostLayered = std::max(mostLayered, LayeredPages(*read));
        }

        // 影の鎖の 9 段の頁が層を持った(一様な根から写した。試験が空でない)
        if (mostLayered != test::MULTIRES_LEVELS)
            return std::unexpected(
                std::format("一様な親の影: 層のある頁が {}(期待 {})", mostLayered, test::MULTIRES_LEVELS));

        Log(Channel::Gpu, Level::Info, "一様な親の影の鎖: {} 刻み毎回一致(層のある頁 最大 {})", test::MULTIRES_END_TICK,
            mostLayered);

        return {};
    }

    // --- 8. 一様な親へ層の違う一様な子を粗くする: 一様な根の本物の鎖(一様の枠の層を写した)を CPU で作り、一番細かい子の層だけを
    //     量の 1/4 に書き換えてから写し、COARSEN_TICK からの粗くする刻みを比べる(親は頁に広がる・次の段は頁の子を一様な親へ)---
    std::expected<void, std::string> RunUniformCoarsenExpand(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                             gpu::DebugRing& ring,
                                                             const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = MakeUniformScene(table, InertAir(table), false);
        sim::SubmitRequests(cpu, test::MultiresRequestsAt(test::MultiresScenario::Real, test::MULTIRES_REFINE_TICK));
        sim::ProcessRequests(cpu);
        uint32_t deepest = MR_NO_BLOCK;
        for (uint32_t slot = 0; slot < cpu.capacity.worldBlocks; ++slot) {
            const MrBlock& block = cpu.blocks[slot];
            if (block.kind == MR_BLOCK_REAL && block.level == (int32_t)test::MULTIRES_LEVELS && MrIsUniform(block))
                deepest = slot;
        }

        if (deepest == MR_NO_BLOCK)
            return std::unexpected("一様な子を粗くする: 一様な鎖を作れない");

        const RxCell value = cpu.cells.uniform[deepest];
        std::vector<uint64_t> quarter(value.speciesCount, 0);
        for (uint32_t i = 0; i < value.speciesCount; ++i)
            quarter[i] = value.amounts[i] / 4;

        sim::nest_detail::SetUniformLayers(cpu, deepest, RxWidenCell(value), quarter);

        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, GpuOptions(StepMode::Full, LAYER_PAGES));
        if (!gpu)
            return std::unexpected(gpu.error());

        uint32_t mostLayered = 0;
        for (uint64_t tick = test::MULTIRES_COARSEN_TICK; tick < test::MULTIRES_END_TICK; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == test::MULTIRES_COARSEN_TICK && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordSceneTick(list, *gpu, ring.GpuAddress(), test::MultiresScenario::Real, tick);
            };
            auto read = Execute(queue, ring, *gpu, record);
            if (!read)
                return std::unexpected(std::format("一様な子を粗くする 刻み {}: {}", tick, read.error()));

            test::StepMultiresScene(cpu, table, test::MultiresScenario::Real, tick);
            if (auto compared = Compare(cpu, *read, std::format("一様な子を粗くする 刻み {}", tick)); !compared)
                return compared;

            if (gpu->ReadLayerShortage() != 0u)
                return std::unexpected(std::format("一様な子を粗くする 刻み {}: 足りるはずの層の頁が尽きた", tick));

            mostLayered = std::max(mostLayered, LayeredPages(*read));
        }

        // 層の違う子を粗くした親は頁に広がって層を持った(試験が空でない)
        if (mostLayered == 0)
            return std::unexpected("一様な子を粗くする: 親が頁に広がらない(層のある頁が無い)");

        Log(Channel::Gpu, Level::Info, "一様な親へ層の違う子を粗くする: 毎回一致(層のある頁 最大 {})", mostLayered);

        return {};
    }

    // --- 6. 層の頁が尽きる場面: 細かくする要求を断って数える(仮【エンジン】T-1240)---
    // 比べる相手は細かくしない CPU(断った要求の分だけ「空きが無い」の数える欄が 1 多い)
    std::expected<void, std::string> RunShortage(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                 gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = MakeScene(table, true);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, GpuOptions(StepMode::Full, LAYER_PAGES));
        if (!gpu)
            return std::unexpected(gpu.error());

        const uint64_t ticks = test::MULTIRES_REFINE_TICK + 2;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordSceneTick(list, *gpu, ring.GpuAddress(), test::MultiresScenario::Real, tick);
            };
            auto read = Execute(queue, ring, *gpu, record);
            if (!read)
                return std::unexpected(std::format("尽きる 刻み {}: {}", tick, read.error()));

            test::StepMultiresScene(cpu, table, test::MultiresScenario::None, tick);
            const uint32_t denied = tick >= test::MULTIRES_REFINE_TICK ? 1u : 0u;
            const std::optional<uint32_t> shortage = gpu->ReadLayerShortage();
            if (shortage != denied || read->counters[MR_COUNTER_NO_SPACE] != cpu.counters[MR_COUNTER_NO_SPACE] + denied)
                return std::unexpected(std::format("尽きる 刻み {}: 断った数が違う(層の頁 {}・空きが無い {})", tick,
                                                   shortage.value_or(~0u), read->counters[MR_COUNTER_NO_SPACE]));

            read->counters[MR_COUNTER_NO_SPACE] = cpu.counters[MR_COUNTER_NO_SPACE];
            if (auto compared = Compare(cpu, *read, std::format("尽きる 刻み {}", tick)); !compared)
                return compared;
        }

        Log(Channel::Gpu, Level::Info,
            "層の頁が尽きる: 細かくする要求を断って数えた(層の頁 {}・木と量は細かくしない世界と一致)", LAYER_PAGES);

        return {};
    }

    // --- 3. まとめて刻む ---
    std::expected<void, std::string> RunBatch(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                              const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = MakeScene(table, true);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, GpuOptions(StepMode::Full, LAYER_PAGES));
        if (!gpu)
            return std::unexpected(gpu.error());

        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            if (!gpu->RecordUpload(list, cpu))
                return false;

            for (uint64_t tick = 0; tick < BATCH_TICKS; ++tick)
                gpu->RecordStep(list, ring.GpuAddress(), test::MULTIRES_TEST_SEED, tick);

            return true;
        };
        auto read = Execute(queue, ring, *gpu, record);
        if (!read)
            return std::unexpected(std::format("まとめて: {}", read.error()));

        for (uint64_t tick = 0; tick < BATCH_TICKS; ++tick)
            StepCpu(cpu, table, StepMode::Full, tick);

        return Compare(cpu, *read, std::format("まとめて {} 刻み", BATCH_TICKS));
    }

    // --- 8. 溢れを使う世界の層(T-1240 の 7 の 5): 溢れの成分にも層のある根で鎖の場面を毎刻み比べる・
    //     粗くすると層を持つ物質が 9 種以上になる要求は断って数える(D-689)---
    sim::GpuMultiresOptions WideOptions() {
        sim::GpuMultiresOptions options = GpuOptions(StepMode::Full, CHAIN_LAYER_PAGES);
        options.wideCells = true;

        return options;
    }

    // 食い違った時に見る: 最初に違うブロック・セル(量か層)
    std::string DescribeWideDifference(const sim::MultiresNest& cpu, const sim::MultiresNest& read) {
        const uint32_t slots = cpu.capacity.worldBlocks + cpu.capacity.observerBlocks;
        for (uint32_t slot = 0; slot < slots; ++slot) {
            const MrBlock& a = cpu.blocks[slot];
            const MrBlock& b = read.blocks[slot];
            if (a.kind != b.kind || a.level != b.level || MrIsUniform(a) != MrIsUniform(b))
                return std::format("枠 {} の見出しが違う(種類 {}/{}・段 {}/{}・一様 {}/{})", slot, a.kind, b.kind,
                                   a.level, b.level, MrIsUniform(a), MrIsUniform(b));

            if (a.kind == MR_BLOCK_UNUSED)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxWideCell x = sim::LoadWideNestCell(cpu, slot, index);
                const RxWideCell y = sim::LoadWideNestCell(read, slot, index);
                if (sim::HashReactionCell(x) != sim::HashReactionCell(y))
                    return std::format("枠 {} セル {} の量が違う(成分 {}/{})", slot, index, x.speciesCount,
                                       y.speciesCount);

                const std::vector<uint64_t> u = sim::LoadWideNestCellLayers(cpu, slot, index);
                const std::vector<uint64_t> v = sim::LoadWideNestCellLayers(read, slot, index);
                for (uint32_t i = 0; i < std::max(u.size(), v.size()); ++i) {
                    const uint64_t left = i < u.size() ? u[i] : 0;
                    const uint64_t right = i < v.size() ? v[i] : 0;
                    if (left != right)
                        return std::format("枠 {}(段 {}) セル {} 成分 {}(物質 {})の層が違う(cpu {} / gpu {})", slot,
                                           a.level, index, i, i < x.speciesCount ? x.species[i] : ~0u, left, right);
                }
            }
        }

        return "セルと層は同じ(ほかの欄が違う)";
    }

    // 溢れの世界の鎖の要求(gpu_multires_test の RefineWideRequests の点: 根の八分の一 2 の中の 2 段)。
    // WIDE_REFINE_TICK に 2 段を 1 つの要求で細かくし、WIDE_COARSEN_TICK から 1 刻み 1 段ずつ粗くする。
    // 9 段の鎖(test::MultiresScenario::Real)は RunWideDeepChain(T-1341。既定の溢れの塊の置き場では足りないので大きくする)
    constexpr uint64_t WIDE_REFINE_TICK = 3;
    constexpr uint64_t WIDE_COARSEN_TICK = 8;
    constexpr uint64_t WIDE_TICKS = 12;
    constexpr int32_t WIDE_CHAIN_LEVEL = 2;

    std::vector<MrRequest> WideChainRequestsAt(uint64_t tick) {
        const sim::MultiresPoint point = {.x = 3, .y = 21, .z = 5, .level = WIDE_CHAIN_LEVEL};
        if (tick == WIDE_REFINE_TICK)
            return {test::MakeMultiresRequest(MR_REQUEST_REFINE, point, WIDE_CHAIN_LEVEL)};

        if (tick >= WIDE_COARSEN_TICK && tick < WIDE_COARSEN_TICK + WIDE_CHAIN_LEVEL) {
            const auto level = static_cast<int32_t>(WIDE_CHAIN_LEVEL - (tick - WIDE_COARSEN_TICK));
            return {test::MakeMultiresRequest(MR_REQUEST_COARSEN, point, level)};
        }

        return {};
    }

    // 1 刻み(CPU): 要求 → 処理 → 刻む。GPU の RecordWideTick と同じ順
    void StepWideCpu(sim::MultiresNest& nest, const sim::BakedReactionTable& table, std::span<const MrRequest> requests,
                     uint64_t tick) {
        sim::SubmitRequests(nest, requests);
        sim::ProcessRequests(nest);
        sim::StepNest(nest, table, test::MULTIRES_TEST_SEED, tick, {});
    }

    bool RecordWideTick(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                        std::span<const MrRequest> requests, uint64_t tick) {
        if (!gpu.RecordRequests(list, requests))
            return false;

        gpu.RecordProcessRequests(list, ring);
        gpu.RecordStep(list, ring, test::MULTIRES_TEST_SEED, tick);

        return true;
    }

    std::expected<void, std::string> RunWideChain(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                  gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                  bool paged, bool layered) {
        sim::MultiresNest cpu = MakeWideImageRoot(table, layered ? 3 : WIDE_NO_LAYERS, paged, true);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, WideOptions());
        if (!gpu)
            return std::unexpected(gpu.error());

        const std::string scene = std::format("溢れの世界の鎖({}・{})", paged ? "頁" : "一様の枠",
                                              layered ? "層あり" : "層なし");
        const uint64_t firstSum = RootLayerSum(cpu);
        uint32_t mostLayered = 0;
        for (uint64_t tick = 0; tick < WIDE_TICKS; ++tick) {
            const std::vector<MrRequest> requests = WideChainRequestsAt(tick);
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordWideTick(list, *gpu, ring.GpuAddress(), requests, tick);
            };
            auto read = Execute(queue, ring, *gpu, record);
            if (!read)
                return std::unexpected(std::format("{} 刻み {}: {}", scene, tick, read.error()));

            StepWideCpu(cpu, table, requests, tick);
            if (auto compared = Compare(cpu, *read, std::format("{} 刻み {}", scene, tick)); !compared)
                return std::unexpected(std::format("{}({}・{})", compared.error(), DescribeRoot(*read),
                                                   DescribeWideDifference(cpu, *read)));

            if (gpu->ReadLayerShortage() != 0u)
                return std::unexpected(std::format("{} 刻み {}: 足りるはずの層の頁が尽きた", scene, tick));

            mostLayered = std::max(mostLayered, LayeredPages(*read));
        }

        // 頁の根は鎖の頁へ層を配った(試験が空でない)。一様の枠の根は一様の鎖(層の頁は使わない)
        if (layered && paged && mostLayered <= 1)
            return std::unexpected(std::format("{}: 層のある頁が {}(鎖へ配られない)", scene, mostLayered));

        Log(Channel::Gpu, Level::Info, "{}: {} 刻み毎回一致(層の和 {} → {}・層のある頁 最大 {})", scene, WIDE_TICKS,
            firstSum, RootLayerSum(cpu), mostLayered);

        return {};
    }

    // --- 溢れを使う世界の 9 段の鎖(T-1341): test::MultiresScenario::Real の鎖(9 段を 1 つの要求で・1 刻み 1 段ずつ粗くする)を
    //     毎刻み比べる。食い違ったら数える器と溢れの塊の置き場を書く。
    //     溢れの塊の置き場は既定(GpuMultiresOptions::wideChunks = 0 → 170 塊)では足りない: 鎖の 9 頁と根の全部のセルが 13 種
    //     (溢れ 5 = 1 面 9 塊)になり、頁は両面の塊を持つので 10 頁 × 2 面 × 9 塊 = 180 塊 > 170。GPU は配れなかった頁を
    //     待たせて数え(MR_COUNTER_LIMIT_PRODUCTS。T-0236)、上限の無い CPU と刻み 10 に食い違った。細かくする段の誤りではない。
    //     ここでは置き場を「全部の頁の両面が表の物質を全部持てる分」にして、待たせない世界で比べる ---

    // 9 段の鎖の要求(test::MultiresScenario::Real と同じ点・同じ 9 段を 1 つの要求で。WARP の時間に収めるため、細かくする前と
    // 細かいままの刻みを縮めた: DEEP_REFINE_TICK に細かくし、DEEP_COARSEN_TICK から 1 刻み 1 段ずつ粗くして戻す)
    constexpr uint64_t DEEP_REFINE_TICK = 2;
    constexpr uint64_t DEEP_COARSEN_TICK = 6;
    constexpr uint64_t DEEP_TICKS = DEEP_COARSEN_TICK + test::MULTIRES_LEVELS + 1;

    std::vector<MrRequest> DeepChainRequestsAt(uint64_t tick) {
        const sim::MultiresPoint point = test::MakeMultiresPoint(test::MULTIRES_LEVELS);
        if (tick == DEEP_REFINE_TICK)
            return {test::MakeMultiresRequest(MR_REQUEST_REFINE, point, point.level)};

        if (tick >= DEEP_COARSEN_TICK && tick < DEEP_COARSEN_TICK + test::MULTIRES_LEVELS) {
            const auto level = static_cast<int32_t>(test::MULTIRES_LEVELS - (tick - DEEP_COARSEN_TICK));
            return {test::MakeMultiresRequest(MR_REQUEST_COARSEN, point, level)};
        }

        return {};
    }

    // 全部の頁の両面が表の物質を全部(溢れ = 表の物質 − インライン)持てる塊の数(待たせない置き場)
    uint32_t WideChainChunks(const sim::MultiresNest& nest, const sim::BakedReactionTable& table) {
        const auto species = std::min(static_cast<uint32_t>(table.species.size()), RX_GPU_WIDE_SPECIES);

        return nest.capacity.pages * 2 * MrWideChunksFor(MR_BLOCK_CELLS * (species - RX_MAX_CELL_SPECIES));
    }

    // レベル level の本物のブロックのセルの成分の最大(溢れを含む)
    uint32_t MostSpeciesAtLevel(const sim::MultiresNest& nest, int32_t level) {
        uint32_t most = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL || block.level != level)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                most = std::max(most, sim::LoadWideNestCell(nest, slot, index).speciesCount);
        }

        return most;
    }

    // CPU の木の頁が今の溢れ(1 面)に要る塊の和(GPU の置き場の両面の目安は × 2)
    uint32_t CpuWideChunkNeed(const sim::MultiresNest& nest) {
        uint32_t chunks = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks + nest.capacity.observerBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind == MR_BLOCK_UNUSED || MrIsUniform(block))
                continue;

            uint32_t entries = 0;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const uint32_t species = sim::LoadWideNestCell(nest, slot, index).speciesCount;
                entries += species > RX_MAX_CELL_SPECIES ? species - RX_MAX_CELL_SPECIES : 0u;
            }

            chunks += MrWideChunksFor(entries);
        }

        return chunks;
    }

    std::string DescribeWideCounters(const sim::MultiresNest& cpu, const sim::MultiresNest& read,
                                     const sim::GpuMultires& gpu) {
        std::string text;
        for (uint32_t i = 0; i < MR_COUNTER_COUNT; ++i) {
            if (cpu.counters[i] != read.counters[i])
                text += std::format("器 {} cpu {} / gpu {}・", i, cpu.counters[i], read.counters[i]);
        }

        if (const auto use = gpu.ReadWideChunkUse(); use)
            text += std::format("塊 全部 {}・空き {}・頁 {}・端数 {}・一様 {}", use->capacity, use->freeChunks,
                                use->pageChunks, use->fractionChunks, use->uniformChunks);

        return text + std::format("・CPU の頁の 1 面に要る塊 {}", CpuWideChunkNeed(cpu));
    }

    std::expected<void, std::string> RunWideDeepChain(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                      gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                      bool paged, bool layered) {
        sim::MultiresNest cpu = MakeWideImageRoot(table, layered ? 3 : WIDE_NO_LAYERS, paged, true);
        sim::GpuMultiresOptions options = WideOptions();
        options.wideChunks = WideChainChunks(cpu, table);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, options);
        if (!gpu)
            return std::unexpected(gpu.error());

        const std::string scene = std::format("溢れの世界の 9 段の鎖({}・{}・塊 {})", paged ? "頁" : "一様の枠",
                                              layered ? "層あり" : "層なし", options.wideChunks);
        const uint64_t firstSum = RootLayerSum(cpu);
        uint32_t mostNeed = 0;
        uint32_t deepest = 0;
        for (uint64_t tick = 0; tick < DEEP_TICKS; ++tick) {
            const std::vector<MrRequest> requests = DeepChainRequestsAt(tick);
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordWideTick(list, *gpu, ring.GpuAddress(), requests, tick);
            };
            auto read = Execute(queue, ring, *gpu, record);
            if (!read)
                return std::unexpected(std::format("{} 刻み {}: {}", scene, tick, read.error()));

            StepWideCpu(cpu, table, requests, tick);
            mostNeed = std::max(mostNeed, CpuWideChunkNeed(cpu));
            deepest = std::max(deepest, MostSpeciesAtLevel(cpu, static_cast<int32_t>(test::MULTIRES_LEVELS)));
            if (auto compared = Compare(cpu, *read, std::format("{} 刻み {}", scene, tick)); !compared)
                return std::unexpected(std::format("{}({}・{})", compared.error(), DescribeWideDifference(cpu, *read),
                                                   DescribeWideCounters(cpu, *read, *gpu)));

            if (gpu->ReadLayerShortage() != 0u)
                return std::unexpected(std::format("{} 刻み {}: 足りるはずの層の頁が尽きた", scene, tick));
        }

        // 最も細かい段(9)のセルが溢れを持った(試験が溢れの鎖を通った)
        if (deepest <= RX_MAX_CELL_SPECIES)
            return std::unexpected(std::format("{}: 段 9 のセルの成分が {}(溢れの鎖になっていない)", scene, deepest));

        Log(Channel::Gpu, Level::Info,
            "{}: {} 刻み毎回一致(段 9 の成分 最大 {}・頁の 1 面に要る塊 最大 {}・層の和 {} → {})", scene, DEEP_TICKS,
            deepest, mostNeed, firstSum, RootLayerSum(cpu));

        return {};
    }

    // 鎖の最も細かい段の頁を、セルごとに層を持つ物質の違うセル(WideImageLayers の stride 3。子 2³ の和集合は 12 種)にして粗くする。
    // GPU は断って数え(層の頁の足りない数・空きが無い)、木と量は粗くしない CPU と一致する
    std::expected<void, std::string> RunWideCoarsenRefusal(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                           gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = MakeWideImageRoot(table, 3, true, true);
        sim::SubmitRequests(cpu, WideChainRequestsAt(WIDE_REFINE_TICK));
        sim::ProcessRequests(cpu);
        uint32_t deepest = MR_NO_BLOCK;
        for (uint32_t slot = 0; slot < cpu.capacity.worldBlocks; ++slot) {
            const MrBlock& block = cpu.blocks[slot];
            if (block.kind == MR_BLOCK_REAL && block.level == WIDE_CHAIN_LEVEL && !MrIsUniform(block))
                deepest = slot;
        }

        if (deepest == MR_NO_BLOCK)
            return std::unexpected("溢れの世界で断る: 頁の鎖を作れない");

        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const RxWideCell cell = WideImageCell(table, index);
            sim::StoreWideNestCellWithLayers(cpu, deepest, index, cell, WideImageLayers(cell, index, 3));
        }

        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, WideOptions());
        if (!gpu)
            return std::unexpected(gpu.error());

        const uint64_t tick = WIDE_COARSEN_TICK;
        const std::vector<MrRequest> requests = WideChainRequestsAt(tick);
        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            if (!gpu->RecordUpload(list, cpu))
                return false;

            return RecordWideTick(list, *gpu, ring.GpuAddress(), requests, tick);
        };
        auto read = Execute(queue, ring, *gpu, record);
        if (!read)
            return std::unexpected(std::format("溢れの世界で断る: {}", read.error()));

        StepWideCpu(cpu, table, {}, tick);
        const std::optional<uint32_t> shortage = gpu->ReadLayerShortage();
        if (shortage != 1u || read->counters[MR_COUNTER_NO_SPACE] != cpu.counters[MR_COUNTER_NO_SPACE] + 1)
            return std::unexpected(std::format("溢れの世界で断る: 断った数が違う(層 {}・空きが無い {})",
                                               shortage.value_or(~0u), read->counters[MR_COUNTER_NO_SPACE]));

        read->counters[MR_COUNTER_NO_SPACE] = cpu.counters[MR_COUNTER_NO_SPACE];
        if (auto compared = Compare(cpu, *read, "溢れの世界で断る"); !compared)
            return std::unexpected(std::format("{}({})", compared.error(), DescribeWideDifference(cpu, *read)));

        Log(Channel::Gpu, Level::Info,
            "溢れの世界で層の物質が 9 種以上になる粗くする: 断って数えた(粗くしない CPU と一致)");

        return {};
    }

    std::expected<sim::BakedReactionTable, std::string> MakeGrainBornTable();

    // --- 時間: 層のある場面の 1 刻み(全部を刻む)を層の頁あり・なしで ---
    std::expected<double, std::string> MeasureStep(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                   gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                   uint32_t layerPages, bool grainWorld = false) {
        sim::MultiresNest cpu = MakeScene(table, true);
        if (grainWorld)
            sim::EnableCellLayers(cpu, table);  // 粒を生む表なら刻む前の写しと比べる段を通る(T-1323)
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, GpuOptions(StepMode::Full, layerPages));
        if (!gpu)
            return std::unexpected(gpu.error());

        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            if (!gpu->RecordUpload(list, cpu))
                return false;

            for (uint32_t i = 0; i < WARMUP_STEPS; ++i)
                gpu->RecordStep(list, ring.GpuAddress(), test::MULTIRES_TEST_SEED, i);

            gpu->RecordTimestamp(list, 0);
            for (uint32_t i = 0; i < MEASURE_STEPS; ++i)
                gpu->RecordStep(list, ring.GpuAddress(), test::MULTIRES_TEST_SEED, WARMUP_STEPS + i);

            gpu->RecordTimestamp(list, 1);

            return true;
        };
        if (auto read = Execute(queue, ring, *gpu, record); !read)
            return std::unexpected(std::format("計測: {}", read.error()));

        const std::vector<uint64_t> stamps = gpu->ReadTimestamps(2);
        if (stamps.size() != 2)
            return std::unexpected("計測: タイムスタンプを読めない");

        uint64_t frequency = 0;
        if (FAILED(queue.Native()->GetTimestampFrequency(&frequency)) || frequency == 0)
            return std::unexpected("計測: タイムスタンプの周波数を読めない");

        return static_cast<double>(stamps[1] - stamps[0]) * 1000.0 / static_cast<double>(frequency) / MEASURE_STEPS;
    }

    // --- 9. 層を使う世界で反応が粒を生む表(T-1322・T-1323)---

    // 層を使う世界(EnableCellLayers。粒の印は表のまま = 熱分解の炭が粒)の燃える場面を毎刻み比べる。uniform なら燃える一様な根
    // (3/4 の一様の枠の層。刻むと頁に広げる)、でなければ燃える根を層なしで(粒が生まれた頁に GPU が層の頁を取る)
    std::expected<void, std::string> RunGrainBorn(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                  gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                  StepMode mode, bool uniform) {
        sim::MultiresNest cpu = uniform ? MakeUniformScene(table, BurningCrate(table), false) : MakeScene(table, false);
        sim::EnableCellLayers(cpu, table);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, GpuOptions(mode, LAYER_PAGES));
        if (!gpu)
            return std::unexpected(gpu.error());

        const std::string scene = std::format("粒を生む {}・{}", ModeName(mode), uniform ? "一様な根" : "層なしの根");
        const uint64_t firstSum = RootLayerSum(cpu);
        uint64_t mostSum = firstSum;
        for (uint64_t tick = 0; tick < LAYER_TICKS; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordGpuStep(list, *gpu, ring.GpuAddress(), mode, tick);
            };
            auto read = Execute(queue, ring, *gpu, record);
            if (!read)
                return std::unexpected(std::format("{} 刻み {}: {}", scene, tick, read.error()));

            StepCpu(cpu, table, mode, tick);
            if (auto compared = Compare(cpu, *read, std::format("{} 刻み {}", scene, tick)); !compared)
                return compared;

            if (gpu->ReadLayerShortage() != 0u)
                return std::unexpected(std::format("{} 刻み {}: 足りるはずの層の頁が尽きた", scene, tick));

            mostSum = std::max(mostSum, RootLayerSum(*read));
        }

        // 生まれた粒が層になった(試験が空でない。舞う粒のままなら層の和は増えない)
        if (mostSum <= firstSum)
            return std::unexpected(std::format("{}: 粒が層にならない(層の和 {} → 最大 {})", scene, firstSum, mostSum));

        Log(Channel::Gpu, Level::Info, "{}: {} 刻み毎回一致(層の和 {} → 最大 {})", scene, LAYER_TICKS, firstSum,
            mostSum);

        return {};
    }

    // 粒を生む表: 試験の燃える表に、湯気が粒の露(ばらける固体)になる規則を足す(燃えて出た湯気がすぐ粒になる)
    std::expected<sim::BakedReactionTable, std::string> MakeGrainBornTable() {
        sim::ReactionTableDefinition definition = sim::MakeCombustionTestTable();
        sim::AddDewCondensationForTest(definition, GRAIN_DEW_RATE_PER_SECOND);
        for (sim::SpeciesDefinition& species : definition.species) {
            if (species.name != "test_dew")
                continue;

            species.density = 917;  // 氷と同じ(kg/m³。ばらける固体は沈むか浮くかに要る)
            species.transport.solidMotion = sim::SolidMotion::Loose;
            species.transport.reposeAngleDegrees = 35;
            species.transport.packingPercent = sim::DEFAULT_PACKING_PERCENT;
        }

        return sim::BakeReactionTable(definition);
    }

    std::expected<void, std::string> RunGrainBornScenes(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                        gpu::DebugRing& ring) {
        const auto baked = MakeGrainBornTable();
        if (!baked)
            return std::unexpected(std::format("粒を生む表: {}", baked.error()));

        const sim::BakedReactionTable& table = *baked;
        for (const bool uniform : {false, true}) {
            for (const StepMode mode : {StepMode::Full, StepMode::Conduction, StepMode::Active}) {
                if (auto run = RunGrainBorn(device, queue, ring, table, mode, uniform); !run)
                    return run;
            }
        }

        return {};
    }

    std::expected<void, std::string> RunScenes(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                               const sim::BakedReactionTable& table) {
        if (auto image = TestImage(table); !image)
            return image;

        const auto limits = sim::BakeReactionTable(test::MakeLimitsTestTable());
        if (!limits)
            return std::unexpected("溢れの像: 表を作れない");

        if (auto wide = TestWideImage(*limits); !wide)
            return wide;

        // 9 段の鎖(T-1341): 層なしの頁・層のある頁・層のある一様の枠
        for (const auto& [paged, layered] : {std::pair{true, false}, std::pair{true, true}, std::pair{false, true}}) {
            if (auto deep = RunWideDeepChain(device, queue, ring, *limits, paged, layered); !deep)
                return deep;
        }

        // 層を使わない溢れの世界(層の段を通らない = 前と同じ)→ 層のある溢れの世界(頁・一様の枠)
        for (const auto& [paged, layered] : {std::pair{true, false}, std::pair{true, true}, std::pair{false, true}}) {
            if (auto chain = RunWideChain(device, queue, ring, *limits, paged, layered); !chain)
                return chain;
        }

        if (auto refusal = RunWideCoarsenRefusal(device, queue, ring, *limits); !refusal)
            return refusal;

        for (const StepMode mode : {StepMode::Full, StepMode::Conduction, StepMode::Active}) {
            if (auto run = RunEachTick(device, queue, ring, table, mode, true); !run)
                return run;
        }

        if (auto plain = RunEachTick(device, queue, ring, table, StepMode::Full, false); !plain)
            return plain;

        for (const test::MultiresScenario scenario : {test::MultiresScenario::Real, test::MultiresScenario::Shadow}) {
            if (auto chain = RunChain(device, queue, ring, table, scenario); !chain)
                return chain;
        }

        if (auto chain = RunChain(device, queue, ring, table, test::MultiresScenario::Real, true); !chain)
            return chain;

        if (auto shortage = RunShortage(device, queue, ring, table); !shortage)
            return shortage;

        if (auto fold = RunUniformFold(device, queue, ring, table); !fold)
            return fold;

        if (auto expand = RunUniformExpand(device, queue, ring, table); !expand)
            return expand;

        if (auto shadow = RunUniformShadow(device, queue, ring, table); !shadow)
            return shadow;

        if (auto coarsen = RunUniformCoarsenExpand(device, queue, ring, table); !coarsen)
            return coarsen;

        if (auto born = RunGrainBornScenes(device, queue, ring); !born)
            return born;

        return RunBatch(device, queue, ring, table);
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_multires_layers_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_multires_layers_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_layers_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_layers_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        // 計測は比べる実行の前に(WARP では測らない)
        if (options->adapter != gpu::AdapterKind::Warp) {
            const auto withLayers = MeasureStep(device->Get(), *queue, *ring, *table, LAYER_PAGES);
            const auto without = MeasureStep(device->Get(), *queue, *ring, *table, 0);
            if (!withLayers || !without) {
                Log(Channel::Gpu, Level::Error, "gpu_multires_layers_test: FAILED(計測)");
                return 1;
            }

            Log(Channel::Gpu, Level::Info,
                "GPU 時間: 1 刻み(全部を刻む・根 1 頁に層)層の頁あり {:.4f} ms・なし {:.4f} ms(層の置き場 {} B)",
                *withLayers, *without, uint64_t{LayerStoreWords(*table)} * sizeof(uint32_t));

            // 層を使う世界で粒を生む表(T-1323): 刻む前の写しと比べる段の分(同じ場面を粒を生む表で・層を使わない世界と比べる)
            const auto grainTable = MakeGrainBornTable();
            if (!grainTable) {
                Log(Channel::Gpu, Level::Error, "gpu_multires_layers_test: FAILED(粒を生む表: {})", grainTable.error());
                return 1;
            }

            const auto grainPlain = MeasureStep(device->Get(), *queue, *ring, *grainTable, LAYER_PAGES);
            const auto grainBorn = MeasureStep(device->Get(), *queue, *ring, *grainTable, LAYER_PAGES, true);
            if (!grainPlain || !grainBorn) {
                Log(Channel::Gpu, Level::Error, "gpu_multires_layers_test: FAILED(粒を生む表の計測)");
                return 1;
            }

            Log(Channel::Gpu, Level::Info,
                "GPU 時間: 粒を生む表の 1 刻み(全部を刻む・根 1 頁に層)層を使う世界 {:.4f} ms・使わない世界 {:.4f} ms",
                *grainBorn, *grainPlain);
        }

        if (auto scenes = RunScenes(device->Get(), *queue, *ring, *table); !scenes) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_layers_test: FAILED ({})", scenes.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_multires_layers_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_multires_layers_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
