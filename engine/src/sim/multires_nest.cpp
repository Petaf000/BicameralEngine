// multires_nest.cpp — 多重解像度の木の CPU リファレンス(multires_nest.h)のうち、観察の枠(影・写し)・刻み・要約・保存量の合計。
// 世界の木の管理(索引・空きのスタック・要求・帳簿)は multires_tree.cpp。
// 1 段ぶんの操作(RefineShadowLevel・PullBackLevel)は、GPU のノード(shaders/sim/multires_graph.hlsl)の 1 グループと同じ順。
#include "sim/multires_nest.h"

#include <algorithm>
#include <concepts>
#include <ranges>
#include <span>

#include "sim/multires_fields.h"
#include "sim/multires_nest_internal.h"
#include "sim/multires_state.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        using nest_detail::FractionAt;
        using nest_detail::LoadCellAt;
        using nest_detail::StoreCellAt;
        using nest_detail::StorePageCell;
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
                    StorePageCell(nest, page, index, LoadNestCell(nest, parentSlot, parentCell));
            }

            // --- 登録した欄も子は親と同じ数(親は変えない。T-0761)---
            nest_fields::RefineShadow(nest, parentSlot, octant, childSlot, page);

            nest.blocks[childSlot] = MrMakeChildBlock(parent, parentSlot, octant, MR_BLOCK_SHADOW, MR_NO_FRACTION,
                                                      page);
        }

        // 溢れを使う世界の 1 段(上限の無い形で引き戻す。親か影に溢れがあっても成分を落とさない。T-0199)。
        // 溢れが要らない所はインラインの形と同じ結果(MrPullBackShadowOf は形によらず同じ計算)
        void PullBackWideLevel(MultiresNest& nest, const ReactionTableView& table, uint32_t shadowSlot) {
            const MrBlock shadow = nest.blocks[shadowSlot];
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                const RxWideCell parentCell = LoadWideNestCell(nest, shadow.parent,
                                                               MrOctantCell(shadow.parentOctant, local));
                MrWideShadowFamily family;
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                    family.cells[j] = LoadWideNestCell(nest, shadowSlot, MrChildCell(local, j));

                const MrWideShadowFamily pulled = MrPullBackShadowOf(table, parentCell, family);
                nest.counters[MR_COUNTER_SHADOW_CLAMPED] += pulled.clamped;
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                    StoreWidePageCell(nest, shadow.page, MrChildCell(local, j), pulled.cells[j]);
            }
        }

        // インラインの形の 1 段(GPU の multires_graph.hlsl と同じ)
        void PullBackInlineLevel(MultiresNest& nest, const ReactionTableView& table, uint32_t shadowSlot) {
            const MrBlock shadow = nest.blocks[shadowSlot];
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                const RxCell parentCell = LoadNestCell(nest, shadow.parent, MrOctantCell(shadow.parentOctant, local));
                MrShadowFamily family{};
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                    family.cells[j] = LoadCellAt(nest, shadowSlot, MrChildCell(local, j));

                const MrShadowFamily pulled = MrPullBackShadow(table, parentCell, family);
                nest.counters[MR_COUNTER_SHADOW_CLAMPED] += pulled.clamped;
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                    StoreCellAt(nest, shadowSlot, MrChildCell(local, j), pulled.cells[j]);
            }
        }

        void PullBackLevel(MultiresNest& nest, const ReactionTableView& table, uint32_t shadowSlot) {
            FX_ASSERT(nest.blocks[shadowSlot].kind == MR_BLOCK_SHADOW);
            if (nest.wideCells)
                PullBackWideLevel(nest, table, shadowSlot);
            else
                PullBackInlineLevel(nest, table, shadowSlot);

            // --- 登録した欄: 子 2³ の和を親 × 8 に(T-0761。ADR-0361)---
            nest_fields::PullBackShadow(nest, shadowSlot);

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

        // 帳簿の 1 つの列(0 = エネルギー、1 + 物質 ID、fieldBase から登録した保存量の欄の値)の量を合計に足す
        void AddLedgerColumn(ConservedTotals& totals, const BakedReactionTable& table, uint32_t column,
                             uint32_t fieldBase, const Wide256& amount) {
            if (column >= fieldBase) {
                AddTo(totals.fields[column - fieldBase], amount);
                return;
            }

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

        // 2 の補数で符号を変える
        Wide256 Negate(const Wide256& value) {
            Wide256 result{};
            uint64_t carry = 1;
            for (size_t i = 0; i < value.size(); ++i) {
                result[i] = ~value[i] + carry;
                carry = (carry != 0 && result[i] == 0) ? 1u : 0u;
            }

            return result;
        }

        // 帳簿の全部の欄を「− (入 − 出)」で足す(出 = セルから減った量を足し戻す・入 = セルに増えた量を引く。T-0712)
        void AddLedgerTotals(ConservedTotals& totals, const MultiresNest& nest, const BakedReactionTable& table,
                             int32_t finestLevel) {
            const uint32_t columns = nest.capacity.ledgerColumns;
            const uint32_t fieldBase = NestFieldLedgerBase(nest);
            for (uint32_t slot = 0; slot < WL_SLOT_COUNT; ++slot) {
                for (uint32_t row = 0; row < MR_LEDGER_LEVELS; ++row) {
                    const int32_t level = MR_LEDGER_LEVEL_MIN + static_cast<int32_t>(row);
                    for (uint32_t column = 0; column < columns; ++column) {
                        const uint64_t value = nest.ledger[WlAddress(slot, level, column, columns)];
                        if (value == 0)
                            continue;

                        const Wide256 amount = LedgerValue(value, level, finestLevel);
                        AddLedgerColumn(totals, table, column, fieldBase, WlSlotIsIn(slot) ? Negate(amount) : amount);
                    }
                }
            }
        }

        // 本物の刻むセルの、登録した保存量の欄の値の合計(値 × 8^(finestLevel − レベル)。T-0762)
        void AddFieldTotals(ConservedTotals& totals, const MultiresNest& nest, int32_t finestLevel) {
            for (uint32_t f = 0, first = 0; f < nest.fields.size(); ++f) {
                const MultiresField& field = nest.fields[f];
                if (!state_field::SfConserved(field.desc))
                    continue;

                for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                    const MrBlock& block = nest.blocks[slot];
                    if (block.kind != MR_BLOCK_REAL)
                        continue;

                    const auto shiftBits = static_cast<uint32_t>(finestLevel - block.level) * MR_LEVEL_SHIFT;
                    for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                        if (!MrIsSteppedCell(block, index))
                            continue;

                        for (uint32_t c = 0; c < field.desc.components; ++c) {
                            const int64_t value = LoadNestField(nest, f, slot, index, c);
                            AddTo(totals.fields[first + c],
                                  ShiftLeft(MakeWide256(static_cast<uint64_t>(value), 0, value < 0), shiftBits));
                        }
                    }
                }

                first += field.desc.components;
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

        // --- 状態の区画の要約(HashWholeNest。区画の一覧の順に。T-0711 より前の HashWholeNest と同じ並び・同じ値)---

        // 見出し(活性と忙しさの印も)
        uint64_t HashRegion(uint64_t hash, const MultiresNest&, const std::vector<MrBlock>& blocks);

        // セル(T-0931): GPU と同じ並びの列(一様の値 → 頁ごとの 512 セル)の順・同じ値で要約する(前の RxCell の密な並びと同じ値)
        uint64_t HashRegion(uint64_t hash, const MultiresNest& nest, const MultiresCellStore& cells) {
            for (const RxCell& cell : cells.uniform)
                hash = FxHashCombine(hash, HashReactionCell(cell));

            for (uint32_t page = 0; page < cells.pages.size(); ++page) {
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                    hash = FxHashCombine(hash, HashReactionCell(nest_detail::LoadPageCell(nest, page, index)));
            }

            // 成分ごとの層(T-1210。0 でない層だけ。層の無い世界はここで何も足さない)
            for (uint32_t page = 0; page < cells.pages.size(); ++page) {
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const uint64_t address = (uint64_t{page} * MR_BLOCK_CELLS) + index;
                    hash = nest_detail::HashCellLayers(hash, nest, page, index, address);
                }
            }

            return hash;
        }

        uint64_t HashRegion(uint64_t hash, const MultiresNest&, const std::vector<MrFraction>& fractions);

        uint64_t HashRegion(uint64_t hash, const MultiresNest&, const std::vector<MultiresOverflowArea>& areas);

        // 一様の値の溢れ(T-0199。使っている一様のブロックだけ。頁を持つ枠・空きの枠に残った古い値は読まないので入れない)
        uint64_t HashRegion(uint64_t hash, const MultiresNest& nest,
                            const std::vector<MultiresUniformOverflow>& tails) {
            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                if (!MrIsUniform(nest.blocks[slot]) || nest.blocks[slot].kind == MR_BLOCK_UNUSED)
                    continue;

                const MultiresUniformOverflow& tail = tails[slot];
                hash = FxHashCombine(hash, tail.species.size());
                for (size_t i = 0; i < tail.species.size(); ++i) {
                    hash = FxHashCombine(hash, tail.species[i]);
                    hash = FxHashCombine(hash, tail.amounts[i]);
                }
            }

            return hash;
        }

        // 整数の並び(空きのスタック・帳簿・数える欄・登録した欄。符号付きは 2 の補数のまま)
        template <typename Values>
            requires std::integral<std::ranges::range_value_t<Values>>
        uint64_t HashRegion(uint64_t hash, const MultiresNest&, const Values& values) {
            for (const auto value : values)
                hash = FxHashCombine(hash, static_cast<uint64_t>(value));

            return hash;
        }

        // 登録した欄の置き場(T-0921): 密の並び(一様の値 → セルの頁ごとの値。疎で無い頁は 0)で要約する。疎と密で同じ値。
        // 並びは T-0921 より前と同じ。違うのは返したセルの頁の欄が 0 になったこと(前は古い値が残って要約に入っていた)
        uint64_t HashRegion(uint64_t hash, const MultiresNest&, const MultiresFieldStore& store) {
            for (const int64_t value : store.uniform)
                hash = FxHashCombine(hash, static_cast<uint64_t>(value));

            const size_t pageValues = size_t{MR_BLOCK_CELLS} * store.components;
            for (uint32_t page = 0; page < store.cellPages; ++page) {
                const int64_t* values = FieldPageValues(store, page);
                for (size_t i = 0; i < pageValues; ++i)
                    hash = FxHashCombine(hash, values == nullptr ? 0 : static_cast<uint64_t>(values[i]));
            }

            return hash;
        }

        // 要約に入る欄か(truthOnly なら真実の欄だけ = 温めた始まりも除く。T-0854)
        bool FieldInHash(const MultiresField& field, bool truthOnly) {
            return truthOnly ? state_field::SfTruthHashed(field.desc) : state_field::SfHashed(field.desc);
        }

        // 本物の葉のセル index の登録した欄(ハッシュに入るセル・面の欄。欄が無ければ hash のまま。T-0711)
        uint64_t HashCellFields(uint64_t hash, const MultiresNest& nest, uint32_t slot, uint32_t index,
                                bool truthOnly) {
            for (uint32_t f = 0; f < nest.fields.size(); ++f) {
                const MultiresField& field = nest.fields[f];
                if (!FieldInHash(field, truthOnly) || !state_field::SfOnCells(field.desc))
                    continue;

                for (uint32_t c = 0; c < field.desc.components; ++c)
                    hash = FxHashCombine(hash, static_cast<uint64_t>(LoadNestField(nest, f, slot, index, c)));
            }

            return hash;
        }

        // 本物のブロック slot のブロックの欄と、世界の欄(slot = MR_NO_BLOCK)の要約
        uint64_t HashOtherFields(uint64_t hash, const MultiresNest& nest, uint32_t slot, bool truthOnly) {
            const uint32_t place = slot == MR_NO_BLOCK ? state_field::SF_PLACE_WORLD : state_field::SF_PLACE_BLOCK;
            for (const MultiresField& field : nest.fields) {
                if (!FieldInHash(field, truthOnly) || field.desc.place != place)
                    continue;

                const size_t first = slot == MR_NO_BLOCK ? 0 : size_t{slot} * field.desc.components;
                for (uint32_t c = 0; c < field.desc.components; ++c)
                    hash = FxHashCombine(hash, static_cast<uint64_t>(field.store.uniform[first + c]));
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

        uint64_t HashRegion(uint64_t hash, const MultiresNest&, const std::vector<MrBlock>& blocks) {
            for (const MrBlock& block : blocks) {
                hash = FxHashCombine(hash, HashBlock(block));
                hash = FxHashCombine(hash, block.activeTick);
                hash = FxHashCombine(hash, block.busyTick);
                hash = FxHashCombine(hash, block.wakeTick);
                hash = FxHashCombine(hash, block.page);
                hash = FxHashCombine(hash, block.quietCheck);
            }

            return hash;
        }

        uint64_t HashRegion(uint64_t hash, const MultiresNest&, const std::vector<MrFraction>& fractions) {
            for (const MrFraction& fraction : fractions)
                hash = FxHashCombine(hash, HashFraction(fraction));

            return hash;
        }

        uint64_t HashRegion(uint64_t hash, const MultiresNest&, const std::vector<MultiresOverflowArea>& areas) {
            for (const MultiresOverflowArea& area : areas)
                hash = HashOverflowArea(hash, area);

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

                RxCell cell = LoadCellAt(nest, slot, index);
                const RxCell before = cell;
                const uint64_t fractionBefore = FractionEnergy(nest, block.fraction, index);

                // --- 伝導 ---
                if (!deltas.empty()) {
                    const MrEnergyDelta& delta = deltas[nest_detail::PageCellAddress(nest, block.page, index)];
                    if (!MrEnergyDeltaIsZero(delta)) {
                        nest_detail::ApplyEnergyDelta(nest, slot, index, cell, delta);
                        StoreCellAt(nest, slot, index, cell);
                    }
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
                    cell = LoadCellAt(nest, slot, index);
                } else if (react) {
                    const RxWaitStep step = MrStepCellWait(view, cell, worldSeed, tick, block, index);
                    result.wakeTick = std::min(result.wakeTick, step.wakeTick);
                    cell = step.cell;
                    StoreCellAt(nest, slot, index, cell);
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
                                                std::span<const CvSpeciesPhase> phases, std::span<uint8_t> stepped,
                                                uint64_t worldSeed, uint64_t tick, const MultiresStepOptions& options,
                                                uint32_t wakeMark) {
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

                // (一様の値に溢れがあれば溢れまで読む。T-0199。溢れの無い一様の値はインラインの形のまま = GPU の溢れの世界と同じ。
                //  溢れを使う世界では 9 種目の生成物を待たせたら頁に広げ、上限の無い形で刻む = 待たせない。T-0276)
                const bool wideValue = nest.wideCells && !nest.uniformOverflow[slot].species.empty();
                const MrUniformWait wait = wideValue ? MrUniformWaitOf(view, LoadWideUniform(nest, slot), worldSeed,
                                                                       tick, block, true)
                                                     : MrUniformWaitOf(view, UniformAt(nest, slot), worldSeed, tick,
                                                                       block, nest.wideCells);
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
                deltas = StepConduction(nest, view, phases, stepped, options, wakeMark, results);
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

                // --- 一様の値で埋める(溢れを使う世界は一様の値の溢れごと。返された頁の溢れは空。T-0199)---
                const uint32_t page = PopPage(nest);
                nest_fields::ExpandPage(nest, slot, page);
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    if (nest.wideCells)
                        StoreWidePageCell(nest, page, index, LoadWideNestCell(nest, slot, index));
                    else
                        StorePageCell(nest, page, index, MrUniformCell(block, UniformAt(nest, slot), index));
                }

                block.page = page;

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

        return nest_detail::LoadPageCell(nest, block.page, index);
    }

    bool ExpandNestPage(MultiresNest& nest, uint32_t slot) {
        FX_ASSERT(slot < nest.capacity.worldBlocks);
        MrBlock& block = nest.blocks[slot];
        if (!MrIsUniform(block))
            return true;

        if (nest.counters[MR_COUNTER_FREE_PAGES] == 0)
            return false;

        // 刻みの外では頁に広げる途中の枠は無いので、広げるのはこの枠だけ(数え方も刻みの中と同じ)
        block.page = MR_PAGE_WANTED;
        const std::vector<uint32_t> expanded = nest_detail::ExpandWantedPages(nest);
        FX_ASSERT(expanded.size() == 1 && expanded[0] == slot);

        return !expanded.empty();
    }

    void StoreNestCell(MultiresNest& nest, uint32_t slot, uint32_t index, const RxCell& cell) {
        FX_ASSERT(!nest.wideCells);
        StoreCellAt(nest, slot, index, cell);
    }

    void PokeNestBlock(MultiresNest& nest, uint32_t slot) {
        nest_detail::PokeBlock(nest, slot);
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
        nest.cells.uniform.assign(blockCount, RxMakeEmptyCell(0));
        nest.cells.pages.assign(pageCount, MultiresCellPage{});
        nest.fractions.assign(size_t{capacity.fractions} * MR_BLOCK_CELLS, MrMakeEmptyFraction());
        nest.ledger.assign(WlLedgerWords(capacity.ledgerColumns), 0);
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
            StorePageCell(nest, page, index, cells[index]);

        // 登録した欄は 0 から(前の影・写しの値を残さない。写す元の欄は呼ぶ側が StoreNestField で書く。T-0761)
        nest_fields::PlaceRoot(nest, slot);
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
            nest_fields::ReleaseBlockPages(nest, firstSlot + i);  // 影の欄の頁も返す(疎。T-0921)
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

        // 伝導が読む見立ての相の表(表を差し替えても刻みごとに作るので後始末は要らない。T-0714)
        const std::vector<CvSpeciesPhase> phases = MakeSpeciesPhases(table);
        const std::vector<nest_detail::BlockStepResult> results = nest_detail::StepBlocks(
            nest, table.View(), phases, stepped, worldSeed, tick, options, 0);
        nest_detail::RecordWaitResults(nest, results, tick);
    }

    void PullBackShadowChain(MultiresNest& nest, const BakedReactionTable& table, uint32_t firstShadowSlot,
                             uint32_t levelCount) {
        const ReactionTableView view = table.View();
        for (uint32_t i = 0; i < levelCount; ++i)
            PullBackLevel(nest, view, firstShadowSlot + i);
    }

    namespace {

        // 本物の葉の要約(truthOnly なら真実の欄だけ。T-0854)
        uint64_t HashLeaves(const MultiresNest& nest, bool truthOnly) {
            uint64_t hash = 0;
            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                const MrBlock& block = nest.blocks[slot];
                if (block.kind != MR_BLOCK_REAL)
                    continue;

                hash = FxHashCombine(hash, HashBlock(block));
                hash = HashOtherFields(hash, nest, slot, truthOnly);
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    if (!MrIsSteppedCell(block, index))
                        continue;

                    hash = FxHashCombine(hash, HashReactionCell(LoadNestCell(nest, slot, index)));
                    hash = FxHashCombine(hash, HashFraction(FractionAt(nest, block.fraction, index)));
                    hash = HashCellOverflow(hash, nest, slot, index);
                    if (!MrIsUniform(block))
                        hash = nest_detail::HashCellLayers(hash, nest, block.page, index, index);  // T-1210
                    hash = HashCellFields(hash, nest, slot, index, truthOnly);
                }
            }

            return HashOtherFields(hash, nest, MR_NO_BLOCK, truthOnly);
        }

    }  // namespace

    uint64_t HashRealLeaves(const MultiresNest& nest) {
        return HashLeaves(nest, false);
    }

    uint64_t HashTruthLeaves(const MultiresNest& nest) {
        return HashLeaves(nest, true);
    }

    uint64_t HashWholeNest(const MultiresNest& nest) {
        uint64_t hash = 0;
        VisitNestState(nest, [&](const NestStateRegion& region, const auto& values) {
            if (region.hashed)
                hash = HashRegion(hash, nest, values);
        });

        return hash;
    }

    // --- 世界の帳簿(T-0712)---

    void AddWorldLedgerBits(MultiresNest& nest, uint32_t slot, int32_t level, uint32_t column, uint64_t bits) {
        for (; bits != 0; --level) {
            const uint32_t address = WlAddress(slot, level, column, nest.capacity.ledgerColumns);
            if (address == MR_NO_BLOCK) {
                nest.counters[MR_COUNTER_LEDGER_OUTSIDE] += 1;
                return;
            }

            uint64_t& value = nest.ledger[address];
            value += bits;
            bits = value < bits ? WlCarryBits() : 0;
        }
    }

    void AddWorldLedgerUnits(MultiresNest& nest, uint32_t slot, int32_t level, uint32_t column, uint64_t units) {
        const WlWholeSplit split = WlSplitWholeUnits(units);
        AddWorldLedgerBits(nest, slot, level - WL_WHOLE_LOW_LEVELS, column, split.low);
        AddWorldLedgerBits(nest, slot, level - WL_WHOLE_HIGH_LEVELS, column, split.high);
    }

    void AddWorldLedgerCell(MultiresNest& nest, uint32_t reason, uint32_t direction, int32_t level,
                            const RxCell& cell) {
        const WlSignedEntry energy = WlEnergyEntry(cell.energy, direction);
        AddWorldLedgerUnits(nest, WlSlot(reason, energy.direction), level, WL_COLUMN_ENERGY, energy.magnitude);

        const uint32_t slot = WlSlot(reason, direction);
        for (uint32_t i = 0; i < cell.speciesCount; ++i)
            AddWorldLedgerUnits(nest, slot, level, WL_COLUMN_SPECIES_BASE + cell.species[i], cell.amounts[i]);
    }

    uint32_t NestConservedComponents(const MultiresNest& nest) {
        uint32_t components = 0;
        for (const MultiresField& field : nest.fields) {
            if (state_field::SfConserved(field.desc))
                components += field.desc.components;
        }

        return components;
    }

    uint32_t NestFieldLedgerBase(const MultiresNest& nest) {
        return nest.capacity.ledgerColumns - NestConservedComponents(nest);
    }

    int64_t WorldLedgerNetUnits(const MultiresNest& nest, uint32_t reason, uint32_t column, int32_t level) {
        const uint32_t columns = nest.capacity.ledgerColumns;
        Wide256 net{};
        for (uint32_t direction = 0; direction < 2; ++direction) {
            const uint32_t slot = WlSlot(reason, direction);
            for (uint32_t row = 0; row < MR_LEDGER_LEVELS; ++row) {
                const int32_t rowLevel = MR_LEDGER_LEVEL_MIN + static_cast<int32_t>(row);
                const uint32_t address = WlAddress(slot, rowLevel, column, columns);
                if (address == MR_NO_BLOCK || nest.ledger[address] == 0)
                    continue;

                FX_ASSERT(rowLevel <= level);
                if (rowLevel > level)
                    continue;

                const Wide256 amount = LedgerValue(nest.ledger[address], rowLevel, level);
                AddTo(net, WlSlotIsIn(slot) ? amount : Negate(amount));
            }
        }

        // net = (入 − 出) × 2^64(2 の補数)。丸ごとの数は 2 語目
        return static_cast<int64_t>(net[1]);
    }

    ConservedTotals ComputeConservedTotals(const MultiresNest& nest, const BakedReactionTable& table,
                                           int32_t finestLevel) {
        ConservedTotals totals;
        totals.elements.assign(table.elementNames.size(), Wide256{});
        totals.fields.assign(NestConservedComponents(nest), Wide256{});
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

        AddFieldTotals(totals, nest, finestLevel);
        AddLedgerTotals(totals, nest, table, finestLevel);

        return totals;
    }

}  // namespace bicameral::sim
