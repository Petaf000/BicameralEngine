// world_physics.h — 世界ごとに違ってよい物理の値(世界のデータ。ADR-0171・D-480)を 1 つにまとめ、物理と気体の設定へ渡す。
// 器(エンジン)の既定は 0 = ブランクの世界(D-455: 何かを足すまで物も気体も重さを持たない)。標準の値(地球)は
// StandardWorldPhysics() にあり、サンプルの世界と、それを写した試験の場面が明示して渡す(標準パッケージ T-0373 が渡すようになるまでの置き場所)。
// 単位・上限・換算は shaders/common/world_physics.hlsli(CPU と GPU が同じ換算を通る)。
//
// データの流れ: 世界の種類(ブランク / サンプル。後でパッケージの環境の値)→ WorldPhysics
//   → PhysicsParametersFor → PxParameters::gravity(物理)
//   → ApplyWorldPhysics → GasConfig::gravityMicrometersPerSecond2(気体)
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
    };

    // 標準の値(地球の重力 9.80665 m/s²)。サンプルの世界と、標準の値で書いた試験の場面が使う
    [[nodiscard]] constexpr WorldPhysics StandardWorldPhysics() {
        return {.gravityMicrometersPerSecond2 = fx::STANDARD_GRAVITY_MICROMETERS_PER_SECOND2};
    }

    // --- 各サブシステムの設定へ渡す ---

    // 物理: 解き方の値(反復の数・硬さなど。器の既定は PxDefaultParameters)に、世界の重力を入れる
    [[nodiscard]] inline physics::PxParameters PhysicsParametersFor(
        const WorldPhysics& world, physics::PxParameters solver = physics::PxDefaultParameters()) {
        solver.gravity = fx::PxGravityFromMicrometers(world.gravityMicrometersPerSecond2);
        return solver;
    }

    // 気体: 設定の重力の強さを世界の値にする(重さの力積・層の重さの圧力・重さの仕事は MakeGasCoefficients が作る)
    inline void ApplyWorldPhysics(const WorldPhysics& world, GasConfig& config) {
        config.gravityMicrometersPerSecond2 = world.gravityMicrometersPerSecond2;
    }

}  // namespace bicameral::sim
