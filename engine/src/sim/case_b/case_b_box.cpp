// case_b_box.cpp — 案 B の試作の箱(B0。T-0866)の本体: 物質と環境を読む・形・受け渡し・見立て・1 刻み・保存・ハッシュ・粗くする。
// 何をするかは case_b_box.h。陰の圧力の口は case_b_pressure.cpp。
#include "sim/case_b/case_b_box.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <utility>

#include "common/fixed.hlsli"
#include "common/units.hlsli"
#include "core/aliases.h"

namespace bicameral::sim::case_b {

    namespace {

        namespace fx = bicameral::fx;

        constexpr int32_t MIN_LEVEL = -3;  // 4 m
        constexpr int32_t MAX_LEVEL = 2;   // 0.125 m(体積を mm³ の整数で持てる細かさまで)
        constexpr uint32_t Q16_ONE = 1u << 16;
        constexpr uint32_t LEGACY_LOOSE_GRAIN_UM = 300;  // solid_motion = "loose" だけの古い書き方(21 §6)
        constexpr uint32_t REPOSE_ANGLE_LIMIT_DEGREES = 90;

        void Add(std::vector<std::string>& list, std::string text) {
            list.push_back(std::move(text));
        }

        // 粒の欄(大きさ・くっつく強さ・詰まり)を書いたか
        bool HasGrainFields(const MaterialDefinition& definition) {
            return definition.grainSizeUm.has_value() || definition.cohesionPa.has_value() ||
                   definition.packedFractionPct.has_value();
        }

        // 気体・液体の欄の既定と知らせ
        void ResolveFluid(const MaterialDefinition& definition, Material& material, Diagnostics& diagnostics) {
            if (HasGrainFields(definition) || definition.solidMotionLoose)
                Add(diagnostics.errors,
                    std::format("{}: 気体・液体に粒の欄(grain_size_um など)は書けない", definition.name));

            if (definition.phase == WrittenPhase::Gas) {
                material.phase = Phase::Gas;
                material.viscosityMicroPascalSec = definition.viscosityMicroPascalSec.value_or(DEFAULT_GAS_VISCOSITY);
                if (!definition.viscosityMicroPascalSec.has_value())
                    Add(diagnostics.warnings,
                        std::format("{}: 気体の粘り viscosity_upa_s が無い(空気の値で数える)", definition.name));

                return;
            }

            material.phase = Phase::Liquid;
            material.viscosityMicroPascalSec = definition.viscosityMicroPascalSec.value_or(DEFAULT_LIQUID_VISCOSITY);
            if (!definition.viscosityMicroPascalSec.has_value())
                Add(diagnostics.warnings,
                    std::format("{}: 液体の粘り viscosity_upa_s が無い(水の値で数える)", definition.name));

            if (material.densityKgPerM3 == 0)
                Add(diagnostics.errors, std::format("{}: 液体に密度 density_kg_per_m3 が無い", definition.name));
        }

        // 粒の欄の既定と知らせ(21 §1.1・§1.3)
        void ResolveGrain(const MaterialDefinition& definition, uint32_t cellEdgeUm, Material& material,
                          Diagnostics& diagnostics) {
            material.phase = Phase::Grain;
            material.grainSizeUm = definition.grainSizeUm.value_or(LEGACY_LOOSE_GRAIN_UM);
            if (!definition.grainSizeUm.has_value()) {
                Add(diagnostics.warnings,
                    std::format("{}: solid_motion = \"loose\" だけで粒の大きさが無い(300 µm の粒として読む)",
                                definition.name));
            } else if (definition.solidMotionLoose) {
                Add(diagnostics.warnings,
                    std::format("{}: 粒の大きさがあるので solid_motion = \"loose\" は要らない(後で消す)",
                                definition.name));
            }

            if (material.grainSizeUm >= cellEdgeUm)
                Add(diagnostics.errors, std::format("{}: 粒の大きさがセル以上(物で書く)", definition.name));

            if (material.densityKgPerM3 == 0)
                Add(diagnostics.errors, std::format("{}: 粒に密度 density_kg_per_m3 が無い", definition.name));

            material.packedFractionPct = definition.packedFractionPct.value_or(DEFAULT_PACKED_FRACTION_PCT);
            if (material.packedFractionPct < PACKED_FRACTION_MIN_PCT ||
                material.packedFractionPct > PACKED_FRACTION_MAX_PCT) {
                Add(diagnostics.errors, std::format("{}: packed_fraction_pct {} が 30〜74 の外", definition.name,
                                                    material.packedFractionPct));
            }

            material.reposeAngleDegrees = definition.reposeAngleDegrees.value_or(DEFAULT_REPOSE_ANGLE_DEGREES);
            if (!definition.reposeAngleDegrees.has_value())
                Add(diagnostics.warnings,
                    std::format("{}: 崩れる角度 repose_angle_deg が無い(35 で読む)", definition.name));
            else if (material.reposeAngleDegrees > REPOSE_ANGLE_LIMIT_DEGREES)
                Add(diagnostics.errors, std::format("{}: repose_angle_deg が 90 を超える", definition.name));
            else if (material.reposeAngleDegrees == REPOSE_ANGLE_LIMIT_DEGREES)
                Add(diagnostics.warnings, std::format("{}: 崩れる角度 90 の粒(横には崩れない)", definition.name));

            material.cohesionPa = definition.cohesionPa.value_or(0);
        }

