// case_b_flow.cpp — 案 B の試作の研究 R1(T-0868): 気体と液体を 1 つの陰の圧力で動かす段。式と単位は case_b_flow.h。
// 段の間で渡すもの(状態式の結果・面の流れ)は FlowScratch に置き、pressure → transport → apply の順に使う(同じ刻みの中だけ)。
// どの面・セルも刻みの初めの値から決め、移す量はすべて受け渡しの関数(MoveMass・MoveEnergy・MoveMomentum・ApplyImpulse)で当てる(04 R2・R3)。
#include "sim/case_b/case_b_flow.h"

#include <algorithm>
#include <cassert>
#include <vector>

#include "common/fixed.hlsli"
#include "common/units.hlsli"

namespace bicameral::sim::case_b {

    namespace {

        namespace fx = bicameral::fx;

        constexpr uint32_t Q16_ONE = 1u << 16;
        constexpr uint32_t CAPACITY_SHIFT = 48;
        constexpr uint64_t NL_PER_MM3 = 1000;
        constexpr int64_t ATMOSPHERE_MICRO_PASCAL = ATMOSPHERE_PA * 1000000;
        constexpr int64_t PRESSURE_MAX_MICRO_PASCAL = (int64_t)1 << 45;  // 約 35 MPa(状態式の探す範囲)
        // 真空の縮みやすさ = 真空の体積 ÷ 大気(真空を埋める体積が入ると、圧力が大気ほど上がる線形化。T-0868 の仮)。
        // 1 kPa で割ると「空きの 100 倍を吸い込める」線形化になり、埋まった後に溢れて暴れた(1 本目の最初の測り)
        constexpr uint64_t VOID_PRESSURE_MICRO_PASCAL = (uint64_t)ATMOSPHERE_PA * 1000000;
        constexpr uint64_t COEFFICIENT_LIMIT = (uint64_t)1
                                               << 58;  // K がこれ以上の面は閉じる(対角が 64bit に入るように)
        constexpr int64_t SATURATED_VOLUME = (int64_t)1 << 60;
        constexpr uint32_t HEAT_CAPACITY_SHIFT = 16;

        // 面(セルの + 側。置き場は流体 = 0)
        struct FlowFace {
            uint32_t axis = 0;
            uint32_t left = 0;
            uint32_t right = 0;
            bool open = false;
            uint64_t areaMm2 = 0;  // 開いた面積
            int64_t massMg = 0;    // 面の流体の質量(両側の平均)
            uint64_t coefficientQ48 = 0;
            int64_t flowNl = 0;       // 解いた後の体積の流れ(left → right が正)
            int64_t movedMassMg = 0;  // 運ぶ段が実際に移した質量(left → right が正)
        };

        struct FlowScratch {
            std::vector<FlowCell> cells;
            std::vector<uint64_t> heatCapacityQ16;  // セルの全部の物質の熱容量(mJ/K の Q16)
            std::vector<int64_t> columnMicroPascal;
            std::vector<FlowFace> faces;
            std::vector<uint32_t> viscosityMicroPascalSec;  // セルの流体の粘り(体積の割合の平均)
        };

        // --- 単位の換算 ---
        uint64_t MaterialVolumeNl(int64_t massMg, uint32_t densityKgPerM3) {
            if (massMg <= 0 || densityKgPerM3 == 0)
                return 0;

            // 1 mg ÷ 1 kg/m³ = 10^6 nL
            return fx::FxDivU128By64(fx::FxMulU64Full((uint64_t)massMg, 1000000), densityKgPerM3).quotient;
        }

        uint64_t CellVolumeNl(const Box& box) {
            return CellVolumeMm3(box) * NL_PER_MM3;
        }

        uint32_t OpenQ16(const CellView& view) {
            const uint64_t taken = (uint64_t)view.grainSolidQ16 + view.fixedQ16 + view.bodyQ16;
            return taken >= Q16_ONE ? 0 : (uint32_t)(Q16_ONE - taken);
        }

        // 圧力の差 Δp(µPa)が面積 A(mm²)に 1 刻みに与える力積(mg·2^-20 m/s)= Δp A 2^20 ÷ (刻み/秒 × 10^6)
        int64_t PressureImpulse(const Box& box, int64_t deltaMicroPascal, uint64_t areaMm2) {
            return fx::FxMulDivS64(deltaMicroPascal, areaMm2 << 20, (uint64_t)box.config.ticksPerSecond * 1000000);
        }

        // 面の運動量 M の 1 刻みの体積の流れ(nL)= M A 10^6 ÷ (m_f 2^20 刻み/秒)
        int64_t FlowNl(const Box& box, int64_t momentum, uint64_t areaMm2, int64_t massMg) {
            if (massMg <= 0 || areaMm2 == 0)
                return 0;

            const uint64_t denominator = ((uint64_t)massMg << 20) * box.config.ticksPerSecond;
            return fx::FxMulDivS64(momentum, areaMm2 * 1000000, denominator);
        }

