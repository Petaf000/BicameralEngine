// gas_reference.cpp — 気体の流れの CPU リファレンス G1(gas_reference.h・07 §2.1・ADR-0043)。
// 1 小刻み = (1) セルから導く値(質量・温度・圧力)→ (2) 面の力(圧力の力積・重さ)を当てる → (3) 力を当てた後の運動量で
// 面の質量の流れを出し、風上のセルごとに出ていく割合を合計 1 までに抑える → (4) 面ごとに風上の中身を同じ割合で移す。
// どの段も前の段の結果を読み、次の配列へ整数の足し引きだけで書く(順番に依存しない。04 R2・R3)。
#include "sim/gas_reference.h"

#include <cassert>
#include <limits>

#include "common/fixed.hlsli"

namespace bicameral::sim {

    namespace {

        namespace fx = bicameral::fx;

        // --- 物理の定数(整数。導き方はコメントに)---

        // 理想気体: p(µPa)= n(µmol)× T(mK)× R ÷ V × 1e-3 = n × T × 0.0665157(R = 8.314462618 J/(mol·K)・V = 0.125 m³)。Q32
        constexpr uint64_t PRESSURE_PER_AMOUNT_TEMPERATURE_Q32 = 285682760;

        // 層の重さで下がる圧力: m(mg)× 1e-6 × g ÷ A(0.25 m²)= m × 39.2266 µPa(g = 9.80665 m/s²)
        constexpr uint64_t HYDROSTATIC_MICROPASCAL_PER_MASS_X10000 = 392266;

        constexpr uint64_t GRAVITY_MICROMETER_PER_SECOND2 = 9806650;
        constexpr uint64_t MICRO = 1000000;
        constexpr uint64_t ONE_Q32 = (uint64_t)1 << 32;

        // 面の片側(箱の中のセル・開いた境界の外・壁)
        enum class SideKind : uint32_t {
            Cell,
            Ghost,
            Wall,
        };

        struct FaceSide {
            SideKind kind = SideKind::Cell;
            uint32_t index = 0;  // Cell: セルの番号 / Ghost: 層 z
        };

        struct GasFace {
            FaceSide left;   // 軸のマイナス側
            FaceSide right;  // 軸のプラス側
            uint32_t axis = 0;
            int64_t flow = 0;       // 質量の流れ(mg/小刻み。左 → 右が正)
            int64_t impulse = 0;    // 面の圧力の力積(左のセルから引き、右のセルへ足す)
            uint64_t fraction = 0;  // 風上の中身のうち移す割合(Q32)

            // --- 重さ(縦の面だけ。基準との質量の差 × g を面の両側の半分ずつのセルで受ける)---
            int64_t weightLeft = 0;  // 左のセルの運動量 z から引く力積
            int64_t weightRight = 0;
        };

        uint64_t MulShiftU64(uint64_t a, uint64_t b, uint32_t shift) {
            return fx::FxShiftRightU128(fx::FxMulU64Full(a, b), shift);
        }

        uint64_t TotalAmount(const GasConfig& config, const GasCell& cell) {
            uint64_t total = 0;
            for (uint32_t s = 0; s < config.speciesCount; ++s)
                total += cell.amounts[s];

            return total;
        }

        // 熱容量(nJ/K)= Σ 物質量(µmol)× 比熱(mJ/(mol·K))
        uint64_t HeatCapacity(const GasConfig& config, const GasCell& cell) {
            uint64_t capacity = 0;
            for (uint32_t s = 0; s < config.speciesCount; ++s)
                capacity += cell.amounts[s] * config.species[s].heatCapacity;

            return capacity;
        }

        // 化学のエネルギー(µJ)= Σ 物質量 × H0
        int64_t ChemicalEnergy(const GasConfig& config, const GasCell& cell) {
            int64_t chemical = 0;
            for (uint32_t s = 0; s < config.speciesCount; ++s)
                chemical += (int64_t)cell.amounts[s] * config.species[s].formationEnergy;

            return chemical;
        }

