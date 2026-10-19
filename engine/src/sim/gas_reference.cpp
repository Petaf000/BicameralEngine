// gas_reference.cpp — 気体の流れの CPU リファレンス G1・G2(gas_reference.h・07 §2.1・§2.2・ADR-0043)。
// 1 小刻み = (1) セルから導く値(質量・温度・圧力)→ (2) 面の力(圧力の力積・重さ)を当てる → (3) 力を当てた後の運動量で
// 面の質量の流れを出し、風上のセルごとに出ていく割合を合計 1 までに抑える → (4) 面ごとに移す成分の物質量を決める
// (MUSCL で面へ延ばした割合・乱数の丸め。G2)→ 風上のセルごとに合計が中身を超えないよう抑える → (5) 移す。
// どの段も前の段の結果を読み、次の配列へ整数の足し引きだけで書く(順番に依存しない。04 R2・R3)。
// 断熱(T-0234・07 §2.4): セルのエネルギーは全エネルギー(熱 + 化学 + 運動)。温度は運動エネルギーを引いた熱から導き、
// (5) で移す塊は風上の熱の分け前 + 押し出す仕事 n R T + 運動エネルギーの分け前を運ぶ(上へ動けば重さの仕事を失い、帳簿へ)。
// (3)〜(5) の上限と当てるは運ぶ計算の共通の土台の当てる段(common/transport_apply.hlsli の TxLimit・TxLimitAmount・TxPacket・
// TxApplyPacket。T-0765・ADR-0345)。帳簿は理由ごと(世界の端・壁の力積・重さ。T-0763)。
#include "sim/gas_reference.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <limits>

#include "common/fixed.hlsli"
#include "common/transport_apply.hlsli"
#include "sim/gas_internal.h"
#include "sim/gas_pressure_system.h"
#include "sim/world_physics.h"

namespace bicameral::sim {

    // 小刻みの部品(gas_tree.cpp も使う。宣言と定数は gas_internal.h)
    namespace gas_detail {

        namespace fx = bicameral::fx;

        // 面の片側と面のつながり(gas_reference.h。T-0720)
        using SideKind = GasSideKind;
        using FaceSide = GasFaceSide;

        // 面のつながり + 小刻みの中の作業の値
        struct GasFace : GasFaceLink {
            int64_t flow = 0;     // 質量の流れ(mg/小刻み。左 → 右が正)
            int64_t impulse = 0;  // 面の圧力の力積(左のセルから引き、右のセルへ足す)
            // 右のセルへ足す力積(面が一部だけ開いた時は左と違う。差は塞いだ物が受けて帳簿へ。T-0593。ふつうは impulse と同じ)
            int64_t impulseRight = 0;
            uint64_t fraction = 0;  // 風上の中身のうち移す割合(Q32)

            // --- 重さ(縦の面だけ。基準との質量の差 × g を面の両側の半分ずつのセルで受ける)---
            int64_t weightLeft = 0;  // 左のセルの運動量 z から引く力積
            int64_t weightRight = 0;

            // --- 移す成分の物質量(µmol。MUSCL と乱数の丸めの後、風上のセルごとに中身を超えないよう抑えた値)---
            GasPerSpecies<uint64_t> moved;
        };

        // 共有の芯(shaders/common/gas_flow.hlsli。GPU と同じ関数。T-0719)へ渡す見立ての値。係数の要らない部品(運動・移す塊のエネルギー・
        // 出ていく割合の換算)は config だけから(係数は 0)
        gas::GfParams FlowParams(const GasConfig& config, const GasCoefficients* coefficients, bool openness) {
            gas::GfParams params = {};
            if (coefficients != nullptr) {
                params.centralFlow = coefficients->centralFlow;
                params.pressureFlow = coefficients->pressureFlow;
                params.pressureImpulse = coefficients->pressureImpulse;
                params.gravityImpulse = coefficients->gravityImpulse;
                params.hydrostatic = coefficients->hydrostatic;
                params.gravityWork = coefficients->gravityWork;
                params.soundScale = coefficients->soundScale;
                params.soundTemperature = coefficients->soundTemperature;
            }

            params.gravity = config.gravity ? 1 : 0;
            params.reconstruction = (uint32_t)config.reconstruction;
            params.stochasticRounding = config.stochasticRounding ? 1 : 0;
            params.adiabatic = config.thermodynamics == GasThermodynamics::Adiabatic ? 1 : 0;
            params.openness = openness ? 1 : 0;

            return params;
        }

        gas::GfParams FlowParams(const GasBox& box) {
            return FlowParams(box.config, &box.coefficients, !box.openQ16.empty());
        }

        static_assert((uint32_t)GasReconstruction::Upwind == gas::GF_RECONSTRUCTION_UPWIND &&
                      (uint32_t)GasReconstruction::Minmod == gas::GF_RECONSTRUCTION_MINMOD &&
                      (uint32_t)GasReconstruction::MonotonizedCentral == gas::GF_RECONSTRUCTION_MC);
        static_assert(GAS_OPEN_FULL == gas::GF_OPEN_FULL &&
                      PRESSURE_PER_AMOUNT_TEMPERATURE_Q32 == gas::GF_PRESSURE_PER_AMOUNT_TEMPERATURE_Q32);

        // セルの見立て: 成分の和(物質量・熱容量・化学・質量)。coefficients が無ければ質量は 0
        gas::GfTotals CellTotals(const GasConfig& config, const GasCoefficients* coefficients, const GasCell& cell) {
            gas::GfTotals totals = gas::GfZeroTotals();
            for (uint32_t s = 0; s < config.speciesCount; ++s) {
                const GasSpecies& species = config.species[s];
                const uint64_t massPerAmount = coefficients == nullptr ? 0 : coefficients->massPerAmount[s];
                totals = gas::GfAddSpecies(totals, cell.amounts[s], species.heatCapacity, species.formationEnergy,
                                           massPerAmount);
            }

            return totals;
        }

        GasDerived ToGasDerived(const gas::GfDerived& flow) {
            return {.mass = flow.mass,
                    .inverseMass = flow.inverseMass,
                    .inverseAmount = flow.inverseAmount,
                    .temperature = flow.temperature,
                    .pressure = flow.pressure,
                    .scaledPressure = flow.scaledPressure};
        }

        uint64_t MulShiftU64(uint64_t a, uint64_t b, uint32_t shift) {
            return gas::GfMulShiftU64(a, b, shift);
        }

        uint64_t TotalAmount(const GasConfig& config, const GasCell& cell) {
            uint64_t total = 0;
            for (uint32_t s = 0; s < config.speciesCount; ++s)
                total += cell.amounts[s];

            return total;
        }

        // 熱容量(nJ/K)= Σ 物質量(µmol)× 比熱(mJ/(mol·K))
        uint64_t HeatCapacity(const GasConfig& config, const GasCell& cell) {
            return CellTotals(config, nullptr, cell).capacity;
        }

        // 化学のエネルギー(µJ)= Σ 物質量 × H0
        int64_t ChemicalEnergy(const GasConfig& config, const GasCell& cell) {
            return CellTotals(config, nullptr, cell).chemical;
        }

        uint64_t CellMass(const GasConfig& config, const GasCoefficients& coefficients, const GasCell& cell) {
            return CellTotals(config, &coefficients, cell).mass;
        }

        // 運動エネルギー(µJ。gas_flow.hlsli の GfKineticEnergy)。等温は 0
        int64_t KineticEnergy(const GasConfig& config, const GasCell& cell, uint64_t mass) {
            return gas::GfKineticEnergy(FlowParams(config, nullptr, false), cell.momentum[0], cell.momentum[1],
                                        cell.momentum[2], mass);
        }

        // 質量・温度・本物の圧力(p̃ は基準が要るので呼ぶ側で)
        GasDerived DeriveRaw(const GasConfig& config, const GasCoefficients& coefficients, const GasCell& cell) {
            return ToGasDerived(gas::GfDeriveRaw(FlowParams(config, &coefficients, false), cell.energy,
                                                 cell.momentum[0], cell.momentum[1], cell.momentum[2],
                                                 CellTotals(config, &coefficients, cell), GAS_OPEN_FULL));
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
            if (!box.cellLayers.empty())
                return box.cellLayers[index];

            return index / (box.config.sizeX * box.config.sizeY);
        }

        const GasCell& SideCell(const GasBox& box, const std::vector<GasCell>& cells, FaceSide side) {
            return side.kind == SideKind::Ghost ? box.ghost[side.index] : cells[side.index];
        }

        const GasDerived& SideDerived(const GasBox& box, const std::vector<GasDerived>& derived, FaceSide side) {
            return side.kind == SideKind::Ghost ? box.referenceDerived[side.index] : derived[side.index];
        }

        // セルの空き(Q16。T-0593)。空きを読まない箱・箱の外は全部
        uint32_t SideOpen(const GasBox& box, FaceSide side) {
            if (box.openQ16.empty() || side.kind != SideKind::Cell)
                return GAS_OPEN_FULL;

            return box.openQ16[side.index];
        }

