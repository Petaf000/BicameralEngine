// gas_cells.cpp — 世界のセルと気体の箱の間の写し(T-0720)。約束は gas_cells.h。式は gas_flow.hlsli(GPU の probe_gas_tick.hlsl と同じ関数)。
// 仮の世界の sim/probe_gas.cpp(T-0590・T-0646)から移した(値は変えていない)。
#include "sim/gas_cells.h"

#include <algorithm>
#include <cassert>

#include "common/fixed.hlsli"
#include "common/gas_flow.hlsli"
#include "core/aliases.h"
#include "sim/world_atmosphere.h"
#include "sim/world_physics.h"

namespace bicameral::sim {

    namespace {

        constexpr uint64_t PARTS_TO_Q32_DIVISOR = ATMOSPHERE_PARTS_PER_WHOLE;

        bool SameCell(const reaction::RxCell& a, const reaction::RxCell& b) {
            if (a.energy != b.energy || a.speciesCount != b.speciesCount)
                return false;

            for (uint32_t i = 0; i < a.speciesCount; ++i) {
                if (a.species[i] != b.species[i] || a.amounts[i] != b.amounts[i])
                    return false;
            }

            return true;
        }

        // セルのエネルギーのうち気体のぶん(mJ。式は gas_flow.hlsli の GfGasEnergyPart。GPU の probe_gas_tick.hlsl と同じ関数)。
        // セルの温度を気体と凝縮相で同じと置く(局所の熱の釣り合い。反応と伝導の温度と同じ 1 つの温度)。セルが気体だけなら全部
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
                if (IsGasSpecies(table, cell.species[i])) {
                    hasGas = true;
                    gasChemical += amountChemical;
                    gasCapacity += amountCapacity;
                } else {
                    hasCondensed = true;
                }
            }

