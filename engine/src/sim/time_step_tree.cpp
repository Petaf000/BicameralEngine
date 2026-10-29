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
                                        .unitA = layout.unitOf[id],
                                        .unitB = layout.unitOf[other],
                                        .gap = gap,
                                        .coefficient = FaceCoefficient(setup, block.level)});
            }
        }

        int64_t FloorDiv(int64_t value, int64_t divisor) {
            return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
        }

        // 揃える段(レベルごと): groupShift と、単位の物の長さ(D-640)に届くまでの段の大きい方
        uint32_t GroupShiftAt(const TimeTreeSetup& setup, int32_t level) {
            const bool grid = setup.unitByLength && setup.windowLevel0Cells == 0;  // 窓は格子の境を持たない(D-669)
            const int32_t byLength = grid ? std::max(level - setup.unitLevel, 0) : 0;

            return std::max(setup.groupShift, static_cast<uint32_t>(byLength));
        }

        // 同じレベルで 2^揃える段 ブロック角の枠どうしを、同じ e に揃える相手に(細かさを同じ物の長さで比べる。T-0985・D-640)。
        // 揃える段があるのは単位が枠の時だけ(UnitEdgeFor)なので、単位 = 枠の最初のセル
        void AddMates(const MultiresNest& nest, const TimeTreeSetup& setup, TimeTreeLayout& layout) {
            if (layout.unitEdge != MR_BLOCK_EDGE)
                return;

            const auto group = [&](uint32_t unit) {
                const MrBlock& block = nest.blocks[SlotOf(unit)];
                const int64_t edge = int64_t{MR_BLOCK_EDGE} << GroupShiftAt(setup, block.level);
                return std::array{int64_t{block.level}, FloorDiv(block.originX, edge), FloorDiv(block.originY, edge),
                                  FloorDiv(block.originZ, edge)};
            };
            for (const uint32_t unit : layout.units) {
                for (const uint32_t other : layout.units) {
                    if (other != unit && group(other) == group(unit))
                        layout.mates[unit].push_back(other);
                }
            }
        }

        // 刻みを選ぶ単位の一辺(T-1089): 窓の時だけ unitEdgeCells(1・2・4・8)。格子・揃える段・退く道は枠
        uint32_t UnitEdgeFor(const TimeTreeSetup& setup) {
            const bool grid = setup.unitByLength && setup.windowLevel0Cells == 0;
            const bool block = setup.windowLevel0Cells == 0 || grid || setup.groupShift > 0 ||
                               setup.select == TimeTreeSelect::Jump;
            if (block)
                return MR_BLOCK_EDGE;

            const uint32_t edge = setup.unitEdgeCells;
            FX_ASSERT(edge == 1 || edge == 2 || edge == 4 || edge == MR_BLOCK_EDGE);

            return edge;
        }

        // 単位の最初のセル(枠の中の座標を一辺の倍数に切り下げた所)
        uint32_t UnitIndex(uint32_t index, uint32_t edge) {
            const uint32_t mask = ~(edge - 1);

            return MrCellIndex(MrCellX(index) & mask, MrCellY(index) & mask, MrCellZ(index) & mask);
        }

        // 窓の格子(D-669・T-1089): 刻むセルを最も細かいレベルの格子に置く(粗いセルは 8^差 の格子のセル)。半径は格子のセルの数
        void AddWindowGrid(const MultiresNest& nest, const TimeTreeSetup& setup, TimeTreeLayout& layout) {
            TimeTreeWindowGrid& grid = layout.window;
            grid.level = layout.finestLevel;
            const uint32_t radius = setup.windowLevel0Cells;
            grid.radius = grid.level >= 0 ? radius << static_cast<uint32_t>(grid.level)
                                          : radius >> static_cast<uint32_t>(-grid.level);
            const auto lowOf = [&](uint32_t id) {
                const MrBlock& block = nest.blocks[SlotOf(id)];
                const auto shift = static_cast<uint32_t>(grid.level - block.level);
                const uint32_t index = IndexOf(id);
                return std::array{(block.originX + MrCellX(index)) << shift, (block.originY + MrCellY(index)) << shift,
                                  (block.originZ + MrCellZ(index)) << shift, int64_t{1} << shift};
            };

            std::array<int64_t, 3> low{INT64_MAX, INT64_MAX, INT64_MAX};
            std::array<int64_t, 3> high{INT64_MIN, INT64_MIN, INT64_MIN};
            for (const uint32_t unit : layout.units) {
                for (const uint32_t id : layout.unitCells[unit]) {
                    const auto cell = lowOf(id);
                    for (size_t axis = 0; axis < 3; ++axis) {
                        low[axis] = std::min(low[axis], cell[axis]);
                        high[axis] = std::max(high[axis], cell[axis] + cell[3]);
                    }
                }
            }

            grid.origin = low;
            for (size_t axis = 0; axis < 3; ++axis)
                grid.size[axis] = static_cast<uint32_t>(high[axis] - low[axis]);

            grid.unitOf.assign(size_t{grid.size[0]} * grid.size[1] * grid.size[2], NO_TIME_TREE_UNIT);
            for (const uint32_t unit : layout.units) {
                for (const uint32_t id : layout.unitCells[unit]) {
                    const auto cell = lowOf(id);
                    const auto x0 = static_cast<uint32_t>(cell[0] - low[0]);
                    const auto y0 = static_cast<uint32_t>(cell[1] - low[1]);
                    const auto z0 = static_cast<uint32_t>(cell[2] - low[2]);
                    const auto edge = static_cast<uint32_t>(cell[3]);
                    for (uint32_t z = z0; z < z0 + edge; ++z) {
                        for (uint32_t y = y0; y < y0 + edge; ++y) {
                            for (uint32_t x = x0; x < x0 + edge; ++x)
                                grid.unitOf[(size_t{z} * grid.size[1] + y) * grid.size[0] + x] = unit;
                        }
                    }
                }
            }
        }

        // 単位を作る(セルの単位・単位のセル。単位は番号の順)
        void AddUnits(const MultiresNest& nest, TimeTreeLayout& layout) {
            const uint32_t slots = nest.capacity.worldBlocks;
            for (uint32_t slot = 0; slot < slots; ++slot) {
                const MrBlock& block = nest.blocks[slot];
                if (block.kind != MR_BLOCK_REAL)
                    continue;

                layout.blocks.push_back(slot);
                layout.finestLevel = std::max(layout.finestLevel, block.level);
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    if (MrIsCoveredCell(block, index))
                        continue;

                    const uint32_t id = CellId(slot, index);
                    const uint32_t unit = CellId(slot, UnitIndex(index, layout.unitEdge));
                    layout.stepped[id] = 1;
                    layout.unitOf[id] = unit;
                    if (layout.unitCells[unit].empty())
                        layout.units.push_back(unit);

                    layout.unitCells[unit].push_back(id);
                }
            }

            std::ranges::sort(layout.units);
        }

        // 隣の差 1 段を許す最も長い e(T-1269 の (c)): 単位のセルごとに、ほかの単位への面の係数の和(そのセルの単位 / 刻み)を出し、
        // 和 × 2^e ≤ 容量 × 2^-couplingShift の最も長い e。和は面の結びつきの数(後ろ向きの Euler で隣を値を固定した面と見た時の
        // 誤差の倍率)。法則の係数と木の形だけで決まる(状態の値を読まない)
        void AddLooseExponents(const TimeTreeSetup& setup, TimeTreeLayout& layout) {
            std::vector<FxU128> crossing(layout.stepped.size(), FxU128{});
            for (const TimeTreeFace& face : layout.faces) {
                if (face.unitA == face.unitB)
                    continue;

                crossing[face.a] = ImWideAdd(crossing[face.a], face.coefficient);
                crossing[face.b] = ImWideAdd(crossing[face.b],
                                             ImWideShiftRight(face.coefficient, MR_LEVEL_SHIFT * face.gap));
            }

            const FxU128 capacity = ImWide(setup.capacityQ32);
            const FxU128 threshold = setup.couplingShift >= 0
                                         ? ImWideShiftRight(capacity, static_cast<uint32_t>(setup.couplingShift))
                                         : ImWideShiftLeft(capacity, static_cast<uint32_t>(-setup.couplingShift));
            layout.looseExponent.assign(layout.stepped.size(), setup.maxExponent);
            for (const uint32_t unit : layout.units) {
                int32_t loose = setup.maxExponent;
                for (const uint32_t id : layout.unitCells[unit]) {
                    while (loose > 0 &&
                           ImWideLess(threshold, ImWideShiftLeft(crossing[id], static_cast<uint32_t>(loose))))
                        --loose;
                }

                layout.looseExponent[unit] = loose;
            }
        }

        TimeTreeLayout MakeLayout(const MultiresNest& nest, const TimeTreeSetup& setup) {
            TimeTreeLayout layout;
            const size_t cells = size_t{nest.capacity.worldBlocks} * MR_BLOCK_CELLS;
            layout.stepped.assign(cells, 0);
            layout.unitEdge = UnitEdgeFor(setup);
            layout.unitOf.assign(cells, NO_TIME_TREE_UNIT);
            layout.unitCells.resize(cells);
            layout.unitFaces.resize(cells);
            layout.neighbors.resize(cells);
            layout.mates.resize(cells);
            layout.finestLevel = nest.capacity.rootLevel;
            AddUnits(nest, layout);
            for (const uint32_t slot : layout.blocks) {
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    if (layout.stepped[CellId(slot, index)] != 0)
                        AddCellFaces(nest, setup, layout, slot, index);
                }
            }

            for (uint32_t f = 0; f < layout.faces.size(); ++f) {
                const TimeTreeFace& face = layout.faces[f];
                layout.unitFaces[face.unitA].push_back(f);
                if (face.unitB == face.unitA)
                    continue;

                layout.unitFaces[face.unitB].push_back(f);
                for (const auto& [from, to] : {std::pair{face.unitA, face.unitB}, std::pair{face.unitB, face.unitA}}) {
                    std::vector<uint32_t>& list = layout.neighbors[from];
                    if (std::ranges::find(list, to) == list.end())
                        list.push_back(to);
                }
            }

            for (std::vector<uint32_t>& list : layout.neighbors)
                std::ranges::sort(list);

            AddMates(nest, setup, layout);
            if (setup.border == TimeTreeBorder::Coupling)
                AddLooseExponents(setup, layout);

            if (setup.windowLevel0Cells > 0)
                AddWindowGrid(nest, setup, layout);

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

        bool HasDelta(const TimeTreeSteps& steps, uint32_t id) {
            return steps.deltaEnergy[id] != 0 || steps.deltaReactant[id] != 0;
        }

        // 歩を閉じた単位が変化なしだった枠を、出来事の区切りでまとめて眠らせてみる(枠の全部のセルに歩の変化が無い時だけ。
        // 単位が枠より小さい時は、ほかの単位の歩の途中の変化が残る枠は眠らせない。T-1089)
        void SleepQuietBlocks(TimeTreeWorld& world) {
            TimeTreeSteps& steps = world.steps;
            for (const uint32_t slot : steps.quietBlocks) {
                bool any = false;
                for (uint32_t index = 0; index < MR_BLOCK_CELLS && !any; ++index)
                    any = HasDelta(steps, CellId(slot, index));

                if (!any)
                    SleepBlock(world, slot);
            }

            steps.quietBlocks.clear();
        }

        // 単位 unit の歩の変化を真実へ。変化(精度の単位 × 2^16 の最大)を返す
        uint64_t SettleUnit(const TimeTreeSetup& setup, TimeTreeWorld& world, uint32_t unit) {
            MultiresNest& nest = world.nest;
            TimeTreeSteps& steps = world.steps;
            const uint32_t slot = SlotOf(unit);
            const std::vector<uint32_t>& cells = world.layout.unitCells[unit];
            const bool any = std::ranges::any_of(cells, [&](uint32_t id) { return HasDelta(steps, id); });
            if (!any) {
                if (std::ranges::find(steps.quietBlocks, slot) == steps.quietBlocks.end())
                    steps.quietBlocks.push_back(slot);

                return 0;
            }

            // --- 眠っていれば起こす(導かれる値は眠った時の平均を配る = C-1。次の歩の初めに解き直す)---
            if (MrIsUniform(nest.blocks[slot])) {
                [[maybe_unused]] const bool expanded = ExpandNestPage(nest, slot);
                FX_ASSERT(expanded);
                ++steps.wakes;
            }

            uint64_t change = 0;
            for (const uint32_t id : cells) {
                const uint32_t index = IndexOf(id);
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

        // 歩の終わりの食い違い(T-1269): 歩の初めに解いた温度(導かれる値の欄)と、真実 + 歩の変化から出した温度の差の最大。
        // 違う刻みの隣との境の面は、短い側が長い側の歩の初めの温度で流れを記録するので、長い側の解いた温度(隣を歩の初めの値で
        // 固定した後ろ向きの Euler)と食い違う。この差がその面の誤差(と解けきらなさ)。眠った枠は導かれる値が平均なので測らない
        uint64_t UnitMismatch(const TimeTreeSetup& setup, const TimeTreeWorld& world, uint32_t unit) {
            const uint32_t slot = SlotOf(unit);
            if (MrIsUniform(world.nest.blocks[slot]))
                return 0;

            uint64_t worst = 0;
            for (const uint32_t id : world.layout.unitCells[unit]) {
                const uint32_t index = IndexOf(id);
                const int64_t energy = LoadNestCell(world.nest, slot, index).energy + world.steps.deltaEnergy[id];
                const int64_t settled = TimeTreeTemperatureQ16(setup, energy);
                const int64_t solved = LoadNestField(world.nest, world.temperatureField, slot, index, 0);
                worst = std::max(worst, FxAbsU64(settled - solved));
            }

            return worst;
        }

        void CloseUnit(const TimeTreeSetup& setup, TimeTreeWorld& world, uint32_t unit, bool measured) {
            TimeTreeSteps& steps = world.steps;
            const uint64_t mismatch = measured ? UnitMismatch(setup, world, unit) : 0;
            const uint64_t change = SettleUnit(setup, world, unit);
            steps.open[unit] = 0;
            if (!measured)
                return;

            steps.worstMismatch = std::max(steps.worstMismatch, mismatch);
            steps.mismatchOverBudget += mismatch > setup.budgetQ16 ? 1u : 0u;

            if (setup.rule == TimeStepRule::Fixed)
                steps.desired[unit] = setup.fixedExponent;
            else if (setup.select == TimeTreeSelect::Jump)
                return;  // 退く道: 決めるのは上限の倍数の刻みの試しの解きだけ(1 刻みの変化は丸めの揺れで測れない)
            else {
                // (a) 食い違いは結びつきの数(∝ 2^e)× 温度の変わり(∝ 2^e)なので N² に比例と見る(規則 B の段)
                int32_t desired = TsDesiredExponent(TS_RULE_CHANGE, steps.exponent[unit], change, setup.budgetQ16);
                if (setup.border == TimeTreeBorder::Mismatch)
                    desired = std::min(
                        desired, TsDesiredExponent(TS_RULE_ESTIMATE, steps.exponent[unit], mismatch, setup.budgetQ16));
                steps.desired[unit] = desired;
            }
        }

        // 歩を刻み tick で打ち切る(記録を経った刻みの割合に縮めてから閉じる。保存はそのまま)
        void TruncateUnit(const TimeTreeSetup& setup, TimeTreeWorld& world, uint32_t unit, uint64_t tick) {
            TimeTreeSteps& steps = world.steps;
            const uint32_t slot = SlotOf(unit);
            for (const uint32_t id : world.layout.unitCells[unit]) {
                const uint32_t index = IndexOf(id);
                TimeTreeCellRecord& record = steps.cellRecords[id];
                if (!Crosses(record.start, record.ticks, tick))
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

            for (const uint32_t f : world.layout.unitFaces[unit])
                CutFaceRecord(world, f, tick);

            CloseUnit(setup, world, unit, false);
            steps.desired[unit] = steps.touched[unit] != 0 ? 0 : steps.exponent[unit];
            ++steps.truncations;
        }

    }  // namespace

    namespace {

        // --- 列(同じ刻みに同じ e で始まる単位の集まり)の歩を始める: 反応・解き・記録 ---

        // V の回数(法則の定数の表。e ごと。ADR-0465 の決めたこと 1)
        uint32_t CyclesFor(const TimeTreeSetup& setup, int32_t exponent) {
            return exponent <= setup.shortExponent && setup.cyclesAtZero != 0 ? setup.cyclesAtZero : setup.cycles;
        }

        struct Span {
            int32_t exponent = 0;
            uint64_t start = 0;
            std::vector<uint32_t> units;
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
            for (const uint32_t unit : span.units) {
                const uint32_t slot = SlotOf(unit);
                const MrBlock& block = world.nest.blocks[slot];
                for (const uint32_t id : world.layout.unitCells[unit]) {
                    const uint32_t index = IndexOf(id);
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
            for (const uint32_t unit : span.units) {
                for (const uint32_t f : world.layout.unitFaces[unit]) {
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

                    const uint32_t outside = nodeA != NO_NODE ? face.unitB : face.unitA;
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

        // 段の重みを使い回せるかの測り(T-1090。結果に効かない): 単位の組が変わったか・温度の帯が変わったセルがあるか
        void MeasureReweights(const TimeTreeSetup& setup, TimeTreeWorld& world, const Span& span) {
            constexpr std::array<int64_t, 3> BAND_Q16 = {int64_t{1000} << 16, int64_t{10000} << 16,
                                                         int64_t{100000} << 16};
            TimeTreeSteps& steps = world.steps;
            ++steps.zeroSolves;
            const bool reshaped = span.units != steps.lastZeroSlots;
            steps.zeroReshaped += reshaped ? 1u : 0u;
            steps.lastZeroSlots = span.units;
            for (size_t b = 0; b < BAND_Q16.size(); ++b) {
                std::vector<int64_t>& bands = steps.cellBand[b];
                if (bands.size() != world.layout.stepped.size())
                    bands.assign(world.layout.stepped.size(), INT64_MIN);

                bool changed = false;
                for (const uint32_t unit : span.units) {
                    for (const uint32_t id : world.layout.unitCells[unit]) {
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
            const uint32_t cycles = CyclesFor(setup, span.exponent);
            options.solver.cycles = setup.residualStop == 0 ? cycles : 1;
            options.faceValues = false;
            options.galerkinCoarse = setup.galerkin;
            options.solver.coarsestSweeps = setup.coarsestSweeps;
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
                for (const uint32_t unit : span.units) {
                    if (steps.desired[unit] != 0)
                        steps.forcedCellSteps += world.layout.unitCells[unit].size();
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

            // --- 単位の歩 ---
            for (const uint32_t unit : span.units) {
                steps.exponent[unit] = span.exponent;
                steps.start[unit] = span.start;
                steps.end[unit] = span.start + ticks;
                steps.open[unit] = 1;
            }
        }

    }  // namespace

    namespace {

        // --- 出来事(刻み tick): 終えた歩を閉じる → e を選ぶ(窓・隣の差 1 段・長すぎる隣は打ち切る)→ 列ごとに歩を始める ---
        // e は単位(layout.units。既定はセル。T-1089)ごと

        struct Choice {
            std::vector<int32_t> next;  // 単位ごと。-1 = 歩の途中(か単位でない番号)
            uint64_t tick = 0;
            int32_t cap = 0;
        };

        int32_t Current(const TimeTreeSteps& steps, const Choice& choice, uint32_t unit) {
            return choice.next[unit] >= 0 ? choice.next[unit] : steps.exponent[unit];
        }

        // 歩を終えた単位の次の e(退く道は 0 か上限だけ。上限で始められない刻みなら毎刻み)
        int32_t FirstNext(const TimeTreeSetup& setup, const TimeTreeSteps& steps, const Choice& choice, uint32_t unit) {
            if (setup.select != TimeTreeSelect::Jump)
                return std::clamp(steps.desired[unit], 0, choice.cap);

            const bool jump = steps.desired[unit] >= setup.maxExponent && choice.cap >= setup.maxExponent;

            return jump ? setup.maxExponent : 0;
        }

        // unit の e を limit までに(長すぎれば、始める側なら縮め、歩の途中なら打ち切って始め直す)。変えたら true
        bool LimitToExponent(const TimeTreeSetup& setup, TimeTreeWorld& world, Choice& choice, uint32_t unit,
                             int32_t limit) {
            TimeTreeSteps& steps = world.steps;
            if (Current(steps, choice, unit) <= limit)
                return false;

            if (choice.next[unit] < 0) {
                TruncateUnit(setup, world, unit, choice.tick);
                choice.next[unit] = FirstNext(setup, steps, choice, unit);
            }

            choice.next[unit] = std::min(choice.next[unit], limit);

            return true;
        }

        // unit の e を other の e + allowed までに
        bool LimitTo(const TimeTreeSetup& setup, TimeTreeWorld& world, Choice& choice, uint32_t unit, uint32_t other,
                     int32_t allowed) {
            return LimitToExponent(setup, world, choice, unit, Current(world.steps, choice, other) + allowed);
        }

        // 境の面の扱い(T-1269)で隣の差を締める: (b) 歩の途中に隣が歩を始めたら 0 段・(b) の行き着く形はいつも 0 段・
        // (c) 長い側の e が結びつきの数の上限を超えるなら 0 段。どれも e は減るだけなので、掃く回は不動点に着く
        int32_t BorderAllowed(const TimeTreeSetup& setup, const TimeTreeWorld& world, const Choice& choice,
                              uint32_t unit, uint32_t neighbor, int32_t allowed) {
            if (setup.select == TimeTreeSelect::Jump || allowed == 0)
                return allowed;

            if (setup.border == TimeTreeBorder::Gap0)
                return 0;

            // 歩の途中で、隣がこの刻みに歩を始める(この単位の歩の中で隣の温度が変わり始める)→ 打ち切って隣に揃える
            if (setup.border == TimeTreeBorder::Restart && choice.next[unit] < 0 && choice.next[neighbor] >= 0)
                return 0;

            if (setup.border == TimeTreeBorder::Coupling &&
                Current(world.steps, choice, neighbor) + 1 > world.layout.looseExponent[unit])
                return 0;

            return allowed;
        }

        // 前線の緩衝を単位で(T-1074): 動いている組から面の隣をたどって frontBufferUnits 組までの単位に印
        std::vector<uint8_t> BufferedUnits(const TimeTreeSetup& setup, const TimeTreeWorld& world) {
            const TimeTreeSteps& steps = world.steps;
            std::vector<uint32_t> distance(world.layout.unitOf.size(), UINT32_MAX);
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
            for (const uint32_t unit : world.layout.units) {
                if (steps.desired[unit] == 0)
                    reach(unit, 0, frontier);
            }

            for (uint32_t d = 1; d <= setup.frontBufferUnits && !frontier.empty(); ++d) {
                std::vector<uint32_t> next;
                for (const uint32_t slot : frontier) {
                    for (const uint32_t neighbor : world.layout.neighbors[slot])
                        reach(neighbor, d, next);
                }

                frontier = std::move(next);
            }

            std::vector<uint8_t> buffered(world.layout.unitOf.size(), 0);
            for (const uint32_t unit : world.layout.units)
                buffered[unit] = distance[unit] <= setup.frontBufferUnits ? 1 : 0;

            return buffered;
        }

        // 格子の 1 軸に沿って、半径 radius の中の最小に(端の外は数えない)
        void MinAlongAxis(std::vector<int8_t>& values, const std::array<uint32_t, 3>& size, size_t axis,
                          uint32_t radius) {
            const std::array<size_t, 3> stride{1, size[0], size_t{size[0]} * size[1]};
            const uint32_t length = size[axis];
            const size_t lines = values.size() / length;
            std::vector<int8_t> line(length);
            for (size_t l = 0; l < lines; ++l) {
                // l を、axis を除いた 2 軸の座標に直して線の初めを出す
                size_t rest = l;
                size_t first = 0;
                for (size_t other = 0; other < 3; ++other) {
                    if (other == axis)
                        continue;

                    first += (rest % size[other]) * stride[other];
                    rest /= size[other];
                }

                for (uint32_t i = 0; i < length; ++i)
                    line[i] = values[first + i * stride[axis]];

                for (uint32_t i = 0; i < length; ++i) {
                    const uint32_t low = i > radius ? i - radius : 0;
                    const uint32_t high = std::min(i + radius, length - 1);
                    int8_t least = line[low];
                    for (uint32_t j = low + 1; j <= high; ++j)
                        least = std::min(least, line[j]);

                    values[first + i * stride[axis]] = least;
                }
            }
        }

        // 窓の上限(D-669・T-1089): 単位ごとに、窓の内の単位の望みの e の最小(局所の上限まで。世界の時計にそろえる上限は
        // 入れない。T-1174)。窓の格子に望みを置き、軸ごとの 3 回の最小を取り、単位の格子のセルの最小を読む(自分も入る)
        std::vector<int32_t> WindowCaps(const TimeTreeSetup& setup, const TimeTreeWorld& world) {
            const TimeTreeSteps& steps = world.steps;
            const TimeTreeWindowGrid& grid = world.layout.window;
            const auto top = static_cast<int8_t>(setup.maxExponent);
            std::vector<int8_t> values(grid.unitOf.size(), top);
            for (size_t v = 0; v < values.size(); ++v) {
                const uint32_t unit = grid.unitOf[v];
                if (unit != NO_TIME_TREE_UNIT)
                    values[v] = static_cast<int8_t>(std::clamp(steps.desired[unit], 0, setup.maxExponent));
            }

            for (size_t axis = 0; axis < 3; ++axis)
                MinAlongAxis(values, grid.size, axis, grid.radius);

            std::vector<int32_t> caps(world.layout.unitOf.size(), setup.maxExponent);
            for (size_t v = 0; v < values.size(); ++v) {
                const uint32_t unit = grid.unitOf[v];
                if (unit != NO_TIME_TREE_UNIT)
                    caps[unit] = std::min<int32_t>(caps[unit], values[v]);
            }

            return caps;
        }

        // 窓の上限・隣どうしの差は 1 段まで・揃える相手(groupShift)とは同じ e
        std::vector<int32_t> ChooseExponents(const TimeTreeSetup& setup, TimeTreeWorld& world, uint64_t tick) {
            TimeTreeSteps& steps = world.steps;
            Choice choice{.next = std::vector<int32_t>(world.layout.unitOf.size(), -1),
                          .tick = tick,
                          .cap = std::min(TsAlignedCap(tick), setup.maxExponent)};
            for (const uint32_t unit : world.layout.units) {
                if (steps.open[unit] == 0)
                    choice.next[unit] = FirstNext(setup, steps, choice, unit);
            }

            // 退く道: 毎刻みと決まった枠(望みの e が 0)の隣は毎刻み。ほかの隣とは差を問わない
            const bool jump = setup.select == TimeTreeSelect::Jump;
            const bool units = setup.frontBufferUnits > 0 && !jump;
            const std::vector<uint8_t> buffered = units ? BufferedUnits(setup, world) : std::vector<uint8_t>{};
            const bool window = setup.windowLevel0Cells > 0 && !jump;
            const std::vector<int32_t> windowCaps = window ? WindowCaps(setup, world) : std::vector<int32_t>{};
            bool changed = true;
            while (changed) {
                changed = false;
                for (const uint32_t unit : world.layout.units) {
                    // 緩衝は 0 に縮める(歩の途中なら打ち切る)= 長さ 0 の相手に揃えるのと同じ
                    if (units && buffered[unit] != 0)
                        changed = LimitToExponent(setup, world, choice, unit, 0) || changed;

                    if (window)
                        changed = LimitToExponent(setup, world, choice, unit, windowCaps[unit]) || changed;

                    for (const uint32_t neighbor : world.layout.neighbors[unit]) {
                        const bool active = steps.desired[neighbor] == 0;
                        if (jump && !active)
                            continue;

                        int32_t allowed = jump || (setup.frontBuffer && !units && active) ? 0 : 1;
                        allowed = BorderAllowed(setup, world, choice, unit, neighbor, allowed);
                        changed = LimitTo(setup, world, choice, unit, neighbor, allowed) || changed;
                    }

                    for (const uint32_t mate : world.layout.mates[unit])
                        changed = LimitTo(setup, world, choice, unit, mate, 0) || changed;
                }
            }

            SleepQuietBlocks(world);  // 打ち切った単位の枠

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

                for (const uint32_t unit : span.span.units) {
                    uint64_t change = 0;
                    for (const uint32_t id : world.layout.unitCells[unit])
                        change = std::max(change, FxAbsU64(span.state.values[span.nodeOf[id]]));

                    if (change <= setup.budgetQ16)
                        continue;

                    if (setup.select == TimeTreeSelect::Jump)
                        steps.desired[unit] = 0;
                    else
                        steps.desired[unit] = TsDesiredExponent(TS_RULE_CHANGE, span.span.exponent, change,
                                                                setup.budgetQ16);
                    ++steps.retakes;
                    any = true;
                }
            }

            return any;
        }

        // 列: 始める単位を e ごとに(長い e から。面の持ち主は短い側)解く
        std::vector<SolvedSpan> SolveSpans(const TimeTreeSetup& setup, TimeTreeWorld& world,
                                           std::span<const int32_t> next) {
            std::vector<SolvedSpan> solved;
            for (int32_t exponent = setup.maxExponent; exponent >= 0; --exponent) {
                Span span{.exponent = exponent, .start = world.steps.tick};
                for (const uint32_t unit : world.layout.units) {
                    if (next[unit] == exponent)
                        span.units.push_back(unit);
                }

                if (!span.units.empty())
                    solved.push_back(SolveSpan(setup, world, span, next));
            }

            return solved;
        }

        // --- 刻みを選ぶ段を見る口(T-1112。結果に効かない)---
        struct ChooseWatch {
            std::vector<uint16_t> before;
            std::vector<uint8_t> windowCaps;
        };

        ChooseWatch WatchBefore(const TimeTreeSetup& setup, const TimeTreeWorld& world) {
            ChooseWatch watch{.before = PackTimeTreeUnits(world)};
            const bool window = setup.windowLevel0Cells > 0 && setup.select != TimeTreeSelect::Jump;
            const std::vector<int32_t> caps = window ? WindowCaps(setup, world) : std::vector<int32_t>{};
            for (const uint32_t unit : world.layout.units)
                watch.windowCaps.push_back(static_cast<uint8_t>(window ? caps[unit] : setup.maxExponent));

            return watch;
        }

        void WatchAfter(const TimeTreeSetup& setup, const TimeTreeWorld& world, const ChooseWatch& watch,
                        const std::vector<int32_t>& next) {
            const uint64_t tick = world.steps.tick;
            std::vector<int32_t> chosen;
            chosen.reserve(world.layout.units.size());
            for (const uint32_t unit : world.layout.units)
                chosen.push_back(next[unit]);

            const std::vector<uint16_t> after = PackTimeTreeUnits(world);
            const TimeTreeChooseView view{.tick = tick,
                                          .cap = std::min(TsAlignedCap(tick), setup.maxExponent),
                                          .before = watch.before,
                                          .windowCaps = watch.windowCaps,
                                          .next = chosen,
                                          .after = after};
            (*setup.chooseObserver)(view);
        }

        void ProcessEvent(const TimeTreeSetup& setup, TimeTreeWorld& world) {
            TimeTreeSteps& steps = world.steps;
            const uint64_t tick = steps.tick;
            for (const uint32_t unit : world.layout.units) {
                if (steps.open[unit] != 0 && steps.end[unit] == tick)
                    CloseUnit(setup, world, unit, true);
            }

            SleepQuietBlocks(world);
            for (const uint32_t unit : world.layout.units) {
                if (steps.touched[unit] != 0)
                    steps.desired[unit] = 0;
            }

            // --- e を選んで解く。2 通り目は予算を超えた枠を縮めて選び直す(望みの e は減るだけなので終わる)---
            // 退く道: 上限の倍数の刻みで、触られていない枠は全部「一気に進めてみる」から(試しの解きで毎刻みへ落とす)
            const bool jump = setup.select == TimeTreeSelect::Jump && setup.rule != TimeStepRule::Fixed;
            if (jump && TsAlignedCap(tick) >= setup.maxExponent) {
                for (const uint32_t unit : world.layout.units) {
                    if (steps.open[unit] == 0 && steps.touched[unit] == 0)
                        steps.desired[unit] = setup.maxExponent;
                }
            }

            const bool retake = (setup.select == TimeTreeSelect::Retake || jump) && setup.rule != TimeStepRule::Fixed;
            ChooseWatch watch;
            if (setup.chooseObserver != nullptr)
                watch = WatchBefore(setup, world);

            std::vector<SolvedSpan> solved;
            std::vector<int32_t> next;
            do {
                next = ChooseExponents(setup, world, tick);
                solved = SolveSpans(setup, world, next);
            } while (retake && MarkRetakes(setup, world, solved));

            // --- 書く(長い e から。値は真実だけを読んだので順は結果に効かない)---
            for (const SolvedSpan& span : solved) {
                for (const uint32_t unit : span.span.units)
                    steps.touched[unit] = 0;

                CommitSpan(setup, world, span);
            }

            for (const uint32_t unit : world.layout.units) {
                for (const uint32_t neighbor : world.layout.neighbors[unit])
                    steps.widestNeighborGap = std::max(steps.widestNeighborGap,
                                                       std::abs(steps.exponent[unit] - steps.exponent[neighbor]));
            }

            if (setup.chooseObserver != nullptr)
                WatchAfter(setup, world, watch, next);

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

        // --- 単位ごとの配列はセルの数の大きさ(単位の番号 = 最初のセルの番号)---
        TimeTreeSteps& steps = world.steps;
        steps.exponent.assign(cells, first);
        steps.desired.assign(cells, first);
        steps.open.assign(cells, 0);
        steps.touched.assign(cells, 0);
        steps.start.assign(cells, 0);
        steps.end.assign(cells, 0);
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
            for (const uint32_t unit : world.layout.units)
                next = std::min(next, steps.end[unit]);

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
        const uint32_t unit = world.layout.unitOf[CellId(slot, index)];
        steps.touched[unit] = 1;
        if (steps.open[unit] != 0 && steps.end[unit] == steps.tick)
            CloseUnit(setup, world, unit, true);
        else if (steps.open[unit] != 0)
            TruncateUnit(setup, world, unit, steps.tick);

        SleepQuietBlocks(world);

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
        for (const uint32_t unit : world.layout.units) {
            mix(steps.exponent[unit]);
            mix(steps.desired[unit]);
            mix(steps.open[unit]);
            mix(steps.touched[unit]);
            mix(static_cast<int64_t>(steps.start[unit]));
            mix(static_cast<int64_t>(steps.end[unit]));
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

    uint32_t TimeTreeUnitOf(const TimeTreeWorld& world, uint32_t slot, uint32_t index) {
        return world.layout.unitOf[CellId(slot, index)];
    }

    int32_t TimeTreeExponentAt(const TimeTreeWorld& world, uint32_t slot, uint32_t index) {
        return world.steps.exponent[TimeTreeUnitOf(world, slot, index)];
    }

    // --- 詰めた歩の記録(T-1112)---

    namespace {

        // 1 単位を詰める(入らなければ false)
        bool PackUnit(const TimeTreeSteps& steps, uint32_t unit, uint16_t& packed) {
            const int32_t exponent = steps.exponent[unit];
            const int32_t desired = steps.desired[unit];
            if (exponent < 0 || exponent > 15 || desired < 0 || desired > 15 || steps.open[unit] > 1 ||
                steps.touched[unit] > 1)
                return false;

            uint32_t bits = static_cast<uint32_t>(exponent) |
                            (static_cast<uint32_t>(desired) << TIME_TREE_RECORD_DESIRED_SHIFT) |
                            (steps.open[unit] != 0 ? TIME_TREE_RECORD_OPEN : 0u) |
                            (steps.touched[unit] != 0 ? TIME_TREE_RECORD_TOUCHED : 0u);
            const uint64_t start = steps.start[unit];
            const uint64_t end = steps.end[unit];
            const uint64_t length = Ticks(exponent);
            if (start == 0 && end == 0 && steps.open[unit] == 0)
                bits |= TIME_TREE_RECORD_FRESH;
            else if (end != start + length || start % length != 0)
                return false;
            else if (end == steps.tick)
                bits |= TIME_TREE_RECORD_ENDS_NOW;
            else if (start != (steps.tick & ~(length - 1)) || steps.tick >= end)
                return false;

            packed = static_cast<uint16_t>(bits);

            return true;
        }

    }  // namespace

    std::vector<uint16_t> PackTimeTreeUnits(const TimeTreeWorld& world) {
        std::vector<uint16_t> packed(world.layout.units.size());
        for (size_t u = 0; u < packed.size(); ++u) {
            if (!PackUnit(world.steps, world.layout.units[u], packed[u]))
                return {};
        }

        return packed;
    }

    bool UnpackTimeTreeUnits(TimeTreeWorld& world, std::span<const uint16_t> packed) {
        TimeTreeSteps& steps = world.steps;
        if (packed.size() != world.layout.units.size())
            return false;

        for (size_t u = 0; u < packed.size(); ++u) {
            const uint32_t unit = world.layout.units[u];
            const uint32_t bits = packed[u];
            const auto exponent = static_cast<int32_t>(bits & TIME_TREE_RECORD_EXPONENT_MASK);
            const uint64_t length = Ticks(exponent);
            steps.exponent[unit] = exponent;
            steps.desired[unit] = static_cast<int32_t>((bits >> TIME_TREE_RECORD_DESIRED_SHIFT) &
                                                       TIME_TREE_RECORD_EXPONENT_MASK);
            steps.open[unit] = (bits & TIME_TREE_RECORD_OPEN) != 0 ? 1 : 0;
            steps.touched[unit] = (bits & TIME_TREE_RECORD_TOUCHED) != 0 ? 1 : 0;
            if ((bits & TIME_TREE_RECORD_FRESH) != 0)
                steps.start[unit] = 0;
            else if ((bits & TIME_TREE_RECORD_ENDS_NOW) != 0)
                steps.start[unit] = steps.tick - length;
            else
                steps.start[unit] = steps.tick & ~(length - 1);

            steps.end[unit] = (bits & TIME_TREE_RECORD_FRESH) != 0 ? 0 : steps.start[unit] + length;
        }

        return true;
    }

    TimeTreeChooseTables MakeTimeTreeChooseTables(const TimeTreeSetup& setup, const TimeTreeLayout& layout) {
        TimeTreeChooseTables tables;
        const bool matesFree = std::ranges::all_of(layout.units,
                                                   [&](uint32_t unit) { return layout.mates[unit].empty(); });
        tables.supported = setup.windowLevel0Cells > 0 && setup.select == TimeTreeSelect::Previous &&
                           !setup.frontBuffer && setup.frontBufferUnits == 0 && setup.maxExponent <= 14 && matesFree &&
                           setup.border == TimeTreeBorder::None;
        tables.units = static_cast<uint32_t>(layout.units.size());
        tables.gridSize = layout.window.size;
        tables.radius = layout.window.radius;
        tables.maxExponent = setup.maxExponent;

        std::vector<uint32_t> order(layout.unitOf.size(), NO_TIME_TREE_UNIT);
        for (uint32_t u = 0; u < tables.units; ++u)
            order[layout.units[u]] = u;

        tables.gridUnits.reserve(layout.window.unitOf.size());
        for (const uint32_t unit : layout.window.unitOf)
            tables.gridUnits.push_back(unit == NO_TIME_TREE_UNIT ? NO_TIME_TREE_UNIT : order[unit]);

        tables.neighborStarts.push_back(0);
        for (const uint32_t unit : layout.units) {
            for (const uint32_t neighbor : layout.neighbors[unit])
                tables.neighbors.push_back(order[neighbor]);

            tables.neighborStarts.push_back(static_cast<uint32_t>(tables.neighbors.size()));
        }

        return tables;
    }

    TimeTreeRecordBytes MeasureTimeTreeRecords(const TimeTreeWorld& world) {
        constexpr uint64_t UNIT_BYTES = 1 + 1 + 1 + 8;  // e・望みの e・歩の途中/触った・始めた刻み(終わり = 始め + 2^e)
        constexpr uint64_t CELL_RECORD_BYTES = 16;      // 反応・地面(区間は枠の歩と同じ。打ち切ると枠も閉じる)
        constexpr uint64_t FACE_RECORD_BYTES = 24;      // 区間(始め・長さ)・受け渡した値
        constexpr uint64_t CELL_DELTA_BYTES = 16;       // 歩の変化(記録の和なので作り直せる)
        const TimeTreeSteps& steps = world.steps;
        TimeTreeRecordBytes bytes;
        for (const uint32_t unit : world.layout.units) {
            bytes.minimal += UNIT_BYTES;
            ++bytes.units;
            for (const uint32_t id : world.layout.unitCells[unit]) {
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
        bytes.packed = bytes.minimal - (bytes.units * (UNIT_BYTES - sizeof(uint16_t)));

        return bytes;
    }

    uint64_t TimeTreeEventNs(const TimeTreeSolveCost& cost, uint64_t cells, uint64_t cellCycles, uint32_t levels) {
        if (cells == 0)
            return 0;

        // 固定費 + 段と V の起動 + セルの傾き(V の回数の違う列は別の組なので、平均の切り上げで数える)
        const uint64_t cycles = (cellCycles + cells - 1) / cells;
        const uint64_t coarse = levels > 1 ? levels - 1 : 0;
        const uint64_t launches = cost.eventNs + (coarse * cost.levelNs) +
                                  (cycles * (cost.cycleNs + (coarse * cost.levelCycleNs)));
        const uint64_t slope = ((cells * cost.cellPs) + (cellCycles * cost.cellCyclePs)) / 1000;

        return launches + slope;
    }

    uint64_t TimeTreeChooseNs(const TimeTreeSolveCost& cost, uint64_t units) {
        if (units == 0)
            return 0;

        return cost.chooseNs + ((units * cost.chooseUnitPs) / 1000);
    }

    namespace {

        // まとめてよい列の組の鍵(V の回数・段の数・節のレベル)。まとめられない列は own = true(その列だけの組)
        struct GroupKey {
            uint32_t cycles = 0;
            uint32_t levels = 0;
            int32_t level = 0;
            bool own = false;
        };

        // 縮約がブロックの中で閉じる粗い段の数(8 セル角 = 2^3)
        constexpr uint32_t MERGE_MAX_COARSE_LEVELS = 3;

        GroupKey KeyOf(const TimeTreeEventSpan& span) {
            const EquilibriumSystem& system = span.system;
            GroupKey key{.cycles = span.cycles};

            // --- 段の数(部品が作る段。解かずに数える)---
            EquilibriumOptions options;
            options.solver.cycles = 0;
            options.faceValues = false;
            EquilibriumState state;
            key.levels = SolveEquilibrium(system, options, state).gridLevels;

            // --- 節のレベルが 1 つで、縮約がブロックの中で閉じるか ---
            key.level = system.nodes.empty() ? 0 : system.nodes.front().level;
            const bool sameLevel = std::ranges::all_of(
                system.nodes, [&key](const EquilibriumNode& node) { return node.level == key.level; });
            key.own = !sameLevel || key.levels > MERGE_MAX_COARSE_LEVELS + 1;

            return key;
        }

        bool SameGroup(const GroupKey& a, const GroupKey& b) {
            return !a.own && !b.own && a.cycles == b.cycles && a.levels == b.levels && a.level == b.level;
        }

        // 列の系を組の系の後ろに足す(番号を節の数だけずらす)
        void Append(EquilibriumSystem& into, const EquilibriumSystem& from) {
            const auto offset = static_cast<uint32_t>(into.nodes.size());
            into.rounding = from.rounding;
            into.nodes.insert(into.nodes.end(), from.nodes.begin(), from.nodes.end());
            for (EquilibriumFace face : from.faces) {
                face.fine += offset;
                face.coarse += offset;
                into.faces.push_back(face);
            }

            for (EquilibriumFixedFace fixed : from.fixedFaces) {
                fixed.node += offset;
                into.fixedFaces.push_back(fixed);
            }

            for (EquilibriumSource source : from.sources) {
                source.node += offset;
                into.sources.push_back(source);
            }
        }

    }  // namespace

    std::vector<TimeTreeEventGroup> GroupTimeTreeEvent(std::span<const TimeTreeEventSpan> spans) {
        std::vector<TimeTreeEventGroup> groups;
        std::vector<GroupKey> keys;
        for (uint32_t index = 0; index < spans.size(); ++index) {
            const GroupKey key = KeyOf(spans[index]);
            const auto found = std::ranges::find_if(keys,
                                                    [&key](const GroupKey& other) { return SameGroup(key, other); });
            const auto at = static_cast<size_t>(found - keys.begin());
            if (found == keys.end()) {
                keys.push_back(key);
                groups.push_back({.cycles = key.cycles, .levels = key.levels});
            }

            TimeTreeEventGroup& group = groups[at];
            group.spans.push_back(index);
            group.nodeStarts.push_back(static_cast<uint32_t>(group.system.nodes.size()));
            Append(group.system, spans[index].system);
        }

        return groups;
    }

    namespace {

        // 見積もりの 1 出来事の組(V の回数・段の数が同じ列。T-1152)
        struct EstimateGroup {
            uint32_t cycles = 0;
            uint32_t levels = 0;
            uint64_t cells = 0;
        };

        void AddEstimateGroup(std::vector<EstimateGroup>& groups, uint32_t cycles, uint32_t levels, uint64_t cells) {
            if (cells == 0)
                return;

            for (EstimateGroup& group : groups) {
                if (group.cycles == cycles && group.levels == levels) {
                    group.cells += cells;
                    return;
                }
            }

            groups.push_back({.cycles = cycles, .levels = levels, .cells = cells});
        }

    }  // namespace

    TimeTreeForwardEstimate EstimateTimeTreeForward(const TimeTreeSetup& setup, const TimeTreeWorld& world,
                                                    uint64_t ticks, const TimeTreeSolveCost& cost,
                                                    std::span<const uint32_t> levelsAt) {
        // --- e ごとの刻むセルの数 ---
        const auto exponents = static_cast<size_t>(std::max(setup.maxExponent, 0)) + 1;
        std::vector<uint64_t> cellsAt(exponents, 0);
        for (const uint32_t unit : world.layout.units) {
            const int32_t exponent = std::clamp(world.steps.exponent[unit], 0, setup.maxExponent);
            cellsAt[static_cast<size_t>(exponent)] += world.layout.unitCells[unit].size();
        }

        // --- 出来事の組 j = 2^j の倍数の刻み(2^(j+1) の倍数を除く。最後の組は残り全部): 解くのは e ≤ j の列 ---
        // 列は (V の回数・段の数) ごとの組にまとまる
        TimeTreeForwardEstimate estimate;
        uint64_t nanoseconds = 0;
        const uint64_t chooseNs = TimeTreeChooseNs(cost, world.layout.units.size());  // 出来事ごとに 1 回(T-1214)
        std::vector<EstimateGroup> groups;
        for (size_t j = 0; j < exponents; ++j) {
            const auto exponent = static_cast<int32_t>(j);
            const uint32_t levels = j < levelsAt.size() ? std::max(levelsAt[j], 1u) : 1u;
            AddEstimateGroup(groups, CyclesFor(setup, exponent), levels, cellsAt[j]);
            const uint64_t multiples = ticks >> j;
            const uint64_t events = j + 1 < exponents ? multiples - (ticks >> (j + 1)) : multiples;
            uint64_t eventNs = 0;
            uint64_t cells = 0;
            uint64_t cycles = 0;
            for (const EstimateGroup& group : groups) {
                eventNs += TimeTreeEventNs(cost, group.cells, group.cells * group.cycles, group.levels);
                cells += group.cells;
                cycles += group.cells * group.cycles;
            }

            if (cells == 0 || events == 0)
                continue;

            estimate.events += events;
            estimate.cellSolves += events * cells;
            estimate.cellCycles += events * cycles;
            nanoseconds += events * eventNs;
        }

        const uint64_t chooseTotalNs = estimate.events * chooseNs;
        nanoseconds += chooseTotalNs;
        estimate.chooseMicroseconds = chooseTotalNs / 1000;
        estimate.waitMicroseconds = nanoseconds / 1000;
        estimate.waitUpperMicroseconds = (nanoseconds / 1000) * cost.marginPercent / 100;

        return estimate;
    }

}  // namespace bicameral::sim
