// multires_nest.cpp — 多重解像度の木の CPU リファレンス(multires_nest.h)のうち、観察の枠(影・写し)・刻み・要約・保存量の合計。
// 世界の木の管理(索引・空きのスタック・要求・帳簿)は multires_tree.cpp。
// 1 段ぶんの操作(RefineShadowLevel・PullBackLevel)は、GPU のノード(shaders/sim/multires_graph.hlsl)の 1 グループと同じ順。
#include "sim/multires_nest.h"

#include <algorithm>

#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        using nest_detail::CellAt;
        using nest_detail::FractionAt;

        // --- 1 段ぶんの操作 ---

        bool IsObserverSlot(const MultiresNest& nest, uint32_t slot) {
            return slot >= nest.capacity.worldBlocks && slot < nest.blocks.size();
        }

        void RefineShadowLevel(MultiresNest& nest, uint32_t parentSlot, uint32_t childSlot,
                               const MultiresPoint& point) {
            FX_ASSERT(IsObserverSlot(nest, childSlot));
            const MrBlock parent = nest.blocks[parentSlot];
            const uint32_t octant = MrOctantOfPoint(parent, point.x, point.y, point.z, point.level);

            // --- セル: 子は親と同じ数(影は端数を持たず、親を覆わない)---
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                CellAt(nest, childSlot, index) = CellAt(nest, parentSlot, MrParentCellOfChild(octant, index));

            nest.blocks[childSlot] = MrMakeChildBlock(parent, parentSlot, octant, MR_BLOCK_SHADOW, MR_NO_FRACTION);
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

        // 帳簿の 1 つの値(レベル level の単位 × 2^-64)を finestLevel の単位に
        Wide256 LedgerValue(uint64_t value, int32_t level, int32_t finestLevel) {
            FX_ASSERT(level <= finestLevel);

            return ShiftLeft(Wide256{value, 0, 0, 0}, static_cast<uint32_t>(finestLevel - level) * MR_LEVEL_SHIFT);
        }

        // 帳簿の 1 つの列(0 = エネルギー、1 + 物質 ID)の量を合計に足す
        void AddLedgerColumn(ConservedTotals& totals, const BakedReactionTable& table, uint32_t column,
                             const Wide256& amount) {
            if (column == 0) {
                AddTo(totals.energy, amount);
                return;
            }

            const uint32_t species = column - 1;
            const size_t elementCount = totals.elements.size();
            for (size_t element = 0; element < elementCount; ++element) {
                const uint32_t atoms = table.speciesElements[(species * elementCount) + element];
                if (atoms != 0)
                    AddTo(totals.elements[element], MultiplySmall(amount, atoms));
            }
        }

        void AddLedgerTotals(ConservedTotals& totals, const MultiresNest& nest, const BakedReactionTable& table,
                             int32_t finestLevel) {
            const uint32_t columns = nest.capacity.ledgerColumns;
            for (uint32_t row = 0; row < MR_LEDGER_LEVELS; ++row) {
                const int32_t level = MR_LEDGER_LEVEL_MIN + static_cast<int32_t>(row);
                for (uint32_t column = 0; column < columns; ++column) {
                    const uint64_t value = nest.ledger[(size_t{row} * columns) + column];
                    if (value != 0)
                        AddLedgerColumn(totals, table, column, LedgerValue(value, level, finestLevel));
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

    MultiresNest MakeMultiresNest(const MultiresCapacity& capacity) {
        FX_ASSERT(capacity.indexEntries == 0 || (capacity.indexEntries & (capacity.indexEntries - 1)) == 0);
        FX_ASSERT(capacity.ledgerColumns >= 1);
        const size_t blockCount = size_t{capacity.worldBlocks} + capacity.observerBlocks;

        MultiresNest nest;
        nest.capacity = capacity;
        nest.blocks.assign(blockCount, MrMakeUnusedBlock());
        nest.cells.assign(blockCount * MR_BLOCK_CELLS, RxMakeEmptyCell(0));
        nest.fractions.assign(size_t{capacity.fractions} * MR_BLOCK_CELLS, MrMakeEmptyFraction());
        nest.ledger.assign(size_t{MR_LEDGER_LEVELS} * capacity.ledgerColumns, 0);
        nest.index.assign(capacity.indexEntries, MR_INDEX_EMPTY);
        nest.requests.assign(MR_MAX_REQUESTS, MrMakeRequest(0, 0, 0, 0, 0));
        nest.states.assign(MR_MAX_REQUESTS, MrMakeRequestState());
        nest.claims.assign(capacity.worldBlocks, MR_NO_CLAIM);

        // --- 空きのスタック: 上(最後)から 0, 1, 2… と取れるように積む ---
        nest.freeBlocks.resize(capacity.worldBlocks);
        for (uint32_t i = 0; i < capacity.worldBlocks; ++i)
            nest.freeBlocks[i] = capacity.worldBlocks - 1 - i;

        nest.freeFractions.resize(capacity.fractions);
        for (uint32_t i = 0; i < capacity.fractions; ++i)
            nest.freeFractions[i] = capacity.fractions - 1 - i;

        nest.counters[MR_COUNTER_FREE_BLOCKS] = capacity.worldBlocks;
        nest.counters[MR_COUNTER_FREE_FRACTIONS] = capacity.fractions;

        return nest;
    }

    void PlaceMirrorBlock(MultiresNest& nest, uint32_t slot, int32_t level, int64_t originX, int64_t originY,
                          int64_t originZ, std::span<const RxCell> cells) {
        FX_ASSERT(cells.size() == MR_BLOCK_CELLS);
        FX_ASSERT(IsObserverSlot(nest, slot));
        nest.blocks[slot] = MrMakeMirrorBlock(level, originX, originY, originZ);
        std::ranges::copy(cells, nest.cells.begin() + static_cast<ptrdiff_t>(size_t{slot} * MR_BLOCK_CELLS));
    }

    void RefineShadowChain(MultiresNest& nest, uint32_t parentSlot, uint32_t firstChildSlot, uint32_t levelCount,
                           const MultiresPoint& point) {
        uint32_t parent = parentSlot;
        for (uint32_t i = 0; i < levelCount; ++i) {
            RefineShadowLevel(nest, parent, firstChildSlot + i, point);
            parent = firstChildSlot + i;
        }
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

        for (const uint32_t slot : nest.freeBlocks)
            hash = FxHashCombine(hash, slot);

        for (const uint32_t slot : nest.freeFractions)
            hash = FxHashCombine(hash, slot);

        for (const uint64_t value : nest.ledger)
            hash = FxHashCombine(hash, value);

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

        AddLedgerTotals(totals, nest, table, finestLevel);

        return totals;
    }

}  // namespace bicameral::sim