        // --- 状態式 ---
        struct FluidContent {
            uint64_t liquidReferenceNl = 0;  // 大気の圧力での液体の体積
            uint64_t stiffnessQ48 = 0;       // S = Σ V ÷ ρc²(nL/µPa の Q48)
            uint64_t gasConstant = 0;        // G = Σ n R T(nJ。V_g(nL)= G × 10^9 ÷ p(µPa))
            int64_t fluidMassMg = 0;
        };

        uint64_t HeatCapacityQ16(const Box& box, uint32_t material, int64_t massMg) {
            const Material& value = box.materials[material];
            if (massMg <= 0 || value.molarMassMgPerMol == 0)
                return 0;

            const fx::FxU128 product = fx::FxMulU64Full((uint64_t)massMg,
                                                        (uint64_t)value.heatCapacity << HEAT_CAPACITY_SHIFT);
            return fx::FxDivU128By64(product, value.molarMassMgPerMol).quotient;
        }

        uint64_t CellHeatCapacityQ16(const Box& box, uint32_t cell) {
            uint64_t total = 0;
            for (uint32_t material = 0; material < box.materials.size(); ++material)
                total += HeatCapacityQ16(box, material, CellMass(box, cell, material));

            return total;
        }

        FluidContent MeasureContent(const Box& box, uint32_t cell, const FlowOptions& options) {
            FluidContent content;
            const uint64_t heatCapacity = CellHeatCapacityQ16(box, cell);
            const int64_t energy = std::max<int64_t>(box.energyMillijoules[cell], 0);
            // 温度(mK)= E × 1000 ÷ 熱容量
            const uint64_t temperature = heatCapacity == 0
                                             ? 0
                                             : fx::FxDivU128By64(fx::FxMulU64Full((uint64_t)energy, 1000ull << 16),
                                                                 heatCapacity)
                                                   .quotient;
            const uint64_t soundSquared = (uint64_t)options.liquidSoundSpeedMmPerS * options.liquidSoundSpeedMmPerS;
            for (uint32_t index = 0; index < box.materials.size(); ++index) {
                const Material& material = box.materials[index];
                const int64_t mass = CellMass(box, cell, index);
                if (mass <= 0)
                    continue;

                if (material.phase == Phase::Liquid) {
                    const uint64_t volume = MaterialVolumeNl(mass, material.densityKgPerM3);
                    content.liquidReferenceNl += volume;
                    // ρc²(µPa)= ρ(kg/m³)× c²(mm²/s²)
                    content.stiffnessQ48 += fx::FxDivU128By64(fx::FxMulU64Full(volume, (uint64_t)1 << CAPACITY_SHIFT),
                                                              material.densityKgPerM3 * soundSquared)
                                                .quotient;
                    content.fluidMassMg += mass;
                } else if (material.phase == Phase::Gas && material.molarMassMgPerMol != 0) {
                    // n R T(nJ)= m ÷ M × R(nJ/(mol·K))× T(mK)÷ 1000
                    const fx::FxU128 product = fx::FxMulU64Full(
                        (uint64_t)mass * fx::GAS_CONSTANT_NANOJOULES_PER_MOLE_KELVIN, temperature);
                    content.gasConstant += fx::FxDivU128By64(product, (uint64_t)material.molarMassMgPerMol * 1000)
                                               .quotient;
                    content.fluidMassMg += mass;
                }
            }

            return content;
        }

        int64_t LiquidVolumeAt(const FluidContent& content, int64_t pressure) {
            const int64_t squeezed = fx::FxMulShiftS64((int64_t)content.stiffnessQ48,
                                                       pressure - ATMOSPHERE_MICRO_PASCAL, CAPACITY_SHIFT);
            return std::max<int64_t>((int64_t)content.liquidReferenceNl - squeezed, 0);
        }

        int64_t GasVolumeAt(const FluidContent& content, int64_t pressure) {
            if (content.gasConstant == 0)
                return 0;

            if (pressure <= 0)
                return SATURATED_VOLUME;

            const fx::FxU128 numerator = fx::FxMulU64Full(content.gasConstant, 1000000000);
            if (numerator.hi >= (uint64_t)pressure)
                return SATURATED_VOLUME;

            return (int64_t)std::min<uint64_t>(fx::FxDivU128By64(numerator, (uint64_t)pressure).quotient,
                                               (uint64_t)SATURATED_VOLUME);
        }

        // 流体の体積が空きを超える(= 圧力を上げる側)か
        bool Overfull(const FluidContent& content, int64_t pressure, int64_t availableNl) {
            return LiquidVolumeAt(content, pressure) + GasVolumeAt(content, pressure) >= availableNl;
        }

