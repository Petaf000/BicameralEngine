// multires_liquid.cpp — 多重解像度の世界の 1 段で液体を流す CPU(運ぶ計算の共通の土台の段 2 ②。T-0717)。約束は multires_liquid.h。
// 1 刻み = 一覧と面の隣 → 見立て(セルと欄を読む)→ 欄の合わせ直し → 圧力 → 面の力 → 上限と当てる → 運動量を運ぶ → エネルギーを熱へ → 書き戻す。
// 式は common/liquid_flow.hlsli(仮の世界 sim/probe_liquid.cpp と同じ関数)。回し方も仮の世界と同じ Jacobi 型(刻みの初めの値だけから決め、
// 足し引きは整数なので順に依存しない)なので、セルを並べる順(ブロックの順 × ブロックの中の番号)が仮の世界と違っても値はビット単位で同じ。
#include "sim/multires_liquid.h"

#include <algorithm>
#include <span>

#include "common/fixed.hlsli"
#include "core/aliases.h"
#include "sim/cell_view.h"
#include "sim/multires_fields.h"

namespace bicameral::sim {

    namespace {

        using liquid::LqCellView;
        using multires::MR_BLOCK_CELLS;
        using multires::MR_BLOCK_EDGE;
        namespace sf = state_field;

        constexpr uint64_t TICK_RATE = 60;                     // 刻み / 秒
        constexpr uint64_t LEVEL_ZERO_EDGE_MILLIMETERS = 500;  // レベル 0 のセルの 1 辺(仮の世界と同じ)
        constexpr uint32_t NO_CELL = UINT32_MAX;
        constexpr std::array<uint32_t, 3> LOCAL_STRIDE = {1, MR_BLOCK_EDGE,
                                                          MR_BLOCK_EDGE * MR_BLOCK_EDGE};  // MrCellIndex

        // --- 1 段の世界のセルの並び(刻むブロックの順 × ブロックの中の番号)と面の隣 ---
        struct LiquidGrid {
            std::vector<uint32_t> slots;  // 刻むブロックの枠(枠の順)
            // ブロックの順 → 隣のブロックの順(−x・+x・−y・+y・−z・+z。無ければ NO_CELL = 世界の端)
            std::vector<std::array<uint32_t, 6>> neighbors;
            std::vector<int64_t> originY;  // ブロックの順 → 原点の y(セル)
            int64_t floorY = 0;            // 一番下のブロックの原点の y(位置のエネルギーの高さの元)

            [[nodiscard]] uint32_t Count() const { return (uint32_t)slots.size() * MR_BLOCK_CELLS; }

            [[nodiscard]] static uint32_t Local(uint32_t cell, uint32_t axis) {
                return ((cell % MR_BLOCK_CELLS) / LOCAL_STRIDE[axis]) % MR_BLOCK_EDGE;
            }

            // + 側の隣のセル(世界の端なら NO_CELL)
            [[nodiscard]] uint32_t Plus(uint32_t cell, uint32_t axis) const {
                if (Local(cell, axis) + 1 < MR_BLOCK_EDGE)
                    return cell + LOCAL_STRIDE[axis];

                const uint32_t block = neighbors[cell / MR_BLOCK_CELLS][(2 * axis) + 1];
                if (block == NO_CELL)
                    return NO_CELL;

                return (block * MR_BLOCK_CELLS) + (cell % MR_BLOCK_CELLS) - ((MR_BLOCK_EDGE - 1) * LOCAL_STRIDE[axis]);
            }

            // − 側の隣のセル(世界の端なら NO_CELL)
            [[nodiscard]] uint32_t Minus(uint32_t cell, uint32_t axis) const {
                if (Local(cell, axis) > 0)
                    return cell - LOCAL_STRIDE[axis];

                const uint32_t block = neighbors[cell / MR_BLOCK_CELLS][2 * axis];
                if (block == NO_CELL)
                    return NO_CELL;

                return (block * MR_BLOCK_CELLS) + (cell % MR_BLOCK_CELLS) + ((MR_BLOCK_EDGE - 1) * LOCAL_STRIDE[axis]);
            }

            // セルの高さ(一番下のブロックの段から数えたセルの数)
            [[nodiscard]] uint32_t HeightCells(uint32_t cell) const {
                return (uint32_t)(originY[cell / MR_BLOCK_CELLS] + Local(cell, 1) - floorY);
            }
        };

