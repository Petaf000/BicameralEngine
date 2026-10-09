// multires_nest.cpp — 多重解像度の木の CPU リファレンス(multires_nest.h)のうち、観察の枠(影・写し)・刻み・要約・保存量の合計。
// 世界の木の管理(索引・空きのスタック・要求・帳簿)は multires_tree.cpp。
// 1 段ぶんの操作(RefineShadowLevel・PullBackLevel)は、GPU のノード(shaders/sim/multires_graph.hlsl)の 1 グループと同じ順。
#include "sim/multires_nest.h"

#include <algorithm>
#include <span>

#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        using nest_detail::CellAt;
        using nest_detail::FractionAt;
        using nest_detail::PageCellAt;
        using nest_detail::UniformAt;

        // --- 1 段ぶんの操作 ---

        bool IsObserverSlot(const MultiresNest& nest, uint32_t slot) {
            return slot >= nest.capacity.worldBlocks && slot < nest.blocks.size();
        }

        void RefineShadowLevel(MultiresNest& nest, uint32_t parentSlot, uint32_t childSlot,
                               const MultiresPoint& point) {
            FX_ASSERT(IsObserverSlot(nest, childSlot));
            const MrBlock parent = nest.blocks[parentSlot];
            const uint32_t octant = MrOctantOfPoint(parent, point.x, point.y, point.z, point.level);

            // --- セル: 子は親と同じ数(影は端数を持たず、親を覆わない。影はいつも頁を持つ。
            //     溢れを使う世界は溢れごと写す。T-0187)---
            const uint32_t page = MrObserverPage(childSlot, nest.capacity.worldBlocks);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const uint32_t parentCell = MrParentCellOfChild(octant, index);
                if (nest.wideCells)
                    StoreWidePageCell(nest, page, index, LoadWideNestCell(nest, parentSlot, parentCell));
                else
                    PageCellAt(nest, page, index) = LoadNestCell(nest, parentSlot, parentCell);
            }

            nest.blocks[childSlot] = MrMakeChildBlock(parent, parentSlot, octant, MR_BLOCK_SHADOW, MR_NO_FRACTION,
                                                      page);
        }

        void PullBackLevel(MultiresNest& nest, const ReactionTableView& table, uint32_t shadowSlot) {
            const MrBlock shadow = nest.blocks[shadowSlot];
            FX_ASSERT(shadow.kind == MR_BLOCK_SHADOW);
            // 溢れのある影・親の引き戻しはまだ(インラインの形で引き戻す。T-0199)
            FX_ASSERT(!PageHasOverflow(nest, shadow.page));
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                const RxCell parentCell = LoadNestCell(nest, shadow.parent, MrOctantCell(shadow.parentOctant, local));
                MrShadowFamily family{};
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                    family.cells[j] = CellAt(nest, shadowSlot, MrChildCell(local, j));

                const MrShadowFamily pulled = MrPullBackShadow(table, parentCell, family);
                nest.counters[MR_COUNTER_SHADOW_CLAMPED] += pulled.clamped;
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                    CellAt(nest, shadowSlot, MrChildCell(local, j)) = pulled.cells[j];
            }

            // --- セルの値が変わりうるので、つついた印にして次の刻みに tc を書き直す(ADR-0018 追記 T-0125)。
            //     観察の枠は種の一覧に入れない(活性でも全部刻む。D-403)---
            nest.blocks[shadowSlot].busyTick = MR_BUSY_POKED;
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

        // セルの形 Cell・端数の形 Fraction(インライン / 上限なし。T-0187)
        template <typename Cell, typename Fraction>
        void AddCellTotals(ConservedTotals& totals, const BakedReactionTable& table, const Cell& cell,
                           const Fraction& fraction, uint32_t shiftBits) {
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

        // 溢れの領域(T-0187。溢れを使う世界だけ要約に入れる)
        uint64_t HashOverflowArea(uint64_t hash, const MultiresOverflowArea& area) {
            for (const uint32_t offset : area.offsets)
                hash = FxHashCombine(hash, offset);

            for (size_t i = 0; i < area.species.size(); ++i) {
                hash = FxHashCombine(hash, area.species[i]);
                hash = FxHashCombine(hash, area.amounts[i]);
            }

            return hash;
        }

        // セル index の溢れ(セルと端数。無ければ hash のまま)
        uint64_t HashCellOverflow(uint64_t hash, const MultiresNest& nest, uint32_t slot, uint32_t index) {
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

        // 刻むセルを 1 刻み(頁を持つブロック)
        // 端数のエネルギー(枠が無ければ 0)
        uint64_t FractionEnergy(const MultiresNest& nest, uint32_t fractionSlot, uint32_t index) {
            return FractionAt(nest, fractionSlot, index).energy;
        }

        // 頁を持つブロックの刻むセル: 伝導の変化(deltas が空なら無し)を足し、react なら反応を待ちの丸めで進める
        // (変わらなかったセルの次に評価の要る刻みの最小も求める。T-0115。上限に当たった印を数える器に足す。T-0163)
        nest_detail::BlockStepResult StepPagedBlock(MultiresNest& nest, const ReactionTableView& view, uint32_t slot,
                                                    uint64_t worldSeed, uint64_t tick, bool react,
                                                    std::span<const MrEnergyDelta> deltas) {
            const MrBlock block = nest.blocks[slot];
            nest_detail::BlockStepResult result;
            result.evaluated = react;
            MrLimitTally tally = MrMakeLimitTally();
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (!MrIsSteppedCell(block, index))
                    continue;

                RxCell& cell = CellAt(nest, slot, index);
                const RxCell before = cell;
                const uint64_t fractionBefore = FractionEnergy(nest, block.fraction, index);

                // --- 伝導 ---
                if (!deltas.empty()) {
                    const MrEnergyDelta& delta = deltas[nest_detail::PageCellAddress(nest, block.page, index)];
                    if (!MrEnergyDeltaIsZero(delta))
                        nest_detail::ApplyEnergyDelta(nest, slot, index, cell, delta);
                }

                // --- 反応(溢れを使う世界は上限の無い形で読み書きする。T-0187)---
                bool wideChanged = false;
                if (react && nest.wideCells) {
                    const RxWideCell wide = LoadWideNestCell(nest, slot, index);
                    const RxWideWaitStep step = MrStepCellWait(view, wide, worldSeed, tick, block, index);
                    result.wakeTick = std::min(result.wakeTick, step.wakeTick);
                    tally = MrAddLimits(tally, step.limits);
                    wideChanged = !RxSameCell(wide, step.cell);
                    StoreWidePageCell(nest, block.page, index, step.cell);
                } else if (react) {
                    const RxWaitStep step = MrStepCellWait(view, cell, worldSeed, tick, block, index);
                    result.wakeTick = std::min(result.wakeTick, step.wakeTick);
                    cell = step.cell;
                    tally = MrAddLimits(tally, step.limits);
                }

                result.changed = result.changed || wideChanged || MrCellChanged(before, cell) ||
                                 FractionEnergy(nest, block.fraction, index) != fractionBefore;
            }

            // --- 上限に当たった印を数える(T-0163。GPU の StepPagedWait・ApplyCell と同じ数)---
            nest.counters[MR_COUNTER_LIMIT_PRODUCTS] += tally.productsHeld;
            nest.counters[MR_COUNTER_LIMIT_CANDIDATES] += tally.candidatesLimited;

            return result;
        }

    }  // namespace

    namespace nest_detail {

        void ApplyEnergyDelta(MultiresNest& nest, uint32_t slot, uint32_t index, RxCell& cell,
                              const MrEnergyDelta& delta) {
            const uint32_t fractionSlot = nest.blocks[slot].fraction;
            MrEnergyDelta value = MrMakeEnergyDelta();
            value.whole = cell.energy;
            value.fraction = FractionEnergy(nest, fractionSlot, index);
            value = MrAddEnergyDelta(value, delta);

            cell.energy = value.whole;
            if (fractionSlot == MR_NO_FRACTION) {
                FX_ASSERT(value.fraction == 0);
                return;
            }

            MrFraction fraction = FractionAt(nest, fractionSlot, index);
            fraction.energy = value.fraction;
            SetFraction(nest, fractionSlot, index, fraction);
        }

        std::vector<uint8_t> ExpandForStep(MultiresNest& nest, std::span<BlockStepResult> results) {
            const auto blockCount = static_cast<uint32_t>(nest.blocks.size());
            std::vector<uint8_t> frozen(blockCount, 0);
            for (uint32_t slot = 0; slot < blockCount; ++slot)
                frozen[slot] = nest.blocks[slot].page == MR_PAGE_WANTED ? 1 : 0;

            for (const uint32_t slot : ExpandWantedPages(nest)) {
                frozen[slot] = 0;
                results[slot].expanded = true;
            }

            return frozen;
        }

        std::vector<BlockStepResult> StepBlocks(MultiresNest& nest, const ReactionTableView& view,
                                                std::span<uint8_t> stepped, uint64_t worldSeed, uint64_t tick,
                                                const MultiresStepOptions& options, uint32_t wakeMark) {
            const auto blockCount = static_cast<uint32_t>(nest.blocks.size());
            std::vector<BlockStepResult> results(blockCount);

            ResolvePokes(nest, tick);

            // --- 一様なブロック: 反応が進む時だけ頁に広げる(T-0102)。伝導の流れがある時も(T-0019)---
            for (uint32_t slot = 0; slot < blockCount; ++slot) {
                MrBlock& block = nest.blocks[slot];
                if (stepped[slot] == 0 || !MrIsUniform(block))
                    continue;

                // --- 待ちの丸め(T-0115): 起こす刻みが来た時だけ評価する(その前はどのセルも変わらない)---
                if (MrChangeMark(tick) < block.wakeTick)
                    continue;

                const MrUniformWait wait = MrUniformWaitOf(view, UniformAt(nest, slot), worldSeed, tick, block);
                if (wait.changed != 0) {
                    block.page = MR_PAGE_WANTED;
                    continue;
                }

                results[slot].evaluated = true;
                results[slot].wakeTick = wait.wakeTick;
            }

            // --- 伝導(小刻みに分けるなら最後の 1 回の手前まで。T-0108)。枠の順に頁を配り、足りなかったブロックはその回は凍らせる
            //     (刻まず、面の流れも 0)---
            std::vector<MrEnergyDelta> deltas;
            if (options.conduction)
                deltas = StepConduction(nest, view, stepped, options, wakeMark, results);
            else
                ExpandForStep(nest, results);

            // --- 頁のブロック: 伝導の変化(最後の小刻みの分)を足してから反応(凍らせたブロックは一様のまま)---
            for (uint32_t slot = 0; slot < blockCount; ++slot) {
                if (MrIsUniform(nest.blocks[slot]))
                    continue;

                const BlockStepResult earlier = results[slot];
                results[slot] = StepPagedBlock(nest, view, slot, worldSeed, tick, stepped[slot] != 0, deltas);
                results[slot].expanded = earlier.expanded;
                results[slot].changed = results[slot].changed || earlier.changed;
            }

            return results;
        }

        void ResolvePokes(MultiresNest& nest, uint64_t tick) {
            for (MrBlock& block : nest.blocks) {
                if (block.busyTick != MR_BUSY_POKED)
                    continue;

                // つつかれた刻みの印(静かさの判定は今までどおり)。評価は「刻みの直前に変わった」として(MrStepCellWait)
                block.busyTick = MrChangeMark(tick);
                block.wakeTick = 0;
            }
        }

        void RecordWaitResults(MultiresNest& nest, std::span<const BlockStepResult> results, uint64_t tick) {
            const uint64_t mark = MrChangeMark(tick);
            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                const BlockStepResult& result = results[slot];
                MrBlock& block = nest.blocks[slot];
                if (result.changed || result.expanded || block.busyTick == mark) {
                    // 変わった・つつかれた: 次の刻みは新しい tc で引き直すので、評価が要る(伝導の面の隣も起こす)
                    block.busyTick = mark;
                    block.wakeTick = mark + 1;
                } else if (result.evaluated) {
                    block.wakeTick = result.wakeTick;
                }
            }
        }

        std::vector<uint32_t> ExpandWantedPages(MultiresNest& nest) {
            std::vector<uint32_t> expanded;
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                MrBlock& block = nest.blocks[slot];
                if (block.page != MR_PAGE_WANTED)
                    continue;

                // --- 頁が足りなければ、この刻みは刻まずに種に残す ---
                if (nest.counters[MR_COUNTER_FREE_PAGES] == 0) {
                    block.page = MR_NO_PAGE;
                    nest.counters[MR_COUNTER_PAGE_SHORTAGE] += 1;
                    nest.seeds[slot] = 1;
                    continue;
                }

                block.page = PopPage(nest);
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                    PageCellAt(nest, block.page, index) = MrUniformCell(block, UniformAt(nest, slot), index);

                nest.counters[MR_COUNTER_EXPANDED] += 1;
                expanded.push_back(slot);
            }

            return expanded;
        }

    }  // namespace nest_detail

    RxCell LoadNestCell(const MultiresNest& nest, uint32_t slot, uint32_t index) {
        const MrBlock& block = nest.blocks[slot];
        if (MrIsUniform(block))
            return MrUniformCell(block, UniformAt(nest, slot), index);

        return nest.cells[nest.blocks.size() + (size_t{block.page} * MR_BLOCK_CELLS) + index];
    }

    uint32_t UsedWorldPages(const MultiresNest& nest) {
        return nest.capacity.pages - nest.counters[MR_COUNTER_FREE_PAGES];
    }

    MultiresNest MakeMultiresNest(const MultiresCapacity& capacity) {
        FX_ASSERT(capacity.indexEntries == 0 || (capacity.indexEntries & (capacity.indexEntries - 1)) == 0);
        FX_ASSERT(capacity.ledgerColumns >= 1);
        const size_t blockCount = size_t{capacity.worldBlocks} + capacity.observerBlocks;
        const size_t pageCount = size_t{capacity.observerBlocks} + capacity.pages;

        MultiresNest nest;
        nest.capacity = capacity;
        nest.blocks.assign(blockCount, MrMakeUnusedBlock());
        nest.cells.assign(blockCount + (pageCount * MR_BLOCK_CELLS), RxMakeEmptyCell(0));
        nest.fractions.assign(size_t{capacity.fractions} * MR_BLOCK_CELLS, MrMakeEmptyFraction());
        nest.ledger.assign(size_t{MR_LEDGER_LEVELS} * capacity.ledgerColumns, 0);
        nest.index.assign(capacity.indexEntries, MR_INDEX_EMPTY);
        nest.requests.assign(MR_MAX_REQUESTS, MrMakeRequest(0, 0, 0, 0, 0));
        nest.states.assign(MR_MAX_REQUESTS, MrMakeRequestState());
        nest.claims.assign(capacity.worldBlocks, MR_NO_CLAIM);
        nest.seeds.assign(capacity.worldBlocks, 0);

        // --- 空きのスタック: 上(最後)から 0, 1, 2… と取れるように積む。世界の頁は観察の枠の頁の後ろの番号 ---
        nest.freeBlocks.resize(size_t{capacity.worldBlocks} + capacity.pages);
        for (uint32_t i = 0; i < capacity.worldBlocks; ++i)
            nest.freeBlocks[i] = capacity.worldBlocks - 1 - i;

        for (uint32_t i = 0; i < capacity.pages; ++i)
            nest_detail::FreePageAt(nest, i) = capacity.observerBlocks + capacity.pages - 1 - i;

        nest.freeFractions.resize(capacity.fractions);
        for (uint32_t i = 0; i < capacity.fractions; ++i)
            nest.freeFractions[i] = capacity.fractions - 1 - i;

        nest.counters[MR_COUNTER_FREE_BLOCKS] = capacity.worldBlocks;
        nest.counters[MR_COUNTER_FREE_FRACTIONS] = capacity.fractions;
        nest.counters[MR_COUNTER_FREE_PAGES] = capacity.pages;

        return nest;
    }

    void PlaceMirrorBlock(MultiresNest& nest, uint32_t slot, int32_t level, int64_t originX, int64_t originY,
                          int64_t originZ, std::span<const RxCell> cells) {
        FX_ASSERT(cells.size() == MR_BLOCK_CELLS);
        FX_ASSERT(IsObserverSlot(nest, slot));
        const uint32_t page = MrObserverPage(slot, nest.capacity.worldBlocks);
        nest.blocks[slot] = MrMakeMirrorBlock(level, originX, originY, originZ, page);
        ClearPageOverflow(nest, page);
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
            PageCellAt(nest, page, index) = cells[index];
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

    void StepNest(MultiresNest& nest, const BakedReactionTable& table, uint64_t worldSeed, uint64_t tick,
                  const MultiresStepOptions& options) {
        std::vector<uint8_t> stepped(nest.blocks.size(), 0);
        for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
            const uint32_t kind = nest.blocks[slot].kind;
            stepped[slot] = kind != MR_BLOCK_UNUSED && kind != MR_BLOCK_MIRROR ? 1 : 0;
        }

        const std::vector<nest_detail::BlockStepResult> results = nest_detail::StepBlocks(nest, table.View(), stepped,
                                                                                          worldSeed, tick, options, 0);
        nest_detail::RecordWaitResults(nest, results, tick);
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

                hash = FxHashCombine(hash, HashReactionCell(LoadNestCell(nest, slot, index)));
                hash = FxHashCombine(hash, HashFraction(FractionAt(nest, block.fraction, index)));
                hash = HashCellOverflow(hash, nest, slot, index);
            }
        }

        return hash;
    }

    uint64_t HashWholeNest(const MultiresNest& nest) {
        uint64_t hash = 0;
        for (const MrBlock& block : nest.blocks) {
            hash = FxHashCombine(hash, HashBlock(block));
            hash = FxHashCombine(hash, block.activeTick);
            hash = FxHashCombine(hash, block.busyTick);
            hash = FxHashCombine(hash, block.wakeTick);
            hash = FxHashCombine(hash, block.page);
            hash = FxHashCombine(hash, block.quietCheck);
        }

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

        // --- 溢れ(T-0187。使う世界だけ。使わない世界の要約は前と同じ)---
        if (nest.wideCells) {
            for (const MultiresOverflowArea& area : nest.cellOverflow)
                hash = HashOverflowArea(hash, area);

            for (const MultiresOverflowArea& area : nest.fractionOverflow)
                hash = HashOverflowArea(hash, area);
        }

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

                if (nest.wideCells) {
                    AddCellTotals(totals, table, LoadWideNestCell(nest, slot, index),
                                  LoadWideFraction(nest, block.fraction, index), shiftBits);
                    continue;
                }

                const MrFraction fraction = FractionAt(nest, block.fraction, index);
                AddCellTotals(totals, table, LoadNestCell(nest, slot, index), fraction, shiftBits);
            }
        }

        AddLedgerTotals(totals, nest, table, finestLevel);

        return totals;
    }

}  // namespace bicameral::sim