        // 面の開き(Q16)= 両側の空きの小さい方
        uint32_t FaceOpen(const GasBox& box, const GasFace& face) {
            return std::min(SideOpen(box, face.left), SideOpen(box, face.right));
        }

        // value × numerator ÷ denominator(0 の向きへ切り捨て。投影も使う)
        int64_t ScaleSigned(int64_t value, uint64_t numerator, uint64_t denominator) {
            return gas::GfScaleTowardZero(value, numerator, denominator);
        }

        // 基準との質量の差(mg。gas_flow.hlsli の GfExcess)。開いた境界の外・壁は 0
        int64_t Excess(const GasBox& box, const std::vector<GasDerived>& derived, FaceSide side) {
            if (side.kind != SideKind::Cell)
                return 0;

            const uint64_t reference = box.referenceDerived[CellLayer(box, side.index)].mass;
            return gas::GfExcess(FlowParams(box), derived[side.index].mass, reference, SideOpen(box, side));
        }

        // 縦の面で重さが働く側の質量の差(横の面・重さの無い箱は 0。要らない所では数えない)
        int64_t VerticalExcess(const GasBox& box, const std::vector<GasDerived>& derived, const GasFace& face,
                               FaceSide side) {
            if (face.axis != 2 || !box.config.gravity)
                return 0;

            return Excess(box, derived, side);
        }

        int64_t HalfHydrostatic(const GasBox& box, int64_t excess) {
            return gas::GfHalfHydrostatic(FlowParams(box), excess);
        }

        // 圧力の拡散を起こす圧力の差(gas_flow.hlsli の GfDrivingPressure)
        int64_t DrivingPressure(const GasBox& box, const std::vector<GasDerived>& derived, const GasFace& face) {
            return gas::GfDrivingPressure(
                FlowParams(box), face.axis, SideDerived(box, derived, face.left).scaledPressure,
                SideDerived(box, derived, face.right).scaledPressure, VerticalExcess(box, derived, face, face.left),
                VerticalExcess(box, derived, face, face.right));
        }

        // 面の力: 圧力の力積と重さ(小刻みの初めの導く値だけから。gas_flow.hlsli の GfFaceWeight・GfMirrorImpulse・GfSharedImpulse)
        void ComputeFaceForceFull(const GasBox& box, const std::vector<GasDerived>& derived, GasFace& face) {
            const gas::GfParams params = FlowParams(box);
            const bool leftWall = face.left.kind == SideKind::Wall;
            const bool rightWall = face.right.kind == SideKind::Wall;
            const gas::GfWeights weights = gas::GfFaceWeight(params, face.axis, leftWall, rightWall,
                                                             VerticalExcess(box, derived, face, face.left),
                                                             VerticalExcess(box, derived, face, face.right));
            face.weightLeft = weights.left;
            face.weightRight = weights.right;

            // --- 壁: 壁の手前のセルの p̃ を鏡に映す(縦の壁は半セル分の静水圧で壁まで延ばす)---
            if (leftWall || rightWall) {
                const FaceSide inside = leftWall ? face.right : face.left;
                face.impulse = gas::GfMirrorImpulse(params, face.axis, derived[inside.index].scaledPressure,
                                                    VerticalExcess(box, derived, face, inside), leftWall);
                return;
            }

            face.impulse = gas::GfSharedImpulse(params, SideDerived(box, derived, face.left).scaledPressure,
                                                SideDerived(box, derived, face.right).scaledPressure);
        }

        // 一部だけ開いた面の 1 つの側が受ける力積(gas_flow.hlsli の GfPartialImpulse)。above = その側が面の上(+ 側)
        int64_t PartialImpulse(const GasBox& box, const std::vector<GasDerived>& derived, const GasFace& face,
                               FaceSide side, bool above, int64_t shared, uint32_t open) {
            return gas::GfPartialImpulse(FlowParams(box), face.axis, SideDerived(box, derived, side).scaledPressure,
                                         VerticalExcess(box, derived, face, side), above, shared, open);
        }

        // 面の力: 圧力の力積と重さ(小刻みの初めの導く値だけから)。
        // 空きを読む箱(T-0593): 一部だけ開いた面は両側が違う力積を受け(差は塞いだ物が受けて帳簿へ)、気体の無いセルは押さない
        void ComputeFaceForce(const GasBox& box, const std::vector<GasDerived>& derived, GasFace& face) {
            ComputeFaceForceFull(box, derived, face);
            face.impulseRight = face.impulse;
            if (box.openQ16.empty())
                return;

            const bool wall = face.left.kind == SideKind::Wall || face.right.kind == SideKind::Wall;
            const uint32_t open = FaceOpen(box, face);
            if (!wall && open != GAS_OPEN_FULL) {
                const int64_t shared = face.impulse;
                face.impulse = PartialImpulse(box, derived, face, face.left, false, shared, open);
                face.impulseRight = PartialImpulse(box, derived, face, face.right, true, shared, open);
            }

            if (face.left.kind == SideKind::Cell && derived[face.left.index].mass == 0)
                face.impulse = 0;

            if (face.right.kind == SideKind::Cell && derived[face.right.index].mass == 0)
                face.impulseRight = 0;
        }

        // 面の質量の流れ(力を当てた後の運動量 + 小刻みの初めの圧力。前進・後退の順〔forward-backward〕。gas_flow.hlsli の GfFaceFlow)。壁は 0
        void ComputeFaceFlow(const GasBox& box, const std::vector<GasDerived>& derived,
                             const std::vector<GasCell>& forced, GasFace& face) {
            if (face.left.kind == SideKind::Wall || face.right.kind == SideKind::Wall)
                return;

            face.flow = gas::GfFaceFlow(FlowParams(box), SideCell(box, forced, face.left).momentum[face.axis],
                                        SideCell(box, forced, face.right).momentum[face.axis], SideOpen(box, face.left),
                                        SideOpen(box, face.right), DrivingPressure(box, derived, face));
        }

        // 風上のセルの中身のうち移す割合(Q32。抑える前)
        uint64_t RawFraction(const GasBox& box, const std::vector<GasDerived>& derived, const GasFace& face) {
            const FaceSide donor = face.flow > 0 ? face.left : face.right;
            return gas::GfRawFraction(face.flow, SideDerived(box, derived, donor).inverseMass);
        }

        // a ÷ b を最も近い整数に(ちょうど半分は 0 から遠い側へ)。b > 0
        int64_t DivideNearest(int64_t a, uint64_t b) {
            return gas::GfDivideNearest(a, b);
        }

        // 移す塊のエネルギー(µJ。丸める前。gas_flow.hlsli の GfMovedEnergy: 熱の分け前 + 化学 + 断熱では押し出す仕事 n R T と運動エネルギーの分け前)
        int64_t MovedEnergy(const GasConfig& config, const GasCell& source, const GasDerived& sourceDerived,
                            const GasCell& moved, uint64_t fraction) {
            return gas::GfMovedEnergy(FlowParams(config, nullptr, false), source.energy,
                                      CellTotals(config, nullptr, source),
                                      KineticEnergy(config, source, sourceDerived.mass), sourceDerived.temperature,
                                      CellTotals(config, nullptr, moved), fraction);
        }

        // 重さの仕事(µJ。gas_flow.hlsli の GfGravityWork)
        int64_t GravityWork(const GasBox& box, const GasFace& face, const GasCell& moved) {
            if (face.axis != 2 || !box.config.gravity || box.config.thermodynamics != GasThermodynamics::Adiabatic)
                return 0;

            return gas::GfGravityWork(FlowParams(box), face.axis, face.flow,
                                      CellMass(box.config, box.coefficients, moved));
        }

        // 押し出す仕事 n R T(µJ。GfMovedEnergy と同じ式。温度は風上の小刻みの初め)
        int64_t PushWork(const GasConfig& config, const GasDerived& sourceDerived, const GasCell& moved) {
            if (config.thermodynamics != GasThermodynamics::Adiabatic)
                return 0;

            const fx::FxU128 amountTemperature = fx::FxMulU64Full(CellTotals(config, nullptr, moved).amount,
                                                                  sourceDerived.temperature);
            return (int64_t)MulShiftU64(amountTemperature.lo, gas::GF_PRESSURE_PER_AMOUNT_TEMPERATURE_Q32, 35);
        }

        // 調べ(T-0787 の案 2): 押し出す仕事を風上のセルの倍率(分子 ÷ 分母)で数え直した塊のエネルギー
        int64_t ScaledPushEnergy(const GasBox& box, const GasHeatProbe& probe, const GasDerived& sourceDerived,
                                 uint32_t donor, const GasCell& moved, int64_t energy) {
            const int64_t push = PushWork(box.config, sourceDerived, moved);
            const int64_t numerator = probe.pushNumerator[donor];
            const int64_t denominator = probe.pushDenominator[donor];
            if (push == 0 || denominator <= 0)
                return energy;

            if (numerator <= 0)
                return energy - push;

            const fx::FxDivResult scaled = fx::FxDivU128By64(fx::FxMulU64Full((uint64_t)push, (uint64_t)numerator),
                                                             (uint64_t)denominator);
            const auto kept = (int64_t)(scaled.quotient + (scaled.remainder * 2 >= (uint64_t)denominator ? 1 : 0));
            return energy - push + kept;
        }

