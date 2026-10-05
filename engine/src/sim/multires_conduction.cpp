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
#include <array>

#include "common/multires_conduction.hlsli"
#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        using nest_detail::CpuTree;

        // 刻みの初めの論理のセルの熱(初めて読んだ時に作る)。一様なブロックは覆われていないセルが全部同じなので枠ごとに 1 つ
        // (読むのは刻むセルと、その面の先の覆われていないセルだけ)
        class CellThermals {
        public:
            CellThermals(const MultiresNest& nest, const ReactionTableView& view)
                : m_nest(&nest),
                  m_view(view),
                  m_values(nest.blocks.size() * (MR_BLOCK_CELLS + 1)),
                  m_known(nest.blocks.size() * (MR_BLOCK_CELLS + 1), 0) {}

            [[nodiscard]] MrThermal At(uint32_t slot, uint32_t index) {
                const bool uniform = MrIsUniform(m_nest->blocks[slot]);
                const size_t address = (size_t{slot} * (MR_BLOCK_CELLS + 1)) + (uniform ? MR_BLOCK_CELLS : index);
                if (m_known[address] == 0) {
                    m_values[address] = MrCellThermal(m_view, LoadNestCell(*m_nest, slot, index));
                    m_known[address] = 1;
                }

                return m_values[address];
            }

        private:
            const MultiresNest* m_nest;
            ReactionTableView m_view;
            std::vector<MrThermal> m_values;  // 枠ごとに [セル × 512][一様の値]
            std::vector<uint8_t> m_known;
        };

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
                                   std::span<const uint8_t> frozen) {
            const CpuTree tree{.nest = &nest};
            const MrBlock& block = nest.blocks[slot];
            const MrThermal self = thermals.At(slot, index);

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
                    faces.sameLevelOutflow += MrSameLevelFlow(self, other, block.level);
                    continue;
                }

                const int64_t flow = MrCrossLevelFlow(self, other, block.level, neighbor.gap);
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
        void MarkBlockWants(MultiresNest& nest, CellThermals& thermals, uint32_t slot,
                            std::span<uint8_t> wantsFraction) {
            const bool uniform = MrIsUniform(nest.blocks[slot]);
            bool flows = false;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (!MrIsSteppedCell(nest.blocks[slot], index) || (uniform && !OnBlockSurface(index)))
                    continue;

                const CellFaces faces = CollectCellFaces(nest, thermals, slot, index, {});
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
                          std::span<const uint8_t> frozen, std::span<MrEnergyDelta> deltas) {
            const CellFaces faces = CollectCellFaces(nest, thermals, slot, index, frozen);
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

    }  // namespace

    namespace nest_detail {

        std::vector<uint8_t> MarkConductionWants(MultiresNest& nest, const ReactionTableView& view,
                                                 std::span<const uint8_t> stepped) {
            CellThermals thermals(nest, view);
            std::vector<uint8_t> wantsFraction(nest.blocks.size(), 0);
            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                if (IsSteppedSlot(stepped, slot))
                    MarkBlockWants(nest, thermals, slot, wantsFraction);
            }

            return wantsFraction;
        }

        std::vector<MrEnergyDelta> ComputeConduction(MultiresNest& nest, const ReactionTableView& view,
                                                     std::span<const uint8_t> stepped, std::span<const uint8_t> frozen,
                                                     std::span<const uint8_t> wantsFraction) {
            AllocateConductionFractions(nest, frozen, wantsFraction);

            // --- 刻む頁のブロック(凍らせたものを除く)のセルの面の流れ ---
            CellThermals thermals(nest, view);
            std::vector<MrEnergyDelta> deltas(nest.cells.size(), MrMakeEnergyDelta());
            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                const MrBlock& block = nest.blocks[slot];
                if (!IsSteppedSlot(stepped, slot) || IsFrozen(frozen, slot) || MrIsUniform(block))
                    continue;

                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    if (MrIsSteppedCell(block, index))
                        AddCellFlows(nest, thermals, slot, index, frozen, deltas);
                }
            }

            return deltas;
        }

    }  // namespace nest_detail

}  // namespace bicameral::sim
