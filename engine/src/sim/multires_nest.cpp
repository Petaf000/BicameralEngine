// multires_nest.cpp — 多重解像度の入れ子の細分の CPU リファレンス(multires_nest.h)。
// 1 段ぶんの操作(RefineLevel・CoarsenLevel・PullBackLevel)は、GPU のノード(shaders/sim/multires_graph.hlsl)の 1 グループと同じ順:
// セルを全部書いてから見出しを直す。端数の枠は「書く 64 セルのどれかが 0 でない」時だけ取る。
#include "sim/multires_nest.h"

#include <algorithm>

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        RxCell& CellAt(MultiresNest& nest, uint32_t slot, uint32_t index) {
            return nest.cells[(size_t{slot} * MR_BLOCK_CELLS) + index];
        }

        const RxCell& CellAt(const MultiresNest& nest, uint32_t slot, uint32_t index) {
            return nest.cells[(size_t{slot} * MR_BLOCK_CELLS) + index];
        }

        // 端数(枠が無ければ空)
        MrFraction FractionAt(const MultiresNest& nest, uint32_t fractionSlot, uint32_t index) {
            if (fractionSlot == MR_NO_FRACTION)
                return MrMakeEmptyFraction();

            return nest.fractions[(size_t{fractionSlot} * MR_BLOCK_CELLS) + index];
        }

        void SetFraction(MultiresNest& nest, uint32_t fractionSlot, uint32_t index, const MrFraction& fraction) {
            nest.fractions[(size_t{fractionSlot} * MR_BLOCK_CELLS) + index] = fraction;
        }

        uint32_t AllocateFraction(MultiresNest& nest) {
            const uint32_t slot = nest.counters[MR_COUNTER_FRACTION_BLOCKS]++;
            FX_ASSERT(slot < nest.fractions.size() / MR_BLOCK_CELLS);

            return slot;
        }

        // --- 1 段ぶんの操作 ---

        void RefineLevel(MultiresNest& nest, uint32_t parentSlot, uint32_t childSlot, uint32_t kind,
                         const MultiresPoint& point) {
            MrBlock& parent = nest.blocks[parentSlot];
            const uint32_t octant = MrOctantBitOfPoint(parent.originX, parent.level, point.x, point.level) |
                                    (MrOctantBitOfPoint(parent.originY, parent.level, point.y, point.level) << 1) |
                                    (MrOctantBitOfPoint(parent.originZ, parent.level, point.z, point.level) << 2);

            MrBlock child = MrMakeUnusedBlock();
            child.originX = MrChildOrigin(parent.originX, octant & 1u);
            child.originY = MrChildOrigin(parent.originY, (octant >> 1) & 1u);
            child.originZ = MrChildOrigin(parent.originZ, (octant >> 2) & 1u);
            child.level = parent.level + 1;
            child.kind = kind;
            child.parent = parentSlot;
            child.parentOctant = octant;

            // --- セル: 子は親と同じ数 ---
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                CellAt(nest, childSlot, index) = CellAt(nest, parentSlot, MrParentCellOfChild(octant, index));

            // --- 端数: 本物の子だけ、親の八分の一に 0 でない端数があれば写す(影は端数を持たない)---
            bool anyFraction = false;
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local)
                anyFraction |= !MrFractionIsZero(FractionAt(nest, parent.fraction, MrOctantCell(octant, local)));

            if (kind == MR_BLOCK_REAL && anyFraction) {
                child.fraction = AllocateFraction(nest);
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const MrFraction fraction = FractionAt(nest, parent.fraction, MrParentCellOfChild(octant, index));
                    SetFraction(nest, child.fraction, index, fraction);
                }
            }

            // --- 本物なら親を覆う(覆われた親のセルと端数は空)---
            if (kind == MR_BLOCK_REAL) {
                for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                    const uint32_t index = MrOctantCell(octant, local);
                    CellAt(nest, parentSlot, index) = RxMakeEmptyCell(0);
                    if (parent.fraction != MR_NO_FRACTION)
                        SetFraction(nest, parent.fraction, index, MrMakeEmptyFraction());
                }

                parent.children[octant] = childSlot;
            }

            nest.blocks[childSlot] = child;
        }

        MrChildren GatherChildren(const MultiresNest& nest, uint32_t childSlot, uint32_t fractionSlot, uint32_t local) {
            MrChildren children{};
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                const uint32_t index = MrChildCell(local, j);
                children.cells[j] = CellAt(nest, childSlot, index);
                children.fractions[j] = FractionAt(nest, fractionSlot, index);
            }

            return children;
        }

        // 子ブロックを親の八分の一へ粗く戻す。親の枠を返す
        uint32_t CoarsenLevel(MultiresNest& nest, uint32_t childSlot) {
            const MrBlock child = nest.blocks[childSlot];
            FX_ASSERT(child.kind == MR_BLOCK_REAL);
            MrBlock& parent = nest.blocks[child.parent];

            std::array<MrCoarsened, MR_OCTANT_CELLS> results{};
            bool anyFraction = false;
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                results[local] = MrCoarsenCell(GatherChildren(nest, childSlot, child.fraction, local));
                nest.counters[MR_COUNTER_LOST] += results[local].lostCount;
                nest.counters[MR_COUNTER_OVERFLOW] += results[local].overflowCount;
                anyFraction |= !MrFractionIsZero(results[local].fraction);
            }

            if (anyFraction && parent.fraction == MR_NO_FRACTION)
                parent.fraction = AllocateFraction(nest);

            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                const uint32_t index = MrOctantCell(child.parentOctant, local);
                CellAt(nest, child.parent, index) = results[local].cell;
                if (parent.fraction != MR_NO_FRACTION)
                    SetFraction(nest, parent.fraction, index, results[local].fraction);
            }

            parent.children[child.parentOctant] = MR_NO_BLOCK;
            nest.blocks[childSlot] = MrMakeUnusedBlock();

            return child.parent;
        }

        void PullBackLevel(MultiresNest& nest, const ReactionTableView& table, uint32_t shadowSlot) {
            const MrBlock shadow = nest.blocks[shadowSlot];
            FX_ASSERT(shadow.kind == MR_BLOCK_SHADOW);
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                const RxCell& parentCell = CellAt(nest, shadow.parent, MrOctantCell(shadow.parentOctant, local));
                MrShadowFamily family{};
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                    family.cells[j] = CellAt(nest, shadowSlot, MrChildCell(local, j));

                const MrShadowFamily pulled = MrPullBackShadow(table, parentCell, family);
                nest.counters[MR_COUNTER_SHADOW_CLAMPED] += pulled.clamped;
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                    CellAt(nest, shadowSlot, MrChildCell(local, j)) = pulled.cells[j];
            }
        }

        // --- 256bit の足し算(保存量の合計)---

        // 128bit の値(上, 下)を符号拡張して 256bit に
        Wide256 MakeWide256(uint64_t high, uint64_t low, bool negative) {
            const uint64_t extension = negative ? ~uint64_t{0} : 0;

            return {low, high, extension, extension};
        }

        // 左へずらす(2 の補数のまま。はみ出す上位は捨てる)
        Wide256 ShiftLeft(const Wide256& value, uint32_t bits) {
            FX_ASSERT(bits < 256);
            const uint32_t words = bits / 64;
            const uint32_t rest = bits % 64;
            Wide256 result{};
            for (size_t i = value.size(); i-- > words;) {
                const size_t source = i - words;
                result[i] = value[source] << rest;
                if (rest != 0 && source > 0)
                    result[i] |= value[source - 1] >> (64 - rest);
            }

            return result;
        }

        // 符号なしの値に小さな数を掛ける(元素の係数)
        Wide256 MultiplySmall(Wide256 value, uint32_t factor) {
            Wide256 result{};
            uint64_t carry = 0;
            for (size_t i = 0; i < value.size(); ++i) {
                const FxU128 product = FxMulU64Full(value[i], factor);
                result[i] = product.lo + carry;
                carry = product.hi + (result[i] < carry ? 1u : 0u);
            }

            return result;
        }

        void AddTo(Wide256& sum, const Wide256& value) {
            uint64_t carry = 0;
            for (size_t i = 0; i < sum.size(); ++i) {
                const uint64_t partial = sum[i] + value[i];
                const uint64_t carryPartial = partial < sum[i] ? 1u : 0u;
                sum[i] = partial + carry;
                carry = carryPartial + (sum[i] < carry ? 1u : 0u);
            }
        }

        void AddCellTotals(ConservedTotals& totals, const BakedReactionTable& table, const RxCell& cell,
                           const MrFraction& fraction, uint32_t shiftBits) {
            AddTo(totals.energy,
                  ShiftLeft(MakeWide256(static_cast<uint64_t>(cell.energy), fraction.energy, cell.energy < 0),
                            shiftBits));

            const size_t elementCount = totals.elements.size();
            for (uint32_t species = MrNextSpecies(cell, fraction, 0); species != MR_NO_SPECIES;
                 species = MrNextSpecies(cell, fraction, species)) {
                const Wide256 amount = MakeWide256(MrAmountOf(cell, species), MrFractionOf(fraction, species), false);
                for (size_t element = 0; element < elementCount; ++element) {
                    const uint32_t atoms = table.speciesElements[(species * elementCount) + element];
                    if (atoms != 0)
                        AddTo(totals.elements[element], ShiftLeft(MultiplySmall(amount, atoms), shiftBits));
                }
            }
        }

        uint64_t HashFraction(const MrFraction& fraction) {
            uint64_t hash = FxHashCombine(0, fraction.energy);
            hash = FxHashCombine(hash, fraction.speciesCount);
            for (uint32_t i = 0; i < fraction.speciesCount; ++i) {
                hash = FxHashCombine(hash, fraction.species[i]);
                hash = FxHashCombine(hash, fraction.amounts[i]);
            }

            return hash;
        }

        uint64_t HashBlock(const MrBlock& block) {
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

    }  // namespace

    MultiresNest MakeMultiresNest(uint32_t blockCapacity, uint32_t fractionCapacity) {
        MultiresNest nest;
        nest.blocks.assign(blockCapacity, MrMakeUnusedBlock());
        nest.cells.assign(size_t{blockCapacity} * MR_BLOCK_CELLS, RxMakeEmptyCell(0));
        nest.fractions.assign(size_t{fractionCapacity} * MR_BLOCK_CELLS, MrMakeEmptyFraction());

        return nest;
    }

    void PlaceRootBlock(MultiresNest& nest, uint32_t slot, int32_t level, int64_t originX, int64_t originY,
                        int64_t originZ, std::span<const RxCell> cells) {
        FX_ASSERT(cells.size() == MR_BLOCK_CELLS);
        MrBlock block = MrMakeMirrorBlock(level, originX, originY, originZ);
        block.kind = MR_BLOCK_REAL;
        nest.blocks[slot] = block;
        std::ranges::copy(cells, nest.cells.begin() + static_cast<ptrdiff_t>(size_t{slot} * MR_BLOCK_CELLS));
    }

    void PlaceMirrorBlock(MultiresNest& nest, uint32_t slot, int32_t level, int64_t originX, int64_t originY,
                          int64_t originZ, std::span<const RxCell> cells) {
        FX_ASSERT(cells.size() == MR_BLOCK_CELLS);
        nest.blocks[slot] = MrMakeMirrorBlock(level, originX, originY, originZ);
        std::ranges::copy(cells, nest.cells.begin() + static_cast<ptrdiff_t>(size_t{slot} * MR_BLOCK_CELLS));
    }

    void RefineChain(MultiresNest& nest, uint32_t parentSlot, uint32_t firstChildSlot, uint32_t levelCount,
                     uint32_t kind, const MultiresPoint& point) {
        uint32_t parent = parentSlot;
        for (uint32_t i = 0; i < levelCount; ++i) {
            RefineLevel(nest, parent, firstChildSlot + i, kind, point);
            parent = firstChildSlot + i;
        }
    }

    void CoarsenChain(MultiresNest& nest, uint32_t deepestSlot, uint32_t levelCount) {
        uint32_t child = deepestSlot;
        for (uint32_t i = 0; i < levelCount; ++i)
            child = CoarsenLevel(nest, child);
    }

    void RemoveShadowChain(MultiresNest& nest, uint32_t firstSlot, uint32_t levelCount) {
        for (uint32_t i = 0; i < levelCount; ++i) {
            FX_ASSERT(nest.blocks[firstSlot + i].kind == MR_BLOCK_SHADOW);
            nest.blocks[firstSlot + i] = MrMakeUnusedBlock();
        }
    }

    void StepNest(MultiresNest& nest, const BakedReactionTable& table, uint64_t worldSeed, uint64_t tick) {
        const ReactionTableView view = table.View();
        for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
            const MrBlock& block = nest.blocks[slot];
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (!MrIsSteppedCell(block, index))
                    continue;

                RxCell& cell = CellAt(nest, slot, index);
                cell = MrStepCell(view, cell, worldSeed, tick, block, index);
            }
        }
    }

    void PullBackShadowChain(MultiresNest& nest, const BakedReactionTable& table, uint32_t firstShadowSlot,
                             uint32_t levelCount) {
        const ReactionTableView view = table.View();
        for (uint32_t i = 0; i < levelCount; ++i)
            PullBackLevel(nest, view, firstShadowSlot + i);
    }

    uint64_t HashRealLeaves(const MultiresNest& nest) {
        uint64_t hash = 0;
        for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL)
                continue;

            hash = FxHashCombine(hash, HashBlock(block));
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (!MrIsSteppedCell(block, index))
                    continue;

                hash = FxHashCombine(hash, HashReactionCell(CellAt(nest, slot, index)));
                hash = FxHashCombine(hash, HashFraction(FractionAt(nest, block.fraction, index)));
            }
        }

        return hash;
    }

    uint64_t HashWholeNest(const MultiresNest& nest) {
        uint64_t hash = 0;
        for (const MrBlock& block : nest.blocks)
            hash = FxHashCombine(hash, HashBlock(block));

        for (const RxCell& cell : nest.cells)
            hash = FxHashCombine(hash, HashReactionCell(cell));

        for (const MrFraction& fraction : nest.fractions)
            hash = FxHashCombine(hash, HashFraction(fraction));

        for (const uint32_t counter : nest.counters)
            hash = FxHashCombine(hash, counter);

        return hash;
    }

    ConservedTotals ComputeConservedTotals(const MultiresNest& nest, const BakedReactionTable& table,
                                           int32_t finestLevel) {
        ConservedTotals totals;
        totals.elements.assign(table.elementNames.size(), Wide256{});
        for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL)
                continue;

            FX_ASSERT(block.level <= finestLevel);
            const auto shiftBits = static_cast<uint32_t>(finestLevel - block.level) * MR_LEVEL_SHIFT;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (!MrIsSteppedCell(block, index))
                    continue;

                const MrFraction fraction = FractionAt(nest, block.fraction, index);
                AddCellTotals(totals, table, CellAt(nest, slot, index), fraction, shiftBits);
            }
        }

        return totals;
    }

}  // namespace bicameral::sim
