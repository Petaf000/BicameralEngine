// multires_gas.cpp — 多重解像度の世界の 1 段で気体を流す CPU(運ぶ計算の共通の土台の段 3 ②。T-0720・T-0763・T-0765)。約束は multires_gas.h。
// 1 刻み = 一覧と面の一覧(座標から)→ 見立て(セルと欄を読む・気体の空き)→ セル ⇔ 気体の箱の写し(sim/gas_cells)→ 小刻み(当てる段)
// → 粘り → 静止の丸め → 書き戻す(起こす)・世界の帳簿。
// 小刻みは仮の世界と同じ関数(gas_reference の Substep。面の一覧を渡すだけ)で、回し方も同じ Jacobi 型なので、セルを並べる順
// (ブロックの順 × ブロックの中の番号)が仮の世界と違っても値はビット単位で同じ。
#include "sim/multires_gas.h"

#include <algorithm>
#include <span>

#include "common/cell_view.hlsli"
#include "common/fixed.hlsli"
#include "common/world_ledger.hlsli"
#include "core/aliases.h"
#include "core/log.h"
#include "sim/cell_view.h"
#include "sim/face_opening.h"
#include "sim/multires_fields.h"
#include "sim/world_physics.h"

namespace bicameral::sim {

    namespace {

        using multires::MR_BLOCK_CELLS;
        using multires::MR_BLOCK_EDGE;
        namespace sf = state_field;

        constexpr uint32_t NO_CELL = UINT32_MAX;
        constexpr uint32_t UP_AXIS = 2;  // 気体の箱の縦(z)。世界の上の軸(WE_UP_AXIS)は BuildGrid がここへ写す

        // --- 1 段の世界のセルの並び(刻むブロックの順 × ブロックの中の番号)と世界の箱の座標 ---
        struct GasGrid {
            std::vector<uint32_t> slots;                         // 刻むブロックの枠(枠の順)
            std::array<uint32_t, GAS_AXES> size = {};            // 世界の箱(セル。根のブロックの外接の箱)
            std::vector<uint32_t> boxToCell;                     // 箱の番号 → セルの番号(NO_CELL = ブロックの無い抜け)
            std::vector<std::array<uint32_t, GAS_AXES>> places;  // セルの番号 → 箱の座標

            [[nodiscard]] uint32_t Count() const { return (uint32_t)places.size(); }

            // 箱の番号(x が最も速く回る。気体の箱の GasBox::Index と同じ = 面の番号と開いた端の外の層が仮の世界と同じ数)
            [[nodiscard]] uint32_t BoxIndex(std::array<uint32_t, GAS_AXES> place) const {
                return place[0] + (size[0] * (place[1] + (size[1] * place[2])));
            }
        };

        // ブロックの原点を気体の箱の軸の順に(横 1・横 2・縦。世界の上の軸 WE_UP_AXIS を縦へ。world_edge.hlsli の WeAxisUpLast)
        std::array<int64_t, GAS_AXES> GasOrigin(const multires::MrBlock& block) {
            std::array<int64_t, GAS_AXES> origin = {};
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                origin[axis] = multires::WeOriginAxis(block, multires::WeAxisUpLast(axis));

            return origin;
        }

        // 刻むブロックの一覧(根のレベルの本物のブロック全部)。1 段の世界だけ(子・違うレベルは断る)
        GasGrid BuildGrid(const MultiresNest& nest) {
            GasGrid grid;
            std::array<int64_t, GAS_AXES> low = {INT64_MAX, INT64_MAX, INT64_MAX};
            std::array<int64_t, GAS_AXES> high = {INT64_MIN, INT64_MIN, INT64_MIN};
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                const multires::MrBlock& block = nest.blocks[slot];
                if (block.kind != multires::MR_BLOCK_REAL)
                    continue;

                FX_ASSERT(block.level == nest.capacity.rootLevel);
                FX_ASSERT(rng::all_of(block.children, [](uint32_t child) { return child == multires::MR_NO_BLOCK; }));
                grid.slots.push_back(slot);
                const std::array<int64_t, GAS_AXES> origin = GasOrigin(block);
                for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                    low[axis] = std::min(low[axis], origin[axis]);
                    high[axis] = std::max(high[axis], origin[axis] + (int64_t)MR_BLOCK_EDGE);
                }
            }