        // --- 当てる段の世界(運ぶ計算の共通の土台 common/transport_apply.hlsli。T-0765・ADR-0345)---
        // 枠 = 側の種類(セル・開いた境界の外)・番号 = セルの番号(外は層)。上限はセルごとの倍率(外は無限の溜め = 縮めない)、
        // 包みは次の値(next)へ当て、外の側は帳簿の「世界の端」へ(外から入った量が正)
        constexpr uint32_t APPLY_CELL = 0;
        constexpr uint32_t APPLY_OUTSIDE = 1;

        uint32_t ApplySlot(FaceSide side) {
            return side.kind == SideKind::Cell ? APPLY_CELL : APPLY_OUTSIDE;
        }

        struct GasApplyWorld {
            GasLedger* ledger = nullptr;
            std::vector<GasCell>* next = nullptr;
            const std::vector<multires::TxLimit>* limits = nullptr;
            bool outgoingOnly = false;  // 調べの口(T-0787): 上流から引く分だけ当てる

            [[nodiscard]] multires::TxLimit Limit(uint32_t slot, uint32_t index) const {
                return slot == APPLY_CELL ? (*limits)[index] : multires::TxNoLimit();
            }

            // 包みを引く(incoming = 0。上流)・足す(1。下流)。包みの物質は箱の欄の番号・運ぶ欄 0〜2 は運動量
            void AddPacket(uint32_t slot, uint32_t index, const multires::TxPacket& packet, uint32_t incoming) const {
                if (outgoingOnly && (incoming != 0 || slot != APPLY_CELL))
                    return;

                const int64_t sign = incoming != 0 ? 1 : -1;
                if (slot == APPLY_CELL) {
                    GasCell& cell = (*next)[index];
                    for (uint32_t i = 0; i < packet.speciesCount; ++i) {
                        uint64_t& amount = cell.amounts[packet.species[i]];
                        assert(incoming != 0 || amount >= packet.amounts[i]);
                        amount = incoming != 0 ? amount + packet.amounts[i] : amount - packet.amounts[i];
                    }

                    cell.energy += sign * packet.energy;
                    for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                        cell.momentum[axis] += sign * packet.carried[axis];

                    return;
                }

                // 開いた境界の外: 外が受けた(incoming)= 箱から出た(帳簿は負)
                for (uint32_t i = 0; i < packet.speciesCount; ++i)
                    ledger->amounts[packet.species[i]] -= sign * (int64_t)packet.amounts[i];

                ledger->energy -= sign * packet.energy;
                ledger->edgeEnergy -= sign * packet.energy;
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                    ledger->momentum[axis] -= sign * packet.carried[axis];
                    ledger->edgeMomentum[axis] -= sign * packet.carried[axis];
                }
            }

            // 重さの仕事 work(mJ。上へ動いた塊が失う分)を下流の側で引き、帳簿の「重さの仕事」へ。下流が外なら、外が受ける量が減る
            // (= 世界の端の出が減る)。気体の種類だけが持つ当て方(位置のエネルギーを数えない)
            void AddGravityWork(uint32_t slot, uint32_t index, int64_t work) const {
                if (work == 0 || outgoingOnly)
                    return;

                ledger->energy -= work;
                ledger->gravityEnergy -= work;
                if (slot == APPLY_CELL) {
                    (*next)[index].energy -= work;
                    return;
                }

                ledger->energy += work;
                ledger->edgeEnergy += work;
            }
        };

        // 中身の一部を移す(風上 → 風下)。塊を包み(TxPacket: 成分の物質量・引く側のエネルギー・運ぶ欄 = 運動量)にして当てる段で当て
        // (TxApplyPacket)、上へ動いた塊は下流で重さの仕事を失う(丸めは最も近い整数で偏りなし)。包みの物質の数を超える成分は包みを分ける
        // (箱は物質の数に上限なし。D-401。エネルギーと運動量は最初の包み)
        void Transfer(const GasBox& box, const std::vector<GasCell>& forced, const std::vector<GasDerived>& derived,
                      const GasApplyWorld& world, const GasFace& face) {
            if (face.fraction == 0)
                return;

            const bool forward = face.flow > 0;
            const FaceSide donor = forward ? face.left : face.right;
            const FaceSide receiver = forward ? face.right : face.left;
            const GasCell& source = SideCell(box, forced, donor);

            GasCell moved;
            moved.amounts = face.moved;
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                moved.momentum[axis] = gas::GfMovedMomentum(source.momentum[axis], face.fraction);

            int64_t energy = MovedEnergy(box.config, source, SideDerived(box, derived, donor), moved, face.fraction);
            if (box.heatProbe != nullptr && box.heatProbe->pushForm != 0 && donor.kind == SideKind::Cell)
                energy = ScaledPushEnergy(box, *box.heatProbe, SideDerived(box, derived, donor), donor.index, moved,
                                          energy);

            const int64_t sent = DivideNearest(energy, 1000);
            const int64_t received = DivideNearest(energy - GravityWork(box, face, moved), 1000);

            // --- 包み(上流 → 下流)---
            const uint32_t fromSlot = ApplySlot(donor);
            const uint32_t toSlot = ApplySlot(receiver);
            multires::TxPacket packet = multires::TxMakePacket();
            packet.energy = sent;
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                packet.carried[axis] = moved.momentum[axis];

            for (uint32_t s = 0; s < box.config.speciesCount; ++s) {
                if (packet.speciesCount == multires::TX_PACKET_SPECIES) {
                    (void)multires::TxApplyPacket(world, fromSlot, donor.index, toSlot, receiver.index, packet);
                    packet = multires::TxMakePacket();
                }

                packet = multires::TxPacketWithSpecies(packet, s, moved.amounts[s]);
            }

            (void)multires::TxApplyPacket(world, fromSlot, donor.index, toSlot, receiver.index, packet);
            world.AddGravityWork(toSlot, receiver.index, sent - received);
        }

        // 面の力積: 左のセルから引き、右のセルへ足す。箱の外(壁・開いた境界)の側は帳簿へ(理由ごと。T-0763)
        void ApplyImpulse(GasBox& box, std::vector<GasCell>& next, const GasFace& face) {
            GasLedger& ledger = box.ledger;

            // --- 重さ(箱の外から働く力なので帳簿の「重さの力積」)---
            if (face.left.kind == SideKind::Cell && face.weightLeft != 0) {
                next[face.left.index].momentum[2] -= face.weightLeft;
                ledger.momentum[2] -= face.weightLeft;
                ledger.gravityMomentum[2] -= face.weightLeft;
            }

            if (face.right.kind == SideKind::Cell && face.weightRight != 0) {
                next[face.right.index].momentum[2] -= face.weightRight;
                ledger.momentum[2] -= face.weightRight;
                ledger.gravityMomentum[2] -= face.weightRight;
            }

            if (face.impulse == 0 && face.impulseRight == 0)
                return;

            // 帳簿 = セルが受けた力積の和(両側が同じなら 0)。開いた境界の外の大気が押した分は「世界の端」、壁・一部だけ開いた面の
            // 塞いだ物(T-0593)・気体の無い側が受けた分は「壁の力積」
            int64_t received = 0;
            if (face.left.kind == SideKind::Cell) {
                next[face.left.index].momentum[face.axis] -= face.impulse;
                received -= face.impulse;
            }

            if (face.right.kind == SideKind::Cell) {
                next[face.right.index].momentum[face.axis] += face.impulseRight;
                received += face.impulseRight;
            }

            const bool edge = face.left.kind == SideKind::Ghost || face.right.kind == SideKind::Ghost;
            ledger.momentum[face.axis] += received;
            (edge ? ledger.edgeMomentum : ledger.wallMomentum)[face.axis] += received;
        }

        // 出ていく割合の合計の、引く側の熱を負にしない換算(T-0646・ADR-0286。gas_flow.hlsli の GfEffectiveOutflow)。
        // 木(gas_tree.cpp の LimitOutflow)・GPU(箱と木の段)も同じ関数(T-0647)
        uint64_t EffectiveOutflow(const GasConfig& config, const GasCell& cell, uint64_t outflow) {
            if (outflow == 0)
                return 0;

            return gas::GfEffectiveOutflow(FlowParams(config, nullptr, false), CellTotals(config, nullptr, cell),
                                           outflow);
        }

