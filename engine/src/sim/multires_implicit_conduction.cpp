// multires_implicit_conduction.cpp — 細かいレベルの熱の陰解法(方式②。ADR-0019)を木の伝導につなぐ CPU リファレンス(T-0119。D-434 の案 a)。
// 面は「流れを計算する側(違うレベルなら細かい側。同じレベルなら両側)」のレベルで分ける: 基準(subcycleBaseLevel)以下は今までの
// 陽解法(multires_conduction.cpp の ComputeConduction)、基準より細かい(Δk 1〜implicitMaxGap)セルの面はここで陰解法の
// 1 刻み(StepImplicit)に入れる。それより細かいセルは陽解法のまま(温度の端数の精度で 1 mK の判定に届かない。下の「約束」)。
// データの流れ: 頁のある・凍っていない本物のブロックのセル(MrCellThermal: 刻みの初めの温度と熱容量)→ 陰解法のセルと面
//   (面の隣は陽解法と同じ MrFindFaceNeighbor)→ SolveImplicitFlows(V サイクル → 面の流れ → 安全網)→ 面の流れを運ぶ計算の
//   当てる段 TxApplyFaceFlow(transport_apply.hlsli。陽の面の TxFlowCell と同じ世界の Add・HasFraction。T-0715)で変化の表 deltas の
//   両側へ(呼ぶ側の StepPagedBlock が反応の前に足す)
// 約束:
//   - 系の未知数は基準より細かいセル。その面の粗い側の基準以下のセルも「境のセル」として入れる(境のセルの基準以下の面は陽解法のまま、
//     境の面だけこの系で受ける = 面で分けた分割)。どちらも頁のある・凍っていない・熱容量のあるセルだけ。一様なブロック・凍らせた
//     ブロック・熱容量 0 のセルとの面は流れない(陽解法と同じ。一様なブロックは刻みの初めに隣と温度差が無いので頁を持たない)
//   - 熱容量は刻みの初めの値で線形にする(相変化・反応で変わる分は次の刻みの温度に出る)。温度は整数部だけから(陽解法と同じ)
//   - 活性で刻まない(眠っている)頁のブロックも入れる: 陰解法では熱が 1 刻みに 1 セルより遠くへ届くので、全部刻んだ時と同じ系にする
//     (活性 = 全部のビット一致)。変わったブロックは StepPagedBlock が changed にし、次の刻みに起こされる
//   - Δk > implicitMaxGap(既定 8)のセルは陽解法のまま(頭打ちで 1 段ごとに約 1/4 遅い)。T* は mK × 2^16 の整数なので、面の流れの
//     誤差は約 4^Δk × 2^-16 mK になり、Δk 8 を超えると「新しい温度の誤差 1 mK」で止まらず上限まで回って安全網が陽解法に戻した
//     (たくさんの要求の場面の深さ 26 段で、毎刻み 64 回・14 万セル。T-0119 の計測)。そこの陽解法の面は、陰解法の系のセルにも
//     刻みの初めの温度から流れを足す(境の面の逆向き)
//   - 影のブロック(覗き窓)は陽解法のまま(中間のセルも刻むので、多重格子の親と重なる。T-0128)
//   - 系の大きさの上限(T-0178。options.implicitLimits。GPU では VRAM に先に取る大きさ): 流れの段の前に MarkImplicitBlocks が、
//     入れられるブロックを枠の順に「未知数・セルの上限の見込み(ImBlockCellBound)の和」が予算(MakeMultiresImplicitBudget)に入る所まで選ぶ。
//     選ばれなかったブロックはその刻みだけ陽解法(頭打ち)のまま(QUESTIONS Q22 の案 A。仮〔ユーザー未確認〕)。選ばれたブロックとの
//     同じレベルの面は、選ばれなかった側が境のセルとしてこの系で受ける(陽解法の側は計算しない。multires_conduction.cpp の CollectCellFaces)。
//     粗い側・細かい側の面は、今までの境のセル・Δk > implicitMaxGap と同じ形。多重格子の 2 段目からの節・隣が上限に入らなければ、
//     その段を作らずに縮約を止める(BuildImplicitGrid の limits。反復は遅くなりうるが、保存と安全網は同じ)
//   - 保存は面ごとに 1 つの値(ADR-0017 の形)なのでビット単位。端数の枠が無い粗い側へは整数の単位の倍数だけ送る
//   - 開いた世界の端(T-0790・ADR-0347。world_edge.hlsli の MR_NEIGHBOR_EDGE): 未知数のセルの端の面は、外のセル(自分と同じレベルの見立て。
//     陽の端 T-0721 と同じ CellViews::Outside)を温度を固定した境として系に入れる(段 0 の対角と右辺に入る。implicit_conduction.hlsli の
//     ImEdgeRhs)。流れ g_端 (T* − T_外) は当てる段 TxApplyEdgeFlow で自分から引き、帳簿の「世界の端」へ。安全網で戻す時は陽の端と同じ流れ。
//     陽解法は陰解法のブロックの面を流さないので、前はここを誰も流さず端が wall と同じだった
//   - 混ざったセル(層 + 気体。相の層の欄がある世界だけ。T-1230 段 2 ②・ADR-0512 の 2)は系の節 2 つ(層・気体。同じ座標)。熱容量は見立ての部分
//     (CV_VIEW_PHASE_LAYER の部分つきの片側)の熱容量の上限 × 8・温度は部分の温度。面は部分つきの片側の組ごとに 1 本(重なり PlPairOverlapsOf・
//     係数 = min(PlSlotConductance) × HC_LIMIT_PER_CONDUCTANCE × 4^kf = 陽の TxPartFaceFlow と同じ値・頭打ちなし)。違うレベルの面は粗い側の
//     層の高さを細かい面の段へ写す(TxPartSideOnRow)。層の上の面(層 ⇄ 気体)は未知数の混ざったセルだけ(境のセルの層の上の面は陽の段が流す)。
//     流れの和はセルのエネルギーへ(今の当てる段)、層の節の分は AddCondensedDelta で Ec へ(粗い側は送った分の整数部だけ。陽と同じ)。
//     安全網で陽に戻す流れは、部分の組の面・層の上の面とも陽の部分の道の値を面に渡す(ImplicitFace::explicitFlow。上限を重なりで縮める。T-1230 段 2 ③)。
//     多重格子の段(BuildImplicitGrid)は変えない: 同じ座標の 2 節は最も細かい段の縮約で同じ親へ(熱容量の和)。相の層の欄が無い世界・混ざらないセルは今の 1 節のまま(ビット単位で同じ)
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#include <algorithm>
#include <limits>