        uint64_t CellMass(const GasConfig& config, const GasCoefficients& coefficients, const GasCell& cell) {
            uint64_t mass = 0;
            for (uint32_t s = 0; s < config.speciesCount; ++s)
                mass += MulShiftU64(cell.amounts[s], coefficients.massPerAmount[s], 32);

            return mass;
        }

        // 温度(mK)= 熱(µJ)× 1e6 ÷ 熱容量(nJ/K)。熱が 0 以下なら 0
        uint32_t CellTemperature(const GasConfig& config, const GasCell& cell) {
            const int64_t heat = cell.energy * 1000 - ChemicalEnergy(config, cell);
            const uint64_t capacity = HeatCapacity(config, cell);
            if (heat <= 0 || capacity == 0)
                return 0;

            const uint64_t temperature = fx::FxDivU128By64(fx::FxMulU64Full((uint64_t)heat, MICRO), capacity).quotient;
            assert(temperature <= std::numeric_limits<uint32_t>::max());

            return (uint32_t)temperature;
        }

        // 本物の圧力(µPa)= n × T × R ÷ V
        int64_t CellPressure(const GasConfig& config, const GasCell& cell, uint32_t temperature) {
            const fx::FxU128 amountTemperature = fx::FxMulU64Full(TotalAmount(config, cell), temperature);
            assert(amountTemperature.hi == 0);

            return (int64_t)MulShiftU64(amountTemperature.lo, PRESSURE_PER_AMOUNT_TEMPERATURE_Q32, 32);
        }

        // 質量・温度・本物の圧力(p̃ は基準が要るので呼ぶ側で)
        GasDerived DeriveRaw(const GasConfig& config, const GasCoefficients& coefficients, const GasCell& cell) {
            GasDerived derived;
            derived.mass = CellMass(config, coefficients, cell);
            derived.inverseMass = derived.mass == 0 ? 0 : std::numeric_limits<uint64_t>::max() / derived.mass;
            derived.temperature = CellTemperature(config, cell);
            derived.pressure = CellPressure(config, cell, derived.temperature);

            return derived;
        }

        // --- 小刻みの部品 ---

        uint32_t AxisSize(const GasConfig& config, uint32_t axis) {
            return axis == 0 ? config.sizeX : (axis == 1 ? config.sizeY : config.sizeZ);
        }

        // セル(x, y, z)の軸 axis の隣(delta = ±1)の番号。箱の外なら -1 は返さず呼ぶ側で境界を見る
        uint32_t Neighbor(const GasBox& box, uint32_t x, uint32_t y, uint32_t z, uint32_t axis, uint32_t coordinate) {
            if (axis == 0)
                return box.Index(coordinate, y, z);

            if (axis == 1)
                return box.Index(x, coordinate, z);

            return box.Index(x, y, coordinate);
        }

        FaceSide BoundarySide(GasBoundary boundary, uint32_t z) {
            if (boundary == GasBoundary::Open)
                return {SideKind::Ghost, z};

            return {SideKind::Wall, 0};
        }

        // 面の一覧: どの面も 1 回だけ(セルのマイナス側の面 + 端のセルのプラス側の境界の面)
        std::vector<GasFace> EnumerateFaces(const GasBox& box) {
            const GasConfig& config = box.config;
            std::vector<GasFace> faces;
            faces.reserve(box.cells.size() * GAS_AXES + 64);

            for (uint32_t z = 0; z < config.sizeZ; ++z) {
                for (uint32_t y = 0; y < config.sizeY; ++y) {
                    for (uint32_t x = 0; x < config.sizeX; ++x) {
                        const uint32_t self = box.Index(x, y, z);
                        const std::array<uint32_t, GAS_AXES> coordinate = {x, y, z};

                        for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                            const uint32_t size = AxisSize(config, axis);
                            const uint32_t i = coordinate[axis];
                            const GasBoundary boundary = config.boundary[axis];
                            GasFace face;
                            face.axis = axis;
                            face.right = {SideKind::Cell, self};

                            if (i > 0)
                                face.left = {SideKind::Cell, Neighbor(box, x, y, z, axis, i - 1)};
                            else if (boundary == GasBoundary::Periodic)
                                face.left = {SideKind::Cell, Neighbor(box, x, y, z, axis, size - 1)};
                            else
                                face.left = BoundarySide(boundary, z);

                            faces.push_back(face);

                            if (i + 1 < size || boundary == GasBoundary::Periodic)
                                continue;

                            GasFace high;
                            high.axis = axis;
                            high.left = {SideKind::Cell, self};
                            high.right = BoundarySide(boundary, z);
                            faces.push_back(high);
                        }
                    }
                }
            }

