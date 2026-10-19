// multires_liquid.cpp — 多重解像度の世界の 1 段で液体を流す CPU(運ぶ計算の共通の土台の段 2 ②。T-0717)。約束は multires_liquid.h。
// 1 刻み = 一覧と面の先 → 見立て(セルと欄を読む)→ 欄の合わせ直し → 圧力 → 面の力 → 上限と当てる → 運動量を運ぶ → エネルギーを熱へ
// (+ 静止の丸め)→ 書き戻す。面の先は世界の Neighbor(MrFindFaceNeighbor・WeEdgeNeighbor。T-0771・T-0774)、縦は WE_UP_AXIS。
// 式は common/liquid_flow.hlsli(仮の世界 sim/probe_liquid.cpp と同じ関数)。回し方も仮の世界と同じ Jacobi 型(刻みの初めの値だけから決め、
// 足し引きは整数なので順に依存しない)なので、セルを並べる順(ブロックの順 × ブロックの中の番号)が仮の世界と違っても値はビット単位で同じ。
#include "sim/multires_liquid.h"

#include <algorithm>
#include <span>

#include "common/fixed.hlsli"
#include "common/rest_rounding.hlsli"
#include "core/aliases.h"
#include "sim/cell_view.h"
#include "sim/multires_fields.h"
#include "sim/multires_nest_internal.h"
#include "sim/transport_packet.h"

namespace bicameral::sim {

    namespace {

        using liquid::LqCellView;
        using multires::MR_BLOCK_CELLS;
        using multires::MR_FACES;
        namespace sf = state_field;

        constexpr uint64_t TICK_RATE = 60;                     // 刻み / 秒
        constexpr uint64_t LEVEL_ZERO_EDGE_MILLIMETERS = 500;  // レベル 0 のセルの 1 辺(仮の世界と同じ)
        constexpr uint32_t NO_CELL = UINT32_MAX;
        constexpr uint32_t EDGE_LINK = 0x80000000u;         // 面の先が開いた世界の端(下の桁 = 外の柱の行)
        constexpr uint32_t UP_AXIS = multires::WE_UP_AXIS;  // 世界の上の軸(D-518。縦の面・柱・高さ。T-0774)

        // + の面・− の面の番号(TxFlowCell の 6 面: 軸 = face / 2、正の向き = face % 2)
        constexpr uint32_t PlusFace(uint32_t axis) {
            return (2 * axis) + 1;
        }

        constexpr uint32_t MinusFace(uint32_t axis) {
            return 2 * axis;
        }

        // --- 1 段の世界のセルの並び(刻むブロックの順 × ブロックの中の番号)と面の先(世界の Neighbor から。T-0771)---
        // 面の先(links の値): セルの番号 / NO_CELL = 閉じた面(wall の端・真空の開いた端・木の穴)/ EDGE_LINK | 行 = 開いた世界の端
        struct LiquidGrid {
            std::vector<uint32_t> slots;                        // 刻むブロックの枠(枠の順)
            std::array<std::vector<uint32_t>, MR_FACES> links;  // 面ごとに セル → 面の先
            std::vector<int64_t> originUp;                      // ブロックの順 → 原点の縦の座標(セル)
            int64_t floorUp = 0;  // 一番下のブロックの原点の縦の座標(位置のエネルギーの高さの元)

            [[nodiscard]] uint32_t Count() const { return (uint32_t)slots.size() * MR_BLOCK_CELLS; }

            [[nodiscard]] static bool IsCell(uint32_t link) { return link < EDGE_LINK; }
            [[nodiscard]] static bool IsEdge(uint32_t link) { return link != NO_CELL && link >= EDGE_LINK; }
            [[nodiscard]] static uint32_t EdgeRow(uint32_t link) { return link & ~EDGE_LINK; }
            [[nodiscard]] static uint32_t CellOf(uint32_t link) { return IsCell(link) ? link : NO_CELL; }

            [[nodiscard]] uint32_t Link(uint32_t cell, uint32_t face) const { return links[face][cell]; }

            // + 側・− 側の隣のセル(セルでなければ NO_CELL。開いた端は Link で見る)
            [[nodiscard]] uint32_t Plus(uint32_t cell, uint32_t axis) const {
                return CellOf(links[PlusFace(axis)][cell]);
            }

            [[nodiscard]] uint32_t Minus(uint32_t cell, uint32_t axis) const {
                return CellOf(links[MinusFace(axis)][cell]);
            }

            // セルの高さ(一番下のブロックの段から数えたセルの数。縦は WE_UP_AXIS)
            [[nodiscard]] uint32_t HeightCells(uint32_t cell) const {
                const int64_t local = multires::WeCellAxis(cell % MR_BLOCK_CELLS, UP_AXIS);

                return (uint32_t)(originUp[cell / MR_BLOCK_CELLS] + local - floorUp);
            }
        };

        // 枠 slot(見出し block)のセル index の面 face の先(世界の Neighbor。T-0771): 同じレベルの隣のセル・開いた世界の端
        // (外の柱の行。WeEdgeNeighbor = 伝導と同じ。T-0774)・閉じた面
        uint32_t FaceLink(const MultiresNest& nest, std::span<const uint32_t> order, uint32_t slot,
                          const multires::MrBlock& block, uint32_t index, uint32_t face) {
            const nest_detail::CpuTree tree{.nest = &nest};
            const multires::MrFaceNeighbor neighbor = multires::MrFindFaceNeighbor(tree, slot, block, index, face,
                                                                                   nest.capacity.rootLevel);
            if (neighbor.kind == multires::MR_NEIGHBOR_SAME) {
                FX_ASSERT(order[neighbor.slot] != NO_CELL);
                return (order[neighbor.slot] * MR_BLOCK_CELLS) + neighbor.index;
            }

            // 違うレベルの面は 1 段の世界に無い(断ったまま。BuildGrid が根のレベル・子なしを確かめた)
            FX_ASSERT(neighbor.kind == multires::MR_NEIGHBOR_NONE);
            const MultiresWorldEdges& edges = nest.edges;
            const multires::MrFaceNeighbor edge = multires::WeEdgeNeighbor(
                edges.world, edges.columnBaseUp, (uint32_t)edges.column.size(), block, index, face);
            if (edge.kind != multires::MR_NEIGHBOR_EDGE)
                return NO_CELL;

            return EDGE_LINK | edge.index;
        }

        // 刻むブロックの一覧(根のレベルの本物のブロック全部)と面の先。1 段の世界だけ(子・違うレベルは断る)
        LiquidGrid BuildGrid(const MultiresNest& nest) {
            LiquidGrid grid;
            std::vector<uint32_t> order(nest.blocks.size(), NO_CELL);
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                const multires::MrBlock& block = nest.blocks[slot];
                if (block.kind != multires::MR_BLOCK_REAL)
                    continue;

                FX_ASSERT(block.level == nest.capacity.rootLevel);
                FX_ASSERT(rng::all_of(block.children, [](uint32_t child) { return child == multires::MR_NO_BLOCK; }));
                order[slot] = (uint32_t)grid.slots.size();
                grid.slots.push_back(slot);
                grid.originUp.push_back(multires::WeOriginAxis(block, UP_AXIS));
            }

            if (!grid.originUp.empty())
                grid.floorUp = *rng::min_element(grid.originUp);

            for (std::vector<uint32_t>& links : grid.links)
                links.assign(grid.Count(), NO_CELL);