        // 刻むブロックの一覧(根のレベルの本物のブロック全部)と面の隣を索引で引く。1 段の世界だけ(子・違うレベルは断る)
        LiquidGrid BuildGrid(const MultiresNest& nest) {
            LiquidGrid grid;
            std::vector<uint32_t> order(nest.capacity.worldBlocks, NO_CELL);
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                const multires::MrBlock& block = nest.blocks[slot];
                if (block.kind != multires::MR_BLOCK_REAL)
                    continue;

                FX_ASSERT(block.level == nest.capacity.rootLevel);
                FX_ASSERT(rng::all_of(block.children, [](uint32_t child) { return child == multires::MR_NO_BLOCK; }));
                order[slot] = (uint32_t)grid.slots.size();
                grid.slots.push_back(slot);
                grid.originY.push_back(block.originY);
            }

            if (!grid.originY.empty())
                grid.floorY = *rng::min_element(grid.originY);

            const auto edge = (int64_t)MR_BLOCK_EDGE;
            for (const uint32_t slot : grid.slots) {
                const multires::MrBlock& block = nest.blocks[slot];
                std::array<uint32_t, 6> neighbors{};
                for (uint32_t side = 0; side < 6; ++side) {
                    std::array<int64_t, 3> origin = {block.originX, block.originY, block.originZ};
                    origin[side / 2] += (side % 2) == 0 ? -edge : edge;
                    const uint32_t neighbor = LookupBlock(nest, block.level, origin[0], origin[1], origin[2]);
                    neighbors[side] = neighbor == multires::MR_NO_BLOCK ? NO_CELL : order[neighbor];
                }

                grid.neighbors.push_back(neighbors);
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

        // --- 当てる(仮。段 1 ④ の当てる段 T-0714 の生の包みと当て方に載せ替える所)---

        // 物質量の増減(セルごと。物質 ID → 増減)
        struct CellDelta {
            std::vector<std::pair<uint32_t, int64_t>> amounts;
            int64_t energy = 0;  // mJ
            bool touched = false;
        };

        void AddAmount(CellDelta& delta, uint32_t species, int64_t amount) {
            delta.touched = true;
            for (auto& entry : delta.amounts) {
                if (entry.first == species) {
                    entry.second += amount;
                    return;
                }
            }

            delta.amounts.emplace_back(species, amount);
        }

        // 増減をセルに当てる(成分は物質 ID の順・0 の成分は消す)。値が変わったら true
        bool ApplyDelta(reaction::RxCell& cell, const CellDelta& delta) {
            if (!delta.touched)
                return false;

            std::vector<SpeciesAmount> amounts;
            for (uint32_t i = 0; i < cell.speciesCount; ++i)
                amounts.push_back({.species = cell.species[i], .amount = cell.amounts[i]});

            for (const auto& [species, change] : delta.amounts) {
                auto found = rng::find(amounts, species, &SpeciesAmount::species);
                if (found == amounts.end()) {
                    FX_ASSERT(change >= 0);
                    amounts.push_back({.species = species, .amount = (uint64_t)change});
                    continue;
                }

                FX_ASSERT(change >= 0 || found->amount >= (uint64_t)-change);
                found->amount = (uint64_t)((int64_t)found->amount + change);
            }

            std::erase_if(amounts, [](const SpeciesAmount& entry) { return entry.amount == 0; });
            rng::sort(amounts, {}, &SpeciesAmount::species);
            FX_ASSERT(amounts.size() <= reaction::RX_MAX_CELL_SPECIES);  // 溢れのセルは断っている(Create)

            reaction::RxCell after = reaction::RxMakeEmptyCell(cell.energy + delta.energy);
            for (uint32_t i = 0; i < amounts.size() && i < reaction::RX_MAX_CELL_SPECIES; ++i) {
                after.species[i] = amounts[i].species;
                after.amounts[i] = amounts[i].amount;
                after.speciesCount = i + 1;
            }

            bool same = after.energy == cell.energy && after.speciesCount == cell.speciesCount;
            for (uint32_t i = 0; same && i < after.speciesCount; ++i)
                same = after.species[i] == cell.species[i] && after.amounts[i] == cell.amounts[i];

            cell = after;

            return !same;
        }

        // --- 1 刻み ---

        struct LiquidStep {
            const BakedReactionTable& table;
            std::span<const multires::CvSpeciesPhase> phases;
            const liquid::LqParams& params;
            const LiquidGrid& grid;

            // --- 刻みの初めの値(セルと欄。書き戻すまで木は変えない)---
            std::vector<reaction::RxCell> cells;
            std::vector<LqCellView> states;
            std::array<std::vector<int64_t>, 3> faces;    // 面の運動量(mg)
            std::array<std::vector<int64_t>, 3> kinetic;  // 面の運動のエネルギー(mJ)
            std::vector<int64_t> potential;               // 位置のエネルギー(mJ)
            std::vector<int64_t> massBefore;              // 前の刻みの終わりの液体の質量(mg)
            ProbeLiquidLedger ledger;

            // --- 作業 ---
            std::vector<int64_t> pressure;                 // 質量の単位(mg)
            std::array<std::vector<uint64_t>, 3> desired;  // 面の望む動く質量(mg)
            std::vector<uint64_t> outgoing;                // セルから出る望む質量の合計
            std::array<std::vector<int64_t>, 3> moved;     // 面を通って実際に動いた質量(mg。+ 向きが正)
            std::vector<CellDelta> deltas;
            std::vector<int64_t> potentialCarried;
            std::array<std::vector<int64_t>, 3> kineticCarried;
            std::vector<uint8_t> changed;

            // + 側の面の開き(Q16。0 = 閉じている〔世界の端・どちらかが固体で満ちている〕)
            [[nodiscard]] uint32_t FaceOpenQ16(uint32_t cell, uint32_t axis) const {
                const uint32_t neighbor = grid.Plus(cell, axis);
                if (neighbor == NO_CELL)
                    return 0;

                return liquid::LqFaceOpen(states[cell].open, states[neighbor].open);
            }

            [[nodiscard]] int64_t PotentialOf(uint32_t cell, uint64_t massMilligrams) const {
                const uint64_t height = liquid::LqCellHeightMillimeters(grid.HeightCells(cell), params);

                return liquid::LqPotentialEnergy(massMilligrams, height, params);
            }

            [[nodiscard]] bool AtRest() const {
                const auto zero = [](int64_t value) {
                    return value == 0;
                };
                for (uint32_t axis = 0; axis < 3; ++axis) {
                    if (!rng::all_of(faces[axis], zero) || !rng::all_of(kinetic[axis], zero))
                        return false;
                }

                return rng::all_of(potential, zero);
            }

            void Read(const MultiresNest& nest, const MultiresLiquidFields& fields, ProbeLiquidStepReport& report);
            void FollowMassChange(ProbeLiquidStepReport& report);
            void ComputePressure(ProbeLiquidStepReport& report);
            void ApplyForces(ProbeLiquidStepReport& report);
            void LimitAndApply(ProbeLiquidStepReport& report);
            void MoveLiquid(uint32_t from, uint32_t to, uint64_t mass);
            void AdvectMomentum(ProbeLiquidStepReport& report);
            void SettleEnergy(ProbeLiquidStepReport& report);
            void AddLedger(const ProbeLiquidStepReport& report);
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
                              ProbeLiquidStepReport& report) {
            const uint32_t count = grid.Count();
            const CellViewTable viewTable = {.reaction = table.View(), .phases = phases};
            cells.resize(count);
            states.resize(count);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                faces[axis].resize(count);
                kinetic[axis].resize(count);
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
                    }

                    potential[cell] = LoadNestField(nest, fields.potential, slot, index, 0);
                    massBefore[cell] = LoadNestField(nest, fields.massBefore, slot, index, 0);
                }
            }

