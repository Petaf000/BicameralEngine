// physics_heat_test.cpp — 物理で失った力学的エネルギーを世界のセルの熱へ返す(T-0542・ADR-0242。common/physics_heat.hlsli)の CPU の試験。
//
// 仮の世界(試験のパッケージ combustion_test の反応の世界 + 積み木)の CPU リファレンス(sim::ProbeReference)と物理(sim::PhysicsWorld)を
// 同じ刻みで進める。物理の刻み t の熱の入口(PhysicsWorld::HeatEntries)は、刻み t + 1 の初めに反応の世界へ足す(ProbeReference::Advance の heat)。
// 確かめること(毎刻み・ビット単位):
//   - 物理の帳簿: Σ dissipatedEnergy = PX_HEAT_UNITS_PER_MILLIJOULE × 返した mJ + 残り(端数と借り)
//   - 返した mJ = セルに増えた熱(足し終わった分)+ まだ足していない直近の刻みの分
//   - 反応の世界のエネルギーの和の増え = その刻みに足した物理の熱(伝導と反応はエネルギーを保つ)
// と、量の目安: 落とした物(500 kg を底 5 m から)でセルに返した熱が m g h に合い、落ちた所の床のセルに集まる。
//   積み木を押して崩す場面でも帳簿が合う。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "reaction_test_table.h"
#include "sim/physics_world.h"
#include "sim/probe_sim.h"
#include "sim/world_physics.h"

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Physics, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    constexpr int64_t METER = int64_t{1} << 20;
    constexpr int64_t UNIT = int64_t{1} << 30;  // Q1.30 の 1
    constexpr double GRAVITY = 9.80665;

    struct PushAt {
        uint64_t tick = 0;
        physics::PxVec3 origin{};
        physics::PxVec3 direction{};
        uint32_t impulseMillinewtonSeconds = 0;
    };

    struct HeatRun {
        int64_t cellHeat = 0;      // 反応の世界のセルに足した物理の熱(mJ)
        int64_t inFlight = 0;      // 最後の刻みに返したが、まだセルへ足していない熱(mJ)
        int64_t remainder = 0;     // 物理の帳簿の残り(2^-10 µJ)
        int64_t mostBorrowed = 0;  // 残りの最も負の値(借り。2^-10 µJ)
        int64_t nearHeat = 0;      // 見る場所のまわりのセルのエネルギーの増え(mJ)
        uint32_t heatTicks = 0;    // 熱を返した刻みの数
        uint64_t finalStateHash = 0;
    };

    int64_t EnergyAround(std::span<const reaction::RxCell> cells, uint32_t centerX, uint32_t centerZ) {
        int64_t sum = 0;
        for (uint32_t z = centerZ - 4; z <= centerZ + 4; ++z) {
            for (uint32_t y = PROBE_GRID_SIZE - 5; y < PROBE_GRID_SIZE; ++y) {
                for (uint32_t x = centerX - 4; x <= centerX + 4; ++x)
                    sum += cells[ProbeCellIndex(x, y, z)].energy;
            }
        }

        return sum;
    }

    // 1 刻みの帳簿の確かめ
    void CheckTick(std::string_view name, uint64_t tick, const PhysicsWorld& world, const ProbeReference& reference,
                   uint64_t energyBefore, uint64_t energyAfter, HeatRun& run) {
        const physics::PxHeatLedger& ledger = world.HeatLedger();
        const int64_t dissipated = SumPhysicsEnergy(world.Bodies()).dissipatedEnergy;
        Expect(dissipated == physics::PX_HEAT_UNITS_PER_MILLIJOULE * ledger.returnedMilliJoules + ledger.remainder,
               std::format("{} 刻み {}: Σ 散逸 ≠ 返した熱 + 残り", name, tick));

        int64_t entrySum = 0;
        for (const physics::PxHeatEntry& entry : world.HeatEntries())
            entrySum += entry.milliJoules;
        Expect(entrySum == ledger.tickMilliJoules,
               std::format("{} 刻み {}: 入口の和 ≠ この刻みに返した熱", name, tick));

        // 反応の世界: エネルギーの和の増え(mod 2^64)= この刻みの初めに足した物理の熱
        Expect(static_cast<int64_t>(energyAfter - energyBefore) == reference.PhysicsHeatEnergy(),
               std::format("{} 刻み {}: 世界のエネルギーの増え {} ≠ 足した物理の熱 {}", name, tick,
                           static_cast<int64_t>(energyAfter - energyBefore), reference.PhysicsHeatEnergy()));

        run.cellHeat += reference.PhysicsHeatEnergy();
        run.inFlight = ledger.tickMilliJoules;
        run.remainder = ledger.remainder;
        run.mostBorrowed = std::min(run.mostBorrowed, ledger.remainder);
        run.heatTicks += ledger.tickMilliJoules > 0 ? 1 : 0;
        Expect(run.cellHeat + run.inFlight == ledger.returnedMilliJoules,
               std::format("{} 刻み {}: セルの熱 + 足していない分 ≠ 返した熱", name, tick));
    }

    HeatRun Run(std::string_view name, const BakedReactionTable& table, const PhysicsScene& scene, uint64_t ticks,
                std::span<const PushAt> pushes, uint32_t lookX, uint32_t lookZ) {
        HeatRun run;
        PhysicsWorld world(scene, PhysicsParametersFor(table.environment));
        ProbeReference reference(table);
        const int64_t nearBefore = EnergyAround(reference.State(0), lookX, lookZ);

        std::vector<physics::PxHeatEntry> heat;  // 前の刻みの物理の熱の入口
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            for (const PushAt& push : pushes) {
                if (push.tick == tick)
                    world.Push(push.origin, push.direction, push.impulseMillinewtonSeconds);
            }

            const uint64_t energyBefore = ProbeEnergySum(reference.State(tick));
            reference.Advance(tick, {}, nullptr, heat);
            world.Step();
            CheckTick(name, tick, world, reference, energyBefore, ProbeEnergySum(reference.State(tick + 1)), run);
            heat = world.HeatEntries();
        }

        run.nearHeat = EnergyAround(reference.State(ticks), lookX, lookZ) - nearBefore;
        run.finalStateHash = reference.StateHash(ticks);
        Log(Channel::Physics, Level::Info,
            "{}: セルへ返した熱 {:.3f} J(+ 足していない {:.3f} J)・見る場所のまわり {:.3f} J・残り {:.6f} J・"
            "最も大きい借り {:.6f} J・熱を返した刻み {}",
            name, run.cellHeat / 1000.0, run.inFlight / 1000.0, run.nearHeat / 1000.0,
            static_cast<double>(run.remainder) / physics::PX_HEAT_UNITS_PER_MILLIJOULE / 1000.0,
            static_cast<double>(run.mostBorrowed) / physics::PX_HEAT_UNITS_PER_MILLIJOULE / 1000.0, run.heatTicks);

        return run;
    }

    // 落とす: 積み木の世界に、500 kg の 1 m の立方体を x = 20 m・z = 16.25 m・底 5 m から落とす(落ちる所は格子 x = 40・z = 32 の床)
    void TestDrop(const BakedReactionTable& table) {
        PhysicsScene scene = MakeLayoutPhysicsScene(table.worldLayout);
        PhysicsSceneBody cube{.massMilligrams = 500'000'000};
        cube.halfExtent = {METER / 2, METER / 2, METER / 2};
        cube.position = {20 * METER, 11 * METER / 2, 16 * METER + METER / 4};
        scene.bodies.push_back(cube);

        const HeatRun run = Run("落とした物", table, scene, 120, {}, 40, 32);
        const double expected = 500 * GRAVITY * 5;
        const double returned = (run.cellHeat + run.inFlight) / 1000.0;
        Expect(std::abs(returned - expected) < expected * 0.01,
               std::format("落とした物: 返した熱 {:.1f} J が m g h {:.1f} J から 1% 以上ずれた", returned, expected));
        Expect(run.nearHeat * 10 > run.cellHeat * 9, "落とした物: 熱の 9 割以上が落ちた所の床のまわりに無い");
        Expect(run.remainder < 2 * physics::PX_HEAT_UNITS_PER_MILLIJOULE * 1000, "落とした物: 残りが 2 J 以上");

        const HeatRun again = Run("落とした物(2 回目)", table, scene, 120, {}, 40, 32);
        Expect(again.finalStateHash == run.finalStateHash && again.cellHeat == run.cellHeat,
               "落とした物: 2 回目の実行の世界が食い違った");
    }

    // 崩す: 積み木の柱(x = 10 m)を上の方で横に押して崩す。帳簿だけ確かめる(押した分は物理の外から入る)
    void TestTopple(const BakedReactionTable& table) {
        const PhysicsScene scene = MakeLayoutPhysicsScene(table.worldLayout);
        const std::array<PushAt, 1> pushes = {{
            {.tick = 20,
             .origin = physics::PxMakeVec3(10 * METER, 17 * METER / 2, 6 * METER),
             .direction = physics::PxMakeVec3(0, 0, UNIT),
             .impulseMillinewtonSeconds = 5'000'000},
        }};

        const HeatRun run = Run("崩した積み木", table, scene, 180, pushes, 20, 32);
        Expect(run.cellHeat > 0, "崩した積み木: 熱が返らない");
    }

}  // namespace

int main() {
    const auto table = BakeReactionTable(MakeCombustionTestTable());
    if (!table) {
        Log(Channel::Physics, Level::Error, "試験の表を作れない");
        SingletonFinalizer::Finalize();
        return 1;
    }

    TestDrop(*table);
    TestTopple(*table);

    Log(Channel::Physics, failureCount == 0 ? Level::Info : Level::Error, "physics_heat_test: {}",
        failureCount == 0 ? "OK" : "FAILED");
    SingletonFinalizer::Finalize();
    return failureCount == 0 ? 0 : 1;
}
