// multires_conduction.cpp — 多重解像度の木の上の熱の伝導の CPU リファレンス(07 §1・17 §5「熱の伝導」。T-0019)。
// 式と面の隣の探し方は shaders/common/multires_conduction.hlsli(GPU も同じ関数を呼ぶ。T-0107)。
// 1 刻みの順(multires_nest.cpp の StepBlocks から呼ぶ):
//   1. MarkConductionWants: 刻むブロックのセルの面の流れを調べ、流れのある一様なブロックに MR_PAGE_WANTED を付け、
//      端数が要る粗いブロックに印を付ける(どちらも「端数の枠がある」とした場合の流れで決める)
//   2. (呼ぶ側)頁を枠の順に配る。足りなかったブロックはこの刻みは凍らせる(刻まず、面の流れも 0)
//   3. ComputeConduction: 端数の枠を枠の順に配り、刻むブロックのセルの面の流れを変化の表に足す
//   4. (呼ぶ側)変化を足してから反応
// 流れはどれも刻みの初めのセルから計算する(Jacobi 型。順に依存しない)。頁に広げても論理のセルは変わらないので、
// セルの見立て(cell_view.hlsli の MakeCellView)は刻みの中で 1 回だけ作って使い回す(CellViews。永続しない)。
// 面の流れと当てる順(流す → 上限 → 当てる → 欄 → 帳簿 → 起こす)は運ぶ計算の共通の土台の当てる段(common/transport_apply.hlsli の
// TxFlowCell。T-0714・ADR-0334)、伝導の式は流れの種類 TxConduction(common/transport_conduction.hlsli)。ここは回し方(世界の約束)だけ。
// 同じレベルの面は両側が同じ式で逆向きの値を出す(どちらも刻むことは活性が保証する。17 §5「活性」)。違うレベルの面は細かい側だけが
// 計算して、粗いセルの変化(整数部 + 端数)を表に足す。浮動小数点は使わない(engine/src/sim は検査の対象)。
// 細かいレベルの刻み(T-0108。StepConduction): レベル k の伝導を 1 刻みに 4^σ(k) 回の小刻みに分け、上の 1〜3 を小刻みごとに繰り返す。
// 小刻みでは、その回に小刻みが始まるレベルのブロックだけが流れを計算し、変化は表に溜めて、そのレベルの小刻みの終わりに足す
// (粗い側の値は粗い小刻みの初めのまま。細かい側が送った流れの合計を粗い側が 1 回で受ける = Berger–Colella の refluxing の形)。
// 活性(StepActive)では、小刻みで変わったブロックの面の隣を起こし、以後の小刻みから刻む(流れのある面は両側とも刻む、を小刻みでも保つ)。
// 細かいレベルの熱の陰解法(T-0119。options.implicitConduction): 基準より細かい本物のブロックの面はここでは計算せず、最後に
// AddImplicitConduction(multires_implicit_conduction.cpp)が陰解法の 1 刻みで解いて変化の表に足す(印と頁・端数の枠はここと同じ)。
// 世界の端(T-0721・ADR-0337。common/world_edge.hlsli): 面の先が開いた端なら面の隣は MR_NEIGHBOR_EDGE(外の柱の行)で、当てる段が
// 外のセル(World::Outside)を同じレベルの相手として流し、流れた分を世界の帳簿の「世界の端」へ載せる(World::AddEdge)。
#include <algorithm>
#include <array>
#include <memory>