            return faces;
        }

        uint32_t CellLayer(const GasBox& box, uint32_t index) {
            return index / (box.config.sizeX * box.config.sizeY);
        }

        const GasCell& SideCell(const GasBox& box, const std::vector<GasCell>& cells, FaceSide side) {
            return side.kind == SideKind::Ghost ? box.ghost[side.index] : cells[side.index];
        }

        const GasDerived& SideDerived(const GasBox& box, const std::vector<GasDerived>& derived, FaceSide side) {
            return side.kind == SideKind::Ghost ? box.referenceDerived[side.index] : derived[side.index];
        }

        // 基準との質量の差(mg)。開いた境界の外は基準そのもの(0)
        int64_t Excess(const GasBox& box, const std::vector<GasDerived>& derived, FaceSide side) {
            if (side.kind != SideKind::Cell)
                return 0;

            return (int64_t)derived[side.index].mass - (int64_t)box.referenceDerived[CellLayer(box, side.index)].mass;
        }

        // 質量の差 Δm(mg)が 1 セルの高さで作る静水圧の差の半分(µPa)= Δm × 39.2266 ÷ 2
        int64_t HalfHydrostatic(int64_t excess) {
            return excess * (int64_t)HYDROSTATIC_MICROPASCAL_PER_MASS_X10000 / 20000;
        }

        // 圧力の拡散を起こす圧力の差: p̃_左 − p̃_右。縦の面では、両側の質量の差の平均が作る静水圧の差を引く
        // (釣り合った重い層・軽い層を拡散が押し上げ続けると、重力と引き合って偽の循環が回る。力の釣り合いをそろえる。07 §2.1)
        int64_t DrivingPressure(const GasBox& box, const std::vector<GasDerived>& derived, const GasFace& face) {
            const int64_t difference = SideDerived(box, derived, face.left).scaledPressure -
                                       SideDerived(box, derived, face.right).scaledPressure;
            if (face.axis != 2 || !box.config.gravity)
                return difference;

            return difference - HalfHydrostatic(Excess(box, derived, face.left) + Excess(box, derived, face.right));
        }

        // 縦の面の重さ: 面の両側の質量の差の平均 × g を、両側のセルが 1/4 ずつ受ける(セルから見ると 1/4・1/2・1/4 で均した重さ。
        // 1 セルおきの温度の縞が作る重さの縞は面の平均で消える〔中心の流れが応じない縞を力で育てない〕。
        // 圧力の差の中心差分と同じ形なので、静水圧の釣り合いが離散の式でそろう。07 §2.1)
        void ComputeFaceWeight(const GasBox& box, const std::vector<GasDerived>& derived, GasFace& face) {
            if (face.axis != 2 || !box.config.gravity)
                return;

            const int64_t coefficient = (int64_t)box.coefficients.gravityImpulse;
            if (face.left.kind == SideKind::Wall) {
                face.weightRight = fx::FxMulShiftS64(Excess(box, derived, face.right), coefficient, 17);
                return;
            }

            if (face.right.kind == SideKind::Wall) {
                face.weightLeft = fx::FxMulShiftS64(Excess(box, derived, face.left), coefficient, 17);
                return;
            }

            const int64_t quarter = fx::FxMulShiftS64(
                Excess(box, derived, face.left) + Excess(box, derived, face.right), coefficient, 18);
            face.weightLeft = quarter;
            face.weightRight = quarter;
        }

