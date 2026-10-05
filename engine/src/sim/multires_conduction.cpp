// multires_conduction.cpp — 多重解像度の木の上の熱の伝導の CPU リファレンス(07 §1・17 §5「熱の伝導」。T-0019)。
// 式と面の隣の探し方は shaders/common/multires_conduction.hlsli(GPU も同じ関数を呼ぶ。T-0107)。
// 1 刻みの順(multires_nest.cpp の StepBlocks から呼ぶ):
//   1. MarkConductionWants: 刻むブロックのセルの面の流れを調べ、流れのある一様なブロックに MR_PAGE_WANTED を付け、
//      端数が要る粗いブロックに印を付ける(どちらも「端数の枠がある」とした場合の流れで決める)
//   2. (呼ぶ側)頁を枠の順に配る。足りなかったブロックはこの刻みは凍らせる(刻まず、面の流れも 0)
//   3. ComputeConduction: 端数の枠を枠の順に配り、刻むブロックのセルの面の流れを変化の表に足す
//   4. (呼ぶ側)変化を足してから反応
// 流れはどれも刻みの初めのセルから計算する(Jacobi 型。順に依存しない)。頁に広げても論理のセルは変わらないので、
// セルの熱は刻みの中で 1 回だけ作って使い回す(CellThermals)。
// 同じレベルの面は両側が同じ式で逆向きの値を出す(どちらも刻むことは活性が保証する。17 §5「活性」)。違うレベルの面は細かい側だけが
// 計算して、粗いセルの変化(整数部 + 端数)を表に足す。浮動小数点は使わない(engine/src/sim は検査の対象)。
// 細かいレベルの刻み(T-0108。StepConduction): レベル k の伝導を 1 刻みに 4^σ(k) 回の小刻みに分け、上の 1〜3 を小刻みごとに繰り返す。
// 小刻みでは、その回に小刻みが始まるレベルのブロックだけが流れを計算し、変化は表に溜めて、そのレベルの小刻みの終わりに足す
// (粗い側の値は粗い小刻みの初めのまま。細かい側が送った流れの合計を粗い側が 1 回で受ける = Berger–Colella の refluxing の形)。
// 活性(StepActive)では、小刻みで変わったブロックの面の隣を起こし、以後の小刻みから刻む(流れのある面は両側とも刻む、を小刻みでも保つ)。
#include <algorithm>
#include <array>