        // 出ていく割合の合計(EffectiveOutflow で換算)が 1 を超えるセルは、その面の割合を比例して縮める(07「共通の形」。負の中身と
        // 物質の無いエネルギーを作らない)。cuts が null でなければ、縮めた質量(mg。上流のセルに残った分 = 正・下流に届かなかった分 = 負)を
        // セルの順に書く(投影の外側の反復が食い違いから除く。T-0722・ADR-0346)
        void LimitOutflow(const GasBox& box, const std::vector<GasDerived>& derived, std::vector<GasFace>& faces,
                          std::vector<int64_t>* cuts = nullptr) {
            std::vector<uint64_t> outflow(box.cells.size(), 0);
            for (GasFace& face : faces) {
                face.fraction = RawFraction(box, derived, face);
                const FaceSide donor = face.flow > 0 ? face.left : face.right;
                if (face.fraction != 0 && donor.kind == SideKind::Cell)
                    outflow[donor.index] += face.fraction;
            }

            // 当てる段の上限(T-0765): 出る側の倍率 = min(1, 1 ÷ 換算した合計)・入る側は縮めない(気体は器で切らない。圧縮できる)。
            // 外(開いた境界)から入る面は縮めない。式は gas_flow.hlsli の GfLimitFraction と同じ切り捨て 1 回(GPU の段とビット一致)
            std::vector<multires::TxLimit> limits(box.cells.size());
            for (uint32_t index = 0; index < box.cells.size(); ++index) {
                limits[index] = multires::TxMakeLimit(ONE_Q32,
                                                      EffectiveOutflow(box.config, box.cells[index], outflow[index]),
                                                      multires::TX_ROOM_UNLIMITED, 0);
            }

            if (cuts != nullptr)
                cuts->assign(box.cells.size(), 0);

            const GasApplyWorld world{.limits = &limits};
            for (GasFace& face : faces) {
                const FaceSide donor = face.flow > 0 ? face.left : face.right;
                const FaceSide receiver = face.flow > 0 ? face.right : face.left;
                const uint64_t raw = face.fraction;
                face.fraction = multires::TxLimitAmount(world, ApplySlot(donor), donor.index, ApplySlot(receiver),
                                                        receiver.index, raw);
                if (cuts == nullptr || raw == face.fraction || donor.kind != SideKind::Cell)
                    continue;

                const auto mass = (int64_t)MulShiftU64(raw - face.fraction, derived[donor.index].mass, 32);
                (*cuts)[donor.index] += mass;
                if (receiver.kind == SideKind::Cell)
                    (*cuts)[receiver.index] -= mass;
            }
        }

        // --- 移す成分の物質量(G2。07 §2.2。式は gas_flow.hlsli)---

        // 制限した勾配(gas_flow.hlsli の GfLimitedSlope。木も使う)
        int64_t LimitedSlope(GasReconstruction reconstruction, int64_t a, int64_t b) {
            gas::GfParams params = {};
            params.reconstruction = (uint32_t)reconstruction;

            return gas::GfLimitedSlope(params, a, b);
        }

        // 側の成分 s の割合(Q32)
        int64_t SpeciesRatio(const GasBox& box, const std::vector<GasCell>& cells,
                             const std::vector<GasDerived>& derived, FaceSide side, uint32_t s) {
            return gas::GfSpeciesRatio(SideCell(box, cells, side).amounts[s],
                                       SideDerived(box, derived, side).inverseAmount);
        }

        // 風上のセルの割合から面へ延ばす差 ½(1 − ν)σ(Q32。gas_flow.hlsli の GfHalfSlope)。
        // 1 次の風上・風上が開いた境界の外(一様)・外側か風下が壁なら 0
        int64_t HalfSlope(const GasBox& box, const std::vector<GasCell>& cells, const std::vector<GasDerived>& derived,
                          const GasFace& face, uint32_t s) {
            const bool forward = face.flow > 0;
            const FaceSide outer = forward ? face.leftOuter : face.rightOuter;
            const FaceSide donor = forward ? face.left : face.right;
            const FaceSide receiver = forward ? face.right : face.left;
            if (box.config.reconstruction == GasReconstruction::Upwind || donor.kind != SideKind::Cell ||
                outer.kind == SideKind::Wall || receiver.kind == SideKind::Wall)
                return 0;

            return gas::GfHalfSlope(FlowParams(box), SpeciesRatio(box, cells, derived, outer, s),
                                    SpeciesRatio(box, cells, derived, donor, s),
                                    SpeciesRatio(box, cells, derived, receiver, s), face.fraction);
        }

        // 成分 s の丸めの鍵(既定は欄の番号。世界のセルに載せる時は物質 ID。T-0591)
        uint64_t RoundingKey(const GasConfig& config, uint32_t s) {
            const uint32_t key = config.species[s].roundingKey;
            return key == GAS_ROUNDING_KEY_SLOT ? s : key;
        }

        // 面ごとに移す成分の物質量(MUSCL で面へ延ばした割合 × 風上の中身・乱数の丸め。gas_flow.hlsli の GfMovedAmountQ32・GfRoundAmount)。
        // 乱数は(世界のシード, 小刻みの通し番号, 面の番号, 用途)+ 成分の鍵(R6。並べた順に依存しない)
        void ComputeMovedAmounts(const GasBox& box, const std::vector<GasCell>& cells,
                                 const std::vector<GasDerived>& derived, uint64_t roundingTick,
                                 std::vector<GasFace>& faces) {
            const gas::GfParams params = FlowParams(box);
            for (GasFace& face : faces) {
                if (face.fraction == 0)
                    continue;

                const GasCell& source = SideCell(box, cells, face.flow > 0 ? face.left : face.right);
                const uint64_t sourceTotal = TotalAmount(box.config, source);
                const uint64_t faceHash = gas::GfFaceHash(box.config.randomSeed, roundingTick, face.id);
                for (uint32_t s = 0; s < box.config.speciesCount; ++s) {
                    const uint64_t amountQ32 = gas::GfMovedAmountQ32(
                        params, source.amounts[s], sourceTotal, face.fraction, HalfSlope(box, cells, derived, face, s));
                    face.moved[s] = gas::GfRoundAmount(params, amountQ32,
                                                       fx::FxHashCombine(faceHash, RoundingKey(box.config, s)));
                }
            }
        }