        // 面の力: 圧力の力積と重さ(小刻みの初めの導く値だけから)
        void ComputeFaceForce(const GasBox& box, const std::vector<GasDerived>& derived, GasFace& face) {
            const GasCoefficients& coefficients = box.coefficients;
            ComputeFaceWeight(box, derived, face);

            // --- 壁: 鏡に映した圧力(両側とも壁の手前のセルの p̃)---
            if (face.left.kind == SideKind::Wall || face.right.kind == SideKind::Wall) {
                // 縦の壁は、壁の手前のセルの圧力を半セル分の静水圧で壁まで延ばす(床は高く・天井は低く。重さの釣り合いとそろえる)
                const bool wallBelow = face.left.kind == SideKind::Wall;
                const FaceSide inside = wallBelow ? face.right : face.left;
                int64_t pressure = derived[inside.index].scaledPressure;
                if (face.axis == 2 && box.config.gravity) {
                    const int64_t hydrostatic = HalfHydrostatic(Excess(box, derived, inside));
                    pressure += wallBelow ? hydrostatic : -hydrostatic;
                }

                face.impulse = fx::FxMulShiftS64(pressure * 2, (int64_t)coefficients.pressureImpulse, 16);
                return;
            }

            const int64_t pressureSum = SideDerived(box, derived, face.left).scaledPressure +
                                        SideDerived(box, derived, face.right).scaledPressure;
            face.impulse = fx::FxMulShiftS64(pressureSum, (int64_t)coefficients.pressureImpulse, 16);
        }

        // 面の質量の流れ(力を当てた後の運動量 + 小刻みの初めの圧力。前進・後退の順〔forward-backward〕)。壁は 0
        void ComputeFaceFlow(const GasBox& box, const std::vector<GasDerived>& derived,
                             const std::vector<GasCell>& forced, GasFace& face) {
            if (face.left.kind == SideKind::Wall || face.right.kind == SideKind::Wall)
                return;

            const int64_t momentumSum = SideCell(box, forced, face.left).momentum[face.axis] +
                                        SideCell(box, forced, face.right).momentum[face.axis];

            // --- 中心の流れ + 圧力の拡散(Rusanov の散逸を圧力の差にかける: 接触面〔温度・成分の境〕を散らさず、静止大気では 0)---
            const int64_t central = fx::FxMulShiftS64(momentumSum, (int64_t)box.coefficients.centralFlow, 52);
            const int64_t diffusion = fx::FxMulShiftS64(DrivingPressure(box, derived, face),
                                                        (int64_t)box.coefficients.pressureFlow, 40);
            face.flow = central + diffusion;
        }

        // 風上のセルの中身のうち移す割合(Q32。抑える前)
        uint64_t RawFraction(const GasBox& box, const std::vector<GasDerived>& derived, const GasFace& face) {
            if (face.flow == 0)
                return 0;

            const FaceSide donor = face.flow > 0 ? face.left : face.right;
            const uint64_t inverseMass = SideDerived(box, derived, donor).inverseMass;

            return MulShiftU64(fx::FxAbsU64(face.flow), inverseMass, 32);
        }

        // a ÷ b を最も近い整数に(ちょうど半分は 0 から遠い側へ)。b > 0
        int64_t DivideNearest(int64_t a, uint64_t b) {
            const uint64_t magnitude = fx::FxAbsU64(a);
            const fx::FxU128 product = {0, magnitude};
            const fx::FxDivResult result = fx::FxDivU128By64(product, b);
            const uint64_t quotient = result.quotient + (result.remainder * 2 >= b ? 1 : 0);

            return fx::FxApplySign(quotient, a < 0);
        }

