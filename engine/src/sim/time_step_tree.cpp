// time_step_tree.cpp — 研究 R-T2 の 2 の CPU の試作(time_step_tree.h の先頭を見よ)。式は shaders/common/time_step.hlsli と
// C-0 の部品(SolveEquilibrium)。出来事(歩の終わり)の刻みだけを回す。浮動小数点は使わない。
//
// 解きは差の形: 未知数は歩の初めの温度 x0 からの差 δ(x = x0 + δ)。後ろ向きの Euler の式
//   (C + N Σg + N g地面) δ_i − N Σ g δ_j = N Σ g (x0_j − x0_i) + N g地面 (T地面 − x0_i) + 反応の熱
// を部品の口(節 C・面 N g・値 0 を固定した面・右辺の項)で渡す。釣り合っている所は右辺が整数で 0 になり、δ も面の流れもちょうど 0
// (眠った所は変わらない。x そのものを解くと、丸めの揺れが長い歩の大きい係数に掛かって静かな所に流れが出た)。
#include "sim/time_step_tree.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <initializer_list>
#include <span>
#include <utility>

#include "common/equilibrium.hlsli"
#include "common/multires_activity.hlsli"
#include "common/multires_conduction.hlsli"
#include "common/time_step.hlsli"
#include "sim/equilibrium.h"
#include "sim/multires_fields.h"
#include "sim/multires_state.h"

namespace bicameral::sim {

    using namespace bicameral::fx;
    using namespace bicameral::multires;
    using namespace bicameral::state_field;
    using namespace bicameral::time_step;

    namespace {

        constexpr uint32_t NO_NODE = UINT32_MAX;
        constexpr uint32_t FACE_ROUNDING_PURPOSE = 0x52543246;
        constexpr uint32_t REACTION_ROUNDING_PURPOSE = 0x52543254;

        // --- 番号と小さな道具 ---

        uint32_t CellId(uint32_t slot, uint32_t index) {
            return slot * MR_BLOCK_CELLS + index;
        }

        uint32_t SlotOf(uint32_t id) {
            return id / MR_BLOCK_CELLS;
        }

        uint32_t IndexOf(uint32_t id) {
            return id % MR_BLOCK_CELLS;
        }

        uint64_t Ticks(int32_t exponent) {
            return 1ull << static_cast<uint32_t>(exponent);
        }

        uint64_t NonNegative(int64_t value) {
            return value > 0 ? static_cast<uint64_t>(value) : 0;
        }

        // 木の索引の約束(multires_activity.hlsli の Tree)
        struct Tree {
            const MultiresNest* nest = nullptr;

            [[nodiscard]] MrBlock Block(uint32_t slot) const { return nest->blocks[slot]; }

            [[nodiscard]] uint32_t Lookup(int32_t level, int64_t originX, int64_t originY, int64_t originZ) const {
                return LookupBlock(*nest, level, originX, originY, originZ);
            }
        };

        // レベル level の面の係数(レベル 0 の値 × 4^level。粗いレベルは ÷ 4^−level)
        FxU128 FaceCoefficient(const TimeTreeSetup& setup, int32_t level) {
            if (level >= 0)
                return ImWideShiftLeft(setup.conductLevel0Q32, 2u * static_cast<uint32_t>(level));

            return ImWideShiftRight(setup.conductLevel0Q32, 2u * static_cast<uint32_t>(-level));
        }

        // 最も細かいレベルの単位へ(× 8^(最も細かい − level))
        int64_t ToFinest(const TimeTreeLayout& layout, int32_t level, int64_t value) {
            return value << (MR_LEVEL_SHIFT * static_cast<uint32_t>(layout.finestLevel - level));
        }

        // A の量(成分 TIME_TREE_REACTANT・TIME_TREE_PRODUCT)
        int64_t AmountOf(const RxCell& cell, uint32_t species) {
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                if (cell.species[i] == species)
                    return static_cast<int64_t>(cell.amounts[i]);
            }

            return 0;
        }

        RxCell WithAmounts(const RxCell& cell, int64_t energy, int64_t reactant, int64_t product) {
            RxCell result = RxMakeEmptyCell(energy);
            result.padding = cell.padding;
            if (reactant > 0) {
                result.species[result.speciesCount] = TIME_TREE_REACTANT;
                result.amounts[result.speciesCount++] = static_cast<uint64_t>(reactant);
            }

            if (product > 0) {
                result.species[result.speciesCount] = TIME_TREE_PRODUCT;
                result.amounts[result.speciesCount++] = static_cast<uint64_t>(product);
            }

            return result;
        }

        int64_t ReactionRate(const TimeTreeSetup& setup, int64_t temperatureQ16) {
            if (setup.fuel == TIME_STEP_FUEL_SLOW)
                return static_cast<int64_t>(setup.rateColdQ32);

            if (setup.fuel == TIME_STEP_FUEL_BURNS && temperatureQ16 >= setup.ignitionTemperatureQ16)
                return static_cast<int64_t>(setup.rateHotQ32);

            return 0;
        }

        // 歩を始めた刻み start に、セル id が反応する量(閉じた形・決まった乱数で丸める)
        int64_t Reacted(const TimeTreeSetup& setup, const RxCell& cell, uint32_t id, uint64_t start, uint64_t ticks) {
            const int64_t reactant = AmountOf(cell, TIME_TREE_REACTANT);
            const int64_t temperature = TimeTreeTemperatureQ16(setup, cell.energy);
            const uint64_t random = FxHash64(0, start, id, REACTION_ROUNDING_PURPOSE);

            return TsDecayedPart(reactant, static_cast<uint64_t>(ReactionRate(setup, temperature)), ticks, random);
        }

