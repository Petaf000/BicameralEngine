// probe_gas.cpp — 仮の世界に気体の流れを載せる CPU リファレンス(T-0590・ADR-0256)。形と約束は probe_gas.h。
#include "sim/probe_gas.h"

#include <algorithm>
#include <cassert>
#include <format>

#include "common/fixed.hlsli"
#include "common/gas_flow.hlsli"
#include "common/probe_region_hash.hlsli"
#include "common/probe_sim.hlsli"
#include "core/aliases.h"
#include "sim/cell_volume.h"
#include "sim/gas_cells.h"
#include "sim/gas_fastest_layout.h"
#include "sim/world_atmosphere.h"
#include "sim/world_physics.h"

namespace bicameral::sim {

    std::expected<ProbeGasField, std::string> ProbeGasField::Create(const BakedReactionTable& table,
                                                                    const ProbeGasOptions& options) {
        if (!options.Enabled())
            return std::unexpected("気体の区画が空");

        for (uint32_t axis = 0; axis < 3; ++axis) {
            if (options.minCell[axis] + options.sizeCells[axis] > PROBE_GRID_SIZE)
                return std::unexpected("気体の区画が格子からはみ出す");
        }

        if (table.atmosphere.Empty())
            return std::unexpected("基準の大気が無い世界では気体を流せない(真空の基準は T-0592 で決める)");

        if (table.environment.soundSpeedMillimetersPerSecond == 0)
            return std::unexpected("音速の基準が 0(環境の値 environment.sound_speed_mm_per_s が要る)");

        if (options.substeps == 0 ||
            (options.pressure == ProbeGasPressure::Explicit &&
             table.environment.soundSpeedMillimetersPerSecond > GasSoundSpeedLimitMmPerS(options.substeps)))
            return std::unexpected("音速の基準が小刻みの安定の上限を超える");

        ProbeGasField field;
        field.m_options = options;
        field.m_worldInstant = table.environment.gasPressureSpeed == GasPressureSpeed::Instant;

        // --- 層ごとの基準の大気(区画の層 = 格子の y。高さ = 床 + y × 0.5 m)---
        const int64_t bottom = options.floorHeightMillimeters +
                               ((int64_t)options.minCell[1] * ATMOSPHERE_PRESSURE_STEP_MILLIMETERS);
        field.m_layers = GasAtmosphereLayers::Create(table, bottom, options.sizeCells[1]);

        // --- 区画の気体の欄(静止)---
        const uint32_t cells = options.sizeCells[0] * options.sizeCells[1] * options.sizeCells[2];
        field.m_fields.Assign(cells);
        field.m_ledger.amounts.assign(table.species.size(), 0);

        return field;
    }

    void ProbeGasField::ApplyTable(const BakedReactionTable& table, const SpeciesRemap& remap) {
        const int64_t bottom = m_options.floorHeightMillimeters +
                               ((int64_t)m_options.minCell[1] * ATMOSPHERE_PRESSURE_STEP_MILLIMETERS);
        m_layers = GasAtmosphereLayers::Create(table, bottom, m_options.sizeCells[1]);
        if (remap.identity)
            return;

        std::vector<int64_t> amounts(table.species.size(), 0);
        for (uint32_t oldId = 1; oldId < m_ledger.amounts.size() && oldId < remap.newIds.size(); ++oldId) {
            if (remap.newIds[oldId] != 0)
                amounts[remap.newIds[oldId]] = m_ledger.amounts[oldId];
        }

        m_ledger.amounts = std::move(amounts);
    }

    uint32_t ProbeGasField::GridIndex(uint32_t region) const {
        const uint32_t width = m_options.sizeCells[0];
        const uint32_t depth = m_options.sizeCells[2];
        const uint32_t x = region % width;
        const uint32_t z = (region / width) % depth;
        const uint32_t y = region / (width * depth);

        return ProbeCellIndex(m_options.minCell[0] + x, m_options.minCell[1] + y, m_options.minCell[2] + z);
    }

    GasConfig ProbeGasField::MakeConfig(const BakedReactionTable& table, std::span<const uint32_t> slots) const {
        std::array<GasBoundary, GAS_AXES> boundary = {};
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
            boundary[axis] = m_options.boundary[PROBE_GRID_AXIS_OF_GAS_AXIS[axis]];

        // 投影は使わない(projection.pressure.substeps)。係数の安定の検査のために陽解法の値
        return m_layers.MakeConfig(table, slots,
                                   {m_options.sizeCells[0], m_options.sizeCells[2], m_options.sizeCells[1]}, boundary,
                                   m_options.substeps, m_options.randomSeed);
    }

    // --- 最速 × 小さい空き(T-0788・D-519・ADR-0346。sim/gas_fastest_check)---

    bool ProbeGasField::FastestProjection() const {
        return m_worldInstant || (m_options.pressure == ProbeGasPressure::Projection &&
                                  m_options.projection.pressure.soundSpeedMmPerS == 0);
    }