        // 移す塊のエネルギー: 熱は風上の熱を熱容量の比で分け(塊の温度 = 風上の温度)、化学のエネルギーは移す物質量から。
        // 物質量の切り捨てとエネルギーの切り捨てが別々だと、塊の温度が少しずつ偏り、流れに沿って熱が運ばれる偽の温度の縞が
        // 浮力で対流を育てる(試作で見た。07 §2.1)。丸めは最も近い整数(偏りなし)
        int64_t MovedEnergy(const GasConfig& config, const GasCell& source, const GasCell& moved) {
            const uint64_t sourceCapacity = HeatCapacity(config, source);
            const int64_t movedChemical = ChemicalEnergy(config, moved);
            if (sourceCapacity == 0)
                return DivideNearest(movedChemical, 1000);

            const int64_t sourceHeat = source.energy * 1000 - ChemicalEnergy(config, source);
            const fx::FxU128 numerator = fx::FxMulU64Full(fx::FxAbsU64(sourceHeat), HeatCapacity(config, moved));
            const fx::FxDivResult share = fx::FxDivU128By64(numerator, sourceCapacity);
            const uint64_t heat = share.quotient + (share.remainder * 2 >= sourceCapacity ? 1 : 0);
            const int64_t movedHeat = fx::FxApplySign(heat, sourceHeat < 0);

            return DivideNearest(movedHeat + movedChemical, 1000);
        }

        // 中身の一部を移す(風上 → 風下)。どちらかが開いた境界の外なら帳簿へ
        void Transfer(GasBox& box, const std::vector<GasCell>& forced, std::vector<GasCell>& next,
                      const GasFace& face) {
            if (face.fraction == 0)
                return;

            const bool forward = face.flow > 0;
            const FaceSide donor = forward ? face.left : face.right;
            const FaceSide receiver = forward ? face.right : face.left;
            const GasCell& source = SideCell(box, forced, donor);
            const uint32_t speciesCount = box.config.speciesCount;

            GasCell moved;
            for (uint32_t s = 0; s < speciesCount; ++s)
                moved.amounts[s] = MulShiftU64(source.amounts[s], face.fraction, 32);

            moved.energy = MovedEnergy(box.config, source, moved);
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                moved.momentum[axis] = fx::FxMulShiftS64(source.momentum[axis], (int64_t)face.fraction, 32);

            // --- 引く側 ---
            if (donor.kind == SideKind::Cell) {
                GasCell& cell = next[donor.index];
                for (uint32_t s = 0; s < speciesCount; ++s) {
                    assert(cell.amounts[s] >= moved.amounts[s]);
                    cell.amounts[s] -= moved.amounts[s];
                }

                cell.energy -= moved.energy;
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                    cell.momentum[axis] -= moved.momentum[axis];
            } else {
                for (uint32_t s = 0; s < speciesCount; ++s)
                    box.ledger.amounts[s] += (int64_t)moved.amounts[s];

                box.ledger.energy += moved.energy;
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                    box.ledger.momentum[axis] += moved.momentum[axis];
            }

            // --- 足す側 ---
            if (receiver.kind == SideKind::Cell) {
                GasCell& cell = next[receiver.index];
                for (uint32_t s = 0; s < speciesCount; ++s)
                    cell.amounts[s] += moved.amounts[s];

                cell.energy += moved.energy;
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                    cell.momentum[axis] += moved.momentum[axis];
            } else {
                for (uint32_t s = 0; s < speciesCount; ++s)
                    box.ledger.amounts[s] -= (int64_t)moved.amounts[s];

                box.ledger.energy -= moved.energy;
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                    box.ledger.momentum[axis] -= moved.momentum[axis];
            }
        }

