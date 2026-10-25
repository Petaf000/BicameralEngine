// probe_reference_radiation.cpp — 仮の世界の CPU リファレンス(ProbeReference)の熱の放射の既定の段(T-1113・D-646・D-658)。
//
// データの流れ(1 刻み。ProbeReference::Advance の (1d)。気体 → 液体 → 放射 → 伝導と反応 の順。23 §3.5):
//   刻みの初めのセル(成分)+ 熱のキャッシュ(温度)+ 物質の光の値(BakedReactionTable::speciesLight。D-610 の書かない値込み)
//   → CascadeGrid(64³ の密な写し。セルの並びは ProbeCellIndex と同じ)
//   → StepCascadeLight(sim/cascade_light。距離ごとの段。放つ面の組 15・熱も待つ = D-658)
//   → セルごとの 吸った − 放った(2^−16 mJ)を最も近い mJ に丸めてセルのエネルギーへ当てる。丸めの端数と世界の外へ出た分は報告の帳簿へ
//   → 変わったセルの熱のキャッシュを作り直し、そのブロックを予定の種にする(気体・液体の段と同じ)。持ち越しは次の刻みへ(ハッシュに入る)。
// 保存: Σ セルのエネルギー × 2^16 + Σ 持ち越し + Σ 外へ出た + Σ 丸め = 一定(ビット単位。probe_radiation_test が確かめる)。
// GPU の仮の世界にはまだ無い(T-1154)。共有の「当てる段」(20 §2.4)がまだ無いので、この段がセルへ直接足す(土台に足りないもの。T-1113)。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#include <algorithm>

#include "core/aliases.h"
#include "sim/probe_sim.h"

namespace bicameral::sim {

    namespace rx = bicameral::reaction;

    namespace {

        struct TableLights {
            const std::vector<rx::LtSpeciesLight>* lights = nullptr;

            [[nodiscard]] rx::LtSpeciesLight Light(uint32_t species) const { return (*lights)[species]; }
        };

        // 物質が光の値(どれか 1 つ)を書いたか。書かない値はベイクが相の既定(D-610)にするので、既定と違えば書いた
        bool WritesLight(const BakedReactionTable& table, size_t species) {
            const rx::LtSpeciesLight& light = table.speciesLight[species];
            const bool gas = species >= table.speciesPhase.size() || table.speciesPhase[species] == SpeciesPhase::Gas;
            if (gas)
                return light.absorptionShort != 0 || light.absorptionLong != 0 || light.reflectanceShortQ16 != 0 ||
                       light.reflectanceLongQ16 != 0;

            const uint32_t greyReflectance = rx::LtPermilleToQ16(rx::LT_DEFAULT_CONDENSED_REFLECTANCE_PERMILLE);
            return light.absorptionShort != rx::LT_OPAQUE_ABSORPTION_MM2_PER_MOL ||
                   light.absorptionLong != rx::LT_OPAQUE_ABSORPTION_MM2_PER_MOL ||
                   light.reflectanceShortQ16 != greyReflectance || light.reflectanceLongQ16 != greyReflectance;
        }

        bool TableWritesLight(const BakedReactionTable& table) {
            for (size_t species = 1; species < table.speciesLight.size(); ++species) {
                if (WritesLight(table, species))
                    return true;
            }

            return false;
        }

        // 2^−16 mJ の正味を最も近い mJ へ(半分は +∞ 側。算術シフト)
        int64_t RoundToMilliJoules(int64_t units) {
            return (units + (int64_t{1} << (rx::RC_ENERGY_SHIFT - 1))) >> rx::RC_ENERGY_SHIFT;
        }

    }  // namespace

    void ProbeReference::StepCascadeRadiation(uint64_t tick, size_t currentBase, std::vector<uint8_t>& seeds) {
        if (m_table->speciesLight.empty() || !TableWritesLight(*m_table))
            return;

        // --- 刻みの初めの写し(温度は熱のキャッシュ。順番によらない)---
        const ReactionTableView table = m_table->View();
        const TableLights lights{.lights = &m_table->speciesLight};
        CascadeGrid grid;
        grid.size = {PROBE_GRID_SIZE, PROBE_GRID_SIZE, PROBE_GRID_SIZE};
        grid.lights.resize(PROBE_CELL_COUNT);
        grid.temperatureMk.resize(PROBE_CELL_COUNT);
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            const rx::RxCell& cell = m_cells[currentBase + index];
            const rx::LtCellBand shortBand = rx::LtCellBandOf(lights, cell, rx::LT_BAND_SHORT);
            const rx::LtCellBand longBand = rx::LtCellBandOf(lights, cell, rx::LT_BAND_LONG);
            grid.lights[index] = {.depthShortQ32 = rx::RdOpticalDepthQ32(shortBand.absorption),
                                  .depthLongQ32 = rx::RdOpticalDepthQ32(longBand.absorption),
                                  .reflectanceShortQ16 = shortBand.reflectanceQ16,
                                  .reflectanceLongQ16 = longBand.reflectanceQ16};
            grid.temperatureMk[index] = m_caches[currentBase + index].temperature;  // 負は 0(熱のキャッシュの約束)
        }

        // --- 段(1 刻み)---
        const CascadeTickResult result = StepCascadeLight(grid, m_cascadeCarry, m_cascade.params, tick);
        m_cascadeCarry = result.carry;
        m_cascadeReport.ran = true;
        m_cascadeReport.rays = result.rays;
        for (const CascadeBandLedger& band : result.ledger)
            m_cascadeReport.escaped += band.escaped;

        // --- セルへ当てる(丸めの端数は帳簿へ)---
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            const uint64_t absorbed = result.absorbed[(size_t{index} * rx::LT_BANDS) + rx::LT_BAND_SHORT] +
                                      result.absorbed[(size_t{index} * rx::LT_BANDS) + rx::LT_BAND_LONG];
            const uint64_t emitted = result.emitted[(size_t{index} * rx::LT_BANDS) + rx::LT_BAND_SHORT] +
                                     result.emitted[(size_t{index} * rx::LT_BANDS) + rx::LT_BAND_LONG];
            m_cascadeReport.absorbed += absorbed;
            m_cascadeReport.emitted += emitted;

            const int64_t net = static_cast<int64_t>(absorbed) - static_cast<int64_t>(emitted);
            const int64_t milliJoules = RoundToMilliJoules(net);
            m_cascadeReport.rounding += net - (milliJoules * (int64_t{1} << rx::RC_ENERGY_SHIFT));
            if (milliJoules == 0)
                continue;

            rx::RxCell& cell = m_cells[currentBase + index];
            cell.energy += milliJoules;
            m_caches[currentBase + index] = ProbeMakeCache(table, cell);
            m_cascadeReport.appliedMilliJoules += milliJoules;
            ++m_cascadeReport.changedCells;
            if (m_caches[currentBase + index].temperature == 0)
                ++m_cascadeReport.negativeCells;

            const uint32_t block = ProbeBlockOfCell(
                index % PROBE_GRID_SIZE, (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE, index / PROBE_SLICE_CELL_COUNT);
            seeds[block] = 1;
            m_changedMarks[block] = ProbeChangeMark(tick) - 1;
        }

        m_cascadeOutside += m_cascadeReport.escaped + static_cast<uint64_t>(m_cascadeReport.rounding);
    }

    uint64_t ProbeReference::CascadeRadiationUnits() const {
        uint64_t units = m_cascadeOutside;
        for (const CascadeCarryEntry& entry : m_cascadeCarry.entries)
            units += entry.amount;

        return units;
    }

}  // namespace bicameral::sim
