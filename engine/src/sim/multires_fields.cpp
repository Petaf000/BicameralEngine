// multires_fields.cpp — 登録した欄の置き場と、木の操作に欄を付いて行かせる所(T-0711。ADR-0329。multires_fields.h)。
// 細かく/粗くする時の値は shaders/common/state_field.hlsli の関数(GPU と同じ)で決める。浮動小数点は使わない。
#include <algorithm>
#include <utility>

#include "common/multires_tree.hlsli"
#include "sim/multires_fields.h"

using namespace bicameral::multires;
using namespace bicameral::state_field;

namespace bicameral::sim {

    namespace {

        // --- 値の番号 ---

        // セル・面の欄: 一様の値は枠 slot、頁のセルは「枠の数 + 頁 × 512 + セル」(nest.cells と同じ並び)× 値の数
        size_t UniformAddress(const MultiresField& field, uint32_t slot, uint32_t component) {
            return (size_t{slot} * field.desc.components) + component;
        }

        size_t PageAddress(const MultiresNest& nest, const MultiresField& field, uint32_t page, uint32_t index,
                           uint32_t component) {
            const size_t cell = nest.blocks.size() + (size_t{page} * MR_BLOCK_CELLS) + index;

            return (cell * field.desc.components) + component;
        }

        size_t ValueCount(const MultiresNest& nest, const SfFieldDesc& desc) {
            if (SfOnCells(desc))
                return nest.cells.size() * desc.components;

            if (desc.place == SF_PLACE_BLOCK)
                return nest.blocks.size() * desc.components;

            return desc.components;
        }

        // 粗くした余りを欄の帳簿の (レベル, 値) へ。帳簿の外なら数える(セルの帳簿と同じ扱い)
        void AddFieldLedger(MultiresNest& nest, MultiresField& field, int32_t level, uint32_t component,
                            uint32_t remainder) {
            if (remainder == 0)
                return;

            const uint32_t address = MrLedgerAddress(level, component, field.desc.components);
            if (address == MR_NO_BLOCK) {
                nest.counters[MR_COUNTER_LEDGER_OUTSIDE] += 1;
                return;
            }

            field.ledger[address] += remainder;
        }

        // 枠 slot(頁を持つ)の欄の頁を畳む時の値(MrFoldValueCell のセル。全部覆われていれば 0)
        int64_t FoldValue(const MultiresNest& nest, const MultiresField& field, uint32_t slot, uint32_t component) {
            const MrBlock& block = nest.blocks[slot];
            const uint32_t valueCell = MrFoldValueCell(block);
            if (valueCell >= MR_BLOCK_CELLS)
                return 0;

            return field.values[PageAddress(nest, field, block.page, valueCell, component)];
        }

    }  // namespace

    uint32_t RegisterNestField(MultiresNest& nest, std::string name, const SfFieldDesc& desc) {
        if (name.empty() || !SfDescValid(desc) || FindNestField(nest, name) != MULTIRES_NO_FIELD)
            return MULTIRES_NO_FIELD;

        // 根を置く前だけ(置いた後に足すと、使っている頁の欄の値が決まらない)
        FX_ASSERT(nest.counters[MR_COUNTER_FREE_BLOCKS] == nest.capacity.worldBlocks);

        MultiresField field;
        field.name = std::move(name);
        field.desc = desc;
        field.values.assign(ValueCount(nest, desc), 0);
        if (desc.scale == SF_SCALE_VOLUME)
            field.ledger.assign(size_t{MR_LEDGER_LEVELS} * desc.components, 0);

        nest.fields.push_back(std::move(field));

        return static_cast<uint32_t>(nest.fields.size() - 1);
    }

    uint32_t FindNestField(const MultiresNest& nest, std::string_view name) {
        for (uint32_t i = 0; i < nest.fields.size(); ++i) {
            if (nest.fields[i].name == name)
                return i;
        }

        return MULTIRES_NO_FIELD;
    }

    // --- セル・面の欄 ---

    int64_t LoadNestField(const MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t index, uint32_t component) {
        const MultiresField& entry = nest.fields[field];
        FX_ASSERT(SfOnCells(entry.desc) && component < entry.desc.components);
        const MrBlock& block = nest.blocks[slot];
        if (MrIsUniform(block))
            return MrIsCoveredCell(block, index) ? 0 : entry.values[UniformAddress(entry, slot, component)];

        return entry.values[PageAddress(nest, entry, block.page, index, component)];
    }