        // 粗い側へ送る値: 面の流れ(細かい側の単位)÷ 8^差 を決まった乱数で丸める(期待値は丸める前と同じ。D-552)
        int64_t MovedToCoarse(int64_t flow, uint32_t gap, uint64_t random) {
            if (gap == 0)
                return flow;

            const uint32_t shift = MR_LEVEL_SHIFT * gap;
            const uint64_t magnitude = FxAbsU64(flow);
            const uint64_t remainder = magnitude & ((1ull << shift) - 1);
            const uint64_t up = (random >> (64 - shift)) < remainder ? 1 : 0;

            return FxApplySign((magnitude >> shift) + up, flow < 0);
        }

    }  // namespace

    namespace {

        // --- 形(木の隣から面を 1 回だけ作る)---

        void AddCellFaces(const MultiresNest& nest, const TimeTreeSetup& setup, TimeTreeLayout& layout, uint32_t slot,
                          uint32_t index) {
            const Tree tree{.nest = &nest};
            const MrBlock block = nest.blocks[slot];
            const uint32_t id = CellId(slot, index);
            for (uint32_t face = 0; face < MR_FACES; ++face) {
                const MrFaceNeighbor neighbor = MrFindFaceNeighbor(tree, slot, block, index, face,
                                                                   nest.capacity.rootLevel);
                if (neighbor.kind != MR_NEIGHBOR_SAME && neighbor.kind != MR_NEIGHBOR_COARSER)
                    continue;

                const uint32_t other = CellId(neighbor.slot, neighbor.index);
                if (neighbor.kind == MR_NEIGHBOR_SAME && other < id)
                    continue;

                const uint32_t gap = neighbor.kind == MR_NEIGHBOR_SAME ? 0 : neighbor.gap;
                layout.faces.push_back({.a = id,
                                        .b = other,
                                        .blockA = slot,
                                        .blockB = neighbor.slot,
                                        .gap = gap,
                                        .coefficient = FaceCoefficient(setup, block.level)});
            }
        }

        int64_t FloorDiv(int64_t value, int64_t divisor) {
            return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
        }

        // 揃える段(レベルごと): groupShift と、単位の物の長さ(D-640)に届くまでの段の大きい方
        uint32_t GroupShiftAt(const TimeTreeSetup& setup, int32_t level) {
            const int32_t byLength = setup.unitByLength ? std::max(level - setup.unitLevel, 0) : 0;

            return std::max(setup.groupShift, static_cast<uint32_t>(byLength));
        }

        // 同じレベルで 2^揃える段 ブロック角の枠どうしを、同じ e に揃える相手に(細かさを同じ物の長さで比べる。T-0985・D-640)
        void AddMates(const MultiresNest& nest, const TimeTreeSetup& setup, TimeTreeLayout& layout) {
            const auto group = [&](uint32_t slot) {
                const MrBlock& block = nest.blocks[slot];
                const int64_t edge = int64_t{MR_BLOCK_EDGE} << GroupShiftAt(setup, block.level);
                return std::array{int64_t{block.level}, FloorDiv(block.originX, edge), FloorDiv(block.originY, edge),
                                  FloorDiv(block.originZ, edge)};
            };
            for (const uint32_t slot : layout.blocks) {
                for (const uint32_t other : layout.blocks) {
                    if (other != slot && group(other) == group(slot))
                        layout.mates[slot].push_back(other);
                }
            }
        }

        TimeTreeLayout MakeLayout(const MultiresNest& nest, const TimeTreeSetup& setup) {
            TimeTreeLayout layout;
            const uint32_t slots = nest.capacity.worldBlocks;
            layout.stepped.assign(size_t{slots} * MR_BLOCK_CELLS, 0);
            layout.blockFaces.resize(slots);
            layout.neighbors.resize(slots);
            layout.mates.resize(slots);
            layout.finestLevel = nest.capacity.rootLevel;
            for (uint32_t slot = 0; slot < slots; ++slot) {
                const MrBlock& block = nest.blocks[slot];
                if (block.kind != MR_BLOCK_REAL)
                    continue;

                layout.blocks.push_back(slot);
                layout.finestLevel = std::max(layout.finestLevel, block.level);
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    if (MrIsCoveredCell(block, index))
                        continue;

                    layout.stepped[CellId(slot, index)] = 1;
                    AddCellFaces(nest, setup, layout, slot, index);
                }
            }

            for (uint32_t f = 0; f < layout.faces.size(); ++f) {
                const TimeTreeFace& face = layout.faces[f];
                layout.blockFaces[face.blockA].push_back(f);
                if (face.blockB == face.blockA)
                    continue;

                layout.blockFaces[face.blockB].push_back(f);
                for (const auto& [from, to] :
                     {std::pair{face.blockA, face.blockB}, std::pair{face.blockB, face.blockA}}) {
                    std::vector<uint32_t>& list = layout.neighbors[from];
                    if (std::ranges::find(list, to) == list.end())
                        list.push_back(to);
                }
            }

            for (std::vector<uint32_t>& list : layout.neighbors)
                std::ranges::sort(list);

            AddMates(nest, setup, layout);

            return layout;
        }

    }  // namespace

    namespace {

        // --- 記録を両側の歩の変化と帳簿へ ---

        int64_t LevelOf(const TimeTreeWorld& world, uint32_t id) {
            return world.nest.blocks[SlotOf(id)].level;
        }

        void ApplyFaceRecord(TimeTreeWorld& world, uint32_t f, const TimeTreeFaceRecord& record, int64_t sign) {
            const TimeTreeFace& face = world.layout.faces[f];
            const int64_t lost = record.moved << (MR_LEVEL_SHIFT * face.gap);
            world.steps.deltaEnergy[face.a] -= sign * lost;
            world.steps.deltaEnergy[face.b] += sign * record.moved;
        }

        void ApplyCellRecord(const TimeTreeSetup& setup, TimeTreeWorld& world, uint32_t id,
                             const TimeTreeCellRecord& record, int64_t sign) {
            TimeTreeSteps& steps = world.steps;
            const auto level = static_cast<int32_t>(LevelOf(world, id));
            const int64_t heat = record.reacted * setup.heatPerAmount;
            steps.deltaEnergy[id] += sign * (heat - record.ground);
            steps.deltaReactant[id] -= sign * record.reacted;
            steps.outsideHeat -= sign * ToFinest(world.layout, level, record.ground);
            steps.groundHeat += sign * ToFinest(world.layout, level, record.ground);
            steps.reactionHeat += sign * ToFinest(world.layout, level, heat);
        }

        bool Crosses(uint64_t start, uint64_t ticks, uint64_t tick) {
            return start <= tick && tick < start + ticks;
        }

        // 面の記録を刻み tick で切る(残りの分を両側の歩の変化から引く。保存はそのまま)
        void CutFaceRecord(TimeTreeWorld& world, uint32_t f, uint64_t tick) {
            TimeTreeFaceRecord& record = world.steps.faceRecords[f];
            if (!Crosses(record.start, record.ticks, tick))
                return;

            const uint64_t kept = tick - record.start;
            TimeTreeFaceRecord scaled = record;
            scaled.ticks = kept;
            scaled.moved = FxMulDivS64(record.moved, kept, record.ticks);
            ApplyFaceRecord(world, f, record, -1);
            ApplyFaceRecord(world, f, scaled, 1);
            record = scaled;
        }

    }  // namespace

    namespace {

        // --- 歩を閉じる(歩の変化を真実へ。眠る・起こす)---

        // 枠 slot の頁を眠らせる(C-1 の FoldQuietPages を、この枠だけが「ちょうど静かになった」印で呼ぶ。導かれる値は覆われていない
        // セルの平均が残る)。試作の仮: 活性の印 busyTick は試作が刻む木では使わないので、畳む判定のためだけに書く
        void SleepBlock(TimeTreeWorld& world, uint32_t slot) {
            MultiresNest& nest = world.nest;
            const uint64_t tick = world.steps.tick;
            if (tick <= MR_QUIET_TICKS || MrIsUniform(nest.blocks[slot]))
                return;

            SfMeanSum sum = SfMakeMeanSum();
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (world.layout.stepped[CellId(slot, index)] != 0)
                    sum = SfAddMeanValue(sum, LoadNestField(nest, world.temperatureField, slot, index, 0));
            }

            const uint64_t mark = MrChangeMark(tick);
            for (const uint32_t other : world.layout.blocks)
                nest.blocks[other].busyTick = other == slot ? mark - (MR_QUIET_TICKS + 1) : mark;

            FoldQuietPages(nest, tick);
            if (!MrIsUniform(nest.blocks[slot]))
                return;

            ++world.steps.sleeps;
            world.steps.sleepMean[slot] = SfSleepValue(SfMeanValue(sum));
        }

        // 枠 slot の歩の変化を真実へ。変化(精度の単位 × 2^16 の最大)を返す
        uint64_t SettleBlock(const TimeTreeSetup& setup, TimeTreeWorld& world, uint32_t slot) {
            MultiresNest& nest = world.nest;
            TimeTreeSteps& steps = world.steps;
            bool any = false;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const uint32_t id = CellId(slot, index);
                any = any || steps.deltaEnergy[id] != 0 || steps.deltaReactant[id] != 0;
            }

            if (!any) {
                SleepBlock(world, slot);
                return 0;
            }

            // --- 眠っていれば起こす(導かれる値は眠った時の平均を配る = C-1。次の歩の初めに解き直す)---
            if (MrIsUniform(nest.blocks[slot])) {
                [[maybe_unused]] const bool expanded = ExpandNestPage(nest, slot);
                FX_ASSERT(expanded);
                ++steps.wakes;
            }

            uint64_t change = 0;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const uint32_t id = CellId(slot, index);
                if (world.layout.stepped[id] == 0)
                    continue;

                int64_t& deltaEnergy = steps.deltaEnergy[id];
                int64_t& deltaReactant = steps.deltaReactant[id];
                const uint64_t magnitude = FxAbsU64(deltaEnergy);
                const FxU128 numerator = {magnitude >> (64 - 48), magnitude << 48};
                change = std::max(change, FxDivU128By64(numerator, setup.capacityQ32).quotient);
                if (deltaEnergy == 0 && deltaReactant == 0)
                    continue;

                const RxCell cell = LoadNestCell(nest, slot, index);
                const int64_t energy = cell.energy + deltaEnergy;
                const int64_t reactant = AmountOf(cell, TIME_TREE_REACTANT) + deltaReactant;
                const int64_t product = AmountOf(cell, TIME_TREE_PRODUCT) - deltaReactant;
                steps.negativeValues += (energy < 0 || reactant < 0 || product < 0) ? 1u : 0u;
                StoreNestCell(nest, slot, index, WithAmounts(cell, energy, reactant, product));
                deltaEnergy = 0;
                deltaReactant = 0;
            }

            return change;
        }

        void CloseBlock(const TimeTreeSetup& setup, TimeTreeWorld& world, uint32_t slot, bool measured) {
            TimeTreeSteps& steps = world.steps;
            const uint64_t change = SettleBlock(setup, world, slot);
            steps.open[slot] = 0;
            if (!measured)
                return;

            if (setup.rule == TimeStepRule::Fixed)
                steps.desired[slot] = setup.fixedExponent;
            else if (setup.select == TimeTreeSelect::Jump)
                return;  // 退く道: 決めるのは上限の倍数の刻みの試しの解きだけ(1 刻みの変化は丸めの揺れで測れない)
            else
                steps.desired[slot] = TsDesiredExponent(TS_RULE_CHANGE, steps.exponent[slot], change, setup.budgetQ16);
        }

        // 歩を刻み tick で打ち切る(記録を経った刻みの割合に縮めてから閉じる。保存はそのまま)
        void TruncateBlock(const TimeTreeSetup& setup, TimeTreeWorld& world, uint32_t slot, uint64_t tick) {
            TimeTreeSteps& steps = world.steps;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const uint32_t id = CellId(slot, index);
                TimeTreeCellRecord& record = steps.cellRecords[id];
                if (world.layout.stepped[id] == 0 || !Crosses(record.start, record.ticks, tick))
                    continue;

                const uint64_t kept = tick - record.start;
                TimeTreeCellRecord scaled = record;
                scaled.ticks = kept;
                scaled.ground = FxMulDivS64(record.ground, kept, record.ticks);
                // 反応は閉じた形で経った刻みの分を出し直す(真実はまだ歩の初めの値)
                const RxCell cell = LoadNestCell(world.nest, slot, index);
                scaled.reacted = kept == 0 ? 0 : Reacted(setup, cell, id, record.start, kept);
                ApplyCellRecord(setup, world, id, record, -1);
                ApplyCellRecord(setup, world, id, scaled, 1);
                record = scaled;
            }

            for (const uint32_t f : world.layout.blockFaces[slot])
                CutFaceRecord(world, f, tick);

            CloseBlock(setup, world, slot, false);
            steps.desired[slot] = steps.touched[slot] != 0 ? 0 : steps.exponent[slot];
            ++steps.truncations;
        }

    }  // namespace

    namespace {

        // --- 列(同じ刻みに同じ e で始まる枠の集まり)の歩を始める: 反応・解き・記録 ---

        struct Span {
            int32_t exponent = 0;
            uint64_t start = 0;
            std::vector<uint32_t> slots;
        };

        struct SpanSystem {
            EquilibriumSystem system;
            std::vector<uint32_t> cells;       // 節 → セルの番号
            std::vector<int64_t> start;        // 節ごとの x0(mK × 2^16)
            std::vector<int64_t> reacted;      // 節ごと
            std::vector<uint32_t> ownedFaces;  // この列が記録する面
        };

        FxU128 TimesTicks(FxU128 coefficient, int32_t exponent) {
            return ImWideShiftLeft(coefficient, static_cast<uint32_t>(exponent));
        }

        FxU128 CoarseSide(FxU128 coefficient, uint32_t gap) {
            return ImWideShiftRight(coefficient, MR_LEVEL_SHIFT * gap);
        }

        int64_t CellTemperature(const TimeTreeSetup& setup, const TimeTreeWorld& world, uint32_t id) {
            return TimeTreeTemperatureQ16(setup, LoadNestCell(world.nest, SlotOf(id), IndexOf(id)).energy);
        }

        // 節・地面・反応の熱を系へ
        void AddNodes(const TimeTreeSetup& setup, const TimeTreeWorld& world, const Span& span, SpanSystem& built,
                      std::vector<uint32_t>& nodeOf) {
            const uint64_t ticks = Ticks(span.exponent);
            const FxU128 ground = TimesTicks(ImWide(setup.groundQ32), span.exponent);
            const FxU128 heatFactor = {0, 1ull << 48};
            for (const uint32_t slot : span.slots) {
                const MrBlock& block = world.nest.blocks[slot];
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const uint32_t id = CellId(slot, index);
                    if (world.layout.stepped[id] == 0)
                        continue;

                    const auto node = static_cast<uint32_t>(built.cells.size());
                    const RxCell cell = LoadNestCell(world.nest, slot, index);
                    const int64_t temperature = TimeTreeTemperatureQ16(setup, cell.energy);
                    const int64_t reacted = Reacted(setup, cell, id, span.start, ticks);
                    nodeOf[id] = node;
                    built.cells.push_back(id);
                    built.start.push_back(temperature);
                    built.reacted.push_back(reacted);
                    built.system.nodes.push_back({.level = block.level,
                                                  .x = block.originX + MrCellX(index),
                                                  .y = block.originY + MrCellY(index),
                                                  .z = block.originZ + MrCellZ(index),
                                                  .capacity = setup.capacityQ32});
                    built.system.fixedFaces.push_back({.node = node, .coefficient = ground, .value = 0});
                    built.system.sources.push_back(
                        {.node = node, .value = setup.groundTemperatureQ16 - temperature, .factor = ground});
                    if (reacted != 0)
                        built.system.sources.push_back(
                            {.node = node, .value = reacted * setup.heatPerAmount, .factor = heatFactor});
                }
            }
        }

        // 面: 両側が列の中なら面、片側なら値 0 を固定した面。どちらも歩の初めの差を右辺の項へ。
        // 列の外の隣が歩の途中か長い歩を始めるなら、この列(短い側)が記録する
        void AddFaces(const TimeTreeSetup& setup, const TimeTreeWorld& world, const Span& span,
                      std::span<const int32_t> next, const std::vector<uint32_t>& nodeOf, SpanSystem& built) {
            std::vector<uint8_t> seen(world.layout.faces.size(), 0);
            for (const uint32_t slot : span.slots) {
                for (const uint32_t f : world.layout.blockFaces[slot]) {
                    if (seen[f] != 0)
                        continue;

                    seen[f] = 1;
                    const TimeTreeFace& face = world.layout.faces[f];
                    const uint32_t nodeA = nodeOf[face.a];
                    const uint32_t nodeB = nodeOf[face.b];
                    const FxU128 coefficient = TimesTicks(face.coefficient, span.exponent);
                    const FxU128 coarse = CoarseSide(coefficient, face.gap);
                    const int64_t startA = nodeA != NO_NODE ? built.start[nodeA]
                                                            : CellTemperature(setup, world, face.a);
                    const int64_t startB = nodeB != NO_NODE ? built.start[nodeB]
                                                            : CellTemperature(setup, world, face.b);
                    if (nodeA != NO_NODE)
                        built.system.sources.push_back(
                            {.node = nodeA, .value = startB - startA, .factor = coefficient});

                    if (nodeB != NO_NODE)
                        built.system.sources.push_back({.node = nodeB, .value = startA - startB, .factor = coarse});

                    if (nodeA != NO_NODE && nodeB != NO_NODE) {
                        built.system.faces.push_back(
                            {.fine = nodeA, .coarse = nodeB, .gap = face.gap, .coefficient = coefficient});
                        built.ownedFaces.push_back(f);
                        continue;
                    }

                    if (nodeA != NO_NODE)
                        built.system.fixedFaces.push_back({.node = nodeA, .coefficient = coefficient, .value = 0});
                    else
                        built.system.fixedFaces.push_back({.node = nodeB, .coefficient = coarse, .value = 0});

                    const uint32_t outside = nodeA != NO_NODE ? face.blockB : face.blockA;
                    if (next[outside] < 0 || next[outside] > span.exponent)
                        built.ownedFaces.push_back(f);
                }
            }
        }

        // 解いた列(真実と歩の記録へ書く前。2 通り目はここで歩の変化を見て取り直す)。解きは真実と next だけを読むので、
        // 全部の列を解いてから書いても、1 列ずつ解いて書いても同じ
        struct SolvedSpan {
            Span span;
            SpanSystem built;
            EquilibriumState state;
            std::vector<uint32_t> nodeOf;
        };

        // 段の重みを使い回せるかの測り(T-1090。結果に効かない): 枠の組が変わったか・温度の帯が変わったセルがあるか
        void MeasureReweights(const TimeTreeSetup& setup, TimeTreeWorld& world, const Span& span) {
            constexpr std::array<int64_t, 3> BAND_Q16 = {int64_t{1000} << 16, int64_t{10000} << 16,
                                                         int64_t{100000} << 16};
            TimeTreeSteps& steps = world.steps;
            ++steps.zeroSolves;
            const bool reshaped = span.slots != steps.lastZeroSlots;
            steps.zeroReshaped += reshaped ? 1u : 0u;
            steps.lastZeroSlots = span.slots;
            for (size_t b = 0; b < BAND_Q16.size(); ++b) {
                std::vector<int64_t>& bands = steps.cellBand[b];
                if (bands.size() != world.layout.stepped.size())
                    bands.assign(world.layout.stepped.size(), INT64_MIN);

                bool changed = false;
                for (const uint32_t slot : span.slots) {
                    for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                        const uint32_t id = CellId(slot, index);
                        if (world.layout.stepped[id] == 0)
                            continue;

                        const int64_t band = FloorDiv(CellTemperature(setup, world, id), BAND_Q16[b]);
                        changed = changed || bands[id] != band;
                        bands[id] = band;
                    }
                }

                steps.zeroRebanded[b] += !reshaped && changed ? 1u : 0u;
            }
        }

        SolvedSpan SolveSpan(const TimeTreeSetup& setup, TimeTreeWorld& world, const Span& span,
                             std::span<const int32_t> next) {
            TimeTreeSteps& steps = world.steps;
            SolvedSpan solved{.span = span};
            solved.built.system.rounding = EQ_ROUNDING_WEIGHT;
            solved.nodeOf.assign(world.layout.stepped.size(), NO_NODE);
            AddNodes(setup, world, span, solved.built, solved.nodeOf);
            AddFaces(setup, world, span, next, solved.nodeOf, solved.built);

            // --- 解く(差の形。初めは δ = 0)---
            EquilibriumOptions options;
            const uint32_t cycles = span.exponent <= setup.shortExponent && setup.cyclesAtZero != 0 ? setup.cyclesAtZero
                                                                                                    : setup.cycles;
            options.solver.cycles = setup.residualStop == 0 ? cycles : 1;
            options.faceValues = false;
            options.galerkinCoarse = setup.galerkin;
            if (setup.spanObserver != nullptr && setup.residualStop == 0) {
                (*setup.spanObserver)(
                    {.system = solved.built.system, .exponent = span.exponent, .start = span.start, .cycles = cycles});
            }

            // 残差で止める形(T-1108): V を 1 回ずつ、残差の計器の最大が閾値以下になるか上限 cycles 回まで(整数の判定 = 決定的)
            EquilibriumResult result = SolveEquilibrium(solved.built.system, options, solved.state);
            uint32_t done = options.solver.cycles;
            while (setup.residualStop != 0 && done < cycles && result.worstResidual > setup.residualStop) {
                result = SolveEquilibrium(solved.built.system, options, solved.state);
                ++done;
            }

            steps.vCycles += done;
            steps.worstResidual = std::max(steps.worstResidual, result.worstResidual);
            steps.cellSteps += solved.built.cells.size();
            ++steps.solves;
            if (span.exponent == 0) {
                MeasureReweights(setup, world, span);
                steps.zeroCellSteps += solved.built.cells.size();
                for (const uint32_t slot : span.slots) {
                    if (steps.desired[slot] == 0)
                        continue;

                    for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                        steps.forcedCellSteps += world.layout.stepped[CellId(slot, index)];
                }
            }

            return solved;
        }

        void CommitSpan(const TimeTreeSetup& setup, TimeTreeWorld& world, const SolvedSpan& solved) {
            TimeTreeSteps& steps = world.steps;
            const Span& span = solved.span;
            const SpanSystem& built = solved.built;
            const std::vector<uint32_t>& nodeOf = solved.nodeOf;
            const std::vector<int64_t>& values = solved.state.values;
            const uint64_t ticks = Ticks(span.exponent);
            const auto end = [&](uint32_t id) {
                const uint32_t node = nodeOf[id];
                return node != NO_NODE ? built.start[node] + values[node] : CellTemperature(setup, world, id);
            };

            // --- セルの記録・導かれる値(解いた温度。頁を持つ枠だけ。眠った所は眠った時の平均のまま)---
            const FxU128 ground = TimesTicks(ImWide(setup.groundQ32), span.exponent);
            for (uint32_t node = 0; node < built.cells.size(); ++node) {
                const uint32_t id = built.cells[node];
                const int64_t temperature = built.start[node] + values[node];
                TimeTreeCellRecord record{.start = span.start, .ticks = ticks, .reacted = built.reacted[node]};
                record.ground = EqFaceValue(ground, temperature, setup.groundTemperatureQ16);
                steps.cellRecords[id] = record;
                ApplyCellRecord(setup, world, id, record, 1);
                if (!MrIsUniform(world.nest.blocks[SlotOf(id)]))
                    StoreNestField(world.nest, world.temperatureField, SlotOf(id), IndexOf(id), 0, temperature);
            }

            // --- 面の記録(前の持ち主の記録はここから先をこの列が持つので切る)---
            for (const uint32_t f : built.ownedFaces) {
                const TimeTreeFace& face = world.layout.faces[f];
                CutFaceRecord(world, f, span.start);
                const int64_t flow = EqFaceValue(TimesTicks(face.coefficient, span.exponent), end(face.a), end(face.b));
                const uint64_t random = FxHash64(0, span.start, f, FACE_ROUNDING_PURPOSE);
                const TimeTreeFaceRecord record{
                    .start = span.start, .ticks = ticks, .moved = MovedToCoarse(flow, face.gap, random)};
                steps.faceRecords[f] = record;
                ApplyFaceRecord(world, f, record, 1);
            }

            // --- 枠の歩 ---
            for (const uint32_t slot : span.slots) {
                steps.exponent[slot] = span.exponent;
                steps.start[slot] = span.start;
                steps.end[slot] = span.start + ticks;
                steps.open[slot] = 1;
            }
        }

    }  // namespace

    namespace {

        // --- 出来事(刻み tick): 終えた歩を閉じる → e を選ぶ(隣の差 1 段・長すぎる隣は打ち切る)→ 列ごとに歩を始める ---

        struct Choice {
            std::vector<int32_t> next;  // -1 = 歩の途中(か刻まない枠)
            uint64_t tick = 0;
            int32_t cap = 0;
        };

        int32_t Current(const TimeTreeSteps& steps, const Choice& choice, uint32_t slot) {
            return choice.next[slot] >= 0 ? choice.next[slot] : steps.exponent[slot];
        }

        // slot の e を other の e + allowed までに(長すぎれば、始める側なら縮め、歩の途中なら打ち切って始め直す)。変えたら true
        // 歩を終えた枠の次の e(退く道は 0 か上限だけ。上限で始められない刻みなら毎刻み)
        int32_t FirstNext(const TimeTreeSetup& setup, const TimeTreeSteps& steps, const Choice& choice, uint32_t slot) {
            if (setup.select != TimeTreeSelect::Jump)
                return std::clamp(steps.desired[slot], 0, choice.cap);

            const bool jump = steps.desired[slot] >= setup.maxExponent && choice.cap >= setup.maxExponent;

            return jump ? setup.maxExponent : 0;
        }

        bool LimitTo(const TimeTreeSetup& setup, TimeTreeWorld& world, Choice& choice, uint32_t slot, uint32_t other,
                     int32_t allowed) {
            TimeTreeSteps& steps = world.steps;
            if (Current(steps, choice, slot) <= Current(steps, choice, other) + allowed)
                return false;

            if (choice.next[slot] < 0) {
                TruncateBlock(setup, world, slot, choice.tick);
                choice.next[slot] = FirstNext(setup, steps, choice, slot);
            }

            choice.next[slot] = std::min(choice.next[slot], Current(steps, choice, other) + allowed);

            return true;
        }

        // 前線の緩衝を単位で(T-1074): 動いている単位から面の隣をたどって frontBufferUnits 単位までの枠に印
        std::vector<uint8_t> BufferedBlocks(const TimeTreeSetup& setup, const TimeTreeWorld& world) {
            const TimeTreeSteps& steps = world.steps;
            std::vector<uint32_t> distance(world.nest.blocks.size(), UINT32_MAX);
            std::vector<uint32_t> frontier;
            const auto reach = [&](uint32_t slot, uint32_t d, std::vector<uint32_t>& into) {
                if (distance[slot] <= d)
                    return;

                distance[slot] = d;
                into.push_back(slot);
                for (const uint32_t mate : world.layout.mates[slot]) {
                    if (distance[mate] > d) {
                        distance[mate] = d;
                        into.push_back(mate);
                    }
                }
            };
            for (const uint32_t slot : world.layout.blocks) {
                if (steps.desired[slot] == 0)
                    reach(slot, 0, frontier);
            }

            for (uint32_t d = 1; d <= setup.frontBufferUnits && !frontier.empty(); ++d) {
                std::vector<uint32_t> next;
                for (const uint32_t slot : frontier) {
                    for (const uint32_t neighbor : world.layout.neighbors[slot])
                        reach(neighbor, d, next);
                }

                frontier = std::move(next);
            }

            std::vector<uint8_t> buffered(world.nest.blocks.size(), 0);
            for (const uint32_t slot : world.layout.blocks)
                buffered[slot] = distance[slot] <= setup.frontBufferUnits ? 1 : 0;

            return buffered;
        }

        // 隣どうしの差は 1 段まで・揃える相手(groupShift)とは同じ e
        std::vector<int32_t> ChooseExponents(const TimeTreeSetup& setup, TimeTreeWorld& world, uint64_t tick) {
            TimeTreeSteps& steps = world.steps;
            Choice choice{.next = std::vector<int32_t>(world.nest.blocks.size(), -1),
                          .tick = tick,
                          .cap = std::min(TsAlignedCap(tick), setup.maxExponent)};
            for (const uint32_t slot : world.layout.blocks) {
                if (steps.open[slot] == 0)
                    choice.next[slot] = FirstNext(setup, steps, choice, slot);
            }

            // 退く道: 毎刻みと決まった枠(望みの e が 0)の隣は毎刻み。ほかの隣とは差を問わない
            const bool jump = setup.select == TimeTreeSelect::Jump;
            const bool units = setup.frontBufferUnits > 0 && !jump;
            const std::vector<uint8_t> buffered = units ? BufferedBlocks(setup, world) : std::vector<uint8_t>{};
            bool changed = true;
            while (changed) {
                changed = false;
                for (const uint32_t slot : world.layout.blocks) {
                    if (units && buffered[slot] != 0 && Current(steps, choice, slot) > 0) {
                        // 0 に縮める(歩の途中なら打ち切る)= 長さ 0 の相手に揃えるのと同じ
                        if (choice.next[slot] < 0) {
                            TruncateBlock(setup, world, slot, choice.tick);
                            choice.next[slot] = FirstNext(setup, steps, choice, slot);
                        }

                        choice.next[slot] = 0;
                        changed = true;
                    }

                    for (const uint32_t neighbor : world.layout.neighbors[slot]) {
                        const bool active = steps.desired[neighbor] == 0;
                        if (jump && !active)
                            continue;

                        const int32_t allowed = jump || (setup.frontBuffer && !units && active) ? 0 : 1;
                        changed = LimitTo(setup, world, choice, slot, neighbor, allowed) || changed;
                    }

                    for (const uint32_t mate : world.layout.mates[slot])
                        changed = LimitTo(setup, world, choice, slot, mate, 0) || changed;
                }
            }

            return choice.next;
        }

        // 2 通り目: 解いた歩の変化(差 δ の最大。精度の単位 × 2^16)が予算を超えた枠は、望みの e を縮めて取り直す印を付ける。
        // まだ真実にも歩の記録にも書いていないので、取り直しは解き直すだけ。取り直す枠があれば true
        bool MarkRetakes(const TimeTreeSetup& setup, TimeTreeWorld& world, const std::vector<SolvedSpan>& solved) {
            TimeTreeSteps& steps = world.steps;
            bool any = false;
            for (const SolvedSpan& span : solved) {
                if (span.span.exponent == 0)
                    continue;

                for (const uint32_t slot : span.span.slots) {
                    uint64_t change = 0;
                    for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                        const uint32_t node = span.nodeOf[CellId(slot, index)];
                        if (node != NO_NODE)
                            change = std::max(change, FxAbsU64(span.state.values[node]));
                    }

                    if (change <= setup.budgetQ16)
                        continue;

                    if (setup.select == TimeTreeSelect::Jump)
                        steps.desired[slot] = 0;
                    else
                        steps.desired[slot] = TsDesiredExponent(TS_RULE_CHANGE, span.span.exponent, change,
                                                                setup.budgetQ16);
                    ++steps.retakes;
                    any = true;
                }
            }

            return any;
        }

        // 列: 始める枠を e ごとに(長い e から。面の持ち主は短い側)解く
        std::vector<SolvedSpan> SolveSpans(const TimeTreeSetup& setup, TimeTreeWorld& world,
                                           std::span<const int32_t> next) {
            std::vector<SolvedSpan> solved;
            for (int32_t exponent = setup.maxExponent; exponent >= 0; --exponent) {
                Span span{.exponent = exponent, .start = world.steps.tick};
                for (const uint32_t slot : world.layout.blocks) {
                    if (next[slot] == exponent)
                        span.slots.push_back(slot);
                }

                if (!span.slots.empty())
                    solved.push_back(SolveSpan(setup, world, span, next));
            }

            return solved;
        }

        void ProcessEvent(const TimeTreeSetup& setup, TimeTreeWorld& world) {
            TimeTreeSteps& steps = world.steps;
            const uint64_t tick = steps.tick;
            for (const uint32_t slot : world.layout.blocks) {
                if (steps.open[slot] != 0 && steps.end[slot] == tick)
                    CloseBlock(setup, world, slot, true);
            }

            for (const uint32_t slot : world.layout.blocks) {
                if (steps.touched[slot] != 0)
                    steps.desired[slot] = 0;
            }

            // --- e を選んで解く。2 通り目は予算を超えた枠を縮めて選び直す(望みの e は減るだけなので終わる)---
            // 退く道: 上限の倍数の刻みで、触られていない枠は全部「一気に進めてみる」から(試しの解きで毎刻みへ落とす)
            const bool jump = setup.select == TimeTreeSelect::Jump && setup.rule != TimeStepRule::Fixed;
            if (jump && TsAlignedCap(tick) >= setup.maxExponent) {
                for (const uint32_t slot : world.layout.blocks) {
                    if (steps.open[slot] == 0 && steps.touched[slot] == 0)
                        steps.desired[slot] = setup.maxExponent;
                }
            }

            const bool retake = (setup.select == TimeTreeSelect::Retake || jump) && setup.rule != TimeStepRule::Fixed;
            std::vector<SolvedSpan> solved;
            do {
                const std::vector<int32_t> next = ChooseExponents(setup, world, tick);
                solved = SolveSpans(setup, world, next);
            } while (retake && MarkRetakes(setup, world, solved));

            // --- 書く(長い e から。値は真実だけを読んだので順は結果に効かない)---
            for (const SolvedSpan& span : solved) {
                for (const uint32_t slot : span.span.slots)
                    steps.touched[slot] = 0;

                CommitSpan(setup, world, span);
            }

            for (const uint32_t slot : world.layout.blocks) {
                for (const uint32_t neighbor : world.layout.neighbors[slot])
                    steps.widestNeighborGap = std::max(steps.widestNeighborGap,
                                                       std::abs(steps.exponent[slot] - steps.exponent[neighbor]));
            }

            ++steps.events;
        }

    }  // namespace

    // --- 外の口 ------------------------------------------------------------------------------------

    int64_t TimeTreeTemperatureQ16(const TimeTreeSetup& setup, int64_t energy) {
        if (energy <= 0)
            return 0;

        const auto magnitude = static_cast<uint64_t>(energy);
        const FxU128 numerator = {magnitude >> (64 - 48), magnitude << 48};

        return static_cast<int64_t>(FxDivU128By64(numerator, setup.capacityQ32).quotient);
    }

    TimeTreeWorld MakeTimeTreeWorld(const TimeTreeSetup& setup, MultiresNest nest, uint32_t temperatureField) {
        TimeTreeWorld world{.nest = std::move(nest), .temperatureField = temperatureField};
        world.layout = MakeLayout(world.nest, setup);
        const size_t slots = world.nest.blocks.size();
        const size_t cells = world.layout.stepped.size();
        const int32_t first = setup.rule == TimeStepRule::Fixed ? setup.fixedExponent : 0;

        TimeTreeSteps& steps = world.steps;
        steps.exponent.assign(slots, first);
        steps.desired.assign(slots, first);
        steps.open.assign(slots, 0);
        steps.touched.assign(slots, 0);
        steps.start.assign(slots, 0);
        steps.end.assign(slots, 0);
        steps.deltaEnergy.assign(cells, 0);
        steps.deltaReactant.assign(cells, 0);
        steps.cellRecords.assign(cells, {});
        steps.faceRecords.assign(world.layout.faces.size(), {});
        steps.sleepMean.assign(slots, 0);

        // --- 頁に広げ、導かれる値を歩の初めの温度に(起きた所として始める。静かな枠は最初の歩の終わりに眠る)---
        for (const uint32_t slot : world.layout.blocks) {
            [[maybe_unused]] const bool expanded = ExpandNestPage(world.nest, slot);
            FX_ASSERT(expanded);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (world.layout.stepped[CellId(slot, index)] == 0)
                    continue;

                const int64_t temperature = TimeTreeTemperatureQ16(setup, LoadNestCell(world.nest, slot, index).energy);
                StoreNestField(world.nest, temperatureField, slot, index, 0, temperature);
            }
        }

        return world;
    }

    void StepTimeTree(const TimeTreeSetup& setup, TimeTreeWorld& world, uint64_t ticks) {
        TimeTreeSteps& steps = world.steps;
        const uint64_t target = steps.tick + ticks;
        for (;;) {
            if (!steps.processed) {
                ProcessEvent(setup, world);
                steps.processed = true;
            }

            uint64_t next = ~0ull;
            for (const uint32_t slot : world.layout.blocks)
                next = std::min(next, steps.end[slot]);

            if (next > target) {
                steps.tick = target;
                return;
            }

            steps.tick = next;
            steps.processed = false;
            if (next == target)
                return;
        }
    }

    void InjectTimeTreeHeat(const TimeTreeSetup& setup, TimeTreeWorld& world, uint32_t slot, uint32_t index,
                            int64_t heat) {
        TimeTreeSteps& steps = world.steps;
        steps.touched[slot] = 1;
        if (steps.open[slot] != 0 && steps.end[slot] == steps.tick)
            CloseBlock(setup, world, slot, true);
        else if (steps.open[slot] != 0)
            TruncateBlock(setup, world, slot, steps.tick);

        if (MrIsUniform(world.nest.blocks[slot])) {
            [[maybe_unused]] const bool expanded = ExpandNestPage(world.nest, slot);
            FX_ASSERT(expanded);
            ++steps.wakes;
        }

        RxCell cell = LoadNestCell(world.nest, slot, index);
        cell.energy += heat;
        StoreNestCell(world.nest, slot, index, cell);
        steps.outsideHeat += ToFinest(world.layout, world.nest.blocks[slot].level, heat);
        steps.processed = false;
    }

    uint64_t HashTimeTree(const TimeTreeWorld& world) {
        const TimeTreeSteps& steps = world.steps;
        uint64_t hash = FxHash64(HashWholeNest(world.nest), steps.tick, steps.processed ? 1 : 0, 0);
        const auto mix = [&hash](int64_t value) {
            hash = FxHashCombine(hash, static_cast<uint64_t>(value));
        };
        for (const uint32_t slot : world.layout.blocks) {
            mix(steps.exponent[slot]);
            mix(steps.desired[slot]);
            mix(steps.open[slot]);
            mix(steps.touched[slot]);
            mix(static_cast<int64_t>(steps.start[slot]));
            mix(static_cast<int64_t>(steps.end[slot]));
        }

        for (size_t id = 0; id < steps.deltaEnergy.size(); ++id) {
            mix(steps.deltaEnergy[id]);
            mix(steps.deltaReactant[id]);
            mix(static_cast<int64_t>(steps.cellRecords[id].start));
            mix(static_cast<int64_t>(steps.cellRecords[id].ticks));
            mix(steps.cellRecords[id].reacted);
            mix(steps.cellRecords[id].ground);
        }

        for (const TimeTreeFaceRecord& record : steps.faceRecords) {
            mix(static_cast<int64_t>(record.start));
            mix(static_cast<int64_t>(record.ticks));
            mix(record.moved);
        }

        mix(steps.outsideHeat);
        mix(steps.reactionHeat);

        return hash;
    }

    TimeTreeTotals ConservedTimeTree(const TimeTreeWorld& world) {
        TimeTreeTotals totals;
        for (size_t id = 0; id < world.layout.stepped.size(); ++id) {
            if (world.layout.stepped[id] == 0)
                continue;

            const auto slot = static_cast<uint32_t>(SlotOf(static_cast<uint32_t>(id)));
            const int32_t level = world.nest.blocks[slot].level;
            const RxCell cell = LoadNestCell(world.nest, slot, IndexOf(static_cast<uint32_t>(id)));
            const int64_t amount = AmountOf(cell, TIME_TREE_REACTANT) + AmountOf(cell, TIME_TREE_PRODUCT);
            totals.heat += ToFinest(world.layout, level, cell.energy + world.steps.deltaEnergy[id]);
            totals.reactant += ToFinest(world.layout, level, amount);
        }

        totals.heat -= world.steps.outsideHeat + world.steps.reactionHeat;

        return totals;
    }

    int64_t TimeTreeSettledEnergy(const TimeTreeWorld& world, uint32_t slot, uint32_t index) {
        return LoadNestCell(world.nest, slot, index).energy + world.steps.deltaEnergy[CellId(slot, index)];
    }

    int64_t TimeTreeSettledReactant(const TimeTreeWorld& world, uint32_t slot, uint32_t index) {
        return AmountOf(LoadNestCell(world.nest, slot, index), TIME_TREE_REACTANT) +
               world.steps.deltaReactant[CellId(slot, index)];
    }

    TimeTreeRecordBytes MeasureTimeTreeRecords(const TimeTreeWorld& world) {
        constexpr uint64_t BLOCK_BYTES = 1 + 1 + 1 +
                                         8;         // e・望みの e・歩の途中/触った・始めた刻み(終わり = 始め + 2^e)
        constexpr uint64_t CELL_RECORD_BYTES = 16;  // 反応・地面(区間は枠の歩と同じ。打ち切ると枠も閉じる)
        constexpr uint64_t FACE_RECORD_BYTES = 24;  // 区間(始め・長さ)・受け渡した値
        constexpr uint64_t CELL_DELTA_BYTES = 16;   // 歩の変化(記録の和なので作り直せる)
        const TimeTreeSteps& steps = world.steps;
        TimeTreeRecordBytes bytes;
        for (const uint32_t slot : world.layout.blocks) {
            bytes.minimal += BLOCK_BYTES;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const uint32_t id = CellId(slot, index);
                if (world.layout.stepped[id] == 0)
                    continue;

                ++bytes.cells;
                const TimeTreeCellRecord& record = steps.cellRecords[id];
                if (Crosses(record.start, record.ticks, steps.tick) && (record.reacted != 0 || record.ground != 0))
                    bytes.minimal += CELL_RECORD_BYTES;
            }
        }

        for (const TimeTreeFaceRecord& record : steps.faceRecords) {
            if (Crosses(record.start, record.ticks, steps.tick) && record.moved != 0)
                bytes.minimal += FACE_RECORD_BYTES;
        }

        bytes.held = bytes.minimal + bytes.cells * CELL_DELTA_BYTES;

        return bytes;
    }

}  // namespace bicameral::sim
