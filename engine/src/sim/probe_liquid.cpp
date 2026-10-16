// probe_liquid.cpp — 仮の世界に液体の流れを載せる CPU リファレンス(T-0609・ADR-0266。移流とエネルギーは T-0610・ADR-0276)。
// 形と約束は probe_liquid.h。1 刻み = 刻みの初めの値 → 圧力 → 面の力 → 質量を動かす → 運動量を運ぶ → エネルギーを熱へ。
#include "sim/probe_liquid.h"

#include <algorithm>

#include "common/fixed.hlsli"
#include "common/probe_region_hash.hlsli"
#include "common/probe_sim.hlsli"
#include "core/aliases.h"
#include "sim/cell_volume.h"

namespace bicameral::sim {

    namespace {

        // 1 刻みに動く質量(mg)= 面の運動量 × 重力加速度 × 刻みの長さ² ÷ (2 × セルの 1 辺)。
        // 運動量 = 値 × g × dt ÷ 2、動く質量 = 運動量 × dt ÷ 1 辺 なので、分母 = 2 × 60² × 1 辺(µm)
        constexpr uint64_t FLUX_DENOMINATOR = 2ull * 60 * 60 * PROBE_LIQUID_CELL_EDGE_MILLIMETERS * 1000;

        // 運動のエネルギー(mJ)= (面の運動量 × 重力加速度〔µm/s²〕)² ÷ (KINETIC_DENOMINATOR × 面の 2 倍の質量〔mg〕)。
        // 本当の運動量 p = 値 × 1e-6 × g × dt ÷ 2・面の質量 M = 2 倍の質量 ÷ 2 から p² ÷ 2M を mJ に直した分母(14400 × 1e15)
        constexpr uint64_t KINETIC_DENOMINATOR = 14400000000000000000ull;

        // 位置のエネルギー(mJ)= 質量(mg)× 高さ(mm)× 重力加速度(µm/s²)÷ 1e12
        constexpr uint64_t POTENTIAL_DENOMINATOR = 1000000000000ull;

        // 速さ(µm/s)→ 面の運動量の上限 = 速さ × 60 × 2 倍の質量 ÷ 重力加速度(刻みは 1/60 秒)
        constexpr uint64_t TICKS_PER_SECOND = 60;

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

        // value × numerator ÷ denominator(0 の向きへ切り捨て)
        int64_t ScaleSigned(int64_t value, uint64_t numerator, uint64_t denominator) {
            return fx::FxApplySign(MulDiv(fx::FxAbsU64(value), numerator, denominator), value < 0);
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
            bool wall = false;    // 固体で満ちている(空き 0)
            uint64_t mass = 0;    // mg(1 mg 未満の液体は流れない)
            uint64_t volume = 0;  // pL
            // --- セルの空き(固体が占めない体積。T-0612・ADR-0268。sim/cell_volume)---
            uint32_t open = CELL_OPEN_FULL;  // Q16
            uint64_t room = 0;               // 液体が入れる体積(pL。セル − 固体)
            uint64_t equivalent =
                0;  // 空きが液体で満ちた時の質量を、セル全部の液体に直したもの(mg = 質量 ÷ 空き)。圧力はこれで書く
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
            }

            state.mass = ProbeLiquidField::LiquidMass(table, cell) / 1000000;  // ng → mg
            state.volume = ProbeLiquidField::LiquidVolume(table, cell);

            // --- 固体が占める体積を除いた空き(密度を書いていない固体は場所を取らない)---
            const CellVolumeShare share = ShareCellVolume(table, cell);
            state.open = LiquidOpenQ16(share, PROBE_LIQUID_CELL_VOLUME_PICOLITERS);
            state.wall = state.open == 0;
            state.room = PROBE_LIQUID_CELL_VOLUME_PICOLITERS -
                         std::min(share.solid, PROBE_LIQUID_CELL_VOLUME_PICOLITERS);
            state.equivalent = state.open == CELL_OPEN_FULL || state.open == 0
                                   ? state.mass
                                   : MulDiv(state.mass, CELL_OPEN_FULL, state.open);
            state.heat = (cell.energy * 1000) - chemical;
            FX_ASSERT(state.heat >= 0);