        // 流体が空きをちょうど満たす最大の圧力(2 分探索。決まった回数)
        int64_t SolveStatePressure(const FluidContent& content, int64_t availableNl) {
            if (!Overfull(content, 0, availableNl))
                return 0;

            if (Overfull(content, PRESSURE_MAX_MICRO_PASCAL, availableNl))
                return PRESSURE_MAX_MICRO_PASCAL;

            int64_t low = 0;
            int64_t high = PRESSURE_MAX_MICRO_PASCAL;
            while (high - low > 1) {
                const int64_t middle = low + ((high - low) / 2);
                if (Overfull(content, middle, availableNl))
                    low = middle;
                else
                    high = middle;
            }

            return low;
        }

        // --- セルの状態式・柱・面 ---
        uint64_t ClampCapacity(uint64_t value) {
            return std::min(value, COEFFICIENT_LIMIT);
        }

        // 縮みやすさ C = 液体の S + 気体の V_g ÷ p + 真空 ÷ 大気(Q48)
        uint64_t CapacityQ48(const FluidContent& content, const FlowCell& cell) {
            uint64_t capacity = content.stiffnessQ48;
            if (cell.gasNl > 0) {
                const fx::FxU128 numerator = fx::FxMulU64Full((uint64_t)cell.gasNl, (uint64_t)1 << CAPACITY_SHIFT);
                const uint64_t pressure = (uint64_t)std::max<int64_t>(cell.pressureMicroPascal, 1);
                capacity += numerator.hi >= pressure ? COEFFICIENT_LIMIT
                                                     : ClampCapacity(fx::FxDivU128By64(numerator, pressure).quotient);
            }

            if (cell.voidNl > 0) {
                const fx::FxU128 numerator = fx::FxMulU64Full((uint64_t)cell.voidNl, (uint64_t)1 << CAPACITY_SHIFT);
                capacity += numerator.hi >= VOID_PRESSURE_MICRO_PASCAL
                                ? COEFFICIENT_LIMIT
                                : ClampCapacity(fx::FxDivU128By64(numerator, VOID_PRESSURE_MICRO_PASCAL).quotient);
            }

            return std::max<uint64_t>(ClampCapacity(capacity), 1);
        }

        // 面の流体の質量の下限(K が上限を超えない。A² ÷ (m 刻み²) ≤ 2^10 nL/µPa)
        int64_t MinimumFaceMassMg(const Box& box, uint64_t areaMm2) {
            const uint64_t ticks = (uint64_t)box.config.ticksPerSecond * box.config.ticksPerSecond;
            return (int64_t)(fx::FxDivU128By64(fx::FxMulU64Full(areaMm2, areaMm2), ticks << 10).quotient + 1);
        }

        // K = A² ÷ (m 刻み²)(Q48)。A² × 2^16 ÷ m を先に作り(m の下限で K ≤ 2^10 なので 2^44 未満)、× 2^32 ÷ 刻み²
        uint64_t CoefficientQ48(const Box& box, uint64_t areaMm2, int64_t massMg) {
            const uint64_t ticks = (uint64_t)box.config.ticksPerSecond * box.config.ticksPerSecond;
            const fx::FxU128 squared = fx::FxMulU64Full(areaMm2 * areaMm2, (uint64_t)1 << 16);
            const uint64_t perMassQ16 = fx::FxDivU128By64(squared, (uint64_t)massMg).quotient;
            return ClampCapacity(fx::FxDivU128By64(fx::FxMulU64Full(perMassQ16, (uint64_t)1 << 32), ticks).quotient);
        }

        // 柱 p_col = 上の流体の重さ + 自分の重さの半分(閉じたセルで切る)
        std::vector<int64_t> BuildFluidColumns(const Box& box, const View& view, const std::vector<FlowCell>& cells) {
            const uint64_t areaMm2 = CellAreaMm2(box);
            const std::array<uint32_t, 3> size = {box.config.sizeX, box.config.sizeY, box.config.sizeZ};
            const uint32_t axis = view.gravityAxis;
            std::vector<int64_t> columns(cells.size(), 0);
            for (uint32_t cell = 0; cell < cells.size(); ++cell) {
                std::array<uint32_t, 3> at = CellCoordinates(box, cell);
                if (at[axis] != (view.gravityDown ? size[axis] - 1 : 0))
                    continue;

                int64_t above = 0;
                for (uint32_t step = 0; step < size[axis]; ++step) {
                    const uint32_t index = CellIndex(box, at[0], at[1], at[2]);
                    const FlowCell& flow = cells[index];
                    const int64_t weight = flow.fluidMassMg * view.gravityUmPerS2 / (int64_t)areaMm2;
                    above = flow.closed ? 0 : above;
                    columns[index] = flow.closed ? 0 : above + (weight / 2);
                    above += flow.closed ? 0 : weight;
                    at[axis] = view.gravityDown ? at[axis] - 1 : at[axis] + 1;
                }
            }

            return columns;
        }

