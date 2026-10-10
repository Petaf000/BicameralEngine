// gas_reference.cpp — 気体の流れの CPU リファレンス G1・G2(gas_reference.h・07 §2.1・§2.2・ADR-0043)。
// 1 小刻み = (1) セルから導く値(質量・温度・圧力)→ (2) 面の力(圧力の力積・重さ)を当てる → (3) 力を当てた後の運動量で
// 面の質量の流れを出し、風上のセルごとに出ていく割合を合計 1 までに抑える → (4) 面ごとに移す成分の物質量を決める
// (MUSCL で面へ延ばした割合・乱数の丸め。G2)→ 風上のセルごとに合計が中身を超えないよう抑える → (5) 移す。
// どの段も前の段の結果を読み、次の配列へ整数の足し引きだけで書く(順番に依存しない。04 R2・R3)。
#include "sim/gas_reference.h"

#include <algorithm>
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
        constexpr uint64_t LOW_32_BITS = ONE_Q32 - 1;

        // 1 セルの物質量の上限(µmol)。移す物質量を Q32 の 64bit で計算するため(0.125 m³ に 2147 mol = 大気の約 400 倍)
        constexpr uint64_t MAX_CELL_AMOUNT = (uint64_t)1 << 31;

        // 丸めの乱数の用途(FxHash64。R6)
        constexpr uint32_t GAS_ROUNDING_PURPOSE = 0x47610001u;

        // 面の片側(箱の中のセル・開いた境界の外・壁)
        enum class SideKind : uint8_t {
            Cell,
            Ghost,
            Wall,
        };

        struct FaceSide {
            SideKind kind = SideKind::Cell;
            uint32_t index = 0;  // Cell: セルの番号 / Ghost: 層 z
        };

        struct GasFace {
            FaceSide left;        // 軸のマイナス側
            FaceSide right;       // 軸のプラス側
            FaceSide leftOuter;   // left のさらにマイナス側(MUSCL の勾配)
            FaceSide rightOuter;  // right のさらにプラス側
            uint32_t axis = 0;
            uint32_t id = 0;        // 面の番号(乱数の丸めの ID。並べた順ではなく場所から決める。R6)
            int64_t flow = 0;       // 質量の流れ(mg/小刻み。左 → 右が正)
            int64_t impulse = 0;    // 面の圧力の力積(左のセルから引き、右のセルへ足す)
            uint64_t fraction = 0;  // 風上の中身のうち移す割合(Q32)

            // --- 重さ(縦の面だけ。基準との質量の差 × g を面の両側の半分ずつのセルで受ける)---
            int64_t weightLeft = 0;  // 左のセルの運動量 z から引く力積
            int64_t weightRight = 0;

            // --- 移す成分の物質量(µmol。MUSCL と乱数の丸めの後、風上のセルごとに中身を超えないよう抑えた値)---
            std::array<uint64_t, GAS_MAX_SPECIES> moved = {};
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
            const uint64_t amount = TotalAmount(config, cell);
            assert(amount < MAX_CELL_AMOUNT);

            GasDerived derived;
            derived.mass = CellMass(config, coefficients, cell);
            derived.inverseMass = derived.mass == 0 ? 0 : std::numeric_limits<uint64_t>::max() / derived.mass;
            derived.inverseAmount = amount == 0 ? 0 : std::numeric_limits<uint64_t>::max() / amount;
            derived.temperature = CellTemperature(config, cell);
            derived.pressure = CellPressure(config, cell, derived.temperature);

            return derived;
        }

        // --- 小刻みの部品 ---

        uint32_t AxisSize(const GasConfig& config, uint32_t axis) {
            return axis == 0 ? config.sizeX : (axis == 1 ? config.sizeY : config.sizeZ);
        }

        FaceSide BoundarySide(GasBoundary boundary, uint32_t z) {
            if (boundary == GasBoundary::Open)
                return {.kind = SideKind::Ghost, .index = z};

            return {.kind = SideKind::Wall, .index = 0};
        }

        std::array<uint32_t, GAS_AXES> CellCoordinate(const GasBox& box, uint32_t index) {
            const uint32_t sizeX = box.config.sizeX;
            const uint32_t sizeY = box.config.sizeY;

            return {index % sizeX, (index / sizeX) % sizeY, index / (sizeX * sizeY)};
        }

        // セル cell から軸 axis の座標を coordinate(箱の外なら −1 や size)に替えた所の側。周期的なら反対側へ回し、それ以外の外は境界
        FaceSide SideAt(const GasBox& box, std::array<uint32_t, GAS_AXES> cell, uint32_t axis, int64_t coordinate) {
            const int64_t size = AxisSize(box.config, axis);
            const GasBoundary boundary = box.config.boundary[axis];
            if (coordinate < 0 || coordinate >= size) {
                if (boundary != GasBoundary::Periodic)
                    return BoundarySide(boundary, cell[2]);

                coordinate = ((coordinate % size) + size) % size;
            }

            cell[axis] = (uint32_t)coordinate;
            return {.kind = SideKind::Cell, .index = box.Index(cell[0], cell[1], cell[2])};
        }

        // セル cell の軸 axis のマイナス側の面(high なら端のセルのプラス側の境界の面)。両側と、その外側の隣(MUSCL の勾配)
        GasFace MakeFace(const GasBox& box, std::array<uint32_t, GAS_AXES> cell, uint32_t axis, bool high) {
            const int64_t left = high ? (int64_t)cell[axis] : (int64_t)cell[axis] - 1;
            GasFace face;
            face.axis = axis;
            face.leftOuter = SideAt(box, cell, axis, left - 1);
            face.left = SideAt(box, cell, axis, left);
            face.right = SideAt(box, cell, axis, left + 1);
            face.rightOuter = SideAt(box, cell, axis, left + 2);
            face.id = ((box.Index(cell[0], cell[1], cell[2]) * GAS_AXES) + axis) * 2 + (high ? 1 : 0);

            return face;
        }

        // 面の一覧: どの面も 1 回だけ(セルのマイナス側の面 + 端のセルのプラス側の境界の面)
        std::vector<GasFace> EnumerateFaces(const GasBox& box) {
            const GasConfig& config = box.config;
            std::vector<GasFace> faces;
            faces.reserve((box.cells.size() * GAS_AXES) + 64);

            for (uint32_t index = 0; index < box.cells.size(); ++index) {
                const std::array<uint32_t, GAS_AXES> cell = CellCoordinate(box, index);
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                    faces.push_back(MakeFace(box, cell, axis, false));

                    const bool lastCell = cell[axis] + 1 == AxisSize(config, axis);
                    if (lastCell && config.boundary[axis] != GasBoundary::Periodic)
                        faces.push_back(MakeFace(box, cell, axis, true));
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

            const auto coefficient = (int64_t)box.coefficients.gravityImpulse;
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
            const fx::FxU128 product = {.hi = 0, .lo = magnitude};
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
            moved.amounts = face.moved;
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

        // --- 移す成分の物質量(G2。07 §2.2)---

        // 制限した勾配(a = 風上 − その外側、b = 風下 − 風上)。符号が違うか 0 なら 0(山と谷では延ばさず、新しい山と谷を作らない)。
        // minmod は絶対値の小さい方、MC(monotonized central)は minmod(2a, 2b, (a + b) ÷ 2)。どちらも TVD(07 §2.2)
        int64_t LimitedSlope(GasReconstruction reconstruction, int64_t a, int64_t b) {
            if (a == 0 || b == 0 || (a > 0) != (b > 0))
                return 0;

            const int64_t sign = a > 0 ? 1 : -1;
            const int64_t smaller = std::min(a * sign, b * sign);
            if (reconstruction == GasReconstruction::Minmod)
                return smaller * sign;

            const int64_t central = ((a * sign) + (b * sign)) / 2;
            return std::min(2 * smaller, central) * sign;
        }

        // 側の成分 s の割合(Q32。物質量 ÷ 全物質量。割り算は導く値の逆数 1 つ。ADR-0010)
        int64_t SpeciesRatio(const GasBox& box, const std::vector<GasCell>& cells,
                             const std::vector<GasDerived>& derived, FaceSide side, uint32_t s) {
            const uint64_t inverseAmount = SideDerived(box, derived, side).inverseAmount;

            return (int64_t)MulShiftU64(SideCell(box, cells, side).amounts[s], inverseAmount, 32);
        }

        // 風上のセルの割合から面へ延ばす差 ½(1 − ν)σ(Q32)。σ = 制限した勾配、ν = 出ていく割合。
        // (1 − ν)は小刻みの間に面を通る塊の平均にするため(van Leer の 1 段の MUSCL。ν ≤ 1 で新しい山と谷を作らない)。
        // 風上が開いた境界の外(一様)か、外側が壁なら 0(1 次の風上)
        int64_t HalfSlope(const GasBox& box, const std::vector<GasCell>& cells, const std::vector<GasDerived>& derived,
                          const GasFace& face, uint32_t s) {
            const bool forward = face.flow > 0;
            const FaceSide outer = forward ? face.leftOuter : face.rightOuter;
            const FaceSide donor = forward ? face.left : face.right;
            const FaceSide receiver = forward ? face.right : face.left;
            if (donor.kind != SideKind::Cell || outer.kind == SideKind::Wall || receiver.kind == SideKind::Wall)
                return 0;

            const int64_t donorRatio = SpeciesRatio(box, cells, derived, donor, s);
            const int64_t slope = LimitedSlope(box.config.reconstruction,
                                               donorRatio - SpeciesRatio(box, cells, derived, outer, s),
                                               SpeciesRatio(box, cells, derived, receiver, s) - donorRatio);

            return fx::FxMulShiftS64(slope, (int64_t)(ONE_Q32 - face.fraction), 33);
        }

        // 面で移す成分 s の物質量(Q32 の µmol)= 割合 × 風上の物質量 + 延ばした差 × 割合 × 風上の全物質量。
        // 延ばした割合は制限で風上の 0〜2 倍に収まるので負にならない(丸めの分は 0 で止める)
        uint64_t MovedAmountQ32(const GasBox& box, const std::vector<GasCell>& cells,
                                const std::vector<GasDerived>& derived, const GasFace& face, uint32_t s) {
            const GasCell& source = SideCell(box, cells, face.flow > 0 ? face.left : face.right);
            const uint64_t moved = source.amounts[s] * face.fraction;
            if (box.config.reconstruction == GasReconstruction::Upwind)
                return moved;

            const int64_t halfSlope = HalfSlope(box, cells, derived, face, s);
            const uint64_t total = TotalAmount(box.config, source) * face.fraction;
            const uint64_t correction = MulShiftU64(total, fx::FxAbsU64(halfSlope), 32);
            if (halfSlope >= 0)
                return moved + correction;

            return moved > correction ? moved - correction : 0;
        }

        // Q32 の物質量を整数へ。端数は決定的な乱数と比べて丸める(期待値が端数どおり。切り捨てだと割合 × 物質量 < 1 の微量の成分が
        // 永久に動かない〔D-428 の嘘〕。R6・D-431 と同じ考え)。乱数の丸めを切った設定なら切り捨て(G1)
        uint64_t RoundAmount(const GasBox& box, uint64_t amountQ32, uint64_t hash) {
            const uint64_t whole = amountQ32 >> 32;
            if (!box.config.stochasticRounding)
                return whole;

            return whole + ((hash >> 32) < (amountQ32 & LOW_32_BITS) ? 1 : 0);
        }

        // 面ごとに移す成分の物質量。乱数は(世界のシード, 小刻みの通し番号, 面の番号, 用途)+ 成分(R6。並べた順に依存しない)
        void ComputeMovedAmounts(const GasBox& box, const std::vector<GasCell>& cells,
                                 const std::vector<GasDerived>& derived, uint64_t roundingTick,
                                 std::vector<GasFace>& faces) {
            for (GasFace& face : faces) {
                if (face.fraction == 0)
                    continue;

                const uint64_t faceHash = fx::FxHash64(box.config.randomSeed, roundingTick, face.id,
                                                       GAS_ROUNDING_PURPOSE);
                for (uint32_t s = 0; s < box.config.speciesCount; ++s) {
                    const uint64_t amountQ32 = MovedAmountQ32(box, cells, derived, face, s);
                    face.moved[s] = RoundAmount(box, amountQ32, fx::FxHashCombine(faceHash, s));
                }
            }
        }

        // 風上のセルごとに、成分ごとの移す物質量の合計が中身を超えたら比例して縮める(切り捨て)。
        // 延ばした割合(風上の最大 2 倍)と丸め上げで、出ていく割合の合計が 1 に近いセルでは超えうる
        void LimitMovedAmounts(const GasBox& box, const std::vector<GasCell>& cells, std::vector<GasFace>& faces) {
            const uint32_t speciesCount = box.config.speciesCount;
            std::vector<std::array<uint64_t, GAS_MAX_SPECIES>> outgoing(box.cells.size());
            for (const GasFace& face : faces) {
                const FaceSide donor = face.flow > 0 ? face.left : face.right;
                if (face.fraction == 0 || donor.kind != SideKind::Cell)
                    continue;

                for (uint32_t s = 0; s < speciesCount; ++s)
                    outgoing[donor.index][s] += face.moved[s];
            }

            for (GasFace& face : faces) {
                const FaceSide donor = face.flow > 0 ? face.left : face.right;
                if (face.fraction == 0 || donor.kind != SideKind::Cell)
                    continue;

                const GasCell& source = cells[donor.index];
                for (uint32_t s = 0; s < speciesCount; ++s) {
                    const uint64_t total = outgoing[donor.index][s];
                    if (total <= source.amounts[s])
                        continue;

                    face.moved[s] = fx::FxDivU128By64(fx::FxMulU64Full(face.moved[s], source.amounts[s]), total)
                                        .quotient;
                }
            }
        }

        std::vector<GasDerived> DeriveAll(const GasBox& box) {
            std::vector<GasDerived> derived(box.cells.size());
            for (uint32_t index = 0; index < box.cells.size(); ++index)
                derived[index] = DeriveGasCell(box, box.cells[index], CellLayer(box, index));

            return derived;
        }

        void Substep(GasBox& box, const std::vector<GasFace>& layout, uint64_t roundingTick) {
            // --- (1) 導く値 ---
            const std::vector<GasDerived> derived = DeriveAll(box);

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

            // --- (4) 移す成分の物質量(MUSCL で面へ延ばした割合・乱数の丸め。G2)---
            ComputeMovedAmounts(box, forced, derived, roundingTick, faces);
            LimitMovedAmounts(box, forced, faces);

            // --- (5) 風上の中身(力を当てた後)を移す ---
            std::vector<GasCell> next = forced;
            for (const GasFace& face : faces)
                Transfer(box, forced, next, face);

            box.cells = std::move(next);
        }

    }  // namespace

    GasCoefficients MakeGasCoefficients(const GasConfig& config) {
        assert(config.substeps > 0 && config.soundSpeedMmPerS > 0);

        // 安定の条件: ν = c̃ Δt_小刻み ÷ Δx = c̃(mm/s)÷ (30000 × 小刻み)。圧力の拡散(陽解法の 3 次元の拡散)で ν ≤ 約 2/3、
        // 余裕を見て 0.6 まで(既定の 340 m/s・小刻み 24 で 0.47。T-0226 で 1% の乱れが育つ境を測った: 小刻み 16 で発散・18 で安定)
        assert((uint64_t)config.soundSpeedMmPerS * 10 <= (uint64_t)config.substeps * 30000 * 6);
        const uint64_t substeps = config.substeps;
        GasCoefficients coefficients;

        // mg/µmol = M(mg/mol)÷ 1e6
        for (uint32_t s = 0; s < config.speciesCount; ++s)
            coefficients.massPerAmount[s] = ((uint64_t)config.species[s].molarMass << 32) / MICRO;

        // 中心の流れ: F(mg)=(P_左 + P_右)÷ 2 × 2^-20 × Δt ÷ Δx、Δt ÷ Δx = 1 ÷ (30 × 小刻み)(s/m)
        coefficients.centralFlow = ((uint64_t)1 << 31) / (30 * substeps);

        // 圧力の拡散: F(mg)= Δp̃(µPa)× 1e-6 × A × Δt ÷ (4 c̃)× 1e6 = Δp̃ × 25 ÷ (24 × 小刻み × c̃(mm/s))
        // 素の Rusanov(÷ 2c̃)の半分。÷ 2c̃ だと 3 次元で ν ≤ 1/3(本物の音速で小刻み 34 以上)になり、1% の乱れが
        // 小刻み 32 でも育った(T-0226)。半分でも音の波の速さ・衝撃波の速さは理論どおり(gas_reference_test の管の試験)
        coefficients.pressureFlow = (25 * ((uint64_t)1 << 40)) / (24 * substeps * config.soundSpeedMmPerS);

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
            const auto mass = (int64_t)box.referenceDerived.back().mass;
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
            Substep(box, layout, (box.tick * box.config.substeps) + substep);

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