            return state;
        }

        // 下のセルが支える割合(分子, 分母)。固体で満ちたセルは全部・液体は空きの満ち具合・空は 0
        // (孔のある固体は、孔が液体で満ちるまで上の液体を支えない = 上の水は孔に染み込む)
        std::array<uint64_t, 2> SupportBelow(const LiquidCellState& below) {
            if (below.wall)
                return {1, 1};

            return {std::min(below.volume, below.room), below.room};
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

        // 1 刻みに 1 セル(0.5 m × 60 = 30 m/s)を超える速さは、流れがセルを飛び越えるので断る
        if (options.maxSpeedMillimetersPerSecond == 0 ||
            options.maxSpeedMillimetersPerSecond > PROBE_LIQUID_CELL_EDGE_MILLIMETERS * TICKS_PER_SECOND)
            return std::unexpected("速さの上限(maxSpeedMillimetersPerSecond)は 1〜30000");

        ProbeLiquidField field;
        field.m_options = options;
        field.m_gravityMicrometersPerSecond2 = table.environment.gravityMicrometersPerSecond2;
        const uint32_t cells = options.sizeCells[0] * options.sizeCells[1] * options.sizeCells[2];
        for (uint32_t axis = 0; axis < 3; ++axis) {
            field.m_faces[axis].assign(cells, 0);
            field.m_kinetic[axis].assign(cells, 0);
        }

        field.m_potential.assign(cells, 0);
        field.m_mass.assign(cells, 0);

        return field;
    }

    uint32_t ProbeLiquidField::RegionIndex(uint32_t x, uint32_t y, uint32_t z) const {
        return x + (m_options.sizeCells[0] * (z + (m_options.sizeCells[2] * y)));
    }

    std::array<uint32_t, 3> ProbeLiquidField::RegionCoordinates(uint32_t region) const {
        const uint32_t width = m_options.sizeCells[0];
        const uint32_t depth = m_options.sizeCells[2];

        return {region % width, region / (width * depth), (region / width) % depth};
    }

    bool ProbeLiquidField::AtRest() const {
        const auto zero = [](int64_t value) {
            return value == 0;
        };
        for (uint32_t axis = 0; axis < 3; ++axis) {
            if (!rng::all_of(m_faces[axis], zero) || !rng::all_of(m_kinetic[axis], zero))
                return false;
        }

        return rng::all_of(m_potential, zero);
    }

    int64_t ProbeLiquidField::PotentialOf(uint32_t region, uint64_t massMilligrams) const {
        const uint64_t heightMillimeters = (RegionCoordinates(region)[1] * PROBE_LIQUID_CELL_EDGE_MILLIMETERS) +
                                           (PROBE_LIQUID_CELL_EDGE_MILLIMETERS / 2);

        return (int64_t)MulDiv(massMilligrams * heightMillimeters, m_gravityMicrometersPerSecond2,
                               POTENTIAL_DENOMINATOR);
    }

    int64_t ProbeLiquidField::KineticOf(int64_t momentum, uint64_t doubledMass) const {
        if (momentum == 0 || doubledMass == 0)
            return 0;

        const fx::FxU128 scaled = fx::FxMulU64Full(fx::FxAbsU64(momentum), m_gravityMicrometersPerSecond2);
        FX_ASSERT(scaled.hi == 0);  // 速さの上限で収まる(上限 30 m/s・2 倍の質量 1e12 mg まで)
        const fx::FxU128 squared = fx::FxMulU64Full(scaled.lo, scaled.lo);

        return (int64_t)(fx::FxDivU128By64(squared, KINETIC_DENOMINATOR).quotient / doubledMass);
    }

