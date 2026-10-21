// case_b_flow.cpp — 案 B の試作の研究 R1(T-0868): 気体と液体を 1 つの陰の圧力で動かす段。式と単位は case_b_flow.h。
// 段の間で渡すもの(状態式の結果・面の流れ)は FlowScratch に置き、pressure → transport → apply の順に使う(同じ刻みの中だけ)。
// どの面・セルも刻みの初めの値から決め、移す量はすべて受け渡しの関数(MoveMass・MoveEnergy・MoveMomentum・ApplyImpulse)で当てる(04 R2・R3)。
#include "sim/case_b/case_b_flow.h"

#include <algorithm>
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

            // --- 引きずり(R2。drag の時だけ)---
            bool coupled = false;          // 引きずりを圧力と一緒に解く面(粒か孔がある)
            bool locked = false;           // 両側が詰まった粒(粒は動かさず、引きずりは孔と同じに。R3 まで・仮)
            uint64_t grainAreaMm2 = 0;     // 粒の面積(風上のセルの粒そのものの割合 × 面積)
            int64_t grainNl = 0;           // 面の粒そのものの体積(両側の平均)
            int64_t grainInertiaQ16 = 0;   // b = 粒の質量 + K(mg の Q16。下限つき)
            int64_t fluidInertiaMg = 0;    // a' = 流体の質量 + K_x + K 粒の質量 ÷ b
            uint64_t couplingAreaMm2 = 0;  // c' = 流体の面積 + 粒の面積 × K ÷ b
            std::array<uint64_t, 2> grainSideAreaMm2{};  // 左右のセルの粒の面積(粒そのものの体積 ÷ 辺。風上で取る)
            int64_t grainMassMg = 0;                     // 面の粒の質量(両側の平均)
            uint64_t dragQ16 = 0;          // 流体 ↔ 粒の引きずり K(1 刻みぶん。mg の Q16。1 mg の粒で 0 に丸まらない)
            uint64_t porousMg = 0;         // 流体 ↔ 据わる孔の引きずり K_x(同上)
            int64_t predictedNl = 0;       // 圧力の式の予測の流れ(引きずりを消去した形)
            int64_t grainFlowNl = 0;       // 解いた後の粒の質量の流れ(mg の Q32。持ち越しと一緒に運ぶ)
            int64_t grainMovedMassMg = 0;  // 運ぶ段が移した粒の質量

            // --- 置き場 ①(T-0963)。④ は carrierSlot = 流体の置き場・carrierAreaMm2 = areaMm2 のまま ---
            uint32_t carrierSlot = 0;     // 圧力の式の主の流体の置き場(① は面の体積の大きい相)
            uint64_t carrierAreaMm2 = 0;  // 主の流体の面積
            int64_t gasMassMg = 0;        // 面の気体・液体の質量(両側の平均の切り上げ。① だけ)
            int64_t liquidMassMg = 0;
            bool twoFluid = false;        // 気体と液体を別の速さで解く面(主でない相が動く向きの風上にその相がある)
            uint32_t otherSlot = 0;       // 主でない流体の置き場(しずく・泡・水面の別の相)
            int64_t otherMassMg = 0;      // 主でない流体の質量
            int64_t otherNl = 0;          // 主でない流体の体積(両側の平均)
            int64_t carrierNl = 0;        // 主の流体の体積(両側の平均)
            uint64_t otherPorousMg = 0;   // 孔の引きずりの主でない流体の分(体積の割合で分ける。仮)
            uint64_t otherAreaMm2 = 0;    // 主でない流体の面積(風上のセルの体積 ÷ 辺)
            uint64_t otherDragQ16 = 0;    // 気体 ↔ 液体の引きずり K(1 刻みぶん。mg の Q16)
            int64_t otherInertiaQ16 = 0;  // b_o = 主でない流体の質量 + K
            int64_t movedGasMg = 0;       // 運ぶ段が移した気体・液体の質量(① の運動量の運び)
            int64_t movedLiquidMg = 0;
        };

        // セルの粒と孔(R2)
        struct GrainCell {
            int64_t massMg = 0;
            uint32_t grainUm = 0;          // 粒の大きさ(質量の平均)
            uint32_t permeabilityUm2 = 0;  // 据わる固体の孔の通しやすさ(質量の平均。0 = 孔の引きずり無し)
            bool packed = false;           // 粒が詰まっている(grainPackedQ16 がほぼ 1)
            bool noRoom = false;           // 粒が入れない(詰まった・据わる固体か物がある。穴の大きさ S8 は 2 周目。仮)
            uint64_t porousMg = 0;         // 孔の引きずり K_x(1 刻みぶん)
        };

        struct FlowScratch {
            std::vector<FlowCell> cells;
            std::vector<uint64_t> heatCapacityQ16;  // セルの全部の物質の熱容量(mJ/K の Q16)
            std::vector<int64_t> columnMicroPascal;
            std::vector<FlowFace> faces;
            std::vector<uint32_t> viscosityMicroPascalSec;  // セルの流体の粘り(体積の割合の平均)

            std::vector<GrainCell> grains;  // 引きずり(R2。drag の時だけ)

            // --- 置き場 ①(T-0963)---
            std::vector<uint8_t> surfaceCells;  // 層に分かれたセル(気体と液体があり、下が液体か閉じている = 水面)
            std::vector<int64_t>
                gasColumnMicroPascal;  // 相ごとの柱(水面のセルは気体が上・液体が下の層。ほかは混ざった柱)
            std::vector<int64_t> liquidColumnMicroPascal;
            std::vector<uint32_t> gasViscosity;  // セルの相ごとの粘り(µPa·s。質量の平均。無ければ 0)
            std::vector<uint32_t> liquidViscosity;
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
                        face.carrierAreaMm2 = face.areaMm2;
                    }

                    faces.push_back(face);
                }
            }

            return faces;
        }

        // 主の流体の置き場(④ は流体の置き場。① は PrepareFluidPhases が面ごとに決める)
        int64_t& FluidMomentum(Box& box, const FlowFace& face) {
            return box.faceMomentum[face.axis][FaceIndex(box, face.left, face.carrierSlot)];
        }

        FaceRef FluidFace(const Box& box, uint32_t axis, uint32_t cell) {
            return {.axis = axis, .cell = cell, .slot = MomentumSlot(box.config.layout, Phase::Gas)};
        }

        FaceRef SlotFace(const FlowFace& face, uint32_t slot) {
            return {.axis = face.axis, .cell = face.left, .slot = slot};
        }

        FaceRef CarrierFace(const FlowFace& face) {
            return SlotFace(face, face.carrierSlot);
        }

        int64_t& SlotMomentum(Box& box, const FlowFace& face, uint32_t slot) {
            return box.faceMomentum[face.axis][FaceIndex(box, face.left, slot)];
        }

        bool PerPhase(const Box& box) {
            return box.config.layout == MomentumLayout::PerPhase;
        }

        FaceRef GrainFace(const Box& box, uint32_t axis, uint32_t cell) {
            return {.axis = axis, .cell = cell, .slot = MomentumSlot(box.config.layout, Phase::Grain)};
        }

        int64_t& GrainMomentum(Box& box, const FlowFace& face) {
            return box
                .faceMomentum[face.axis][FaceIndex(box, face.left, MomentumSlot(box.config.layout, Phase::Grain))];
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

        // --- 引きずり(R2・T-0869。drag の時だけ)---
        // 係数 K(mg)= β(kg/(m³ s)の Q8)× 体積(nL)÷(10^6 × 刻み/秒 × 2^8)。面の K は両側のセルの K の平均(界面で半分ずつ)
        constexpr uint32_t BETA_SHIFT = 8;
        constexpr uint64_t DRAG_LIMIT_Q16 = (uint64_t)1 << 62;  // 引きずりの頭打ち(約 7×10^13 mg。数える)
        constexpr uint64_t KOZENY_CARMAN = 180;                 // 詰まった粒の通しやすさの定数(Kozeny–Carman)
        constexpr uint64_t STOKES = 18;                         // 1 粒の粘りの抗力(C_d = 24/Re の項)
        constexpr uint64_t MIDDLE_DRAG = 4;                     // C_d = 4/√Re の項(仮。中間の Re をつなぐ)

        // K(mg の Q16)= β(Q8)× 体積(nL)× 2^8 ÷(10^6 × 刻み/秒)
        uint64_t DragFromBeta(const Box& box, uint64_t betaQ8, int64_t volumeNl) {
            if (betaQ8 == 0 || volumeNl <= 0)
                return 0;

            const uint64_t denominator = (uint64_t)box.config.ticksPerSecond * 1000000;
            const fx::FxU128 product = fx::FxMulU64Full(betaQ8 << (16 - BETA_SHIFT), (uint64_t)volumeNl);
            if (product.hi >= denominator)
                return DRAG_LIMIT_Q16;

            return std::min(fx::FxDivU128By64(product, denominator).quotient, DRAG_LIMIT_Q16);
        }

        // μ ÷ d²(µPa·s ÷ µm² → kg/(m³ s)。× 10^6)の Q8
        uint64_t ViscousPerSquareQ8(uint32_t viscosity, uint64_t lengthSquaredUm2) {
            if (lengthSquaredUm2 == 0)
                return 0;

            return fx::FxDivU128By64(fx::FxMulU64Full((uint64_t)viscosity * 1000000, (uint64_t)1 << BETA_SHIFT),
                                     lengthSquaredUm2)
                .quotient;
        }

        // a × b ÷ d を上限 cap で(引きずりの係数。頭打ちは数える側が見る)
        uint64_t MulDivCap(uint64_t a, uint64_t b, uint64_t d, uint64_t cap) {
            const fx::FxU128 product = fx::FxMulU64Full(a, b);
            if (product.hi >= d)
                return cap;

            return std::min(fx::FxDivU128By64(product, d).quotient, cap);
        }

        // 1 粒の抗力(粒の体積あたり。C_d = 24/Re + 4/√Re + C₂ の形)を |Δv|(2^-20 m/s)・流体の密度 ρ(g/m³)で:
        //   18 μ/d² + 3 √(ρ μ |Δv| / d³) + ¾ C₂ ρ |Δv| / d
        // 中の項は 24/Re と C₂ の間をつなぐ(Ferguson & Church 2004 の終端の速さと砂・粉で数 % 以内。T-0869 の表)
        uint64_t DispersedBetaQ8(uint32_t viscosity, uint32_t grainUm, uint64_t fluidDensityGPerM3, uint64_t slip,
                                 uint32_t shapePermille) {
            const uint64_t diameter = grainUm;
            // 速さの差は 128 m/s で頭打ち(積が 64bit に収まる。それより速い粒は衝撃の区の側)
            const uint64_t speed = std::min<uint64_t>(slip, (uint64_t)1 << 27);
            uint64_t beta = STOKES * ViscousPerSquareQ8(viscosity, diameter * diameter);

            // 中の項: ρ_g μ |Δv| × 10^9 ÷ (2^20 d³) の Q16(= ÷ 2^4)の平方根 = Q8
            const uint64_t densityViscosity = fluidDensityGPerM3 * viscosity;
            const uint64_t scaled = fx::FxShiftRightU128(fx::FxMulU64Full(densityViscosity, speed), 4);
            const uint64_t middle = MulDivCap(scaled, 1000000000ull, diameter * diameter * diameter, (uint64_t)1 << 62);
            beta += MIDDLE_DRAG * 3 / 4 * fx::FxSqrtU64(middle);

            // Newton の項: 750 C₂‰ ρ_g |Δv| ÷ (1000 × 2^20 d) の Q8
            beta += MulDivCap(750ull * shapePermille * fluidDensityGPerM3, speed, (1000ull * diameter) << 12,
                              (uint64_t)1 << 62);

            return beta;
        }

        // 詰まった粒(Kozeny–Carman。粒の体積あたり)= 180 μ φ_s ÷ (d² φ_f)
        uint64_t PackedBetaQ8(uint32_t viscosity, uint32_t grainUm, uint32_t solidQ16, uint32_t openQ16) {
            if (openQ16 == 0)
                return 0;

            const uint64_t base = KOZENY_CARMAN * ViscousPerSquareQ8(viscosity, (uint64_t)grainUm * grainUm);
            return fx::FxMulDivS64((int64_t)base, solidQ16, openQ16);
        }

        // 据わる固体の孔(Darcy。全体の体積あたり)= μ φ_f² ÷ k
        uint64_t PorousBetaQ8(uint32_t viscosity, uint32_t permeabilityUm2, uint32_t openQ16) {
            if (permeabilityUm2 == 0)
                return 0;

            const uint64_t base = ViscousPerSquareQ8(viscosity, permeabilityUm2);
            return fx::FxMulDivS64(fx::FxMulDivS64((int64_t)base, openQ16, Q16_ONE), openQ16, Q16_ONE);
        }

        // 面の流体の密度(g/m³ = mg/nL × 10^9)
        uint64_t FaceFluidDensity(const FlowCell& left, const FlowCell& right) {
            const int64_t mass = left.fluidMassMg + right.fluidMassMg;
            const int64_t volume = left.liquidNl + left.gasNl + right.liquidNl + right.gasNl;
            if (mass <= 0 || volume <= 0)
                return 0;

            return (uint64_t)fx::FxMulDivS64(mass, 1000000000, (uint64_t)volume);
        }

        // 面の運動量 M の 1 刻みの体積の流れ(nL)を大きい質量でも溢れない形で(M A 10^6 ÷ (m 刻み/秒) ÷ 2^20)
        int64_t CoupledFlowNl(const Box& box, int64_t momentum, uint64_t areaMm2, int64_t massMg) {
            if (massMg <= 0 || areaMm2 == 0)
                return 0;

            const int64_t scaled = fx::FxMulDivS64(momentum, areaMm2 * 1000000,
                                                   (uint64_t)massMg * box.config.ticksPerSecond);
            return scaled / ((int64_t)1 << 20);
        }

        // 重さの加速 a(µm/s²)が質量 m に 1 刻みに与える力積(mg·2^-20 m/s)
        int64_t GravityImpulse(const Box& box, uint32_t axis, int64_t massMg) {
            return fx::FxMulDivS64(box.config.accelerationUmPerS2[axis], (uint64_t)massMg << 20,
                                   (uint64_t)box.config.ticksPerSecond * 1000000);
        }

        // --- 段の間で渡すもの ---
        struct FlowRuntime {
            FlowOptions options;
            std::shared_ptr<FlowStats> stats;
            FlowScratch scratch;
        };

        void ApplyViscosity(FlowRuntime& runtime, Box& box);
        void StopEmptyUpwind(FlowRuntime& runtime, Box& box, const FlowFace& face);

        // --- 引きずりを圧力と一緒に解く(R2・T-0869)---
        constexpr uint32_t PACKED_MARGIN_Q16 = 1u << 10;  // 詰まりの 98.4 % で詰まったとみなす(仮)

        void MeasureGrains(FlowRuntime& runtime, const Box& box, const View& view) {
            FlowScratch& scratch = runtime.scratch;
            const uint32_t count = CellCount(box);
            const auto cellNl = (int64_t)CellVolumeNl(box);
            scratch.grains.assign(count, {});
            for (uint32_t cell = 0; cell < count; ++cell) {
                GrainCell& grain = scratch.grains[cell];
                uint64_t grainSum = 0;
                uint64_t permeabilitySum = 0;
                uint64_t fixedMass = 0;
                for (uint32_t index = 0; index < box.materials.size(); ++index) {
                    const Material& material = box.materials[index];
                    const int64_t mass = std::max<int64_t>(CellMass(box, cell, index), 0);
                    if (material.phase == Phase::Grain) {
                        grain.massMg += mass;
                        grainSum += (uint64_t)mass * material.grainSizeUm;
                    } else if (material.phase == Phase::Fixed) {
                        permeabilitySum += (uint64_t)mass * material.permeabilityUm2;
                        fixedMass += (uint64_t)mass;
                    }
                }

                const CellView& cellView = view.cells[cell];
                grain.grainUm = grain.massMg == 0 ? 0 : (uint32_t)(grainSum / (uint64_t)grain.massMg);
                grain.permeabilityUm2 = fixedMass == 0 ? 0 : (uint32_t)(permeabilitySum / fixedMass);
                grain.packed = cellView.grainPackedQ16 + PACKED_MARGIN_Q16 >= Q16_ONE;
                grain.noRoom = grain.packed || cellView.fixedQ16 > 0 || cellView.bodyQ16 > 0;
                const uint64_t beta = PorousBetaQ8(scratch.viscosityMicroPascalSec[cell], grain.permeabilityUm2,
                                                   scratch.cells[cell].openQ16);
                grain.porousMg = DragFromBeta(box, beta, cellNl) >> 16;
            }
        }

        // 粒の重さと浮力を陽に当てる。流体の重さは R1 のまま(重さの軸は柱と打ち消して 0・横は柱の差)。
        // 浮力 = 押しのけた流体の重さで、流体 → 粒の受け渡し(和 0)。粒の重さの浮力を引いた残りは引きずりで流体へ渡り、陰の圧力が支える
        void ApplyGrainWeight(Box& box, const View& view, const FlowScratch& scratch, const FlowFace& face) {
            if (face.grainMassMg <= 0)
                return;

            const FaceRef grain = GrainFace(box, face.axis, face.left);
            ApplyImpulse(box, grain, GravityImpulse(box, face.axis, face.grainMassMg), ImpulseSource::Gravity);
            const uint64_t density = FaceFluidDensity(scratch.cells[face.left], scratch.cells[face.right]);
            // 押しのけた流体の質量(mg の Q20)= 体積(nL)× 密度(g/m³)÷ 10^9
            const int64_t displacedQ20 = fx::FxMulDivS64(face.grainNl, density << 20, 1000000000);
            const int64_t buoyancy = -fx::FxMulDivS64(box.config.accelerationUmPerS2[face.axis], (uint64_t)displacedQ20,
                                                      (uint64_t)box.config.ticksPerSecond * 1000000);
            MoveMomentum(box, CarrierFace(face), grain, buoyancy);
            if (face.axis == view.gravityAxis)
                return;

            const int64_t column = scratch.columnMicroPascal[face.left] - scratch.columnMicroPascal[face.right];
            ApplyImpulse(box, grain, PressureImpulse(box, column, face.grainAreaMm2), ImpulseSource::Pressure);
        }

        // 粒の面積は風上のセルの粒そのものの割合で(運ぶ量と圧力の式を合わせる。平均だと先端で半分の速さになった。T-0869)
        uint64_t UpwindGrainArea(const FlowFace& face, int64_t momentum) {
            return face.grainSideAreaMm2[momentum >= 0 ? 0 : 1];
        }

        // 粒が動く面か(粒があり、詰まり・据わる物で止めていない)。粒の面積は 1 mm² 未満で 0 になることがある
        bool GrainMoves(const FlowFace& face) {
            return face.grainMassMg > 0 && !face.locked;
        }

        // 粒の 1 刻みの質量の流れ(mg の Q32)= 風上の粒の質量 × v Δt ÷ Δx(面積を通さない。1 mg の粒でも丸めで膨らまない。T-0869)
        // 相の 1 刻みの質量の流れ(mg の Q32)= 風上の質量 × v Δt ÷ Δx(v = 面の運動量 ÷ 面の質量)
        int64_t PhaseFlowQ32(const Box& box, int64_t faceMassMg, int64_t momentum, int64_t upwindMassMg) {
            if (faceMassMg <= 0 || upwindMassMg <= 0)
                return 0;

            // v Δt ÷ Δx の Q32 = M × 10^6 × 2^12 ÷ (m × 刻み/秒 × 辺 µm)
            const uint64_t denominator = (uint64_t)faceMassMg * box.config.ticksPerSecond *
                                         CellEdgeUm(box.config.level);
            const int64_t fractionQ32 = fx::FxMulDivS64(momentum, 1000000ull << 12, denominator);
            const int64_t bounded = std::clamp<int64_t>(fractionQ32, -((int64_t)1 << 32), (int64_t)1 << 32);

            return bounded * upwindMassMg;
        }

        int64_t GrainFlowQ32(const Box& box, const FlowFace& face, int64_t momentum, int64_t upwindGrainMg) {
            return PhaseFlowQ32(box, face.grainMassMg, momentum, upwindGrainMg);
        }

        void StopGrain(Box& box, const FlowFace& face) {
            const int64_t momentum = GrainMomentum(box, face);
            if (momentum != 0)
                ApplyImpulse(box, GrainFace(box, face.axis, face.left), -momentum, ImpulseSource::Wall);
        }

        // セルの詰まった粒の引きずり K(mg の Q16)= 180 μ φ_s ÷ (d² φ_f) × 粒そのものの体積
        uint64_t PackedCellDragQ16(const Box& box, const FlowScratch& scratch, uint32_t cell, uint32_t grainUm) {
            const FlowCell& flow = scratch.cells[cell];
            if (flow.grainNl <= 0 || flow.closed)
                return 0;

            const auto solidQ16 = (uint32_t)(((uint64_t)flow.grainNl << 16) / CellVolumeNl(box));
            const uint64_t beta = PackedBetaQ8(scratch.viscosityMicroPascalSec[cell], grainUm, solidQ16, flow.openQ16);

            return DragFromBeta(box, beta, flow.grainNl);
        }

        // 面の流体 ↔ 粒の引きずり K(刻みの初めの速さの差で。Picard の 1 回)
        uint64_t FaceDragMg(const Box& box, const FlowOptions& options, const FlowScratch& scratch,
                            const FlowFace& face, int64_t fluidMomentum, int64_t grainMomentum) {
            const GrainCell& left = scratch.grains[face.left];
            const GrainCell& right = scratch.grains[face.right];
            const uint32_t grainUm = std::max(left.grainUm, right.grainUm);
            if (grainUm == 0)
                return 0;

            const int64_t slip = (grainMomentum / face.grainMassMg) - (fluidMomentum / face.massMg);
            const uint32_t viscosity = (scratch.viscosityMicroPascalSec[face.left] +
                                        scratch.viscosityMicroPascalSec[face.right]) /
                                       2;
            const uint64_t density = FaceFluidDensity(scratch.cells[face.left], scratch.cells[face.right]);
            const uint64_t dispersed = DispersedBetaQ8(viscosity, grainUm, density, fx::FxAbsU64(slip),
                                                       options.grainShapeDragPermille);
            // 詰まった粒(Kozeny–Carman)はセルごとに K を作ってから面で平均する(φ_s² の形なので、面で割合を平均してから掛けると
            // 栓の端の面が 1/4 になり、栓が半セル短く見えた。T-0963 の砂の芯で 14 % 速かった)
            const uint64_t packed = (PackedCellDragQ16(box, scratch, face.left, grainUm) +
                                     PackedCellDragQ16(box, scratch, face.right, grainUm)) /
                                    2;

            return std::max(DragFromBeta(box, dispersed, face.grainNl), packed);
        }

        // 1 つの面の引きずりの係数と、粒を消去した形(a'・b・c')を作る。粒が動けない面は粒を据わる孔として扱う
        void PrepareCoupledFace(FlowRuntime& runtime, Box& box, const View& view, FlowFace& face) {
            FlowScratch& scratch = runtime.scratch;
            FlowStats& stats = *runtime.stats;
            const GrainCell& left = scratch.grains[face.left];
            const GrainCell& right = scratch.grains[face.right];
            // 切り上げ(1 mg の粒が面で 0 にならない。相が消える所)
            face.grainMassMg = (left.massMg + right.massMg + 1) / 2;
            // 体積は切り上げた質量にそろえる(重さ・浮力・引きずりが同じ質量を見る。終端の速さが丸めで変わらない)
            const int64_t grainSum = left.massMg + right.massMg;
            const int64_t volumeSum = scratch.cells[face.left].grainNl + scratch.cells[face.right].grainNl;
            face.grainNl = grainSum <= 0 ? 0
                                         : fx::FxMulDivS64(volumeSum, (uint64_t)face.grainMassMg, (uint64_t)grainSum);
            if (!face.open) {
                StopGrain(box, face);
                return;
            }

            const int64_t grainMomentum = GrainMomentum(box, face);
            if (face.grainMassMg <= 0 && grainMomentum != 0) {
                // 相が消える所: 粒の居ない面の運動量は流体へ
                MoveMomentum(box, GrainFace(box, face.axis, face.left), CarrierFace(face), grainMomentum);
                ++stats.vanishedGrainFaces;
            }

            face.porousMg = (left.porousMg + right.porousMg) / 2;
            face.coupled = face.twoFluid || face.porousMg > 0 || face.grainMassMg > 0;
            if (!face.coupled)
                return;

            // 粒の面積 = 粒そのものの体積 ÷ 辺(Q16 の割合では 0.001 % の粉が 0 に丸まった。T-0869)
            const uint64_t edgeNlPerMm2 = (uint64_t)CellEdgeUm(box.config.level);
            face.grainSideAreaMm2 = {(uint64_t)scratch.cells[face.left].grainNl / edgeNlPerMm2,
                                     (uint64_t)scratch.cells[face.right].grainNl / edgeNlPerMm2};
            const uint64_t meanArea = (face.grainSideAreaMm2[0] + face.grainSideAreaMm2[1]) / 2;
            face.grainAreaMm2 = face.grainMassMg > 0 ? meanArea : 0;
            const int64_t fluidStart = FluidMomentum(box, face);
            const int64_t grainStart = GrainMomentum(box, face);
            ApplyGrainWeight(box, view, scratch, face);
            if (face.grainMassMg > 0)
                face.grainAreaMm2 = UpwindGrainArea(face, GrainMomentum(box, face));

            if (face.grainMassMg <= 0) {
                stats.porousFaces += face.porousMg > 0 ? 1u : 0u;
                return;
            }

            uint64_t drag = FaceDragMg(box, runtime.options, scratch, face, fluidStart, grainStart);
            stats.cappedDragFaces += drag >= DRAG_LIMIT_Q16 ? 1u : 0u;
            const int64_t grainNow = GrainMomentum(box, face);
            const bool intoLeft = grainNow < 0;
            const bool locked = (left.packed && right.packed) || (intoLeft ? left.noRoom : right.noRoom);
            if (locked) {
                // 粒は動けない(押し合いは R3)。流体から見た粒は据わる孔
                StopGrain(box, face);
                face.porousMg += drag >> 16;
                face.locked = true;
                face.grainAreaMm2 = 0;
                drag = 0;
                ++stats.blockedGrainFaces;
            }

            face.dragQ16 = drag;
            ++stats.dragFaces;
        }

        // 粒を消去した形: b = m_s + K・a' = m_f + K_x + K m_s ÷ b・c' = A_f + A_s K ÷ b(K・b は mg の Q16)
        void EliminateGrain(const Box& box, FlowFace& face) {
            const auto drag = (int64_t)face.dragQ16;
            const bool moving = GrainMoves(face);
            const int64_t minimum = MinimumFaceMassMg(box, face.grainAreaMm2) << 16;
            face.grainInertiaQ16 = moving ? std::max((face.grainMassMg << 16) + drag, minimum) : 0;
            // ①: 孔の引きずりを相の体積の割合で主と主でない流体へ分ける(仮)
            face.otherPorousMg = face.twoFluid ? (uint64_t)fx::FxMulDivS64(
                                                     (int64_t)face.porousMg, (uint64_t)face.otherNl,
                                                     (uint64_t)std::max<int64_t>(face.otherNl + face.carrierNl, 1))
                                               : 0;
            int64_t fluid = face.massMg + (int64_t)(face.porousMg - face.otherPorousMg);
            uint64_t area = face.carrierAreaMm2;
            if (moving) {
                fluid += fx::FxMulDivS64(face.grainMassMg, (uint64_t)drag, (uint64_t)face.grainInertiaQ16);
                area += (uint64_t)fx::FxMulDivS64((int64_t)face.grainAreaMm2, (uint64_t)drag,
                                                  (uint64_t)face.grainInertiaQ16);
            }

            // 置き場 ①: 主でない流体も粒と同じ形で消去する(どちらも主の流体とだけつながる星の形なので、足し合わせられる)
            if (face.twoFluid) {
                const auto otherDrag = (int64_t)face.otherDragQ16;
                const int64_t otherMinimum = MinimumFaceMassMg(box, face.otherAreaMm2) << 16;
                const int64_t otherHeld = face.otherMassMg + (int64_t)face.otherPorousMg;
                face.otherInertiaQ16 = std::max((otherHeld << 16) + otherDrag, otherMinimum);
                fluid += fx::FxMulDivS64(otherHeld, (uint64_t)otherDrag, (uint64_t)face.otherInertiaQ16);
                area += (uint64_t)fx::FxMulDivS64((int64_t)face.otherAreaMm2, (uint64_t)otherDrag,
                                                  (uint64_t)face.otherInertiaQ16);
            }

            face.couplingAreaMm2 = area;
            face.fluidInertiaMg = std::max(fluid, MinimumFaceMassMg(box, area));
        }

        // 粒の慣性(mg。係数と予測の流れの式に。1 mg 未満は 1)
        int64_t GrainInertiaMg(const FlowFace& face) {
            return std::max<int64_t>(face.grainInertiaQ16 >> 16, 1);
        }

        // 圧力の式の係数と予測の流れ(c'² ÷ a' + A_s² ÷ b と c' (M_f + r M_s) ÷ a' + A_s M_s ÷ b)
        void CoupledSystemTerms(Box& box, FlowFace& face) {
            EliminateGrain(box, face);
            const int64_t fluidMomentum = FluidMomentum(box, face);
            int64_t combined = fluidMomentum;
            face.coefficientQ48 = CoefficientQ48(box, face.couplingAreaMm2, face.fluidInertiaMg);
            face.predictedNl = 0;
            if (GrainMoves(face)) {
                const int64_t grainMomentum = GrainMomentum(box, face);
                combined += fx::FxMulDivS64(grainMomentum, face.dragQ16, (uint64_t)face.grainInertiaQ16);
                if (face.grainAreaMm2 > 0) {
                    face.coefficientQ48 += CoefficientQ48(box, face.grainAreaMm2, GrainInertiaMg(face));
                    face.predictedNl += CoupledFlowNl(box, grainMomentum, face.grainAreaMm2, GrainInertiaMg(face));
                }
            }

            if (face.twoFluid) {
                const int64_t otherMomentum = SlotMomentum(box, face, face.otherSlot);
                const int64_t inertia = std::max<int64_t>(face.otherInertiaQ16 >> 16, 1);
                combined += fx::FxMulDivS64(otherMomentum, face.otherDragQ16, (uint64_t)face.otherInertiaQ16);
                if (face.otherAreaMm2 > 0) {
                    face.coefficientQ48 += CoefficientQ48(box, face.otherAreaMm2, inertia);
                    face.predictedNl += CoupledFlowNl(box, otherMomentum, face.otherAreaMm2, inertia);
                }
            }

            face.coefficientQ48 = ClampCapacity(face.coefficientQ48);
            face.predictedNl += CoupledFlowNl(box, combined, face.couplingAreaMm2, face.fluidInertiaMg);
        }

        // 解いた y から、圧力の力積 → 相どうしの受け渡し → 孔の引きずり(残り)を当てる。足し合わせて保存
        void SolveCoupledFace(FlowRuntime& runtime, Box& box, FlowFace& face, int64_t deltaMicroPascal) {
            const int64_t fluidImpulse = PressureImpulse(box, deltaMicroPascal, face.carrierAreaMm2);
            const int64_t grainImpulse = PressureImpulse(box, deltaMicroPascal, face.grainAreaMm2);
            const int64_t fluidBefore = FluidMomentum(box, face) + fluidImpulse;
            const int64_t grainBefore = GrainMomentum(box, face) + grainImpulse;
            int64_t combined = fluidBefore;
            if (GrainMoves(face))
                combined += fx::FxMulDivS64(grainBefore, face.dragQ16, (uint64_t)face.grainInertiaQ16);

            const int64_t otherImpulse = face.twoFluid ? PressureImpulse(box, deltaMicroPascal, face.otherAreaMm2) : 0;
            const int64_t otherBefore = face.twoFluid ? SlotMomentum(box, face, face.otherSlot) + otherImpulse : 0;
            if (face.twoFluid)
                combined += fx::FxMulDivS64(otherBefore, face.otherDragQ16, (uint64_t)face.otherInertiaQ16);

            const int64_t fluidAfter = fx::FxMulDivS64(combined, (uint64_t)face.massMg, (uint64_t)face.fluidInertiaMg);
            int64_t transfer = 0;
            if (GrainMoves(face)) {
                // m_s v_s = m_s (M_s + I_s + K v_f) ÷ b
                // m_s v_s = M_s m_s ÷ b + v_f K m_s ÷ b(K m_s ÷ b ≤ m_s なので 64bit に収まる)
                const auto grainQ16 = (uint64_t)face.grainMassMg << 16;
                const auto pulledQ16 = (uint64_t)fx::FxMulDivS64((int64_t)face.dragQ16, (uint64_t)face.grainMassMg,
                                                                 (uint64_t)face.grainInertiaQ16);
                const int64_t grainAfter = fx::FxMulDivS64(grainBefore, grainQ16, (uint64_t)face.grainInertiaQ16) +
                                           fx::FxMulDivS64(fluidAfter, pulledQ16, (uint64_t)face.massMg << 16);
                transfer = grainAfter - grainBefore;
                ApplyImpulse(box, GrainFace(box, face.axis, face.left), grainImpulse, ImpulseSource::Pressure);
                MoveMomentum(box, CarrierFace(face), GrainFace(box, face.axis, face.left), transfer);
            }

            // 主でない流体(①): m_o v_o = M_o m_o ÷ b_o + v_c K m_o ÷ b_o(粒と同じ形)
            int64_t otherTransfer = 0;
            if (face.twoFluid) {
                const auto otherQ16 = (uint64_t)face.otherMassMg << 16;
                const auto pulledQ16 = (uint64_t)fx::FxMulDivS64((int64_t)face.otherDragQ16, (uint64_t)face.otherMassMg,
                                                                 (uint64_t)face.otherInertiaQ16);
                const int64_t otherAfter = fx::FxMulDivS64(otherBefore, otherQ16, (uint64_t)face.otherInertiaQ16) +
                                           fx::FxMulDivS64(fluidAfter, pulledQ16, (uint64_t)face.massMg << 16);
                // 主でない流体の孔の引きずり K_xo v_o は外へ(帳簿 FixedPhase)。残りが主の流体との受け渡し
                const int64_t otherPorous = face.otherPorousMg == 0
                                                ? 0
                                                : fx::FxMulDivS64(otherAfter, face.otherPorousMg,
                                                                  (uint64_t)std::max<int64_t>(face.otherMassMg, 1));
                otherTransfer = otherAfter - otherBefore + otherPorous;
                ApplyImpulse(box, SlotFace(face, face.otherSlot), otherImpulse, ImpulseSource::Pressure);
                MoveMomentum(box, CarrierFace(face), SlotFace(face, face.otherSlot), otherTransfer);
                ApplyImpulse(box, SlotFace(face, face.otherSlot), -otherPorous, ImpulseSource::FixedPhase);
            }

            ApplyImpulse(box, CarrierFace(face), fluidImpulse, ImpulseSource::Pressure);
            const int64_t porous = fluidBefore - transfer - otherTransfer - fluidAfter;
            ApplyImpulse(box, CarrierFace(face), -porous, ImpulseSource::FixedPhase);
            face.flowNl = CoupledFlowNl(box, FluidMomentum(box, face), face.carrierAreaMm2, face.massMg);
            if (face.twoFluid)
                StopEmptyUpwind(runtime, box, face);

            face.grainFlowNl = 0;
            if (!GrainMoves(face))
                return;

            const int64_t grainMomentum = GrainMomentum(box, face);
            const uint32_t upwind = grainMomentum >= 0 ? face.left : face.right;
            face.grainFlowNl = GrainFlowQ32(box, face, grainMomentum, runtime.scratch.grains[upwind].massMg);
            const GrainCell& target = runtime.scratch.grains[face.grainFlowNl > 0 ? face.right : face.left];
            if (target.noRoom && face.grainFlowNl != 0) {
                StopGrain(box, face);
                face.grainFlowNl = 0;
                ++runtime.stats->blockedGrainFaces;
            }
        }

        // --- 置き場 ①(T-0963): 気体と液体を別の速さで、相どうしの引きずりと一緒に解く ---
        // 面ごとに体積の大きい相を主の流体(carrier)、もう一方を主でない流体(other)にし、粒と同じ形で消去して陰の圧力の式に入れる。
        // 水面のセル(気体と液体が層に分かれたセル)は相ごとの柱で支え、気体 ↔ 液体は境の剪断で弱くつなぐ(21 §2.4)。
        // 混ざったセル(しずく・泡)は浮力(重さ − 押しのけた混ざりの重さ)を相どうしで受け渡し、しずく・泡の抗力でつなぐ。

        // 相ごとのセルの粘り(µPa·s。質量の平均。相が無ければ 0)
        void MeasurePhaseViscosity(const Box& box, uint32_t cell, FlowScratch& scratch) {
            std::array<uint64_t, 2> sum{};
            std::array<uint64_t, 2> mass{};
            for (uint32_t index = 0; index < box.materials.size(); ++index) {
                const Material& material = box.materials[index];
                const int64_t amount = CellMass(box, cell, index);
                if (amount <= 0 || (material.phase != Phase::Gas && material.phase != Phase::Liquid))
                    continue;

                const size_t side = material.phase == Phase::Liquid ? 1 : 0;
                sum[side] += ((uint64_t)amount >> 8) * material.viscosityMicroPascalSec;
                mass[side] += (uint64_t)amount >> 8;
            }

            scratch.gasViscosity[cell] = mass[0] == 0 ? 0 : (uint32_t)(sum[0] / mass[0]);
            scratch.liquidViscosity[cell] = mass[1] == 0 ? 0 : (uint32_t)(sum[1] / mass[1]);
        }

        bool HasGas(const FlowCell& cell) {
            return !cell.closed && cell.gasNl > 0;
        }

        bool HasLiquid(const FlowCell& cell) {
            return !cell.closed && cell.liquidNl > 0;
        }

        // 重さの向きの下のセル(無ければ std::nullopt = 床の壁)
        std::optional<uint32_t> BelowCell(const Box& box, const View& view, uint32_t cell) {
            if (!view.gravityDown)
                return Neighbor(box, cell, view.gravityAxis);

            std::array<uint32_t, 3> at = CellCoordinates(box, cell);
            if (at[view.gravityAxis] == 0)
                return std::nullopt;

            --at[view.gravityAxis];

            return CellIndex(box, at[0], at[1], at[2]);
        }

        // 水面のセル = 気体と液体があり、下が壁・閉じたセル・気体の無いセル(液体が下に溜まり、気体が上に乗る層)。
        // 相ごとの柱(全部のセルで、層に分かれているとして): 気体 = 上の重さ + 自分の気体の半分、
        // 液体 = 上の重さ + 自分の気体 + 自分の液体の半分。気体だけ・液体だけのセルは混ざった柱と同じ値になる。層の横の面だけが使う
        void MeasureSurfaceCells(const Box& box, const View& view, FlowScratch& scratch) {
            const auto count = (uint32_t)scratch.cells.size();
            const auto areaMm2 = (int64_t)CellAreaMm2(box);
            scratch.surfaceCells.assign(count, 0);
            scratch.gasColumnMicroPascal = scratch.columnMicroPascal;
            scratch.liquidColumnMicroPascal = scratch.columnMicroPascal;
            for (uint32_t cell = 0; cell < count; ++cell) {
                const FlowCell& flow = scratch.cells[cell];
                if (flow.closed)
                    continue;

                const int64_t weight = flow.fluidMassMg * view.gravityUmPerS2 / areaMm2;
                const int64_t above = scratch.columnMicroPascal[cell] - (weight / 2);
                const int64_t gasWeight = view.cells[cell].phaseMassMg[0] * view.gravityUmPerS2 / areaMm2;
                const int64_t liquidWeight = view.cells[cell].phaseMassMg[1] * view.gravityUmPerS2 / areaMm2;
                scratch.gasColumnMicroPascal[cell] = above + (gasWeight / 2);
                scratch.liquidColumnMicroPascal[cell] = above + gasWeight + (liquidWeight / 2);
                if (!HasGas(flow) || !HasLiquid(flow))
                    continue;

                const std::optional<uint32_t> below = BelowCell(box, view, cell);
                const bool supported = !below.has_value() || scratch.cells[*below].closed ||
                                       scratch.cells[*below].gasNl == 0;
                scratch.surfaceCells[cell] = supported ? 1 : 0;
            }
        }

        // 面の相の質量(両側の平均の切り上げ。1 mg の相が面で 0 にならない。相が無ければ 0)
        int64_t PhaseFaceMass(int64_t left, int64_t right) {
            return (std::max<int64_t>(left, 0) + std::max<int64_t>(right, 0) + 1) / 2;
        }

        // 層の面(相ごとに柱が支え、引きずりは境の剪断): 重さの軸では、下のセルが「気体が無いか水面」・上のセルが「液体が無いか水面」で、
        // 気体の上に液体が乗っていない。横の面は両側のどちらかが水面
        bool LayeredFace(const FlowScratch& scratch, const View& view, const FlowFace& face) {
            const bool leftSurface = scratch.surfaceCells[face.left] != 0;
            const bool rightSurface = scratch.surfaceCells[face.right] != 0;
            if (face.axis != view.gravityAxis)
                return leftSurface || rightSurface;

            const uint32_t lower = view.gravityDown ? face.left : face.right;
            const uint32_t upper = view.gravityDown ? face.right : face.left;
            const bool lowerGas = HasGas(scratch.cells[lower]);
            const bool upperLiquid = HasLiquid(scratch.cells[upper]);
            const bool lowerHolds = !lowerGas || scratch.surfaceCells[lower] != 0;
            const bool upperRests = !upperLiquid || scratch.surfaceCells[upper] != 0;

            return lowerHolds && upperRests && !(lowerGas && upperLiquid);
        }

        // 主の流体の粘り(面の両側の平均。片側に相が無ければある側)
        uint32_t PhaseFaceViscosity(const FlowScratch& scratch, const FlowFace& face, bool liquid) {
            const std::vector<uint32_t>& values = liquid ? scratch.liquidViscosity : scratch.gasViscosity;
            const uint32_t left = values[face.left];
            const uint32_t right = values[face.right];
            if (left == 0 || right == 0)
                return std::max(left, right);

            return (left + right) / 2;
        }

        // 相の密度(g/m³)= 質量 ÷ 体積 × 10^9(上限 10^7 g/m³)
        uint64_t PhaseDensity(int64_t massMg, int64_t volumeNl) {
            if (massMg <= 0 || volumeNl <= 0)
                return 0;

            return std::min<uint64_t>((uint64_t)fx::FxMulDivS64(massMg, 1000000000, (uint64_t)volumeNl), 10000000);
        }

        // 気体 ↔ 液体の引きずり K(1 刻みぶん。mg の Q16)
        //   層の面: 境の剪断 τ = ρ_g C_f |Δv| Δv を境の面積で。K = ρ_g C_f |Δv| A Δt(C_f は 21 §2.6 の面の剪断と同じ 0.003。仮)
        //   混ざった面: しずく・泡の抗力(粒と同じ式。大きさ drop_size_um・主の流体の粘りと密度・形の定数は滑らかな球)× 主でない流体の体積
        uint64_t FluidPairDragQ16(const Box& box, const FlowOptions& options, const FlowScratch& scratch,
                                  const FlowFace& face, bool layered, bool liquidCarrier, uint64_t slip,
                                  bool gravityFace) {
            const uint64_t speed = std::min<uint64_t>(slip, (uint64_t)1 << 27);
            if (layered) {
                const int64_t gasNl = liquidCarrier ? face.otherNl : face.carrierNl;
                const uint64_t density = PhaseDensity(face.gasMassMg, gasNl);
                // 境の面積: 横の面は水面のセルの半分ずつ、重さの軸の面はセルの面積
                const uint64_t cellArea = CellAreaMm2(box);
                const uint64_t sides = (uint64_t)scratch.surfaceCells[face.left] + scratch.surfaceCells[face.right];
                const uint64_t interfaceMm2 = gravityFace || sides == 0 ? cellArea : cellArea * sides / 2;
                // K(Q16)= ρ_g(g/m³)× C_f(÷ 10^4)× |Δv|(2^-20 m/s)× A(mm²)÷(10^7 × 2^4 × 刻み/秒)
                return MulDivCap(density * options.surfaceShearPer10000 * speed, interfaceMm2,
                                 160000000ull * box.config.ticksPerSecond, DRAG_LIMIT_Q16);
            }

            const uint64_t density = PhaseDensity(liquidCarrier ? face.liquidMassMg : face.gasMassMg, face.carrierNl);
            const uint32_t viscosity = PhaseFaceViscosity(scratch, face, liquidCarrier);
            const uint32_t dropUm = std::max<uint32_t>(box.environment.dropSizeUm, 1);
            const uint64_t beta = DispersedBetaQ8(viscosity, dropUm, density, speed, options.dropShapeDragPermille);

            return DragFromBeta(box, beta, face.otherNl);
        }

        // 主と主でない流体の面積 = それぞれの速さの向きの風上のセルの相の体積 ÷ 辺(運ぶ量と圧力の式を合わせる。粒と同じ)
        void SplitFluidArea(const Box& box, const FlowScratch& scratch, FlowFace& face, bool liquidCarrier,
                            int64_t carrierMomentum, int64_t otherMomentum) {
            const uint64_t edge = CellEdgeUm(box.config.level);
            const auto phaseNl = [&](uint32_t cell, bool liquid) {
                const FlowCell& flow = scratch.cells[cell];
                return (uint64_t)std::max<int64_t>(liquid ? flow.liquidNl : flow.gasNl, 0);
            };
            uint64_t carrier = std::min(phaseNl(carrierMomentum >= 0 ? face.left : face.right, liquidCarrier) / edge,
                                        face.areaMm2);
            uint64_t other = std::min(phaseNl(otherMomentum >= 0 ? face.left : face.right, !liquidCarrier) / edge,
                                      face.areaMm2);
            if (carrier + other > face.areaMm2) {
                carrier = carrier * face.areaMm2 / (carrier + other);
                other = face.areaMm2 - carrier;
            }

            face.carrierAreaMm2 = carrier;
            face.otherAreaMm2 = other;
        }

        // 陽に足す力: 横の面は相ごとの柱の差を相の面積に。重さの軸の層の面は何も足さない(相ごとに柱が支える)。
        // 重さの軸の混ざった面は浮力の受け渡し: 液体 = 重さ − 押しのけた混ざりの重さ(体積の割合)、気体はその逆(和 0。柱は混ざりの重さを支える)
        void ApplyFluidForces(Box& box, const View& view, const FlowScratch& scratch, const FlowFace& face,
                              bool layered, bool liquidCarrier, int64_t liquidNl, int64_t gasNl) {
            const FaceRef gas = SlotFace(face, MomentumSlot(box.config.layout, Phase::Gas));
            const FaceRef liquid = SlotFace(face, MomentumSlot(box.config.layout, Phase::Liquid));
            if (face.axis != view.gravityAxis && !layered) {
                // 混ざった横の面: 混ざった柱の差を面積に掛け、相の質量の割合で分ける(柱は重さの形の力。気体が液体の柱の坂で暴れない。
                // 相の分かれは重さの軸の浮力と引きずりだけから。T-0868 の仮への答え)
                const int64_t delta = scratch.columnMicroPascal[face.left] - scratch.columnMicroPascal[face.right];
                const int64_t impulse = PressureImpulse(box, delta, face.areaMm2);
                const int64_t liquidShare = fx::FxMulDivS64(impulse, (uint64_t)face.liquidMassMg,
                                                            (uint64_t)(face.liquidMassMg + face.gasMassMg));
                ApplyImpulse(box, liquid, liquidShare, ImpulseSource::Pressure);
                ApplyImpulse(box, gas, impulse - liquidShare, ImpulseSource::Pressure);
                return;
            }

            if (face.axis != view.gravityAxis) {
                const uint64_t liquidArea = liquidCarrier ? face.carrierAreaMm2 : face.otherAreaMm2;
                const uint64_t gasArea = liquidCarrier ? face.otherAreaMm2 : face.carrierAreaMm2;
                const int64_t liquidDelta = scratch.liquidColumnMicroPascal[face.left] -
                                            scratch.liquidColumnMicroPascal[face.right];
                const int64_t gasDelta = scratch.gasColumnMicroPascal[face.left] -
                                         scratch.gasColumnMicroPascal[face.right];
                ApplyImpulse(box, liquid, PressureImpulse(box, liquidDelta, liquidArea), ImpulseSource::Pressure);
                ApplyImpulse(box, gas, PressureImpulse(box, gasDelta, gasArea), ImpulseSource::Pressure);
                return;
            }

            // 層の面(水面の上下)は相ごとに柱が支えるので何も足さない(足すと水の重さの半分が気体へ渡って暴れた。T-0963 の 2 回目)
            if (layered)
                return;

            const int64_t liquidWeight = GravityImpulse(box, face.axis, face.liquidMassMg);
            const int64_t mixtureWeight = GravityImpulse(box, face.axis, face.liquidMassMg + face.gasMassMg);
            const int64_t displaced = fx::FxMulDivS64(mixtureWeight, (uint64_t)std::max<int64_t>(liquidNl, 0),
                                                      (uint64_t)std::max<int64_t>(liquidNl + gasNl, 1));
            MoveMomentum(box, gas, liquid, liquidWeight - displaced);
        }

        // 1 つの速さの面(④ と同じ形。1 つの相だけか、主の流体が片側に無い面): 運動量を主の置き場にまとめ、横の面は混ざった柱の差
        void PrepareSinglePhaseFace(Box& box, const View& view, const FlowScratch& scratch, FlowStats& stats,
                                    const FlowFace& face) {
            // 主でない置き場の運動量を主へまとめる(解いた後に SplitMergedFace が質量の割合で分け直す)
            const int64_t rest = SlotMomentum(box, face, face.otherSlot);
            if (rest != 0)
                MoveMomentum(box, SlotFace(face, face.otherSlot), CarrierFace(face), rest);

            stats.mergedFluidFaces += face.otherMassMg > 0 ? 1u : 0u;

            if (face.axis == view.gravityAxis)
                return;

            const int64_t delta = scratch.columnMicroPascal[face.left] - scratch.columnMicroPascal[face.right];
            ApplyImpulse(box, CarrierFace(face), PressureImpulse(box, delta, face.areaMm2), ImpulseSource::Pressure);
        }

        // 1 つの速さで解いた面の運動量を、相の質量の割合で置き場へ分ける(同じ速さ。和は変わらない)
        void SplitMergedFace(Box& box, const FlowFace& face) {
            const int64_t total = face.gasMassMg + face.liquidMassMg;
            if (!face.open || face.twoFluid || face.otherMassMg <= 0 || total <= 0)
                return;

            const int64_t share = fx::FxMulDivS64(SlotMomentum(box, face, face.carrierSlot), (uint64_t)face.otherMassMg,
                                                  (uint64_t)total);
            MoveMomentum(box, CarrierFace(face), SlotFace(face, face.otherSlot), share);
        }

        // 主でない流体が、その相の無いセルから流れ出る向きなら止める(帳簿 Wall。粒が詰まった所へ入れないのと同じ)
        void StopEmptyUpwind(FlowRuntime& runtime, Box& box, const FlowFace& face) {
            const int64_t momentum = SlotMomentum(box, face, face.otherSlot);
            if (momentum == 0)
                return;

            const FlowCell& upwind = runtime.scratch.cells[momentum > 0 ? face.left : face.right];
            const bool otherLiquid = face.otherSlot == MomentumSlot(box.config.layout, Phase::Liquid);
            if (otherLiquid ? HasLiquid(upwind) : HasGas(upwind))
                return;

            ApplyImpulse(box, SlotFace(face, face.otherSlot), -momentum, ImpulseSource::Wall);
            ++runtime.stats->stoppedFluidFaces;
        }

        void PrepareFluidPhases(FlowRuntime& runtime, Box& box, const View& view, FlowFace& face) {
            if (!face.open)
                return;

            const FlowScratch& scratch = runtime.scratch;
            FlowStats& stats = *runtime.stats;
            const FlowCell& left = scratch.cells[face.left];
            const FlowCell& right = scratch.cells[face.right];
            face.gasMassMg = PhaseFaceMass(view.cells[face.left].phaseMassMg[0], view.cells[face.right].phaseMassMg[0]);
            face.liquidMassMg = PhaseFaceMass(view.cells[face.left].phaseMassMg[1],
                                              view.cells[face.right].phaseMassMg[1]);
            const int64_t gasNl = (left.gasNl + right.gasNl) / 2;
            const int64_t liquidNl = (left.liquidNl + right.liquidNl) / 2;
            // 主の流体: 液体が面の流体の 1/9 以上なら液体(泡のある水・水面)、それより少なければ気体(しずくの雨・霧)。
            // 半々の面(ダムの先端)をしずくの弱い引きずりで扱うと、気体が水の間を素通りする(T-0963 の仮)
            bool liquidCarrier = face.gasMassMg <= 0 || (face.liquidMassMg > 0 && liquidNl * 8 >= gasNl);
            // 主の流体は両側のセルにあること(無い側から主が流れると、圧力の式が運べない体積を動かす。T-0963 の 1 回目:
            // 水 → 空気の面で液体を主にして、静かな池が 0.5 m で暴れた)。両側に無ければもう一方、どちらも無ければ 1 つの速さ(④ の形)
            const bool gasBoth = HasGas(left) && HasGas(right);
            const bool liquidBoth = HasLiquid(left) && HasLiquid(right);
            if (liquidCarrier ? !liquidBoth : !gasBoth)
                liquidCarrier = !liquidCarrier;

            const bool carrierBoth = liquidCarrier ? liquidBoth : gasBoth;
            face.carrierSlot = MomentumSlot(box.config.layout, liquidCarrier ? Phase::Liquid : Phase::Gas);
            face.otherSlot = MomentumSlot(box.config.layout, liquidCarrier ? Phase::Gas : Phase::Liquid);
            face.carrierNl = liquidCarrier ? liquidNl : gasNl;
            face.otherNl = liquidCarrier ? gasNl : liquidNl;
            face.otherMassMg = liquidCarrier ? face.gasMassMg : face.liquidMassMg;
            // 2 本目(T-0963): 別の速さで解くのは、しずく・霧(気体が主・液体が主でない混ざった面)と、水面の層の横の面
            // (両側が水面のセル)だけ。液体の中の気体(泡・水面の下の面)と層の重さの軸の面は 1 つの速さ(④ の形)。
            // 1 本目は全部の面で分けて、水の中に紛れた少しの気体が液体の圧力の坂を受け、静かな池(0.5 m)・風洞が暴れた
            const bool layered = LayeredFace(scratch, view, face);
            const bool surfaceLayer = runtime.options.splitSurfaceLayer && layered && face.axis != view.gravityAxis &&
                                      scratch.surfaceCells[face.left] != 0 && scratch.surfaceCells[face.right] != 0;
            // しずく・霧は液体の質量が気体以下の時だけ(重いしぶきは 1 つの速さ = 重い雲として落ちる。消去の a' が液体に引きずられて
            // 気体の速さが暴れない。2 本目の 1 回目は体積 1/9 まで分けて、ダム・連通管の水しぶきで暴れた)
            const bool spray = !liquidCarrier && !layered && face.liquidMassMg <= face.gasMassMg;
            if (face.otherMassMg <= 0 || !carrierBoth || !(surfaceLayer || spray)) {
                PrepareSinglePhaseFace(box, view, scratch, stats, face);
                return;
            }

            face.twoFluid = true;
            face.coupled = true;
            ++stats.fluidPairFaces;
            stats.surfaceFaces += layered ? 1u : 0u;
            const int64_t carrierStart = SlotMomentum(box, face, face.carrierSlot);
            const int64_t otherStart = SlotMomentum(box, face, face.otherSlot);
            SplitFluidArea(box, scratch, face, liquidCarrier, carrierStart, otherStart);
            face.massMg = std::max<int64_t>(liquidCarrier ? face.liquidMassMg : face.gasMassMg,
                                            MinimumFaceMassMg(box, face.carrierAreaMm2));
            ApplyFluidForces(box, view, scratch, face, layered, liquidCarrier, liquidNl, gasNl);
            const int64_t slip = (otherStart / face.otherMassMg) - (carrierStart / face.massMg);
            face.otherDragQ16 = FluidPairDragQ16(box, runtime.options, scratch, face, layered, liquidCarrier,
                                                 fx::FxAbsU64(slip), face.axis == view.gravityAxis);
            stats.cappedDragFaces += face.otherDragQ16 >= DRAG_LIMIT_Q16 ? 1u : 0u;
        }

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

                const int64_t predicted = face.coupled
                                              ? face.predictedNl
                                              : FlowNl(box, FluidMomentum(box, face), face.carrierAreaMm2, face.massMg);
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
            scratch.gasViscosity.resize(count);
            scratch.liquidViscosity.resize(count);
            for (uint32_t cell = 0; cell < count; ++cell) {
                scratch.cells[cell] = MeasureFlowCell(box, context.view, cell, runtime.options);
                scratch.heatCapacityQ16[cell] = CellHeatCapacityQ16(box, cell);
                scratch.viscosityMicroPascalSec[cell] = CellViscosity(box, cell, scratch.cells[cell]);
                if (PerPhase(box))
                    MeasurePhaseViscosity(box, cell, scratch);

                stats.voidCells += scratch.cells[cell].voidNl > 0 ? 1u : 0u;
            }

            scratch.columnMicroPascal = BuildFluidColumns(box, context.view, scratch.cells);
            scratch.faces = BuildFaces(box, scratch.cells, stats);
            if (PerPhase(box)) {
                MeasureSurfaceCells(box, context.view, scratch);
                for (FlowFace& face : scratch.faces)
                    PrepareFluidPhases(runtime, box, context.view, face);
            } else {
                ApplyColumnImpulses(box, context.view, scratch);
            }

            if (runtime.options.drag) {
                MeasureGrains(runtime, box, context.view);
                for (FlowFace& face : scratch.faces)
                    PrepareCoupledFace(runtime, box, context.view, face);
            }

            // 粘りは陰の圧力の前に(面の運動量を受け渡してから、その流れで圧力の式を組む。運ぶ段の流れと食い違わない)
            ApplyViscosity(runtime, box);
            for (FlowFace& face : scratch.faces) {
                if (face.coupled)
                    CoupledSystemTerms(box, face);
            }

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

                if (face.coupled) {
                    SolveCoupledFace(runtime, box, face, values[face.left] - values[face.right]);
                    continue;
                }

                const int64_t impulse = PressureImpulse(box, values[face.left] - values[face.right],
                                                        face.carrierAreaMm2);
                ApplyImpulse(box, CarrierFace(face), impulse, ImpulseSource::Pressure);
                face.flowNl = FlowNl(box, FluidMomentum(box, face), face.carrierAreaMm2, face.massMg);
            }

            if (PerPhase(box)) {
                for (const FlowFace& face : scratch.faces)
                    SplitMergedFace(box, face);
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
            int64_t grainNl = 0;  // 粒そのものの体積(R2。粒の面だけ)
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
                const bool grain = phase == Phase::Grain;
                const int64_t fluidShare = liquid ? take.liquidNl : (phase == Phase::Gas ? take.gasNl : 0);
                const int64_t share = grain ? take.grainNl : fluidShare;
                const int64_t phaseVolume = grain ? cell.grainNl : (liquid ? cell.liquidNl : cell.gasNl);
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

        // 粒の運び(R2): 粒の質量の流れ(Q32)に面の持ち越し(端数。21 §2.10。faceCarry の粒の置き場・mg の Q32)を足し、
        // 整数の mg だけを移して残りを持ち越す(1 mg の粒・薄い粉が丸めで止まらない)。出る量が中の粒を超えたら同じ割合で縮める。
        // 物質は風上のセルの粒の質量の割合で分け(端は最後の物質へ)、エネルギーは熱容量の割合で一緒に
        int64_t MoveGrainMass(Box& box, const StartContent& start, const FlowScratch& scratch, uint32_t from,
                              uint32_t to, int64_t massMg) {
            const auto materials = (uint32_t)box.materials.size();
            const int64_t total = scratch.grains[from].massMg;
            int64_t left = massMg;
            uint64_t heatCapacity = 0;
            uint32_t last = materials;
            for (uint32_t material = 0; material < materials; ++material) {
                if (box.materials[material].phase == Phase::Grain &&
                    start.massMg[((size_t)from * materials) + material] > 0)
                    last = material;
            }

            for (uint32_t material = 0; material < materials && left > 0; ++material) {
                const int64_t startMass = start.massMg[((size_t)from * materials) + material];
                if (box.materials[material].phase != Phase::Grain || startMass <= 0)
                    continue;

                const int64_t share = material == last ? left
                                                       : fx::FxMulDivS64(massMg, (uint64_t)startMass, (uint64_t)total);
                const int64_t mass = std::min({share, left, CellMass(box, from, material)});
                if (mass <= 0)
                    continue;

                heatCapacity += HeatCapacityQ16(box, material, mass);
                left -= mass;
                MoveMass(box, from, to, material, mass);
            }

            const uint64_t capacity = scratch.heatCapacityQ16[from];
            if (heatCapacity > 0 && capacity > 0) {
                const int64_t energy = std::max<int64_t>(start.energyMillijoules[from], 0);
                MoveEnergy(box, from, to, fx::FxMulDivS64(energy, std::min(heatCapacity, capacity), capacity));
            }

            return massMg - left;
        }

        void TransportGrains(FlowRuntime& runtime, Box& box, const StartContent& start) {
            FlowScratch& scratch = runtime.scratch;
            std::vector<uint64_t> demand(scratch.cells.size(), 0);
            for (const FlowFace& face : scratch.faces)
                demand[face.grainFlowNl > 0 ? face.left : face.right] += fx::FxAbsU64(face.grainFlowNl);

            for (size_t cell = 0; cell < demand.size(); ++cell) {
                const auto have = (uint64_t)scratch.grains[cell].massMg << 32;
                runtime.stats->grainLimitedCells += demand[cell] > have ? 1u : 0u;
            }

            const uint32_t slot = MomentumSlot(box.config.layout, Phase::Grain);
            for (FlowFace& face : scratch.faces) {
                face.grainMovedMassMg = 0;
                int64_t& carry = box.faceCarry[face.axis][FaceIndex(box, face.left, slot)];
                if (!GrainMoves(face)) {
                    carry = 0;
                    continue;
                }

                const uint32_t upwind = face.grainFlowNl > 0 ? face.left : face.right;
                const auto have = (uint64_t)scratch.grains[upwind].massMg << 32;
                int64_t flow = face.grainFlowNl;
                if (demand[upwind] > have)
                    flow = fx::FxMulDivS64(flow, have, demand[upwind]);

                // 持ち越しは向きが変わったら捨てる(戻る粒を前の端数で押さない)
                carry = (carry > 0) == (flow > 0) ? carry : 0;
                const int64_t wanted = flow + carry;
                const int64_t whole = wanted / ((int64_t)1 << 32);
                carry = wanted - (whole * ((int64_t)1 << 32));
                if (whole == 0)
                    continue;

                const bool forward = whole > 0;
                const uint32_t from = forward ? face.left : face.right;
                const uint32_t to = forward ? face.right : face.left;
                const int64_t moved = MoveGrainMass(box, start, scratch, from, to, forward ? whole : -whole);
                face.grainMovedMassMg = forward ? moved : -moved;
            }
        }

        // --- 置き場 ①の運び(T-0963)---
        // 相ごとに、風上のセルのその相の割合(Q32)を移す。主の流体と 1 つの相だけの面は解いた体積の流れ(R1 の取り方)から、
        // 主でない流体は面の速さ × Δt ÷ Δx から(しずくの体積が面積の mm² で丸まらない。粒の運びと同じ)
        struct PhaseTake {
            uint32_t face = 0;
            bool liquid = false;
            int64_t fractionQ32 = 0;  // 風上のセルのその相の何割を移すか(left → right が正)
        };

        constexpr int64_t WHOLE_Q32 = (int64_t)1 << 32;

        // 体積(nL)を風上のセルの相の体積の割合(Q32。1 で頭打ち)に
        int64_t VolumeFractionQ32(const FlowCell& upwind, bool liquid, int64_t volumeNl) {
            const int64_t have = liquid ? upwind.liquidNl : upwind.gasNl;
            if (volumeNl == 0 || have <= 0)
                return 0;

            const uint64_t magnitude = fx::FxAbsU64(volumeNl);
            const int64_t fraction = magnitude >= (uint64_t)have
                                         ? WHOLE_Q32
                                         : (int64_t)fx::FxDivU128By64(fx::FxMulU64Full(magnitude, (uint64_t)WHOLE_Q32),
                                                                      (uint64_t)have)
                                               .quotient;

            return volumeNl > 0 ? fraction : -fraction;
        }

        // 面の速さ × Δt ÷ Δx(Q32。符号つき・±1 で頭打ち)= M × 10^6 × 2^12 ÷ (m × 刻み/秒 × 辺 µm)
        int64_t CourantQ32(const Box& box, int64_t faceMassMg, int64_t momentum) {
            if (faceMassMg <= 0)
                return 0;

            const uint64_t denominator = (uint64_t)faceMassMg * box.config.ticksPerSecond *
                                         CellEdgeUm(box.config.level);
            const int64_t fraction = fx::FxMulDivS64(momentum, 1000000ull << 12, denominator);

            return std::clamp<int64_t>(fraction, -WHOLE_Q32, WHOLE_Q32);
        }

        void CollectPhaseTakes(const Box& box, const View& view, const FlowScratch& scratch,
                               std::vector<PhaseTake>& takes) {
            const uint32_t liquidSlot = MomentumSlot(box.config.layout, Phase::Liquid);
            for (uint32_t index = 0; index < scratch.faces.size(); ++index) {
                const FlowFace& face = scratch.faces[index];
                if (!face.open)
                    continue;

                if (face.twoFluid) {
                    const bool carrierLiquid = face.carrierSlot == liquidSlot;
                    const FlowCell& upwind = scratch.cells[face.flowNl > 0 ? face.left : face.right];
                    const int64_t otherMomentum = box.faceMomentum[face.axis]
                                                                  [FaceIndex(box, face.left, face.otherSlot)];
                    takes.push_back({.face = index,
                                     .liquid = carrierLiquid,
                                     .fractionQ32 = VolumeFractionQ32(upwind, carrierLiquid, face.flowNl)});
                    takes.push_back({.face = index,
                                     .liquid = !carrierLiquid,
                                     .fractionQ32 = CourantQ32(box, face.otherMassMg, otherMomentum)});
                    continue;
                }

                if (face.flowNl == 0)
                    continue;

                const FlowCell& upwind = scratch.cells[face.flowNl > 0 ? face.left : face.right];
                const Take take = FaceTake(face, upwind, view);
                const int64_t sign = face.flowNl > 0 ? 1 : -1;
                takes.push_back({.face = index,
                                 .liquid = true,
                                 .fractionQ32 = VolumeFractionQ32(upwind, true, sign * take.liquidNl)});
                takes.push_back({.face = index,
                                 .liquid = false,
                                 .fractionQ32 = VolumeFractionQ32(upwind, false, sign * take.gasNl)});
            }
        }

        // 1 つの相を割合で移す(物質ごとに刻みの初めの質量 × 割合。エネルギーは熱容量の割合で一緒に)。移した質量を返す
        int64_t MovePhaseFraction(Box& box, const StartContent& start, const FlowScratch& scratch, uint32_t from,
                                  uint32_t to, bool liquid, uint64_t fractionQ32) {
            const auto materials = (uint32_t)box.materials.size();
            const Phase phase = liquid ? Phase::Liquid : Phase::Gas;
            int64_t moved = 0;
            uint64_t heatCapacity = 0;
            for (uint32_t material = 0; material < materials; ++material) {
                const int64_t startMass = start.massMg[((size_t)from * materials) + material];
                if (box.materials[material].phase != phase || startMass <= 0)
                    continue;

                const int64_t mass = fx::FxMulShiftS64(startMass, (int64_t)fractionQ32, 32);
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

        // 1 つのセルの相から出る割合の和が 1 を超えたら同じ割合で縮める(数える)。移す量は刻みの初めの中身から(順によらない)
        void TransportPerPhase(FlowRuntime& runtime, Box& box, const StartContent& start, const View& view) {
            FlowScratch& scratch = runtime.scratch;
            for (FlowFace& face : scratch.faces) {
                face.movedMassMg = 0;
                face.movedGasMg = 0;
                face.movedLiquidMg = 0;
            }

            std::vector<PhaseTake> takes;
            CollectPhaseTakes(box, view, scratch, takes);
            std::vector<std::array<uint64_t, 2>> demand(scratch.cells.size(), std::array<uint64_t, 2>{});
            for (const PhaseTake& take : takes) {
                const FlowFace& face = scratch.faces[take.face];
                demand[take.fractionQ32 > 0 ? face.left : face.right][take.liquid ? 1 : 0] += fx::FxAbsU64(
                    take.fractionQ32);
            }

            for (const auto& cell : demand) {
                runtime.stats->limitedCells += cell[0] > (uint64_t)WHOLE_Q32 ? 1u : 0u;
                runtime.stats->limitedCells += cell[1] > (uint64_t)WHOLE_Q32 ? 1u : 0u;
            }

            for (const PhaseTake& take : takes) {
                if (take.fractionQ32 == 0)
                    continue;

                FlowFace& face = scratch.faces[take.face];
                const bool forward = take.fractionQ32 > 0;
                const uint32_t from = forward ? face.left : face.right;
                const uint32_t to = forward ? face.right : face.left;
                uint64_t fraction = fx::FxAbsU64(take.fractionQ32);
                const uint64_t total = demand[from][take.liquid ? 1 : 0];
                if (total > (uint64_t)WHOLE_Q32)
                    fraction = fx::FxDivU128By64(fx::FxMulU64Full(fraction, (uint64_t)WHOLE_Q32), total).quotient;

                const int64_t moved = MovePhaseFraction(box, start, scratch, from, to, take.liquid, fraction);
                const int64_t signedMoved = forward ? moved : -moved;
                (take.liquid ? face.movedLiquidMg : face.movedGasMg) += signedMoved;
                face.movedMassMg += signedMoved;
            }
        }

        void TransportStage(FlowRuntime& runtime, Box& box, const StepContext& context) {
            FlowScratch& scratch = runtime.scratch;
            if (PerPhase(box)) {
                const StartContent start = {.massMg = box.massMg, .energyMillijoules = box.energyMillijoules};
                TransportPerPhase(runtime, box, start, context.view);
                if (runtime.options.drag)
                    TransportGrains(runtime, box, start);

                return;
            }

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

            if (runtime.options.drag)
                TransportGrains(runtime, box, start);
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

        // 運ぶ段が面で移した相の質量と、面の相の質量(④ は流体 1 つ + 粒・① は相ごと)
        int64_t MovedMass(const FlowFace& face, Phase phase, bool perPhase) {
            if (phase == Phase::Grain)
                return face.grainMovedMassMg;

            if (!perPhase)
                return face.movedMassMg;

            return phase == Phase::Liquid ? face.movedLiquidMg : face.movedGasMg;
        }

        int64_t FacePhaseMass(const FlowFace& face, Phase phase, bool perPhase) {
            if (phase == Phase::Grain)
                return face.grainMassMg;

            if (!perPhase)
                return face.massMg;

            return phase == Phase::Liquid ? face.liquidMassMg : face.gasMassMg;
        }

        void CollectTransfers(const Box& box, const FlowScratch& scratch, const FaceTable& table, uint32_t index,
                              Phase phase, std::vector<MomentumTransfer>& transfers) {
            const FlowFace& face = scratch.faces[index];
            for (uint32_t side = 0; side < 3; ++side) {
                const std::optional<uint32_t> beside = Neighbor(box, face.left, side);
                if (!beside.has_value() || table[face.axis][*beside] < 0)
                    continue;

                const int32_t first = table[side][face.left];
                const int32_t second = table[side][face.right];
                if (first < 0 || second < 0)
                    continue;

                const bool perPhase = PerPhase(box);
                const int64_t flux = (MovedMass(scratch.faces[(size_t)first], phase, perPhase) +
                                      MovedMass(scratch.faces[(size_t)second], phase, perPhase)) /
                                     2;
                if (flux == 0)
                    continue;

                const auto other = (uint32_t)table[face.axis][*beside];
                transfers.push_back(
                    {.from = flux > 0 ? index : other, .to = flux > 0 ? other : index, .massMg = fx::FxAbsU64(flux)});
            }
        }

        // 1 つの置き場(④ は流体か粒・① は相ごと)の面の運動量を運ぶ
        void AdvectSlot(const FlowScratch& scratch, Box& box, const FaceTable& table, Phase phase) {
            std::vector<MomentumTransfer> transfers;
            for (uint32_t index = 0; index < scratch.faces.size(); ++index) {
                if (scratch.faces[index].open)
                    CollectTransfers(box, scratch, table, index, phase, transfers);
            }

            std::vector<uint64_t> demand(scratch.faces.size(), 0);
            for (const MomentumTransfer& transfer : transfers)
                demand[transfer.from] += transfer.massMg;

            // 運ぶ量は刻みの初めの面の運動量から決める(移す順に結果がよらない)
            const uint32_t slot = MomentumSlot(box.config.layout, phase);
            const bool perPhase = PerPhase(box);
            const std::array<std::vector<int64_t>, 3> start = box.faceMomentum;
            for (const MomentumTransfer& transfer : transfers) {
                const FlowFace& from = scratch.faces[transfer.from];
                const FlowFace& to = scratch.faces[transfer.to];
                const auto faceMass = (uint64_t)std::max<int64_t>(FacePhaseMass(from, phase, perPhase), 0);
                if (faceMass == 0)
                    continue;

                const uint64_t mass = demand[transfer.from] > faceMass
                                          ? (uint64_t)fx::FxMulDivS64((int64_t)transfer.massMg, faceMass,
                                                                      demand[transfer.from])
                                          : transfer.massMg;
                const int64_t momentum = start[from.axis][FaceIndex(box, from.left, slot)];
                const FaceRef source = {.axis = from.axis, .cell = from.left, .slot = slot};
                const FaceRef target = {.axis = to.axis, .cell = to.left, .slot = slot};
                MoveMomentum(box, source, target, fx::FxMulDivS64(momentum, mass, faceMass));
            }
        }

        void ApplyStage(FlowRuntime& runtime, Box& box, const StepContext&) {
            const FlowScratch& scratch = runtime.scratch;
            const FaceTable table = IndexFaces(scratch, CellCount(box));
            AdvectSlot(scratch, box, table, Phase::Gas);
            if (PerPhase(box))
                AdvectSlot(scratch, box, table, Phase::Liquid);

            if (runtime.options.drag)
                AdvectSlot(scratch, box, table, Phase::Grain);
        }

        // --- 粘り(T-0927・T-0926 の仮説 2。陰の圧力の段の中で、系を組む前に)---
        // 面 F の速さ v = M ÷ m(m は面の流体の質量)。隣の面 G(同じ軸・セルを e_b ずらす)との間で
        //   物の粘り: ΔM = μ A Δt (v_G − v_F) ÷ Δ = μ × 辺 × Δv ÷ 刻み/秒(µPa·s × mm × 2^-20 m/s ÷ 1000 で mg·2^-20 m/s)
        //   渦の粘り: ΔM = m C_s² Δt |Δv| Δv ÷ Δ(m は 2 つの面の小さい方の質量)
        // 1 つの組で渡すのは、2 つの面の速さをそろえる量の 1/12 まで(1 つの面は隣が 6 つ。順によらず行き過ぎない)
        int64_t ViscousTransfer(const Box& box, const FlowOptions& options, uint32_t viscosity, int64_t firstMassMg,
                                int64_t secondMassMg, int64_t firstMomentum, int64_t secondMomentum) {
            const int64_t firstVelocity = firstMomentum / firstMassMg;
            const int64_t secondVelocity = secondMomentum / secondMassMg;
            const int64_t difference = secondVelocity - firstVelocity;
            if (difference == 0)
                return 0;

            const uint64_t magnitude = fx::FxAbsU64(difference);
            const uint64_t edgeMm = CellEdgeUm(box.config.level) / 1000;
            const uint64_t ticks = box.config.ticksPerSecond;
            const auto lighter = (uint64_t)std::min(firstMassMg, secondMassMg);
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

        // 置き場 ①の粘り: 相ごとの置き場で、同じ相の隣の面どうし(4 つのセルの相の粘りの小さい方。相の無いセルを挟めば 0)
        void ApplyPhaseViscosity(FlowRuntime& runtime, Box& box, Phase phase) {
            const FlowOptions& options = runtime.options;
            const FlowScratch& scratch = runtime.scratch;
            const std::vector<uint32_t>& values = phase == Phase::Liquid ? scratch.liquidViscosity
                                                                         : scratch.gasViscosity;
            const uint32_t slot = MomentumSlot(box.config.layout, phase);
            const FaceTable table = IndexFaces(scratch, CellCount(box));
            const std::array<std::vector<int64_t>, 3> start = box.faceMomentum;
            for (uint32_t index = 0; index < scratch.faces.size(); ++index) {
                const FlowFace& face = scratch.faces[index];
                const int64_t mass = FacePhaseMass(face, phase, true);
                if (!face.open || mass <= 0)
                    continue;

                for (uint32_t side = 0; side < 3; ++side) {
                    const std::optional<uint32_t> beside = Neighbor(box, face.left, side);
                    if (!beside.has_value() || table[face.axis][*beside] < 0)
                        continue;

                    const FlowFace& other = scratch.faces[(size_t)table[face.axis][*beside]];
                    const int64_t otherMass = FacePhaseMass(other, phase, true);
                    if (otherMass <= 0)
                        continue;

                    const uint32_t viscosity = std::min(
                        {values[face.left], values[face.right], values[other.left], values[other.right]});
                    const int64_t moved = ViscousTransfer(box, options, viscosity, mass, otherMass,
                                                          start[face.axis][FaceIndex(box, face.left, slot)],
                                                          start[other.axis][FaceIndex(box, other.left, slot)]);
                    if (moved == 0)
                        continue;

                    MoveMomentum(box, {.axis = other.axis, .cell = other.left, .slot = slot},
                                 {.axis = face.axis, .cell = face.left, .slot = slot}, moved);
                    ++runtime.stats->viscousPairs;
                }
            }
        }

        void ApplyViscosity(FlowRuntime& runtime, Box& box) {
            const FlowOptions& options = runtime.options;
            // ① の粘り(相ごとの置き場)はまだ無い(0.5 m では効かない。ADR-0409。T-0963 の仮)
            if (PerPhase(box) || (!options.materialViscosity && options.eddyCoefficientPer10000 == 0))
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
                    const int64_t moved = ViscousTransfer(box, options, viscosity, face.massMg, other.massMg,
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
        for (uint32_t index = 0; index < box.materials.size(); ++index) {
            if (box.materials[index].phase == Phase::Grain)
                result.grainNl += (int64_t)MaterialVolumeNl(CellMass(box, cell, index),
                                                            box.materials[index].densityKgPerM3);
        }

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
