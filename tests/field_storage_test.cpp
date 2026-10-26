// field_storage_test.cpp — 欄の持ち方(T-0854。試作 C-1 の 1 チャット目。20 §10・§13.2・ADR-0373・ADR-0399)の CPU のテスト。確かめること:
//   1. 登録の種類: 真実 / 導かれる値 / 温めた始まり(state_field.hlsli の SF_FLAG_DERIVED・SF_FLAG_WARM_START)。導かれる値は
//      セル・面・SAME・保存量でないものだけ登録できる。ハッシュ・保存点に入るかは種類から決まる(導かれる値は入らない・
//      温めた始まりは D-549 の仮 (A) で入る)。眠る時の平均(SfMeanValue)は −∞ 向きの切り捨てで、64bit の端でも溢れない
//   2. 眠らせて起こす往復: 一様なブロックの場面(燃え尽きる木箱 + 空気の根の鎖)に真実の欄と導かれる値・温めた始まりの欄を持たせ、
//      静かな頁を畳む(眠る)世界と畳まない(起きたまま)世界を並べて刻む。導かれる値がセルごとに違っても眠れる。
//      眠る時に導かれる値は覆われていないセルの平均を残し(D-542 (b))、起こすと子は親と同じ数で配る。
//      真実の要約(HashTruthLeaves)と保存量(Σ − 帳簿)が 2 つの世界で毎刻みビット一致・最初と同じ(帳簿が合う)
//   3. 眠っている途中で保存点を取り、戻して同じ刻みを進めると、続けた場合と毎刻み同じ状態の要約(HashWholeNest。温めた始まりも入る)
// 導かれる値の「解き方」は試験用の作り(真実だけから決まる値 + 前の答えから半分ずつ寄る温めた始まり)。本物の解き方は C-0(T-0853)。
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "multires_uniform_scene.h"
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

    // --- 1. 登録の種類 ---

    void TestKinds() {
        const SfFieldDesc truth = SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_VOLUME,
                                                  SF_FLAG_CONSERVED | SF_FLAG_HASHED, 1);
        const SfFieldDesc derived = SfMakeDerivedDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, false, 1);
        const SfFieldDesc warm = SfMakeDerivedDesc(SF_PLACE_FACE, SF_TYPE_I64, SF_PHASE_NONE, true, 3);
        Expect(SfDescValid(truth) && SfDescValid(derived) && SfDescValid(warm), "種類: 3 つとも登録できる");
        Expect(SfFieldKind(truth) == SF_KIND_TRUTH && SfFieldKind(derived) == SF_KIND_DERIVED &&
                   SfFieldKind(warm) == SF_KIND_WARM_START,
               "種類: 印から種類が決まる");
        Expect(SfHashed(truth) && SfSaved(truth) && !SfHashed(derived) && !SfSaved(derived),
               "種類: 真実はハッシュと保存点に入り、導かれる値は入らない");
        Expect(
            SfHashed(warm) == SF_WARM_START_IN_STATE && SfSaved(warm) == SF_WARM_START_IN_STATE && !SfTruthHashed(warm),
            "種類: 温めた始まりは D-549 の印どおり(真実の要約には入らない)");

        // --- 登録できない形 ---
        const SfFieldDesc derivedVolume = SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_VOLUME,
                                                          SF_FLAG_DERIVED | SF_FLAG_CONSERVED, 1);
        const SfFieldDesc derivedHashed = SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_SAME,
                                                          SF_FLAG_DERIVED | SF_FLAG_HASHED, 1);
        const SfFieldDesc derivedBlock = SfMakeDerivedDesc(SF_PLACE_BLOCK, SF_TYPE_I64, SF_PHASE_NONE, false, 1);
        const SfFieldDesc warmAlone = SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_SAME,
                                                      SF_FLAG_WARM_START, 1);
        Expect(!SfDescValid(derivedVolume) && !SfDescValid(derivedHashed) && !SfDescValid(derivedBlock) &&
                   !SfDescValid(warmAlone),
               "種類: 量・保存量・HASHED の導かれる値、ブロックの導かれる値、導かれる値でない温めた始まりは登録しない");

        // --- 眠る時に捨てる(T-1159): 導かれる値だけ。捨てる欄は 0・ほかは平均 ---
        SfFieldDesc discard = warm;
        discard.flags |= SF_FLAG_SLEEP_DISCARD;
        SfFieldDesc truthDiscard = truth;
        truthDiscard.flags |= SF_FLAG_SLEEP_DISCARD;
        Expect(SfDescValid(discard) && !SfDescValid(truthDiscard) && SfSleepValueOf(discard, 12) == 0 &&
                   SfSleepValueOf(warm, 12) == SfSleepValue(12),
               "種類: 眠る時に捨てる印は導かれる値だけ・捨てる欄は 0 を残す");
    }

    int64_t MeanOf(std::initializer_list<int64_t> values) {
        SfMeanSum sum = SfMakeMeanSum();
        for (const int64_t value : values)
            sum = SfAddMeanValue(sum, value);

        return SfMeanValue(sum);
    }

    void TestMean() {
        Expect(MeanOf({}) == 0 && MeanOf({7}) == 7 && MeanOf({1, 2}) == 1 && MeanOf({-1, -2}) == -2,
               "平均: −∞ 向きの切り捨て");
        Expect(MeanOf({INT64_MAX, INT64_MIN}) == -1, "平均: 端の値の和 −1 の半分は −1");

        SfMeanSum high = SfMakeMeanSum();
        SfMeanSum low = SfMakeMeanSum();
        for (uint32_t i = 0; i < MR_BLOCK_CELLS; ++i) {
            high = SfAddMeanValue(high, INT64_MAX);
            low = SfAddMeanValue(low, INT64_MIN);
        }

        Expect(SfMeanValue(high) == INT64_MAX && SfMeanValue(low) == INT64_MIN, "平均: 512 個の端の値でも溢れない");
    }

    // --- 2・3. 眠らせて起こす往復 ---

    struct Fields {
        uint32_t amount = MULTIRES_NO_FIELD;    // 真実・セル・量(保存量)
        uint32_t momentum = MULTIRES_NO_FIELD;  // 真実・面 ×3・量(保存量)
        uint32_t pressure = MULTIRES_NO_FIELD;  // 導かれる値・セル
        uint32_t force = MULTIRES_NO_FIELD;     // 導かれる値・面 ×3
        uint32_t warm = MULTIRES_NO_FIELD;      // 温めた始まり・セル(前の刻みの答えから解き始める)
        uint32_t discard = MULTIRES_NO_FIELD;   // 温めた始まり・セル・眠る時に捨てる(T-1159。ADR-0469 の 2-3)
    };

    constexpr uint32_t CONSERVED = SF_FLAG_CONSERVED | SF_FLAG_HASHED;
    constexpr uint64_t END_TICK = 200;
    constexpr uint64_t WAKE_DELAY = 6;  // 枠 0 が眠ってから起こすまでの刻み

    struct World {
        MultiresNest nest;
        Fields fields;
        bool sleeps = true;  // false なら静かな頁を畳まない(起きたまま)
    };

    World MakeWorld(const BakedReactionTable& table, bool sleeps) {
        World world{.nest = MakeMultiresNest(
                        test::MakeMultiresCapacity(table, test::UNIFORM_WORLD_BLOCKS, 0, test::UNIFORM_FRACTIONS)),
                    .sleeps = sleeps};
        MultiresNest& nest = world.nest;
        Fields& fields = world.fields;
        fields.amount = RegisterNestField(
            nest, "truth.amount",
            SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_VOLUME, CONSERVED, 1));
        fields.momentum = RegisterNestField(
            nest, "truth.momentum",
            SfMakeFieldDesc(SF_PLACE_FACE, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_VOLUME, CONSERVED, 3));
        fields.pressure = RegisterNestField(nest, "derived.pressure",
                                            SfMakeDerivedDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, false, 1));
        fields.force = RegisterNestField(nest, "derived.force",
                                         SfMakeDerivedDesc(SF_PLACE_FACE, SF_TYPE_I64, SF_PHASE_NONE, false, 3));
        fields.warm = RegisterNestField(nest, "derived.warm",
                                        SfMakeDerivedDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, true, 1));
        SfFieldDesc discard = SfMakeDerivedDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, true, 1);
        discard.flags |= SF_FLAG_SLEEP_DISCARD;
        fields.discard = RegisterNestField(nest, "derived.discard", discard);

        // --- 根 8 つ(枠 0・1 は燃え尽きる木箱、残りは空気。真実の欄はブロックごとに一様な値)---
        const std::vector<RxCell> air(MR_BLOCK_CELLS, test::MakeAirCell(table));
        const std::vector<RxCell> crate(MR_BLOCK_CELLS, test::MakeCrateCell(table));
        for (uint32_t root = 0; root < test::UNIFORM_ROOTS; ++root) {
            const int64_t x = root % test::UNIFORM_ROOT_EDGE;
            const int64_t y = (root / test::UNIFORM_ROOT_EDGE) % test::UNIFORM_ROOT_EDGE;
            const int64_t z = root / (test::UNIFORM_ROOT_EDGE * test::UNIFORM_ROOT_EDGE);
            const uint32_t slot = PlaceRootBlock(nest, x * 8, y * 8, z * 8,
                                                 root < test::UNIFORM_CRATE_ROOTS ? crate : air);
            StoreNestFieldUniform(nest, fields.amount, slot, 0, 40 + root);
            for (uint32_t c = 0; c < 3; ++c)
                StoreNestFieldUniform(nest, fields.momentum, slot, c, static_cast<int64_t>(root * 3 + c) - 10);
        }

        return world;
    }

    // 試験用の「解く」: 起きている(頁を持つ)本物のブロックのセルごとに、導かれる値を真実(セルのエネルギー)と場所から作り、
    // 温めた始まりは前の答えから導かれる値へ半分寄る(決まった回数で解く法則の形。前の答えが次の答えを変える)。眠った所は触らない
    void SolveDerived(World& world) {
        MultiresNest& nest = world.nest;
        const Fields& fields = world.fields;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL || MrIsUniform(block))
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (MrIsCoveredCell(block, index))
                    continue;

                const int64_t pressure = (LoadNestCell(nest, slot, index).energy >> 6) +
                                         static_cast<int64_t>(index & 7u) - 3;
                StoreNestField(nest, fields.pressure, slot, index, 0, pressure);
                for (uint32_t c = 0; c < 3; ++c)
                    StoreNestField(nest, fields.force, slot, index, c, pressure - static_cast<int64_t>(c * 5));

                const int64_t previous = LoadNestField(nest, fields.warm, slot, index, 0);
                StoreNestField(nest, fields.warm, slot, index, 0, (previous + pressure) >> 1);
                StoreNestField(nest, fields.discard, slot, index, 0, pressure + 7);
            }
        }
    }

    // 枠 slot(頁を持つ)の欄 field の値 0 の、覆われていないセルの平均(眠る時に残るはずの値)
    int64_t PageMean(const MultiresNest& nest, uint32_t field, uint32_t slot) {
        SfMeanSum sum = SfMakeMeanSum();
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            if (!MrIsCoveredCell(nest.blocks[slot], index))
                sum = SfAddMeanValue(sum, LoadNestField(nest, field, slot, index, 0));
        }

        return SfMeanValue(sum);
    }

    // 眠った枠 0 を起こす(段の外からセルに書く。エネルギーをセル 0 から 1 へ移す = 保存量は変わらない)
    void Wake(World& world) {
        MultiresNest& nest = world.nest;
        const bool expanded = ExpandNestPage(nest, 0);
        Expect(expanded, "起こす: 頁に広げられる");
        if (!expanded)
            return;

        RxCell from = LoadNestCell(nest, 0, 0);
        RxCell to = LoadNestCell(nest, 0, 1);
        from.energy -= 1000;
        to.energy += 1000;
        StoreNestCell(nest, 0, 0, from);
        StoreNestCell(nest, 0, 1, to);
        PokeNestBlock(nest, 0);
    }

    struct SleepRecord {
        int checkedSleeps = 0;     // 眠った時に平均が残ったのを確かめた数
        int wrongSleeps = 0;       // 平均と違った数
        int wrongDiscards = 0;     // 眠る時に捨てる欄が 0 でなかった数
        bool checkedWake = false;  // 起こした時に眠った値が配られたのを確かめた
        bool wrongWake = false;
    };

    // 1 刻み(前半 → 起こす → 後半 → 解く)。眠る世界は静かな頁を畳む
    void StepWorld(World& world, const BakedReactionTable& table, uint64_t tick, uint64_t wakeTick,
                   SleepRecord* record) {
        MultiresNest& nest = world.nest;

        // --- 眠る前の平均を控える(畳まれたら一様の値と比べる)---
        std::vector<int64_t> means(test::UNIFORM_CRATE_ROOTS, 0);
        std::vector<bool> paged(test::UNIFORM_CRATE_ROOTS, false);
        for (uint32_t slot = 0; slot < test::UNIFORM_CRATE_ROOTS; ++slot) {
            paged[slot] = !MrIsUniform(nest.blocks[slot]);
            if (paged[slot])
                means[slot] = PageMean(nest, world.fields.warm, slot);
        }

        if (world.sleeps)
            test::BeginUniformTick(nest, tick);
        else {
            SubmitRequests(nest, test::UniformRequestsAt(tick));
            SubmitQuietCoarsenRequests(nest, tick);
            ProcessRequests(nest);
        }

        for (uint32_t slot = 0; record != nullptr && slot < test::UNIFORM_CRATE_ROOTS; ++slot) {
            if (!paged[slot] || !MrIsUniform(nest.blocks[slot]))
                continue;

            record->checkedSleeps += 1;
            const int64_t kept = LoadNestField(nest, world.fields.warm, slot, 0, 0);
            record->wrongSleeps += kept == SfSleepValue(means[slot]) ? 0 : 1;
            record->wrongDiscards += LoadNestField(nest, world.fields.discard, slot, 0, 0) == 0 ? 0 : 1;
        }

        // --- 起こす(起こした直後の頁は、眠った値が子は親と同じ数で配られている)---
        if (tick == wakeTick) {
            const bool asleep = MrIsUniform(nest.blocks[0]);
            const int64_t kept = asleep ? LoadNestField(nest, world.fields.warm, 0, 0, 0) : 0;
            Wake(world);
            if (record != nullptr && asleep) {
                record->checkedWake = true;
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                    record->wrongWake = record->wrongWake ||
                                        LoadNestField(nest, world.fields.warm, 0, index, 0) != kept;
            }
        }

        StepActive(nest, table, test::STRESS_SEED, tick);
        SolveDerived(world);
    }

    // 眠る世界で枠 0 が眠った最初の刻み + WAKE_DELAY(眠らなければ END_TICK)
    uint64_t FindWakeTick(const BakedReactionTable& table) {
        World world = MakeWorld(table, true);
        for (uint64_t tick = 0; tick < END_TICK; ++tick) {
            StepWorld(world, table, tick, END_TICK, nullptr);
            if (MrIsUniform(world.nest.blocks[0]) && world.nest.counters[MR_COUNTER_FOLDED] > 0)
                return tick + WAKE_DELAY;
        }

        return END_TICK;
    }

    void TestSleepWake(const BakedReactionTable& table) {
        const uint64_t wakeTick = FindWakeTick(table);
        Expect(wakeTick < END_TICK, "往復: 導かれる値がセルごとに違っても燃え尽きた木箱は眠る");
        if (wakeTick >= END_TICK)
            return;

        World sleeping = MakeWorld(table, true);
        World awake = MakeWorld(table, false);
        constexpr int32_t FINEST = test::UNIFORM_CHAIN_DEPTH;
        const ConservedTotals initial = ComputeConservedTotals(sleeping.nest, table, FINEST);
        SleepRecord record;
        int truthMismatches = 0;
        int unbalanced = 0;
        const uint64_t saveTick = wakeTick - 2;  // 枠 0 が眠っている途中
        MultiresSaveState save;
        std::vector<uint64_t> wholeHashes;
        for (uint64_t tick = 0; tick < END_TICK; ++tick) {
            if (tick == saveTick)
                save = SaveNestState(sleeping.nest);

            StepWorld(sleeping, table, tick, wakeTick, &record);
            StepWorld(awake, table, tick, wakeTick, nullptr);
            truthMismatches += HashTruthLeaves(sleeping.nest) == HashTruthLeaves(awake.nest) ? 0 : 1;
            unbalanced += ComputeConservedTotals(sleeping.nest, table, FINEST) == initial ? 0 : 1;
            unbalanced += ComputeConservedTotals(awake.nest, table, FINEST) == initial ? 0 : 1;
            if (tick >= saveTick)
                wholeHashes.push_back(HashWholeNest(sleeping.nest));
        }

        Log(Channel::Sim, Level::Info, "往復: 起こした刻み {}・眠った頁 {}・平均を確かめた {} 回", wakeTick,
            sleeping.nest.counters[MR_COUNTER_FOLDED], record.checkedSleeps);
        Expect(sleeping.nest.counters[MR_COUNTER_FOLDED] > 0 && awake.nest.counters[MR_COUNTER_FOLDED] == 0,
               "往復: 眠る世界だけ頁を畳んだ");
        Expect(record.checkedSleeps > 0 && record.wrongSleeps == 0,
               "往復: 眠る時に導かれる値は覆われていないセルの平均を残す");
        Expect(record.wrongDiscards == 0, "往復: 眠る時に捨てる印の欄は 0 を残す(T-1159)");
        Expect(record.checkedWake && !record.wrongWake, "往復: 起こすと眠った値を子は親と同じ数で配る");
        Expect(truthMismatches == 0, "往復: 眠らせて起こしても真実の要約が起きたままの世界と毎刻みビット一致");
        Expect(unbalanced == 0, "往復: Σ 保存量 − 帳簿が両方の世界で毎刻み最初と同じ");
        Expect(HashRealLeaves(sleeping.nest) != HashRealLeaves(awake.nest) || !SF_WARM_START_IN_STATE,
               "往復: 温めた始まりは眠った所の粗い値から解き始めるので、真実でない要約は違ってよい(見分けられる)");

        // --- 保存点(眠っている途中)に戻して進めると、続けた場合と毎刻み同じ状態の要約 ---
        Expect(RestoreNestState(sleeping.nest, save), "保存点: 戻せる");
        int replayMismatches = 0;
        for (uint64_t tick = saveTick; tick < END_TICK; ++tick) {
            StepWorld(sleeping, table, tick, wakeTick, nullptr);
            replayMismatches += HashWholeNest(sleeping.nest) == wholeHashes[tick - saveTick] ? 0 : 1;
        }

        Expect(replayMismatches == 0, "保存点: 戻して進めると毎刻み同じ要約(温めた始まりも保存点に入る = D-549 (A))");
    }

    // ハッシュ: 導かれる値は入らない・温めた始まりは D-549 の印どおり・保存点に戻すと導かれる値は 0
    void TestHashAndSave(const BakedReactionTable& table) {
        World world = MakeWorld(table, true);
        StepWorld(world, table, 0, END_TICK, nullptr);
        MultiresNest& nest = world.nest;
        Expect(!MrIsUniform(nest.blocks[0]), "要約: 木箱は最初の刻みに頁に広がる");

        const MultiresSaveState save = SaveNestState(nest);
        const uint64_t whole = HashWholeNest(nest);
        const uint64_t leaves = HashRealLeaves(nest);
        const uint64_t truth = HashTruthLeaves(nest);
        StoreNestField(nest, world.fields.pressure, 0, 9, 0, 12345);
        StoreNestField(nest, world.fields.force, 0, 9, 2, -12345);
        Expect(HashWholeNest(nest) == whole && HashRealLeaves(nest) == leaves && HashTruthLeaves(nest) == truth,
               "要約: 導かれる値を変えても要約は変わらない");
        StoreNestField(nest, world.fields.warm, 0, 9, 0, 777);
        Expect((HashWholeNest(nest) != whole) == SF_WARM_START_IN_STATE &&
                   (HashRealLeaves(nest) != leaves) == SF_WARM_START_IN_STATE && HashTruthLeaves(nest) == truth,
               "要約: 温めた始まりは D-549 の印どおり・真実の要約には入らない");
        StoreNestField(nest, world.fields.amount, 0, 9, 0, 1);
        Expect(HashTruthLeaves(nest) != truth, "要約: 真実を変えると真実の要約が変わる");

        Expect(RestoreNestState(nest, save) && HashWholeNest(nest) == whole, "保存点: 戻すと保存した時の要約");
        Expect(LoadNestField(nest, world.fields.pressure, 0, 9, 0) == 0 &&
                   LoadNestField(nest, world.fields.force, 0, 9, 2) == 0,
               "保存点: 導かれる値は保存点に入らず、戻すと 0(次に解き直す)");
        Expect(LoadNestField(nest, world.fields.warm, 0, 9, 0) != 777, "保存点: 温めた始まりは保存した時の値に戻る");
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "試験の表をベイクできない: {}", table.error());
            return 1;
        }

        TestKinds();
        TestMean();
        TestHashAndSave(*table);
        TestSleepWake(*table);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "field_storage_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "field_storage_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