        uint32_t CellEdgeMm(int32_t level) {
            return CellEdgeUm(level) / 1000;
        }

        // 符号付きの a × b ÷ d(128bit の積)
        int64_t MulDiv(int64_t a, uint64_t b, uint64_t d) {
            return fx::FxMulDivS64(a, b, d);
        }

        // 体積の割合(Q16)= 体積(mm³)× 2^16 ÷ セルの体積
        uint32_t FractionQ16(uint64_t volumeMm3, uint64_t cellVolumeMm3) {
            return (uint32_t)std::min<uint64_t>(
                fx::FxDivU128By64(fx::FxMulU64Full(volumeMm3, Q16_ONE), cellVolumeMm3).quotient, UINT32_MAX);
        }

        // 質量(mg)と密度(kg/m³)から体積(mm³。1 mg ÷ 1 kg/m³ = 1000 mm³)
        uint64_t VolumeMm3(int64_t massMg, uint32_t densityKgPerM3) {
            if (massMg <= 0 || densityKgPerM3 == 0)
                return 0;

            return fx::FxDivU128By64(fx::FxMulU64Full((uint64_t)massMg, 1000), densityKgPerM3).quotient;
        }

        // 置き場 slot に入る相の質量
        int64_t SlotMass(MomentumLayout layout, const CellView& cell, uint32_t slot) {
            int64_t mass = 0;
            for (uint32_t phase = 0; phase < MOVING_PHASE_COUNT; ++phase) {
                if (MomentumSlot(layout, (Phase)phase) == slot)
                    mass += cell.phaseMassMg[phase];
            }

            return mass;
        }

        // 置き場 slot に入る質量(GrainTwoState の粒は層と舞う粒に分ける。層が無い箱 = 初めの刻みの前は全部層。T-1138)
        int64_t SlotMassAt(const Box& box, const View& view, uint32_t cell, uint32_t slot) {
            const MomentumLayout layout = box.config.layout;
            if (layout != MomentumLayout::GrainTwoState || slot == 0)
                return SlotMass(layout, view.cells[cell], slot);

            const int64_t grain = view.cells[cell].phaseMassMg[(uint32_t)Phase::Grain];
            if (box.grainLayerMg.size() != box.massMg.size())
                return slot == 1 ? grain : 0;

            const auto materials = (uint32_t)box.materials.size();
            int64_t layer = 0;
            for (uint32_t index = 0; index < materials; ++index) {
                if (box.materials[index].phase != Phase::Grain)
                    continue;

                const int64_t mass = std::max<int64_t>(CellMass(box, cell, index), 0);
                layer += std::clamp<int64_t>(box.grainLayerMg[((size_t)cell * materials) + index], 0, mass);
            }

            return slot == 1 ? layer : std::max<int64_t>(grain - layer, 0);
        }

        // 柱の軸(加速の絶対値が最も大きい軸。全部 0 なら y)
        void FindGravityAxis(const BoxConfig& config, View& view) {
            view.gravityAxis = 1;
            view.gravityUmPerS2 = 0;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const int64_t magnitude = (int64_t)fx::FxAbsU64(config.accelerationUmPerS2[axis]);
                if (magnitude > view.gravityUmPerS2) {
                    view.gravityAxis = axis;
                    view.gravityUmPerS2 = magnitude;
                }
            }

            view.gravityDown = config.accelerationUmPerS2[view.gravityAxis] <= 0;
        }