            for (uint32_t blockOrder = 0; blockOrder < grid.slots.size(); ++blockOrder) {
                const uint32_t slot = grid.slots[blockOrder];
                const multires::MrBlock& block = nest.blocks[slot];
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const uint32_t cell = (blockOrder * MR_BLOCK_CELLS) + index;
                    for (uint32_t face = 0; face < MR_FACES; ++face)
                        grid.links[face][cell] = FaceLink(nest, order, slot, block, index, face);
                }
            }

            return grid;
        }

        bool IsLiquid(std::span<const multires::CvSpeciesPhase> phases, uint32_t species) {
            return species < phases.size() && phases[species].phase == multires::CV_PHASE_LIQUID;
        }

        // セルの液体の質量(mg。見立ての liquidMass と同じ式)
        uint64_t LiquidMassMilligrams(std::span<const multires::CvSpeciesPhase> phases, const reaction::RxCell& cell) {
            uint64_t mass = 0;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                if (IsLiquid(phases, cell.species[i]))
                    mass += cell.amounts[i] * phases[cell.species[i]].molarMass;
            }

            return mass / multires::CV_NANOGRAMS_PER_MILLIGRAM;
        }

        // 液体の包みの運ぶ欄(TxPacket::carried の番号)
        constexpr uint32_t CARRIED_POTENTIAL = 0;  // 動く液体の位置のエネルギー(mJ)

        // 帳簿の 1 つの量を載せる(符号つき: 正 → direction・負 → 逆の向き。T-0762 の書き方の約束)
        void AddLedgerSigned(MultiresNest& nest, uint32_t reason, uint32_t direction, int32_t level, uint32_t column,
                             int64_t amount) {
            if (amount == 0)
                return;

            const multires::WlSignedEntry entry = multires::WlEnergyEntry(amount, direction);
            AddWorldLedgerUnits(nest, multires::WlSlot(reason, entry.direction), level, column, entry.magnitude);
        }

        // 運動量の列に載せる理由(報告の内訳の順。和が仮の世界の帳簿 external。世界の端は仮の世界に無いので 0)
        constexpr std::array<uint32_t, 8> MOMENTUM_REASONS = {
            multires::WL_REASON_WALL_IMPULSE, multires::WL_REASON_GRAVITY_WORK, multires::WL_REASON_FRICTION,
            multires::WL_REASON_NO_OUTLET,    multires::WL_REASON_SPEED_LIMIT,  multires::WL_REASON_FOLLOW_MASS,
            multires::WL_REASON_WORLD_EDGE,   multires::WL_REASON_REST_ROUNDING};

        // 面の記録: + 側の面の欄の持ち主(− 側のセル。− 側が開いた端なら + 側のセル)が ApplyForces で決めた向きと望む動く質量
        struct FaceRecord {
            uint64_t desired = 0;
            bool forward = false;      // + 向き(− 側 → + 側)
            uint32_t lower = NO_CELL;  // − 側(セルの番号か開いた端の先)
            uint32_t upper = NO_CELL;  // + 側
        };

        struct LiquidApplyWorld;

        // --- 1 刻み ---

        struct LiquidStep {
            const BakedReactionTable& table;
            std::span<const multires::CvSpeciesPhase> phases;
            const liquid::LqParams& params;
            const LiquidGrid& grid;
            uint32_t restSpeed = 0;  // 静止の丸めの速さ(µm/s。0 = 切る。T-0777)
            uint64_t tick = 0;       // 世界の刻み(1 mg までの面の順番 LqHoldTurn。T-0778)

            // --- 刻みの初めの値(セルと欄。書き戻すまで木は変えない)---
            std::vector<reaction::RxCell> cells;
            std::vector<LqCellView> states;
            std::array<std::vector<int64_t>, 3> faces;    // 面の運動量(mg)
            std::array<std::vector<int64_t>, 3> kinetic;  // 面の運動のエネルギー(mJ)
            std::array<std::vector<int64_t>, 3> carry;    // 面の持ち越す流れ(T-0778)
            std::vector<int64_t> potential;               // 位置のエネルギー(mJ)
            std::vector<int64_t> massBefore;              // 前の刻みの終わりの液体の質量(mg)
            std::vector<LqCellView> outside;              // 外の柱の行の見立て(開いた世界の端の相手。T-0774)
            std::vector<int64_t> outsidePressure;         // 外の柱の行の圧力(液体が無いので 0。式は内側と同じ)
            // 刻みの初めの面の運動量(合わせ直しの後。静止の丸め・開いた端が読む)
            std::array<std::vector<int64_t>, 3> facesBefore;

            // --- 作業 ---
            std::vector<int64_t> pressure;                 // 質量の単位(mg)
            std::vector<uint64_t> above;                   // 上から支えている液体(整数の釣り合いが使う。T-0781)
            std::array<std::vector<uint64_t>, 3> desired;  // 面の望む動く質量(mg)
            // − 側が開いた端の面の運動量(この刻みだけ。+ 側のセルが持つ)
            std::array<std::vector<int64_t>, 3> edgeFaces;
            std::array<std::vector<uint64_t>, 3> edgeDesired;  // その面の望む動く質量(mg)
            std::vector<uint64_t> outgoing;                    // セルから出る望む質量の合計
            std::vector<uint64_t> incoming;             // セルに入る望む質量の合計(入る側の上限。液体は器で切らない)
            std::vector<multires::TxLimit> limits;      // 当てる段の上限(セルごとの倍率)
            std::array<std::vector<int64_t>, 3> moved;  // 面を通って実際に動いた質量(mg。+ 向きが正)
            std::vector<TxCellChange> changes;          // 当てる段の包みの和(セルごと)
            TxCellChange edgeChange;  // 開いた世界の端から外へ出た包みの和(正 = 出た。帳簿の「世界の端」)
            std::vector<int64_t> potentialCarried;
            int64_t potentialHeat = 0;  // 位置のエネルギー → セルの熱(帳簿の「重さの仕事」)
            std::array<int64_t, 3>
                kineticHeat{};                  // 運動のエネルギー → セルの熱(軸ごと。帳簿の「物理の熱」+ 下の丸めの分)
            std::array<int64_t, 3> restHeat{};  // そのうち静止の丸めの分(帳簿の「静止の丸め」)
            std::array<std::vector<int64_t>, 3> kineticCarried;
            std::vector<uint8_t> changed;

            // 面の先の見立てと圧力(セルか開いた端の外のセル。種類は外のセルを読むだけで端の場合分けを持たない。T-0774)
            [[nodiscard]] const LqCellView& StateOf(uint32_t link) const {
                return LiquidGrid::IsEdge(link) ? outside[LiquidGrid::EdgeRow(link)] : states[link];
            }

            [[nodiscard]] int64_t PressureOf(uint32_t link) const {
                return LiquidGrid::IsEdge(link) ? outsidePressure[LiquidGrid::EdgeRow(link)] : pressure[link];
            }

            // セル cell の面 face の開き(Q16。0 = 閉じている〔wall の端・どちらかが固体で満ちている〕)
            [[nodiscard]] uint32_t FaceOpenQ16(uint32_t cell, uint32_t face) const {
                const uint32_t link = grid.Link(cell, face);
                if (link == NO_CELL)
                    return 0;

                const LqCellView& other = StateOf(link);
                if (multires::TxFaceFieldIsOwn(face))
                    return liquid::LqFaceOpen(states[cell].open, other.open);

                return liquid::LqFaceOpen(other.open, states[cell].open);
            }

            // 整数の釣り合いを見る面の片側(T-0781。sim/probe_liquid の BalanceSide と同じ)
            // 下は ComputePressure と同じ(開いた世界の端なら外のセル)
            [[nodiscard]] liquid::LqBalanceSide BalanceSide(uint32_t cell) const {
                const uint32_t below = grid.Link(cell, MinusFace(UP_AXIS));
                const uint32_t hasBelow = below == NO_CELL ? 0u : 1u;

                return liquid::LqMakeBalanceSide(states[cell], pressure[cell], above[cell], hasBelow,
                                                 hasBelow != 0 ? StateOf(below) : states[cell]);
            }

            [[nodiscard]] int64_t PotentialOf(uint32_t cell, uint64_t massMilligrams) const {
                const uint64_t height = liquid::LqCellHeightMillimeters(grid.HeightCells(cell), params);

                return liquid::LqPotentialEnergy(massMilligrams, height, params);
            }

            // 1 mg までの面の順番(LqHoldTurn)。軸の組は仮の世界の格子の x・y・z の順(上 = y)、座標の偶奇はブロックの中の座標
            // (ブロックの原点はそのレベルの 8 セルの倍数)
            [[nodiscard]] uint32_t HoldTurn(uint32_t cell, uint32_t axis) const {
                const uint32_t group = axis == UP_AXIS ? 1u : (axis == multires::WeAxisUpLast(0) ? 0u : 2u);

                return liquid::LqHoldTurn(tick, group, (uint32_t)multires::WeCellAxis(cell % MR_BLOCK_CELLS, axis));
            }

            [[nodiscard]] bool AtRest() const {
                const auto zero = [](int64_t value) {
                    return value == 0;
                };
                for (uint32_t axis = 0; axis < 3; ++axis) {
                    if (!rng::all_of(faces[axis], zero) || !rng::all_of(kinetic[axis], zero) ||
                        !rng::all_of(carry[axis], zero))
                        return false;
                }

                return rng::all_of(potential, zero);
            }

            void Read(const MultiresNest& nest, const MultiresLiquidFields& fields, MultiresLiquidStepReport& report);
            void FollowMassChange(MultiresLiquidStepReport& report);
            void ComputePressure(MultiresLiquidStepReport& report);
            void ApplyForces(MultiresLiquidStepReport& report);
            void MinusEdgeForce(uint32_t cell, uint32_t axis, uint32_t link, MultiresLiquidStepReport& report);
            [[nodiscard]] int64_t EdgeStartMomentum(uint32_t cell, uint32_t axis, bool plusEdge) const;
            void AddDesired(uint32_t from, uint32_t to, uint64_t mass);
            void LimitAndApply(MultiresLiquidStepReport& report);
            [[nodiscard]] FaceRecord RecordOf(uint32_t cell, uint32_t face) const;
            void ApplyFace(const LiquidApplyWorld& world, uint32_t cell, uint32_t face,
                           MultiresLiquidStepReport& report);
            [[nodiscard]] multires::TxPacket Pack(uint32_t from, uint64_t mass) const;
            void AdvectMomentum(MultiresLiquidStepReport& report);
            void SettleEnergy(MultiresLiquidStepReport& report);
            void RestFace(liquid::LqSettledFace& settled, uint32_t axis, uint32_t cell, uint64_t doubledMass,
                          MultiresLiquidStepReport& report);
            void AddLedger(MultiresNest& nest, const MultiresLiquidFields& fields,
                           const MultiresLiquidStepReport& report) const;
            void Write(MultiresNest& nest, const MultiresLiquidFields& fields,
                       std::vector<uint32_t>& changedSlots) const;

            // 運動量の面 axis の、across 向きの境ごとに visit(下の面, 上の面, 2 倍の質量の流れ)(仮の世界の ForEachBoundary と同じ)
            template <typename Visit>
            void ForEachBoundary(uint32_t axis, uint32_t across, Visit&& visit) const {
                const uint32_t count = grid.Count();
                for (uint32_t cell = 0; cell < count; ++cell) {
                    const uint32_t alongAxis = grid.Plus(cell, axis);
                    const uint32_t alongAcross = grid.Plus(cell, across);
                    if (alongAxis == NO_CELL || alongAcross == NO_CELL)
                        continue;  // 世界の端の面(閉じている)

                    const int64_t flow = moved[across][cell] + moved[across][alongAxis];
                    if (flow != 0)
                        visit(cell, alongAcross, flow);
                }
            }
        };

        // 刻みの初めのセルと欄を読む。セルの見立て(MakeCellView)から液体の見立てを作る(一様なブロックは 1 回)
        void LiquidStep::Read(const MultiresNest& nest, const MultiresLiquidFields& fields,
                              MultiresLiquidStepReport& report) {
            const uint32_t count = grid.Count();
            const CellViewTable viewTable = {.reaction = table.View(), .phases = phases};
            cells.resize(count);
            states.resize(count);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                faces[axis].resize(count);
                kinetic[axis].resize(count);
                carry[axis].resize(count);
            }

            potential.resize(count);
            massBefore.resize(count);
            for (uint32_t order = 0; order < grid.slots.size(); ++order) {
                const uint32_t slot = grid.slots[order];
                const multires::MrBlock& block = nest.blocks[slot];
                const bool uniform = multires::MrIsUniform(block);
                LqCellView uniformState{};
                if (uniform)
                    uniformState = MakeLiquidCellView(
                        multires::MakeCellView(viewTable, LoadNestCell(nest, slot, 0), block.level, 0));

                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const uint32_t cell = (order * MR_BLOCK_CELLS) + index;
                    cells[cell] = LoadNestCell(nest, slot, index);
                    states[cell] = uniform ? uniformState
                                           : MakeLiquidCellView(
                                                 multires::MakeCellView(viewTable, cells[cell], block.level, 0));
                    if (states[cell].mass != 0)
                        ++report.wetCells;

                    for (uint32_t axis = 0; axis < 3; ++axis) {
                        faces[axis][cell] = LoadNestField(nest, fields.momentum, slot, index, axis);
                        kinetic[axis][cell] = LoadNestField(nest, fields.kinetic, slot, index, axis);
                        carry[axis][cell] = LoadNestField(nest, fields.carry, slot, index, axis);
                    }

                    potential[cell] = LoadNestField(nest, fields.potential, slot, index, 0);
                    massBefore[cell] = LoadNestField(nest, fields.massBefore, slot, index, 0);
                }
            }

            // 開いた世界の端の外のセル(外の柱の行を根のレベルで。液体は無いので圧力 0 = 出ていくだけ・下にあれば支えない。T-0774)
            outside.clear();
            outsidePressure.clear();
            for (const reaction::RxCell& cell : nest.edges.column) {
                const LqCellView view = MakeLiquidCellView(
                    multires::MakeCellView(viewTable, cell, nest.capacity.rootLevel, 0));
                outside.push_back(view);
                outsidePressure.push_back(liquid::LqColumnPressure(view, 0, 0, view, params).pressure);
            }
        }

        // 段の外(蒸発・凝縮・反応・置く操作)で液体の質量が変わった面の運動量を合わせ直す(仮の世界の FollowMassChange。T-0656。
        // 当てる段の「欄が物質に付く」T-0718 へ移す所)
        void LiquidStep::FollowMassChange(MultiresLiquidStepReport& report) {
            const uint32_t count = grid.Count();
            for (uint32_t axis = 0; axis < 3; ++axis) {
                for (uint32_t cell = 0; cell < count; ++cell) {
                    int64_t& face = faces[axis][cell];
                    const uint32_t neighbor = grid.Plus(cell, axis);
                    if (face == 0 || neighbor == NO_CELL)
                        continue;

                    const auto before = (uint64_t)(massBefore[cell] + massBefore[neighbor]);
                    const uint64_t now = states[cell].mass + states[neighbor].mass;
                    if (now >= before)
                        continue;

                    const int64_t kept = follow::FmMomentumAfterMassChange(face, before, now);
                    report.vanished[axis] -= face - kept;
                    face = kept;
                }
            }
        }

        // 圧力(質量の単位)。列ごとに上から(列の一番上 = 上〔WE_UP_AXIS〕の隣がセルでない)。下が開いた世界の端なら
        // 外のセル(液体が無い = 上の液体を支えない。区画の床〔閉じた面〕は全部支える)
        void LiquidStep::ComputePressure(MultiresLiquidStepReport& report) {
            const uint32_t count = grid.Count();
            pressure.assign(count, 0);
            above.assign(count, 0);
            for (uint32_t top = 0; top < count; ++top) {
                if (grid.Plus(top, UP_AXIS) != NO_CELL)
                    continue;

                uint64_t supported = 0;
                for (uint32_t cell = top; cell != NO_CELL; cell = grid.Minus(cell, UP_AXIS)) {
                    const uint32_t below = grid.Link(cell, MinusFace(UP_AXIS));
                    const uint32_t hasBelow = below == NO_CELL ? 0u : 1u;
                    const liquid::LqColumnStep column = liquid::LqColumnPressure(
                        states[cell], supported, hasBelow, hasBelow != 0 ? StateOf(below) : states[cell], params);
                    pressure[cell] = column.pressure;
                    above[cell] = supported;
                    report.overfilledCells += column.overfilled;
                    supported = column.above;
                }
            }
        }

        void AddForce(MultiresLiquidStepReport& report, uint32_t axis, const liquid::LqFaceForce& force) {
            report.wall[axis] += force.wall;
            report.gravity[axis] += force.gravity;
            report.friction[axis] += force.friction;
            report.dropped[axis] += force.dropped;
        }

        // 望む動く質量を上流の出る合計・下流の入る合計へ(開いた端の外は数えない = 無限の溜め)
        void LiquidStep::AddDesired(uint32_t from, uint32_t to, uint64_t mass) {
            if (LiquidGrid::IsCell(from))
                outgoing[from] += mass;

            if (LiquidGrid::IsCell(to))
                incoming[to] += mass;
        }

        // 面の運動量に力(圧力の差・重さ・壁・摩擦)を足し、望む動く質量を決める。面の力は + 側の面の欄の持ち主(− 側のセル)が 1 回。
        // − 側が開いた世界の端の面は持ち主のセルが無いので、+ 側のセル(自分)が刻みの中だけの値で持つ(MinusEdgeForce)
        void LiquidStep::ApplyForces(MultiresLiquidStepReport& report) {
            const uint32_t count = grid.Count();
            outgoing.assign(count, 0);
            incoming.assign(count, 0);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                desired[axis].assign(count, 0);
                edgeFaces[axis].assign(count, 0);
                edgeDesired[axis].assign(count, 0);
                const uint32_t vertical = axis == UP_AXIS ? 1u : 0u;
                for (uint32_t cell = 0; cell < count; ++cell) {
                    const LqCellView& a = states[cell];
                    int64_t& face = faces[axis][cell];

                    // − 側が閉じていれば、その面の押し返しは壁から(+ 向き)。開いた世界の端なら外のセルとの面
                    const uint32_t minus = grid.Link(cell, MinusFace(axis));
                    const uint32_t minusClosed = FaceOpenQ16(cell, MinusFace(axis)) == 0 ? 1u : 0u;
                    report.wall[axis] += liquid::LqMinusWallPush(a, pressure[cell], minusClosed);
                    if (LiquidGrid::IsEdge(minus))
                        MinusEdgeForce(cell, axis, minus, report);

                    // + 側の面(開いた世界の端なら刻みの初めの運動量を内側の面から写す。帳簿の「世界の端」の入)
                    const uint32_t plus = grid.Link(cell, PlusFace(axis));
                    if (LiquidGrid::IsEdge(plus)) {
                        face = EdgeStartMomentum(cell, axis, true);
                        report.edge[axis] += face;
                    }

                    const uint32_t faceOpen = FaceOpenQ16(cell, PlusFace(axis));
                    liquid::LqFaceForce force = liquid::LqClosedFace(face, a, pressure[cell]);
                    if (faceOpen != 0) {
                        force = liquid::LqOpenFaceForce(face, faceOpen, vertical, a, pressure[cell], StateOf(plus),
                                                        PressureOf(plus), params);

                        // 整数の釣り合い(T-0781・ADR-0341)。開いた端の外のセルとの面は見ない(外は柱を持たない無限の溜め)
                        if (LiquidGrid::IsCell(plus))
                            force = liquid::LqHoldAtBalance(force, carry[axis][cell], HoldTurn(cell, axis), vertical,
                                                            BalanceSide(cell), BalanceSide(plus), params);
                    }

                    face = force.face;
                    carry[axis][cell] = force.carry;
                    AddForce(report, axis, force);
                    if (force.desired == 0)
                        continue;

                    desired[axis][cell] = force.desired;
                    AddDesired(face > 0 ? cell : plus, face > 0 ? plus : cell, force.desired);
                }
            }
        }

        // 開いた世界の端の面の刻みの初めの運動量(流れ出しの境界 = 端に垂直な向きの速さの勾配 0。ADR-0349 Proposed):
        // 内側の面(同じ軸の自分のもう片方の面。+ の端なら − の面〔− 側の隣の欄〕、− の端なら + の面〔自分の欄〕)の速さを写す。
        // 面の運動量 = 速さ × 両側の質量なので、内側の面の運動量 × 端の面の両側の質量 ÷ 内側の面の両側の質量(外のセルの液体は 0)。
        // 端の面の値は持ち越さない(写した分は帳簿の「世界の端」の入、刻みの終わりの値は出)
        int64_t LiquidStep::EdgeStartMomentum(uint32_t cell, uint32_t axis, bool plusEdge) const {
            const uint32_t other = plusEdge ? grid.Minus(cell, axis) : grid.Plus(cell, axis);
            if (other == NO_CELL)
                return 0;

            const int64_t inner = facesBefore[axis][plusEdge ? other : cell];
            const uint64_t innerMass = states[cell].mass + states[other].mass;
            if (inner == 0 || innerMass == 0)
                return 0;

            const uint32_t link = grid.Link(cell, plusEdge ? PlusFace(axis) : MinusFace(axis));
            const uint64_t edgeMass = states[cell].mass + StateOf(link).mass;

            return liquid::LqScaleSigned(inner, edgeMass, innerMass);
        }

        // − 側が開いた世界の端の面(− 側 = 外のセル・+ 側 = 自分)。運動量は持ち越さない(内側の面から写した値に力を足し、
        // 刻みの終わりに帳簿の「世界の端」へ。+ 側が開いた端の面と同じ扱い。ADR-0349 Proposed)
        void LiquidStep::MinusEdgeForce(uint32_t cell, uint32_t axis, uint32_t link, MultiresLiquidStepReport& report) {
            const uint32_t faceOpen = FaceOpenQ16(cell, MinusFace(axis));
            if (faceOpen == 0)
                return;  // 閉じた面(押し返しは LqMinusWallPush)

            const uint32_t vertical = axis == UP_AXIS ? 1u : 0u;
            const int64_t start = EdgeStartMomentum(cell, axis, false);
            report.edge[axis] += start;
            const liquid::LqFaceForce force = liquid::LqOpenFaceForce(
                start, faceOpen, vertical, StateOf(link), PressureOf(link), states[cell], pressure[cell], params);
            edgeFaces[axis][cell] = force.face;
            AddForce(report, axis, force);
            if (force.desired == 0)
                return;

            edgeDesired[axis][cell] = force.desired;
            AddDesired(force.face > 0 ? link : cell, force.face > 0 ? cell : link, force.desired);
        }

        // 当てる段の世界(液体の 1 段。枠は使わず、番号 = 1 段の世界のセルの並び。T-0770)。開いた世界の端の外は番号 = 面の先
        // (EDGE_LINK | 行)で、無限の溜め(縮めない。TxLimitFaceFlow と同じ)・外の側は帳簿へ(AddEdgePacket。T-0774)
        struct LiquidApplyWorld {
            LiquidStep* step = nullptr;

            [[nodiscard]] multires::TxLimit Limit(uint32_t /*slot*/, uint32_t cell) const {
                return LiquidGrid::IsCell(cell) ? step->limits[cell] : multires::TxNoLimit();
            }

            void AddPacket(uint32_t /*slot*/, uint32_t cell, const multires::TxPacket& packet,
                           uint32_t incoming) const {
                sim::AddPacket(step->changes[cell], packet, incoming != 0);
            }

            // 外の側(外のセルは変わらない): 自分が上流(incoming = 0)なら外へ出た = 正
            void AddEdgePacket(uint32_t /*slot*/, uint32_t /*cell*/, const multires::TxPacket& packet,
                               uint32_t incoming) const {
                sim::AddPacket(step->edgeChange, packet, incoming == 0);
            }
        };

        // 液体の種類の包み: 上流 from から質量 mass(mg)の液体 = 成分の比で物質量・液体のぶんのエネルギー・位置のエネルギーの比
        // (運ぶ欄 CARRIED_POTENTIAL。速さ〔面の運動量〕は面の欄なので AdvectMomentum が運ぶ)
        multires::TxPacket LiquidStep::Pack(uint32_t from, uint64_t mass) const {
            const reaction::RxCell& cell = cells[from];
            const LqCellView& state = states[from];
            multires::TxPacket packet = multires::TxMakePacket();
            uint64_t movedCapacity = 0;
            int64_t movedChemical = 0;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                const uint32_t species = cell.species[i];
                if (!IsLiquid(phases, species))
                    continue;

                const uint64_t amount = liquid::LqMovedShare(cell.amounts[i], mass, state.mass);
                if (amount == 0)
                    continue;

                packet = multires::TxPacketWithSpecies(packet, species, amount);
                movedCapacity += amount * table.species[species].heatCapacity;
                movedChemical += (int64_t)amount * table.species[species].h0;
            }

            if (movedCapacity != 0)
                packet.energy = liquid::LqMovedEnergy(state, movedCapacity, movedChemical);

            packet.carried[CARRIED_POTENTIAL] = (int64_t)liquid::LqMovedShare((uint64_t)potential[from], mass,
                                                                              state.mass);

            return packet;
        }

        // 当てる段(T-0714・T-0770・T-0771): 上限(出る側 = 中身 ÷ 出る合計・入る側 = 器の残り ÷ 入る合計。液体は器で切らない)→
        // セルごとに 6 面(TxFlowCell と同じ回し方)を自分の側だけ当てる(ApplyFace)。望む流れは刻みの初めの面の運動量から(Jacobi)
        void LiquidStep::LimitAndApply(MultiresLiquidStepReport& report) {
            const uint32_t count = grid.Count();
            changes.assign(count, {});
            edgeChange = {};
            limits.resize(count);
            for (uint32_t cell = 0; cell < count; ++cell) {
                if (outgoing[cell] > states[cell].mass)
                    ++report.limitedCells;

                limits[cell] = multires::TxMakeLimit(states[cell].mass, outgoing[cell], multires::TX_ROOM_UNLIMITED,
                                                     incoming[cell]);
            }

            for (uint32_t axis = 0; axis < 3; ++axis)
                moved[axis].assign(count, 0);

            const LiquidApplyWorld world{.step = this};
            for (uint32_t cell = 0; cell < count; ++cell) {
                for (uint32_t face = 0; face < MR_FACES; ++face)
                    ApplyFace(world, cell, face, report);
            }

            potentialCarried = potential;
            for (uint32_t cell = 0; cell < count; ++cell)
                potentialCarried[cell] += changes[cell].carried[CARRIED_POTENTIAL];
        }

        // セル cell の面 face の記録(面の欄を読む向き: + の面は自分の欄・− の面は − 側の隣の欄。TxFaceFieldIsOwn)
        FaceRecord LiquidStep::RecordOf(uint32_t cell, uint32_t face) const {
            const uint32_t axis = face / 2;
            const uint32_t link = grid.Link(cell, face);
            if (multires::TxFaceFieldIsOwn(face))
                return {.desired = desired[axis][cell], .forward = faces[axis][cell] > 0, .lower = cell, .upper = link};

            if (LiquidGrid::IsCell(link))
                return {.desired = desired[axis][link], .forward = faces[axis][link] > 0, .lower = link, .upper = cell};

            if (LiquidGrid::IsEdge(link))
                return {.desired = edgeDesired[axis][cell],
                        .forward = edgeFaces[axis][cell] > 0,
                        .lower = link,
                        .upper = cell};

            return {};
        }

        // セル cell の面 face を自分の側だけ当てる(相手は相手の回で同じ包みを逆向きに。開いた端の外の側は帳簿へ)。
        // 上限と包みは両側が同じ刻みの初めの値から同じに作る。動いた質量は面の記録の持ち主の回で 1 回だけ数える
        void LiquidStep::ApplyFace(const LiquidApplyWorld& world, uint32_t cell, uint32_t face,
                                   MultiresLiquidStepReport& report) {
            const FaceRecord record = RecordOf(cell, face);
            if (record.desired == 0)
                return;

            const uint32_t from = record.forward ? record.lower : record.upper;
            const uint32_t to = record.forward ? record.upper : record.lower;
            const uint64_t mass = multires::TxLimitAmount(world, 0, from, 0, to, record.desired);
            if (mass == 0)
                return;

            FX_ASSERT(LiquidGrid::IsCell(from));  // 外のセルに液体は無い(上流にならない)
            const bool edge = LiquidGrid::IsEdge(grid.Link(cell, face));
            (void)multires::TxApplyPacketSide(world, 0, cell, to == cell ? 1u : 0u, edge ? 1u : 0u, Pack(from, mass));

            const bool own = multires::TxFaceFieldIsOwn(face);
            if (!own && !edge)
                return;

            report.movedMilligrams += mass;
            if (edge)
                report.edgeMilligrams += mass;

            if (own)
                moved[face / 2][cell] = record.forward ? (int64_t)mass : -(int64_t)mass;
        }

        // 面の運動量(と運動のエネルギー)を、面のまわりの体積の境を通る質量の流れで風上に運ぶ(T-0610・ADR-0276)
        void LiquidStep::AdvectMomentum(MultiresLiquidStepReport& report) {
            const uint32_t count = grid.Count();
            for (uint32_t axis = 0; axis < 3; ++axis) {
                // 面から出る 2 倍の流れの合計(2 倍の質量を超えれば比で縮める)
                std::vector<uint64_t> leaving(count, 0);
                for (uint32_t across = 0; across < 3; ++across) {
                    ForEachBoundary(axis, across, [&](uint32_t lower, uint32_t upper, int64_t flow) {
                        leaving[flow > 0 ? lower : upper] += fx::FxAbsU64(flow);
                    });
                }

                const std::vector<int64_t> before = faces[axis];
                const std::vector<int64_t> kineticBefore = kineticCarried[axis];
                for (uint32_t across = 0; across < 3; ++across) {
                    ForEachBoundary(axis, across, [&](uint32_t lower, uint32_t upper, int64_t flow) {
                        const uint32_t from = flow > 0 ? lower : upper;
                        const uint32_t to = flow > 0 ? upper : lower;
                        // 開いた世界の端の面の運動量は運ばない(持ち越さない値。SettleEnergy が帳簿の「世界の端」へ。T-0774)
                        if (before[from] == 0 || LiquidGrid::IsEdge(grid.Link(from, PlusFace(axis))))
                            return;

                        // 運動量のある面は開いている(閉じた面の運動量は壁が受けた)ので + 側の隣がある
                        const uint32_t fromNeighbor = grid.Plus(from, axis);
                        FX_ASSERT(fromNeighbor != NO_CELL);
                        const uint64_t doubledMass = states[from].mass + states[fromNeighbor].mass;
                        const liquid::LqCarried carried = liquid::LqAdvectShare(
                            before[from], kineticBefore[from], fx::FxAbsU64(flow), doubledMass, leaving[from]);
                        faces[axis][from] -= carried.momentum;
                        kineticCarried[axis][from] -= carried.energy;
                        kineticCarried[axis][to] += carried.energy;  // 閉じた面へ入った分は SettleEnergy で熱に

                        // 閉じた面(壁・wall の世界の端)へ入る運動量は壁が受ける(開いた端の面へ入った分は SettleEnergy が「世界の端」へ)
                        if (FaceOpenQ16(to, PlusFace(axis)) == 0)
                            report.wall[axis] -= carried.momentum;
                        else
                            faces[axis][to] += carried.momentum;
                    });
                }
            }
        }

        // 液体を当て、まわりが空の面・速さの上限を直し、位置と運動のエネルギーを計算し直して差をセルの熱にする
        void LiquidStep::SettleEnergy(MultiresLiquidStepReport& report) {
            const uint32_t count = grid.Count();
            changed.assign(count, 0);
            std::vector<uint64_t> mass(count, 0);
            for (uint32_t cell = 0; cell < count; ++cell) {
                if (ApplyCellChange(cells[cell], changes[cell]))
                    changed[cell] = 1;

                mass[cell] = changes[cell].touched ? LiquidMassMilligrams(phases, cells[cell]) : states[cell].mass;
                massBefore[cell] = (int64_t)mass[cell];
            }

            // --- 面: まわりが空になった面・速さの上限・運動のエネルギー(差は両側のセルへ半分ずつ)---
            std::vector<int64_t> heat(count, 0);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                for (uint32_t cell = 0; cell < count; ++cell) {
                    int64_t& face = faces[axis][cell];

                    // 開いた世界の端の面は運動量を持ち越さない(刻みの終わりの値は帳簿の「世界の端」の出。運んできた運動のエネルギーは
                    // 自分の熱へ。T-0774)
                    report.edge[axis] -= edgeFaces[axis][cell];
                    if (LiquidGrid::IsEdge(grid.Link(cell, PlusFace(axis)))) {
                        report.edge[axis] -= face;
                        face = 0;
                        kinetic[axis][cell] = 0;
                        kineticHeat[axis] += kineticCarried[axis][cell];
                        heat[cell] += kineticCarried[axis][cell];
                        continue;
                    }

                    FX_ASSERT(face == 0 || FaceOpenQ16(cell, PlusFace(axis)) != 0);
                    const uint32_t neighbor = grid.Plus(cell, axis);
                    const bool inside = neighbor != NO_CELL;
                    const uint64_t doubled = inside ? mass[cell] + mass[neighbor] : 0;
                    liquid::LqSettledFace settled = liquid::LqSettleFace(face, doubled, kineticCarried[axis][cell],
                                                                         params);
                    RestFace(settled, axis, cell, doubled, report);
                    face = settled.face;
                    report.dropped[axis] += settled.dropped;
                    report.capped[axis] += settled.capped;
                    report.cappedFaces += settled.cappedFace;
                    kinetic[axis][cell] = settled.kinetic;
                    const int64_t released = settled.released;
                    if (released == 0)
                        continue;

                    kineticHeat[axis] += released;

                    if (!inside) {
                        heat[cell] += released;
                        continue;
                    }

                    const int64_t share = liquid::LqReleasedShare(released);
                    heat[cell] += released - share;
                    heat[neighbor] += share;
                }
            }

            // --- セル: 位置のエネルギー ---
            for (uint32_t cell = 0; cell < count; ++cell) {
                const int64_t settledPotential = PotentialOf(cell, mass[cell]);
                heat[cell] += potentialCarried[cell] - settledPotential;
                potentialHeat += potentialCarried[cell] - settledPotential;
                potential[cell] = settledPotential;
                if (heat[cell] == 0)
                    continue;

                cells[cell].energy += heat[cell];
                report.heat += heat[cell];
                changed[cell] = 1;
            }
        }

        // 静止の丸め(T-0777。既定は切る。形は仮の世界の液体の面と同じ = common/rest_rounding.hlsli の RrRestMomentum)。当てる段の後
        // (上限・当てる・運ぶの後、運動のエネルギーを熱へ直す所)で、速さが restSpeed 未満で、この刻みで速くなっていない面の運動量を 0 に
        // (帳簿の「静止の丸め」)。運動のエネルギーは released に足して両側のセルの熱へ(速さの上限で止めた分と同じ道)
        void LiquidStep::RestFace(liquid::LqSettledFace& settled, uint32_t axis, uint32_t cell, uint64_t doubledMass,
                                  MultiresLiquidStepReport& report) {
            if (restSpeed == 0 || settled.face == 0)
                return;

            const uint64_t limit = liquid::LqMulDiv((uint64_t)restSpeed * TICK_RATE, doubledMass,
                                                    params.gravityMicrometersPerSecond2);
            const rest::RrRested result = rest::RrRestMomentum(settled.face, facesBefore[axis][cell], settled.kinetic,
                                                               limit);
            if (result.rested == 0)
                return;

            report.rested[axis] += result.ledgerMomentum;
            restHeat[axis] += result.heat;
            settled.face = result.momentum;
            settled.released += result.heat;
            settled.kinetic = result.kinetic;
            ++report.restedFaces;
        }

        // 帳簿(世界の帳簿の列 = 量 × 理由。T-0769・T-0762 の書き方の約束): 面の運動量の列に外から入った運動量を理由ごとに
        // (正 → 入・負 → 出)、位置と運動のエネルギーの欄の列に「熱になった分」を出し、同じ量をエネルギーの列(セルの熱)に入れる。
        // 段の外で液体が現れた・消えた分の位置のエネルギーは「段の外の質量の変化」の入。
        // これで ComputeConservedTotals(Σ 保存量 − 帳簿)が液体の刻みで列ごとに変わらない
        void LiquidStep::AddLedger(MultiresNest& nest, const MultiresLiquidFields& fields,
                                   const MultiresLiquidStepReport& report) const {
            const int32_t level = nest.capacity.rootLevel;
            const uint32_t base = NestFieldLedgerBase(nest);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const uint32_t column = base + NestConservedIndex(nest, fields.momentum) + axis;
                const std::array<int64_t, MOMENTUM_REASONS.size()> parts = {
                    report.wall[axis],   report.gravity[axis],  report.friction[axis], report.dropped[axis],
                    report.capped[axis], report.vanished[axis], report.edge[axis],     report.rested[axis]};
                for (uint32_t part = 0; part < parts.size(); ++part)
                    AddLedgerSigned(nest, MOMENTUM_REASONS[part], multires::WL_DIRECTION_IN, level, column,
                                    parts[part]);

                // 運動のエネルギー → セルの熱(静止の丸めで消した分は「静止の丸め」・残りは「物理の熱」)
                const uint32_t kineticColumn = base + NestConservedIndex(nest, fields.kinetic) + axis;
                const std::array<std::pair<uint32_t, int64_t>, 2> heats = {
                    std::pair{multires::WL_REASON_PHYSICS_HEAT, kineticHeat[axis] - restHeat[axis]},
                    std::pair{multires::WL_REASON_REST_ROUNDING, restHeat[axis]}};
                for (const auto& [reason, amount] : heats) {
                    AddLedgerSigned(nest, reason, multires::WL_DIRECTION_OUT, level, kineticColumn, amount);
                    AddLedgerSigned(nest, reason, multires::WL_DIRECTION_IN, level, multires::WL_COLUMN_ENERGY, amount);
                }
            }

            const uint32_t potentialColumn = base + NestConservedIndex(nest, fields.potential);
            AddLedgerSigned(nest, multires::WL_REASON_FOLLOW_MASS, multires::WL_DIRECTION_IN, level, potentialColumn,
                            report.potentialEntered);
            AddLedgerSigned(nest, multires::WL_REASON_GRAVITY_WORK, multires::WL_DIRECTION_OUT, level, potentialColumn,
                            potentialHeat);
            AddLedgerSigned(nest, multires::WL_REASON_GRAVITY_WORK, multires::WL_DIRECTION_IN, level,
                            multires::WL_COLUMN_ENERGY, potentialHeat);

            // 開いた世界の端から外へ出た液体(物質・エネルギー・位置のエネルギー。外のセルは変わらない。T-0774)
            for (const auto& [species, amount] : edgeChange.amounts)
                AddLedgerSigned(nest, multires::WL_REASON_WORLD_EDGE, multires::WL_DIRECTION_OUT, level,
                                multires::WL_COLUMN_SPECIES_BASE + species, amount);

            AddLedgerSigned(nest, multires::WL_REASON_WORLD_EDGE, multires::WL_DIRECTION_OUT, level,
                            multires::WL_COLUMN_ENERGY, edgeChange.energy);
            AddLedgerSigned(nest, multires::WL_REASON_WORLD_EDGE, multires::WL_DIRECTION_OUT, level, potentialColumn,
                            edgeChange.carried[CARRIED_POTENTIAL]);
        }

        // セルと欄を木へ書き戻す。変わったブロックだけ(一様なら頁に広げる)。セルが変わったブロックはつつく
        void LiquidStep::Write(MultiresNest& nest, const MultiresLiquidFields& fields,
                               std::vector<uint32_t>& changedSlots) const {
            for (uint32_t order = 0; order < grid.slots.size(); ++order) {
                const uint32_t slot = grid.slots[order];
                const uint32_t first = order * MR_BLOCK_CELLS;
                bool cellsChanged = false;
                bool fieldsChanged = false;
                for (uint32_t index = 0; index < MR_BLOCK_CELLS && !fieldsChanged; ++index) {
                    const uint32_t cell = first + index;
                    cellsChanged = cellsChanged || changed[cell] != 0;
                    for (uint32_t axis = 0; axis < 3; ++axis) {
                        fieldsChanged = fieldsChanged ||
                                        LoadNestField(nest, fields.momentum, slot, index, axis) != faces[axis][cell] ||
                                        LoadNestField(nest, fields.kinetic, slot, index, axis) != kinetic[axis][cell] ||
                                        LoadNestField(nest, fields.carry, slot, index, axis) != carry[axis][cell];
                    }

                    fieldsChanged = fieldsChanged ||
                                    LoadNestField(nest, fields.potential, slot, index, 0) != potential[cell] ||
                                    LoadNestField(nest, fields.massBefore, slot, index, 0) != massBefore[cell];
                }

                for (uint32_t index = 0; index < MR_BLOCK_CELLS && !cellsChanged; ++index)
                    cellsChanged = changed[first + index] != 0;

                if (!cellsChanged && !fieldsChanged)
                    continue;

                [[maybe_unused]] const bool paged = ExpandNestPage(nest, slot);
                FX_ASSERT(paged);  // 頁は枠の数だけある(Create が確かめた)
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const uint32_t cell = first + index;
                    if (changed[cell] != 0)
                        StoreNestCell(nest, slot, index, cells[cell]);

                    for (uint32_t axis = 0; axis < 3; ++axis) {
                        StoreNestField(nest, fields.momentum, slot, index, axis, faces[axis][cell]);
                        StoreNestField(nest, fields.kinetic, slot, index, axis, kinetic[axis][cell]);
                        StoreNestField(nest, fields.carry, slot, index, axis, carry[axis][cell]);
                    }

                    StoreNestField(nest, fields.potential, slot, index, 0, potential[cell]);
                    StoreNestField(nest, fields.massBefore, slot, index, 0, massBefore[cell]);
                }

                if (!cellsChanged)
                    continue;

                PokeNestBlock(nest, slot);
                changedSlots.push_back(slot);
            }
        }

        uint32_t Register(MultiresNest& nest, const char* name, uint32_t place, uint32_t scale, uint32_t flags,
                          uint32_t components) {
            const uint32_t phase = place == sf::SF_PLACE_WORLD ? sf::SF_PHASE_NONE : sf::SF_PHASE_LIQUID;

            return RegisterNestField(nest, name,
                                     sf::SfMakeFieldDesc(place, sf::SF_TYPE_I64, phase, scale, flags, components));
        }

        int64_t SumCellField(const MultiresNest& nest, uint32_t field, uint32_t component) {
            int64_t sum = 0;
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                if (nest.blocks[slot].kind != multires::MR_BLOCK_REAL)
                    continue;

                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                    sum += LoadNestField(nest, field, slot, index, component);
            }

            return sum;
        }

    }  // namespace

    uint64_t LiquidCellEdgeMillimeters(int32_t level) {
        if (level < 0)
            return LEVEL_ZERO_EDGE_MILLIMETERS << (uint32_t)(-level);

        return LEVEL_ZERO_EDGE_MILLIMETERS >> (uint32_t)level;
    }

    liquid::LqCellView MakeLiquidCellView(const multires::CvCellView& view) {
        const LqCellView liquidView = liquid::LqMakeCellView(view.liquidMass, view.liquid.volume, view.solid.volume,
                                                             multires::CV_CELL_VOLUME_LEVEL_UNITS, view.heat,
                                                             view.heatCapacity);
        FX_ASSERT(liquidView.open == view.liquidOpen && liquidView.equivalent == view.liquidEquivalentMass);

        return liquidView;
    }

    std::expected<MultiresLiquid, std::string> MultiresLiquid::Create(MultiresNest& nest,
                                                                      const BakedReactionTable& table,
                                                                      const MultiresLiquidOptions& options) {
        if (table.environment.gravityMicrometersPerSecond2 == 0)
            return std::unexpected("重力が 0(環境の値 environment.gravity が要る)");

        if (options.frictionShift == 0 || options.frictionShift > 30)
            return std::unexpected("摩擦の段(frictionShift)は 1〜30");

        // 1 刻みに 1 セルを超える速さは、流れがセルを飛び越えるので断る
        const uint64_t edge = LiquidCellEdgeMillimeters(nest.capacity.rootLevel);
        if (edge == 0 || options.maxSpeedMillimetersPerSecond == 0 ||
            options.maxSpeedMillimetersPerSecond > edge * TICK_RATE)
            return std::unexpected("速さの上限(maxSpeedMillimetersPerSecond)は 1〜セルの 1 辺 × 60");

        if (nest.wideCells)
            return std::unexpected("溢れのセルを使う世界の液体はまだ無い");

        if (nest.capacity.pages < nest.capacity.worldBlocks)
            return std::unexpected("頁が世界の枠より少ない(液体はどのブロックにも書くことがある)");

        MultiresLiquid result;
        result.m_options = options;
        result.m_params = liquid::LqMakeParams(table.environment.gravityMicrometersPerSecond2, edge,
                                               options.frictionShift, options.overfillStiffness,
                                               options.maxSpeedMillimetersPerSecond, options.advection ? 1u : 0u);

        // --- 欄(面の運動量・運動のエネルギー・位置のエネルギーは保存量 = 世界の帳簿に列がある。前の質量は合わせ直しの元)---
        constexpr uint32_t conserved = sf::SF_FLAG_CONSERVED | sf::SF_FLAG_HASHED;
        MultiresLiquidFields& fields = result.m_fields;
        fields.momentum = Register(nest, "liquid.momentum", sf::SF_PLACE_FACE, sf::SF_SCALE_VOLUME, conserved, 3);
        fields.kinetic = Register(nest, "liquid.kinetic_energy", sf::SF_PLACE_FACE, sf::SF_SCALE_VOLUME, conserved, 3);
        fields.potential = Register(nest, "liquid.potential_energy", sf::SF_PLACE_CELL, sf::SF_SCALE_VOLUME, conserved,
                                    1);
        // 前の質量は保存量ではない(セルの液体の写し)ので余りを帳簿へ出さない SAME(レベルの単位なので、粗くした平均 = 和 ÷ 8 は量の規則と同じ数)
        fields.massBefore = Register(nest, "liquid.mass_before", sf::SF_PLACE_CELL, sf::SF_SCALE_SAME,
                                     sf::SF_FLAG_HASHED, 1);
        // 1 mg までの面の持ち越す流れ(T-0778・ADR-0357)は量でない(まだ動いていない質量の割合)ので SAME。粗く = 子の平均・細かく = 親と同じ
        // (どちらも次の刻みに LqHoldAtBalance が LQ_HOLD_CARRY_MG で止める)
        fields.carry = Register(nest, "liquid.flux_carry", sf::SF_PLACE_FACE, sf::SF_SCALE_SAME, sf::SF_FLAG_HASHED, 3);
        for (const uint32_t field :
             {fields.momentum, fields.kinetic, fields.potential, fields.massBefore, fields.carry}) {
            if (field == MULTIRES_NO_FIELD)
                return std::unexpected("液体の欄を登録できない(名前が重なる・根を置いた後)");
        }

        return result;
    }

    MultiresLiquidStepReport MultiresLiquid::Step(MultiresNest& nest, const BakedReactionTable& table, uint64_t tick,
                                                  std::vector<uint32_t>& changedSlots) const {
        MultiresLiquidStepReport report;
        const LiquidGrid grid = BuildGrid(nest);
        const std::vector<multires::CvSpeciesPhase> phases = MakeSpeciesPhases(table);
        const uint32_t count = grid.Count();

        // --- 刻みの初めの値(セルの見立てと欄)---
        LiquidStep step{.table = table,
                        .phases = phases,
                        .params = m_params,
                        .grid = grid,
                        .restSpeed = m_options.restSpeedMicrometersPerSecond};
        step.Read(nest, m_fields, report);
        step.tick = tick;
        if (report.wetCells == 0 && step.AtRest()) {
            // 液体が無く、動いている面もエネルギーも無い(ブランクの世界・液体の無い表)。前の質量だけ 0 に
            rng::fill(step.massBefore, int64_t{0});
            step.changed.assign(count, 0);
            step.Write(nest, m_fields, changedSlots);
            return report;
        }

        // --- 段の外(反応・置いた水)で液体が現れた・消えた分の位置のエネルギーを帳簿へ ---
        for (uint32_t cell = 0; cell < count; ++cell) {
            const int64_t potential = step.PotentialOf(cell, step.states[cell].mass);
            report.potentialEntered += potential - step.potential[cell];
            step.potential[cell] = potential;
        }

        step.FollowMassChange(report);
        step.facesBefore =
            step.faces;  // 刻みの初めの面の運動量(合わせ直しの後。静止の丸め・開いた端が読む。T-0777・T-0774)

        // --- 圧力 → 面の力 → 上限と当てる → 運動量を運ぶ → エネルギーを熱へ → 帳簿 → 書き戻す(起こす)---
        step.kineticCarried = step.kinetic;
        step.ComputePressure(report);
        step.ApplyForces(report);
        step.LimitAndApply(report);
        if (m_options.advection)
            step.AdvectMomentum(report);

        step.SettleEnergy(report);
        step.AddLedger(nest, m_fields, report);
        report.changedCells = (uint32_t)rng::count(step.changed, uint8_t{1});
        step.Write(nest, m_fields, changedSlots);

        return report;
    }

    int64_t MultiresLiquid::MomentumSum(const MultiresNest& nest, uint32_t axis) const {
        return SumCellField(nest, m_fields.momentum, axis);
    }

    int64_t MultiresLiquid::PotentialEnergySum(const MultiresNest& nest) const {
        return SumCellField(nest, m_fields.potential, 0);
    }

    int64_t MultiresLiquid::KineticEnergySum(const MultiresNest& nest) const {
        int64_t sum = 0;
        for (uint32_t axis = 0; axis < 3; ++axis)
            sum += SumCellField(nest, m_fields.kinetic, axis);

        return sum;
    }

    ProbeLiquidLedger MultiresLiquid::Ledger(const MultiresNest& nest) const {
        // 世界の帳簿の列から(運動量の列の理由ごとの入 − 出の和・位置のエネルギーの列の「段の外の質量の変化」。T-0769)
        ProbeLiquidLedger ledger;
        const int32_t level = nest.capacity.rootLevel;
        const uint32_t base = NestFieldLedgerBase(nest);
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const uint32_t column = base + NestConservedIndex(nest, m_fields.momentum) + axis;
            for (const uint32_t reason : MOMENTUM_REASONS)
                ledger.external[axis] += WorldLedgerNetUnits(nest, reason, column, level);
        }

        ledger.energy = WorldLedgerNetUnits(nest, multires::WL_REASON_FOLLOW_MASS,
                                            base + NestConservedIndex(nest, m_fields.potential), level);

        return ledger;
    }

}  // namespace bicameral::sim
