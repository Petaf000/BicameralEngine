// multires_layers_test.cpp — セルの成分ごとの層の量(T-1210。ADR-0481 の 5・20 §10.5)の CPU リファレンスの試験(1 段目: 頁のセル)。
//   - 付け替え: 成分の量だけを書く道は、層を物質で結んで 層 = min(前の層, 新しい量) にする(増えた分は残りへ・無くなった物質の層は 0・
//     新しい物質の層は 0・並びの位置がずれても物質で結ぶ・頁の幅が広がる / 詰まる時も層を保つ)
//   - 刻み: 層のある世界と無い世界で、セルの値(成分の量・エネルギー)は毎刻みビット一致(物質は 1 つのまま。反応は量の全部に当たる)。
//     層は毎刻み 0 ≤ 層 ≤ 量 で、どの物質の層も増えない(層を書く法則がまだ無いので)
//   - 要約と保存点: 層は状態の要約(HashWholeNest・HashRealLeaves)に入り、保存点から戻る。層の無い世界の要約は層を足して消した後と同じ
//   - 頁を返す: 細かくした子の頁に層を書いて粗くして返し、また細かくしても古い層が残らない
//   - 眠る: 層のある頁は畳まない(1 段目)。層の無い同じ頁は畳む
//   - 費用: 層のある頁の 1 セルのバイトと、書く道の時間(表示だけ。docs/perf.md 用)
// 2 段目(T-1220。ADR-0489。反応しない物質 12 種の表):
//   - 細かくする / 粗くする(インライン・溢れの世界): 子は親と同じ量と層(子の層の和 = 親の層 × 8)・粗くすると量は元に戻り層は成分ごとに和 ÷ 8 の切り捨て
//   - 一様の枠: 層まで一様な頁は層ごと畳む・一様な子へ写す・同じ層なら一様な親のまま・頁に広げると写す・要約と保存点に入る
//   - 溢れ: インライン ↔ 溢れの間で物質が動いても層は物質についていく・要約と保存点
//   - 層を使う世界の眠る: 粒の物質の残り(舞う粒)のある頁は畳まない(気体などほかの物質の残りは数えない)
// 表は試験のパッケージの表(tests/reaction_test_table.h)。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

