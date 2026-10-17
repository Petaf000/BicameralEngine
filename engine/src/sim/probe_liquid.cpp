// probe_liquid.cpp — 仮の世界に液体の流れを載せる CPU リファレンス(T-0609・ADR-0266。移流とエネルギーは T-0610・ADR-0276)。
// 形と約束は probe_liquid.h。1 刻み = 刻みの初めの値 → 圧力 → 面の力 → 質量を動かす → 運動量を運ぶ → エネルギーを熱へ。
// 式(柱の静水圧・面の力・摩擦・移流・速さの上限・位置/運動のエネルギー)は common/liquid_flow.hlsli の 1 つの源を GPU
// (shaders/sim/probe_liquid_tick.hlsl)と同じく呼ぶ(T-0716)。ここに残るのは区画の回し方・成分の書き戻し・帳簿への足し方。
#include "sim/probe_liquid.h"

#include <algorithm>

#include "common/fixed.hlsli"
#include "common/liquid_flow.hlsli"
#include "common/probe_region_hash.hlsli"
#include "common/probe_sim.hlsli"
#include "common/rest_rounding.hlsli"
#include "core/aliases.h"
#include "sim/cell_volume.h"

namespace bicameral::sim {

    namespace {

        // 1 刻みに 1 セルを超える速さを断る(刻みは 1/60 秒)
        constexpr uint64_t TICKS_PER_SECOND = 60;

        constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
        constexpr uint64_t FNV_PRIME = 1099511628211ull;

        using liquid::LqCellView;

        bool IsLiquid(const BakedReactionTable& table, uint32_t species) {
            return species < table.speciesLiquid.size() && table.speciesLiquid[species] != 0;
        }

        uint64_t Mix(uint64_t hash, uint64_t value) {
            for (uint32_t byte = 0; byte < 8; ++byte) {
                hash ^= (value >> (byte * 8)) & 0xffu;
                hash *= FNV_PRIME;
            }

            return hash;
        }

        // 刻みの初めのセルから液体の見立て(区画のセルごと。common/liquid_flow.hlsli の LqCellView)を作る。
        // 固体が占める体積(sim/cell_volume。密度を書いていない固体は場所を取らない)を除いた空きに液体が入る
        LqCellView ReadState(const BakedReactionTable& table, const reaction::RxCell& cell) {
            int64_t chemical = 0;
            uint64_t capacity = 0;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                const uint32_t species = cell.species[i];
                const uint64_t amount = cell.amounts[i];
                chemical += (int64_t)amount * table.species[species].h0;
                capacity += amount * table.species[species].heatCapacity;
            }

            const uint64_t mass = ProbeLiquidField::LiquidMass(table, cell) / 1000000;  // ng → mg
            const CellVolumeShare share = ShareCellVolume(table, cell);
            const int64_t heat = (cell.energy * 1000) - chemical;
            FX_ASSERT(heat >= 0);

            return liquid::LqMakeCellView(mass, ProbeLiquidField::LiquidVolume(table, cell), share.solid,
                                          PROBE_LIQUID_CELL_VOLUME_PICOLITERS, heat, capacity);
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
        void MoveLiquid(const BakedReactionTable& table, const reaction::RxCell& cell, const LqCellView& state,
                        uint64_t mass, CellDelta& from, CellDelta& to) {
            uint64_t movedCapacity = 0;
            int64_t movedChemical = 0;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                const uint32_t species = cell.species[i];
                if (!IsLiquid(table, species))
                    continue;

                const uint64_t moved = liquid::LqMovedShare(cell.amounts[i], mass, state.mass);
                if (moved == 0)
                    continue;

                AddAmount(from, species, -(int64_t)moved);
                AddAmount(to, species, (int64_t)moved);
                movedCapacity += moved * table.species[species].heatCapacity;
                movedChemical += (int64_t)moved * table.species[species].h0;
            }

            if (movedCapacity == 0)
                return;

            const int64_t movedEnergy = liquid::LqMovedEnergy(state, movedCapacity, movedChemical);
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
        field.m_params = liquid::LqMakeParams(
            table.environment.gravityMicrometersPerSecond2, PROBE_LIQUID_CELL_EDGE_MILLIMETERS, options.frictionShift,
            options.overfillStiffness, options.maxSpeedMillimetersPerSecond, options.advection ? 1u : 0u);
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
        const uint64_t height = liquid::LqCellHeightMillimeters(RegionCoordinates(region)[1], m_params);

        return liquid::LqPotentialEnergy(massMilligrams, height, m_params);
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

            term = ProbeRegionHashAdd(term, m_potential[region]);
            const uint64_t mass = region < m_mass.size() ? m_mass[region] : 0;  // GPU の欄は作った時 0
            sum += ProbeRegionHashEnd(ProbeRegionHashAdd(term, (int64_t)mass));
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
        std::vector<LqCellView> states;
        std::vector<int64_t> pressure;                 // 質量の単位(mg)
        std::array<std::vector<uint64_t>, 3> desired;  // 面の望む動く質量(mg)
        std::vector<uint64_t> outgoing;                // セルから出る望む質量の合計
        std::array<std::vector<int64_t>, 3> moved;     // 面を通って実際に動いた質量(mg。+ 向きが正)
        std::vector<CellDelta> deltas;
        std::vector<int64_t> potentialCarried;               // 液体と一緒に運んだ後の位置のエネルギー(mJ)
        std::array<std::vector<int64_t>, 3> kineticCarried;  // 運動量と一緒に運んだ後の運動のエネルギー(mJ)
        std::array<std::vector<int64_t>, 3> facesBefore;     // 刻みの初めの面の運動量(静止の丸めが比べる。T-0758)
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
        work.facesBefore = m_faces;  // 刻みの初めの面の運動量(合わせ直しの後。静止の丸めが比べる。T-0758)

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
                                       report.dropped[axis] + report.capped[axis] + report.vanished[axis] +
                                       report.rested[axis];

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