#include "common/transport_conduction.hlsli"
#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        using nest_detail::CellViews;
        using nest_detail::CpuTree;

        // レベル level の小刻みの数の指数(4^σ 回。T-0108)
        uint32_t SubcycleShift(const MultiresStepOptions& options, int32_t level) {
            return MrSubcycleShift(level, options.subcycleBaseLevel, options.maxSubcycleGap);
        }

        // 伝導の流れの種類(安定の条件の値は options の小刻み。T-0108)
        TxConduction MakeConductionKind(const MultiresStepOptions& options) {
            return TxMakeConduction(options.subcycleBaseLevel, options.maxSubcycleGap);
        }

        bool IsFrozen(std::span<const uint8_t> frozen, uint32_t slot) {
            return !frozen.empty() && frozen[slot] != 0;
        }

        // 伝導の世界(transport_apply.hlsli の World の約束): 面の隣・刻みの初めの見立て・端数の置き場・変化の表。
        // 印の回(MarkConductionWants)は凍らせ・陰解法の印・変化の表を持たない
        struct ConductionWorld {
            const MultiresNest* nest = nullptr;
            CellViews* views = nullptr;
            std::span<const uint8_t> frozen;
            std::span<const uint8_t> implicitBlocks;
            std::span<MrEnergyDelta> deltas;
            MultiresNest* ledger = nullptr;  // 帳簿を書く世界(当てる回だけ。印の回は帳簿を書かない)

            // 細かい側の面(相手が計算する)は種類のまま、世界の外は開いた端なら MR_NEIGHBOR_EDGE(外の柱の行)・wall なら NONE を返す(T-0721)。凍らせたブロックとの面と、陰解法で解くブロックとの同じレベルの面
            // (こちらのセルが境のセルとして陰解法の系で受ける。T-0178)は流さない
            [[nodiscard]] MrFaceNeighbor Neighbor(uint32_t slot, const MrBlock& block, uint32_t index,
                                                  uint32_t face) const {
                const CpuTree tree{.nest = nest};
                const MrFaceNeighbor neighbor = MrFindFaceNeighbor(tree, slot, block, index, face,
                                                                   nest->capacity.rootLevel);
                if (neighbor.kind == MR_NEIGHBOR_NONE) {
                    const MultiresWorldEdges& edges = nest->edges;
                    return WeEdgeNeighbor(edges.world, edges.columnBaseUp, static_cast<uint32_t>(edges.column.size()),
                                          block, index, face);
                }

                if (neighbor.kind != MR_NEIGHBOR_SAME && neighbor.kind != MR_NEIGHBOR_COARSER)
                    return neighbor;

                const MrFaceNeighbor none = MrMakeFaceNeighbor(MR_NEIGHBOR_NONE, MR_NO_BLOCK, 0, 0);
                if (IsFrozen(frozen, neighbor.slot))
                    return none;

                const bool joinsImplicit = slot < nest->capacity.worldBlocks && block.kind == MR_BLOCK_REAL;
                if (neighbor.kind == MR_NEIGHBOR_SAME && joinsImplicit &&
                    nest_detail::IsImplicitBlock(implicitBlocks, neighbor.slot))
                    return none;

                return neighbor;
            }

            // 見立ては置き場の参照で渡す(面ごとに写さない。T-0767)
            [[nodiscard]] const CvCellView& View(uint32_t slot, uint32_t index) const {
                return views->View(slot, index);
            }

            // 開いた世界の端の外のセルの見立て(外の柱の行 row を自分と同じレベル level で。T-0721)
            [[nodiscard]] CvCellView Outside(uint32_t row, int32_t level) const { return views->Outside(row, level); }
            [[nodiscard]] int32_t Level(uint32_t slot) const { return nest->blocks[slot].level; }
            [[nodiscard]] bool HasFraction(uint32_t slot) const {
                return nest->blocks[slot].fraction != MR_NO_FRACTION;
            }

            // 伝導は上限の段を使わない(C/8 の安定の条件で行き過ぎない)・欄を持たない
            [[nodiscard]] TxLimit Limit(uint32_t /*slot*/, uint32_t /*index*/) const { return TxNoLimit(); }
            void FollowFields(uint32_t /*slot*/, uint32_t /*index*/) const {}

            // 当てる: 頁のセルの変化の表に書き足す(粗い側も頁に広げてある。MarkConductionWants の印)
            void Add(uint32_t slot, uint32_t index, const MrEnergyDelta& delta) const {
                nest_detail::AddConductionDelta(*nest, deltas, slot, index, delta);
            }

            // 5 帳簿: 開いた端の面の流れ outflow(出ていくのが正。枠 slot のレベルの単位)を世界の帳簿の「世界の端」のエネルギーの列へ
            // (出 = 外へ出た・入 = 外から入った。T-0762 の帳簿の書き方の約束)
            void AddEdge(uint32_t slot, uint32_t /*index*/, int64_t outflow) const {
                if (outflow == 0)
                    return;

                FX_ASSERT(ledger != nullptr);
                const WlSignedEntry entry = WlEnergyEntry(outflow, WL_DIRECTION_OUT);
                AddWorldLedgerUnits(*ledger, WlSlot(WL_REASON_WORLD_EDGE, entry.direction), nest->blocks[slot].level,
                                    WL_COLUMN_ENERGY, entry.magnitude);
            }
        };

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

        // 面の先が自分のブロックの外か(ブロックの中の面の隣はいつも同じレベル。粗い隣は外にしかない。MrFindFaceNeighbor)
        bool LeavesBlock(uint32_t index, uint32_t face) {
            constexpr uint32_t LAST = MR_BLOCK_EDGE - 1;
            const uint32_t axis = face >> 1;
            const uint32_t coordinate = axis == 0 ? MrCellX(index) : (axis == 1 ? MrCellY(index) : MrCellZ(index));

            return (face & 1u) != 0 ? coordinate == LAST : coordinate == 0;
        }

        // 粗い側へ送る面 1 つの印(送るなら相手も変わるので一様なら頁に広げる。端数が要るなら相手に印)。送るなら true。
        // 流れは端数の枠があるとした場合
        bool MarkCoarseSend(MultiresNest& nest, int64_t flow, const MrFaceNeighbor& neighbor,
                            std::span<uint8_t> wantsFraction) {
            const MrCrossTransfer transfer = MrSplitCrossFlow(flow, neighbor.gap, true);
            if (transfer.fineDelta == 0)
                return false;

            if (nest.blocks[neighbor.slot].page == MR_NO_PAGE)
                nest.blocks[neighbor.slot].page = MR_PAGE_WANTED;

            if (transfer.coarseDelta.fraction != 0)
                wantsFraction[neighbor.slot] = 1;

            return true;
        }

        // 頁の無い(一様な)ブロックのセル 1 つの面の流れを全部調べ、粗い側へ送る面を印にする。
        // 自分が変わるなら true(同じレベルの面の流れの和が 0 でないか、粗い側へ送る)
        bool MarkCellWants(MultiresNest& nest, const TxConduction& kind, const ConductionWorld& world, uint32_t slot,
                           uint32_t index, std::span<uint8_t> wantsFraction) {
            const MrBlock block = nest.blocks[slot];
            const TxSelfSide side = TxMakeSelfSide(kind, world.View(slot, index), block.level);
            int64_t sameLevelOutflow = 0;
            bool sends = false;
            for (uint32_t face = 0; face < MR_FACES; ++face) {
                const MrFaceNeighbor neighbor = world.Neighbor(slot, block, index, face);
                if (neighbor.kind != MR_NEIGHBOR_SAME && neighbor.kind != MR_NEIGHBOR_COARSER &&
                    neighbor.kind != MR_NEIGHBOR_EDGE)
                    continue;

                // 開いた世界の端の面も自分だけが変わる(同じレベルの面と同じ数え方。T-0721)
                const int64_t flow = TxSideFaceFlow(kind, world, side, neighbor);
                if (neighbor.kind == MR_NEIGHBOR_SAME || neighbor.kind == MR_NEIGHBOR_EDGE) {
                    sameLevelOutflow += flow;
                    continue;
                }

                const bool sent = MarkCoarseSend(nest, flow, neighbor, wantsFraction);
                sends = sends || sent;
            }

            return sends || sameLevelOutflow != 0;
        }

        // 頁のあるブロックのセル 1 つ: ブロックの外へ出る面のうち粗い隣の面だけを流して印にする。頁のあるブロックは自分が変わるかを
        // 数えなくてよい(頁はもうある)ので、同じレベルの面・世界の端の面は流さない(同じ印を、面の約 1/8 の手間で。T-0768)
        void MarkCellCoarseSends(MultiresNest& nest, const TxConduction& kind, const ConductionWorld& world,
                                 uint32_t slot, uint32_t index, std::span<uint8_t> wantsFraction) {
            const MrBlock block = nest.blocks[slot];
            TxSelfSide side{};
            bool sideMade = false;
            for (uint32_t face = 0; face < MR_FACES; ++face) {
                if (!LeavesBlock(index, face))
                    continue;

                const MrFaceNeighbor neighbor = world.Neighbor(slot, block, index, face);
                if (neighbor.kind != MR_NEIGHBOR_COARSER)
                    continue;

                if (!sideMade) {
                    side = TxMakeSelfSide(kind, world.View(slot, index), block.level);
                    sideMade = true;
                }

                MarkCoarseSend(nest, TxSideFaceFlow(kind, world, side, neighbor), neighbor, wantsFraction);
            }
        }

        // 刻むブロック 1 つの面の流れを調べ、印を付ける。頁が無くて(一様で)流れがあれば自分も頁に広げる。
        // 調べるのは面に接するセルだけ(一様なブロックの中のセルどうしは同じ値・粗い隣はブロックの外にしかない)
        void MarkBlockWants(MultiresNest& nest, CellViews& views, uint32_t slot, std::span<uint8_t> wantsFraction,
                            const MultiresStepOptions& options) {
            const TxConduction kind = MakeConductionKind(options);
            const ConductionWorld world{.nest = &nest, .views = &views};
            const bool needsPage = nest.blocks[slot].page == MR_NO_PAGE;
            bool flows = false;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (!MrIsSteppedCell(nest.blocks[slot], index) || !OnBlockSurface(index))
                    continue;

                if (!needsPage) {
                    MarkCellCoarseSends(nest, kind, world, slot, index, wantsFraction);
                    continue;
                }

                const bool changes = MarkCellWants(nest, kind, world, slot, index, wantsFraction);
                flows = flows || changes;
            }

            if (flows)
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
        std::vector<uint32_t> ApplyEndingDeltas(MultiresNest& nest, CellViews& views, uint32_t substep,
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
                    if (!MrIsSteppedCell(block, index) || !TxWakes(delta))
                        continue;

                    nest_detail::ApplyEnergyDelta(nest, slot, index, nest_detail::CellAt(nest, slot, index), delta);
                    delta = MrMakeEnergyDelta();
                    blockChanged = true;
                }

                if (!blockChanged)
                    continue;

                views.Invalidate(slot);
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

        CellViews::CellViews(MultiresNest& nest, const ReactionTableView& view, std::span<const CvSpeciesPhase> phases,
                             uint32_t parts)
            : m_nest(&nest), m_table{.reaction = view, .phases = phases}, m_parts(parts), m_pages(nest.blocks.size()) {
            // 取っておきは木の写しどうしで共有しない(写した木は自分の取っておきを作る)
            if (!nest.conductionViewPool || nest.conductionViewPool.use_count() != 1)
                nest.conductionViewPool = std::make_shared<CellViewPool>();

            m_pool = nest.conductionViewPool.get();
        }

        CellViews::~CellViews() {
            for (std::unique_ptr<CellViewPage>& page : m_pages) {
                if (page)
                    m_pool->free.push_back(std::move(page));
            }
        }

        void CellViews::Build(uint32_t slot, uint32_t index, uint32_t position) {
            std::unique_ptr<CellViewPage>& page = m_pages[slot];
            if (!page) {
                // 前の刻みの塊を使い回す(無ければ取る。0 で埋めない = 作ったかは known。T-0767・T-0768)
                if (m_pool->free.empty())
                    page = std::make_unique_for_overwrite<CellViewPage>();
                else {
                    page = std::move(m_pool->free.back());
                    m_pool->free.pop_back();
                }

                page->known.fill(0);
            }

            // 溢れを使う世界は溢れの成分も入れる(T-0187)。伝導は圧力を読まないので基準の圧力は 0。
            // 作るのは読む種類の部分だけ(m_parts。伝導はセル全体の熱だけ。T-0767)
            const MrBlock& block = m_nest->blocks[slot];
            page->views[position] = m_nest->wideCells
                                        ? MakeCellViewParts(m_table, LoadWideNestCell(*m_nest, slot, index),
                                                            block.level, 0, m_parts)
                                        : MakeCellViewParts(m_table, LoadNestCell(*m_nest, slot, index), block.level, 0,
                                                            m_parts);
            page->known[position] = 1;
        }

        CvCellView CellViews::Outside(uint32_t row, int32_t level) const {
            return MakeCellViewParts(m_table, m_nest->edges.column[row], level, 0, m_parts);
        }

        void CellViews::Invalidate(uint32_t slot) {
            if (m_pages[slot])
                m_pages[slot]->known.fill(0);
        }

        std::vector<uint8_t> MarkConductionWants(MultiresNest& nest, CellViews& views, std::span<const uint8_t> stepped,
                                                 const MultiresStepOptions& options) {
            std::vector<uint8_t> wantsFraction(nest.blocks.size(), 0);
            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                if (IsSteppedSlot(stepped, slot))
                    MarkBlockWants(nest, views, slot, wantsFraction, options);
            }

            return wantsFraction;
        }

        void ComputeConduction(MultiresNest& nest, CellViews& views, std::span<const uint8_t> stepped,
                               std::span<const uint8_t> frozen, std::span<const uint8_t> wantsFraction,
                               std::span<const uint8_t> implicitBlocks, const MultiresStepOptions& options,
                               std::span<MrEnergyDelta> deltas) {
            AllocateConductionFractions(nest, frozen, wantsFraction);

            // --- 刻む頁のブロック(凍らせたものを除く)のセルの面の流れを当てる段で(transport_apply.hlsli の TxFlowCell)---
            const TxConduction kind = MakeConductionKind(options);
            const ConductionWorld world{.nest = &nest,
                                        .views = &views,
                                        .frozen = frozen,
                                        .implicitBlocks = implicitBlocks,
                                        .deltas = deltas,
                                        .ledger = &nest};
            for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
                const MrBlock& block = nest.blocks[slot];
                if (!IsSteppedSlot(stepped, slot) || IsFrozen(frozen, slot) || MrIsUniform(block) ||
                    IsImplicitBlock(implicitBlocks, slot))
                    continue;

                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    if (MrIsSteppedCell(block, index))
                        TxFlowCell(kind, world, slot, block, index);
                }
            }
        }

        std::vector<MrEnergyDelta> StepConduction(MultiresNest& nest, const ReactionTableView& view,
                                                  std::span<const CvSpeciesPhase> phases, std::span<uint8_t> stepped,
                                                  const MultiresStepOptions& options, uint32_t wakeMark,
                                                  std::span<BlockStepResult> results) {
            FX_ASSERT(options.maxSubcycleGap <= MULTIRES_MAX_SUBCYCLE_GAP);
            FX_ASSERT(!options.implicitConduction || options.maxSubcycleGap == 0);
            const uint32_t substeps = 1u << (2 * options.maxSubcycleGap);
            CellViews views(nest, view, phases, MakeConductionKind(options).ViewParts());
            std::vector<MrEnergyDelta> deltas(nest.cells.size(), MrMakeEnergyDelta());
            std::vector<uint8_t> active(nest.blocks.size(), 0);
            for (uint32_t substep = 0;; ++substep) {
                // --- 印 → 頁 → 端数の枠と流れ(T-0019 の 1 刻みと同じ順)---
                MarkSubstepBlocks(nest, stepped, substep, options, active);
                const std::vector<uint8_t> wantsFraction = MarkConductionWants(nest, views, active, options);
                const std::vector<uint8_t> frozen = ExpandForStep(nest, results);
                const std::vector<uint8_t> implicitBlocks = MarkImplicitBlocks(nest, views, frozen, options);
                ComputeConduction(nest, views, active, frozen, wantsFraction, implicitBlocks, options, deltas);
                if (substep + 1 == substeps) {
                    if (options.implicitConduction)
                        AddImplicitConduction(nest, views, frozen, implicitBlocks, options, deltas);

                    return deltas;
                }

                // --- 小刻みが終わるレベルに変化を足す。変わったブロックの隣を起こす(活性)---
                const std::vector<uint32_t> changed = ApplyEndingDeltas(nest, views, substep, options, deltas);
                for (const uint32_t slot : changed)
                    results[slot].changed = true;

                if (wakeMark != 0)
                    WakeChangedBlocks(nest, changed, wakeMark, stepped);
            }
        }

    }  // namespace nest_detail

}  // namespace bicameral::sim
