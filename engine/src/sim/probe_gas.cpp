// probe_gas.cpp — 仮の世界に気体の流れを載せる CPU リファレンス(T-0590・ADR-0256)。形と約束は probe_gas.h。
#include "sim/probe_gas.h"

#include <algorithm>
#include <cassert>

#include "common/fixed.hlsli"
#include "common/probe_sim.hlsli"
#include "core/aliases.h"
#include "sim/cell_volume.h"
#include "sim/world_atmosphere.h"
#include "sim/world_physics.h"

namespace bicameral::sim {

    namespace {

        constexpr uint64_t PARTS_TO_Q32_DIVISOR = ATMOSPHERE_PARTS_PER_WHOLE;

        // 床で割る(負の数も下へ)
        int64_t DivideDown(int64_t value, int64_t divisor) {
            const int64_t quotient = value / divisor;
            return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
        }

        // value × numerator ÷ denominator(0 の向きへ切り捨て。value は符号付き・numerator ≤ denominator)
        int64_t ScaleToward0(int64_t value, uint64_t numerator, uint64_t denominator) {
            const uint64_t magnitude = fx::FxAbsU64(value);
            const uint64_t scaled = fx::FxDivU128By64(fx::FxMulU64Full(magnitude, numerator), denominator).quotient;
            return value < 0 ? -(int64_t)scaled : (int64_t)scaled;
        }

        bool SameCell(const reaction::RxCell& a, const reaction::RxCell& b) {
            if (a.energy != b.energy || a.speciesCount != b.speciesCount)
                return false;

            for (uint32_t i = 0; i < a.speciesCount; ++i) {
                if (a.species[i] != b.species[i] || a.amounts[i] != b.amounts[i])
                    return false;
            }

            return true;
        }

        bool IsGas(const BakedReactionTable& table, uint32_t species) {
            return species < table.speciesCondensed.size() ? table.speciesCondensed[species] == 0 : true;
        }

        // セルのエネルギーのうち気体のぶん(mJ)= 熱 × 気体の熱容量 ÷ セルの熱容量 + 気体の化学(セルの温度を気体と凝縮相で同じと置く。
        // 局所の熱の釣り合い。反応と伝導の温度と同じ 1 つの温度)。セルが気体だけなら全部
        int64_t GasEnergyPart(const BakedReactionTable& table, const reaction::RxCell& cell) {
            int64_t chemical = 0;
            int64_t gasChemical = 0;
            uint64_t capacity = 0;
            uint64_t gasCapacity = 0;
            bool hasCondensed = false;
            bool hasGas = false;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                const reaction::RxSpecies& species = table.species[cell.species[i]];
                const int64_t amountChemical = (int64_t)cell.amounts[i] * species.h0;
                const uint64_t amountCapacity = cell.amounts[i] * species.heatCapacity;
                chemical += amountChemical;
                capacity += amountCapacity;
                if (IsGas(table, cell.species[i])) {
                    hasGas = true;
                    gasChemical += amountChemical;
                    gasCapacity += amountCapacity;
                } else {
                    hasCondensed = true;
                }
            }

            if (!hasGas)
                return 0;

            if (!hasCondensed || capacity == 0)
                return cell.energy;

            const int64_t heat = (cell.energy * 1000) - chemical;  // µJ
            const int64_t gasHeat = ScaleToward0(heat, gasCapacity, capacity);

            return DivideDown(gasHeat + gasChemical, 1000);
        }