        std::vector<FlowFace> BuildFaces(const Box& box, const std::vector<FlowCell>& cells, FlowStats& stats) {
            const uint64_t areaMm2 = CellAreaMm2(box);
            std::vector<FlowFace> faces;
            for (uint32_t cell = 0; cell < cells.size(); ++cell) {
                for (uint32_t axis = 0; axis < 3; ++axis) {
                    const std::optional<uint32_t> neighbor = Neighbor(box, cell, axis);
                    if (!neighbor.has_value())
                        continue;

                    FlowFace face = {.axis = axis, .left = cell, .right = *neighbor};
                    const uint32_t open = std::min(cells[cell].openQ16, cells[*neighbor].openQ16);
                    face.open = !cells[cell].closed && !cells[*neighbor].closed && open > 0;
                    face.areaMm2 = (areaMm2 * open) >> 16;
                    face.open = face.open && face.areaMm2 > 0;
                    if (face.open) {
                        const int64_t mass = (cells[cell].fluidMassMg + cells[*neighbor].fluidMassMg) / 2;
                        const int64_t minimum = MinimumFaceMassMg(box, face.areaMm2);
                        stats.lightFaces += mass < minimum ? 1u : 0u;
                        face.massMg = std::max(mass, minimum);
                        face.coefficientQ48 = CoefficientQ48(box, face.areaMm2, face.massMg);
                    }

                    faces.push_back(face);
                }
            }

            return faces;
        }

        int64_t& FluidMomentum(Box& box, const FlowFace& face) {
            return box.faceMomentum[face.axis][FaceIndex(box, face.left, MomentumSlot(box.config.layout, Phase::Gas))];
        }

        FaceRef FluidFace(const Box& box, uint32_t axis, uint32_t cell) {
            return {.axis = axis, .cell = cell, .slot = MomentumSlot(box.config.layout, Phase::Gas)};
        }

        // セルの流体の粘り μ(µPa·s)= 液体の物質は体積で・気体は質量の割合で平均し、液体と気体の体積で重みを付ける
        uint32_t CellViscosity(const Box& box, uint32_t cell, const FlowCell& flow) {
            uint64_t liquidSum = 0;
            uint64_t liquidVolume = 0;
            uint64_t gasSum = 0;
            uint64_t gasMass = 0;
            for (uint32_t index = 0; index < box.materials.size(); ++index) {
                const Material& material = box.materials[index];
                const int64_t mass = CellMass(box, cell, index);
                if (mass <= 0)
                    continue;

                if (material.phase == Phase::Liquid) {
                    const uint64_t volume = MaterialVolumeNl(mass, material.densityKgPerM3);
                    liquidSum += (volume >> 16) * material.viscosityMicroPascalSec;
                    liquidVolume += volume >> 16;
                } else if (material.phase == Phase::Gas) {
                    gasSum += ((uint64_t)mass >> 8) * material.viscosityMicroPascalSec;
                    gasMass += (uint64_t)mass >> 8;
                }
            }

            const uint64_t liquid = liquidVolume == 0 ? 0 : liquidSum / liquidVolume;
            const uint64_t gas = gasMass == 0 ? 0 : gasSum / gasMass;
            const uint64_t liquidPart = (uint64_t)std::max<int64_t>(flow.liquidNl, 0) >> 16;
            const uint64_t gasPart = (uint64_t)std::max<int64_t>(flow.gasNl, 0) >> 16;
            if (liquidPart + gasPart == 0)
                return (uint32_t)std::max(liquid, gas);

            return (uint32_t)(((liquid * liquidPart) + (gas * gasPart)) / (liquidPart + gasPart));
        }

        // --- 段の間で渡すもの ---
        struct FlowRuntime {
            FlowOptions options;
            std::shared_ptr<FlowStats> stats;
            FlowScratch scratch;
        };

        void ApplyViscosity(FlowRuntime& runtime, Box& box);

        // --- 陰の圧力の段 ---
        // 横の面に柱の差の力積を足し(重さの軸の面は重さと柱が打ち消すので何も足さない)、陰の圧力で残り y を解いて力積を足す
        void ApplyColumnImpulses(Box& box, const View& view, FlowScratch& scratch) {
            for (const FlowFace& face : scratch.faces) {
                if (!face.open || face.axis == view.gravityAxis)
                    continue;

                const int64_t delta = scratch.columnMicroPascal[face.left] - scratch.columnMicroPascal[face.right];
                ApplyImpulse(box, FluidFace(box, face.axis, face.left), PressureImpulse(box, delta, face.areaMm2),
                             ImpulseSource::Pressure);
            }
        }

