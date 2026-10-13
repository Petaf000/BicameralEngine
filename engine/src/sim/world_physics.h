// world_physics.h — 世界ごとに違ってよい物理の値(世界のデータ = 環境の値。ADR-0171・ADR-0176・D-480)を 1 つにまとめ、物理と気体の設定へ渡す。
// 器(エンジン)の既定は 0 = ブランクの世界(D-455: 何かを足すまで物も気体も重さを持たず、気体の音速の基準も無い)。地球の値は
// 標準パッケージ data/standard/earth_environment にある(環境ミキサーで足す。T-0373。エンジンは中身を持たない D-454)。
// 世界のパッケージは分類 environment に書き、script::ReadWorldEnvironment がこれに読む(script/environment_package.h。T-0400)。
// 地球の値で書いた試験の場面は tests/standard_world_physics.h の写しを使う(standard_packages_test がパッケージと同じか確かめる)。
// 重力の単位・上限・換算は shaders/common/world_physics.hlsli(CPU と GPU が同じ換算を通る)。音速の基準は CPU で係数にするだけなので、
// 単位と上限(安定の条件)は gas_reference.h の GasSoundSpeedLimitMmPerS。
//
// データの流れ: 世界のパッケージの environment(ブランクは無い = 0)→ ReadWorldEnvironment → WorldPhysics
//   → PhysicsParametersFor → PxParameters::gravity(物理)
//   → ApplyWorldPhysics → GasConfig::gravityMicrometersPerSecond2・soundSpeedMmPerS(気体)
#pragma once

#include <cstdint>

#include "common/physics_solver.hlsli"
#include "common/world_physics.hlsli"
#include "sim/gas_reference.h"

namespace bicameral::sim {

    // --- 世界の物理の値 ---
    struct WorldPhysics {
        // 重力の強さ(µm/s²。0 = 無重力。上限 fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2)。ブランクの世界は 0(D-480)
        uint32_t gravityMicrometersPerSecond2 = 0;

        // 気体の音速の基準 c̃(mm/s。ADR-0176)。気体の圧力の波・衝撃波の速さの基準で、気体の箱を作るのに要る。
        // 0 = 無い(ブランクの世界。気体を使う世界は環境で渡す)。上限は気体の小刻みの数で決まる(GasSoundSpeedLimitMmPerS)
        uint32_t soundSpeedMillimetersPerSecond = 0;
    };

    // --- 各サブシステムの設定へ渡す ---

    // 物理: 解き方の値(反復の数・硬さなど。器の既定は PxDefaultParameters)に、世界の重力を入れる
    [[nodiscard]] inline physics::PxParameters PhysicsParametersFor(
        const WorldPhysics& world, physics::PxParameters solver = physics::PxDefaultParameters()) {
        solver.gravity = fx::PxGravityFromMicrometers(world.gravityMicrometersPerSecond2);
        return solver;
    }

    // 気体: 設定の重力の強さと音速の基準を世界の値にする(重さの力積・層の重さの圧力・重さの仕事・圧力の拡散・α は
    // MakeGasCoefficients・MakeGasBox が作る)
    inline void ApplyWorldPhysics(const WorldPhysics& world, GasConfig& config) {
        config.gravityMicrometersPerSecond2 = world.gravityMicrometersPerSecond2;
        config.soundSpeedMmPerS = world.soundSpeedMillimetersPerSecond;
    }

}  // namespace bicameral::sim