        // 区画の気体の物質: 基準の大気の物質と、区画のセルにある凝縮相でない物質(ID の順)
        std::vector<uint32_t> CollectGasSpecies(const BakedReactionTable& table,
                                                std::span<const reaction::RxCell> cells, const ProbeGasField& field,
                                                std::span<const uint32_t> atmosphere) {
            std::vector<uint32_t> species(atmosphere.begin(), atmosphere.end());
            for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
                const reaction::RxCell& cell = cells[field.GridIndex(region)];
                for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                    if (cell.amounts[i] != 0 && IsGas(table, cell.species[i]))
                        species.push_back(cell.species[i]);
                }
            }

            rng::sort(species);
            species.erase(std::unique(species.begin(), species.end()), species.end());

            return species;
        }

        // 書き戻すセルの成分(物質 ID の順): 前のセルの凝縮相の成分 + 気体の箱の 0 でない成分
        std::vector<SpeciesAmount> MergeCellSpecies(const BakedReactionTable& table, const reaction::RxCell& before,
                                                    const GasCell& gas, std::span<const uint32_t> slots) {
            std::vector<SpeciesAmount> amounts;
            for (uint32_t i = 0; i < before.speciesCount; ++i) {
                if (!IsGas(table, before.species[i]))
                    amounts.push_back({.species = before.species[i], .amount = before.amounts[i]});
            }

            for (uint32_t s = 0; s < slots.size(); ++s) {
                if (gas.amounts[s] != 0)
                    amounts.push_back({.species = slots[s], .amount = gas.amounts[s]});
            }

            rng::sort(amounts, {}, &SpeciesAmount::species);

            return amounts;
        }

        static_assert(CELL_OPEN_FULL == GAS_OPEN_FULL);

        // 区画のセルの気体の空き(Q16。固体と液体が占めない体積の割合。T-0593・ADR-0268)。全部のセルが全部なら空(今まで通り)
        std::vector<uint32_t> CollectGasOpen(const BakedReactionTable& table, std::span<const reaction::RxCell> cells,
                                             const ProbeGasField& field, ProbeGasStepReport& report) {
            std::vector<uint32_t> open(field.RegionCellCount(), CELL_OPEN_FULL);
            bool partial = false;
            for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
                const CellVolumeShare share = ShareCellVolume(table, cells[field.GridIndex(region)]);
                open[region] = GasOpenQ16(share, PROBE_CELL_VOLUME_PICOLITERS);
                if (open[region] == CELL_OPEN_FULL)
                    continue;

                partial = true;
                if (open[region] == 0)
                    ++report.blockedCells;
                else
                    ++report.partialCells;
            }

            if (!partial)
                open.clear();

            return open;
        }

        // 気体の箱の軸(x・y・z = 上)→ 格子の軸(x・z・y)
        constexpr std::array<uint32_t, GAS_AXES> GRID_AXIS_OF_GAS_AXIS = {0, 2, 1};

    }  // namespace

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
        field.m_soundSpeedMmPerS = table.environment.soundSpeedMillimetersPerSecond;
        field.m_gravityMicrometersPerSecond2 = table.environment.gravityMicrometersPerSecond2;

        // --- 層ごとの基準の大気(区画の層 = 格子の y。高さ = 床 + y × 0.5 m)---
        const uint32_t layers = options.sizeCells[1];
        for (uint32_t layer = 0; layer < layers; ++layer) {
            const int64_t height = options.floorHeightMillimeters +
                                   ((int64_t)(options.minCell[1] + layer) * ATMOSPHERE_PRESSURE_STEP_MILLIMETERS);
            const AtmosphereSample sample = SampleAtmosphere(table.atmosphere, height);
            std::vector<std::array<uint32_t, 2>> fractions;
            for (const AtmosphereFraction& fraction : sample.fractions) {
                fractions.push_back({fraction.species, fraction.partsPerMillion});
                field.m_atmosphereSpecies.push_back(fraction.species);
            }

            field.m_layerAtmosphere.push_back(std::move(fractions));
            field.m_layerTemperatures.push_back(sample.temperatureMilliKelvin);
        }

        rng::sort(field.m_atmosphereSpecies);
        field.m_atmosphereSpecies.erase(std::unique(field.m_atmosphereSpecies.begin(), field.m_atmosphereSpecies.end()),
                                        field.m_atmosphereSpecies.end());
        const int64_t bottom = options.floorHeightMillimeters +
                               ((int64_t)options.minCell[1] * ATMOSPHERE_PRESSURE_STEP_MILLIMETERS);
        field.m_surfacePressureMicroPascal = AtmospherePressureMicroPascal(
            table.atmosphere, table, field.m_gravityMicrometersPerSecond2, bottom);

        // --- 区画の気体の欄(静止)---
        const uint32_t cells = options.sizeCells[0] * options.sizeCells[1] * options.sizeCells[2];
        field.m_momentum.assign(cells, {0, 0, 0});
        field.m_kinetic.assign(cells, 0);
        field.m_ledger.amounts.assign(table.species.size(), 0);

        return field;
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
        GasConfig config;
        config.sizeX = m_options.sizeCells[0];
        config.sizeY = m_options.sizeCells[2];
        config.sizeZ = m_options.sizeCells[1];
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
            config.boundary[axis] = m_options.boundary[GRID_AXIS_OF_GAS_AXIS[axis]];

        config.speciesCount = (uint32_t)slots.size();
        for (uint32_t s = 0; s < slots.size(); ++s) {
            const reaction::RxSpecies& species = table.species[slots[s]];
            config.species[s] = {.molarMass = table.molarMasses[slots[s]],
                                 .heatCapacity = species.heatCapacity,
                                 .formationEnergy = species.h0,
                                 .roundingKey = slots[s]};  // 物質 ID(区画の物質が増えても他の成分の丸めは変わらない)
        }

        config.substeps =
            m_options.substeps;  // 投影は使わない(projection.pressure.substeps)。係数の安定の検査のために陽解法の値
        config.gravity = m_gravityMicrometersPerSecond2 != 0;
        ApplyWorldPhysics(table.environment, config);
        config.randomSeed = m_options.randomSeed;

        return config;
    }

    std::vector<GasCell> ProbeGasField::MakeReference(const BakedReactionTable& /*table*/, const GasConfig& config,
                                                      std::span<const uint32_t> slots) const {
        std::vector<GasReferenceLayer> layers;
        for (uint32_t layer = 0; layer < config.sizeZ; ++layer) {
            GasReferenceLayer reference{.temperatureMilliKelvin = m_layerTemperatures[layer]};
            for (const std::array<uint32_t, 2>& fraction : m_layerAtmosphere[layer]) {
                const auto slot = rng::find(slots, fraction[0]);
                assert(slot != slots.end());
                const uint64_t q32 = ((uint64_t)fraction[1] << 32) / PARTS_TO_Q32_DIVISOR;
                reference.fractionsQ32[slot - slots.begin()] = (uint32_t)std::min<uint64_t>(q32, UINT32_MAX);
            }

            layers.push_back(reference);
        }

        return MakeHydrostaticReference(config, layers, m_surfacePressureMicroPascal);
    }

    GasBox ProbeGasField::MakeAtmosphereBox(const BakedReactionTable& table) const {
        const GasConfig config = MakeConfig(table, m_atmosphereSpecies);

        return MakeGasBox(config, MakeReference(table, config, m_atmosphereSpecies));
    }

    reaction::RxCell ProbeGasField::ReferenceCell(const BakedReactionTable& table, uint32_t gridY) const {
        assert(gridY >= m_options.minCell[1] && gridY < m_options.minCell[1] + m_options.sizeCells[1]);
        const GasConfig config = MakeConfig(table, m_atmosphereSpecies);
        const GasCell reference = MakeReference(table, config, m_atmosphereSpecies)[gridY - m_options.minCell[1]];

        reaction::RxCell cell = reaction::RxMakeEmptyCell(reference.energy);
        for (uint32_t s = 0; s < config.speciesCount; ++s) {
            if (reference.amounts[s] == 0)
                continue;

            cell.species[cell.speciesCount] = m_atmosphereSpecies[s];
            cell.amounts[cell.speciesCount] = reference.amounts[s];
            ++cell.speciesCount;
        }

        return cell;
    }

    ProbeGasStepReport ProbeGasField::Step(const BakedReactionTable& table, std::span<reaction::RxCell> cells,
                                           uint64_t tick, std::vector<uint32_t>& changed) {
        ProbeGasStepReport report;
        const std::vector<uint32_t> slots = CollectGasSpecies(table, cells, *this, m_atmosphereSpecies);
        report.gasSpecies = (uint32_t)slots.size();
        if (slots != m_projectionSpecies) {
            m_projection = {};
            m_projectionSpecies = slots;
        }

        // --- 区画のセル → 気体の箱 ---
        const GasConfig config = MakeConfig(table, slots);
        GasBox box = MakeGasBox(config, MakeReference(table, config, slots));
        box.tick = tick;
        box.openQ16 = CollectGasOpen(table, cells, *this, report);
        if (m_options.pressure == ProbeGasPressure::Projection && !box.openQ16.empty()) {
            report.opennessIgnored = true;  // 投影は空きを読まない(T-0617)
            box.openQ16.clear();
        }
        std::vector<int64_t> condensedEnergy(RegionCellCount(), 0);
        std::array<int64_t, GAS_AXES> lostMomentum = {};  // 気体が無くなったセルの運動量(格子の軸。書き戻す時に帳簿へ)
        for (uint32_t region = 0; region < RegionCellCount(); ++region) {
            const reaction::RxCell& cell = cells[GridIndex(region)];
            GasCell& gas = box.cells[region];
            gas = {};
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                const auto slot = rng::find(slots, cell.species[i]);
                if (slot != slots.end())
                    gas.amounts[slot - slots.begin()] = cell.amounts[i];
            }

            const int64_t gasPart = GasEnergyPart(table, cell);
            condensedEnergy[region] = cell.energy - gasPart;
            gas.energy = gasPart;

            // 気体が無くなったセル(反応が使い切った)の運動量は外へ出たものとして帳簿へ、運動エネルギーのぶんは凝縮相の熱へ
            // (欄と帳簿は書き戻す時に替える。流さない刻みは何も変えない)
            const bool empty = std::ranges::all_of(gas.amounts, [](uint64_t amount) { return amount == 0; });
            if (empty) {
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                    lostMomentum[GRID_AXIS_OF_GAS_AXIS[axis]] += m_momentum[region][axis];

                condensedEnergy[region] += m_kinetic[region];
                continue;
            }

            gas.momentum = m_momentum[region];
            gas.energy += m_kinetic[region];
        }

        // --- 1 刻み(圧力の解き方を選ぶ)---
        const GasProjection projectionBefore = m_projection;
        if (m_options.pressure == ProbeGasPressure::Explicit)
            StepGas(box);
        else
            report.projection = StepGasProjection(box, m_projection, m_options.projection);

        // --- 書き戻せるか(1 セルの成分がインラインの数に収まる。収まらなければ流さない)---
        std::vector<std::vector<SpeciesAmount>> merged(RegionCellCount());
        for (uint32_t region = 0; region < RegionCellCount(); ++region) {
            merged[region] = MergeCellSpecies(table, cells[GridIndex(region)], box.cells[region], slots);
            if (merged[region].size() > reaction::RX_MAX_CELL_SPECIES) {
                m_projection = projectionBefore;
                report.skippedCellOverflow = true;
                return report;
            }
        }

        for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
            m_ledger.momentum[axis] -= lostMomentum[axis];

        // --- 帳簿(物質 ID ごと・格子の軸)---
        for (uint32_t s = 0; s < slots.size(); ++s)
            m_ledger.amounts[slots[s]] += box.ledger.amounts[s];

        m_ledger.energy += box.ledger.energy;
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
            m_ledger.momentum[GRID_AXIS_OF_GAS_AXIS[axis]] += box.ledger.momentum[axis];

        // --- 気体の箱 → 区画のセル(凝縮相の成分とエネルギーはそのまま)---
        for (uint32_t region = 0; region < RegionCellCount(); ++region) {
            const uint32_t index = GridIndex(region);
            const reaction::RxCell& before = cells[index];
            const GasCell& gas = box.cells[region];
            const int64_t kinetic = (GasKineticEnergy(box, gas) + 500) / 1000;  // µJ → mJ(最も近い整数。負にならない)

            const std::vector<SpeciesAmount>& amounts = merged[region];
            reaction::RxCell after = reaction::RxMakeEmptyCell(condensedEnergy[region] + gas.energy - kinetic);
            for (uint32_t i = 0; i < amounts.size(); ++i) {
                after.species[i] = amounts[i].species;
                after.amounts[i] = amounts[i].amount;
            }

            after.speciesCount = (uint32_t)amounts.size();
            m_momentum[region] = gas.momentum;
            m_kinetic[region] = kinetic;

            if (SameCell(after, before))
                continue;

            cells[index] = after;
            changed.push_back(index);
            ++report.changedCells;
        }

        return report;
    }

    int64_t ProbeGasField::KineticEnergySum() const {
        int64_t sum = 0;
        for (const int64_t kinetic : m_kinetic)
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
            for (const int64_t momentum : m_momentum[region])
                mix((uint64_t)momentum);

            mix((uint64_t)m_kinetic[region]);
        }

        for (const int64_t amount : m_ledger.amounts)
            mix((uint64_t)amount);

        mix((uint64_t)m_ledger.energy);
        for (const int64_t momentum : m_ledger.momentum)
            mix((uint64_t)momentum);

        return hash;
    }

}  // namespace bicameral::sim
