// probe_liquid.cpp — 仮の世界に液体の流れを載せる CPU リファレンス(T-0609・ADR-0266)。形と約束は probe_liquid.h。
#include "sim/probe_liquid.h"

#include <algorithm>

#include "common/fixed.hlsli"
#include "common/probe_sim.hlsli"
#include "core/aliases.h"

namespace bicameral::sim {

    namespace {

        // 1 刻みに動く質量(mg)= 面の運動量 × 重力加速度 × 刻みの長さ² ÷ (2 × セルの 1 辺)。
        // 運動量 = 値 × g × dt ÷ 2、動く質量 = 運動量 × dt ÷ 1 辺 なので、分母 = 2 × 60² × 1 辺(µm)
        constexpr uint64_t FLUX_DENOMINATOR = 2ull * 60 * 60 * PROBE_LIQUID_CELL_EDGE_MILLIMETERS * 1000;

        constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
        constexpr uint64_t FNV_PRIME = 1099511628211ull;

        // 床で割る(負の数も下へ)
        int64_t DivideDown(int64_t value, int64_t divisor) {
            const int64_t quotient = value / divisor;
            return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
        }

        // a × b ÷ c(切り捨て。結果が 64bit に収まること)
        uint64_t MulDiv(uint64_t a, uint64_t b, uint64_t c) {
            return fx::FxDivU128By64(fx::FxMulU64Full(a, b), c).quotient;
        }

        bool IsLiquid(const BakedReactionTable& table, uint32_t species) {
            return species < table.speciesLiquid.size() && table.speciesLiquid[species] != 0;
        }

        // 液体でない凝縮相(固体)。少しでもあれば液体を通さない(仮。T-0612)
        bool IsSolid(const BakedReactionTable& table, uint32_t species) {
            return species < table.speciesCondensed.size() && table.speciesCondensed[species] != 0 &&
                   !IsLiquid(table, species);
        }

        uint64_t Mix(uint64_t hash, uint64_t value) {
            for (uint32_t byte = 0; byte < 8; ++byte) {
                hash ^= (value >> (byte * 8)) & 0xffu;
                hash *= FNV_PRIME;
            }

            return hash;
        }

        // 刻みの初めのセルから読んだ値(区画のセルごと)
        struct LiquidCellState {
            bool wall = false;
            uint64_t mass = 0;      // mg(1 mg 未満の液体は流れない)
            uint64_t volume = 0;    // pL
            int64_t heat = 0;       // µJ(= エネルギー × 1000 − 化学)
            uint64_t capacity = 0;  // nJ/K(セルの全部の成分)
        };

        LiquidCellState ReadState(const BakedReactionTable& table, const reaction::RxCell& cell) {
            LiquidCellState state;
            int64_t chemical = 0;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                const uint32_t species = cell.species[i];
                const uint64_t amount = cell.amounts[i];
                chemical += (int64_t)amount * table.species[species].h0;
                state.capacity += amount * table.species[species].heatCapacity;
                if (amount != 0 && IsSolid(table, species))
                    state.wall = true;
            }

            state.mass = ProbeLiquidField::LiquidMass(table, cell) / 1000000;  // ng → mg
            state.volume = ProbeLiquidField::LiquidVolume(table, cell);
            state.heat = (cell.energy * 1000) - chemical;
            FX_ASSERT(state.heat >= 0);

            return state;
        }

        // 下のセルが支える割合(pL の単位で、セルの体積 = 全部)。壁は全部・液体は満ち具合・空は 0
        uint64_t SupportBelow(const LiquidCellState& below) {
            if (below.wall)
                return PROBE_LIQUID_CELL_VOLUME_PICOLITERS;

            return std::min(below.volume, PROBE_LIQUID_CELL_VOLUME_PICOLITERS);
        }

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

        // 上流 from のセルから質量 mass(ng)の液体を下流 to へ(成分の比で物質量・液体のぶんのエネルギー)
        void MoveLiquid(const BakedReactionTable& table, const reaction::RxCell& cell, const LiquidCellState& state,
                        uint64_t mass, CellDelta& from, CellDelta& to) {
            uint64_t movedCapacity = 0;
            int64_t movedChemical = 0;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                const uint32_t species = cell.species[i];
                if (!IsLiquid(table, species))
                    continue;

                const uint64_t moved = MulDiv(cell.amounts[i], mass, state.mass);
                if (moved == 0)
                    continue;

                AddAmount(from, species, -(int64_t)moved);
                AddAmount(to, species, (int64_t)moved);
                movedCapacity += moved * table.species[species].heatCapacity;
                movedChemical += (int64_t)moved * table.species[species].h0;
            }

            if (movedCapacity == 0)
                return;

