// species_remap.cpp — 物質 ID の名前での付け替え(T-0223・ADR-0065)。規則は species_remap.h の先頭。
// 表 2 つから付け替えの表を作る所(BuildSpeciesRemap)と、セル 1 つに当てる所(RemapReactionCell)に分かれる。
// セルに当てる本体は common/species_remap.hlsli の RxRemapCell(GPU の付け替えの段〔probe_tick.hlsl の RemapSpecies〕と同じ関数。ここは表の読み方を渡すだけ)。
#include "sim/species_remap.h"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

#include "core/aliases.h"

namespace bicameral::sim {

    namespace {

        // 物質 1 mol の組み立て(元素の名前と原子の数。元素の ID は名前のバイト順なので、ID の順に並べれば名前の順になる)
        using Composition = std::vector<std::pair<std::string_view, uint32_t>>;

        Composition CompositionOf(const BakedReactionTable& table, uint32_t species) {
            const size_t elementCount = table.elementNames.size();
            Composition composition;
            for (size_t element = 0; element < elementCount; ++element) {
                const uint32_t count = table.speciesElements[(species * elementCount) + element];
                if (count != 0)
                    composition.emplace_back(table.elementNames[element], count);
            }

            return composition;
        }

        // 元素ごとの単体の候補(元素の ID は next のもの)
        struct UnitSpecies {
            uint32_t species = 0;  // 0 = 単体が無い
            uint32_t atoms = 0;
        };

        // a は b より単体に向くか: 原子 1 つあたりの h0 が低い(安定)→ 原子の数が少ない。同じなら先に見た(ID が小さい)方のまま
        bool MoreStableUnit(const BakedReactionTable& table, UnitSpecies a, UnitSpecies b) {
            const int64_t left = table.species[a.species].h0 * int64_t{b.atoms};
            const int64_t right = table.species[b.species].h0 * int64_t{a.atoms};
            if (left != right)
                return left < right;

            return a.atoms < b.atoms;
        }

        // next の元素ごとの単体(その元素だけでできた物質のうち一番安定なもの)
        std::vector<UnitSpecies> FindUnitSpecies(const BakedReactionTable& next) {
            const size_t elementCount = next.elementNames.size();
            std::vector<UnitSpecies> units(elementCount);
            for (uint32_t species = 1; species < next.species.size(); ++species) {
                const Composition composition = CompositionOf(next, species);
                if (composition.size() != 1)
                    continue;

                const auto element = static_cast<size_t>(rng::find(next.elementNames, composition.front().first) -
                                                         next.elementNames.begin());
                const UnitSpecies candidate{.species = species, .atoms = composition.front().second};
                if (units[element].species == 0 || MoreStableUnit(next, candidate, units[element]))
                    units[element] = candidate;
            }

            return units;
        }

        // 元素の名前から next の単体(無ければ species = 0)
        UnitSpecies UnitOf(const BakedReactionTable& next, const std::vector<UnitSpecies>& units,
                           std::string_view element) {
            const auto found = rng::find(next.elementNames, element);
            if (found == next.elementNames.end())
                return {};

            return units[static_cast<size_t>(found - next.elementNames.begin())];
        }

        // 使う単体を ID の昇順に 1 回ずつ並べる(RxRemapCell が足す順)
        void AddUnit(std::vector<reaction::RxRemapUnit>& units, reaction::RxRemapUnit unit) {
            const auto position = rng::lower_bound(units, unit.species, {}, &reaction::RxRemapUnit::species);
            if (position != units.end() && position->species == unit.species)
                return;

            units.insert(position, unit);
        }

    }  // namespace

    void SpeciesRemapReport::Add(const SpeciesRemapReport& other) {
        decomposedMicromoles += other.decomposedMicromoles;
        remainderAtomMicromoles += other.remainderAtomMicromoles;
        overflowAtomMicromoles += other.overflowAtomMicromoles;
        energyDeltaMilliJoules += other.energyDeltaMilliJoules;
    }