    // 表の環境の圧力の伝わる速さを読み直す(T-0804。ホットリロードで替わったら、小さい空きの検査を初めからやり直す)
    void ProbeGasField::SyncWorldPressure(const BakedReactionTable& table) {
        const bool instant = table.environment.gasPressureSpeed == GasPressureSpeed::Instant;
        if (instant == m_worldInstant)
            return;

        m_worldInstant = instant;
        m_openingsChecked = false;
        m_joints.clear();
    }

    std::string ProbeGasField::JointWarning(const GasSmallOpenJoint& joint) const {
        return ProbeRegionJointWarning(joint, m_options.minCell);
    }

    std::vector<std::string> ProbeGasField::CheckFastestOpenings(const BakedReactionTable& table,
                                                                 std::span<const reaction::RxCell> cells) const {
        if (!FastestProjection())
            return {};

        std::vector<reaction::RxCell> region(RegionCellCount());
        for (uint32_t index = 0; index < RegionCellCount(); ++index)
            region[index] = cells[GridIndex(index)];

        return CheckRegionOpenings(table, region);
    }

    std::vector<std::string> ProbeGasField::CheckRegionOpenings(const BakedReactionTable& table,
                                                                std::span<const reaction::RxCell> region) const {
        GasOpenCounts counts;
        const std::vector<uint32_t> open = CollectProbeGasOpen(table, region, counts);
        const GasConfig config = MakeConfig(table, m_layers.Species());
        std::vector<std::string> warnings;
        for (const GasSmallOpenJoint& joint : FindGasSmallOpenJoints(config, open))
            warnings.push_back(JointWarning(joint));

        return warnings;
    }

    void ProbeGasField::CheckOpeningsOnStep(const GasConfig& config, std::span<const uint32_t> openQ16,
                                            ProbeGasStepReport& report) {
        if (!m_openingsChecked || !rng::equal(openQ16, m_checkedOpen)) {
            std::vector<GasSmallOpenJoint> joints = FindGasSmallOpenJoints(config, openQ16);
            // 新しい組(前の組と範囲・部分の数が違う)だけ警告する(燃えて空きが少しずつ変わる刻みごとに同じ文を繰り返さない)
            for (const GasSmallOpenJoint& joint : joints) {
                const bool known = rng::any_of(m_joints, [&](const GasSmallOpenJoint& before) {
                    return before.minCell == joint.minCell && before.maxCell == joint.maxCell &&
                           before.parts == joint.parts && before.outside == joint.outside;
                });
                if (!known)
                    report.warnings.push_back(JointWarning(joint));
            }

            m_joints = std::move(joints);
            m_checkedOpen.assign(openQ16.begin(), openQ16.end());
            m_openingsChecked = true;
        }

        report.smallOpenJoints = static_cast<uint32_t>(m_joints.size());
    }

    GasBox ProbeGasField::MakeAtmosphereBox(const BakedReactionTable& table) const {
        const GasConfig config = MakeConfig(table, m_layers.Species());

        return MakeGasBox(config, m_layers.MakeReference(config, m_layers.Species()));
    }

    reaction::RxCell ProbeGasField::ReferenceCell(const BakedReactionTable& table, uint32_t gridY) const {
        assert(gridY >= m_options.minCell[1] && gridY < m_options.minCell[1] + m_options.sizeCells[1]);
        const std::span<const uint32_t> atmosphere = m_layers.Species();
        const GasConfig config = MakeConfig(table, atmosphere);
        const GasCell reference = m_layers.MakeReference(config, atmosphere)[gridY - m_options.minCell[1]];

        reaction::RxCell cell = reaction::RxMakeEmptyCell(reference.energy);
        for (uint32_t s = 0; s < config.speciesCount; ++s) {
            if (reference.amounts[s] == 0)
                continue;

            cell.species[cell.speciesCount] = atmosphere[s];
            cell.amounts[cell.speciesCount] = reference.amounts[s];
            ++cell.speciesCount;
        }

        return cell;
    }