    void StoreNestField(MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t index, uint32_t component,
                        int64_t value) {
        MultiresField& entry = nest.fields[field];
        FX_ASSERT(SfOnCells(entry.desc) && component < entry.desc.components);
        FX_ASSERT(SfValueFits(entry.desc, value));
        FX_ASSERT(!MrIsUniform(nest.blocks[slot]));
        entry.values[PageAddress(nest, entry, nest.blocks[slot].page, index, component)] = value;
    }

    void StoreNestFieldUniform(MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t component, int64_t value) {
        MultiresField& entry = nest.fields[field];
        FX_ASSERT(SfOnCells(entry.desc) && component < entry.desc.components);
        FX_ASSERT(SfValueFits(entry.desc, value));
        FX_ASSERT(MrIsUniform(nest.blocks[slot]));
        entry.values[UniformAddress(entry, slot, component)] = value;
    }

    // --- ブロック・世界の欄 ---

    int64_t LoadNestBlockField(const MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t component) {
        const MultiresField& entry = nest.fields[field];
        FX_ASSERT(entry.desc.place == SF_PLACE_BLOCK && component < entry.desc.components);

        return entry.values[UniformAddress(entry, slot, component)];
    }

    void StoreNestBlockField(MultiresNest& nest, uint32_t field, uint32_t slot, uint32_t component, int64_t value) {
        MultiresField& entry = nest.fields[field];
        FX_ASSERT(entry.desc.place == SF_PLACE_BLOCK && component < entry.desc.components);
        FX_ASSERT(SfValueFits(entry.desc, value));
        entry.values[UniformAddress(entry, slot, component)] = value;
    }

    int64_t LoadNestWorldField(const MultiresNest& nest, uint32_t field, uint32_t component) {
        const MultiresField& entry = nest.fields[field];
        FX_ASSERT(entry.desc.place == SF_PLACE_WORLD && component < entry.desc.components);

        return entry.values[component];
    }

    void StoreNestWorldField(MultiresNest& nest, uint32_t field, uint32_t component, int64_t value) {
        MultiresField& entry = nest.fields[field];
        FX_ASSERT(entry.desc.place == SF_PLACE_WORLD && component < entry.desc.components);
        FX_ASSERT(SfValueFits(entry.desc, value));
        entry.values[component] = value;
    }

    int64_t LoadNestFieldLedger(const MultiresNest& nest, uint32_t field, int32_t level, uint32_t component) {
        const MultiresField& entry = nest.fields[field];
        const uint32_t address = MrLedgerAddress(level, component, entry.desc.components);
        if (entry.ledger.empty() || address == MR_NO_BLOCK)
            return 0;

        return entry.ledger[address];
    }

    // --- 木の操作が呼ぶ ---
    namespace nest_fields {

        void PlaceRoot(MultiresNest& nest, uint32_t slot) {
            const MrBlock& block = nest.blocks[slot];
            for (MultiresField& field : nest.fields) {
                const uint32_t components = field.desc.components;
                if (field.desc.place == SF_PLACE_BLOCK) {
                    std::fill_n(field.values.begin() + static_cast<ptrdiff_t>(UniformAddress(field, slot, 0)),
                                components, 0);
                    continue;
                }

                if (!SfOnCells(field.desc))
                    continue;

                if (MrIsUniform(block)) {
                    std::fill_n(field.values.begin() + static_cast<ptrdiff_t>(UniformAddress(field, slot, 0)),
                                components, 0);
                    continue;
                }

                const size_t first = PageAddress(nest, field, block.page, 0, 0);
                std::fill_n(field.values.begin() + static_cast<ptrdiff_t>(first), size_t{MR_BLOCK_CELLS} * components,
                            0);
            }
        }

        void ExpandPage(MultiresNest& nest, uint32_t slot, uint32_t page) {
            const MrBlock& block = nest.blocks[slot];
            FX_ASSERT(MrIsUniform(block));
            for (MultiresField& field : nest.fields) {
                if (!SfOnCells(field.desc))
                    continue;

                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const bool covered = MrIsCoveredCell(block, index);
                    for (uint32_t c = 0; c < field.desc.components; ++c) {
                        const int64_t value = covered ? 0 : field.values[UniformAddress(field, slot, c)];
                        field.values[PageAddress(nest, field, page, index, c)] = value;
                    }
                }
            }
        }

        bool PageFoldable(const MultiresNest& nest, uint32_t slot) {
            const MrBlock& block = nest.blocks[slot];
            FX_ASSERT(!MrIsUniform(block));
            for (const MultiresField& field : nest.fields) {
                if (!SfOnCells(field.desc))
                    continue;

                for (uint32_t c = 0; c < field.desc.components; ++c) {
                    const int64_t value = FoldValue(nest, field, slot, c);
                    for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                        const int64_t expected = MrIsCoveredCell(block, index) ? 0 : value;
                        if (field.values[PageAddress(nest, field, block.page, index, c)] != expected)
                            return false;
                    }
                }
            }