        // 面の力積: 左のセルから引き、右のセルへ足す。箱の外(壁・開いた境界)の側は帳簿へ
        void ApplyImpulse(GasBox& box, std::vector<GasCell>& next, const GasFace& face) {
            // --- 重さ(箱の外から働く力なので帳簿へ)---
            if (face.left.kind == SideKind::Cell && face.weightLeft != 0) {
                next[face.left.index].momentum[2] -= face.weightLeft;
                box.ledger.momentum[2] -= face.weightLeft;
            }

            if (face.right.kind == SideKind::Cell && face.weightRight != 0) {
                next[face.right.index].momentum[2] -= face.weightRight;
                box.ledger.momentum[2] -= face.weightRight;
            }

            if (face.impulse == 0)
                return;

            if (face.left.kind == SideKind::Cell)
                next[face.left.index].momentum[face.axis] -= face.impulse;
            else
                box.ledger.momentum[face.axis] += face.impulse;

            if (face.right.kind == SideKind::Cell)
                next[face.right.index].momentum[face.axis] += face.impulse;
            else
                box.ledger.momentum[face.axis] -= face.impulse;
        }

        // 出ていく割合の合計が 1 を超えるセルは、その面の割合を比例して縮める(07「共通の形」。負の中身を作らない)
        void LimitOutflow(const GasBox& box, const std::vector<GasDerived>& derived, std::vector<GasFace>& faces) {
            std::vector<uint64_t> outflow(box.cells.size(), 0);
            for (GasFace& face : faces) {
                face.fraction = RawFraction(box, derived, face);
                const FaceSide donor = face.flow > 0 ? face.left : face.right;
                if (face.fraction != 0 && donor.kind == SideKind::Cell)
                    outflow[donor.index] += face.fraction;
            }

            for (GasFace& face : faces) {
                const FaceSide donor = face.flow > 0 ? face.left : face.right;
                if (face.fraction == 0 || donor.kind != SideKind::Cell || outflow[donor.index] <= ONE_Q32)
                    continue;

                face.fraction = fx::FxDivU128By64(fx::FxMulU64Full(face.fraction, ONE_Q32), outflow[donor.index])
                                    .quotient;
            }
        }

