// multires_layers_test.cpp — セルの成分ごとの層の量(T-1210。ADR-0481 の 5・20 §10.5)の CPU リファレンスの試験(1 段目: 頁のセル)。
//   - 付け替え: 成分の量だけを書く道は、層を物質で結んで 層 = min(前の層, 新しい量) にする(増えた分は残りへ・無くなった物質の層は 0・
//     新しい物質の層は 0・並びの位置がずれても物質で結ぶ・頁の幅が広がる / 詰まる時も層を保つ)
//   - 刻み: 層のある世界と無い世界で、セルの値(成分の量・エネルギー)は毎刻みビット一致(物質は 1 つのまま。反応は量の全部に当たる)。
//     層は毎刻み 0 ≤ 層 ≤ 量 で、どの物質の層も増えない(層を書く法則がまだ無いので)
//   - 要約と保存点: 層は状態の要約(HashWholeNest・HashRealLeaves)に入り、保存点から戻る。層の無い世界の要約は層を足して消した後と同じ
//   - 頁を返す: 細かくした子の頁に層を書いて粗くして返し、また細かくしても古い層が残らない
//   - 眠る: 層のある頁は畳まない(1 段目)。層の無い同じ頁は畳む
//   - 費用: 層のある頁の 1 セルのバイトと、書く道の時間(表示だけ。docs/perf.md 用)
// 表は試験のパッケージの表(tests/reaction_test_table.h)。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "common/multires_activity.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "multires_test_scene.h"
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

            const std::vector<std::vector<LayerEntry>> after = RootLayers(layered);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                for (const LayerEntry& entry : after[index]) {
                    const uint64_t previous = PreviousLayer(before[index], entry.species);
                    grown += entry.layer > previous ? 1u : 0u;
                    shrunk += entry.layer < previous ? 1u : 0u;
                }
            }

            before = after;
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