            return true;
        }

        void FoldPage(MultiresNest& nest, uint32_t slot) {
            for (MultiresField& field : nest.fields) {
                if (!SfOnCells(field.desc))
                    continue;

                for (uint32_t c = 0; c < field.desc.components; ++c)
                    field.values[UniformAddress(field, slot, c)] = FoldValue(nest, field, slot, c);
            }
        }

        void Refine(MultiresNest& nest, uint32_t parentSlot, uint32_t octant, uint32_t childSlot, uint32_t childPage) {
            const MrBlock& parent = nest.blocks[parentSlot];
            const bool uniform = MrIsUniform(parent);
            FX_ASSERT(uniform == (childPage == MR_NO_PAGE));
            for (MultiresField& field : nest.fields) {
                const uint32_t components = field.desc.components;

                // --- ブロックの欄: 子のブロックは親の値 ---
                if (field.desc.place == SF_PLACE_BLOCK) {
                    for (uint32_t c = 0; c < components; ++c)
                        field.values[UniformAddress(field, childSlot, c)] = field.values[UniformAddress(field,
                                                                                                        parentSlot, c)];
                    continue;
                }

                if (!SfOnCells(field.desc))
                    continue;

                // --- セル・面の欄: 子は親と同じ数(一様なら値 1 つ)---
                if (uniform) {
                    for (uint32_t c = 0; c < components; ++c)
                        field.values[UniformAddress(field, childSlot, c)] = SfRefineValue(
                            field.values[UniformAddress(field, parentSlot, c)]);
                    continue;
                }

                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const uint32_t parentCell = MrParentCellOfChild(octant, index);
                    for (uint32_t c = 0; c < components; ++c)
                        field.values[PageAddress(nest, field, childPage, index, c)] = SfRefineValue(
                            field.values[PageAddress(nest, field, parent.page, parentCell, c)]);
                }

                // --- 親を覆う(覆われた親のセルは 0。一様な親は覆われた所を 0 と読むので書かない)---
                for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                    const uint32_t index = MrOctantCell(octant, local);
                    for (uint32_t c = 0; c < components; ++c)
                        field.values[PageAddress(nest, field, parent.page, index, c)] = 0;
                }
            }
        }

        bool CoarsenKeepsUniform(const MultiresNest& nest, uint32_t childSlot, uint32_t parentSlot) {
            FX_ASSERT(MrIsUniform(nest.blocks[parentSlot]));
            if (nest.fields.empty())
                return true;

            // 頁を持つ子は控えめに「広げる」(欄の頁を 64 組ずつ調べる手間を取らない。欄のある世界だけの扱い)
            const bool childUniform = MrIsUniform(nest.blocks[childSlot]);
            for (const MultiresField& field : nest.fields) {
                if (!SfOnCells(field.desc))
                    continue;

                if (!childUniform)
                    return false;

                // 同じ値 8 つの和 ÷ 8 は同じ値・余り 0
                for (uint32_t c = 0; c < field.desc.components; ++c) {
                    if (field.values[UniformAddress(field, childSlot, c)] !=
                        field.values[UniformAddress(field, parentSlot, c)])
                        return false;
                }
            }

            return true;
        }

        void Coarsen(MultiresNest& nest, uint32_t childSlot) {
            const MrBlock& child = nest.blocks[childSlot];
            const MrBlock& parent = nest.blocks[child.parent];
            const bool parentUniform = MrIsUniform(parent);
            for (uint32_t f = 0; f < nest.fields.size(); ++f) {
                MultiresField& field = nest.fields[f];
                if (!SfOnCells(field.desc))
                    continue;  // ブロック・世界の欄: 親は自分の値のまま

                for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                    const uint32_t parentCell = MrOctantCell(child.parentOctant, local);
                    for (uint32_t c = 0; c < field.desc.components; ++c) {
                        SfCoarsenSum sum = SfMakeCoarsenSum();
                        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                            sum = SfAddCoarsenChild(sum, LoadNestField(nest, f, childSlot, MrChildCell(local, j), c));

                        const int64_t value = SfCoarsenValue(sum);
                        if (field.desc.scale == SF_SCALE_VOLUME)
                            AddFieldLedger(nest, field, child.level, c, SfCoarsenRemainder(sum));

                        if (parentUniform) {
                            FX_ASSERT(value == field.values[UniformAddress(field, child.parent, c)]);
                            continue;
                        }

                        field.values[PageAddress(nest, field, parent.page, parentCell, c)] = value;
                    }
                }
            }
        }

    }  // namespace nest_fields

}  // namespace bicameral::sim
