// multires_state.cpp — 保存点と写し(multires_state.h)。区画の一覧(VisitNestState)を回るだけ(T-0711。ADR-0329)。
#include <algorithm>
#include <ranges>
#include <type_traits>
#include <utility>

#include "sim/multires_state.h"

namespace bicameral::sim {

    namespace {

        // 戻す先の区画が保存点の区画と同じ形か(型と大きさ)
        template <typename Values>
        bool SameShape(const std::any& saved, const Values& values) {
            const auto* copy = std::any_cast<Values>(&saved);

            return copy != nullptr && std::ranges::size(*copy) == std::ranges::size(values);
        }

        bool SameFields(const MultiresNest& nest, const MultiresSaveState& save) {
            if (save.fieldDescs.size() != nest.fields.size())
                return false;

            for (size_t i = 0; i < nest.fields.size(); ++i) {
                if (!state_field::SfSameDesc(save.fieldDescs[i], nest.fields[i].desc))
                    return false;
            }

            return true;
        }

    }  // namespace

    MultiresSaveState SaveNestState(const MultiresNest& nest) {
        MultiresSaveState save;
        VisitNestState(nest, [&](const NestStateRegion& region, const auto& values) {
            save.names.push_back(region.name);
            save.regions.emplace_back(values);
        });

        for (const MultiresField& field : nest.fields)
            save.fieldDescs.push_back(field.desc);

        return save;
    }

    bool RestoreNestState(MultiresNest& nest, const MultiresSaveState& save) {
        // --- 形を先に全部確かめる(途中で止めて半分だけ戻すことをしない)---
        if (!SameFields(nest, save))
            return false;

        size_t position = 0;
        bool same = true;
        VisitNestState(std::as_const(nest), [&](const NestStateRegion& region, const auto& values) {
            same = same && position < save.names.size() && save.names[position] == region.name &&
                   SameShape(save.regions[position], values);
            ++position;
        });
        if (!same || position != save.names.size())
            return false;

        // --- 戻す ---
        position = 0;
        VisitNestState(nest, [&](const NestStateRegion&, auto& values) {
            values = std::any_cast<const std::remove_cvref_t<decltype(values)>&>(save.regions[position]);
            ++position;
        });

        // --- 保存点に入らない導かれる値は 0 から(前の値を残さない。T-0854)---
        for (MultiresField& field : nest.fields) {
            if (!state_field::SfSaved(field.desc))
                std::ranges::fill(field.values, 0);
        }

        return true;
    }

    bool CopyNestState(const MultiresNest& from, MultiresNest& to) {
        return RestoreNestState(to, SaveNestState(from));
    }

}  // namespace bicameral::sim