    uint64_t ProbeLiquidField::MaxMomentum(uint64_t doubledMass) const {
        const uint64_t speedMicrometers = (uint64_t)m_options.maxSpeedMillimetersPerSecond * 1000;

        return MulDiv(speedMicrometers * TICKS_PER_SECOND, doubledMass, m_gravityMicrometersPerSecond2);
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

    int64_t ProbeLiquidField::PotentialEnergySum() const {
        int64_t sum = 0;
        for (const int64_t value : m_potential)
            sum += value;

        return sum;
    }

    int64_t ProbeLiquidField::KineticEnergySum() const {
        int64_t sum = 0;
        for (const auto& kinetic : m_kinetic) {
            for (const int64_t value : kinetic)
                sum += value;
        }

        return sum;
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

        for (const auto& kinetic : m_kinetic) {
            for (const int64_t value : kinetic)
                hash = Mix(hash, (uint64_t)value);
        }

        for (const int64_t value : m_potential)
            hash = Mix(hash, (uint64_t)value);

        for (const uint64_t value : m_mass)
            hash = Mix(hash, value);

        for (uint32_t axis = 0; axis < 3; ++axis)
            hash = Mix(hash, (uint64_t)m_ledger.external[axis]);

        return Mix(hash, (uint64_t)m_ledger.energy);
    }

    uint64_t ProbeLiquidField::HashTerm() const {
        uint64_t sum = 0;
        for (uint32_t region = 0; region < RegionCellCount(); ++region) {
            uint64_t term = ProbeRegionHashBegin(PROBE_LIQUID_CELL_HASH_SEED, region);
            for (const auto& faces : m_faces)
                term = ProbeRegionHashAdd(term, faces[region]);

            for (const auto& kinetic : m_kinetic)
                term = ProbeRegionHashAdd(term, kinetic[region]);

            sum += ProbeRegionHashEnd(ProbeRegionHashAdd(term, m_potential[region]));
        }

        // 帳簿: 外から入った運動量 × 3 → 位置のエネルギーの出入り
        uint64_t ledger = ProbeRegionHashBegin(PROBE_LIQUID_LEDGER_HASH_SEED, 0);
        for (const int64_t external : m_ledger.external)
            ledger = ProbeRegionHashAdd(ledger, external);

        return sum + ProbeRegionHashEnd(ProbeRegionHashAdd(ledger, m_ledger.energy));
    }

    // 1 刻みの作業の値(Jacobi 型: 力と流れは刻みの初めの値 states だけから決める)
    struct ProbeLiquidField::StepWork {
        std::array<uint32_t, 3> stride = {};  // 区画の中の隣(格子の軸 x・y・z)
        std::vector<LiquidCellState> states;
        std::vector<int64_t> pressure;                 // 質量の単位(mg)
        std::array<std::vector<uint64_t>, 3> desired;  // 面の望む動く質量(mg)
        std::vector<uint64_t> outgoing;                // セルから出る望む質量の合計
        std::array<std::vector<int64_t>, 3> moved;     // 面を通って実際に動いた質量(mg。+ 向きが正)
        std::vector<CellDelta> deltas;
        std::vector<int64_t> potentialCarried;               // 液体と一緒に運んだ後の位置のエネルギー(mJ)
        std::array<std::vector<int64_t>, 3> kineticCarried;  // 運動量と一緒に運んだ後の運動のエネルギー(mJ)
        std::vector<uint8_t> changed;
    };

    namespace {

        // 運動量の面 axis の、across 向きの境ごとに visit(下の面, 上の面, 2 倍の質量の流れ)を呼ぶ。
        // 境は、面のまわりの体積(セル region の中心から隣のセルの中心まで)の across 側の境。そこを通る 2 倍の流れ =
        // 2 つのセル(region と region + 軸の隣)の across 向きの面の流れの和(across = axis のときは隣のセルの中心を通る流れ)
        template <typename Visit>
        void ForEachBoundary(const std::array<uint32_t, 3>& size, const std::array<uint32_t, 3>& stride,
                             const std::array<std::vector<int64_t>, 3>& moved, uint32_t axis, uint32_t across,
                             Visit&& visit) {
            const uint32_t count = size[0] * size[1] * size[2];
            for (uint32_t region = 0; region < count; ++region) {
                const std::array<uint32_t, 3> at = {region % size[0], region / (size[0] * size[2]),
                                                    (region / size[0]) % size[2]};
                if (at[axis] + 1 >= size[axis] || at[across] + 1 >= size[across])
                    continue;  // 区画の外の面(閉じている)

                const int64_t flow = moved[across][region] + moved[across][region + stride[axis]];
                if (flow != 0)
                    visit(region, region + stride[across], flow);
            }
        }

    }  // namespace

    ProbeLiquidStepReport ProbeLiquidField::Step(const BakedReactionTable& table, std::span<reaction::RxCell> cells,
                                                 std::vector<uint32_t>& changed) {
        ProbeLiquidStepReport report;
        const auto size = m_options.sizeCells;
        const uint32_t count = RegionCellCount();

        // --- 刻みの初めの値 ---
        StepWork work;
        work.stride = {1, size[0] * size[2], size[0]};
        work.states.resize(count);
        for (uint32_t region = 0; region < count; ++region) {
            work.states[region] = ReadState(table, cells[GridIndex(region)]);
            if (work.states[region].mass != 0)
                ++report.wetCells;
        }

        if (report.wetCells == 0 && AtRest()) {
            rng::fill(m_mass, uint64_t{0});
            return report;  // 液体が無く、動いている面もエネルギーも無い(ブランクの世界・液体の無い表)
        }

        // --- 区画の外の段(反応・置いた水)で液体が現れた・消えた分の位置のエネルギーを帳簿へ ---
        for (uint32_t region = 0; region < count; ++region) {
            const int64_t potential = PotentialOf(region, work.states[region].mass);
            report.potentialEntered += potential - m_potential[region];
            m_potential[region] = potential;
        }

        m_ledger.energy += report.potentialEntered;
        FollowMassChange(work, report);

        // --- 圧力 → 面の力 → 質量を動かす → 運動量を運ぶ → エネルギーを熱へ ---
        work.kineticCarried = m_kinetic;
        ComputePressure(work, report);
        ApplyForces(work, report);
        MoveMass(table, cells, work, report);
        if (m_options.advection)
            AdvectMomentum(work, report);

        SettleEnergy(table, cells, work, report);

        for (uint32_t axis = 0; axis < 3; ++axis)
            m_ledger.external[axis] += report.wall[axis] + report.gravity[axis] + report.friction[axis] +
                                       report.dropped[axis] + report.capped[axis] + report.vanished[axis];

        for (uint32_t region = 0; region < count; ++region) {
            if (work.changed[region] == 0)
                continue;

            changed.push_back(GridIndex(region));
            ++report.changedCells;
        }

        return report;
    }

    // 区画のセル region の + 側の面(格子の軸 axis)の開き(Q16。両側の空きの小さい方。T-0612・ADR-0268)。
    // 0 = 閉じている(区画の外・どちらかのセルが固体で満ちている)。力・移流・運動のエネルギーの閉じた面の判定はすべてここ
    uint32_t ProbeLiquidField::FaceOpenQ16At(const StepWork& work, uint32_t region, uint32_t axis) const {
        if (RegionCoordinates(region)[axis] + 1 >= m_options.sizeCells[axis])
            return 0;

        return FaceOpenQ16(work.states[region].open, work.states[region + work.stride[axis]].open);
    }

    // 区画の外の段(蒸発・凝縮・凍る・溶ける・反応・置く操作)で液体の質量が変わった面の運動量を合わせ直す(T-0656・ADR-0286 の液体の版)。
    // 面の質量 = 両側のセルの液体の質量の和(前の刻みの終わり m_mass → 今)。減った時は速さを保つ(運動量 × 今 ÷ 前。0 の向きへ切り捨て。
    // 減った液体の運動量は帳簿へ)。増えた時は運動量を保つ。運動のエネルギー(m_kinetic)は前の値のまま運び、SettleEnergy で今の質量から
    // 計算し直すので、増減で減った分は熱になる(速さを保てば運動のエネルギーは増えない = 熱を負にしない)
    void ProbeLiquidField::FollowMassChange(const StepWork& work, ProbeLiquidStepReport& report) {
        const uint32_t count = RegionCellCount();
        for (uint32_t axis = 0; axis < 3; ++axis) {
            for (uint32_t region = 0; region < count; ++region) {
                int64_t& face = m_faces[axis][region];
                if (face == 0 || RegionCoordinates(region)[axis] + 1 >= m_options.sizeCells[axis])
                    continue;

                const uint32_t neighbor = region + work.stride[axis];
                const uint64_t before = m_mass[region] + m_mass[neighbor];
                const uint64_t now = work.states[region].mass + work.states[neighbor].mass;
                if (now >= before)
                    continue;

                const int64_t kept = ScaleSigned(face, now, before);
                report.vanished[axis] -= face - kept;
                face = kept;
            }
        }
    }

    // 圧力(質量の単位。P = 2 × 支えている上の液体 + 自分 + 溢れ)。列ごとに上から
    void ProbeLiquidField::ComputePressure(StepWork& work, ProbeLiquidStepReport& report) const {
        const auto size = m_options.sizeCells;
        const auto& states = work.states;
        work.pressure.assign(states.size(), 0);
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

                    // 圧力は空きの体積(セル − 固体)と「セル全部の液体に直した質量」で書く(孔の中の水の圧力 = 開けた水と同じ高さの圧力)
                    uint64_t overfill = 0;
                    if (state.volume > state.room) {
                        ++report.overfilledCells;
                        overfill = MulDiv(state.equivalent * m_options.overfillStiffness, state.volume - state.room,
                                          state.volume);
                    }

                    // 満ちていないセルは上の液体を満ち具合の分だけ支える(残りは落ちてくる力になる)
                    const uint64_t filled = std::min(state.volume, state.room);
                    const uint64_t supported = MulDiv(above, filled, state.room);
                    // 下に支えられている分だけ圧力になる(下が空のセルは落ちている途中で、横へ押し広げない)
                    const std::array<uint64_t, 2> support = y == 0 ? std::array<uint64_t, 2>{1, 1}
                                                                   : SupportBelow(states[RegionIndex(x, y - 1, z)]);
                    work.pressure[region] = (int64_t)MulDiv((2 * supported) + state.equivalent + overfill, support[0],
                                                            support[1]);
                    above = supported + state.equivalent;
                }
            }
        }
    }

    // 面の運動量に力(圧力の差・重さ・壁・摩擦)を足し、望む動く質量を決める
    void ProbeLiquidField::ApplyForces(StepWork& work, ProbeLiquidStepReport& report) {
        const uint32_t count = RegionCellCount();
        const auto& states = work.states;
        const auto& pressure = work.pressure;
        work.outgoing.assign(count, 0);
        for (uint32_t axis = 0; axis < 3; ++axis) {
            work.desired[axis].assign(count, 0);
            const uint32_t step = work.stride[axis];
            for (uint32_t region = 0; region < count; ++region) {
                const auto at = RegionCoordinates(region);
                const LiquidCellState& a = states[region];
                int64_t& face = m_faces[axis][region];

                // − 側が区画の外か壁なら、その面の押し返しは壁から(+ 向き)
                const bool minusClosed = at[axis] == 0 || FaceOpenQ16At(work, region - step, axis) == 0;
                if (!a.wall && minusClosed)
                    report.wall[axis] += pressure[region];

                // + 側の面(一部だけ開いた面 = 孔は、力も重さも開いた割合だけ)
                const uint32_t faceOpen = FaceOpenQ16At(work, region, axis);
                if (faceOpen == 0) {
                    if (!a.wall)
                        report.wall[axis] -= pressure[region];

                    // 閉じた面に残った運動量(壁が置かれた)は壁が受ける
                    report.wall[axis] -= face;
                    face = 0;
                    continue;
                }

                const uint32_t neighbor = region + step;
                const LiquidCellState& b = states[neighbor];
                const int64_t push = pressure[region] - pressure[neighbor];
                const auto weight = axis == 1 ? (int64_t)(a.equivalent + b.equivalent) : 0;
                if (faceOpen == CELL_OPEN_FULL) {
                    face += push - weight;
                    report.gravity[axis] -= weight;
                } else {
                    // 一部だけ開いた面(孔): 力も重さも開いた割合だけ。押し切れない分は固体が受ける(帳簿の壁へ。T-0612)
                    const int64_t openWeight = ScaleSigned(weight, faceOpen, CELL_OPEN_FULL);
                    const int64_t change = ScaleSigned(push - weight, faceOpen, CELL_OPEN_FULL);
                    face += change;
                    report.gravity[axis] -= openWeight;
                    report.wall[axis] += change - push + openWeight;
                }

                // まわりに液体が無い面の運動量は行き場が無い。移流なし(T-0609 の形)では上流が空の面も
                const LiquidCellState& upwind = face > 0 ? a : b;
                if (a.mass + b.mass == 0 || (!m_options.advection && upwind.mass == 0)) {
                    report.dropped[axis] -= face;
                    face = 0;
                    continue;
                }

                const int64_t friction = face / (int64_t{1} << m_options.frictionShift);
                face -= friction;
                report.friction[axis] -= friction;
                if (upwind.mass == 0)
                    continue;  // 上流が空: 運動量は残し(移流が液体と一緒に運ぶ)、この面は流れない

                const uint64_t flux = MulDiv(fx::FxAbsU64(face), m_gravityMicrometersPerSecond2, FLUX_DENOMINATOR);
                work.desired[axis][region] = flux;
                work.outgoing[face > 0 ? region : neighbor] += flux;
            }
        }
    }

    // 望む質量を動かす(出る合計が中身を超えるセルは比で縮める)。成分・液体のぶんのエネルギー・位置のエネルギーも一緒に
    void ProbeLiquidField::MoveMass(const BakedReactionTable& table, std::span<reaction::RxCell> cells, StepWork& work,
                                    ProbeLiquidStepReport& report) {
        const uint32_t count = RegionCellCount();
        const auto& states = work.states;
        work.deltas.assign(count, {});
        work.potentialCarried = m_potential;
        for (uint32_t region = 0; region < count; ++region) {
            if (work.outgoing[region] > states[region].mass)
                ++report.limitedCells;
        }

        for (uint32_t axis = 0; axis < 3; ++axis) {
            work.moved[axis].assign(count, 0);
            for (uint32_t region = 0; region < count; ++region) {
                const uint64_t flux = work.desired[axis][region];
                if (flux == 0)
                    continue;

                const uint32_t neighbor = region + work.stride[axis];
                const bool forward = m_faces[axis][region] > 0;
                const uint32_t from = forward ? region : neighbor;
                const uint32_t to = forward ? neighbor : region;
                const LiquidCellState& state = states[from];
                const uint64_t outgoing = work.outgoing[from];
                const uint64_t mass = outgoing > state.mass ? MulDiv(flux, state.mass, outgoing) : flux;
                if (mass == 0)
                    continue;

                report.movedMilligrams += mass;
                work.moved[axis][region] = forward ? (int64_t)mass : -(int64_t)mass;
                MoveLiquid(table, cells[GridIndex(from)], state, mass, work.deltas[from], work.deltas[to]);

                const auto potential = (int64_t)MulDiv((uint64_t)m_potential[from], mass, state.mass);
                work.potentialCarried[from] -= potential;
                work.potentialCarried[to] += potential;
            }
        }
    }

    // 面の運動量(と運動のエネルギー)を、面のまわりの体積の境を通る質量の流れで風上に運ぶ(T-0610・ADR-0276)
    void ProbeLiquidField::AdvectMomentum(StepWork& work, ProbeLiquidStepReport& report) {
        const auto size = m_options.sizeCells;
        const uint32_t count = RegionCellCount();
        const auto& states = work.states;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const uint32_t step = work.stride[axis];
            const auto doubledMass = [&](uint32_t region) {
                return states[region].mass + states[region + step].mass;  // 開いた面だけ呼ぶ(運動量が 0 でない)
            };

            // 面から出る 2 倍の流れの合計(2 倍の質量を超えれば比で縮める)
            std::vector<uint64_t> leaving(count, 0);
            for (uint32_t across = 0; across < 3; ++across) {
                ForEachBoundary(size, work.stride, work.moved, axis, across,
                                [&](uint32_t lower, uint32_t upper, int64_t flow) {
                                    leaving[flow > 0 ? lower : upper] += fx::FxAbsU64(flow);
                                });
            }

            const std::vector<int64_t> before = m_faces[axis];
            const std::vector<int64_t> kineticBefore = work.kineticCarried[axis];
            for (uint32_t across = 0; across < 3; ++across) {
                ForEachBoundary(
                    size, work.stride, work.moved, axis, across, [&](uint32_t lower, uint32_t upper, int64_t flow) {
                        const uint32_t from = flow > 0 ? lower : upper;
                        const uint32_t to = flow > 0 ? upper : lower;
                        if (before[from] == 0)
                            return;

                        const uint64_t share = fx::FxAbsU64(flow);
                        const uint64_t denominator = std::max(doubledMass(from), leaving[from]);
                        const auto magnitude = (int64_t)MulDiv(fx::FxAbsU64(before[from]), share, denominator);
                        const int64_t momentum = before[from] > 0 ? magnitude : -magnitude;
                        const auto energy = (int64_t)MulDiv((uint64_t)kineticBefore[from], share, denominator);
                        m_faces[axis][from] -= momentum;
                        work.kineticCarried[axis][from] -= energy;
                        work.kineticCarried[axis][to] += energy;  // 閉じた面へ入った分は SettleEnergy で熱に

                        // 閉じた面(壁・区画の外)へ入る運動量は壁が受ける
                        if (FaceOpenQ16At(work, to, axis) == 0)
                            report.wall[axis] -= momentum;
                        else
                            m_faces[axis][to] += momentum;
                    });
            }
        }
    }

    // 液体を当て、まわりが空の面・速さの上限を直し、位置と運動のエネルギーを計算し直して差をセルの熱にする
    void ProbeLiquidField::SettleEnergy(const BakedReactionTable& table, std::span<reaction::RxCell> cells,
                                        StepWork& work, ProbeLiquidStepReport& report) {
        const auto size = m_options.sizeCells;
        const uint32_t count = RegionCellCount();
        work.changed.assign(count, 0);
        std::vector<uint64_t> mass(count, 0);
        for (uint32_t region = 0; region < count; ++region) {
            reaction::RxCell& cell = cells[GridIndex(region)];
            if (ApplyDelta(cell, work.deltas[region]))
                work.changed[region] = 1;

            mass[region] = LiquidMass(table, cell) / 1000000;  // ng → mg(ReadState と同じ)
            m_mass[region] = mass[region];
        }

        // --- 面: まわりが空になった面・速さの上限・運動のエネルギー(差は両側のセルへ半分ずつ)---
        std::vector<int64_t> heat(count, 0);
        for (uint32_t axis = 0; axis < 3; ++axis) {
            for (uint32_t region = 0; region < count; ++region) {
                int64_t& face = m_faces[axis][region];
                FX_ASSERT(face == 0 || FaceOpenQ16At(work, region, axis) != 0);  // 閉じた面の運動量は壁が受けた
                const bool inside = RegionCoordinates(region)[axis] + 1 < size[axis];
                const uint32_t neighbor = region + work.stride[axis];
                const uint64_t doubled = inside ? mass[region] + mass[neighbor] : 0;
                if (face != 0 && doubled == 0) {
                    report.dropped[axis] -= face;
                    face = 0;
                }

                const uint64_t limit = MaxMomentum(doubled);
                if (fx::FxAbsU64(face) > limit) {
                    const int64_t kept = face > 0 ? (int64_t)limit : -(int64_t)limit;
                    report.capped[axis] -= face - kept;
                    face = kept;
                    ++report.cappedFaces;
                }

                const int64_t kinetic = KineticOf(face, doubled);
                const int64_t released = work.kineticCarried[axis][region] - kinetic;
                m_kinetic[axis][region] = kinetic;
                if (released == 0)
                    continue;

                if (!inside) {
                    heat[region] += released;
                    continue;
                }

                const int64_t share = DivideDown(released, 2);
                heat[region] += released - share;
                heat[neighbor] += share;
            }
        }

        // --- セル: 位置のエネルギー ---
        for (uint32_t region = 0; region < count; ++region) {
            const int64_t potential = PotentialOf(region, mass[region]);
            heat[region] += work.potentialCarried[region] - potential;
            m_potential[region] = potential;
            if (heat[region] == 0)
                continue;

            cells[GridIndex(region)].energy += heat[region];
            report.heat += heat[region];
            work.changed[region] = 1;
        }
    }

}  // namespace bicameral::sim