        // 風上のセルごとに、成分ごとの移す物質量の合計が中身を超えたら比例して縮める(切り捨て。gas_flow.hlsli の GfLimitMovedAmount)。
        // 延ばした割合(風上の最大 2 倍)と丸め上げで、出ていく割合の合計が 1 に近いセルでは超えうる
        void LimitMovedAmounts(const GasBox& box, const std::vector<GasCell>& cells, std::vector<GasFace>& faces) {
            const uint32_t speciesCount = box.config.speciesCount;
            std::vector<GasPerSpecies<uint64_t>> outgoing(box.cells.size());
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

                // 列(成分)ごとの当てる段の上限(T-0765。gas_flow.hlsli の GfLimitMovedAmount と同じ切り捨て 1 回)
                const GasCell& source = cells[donor.index];
                for (uint32_t s = 0; s < speciesCount; ++s) {
                    const multires::TxLimit limit = multires::TxMakeLimit(source.amounts[s], outgoing[donor.index][s],
                                                                          multires::TX_ROOM_UNLIMITED, 0);
                    face.moved[s] = multires::TxLimitColumn(limit, multires::TxNoLimit(), face.moved[s]);
                }
            }
        }

        // セルの導く値(層 z の基準。空きが一部のセルは本物の圧力を空きの体積で読む。gas_flow.hlsli の GfDerive。T-0593)
        GasDerived DeriveCell(const GasBox& box, const GasCell& cell, uint32_t z, uint32_t open) {
            return ToGasDerived(gas::GfDerive(FlowParams(box), cell.energy, cell.momentum[0], cell.momentum[1],
                                              cell.momentum[2], CellTotals(box.config, &box.coefficients, cell),
                                              box.referenceDerived[z].pressure, open));
        }

        std::vector<GasDerived> DeriveAll(const GasBox& box) {
            std::vector<GasDerived> derived(box.cells.size());
            for (uint32_t index = 0; index < box.cells.size(); ++index)
                derived[index] = DeriveCell(box, box.cells[index], CellLayer(box, index),
                                            SideOpen(box, {.index = index}));

            return derived;
        }

        // セルの熱(µJ)= エネルギー − 化学 − 運動
        int64_t CellHeat(const GasBox& box, const GasCell& cell) {
            const uint64_t mass = CellMass(box.config, box.coefficients, cell);
            return (cell.energy * 1000) - ChemicalEnergy(box.config, cell) - KineticEnergy(box.config, cell, mass);
        }

        // --- 当てる段の上限の熱の列(T-0787・ADR-0355。D-508 の安全網。設計 20 §2.4 ②「量・エネルギー〔押し出す仕事込み〕の列ごとの最小」)---
        // 風上のセルは、自分の熱(力を当てた後)− 余裕より多くの熱を塊で持ち出せない(式は gas_flow.hlsli の GfMovedHeatPart・GfHeatRoom。
        // 塊のエネルギーは Transfer と同じ MovedEnergy)。超えるセルは出ていく面の割合と成分の量を同じ倍率で縮める(当てる段の TxMakeLimit・
        // TxFaceScale・TxScaleAmount。入る側は縮めない)。物質の量の上限(LimitOutflow・LimitMovedAmounts)の後に掛ける。
        // 根(T-0787 の測り): 強い爆発の 1 小刻みの終わりに熱が負のセルは、押し出す仕事を小刻みの初めの温度で数えて自分の熱より多く持ち出していた。
        // 空気の箱の 5 倍・1500 K では届かない。燃える木箱の試験(約 440 気圧の爆発)では力の後の熱が一時負のセルの出ていく面を 0 に縮める。
        // 断熱のときだけ。縮めた面の数を返す
        uint32_t LimitMovedEnergy(const GasBox& box, const std::vector<GasCell>& forced,
                                  const std::vector<GasDerived>& derived, std::vector<GasFace>& faces) {
            if (box.config.thermodynamics != GasThermodynamics::Adiabatic)
                return 0;

            const auto count = (uint32_t)box.cells.size();
            std::vector<uint64_t> outgoing(count, 0);
            for (const GasFace& face : faces) {
                const FaceSide donor = face.flow > 0 ? face.left : face.right;
                if (face.fraction == 0 || donor.kind != SideKind::Cell)
                    continue;

                const GasCell& source = forced[donor.index];
                const GasDerived& sourceDerived = derived[donor.index];
                GasCell moved;
                moved.amounts = face.moved;
                const int64_t energy = MovedEnergy(box.config, source, sourceDerived, moved, face.fraction);
                outgoing[donor.index] += gas::GfMovedHeatPart(energy, ChemicalEnergy(box.config, moved),
                                                              KineticEnergy(box.config, source, sourceDerived.mass),
                                                              face.fraction);
            }

            std::vector<multires::TxLimit> limits(count, multires::TxNoLimit());
            bool any = false;
            for (uint32_t index = 0; index < count; ++index) {
                if (outgoing[index] == 0)
                    continue;

                limits[index] = multires::TxMakeLimit(gas::GfHeatRoom(CellHeat(box, forced[index])), outgoing[index],
                                                      multires::TX_ROOM_UNLIMITED, 0);
                any = any || !multires::TxScaleIsOne(limits[index].outScale);
            }

            if (!any)
                return 0;

            uint32_t limited = 0;
            for (GasFace& face : faces) {
                const FaceSide donor = face.flow > 0 ? face.left : face.right;
                if (face.fraction == 0 || donor.kind != SideKind::Cell)
                    continue;

                const multires::TxScale scale = multires::TxFaceScale(limits[donor.index], multires::TxNoLimit());
                if (multires::TxScaleIsOne(scale))
                    continue;

                face.fraction = multires::TxScaleAmount(face.fraction, scale);
                for (uint32_t s = 0; s < box.config.speciesCount; ++s)
                    face.moved[s] = multires::TxScaleAmount(face.moved[s], scale);

                ++limited;
            }

            return limited;
        }

        // 調べの口(T-0787): 熱が負になったセルをどこで負になったかで数える
        void ProbeHeat(GasBox& box, const std::vector<GasDerived>& derived, const std::vector<GasCell>& forced,
                       const std::vector<GasFace>& faces, const std::vector<GasCell>& next) {
            GasHeatProbe& probe = *box.heatProbe;
            ++probe.substeps;
            std::vector<GasCell> outOnly = forced;
            GasLedger unused;
            const GasApplyWorld world{.ledger = &unused, .next = &outOnly, .outgoingOnly = true};
            for (const GasFace& face : faces)
                Transfer(box, forced, derived, world, face);

            // 小刻みの終わりに熱が負のセルだけ、最初に負になった所で数える(途中だけ負で終わりに戻るセルは transient)
            for (uint32_t index = 0; index < box.cells.size(); ++index) {
                const int64_t afterForce = CellHeat(box, forced[index]);
                const int64_t afterOut = CellHeat(box, outOnly[index]);
                const int64_t after = CellHeat(box, next[index]);
                if (after >= 0) {
                    if (afterForce < 0 || afterOut < 0)
                        ++probe.transientCells;

                    continue;
                }

                if (afterForce < 0) {
                    ++probe.impulseCells;
                    probe.worstImpulse = std::min(probe.worstImpulse, after);
                } else if (afterOut < 0) {
                    ++probe.outflowCells;
                    probe.worstOutflow = std::min(probe.worstOutflow, after);
                    if (DeriveCell(box, forced[index], CellLayer(box, index), SideOpen(box, {.index = index}))
                            .temperature < derived[index].temperature)
                        ++probe.coolerAfterForce;
                } else {
                    ++probe.otherCells;
                    probe.worstOther = std::min(probe.worstOther, after);
                }
            }
        }

        // 調べ(T-0787 の案 2): 風上のセルごとに、出ていく塊の熱の分け前の和 S と押し出す仕事の和 W(初めの温度)を集め、
        // 残る熱 A = 熱(力の後)− S から倍率を閉じた形で出す。終わりの温度 ÷ 初めの温度 = 1 − (W ÷ A) × x(x = 数える温度 ÷ 初めの温度)なので
        // 平均(台形): x = 2A ÷ (2A + W)・終わり: x = A ÷ (A + W)(残る熱 = A² ÷ (A + W) ≥ 0)
        void PreparePushForm(GasBox& box, const std::vector<GasCell>& forced, const std::vector<GasDerived>& derived,
                             const std::vector<GasFace>& faces) {
            GasHeatProbe& probe = *box.heatProbe;
            const auto count = (uint32_t)box.cells.size();
            std::vector<int64_t> shares(count, 0);
            std::vector<int64_t> pushes(count, 0);
            for (const GasFace& face : faces) {
                const FaceSide donor = face.flow > 0 ? face.left : face.right;
                if (face.fraction == 0 || donor.kind != SideKind::Cell)
                    continue;

                const GasCell& source = forced[donor.index];
                const GasDerived& sourceDerived = derived[donor.index];
                GasCell moved;
                moved.amounts = face.moved;
                const int64_t energy = MovedEnergy(box.config, source, sourceDerived, moved, face.fraction);
                const int64_t push = PushWork(box.config, sourceDerived, moved);
                const int64_t kineticShare = (int64_t)MulShiftU64(
                    (uint64_t)std::max<int64_t>(KineticEnergy(box.config, source, sourceDerived.mass), 0),
                    face.fraction, 32);
                shares[donor.index] += energy - ChemicalEnergy(box.config, moved) - kineticShare - push;
                pushes[donor.index] += push;
            }

            probe.pushNumerator.assign(count, 0);
            probe.pushDenominator.assign(count, 0);
            for (uint32_t index = 0; index < count; ++index) {
                if (pushes[index] == 0)
                    continue;

                const int64_t remaining = CellHeat(box, forced[index]) - shares[index];
                const int64_t weight = probe.pushForm == 1 ? 2 : 1;
                probe.pushNumerator[index] = remaining > 0 ? remaining * weight : 0;
                probe.pushDenominator[index] = (remaining > 0 ? remaining * weight : 0) + pushes[index];
            }
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
            if (box.limitMovedHeat)  // 熱の列(T-0787・ADR-0355。既定で入れる。GasBox::limitMovedHeat)
                box.energyLimitedFaces += LimitMovedEnergy(box, forced, derived, faces);

            if (box.heatProbe != nullptr && box.heatProbe->pushForm != 0)
                PreparePushForm(box, forced, derived, faces);

            // --- (5) 風上の中身(力を当てた後)を移す ---
            std::vector<GasCell> next = forced;
            const GasApplyWorld world{.ledger = &box.ledger, .next = &next};
            for (const GasFace& face : faces)
                Transfer(box, forced, derived, world, face);

            if (box.heatProbe != nullptr)
                ProbeHeat(box, derived, forced, faces, next);

            box.cells = std::move(next);
        }

        // --- 粘り(静まる性質。T-0758・D-516。小刻みの後に 1 刻みに 1 回。式は gas_flow.hlsli)---

        bool HasViscosity(const GasConfig& config) {
            if (config.eddyViscosityMm2PerS != 0)
                return true;

            for (uint32_t s = 0; s < config.speciesCount; ++s) {
                if (config.species[s].viscosityMicroPascalSeconds != 0)
                    return true;
            }

            return false;
        }

        // セルの物質の粘りの動粘性(mm²/s。成分の物質量で重みを付けた μ ÷ 空きの体積の密度)
        uint64_t CellMolecularViscosity(const GasBox& box, const GasCell& cell, uint64_t mass, uint32_t open) {
            uint64_t weighted = 0;
            uint64_t amount = 0;
            for (uint32_t s = 0; s < box.config.speciesCount; ++s) {
                weighted += cell.amounts[s] * box.config.species[s].viscosityMicroPascalSeconds;
                amount += cell.amounts[s];
            }

            return gas::GfMolecularViscosity(weighted, amount, mass, open);
        }

        // 粘りの面の係数 κ = ν Δt ÷ Δx² をセルのレベルに(× 4^k。T-0765)。gas_flow.hlsli の GfViscosityQ32 は 0.5 m のセルの式なので、
        // ν を 4^k 倍して渡す(上限 1/8 は GfViscosityQ32 の頭打ちのまま)。レベル 0 はそのまま
        uint64_t LevelViscosity(uint64_t viscosity, int32_t level) {
            if (level <= 0)
                return viscosity >> (2 * (uint32_t)(-level));

            return std::min<uint64_t>(viscosity << (2 * (uint32_t)level), gas::GF_VISCOSITY_LIMIT_MM2_PER_S);
        }

        // 面ごとに運動量を分け合う(Jacobi: 刻みの小刻みの後の値だけから決め、整数で足し引き)。運動量は保存し、減った運動のエネルギーは両側の熱へ。
        // 壁・開いた境界の外とは分け合わない(滑る面。運動量を外へ出さない)。面の開きの割合だけ効く
        void ApplyViscosity(GasBox& box, const std::vector<GasFace>& layout) {
            if (!HasViscosity(box.config))
                return;

            const gas::GfParams params = FlowParams(box);
            const auto count = (uint32_t)box.cells.size();
            std::vector<uint64_t> mass(count, 0);
            std::vector<uint64_t> viscosity(count, 0);
            for (uint32_t index = 0; index < count; ++index) {
                mass[index] = CellMass(box.config, box.coefficients, box.cells[index]);
                viscosity[index] = CellMolecularViscosity(box, box.cells[index], mass[index],
                                                          SideOpen(box, {.index = index}));
            }

            std::vector<GasCell> next = box.cells;
            for (const GasFace& face : layout) {
                if (face.left.kind != SideKind::Cell || face.right.kind != SideKind::Cell)
                    continue;

                const uint32_t open = FaceOpen(box, face);
                if (open == 0)
                    continue;

                const uint32_t a = face.left.index;
                const uint32_t b = face.right.index;
                const uint64_t total = box.config.eddyViscosityMm2PerS + ((viscosity[a] + viscosity[b]) / 2);
                const uint64_t coefficient = (gas::GfViscosityQ32(LevelViscosity(total, box.config.cellLevel)) *
                                              open) >>
                                             16;
                int64_t energy = 0;
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                    const int64_t pa = box.cells[a].momentum[axis];
                    const int64_t pb = box.cells[b].momentum[axis];
                    const int64_t moved = gas::GfViscousMomentum(pa, mass[a], pb, mass[b], coefficient);
                    next[a].momentum[axis] -= moved;
                    next[b].momentum[axis] += moved;
                    energy += gas::GfViscousEnergy(params, moved, pa, mass[a], pb, mass[b]);
                }

                next[a].energy -= energy;
                next[b].energy += energy;
            }

            box.cells = std::move(next);
        }

    }  // namespace gas_detail

    using namespace gas_detail;  // 下の公開の関数から小刻みの部品を使う

    uint32_t GasSoundCapTemperature(uint32_t soundSpeedMmPerS, uint64_t soundSpeedLimitMmPerS,
                                    uint32_t referenceTemperature) {
        const uint64_t limit = soundSpeedLimitMmPerS;
        assert(soundSpeedMmPerS > 0 && soundSpeedMmPerS <= limit && limit < ((uint64_t)1 << 32));

        // T_基準 × 上限 ÷ c̃(圧力の拡散の数で決める。T-0631。上限 < 2^32 なので 64bit に収まる)
        const uint64_t cap = (uint64_t)referenceTemperature * limit / soundSpeedMmPerS;
        return (uint32_t)std::min<uint64_t>(cap, UINT32_MAX);
    }

    namespace {

        // 2^(exponent + level)(面の式の × 2^k。level は GAS_MIN_CELL_LEVEL〜GAS_MAX_CELL_LEVEL)
        uint64_t LevelPower(int32_t exponent, int32_t level) {
            return (uint64_t)1 << (uint32_t)(exponent + level);
        }

        // value × 2^-level(層の重さ・重さの仕事。1 段細かいと 1 層の厚さが半分。切り捨て)
        uint64_t PerLevelLayer(uint64_t value, int32_t level) {
            return level >= 0 ? value >> (uint32_t)level : value << (uint32_t)(-level);
        }

    }  // namespace

    GasCoefficients MakeGasCoefficients(const GasConfig& config) {
        // 音速の基準は世界の環境の値(ADR-0176)。0 = 無い世界(ブランク)では気体の箱を作れない
        assert(config.substeps > 0 && config.soundSpeedMmPerS > 0);
        assert(config.gravityMicrometersPerSecond2 <= fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2);
        assert(config.cellLevel >= GAS_MIN_CELL_LEVEL && config.cellLevel <= GAS_MAX_CELL_LEVEL);

        // 安定の条件: ν = c̃ Δt_小刻み ÷ Δx = c̃(mm/s)÷ (30000 × 小刻み)。圧力の拡散(陽解法の 3 次元の拡散)で ν ≤ 約 2/3、
        // 余裕を見て 0.6 まで(既定の 340 m/s・小刻み 24 で 0.47。T-0226 で 1% の乱れが育つ境を測った: 小刻み 16 で発散・18 で安定)。
        // レベル k のセルは Δx = 0.5 m × 2^-k(T-0765。下の面の式の × 2^k・層の × 2^-k。レベル 0 は今までとビット単位で同じ)
        assert(config.soundSpeedMmPerS <= GasSoundSpeedLimitMmPerS(config.substeps, config.cellLevel));
        const uint64_t substeps = config.substeps;
        const int32_t level = config.cellLevel;
        GasCoefficients coefficients;

        // mg/µmol = M(mg/mol)÷ 1e6
        for (uint32_t s = 0; s < config.speciesCount; ++s)
            coefficients.massPerAmount[s] = ((uint64_t)config.species[s].molarMass << 32) / MICRO;

        // 中心の流れ: F(mg)=(P_左 + P_右)÷ 2 × 2^-20 × Δt ÷ Δx、Δt ÷ Δx = 1 ÷ (30 × 小刻み)(s/m)
        coefficients.centralFlow = LevelPower(31, level) / (30 * substeps);

        // 圧力の拡散: F(mg)= Δp̃(µPa)× 1e-6 × A × Δt ÷ (4 c̃)× 1e6 = Δp̃ × 25 ÷ (24 × 小刻み × c̃(mm/s))
        // 素の Rusanov(÷ 2c̃)の半分。÷ 2c̃ だと 3 次元で ν ≤ 1/3(本物の音速で小刻み 34 以上)になり、1% の乱れが
        // 小刻み 32 でも育った(T-0226)。半分でも音の波の速さ・衝撃波の速さは理論どおり(gas_reference_test の管の試験)
        coefficients.pressureFlow = (25 * LevelPower(40, level)) / (24 * substeps * config.soundSpeedMmPerS);

        // 面の力積: (p̃_左 + p̃_右)÷ 2 × 1e-6 × A × Δt × 1e6(mg/kg)× 2^20 =(和)× 2^15 ÷ (15 × 小刻み)
        coefficients.pressureImpulse = LevelPower(31, level) / (15 * substeps);

        // 浮力: Δm(mg)× g × Δt × 2^20 = Δm × g(µm/s²)× 2^36 ÷ (60 × 小刻み × 10^6)(2^36 は 2^20 と mg ÷ kg の 2^16 の分)。
        // セルの大きさによらない(量がレベルの単位 = 密度なので)
        const uint64_t gravity = config.gravityMicrometersPerSecond2;
        coefficients.gravityImpulse = (gravity << 36) / (60 * substeps * MICRO);

        // 層の重さの圧力・重さの仕事(換算は world_physics.hlsli。標準の重力で 392266・4903325。レベル 0 の 1 層 = 0.5 m。× 2^-k)
        coefficients.hydrostatic = PerLevelLayer(fx::GasHydrostaticFromMicrometers(config.gravityMicrometersPerSecond2),
                                                 level);
        coefficients.gravityWork = PerLevelLayer(fx::GasGravityWorkFromMicrometers(config.gravityMicrometersPerSecond2),
                                                 level);

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
        GasReferenceLayer uniform{.temperatureMilliKelvin = temperatureMilliKelvin};
        for (uint32_t s = 0; s < config.speciesCount && s < fractionsQ32.size(); ++s)
            uniform.fractionsQ32[s] = fractionsQ32[s];

        const std::vector<GasReferenceLayer> layers(config.sizeZ, uniform);

        return MakeHydrostaticReference(config, layers, surfacePressureMicroPascal);
    }

    std::vector<GasCell> MakeHydrostaticReference(const GasConfig& config, std::span<const GasReferenceLayer> profile,
                                                  uint64_t surfacePressureMicroPascal) {
        assert(profile.size() == config.sizeZ);
        const GasCoefficients coefficients = MakeGasCoefficients(config);
        std::vector<GasCell> layers;
        uint64_t pressure = surfacePressureMicroPascal;

        for (uint32_t z = 0; z < config.sizeZ; ++z) {
            // --- 物質量 n = p ÷ (T × R ÷ V)(この層の温度と成分。T-0499)---
            const uint32_t temperatureMilliKelvin = profile[z].temperatureMilliKelvin;
            const uint64_t divisor = PRESSURE_PER_AMOUNT_TEMPERATURE_Q32 * temperatureMilliKelvin;
            const uint64_t total = fx::FxDivU128By64(fx::FxMulU64Full(pressure, ONE_Q32), divisor).quotient;

            GasPerSpecies<uint64_t> amounts;
            for (uint32_t s = 0; s < config.speciesCount; ++s)
                amounts[s] = MulShiftU64(total, profile[z].fractionsQ32[s], 32);

            const GasCell cell = MakeGasCell(config, amounts, temperatureMilliKelvin);
            layers.push_back(cell);

            // --- 上の層の圧力 = この層の重さの分だけ下げる(釣り合いは近似でよい: 力は基準との差で入れるので、基準そのものは止まっている)---
            const uint64_t mass = CellMass(config, coefficients, cell);
            pressure -= mass * coefficients.hydrostatic / 10000;
        }

        return layers;
    }

    namespace {

        // 設定・係数・層の基準から、基準の導く値・開いた境界の外・α・頭打ちの温度を作る(MakeGasBox・SetGasEnvironment)
        void MakeGasLayers(GasBox& box) {
            const GasConfig& config = box.config;
            box.referenceDerived.clear();
            box.ghost.clear();

            // --- 基準の導く値(p̃ = 0)と開いた境界の外(基準 + 風)---
            for (const GasCell& layer : box.reference) {
                box.referenceDerived.push_back(DeriveRaw(config, box.coefficients, layer));

                GasCell ghost = layer;
                const auto mass = (int64_t)box.referenceDerived.back().mass;
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                    ghost.momentum[axis] = mass * config.windVelocity[axis];

                ghost.energy += DivideNearest(KineticEnergy(config, ghost, (uint64_t)mass),
                                              1000);  // 風の運動エネルギー(断熱)

                box.ghost.push_back(ghost);
            }

            // --- α = c̃² ÷ c_iso²、c_iso² = p ÷ ρ = p(µPa)× 0.125 ÷ m(mg)(地面の層)---
            // α(Q32)= c̃(mm/s)² × m × 8 × 2^32 ÷ (p × 1e6)
            const GasDerived& ground = box.referenceDerived.front();
            const uint64_t sound = config.soundSpeedMmPerS;
            const fx::FxU128 numerator = fx::FxMulU64Full(sound * sound * 8 * ground.mass, ONE_Q32);
            box.coefficients.soundScale = fx::FxDivU128By64(numerator, (uint64_t)ground.pressure * MICRO).quotient;

            // 断熱: α を 1 ÷ γ = C_v ÷ (C_v + n R) 倍(圧縮で温まる分だけ圧力が強く戻るので、音速 √(α γ c_iso²) を c̃ にそろえる)
            if (config.thermodynamics == GasThermodynamics::Adiabatic) {
                const GasCell& groundCell = box.reference.front();
                const uint64_t capacity = HeatCapacity(config, groundCell);
                const uint64_t gasConstant = fx::FxDivU128By64(fx::FxMulU64Full(TotalAmount(config, groundCell),
                                                                                GAS_CONSTANT_NANOJOULE_PER_AMOUNT_X1E6),
                                                               MICRO)
                                                 .quotient;
                box.coefficients.soundScale = fx::FxDivU128By64(fx::FxMulU64Full(box.coefficients.soundScale, capacity),
                                                                capacity + gasConstant)
                                                  .quotient;
            }

            // 頭打ちの温度(D-461・T-0269): 既定は安定の上限まで本物の音速(T_基準 ×(上限 ÷ c̃)²)
            box.coefficients.soundTemperature = config.soundCapTemperature == 0
                                                    ? GasSoundCapTemperature(
                                                          config.soundSpeedMmPerS,
                                                          GasSoundSpeedLimitMmPerS(config.substeps, config.cellLevel),
                                                          ground.temperature)
                                                    : config.soundCapTemperature;
        }

    }  // namespace

    GasBox MakeGasBox(const GasConfig& config, std::vector<GasCell> reference) {
        assert(reference.size() == config.sizeZ);
        GasBox box;
        box.config = config;
        box.coefficients = MakeGasCoefficients(config);
        box.reference = std::move(reference);
        MakeGasLayers(box);

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

    std::expected<void, std::string> CheckGasEnvironment(const GasConfig& current, const GasConfig& next) {
        // 環境の値の外は今と同じであること
        GasConfig expected = next;
        expected.soundSpeedMmPerS = current.soundSpeedMmPerS;
        expected.gravityMicrometersPerSecond2 = current.gravityMicrometersPerSecond2;
        if (expected != current)
            return std::unexpected("気体の箱の作り(大きさ・物質・小刻み・境界・風・丸め)は環境の値では替えられない");

        if (next.soundSpeedMmPerS == 0)
            return std::unexpected("気体の箱には音速の基準が要る(0 にはできない)");

        // 範囲(重力の上限・小刻みの安定の上限)は読み込み・ホットリロードと同じ決まりと理由の形(T-0470)
        const WorldPhysics world = {.gravityMicrometersPerSecond2 = next.gravityMicrometersPerSecond2,
                                    .soundSpeedMillimetersPerSecond = next.soundSpeedMmPerS};
        return CheckWorldPhysics(world, next.substeps);
    }

    void SetGasEnvironment(GasBox& box, const GasConfig& config, std::vector<GasCell> reference) {
        assert(CheckGasEnvironment(box.config, config).has_value());
        assert(reference.size() == config.sizeZ);
        box.config = config;
        box.coefficients = MakeGasCoefficients(config);
        box.reference = std::move(reference);
        MakeGasLayers(box);
    }

    GasDerived DeriveGasCell(const GasBox& box, const GasCell& cell, uint32_t z) {
        return DeriveCell(box, cell, z, GAS_OPEN_FULL);
    }

    int64_t GasKineticEnergy(const GasBox& box, const GasCell& cell) {
        return KineticEnergy(box.config, cell, CellMass(box.config, box.coefficients, cell));
    }

    uint64_t GasCellMass(const GasBox& box, const GasCell& cell) {
        return CellMass(box.config, box.coefficients, cell);
    }

    bool GasHasViscosity(const GasConfig& config) {
        return HasViscosity(config);
    }

    void SetGasMomentum(const GasBox& box, GasCell& cell, const std::array<int64_t, GAS_AXES>& momentum) {
        const uint64_t mass = CellMass(box.config, box.coefficients, cell);
        const int64_t before = KineticEnergy(box.config, cell, mass);
        cell.momentum = momentum;
        cell.energy += DivideNearest(KineticEnergy(box.config, cell, mass) - before, 1000);
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

        ApplyViscosity(box, layout);
        ++box.tick;
    }

    std::vector<GasFaceLink> MakeGasBoxLinks(const GasBox& box) {
        const std::vector<GasFace> faces = EnumerateFaces(box);

        return {faces.begin(), faces.end()};
    }

    void StepGasLinks(GasBox& box, std::span<const GasFaceLink> links) {
        assert(box.cellLayers.empty() || box.cellLayers.size() == box.cells.size());
        std::vector<GasFace> layout(links.size());
        for (size_t face = 0; face < links.size(); ++face)
            static_cast<GasFaceLink&>(layout[face]) = links[face];

        for (uint32_t substep = 0; substep < box.config.substeps; ++substep)
            Substep(box, layout, (box.tick * box.config.substeps) + substep);

        ApplyViscosity(box, layout);
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

    // --- 圧力の投影の模型の部品(T-0503 研究。何をするかは gas_pressure_system.h。小刻みの部品をそのまま使う)---

    namespace {

        // 1 回の投影の係数: 中心の流れ・面の力積・重さの力積を Δt = 1/60 s ÷ substeps で作り直す(式は MakeGasCoefficients と同じ。
        // 陰的な圧力には音速の安定の条件が無いので、検査を通すだけの音速 1 mm/s で作る。圧力の拡散の係数は使わない)
        GasCoefficients PressureCoefficients(const GasBox& box, uint32_t substeps) {
            GasConfig config = box.config;
            config.substeps = substeps;
            config.soundSpeedMmPerS = 1;
            const GasCoefficients step = MakeGasCoefficients(config);

            GasCoefficients coefficients = box.coefficients;
            coefficients.centralFlow = step.centralFlow;
            coefficients.pressureImpulse = step.pressureImpulse;
            coefficients.gravityImpulse = step.gravityImpulse;

            return coefficients;
        }

        // V ÷ c_i²(mg/µPa の Q48)= 2^45 × 10^6 × T_地面 ÷ (c̃² × T)(V = 0.125 m³ = 2^-3。c_i² = c̃² T ÷ T_地面)。c̃ = 0 は最速の下限
        uint64_t PressureCapacity(uint64_t soundMmPerS, uint32_t groundTemperature, uint32_t temperature) {
            if (soundMmPerS == 0)
                return GAS_PRESSURE_MIN_CAPACITY;

            const fx::FxU128 numerator = fx::FxMulU64Full((uint64_t)1 << 45, MICRO * groundTemperature);
            const uint64_t perSound = fx::FxDivU128By64(numerator, soundMmPerS * soundMmPerS).quotient;
            const uint64_t capacity = perSound / std::max<uint32_t>(temperature, 1);

            return std::max(capacity, GAS_PRESSURE_MIN_CAPACITY);
        }

        // K = A Δt² ÷ Δx(mg/µPa の Q48)= 0.5 ÷ (3600 × substeps²)× 2^48
        uint64_t PressureLinkCoefficient(uint32_t substeps) {
            return ((uint64_t)1 << 47) / (3600 * (uint64_t)substeps * substeps);
        }

        // 面の係数 K_f = K × 面の開き(Q16。0 の向きへ切り捨て)。壁は 0(T-0667)
        uint64_t PressureFaceCoefficient(const GasBox& box, uint64_t link, const GasFace& face) {
            if (face.left.kind == SideKind::Wall || face.right.kind == SideKind::Wall)
                return 0;

            const uint32_t open = FaceOpen(box, face);
            if (open == GAS_OPEN_FULL)
                return link;

            return fx::FxDivU128By64(fx::FxMulU64Full(link, open), GAS_OPEN_FULL).quotient;
        }

        // 圧力の式の V の割合(Q16)= セルの空き(下限 1/256。陽解法の DeriveOpenCell と同じ体積で圧力を読む)
        uint32_t PressureVolumeShare(const GasBox& box, uint32_t index) {
            if (box.openQ16.empty())
                return GAS_OPEN_FULL;

            return std::max(box.openQ16[index], gas::GF_OPEN_PRESSURE_FLOOR);
        }

        // 容量 × V の割合(1 以上。塞がったセルも対角を 0 にしない)
        uint64_t ShareCapacity(uint64_t capacity, uint32_t share) {
            if (share == GAS_OPEN_FULL)
                return capacity;

            return std::max<uint64_t>(fx::FxDivU128By64(fx::FxMulU64Full(capacity, share), GAS_OPEN_FULL).quotient, 1);
        }

        // 面の予測の流れ F*(mg。左 → 右が正): 今の運動量の中心の流れ − K_f × 縦の面の両側の質量の差の静水圧(陽解法の圧力の拡散と同じ釣り合い)。
        // 一部だけ開いた面は各側の運動量を「開き ÷ 空き」倍(陽解法の ComputeFaceFlow と同じ。孔の中の速さ × 開いた面積)。link = K_f(0 なら流さない)
        int64_t PredictedFlow(const GasBox& box, const std::vector<GasDerived>& derived,
                              const GasCoefficients& coefficients, uint64_t link, const GasFace& face) {
            if (link == 0)
                return 0;

            const int64_t leftMomentum = SideCell(box, box.cells, face.left).momentum[face.axis];
            const int64_t rightMomentum = SideCell(box, box.cells, face.right).momentum[face.axis];
            int64_t momentumSum = leftMomentum + rightMomentum;
            const uint32_t open = FaceOpen(box, face);
            if (open != GAS_OPEN_FULL) {
                momentumSum = ScaleSigned(leftMomentum, open, SideOpen(box, face.left)) +
                              ScaleSigned(rightMomentum, open, SideOpen(box, face.right));
            }

            const int64_t central = fx::FxMulShiftS64(momentumSum, (int64_t)coefficients.centralFlow, 52);
            if (face.axis != 2 || !box.config.gravity)
                return central;

            const int64_t hydrostatic = HalfHydrostatic(
                box, Excess(box, derived, face.left) + Excess(box, derived, face.right));

            return central - fx::FxMulShiftS64((int64_t)link, hydrostatic, 48);
        }

    }  // namespace

    GasPressureSystem BeginGasPressure(const GasBox& box, const GasPressureOptions& options) {
        assert(options.substeps > 0);
        const std::vector<GasDerived> derived = DeriveAll(box);
        const std::vector<GasFace> faces = EnumerateFaces(box);
        const GasCoefficients coefficients = PressureCoefficients(box, options.substeps);
        const uint32_t ground = box.referenceDerived.front().temperature;
        const size_t count = box.cells.size();

        // --- 節: C_i・E_i の係数と p̃(頭打ちなし。陰的なので熱いセルの音速を抑えなくてよい)---
        GasPressureSystem system;
        system.linkCoefficient = PressureLinkCoefficient(options.substeps);
        system.capacities.resize(count);
        system.stateCapacities.resize(count);
        system.statePressures.resize(count);
        system.divergences.assign(count, 0);
        for (uint32_t index = 0; index < count; ++index) {
            const uint32_t temperature = derived[index].temperature;
            const int64_t deviation = derived[index].pressure - box.referenceDerived[CellLayer(box, index)].pressure;
            const uint32_t share = PressureVolumeShare(box, index);
            system.capacities[index] = ShareCapacity(PressureCapacity(options.soundSpeedMmPerS, ground, temperature),
                                                     share);
            system.stateCapacities[index] = ShareCapacity(
                PressureCapacity(box.config.soundSpeedMmPerS, ground, temperature), share);
            system.statePressures[index] = fx::FxMulShiftS64(deviation, (int64_t)box.coefficients.soundScale, 32);
        }

        system.cellCapacities = system.capacities;

        // --- 面: F* と出ていく量の合計。開いた境界の面は外を x = 0 として対角へ。壁・閉じた面(開き 0)は式に入れない ---
        system.predictedFlows.resize(faces.size());
        system.faceCoefficients.resize(faces.size());
        for (size_t f = 0; f < faces.size(); ++f) {
            const GasFace& face = faces[f];
            const uint64_t link = PressureFaceCoefficient(box, system.linkCoefficient, face);
            const int64_t flow = PredictedFlow(box, derived, coefficients, link, face);
            system.faceCoefficients[f] = link;
            system.predictedFlows[f] = flow;
            if (link == 0)
                continue;

            const bool leftCell = face.left.kind == SideKind::Cell;
            const bool rightCell = face.right.kind == SideKind::Cell;
            if (leftCell)
                system.divergences[face.left.index] += flow;

            if (rightCell)
                system.divergences[face.right.index] -= flow;

            if (leftCell && rightCell)
                system.links.push_back({.left = face.left.index, .right = face.right.index, .coefficient = link});
            else
                system.capacities[leftCell ? face.left.index : face.right.index] += link;
        }

        return system;
    }

    void FinishGasPressure(GasBox& box, const GasPressureOptions& options, const GasPressureSystem& system,
                           std::span<const int64_t> pressures, uint64_t roundingTick, std::vector<int64_t>* cuts) {
        assert(pressures.size() == box.cells.size());
        const GasCoefficients saved = box.coefficients;
        box.coefficients = PressureCoefficients(box, options.substeps);

        // --- (1) 導く値。圧力は解いた x に替える(面の力積が読む)---
        std::vector<GasDerived> derived = DeriveAll(box);
        for (size_t index = 0; index < derived.size(); ++index)
            derived[index].scaledPressure = pressures[index];

        // --- (2) 面の力(x の力積・重さ)を当てる ---
        std::vector<GasFace> faces = EnumerateFaces(box);
        assert(faces.size() == system.predictedFlows.size());
        for (GasFace& face : faces)
            ComputeFaceForce(box, derived, face);

        std::vector<GasCell> forced = box.cells;
        for (const GasFace& face : faces)
            ApplyImpulse(box, forced, face);

        // --- (3) 面の流れ F = F* + K_f (x_左 − x_右)(開いた境界の外は x = 0。壁・閉じた面は 0)。出ていく割合を抑える ---
        for (size_t f = 0; f < faces.size(); ++f) {
            GasFace& face = faces[f];
            const auto link = (int64_t)system.faceCoefficients[f];
            if (link == 0)
                continue;

            const int64_t left = face.left.kind == SideKind::Cell ? pressures[face.left.index] : 0;
            const int64_t right = face.right.kind == SideKind::Cell ? pressures[face.right.index] : 0;
            face.flow = system.predictedFlows[f] + fx::FxMulShiftS64(link, left - right, 48);
        }

        LimitOutflow(box, derived, faces, cuts);

        // --- (4)(5) 移す成分の物質量と、風上の中身(力を当てた後)を移す(小刻みと同じ部品)---
        ComputeMovedAmounts(box, forced, derived, roundingTick, faces);
        LimitMovedAmounts(box, forced, faces);

        std::vector<GasCell> next = forced;
        const GasApplyWorld world{.ledger = &box.ledger, .next = &next};
        for (const GasFace& face : faces)
            Transfer(box, forced, derived, world, face);

        box.cells = std::move(next);
        box.coefficients = saved;
    }

}  // namespace bicameral::sim
