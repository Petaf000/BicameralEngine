// transport_packet.cpp — 当てる段の包みをセルに当てる CPU(T-0770)。約束は transport_packet.h。
#include "sim/transport_packet.h"

#include <algorithm>

#include "core/aliases.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    namespace {

        void AddAmount(TxCellChange& change, uint32_t species, int64_t amount) {
            change.touched = true;
            for (auto& entry : change.amounts) {
                if (entry.first == species) {
                    entry.second += amount;
                    return;
                }
            }

            change.amounts.emplace_back(species, amount);
        }

    }  // namespace

    void AddPacket(TxCellChange& change, const multires::TxPacket& packet, bool incoming) {
        for (uint32_t i = 0; i < packet.speciesCount; ++i) {
            if (packet.amounts[i] == 0)
                continue;

            const auto amount = (int64_t)packet.amounts[i];
            AddAmount(change, packet.species[i], incoming ? amount : -amount);
        }

        change.energy += incoming ? packet.energy : -packet.energy;
        for (uint32_t c = 0; c < multires::TX_PACKET_CARRIED; ++c)
            change.carried[c] += incoming ? packet.carried[c] : -packet.carried[c];
    }

    bool ApplyCellChange(reaction::RxCell& cell, const TxCellChange& change) {
        if (!change.touched)
            return false;

        std::vector<SpeciesAmount> amounts;
        for (uint32_t i = 0; i < cell.speciesCount; ++i)
            amounts.push_back({.species = cell.species[i], .amount = cell.amounts[i]});

        for (const auto& [species, delta] : change.amounts) {
            auto found = rng::find(amounts, species, &SpeciesAmount::species);
            if (found == amounts.end()) {
                FX_ASSERT(delta >= 0);
                amounts.push_back({.species = species, .amount = (uint64_t)delta});
                continue;
            }

            FX_ASSERT(delta >= 0 || found->amount >= (uint64_t)-delta);
            found->amount = (uint64_t)((int64_t)found->amount + delta);
        }

        std::erase_if(amounts, [](const SpeciesAmount& entry) { return entry.amount == 0; });
        rng::sort(amounts, {}, &SpeciesAmount::species);
        FX_ASSERT(amounts.size() <= reaction::RX_MAX_CELL_SPECIES);

        reaction::RxCell after = reaction::RxMakeEmptyCell(cell.energy + change.energy);
        for (uint32_t i = 0; i < amounts.size() && i < reaction::RX_MAX_CELL_SPECIES; ++i) {
            after.species[i] = amounts[i].species;
            after.amounts[i] = amounts[i].amount;
            after.speciesCount = i + 1;
        }

        bool same = after.energy == cell.energy && after.speciesCount == cell.speciesCount;
        for (uint32_t i = 0; same && i < after.speciesCount; ++i)
            same = after.species[i] == cell.species[i] && after.amounts[i] == cell.amounts[i];

        cell = after;

        return !same;
    }

}  // namespace bicameral::sim
