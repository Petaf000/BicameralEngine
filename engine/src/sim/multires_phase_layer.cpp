// multires_phase_layer.cpp — 相の層のエネルギー Ec の欄と、細かく/粗くする時の配り(相の層の配り手。T-1230・T-1231。ADR-0512 の 1・ADR-0500・D-674)。
// データの流れ: EnablePhaseLayerEnergy が欄(SF_OWNER_PHASE_LAYER)を登録し物質の表を木に持たせる → 木の操作のセルの配りの口
// (multires_fields.cpp の RefineOwners / CoarsenOwners)が、頁を持つ親のセルごとにここの RefinePhaseLayerCell / CoarsenPhaseLayerCell を呼ぶ
// (セルの中身の配り〔置かれ方 T-1176 か SAME〕の後)。式は shaders/common/phase_layer.hlsli(GPU と 1 つの源。GPU は T-1252 段 2 の後)。
//   細かく: 親が混ざったセルなら、層の熱と気体の熱を子の凝縮相・気体の熱容量の重みで別々に配り(D-674 を 2 回)、子のエネルギーを書き直す。
//           混ざらない親は子のエネルギーはそのまま、混ざった子だけ温度の揃う Ec(PlEvenCondensedEnergy)。
//   粗く:   親の Ec = Σ 子の凝縮相の分 ÷ 8 の床を、部分の熱が負にならない範囲に収める(混ざらない親は 0)。
// 子が親の写し(置かれ方が一様)なら、子の値は親と同じ(今とビット単位で同じ)。Ec を持たない世界は何も通らない。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#include <array>
#include <span>

#include "common/phase_layer.hlsli"
#include "sim/multires_fields.h"
#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::state_field;

namespace bicameral::sim {

    namespace {

        // phase_layer.hlsli の関数に渡す物質の表(Species だけ読む)
        struct SpeciesTable {
            std::span<const RxSpecies> species;

            [[nodiscard]] RxSpecies Species(uint32_t id) const { return species[id]; }
        };

        // 子 8 つの部分の和(熱容量の和と、熱容量を持つ最後の子)
        struct PartWeights {
            std::array<PlPartSums, MR_CHILDREN_PER_CELL> children{};
            uint64_t condensedCapacity = 0;
            uint64_t gasCapacity = 0;
            uint32_t lastCondensed = MR_CHILDREN_PER_CELL;
            uint32_t lastGas = MR_CHILDREN_PER_CELL;
        };

        PartWeights WeightsOf(const SpeciesTable& table, const std::array<RxCell, MR_CHILDREN_PER_CELL>& children) {
            PartWeights weights;
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                weights.children[j] = PlPartSumsOf(table, children[j]);
                weights.condensedCapacity += weights.children[j].condensedCapacity;
                weights.gasCapacity += weights.children[j].gasCapacity;
                weights.lastCondensed = weights.children[j].condensedCapacity > 0 ? j : weights.lastCondensed;
                weights.lastGas = weights.children[j].gasCapacity > 0 ? j : weights.lastGas;
            }

            return weights;
        }

        // 部分 1 つを子 8 つへ(最後の重みのある子が残り = 子の和 = total。ビット単位)
        std::array<int64_t, MR_CHILDREN_PER_CELL> SplitPart(
            int64_t heat, int64_t total, uint64_t capacitySum, uint32_t last,
            const std::array<int64_t, MR_CHILDREN_PER_CELL>& chemical,
            const std::array<uint64_t, MR_CHILDREN_PER_CELL>& capacity) {
            std::array<int64_t, MR_CHILDREN_PER_CELL> parts{};
            int64_t given = 0;
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                if (j == last)
                    continue;

                parts[j] = PlRefinePartEnergy(heat, chemical[j], capacity[j], capacitySum);
                given += parts[j];
            }

            parts[last] = total - given;

            return parts;
        }

    }  // namespace

    uint32_t EnablePhaseLayerEnergy(MultiresNest& nest, const BakedReactionTable& table) {
        const SfFieldDesc desc = SfMakeOwnerDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, SF_FLAG_HASHED, 1,
                                                 SF_OWNER_PHASE_LAYER);
        const uint32_t field = RegisterNestField(nest, "phase_layer_energy", desc);
        if (field == MULTIRES_NO_FIELD)
            return MULTIRES_NO_FIELD;

        nest.phaseLayer.field = field;
        nest.phaseLayer.species = table.species;

        return field;
    }

    int64_t LoadNestPhaseLayerEnergy(const MultiresNest& nest, uint32_t slot, uint32_t index) {
        if (nest.phaseLayer.field == MULTIRES_NO_FIELD)
            return 0;

        return LoadNestField(nest, nest.phaseLayer.field, slot, index, 0);
    }

    namespace nest_detail {

        void RefinePhaseLayerCell(std::span<const RxSpecies> species, const RxCell& parent, int64_t parentEnergy,
                                  std::array<RxCell, MR_CHILDREN_PER_CELL>& children,
                                  std::array<int64_t, MR_CHILDREN_PER_CELL>& childEnergies) {
            const SpeciesTable table{.species = species};
            const PlRefineHeats heats = PlRefineHeatsOf(table, parent, parentEnergy);
            const PartWeights weights = WeightsOf(table, children);
            const bool split = heats.mixed != 0 && weights.condensedCapacity != 0 && weights.gasCapacity != 0;

            // --- 混ざった親: 層と気体を別々に配り(D-674 を 2 回)、子のエネルギー = 子の層 + 子の気体 ---
            std::array<int64_t, MR_CHILDREN_PER_CELL> condensedParts{};
            if (split) {
                std::array<int64_t, MR_CHILDREN_PER_CELL> chemical{};
                std::array<uint64_t, MR_CHILDREN_PER_CELL> capacity{};
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                    chemical[j] = weights.children[j].condensedChemical;
                    capacity[j] = weights.children[j].condensedCapacity;
                }

                condensedParts = SplitPart(heats.condensedHeat, heats.condensedEnergy, weights.condensedCapacity,
                                           weights.lastCondensed, chemical, capacity);
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                    chemical[j] = weights.children[j].gasChemical;
                    capacity[j] = weights.children[j].gasCapacity;
                }

                const std::array<int64_t, MR_CHILDREN_PER_CELL> gasParts = SplitPart(
                    heats.gasHeat, heats.gasEnergy, weights.gasCapacity, weights.lastGas, chemical, capacity);
                for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                    children[j].energy = condensedParts[j] + gasParts[j];
            }

            // --- 子の Ec: 混ざった子だけ(配った層か、温度の揃う分け方)---
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                childEnergies[j] = 0;
                if (PlLayerOf(table, children[j]).mixed == 0)
                    continue;

                childEnergies[j] = split ? condensedParts[j] : PlEvenCondensedEnergy(table, children[j]);
            }
        }

        int64_t CoarsenPhaseLayerCell(std::span<const RxSpecies> species, const RxCell& parent,
                                      const std::array<RxCell, MR_CHILDREN_PER_CELL>& children,
                                      const std::array<int64_t, MR_CHILDREN_PER_CELL>& childEnergies) {
            const SpeciesTable table{.species = species};
            int64_t condensedSum = 0;
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                condensedSum += PlCondensedPartOf(table, children[j], childEnergies[j]);

            return PlCoarsenCondensedEnergy(table, parent, condensedSum);
        }

    }  // namespace nest_detail

}  // namespace bicameral::sim
