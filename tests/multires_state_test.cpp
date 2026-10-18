// multires_state_test.cpp — 状態の登録(運ぶ計算の共通の土台の段 1 ①。T-0711。ADR-0329)の CPU のテスト。確かめること:
//   - 状態のハッシュが区画の一覧(VisitNestState)を回る形になっても、欄の無い世界の要約(HashWholeNest・HashRealLeaves)は
//     T-0711 より前の作り(このファイルの Legacy*。前の multires_nest.cpp の写し)と毎刻みビット単位で同じ
//     (本物の鎖・影の鎖・溢れを使う世界・たくさんの要求・一様なブロックの頁に広げる/畳む)
//   - 保存点: 刻みの途中で写して先へ進め、戻して同じ刻みを進めると、毎刻み同じ要約(活性の種・索引も戻る)。
//     写し(CopyNestState)を早送りしても本物と同じ。形(登録した欄)の違う世界へは戻さない(false・何も変えない)
//   - 試験用の欄(セル・面・ブロック・世界。量の欄・量でない欄・ハッシュに入らない欄): 細かくすると子は親と同じ数・覆われた親のセルは 0、
//     粗くすると和 ÷ 8 で余りは欄の帳簿へ。量の欄の「値 × 8^(最も細かい − レベル) + 帳簿」が毎刻み最初とビット一致。
//     一様なブロックは頁に広がる時に欄も広がり、欄が一様なら畳まれ・一様でなければ畳まれない。違う値の一様な子を粗くすると親が頁に広がる。
//     ハッシュに入る欄を変えると要約が変わり、入らない欄では変わらない
//   - 観察の枠(影・写し)の欄(T-0761。ADR-0361): 運ぶ計算と同じ形の欄(気体の運動量・液体の面の運動量など)を持つ世界で、
//     影を細かくする/引き戻す・写しを置くの前後で本物の葉の要約と保存量(帳簿込み)がビット単位で同じ・覗かない世界と毎刻み同じ。
//     影は細かくすると親と同じ数・引き戻すと親のセルごとに子の和 = 親 × 8(負にならない欄は比で配る・負の値がある欄は等しく足す)
#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "multires_test_scene.h"
#include "multires_uniform_scene.h"
#include "reaction_test_table.h"
#include "sim/multires_fields.h"
#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"
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

    // --- T-0711 より前の要約(multires_nest.cpp の写し。区画の一覧を回る形と比べる基準)---

    uint64_t LegacyHashFraction(const MrFraction& fraction) {
        uint64_t hash = FxHashCombine(0, fraction.energy);
        hash = FxHashCombine(hash, fraction.speciesCount);
        for (uint32_t i = 0; i < fraction.speciesCount; ++i) {
            hash = FxHashCombine(hash, fraction.species[i]);
            hash = FxHashCombine(hash, fraction.amounts[i]);
        }

        return hash;
    }

    uint64_t LegacyHashOverflowArea(uint64_t hash, const MultiresOverflowArea& area) {
        for (const uint32_t offset : area.offsets)
            hash = FxHashCombine(hash, offset);

        for (size_t i = 0; i < area.species.size(); ++i) {
            hash = FxHashCombine(hash, area.species[i]);
            hash = FxHashCombine(hash, area.amounts[i]);
        }

        return hash;
    }

    uint64_t LegacyHashCellOverflow(uint64_t hash, const MultiresNest& nest, uint32_t slot, uint32_t index) {
        if (!nest.wideCells)
            return hash;

        const RxWideCell cell = LoadWideNestCell(nest, slot, index);
        for (uint32_t i = RX_MAX_CELL_SPECIES; i < cell.speciesCount; ++i) {
            hash = FxHashCombine(hash, cell.species[i]);
            hash = FxHashCombine(hash, cell.amounts[i]);
        }

        const MrWideFraction fraction = LoadWideFraction(nest, nest.blocks[slot].fraction, index);
        for (uint32_t i = RX_MAX_CELL_SPECIES; i < fraction.speciesCount; ++i) {
            hash = FxHashCombine(hash, fraction.species[i]);
            hash = FxHashCombine(hash, fraction.amounts[i]);
        }

        return hash;
    }

    uint64_t LegacyHashBlock(const MrBlock& block) {
        uint64_t hash = FxHashCombine(0, static_cast<uint64_t>(block.originX));
        hash = FxHashCombine(hash, static_cast<uint64_t>(block.originY));
        hash = FxHashCombine(hash, static_cast<uint64_t>(block.originZ));
        hash = FxHashCombine(hash, static_cast<uint64_t>(static_cast<int64_t>(block.level)));
        hash = FxHashCombine(hash, block.kind);
        hash = FxHashCombine(hash, block.parent);
        hash = FxHashCombine(hash, block.parentOctant);
        hash = FxHashCombine(hash, block.fraction);
        for (const uint32_t child : block.children)
            hash = FxHashCombine(hash, child);

        return hash;
    }

    uint64_t LegacyHashRealLeaves(const MultiresNest& nest) {
        uint64_t hash = 0;
        for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL)
                continue;

            hash = FxHashCombine(hash, LegacyHashBlock(block));
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (!MrIsSteppedCell(block, index))
                    continue;

                hash = FxHashCombine(hash, HashReactionCell(LoadNestCell(nest, slot, index)));
                hash = FxHashCombine(hash, LegacyHashFraction(nest_detail::FractionAt(nest, block.fraction, index)));
                hash = LegacyHashCellOverflow(hash, nest, slot, index);
            }
        }

        return hash;
    }

    uint64_t LegacyHashWholeNest(const MultiresNest& nest) {
        uint64_t hash = 0;
        for (const MrBlock& block : nest.blocks) {
            hash = FxHashCombine(hash, LegacyHashBlock(block));
            hash = FxHashCombine(hash, block.activeTick);
            hash = FxHashCombine(hash, block.busyTick);
            hash = FxHashCombine(hash, block.wakeTick);
            hash = FxHashCombine(hash, block.page);
            hash = FxHashCombine(hash, block.quietCheck);
        }

        for (const RxCell& cell : nest.cells)
            hash = FxHashCombine(hash, HashReactionCell(cell));

        for (const MrFraction& fraction : nest.fractions)
            hash = FxHashCombine(hash, LegacyHashFraction(fraction));

        for (const uint32_t slot : nest.freeBlocks)
            hash = FxHashCombine(hash, slot);

        for (const uint32_t slot : nest.freeFractions)
            hash = FxHashCombine(hash, slot);

        for (const uint64_t value : nest.ledger)
            hash = FxHashCombine(hash, value);

        for (const uint32_t counter : nest.counters)
            hash = FxHashCombine(hash, counter);

        if (nest.wideCells) {
            for (const MultiresOverflowArea& area : nest.cellOverflow)
                hash = LegacyHashOverflowArea(hash, area);

            for (const MultiresOverflowArea& area : nest.fractionOverflow)
                hash = LegacyHashOverflowArea(hash, area);

            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                if (!MrIsUniform(nest.blocks[slot]) || nest.blocks[slot].kind == MR_BLOCK_UNUSED)
                    continue;

                const MultiresUniformOverflow& tail = nest.uniformOverflow[slot];
                hash = FxHashCombine(hash, tail.species.size());
                for (size_t i = 0; i < tail.species.size(); ++i) {
                    hash = FxHashCombine(hash, tail.species[i]);
                    hash = FxHashCombine(hash, tail.amounts[i]);
                }
            }
        }

        return hash;
    }

    // 2 つの要約が前の作りと同じか(同じでない刻みを数える)
    int LegacyMismatch(const MultiresNest& nest) {
        const bool same = HashWholeNest(nest) == LegacyHashWholeNest(nest) &&
                          HashRealLeaves(nest) == LegacyHashRealLeaves(nest);

        return same ? 0 : 1;
    }

    // --- 1. 欄の無い世界の要約は前と同じ ---

    void TestLegacyHashes(const BakedReactionTable& table) {
        int mismatched = 0;
        for (const test::MultiresScenario scenario : {test::MultiresScenario::Real, test::MultiresScenario::Shadow}) {
            MultiresNest nest = test::MakeMultiresNestForTest(table);
            for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
                test::StepMultiresScene(nest, table, scenario, tick);
                mismatched += LegacyMismatch(nest);
            }
        }

        Expect(mismatched == 0, "欄の無い世界: 本物の鎖・影の鎖の要約が前の作りと毎刻み同じ");

        // 溢れを使う世界(区画が増える)
        mismatched = 0;
        MultiresNest wide = test::MakeMultiresNestForTest(table);
        EnableWideCells(wide);
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            test::StepMultiresScene(wide, table, test::MultiresScenario::Real, tick);
            mismatched += LegacyMismatch(wide);
        }

        Expect(mismatched == 0, "欄の無い世界: 溢れを使う世界の要約が前の作りと毎刻み同じ");

        // たくさんの要求(取り合い・枠不足・端数が帳簿へ)
        mismatched = 0;
        MultiresNest stress = test::MakeStressNest(table);
        for (uint64_t tick = 0; tick < test::STRESS_TICKS; ++tick) {
            SubmitRequests(stress, test::MakeStressRequests(stress, tick));
            ProcessRequests(stress);
            StepNest(stress, table, test::STRESS_SEED, tick);
            mismatched += LegacyMismatch(stress);
        }

        Expect(mismatched == 0, "欄の無い世界: たくさんの要求の要約が前の作りと毎刻み同じ");

        // 一様なブロック(頁に広げる・畳む・静かな葉を粗くする)
        mismatched = 0;
        MultiresNest uniform = test::MakeUniformNest(table, test::UNIFORM_WORLD_BLOCKS);
        for (uint64_t tick = 0; tick < test::UNIFORM_TICKS; ++tick) {
            test::BeginUniformTick(uniform, tick);
            StepActive(uniform, table, test::STRESS_SEED, tick);
            mismatched += LegacyMismatch(uniform);
        }

        Expect(mismatched == 0, "欄の無い世界: 一様なブロックの要約が前の作りと毎刻み同じ");
        Expect(uniform.counters[MR_COUNTER_FOLDED] > 0, "欄の無い世界: 一様なブロックの場面で畳んだ");
    }

    // --- 試験用の欄 ---

    struct TestFields {
        uint32_t amount = MULTIRES_NO_FIELD;  // セル・量(VOLUME)・保存量・ハッシュに入る
        uint32_t face = MULTIRES_NO_FIELD;    // 面 ×3・量・液体に付く・ハッシュに入る
        uint32_t tag = MULTIRES_NO_FIELD;     // セル・量でない(SAME)・気体に付く・ハッシュに入らない
        uint32_t block = MULTIRES_NO_FIELD;   // ブロック・ハッシュに入る
        uint32_t world = MULTIRES_NO_FIELD;   // 世界 ×2・ハッシュに入る
    };

    TestFields RegisterTestFields(MultiresNest& nest) {
        TestFields fields;
        fields.amount = RegisterNestField(nest, "test_amount",
                                          SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_VOLUME,
                                                          SF_FLAG_CONSERVED | SF_FLAG_HASHED, 1));
        fields.face = RegisterNestField(nest, "test_face",
                                        SfMakeFieldDesc(SF_PLACE_FACE, SF_TYPE_I32, SF_PHASE_LIQUID, SF_SCALE_VOLUME,
                                                        SF_FLAG_CONSERVED | SF_FLAG_HASHED, 3));
        fields.tag = RegisterNestField(nest, "test_tag",
                                       SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_U32, SF_PHASE_GAS, SF_SCALE_SAME, 0, 1));
        fields.block = RegisterNestField(
            nest, "test_block",
            SfMakeFieldDesc(SF_PLACE_BLOCK, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_SAME, SF_FLAG_HASHED, 1));
        fields.world = RegisterNestField(
            nest, "test_world",
            SfMakeFieldDesc(SF_PLACE_WORLD, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_SAME, SF_FLAG_HASHED, 2));

        return fields;
    }

    // 符号付きのいろいろな値(-1000〜1000)
    int64_t VariedValue(uint64_t a, uint64_t b, uint32_t purpose) {
        return static_cast<int64_t>(FxHash64(test::MULTIRES_TEST_SEED, a, b, purpose) % 2001) - 1000;
    }

    // 頁を持つ枠 slot の欄をいろいろな値で埋める
    void FillPageFields(MultiresNest& nest, const TestFields& fields, uint32_t slot) {
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            StoreNestField(nest, fields.amount, slot, index, 0, VariedValue(slot, index, 1));
            for (uint32_t c = 0; c < 3; ++c)
                StoreNestField(nest, fields.face, slot, index, c, VariedValue(slot, index, 2 + c));

            StoreNestField(nest, fields.tag, slot, index, 0, VariedValue(slot, index, 5) + 1000);
        }

        StoreNestBlockField(nest, fields.block, slot, 0, VariedValue(slot, 0, 6));
    }

    // 量の欄の合計(保存の検査の 1 関数の欄の列。本物の刻むセルの値 × 8^(finest − レベル) − 世界の帳簿のその欄の列。T-0762)
    Wide256 FieldTotal(const MultiresNest& nest, const BakedReactionTable& table, uint32_t field, uint32_t component,
                       int32_t finestLevel) {
        return ComputeConservedTotals(nest, table, finestLevel).fields[NestConservedIndex(nest, field) + component];
    }

    // 欄の値 component の、世界の帳簿の「粗くした端数・出」の列に何か載っているか
    bool FieldRemainderBooked(const MultiresNest& nest, uint32_t field, uint32_t component) {
        const uint32_t column = NestFieldLedgerBase(nest) + NestConservedIndex(nest, field) + component;
        const uint32_t slot = WlSlot(WL_REASON_COARSEN_REMAINDER, WL_DIRECTION_OUT);
        for (uint32_t row = 0; row < MR_LEDGER_LEVELS; ++row) {
            const int32_t level = MR_LEDGER_LEVEL_MIN + static_cast<int32_t>(row);
            if (nest.ledger[WlAddress(slot, level, column, nest.capacity.ledgerColumns)] != 0)
                return true;
        }

        return false;
    }

    // 本物の鎖に欄を付けた世界
    MultiresNest MakeFieldChainNest(const BakedReactionTable& table, TestFields& fields) {
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(
            table, test::MULTIRES_WORLD_BLOCKS, test::MULTIRES_LEVELS, test::MULTIRES_FRACTION_CAPACITY));
        fields = RegisterTestFields(nest);
        PlaceRootBlock(nest, 0, 0, 0, test::MakeMultiresRootCells(table));
        FillPageFields(nest, fields, test::MULTIRES_ROOT_SLOT);
        StoreNestWorldField(nest, fields.world, 1, -77);

        return nest;
    }

    // --- 2. 欄が細かく/粗くに付いて行く・量の欄の保存 ---

    void TestFieldsFollowTree(const BakedReactionTable& table) {
        TestFields fields;
        MultiresNest nest = MakeFieldChainNest(table, fields);
        Expect(fields.amount == 0 && fields.world == 4, "欄: 登録の順に番号");
        Expect(RegisterNestField(nest, "test_amount", nest.fields[0].desc) == MULTIRES_NO_FIELD,
               "欄: 同じ名前は登録しない");
        Expect(nest.capacity.ledgerColumns == 1 + table.species.size() + 4 &&
                   NestFieldLedgerBase(nest) == 1 + table.species.size() &&
                   NestConservedIndex(nest, fields.face) == 1 &&
                   NestConservedIndex(nest, fields.tag) == MULTIRES_NO_FIELD,
               "欄: 保存量の欄の値の数だけ帳簿の列が増える(量 1 + 面 3)");
        MultiresNest other = MakeMultiresNest(nest.capacity);
        Expect(RegisterNestField(other, "volume_only",
                                 SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_SCALE_VOLUME, 0, 1)) ==
                   MULTIRES_NO_FIELD,
               "欄: 保存量でない量の欄は登録しない(量の欄 = 保存量)");

        constexpr int32_t FINEST = static_cast<int32_t>(test::MULTIRES_LEVELS);
        const Wide256 initialAmount = FieldTotal(nest, table, fields.amount, 0, FINEST);
        const Wide256 initialFace = FieldTotal(nest, table, fields.face, 2, FINEST);
        int lostTicks = 0;
        bool checkedRefine = false;
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            test::StepMultiresScene(nest, table, test::MultiresScenario::Real, tick);
            lostTicks += FieldTotal(nest, table, fields.amount, 0, FINEST) == initialAmount ? 0 : 1;
            lostTicks += FieldTotal(nest, table, fields.face, 2, FINEST) == initialFace ? 0 : 1;
            if (tick != test::MULTIRES_REFINE_TICK)
                continue;

            // --- 細かくした直後: レベル 1 の子のセルは親の同じ所と同じ数・覆われた親のセルは 0 ---
            const MrBlock& root = nest.blocks[test::MULTIRES_ROOT_SLOT];
            const uint32_t octant = MrOctantOfCell(MrCellIndex(3, 4, 5));
            const uint32_t child = root.children[octant];
            bool same = child != MR_NO_BLOCK;
            for (uint32_t index = 0; same && index < MR_BLOCK_CELLS; ++index) {
                if (MrIsCoveredCell(nest.blocks[child], index))  // レベル 2 の子に覆われた所は 0
                    continue;

                const uint32_t parentCell = MrParentCellOfChild(octant, index);
                same = LoadNestField(nest, fields.amount, child, index, 0) ==
                           VariedValue(test::MULTIRES_ROOT_SLOT, parentCell, 1) &&
                       LoadNestField(nest, fields.face, child, index, 1) ==
                           VariedValue(test::MULTIRES_ROOT_SLOT, parentCell, 3) &&
                       LoadNestField(nest, fields.amount, test::MULTIRES_ROOT_SLOT, parentCell, 0) == 0;
            }

            Expect(same, "欄: 細かくすると子は親と同じ数・覆われた親のセルは 0");
            Expect(child != MR_NO_BLOCK &&
                       LoadNestBlockField(nest, fields.block, child, 0) == VariedValue(test::MULTIRES_ROOT_SLOT, 0, 6),
                   "欄: ブロックの欄は子のブロックが親の値");
            checkedRefine = true;
        }

        Expect(checkedRefine, "欄: 細かくした刻みを調べた");
        Expect(lostTicks == 0, "欄: 量の欄の「値 + 帳簿」が毎刻み最初とビット一致(細かく・粗く)");
        Expect(nest.counters[MR_COUNTER_FREE_BLOCKS] == test::MULTIRES_WORLD_BLOCKS - 1, "欄: 戻した後は根だけ");

        // 戻った根の欄: 粗くした八分の一は和 ÷ 8(同じ数 8 つの和なので元の値)・そのほかは元のまま
        bool restored = true;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
            restored = restored && LoadNestField(nest, fields.amount, test::MULTIRES_ROOT_SLOT, index, 0) ==
                                       VariedValue(test::MULTIRES_ROOT_SLOT, index, 1);

        Expect(restored, "欄: 細かくして粗く戻すと根の値は元どおり(子が親と同じ数のままなら)");
        Expect(LoadNestWorldField(nest, fields.world, 1) == -77, "欄: 世界の欄はそのまま");

        // --- ハッシュ: 入る欄を変えると変わる・入らない欄では変わらない ---
        const uint64_t whole = HashWholeNest(nest);
        const uint64_t leaves = HashRealLeaves(nest);
        StoreNestField(nest, fields.tag, test::MULTIRES_ROOT_SLOT, 7, 0, 123);
        Expect(HashWholeNest(nest) == whole && HashRealLeaves(nest) == leaves,
               "欄: ハッシュに入らない欄では要約が変わらない");
        StoreNestField(nest, fields.amount, test::MULTIRES_ROOT_SLOT, 7, 0, 123);
        Expect(HashWholeNest(nest) != whole && HashRealLeaves(nest) != leaves,
               "欄: ハッシュに入る欄を変えると要約が変わる");
        const uint64_t afterCell = HashWholeNest(nest);
        StoreNestWorldField(nest, fields.world, 0, 5);
        Expect(HashWholeNest(nest) != afterCell, "欄: 世界の欄も要約に入る");
    }

    // 粗くした余りが帳簿へ: 子のセル 8 つに違う値を置いて粗くする
    void TestCoarsenRemainder(const BakedReactionTable& table) {
        TestFields fields;
        MultiresNest nest = MakeFieldChainNest(table, fields);
        constexpr int32_t FINEST = static_cast<int32_t>(test::MULTIRES_LEVELS);
        const sim::MultiresPoint point = test::MakeMultiresPoint(test::MULTIRES_LEVELS);
        const MrRequest refine = test::MakeMultiresRequest(MR_REQUEST_REFINE, point, 1);
        SubmitRequests(nest, std::span(&refine, 1));
        ProcessRequests(nest);

        const uint32_t child = nest.blocks[test::MULTIRES_ROOT_SLOT].children[MrOctantOfCell(MrCellIndex(3, 4, 5))];
        Expect(child != MR_NO_BLOCK && !MrIsUniform(nest.blocks[child]), "余り: レベル 1 の子が頁を持つ");
        if (child == MR_NO_BLOCK)
            return;

        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
            StoreNestField(nest, fields.amount, child, index, 0, VariedValue(child, index, 9));

        const Wide256 before = FieldTotal(nest, table, fields.amount, 0, FINEST);
        const MrRequest coarsen = test::MakeMultiresRequest(MR_REQUEST_COARSEN, point, 1);
        SubmitRequests(nest, std::span(&coarsen, 1));
        ProcessRequests(nest);

        Expect(nest.blocks[child].kind == MR_BLOCK_UNUSED, "余り: 粗くした");
        Expect(FieldRemainderBooked(nest, fields.amount, 0) && !FieldRemainderBooked(nest, fields.face, 0),
               "余り: 粗くした余りが世界の帳簿のその欄の列へ");
        Expect(FieldTotal(nest, table, fields.amount, 0, FINEST) == before,
               "余り: 「値 + 帳簿」が粗くする前とビット一致");
    }

    // --- 3. 一様なブロック: 頁に広げる・畳む・違う値の一様な子を粗くすると親が頁に広がる ---

    MultiresNest MakeUniformFieldNest(const BakedReactionTable& table, uint32_t& amount) {
        MultiresCapacity capacity = test::MakeMultiresCapacity(table, test::UNIFORM_WORLD_BLOCKS, 0,
                                                               test::UNIFORM_FRACTIONS);
        MultiresNest nest = MakeMultiresNest(capacity);
        amount = RegisterTestFields(nest).amount;
        const std::vector<RxCell> air(MR_BLOCK_CELLS, test::MakeAirCell(table));
        const std::vector<RxCell> crate(MR_BLOCK_CELLS, test::MakeCrateCell(table));
        for (uint32_t root = 0; root < test::UNIFORM_ROOTS; ++root) {
            const int64_t x = root % test::UNIFORM_ROOT_EDGE;
            const int64_t y = (root / test::UNIFORM_ROOT_EDGE) % test::UNIFORM_ROOT_EDGE;
            const int64_t z = root / (test::UNIFORM_ROOT_EDGE * test::UNIFORM_ROOT_EDGE);
            const uint32_t slot = PlaceRootBlock(nest, x * 8, y * 8, z * 8,
                                                 root < test::UNIFORM_CRATE_ROOTS ? crate : air);
            StoreNestFieldUniform(nest, amount, slot, 0, 40 + root);
        }

        return nest;
    }

    void TestUniformFields(const BakedReactionTable& table) {
        uint32_t amount = MULTIRES_NO_FIELD;
        MultiresNest nest = MakeUniformFieldNest(table, amount);
        const Wide256 initial = FieldTotal(nest, table, amount, 0, test::UNIFORM_CHAIN_DEPTH);
        bool expanded = false;
        bool expandedValues = true;
        int lostTicks = 0;
        for (uint64_t tick = 0; tick < test::UNIFORM_TICKS; ++tick) {
            test::BeginUniformTick(nest, tick);
            StepActive(nest, table, test::STRESS_SEED, tick);
            lostTicks += FieldTotal(nest, table, amount, 0, test::UNIFORM_CHAIN_DEPTH) == initial ? 0 : 1;
            if (MrIsUniform(nest.blocks[0]))
                continue;

            // 頁に広がった木箱(枠 0): 欄も広がって一様の値のまま
            expanded = true;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                expandedValues = expandedValues && LoadNestField(nest, amount, 0, index, 0) == 40;
        }

        Expect(expanded && expandedValues, "一様: 頁に広がる時に欄も一様の値で広がる");
        Expect(MrIsUniform(nest.blocks[0]) && LoadNestField(nest, amount, 0, 0, 0) == 40,
               "一様: 欄が一様なら燃え尽きた木箱は畳まれ、欄は値 1 つ");
        Expect(lostTicks == 0, "一様: 量の欄の「値 + 帳簿」が毎刻み最初とビット一致(鎖の細かく・粗く)");
        Expect(nest.counters[MR_COUNTER_FREE_BLOCKS] == test::UNIFORM_WORLD_BLOCKS - test::UNIFORM_ROOTS,
               "一様: 空気の根の鎖は静かになって根だけに戻る");

        // --- 欄が一様でない頁は畳まない ---
        MultiresNest uneven = MakeUniformFieldNest(table, amount);
        bool storedUneven = false;
        for (uint64_t tick = 0; tick < test::UNIFORM_TICKS; ++tick) {
            test::BeginUniformTick(uneven, tick);
            StepActive(uneven, table, test::STRESS_SEED, tick);
            if (!storedUneven && !MrIsUniform(uneven.blocks[0])) {
                StoreNestField(uneven, amount, 0, 9, 0, 41);
                storedUneven = true;
            }
        }

        Expect(storedUneven && !MrIsUniform(uneven.blocks[0]), "一様: 欄が一様でない頁は畳まない");

        // --- 違う値の一様な子を一様な親へ粗くすると、親が頁に広がる(八分の一は子の値・残りは親の値)---
        MultiresNest chain = MakeUniformFieldNest(table, amount);
        test::BeginUniformTick(chain, 0);  // 空気の根(枠 7)の中に一様な鎖 6 段
        uint32_t deepest = test::UNIFORM_CHAIN_ROOT;
        while (MrHasRealChild(chain.blocks[deepest])) {
            const MrBlock& block = chain.blocks[deepest];
            deepest = *std::ranges::find_if(block.children, [](uint32_t c) { return c != MR_NO_BLOCK; });
        }

        Expect(MrIsUniform(chain.blocks[deepest]) && chain.blocks[deepest].level == test::UNIFORM_CHAIN_DEPTH &&
                   LoadNestField(chain, amount, deepest, 0, 0) == 47,
               "一様: 一様な親の子は一様で、欄は親の値");
        StoreNestFieldUniform(chain, amount, deepest, 0, 50);
        const Wide256 before = FieldTotal(chain, table, amount, 0, test::UNIFORM_CHAIN_DEPTH);
        const uint32_t parent = chain.blocks[deepest].parent;
        const uint32_t octant = chain.blocks[deepest].parentOctant;
        const MrRequest coarsen = test::MakeUniformChainRequest(MR_REQUEST_COARSEN, test::UNIFORM_CHAIN_DEPTH);
        SubmitRequests(chain, std::span(&coarsen, 1));
        ProcessRequests(chain);
        Expect(!MrIsUniform(chain.blocks[parent]), "一様: 欄の値が違う一様な子を粗くすると親が頁に広がる");
        Expect(LoadNestField(chain, amount, parent, MrOctantCell(octant, 0), 0) == 50 &&
                   LoadNestField(chain, amount, parent, MrOctantCell(octant ^ 1u, 0), 0) == 47,
               "一様: 八分の一は子の値・残りは親の値");
        Expect(FieldTotal(chain, table, amount, 0, test::UNIFORM_CHAIN_DEPTH) == before,
               "一様: 粗くしても「値 + 帳簿」が同じ");
    }

    // --- 4. 保存点と写し ---

    MultiresNest MakeFieldStressNest(const BakedReactionTable& table, bool withFields) {
        MultiresNest nest = MakeMultiresNest(
            test::MakeMultiresCapacity(table, test::STRESS_WORLD_BLOCKS, 0, test::STRESS_FRACTIONS));
        TestFields fields;
        if (withFields)
            fields = RegisterTestFields(nest);

        const std::vector<RxCell> cells = test::MakeMultiresRootCells(table);
        for (uint32_t root = 0; root < 8; ++root) {
            const uint32_t slot = PlaceRootBlock(nest, int64_t{root & 1u} * 8, int64_t{(root >> 1) & 1u} * 8,
                                                 int64_t{(root >> 2) & 1u} * 8, cells);
            if (withFields)
                FillPageFields(nest, fields, slot);
        }

        return nest;
    }

    void StepStress(MultiresNest& nest, const BakedReactionTable& table, uint64_t tick) {
        SubmitRequests(nest, test::MakeStressRequests(nest, tick));
        ProcessRequests(nest);
        StepActive(nest, table, test::STRESS_SEED, tick);
    }

    void TestSaveRestore(const BakedReactionTable& table, bool withFields) {
        constexpr uint64_t SAVE_TICK = 40;
        constexpr uint64_t END_TICK = 100;
        MultiresNest nest = MakeFieldStressNest(table, withFields);
        for (uint64_t tick = 0; tick < SAVE_TICK; ++tick)
            StepStress(nest, table, tick);

        const MultiresSaveState save = SaveNestState(nest);
        const uint64_t savedHash = HashWholeNest(nest);
        MultiresNest copy = MakeFieldStressNest(table, withFields);
        Expect(CopyNestState(nest, copy) && HashWholeNest(copy) == savedHash, "保存点: 写しの要約が写す元と同じ");

        std::vector<uint64_t> hashes;
        for (uint64_t tick = SAVE_TICK; tick < END_TICK; ++tick) {
            StepStress(nest, table, tick);
            hashes.push_back(HashWholeNest(nest));
        }

        // --- 戻して同じ刻みを進める・写しを早送りする ---
        Expect(RestoreNestState(nest, save) && HashWholeNest(nest) == savedHash, "保存点: 戻すと保存した時の要約");
        int replayMismatches = 0;
        int copyMismatches = 0;
        for (uint64_t tick = SAVE_TICK; tick < END_TICK; ++tick) {
            StepStress(nest, table, tick);
            StepStress(copy, table, tick);
            replayMismatches += HashWholeNest(nest) == hashes[tick - SAVE_TICK] ? 0 : 1;
            copyMismatches += HashWholeNest(copy) == hashes[tick - SAVE_TICK] ? 0 : 1;
        }

        Expect(replayMismatches == 0, "保存点: 戻して進めると毎刻み同じ要約(種・索引も戻る)");
        Expect(copyMismatches == 0, "保存点: 写しを早送りすると本物と毎刻み同じ要約");

        // --- 形の違う世界(登録した欄が違う)へは戻さない ---
        MultiresNest other = MakeFieldStressNest(table, !withFields);
        const uint64_t otherHash = HashWholeNest(other);
        Expect(!RestoreNestState(other, save) && HashWholeNest(other) == otherHash, "保存点: 形の違う世界へは戻さない");
    }

    // --- 5. 観察の枠(影・写し)の欄(T-0761。ADR-0361)---

    // 運ぶ計算が登録する欄と同じ形(気体の運動量・運動のエネルギー・前の質量・液体の面の運動量。名前と形は multires_gas.cpp・
    // multires_liquid.cpp の登録と同じ。試験の表は基準の大気を持たないので形だけ借りる)
    struct TransportFields {
        uint32_t gasMomentum = MULTIRES_NO_FIELD;     // セル ×3・符号付き・保存量
        uint32_t gasKinetic = MULTIRES_NO_FIELD;      // セル・負にならない・保存量
        uint32_t gasMassBefore = MULTIRES_NO_FIELD;   // セル・SAME・負にならない
        uint32_t liquidMomentum = MULTIRES_NO_FIELD;  // 面 ×3・符号付き・保存量
    };

    constexpr uint32_t OBSERVER_MIRROR_SLOT = test::MULTIRES_SHADOW_SLOT + test::MULTIRES_LEVELS;  // 影の鎖の後ろ
    constexpr uint64_t OBSERVER_REFINE_TICK = 12;  // 本物の鎖(刻み 10)の後に覗き始める
    constexpr uint64_t OBSERVER_REMOVE_TICK = 66;  // 本物の鎖を戻している途中で覗きをやめる

    TransportFields RegisterTransportFields(MultiresNest& nest) {
        constexpr uint32_t CONSERVED = SF_FLAG_CONSERVED | SF_FLAG_HASHED;
        const auto cell = [&](const char* name, uint32_t phase, uint32_t scale, uint32_t flags, uint32_t components) {
            return RegisterNestField(nest, name,
                                     SfMakeFieldDesc(SF_PLACE_CELL, SF_TYPE_I64, phase, scale, flags, components));
        };

        TransportFields fields;
        fields.gasMomentum = cell("gas.momentum", SF_PHASE_GAS, SF_SCALE_VOLUME, CONSERVED, 3);
        fields.gasKinetic = cell("gas.kinetic_energy", SF_PHASE_GAS, SF_SCALE_VOLUME, CONSERVED, 1);
        fields.gasMassBefore = cell("gas.mass_before", SF_PHASE_GAS, SF_SCALE_SAME, SF_FLAG_HASHED, 1);
        fields.liquidMomentum = RegisterNestField(
            nest, "liquid.momentum",
            SfMakeFieldDesc(SF_PLACE_FACE, SF_TYPE_I64, SF_PHASE_LIQUID, SF_SCALE_VOLUME, CONSERVED, 3));

        return fields;
    }

    // 欄が負にならない量か(引き戻しで比で配る方になる欄。試験の値の作り方を分ける)
    bool NonNegativeField(const MultiresNest& nest, uint32_t field) {
        const std::string& name = nest.fields[field].name;

        return nest.fields[field].desc.type == SF_TYPE_U32 || name == "gas.kinetic_energy" || name == "gas.mass_before";
    }

    // 頁を持つ枠 slot のセル・面の欄を全部いろいろな値で(負にならない欄は 0〜2000)、ブロックの欄も。salt で値を変える
    void FillAllPageFields(MultiresNest& nest, uint32_t slot, uint64_t salt) {
        for (uint32_t f = 0; f < nest.fields.size(); ++f) {
            const SfFieldDesc& desc = nest.fields[f].desc;
            const int64_t offset = NonNegativeField(nest, f) ? 1000 : 0;
            if (desc.place == SF_PLACE_BLOCK)
                StoreNestBlockField(nest, f, slot, 0, VariedValue(salt, slot, 40 + f));

            if (!SfOnCells(desc))
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                for (uint32_t c = 0; c < desc.components; ++c)
                    StoreNestField(nest, f, slot, index, c, VariedValue(salt + slot, index, (f * 4) + c) + offset);
            }
        }
    }

    // 根に試験の欄と運ぶ計算の欄を付けた世界(観察の枠 = 影の鎖 9 段 + 写し 1)
    MultiresNest MakeObserverFieldNest(const BakedReactionTable& table) {
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(
            table, test::MULTIRES_WORLD_BLOCKS, test::MULTIRES_LEVELS + 1, test::MULTIRES_FRACTION_CAPACITY));
        const TestFields fields = RegisterTestFields(nest);
        RegisterTransportFields(nest);
        PlaceRootBlock(nest, 0, 0, 0, test::MakeMultiresRootCells(table));
        FillAllPageFields(nest, test::MULTIRES_ROOT_SLOT, 0);
        StoreNestWorldField(nest, fields.world, 1, -77);

        return nest;
    }

    // 世界の姿(本物の葉の要約と、帳簿込みの保存量)
    struct WorldView {
        uint64_t leaves = 0;
        ConservedTotals totals;

        bool operator==(const WorldView&) const = default;
    };

    WorldView ViewWorld(const MultiresNest& nest, const BakedReactionTable& table) {
        return {.leaves = HashRealLeaves(nest),
                .totals = ComputeConservedTotals(nest, table, static_cast<int32_t>(test::MULTIRES_LEVELS))};
    }

    // 影 shadow のセル・面の欄が親と同じ数で、ブロックの欄が親の値か(細かくした直後)
    bool ShadowCopiesParent(const MultiresNest& nest, uint32_t shadow) {
        const MrBlock& block = nest.blocks[shadow];
        for (uint32_t f = 0; f < nest.fields.size(); ++f) {
            const SfFieldDesc& desc = nest.fields[f].desc;
            if (desc.place == SF_PLACE_BLOCK &&
                LoadNestBlockField(nest, f, shadow, 0) != LoadNestBlockField(nest, f, block.parent, 0))
                return false;

            if (!SfOnCells(desc))
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const uint32_t parentCell = MrParentCellOfChild(block.parentOctant, index);
                for (uint32_t c = 0; c < desc.components; ++c) {
                    if (LoadNestField(nest, f, shadow, index, c) != LoadNestField(nest, f, block.parent, parentCell, c))
                        return false;
                }
            }
        }

        return true;
    }

    // 影 shadow の欄が引き戻されているか: 親のセルごとに子 2³ の和 = 親 × 8・負にならない欄は 0 以上・ブロックの欄は親の値
    bool ShadowPulledBack(const MultiresNest& nest, uint32_t shadow) {
        const MrBlock& block = nest.blocks[shadow];
        for (uint32_t f = 0; f < nest.fields.size(); ++f) {
            const SfFieldDesc& desc = nest.fields[f].desc;
            if (desc.place == SF_PLACE_BLOCK &&
                LoadNestBlockField(nest, f, shadow, 0) != LoadNestBlockField(nest, f, block.parent, 0))
                return false;

            if (!SfOnCells(desc))
                continue;

            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                for (uint32_t c = 0; c < desc.components; ++c) {
                    int64_t sum = 0;
                    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                        const int64_t value = LoadNestField(nest, f, shadow, MrChildCell(local, j), c);
                        if (NonNegativeField(nest, f) && value < 0)
                            return false;

                        sum += value;
                    }

                    const uint32_t parentCell = MrOctantCell(block.parentOctant, local);
                    if (sum != 8 * LoadNestField(nest, f, block.parent, parentCell, c))
                        return false;
                }
            }
        }

        return true;
    }

    // 影の鎖の欄を揺らして(影が自分で動いた代わり)から引き戻し、世界が変わらず・影が親に従ったか
    void ShakeAndPullBack(MultiresNest& nest, const BakedReactionTable& table, uint32_t levels, uint64_t tick,
                          int& worldChanged, int& notPulled) {
        for (uint32_t i = 0; i < levels; ++i)
            FillAllPageFields(nest, test::MULTIRES_SHADOW_SLOT + i, 1000 + tick);

        const WorldView before = ViewWorld(nest, table);
        PullBackShadowChain(nest, table, test::MULTIRES_SHADOW_SLOT, levels);
        worldChanged += ViewWorld(nest, table) == before ? 0 : 1;
        for (uint32_t i = 0; i < levels; ++i)
            notPulled += ShadowPulledBack(nest, test::MULTIRES_SHADOW_SLOT + i) ? 0 : 1;
    }

    // 本物の鎖の場面を、覗く世界と覗かない世界で並べて進める: 影を細かくする/引き戻すの前後で世界の姿がビット単位で同じ・
    // 覗かない世界とも毎刻み同じ(見るだけなら世界は変わらない。D-403)
    void TestShadowFields(const BakedReactionTable& table) {
        MultiresNest watched = MakeObserverFieldNest(table);
        MultiresNest plain = MakeObserverFieldNest(table);
        // 影の点は本物の鎖と別の八分の一(根のセル (6, 1, 1)。覆われていない親から細かくする)
        const MultiresPoint point{.x = (6 << 9) | 100, .y = (1 << 9) | 7, .z = (1 << 9) | 300, .level = 9};
        int worldChanged = 0;
        int notCopied = 0;
        int notPulled = 0;
        int diverged = 0;
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            test::StepMultiresScene(plain, table, test::MultiresScenario::Real, tick);
            test::StepMultiresScene(watched, table, test::MultiresScenario::Real, tick);
            if (tick == OBSERVER_REFINE_TICK) {
                const WorldView before = ViewWorld(watched, table);
                RefineShadowChain(watched, test::MULTIRES_ROOT_SLOT, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS,
                                  point);
                worldChanged += ViewWorld(watched, table) == before ? 0 : 1;
                for (uint32_t i = 0; i < test::MULTIRES_LEVELS; ++i)
                    notCopied += ShadowCopiesParent(watched, test::MULTIRES_SHADOW_SLOT + i) ? 0 : 1;
            } else if (tick > OBSERVER_REFINE_TICK && tick < OBSERVER_REMOVE_TICK) {
                ShakeAndPullBack(watched, table, test::MULTIRES_LEVELS, tick, worldChanged, notPulled);
            } else if (tick == OBSERVER_REMOVE_TICK) {
                RemoveShadowChain(watched, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS);
            }

            diverged += ViewWorld(watched, table) == ViewWorld(plain, table) ? 0 : 1;
        }

        Expect(notCopied == 0, "影の欄: 細かくすると子は親と同じ数・ブロックの欄は親の値(親は覆わない)");
        Expect(notPulled == 0,
               "影の欄: 引き戻すと親のセルごとに子の和 = 親 × 8・負にならない欄は 0 以上・ブロックの欄は親の値");
        Expect(worldChanged == 0,
               "影の欄: 細かくする/引き戻すの前後で本物の葉の要約と保存量(帳簿込み)がビット単位で同じ");
        Expect(diverged == 0, "影の欄: 覗く世界と覗かない世界の本物の葉の要約・保存量が毎刻み同じ");
    }

    // 世界の写し: 置くと欄は 0 から・呼ぶ側が書いた欄に影が付いて行く・世界は変わらない
    void TestMirrorFields(const BakedReactionTable& table) {
        constexpr uint32_t LEVELS = 3;
        constexpr int64_t MIRROR_X = 8;  // 根の隣の 0.5 m のブロック(写す元は木の形をしていない世界)
        MultiresNest nest = MakeObserverFieldNest(table);
        const std::vector<RxCell> cells = test::MakeMultiresRootCells(table);
        const WorldView before = ViewWorld(nest, table);

        // --- 置き直すと前の値を残さない ---
        PlaceMirrorBlock(nest, OBSERVER_MIRROR_SLOT, 0, MIRROR_X, 0, 0, cells);
        FillAllPageFields(nest, OBSERVER_MIRROR_SLOT, 7);
        PlaceMirrorBlock(nest, OBSERVER_MIRROR_SLOT, 0, MIRROR_X, 0, 0, cells);
        bool cleared = true;
        for (uint32_t f = 0; f < nest.fields.size(); ++f) {
            const SfFieldDesc& desc = nest.fields[f].desc;
            if (desc.place == SF_PLACE_BLOCK)
                cleared = cleared && LoadNestBlockField(nest, f, OBSERVER_MIRROR_SLOT, 0) == 0;

            for (uint32_t index = 0; SfOnCells(desc) && index < MR_BLOCK_CELLS; ++index) {
                for (uint32_t c = 0; c < desc.components; ++c)
                    cleared = cleared && LoadNestField(nest, f, OBSERVER_MIRROR_SLOT, index, c) == 0;
            }
        }

        Expect(cleared, "写しの欄: 置くと 0 から(前の写しの値を残さない)");

        // --- 写す元の欄を書き、影を細かくして引き戻す ---
        FillAllPageFields(nest, OBSERVER_MIRROR_SLOT, 9);
        const MultiresPoint point{.x = ((MIRROR_X + 2) << 9) | 5, .y = (5 << 9) | 60, .z = (2 << 9) | 9, .level = 9};
        RefineShadowChain(nest, OBSERVER_MIRROR_SLOT, test::MULTIRES_SHADOW_SLOT, LEVELS, point);
        bool copied = true;
        for (uint32_t i = 0; i < LEVELS; ++i)
            copied = copied && ShadowCopiesParent(nest, test::MULTIRES_SHADOW_SLOT + i);

        Expect(copied, "写しの欄: 写しから細かくした影は写しと同じ数");
        int worldChanged = 0;
        int notPulled = 0;
        ShakeAndPullBack(nest, table, LEVELS, 0, worldChanged, notPulled);
        Expect(notPulled == 0, "写しの欄: 引き戻すと影は写しに従う(子の和 = 親 × 8)");
        Expect(worldChanged == 0 && ViewWorld(nest, table) == before,
               "写しの欄: 写しを置く・影を細かくする/引き戻すで世界の姿がビット単位で同じ");
    }

    // 引き戻しの 1 つの規則(state_field.hlsli)の境目: 負の値がある組は等しく足す・負にならない組は比で配る
    void TestPullBackRule() {
        const auto pull = [](int64_t parent, const SfFamily& family) {
            SfFamily result{};
            int64_t sum = 0;
            for (uint32_t j = 0; j < SF_CHILDREN; ++j) {
                result.values[j] = SfPullBackChild(parent, family, j);
                sum += result.values[j];
            }

            return std::pair{result, sum};
        };

        // 負の値がある: 足りない分 8 × 3 − 1 = 23 = 8 × 2 + 7 → 2 ずつ + 先頭の 7 つに 1
        const SfFamily signedFamily{{-10, 5, -3, 0, 7, -1, 2, 1}};
        const auto [shifted, shiftedSum] = pull(3, signedFamily);
        Expect(shiftedSum == 24 && shifted.values[0] == -7 && shifted.values[1] == 8 && shifted.values[2] == 0 &&
                   shifted.values[7] == 3,
               "引き戻しの規則: 負の値がある組は等しく足す(影の中の違いはそのまま)");

        // 負にならない: 親 × 8 = 80 を 1:0:3:0:0:0:0:0 で → 20・0・60
        const SfFamily massFamily{{1, 0, 3, 0, 0, 0, 0, 0}};
        const auto [shared, sharedSum] = pull(10, massFamily);
        Expect(sharedSum == 80 && shared.values[0] == 20 && shared.values[1] == 0 && shared.values[2] == 60,
               "引き戻しの規則: 負にならない組は比で配る(0 の子は 0 のまま)");

        // 負にならない・全部 0: 親と同じ数
        const auto [even, evenSum] = pull(6, SfFamily{});
        Expect(evenSum == 48 && even.values[0] == 6 && even.values[7] == 6, "引き戻しの規則: 全部 0 の子は親と同じ数");
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "試験の表をベイクできない: {}", table.error());
            return 1;
        }

        TestLegacyHashes(*table);
        TestFieldsFollowTree(*table);
        TestCoarsenRemainder(*table);
        TestUniformFields(*table);
        TestSaveRestore(*table, false);
        TestSaveRestore(*table, true);
        TestShadowFields(*table);
        TestMirrorFields(*table);
        TestPullBackRule();

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_state_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_state_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