        PressureSystem BuildPressureSystem(Box& box, const FlowScratch& scratch, std::vector<int64_t>& values) {
            PressureSystem system = {.sizeX = box.config.sizeX, .sizeY = box.config.sizeY, .sizeZ = box.config.sizeZ};
            const auto count = (uint32_t)scratch.cells.size();
            system.capacities.resize(count);
            system.sources.resize(count);
            values.resize(count);
            for (uint32_t cell = 0; cell < count; ++cell) {
                const FlowCell& flow = scratch.cells[cell];
                const int64_t start = flow.closed ? 0 : flow.pressureMicroPascal - scratch.columnMicroPascal[cell];
                // 閉じたセルは系から切れている(面が無い)。容量は最小にして、多重格子の粗い段で隣の親を薄めないように
                system.capacities[cell] = flow.closed ? 1 : flow.capacityQ48;
                system.sources[cell] = flow.closed
                                           ? 0
                                           : fx::FxMulShiftS64(start, (int64_t)flow.capacityQ48, CAPACITY_SHIFT);
                values[cell] = start;
            }

            for (const FlowFace& face : scratch.faces) {
                if (!face.open)
                    continue;

                const int64_t predicted = FlowNl(box, FluidMomentum(box, face), face.areaMm2, face.massMg);
                system.sources[face.left] -= predicted;
                system.sources[face.right] += predicted;
                system.links.push_back({.left = face.left, .right = face.right, .coefficient = face.coefficientQ48});
            }

            return system;
        }

        void PressureStage(FlowRuntime& runtime, Box& box, const StepContext& context) {
            FlowScratch& scratch = runtime.scratch;
            FlowStats& stats = *runtime.stats;
            const uint32_t count = CellCount(box);
            scratch.cells.resize(count);
            scratch.heatCapacityQ16.resize(count);
            scratch.viscosityMicroPascalSec.resize(count);
            for (uint32_t cell = 0; cell < count; ++cell) {
                scratch.cells[cell] = MeasureFlowCell(box, context.view, cell, runtime.options);
                scratch.heatCapacityQ16[cell] = CellHeatCapacityQ16(box, cell);
                scratch.viscosityMicroPascalSec[cell] = CellViscosity(box, cell, scratch.cells[cell]);
                stats.voidCells += scratch.cells[cell].voidNl > 0 ? 1u : 0u;
            }

            scratch.columnMicroPascal = BuildFluidColumns(box, context.view, scratch.cells);
            scratch.faces = BuildFaces(box, scratch.cells, stats);
            ApplyColumnImpulses(box, context.view, scratch);
            // 粘りは陰の圧力の前に(面の運動量を受け渡してから、その流れで圧力の式を組む。運ぶ段の流れと食い違わない)
            ApplyViscosity(runtime, box);

            std::vector<int64_t> values;
            const PressureSystem system = BuildPressureSystem(box, scratch, values);
            const PressureCost cost = context.pressureSolver(system, values);
            stats.lastCycles = cost.cycles;
            stats.maxCycles = std::max(stats.maxCycles, cost.cycles);
            stats.lastErrorNl = cost.worstErrorMg;
            stats.maxErrorNl = std::max(stats.maxErrorNl, cost.worstErrorMg);

            // 解いた y の力積と、解いた後の体積の流れ
            for (FlowFace& face : scratch.faces) {
                if (!face.open)
                    continue;

                const int64_t impulse = PressureImpulse(box, values[face.left] - values[face.right], face.areaMm2);
                ApplyImpulse(box, FluidFace(box, face.axis, face.left), impulse, ImpulseSource::Pressure);
                face.flowNl = FlowNl(box, FluidMomentum(box, face), face.areaMm2, face.massMg);
            }

            for (uint32_t cell = 0; cell < count; ++cell)
                box.pressureMicroPascal[cell] = scratch.cells[cell].closed
                                                    ? 0
                                                    : values[cell] + scratch.columnMicroPascal[cell];

            ++stats.ticks;
        }

        // --- 運ぶ段 ---
        struct Take {
            int64_t liquidNl = 0;
            int64_t gasNl = 0;
        };

        // 面が風上のセルから取る体積(重さの軸は、上へ出るなら気体から・下へ出るなら液体から。横は体積の割合)
        Take FaceTake(const FlowFace& face, const FlowCell& upwind, const View& view) {
            const int64_t volume = (int64_t)fx::FxAbsU64(face.flowNl);
            Take take;
            if (face.axis == view.gravityAxis) {
                const bool leavingUp = (face.flowNl > 0) == view.gravityDown;
                if (leavingUp) {
                    take.gasNl = std::min(volume, upwind.gasNl);
                    take.liquidNl = volume - take.gasNl;
                } else {
                    take.liquidNl = std::min(volume, upwind.liquidNl);
                    take.gasNl = volume - take.liquidNl;
                }

                return take;
            }

            const int64_t fluid = upwind.liquidNl + upwind.gasNl;
            if (fluid <= 0)
                return take;

            take.liquidNl = fx::FxMulDivS64(volume, (uint64_t)upwind.liquidNl, (uint64_t)fluid);
            take.gasNl = volume - take.liquidNl;

            return take;
        }