            if (grid.slots.empty())
                return grid;

            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                grid.size[axis] = (uint32_t)(high[axis] - low[axis]);

            grid.boxToCell.assign((size_t)grid.size[0] * grid.size[1] * grid.size[2], NO_CELL);
            for (uint32_t order = 0; order < grid.slots.size(); ++order) {
                const multires::MrBlock& block = nest.blocks[grid.slots[order]];
                const std::array<int64_t, GAS_AXES> origin = GasOrigin(block);
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    // ブロックの中の座標を気体の箱の軸の順に(GasOrigin と同じ写し)
                    std::array<uint32_t, GAS_AXES> local = {};
                    for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                        local[axis] = multires::WeCellAxis(index, multires::WeAxisUpLast(axis));

                    std::array<uint32_t, GAS_AXES> place = {};
                    for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                        place[axis] = (uint32_t)(origin[axis] - low[axis]) + local[axis];

                    grid.boxToCell[grid.BoxIndex(place)] = (uint32_t)grid.places.size();
                    grid.places.push_back(place);
                }
            }

            return grid;
        }

        // --- 面の一覧(gas_reference の EnumerateFaces と同じ面・同じ番号。端は世界の端の種類から)---
        struct LinkMaker {
            const GasGrid& grid;
            const multires::WeWorld& edges;  // 世界の端の種類(MultiresNest::edges.world。T-0721・T-0772)

            // 箱の外の側: 端が開いていれば外の層(面を作ったセルの層。仮の世界と同じ)・閉じていれば壁
            [[nodiscard]] GasFaceSide EdgeSide(uint32_t axis, bool high, uint32_t layer) const {
                const uint32_t edge = axis == UP_AXIS ? (high ? edges.top : edges.bottom) : edges.sides;
                if (edge == multires::WE_OPEN)
                    return {.kind = GasSideKind::Ghost, .index = layer};

                return {.kind = GasSideKind::Wall, .index = 0};
            }

            // セル place から軸 axis の座標を coordinate に替えた所の側(箱の外は端・抜けは壁)
            [[nodiscard]] GasFaceSide SideAt(std::array<uint32_t, GAS_AXES> place, uint32_t axis,
                                             int64_t coordinate) const {
                if (coordinate < 0 || coordinate >= (int64_t)grid.size[axis])
                    return EdgeSide(axis, coordinate >= 0, place[UP_AXIS]);

                place[axis] = (uint32_t)coordinate;
                const uint32_t cell = grid.boxToCell[grid.BoxIndex(place)];
                if (cell == NO_CELL)
                    return {.kind = GasSideKind::Wall, .index = 0};

                return {.kind = GasSideKind::Cell, .index = cell};
            }

            // セル place の軸 axis のマイナス側の面(high ならプラス側の面。left の座標から 4 つの側)
            [[nodiscard]] GasFaceLink Make(std::array<uint32_t, GAS_AXES> place, uint32_t axis, bool high) const {
                const int64_t left = high ? (int64_t)place[axis] : (int64_t)place[axis] - 1;
                GasFaceLink link;
                link.axis = axis;
                link.leftOuter = SideAt(place, axis, left - 1);
                link.left = SideAt(place, axis, left);
                link.right = SideAt(place, axis, left + 1);
                link.rightOuter = SideAt(place, axis, left + 2);
                link.id = ((grid.BoxIndex(place) * GAS_AXES) + axis) * 2 + (high ? 1 : 0);

                return link;
            }

            // どの面も 1 回だけ: セルのマイナス側の面 + プラス側の隣がセルでない(端・抜け)面
            [[nodiscard]] std::vector<GasFaceLink> All() const {
                std::vector<GasFaceLink> links;
                links.reserve(((size_t)grid.Count() * GAS_AXES) + 64);
                for (const std::array<uint32_t, GAS_AXES>& place : grid.places) {
                    for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                        links.push_back(Make(place, axis, false));
                        if (SideAt(place, axis, (int64_t)place[axis] + 1).kind != GasSideKind::Cell)
                            links.push_back(Make(place, axis, true));
                    }
                }

                return links;
            }
        };

        // 刻みの初めのセル・気体の空き(セルの見立て。一様なブロックは 1 回)・欄を読む
        struct GasRead {
            std::vector<reaction::RxCell> cells;
            std::vector<uint32_t> open;  // Q16。全部のセルが全部なら空(仮の世界と同じ = 空きを読まない見立ての値)
            // 面の塞ぐ割合(Q16。気体の箱の軸ごと・セルの + 側の面。面の開き具合の欄の写し。塞ぐ面が無ければ空。T-0976)
            std::array<std::vector<uint32_t>, GAS_AXES> blocked;
            GasCellFields fields;
        };

        GasRead ReadCells(const MultiresNest& nest, const BakedReactionTable& table, const GasGrid& grid,
                          const MultiresGasFields& ids, MultiresGasStepReport& report) {
            const std::vector<multires::CvSpeciesPhase> phases = MakeSpeciesPhases(table);
            const CellViewTable viewTable = {.reaction = table.View(), .phases = phases};
            GasRead read;
            read.cells.resize(grid.Count());
            read.open.resize(grid.Count());
            read.fields.Assign(grid.Count());
            bool partial = false;
            for (uint32_t order = 0; order < grid.slots.size(); ++order) {
                const uint32_t slot = grid.slots[order];
                const multires::MrBlock& block = nest.blocks[slot];
                const bool uniform = multires::MrIsUniform(block);
                const uint32_t uniformOpen = uniform ? multires::MakeCellView(viewTable, LoadNestCell(nest, slot, 0),
                                                                              block.level, 0)
                                                           .gasOpen
                                                     : 0;
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const uint32_t cell = (order * MR_BLOCK_CELLS) + index;
                    read.cells[cell] = LoadNestCell(nest, slot, index);
                    read.open[cell] = uniform
                                          ? uniformOpen
                                          : multires::MakeCellView(viewTable, read.cells[cell], block.level, 0).gasOpen;
                    if (read.open[cell] != GAS_OPEN_FULL) {
                        partial = true;
                        if (read.open[cell] == 0)
                            ++report.blockedCells;
                        else
                            ++report.partialCells;
                    }

                    for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                        read.fields.momentum[cell][axis] = LoadNestField(nest, ids.momentum, slot, index, axis);

                    read.fields.kinetic[cell] = LoadNestField(nest, ids.kinetic, slot, index, 0);
                    read.fields.mass[cell] = (uint64_t)LoadNestField(nest, ids.massBefore, slot, index, 0);
                }
            }

            if (!partial)
                read.open.clear();

            return read;
        }

        // 面の開き具合の欄(sim/face_opening。登録した世界だけ。名前で引く。T-0976・ADR-0420)を気体の箱の軸の順に写す。
        // 欄の軸は世界の軸なので、箱の軸 a は世界の軸 WeAxisUpLast(a) を読む。塞ぐ面が 1 つも無ければ写さない(今とビット単位で同じ)。
        // 塞ぐ面がある時は空きを読む箱にする(空きが全部 1 でも。面の開き = min(両側の空き, 1 − 塞ぐ割合) を空きを読む式で読む)
        void ReadFaceBlocked(const MultiresNest& nest, const GasGrid& grid, GasRead& read) {
            const uint32_t field = FindNestField(nest, FACE_BLOCKED_FIELD_NAME);
            if (field == MULTIRES_NO_FIELD)
                return;

            bool any = false;
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                read.blocked[axis].assign(grid.Count(), 0);

            for (uint32_t order = 0; order < grid.slots.size(); ++order) {
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    const uint32_t cell = (order * MR_BLOCK_CELLS) + index;
                    for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                        const uint32_t worldAxis = multires::WeAxisUpLast(axis);
                        read.blocked[axis][cell] = LoadFaceBlockedQ16(nest, field, grid.slots[order], index, worldAxis);
                        any = any || read.blocked[axis][cell] != 0;
                    }
                }
            }

            if (!any) {
                for (std::vector<uint32_t>& axisBlocked : read.blocked)
                    axisBlocked.clear();

                return;
            }

            if (read.open.empty())
                read.open.assign(grid.Count(), GAS_OPEN_FULL);
        }

        // 変わったブロックだけ書き戻す(セルか欄が変わったブロックを頁に広げる。セルが変わったブロックはつつく)
        void WriteCells(MultiresNest& nest, const GasGrid& grid, const MultiresGasFields& ids, const GasRead& read,
                        const std::vector<uint8_t>& changed, std::vector<uint32_t>& changedSlots) {
            for (uint32_t order = 0; order < grid.slots.size(); ++order) {
                const uint32_t slot = grid.slots[order];
                const uint32_t first = order * MR_BLOCK_CELLS;
                bool cellsChanged = false;
                bool fieldsChanged = false;
                for (uint32_t index = 0; index < MR_BLOCK_CELLS && !fieldsChanged; ++index) {
                    const uint32_t cell = first + index;
                    cellsChanged = cellsChanged || changed[cell] != 0;
                    for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                        fieldsChanged = fieldsChanged || LoadNestField(nest, ids.momentum, slot, index, axis) !=
                                                             read.fields.momentum[cell][axis];
                    }

                    fieldsChanged = fieldsChanged ||
                                    LoadNestField(nest, ids.kinetic, slot, index, 0) != read.fields.kinetic[cell] ||
                                    LoadNestField(nest, ids.massBefore, slot, index, 0) !=
                                        (int64_t)read.fields.mass[cell];
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
                        StoreNestCell(nest, slot, index, read.cells[cell]);

                    for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                        StoreNestField(nest, ids.momentum, slot, index, axis, read.fields.momentum[cell][axis]);

                    StoreNestField(nest, ids.kinetic, slot, index, 0, read.fields.kinetic[cell]);
                    StoreNestField(nest, ids.massBefore, slot, index, 0, (int64_t)read.fields.mass[cell]);
                }

                if (!cellsChanged)
                    continue;

                PokeNestBlock(nest, slot);
                changedSlots.push_back(slot);
            }
        }

        // --- 帳簿(世界の帳簿の列 = 量 × 理由。T-0763・T-0762 の書き方の約束)---

        // 帳簿の 1 つの量を載せる(符号つき: 正 → direction・負 → 逆の向き)
        void AddLedgerSigned(MultiresNest& nest, uint32_t reason, uint32_t direction, uint32_t column, int64_t amount) {
            if (amount == 0)
                return;

            const multires::WlSignedEntry entry = multires::WlEnergyEntry(amount, direction);
            AddWorldLedgerUnits(nest, multires::WlSlot(reason, entry.direction), nest.capacity.rootLevel, column,
                                entry.magnitude);
        }

        // エネルギーの列・運動量の列に気体が載せる理由(LedgerEnergy・LedgerMomentum が足す。運動のエネルギーの欄 ⇔ セルの熱の
        // 物理の熱・静止の丸めは中の移し替えなので数えない)
        constexpr std::array<uint32_t, 2> ENERGY_REASONS = {multires::WL_REASON_WORLD_EDGE,
                                                            multires::WL_REASON_GRAVITY_WORK};
        constexpr std::array<uint32_t, 5> MOMENTUM_REASONS = {
            multires::WL_REASON_WORLD_EDGE, multires::WL_REASON_WALL_IMPULSE, multires::WL_REASON_GRAVITY_WORK,
            multires::WL_REASON_FOLLOW_MASS, multires::WL_REASON_REST_ROUNDING};

        // 物質 = 世界の端(開いた端の出入りだけが物質を変える)。エネルギーの列 = 世界の端・重さの仕事(正 → 入)。
        // 運動のエネルギーの欄から熱になった分 kineticHeat(刻みの初めの欄の和 − 終わりの欄の和。粘り・衝撃・欄が物質に付く・静止の丸め)は
        // 欄の列の出 + エネルギーの列の入(静止の丸めの分は「静止の丸め」、残りは「物理の熱」。液体の T-0769 と同じ形)。
        // 運動量の列 = 世界の端・壁の力積・重さ・欄が物質に付く・静止の丸め(正 → 入)。
        // これで ComputeConservedTotals(Σ 保存量 − 帳簿)が気体の刻みで列ごとに変わらない
        void AddLedger(MultiresNest& nest, const MultiresGasFields& ids, const GasCellsLedger& ledger,
                       int64_t kineticHeat) {
            for (uint32_t id = 0; id < ledger.amounts.size(); ++id) {
                AddLedgerSigned(nest, multires::WL_REASON_WORLD_EDGE, multires::WL_DIRECTION_IN,
                                multires::WL_COLUMN_SPECIES_BASE + id, ledger.amounts[id]);
            }

            AddLedgerSigned(nest, multires::WL_REASON_WORLD_EDGE, multires::WL_DIRECTION_IN, multires::WL_COLUMN_ENERGY,
                            ledger.edgeEnergy);
            AddLedgerSigned(nest, multires::WL_REASON_GRAVITY_WORK, multires::WL_DIRECTION_IN,
                            multires::WL_COLUMN_ENERGY, ledger.gravityEnergy);

            // --- 運動のエネルギーの欄 → セルの熱 ---
            const uint32_t base = NestFieldLedgerBase(nest);
            const uint32_t kineticColumn = base + NestConservedIndex(nest, ids.kinetic);
            const std::array<int64_t, 2> heats = {kineticHeat - ledger.restEnergy, ledger.restEnergy};
            const std::array<uint32_t, 2> heatReasons = {multires::WL_REASON_PHYSICS_HEAT,
                                                         multires::WL_REASON_REST_ROUNDING};
            for (uint32_t part = 0; part < heats.size(); ++part) {
                AddLedgerSigned(nest, heatReasons[part], multires::WL_DIRECTION_OUT, kineticColumn, heats[part]);
                AddLedgerSigned(nest, heatReasons[part], multires::WL_DIRECTION_IN, multires::WL_COLUMN_ENERGY,
                                heats[part]);
            }

            // --- 運動量の列(気体の箱の軸 = 欄の値の番号)---
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                const uint32_t column = base + NestConservedIndex(nest, ids.momentum) + axis;
                const std::array<int64_t, MOMENTUM_REASONS.size()> parts = {
                    ledger.edgeMomentum[axis], ledger.wallMomentum[axis], ledger.gravityMomentum[axis],
                    ledger.followMomentum[axis], ledger.restMomentum[axis]};
                for (uint32_t part = 0; part < parts.size(); ++part)
                    AddLedgerSigned(nest, MOMENTUM_REASONS[part], multires::WL_DIRECTION_IN, column, parts[part]);
            }
        }

        int64_t SumKinetic(const GasCellFields& fields) {
            int64_t sum = 0;
            for (const int64_t kinetic : fields.kinetic)
                sum += kinetic;

            return sum;
        }

        // 層ごとの基準の大気(層の数 layerCount・厚さ = 根のレベルのセルの 1 辺。層 k の値は下の層だけで決まる)
        GasAtmosphereLayers MakeLayers(const MultiresNest& nest, const BakedReactionTable& table, uint32_t layerCount,
                                       const MultiresGasOptions& options) {
            return GasAtmosphereLayers::Create(table, options.floorHeightMillimeters, layerCount,
                                               nest.capacity.rootLevel);
        }

        // 気体の箱の設定(物質 slots・大きさ size〔気体の箱の軸〕・小刻み substeps〔× 2^σ 済み〕・セルのレベル = 根のレベル)
        GasConfig MakeBoxConfig(const MultiresNest& nest, const BakedReactionTable& table,
                                const GasAtmosphereLayers& layers, std::span<const uint32_t> slots,
                                std::array<uint32_t, GAS_AXES> size, const MultiresGasOptions& options,
                                uint32_t substeps) {
            constexpr std::array<GasBoundary, GAS_AXES> unused = {GasBoundary::Wall, GasBoundary::Wall,
                                                                  GasBoundary::Wall};  // 端は面の一覧が持つ
            GasConfig config = layers.MakeConfig(table, slots, size, unused, substeps, options.randomSeed);
            config.cellLevel = nest.capacity.rootLevel;

            return config;
        }

        uint32_t Register(MultiresNest& nest, const char* name, uint32_t place, uint32_t scale, uint32_t flags,
                          uint32_t components) {
            const uint32_t phase = place == sf::SF_PLACE_WORLD ? sf::SF_PHASE_NONE : sf::SF_PHASE_GAS;

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

    std::expected<MultiresGas, std::string> MultiresGas::Create(MultiresNest& nest, const BakedReactionTable& table,
                                                                const MultiresGasOptions& options) {
        if (table.atmosphere.Empty())
            return std::unexpected("基準の大気が無い世界では気体を流せない");

        if (table.environment.soundSpeedMillimetersPerSecond == 0)
            return std::unexpected("音速の基準が 0(環境の値 environment.sound_speed_mm_per_s が要る)");

        // --- レベルと小刻み(T-0765): 根のレベル k のセルは 1 辺 0.5 m × 2^-k。小刻みは substeps × 2^σ(σ = max(0, k)。
        //     細かいセルでも音の速さの数 ν をレベル 0 と同じに保つ。粗いセルは同じ小刻みのまま = ν が小さく安定)---
        const int32_t level = nest.capacity.rootLevel;
        if (level < GAS_MIN_CELL_LEVEL || level > GAS_MAX_CELL_LEVEL)
            return std::unexpected("根のレベルが気体のセルの範囲(GAS_MIN_CELL_LEVEL〜GAS_MAX_CELL_LEVEL)の外");

        const uint32_t sigma = level > 0 ? (uint32_t)level : 0;
        if (options.substeps == 0 || options.substeps > (UINT32_MAX >> sigma))
            return std::unexpected("小刻みの数が 0 か、レベルの 2^σ 倍が 32bit を超える");

        const uint32_t substeps = options.substeps << sigma;
        if (table.environment.soundSpeedMillimetersPerSecond > GasSoundSpeedLimitMmPerS(substeps, level))
            return std::unexpected("音速の基準が小刻みの安定の上限を超える");

        if (nest.wideCells)
            return std::unexpected("溢れのセルを使う世界の気体はまだ無い");

        if (nest.capacity.pages < nest.capacity.worldBlocks)
            return std::unexpected("頁が世界の枠より少ない(気体はどのブロックにも書くことがある)");

        MultiresGas result;
        result.m_options = options;
        result.m_substeps = substeps;

        // 世界が最速を選んだ(T-0804・D-526 ②): 多重解像度の気体は陽解法だけなので本物の音速で流し、作り手へ知らせる
        if (std::string notice = GasInstantNotYetNotice(table.environment, "多重解像度の世界の気体"); !notice.empty()) {
            Log(Channel::Sim, Level::Warning, "{}", notice);
            result.m_notices.push_back(std::move(notice));
        }

        // --- 欄(運動量・運動のエネルギーは保存量 = 世界の帳簿に列がある。前の質量は合わせ直しの元。T-0763)---
        constexpr uint32_t conserved = sf::SF_FLAG_CONSERVED | sf::SF_FLAG_HASHED;
        MultiresGasFields& fields = result.m_fields;
        fields.momentum = Register(nest, "gas.momentum", sf::SF_PLACE_CELL, sf::SF_SCALE_VOLUME, conserved, 3);
        fields.kinetic = Register(nest, "gas.kinetic_energy", sf::SF_PLACE_CELL, sf::SF_SCALE_VOLUME, conserved, 1);
        // 前の質量は保存量ではない(セルの気体の写し)ので余りを帳簿へ出さない SAME(液体の liquid.mass_before と同じ)
        fields.massBefore = Register(nest, "gas.mass_before", sf::SF_PLACE_CELL, sf::SF_SCALE_SAME, sf::SF_FLAG_HASHED,
                                     1);
        for (const uint32_t field : {fields.momentum, fields.kinetic, fields.massBefore}) {
            if (field == MULTIRES_NO_FIELD)
                return std::unexpected("気体の欄を登録できない(名前が重なる・根を置いた後)");
        }

        return result;
    }

    MultiresGasStepReport MultiresGas::Step(MultiresNest& nest, const BakedReactionTable& table, uint64_t tick,
                                            std::vector<uint32_t>& changedSlots) const {
        MultiresGasStepReport report;
        const GasGrid grid = BuildGrid(nest);
        if (grid.Count() == 0)
            return report;

        // --- 刻みの初めの値(セル・気体の空き・欄)---
        GasRead read = ReadCells(nest, table, grid, m_fields, report);
        ReadFaceBlocked(nest, grid, read);

        // --- セル → 気体の箱(層 = 世界の箱の z。基準の大気は一番下の層の高さから。層の厚さ・係数は根のレベル)---
        const GasAtmosphereLayers layers = MakeLayers(nest, table, grid.size[UP_AXIS], m_options);
        GasCellsStep step;
        step.slots = CollectGasSpecies(table, read.cells, layers.Species());
        report.gasSpecies = (uint32_t)step.slots.size();

        const GasConfig config = MakeBoxConfig(nest, table, layers, step.slots, grid.size, m_options, m_substeps);
        step.box = MakeGasBox(config, layers.MakeReference(config, step.slots));
        step.box.cells.resize(grid.Count());  // 抜けのある世界は箱より少ない
        step.box.tick = tick;
        step.box.openQ16 = read.open;
        step.box.faceBlockedQ16 = read.blocked;
        step.box.cellLayers.resize(grid.Count());
        for (uint32_t cell = 0; cell < grid.Count(); ++cell)
            step.box.cellLayers[cell] = grid.places[cell][UP_AXIS];

        LoadGasCells(table, read.cells, read.fields, step);

        // --- 1 刻み(陽解法の小刻み。面の一覧は世界の箱の座標から)---
        const std::vector<GasFaceLink> links = LinkMaker{.grid = grid, .edges = nest.edges.world}.All();
        StepGasLinks(step.box, links);

        // --- 静止の丸め(当てる段・粘りの後)→ 気体の箱 → セル・欄・帳簿 → 書き戻す(起こす)---
        GasCellsLedger ledger{.amounts = std::vector<int64_t>(table.species.size(), 0)};
        RestGasCells(step, m_options.restSpeedUnits, ledger);
        const int64_t kineticBefore = SumKinetic(read.fields);
        std::vector<uint8_t> changed;
        if (!StoreGasCells(table, step, read.cells, read.fields, ledger, changed)) {
            report.skippedCellOverflow = true;
            return report;
        }

        AddLedger(nest, m_fields, ledger, kineticBefore - SumKinetic(read.fields));
        report.restedCells = ledger.restedCells;
        report.energyLimitedFaces = step.box.energyLimitedFaces;
        report.changedCells = (uint32_t)rng::count(changed, uint8_t{1});
        WriteCells(nest, grid, m_fields, read, changed, changedSlots);

        return report;
    }

    int64_t MultiresGas::MomentumSum(const MultiresNest& nest, uint32_t axis) const {
        return SumCellField(nest, m_fields.momentum, axis);
    }

    int64_t MultiresGas::KineticEnergySum(const MultiresNest& nest) const {
        return SumCellField(nest, m_fields.kinetic, 0);
    }

    int64_t MultiresGas::LedgerEnergy(const MultiresNest& nest) const {
        int64_t energy = 0;
        for (const uint32_t reason : ENERGY_REASONS)
            energy += WorldLedgerNetUnits(nest, reason, multires::WL_COLUMN_ENERGY, nest.capacity.rootLevel);

        return energy;
    }

    std::array<int64_t, GAS_AXES> MultiresGas::LedgerMomentum(const MultiresNest& nest) const {
        std::array<int64_t, GAS_AXES> momentum = {};
        const uint32_t first = NestFieldLedgerBase(nest) + NestConservedIndex(nest, m_fields.momentum);
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
            for (const uint32_t reason : MOMENTUM_REASONS)
                momentum[axis] += WorldLedgerNetUnits(nest, reason, first + axis, nest.capacity.rootLevel);
        }

        return momentum;
    }

    reaction::RxCell MultiresGas::ReferenceCell(const MultiresNest& nest, const BakedReactionTable& table,
                                                uint32_t layer) const {
        const GasAtmosphereLayers layers = MakeLayers(nest, table, layer + 1, m_options);
        const std::span<const uint32_t> atmosphere = layers.Species();
        const GasConfig config = MakeBoxConfig(nest, table, layers, atmosphere, {1, 1, layer + 1}, m_options,
                                               m_substeps);
        const GasCell reference = layers.MakeReference(config, atmosphere)[layer];

        reaction::RxCell cell = reaction::RxMakeEmptyCell(reference.energy);
        for (uint32_t s = 0; s < config.speciesCount; ++s) {
            if (reference.amounts[s] == 0)
                continue;

            cell.species[cell.speciesCount] = atmosphere[s];
            cell.amounts[cell.speciesCount] = reference.amounts[s];
            ++cell.speciesCount;
        }

        return cell;
    }

}  // namespace bicameral::sim