        // 相の質量と体積の割合
        CellView MeasureCell(const Box& box, uint32_t cell, uint64_t cellVolumeMm3) {
            CellView view;
            uint64_t liquidMm3 = 0;
            uint64_t grainMm3 = 0;
            uint64_t grainPackedMm3 = 0;
            uint64_t fixedMm3 = 0;
            for (uint32_t index = 0; index < box.materials.size(); ++index) {
                const Material& material = box.materials[index];
                const int64_t mass = CellMass(box, cell, index);
                const uint64_t volume = VolumeMm3(mass, material.densityKgPerM3);
                view.totalMassMg += mass;
                if (material.phase == Phase::Fixed) {
                    view.fixedMassMg += mass;
                    fixedMm3 += volume;
                    continue;
                }

                view.phaseMassMg[(uint32_t)material.phase] += mass;
                if (material.phase == Phase::Liquid)
                    liquidMm3 += volume;

                if (material.phase == Phase::Grain) {
                    grainMm3 += volume;
                    grainPackedMm3 += volume * 100 / material.packedFractionPct;
                }
            }

            view.liquidQ16 = FractionQ16(liquidMm3, cellVolumeMm3);
            view.grainSolidQ16 = FractionQ16(grainMm3, cellVolumeMm3);
            view.grainPackedQ16 = FractionQ16(grainPackedMm3, cellVolumeMm3);
            view.fixedQ16 = FractionQ16(fixedMm3, cellVolumeMm3);
            view.bodyQ16 = box.bodyOccupancyQ16[cell];

            const uint64_t taken = (uint64_t)view.liquidQ16 + view.grainSolidQ16 + view.fixedQ16 + view.bodyQ16;
            view.overfilled = taken > Q16_ONE;
            view.openQ16 = view.overfilled ? 0 : (uint32_t)(Q16_ONE - taken);

            return view;
        }

        // 柱: 重さの逆向きの端から、自分より上のセルの重さを足していく
        void BuildColumns(const Box& box, View& view) {
            const uint64_t areaMm2 = CellAreaMm2(box);
            const std::array<uint32_t, 3> size = {box.config.sizeX, box.config.sizeY, box.config.sizeZ};
            for (uint32_t cell = 0; cell < view.cells.size(); ++cell) {
                // 重さ(1e-12 N)÷ 面積(mm²)= µPa
                const int64_t force = view.cells[cell].totalMassMg * view.gravityUmPerS2;
                view.cells[cell].weightMicroPascal = force / (int64_t)areaMm2;
            }

            const uint32_t axis = view.gravityAxis;
            for (uint32_t cell = 0; cell < view.cells.size(); ++cell) {
                std::array<uint32_t, 3> at = CellCoordinates(box, cell);
                if (at[axis] != (view.gravityDown ? size[axis] - 1 : 0))
                    continue;

                // 上の端のセルから下へ
                int64_t above = 0;
                for (uint32_t step = 0; step < size[axis]; ++step) {
                    const uint32_t index = CellIndex(box, at[0], at[1], at[2]);
                    view.cells[index].columnAboveMicroPascal = above;
                    above += view.cells[index].weightMicroPascal;
                    at[axis] = view.gravityDown ? at[axis] - 1 : at[axis] + 1;
                }
            }
        }