        // セルから出る体積が中の流体を超えたら、その相の出る量を同じ割合で縮める
        void LimitTakes(const FlowScratch& scratch, std::vector<Take>& takes, FlowStats& stats) {
            std::vector<Take> demand(scratch.cells.size());
            for (size_t index = 0; index < scratch.faces.size(); ++index) {
                const FlowFace& face = scratch.faces[index];
                const uint32_t upwind = face.flowNl > 0 ? face.left : face.right;
                demand[upwind].liquidNl += takes[index].liquidNl;
                demand[upwind].gasNl += takes[index].gasNl;
            }

            for (size_t index = 0; index < scratch.faces.size(); ++index) {
                const FlowFace& face = scratch.faces[index];
                const uint32_t upwind = face.flowNl > 0 ? face.left : face.right;
                const FlowCell& cell = scratch.cells[upwind];
                Take& take = takes[index];
                if (demand[upwind].liquidNl > cell.liquidNl)
                    take.liquidNl = fx::FxMulDivS64(take.liquidNl, (uint64_t)cell.liquidNl,
                                                    (uint64_t)demand[upwind].liquidNl);

                if (demand[upwind].gasNl > cell.gasNl)
                    take.gasNl = fx::FxMulDivS64(take.gasNl, (uint64_t)cell.gasNl, (uint64_t)demand[upwind].gasNl);
            }

            for (size_t cell = 0; cell < demand.size(); ++cell) {
                stats.limitedCells += demand[cell].liquidNl > scratch.cells[cell].liquidNl ? 1u : 0u;
                stats.limitedCells += demand[cell].gasNl > scratch.cells[cell].gasNl ? 1u : 0u;
            }
        }

        // 1 つの面の中身を移す量を、刻みの初めのセルの中身から決める(物質ごとに相の体積の割合。エネルギーは熱容量の割合)。
        // 移す順に結果がよらないように、読むのは写し(start)だけ。移した質量を返す
        struct StartContent {
            std::vector<int64_t> massMg;
            std::vector<int64_t> energyMillijoules;
        };

        int64_t MoveTake(Box& box, const StartContent& start, const FlowScratch& scratch, uint32_t from, uint32_t to,
                         const Take& take) {
            const FlowCell& cell = scratch.cells[from];
            const auto materials = (uint32_t)box.materials.size();
            int64_t moved = 0;
            uint64_t heatCapacity = 0;
            for (uint32_t material = 0; material < materials; ++material) {
                const Phase phase = box.materials[material].phase;
                const bool liquid = phase == Phase::Liquid;
                const int64_t share = liquid ? take.liquidNl : (phase == Phase::Gas ? take.gasNl : 0);
                const int64_t phaseVolume = liquid ? cell.liquidNl : cell.gasNl;
                if (share <= 0 || phaseVolume <= 0)
                    continue;

                const int64_t startMass = start.massMg[((size_t)from * materials) + material];
                const int64_t mass = fx::FxMulDivS64(startMass, (uint64_t)std::min(share, phaseVolume),
                                                     (uint64_t)phaseVolume);
                if (mass <= 0)
                    continue;

                heatCapacity += HeatCapacityQ16(box, material, mass);
                moved += mass;
                MoveMass(box, from, to, material, mass);
            }

            const uint64_t total = scratch.heatCapacityQ16[from];
            if (heatCapacity > 0 && total > 0) {
                const int64_t energy = std::max<int64_t>(start.energyMillijoules[from], 0);
                MoveEnergy(box, from, to, fx::FxMulDivS64(energy, std::min(heatCapacity, total), total));
            }

            return moved;
        }

        void TransportStage(FlowRuntime& runtime, Box& box, const StepContext& context) {
            FlowScratch& scratch = runtime.scratch;
            std::vector<Take> takes(scratch.faces.size());
            for (size_t index = 0; index < scratch.faces.size(); ++index) {
                const FlowFace& face = scratch.faces[index];
                if (!face.open || face.flowNl == 0)
                    continue;

                takes[index] = FaceTake(face, scratch.cells[face.flowNl > 0 ? face.left : face.right], context.view);
            }

            LimitTakes(scratch, takes, *runtime.stats);

            const StartContent start = {.massMg = box.massMg, .energyMillijoules = box.energyMillijoules};
            for (size_t index = 0; index < scratch.faces.size(); ++index) {
                FlowFace& face = scratch.faces[index];
                face.movedMassMg = 0;
                if (!face.open || face.flowNl == 0)
                    continue;

                const bool forward = face.flowNl > 0;
                const uint32_t from = forward ? face.left : face.right;
                const uint32_t to = forward ? face.right : face.left;
                const int64_t moved = MoveTake(box, start, scratch, from, to, takes[index]);
                face.movedMassMg = forward ? moved : -moved;
            }
        }

