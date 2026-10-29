// field_owner_test.cpp — 欄の書き手と寿命(T-1252 段 1。ADR-0500・D-683・20 §10.7)の CPU のテスト。確かめること:
//   1. 登録: 配り手(SF_SCALE_OWNER + 配り手の番号)・この刻みだけ(SF_FLAG_TICK_ONLY = SF_KIND_TICK)・値の数の上限 12・
//      保存量 ⇒ VOLUME か配り手・VOLUME ⇒ 保存量。この刻みだけはハッシュ・保存点に入らない
//   2. 配り手の欄の往復: 頁を持つ親を細かくすると、一般の段は写さず配り手の関数で配る(子の和 = 親 × 8)。粗くすると親の値に戻る。
//      保存量の検査(ComputeConservedTotals)が細かく/粗くの前後で同じ。一様な親は子も一様・粗くしても一様のまま。
//      観察の影も配り手の関数で細かくし、引き戻した影を粗くすると親の値(ADR-0361 と揃う)
//   3. この刻みだけの欄: 書いても状態の要約は変わらない・当てる段の読む口で 1 回読んで 0(刻みの境目の検査)
//   4. 集めて書く段: 足す順を入れ替えても欄の頁の像(頁の表まで)が同じ・一様な枠は広げてから書く・
//      頁の予算が尽きると書かずに作業に残す(量を捨てない)
#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "multires_activity_scene.h"
#include "reaction_test_table.h"
#include "sim/multires_fields.h"
#include "sim/multires_nest.h"
#include "sim/multires_state.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::sim;
using namespace bicameral::state_field;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    constexpr uint32_t CONSERVED = SF_FLAG_CONSERVED | SF_FLAG_HASHED;
    constexpr uint32_t OWNER_COMPONENTS = SF_MAX_COMPONENTS;  // 放射の持ち越しと同じ 12 値

    // --- 1. 登録 ---

    void TestRegistry() {
        const SfFieldDesc owned = SfMakeOwnerDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, CONSERVED, 12,
                                                  SF_OWNER_TEST);
        const SfFieldDesc ownedPlain = SfMakeOwnerDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_FLAG_HASHED, 1,
                                                       SF_OWNER_TEST);
        const SfFieldDesc tick = SfMakeTickDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, 1);
        Expect(SfDescValid(owned) && SfDescValid(ownedPlain) && SfDescValid(tick),
               "登録: 配り手(保存量・保存量でない)・この刻みだけを登録できる");

        // --- 保存量 ⇒ VOLUME か配り手・VOLUME ⇒ 保存量 ---
        const SfFieldDesc sameConserved = SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_SAME,
                                                          CONSERVED, 1);
        const SfFieldDesc volumePlain = SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_VOLUME,
                                                        SF_FLAG_HASHED, 1);
        Expect(!SfDescValid(sameConserved) && !SfDescValid(volumePlain),
               "登録: 保存量の SAME・保存量でない VOLUME は断る");

        // --- 配り手の番号 ---
        SfFieldDesc noOwner = owned;
        noOwner.owner = SF_NO_OWNER;
        SfFieldDesc badOwner = owned;
        badOwner.owner = SF_OWNER_COUNT;
        SfFieldDesc strayOwner = sameConserved;
        strayOwner.scale = SF_SCALE_VOLUME;
        strayOwner.owner = SF_OWNER_TEST;
        const SfFieldDesc blockOwner = SfMakeOwnerDesc(SF_PLACE_BLOCK, SF_TYPE_I64, SF_PHASE_NONE, SF_FLAG_HASHED, 1,
                                                       SF_OWNER_TEST);
        const SfFieldDesc placedOwner = SfMakeOwnerDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE,
                                                        CONSERVED | SF_FLAG_PLACE_OPEN, 1, SF_OWNER_TEST);
        Expect(!SfDescValid(noOwner) && !SfDescValid(badOwner) && !SfDescValid(strayOwner) &&
                   !SfDescValid(blockOwner) && !SfDescValid(placedOwner),
               "登録: 配り手は番号を持つセル・面の真実の欄だけ(置かれ方の印を持たない)");

        // --- この刻みだけ: 状態に入らない・真実や導かれる値の印と一緒にしない ---
        SfFieldDesc tickHashed = tick;
        tickHashed.flags |= SF_FLAG_HASHED;
        SfFieldDesc tickDerived = tick;
        tickDerived.flags |= SF_FLAG_DERIVED;
        const SfFieldDesc tickBlock = SfMakeTickDesc(SF_PLACE_BLOCK, SF_TYPE_I64, SF_PHASE_NONE, 1);
        Expect(!SfDescValid(tickHashed) && !SfDescValid(tickDerived) && !SfDescValid(tickBlock),
               "登録: この刻みだけは HASHED・導かれる値の印を持たないセル・面の欄");
        Expect(SfFieldKind(tick) == SF_KIND_TICK && !SfHashed(tick) && !SfSaved(tick) && !SfTruthHashed(tick) &&
                   !SfConserved(tick),
               "登録: この刻みだけはハッシュ・保存点・保存量に入らない");

        // --- 値の数の上限 12 ---
        SfFieldDesc tooMany = owned;
        tooMany.components = SF_MAX_COMPONENTS + 1;
        Expect(SF_MAX_COMPONENTS == 12 && !SfDescValid(tooMany), "登録: 値の数は 12 まで");

        // --- 配り手の関数: 子の和 = 親 × 8・粗くすると親に戻る ---
        bool keeps = true;
        for (const int64_t parent : {int64_t{0}, int64_t{1}, int64_t{-7}, int64_t{12345}, int64_t{-987654321}}) {
            SfFamily family{};
            int64_t sum = 0;
            for (uint32_t j = 0; j < SF_CHILDREN; ++j) {
                family.values[j] = SfOwnerRefineChild(SF_OWNER_TEST, parent, j);
                sum += family.values[j];
            }

            const SfCoarsenSum coarse = SfOwnerCoarsen(SF_OWNER_TEST, family);
            keeps = keeps && sum == parent * 8 && SfCoarsenValue(coarse) == parent && SfCoarsenRemainder(coarse) == 0;
        }

        Expect(keeps, "配り手の関数: 子の和 = 親 × 8・粗くすると親・余り 0");
    }

    // --- 2. 配り手の欄の往復 ---

    struct OwnerWorld {
        MultiresNest nest;
        uint32_t owned = MULTIRES_NO_FIELD;
        uint32_t plain = MULTIRES_NO_FIELD;
        uint32_t root = MR_NO_BLOCK;
    };

    int64_t OwnerValue(uint32_t index, uint32_t c) {
        return (int64_t)(index * 37) - (int64_t)(c * 1001) + 5;
    }

    OwnerWorld MakeOwnerWorld(const BakedReactionTable& table, bool paged) {
        OwnerWorld world{.nest = MakeMultiresNest(test::MakeMultiresCapacity(table, 16, 4, 16))};
        MultiresNest& nest = world.nest;
        world.owned = RegisterNestField(
            nest, "owned",
            SfMakeOwnerDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, CONSERVED, OWNER_COMPONENTS, SF_OWNER_TEST));
        world.plain = RegisterNestField(
            nest, "plain", SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_VOLUME, CONSERVED, 1));
        Expect(world.owned != MULTIRES_NO_FIELD && world.plain != MULTIRES_NO_FIELD, "往復: 欄を登録できる");

        const std::vector<RxCell> air(MR_BLOCK_CELLS, test::MakeAirCell(table));
        world.root = PlaceRootBlock(nest, 0, 0, 0, air);
        if (!paged) {
            for (uint32_t c = 0; c < OWNER_COMPONENTS; ++c)
                StoreNestFieldUniform(nest, world.owned, world.root, c, OwnerValue(0, c));
            return world;
        }

        const bool expanded = ExpandNestPage(nest, world.root);
        Expect(expanded, "往復: 根を頁に広げられる");
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            StoreNestField(nest, world.plain, world.root, index, 0, (int64_t)index * 3);
            for (uint32_t c = 0; c < OWNER_COMPONENTS; ++c)
                StoreNestField(nest, world.owned, world.root, index, c, OwnerValue(index, c));
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

    void TestOwnerRoundTrip(const BakedReactionTable& table) {
        OwnerWorld world = MakeOwnerWorld(table, true);
        MultiresNest& nest = world.nest;
        const ConservedTotals before = ComputeConservedTotals(nest, table, 2);

        // --- 細かくする: 配り手の欄は配り手の関数で・一般の欄は子は親と同じ数 ---
        Request(nest, MR_REQUEST_REFINE);
        const uint32_t child = ChildOfRoot(nest);
        Expect(child != MR_NO_BLOCK && !MrIsUniform(nest.blocks[child]), "往復: 頁を持つ子ができる");
        if (child == MR_NO_BLOCK)
            return;

        const uint32_t octant = nest.blocks[child].parentOctant;
        uint32_t wrongOwned = 0;
        uint32_t wrongPlain = 0;
        uint32_t uncovered = 0;
        for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
            const uint32_t parentCell = MrOctantCell(octant, local);
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                const uint32_t index = MrChildCell(local, j);
                wrongPlain += LoadNestField(nest, world.plain, child, index, 0) == (int64_t)parentCell * 3 ? 0u : 1u;
                for (uint32_t c = 0; c < OWNER_COMPONENTS; ++c) {
                    const int64_t want = SfOwnerRefineChild(SF_OWNER_TEST, OwnerValue(parentCell, c), j);
                    wrongOwned += LoadNestField(nest, world.owned, child, index, c) == want ? 0u : 1u;
                }
            }

            for (uint32_t c = 0; c < OWNER_COMPONENTS; ++c)
                uncovered += LoadNestField(nest, world.owned, world.root, parentCell, c) == 0 ? 0u : 1u;
        }

        Expect(wrongOwned == 0, "往復: 配り手の欄の子は配り手の関数の値(一般の段は写さない)");
        Expect(wrongPlain == 0, "往復: 一般の欄の子は親と同じ数(今のまま)");
        Expect(uncovered == 0, "往復: 覆われた親のセルの配り手の欄は 0");
        Expect(ComputeConservedTotals(nest, table, 2) == before, "往復: 細かくしても保存量(配り手の欄の列も)が同じ");

        // --- 粗くする: 親の値に戻る・保存量が同じ・状態の要約が最初と同じ ---
        Request(nest, MR_REQUEST_COARSEN);
        Expect(ChildOfRoot(nest) == MR_NO_BLOCK, "往復: 粗くすると子が無い");
        uint32_t wrongBack = 0;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            for (uint32_t c = 0; c < OWNER_COMPONENTS; ++c)
                wrongBack += LoadNestField(nest, world.owned, world.root, index, c) == OwnerValue(index, c) ? 0u : 1u;
        }

        Expect(wrongBack == 0, "往復: 粗くすると配り手の欄は親の値に戻る");
        Expect(ComputeConservedTotals(nest, table, 2) == before, "往復: 粗くしても保存量が同じ");

        // --- 一様な親: 子も一様・粗くしても一様のまま ---
        OwnerWorld uniform = MakeOwnerWorld(table, false);
        Request(uniform.nest, MR_REQUEST_REFINE);
        const uint32_t uniformChild = ChildOfRoot(uniform.nest);
        Expect(uniformChild != MR_NO_BLOCK && MrIsUniform(uniform.nest.blocks[uniformChild]) &&
                   LoadNestField(uniform.nest, uniform.owned, uniformChild, 9, 4) == OwnerValue(0, 4),
               "一様: 一様な親の子は一様で、配り手の一様の配り(親と同じ数)");
        Request(uniform.nest, MR_REQUEST_COARSEN);
        Expect(ChildOfRoot(uniform.nest) == MR_NO_BLOCK && MrIsUniform(uniform.nest.blocks[uniform.root]) &&
                   LoadNestField(uniform.nest, uniform.owned, uniform.root, 9, 4) == OwnerValue(0, 4),
               "一様: 粗くしても一様な親のまま");
    }

    // 影: 配り手の関数で細かくし、引き戻した影を配り手の関数で粗くすると親の値・余り 0(ADR-0361 と揃う)
    void TestOwnerShadow(const BakedReactionTable& table) {
        OwnerWorld world = MakeOwnerWorld(table, true);
        MultiresNest& nest = world.nest;
        const uint32_t shadow = nest.capacity.worldBlocks;
        const MultiresPoint point{.x = 1, .y = 1, .z = 1, .level = 1};
        RefineShadowChain(nest, world.root, shadow, 1, point);
        Expect(nest.blocks[shadow].kind == MR_BLOCK_SHADOW, "影: 影ができる");
        if (nest.blocks[shadow].kind != MR_BLOCK_SHADOW)
            return;

        const uint32_t octant = nest.blocks[shadow].parentOctant;
        uint32_t wrongRefine = 0;
        for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
            const uint32_t parentCell = MrOctantCell(octant, local);
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                const int64_t want = SfOwnerRefineChild(SF_OWNER_TEST, OwnerValue(parentCell, 2), j);
                wrongRefine += LoadNestField(nest, world.owned, shadow, MrChildCell(local, j), 2) == want ? 0u : 1u;
            }
        }

        Expect(wrongRefine == 0, "影: 配り手の欄は配り手の関数で細かくする");

        // 影の中を動かしてから引き戻す
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; index += 3)
            StoreNestField(nest, world.owned, shadow, index, 2, (int64_t)index * 11 - 40);

        PullBackShadowChain(nest, table, shadow, 1);
        uint32_t wrongPull = 0;
        for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
            const uint32_t parentCell = MrOctantCell(octant, local);
            SfFamily family{};
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                family.values[j] = LoadNestField(nest, world.owned, shadow, MrChildCell(local, j), 2);

            const SfCoarsenSum sum = SfOwnerCoarsen(SF_OWNER_TEST, family);
            wrongPull += SfCoarsenValue(sum) == OwnerValue(parentCell, 2) && SfCoarsenRemainder(sum) == 0 ? 0u : 1u;
        }

        Expect(wrongPull == 0, "影: 引き戻した影を配り手の関数で粗くすると親の値・余り 0");
    }

    // --- 3. この刻みだけの欄 ---

    void TestTickOnly(const BakedReactionTable& table) {
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(table, 16, 0, 16));
        const uint32_t tick = RegisterNestField(nest, "source",
                                                SfMakeTickDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, 2));
        const std::vector<RxCell> air(MR_BLOCK_CELLS, test::MakeAirCell(table));
        const uint32_t root = PlaceRootBlock(nest, 0, 0, 0, air);
        const bool expanded = ExpandNestPage(nest, root);
        Expect(tick != MULTIRES_NO_FIELD && expanded, "刻みだけ: 登録して頁に広げる");

        const uint64_t hash = HashWholeNest(nest);
        const MultiresSaveState save = SaveNestState(nest);
        StoreNestField(nest, tick, root, 5, 1, 77);
        StoreNestField(nest, tick, root, 400, 0, -3);
        Expect(HashWholeNest(nest) == hash && !TickOnlyFieldsClear(nest),
               "刻みだけ: 書いても状態の要約は変わらない(境目の検査は 0 でないと言う)");

        int64_t sum = 0;
        uint32_t calls = 0;
        bool sameSlot = true;
        ConsumeTickOnlyField(nest, tick, [&](uint32_t slot, uint32_t index, uint32_t component, int64_t value) {
            sum += value * (int64_t)(index + component + 1);
            sameSlot = sameSlot && slot == root;
            ++calls;
        });
        Expect(calls == 2 && sameSlot && sum == (77 * 7) + (-3 * 401),
               "刻みだけ: 当てる段の読む口が 0 でない値を 1 回ずつ渡す");
        Expect(TickOnlyFieldsClear(nest) && UsedNestFieldPages(nest, tick) == 0,
               "刻みだけ: 読んだ後は 0・頁を返す(刻みの境目)");

        StoreNestField(nest, tick, root, 9, 0, 5);
        Expect(RestoreNestState(nest, save) && TickOnlyFieldsClear(nest), "刻みだけ: 保存点に入らない(戻すと 0)");
    }

    // --- 4. 集めて書く段 ---

    struct GatherWorld {
        MultiresNest nest;
        uint32_t field = MULTIRES_NO_FIELD;
        std::vector<uint32_t> roots;
    };

    GatherWorld MakeGatherWorld(const BakedReactionTable& table) {
        GatherWorld world{.nest = MakeMultiresNest(test::MakeMultiresCapacity(table, 16, 0, 16))};
        world.field = RegisterNestField(
            world.nest, "carry",
            SfMakeOwnerDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, CONSERVED, OWNER_COMPONENTS, SF_OWNER_TEST));
        const std::vector<RxCell> air(MR_BLOCK_CELLS, test::MakeAirCell(table));
        for (int64_t x = 0; x < 4; ++x)
            world.roots.push_back(PlaceRootBlock(world.nest, x * 8, 0, 0, air));

        return world;
    }

    struct GatherAdd {
        uint32_t root;  // world.roots の添字
        uint32_t index;
        uint32_t component;
        int64_t value;
    };

    const std::vector<GatherAdd>& GatherAdds() {
        static const std::vector<GatherAdd> adds = {{3, 10, 0, 5},    {1, 2, 11, -9}, {3, 10, 0, 7},
                                                    {0, 511, 4, 100}, {1, 300, 6, 1}, {3, 0, 1, -2}};
        return adds;
    }

    NestGatherReport Gather(GatherWorld& world, bool reversed, NestGatherWork* kept = nullptr) {
        NestGatherWork work = MakeNestGatherWork(world.nest, world.field);
        const std::vector<GatherAdd>& adds = GatherAdds();
        for (size_t i = 0; i < adds.size(); ++i) {
            const GatherAdd& add = adds[reversed ? adds.size() - 1 - i : i];
            AddNestGatherWork(work, world.roots[add.root], add.index, add.component, add.value);
        }

        const NestGatherReport report = GatherNestField(world.nest, work);
        if (kept != nullptr)
            *kept = work;

        return report;
    }

    int64_t FieldTotal(const MultiresNest& nest, uint32_t field, uint32_t component) {
        int64_t total = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            if (nest.blocks[slot].kind != MR_BLOCK_REAL)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                total += LoadNestField(nest, field, slot, index, component);
        }

        return total;
    }

    void TestGather(const BakedReactionTable& table) {
        GatherWorld forward = MakeGatherWorld(table);
        GatherWorld backward = MakeGatherWorld(table);
        const NestGatherReport a = Gather(forward, false);
        const NestGatherReport b = Gather(backward, true);
        const MultiresFieldStore& fa = forward.nest.fields[forward.field].store;
        const MultiresFieldStore& fb = backward.nest.fields[backward.field].store;
        Expect(a.blocks == 3 && a.pagesTaken == 3 && a.expanded == 3 && a.pageShortage == 0,
               "集める: 触った一様な枠 3 つを広げ、欄の頁を 3 つ取る");
        Expect(fa.pageOf == fb.pageOf && fa.pages == fb.pages && b.pagesTaken == a.pagesTaken &&
                   HashWholeNest(forward.nest) == HashWholeNest(backward.nest),
               "集める: 足す順を入れ替えても頁の番号と値(状態の要約)が同じ");
        std::vector<uint32_t> touched = {forward.roots[0], forward.roots[1], forward.roots[3]};
        std::ranges::sort(touched);
        bool slotOrder = true;
        for (uint32_t k = 0; k < touched.size(); ++k)
            slotOrder = slotOrder && fa.pageOf[forward.nest.blocks[touched[k]].page] == k;

        Expect(slotOrder, "集める: 欄の頁は枠の順(プレフィックス和の順)");
        Expect(MrIsUniform(forward.nest.blocks[forward.roots[2]]), "集める: 触らない枠は一様のまま");
        Expect(LoadNestField(forward.nest, forward.field, forward.roots[3], 10, 0) == 12 &&
                   LoadNestField(forward.nest, forward.field, forward.roots[1], 2, 11) == -9,
               "集める: 同じセルへの足し込みは和");

        // --- 頁の予算が尽きる: 書かずに作業に残す(量を捨てない)---
        GatherWorld tight = MakeGatherWorld(table);
        SetNestFieldPageBudget(tight.nest, tight.field, 1);
        NestGatherWork rest;
        const NestGatherReport c = Gather(tight, false, &rest);
        Expect(c.blocks == 1 && c.pageShortage == 2, "予算: 頁が 1 つなら 1 枠だけ書き、2 枠は計器の誤り");
        bool kept = true;
        for (uint32_t comp = 0; comp < OWNER_COMPONENTS; ++comp) {
            int64_t added = 0;
            for (const GatherAdd& add : GatherAdds())
                added += add.component == comp ? add.value : 0;

            kept = kept && FieldTotal(tight.nest, tight.field, comp) + NestGatherWorkTotal(rest, comp) == added;
        }

        Expect(kept, "予算: 欄の値 + 作業に残った値 = 足した値(量を捨てない)");
    }

    int Run() {
        TestRegistry();

        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "試験の表をベイクできない: {}", table.error());
            return 1;
        }

        TestOwnerRoundTrip(*table);
        TestOwnerShadow(*table);
        TestTickOnly(*table);
        TestGather(*table);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "field_owner_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "field_owner_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