        void Substep(GasBox& box, const std::vector<GasFace>& layout) {
            const GasConfig& config = box.config;

            // --- (1) 導く値 ---
            std::vector<GasDerived> derived(box.cells.size());
            for (uint32_t z = 0; z < config.sizeZ; ++z) {
                for (uint32_t y = 0; y < config.sizeY; ++y) {
                    for (uint32_t x = 0; x < config.sizeX; ++x) {
                        const uint32_t index = box.Index(x, y, z);
                        derived[index] = DeriveGasCell(box, box.cells[index], z);
                    }
                }
            }

            // --- (2) 面の力(圧力の力積・重さ)を先に当てる ---
            std::vector<GasFace> faces = layout;
            for (GasFace& face : faces)
                ComputeFaceForce(box, derived, face);

            std::vector<GasCell> forced = box.cells;
            for (const GasFace& face : faces)
                ApplyImpulse(box, forced, face);

            // --- (3) 力を当てた後の運動量で面の質量の流れ・出ていく割合を抑える ---
            // 前進・後退の順: 力と流れを同じ古い値から出すと(前進 Euler)、重さと音の波が組んだゆっくりした振動が育つ
            // (小刻み 4・c̃ 30 m/s で 10^4 刻みに 10 倍。試作で見た。07 §2.1)
            for (GasFace& face : faces)
                ComputeFaceFlow(box, derived, forced, face);

            LimitOutflow(box, derived, faces);

            // --- (4) 風上の中身(力を当てた後)を移す ---
            std::vector<GasCell> next = forced;
            for (const GasFace& face : faces)
                Transfer(box, forced, next, face);

            box.cells = std::move(next);
        }

    }  // namespace

    GasCoefficients MakeGasCoefficients(const GasConfig& config) {
        assert(config.substeps > 0 && config.soundSpeedMmPerS > 0);
        const uint64_t substeps = config.substeps;
        GasCoefficients coefficients;

        // mg/µmol = M(mg/mol)÷ 1e6
        for (uint32_t s = 0; s < config.speciesCount; ++s)
            coefficients.massPerAmount[s] = ((uint64_t)config.species[s].molarMass << 32) / MICRO;

        // 中心の流れ: F(mg)=(P_左 + P_右)÷ 2 × 2^-20 × Δt ÷ Δx、Δt ÷ Δx = 1 ÷ (30 × 小刻み)(s/m)
        coefficients.centralFlow = ((uint64_t)1 << 31) / (30 * substeps);

        // 圧力の拡散: F(mg)= Δp̃(µPa)× 1e-6 × A × Δt ÷ (2 c̃)× 1e6 = Δp̃ × 25 ÷ (12 × 小刻み × c̃(mm/s))
        coefficients.pressureFlow = (25 * ((uint64_t)1 << 40)) / (12 * substeps * config.soundSpeedMmPerS);

        // 面の力積: (p̃_左 + p̃_右)÷ 2 × 1e-6 × A × Δt × 1e6(mg/kg)× 2^20 =(和)× 2^15 ÷ (15 × 小刻み)
        coefficients.pressureImpulse = ((uint64_t)1 << 31) / (15 * substeps);

        // 浮力: Δm(mg)× g × Δt × 2^20 = Δm × 9.80665 × 2^20 ÷ (60 × 小刻み)
        coefficients.gravityImpulse = (GRAVITY_MICROMETER_PER_SECOND2 << 36) / (60 * substeps * MICRO);

        return coefficients;
    }

    GasCell MakeGasCell(const GasConfig& config, std::span<const uint64_t> amounts, uint32_t temperatureMilliKelvin) {
        GasCell cell;
        for (uint32_t s = 0; s < config.speciesCount && s < amounts.size(); ++s)
            cell.amounts[s] = amounts[s];

        // 熱(µJ)= 熱容量(nJ/K)× T(mK)÷ 1e6
        const uint64_t heat = fx::FxDivU128By64(fx::FxMulU64Full(HeatCapacity(config, cell), temperatureMilliKelvin),
                                                MICRO)
                                  .quotient;
        cell.energy = fx::FxDivS64((int64_t)heat + ChemicalEnergy(config, cell), 1000);

        return cell;
    }

    std::vector<GasCell> MakeHydrostaticReference(const GasConfig& config, std::span<const uint32_t> fractionsQ32,
                                                  uint32_t temperatureMilliKelvin,
                                                  uint64_t surfacePressureMicroPascal) {
        const GasCoefficients coefficients = MakeGasCoefficients(config);
        std::vector<GasCell> layers;
        uint64_t pressure = surfacePressureMicroPascal;

        for (uint32_t z = 0; z < config.sizeZ; ++z) {
            // --- 物質量 n = p ÷ (T × R ÷ V)---
            const uint64_t divisor = PRESSURE_PER_AMOUNT_TEMPERATURE_Q32 * temperatureMilliKelvin;
            const uint64_t total = fx::FxDivU128By64(fx::FxMulU64Full(pressure, ONE_Q32), divisor).quotient;

            std::array<uint64_t, GAS_MAX_SPECIES> amounts = {};
            for (uint32_t s = 0; s < config.speciesCount && s < fractionsQ32.size(); ++s)
                amounts[s] = MulShiftU64(total, fractionsQ32[s], 32);

            const GasCell cell = MakeGasCell(config, amounts, temperatureMilliKelvin);
            layers.push_back(cell);

            // --- 上の層の圧力 = この層の重さの分だけ下げる(釣り合いは近似でよい: 力は基準との差で入れるので、基準そのものは止まっている)---
            const uint64_t mass = CellMass(config, coefficients, cell);
            pressure -= mass * HYDROSTATIC_MICROPASCAL_PER_MASS_X10000 / 10000;
        }

        return layers;
    }

    GasBox MakeGasBox(const GasConfig& config, std::vector<GasCell> reference) {
        assert(reference.size() == config.sizeZ);
        GasBox box;
        box.config = config;
        box.coefficients = MakeGasCoefficients(config);
        box.reference = std::move(reference);

        // --- 基準の導く値(p̃ = 0)と開いた境界の外(基準 + 風)---
        for (const GasCell& layer : box.reference) {
            box.referenceDerived.push_back(DeriveRaw(config, box.coefficients, layer));

            GasCell ghost = layer;
            const int64_t mass = (int64_t)box.referenceDerived.back().mass;
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                ghost.momentum[axis] = mass * config.windVelocity[axis];

            box.ghost.push_back(ghost);
        }

        // --- α = c̃² ÷ c_iso²、c_iso² = p ÷ ρ = p(µPa)× 0.125 ÷ m(mg)(地面の層)---
        // α(Q32)= c̃(mm/s)² × m × 8 × 2^32 ÷ (p × 1e6)
        const GasDerived& ground = box.referenceDerived.front();
        const uint64_t sound = config.soundSpeedMmPerS;
        const fx::FxU128 numerator = fx::FxMulU64Full(sound * sound * 8 * ground.mass, ONE_Q32);
        box.coefficients.soundScale = fx::FxDivU128By64(numerator, (uint64_t)ground.pressure * MICRO).quotient;
        box.coefficients.soundTemperature = ground.temperature;

        // --- セル = 層の基準 + 風 ---
        box.cells.resize((size_t)config.sizeX * config.sizeY * config.sizeZ);
        for (uint32_t z = 0; z < config.sizeZ; ++z) {
            for (uint32_t y = 0; y < config.sizeY; ++y) {
                for (uint32_t x = 0; x < config.sizeX; ++x)
                    box.cells[box.Index(x, y, z)] = box.ghost[z];
            }
        }

        return box;
    }

    GasDerived DeriveGasCell(const GasBox& box, const GasCell& cell, uint32_t z) {
        GasDerived derived = DeriveRaw(box.config, box.coefficients, cell);

        // p̃ = α(p − p_基準)。基準より熱いセルは α × T_基準 ÷ T(実効の音速を c̃ に抑える。c_iso² ∝ T)
        const int64_t deviation = derived.pressure - box.referenceDerived[z].pressure;
        int64_t scaled = fx::FxMulShiftS64(deviation, (int64_t)box.coefficients.soundScale, 32);
        const uint32_t soundTemperature = box.coefficients.soundTemperature;
        if (derived.temperature > soundTemperature)
            scaled = fx::FxDivS64(fx::FxMulShiftS64(scaled, soundTemperature, 0), derived.temperature);

        derived.scaledPressure = scaled;

        return derived;
    }

    int64_t GasVelocity(const GasBox& box, const GasCell& cell, uint32_t axis) {
        const uint64_t mass = CellMass(box.config, box.coefficients, cell);
        if (mass == 0)
            return 0;

        return fx::FxDivS64(cell.momentum[axis], (int64_t)mass);
    }

    void StepGas(GasBox& box) {
        const std::vector<GasFace> layout = EnumerateFaces(box);
        for (uint32_t substep = 0; substep < box.config.substeps; ++substep)
            Substep(box, layout);

        ++box.tick;
    }

    GasTotals SumGas(const GasBox& box) {
        GasTotals totals;
        for (const GasCell& cell : box.cells) {
            for (uint32_t s = 0; s < box.config.speciesCount; ++s)
                totals.amounts[s] += cell.amounts[s];

            totals.energy += cell.energy;
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                totals.momentum[axis] += cell.momentum[axis];
        }

        return totals;
    }

    uint64_t HashGas(const GasBox& box) {
        uint64_t hash = fx::FxMix64(box.tick);
        for (const GasCell& cell : box.cells) {
            for (uint32_t s = 0; s < box.config.speciesCount; ++s)
                hash = fx::FxHashCombine(hash, cell.amounts[s]);

            hash = fx::FxHashCombine(hash, (uint64_t)cell.energy);
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                hash = fx::FxHashCombine(hash, (uint64_t)cell.momentum[axis]);
        }

        return hash;
    }

}  // namespace bicameral::sim