        // --- 当てる段: 面の運動量を運ぶ(MAC の格子の運び)---
        // 面 F =(軸 a・セル c)の受け持ちの箱は c と c + e_a の間。その箱の + e_b の側を通る質量の流れ
        //   b = a: (F の移した質量 + 面 (a, c + e_a) の移した質量)÷ 2(セル c + e_a の中心)
        //   b ≠ a: (面 (b, c) + 面 (b, c + e_a) の移した質量)÷ 2
        // で、風上の面の速さの運動量を隣の面 G =(a, c + e_b)へ移す。1 つの面から出る質量の流れの和が面の質量を超えたら同じ割合で縮める
        struct MomentumTransfer {
            uint32_t from = 0;  // 面の番号(scratch.faces)
            uint32_t to = 0;
            uint64_t massMg = 0;  // 運ぶ質量(縮める前)
        };

        using FaceTable = std::array<std::vector<int32_t>, 3>;

        FaceTable IndexFaces(const FlowScratch& scratch, uint32_t count) {
            FaceTable table;
            for (auto& faces : table)
                faces.assign(count, -1);

            for (size_t index = 0; index < scratch.faces.size(); ++index) {
                const FlowFace& face = scratch.faces[index];
                if (face.open)
                    table[face.axis][face.left] = (int32_t)index;
            }

            return table;
        }

        void CollectTransfers(const Box& box, const FlowScratch& scratch, const FaceTable& table, uint32_t index,
                              std::vector<MomentumTransfer>& transfers) {
            const FlowFace& face = scratch.faces[index];
            for (uint32_t side = 0; side < 3; ++side) {
                const std::optional<uint32_t> beside = Neighbor(box, face.left, side);
                if (!beside.has_value() || table[face.axis][*beside] < 0)
                    continue;

                const int32_t first = table[side][face.left];
                const int32_t second = table[side][face.right];
                if (first < 0 || second < 0)
                    continue;

                const int64_t flux = (scratch.faces[(size_t)first].movedMassMg +
                                      scratch.faces[(size_t)second].movedMassMg) /
                                     2;
                if (flux == 0)
                    continue;

                const auto other = (uint32_t)table[face.axis][*beside];
                transfers.push_back(
                    {.from = flux > 0 ? index : other, .to = flux > 0 ? other : index, .massMg = fx::FxAbsU64(flux)});
            }
        }

        void ApplyStage(FlowRuntime& runtime, Box& box, const StepContext&) {
            const FlowScratch& scratch = runtime.scratch;
            const FaceTable table = IndexFaces(scratch, CellCount(box));
            std::vector<MomentumTransfer> transfers;
            for (uint32_t index = 0; index < scratch.faces.size(); ++index) {
                if (scratch.faces[index].open)
                    CollectTransfers(box, scratch, table, index, transfers);
            }

            std::vector<uint64_t> demand(scratch.faces.size(), 0);
            for (const MomentumTransfer& transfer : transfers)
                demand[transfer.from] += transfer.massMg;

            // 運ぶ量は刻みの初めの面の運動量から決める(移す順に結果がよらない)
            const std::array<std::vector<int64_t>, 3> start = box.faceMomentum;
            for (const MomentumTransfer& transfer : transfers) {
                const FlowFace& from = scratch.faces[transfer.from];
                const FlowFace& to = scratch.faces[transfer.to];
                const uint64_t faceMass = (uint64_t)from.massMg;
                const uint64_t mass = demand[transfer.from] > faceMass
                                          ? (uint64_t)fx::FxMulDivS64((int64_t)transfer.massMg, faceMass,
                                                                      demand[transfer.from])
                                          : transfer.massMg;
                const int64_t momentum = start[from.axis][FaceIndex(box, from.left, 0)];
                MoveMomentum(box, FluidFace(box, from.axis, from.left), FluidFace(box, to.axis, to.left),
                             fx::FxMulDivS64(momentum, mass, faceMass));
            }
        }