#include "common/multires_activity.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "multires_test_scene.h"
#include "reaction_limits_table.h"
#include "reaction_test_table.h"
#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"
#include "sim/multires_state.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    constexpr int32_t ROOM_MILLIKELVIN = 300000;
    constexpr uint64_t LAYER_SEED = 0x6c61796572120000ull;

    // 物質 species の層(セルに無ければ 0)
    uint64_t LayerOf(const RxCell& cell, const MrCellLayers& layers, uint32_t species) {
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            if (cell.species[i] == species)
                return layers.amounts[i];
        }

        return 0;
    }

    // セルの成分ごとに 量 × numerator ÷ 4 の層
    MrCellLayers QuarterLayers(const RxCell& cell, uint64_t numerator) {
        MrCellLayers layers;
        for (uint32_t i = 0; i < cell.speciesCount; ++i)
            layers.amounts[i] = cell.amounts[i] / 4 * numerator;

        return layers;
    }

    // 枠 slot の全部のセルに QuarterLayers の層を書く
    void LayerWholeBlock(MultiresNest& nest, uint32_t slot, uint64_t numerator) {
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const RxCell cell = LoadNestCell(nest, slot, index);
            StoreNestCellWithLayers(nest, slot, index, cell, QuarterLayers(cell, numerator));
        }
    }

    // 根の頁を持つ小さな世界(セルは全部 cell。1 つだけ違う値で置いてから書き直して頁を取る)
    MultiresNest MakePagedRoot(const BakedReactionTable& table, const RxCell& cell) {
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(table, 4, 0, 4));
        std::vector<RxCell> rootCells(MR_BLOCK_CELLS, cell);
        rootCells[1] = RxMakeEmptyCell(1);
        const uint32_t root = PlaceRootBlock(nest, 0, 0, 0, rootCells);
        StoreNestCell(nest, root, 1, cell);

        return nest;
    }

    RxCell MakeCell(const BakedReactionTable& table,
                    std::initializer_list<std::pair<std::string_view, uint64_t>> parts) {
        std::vector<SpeciesAmount> amounts;
        for (const auto& [name, amount] : parts)
            amounts.push_back({.species = table.SpeciesId(name), .amount = amount});

        return MakeReactionCell(table, amounts, ROOM_MILLIKELVIN);
    }

    // --- 付け替え ---

    void TestRebind(const BakedReactionTable& table) {
        const uint32_t cellulose = table.SpeciesId("cellulose");
        const uint32_t nitrogen = table.SpeciesId("nitrogen");
        const uint32_t oxygen = table.SpeciesId("oxygen");
        MultiresNest nest = MakePagedRoot(table, MakeCell(table, {{"nitrogen", 1000}}));
        const uint32_t root = 0;
        const uint32_t page = nest.blocks[root].page;
        nest_detail::NarrowPageCells(nest, page);
        Expect(nest.cells.pages[page].width == 1, "付け替え: 1 成分の頁の幅は 1");
        Expect(nest.cells.pages[page].layers.empty(), "付け替え: 層を書いていない頁は層の並びを持たない");

        // --- 層を書く(粉 600 / 1000・酸素 500 / 500)---
        const RxCell first = MakeCell(table, {{"cellulose", 1000}, {"oxygen", 500}});
        MrCellLayers firstLayers;
        firstLayers.amounts[0] = cellulose < oxygen ? 600 : 500;
        firstLayers.amounts[1] = cellulose < oxygen ? 500 : 600;
        StoreNestCellWithLayers(nest, root, 7, first, firstLayers);
        Expect(nest.cells.pages[page].width == 2, "付け替え: 成分が増えると頁が広がる");
        Expect(LayerOf(first, LoadNestCellLayers(nest, root, 7), cellulose) == 600, "付け替え: 書いた層を読む");

        // --- 量だけを書く: 粉は減って層の上 → 層は残る・窒素が間に入る(位置がずれる)・酸素は増える → 層は前のまま ---
        const RxCell second = MakeCell(table, {{"cellulose", 800}, {"nitrogen", 300}, {"oxygen", 700}});
        StoreNestCell(nest, root, 7, second);
        MrCellLayers layers = LoadNestCellLayers(nest, root, 7);
        Expect(LayerOf(second, layers, cellulose) == 600, "付け替え: 減っても層より多ければ層はそのまま");
        Expect(LayerOf(second, layers, nitrogen) == 0, "付け替え: 新しい物質の層は 0");
        Expect(LayerOf(second, layers, oxygen) == 500, "付け替え: 増えた分は残りへ(層は前のまま)");
        Expect(nest.cells.pages[page].width == 3, "付け替え: 頁が広がっても層を保つ");

        // --- 粉が無くなり、酸素が層より少なくなる ---
        const RxCell third = MakeCell(table, {{"nitrogen", 300}, {"oxygen", 200}});
        StoreNestCell(nest, root, 7, third);
        layers = LoadNestCellLayers(nest, root, 7);
        Expect(LayerOf(third, layers, oxygen) == 200, "付け替え: 層より少なくなれば 層 = 量");
        Expect(MrLayersFit(third, layers), "付け替え: 成分の数の外の層は 0");

        // --- 粉が戻る(前の層は覚えていない)---
        const RxCell fourth = MakeCell(table, {{"cellulose", 900}, {"nitrogen", 300}, {"oxygen", 900}});
        StoreNestCell(nest, root, 7, fourth);
        layers = LoadNestCellLayers(nest, root, 7);
        Expect(LayerOf(fourth, layers, cellulose) == 0, "付け替え: 一度無くなった物質の層は 0 から");
        Expect(LayerOf(fourth, layers, oxygen) == 200, "付け替え: 増えても層は前の 200 のまま");

        // --- 頁を詰める(層のあるセルを 1 成分に戻すと幅 1・層の並びも)---
        StoreNestCell(nest, root, 7, MakeCell(table, {{"oxygen", 900}}));
        nest_detail::NarrowPageCells(nest, page);
        Expect(nest.cells.pages[page].width == 1, "付け替え: 詰めると幅 1");
        Expect(LoadNestCellLayers(nest, root, 7).amounts[0] == 200, "付け替え: 詰めても層を保つ");
        StoreNestCell(nest, root, 7, MakeCell(table, {{"oxygen", 100}}));
        StoreNestCell(nest, root, 7, MakeCell(table, {{"oxygen", 900}}));
        nest_detail::NarrowPageCells(nest, page);
        Expect(LoadNestCellLayers(nest, root, 7).amounts[0] == 100, "付け替え: 減って増えると層は減った時の量");
        StoreNestCellWithLayers(nest, root, 7, MakeCell(table, {{"oxygen", 900}}), MrCellLayers{});
        nest_detail::NarrowPageCells(nest, page);
        Expect(nest.cells.pages[page].layers.empty(), "付け替え: 層が全部 0 になった頁は層の並びを返す");
        Expect(CountLayerMisfits(nest) == 0, "付け替え: 0 ≤ 層 ≤ 量");

        // --- 検査が壊れた層を見つける ---
        StoreNestCellWithLayers(nest, root, 3, MakeCell(table, {{"nitrogen", 1000}}),
                                QuarterLayers(MakeCell(table, {{"nitrogen", 1000}}), 2));
        nest.cells.pages[page].layers[3] = 1001;
        Expect(CountLayerMisfits(nest) == 1, "付け替え: 量を超える層を数える");
    }

    // --- 刻み: セルの値は層に依らない・層は増えない ---

    struct LayerEntry {
        uint32_t species = 0;
        uint64_t layer = 0;
    };

    std::vector<std::vector<LayerEntry>> RootLayers(const MultiresNest& nest) {
        std::vector<std::vector<LayerEntry>> result(MR_BLOCK_CELLS);
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const RxCell cell = LoadNestCell(nest, test::MULTIRES_ROOT_SLOT, index);
            const MrCellLayers layers = LoadNestCellLayers(nest, test::MULTIRES_ROOT_SLOT, index);
            for (uint32_t i = 0; i < cell.speciesCount; ++i)
                result[index].push_back({.species = cell.species[i], .layer = layers.amounts[i]});
        }

        return result;
    }

    uint64_t PreviousLayer(const std::vector<LayerEntry>& entries, uint32_t species) {
        for (const LayerEntry& entry : entries) {
            if (entry.species == species)
                return entry.layer;
        }

        return 0;
    }

    void TestStepKeepsCells(const BakedReactionTable& table) {
        MultiresNest plain = test::MakeMultiresNestForTest(table);
        MultiresNest layered = test::MakeMultiresNestForTest(table);
        const uint64_t plainHash = HashWholeNest(layered);
        LayerWholeBlock(layered, test::MULTIRES_ROOT_SLOT, 3);
        Expect(HashWholeNest(layered) != plainHash, "刻み: 層は状態の要約に入る");
        Expect(HashRealLeaves(layered) != HashRealLeaves(plain), "刻み: 層は本物の葉の要約に入る");

        int mismatchedTicks = 0;
        int misfitTicks = 0;
        uint64_t grown = 0;
        uint64_t shrunk = 0;
        std::vector<std::vector<LayerEntry>> before = RootLayers(layered);
        const auto sameCell = [](const RxCell& a, const RxCell& b) {
            return RxSameCell(a, b);
        };
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            test::StepMultiresScene(plain, table, test::MultiresScenario::Real, tick);
            test::StepMultiresScene(layered, table, test::MultiresScenario::Real, tick);
            mismatchedTicks += std::ranges::equal(DenseCellImage(plain), DenseCellImage(layered), sameCell) ? 0 : 1;
            misfitTicks += CountLayerMisfits(layered) == 0 ? 0 : 1;

            // 子に覆われたセルは覆う前の層と比べる(子は親と同じ層で始まり、粗くすると層の和 ÷ 8 で戻る。T-1220)
            const std::vector<std::vector<LayerEntry>> after = RootLayers(layered);
            const MrBlock& rootBlock = layered.blocks[test::MULTIRES_ROOT_SLOT];
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (MrIsCoveredCell(rootBlock, index))
                    continue;

                for (const LayerEntry& entry : after[index]) {
                    const uint64_t previous = PreviousLayer(before[index], entry.species);
                    grown += entry.layer > previous ? 1u : 0u;
                    shrunk += entry.layer < previous ? 1u : 0u;
                }

                before[index] = after[index];
            }
        }

        uint64_t remaining = 0;
        for (const std::vector<LayerEntry>& entries : before) {
            for (const LayerEntry& entry : entries)
                remaining += entry.layer != 0 ? 1u : 0u;
        }

        Expect(mismatchedTicks == 0, "刻み: 層のある世界と無い世界でセルの値が毎刻みビット一致");
        Expect(misfitTicks == 0, "刻み: 毎刻み 0 ≤ 層 ≤ 量");
        Expect(grown == 0, "刻み: 層を書く法則が無ければ層は増えない");
        Expect(shrunk > 0, "刻み: 燃えて減った物質の層が減る(試験が空でない)");
        Expect(remaining > 0, "刻み: 減らない物質の層は残る");
        Log(Channel::Sim, Level::Info, "刻み: 層が減った成分 {}・最後に層のある成分 {}", shrunk, remaining);
    }

    // --- 要約と保存点 ---

    void TestHashAndSave(const BakedReactionTable& table) {
        MultiresNest nest = test::MakeMultiresNestForTest(table);
        const uint64_t plainHash = HashWholeNest(nest);
        const MultiresSaveState plainSave = SaveNestState(nest);

        LayerWholeBlock(nest, test::MULTIRES_ROOT_SLOT, 2);
        const uint64_t layeredHash = HashWholeNest(nest);
        const MultiresSaveState layeredSave = SaveNestState(nest);
        Expect(layeredHash != plainHash, "保存点: 層は要約に入る");

        // --- 1 セルの層だけを変えると要約が変わる ---
        const RxCell cell = LoadNestCell(nest, test::MULTIRES_ROOT_SLOT, 9);
        StoreNestCellWithLayers(nest, test::MULTIRES_ROOT_SLOT, 9, cell, QuarterLayers(cell, 1));
        Expect(HashWholeNest(nest) != layeredHash, "保存点: 1 セルの層の違いが要約に出る");

        // --- 保存点から戻す ---
        Expect(RestoreNestState(nest, layeredSave), "保存点: 層のある状態に戻せる");
        Expect(HashWholeNest(nest) == layeredHash, "保存点: 戻すと層まで同じ");
        Expect(LoadNestCellLayers(nest, test::MULTIRES_ROOT_SLOT, 9).amounts == QuarterLayers(cell, 2).amounts,
               "保存点: 戻した層を読む");
        Expect(RestoreNestState(nest, plainSave), "保存点: 層の無い状態に戻せる");
        Expect(HashWholeNest(nest) == plainHash, "保存点: 層の無い状態の要約は層を書く前と同じ");

        // --- 層を全部 0 に書き直すと(並びを持っていても)層の無い世界と同じ要約 ---
        LayerWholeBlock(nest, test::MULTIRES_ROOT_SLOT, 2);
        LayerWholeBlock(nest, test::MULTIRES_ROOT_SLOT, 0);
        Expect(HashWholeNest(nest) == plainHash, "保存点: 層が全部 0 なら層の無い世界と同じ要約");
    }

    // --- 頁を返す: 古い層が残らない ---

    void TestReleasedPagesForgetLayers(const BakedReactionTable& table) {
        constexpr uint64_t LAYER_TICK = 30;
        MultiresNest nest = test::MakeMultiresNestForTest(table);
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            test::StepMultiresScene(nest, table, test::MultiresScenario::Real, tick);
            if (tick != LAYER_TICK)
                continue;

            uint32_t layeredBlocks = 0;
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                const MrBlock& block = nest.blocks[slot];
                if (slot == test::MULTIRES_ROOT_SLOT || block.kind != MR_BLOCK_REAL || MrIsUniform(block))
                    continue;

                LayerWholeBlock(nest, slot, 4);
                layeredBlocks += 1;
            }

            Expect(layeredBlocks > 0, "頁を返す: 細かくした子の頁がある");
        }

        Expect(CountLayerMisfits(nest) == 0, "頁を返す: 0 ≤ 層 ≤ 量");
        const uint32_t rootPage = nest.blocks[test::MULTIRES_ROOT_SLOT].page;
        uint32_t stalePages = 0;
        for (uint32_t page = 0; page < nest.cells.pages.size(); ++page)
            stalePages += page != rootPage && !nest.cells.pages[page].layers.empty() ? 1u : 0u;

        Expect(stalePages == 0, "頁を返す: 粗くして返した頁は層を持たない");

        // --- 根の層を 0 にする(粗くすると子の層は根へ戻るので。T-1220)---
        if (!MrIsUniform(nest.blocks[test::MULTIRES_ROOT_SLOT])) {
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxCell cell = LoadNestCell(nest, test::MULTIRES_ROOT_SLOT, index);
                StoreNestCellWithLayers(nest, test::MULTIRES_ROOT_SLOT, index, cell, MrCellLayers{});
            }
        }

        // --- もう一度細かくする(返した頁を使う)---
        const MultiresPoint point = test::MakeMultiresPoint(test::MULTIRES_LEVELS);
        const MrRequest request = test::MakeMultiresRequest(MR_REQUEST_REFINE, point, point.level);
        SubmitRequests(nest, {&request, 1});
        ProcessRequests(nest);
        uint32_t refined = 0;
        uint32_t staleCells = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (slot == test::MULTIRES_ROOT_SLOT || block.kind != MR_BLOCK_REAL || MrIsUniform(block))
                continue;

            refined += 1;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const MrCellLayers layers = LoadNestCellLayers(nest, slot, index);
                staleCells += std::ranges::any_of(layers.amounts, [](uint64_t layer) { return layer != 0; }) ? 1u : 0u;
            }
        }

        Expect(refined > 0, "頁を返す: もう一度細かくした");
        Expect(staleCells == 0, "頁を返す: 使い直した頁に古い層が残らない");
    }

    // --- 眠る: 層のある頁は畳まない ---

    bool RootFoldsAfterQuiet(const BakedReactionTable& table, bool withLayer) {
        const RxCell air = MakeCell(table, {{"nitrogen", 3697000}, {"oxygen", 983000}});
        MultiresNest nest = MakePagedRoot(table, air);
        if (withLayer)
            StoreNestCellWithLayers(nest, 0, 0, air, QuarterLayers(air, 1));

        for (uint64_t tick = 0; tick < MR_QUIET_TICKS + 4; ++tick) {
            StepActive(nest, table, LAYER_SEED, tick);
            FoldQuietPages(nest, tick);
        }

        return MrIsUniform(nest.blocks[0]);
    }

    void TestLayeredPageStaysAwake(const BakedReactionTable& table) {
        Expect(RootFoldsAfterQuiet(table, false), "眠る: 層の無い一様な頁は畳む(試験が空でない)");
        Expect(!RootFoldsAfterQuiet(table, true), "眠る: 層のある頁は畳まない");
    }

    // --- 2 段目(T-1220): 細かくする / 粗くする・一様の枠・溢れの成分・層を使う世界の眠る ---

    constexpr uint32_t INERT_SPECIES = 12;

    // 反応しない物質 12 種の表(multires_wide_test と同じ形)。grainSpecies が 0 でなければその物質を粒に
    BakedReactionTable MakeInertTable(uint32_t grainSpecies = 0) {
        ReactionTableDefinition definition;
        definition.elements = {{.name = "X", .atomicMass = 10000}};
        for (uint32_t i = 1; i <= INERT_SPECIES; ++i)
            definition.species.push_back(test::LimitsSpecies(std::format("s{:02}", i), -700 * static_cast<int64_t>(i)));

        BakedReactionTable table = BakeReactionTable(definition).value();
        if (grainSpecies != 0)
            table.species[table.SpeciesId(std::format("s{:02}", grainSpecies))].flags = RX_SPECIES_GRAIN;

        return table;
    }

    // 物質 s01〜s{count} のセル(量はセルと物質で変える)
    RxWideCell MakeInertCell(const BakedReactionTable& table, uint32_t count, uint32_t index) {
        std::vector<SpeciesAmount> amounts;
        for (uint32_t i = 1; i <= count; ++i) {
            const uint64_t amount = 1000000 + (uint64_t{index} * 37) + (uint64_t{i} * 11);
            amounts.push_back({.species = table.SpeciesId(std::format("s{:02}", i)), .amount = amount});
        }

        return MakeWideReactionCell(table, amounts, ROOM_MILLIKELVIN);
    }

    // 成分ごとに 量 ÷ 4 × ((index + 位置) % 5)(0〜量)
    std::vector<uint64_t> SomeLayers(const RxWideCell& cell, uint32_t index) {
        std::vector<uint64_t> layers(cell.speciesCount, 0);
        for (uint32_t i = 0; i < cell.speciesCount; ++i)
            layers[i] = cell.amounts[i] / 4 * ((index + i) % 5);

        return layers;
    }

    // 物質 species の層(セルに無ければ 0)
    uint64_t WideLayerOf(const RxWideCell& cell, const std::vector<uint64_t>& layers, uint32_t species) {
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            if (cell.species[i] == species)
                return layers[i];
        }

        return 0;
    }

    struct LayeredWorld {
        MultiresNest nest;
        uint32_t root = MR_NO_BLOCK;
    };

    // 根の頁(セル index は cellOf(index)・層は layersOf(cell, index))。wide なら溢れを使う世界
    template <typename CellOf, typename LayersOf>
    LayeredWorld MakeLayeredRoot(const BakedReactionTable& table, bool wide, CellOf cellOf, LayersOf layersOf) {
        LayeredWorld world{.nest = MakeMultiresNest(test::MakeMultiresCapacity(table, 16, 0, 16))};
        if (wide)
            EnableWideCells(world.nest);

        std::vector<RxCell> cells(MR_BLOCK_CELLS, RxMakeEmptyCell(0));
        cells[1] = RxMakeEmptyCell(1);  // 頁を取る
        world.root = PlaceRootBlock(world.nest, 0, 0, 0, cells);
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const RxWideCell cell = cellOf(index);
            StoreWideNestCellWithLayers(world.nest, world.root, index, cell, layersOf(cell, index));
        }

        return world;
    }

    void Request(MultiresNest& nest, uint32_t op) {
        const MrRequest request = MrMakeRequest(op, 1, 1, 1, 1);
        SubmitRequests(nest, std::span(&request, 1));
        ProcessRequests(nest);
    }

    uint32_t ChildOfRoot(const MultiresNest& nest) {
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            if (nest.blocks[slot].kind == MR_BLOCK_REAL && nest.blocks[slot].level == 1)
                return slot;
        }

        return MR_NO_BLOCK;
    }

    // 細かくする(子は親と同じ量・同じ層)→ 子の層を変える → 粗くする(層は成分ごとに和 ÷ 8 の切り捨て・量は元に戻る)
    void TestRefineCoarsen(const BakedReactionTable& table, bool wide) {
        const uint32_t count = wide ? INERT_SPECIES : 4;
        const std::string_view name = wide ? "溢れの世界" : "インラインの世界";
        const auto cellOf = [&](uint32_t index) {
            return MakeInertCell(table, count, index);
        };
        LayeredWorld world = MakeLayeredRoot(table, wide, cellOf, SomeLayers);
        MultiresNest& nest = world.nest;

        Request(nest, MR_REQUEST_REFINE);
        const uint32_t child = ChildOfRoot(nest);
        Expect(child != MR_NO_BLOCK, std::format("細かくする({}): 子がある", name));
        if (child == MR_NO_BLOCK)
            return;

        // --- 子は親と同じ量と層(子の層の和 = 親の層 × 8)---
        const uint32_t octant = nest.blocks[child].parentOctant;
        uint32_t sameMismatches = 0;
        for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
            const uint32_t parentIndex = MrOctantCell(octant, local);
            const RxWideCell parent = cellOf(parentIndex);
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                const uint32_t index = MrChildCell(local, j);
                const bool same = RxSameCell(LoadWideNestCell(nest, child, index), parent) &&
                                  LoadWideNestCellLayers(nest, child, index) == SomeLayers(parent, parentIndex);
                sameMismatches += same ? 0u : 1u;
            }
        }

        Expect(sameMismatches == 0, std::format("細かくする({}): 子は親と同じ量と層", name));
        Expect(CountLayerMisfits(nest) == 0, std::format("細かくする({}): 0 ≤ 層 ≤ 量", name));

        // --- 子の層を子ごとに変える(子 j は 量 × j ÷ 8)---
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const RxWideCell cell = LoadWideNestCell(nest, child, index);
            std::vector<uint64_t> layers(cell.speciesCount, 0);
            for (uint32_t i = 0; i < cell.speciesCount; ++i)
                layers[i] = cell.amounts[i] / 8 * (index % 8);

            StoreWideNestCellWithLayers(nest, child, index, cell, layers);
        }

        std::vector<std::vector<uint64_t>> expected(MR_OCTANT_CELLS);
        for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
            expected[local].assign(count, 0);
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                const std::vector<uint64_t> layers = LoadWideNestCellLayers(nest, child, MrChildCell(local, j));
                for (uint32_t i = 0; i < count; ++i)
                    expected[local][i] += layers[i];
            }

            for (uint64_t& layer : expected[local])
                layer /= MR_CHILDREN_PER_CELL;
        }

        // --- 粗くする ---
        Request(nest, MR_REQUEST_COARSEN);
        Expect(ChildOfRoot(nest) == MR_NO_BLOCK, std::format("粗くする({}): 子が無い", name));
        uint32_t coarseMismatches = 0;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const bool inOctant = MrOctantOfCell(index) == octant;
            const RxWideCell original = cellOf(index);
            uint32_t local = 0;
            while (inOctant && MrOctantCell(octant, local) != index)
                ++local;

            const std::vector<uint64_t> want = inOctant ? expected[local] : SomeLayers(original, index);
            const bool same = RxSameCell(LoadWideNestCell(nest, world.root, index), original) &&
                              LoadWideNestCellLayers(nest, world.root, index) == want;
            coarseMismatches += same ? 0u : 1u;
        }

        Expect(coarseMismatches == 0, std::format("粗くする({}): 量は元に戻り、層は成分ごとに和 ÷ 8 の切り捨て", name));
        Expect(CountLayerMisfits(nest) == 0, std::format("粗くする({}): 0 ≤ 層 ≤ 量", name));
    }

    // 静かな刻みを回して頁を畳む(畳めたら true)
    bool FoldsAfterQuiet(MultiresNest& nest, const BakedReactionTable& table, uint32_t slot) {
        for (uint64_t tick = 0; tick < MR_QUIET_TICKS + 4; ++tick) {
            StepActive(nest, table, LAYER_SEED, tick);
            FoldQuietPages(nest, tick);
        }

        return MrIsUniform(nest.blocks[slot]);
    }

    // 一様の枠の層: 層まで一様な頁は層ごと畳む → 一様な子へ写す → 一様な親に戻る → 頁に広げて写す → 層の違う子を粗くすると親は頁に
    void TestUniformLayers(const BakedReactionTable& table, bool wide) {
        const uint32_t count = wide ? INERT_SPECIES : 4;
        const std::string_view name = wide ? "溢れの世界" : "インラインの世界";
        const RxWideCell value = MakeInertCell(table, count, 0);
        const std::vector<uint64_t> layers = SomeLayers(value, 2);
        LayeredWorld world = MakeLayeredRoot(
            table, wide, [&](uint32_t) { return value; }, [&](const RxWideCell&, uint32_t) { return layers; });
        MultiresNest& nest = world.nest;
        const uint64_t layeredPageHash = HashWholeNest(nest);

        Expect(FoldsAfterQuiet(nest, table, world.root), std::format("一様({}): 層まで一様な頁は畳む", name));
        Expect(LoadWideNestCellLayers(nest, world.root, 7) == layers,
               std::format("一様({}): 一様の値の層を読む", name));
        Expect(CountLayerMisfits(nest) == 0, std::format("一様({}): 0 ≤ 層 ≤ 量", name));
        Expect(HashWholeNest(nest) != layeredPageHash,
               std::format("一様({}): 畳むと要約は頁の時と違う(区画が違う)", name));

        // --- 要約と保存点 ---
        const uint64_t uniformHash = HashWholeNest(nest);
        const uint64_t uniformLeaves = HashRealLeaves(nest);
        const MultiresSaveState save = SaveNestState(nest);
        LayeredWorld plain = MakeLayeredRoot(
            table, wide, [&](uint32_t) { return value; },
            [&](const RxWideCell& cell, uint32_t) { return std::vector<uint64_t>(cell.speciesCount, 0); });
        Expect(FoldsAfterQuiet(plain.nest, table, plain.root), std::format("一様({}): 層の無い頁も畳む", name));
        Expect(HashWholeNest(plain.nest) != uniformHash && HashRealLeaves(plain.nest) != uniformLeaves,
               std::format("一様({}): 一様の値の層は要約に入る", name));

        // --- 一様な子は親と同じ層 ---
        Request(nest, MR_REQUEST_REFINE);
        const uint32_t child = ChildOfRoot(nest);
        Expect(child != MR_NO_BLOCK && MrIsUniform(nest.blocks[child]), std::format("一様({}): 一様な子", name));
        if (child == MR_NO_BLOCK)
            return;

        Expect(LoadWideNestCellLayers(nest, child, 3) == layers, std::format("一様({}): 一様な子は親と同じ層", name));

        // --- 粗くすると一様な親のまま(層も同じ)---
        Request(nest, MR_REQUEST_COARSEN);
        Expect(MrIsUniform(nest.blocks[world.root]) && LoadWideNestCellLayers(nest, world.root, 3) == layers,
               std::format("一様({}): 同じ層の子を粗くすると一様な親のまま", name));
        Expect(RestoreNestState(nest, save) && HashWholeNest(nest) == uniformHash,
               std::format("一様({}): 保存点から一様の値の層まで戻る", name));

        // --- 頁に広げると層を写す。1 セルの層を変えた子を粗くすると親は頁に広がる ---
        Request(nest, MR_REQUEST_REFINE);
        const uint32_t again = ChildOfRoot(nest);
        Expect(again != MR_NO_BLOCK && ExpandNestPage(nest, again), std::format("一様({}): 子を頁に広げる", name));
        uint32_t expandMismatches = 0;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
            expandMismatches += LoadWideNestCellLayers(nest, again, index) == layers ? 0u : 1u;

        Expect(expandMismatches == 0, std::format("一様({}): 頁に広げると層を写す", name));
        const std::vector<uint64_t> none(value.speciesCount, 0);
        StoreWideNestCellWithLayers(nest, again, 0, value, none);  // セル 0 は子 0 の組(local 0)
        const uint32_t octant = nest.blocks[again].parentOctant;
        Request(nest, MR_REQUEST_COARSEN);
        Expect(!MrIsUniform(nest.blocks[world.root]), std::format("一様({}): 層の違う子を粗くすると親は頁に", name));

        std::vector<uint64_t> mixed(value.speciesCount, 0);
        for (uint32_t i = 0; i < value.speciesCount; ++i)
            mixed[i] = layers[i] * 7 / 8;

        const uint32_t outside = MrOctantCell((octant + 1) % 8, 0);
        Expect(LoadWideNestCellLayers(nest, world.root, MrOctantCell(octant, 0)) == mixed &&
                   LoadWideNestCellLayers(nest, world.root, outside) == layers,
               std::format("一様({}): 粗くした所は層の和 ÷ 8・外は一様の値の層", name));
        Expect(CountLayerMisfits(nest) == 0, std::format("一様({}): 0 ≤ 層 ≤ 量(最後)", name));
    }

    // 溢れの成分の層: インライン ↔ 溢れの間で物質が動いても層は物質についていく
    void TestOverflowRebind(const BakedReactionTable& table) {
        const RxWideCell full = MakeInertCell(table, INERT_SPECIES, 0);
        LayeredWorld world = MakeLayeredRoot(table, true, [&](uint32_t) { return full; }, SomeLayers);
        MultiresNest& nest = world.nest;
        const std::vector<uint64_t> before = LoadWideNestCellLayers(nest, world.root, 3);
        const uint32_t s01 = table.SpeciesId("s01");
        const uint32_t s09 = table.SpeciesId("s09");

        // --- s01 を消す(s09 がインラインへ)→ 戻す(s09 は溢れへ。戻した s01 の層は 0)---
        RxWideCell fewer = full;
        fewer.species.erase(fewer.species.begin());
        fewer.amounts.erase(fewer.amounts.begin());
        fewer.speciesCount -= 1;
        StoreWidePageCell(nest, nest.blocks[world.root].page, 3, fewer);
        const RxWideCell moved = LoadWideNestCell(nest, world.root, 3);
        const std::vector<uint64_t> movedLayers = LoadWideNestCellLayers(nest, world.root, 3);
        Expect(WideLayerOf(moved, movedLayers, s09) == WideLayerOf(full, before, s09) &&
                   WideLayerOf(full, before, s09) != 0,
               "溢れ: 溢れからインラインへ動いた物質の層を保つ");

        StoreWidePageCell(nest, nest.blocks[world.root].page, 3, full);
        const std::vector<uint64_t> back = LoadWideNestCellLayers(nest, world.root, 3);
        Expect(WideLayerOf(full, back, s09) == WideLayerOf(full, before, s09) && WideLayerOf(full, back, s01) == 0,
               "溢れ: インラインから溢れへ動いた物質の層を保ち、新しく来た物質の層は 0");
        Expect(CountLayerMisfits(nest) == 0, "溢れ: 0 ≤ 層 ≤ 量");

        // --- 溢れの成分の層は要約と保存点に入る ---
        const uint64_t hash = HashWholeNest(nest);
        const MultiresSaveState save = SaveNestState(nest);
        std::vector<uint64_t> changed = back;
        changed[INERT_SPECIES - 1] = 0;
        StoreWideNestCellWithLayers(nest, world.root, 3, full, changed);
        Expect(HashWholeNest(nest) != hash, "溢れ: 溢れの成分の層の違いが要約に出る");
        Expect(RestoreNestState(nest, save) && LoadWideNestCellLayers(nest, world.root, 3) == back,
               "溢れ: 保存点から溢れの成分の層まで戻る");
    }

    // 層を使う世界の眠る: 粒の物質の残り(舞う粒)のある頁は畳まない。粒が全部層なら畳む。気体の残りは数えない
    void TestFloatingGrainStaysAwake() {
        const BakedReactionTable table = MakeInertTable(1);
        const uint32_t s01 = table.SpeciesId("s01");
        const auto foldsWith = [&](uint64_t grainNumerator) {
            const RxWideCell value = MakeInertCell(table, 4, 0);
            std::vector<uint64_t> layers(value.speciesCount, 0);
            for (uint32_t i = 0; i < value.speciesCount; ++i) {
                if (value.species[i] == s01)
                    layers[i] = value.amounts[i] / 4 * grainNumerator +
                                (grainNumerator == 4 ? value.amounts[i] % 4 : 0);
            }

            LayeredWorld world = MakeLayeredRoot(
                table, false, [&](uint32_t) { return value; }, [&](const RxWideCell&, uint32_t) { return layers; });
            EnableCellLayers(world.nest, table);
            return FoldsAfterQuiet(world.nest, table, world.root);
        };

        Expect(foldsWith(4), "層を使う世界: 粒が全部層なら畳む(ほかの物質の残りは数えない)");
        Expect(!foldsWith(2), "層を使う世界: 舞う粒の残る頁は畳まない");
        Expect(!foldsWith(0), "層を使う世界: 層の無い粒は舞う粒(畳まない)");
    }

    // --- 費用(表示だけ)---

    void MeasureCost(const BakedReactionTable& table) {
        const RxCell sand = MakeCell(table, {{"cellulose", 38600000}, {"oxygen", 983000}});
        MultiresNest nest = MakePagedRoot(table, sand);
        const uint32_t page = nest.blocks[0].page;
        nest_detail::NarrowPageCells(nest, page);
        const uint64_t plainBytes = MeasureCellPageBytes(nest.cells.pages[page]);
        LayerWholeBlock(nest, 0, 2);
        const uint64_t layeredBytes = MeasureCellPageBytes(nest.cells.pages[page]);
        Expect(layeredBytes == plainBytes + (uint64_t{MR_BLOCK_CELLS} * 2 * sizeof(uint64_t)),
               "費用: 層は幅 2 の頁で 1 セル 16 B");
        Log(Channel::Sim, Level::Info, "費用: 幅 {} の頁の 1 セル 層なし {} B → 層あり {} B(保存点も同じだけ増える)",
            nest.cells.pages[page].width, plainBytes / MR_BLOCK_CELLS, layeredBytes / MR_BLOCK_CELLS);

        // --- 書く道の時間(量だけを書く。層の無い頁 / 層のある頁)---
        constexpr uint32_t ROUNDS = 400;
        const auto timeWrites = [&](MultiresNest& target) {
            const auto start = std::chrono::steady_clock::now();
            for (uint32_t round = 0; round < ROUNDS; ++round) {
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    RxCell cell = LoadNestCell(target, 0, index);
                    cell.amounts[0] -= round & 1u;
                    StoreNestCell(target, 0, index, cell);
                }
            }

            const auto elapsed = std::chrono::steady_clock::now() - start;
            return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / (ROUNDS * MR_BLOCK_CELLS);
        };
        MultiresNest plain = MakePagedRoot(table, sand);
        nest_detail::NarrowPageCells(plain, plain.blocks[0].page);
        const auto plainNs = timeWrites(plain);
        const auto layeredNs = timeWrites(nest);
        Log(Channel::Sim, Level::Info, "費用: 1 セルの読み書き 層なし {} ns・層あり {} ns(幅 2・CPU)", plainNs,
            layeredNs);
        Expect(CountLayerMisfits(nest) == 0, "費用: 書いた後も 0 ≤ 層 ≤ 量");
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "試験の表をベイクできない: {}", table.error());
            return 1;
        }

        TestRebind(*table);
        TestStepKeepsCells(*table);
        TestHashAndSave(*table);
        TestReleasedPagesForgetLayers(*table);
        TestLayeredPageStaysAwake(*table);
        const BakedReactionTable inert = MakeInertTable();
        TestRefineCoarsen(inert, false);
        TestRefineCoarsen(inert, true);
        TestUniformLayers(inert, false);
        TestUniformLayers(inert, true);
        TestOverflowRebind(inert);
        TestFloatingGrainStaysAwake();
        MeasureCost(*table);
        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_layers_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_layers_test: OK");
        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