#include "common/multires_conduction.hlsli"
#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        using nest_detail::CellThermals;
        using nest_detail::CpuTree;

        // レベル level の小刻みの数の指数(4^σ 回。T-0108)
        uint32_t SubcycleShift(const MultiresStepOptions& options, int32_t level) {
            return MrSubcycleShift(level, options.subcycleBaseLevel, options.maxSubcycleGap);
        }

        // セル 1 つの面の流れ
        struct CellFaces {
            int64_t sameLevelOutflow = 0;  // 同じレベルの面の流れの和(出ていくのが正。自分のレベルの単位)

            // --- 粗い側への面(細かい単位の流れ。細かい → 粗いが正)---
            uint32_t crossCount = 0;
            std::array<MrFaceNeighbor, MR_FACES> crossNeighbors{};
            std::array<int64_t, MR_FACES> crossFlows{};
        };

        bool IsFrozen(std::span<const uint8_t> frozen, uint32_t slot) {
            return !frozen.empty() && frozen[slot] != 0;
        }

        // 枠 slot のセル index の 6 面。細かい側の面(相手が計算する)・外(断熱)・凍らせたブロックとの面は流れない
        CellFaces CollectCellFaces(const MultiresNest& nest, CellThermals& thermals, uint32_t slot, uint32_t index,
                                   std::span<const uint8_t> frozen, const MultiresStepOptions& options) {
            const CpuTree tree{.nest = &nest};
            const MrBlock& block = nest.blocks[slot];
            const MrThermal self = thermals.At(slot, index);
            const uint32_t shift = SubcycleShift(options, block.level);

            CellFaces faces;
            for (uint32_t face = 0; face < MR_FACES; ++face) {
                const MrFaceNeighbor neighbor = MrFindFaceNeighbor(tree, slot, block, index, face,
                                                                   nest.capacity.rootLevel);
                if (neighbor.kind != MR_NEIGHBOR_SAME && neighbor.kind != MR_NEIGHBOR_COARSER)
                    continue;

                if (IsFrozen(frozen, neighbor.slot))
                    continue;

                const MrThermal other = thermals.At(neighbor.slot, neighbor.index);
                if (neighbor.kind == MR_NEIGHBOR_SAME) {
                    faces.sameLevelOutflow += MrSubstepSameLevelFlow(self, other, block.level, shift);
                    continue;
                }

                const uint32_t coarseShift = SubcycleShift(options, nest.blocks[neighbor.slot].level);
                const int64_t flow = MrSubstepCrossLevelFlow(self, other, block.level, neighbor.gap, shift,
                                                             coarseShift);
                if (flow == 0)
                    continue;

                faces.crossNeighbors[faces.crossCount] = neighbor;
                faces.crossFlows[faces.crossCount] = flow;
                faces.crossCount += 1;
            }

            return faces;
        }

        bool IsSteppedSlot(std::span<const uint8_t> stepped, uint32_t slot) {
            return stepped[slot] != 0;
        }

        // ブロックの面に接するセルか。一様なブロックの中のセルどうしは同じ値なので、流れがありうるのは面に接するセルだけ
        bool OnBlockSurface(uint32_t index) {
            constexpr uint32_t LAST = MR_BLOCK_EDGE - 1;
            const uint32_t x = MrCellX(index);
            const uint32_t y = MrCellY(index);
            const uint32_t z = MrCellZ(index);

            return x == 0 || x == LAST || y == 0 || y == LAST || z == 0 || z == LAST;
        }

        // 粗い側へ送る面を印にする(送るなら相手も変わるので一様なら頁に広げる。端数が要るなら相手に印)。送る面があれば true
        bool MarkCrossSends(MultiresNest& nest, const CellFaces& faces, std::span<uint8_t> wantsFraction) {
            bool sends = false;
            for (uint32_t i = 0; i < faces.crossCount; ++i) {
                const MrCrossTransfer transfer = MrSplitCrossFlow(faces.crossFlows[i], faces.crossNeighbors[i].gap,
                                                                  true);
                if (transfer.fineDelta == 0)
                    continue;

                sends = true;
                const uint32_t coarseSlot = faces.crossNeighbors[i].slot;
                if (nest.blocks[coarseSlot].page == MR_NO_PAGE)
                    nest.blocks[coarseSlot].page = MR_PAGE_WANTED;

                if (transfer.coarseDelta.fraction != 0)
                    wantsFraction[coarseSlot] = 1;
            }

            return sends;
        }

        // 刻むブロック 1 つの面の流れを調べ、印を付ける。一様で流れがあれば自分も頁に広げる
        void MarkBlockWants(MultiresNest& nest, CellThermals& thermals, uint32_t slot, std::span<uint8_t> wantsFraction,
                            const MultiresStepOptions& options) {
            const bool uniform = MrIsUniform(nest.blocks[slot]);
            bool flows = false;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (!MrIsSteppedCell(nest.blocks[slot], index) || (uniform && !OnBlockSurface(index)))
                    continue;

                const CellFaces faces = CollectCellFaces(nest, thermals, slot, index, {}, options);
                const bool sends = MarkCrossSends(nest, faces, wantsFraction);
                flows = flows || sends || faces.sameLevelOutflow != 0;
            }

            if (flows && nest.blocks[slot].page == MR_NO_PAGE)
                nest.blocks[slot].page = MR_PAGE_WANTED;
        }

        // 端数が要るブロックに端数の枠を枠の順に配る(空きのスタックの上から。足りなければ数え、整数の単位の倍数だけ送る)
        void AllocateConductionFractions(MultiresNest& nest, std::span<const uint8_t> frozen,
                                         std::span<const uint8_t> wantsFraction) {
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                MrBlock& block = nest.blocks[slot];
                if (wantsFraction[slot] == 0 || frozen[slot] != 0 || block.fraction != MR_NO_FRACTION)
                    continue;

                uint32_t& freeCount = nest.counters[MR_COUNTER_FREE_FRACTIONS];
                if (freeCount == 0) {
                    nest.counters[MR_COUNTER_FRACTION_SHORTAGE] += 1;
                    continue;
                }

                block.fraction = nest.freeFractions[--freeCount];
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                    nest_detail::SetFraction(nest, block.fraction, index, MrMakeEmptyFraction());
            }
        }

        // セル 1 つの面の流れを変化の表に足す(自分と、粗い側へ送った先)
        void AddCellFlows(const MultiresNest& nest, CellThermals& thermals, uint32_t slot, uint32_t index,
                          std::span<const uint8_t> frozen, const MultiresStepOptions& options,
                          std::span<MrEnergyDelta> deltas) {
            const CellFaces faces = CollectCellFaces(nest, thermals, slot, index, frozen, options);
            MrEnergyDelta own = MrMakeEnergyDelta();
            own.whole = -faces.sameLevelOutflow;
            for (uint32_t i = 0; i < faces.crossCount; ++i) {
                const MrFaceNeighbor& coarse = faces.crossNeighbors[i];
                const MrBlock& coarseBlock = nest.blocks[coarse.slot];
                const MrCrossTransfer transfer = MrSplitCrossFlow(faces.crossFlows[i], coarse.gap,
                                                                  coarseBlock.fraction != MR_NO_FRACTION);
                if (transfer.fineDelta == 0)
                    continue;

                FX_ASSERT(!MrIsUniform(coarseBlock));
                own.whole += transfer.fineDelta;
                MrEnergyDelta& target = deltas[nest_detail::PageCellAddress(nest, coarseBlock.page, coarse.index)];
                target = MrAddEnergyDelta(target, transfer.coarseDelta);
            }

            MrEnergyDelta& entry = deltas[nest_detail::PageCellAddress(nest, nest.blocks[slot].page, index)];
            entry = MrAddEnergyDelta(entry, own);
        }

        // 小刻み substep に小刻みが始まるブロックだけを刻む印
        void MarkSubstepBlocks(const MultiresNest& nest, std::span<const uint8_t> stepped, uint32_t substep,
                               const MultiresStepOptions& options, std::span<uint8_t> active) {
            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                const uint32_t shift = SubcycleShift(options, nest.blocks[slot].level);
                const bool begins = MrSubstepBegins(substep, shift, options.maxSubcycleGap);
                active[slot] = stepped[slot] != 0 && begins ? 1 : 0;
            }
        }

        // 小刻み substep に小刻みが終わるレベルの頁のブロックに、溜めた変化を足して表を 0 に戻す。変わったブロックの枠を返す(枠の順)
        std::vector<uint32_t> ApplyEndingDeltas(MultiresNest& nest, CellThermals& thermals, uint32_t substep,
                                                const MultiresStepOptions& options, std::span<MrEnergyDelta> deltas) {
            std::vector<uint32_t> changed;
            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                const MrBlock block = nest.blocks[slot];
                const uint32_t shift = SubcycleShift(options, block.level);
                if (MrIsUniform(block) || !MrSubstepEnds(substep, shift, options.maxSubcycleGap))
                    continue;

                bool blockChanged = false;
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    MrEnergyDelta& delta = deltas[nest_detail::PageCellAddress(nest, block.page, index)];
                    if (!MrIsSteppedCell(block, index) || MrEnergyDeltaIsZero(delta))
                        continue;

                    nest_detail::ApplyEnergyDelta(nest, slot, index, nest_detail::CellAt(nest, slot, index), delta);
                    delta = MrMakeEnergyDelta();
                    blockChanged = true;
                }

                if (!blockChanged)
                    continue;

                thermals.Invalidate(slot);
                changed.push_back(slot);
            }

            return changed;
        }

        // 小刻みで変わったブロックの面の隣を起こし、以後の小刻みから刻む(活性。wakeMark = この刻みの印)
        void WakeChangedBlocks(MultiresNest& nest, std::span<const uint32_t> changed, uint32_t wakeMark,
                               std::span<uint8_t> stepped) {
            for (const uint32_t slot : changed) {
                if (slot < nest.capacity.worldBlocks && nest.blocks[slot].kind == MR_BLOCK_REAL)
                    nest_detail::WakeAround(nest, slot, wakeMark);
            }

            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                if (nest.blocks[slot].activeTick == wakeMark)
                    stepped[slot] = 1;
            }
        }

    }  // namespace

    namespace nest_detail {

        CellThermals::CellThermals(const MultiresNest& nest, const ReactionTableView& view)
            : m_nest(&nest),
              m_view(view),
              m_values(nest.blocks.size() * (MR_BLOCK_CELLS + 1)),
              m_known(nest.blocks.size() * (MR_BLOCK_CELLS + 1), 0) {}

        MrThermal CellThermals::At(uint32_t slot, uint32_t index) {
            const bool uniform = MrIsUniform(m_nest->blocks[slot]);
            const size_t address = (size_t{slot} * (MR_BLOCK_CELLS + 1)) + (uniform ? MR_BLOCK_CELLS : index);
            if (m_known[address] == 0) {
                m_values[address] = MrCellThermal(m_view, LoadNestCell(*m_nest, slot, index));
                m_known[address] = 1;
            }

            return m_values[address];
        }

        void CellThermals::Invalidate(uint32_t slot) {
            const auto first = static_cast<std::ptrdiff_t>(size_t{slot} * (MR_BLOCK_CELLS + 1));
            std::fill(m_known.begin() + first, m_known.begin() + first + MR_BLOCK_CELLS + 1, uint8_t{0});
        }

        std::vector<uint8_t> MarkConductionWants(MultiresNest& nest, CellThermals& thermals,
                                                 std::span<const uint8_t> stepped, const MultiresStepOptions& options) {
            std::vector<uint8_t> wantsFraction(nest.blocks.size(), 0);
            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                if (IsSteppedSlot(stepped, slot))
                    MarkBlockWants(nest, thermals, slot, wantsFraction, options);
            }

            return wantsFraction;
        }

        void ComputeConduction(MultiresNest& nest, CellThermals& thermals, std::span<const uint8_t> stepped,
                               std::span<const uint8_t> frozen, std::span<const uint8_t> wantsFraction,
                               const MultiresStepOptions& options, std::span<MrEnergyDelta> deltas) {
            AllocateConductionFractions(nest, frozen, wantsFraction);

            // --- 刻む頁のブロック(凍らせたものを除く)のセルの面の流れ ---
            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                const MrBlock& block = nest.blocks[slot];
                if (!IsSteppedSlot(stepped, slot) || IsFrozen(frozen, slot) || MrIsUniform(block))
                    continue;

                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    if (MrIsSteppedCell(block, index))
                        AddCellFlows(nest, thermals, slot, index, frozen, options, deltas);
                }
            }
        }

        std::vector<MrEnergyDelta> StepConduction(MultiresNest& nest, const ReactionTableView& view,
                                                  std::span<uint8_t> stepped, const MultiresStepOptions& options,
                                                  uint32_t wakeMark, std::span<BlockStepResult> results) {
            FX_ASSERT(options.maxSubcycleGap <= MULTIRES_MAX_SUBCYCLE_GAP);
            const uint32_t substeps = 1u << (2 * options.maxSubcycleGap);
            CellThermals thermals(nest, view);
            std::vector<MrEnergyDelta> deltas(nest.cells.size(), MrMakeEnergyDelta());
            std::vector<uint8_t> active(nest.blocks.size(), 0);
            for (uint32_t substep = 0;; ++substep) {
                // --- 印 → 頁 → 端数の枠と流れ(T-0019 の 1 刻みと同じ順)---
                MarkSubstepBlocks(nest, stepped, substep, options, active);
                const std::vector<uint8_t> wantsFraction = MarkConductionWants(nest, thermals, active, options);
                const std::vector<uint8_t> frozen = ExpandForStep(nest, results);
                ComputeConduction(nest, thermals, active, frozen, wantsFraction, options, deltas);
                if (substep + 1 == substeps)
                    return deltas;

                // --- 小刻みが終わるレベルに変化を足す。変わったブロックの隣を起こす(活性)---
                const std::vector<uint32_t> changed = ApplyEndingDeltas(nest, thermals, substep, options, deltas);
                for (const uint32_t slot : changed)
                    results[slot].changed = true;

                if (wakeMark != 0)
                    WakeChangedBlocks(nest, changed, wakeMark, stepped);
            }
        }

    }  // namespace nest_detail

}  // namespace bicameral::sim