        uint64_t HashVector(uint64_t hash, std::span<const int64_t> values) {
            hash = fx::FxHashCombine(hash, values.size());
            for (const int64_t value : values)
                hash = fx::FxHashCombine(hash, (uint64_t)value);

            return hash;
        }

    }  // namespace

    // --- 読む ---
    std::vector<Material> ResolveMaterials(std::span<const MaterialDefinition> materials, uint32_t cellEdgeUm,
                                           Diagnostics& diagnostics) {
        std::vector<Material> resolved;
        resolved.reserve(materials.size());
        for (const MaterialDefinition& definition : materials) {
            Material material;
            material.name = definition.name;
            material.molarMassMgPerMol = definition.molarMassMgPerMol;
            material.heatCapacity = definition.heatCapacity;
            material.densityKgPerM3 = definition.densityKgPerM3.value_or(0);
            material.permeabilityUm2 = definition.permeabilityUm2.value_or(0);
            material.suctionPa = definition.suctionPa.value_or(0);
            if (material.molarMassMgPerMol == 0)
                Add(diagnostics.errors, std::format("{}: モル質量が無い", definition.name));

            const bool grain = definition.phase == WrittenPhase::Solid &&
                               (definition.grainSizeUm.has_value() || definition.solidMotionLoose);
            if (definition.phase != WrittenPhase::Solid)
                ResolveFluid(definition, material, diagnostics);
            else if (grain)
                ResolveGrain(definition, cellEdgeUm, material, diagnostics);
            else {
                material.phase = Phase::Fixed;
                if (definition.cohesionPa.has_value() || definition.packedFractionPct.has_value())
                    Add(diagnostics.warnings,
                        std::format("{}: 粒の大きさが無いので据わる(粒の欄は読まない)", definition.name));
            }

            resolved.push_back(std::move(material));
        }

        return resolved;
    }

    Environment ResolveEnvironment(const EnvironmentDefinition& environment, Diagnostics& diagnostics) {
        Environment resolved;
        resolved.dropSizeUm = environment.dropSizeUm.value_or(DEFAULT_DROP_SIZE_UM);
        resolved.shockZonePa = environment.shockZonePa.value_or(DEFAULT_SHOCK_ZONE_PA);
        resolved.pressureRelaxMsPerM = environment.pressureRelaxMsPerM.value_or(DEFAULT_PRESSURE_RELAX_MS_PER_M);
        resolved.shockZoneSpeedMmPerS = environment.shockZoneSpeedMmPerS.value_or(0);
        resolved.shockZoneMarginMm = environment.shockZoneMarginMm.value_or(0);
        resolved.grainPassRatioPct = environment.grainPassRatioPct.value_or(DEFAULT_GRAIN_PASS_RATIO_PCT);
        if (resolved.shockZonePa <= 0)
            Add(diagnostics.errors, "環境: shock_zone_pa が 0 以下");
        else if (resolved.shockZonePa * 100 < ATMOSPHERE_PA)
            Add(diagnostics.warnings, "環境: shock_zone_pa が大気の 1 % 未満(重さの検査で知らせる)");

        if (resolved.pressureRelaxMsPerM < 0)
            Add(diagnostics.errors, "環境: pressure_relax_ms_per_m が負");

        return resolved;
    }

    std::expected<Box, std::string> CreateBox(const BoxConfig& config, std::span<const MaterialDefinition> materials,
                                              const EnvironmentDefinition& environment, Diagnostics* diagnostics) {
        Diagnostics local;
        Diagnostics& out = diagnostics != nullptr ? *diagnostics : local;
        if (config.sizeX == 0 || config.sizeY == 0 || config.sizeZ == 0)
            Add(out.errors, "箱: 大きさが 0");

        if (config.level < MIN_LEVEL || config.level > MAX_LEVEL)
            Add(out.errors, std::format("箱: レベル {} が {}〜{} の外", config.level, MIN_LEVEL, MAX_LEVEL));

        if (config.ticksPerSecond == 0)
            Add(out.errors, "箱: 1 秒の刻みの数が 0");

        const int32_t level = std::clamp(config.level, MIN_LEVEL, MAX_LEVEL);
        Box box;
        box.config = config;
        box.materials = ResolveMaterials(materials, CellEdgeUm(level), out);
        box.environment = ResolveEnvironment(environment, out);
        if (!out.errors.empty()) {
            std::string text;
            for (const std::string& error : out.errors)
                text += error + "\n";

            return std::unexpected(text);
        }

        const uint32_t cells = CellCount(box);
        const uint32_t slots = SlotCount(config.layout);
        box.massMg.assign((size_t)cells * box.materials.size(), 0);
        box.energyMillijoules.assign(cells, 0);
        box.bodyOccupancyQ16.assign(cells, 0);
        box.bodyVelocity.assign(cells, {});
        box.pressureMicroPascal.assign(cells, 0);
        for (uint32_t axis = 0; axis < 3; ++axis) {
            box.faceMomentum[axis].assign((size_t)cells * slots, 0);
            box.faceCarry[axis].assign((size_t)cells * slots, 0);
        }

        ResetLedger(box);

        return box;
    }

    // --- 形 ---
    uint32_t CellCount(const Box& box) {
        return box.config.sizeX * box.config.sizeY * box.config.sizeZ;
    }

    uint32_t CellIndex(const Box& box, uint32_t x, uint32_t y, uint32_t z) {
        assert(x < box.config.sizeX && y < box.config.sizeY && z < box.config.sizeZ);
        return x + (box.config.sizeX * (y + (box.config.sizeY * z)));
    }

    std::array<uint32_t, 3> CellCoordinates(const Box& box, uint32_t cell) {
        const uint32_t x = cell % box.config.sizeX;
        const uint32_t y = (cell / box.config.sizeX) % box.config.sizeY;
        const uint32_t z = cell / (box.config.sizeX * box.config.sizeY);

        return {x, y, z};
    }

    uint32_t SlotCount(MomentumLayout layout) {
        if (layout == MomentumLayout::GrainSeparate)
            return 2;

        return 3;  // ①(相ごと)・GrainTwoState(流体・層・舞う粒)
    }

    uint32_t MomentumSlot(MomentumLayout layout, Phase phase) {
        assert((uint32_t)phase < MOVING_PHASE_COUNT);
        if (layout == MomentumLayout::PerPhase)
            return (uint32_t)phase;

        return phase == Phase::Grain ? 1 : 0;
    }

    uint32_t SuspendedGrainSlot(MomentumLayout layout) {
        return layout == MomentumLayout::GrainTwoState ? 2 : MomentumSlot(layout, Phase::Grain);
    }

    uint32_t FaceIndex(const Box& box, uint32_t cell, uint32_t slot) {
        return (cell * SlotCount(box.config.layout)) + slot;
    }

    std::optional<uint32_t> Neighbor(const Box& box, uint32_t cell, uint32_t axis) {
        std::array<uint32_t, 3> at = CellCoordinates(box, cell);
        const std::array<uint32_t, 3> size = {box.config.sizeX, box.config.sizeY, box.config.sizeZ};
        if (at[axis] + 1 >= size[axis])
            return std::nullopt;

        ++at[axis];

        return CellIndex(box, at[0], at[1], at[2]);
    }

    uint32_t FaceBlockedQ16(const Box& box, uint32_t axis, uint32_t cell) {
        const std::vector<uint32_t>& values = box.faceBlockedQ16[axis];
        return cell < values.size() ? values[cell] : 0u;
    }

    uint32_t FaceHoleSizeUm(const Box& box, uint32_t axis, uint32_t cell) {
        const std::vector<uint32_t>& values = box.faceHoleSizeUm[axis];
        return cell < values.size() ? values[cell] : 0u;
    }

    uint32_t CellEdgeUm(int32_t level) {
        return level >= 0 ? LEVEL0_CELL_EDGE_UM >> level : LEVEL0_CELL_EDGE_UM << (-level);
    }

    uint64_t CellAreaMm2(const Box& box) {
        const uint64_t edge = CellEdgeMm(box.config.level);
        return edge * edge;
    }

    uint64_t CellVolumeMm3(const Box& box) {
        const uint64_t edge = CellEdgeMm(box.config.level);
        return edge * edge * edge;
    }

    uint32_t MaterialIndex(const Box& box, std::string_view name) {
        for (uint32_t index = 0; index < box.materials.size(); ++index) {
            if (box.materials[index].name == name)
                return index;
        }

        return (uint32_t)box.materials.size();
    }

    int64_t& CellMass(Box& box, uint32_t cell, uint32_t material) {
        return box.massMg[((size_t)cell * box.materials.size()) + material];
    }

    int64_t CellMass(const Box& box, uint32_t cell, uint32_t material) {
        return box.massMg[((size_t)cell * box.materials.size()) + material];
    }

    // --- 受け渡し ---
    void MoveMass(Box& box, uint32_t from, uint32_t to, uint32_t material, int64_t massMg) {
        CellMass(box, from, material) -= massMg;
        CellMass(box, to, material) += massMg;
    }

    void ExchangeMass(Box& box, uint32_t cell, uint32_t material, int64_t massMg) {
        CellMass(box, cell, material) += massMg;
        box.ledger.massInMg[material] += massMg;
    }

    void MoveEnergy(Box& box, uint32_t from, uint32_t to, int64_t millijoules) {
        box.energyMillijoules[from] -= millijoules;
        box.energyMillijoules[to] += millijoules;
    }

    void ExchangeEnergy(Box& box, uint32_t cell, int64_t millijoules, EnergySource source) {
        box.energyMillijoules[cell] += millijoules;
        box.ledger.energyInMillijoules[(uint32_t)source] += millijoules;
    }

    void MoveMomentum(Box& box, FaceRef from, FaceRef to, int64_t momentum) {
        assert(from.axis == to.axis);
        box.faceMomentum[from.axis][FaceIndex(box, from.cell, from.slot)] -= momentum;
        box.faceMomentum[to.axis][FaceIndex(box, to.cell, to.slot)] += momentum;
    }

    void ApplyImpulse(Box& box, FaceRef face, int64_t momentum, ImpulseSource source) {
        box.faceMomentum[face.axis][FaceIndex(box, face.cell, face.slot)] += momentum;
        box.ledger.impulses[(uint32_t)source][face.axis] += momentum;
    }

    // --- 見立て ---
    View BuildView(const Box& box) {
        View view;
        const uint64_t cellVolume = CellVolumeMm3(box);
        view.cells.resize(CellCount(box));
        for (uint32_t cell = 0; cell < view.cells.size(); ++cell)
            view.cells[cell] = MeasureCell(box, cell, cellVolume);

        FindGravityAxis(box.config, view);
        BuildColumns(box, view);

        return view;
    }

    int64_t FaceVelocity(const Box& box, const View& view, FaceRef face) {
        const std::optional<uint32_t> neighbor = Neighbor(box, face.cell, face.axis);
        if (!neighbor.has_value())
            return 0;

        const int64_t mass = SlotMassAt(box, view, face.cell, face.slot) + SlotMassAt(box, view, *neighbor, face.slot);
        if (mass <= 0)
            return 0;

        const int64_t momentum = box.faceMomentum[face.axis][FaceIndex(box, face.cell, face.slot)];

        return MulDiv(momentum, 2, (uint64_t)mass);
    }

    std::array<int64_t, 3> CellPhaseVelocity(const Box& box, const View& view, uint32_t cell, Phase phase) {
        const uint32_t slot = MomentumSlot(box.config.layout, phase);
        std::array<int64_t, 3> velocity{};
        for (uint32_t axis = 0; axis < 3; ++axis) {
            // − 側の面は − 側の隣のセルが持つ
            std::array<uint32_t, 3> at = CellCoordinates(box, cell);
            const int64_t plus = FaceVelocity(box, view, {.axis = axis, .cell = cell, .slot = slot});
            int64_t minus = 0;
            if (at[axis] > 0) {
                --at[axis];
                minus = FaceVelocity(box, view,
                                     {.axis = axis, .cell = CellIndex(box, at[0], at[1], at[2]), .slot = slot});
            }

            velocity[axis] = (plus + minus) / 2;
        }

        return velocity;
    }

    // --- 1 刻み ---
    void StepBox(Box& box, const Stages& stages, const PressureSolver& pressureSolver) {
        const View view = BuildView(box);
        const StepContext context = {.view = view, .pressureSolver = pressureSolver};
        const std::array<const Stage*, 7> order = {&stages.drag,     &stages.pressure,   &stages.shockZone,
                                                   &stages.momentum, &stages.grainYield, &stages.transport,
                                                   &stages.apply};
        for (const Stage* stage : order) {
            if (*stage)
                (*stage)(box, context);
        }

        ++box.tick;
    }

    // --- 保存と決定性 ---
    ConservedTotals MeasureTotals(const Box& box) {
        ConservedTotals totals;
        const auto materialCount = (uint32_t)box.materials.size();
        totals.massMg.assign(materialCount, 0);
        for (size_t index = 0; materialCount != 0 && index < box.massMg.size(); ++index)
            totals.massMg[index % materialCount] += box.massMg[index];

        for (const int64_t energy : box.energyMillijoules)
            totals.energyMillijoules += energy;

        for (uint32_t axis = 0; axis < 3; ++axis) {
            for (const int64_t momentum : box.faceMomentum[axis])
                totals.momentum[axis] += momentum;
        }

        return totals;
    }

    bool ConservationError::IsZero() const {
        const bool massZero = rng::all_of(massMg, [](int64_t value) { return value == 0; });
        const bool momentumZero = rng::all_of(momentum, [](int64_t value) { return value == 0; });

        return massZero && momentumZero && energyMillijoules == 0 && wallFaces == 0 && !negativeMass;
    }

    ConservationError CheckConservation(const Box& box) {
        const ConservedTotals now = MeasureTotals(box);
        ConservationError error;
        error.massMg.resize(now.massMg.size());
        for (size_t index = 0; index < now.massMg.size(); ++index)
            error.massMg[index] = now.massMg[index] - box.initial.massMg[index] - box.ledger.massInMg[index];

        error.energyMillijoules = now.energyMillijoules - box.initial.energyMillijoules;
        for (const int64_t energy : box.ledger.energyInMillijoules)
            error.energyMillijoules -= energy;

        for (uint32_t axis = 0; axis < 3; ++axis) {
            error.momentum[axis] = now.momentum[axis] - box.initial.momentum[axis];
            for (const auto& source : box.ledger.impulses)
                error.momentum[axis] -= source[axis];
        }

        // 箱の外側の面(壁)には運動量を持たない
        const uint32_t slots = SlotCount(box.config.layout);
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            for (uint32_t axis = 0; axis < 3; ++axis) {
                if (Neighbor(box, cell, axis).has_value())
                    continue;

                for (uint32_t slot = 0; slot < slots; ++slot)
                    error.wallFaces += box.faceMomentum[axis][FaceIndex(box, cell, slot)] != 0 ? 1u : 0u;
            }
        }

        error.negativeMass = rng::any_of(box.massMg, [](int64_t value) { return value < 0; });
        // 層は 0 以上・そのセルの質量以下(T-1136。層は質量の内訳)
        for (size_t index = 0; index < box.grainLayerMg.size(); ++index)
            error.negativeMass = error.negativeMass || box.grainLayerMg[index] < 0 ||
                                 box.grainLayerMg[index] > box.massMg[index];

        return error;
    }

    void ResetLedger(Box& box) {
        box.ledger = {};
        box.ledger.massInMg.assign(box.materials.size(), 0);
        box.initial = MeasureTotals(box);
    }

    uint64_t HashBox(const Box& box) {
        uint64_t hash = fx::FxHash64(box.config.seed, box.tick, CellCount(box), (uint32_t)box.config.layout);
        hash = HashVector(hash, box.massMg);
        hash = HashVector(hash, box.energyMillijoules);
        for (uint32_t cell = 0; cell < CellCount(box); ++cell) {
            hash = fx::FxHashCombine(hash, box.bodyOccupancyQ16[cell]);
            hash = HashVector(hash, box.bodyVelocity[cell]);
        }

        hash = HashVector(hash, box.pressureMicroPascal);
        // 層が全部 0 の箱(粒が無い)は層を持たない箱と同じハッシュ(T-1136)
        if (rng::any_of(box.grainLayerMg, [](int64_t value) { return value != 0; }))
            hash = HashVector(hash, box.grainLayerMg);

        for (uint32_t axis = 0; axis < 3; ++axis) {
            hash = HashVector(hash, box.faceMomentum[axis]);
            hash = HashVector(hash, box.faceCarry[axis]);
        }

        hash = HashVector(hash, box.ledger.massInMg);
        hash = HashVector(hash, box.ledger.energyInMillijoules);
        for (const auto& source : box.ledger.impulses)
            hash = HashVector(hash, source);

        return hash;
    }

    // --- 粗くする ---
    std::expected<Box, std::string> CoarsenBox(const Box& fine, uint32_t factor) {
        uint32_t shift = 0;
        while ((1u << shift) < factor)
            ++shift;

        const BoxConfig& config = fine.config;
        if (factor == 0 || (1u << shift) != factor)
            return std::unexpected(std::format("粗くする倍率 {} が 2 の冪でない", factor));

        // 厚さ 1 セルの軸は板として押し出す(粗いセルは細かい板 factor 枚分の厚さ。場面の箱と同じ。BuildScene)
        const std::array<uint32_t, 3> size = {config.sizeX, config.sizeY, config.sizeZ};
        std::array<uint32_t, 3> merge{};
        int64_t extrude = 1;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            merge[axis] = size[axis] == 1 ? 1 : factor;
            extrude *= size[axis] == 1 ? factor : 1;
            if (size[axis] % merge[axis] != 0)
                return std::unexpected(std::format("箱の大きさが倍率 {} で割り切れない", factor));
        }

        if (config.level - (int32_t)shift < MIN_LEVEL)
            return std::unexpected(std::format("粗くするとレベルが {} より粗い", MIN_LEVEL));

        for (const auto& faces : fine.faceMomentum) {
            if (rng::any_of(faces, [](int64_t value) { return value != 0; }))
                return std::unexpected("動き出した箱は粗くしない(面の運動量が 0 でない)");
        }

        Box coarse = fine;
        coarse.config.sizeX /= merge[0];
        coarse.config.sizeY /= merge[1];
        coarse.config.sizeZ /= merge[2];
        coarse.config.level -= (int32_t)shift;

        const uint32_t cells = CellCount(coarse);
        const size_t materials = fine.materials.size();
        const uint32_t slots = SlotCount(config.layout);
        coarse.massMg.assign((size_t)cells * materials, 0);
        coarse.energyMillijoules.assign(cells, 0);
        coarse.bodyOccupancyQ16.assign(cells, 0);
        coarse.bodyVelocity.assign(cells, {});
        coarse.pressureMicroPascal.assign(cells, 0);
        for (uint32_t axis = 0; axis < 3; ++axis) {
            coarse.faceMomentum[axis].assign((size_t)cells * slots, 0);
            coarse.faceCarry[axis].assign((size_t)cells * slots, 0);
            // 面の開き具合は導かれる値(形から作り直す)。試作の箱は形を持たないので空(塞がない)に戻す
            coarse.faceBlockedQ16[axis].clear();
            coarse.faceHoleSizeUm[axis].clear();
        }

        // 和(真実)と平均(導かれる値・割合)
        std::vector<int64_t> occupancy(cells, 0);
        std::vector<int64_t> pressure(cells, 0);
        for (uint32_t cell = 0; cell < CellCount(fine); ++cell) {
            const std::array<uint32_t, 3> at = CellCoordinates(fine, cell);
            const uint32_t target = CellIndex(coarse, at[0] / merge[0], at[1] / merge[1], at[2] / merge[2]);
            for (uint32_t material = 0; material < materials; ++material)
                CellMass(coarse, target, material) += CellMass(fine, cell, material) * extrude;

            // 層も和(21 §2.12。T-1136)
            if (!fine.grainLayerMg.empty()) {
                coarse.grainLayerMg.resize(coarse.massMg.size(), 0);
                for (uint32_t material = 0; material < materials; ++material)
                    coarse.grainLayerMg[((size_t)target * materials) +
                                        material] += fine.grainLayerMg[((size_t)cell * materials) + material] * extrude;
            }

            coarse.energyMillijoules[target] += fine.energyMillijoules[cell] * extrude;
            occupancy[target] += fine.bodyOccupancyQ16[cell];
            pressure[target] += fine.pressureMicroPascal[cell];
        }

        const int64_t children = (int64_t)merge[0] * merge[1] * merge[2];
        for (uint32_t cell = 0; cell < cells; ++cell) {
            coarse.bodyOccupancyQ16[cell] = (uint32_t)(occupancy[cell] / children);
            coarse.pressureMicroPascal[cell] = pressure[cell] / children;
        }

        return coarse;
    }

    // --- 初めの中身を作る道具 ---
    int64_t ThermalEnergyMillijoules(const Box& box, uint32_t cell, uint32_t temperatureMilliKelvin) {
        int64_t energy = 0;
        for (uint32_t index = 0; index < box.materials.size(); ++index) {
            const Material& material = box.materials[index];
            const int64_t mass = CellMass(box, cell, index);
            if (mass <= 0 || material.molarMassMgPerMol == 0)
                continue;

            // 質量 ÷ モル質量 × 比熱(mJ/(mol·K))× 温度(mK ÷ 1000)
            const fx::FxU128 product = fx::FxMulU64Full((uint64_t)mass * material.heatCapacity, temperatureMilliKelvin);
            energy += (int64_t)fx::FxDivU128By64(product, (uint64_t)material.molarMassMgPerMol * 1000).quotient;
        }

        return energy;
    }

    int64_t IdealGasMassMg(uint64_t pressurePa, uint64_t volumeMm3, uint32_t molarMassMgPerMol,
                           uint32_t temperatureMilliKelvin) {
        // m = p V M ÷ (R T)。p Pa・V mm³(1e-9 m³)・R nJ/(mol·K)・T mK → p V M × 1000 ÷ (R_nJ × T_mK)
        const fx::FxU128 numerator = fx::FxMulU64Full(pressurePa * volumeMm3, (uint64_t)molarMassMgPerMol * 1000);
        const uint64_t denominator = fx::GAS_CONSTANT_NANOJOULES_PER_MOLE_KELVIN * temperatureMilliKelvin;

        return (int64_t)fx::FxDivU128By64(numerator, denominator).quotient;
    }

}  // namespace bicameral::sim::case_b
