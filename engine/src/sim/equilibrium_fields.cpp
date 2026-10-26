// equilibrium_fields.cpp — 部品の温めた始まりを C-1 の欄に置く口(equilibrium_fields.h。T-0922・ADR-0427)。
#include "sim/equilibrium_fields.h"

#include <utility>

#include "common/state_field.hlsli"
#include "sim/multires_fields.h"

using namespace bicameral::multires;
using namespace bicameral::state_field;

namespace bicameral::sim {

    namespace {

        // 節のセル(ブロックの枠とセルの番号)。ブロックが無ければ slot = MR_NO_BLOCK
        struct NodeCell {
            uint32_t slot = MR_NO_BLOCK;
            uint32_t index = 0;
        };

        // ブロックの原点はそのレベルのセルの単位で 8 の倍数(負の座標も −∞ 向きに揃える)
        NodeCell FindNodeCell(const MultiresNest& nest, const EquilibriumNode& node) {
            constexpr int64_t EDGE_MASK = MR_BLOCK_EDGE - 1;
            const uint32_t slot = LookupBlock(nest, node.level, node.x & ~EDGE_MASK, node.y & ~EDGE_MASK,
                                              node.z & ~EDGE_MASK);

            return {.slot = slot,
                    .index = MrCellIndex((uint32_t)(node.x & EDGE_MASK), (uint32_t)(node.y & EDGE_MASK),
                                         (uint32_t)(node.z & EDGE_MASK))};
        }

    }  // namespace

    uint32_t RegisterWarmStartField(MultiresNest& nest, std::string name, uint32_t phase, uint32_t components,
                                    bool discardOnSleep) {
        SfFieldDesc desc = SfMakeDerivedDesc(SF_PLACE_CELL, SF_TYPE_I64, phase, true, components);
        desc.flags |= discardOnSleep ? SF_FLAG_SLEEP_DISCARD : 0u;

        return RegisterNestField(nest, std::move(name), desc);
    }

    uint32_t StoreWarmStart(const EquilibriumWarmStart& warm, std::span<const EquilibriumNode> nodes,
                            std::span<const int64_t> values, uint32_t perNode) {
        MultiresNest& nest = *warm.nest;
        FX_ASSERT(values.size() == nodes.size() * perNode);
        FX_ASSERT(warm.component + perNode <= nest.fields[warm.field].desc.components);
        uint32_t missed = 0;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const NodeCell cell = FindNodeCell(nest, nodes[i]);
            if (cell.slot == MR_NO_BLOCK || MrIsUniform(nest.blocks[cell.slot]) ||
                MrIsCoveredCell(nest.blocks[cell.slot], cell.index)) {
                ++missed;
                continue;
            }

            for (uint32_t c = 0; c < perNode; ++c)
                StoreNestField(nest, warm.field, cell.slot, cell.index, warm.component + c, values[(i * perNode) + c]);
        }

        return missed;
    }

    std::vector<int64_t> LoadWarmStart(const EquilibriumWarmStart& warm, std::span<const EquilibriumNode> nodes,
                                       uint32_t perNode) {
        const MultiresNest& nest = *warm.nest;
        FX_ASSERT(warm.component + perNode <= nest.fields[warm.field].desc.components);
        std::vector<int64_t> values(nodes.size() * perNode, 0);
        for (size_t i = 0; i < nodes.size(); ++i) {
            const NodeCell cell = FindNodeCell(nest, nodes[i]);
            if (cell.slot == MR_NO_BLOCK)
                continue;

            for (uint32_t c = 0; c < perNode; ++c)
                values[(i * perNode) + c] = LoadNestField(nest, warm.field, cell.slot, cell.index, warm.component + c);
        }

        return values;
    }

    uint64_t WarmStartFieldBytes(const MultiresNest& nest, uint32_t field) {
        const MultiresFieldStore& store = nest.fields[field].store;

        return (store.uniform.size() + store.pages.size()) * sizeof(int64_t) + (store.pageOf.size() * sizeof(uint32_t));
    }

}  // namespace bicameral::sim