#include "common/implicit_conduction.hlsli"
#include "common/multires_conduction.hlsli"
#include "common/transport_conduction.hlsli"
#include "common/transport_conduction_parts.hlsli"
#include "sim/implicit_conduction.h"
#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        using nest_detail::CellViews;
        using nest_detail::CpuTree;

        constexpr uint32_t NOT_IN_SYSTEM = std::numeric_limits<uint32_t>::max();

        // 陰解法の系(セルの番号 = grid.cells の添字)
        struct ImplicitSystem {
            std::vector<ImplicitCell> cells;
            std::vector<ImplicitFace> faces;

            // --- セルごと: 木のどこか ---
            std::vector<uint32_t> slots;
            std::vector<uint32_t> indices;
            // 節の部分: PL_PART_WHOLE・混ざったセルは層 PL_PART_CONDENSED(番号 n)と気体 PL_PART_GAS(n + 1)
            std::vector<uint32_t> parts;

            std::vector<uint32_t> ids;        // [枠 × 512] → セルの番号(無ければ NOT_IN_SYSTEM)
            std::vector<ImplicitEdge> edges;  // 開いた世界の端の面(未知数のセルの。T-0790)
        };

        // 系に入れられるブロックか: 世界の本物のブロックで、頁があり、凍っていない
        bool CanJoin(const MultiresNest& nest, std::span<const uint8_t> frozen, uint32_t slot) {
            const MrBlock& block = nest.blocks[slot];
            const bool frozenNow = !frozen.empty() && frozen[slot] != 0;

            return slot < nest.capacity.worldBlocks && block.kind == MR_BLOCK_REAL && !MrIsUniform(block) && !frozenNow;
        }

        // セルが系の節 2 つ(層・気体)になる混ざったセルか(部分の熱容量の上限がどちらも 0 でない時。相の層の欄が無ければ false)
        bool IsTwoNodeCell(CellViews& thermals, uint32_t slot, uint32_t index) {
            const TxCellParts& cellParts = thermals.Parts(slot, index);

            return cellParts.mixed != 0 && cellParts.lateral.lower.capacityLimit != 0 &&
                   cellParts.lateral.upper.capacityLimit != 0;
        }

        // 系の節 1 つを足す(熱容量 C = 8 × 上限の係数〔レベルによらない数〕。仮のエネルギーは刻みの初めの温度に当たる値)
        void PushNode(ImplicitSystem& system, const MrBlock& block, uint32_t slot, uint32_t index,
                      uint64_t capacityLimit, uint32_t conductance, uint32_t temperature, uint32_t part) {
            ImplicitCell cell;
            cell.level = block.level;
            cell.x = block.originX + static_cast<int64_t>(MrCellX(index));
            cell.y = block.originY + static_cast<int64_t>(MrCellY(index));
            cell.z = block.originZ + static_cast<int64_t>(MrCellZ(index));
            cell.heatCapacity = capacityLimit << IM_ENERGY_BITS_PER_LEVEL;
            cell.conductance = conductance;
            cell.startTemperature = int64_t{temperature} << IM_TEMPERATURE_SHIFT;
            cell.energy = ImplicitEnergyFor(cell.heatCapacity, temperature);
            cell.coarseFraction = block.fraction != MR_NO_FRACTION;

            system.cells.push_back(cell);
            system.slots.push_back(slot);
            system.indices.push_back(index);
            system.parts.push_back(part);
        }

        // セルを系に足す(熱容量 0 なら足さない)。番号(混ざったセルは層の節の番号)を返す
        uint32_t AddCell(ImplicitSystem& system, const MultiresNest& nest, CellViews& thermals, uint32_t slot,
                         uint32_t index) {
            const size_t address = (size_t{slot} * MR_BLOCK_CELLS) + index;
            if (system.ids[address] != NOT_IN_SYSTEM)
                return system.ids[address];

            const MrBlock& block = nest.blocks[slot];
            const auto id = static_cast<uint32_t>(system.cells.size());

            // --- 混ざったセルは層と気体の 2 節(見立ての部分の横の片側 = lower が層・upper が気体)---
            if (IsTwoNodeCell(thermals, slot, index)) {
                const TxCellParts& cellParts = thermals.Parts(slot, index);
                const PlSideSlot& condensed = cellParts.lateral.lower;
                const PlSideSlot& gas = cellParts.lateral.upper;
                PushNode(system, block, slot, index, condensed.capacityLimit, condensed.conductance,
                         condensed.temperature, PL_PART_CONDENSED);
                PushNode(system, block, slot, index, gas.capacityLimit, gas.conductance, gas.temperature, PL_PART_GAS);
                system.ids[address] = id;

                return id;
            }

            const MrThermal thermal = thermals.At(slot, index);
            if (thermal.capacityLimit == 0)
                return NOT_IN_SYSTEM;

            PushNode(system, block, slot, index, thermal.capacityLimit, thermal.conductance, thermal.temperature,
                     PL_PART_WHOLE);
            system.ids[address] = id;

            return id;
        }

        // セルの節 id(AddCell の番号)のうち部分 part を受け持つ節(混ざらないセルは id のまま)
        uint32_t NodeForPart(const ImplicitSystem& system, uint32_t id, uint32_t part) {
            if (system.parts[id] == PL_PART_WHOLE || part != PL_PART_GAS)
                return id;

            return id + 1;
        }

        // セルの節 id の面 face の部分つきの片側(見立ての部分。混ざらないセルは見立ての熱の部分 1 つ = 今の面の値)
        PlPartSide NodeSide(const ImplicitSystem& system, CellViews& thermals, uint32_t id, uint32_t face) {
            const uint32_t slot = system.slots[id];
            const uint32_t index = system.indices[id];
            if (system.parts[id] == PL_PART_WHOLE)
                return TxWholePartSide(thermals.At(slot, index));

            return TxCellPartSide(thermals.Parts(slot, index), face);
        }

        // 面の係数(細かい側の単位。頭打ちなし)= min(G) × G の係数 × 4^kf(ADR-0019。陽解法の MrConductanceLimit と同じ値)
        fx::FxU128 FaceCoefficient(uint32_t fineConductance, uint32_t otherConductance, int32_t fineLevel) {
            const uint64_t conductance = std::min(fineConductance, otherConductance);

            return ImWideShiftLeft(ImWide(conductance * HC_LIMIT_PER_CONDUCTANCE),
                                   2 * static_cast<uint32_t>(fineLevel));
        }

        // 混ざったセルの節 id の開いた世界の端の面: 部分ごとに外のセル(混ざらない)と 1 本ずつ。安全網で戻す流れは陽の部分の道と同じ組の値
        void AddPartEdges(ImplicitSystem& system, CellViews& thermals, uint32_t id, const MrBlock& block, uint32_t face,
                          const CvCellView& outside) {
            const PlPartSide self = NodeSide(system, thermals, id, face);
            const TxPartPairs pairs = TxPartPairsOf(self, TxWholePartSide(outside.thermal), block.level, 0);
            auto push = [&](const TxPartPair& pair) {
                if (pair.conductance == 0)
                    return;

                const auto value = static_cast<uint32_t>(pair.conductance);
                system.edges.push_back({.cell = NodeForPart(system, id, pair.selfPart),
                                        .coefficient = FaceCoefficient(value, value, block.level),
                                        .temperature = int64_t{outside.thermal.temperature} << IM_TEMPERATURE_SHIFT,
                                        .explicitFlow = pair.explicitFlow});
            };
            push(pairs.lowerLower);
            push(pairs.upperUpper);
            push(pairs.lowerUpper);
            push(pairs.upperLower);
        }

        // 未知数のセル id の面 face が開いた世界の端なら、外のセル(自分と同じレベルの見立て。world_edge.hlsli)を温度を固定した境として
        // 系に入れる(T-0790)。係数は面と同じ式・外の温度は外の見立て・安全網で戻す流れは陽の端(T-0721。当てる段の TxEdgeFaceFlow)と同じ
        void AddCellEdge(ImplicitSystem& system, const MultiresNest& nest, CellViews& thermals,
                         const TxConduction& kind, uint32_t id, const MrBlock& block, uint32_t face) {
            const MultiresWorldEdges& edges = nest.edges;
            const uint32_t index = system.indices[id];
            const MrFaceNeighbor edge = WeEdgeNeighbor(edges.world, edges.columnBaseUp,
                                                       static_cast<uint32_t>(edges.column.size()), block, index, face);
            if (edge.kind != MR_NEIGHBOR_EDGE)
                return;

            const CvCellView outside = thermals.Outside(edge.index, block.level);
            if (system.parts[id] != PL_PART_WHOLE) {
                AddPartEdges(system, thermals, id, block, face, outside);
                return;
            }

            const TxSelfSide side = TxMakeSelfSide(kind, thermals.View(system.slots[id], index), block.level);
            system.edges.push_back(
                {.cell = id,
                 .coefficient = FaceCoefficient(system.cells[id].conductance, outside.thermal.conductance, block.level),
                 .temperature = int64_t{outside.thermal.temperature} << IM_TEMPERATURE_SHIFT,
                 .explicitFlow = TxEdgeFaceFlow(kind, side, outside)});
        }

        // どちらかが混ざったセルの面: 部分つきの片側の組ごとに 1 本(違うレベルは粗い側の層の高さを細かい面の段 row へ写す。陽の TxFlowConductionCell と同じ)
        void AddPartFaces(ImplicitSystem& system, CellViews& thermals, uint32_t id, uint32_t other,
                          const MrBlock& block, uint32_t index, uint32_t face, uint32_t gap) {
            const PlPartSide self = NodeSide(system, thermals, id, face);
            PlPartSide neighbor = NodeSide(system, thermals, other, face ^ 1u);
            if (gap != 0) {
                const uint32_t mask = gap < 32u ? (1u << gap) - 1u : 0xFFFFFFFFu;
                const uint32_t row = static_cast<uint32_t>(block.originY + static_cast<int64_t>(MrCellY(index))) & mask;
                neighbor = TxPartSideOnRow(neighbor, gap, row);
            }

            // 組の値は transport_conduction_parts.hlsli の TxPartPairsOf(GPU の木の系を作る段と同じ関数)。
            // 安全網で戻す流れ = 陽の部分の道の組の値(上限を重なりで縮める。開いた世界の端 AddPartEdges と同じ置き方)
            const TxPartPairs pairs = TxPartPairsOf(self, neighbor, block.level, gap);
            auto push = [&](const TxPartPair& pair) {
                if (pair.conductance == 0)
                    return;

                const auto value = static_cast<uint32_t>(pair.conductance);
                system.faces.push_back({.fine = NodeForPart(system, id, pair.selfPart),
                                        .coarse = NodeForPart(system, other, pair.otherPart),
                                        .gap = gap,
                                        .coefficient = FaceCoefficient(value, value, block.level),
                                        .explicitFlow = pair.explicitFlow,
                                        .explicitFlowGiven = true});
            };
            push(pairs.lowerLower);
            push(pairs.upperUpper);
            push(pairs.lowerUpper);
            push(pairs.upperLower);
        }

        // 未知数のセル(基準より細かい)の 6 面。同じレベルの面は番号の小さい側が 1 回だけ足す。粗い側は境のセルとして足す
        void AddCellFaces(ImplicitSystem& system, const MultiresNest& nest, CellViews& thermals,
                          std::span<const uint8_t> frozen, const TxConduction& kind, uint32_t id) {
            const CpuTree tree{.nest = &nest};
            const uint32_t slot = system.slots[id];
            const uint32_t index = system.indices[id];
            const MrBlock block = nest.blocks[slot];
            for (uint32_t face = 0; face < MR_FACES; ++face) {
                const MrFaceNeighbor neighbor = MrFindFaceNeighbor(tree, slot, block, index, face,
                                                                   nest.capacity.rootLevel);
                if (neighbor.kind == MR_NEIGHBOR_NONE) {
                    AddCellEdge(system, nest, thermals, kind, id, block, face);
                    continue;
                }

                if (neighbor.kind != MR_NEIGHBOR_SAME && neighbor.kind != MR_NEIGHBOR_COARSER)
                    continue;

                if (!CanJoin(nest, frozen, neighbor.slot))
                    continue;

                const uint32_t other = AddCell(system, nest, thermals, neighbor.slot, neighbor.index);
                if (other == NOT_IN_SYSTEM || (neighbor.kind == MR_NEIGHBOR_SAME && other < id))
                    continue;

                const uint32_t gap = neighbor.kind == MR_NEIGHBOR_SAME ? 0 : neighbor.gap;
                if (system.parts[id] != PL_PART_WHOLE || system.parts[other] != PL_PART_WHOLE) {
                    AddPartFaces(system, thermals, id, other, block, index, face, gap);
                    continue;
                }

                system.faces.push_back({.fine = id,
                                        .coarse = other,
                                        .gap = gap,
                                        .coefficient = FaceCoefficient(system.cells[id].conductance,
                                                                       system.cells[other].conductance, block.level)});
            }
        }

        // 系を作る: まず未知数のセル(MarkImplicitBlocks が選んだブロック)を枠の順に全部足し(番号を先に決める)、次にその面と境のセル
        ImplicitSystem MakeSystem(const MultiresNest& nest, CellViews& thermals, std::span<const uint8_t> frozen,
                                  std::span<const uint8_t> implicitBlocks, const MultiresStepOptions& options) {
            ImplicitSystem system;
            system.ids.assign(nest.blocks.size() * MR_BLOCK_CELLS, NOT_IN_SYSTEM);
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                if (!CanJoin(nest, frozen, slot) || !nest_detail::IsImplicitBlock(implicitBlocks, slot))
                    continue;

                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    if (MrIsSteppedCell(nest.blocks[slot], index))
                        AddCell(system, nest, thermals, slot, index);
                }
            }

            const TxConduction kind = TxMakeConduction(options.subcycleBaseLevel, options.maxSubcycleGap);
            const auto unknowns = static_cast<uint32_t>(system.cells.size());
            for (uint32_t id = 0; id < unknowns; ++id) {
                if (system.parts[id] != PL_PART_GAS)
                    AddCellFaces(system, nest, thermals, frozen, kind, id);
            }

            // --- 層の上の面(未知数の混ざったセルの層 → 気体。陽の TxPartInnerFlow と同じ直列の値)---
            for (uint32_t id = 0; id < unknowns; ++id) {
                if (system.parts[id] != PL_PART_CONDENSED)
                    continue;

                const TxCellParts& cellParts = thermals.Parts(system.slots[id], system.indices[id]);
                const uint32_t series = PlClampU32(cellParts.seriesConductance);
                if (series == 0)
                    continue;

                const int32_t level = system.cells[id].level;
                system.faces.push_back(
                    {.fine = id,
                     .coarse = id + 1,
                     .gap = 0,
                     .coefficient = FaceCoefficient(series, series, level),
                     .explicitFlow = TxPartInnerFlow(cellParts.lateral.lower, cellParts.lateral.upper,
                                                     cellParts.seriesConductance, level),
                     .explicitFlowGiven = true});
            }

            return system;
        }

        // 陰の伝導の面の流れを当てる世界(transport_apply.hlsli の Add・HasFraction・AddEdge。陽の面の ConductionWorld と同じ変化の表と
        // 帳簿。T-0715・T-0790)
        struct ImplicitApplyWorld {
            MultiresNest* nest = nullptr;
            std::span<MrEnergyDelta> deltas;

            [[nodiscard]] bool HasFraction(uint32_t slot) const {
                return nest->blocks[slot].fraction != MR_NO_FRACTION;
            }

            void Add(uint32_t slot, uint32_t index, const MrEnergyDelta& delta) const {
                nest_detail::AddConductionDelta(*nest, deltas, slot, index, delta);
            }

            void AddEdge(uint32_t slot, uint32_t /*index*/, int64_t outflow) const {
                nest_detail::AddConductionEdge(*nest, slot, outflow);
            }
        };

        // 層の上の面(同じセルの層の節 → 気体の節)か。セルのエネルギーは変えず層の分だけ
        bool IsInnerFace(const ImplicitSystem& system, const ImplicitFace& face) {
            return face.gap == 0 && system.slots[face.fine] == system.slots[face.coarse] &&
                   system.indices[face.fine] == system.indices[face.coarse];
        }

        // 節 node が層の節なら、層の部分の変化 delta(そのセルのレベルの単位)を Ec へ(ADR-0512 の 2 の AddCondensed)
        void AddNodeCondensed(MultiresNest& nest, const ImplicitSystem& system, uint32_t node, int64_t delta) {
            if (delta == 0 || system.parts[node] != PL_PART_CONDENSED)
                return;

            nest_detail::AddCondensedDelta(nest, system.slots[node], system.indices[node], delta);
        }

        // 面の流れ flow(細かい側 → 相手が正)の層の節の分。違うレベルは陽の TxFlowConductionCell と同じ: 送った分(MrSplitCrossFlow)を細かい側から・
        // 粗い側は送った分の整数部だけ(端数はセルのエネルギーにだけ)
        void ApplyFaceCondensed(MultiresNest& nest, const ImplicitSystem& system, const ImplicitFace& face,
                                int64_t flow) {
            if (face.gap == 0) {
                AddNodeCondensed(nest, system, face.fine, -flow);
                AddNodeCondensed(nest, system, face.coarse, flow);
                return;
            }

            const bool fraction = nest.blocks[system.slots[face.coarse]].fraction != MR_NO_FRACTION;
            const int64_t sent = -MrSplitCrossFlow(flow, face.gap, fraction).fineDelta;
            AddNodeCondensed(nest, system, face.fine, -sent);
            AddNodeCondensed(nest, system, face.coarse, TxCoarseCondensedWhole(sent, face.gap));
        }

        // 多重格子の段ごとの節の数と隣の最大(計測用。GPU の ImTail の境〔節 1024・隣 32〕を見る。T-0120)
        void RecordLevels(MultiresNest& nest, const ImplicitGrid& grid) {
            for (const ImplicitGridLevel& level : grid.levels) {
                uint32_t widest = 0;
                for (size_t node = 0; node + 1 < level.rowStarts.size(); ++node)
                    widest = std::max(widest, level.rowStarts[node + 1] - level.rowStarts[node]);

                nest.implicitLevels.push_back({static_cast<uint32_t>(level.levels.size()), widest});
            }
        }

        // 系を作る時の木を写す(試験用。T-0129)
        void CaptureNest(MultiresNest& nest, std::span<const uint8_t> frozen) {
            nest.implicitNest.reset();
            auto copy = std::make_shared<MultiresNest>(nest);
            copy->captureImplicitNest = false;
            copy->implicitFrozen.clear();
            copy->implicitGrid = {};
            nest.implicitNest = std::move(copy);
            nest.implicitFrozen.assign(frozen.begin(), frozen.end());
        }

        // ブロックの未知数の節の数(刻むセルで熱容量のあるもの・混ざったセルは 2。AddCell が足すものと同じ)
        uint32_t CountUnknowns(const MultiresNest& nest, CellViews& thermals, uint32_t slot) {
            uint32_t count = 0;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (!MrIsSteppedCell(nest.blocks[slot], index))
                    continue;

                if (IsTwoNodeCell(thermals, slot, index))
                    count += 2;
                else if (thermals.At(slot, index).capacityLimit != 0)
                    ++count;
            }

            return count;
        }

        // 選んだ形(ADR-0019): V(2,2)・新しい温度の誤差の見込み 1 mK で止める
        ImplicitOptions MakeImplicitOptions(const MultiresStepOptions& options) {
            ImplicitOptions implicit;
            implicit.method = ImplicitMethod::Multigrid;
            implicit.cycles = options.implicitMaxCycles;
            implicit.toleranceMillikelvin = 1;

            return implicit;
        }

    }  // namespace

    namespace nest_detail {

        bool InImplicitConduction(const MultiresNest& nest, uint32_t slot, const MultiresStepOptions& options) {
            const MrBlock& block = nest.blocks[slot];

            const int32_t finest = options.subcycleBaseLevel + static_cast<int32_t>(options.implicitMaxGap);

            return options.implicitConduction && slot < nest.capacity.worldBlocks && block.kind == MR_BLOCK_REAL &&
                   block.level > options.subcycleBaseLevel && block.level <= finest;
        }

        bool IsImplicitBlock(std::span<const uint8_t> implicitBlocks, uint32_t slot) {
            return slot < implicitBlocks.size() && implicitBlocks[slot] != 0;
        }

        std::vector<uint8_t> MarkImplicitBlocks(MultiresNest& nest, CellViews& thermals,
                                                std::span<const uint8_t> frozen, const MultiresStepOptions& options) {
            std::vector<uint8_t> marks(nest.blocks.size(), 0);
            nest.implicitDeferredBlocks = 0;
            if (!options.implicitConduction)
                return marks;

            // --- 枠の順に、和が予算に入る所まで(超えたブロックから後は全部陽解法。GPU は同じ和を接頭和で)---
            FX_ASSERT(options.implicitOverflow == ImplicitOverflow::Explicit);  // B・C は未実装(Q22)
            const MultiresImplicitBudget budget = MakeMultiresImplicitBudget(options.implicitLimits);
            uint64_t unknowns = 0;
            uint64_t cells = 0;
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                if (!CanJoin(nest, frozen, slot) || !InImplicitConduction(nest, slot, options))
                    continue;

                const uint32_t count = CountUnknowns(nest, thermals, slot);
                unknowns += count;
                cells += ImBlockCellBound(count);
                if (unknowns > budget.unknowns || cells > budget.cells) {
                    nest.implicitDeferredBlocks += 1;
                    continue;
                }

                marks[slot] = 1;
            }

            return marks;
        }

        void AddImplicitConduction(MultiresNest& nest, CellViews& thermals, std::span<const uint8_t> frozen,
                                   std::span<const uint8_t> implicitBlocks, const MultiresStepOptions& options,
                                   std::span<MrEnergyDelta> deltas) {
            nest.implicitCost = {};
            nest.implicitCells = 0;
            nest.implicitLevels.clear();
            nest.implicitGrid = {};
            nest.implicitNest.reset();
            ImplicitSystem system = MakeSystem(nest, thermals, frozen, implicitBlocks, options);
            if (system.faces.empty() && system.edges.empty())
                return;

            if (nest.captureImplicitNest)
                CaptureNest(nest, frozen);

            // --- 1 刻み解いて、面の流れを当てる段で変化の表へ ---
            const ImplicitGridLimits gridLimits{.nodes = options.implicitLimits.nodes,
                                                .links = options.implicitLimits.links};
            ImplicitGrid grid = BuildImplicitGrid(std::move(system.cells), std::move(system.faces), false, gridLimits,
                                                  std::move(system.edges));
            if (nest.captureImplicitGrid)
                nest.implicitGrid = grid;

            std::vector<int64_t> flows;
            std::vector<int64_t> edgeFlows;
            nest.implicitCost = SolveImplicitFlows(grid, MakeImplicitOptions(options), flows, edgeFlows);
            nest.implicitCells = static_cast<uint32_t>(grid.cells.size());
            RecordLevels(nest, grid);

            // --- 3 当てる: 面の流れを両側へ(同じレベルは逆向きに同じ値・違うレベルは粗い側に整数部 + 端数。陽の面と同じ当てる段)---
            const ImplicitApplyWorld world{.nest = &nest, .deltas = deltas};
            for (size_t f = 0; f < grid.faces.size(); ++f) {
                const ImplicitFace& face = grid.faces[f];
                if (IsInnerFace(system, face)) {
                    AddNodeCondensed(nest, system, face.fine, -flows[f]);
                    continue;
                }

                TxApplyFaceFlow(world, system.slots[face.fine], system.indices[face.fine], system.slots[face.coarse],
                                system.indices[face.coarse], face.gap, flows[f]);
                ApplyFaceCondensed(nest, system, face, flows[f]);
            }

            // --- 3 当てる・5 帳簿: 開いた世界の端の面(外のセルは変わらず、流れた分は帳簿の「世界の端」へ。陽の端と同じ。T-0790)---
            for (size_t e = 0; e < grid.edges.size(); ++e) {
                const uint32_t cell = grid.edges[e].cell;
                TxApplyEdgeFlow(world, system.slots[cell], system.indices[cell], edgeFlows[e]);
                AddNodeCondensed(nest, system, cell, -edgeFlows[e]);
            }
        }

    }  // namespace nest_detail

}  // namespace bicameral::sim
