// cell_view.cpp — セルの見立ての相の表(T-0713)。約束は cell_view.h。
#include "sim/cell_view.h"

#include "sim/cell_volume.h"

namespace bicameral::sim {

    // 見立ての定数は今の仕組みの定数と同じ値(同じ式をビット単位で出すため。T-0713 の試験)
    static_assert(multires::CV_OPEN_FULL == CELL_OPEN_FULL);
    static_assert(multires::CV_CELL_VOLUME_LEVEL_UNITS == PROBE_CELL_VOLUME_PICOLITERS);
    // 相の値はベイクの相の表(SpeciesPhase。T-0710)と同じ並び(写すだけ)
    static_assert(multires::CV_PHASE_GAS == static_cast<uint32_t>(SpeciesPhase::Gas));
    static_assert(multires::CV_PHASE_LIQUID == static_cast<uint32_t>(SpeciesPhase::Liquid));
    static_assert(multires::CV_PHASE_SOLID == static_cast<uint32_t>(SpeciesPhase::Solid));

    std::vector<multires::CvSpeciesPhase> MakeSpeciesPhases(const BakedReactionTable& table) {
        std::vector<multires::CvSpeciesPhase> phases(table.species.size());
        for (uint32_t id = 0; id < phases.size(); ++id) {
            const SpeciesPhase speciesPhase = id < table.speciesPhase.size() ? table.speciesPhase[id]
                                                                             : SpeciesPhase::Gas;
            const bool condensed = speciesPhase != SpeciesPhase::Gas;
            const auto phase = static_cast<uint32_t>(speciesPhase);

            // 気体のモル体積は使わない(圧力で決まる)。密度の無い凝縮相は 0(場所を取らない。ShareCellVolume と同じ)
            phases[id] = {
                .molarVolume = condensed && id < table.speciesMolarVolume.size() ? table.speciesMolarVolume[id] : 0,
                .molarMass = id < table.molarMasses.size() ? table.molarMasses[id] : 0,
                .phase = phase};
        }

        return phases;
    }

}  // namespace bicameral::sim