    std::expected<SpeciesRemap, std::string> BuildSpeciesRemap(const BakedReactionTable& current,
                                                               const BakedReactionTable& next) {
        const auto currentCount = static_cast<uint32_t>(current.species.size());
        const std::vector<UnitSpecies> units = FindUnitSpecies(next);

        SpeciesRemap remap;
        remap.newIds.assign(currentCount, 0);
        remap.partBegin.assign(size_t{currentCount} + 1, 0);
        remap.removedH0.assign(currentCount, 0);
        remap.identity = current.speciesNames == next.speciesNames;

        std::string missing;
        for (uint32_t species = 1; species < currentCount; ++species) {
            remap.partBegin[species] = static_cast<uint32_t>(remap.parts.size());
            remap.removedH0[species] = current.species[species].h0;

            // --- 名前と組み立てが同じなら同じ物質 ---
            const Composition composition = CompositionOf(current, species);
            const uint32_t sameName = next.SpeciesId(current.speciesNames[species]);
            if (sameName != 0 && CompositionOf(next, sameName) == composition) {
                remap.newIds[species] = sameName;
                remap.identity = remap.identity && sameName == species;
                continue;
            }

            // --- 消えた: 元素ごとに単体へ分ける ---
            remap.identity = false;
            for (const auto& [element, atoms] : composition) {
                const UnitSpecies unit = UnitOf(next, units, element);
                if (unit.species == 0) {
                    missing += std::format("{}{} の {}", missing.empty() ? "" : "・", current.speciesNames[species],
                                           element);
                    continue;
                }

                remap.parts.push_back({.species = unit.species, .atomsPerMol = atoms});
                AddUnit(remap.units, {.species = unit.species, .atoms = unit.atoms});
            }
        }
        remap.partBegin[currentCount] = static_cast<uint32_t>(remap.parts.size());

        if (!missing.empty())
            return std::unexpected(
                std::format("消す物質を元素に分けて戻す先(その元素だけでできた物質)が新しい表に無い: {}"
                            "(単体を足すか、物質を残す)",
                            missing));

        return remap;
    }

    SpeciesRemapReport RemapReactionCell(const SpeciesRemap& remap, const BakedReactionTable& next,
                                         reaction::RxCell& cell) {
        if (remap.identity)
            return {};

        const reaction::RxRemapResult result = reaction::RxRemapCell(remap.View(), next.View(), cell);
        cell = result.cell;

        return {.decomposedMicromoles = result.decomposed,
                .remainderAtomMicromoles = result.remainderAtoms,
                .overflowAtomMicromoles = result.overflowAtoms,
                .energyDeltaMilliJoules = result.energyDelta};
    }

    std::vector<uint32_t> PackSpeciesRemap(const SpeciesRemap& remap) {
        const auto speciesCount = static_cast<uint32_t>(remap.newIds.size());
        std::vector<uint32_t> words = {speciesCount, static_cast<uint32_t>(remap.parts.size()),
                                       static_cast<uint32_t>(remap.units.size()), 0};
        static_assert(reaction::RX_REMAP_HEADER_WORDS == 4);

        for (const int64_t h0 : remap.removedH0) {
            const auto bits = static_cast<uint64_t>(h0);
            words.push_back(static_cast<uint32_t>(bits));
            words.push_back(static_cast<uint32_t>(bits >> 32));
        }

        words.insert(words.end(), remap.newIds.begin(), remap.newIds.end());
        words.insert(words.end(), remap.partBegin.begin(), remap.partBegin.end());
        for (const reaction::RxRemapPart& part : remap.parts) {
            words.push_back(part.species);
            words.push_back(part.atomsPerMol);
        }

        for (const reaction::RxRemapUnit& unit : remap.units) {
            words.push_back(unit.species);
            words.push_back(unit.atoms);
        }

        return words;
    }

    std::vector<NamedElementCount> CountNamedElements(const BakedReactionTable& table, const reaction::RxCell& cell) {
        const std::vector<uint64_t> counts = CountElements(table, cell);
        std::vector<NamedElementCount> named;
        for (size_t element = 0; element < counts.size(); ++element)
            named.push_back({.element = table.elementNames[element], .atomMicromoles = counts[element]});

        return named;
    }

}  // namespace bicameral::sim
