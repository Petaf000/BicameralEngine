// cell_view.cpp — セルの見立ての相の表(T-0713。固体の動き方と安息角は T-0819)。約束は cell_view.h。
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

    namespace {

        // 安息角 0〜89 度の tan θ(Q16 の最も近い整数。tan 45° = 65536)。90 度は CV_REPOSE_VERTICAL(横には崩れない)。
        // シミュのコードに浮動小数点を書かないので、表で持つ(tan を一度だけ計算して書き写した値。T-0819)
        constexpr uint32_t REPOSE_SLOPE_Q16[REPOSE_ANGLE_MAX_DEGREES] = {
            0,      1144,   2289,   3435,   4583,   5734,   6888,   8047,   9210,   10380,   11556,   12739,   13930,
            15130,  16340,  17560,  18792,  20036,  21294,  22566,  23853,  25157,  26478,   27818,   29179,   30560,
            31964,  33392,  34846,  36327,  37837,  39378,  40951,  42560,  44205,  45889,   47615,   49385,   51202,
            53070,  54991,  56970,  59009,  61113,  63287,  65536,  67865,  70279,  72785,   75391,   78103,   80930,
            83882,  86969,  90203,  93595,  97161,  100917, 104880, 109070, 113512, 118230,  123255,  128622,  134369,
            140542, 147196, 154393, 162207, 170727, 180059, 190330, 201699, 214359, 228551,  244584,  262851,  283868,
            308323, 337153, 371673, 413778, 466313, 533748, 623533, 749080, 937208, 1250501, 1876705, 3754555,
        };
        static_assert(REPOSE_SLOPE_Q16[45] == multires::CV_OPEN_FULL);

        // 物質の安息角の傾き(据わる物質は 0。ばらける固体は書いた角、書き忘れは既定の角〔ベイクが警告済み〕)
        uint32_t ReposeSlopeQ16(const SpeciesTransport& transport) {
            if (transport.solidMotion != SolidMotion::Loose)
                return 0;

            const uint32_t degrees = transport.reposeAngleDegrees.value_or(DEFAULT_REPOSE_ANGLE_DEGREES);

            return degrees >= REPOSE_ANGLE_MAX_DEGREES ? multires::CV_REPOSE_VERTICAL : REPOSE_SLOPE_Q16[degrees];
        }

    }  // namespace

    std::vector<multires::CvSpeciesPhase> MakeSpeciesPhases(const BakedReactionTable& table) {
        std::vector<multires::CvSpeciesPhase> phases(table.species.size());
        for (uint32_t id = 0; id < phases.size(); ++id) {
            const SpeciesPhase speciesPhase = id < table.speciesPhase.size() ? table.speciesPhase[id]
                                                                             : SpeciesPhase::Gas;
            const bool condensed = speciesPhase != SpeciesPhase::Gas;
            const auto phase = static_cast<uint32_t>(speciesPhase);

            // 固体の動き方(T-0819)。ばらけるのは固体だけ(ベイクが固体の外の solid_motion を断る。ここでも固体の外は据わる)
            const SpeciesTransport transport = id < table.speciesTransport.size() ? table.speciesTransport[id]
                                                                                  : SpeciesTransport{};
            const bool loose = speciesPhase == SpeciesPhase::Solid && transport.solidMotion == SolidMotion::Loose;

            // 気体のモル体積は使わない(圧力で決まる)。密度の無い凝縮相は 0(場所を取らない。ShareCellVolume と同じ)
            phases[id] = {
                .molarVolume = condensed && id < table.speciesMolarVolume.size() ? table.speciesMolarVolume[id] : 0,
                .molarMass = id < table.molarMasses.size() ? table.molarMasses[id] : 0,
                .phase = phase,
                .motion = loose ? multires::CV_MOTION_LOOSE : multires::CV_MOTION_FIXED,
                .reposeSlopeQ16 = loose ? ReposeSlopeQ16(transport) : 0};
        }

        return phases;
    }

}  // namespace bicameral::sim