    ProbeGasStepReport ProbeGasField::Step(const BakedReactionTable& table, std::span<reaction::RxCell> cells,
                                           uint64_t tick, std::vector<uint32_t>& changed) {
        ProbeGasStepReport report;
        SyncWorldPressure(table);
        std::vector<reaction::RxCell> region(RegionCellCount());
        for (uint32_t index = 0; index < RegionCellCount(); ++index)
            region[index] = cells[GridIndex(index)];

        // --- 区画のセル → 気体の箱(sim/gas_cells。多重解像度の世界と同じ写し)---
        GasCellsStep step;
        step.slots = CollectGasSpecies(table, region, m_layers.Species());
        report.gasSpecies = (uint32_t)step.slots.size();
        if (step.slots != m_projectionSpecies) {
            m_projection = {};
            m_projectionSpecies = step.slots;
        }

        const GasConfig config = MakeConfig(table, step.slots);
        step.box = MakeGasBox(config, m_layers.MakeReference(config, step.slots));
        step.box.tick = tick;
        step.box.limitMovedHeat = m_options.limitMovedHeat;
        GasOpenCounts counts;
        step.box.openQ16 = CollectProbeGasOpen(table, region, counts);
        report.blockedCells = counts.blockedCells;
        report.partialCells = counts.partialCells;
        if (FastestProjection())
            CheckOpeningsOnStep(config, step.box.openQ16, report);
        LoadGasCells(table, region, m_fields, step);

        // --- 1 刻み(圧力の解き方を選ぶ)---
        const GasProjection projectionBefore = m_projection;
        // 世界が最速を選んだら(T-0804)、options の投影の設定のまま音速だけ 0 にして解く
        GasProjectionOptions projection = m_options.projection;
        if (m_worldInstant)
            projection.pressure.soundSpeedMmPerS = 0;

        if (!m_worldInstant && m_options.pressure == ProbeGasPressure::Explicit)
            StepGas(step.box);
        else
            report.projection = StepGasProjection(step.box, m_projection, projection);

        // --- 静止の丸め(当てる段の後)→ 気体の箱 → 区画のセル・欄・帳簿(帳簿は格子の軸)---
        GasCellsLedger ledger{.amounts = std::vector<int64_t>(m_ledger.amounts.size(), 0)};
        RestGasCells(step, m_options.restSpeedUnits, ledger);
        std::vector<uint8_t> regionChanged;
        if (!StoreGasCells(table, step, region, m_fields, ledger, regionChanged)) {
            m_projection = projectionBefore;
            report.skippedCellOverflow = true;
            return report;
        }

        for (uint32_t id = 0; id < ledger.amounts.size(); ++id)
            m_ledger.amounts[id] += ledger.amounts[id];

        m_ledger.energy += ledger.energy;
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
            m_ledger.momentum[PROBE_GRID_AXIS_OF_GAS_AXIS[axis]] += ledger.momentum[axis];
            m_ledger.restMomentum[PROBE_GRID_AXIS_OF_GAS_AXIS[axis]] += ledger.restMomentum[axis];
        }

        m_ledger.restEnergy += ledger.restEnergy;
        report.restedCells = ledger.restedCells;
        report.energyLimitedFaces = step.box.energyLimitedFaces;

        for (uint32_t index = 0; index < RegionCellCount(); ++index) {
            if (regionChanged[index] == 0)
                continue;

            cells[GridIndex(index)] = region[index];
            changed.push_back(GridIndex(index));
            ++report.changedCells;
        }

        return report;
    }

    int64_t ProbeGasField::KineticEnergySum() const {
        int64_t sum = 0;
        for (const int64_t kinetic : m_fields.kinetic)
            sum += kinetic;

        return sum;
    }

    uint64_t ProbeGasField::Hash() const {
        // FNV-1a 風に、区画の順で混ぜる
        uint64_t hash = 14695981039346656037ull;
        const auto mix = [&hash](uint64_t value) {
            hash ^= value;
            hash *= 1099511628211ull;
        };

        for (uint32_t region = 0; region < RegionCellCount(); ++region) {
            for (const int64_t momentum : m_fields.momentum[region])
                mix((uint64_t)momentum);

            mix((uint64_t)m_fields.kinetic[region]);
            mix(m_fields.mass[region]);
        }

        for (const int64_t amount : m_ledger.amounts)
            mix((uint64_t)amount);

        mix((uint64_t)m_ledger.energy);
        for (const int64_t momentum : m_ledger.momentum)
            mix((uint64_t)momentum);

        return hash;
    }

    uint64_t ProbeGasField::HashTerm() const {
        uint64_t sum = 0;
        for (uint32_t region = 0; region < RegionCellCount(); ++region) {
            uint64_t term = ProbeRegionHashBegin(PROBE_GAS_CELL_HASH_SEED, region);
            for (const int64_t momentum : m_fields.momentum[region])
                term = ProbeRegionHashAdd(term, momentum);

            term = ProbeRegionHashAdd(term, m_fields.kinetic[region]);
            const uint64_t mass = region < m_fields.mass.size() ? m_fields.mass[region] : 0;  // GPU の欄は作った時 0
            sum += ProbeRegionHashEnd(ProbeRegionHashAdd(term, (int64_t)mass));
        }

        // 帳簿: 物質 ID ごとの量 → エネルギー → 運動量 × 3(GPU の作業場の帳簿の並び)
        uint64_t ledger = ProbeRegionHashBegin(PROBE_GAS_LEDGER_HASH_SEED, (uint32_t)m_ledger.amounts.size());
        for (const int64_t amount : m_ledger.amounts)
            ledger = ProbeRegionHashAdd(ledger, amount);

        ledger = ProbeRegionHashAdd(ledger, m_ledger.energy);
        for (const int64_t momentum : m_ledger.momentum)
            ledger = ProbeRegionHashAdd(ledger, momentum);

        return sum + ProbeRegionHashEnd(ledger);
    }

}  // namespace bicameral::sim