        return liquid::LqFaceOpen(work.states[region].open, work.states[region + work.stride[axis]].open);
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

                const int64_t kept = liquid::LqFollowMassChange(face, before, now);
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
                    const LqCellView& below = y == 0 ? states[region] : states[RegionIndex(x, y - 1, z)];
                    const liquid::LqColumnStep cellPressure = liquid::LqColumnPressure(
                        states[region], above, y == 0 ? 0u : 1u, below, m_params);
                    work.pressure[region] = cellPressure.pressure;
                    report.overfilledCells += cellPressure.overfilled;
                    above = cellPressure.above;
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
                const LqCellView& a = states[region];
                int64_t& face = m_faces[axis][region];

                // − 側が区画の外か壁なら、その面の押し返しは壁から(+ 向き)
                const bool minusClosed = at[axis] == 0 || FaceOpenQ16At(work, region - step, axis) == 0;
                report.wall[axis] += liquid::LqMinusWallPush(a, pressure[region], minusClosed ? 1u : 0u);

                // + 側の面(閉じた面・開いた面。式は common/liquid_flow.hlsli)
                const uint32_t faceOpen = FaceOpenQ16At(work, region, axis);
                const uint32_t neighbor = region + step;
                const liquid::LqFaceForce force = faceOpen == 0
                                                      ? liquid::LqClosedFace(face, a, pressure[region])
                                                      : liquid::LqOpenFaceForce(face, faceOpen, axis == 1 ? 1u : 0u, a,
                                                                                pressure[region], states[neighbor],
                                                                                pressure[neighbor], m_params);
                face = force.face;
                report.wall[axis] += force.wall;
                report.gravity[axis] += force.gravity;
                report.friction[axis] += force.friction;
                report.dropped[axis] += force.dropped;
                if (force.desired == 0)
                    continue;

                work.desired[axis][region] = force.desired;
                work.outgoing[face > 0 ? region : neighbor] += force.desired;
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
                const LqCellView& state = states[from];
                const uint64_t mass = liquid::LqLimitOutflow(flux, state.mass, work.outgoing[from]);
                if (mass == 0)
                    continue;

                report.movedMilligrams += mass;
                work.moved[axis][region] = forward ? (int64_t)mass : -(int64_t)mass;
                MoveLiquid(table, cells[GridIndex(from)], state, mass, work.deltas[from], work.deltas[to]);

                const auto potential = (int64_t)liquid::LqMovedShare((uint64_t)m_potential[from], mass, state.mass);
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

                        const liquid::LqCarried carried = liquid::LqAdvectShare(
                            before[from], kineticBefore[from], fx::FxAbsU64(flow), doubledMass(from), leaving[from]);
                        const int64_t momentum = carried.momentum;
                        const int64_t energy = carried.energy;
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

    // 静止の丸め(T-0723・T-0758。既定は切る。形は common/rest_rounding.hlsli = 気体のセルと同じ): 速さが restSpeedMicrometersPerSecond 未満で、
    // この刻みで速くなっていない(|運動量| が刻みの初め before 以下)面の運動量を 0 にする(帳簿へ)。押されて動き出した遅い流れ(薄い膜)は止めない。
    // 運動のエネルギーは released に足して両側のセルの熱へ(速さの上限で止めた分と同じ道)
    void ProbeLiquidField::RestFace(liquid::LqSettledFace& settled, int64_t before, uint64_t doubledMass,
                                    int64_t& rested, uint32_t& restedFaces) const {
        if (m_options.restSpeedMicrometersPerSecond == 0 || settled.face == 0)
            return;

        const uint64_t limit = liquid::LqMulDiv((uint64_t)m_options.restSpeedMicrometersPerSecond * TICKS_PER_SECOND,
                                                doubledMass, m_params.gravityMicrometersPerSecond2);
        if (!rest::RrShouldRest(fx::FxAbsU64(settled.face), fx::FxAbsU64(before), limit))
            return;

        rested -= settled.face;
        settled.face = 0;
        settled.released += settled.kinetic;
        settled.kinetic = 0;
        ++restedFaces;
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
                liquid::LqSettledFace settled = liquid::LqSettleFace(face, doubled, work.kineticCarried[axis][region],
                                                                     m_params);
                RestFace(settled, work.facesBefore[axis][region], doubled, report.rested[axis], report.restedFaces);
                face = settled.face;
                report.dropped[axis] += settled.dropped;
                report.capped[axis] += settled.capped;
                report.cappedFaces += settled.cappedFace;
                m_kinetic[axis][region] = settled.kinetic;
                const int64_t released = settled.released;
                if (released == 0)
                    continue;

                if (!inside) {
                    heat[region] += released;
                    continue;
                }

                const int64_t share = liquid::LqReleasedShare(released);
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