        // --- 粘り(T-0927・T-0926 の仮説 2。陰の圧力の段の中で、系を組む前に)---
        // 面 F の速さ v = M ÷ m(m は面の流体の質量)。隣の面 G(同じ軸・セルを e_b ずらす)との間で
        //   物の粘り: ΔM = μ A Δt (v_G − v_F) ÷ Δ = μ × 辺 × Δv ÷ 刻み/秒(µPa·s × mm × 2^-20 m/s ÷ 1000 で mg·2^-20 m/s)
        //   渦の粘り: ΔM = m C_s² Δt |Δv| Δv ÷ Δ(m は 2 つの面の小さい方の質量)
        // 1 つの組で渡すのは、2 つの面の速さをそろえる量の 1/12 まで(1 つの面は隣が 6 つ。順によらず行き過ぎない)
        int64_t ViscousTransfer(const Box& box, const FlowOptions& options, uint32_t viscosity, const FlowFace& first,
                                const FlowFace& second, int64_t firstMomentum, int64_t secondMomentum) {
            const int64_t firstVelocity = firstMomentum / first.massMg;
            const int64_t secondVelocity = secondMomentum / second.massMg;
            const int64_t difference = secondVelocity - firstVelocity;
            if (difference == 0)
                return 0;

            const uint64_t magnitude = fx::FxAbsU64(difference);
            const uint64_t edgeMm = CellEdgeUm(box.config.level) / 1000;
            const uint64_t ticks = box.config.ticksPerSecond;
            const auto lighter = (uint64_t)std::min(first.massMg, second.massMg);
            uint64_t amount = 0;
            if (options.materialViscosity)
                amount += fx::FxDivU128By64(fx::FxMulU64Full(magnitude, (uint64_t)viscosity * edgeMm), 1000 * ticks)
                              .quotient;

            if (options.eddyCoefficientPer10000 != 0) {
                // m × C_s² × |Δv|² × 10^6 ÷ (2^20 × 辺 µm × 刻み/秒 × 10^4)
                const fx::FxU128 product = fx::FxMulU64Full(lighter * options.eddyCoefficientPer10000,
                                                            std::min<uint64_t>(magnitude, (uint64_t)1 << 30) *
                                                                std::min<uint64_t>(magnitude, (uint64_t)1 << 30));
                const uint64_t denominator = ((uint64_t)CellEdgeUm(box.config.level) << 20) * ticks / 100;
                amount += product.hi >= denominator ? UINT64_MAX / 2 : fx::FxDivU128By64(product, denominator).quotient;
            }

            const auto limit = (uint64_t)fx::FxMulDivS64((int64_t)magnitude, lighter, 12);
            const auto moved = (int64_t)std::min(amount, limit);

            return difference > 0 ? moved : -moved;
        }

        void ApplyViscosity(FlowRuntime& runtime, Box& box) {
            const FlowOptions& options = runtime.options;
            if (!options.materialViscosity && options.eddyCoefficientPer10000 == 0)
                return;

            const FlowScratch& scratch = runtime.scratch;
            const FaceTable table = IndexFaces(scratch, CellCount(box));
            // 運ぶ量は段の初めの面の運動量から決める(受け渡す順に結果がよらない)
            const std::array<std::vector<int64_t>, 3> start = box.faceMomentum;
            for (uint32_t index = 0; index < scratch.faces.size(); ++index) {
                const FlowFace& face = scratch.faces[index];
                if (!face.open)
                    continue;

                for (uint32_t side = 0; side < 3; ++side) {
                    const std::optional<uint32_t> beside = Neighbor(box, face.left, side);
                    if (!beside.has_value() || table[face.axis][*beside] < 0)
                        continue;

                    const FlowFace& other = scratch.faces[(size_t)table[face.axis][*beside]];
                    const uint32_t viscosity = std::min(
                        {scratch.viscosityMicroPascalSec[face.left], scratch.viscosityMicroPascalSec[face.right],
                         scratch.viscosityMicroPascalSec[other.left], scratch.viscosityMicroPascalSec[other.right]});
                    const int64_t moved = ViscousTransfer(box, options, viscosity, face, other,
                                                          start[face.axis][FaceIndex(box, face.left, 0)],
                                                          start[other.axis][FaceIndex(box, other.left, 0)]);
                    if (moved == 0)
                        continue;

                    MoveMomentum(box, FluidFace(box, other.axis, other.left), FluidFace(box, face.axis, face.left),
                                 moved);
                    ++runtime.stats->viscousPairs;
                }
            }
        }

    }  // namespace

    FlowCell MeasureFlowCell(const Box& box, const View& view, uint32_t cell, const FlowOptions& options) {
        FlowCell result;
        const FluidContent content = MeasureContent(box, cell, options);
        result.fluidMassMg = content.fluidMassMg;
        result.openQ16 = OpenQ16(view.cells[cell]);
        result.availableNl = (int64_t)((CellVolumeNl(box) * result.openQ16) >> 16);
        result.closed = result.availableNl == 0;
        if (result.closed)
            return result;

        result.pressureMicroPascal = SolveStatePressure(content, result.availableNl);
        result.liquidNl = std::min(LiquidVolumeAt(content, result.pressureMicroPascal), result.availableNl);
        const int64_t rest = result.availableNl - result.liquidNl;
        result.gasNl = content.gasConstant != 0 ? rest : 0;
        result.voidNl = content.gasConstant != 0 ? 0 : rest;
        result.capacityQ48 = CapacityQ48(content, result);

        return result;
    }

    FlowStages MakeFlowStages(const FlowOptions& options) {
        auto runtime = std::make_shared<FlowRuntime>();
        runtime->options = options;
        runtime->stats = std::make_shared<FlowStats>();
        FlowStages result;
        result.stats = runtime->stats;
        result.stages.pressure = [runtime](Box& box, const StepContext& context) {
            assert(box.config.layout == MomentumLayout::GrainSeparate);
            PressureStage(*runtime, box, context);
        };
        result.stages.transport = [runtime](Box& box, const StepContext& context) {
            TransportStage(*runtime, box, context);
        };
        result.stages.apply = [runtime](Box& box, const StepContext& context) {
            ApplyStage(*runtime, box, context);
        };

        return result;
    }

}  // namespace bicameral::sim::case_b