            // 熱は動く熱容量の比で(液体とセルの温度を同じと置く)。mJ へは床で(渡す側の熱が負にならない)
            const auto movedHeat = (int64_t)MulDiv((uint64_t)state.heat, movedCapacity, state.capacity);
            const int64_t movedEnergy = DivideDown(movedHeat + movedChemical, 1000);
            from.energy -= movedEnergy;
            to.energy += movedEnergy;
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
            FX_ASSERT(amounts.size() <= reaction::RX_MAX_CELL_SPECIES);  // 溢れのセル(RxWideCell)は R-MULTI-4

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

    }  // namespace

    std::expected<ProbeLiquidField, std::string> ProbeLiquidField::Create(const BakedReactionTable& table,
                                                                          const ProbeLiquidOptions& options) {
        if (!options.Enabled())
            return std::unexpected("液体の区画が空");

        for (uint32_t axis = 0; axis < 3; ++axis) {
            if (options.minCell[axis] + options.sizeCells[axis] > PROBE_GRID_SIZE)
                return std::unexpected("液体の区画が格子からはみ出す");
        }

        if (table.environment.gravityMicrometersPerSecond2 == 0)
            return std::unexpected("重力が 0(環境の値 environment.gravity が要る。無重力の液体は T-0610 の後で決める)");

        if (options.frictionShift == 0 || options.frictionShift > 30)
            return std::unexpected("摩擦の段(frictionShift)は 1〜30");

        ProbeLiquidField field;
        field.m_options = options;
        field.m_gravityMicrometersPerSecond2 = table.environment.gravityMicrometersPerSecond2;
        const uint32_t cells = options.sizeCells[0] * options.sizeCells[1] * options.sizeCells[2];
        for (auto& faces : field.m_faces)
            faces.assign(cells, 0);

        return field;
    }

    uint32_t ProbeLiquidField::RegionIndex(uint32_t x, uint32_t y, uint32_t z) const {
        return x + (m_options.sizeCells[0] * (z + (m_options.sizeCells[2] * y)));
    }

    uint32_t ProbeLiquidField::GridIndex(uint32_t region) const {
        const uint32_t width = m_options.sizeCells[0];
        const uint32_t depth = m_options.sizeCells[2];
        const uint32_t x = region % width;
        const uint32_t z = (region / width) % depth;
        const uint32_t y = region / (width * depth);

        return ProbeCellIndex(m_options.minCell[0] + x, m_options.minCell[1] + y, m_options.minCell[2] + z);
    }

    uint64_t ProbeLiquidField::LiquidMass(const BakedReactionTable& table, const reaction::RxCell& cell) {
        uint64_t mass = 0;
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            if (IsLiquid(table, cell.species[i]))
                mass += cell.amounts[i] * table.molarMasses[cell.species[i]];
        }

        return mass;
    }

    uint64_t ProbeLiquidField::LiquidVolume(const BakedReactionTable& table, const reaction::RxCell& cell) {
        uint64_t volume = 0;
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            if (IsLiquid(table, cell.species[i]))
                volume += cell.amounts[i] * table.speciesMolarVolume[cell.species[i]];
        }

        return volume;
    }

    int64_t ProbeLiquidField::MomentumSum(uint32_t axis) const {
        int64_t sum = 0;
        for (const int64_t value : m_faces[axis])
            sum += value;

        return sum;
    }

    uint64_t ProbeLiquidField::Hash() const {
        uint64_t hash = FNV_OFFSET;
        for (const auto& faces : m_faces) {
            for (const int64_t value : faces)
                hash = Mix(hash, (uint64_t)value);
        }

        for (uint32_t axis = 0; axis < 3; ++axis)
            hash = Mix(hash, (uint64_t)m_ledger.external[axis]);

        return hash;
    }

    ProbeLiquidStepReport ProbeLiquidField::Step(const BakedReactionTable& table, std::span<reaction::RxCell> cells,
                                                 std::vector<uint32_t>& changed) {
        ProbeLiquidStepReport report;
        const auto size = m_options.sizeCells;
        const uint32_t count = RegionCellCount();
        const std::array<uint32_t, 3> stride = {1, size[0] * size[2], size[0]};  // 区画の中の隣(格子の軸 x・y・z)

        // --- 刻みの初めの値(Jacobi 型: ここから先は全部この値から決める)---
        std::vector<LiquidCellState> states(count);
        for (uint32_t region = 0; region < count; ++region) {
            states[region] = ReadState(table, cells[GridIndex(region)]);
            if (states[region].mass != 0)
                ++report.wetCells;
        }

        if (report.wetCells == 0 && rng::all_of(m_faces, [](const auto& faces) {
                return rng::all_of(faces, [](int64_t value) { return value == 0; });
            }))
            return report;  // 液体が無く、動いている面も無い(ブランクの世界・液体の無い表)

        // --- 圧力(質量の単位。P = 2 × 支えている上の液体 + 自分 + 溢れ)。列ごとに上から ---
        std::vector<int64_t> pressure(count, 0);
        for (uint32_t z = 0; z < size[2]; ++z) {
            for (uint32_t x = 0; x < size[0]; ++x) {
                uint64_t above = 0;
                for (uint32_t y = size[1]; y-- > 0;) {
                    const uint32_t region = RegionIndex(x, y, z);
                    const LiquidCellState& state = states[region];
                    if (state.wall || state.mass == 0) {
                        above = 0;
                        continue;
                    }

                    uint64_t overfill = 0;
                    if (state.volume > PROBE_LIQUID_CELL_VOLUME_PICOLITERS) {
                        ++report.overfilledCells;
                        overfill = MulDiv(state.mass * m_options.overfillStiffness,
                                          state.volume - PROBE_LIQUID_CELL_VOLUME_PICOLITERS, state.volume);
                    }

                    // 満ちていないセルは上の液体を満ち具合の分だけ支える(残りは落ちてくる力になる)
                    const uint64_t filled = std::min(state.volume, PROBE_LIQUID_CELL_VOLUME_PICOLITERS);
                    const uint64_t supported = MulDiv(above, filled, PROBE_LIQUID_CELL_VOLUME_PICOLITERS);
                    // 下に支えられている分だけ圧力になる(下が空のセルは落ちている途中で、横へ押し広げない)
                    const uint64_t support = y == 0 ? PROBE_LIQUID_CELL_VOLUME_PICOLITERS
                                                    : SupportBelow(states[RegionIndex(x, y - 1, z)]);
                    pressure[region] = (int64_t)MulDiv((2 * supported) + state.mass + overfill, support,
                                                       PROBE_LIQUID_CELL_VOLUME_PICOLITERS);
                    above = supported + state.mass;
                }
            }
        }

        // --- 面の運動量(力・壁・重さ・行き場なし・摩擦)と、望む動く質量 ---
        std::array<std::vector<uint64_t>, 3> desired;
        std::vector<uint64_t> outgoing(count, 0);
        for (uint32_t axis = 0; axis < 3; ++axis) {
            desired[axis].assign(count, 0);
            for (uint32_t region = 0; region < count; ++region) {
                const uint32_t x = region % size[0];
                const uint32_t z = (region / size[0]) % size[2];
                const uint32_t y = region / (size[0] * size[2]);
                const std::array<uint32_t, 3> at = {x, y, z};
                const LiquidCellState& a = states[region];
                int64_t& face = m_faces[axis][region];

                // − 側が区画の外か壁なら、その面の押し返しは壁から(+ 向き)
                const bool minusClosed = at[axis] == 0 || states[region - stride[axis]].wall;
                if (!a.wall && minusClosed)
                    report.wall[axis] += pressure[region];

                // + 側の面
                const bool inside = at[axis] + 1 < size[axis];
                const uint32_t neighbor = inside ? region + stride[axis] : region;
                const bool open = inside && !a.wall && !states[neighbor].wall;
                if (!open) {
                    if (!a.wall)
                        report.wall[axis] -= pressure[region];

                    // 閉じた面に残った運動量(壁が置かれた)は壁が受ける
                    report.wall[axis] -= face;
                    face = 0;
                    continue;
                }

                const LiquidCellState& b = states[neighbor];
                face += pressure[region] - pressure[neighbor];
                if (axis == 1) {
                    const auto weight = (int64_t)(a.mass + b.mass);
                    face -= weight;
                    report.gravity[axis] -= weight;
                }

                // 上流が空なら、運動量の行き場が無い(移流が無いぶん。T-0610)
                const LiquidCellState& upwind = face > 0 ? a : b;
                if (upwind.mass == 0) {
                    report.dropped[axis] -= face;
                    face = 0;
                    continue;
                }

                const int64_t friction = face / (int64_t{1} << m_options.frictionShift);
                face -= friction;
                report.friction[axis] -= friction;

                const uint64_t flux = MulDiv(fx::FxAbsU64(face), m_gravityMicrometersPerSecond2, FLUX_DENOMINATOR);
                desired[axis][region] = flux;
                outgoing[face > 0 ? region : neighbor] += flux;
            }
        }

        for (uint32_t axis = 0; axis < 3; ++axis)
            m_ledger.external[axis] += report.wall[axis] + report.gravity[axis] + report.friction[axis] +
                                       report.dropped[axis];

        // --- 動かす(出る合計が中身を超えるセルは比で縮める)---
        std::vector<CellDelta> deltas(count);
        for (uint32_t region = 0; region < count; ++region) {
            if (outgoing[region] > states[region].mass)
                ++report.limitedCells;
        }

        for (uint32_t axis = 0; axis < 3; ++axis) {
            for (uint32_t region = 0; region < count; ++region) {
                const uint64_t flux = desired[axis][region];
                if (flux == 0)
                    continue;

                const uint32_t neighbor = region + stride[axis];
                const bool forward = m_faces[axis][region] > 0;
                const uint32_t from = forward ? region : neighbor;
                const uint32_t to = forward ? neighbor : region;
                const LiquidCellState& state = states[from];
                const uint64_t mass = outgoing[from] > state.mass ? MulDiv(flux, state.mass, outgoing[from]) : flux;
                if (mass == 0)
                    continue;

                report.movedMilligrams += mass;
                MoveLiquid(table, cells[GridIndex(from)], state, mass, deltas[from], deltas[to]);
            }
        }

        // --- セルへ書き戻す ---
        for (uint32_t region = 0; region < count; ++region) {
            const uint32_t index = GridIndex(region);
            if (!ApplyDelta(cells[index], deltas[region]))
                continue;

            changed.push_back(index);
            ++report.changedCells;
        }

        return report;
    }

}  // namespace bicameral::sim