            for (uint32_t axis = 0; axis < 3; ++axis)
                ledger.external[axis] = LoadNestWorldField(nest, fields.ledgerMomentum, axis);

            ledger.energy = LoadNestWorldField(nest, fields.ledgerEnergy, 0);
        }

        // 段の外(蒸発・凝縮・反応・置く操作)で液体の質量が変わった面の運動量を合わせ直す(仮の世界の FollowMassChange。T-0656。
        // 当てる段の「欄が物質に付く」T-0718 へ移す所)
        void LiquidStep::FollowMassChange(ProbeLiquidStepReport& report) {
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

                    const int64_t kept = liquid::LqFollowMassChange(face, before, now);
                    report.vanished[axis] -= face - kept;
                    face = kept;
                }
            }
        }

        // 圧力(質量の単位)。列ごとに上から(列の一番上 = + y の隣が無いセル)
        void LiquidStep::ComputePressure(ProbeLiquidStepReport& report) {
            const uint32_t count = grid.Count();
            pressure.assign(count, 0);
            for (uint32_t top = 0; top < count; ++top) {
                if (grid.Plus(top, 1) != NO_CELL)
                    continue;

                uint64_t above = 0;
                for (uint32_t cell = top; cell != NO_CELL;) {
                    const uint32_t below = grid.Minus(cell, 1);
                    const liquid::LqColumnStep column = liquid::LqColumnPressure(
                        states[cell], above, below == NO_CELL ? 0u : 1u,
                        below == NO_CELL ? states[cell] : states[below], params);
                    pressure[cell] = column.pressure;
                    report.overfilledCells += column.overfilled;
                    above = column.above;
                    cell = below;
                }
            }
        }

        // 面の運動量に力(圧力の差・重さ・壁・摩擦)を足し、望む動く質量を決める
        void LiquidStep::ApplyForces(ProbeLiquidStepReport& report) {
            const uint32_t count = grid.Count();
            outgoing.assign(count, 0);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                desired[axis].assign(count, 0);
                for (uint32_t cell = 0; cell < count; ++cell) {
                    const LqCellView& a = states[cell];
                    int64_t& face = faces[axis][cell];

                    // − 側が世界の端か壁なら、その面の押し返しは壁から(+ 向き)
                    const uint32_t minus = grid.Minus(cell, axis);
                    const bool minusClosed = minus == NO_CELL || FaceOpenQ16(minus, axis) == 0;
                    report.wall[axis] += liquid::LqMinusWallPush(a, pressure[cell], minusClosed ? 1u : 0u);

                    // + 側の面
                    const uint32_t faceOpen = FaceOpenQ16(cell, axis);
                    const uint32_t neighbor = grid.Plus(cell, axis);
                    const liquid::LqFaceForce force = faceOpen == 0
                                                          ? liquid::LqClosedFace(face, a, pressure[cell])
                                                          : liquid::LqOpenFaceForce(face, faceOpen, axis == 1 ? 1u : 0u,
                                                                                    a, pressure[cell], states[neighbor],
                                                                                    pressure[neighbor], params);
                    face = force.face;
                    report.wall[axis] += force.wall;
                    report.gravity[axis] += force.gravity;
                    report.friction[axis] += force.friction;
                    report.dropped[axis] += force.dropped;
                    if (force.desired == 0)
                        continue;

                    desired[axis][cell] = force.desired;
                    outgoing[face > 0 ? cell : neighbor] += force.desired;
                }
            }
        }

        // 上流 from から質量 mass(mg)の液体を下流 to へ(成分の比で物質量・液体のぶんのエネルギー)
        void LiquidStep::MoveLiquid(uint32_t from, uint32_t to, uint64_t mass) {
            const reaction::RxCell& cell = cells[from];
            const LqCellView& state = states[from];
            uint64_t movedCapacity = 0;
            int64_t movedChemical = 0;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                const uint32_t species = cell.species[i];
                if (!IsLiquid(phases, species))
                    continue;

                const uint64_t amount = liquid::LqMovedShare(cell.amounts[i], mass, state.mass);
                if (amount == 0)
                    continue;

                AddAmount(deltas[from], species, -(int64_t)amount);
                AddAmount(deltas[to], species, (int64_t)amount);
                movedCapacity += amount * table.species[species].heatCapacity;
                movedChemical += (int64_t)amount * table.species[species].h0;
            }

            if (movedCapacity == 0)
                return;

            const int64_t movedEnergy = liquid::LqMovedEnergy(state, movedCapacity, movedChemical);
            deltas[from].energy -= movedEnergy;
            deltas[to].energy += movedEnergy;
        }

        // 上限(出る合計が中身を超えるセルは比で縮める)と当てる(成分・エネルギー・位置のエネルギーの増減を包みに)。
        // 段 1 ④ の当てる段(T-0714。上限は出る側と入る側の倍率の min)へ載せ替える所
        void LiquidStep::LimitAndApply(ProbeLiquidStepReport& report) {
            const uint32_t count = grid.Count();
            deltas.assign(count, {});
            potentialCarried = potential;
            for (uint32_t cell = 0; cell < count; ++cell) {
                if (outgoing[cell] > states[cell].mass)
                    ++report.limitedCells;
            }

            for (uint32_t axis = 0; axis < 3; ++axis) {
                moved[axis].assign(count, 0);
                for (uint32_t cell = 0; cell < count; ++cell) {
                    const uint64_t flux = desired[axis][cell];
                    if (flux == 0)
                        continue;

                    const uint32_t neighbor = grid.Plus(cell, axis);
                    const bool forward = faces[axis][cell] > 0;
                    const uint32_t from = forward ? cell : neighbor;
                    const uint32_t to = forward ? neighbor : cell;
                    const uint64_t fromMass = states[from].mass;
                    const uint64_t mass = liquid::LqLimitOutflow(flux, fromMass, outgoing[from]);
                    if (mass == 0)
                        continue;

                    report.movedMilligrams += mass;
                    moved[axis][cell] = forward ? (int64_t)mass : -(int64_t)mass;
                    MoveLiquid(from, to, mass);

                    const auto share = (int64_t)liquid::LqMovedShare((uint64_t)potential[from], mass, fromMass);
                    potentialCarried[from] -= share;
                    potentialCarried[to] += share;
                }
            }
        }

        // 面の運動量(と運動のエネルギー)を、面のまわりの体積の境を通る質量の流れで風上に運ぶ(T-0610・ADR-0276)
        void LiquidStep::AdvectMomentum(ProbeLiquidStepReport& report) {
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
                        if (before[from] == 0)
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

                        // 閉じた面(壁・世界の端)へ入る運動量は壁が受ける
                        if (FaceOpenQ16(to, axis) == 0)
                            report.wall[axis] -= carried.momentum;
                        else
                            faces[axis][to] += carried.momentum;
                    });
                }
            }
        }

        // 液体を当て、まわりが空の面・速さの上限を直し、位置と運動のエネルギーを計算し直して差をセルの熱にする
        void LiquidStep::SettleEnergy(ProbeLiquidStepReport& report) {
            const uint32_t count = grid.Count();
            changed.assign(count, 0);
            std::vector<uint64_t> mass(count, 0);
            for (uint32_t cell = 0; cell < count; ++cell) {
                if (ApplyDelta(cells[cell], deltas[cell]))
                    changed[cell] = 1;

                mass[cell] = deltas[cell].touched ? LiquidMassMilligrams(phases, cells[cell]) : states[cell].mass;
                massBefore[cell] = (int64_t)mass[cell];
            }

            // --- 面: まわりが空になった面・速さの上限・運動のエネルギー(差は両側のセルへ半分ずつ)---
            std::vector<int64_t> heat(count, 0);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                for (uint32_t cell = 0; cell < count; ++cell) {
                    int64_t& face = faces[axis][cell];
                    FX_ASSERT(face == 0 || FaceOpenQ16(cell, axis) != 0);
                    const uint32_t neighbor = grid.Plus(cell, axis);
                    const bool inside = neighbor != NO_CELL;
                    const uint64_t doubled = inside ? mass[cell] + mass[neighbor] : 0;
                    const liquid::LqSettledFace settled = liquid::LqSettleFace(face, doubled,
                                                                               kineticCarried[axis][cell], params);
                    face = settled.face;
                    report.dropped[axis] += settled.dropped;
                    report.capped[axis] += settled.capped;
                    report.cappedFaces += settled.cappedFace;
                    kinetic[axis][cell] = settled.kinetic;
                    const int64_t released = settled.released;
                    if (released == 0)
                        continue;

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
                potential[cell] = settledPotential;
                if (heat[cell] == 0)
                    continue;

                cells[cell].energy += heat[cell];
                report.heat += heat[cell];
                changed[cell] = 1;
            }
        }

        // 外から入った運動量を帳簿へ(世界の帳簿の 1 型への合流 T-0762 で列 = 量 × 理由へ移す所)
        void LiquidStep::AddLedger(const ProbeLiquidStepReport& report) {
            for (uint32_t axis = 0; axis < 3; ++axis)
                ledger.external[axis] += report.wall[axis] + report.gravity[axis] + report.friction[axis] +
                                         report.dropped[axis] + report.capped[axis] + report.vanished[axis];
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
                                        LoadNestField(nest, fields.kinetic, slot, index, axis) != kinetic[axis][cell];
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
                    }

                    StoreNestField(nest, fields.potential, slot, index, 0, potential[cell]);
                    StoreNestField(nest, fields.massBefore, slot, index, 0, massBefore[cell]);
                }

                if (!cellsChanged)
                    continue;

                PokeNestBlock(nest, slot);
                changedSlots.push_back(slot);
            }

            for (uint32_t axis = 0; axis < 3; ++axis)
                StoreNestWorldField(nest, fields.ledgerMomentum, axis, ledger.external[axis]);

            StoreNestWorldField(nest, fields.ledgerEnergy, 0, ledger.energy);
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

        // --- 欄(面の運動量・運動のエネルギー・位置のエネルギーは保存量。前の質量は合わせ直しの元。帳簿は世界の欄)---
        constexpr uint32_t conserved = sf::SF_FLAG_CONSERVED | sf::SF_FLAG_HASHED;
        MultiresLiquidFields& fields = result.m_fields;
        fields.momentum = Register(nest, "liquid.momentum", sf::SF_PLACE_FACE, sf::SF_SCALE_VOLUME, conserved, 3);
        fields.kinetic = Register(nest, "liquid.kinetic_energy", sf::SF_PLACE_FACE, sf::SF_SCALE_VOLUME, conserved, 3);
        fields.potential = Register(nest, "liquid.potential_energy", sf::SF_PLACE_CELL, sf::SF_SCALE_VOLUME, conserved,
                                    1);
        fields.massBefore = Register(nest, "liquid.mass_before", sf::SF_PLACE_CELL, sf::SF_SCALE_VOLUME,
                                     sf::SF_FLAG_HASHED, 1);
        fields.ledgerMomentum = Register(nest, "liquid.ledger_momentum", sf::SF_PLACE_WORLD, sf::SF_SCALE_SAME,
                                         sf::SF_FLAG_HASHED, 3);
        fields.ledgerEnergy = Register(nest, "liquid.ledger_energy", sf::SF_PLACE_WORLD, sf::SF_SCALE_SAME,
                                       sf::SF_FLAG_HASHED, 1);
        for (const uint32_t field : {fields.momentum, fields.kinetic, fields.potential, fields.massBefore,
                                     fields.ledgerMomentum, fields.ledgerEnergy}) {
            if (field == MULTIRES_NO_FIELD)
                return std::unexpected("液体の欄を登録できない(名前が重なる・根を置いた後)");
        }

        return result;
    }

    MultiresLiquidStepReport MultiresLiquid::Step(MultiresNest& nest, const BakedReactionTable& table,
                                                  std::vector<uint32_t>& changedSlots) const {
        MultiresLiquidStepReport report;
        const LiquidGrid grid = BuildGrid(nest);
        const std::vector<multires::CvSpeciesPhase> phases = MakeSpeciesPhases(table);
        const uint32_t count = grid.Count();

        // --- 刻みの初めの値(セルの見立てと欄)---
        LiquidStep step{.table = table, .phases = phases, .params = m_params, .grid = grid};
        step.Read(nest, m_fields, report);
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

        step.ledger.energy += report.potentialEntered;
        step.FollowMassChange(report);

        // --- 圧力 → 面の力 → 上限と当てる → 運動量を運ぶ → エネルギーを熱へ → 帳簿 → 書き戻す(起こす)---
        step.kineticCarried = step.kinetic;
        step.ComputePressure(report);
        step.ApplyForces(report);
        step.LimitAndApply(report);
        if (m_options.advection)
            step.AdvectMomentum(report);

        step.SettleEnergy(report);
        step.AddLedger(report);
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
        ProbeLiquidLedger ledger;
        for (uint32_t axis = 0; axis < 3; ++axis)
            ledger.external[axis] = LoadNestWorldField(nest, m_fields.ledgerMomentum, axis);

        ledger.energy = LoadNestWorldField(nest, m_fields.ledgerEnergy, 0);

        return ledger;
    }

}  // namespace bicameral::sim