            return gas::GfGasEnergyPart(cell.energy, chemical, capacity, gasChemical, gasCapacity, hasGas,
                                        hasCondensed);
        }

        // 書き戻すセルの成分(物質 ID の順): 前のセルの凝縮相の成分 + 気体の箱の 0 でない成分
        std::vector<SpeciesAmount> MergeCellSpecies(const BakedReactionTable& table, const reaction::RxCell& before,
                                                    const GasCell& gas, std::span<const uint32_t> slots) {
            std::vector<SpeciesAmount> amounts;
            for (uint32_t i = 0; i < before.speciesCount; ++i) {
                if (!IsGasSpecies(table, before.species[i]))
                    amounts.push_back({.species = before.species[i], .amount = before.amounts[i]});
            }

            for (uint32_t s = 0; s < slots.size(); ++s) {
                if (gas.amounts[s] != 0)
                    amounts.push_back({.species = slots[s], .amount = gas.amounts[s]});
            }

            rng::sort(amounts, {}, &SpeciesAmount::species);

            return amounts;
        }

        // 気体の段の外(反応・相変化・置く操作)で、セルの気体の質量が前の段の終わりの before から now に変わった後の運動量
        // (gas_flow.hlsli の GfMomentumAfterMassChange。減れば速さを保つ・増えれば運動量を保つ。T-0646・ADR-0286)
        std::array<int64_t, GAS_AXES> MomentumAfterMassChange(const std::array<int64_t, GAS_AXES>& momentum,
                                                              uint64_t before, uint64_t now) {
            std::array<int64_t, GAS_AXES> kept = {};
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                kept[axis] = gas::GfMomentumAfterMassChange(momentum[axis], before, now);

            return kept;
        }

    }  // namespace

    // --- 層ごとの基準の大気 ---

    GasAtmosphereLayers GasAtmosphereLayers::Create(const BakedReactionTable& table, int64_t bottomHeightMillimeters,
                                                    uint32_t layerCount) {
        GasAtmosphereLayers layers;
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            const int64_t height = bottomHeightMillimeters + ((int64_t)layer * ATMOSPHERE_PRESSURE_STEP_MILLIMETERS);
            const AtmosphereSample sample = SampleAtmosphere(table.atmosphere, height);
            std::vector<std::array<uint32_t, 2>> fractions;
            for (const AtmosphereFraction& fraction : sample.fractions) {
                fractions.push_back({fraction.species, fraction.partsPerMillion});
                layers.m_species.push_back(fraction.species);
            }

            layers.m_layerFractions.push_back(std::move(fractions));
            layers.m_temperatures.push_back(sample.temperatureMilliKelvin);
        }

        rng::sort(layers.m_species);
        layers.m_species.erase(std::unique(layers.m_species.begin(), layers.m_species.end()), layers.m_species.end());
        layers.m_surfacePressureMicroPascal = AtmospherePressureMicroPascal(
            table.atmosphere, table, table.environment.gravityMicrometersPerSecond2, bottomHeightMillimeters);

        return layers;
    }

    GasConfig GasAtmosphereLayers::MakeConfig(const BakedReactionTable& table, std::span<const uint32_t> slots,
                                              std::array<uint32_t, GAS_AXES> sizes,
                                              std::array<GasBoundary, GAS_AXES> boundary, uint32_t substeps,
                                              uint64_t randomSeed) const {
        GasConfig config;
        config.sizeX = sizes[0];
        config.sizeY = sizes[1];
        config.sizeZ = sizes[2];
        config.boundary = boundary;
        config.speciesCount = (uint32_t)slots.size();
        for (uint32_t s = 0; s < slots.size(); ++s) {
            const reaction::RxSpecies& species = table.species[slots[s]];
            config.species[s] = {.molarMass = table.molarMasses[slots[s]],
                                 .heatCapacity = species.heatCapacity,
                                 .formationEnergy = species.h0,
                                 .roundingKey = slots[s]};  // 物質 ID(物質が増えても他の成分の丸めは変わらない)
        }

        config.substeps = substeps;
        config.gravity = table.environment.gravityMicrometersPerSecond2 != 0;
        ApplyWorldPhysics(table.environment, config);
        config.randomSeed = randomSeed;

        return config;
    }

    std::vector<GasCell> GasAtmosphereLayers::MakeReference(const GasConfig& config,
                                                            std::span<const uint32_t> slots) const {
        assert(config.sizeZ == LayerCount());
        std::vector<GasReferenceLayer> layers;
        for (uint32_t layer = 0; layer < config.sizeZ; ++layer) {
            GasReferenceLayer reference{.temperatureMilliKelvin = m_temperatures[layer]};
            for (const std::array<uint32_t, 2>& fraction : m_layerFractions[layer]) {
                const auto slot = rng::find(slots, fraction[0]);
                assert(slot != slots.end());
                const uint64_t q32 = ((uint64_t)fraction[1] << 32) / PARTS_TO_Q32_DIVISOR;
                reference.fractionsQ32[slot - slots.begin()] = (uint32_t)std::min<uint64_t>(q32, UINT32_MAX);
            }

            layers.push_back(reference);
        }

        return MakeHydrostaticReference(config, layers, m_surfacePressureMicroPascal);
    }

    // --- セル ⇔ 箱 ---

    bool IsGasSpecies(const BakedReactionTable& table, uint32_t species) {
        return species < table.speciesCondensed.size() ? table.speciesCondensed[species] == 0 : true;
    }

    std::vector<uint32_t> CollectGasSpecies(const BakedReactionTable& table, std::span<const reaction::RxCell> cells,
                                            std::span<const uint32_t> atmosphere) {
        std::vector<uint32_t> species(atmosphere.begin(), atmosphere.end());
        for (const reaction::RxCell& cell : cells) {
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                if (cell.amounts[i] != 0 && IsGasSpecies(table, cell.species[i]))
                    species.push_back(cell.species[i]);
            }
        }

        rng::sort(species);
        species.erase(std::unique(species.begin(), species.end()), species.end());

        return species;
    }

    void LoadGasCells(const BakedReactionTable& table, std::span<const reaction::RxCell> cells,
                      const GasCellFields& fields, GasCellsStep& step) {
        GasBox& box = step.box;
        assert(box.cells.size() == cells.size());
        step.condensedEnergy.assign(cells.size(), 0);
        step.lost = {};
        for (uint32_t index = 0; index < cells.size(); ++index) {
            const reaction::RxCell& cell = cells[index];
            GasCell& gas = box.cells[index];
            gas = {};
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                const auto slot = rng::find(step.slots, cell.species[i]);
                if (slot != step.slots.end())
                    gas.amounts[slot - step.slots.begin()] = cell.amounts[i];
            }

            const int64_t gasPart = GasEnergyPart(table, cell);
            step.condensedEnergy[index] = cell.energy - gasPart;
            gas.energy = gasPart;

            // 気体が無くなったセル(反応が使い切った)の運動量は外へ出たものとして帳簿へ、運動エネルギーのぶんは凝縮相の熱へ
            // (欄と帳簿は書き戻す時に替える。流さない刻みは何も変えない)
            const bool empty = std::ranges::all_of(gas.amounts, [](uint64_t amount) { return amount == 0; });
            if (empty) {
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                    step.lost[axis] += fields.momentum[index][axis];

                step.condensedEnergy[index] += fields.kinetic[index];
                continue;
            }

            // 段の外で質量が減ったセルは速さを保つ(減った気体の運動量は帳簿へ)。運動のエネルギーの減った分は、箱のエネルギーに
            // 前の運動エネルギーのぶんを足すので熱になる(T-0646)
            const std::array<int64_t, GAS_AXES> kept = MomentumAfterMassChange(
                fields.momentum[index], fields.mass[index], GasCellMass(box, gas));
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                step.lost[axis] += fields.momentum[index][axis] - kept[axis];

            gas.momentum = kept;
            gas.energy += fields.kinetic[index];
        }
    }

    bool StoreGasCells(const BakedReactionTable& table, const GasCellsStep& step, uint32_t restSpeedUnits,
                       std::span<reaction::RxCell> cells, GasCellFields& fields, GasCellsLedger& ledger,
                       std::vector<uint8_t>& changed) {
        const GasBox& box = step.box;

        // --- 書き戻せるか(1 セルの成分がインラインの数に収まる。収まらなければ流さない)---
        std::vector<std::vector<SpeciesAmount>> merged(cells.size());
        for (uint32_t index = 0; index < cells.size(); ++index) {
            merged[index] = MergeCellSpecies(table, cells[index], box.cells[index], step.slots);
            if (merged[index].size() > reaction::RX_MAX_CELL_SPECIES)
                return false;
        }

        // --- 帳簿(物質 ID ごと)---
        for (uint32_t s = 0; s < step.slots.size(); ++s)
            ledger.amounts[step.slots[s]] += box.ledger.amounts[s];

        ledger.energy += box.ledger.energy;
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
            ledger.momentum[axis] += box.ledger.momentum[axis] - step.lost[axis];

        // --- 気体の箱 → セル(凝縮相の成分とエネルギーはそのまま)---
        changed.assign(cells.size(), 0);
        for (uint32_t index = 0; index < cells.size(); ++index) {
            const GasCell& gas = box.cells[index];
            const int64_t kinetic = gas::GfKineticMilliJoule(GasKineticEnergy(box, gas));  // µJ → mJ(最も近い整数)
            const uint64_t mass = GasCellMass(box, gas);

            const std::vector<SpeciesAmount>& amounts = merged[index];
            reaction::RxCell after = reaction::RxMakeEmptyCell(step.condensedEnergy[index] + gas.energy - kinetic);
            for (uint32_t i = 0; i < amounts.size(); ++i) {
                after.species[i] = amounts[i].species;
                after.amounts[i] = amounts[i].amount;
            }

            after.speciesCount = (uint32_t)amounts.size();
            fields.momentum[index] = gas.momentum;
            fields.kinetic[index] = kinetic;
            fields.mass[index] = mass;

            // 気体が残らなかったセルの運動量は帳簿へ(質量の無い運動量は持たない。次に現れる気体は静止して現れる。T-0646)
            if (mass == 0) {
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                    ledger.momentum[axis] -= gas.momentum[axis];

                fields.momentum[index] = {0, 0, 0};
            }

            // 静止の丸め(T-0723。既定は切る): 速さがどの軸も restSpeedUnits 未満なら運動量を 0 に(帳簿へ)、運動エネルギーのぶんは熱へ
            std::array<int64_t, GAS_AXES>& momentum = fields.momentum[index];
            if (restSpeedUnits != 0 && mass != 0 && momentum != std::array<int64_t, GAS_AXES>{} &&
                rng::all_of(momentum, [&](int64_t value) { return fx::FxAbsU64(value) < mass * restSpeedUnits; })) {
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                    ledger.momentum[axis] -= momentum[axis];
                    ledger.restMomentum[axis] -= momentum[axis];
                }

                after.energy += fields.kinetic[index];
                ledger.restEnergy += fields.kinetic[index];
                fields.kinetic[index] = 0;
                momentum = {0, 0, 0};
                ++ledger.restedCells;
            }

            if (SameCell(after, cells[index]))
                continue;

            cells[index] = after;
            changed[index] = 1;
        }

        return true;
    }

}  // namespace bicameral::sim
