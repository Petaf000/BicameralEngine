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
// ホットリロード(T-0444・ADR-0194): 表の差し替えで値が変わったら、差し替えの刻みから物理(ProbeSim → GpuPhysics::SetGravity)と
//   気体(SetGasEnvironment・GpuGas::RecordEnvironment)が新しい値で刻む。器が受けられるかは CheckWorldPhysics(読み込みと同じ決まり)。
#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <string>

#include "common/gas_flow.hlsli"
#include "common/physics_solver.hlsli"
#include "common/world_physics.hlsli"
#include "sim/gas_reference.h"

namespace bicameral::sim {

    // --- 世界の端(T-0710・土台の決定 5。外との境は世界の端だけ)---

    // 端の 1 つの面の種類
    enum class WorldEdge : uint8_t {
        Wall = 0,  // 閉じた壁(既定。何も出入りしない)
        Open = 1,  // 開いた端(外は基準の大気。流れが出入りし、帳簿の「外へ」の列に載る)
    };

    // 環境の world_edges = { top, bottom, sides }。既定は全部 wall。まだ刻みは読まない(土台の段 1 以降)
    struct WorldEdges {
        WorldEdge top = WorldEdge::Wall;     // 上(空)
        WorldEdge bottom = WorldEdge::Wall;  // 下(地の底)
        WorldEdge sides = WorldEdge::Wall;   // 横の 4 面

        [[nodiscard]] bool AllWall() const {
            return top == WorldEdge::Wall && bottom == WorldEdge::Wall && sides == WorldEdge::Wall;
        }

        [[nodiscard]] bool operator==(const WorldEdges&) const = default;
    };

    // --- 世界の物理の値 ---
    struct WorldPhysics {
        // 重力の強さ(µm/s²。0 = 無重力。上限 fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2)。ブランクの世界は 0(D-480)
        uint32_t gravityMicrometersPerSecond2 = 0;

        // 気体の音速の基準 c̃(mm/s。ADR-0176)。気体の圧力の波・衝撃波の速さの基準で、気体の箱を作るのに要る。
        // 0 = 無い(ブランクの世界。気体を使う世界は環境で渡す)。上限は気体の小刻みの数で決まる(GasSoundSpeedLimitMmPerS)
        uint32_t soundSpeedMillimetersPerSecond = 0;

        // 世界の端(既定は全部 wall。T-0710)
        WorldEdges edges;

        // 気体の格子で解けない乱れの粘り(動粘性 mm²/s。T-0758・D-516。名前と単位は仮)。乱された気体(火・爆発の後の風と残響)が、
        // 隣のセルと運動量を分け合って値で決まる時間で静まる(一様な風は減らない)。0 = 無い(ブランクの世界・既定。今までとビット単位で同じ)。
        // 上限 gas::GF_VISCOSITY_LIMIT_MM2_PER_S(0.5 m のセルで面の係数 1/8)。物質の粘り(viscosity_upa_s)は気体の物質にも書け、足し合わせる。
        // 6 m の部屋のいちばん大きい揺れ(波長 12 m)が e 分の 1 になる時間 ≒ 3.6 s ÷ (ν ÷ 10^6)。GPU の気体も同じ式(T-0775)。
        // 凍結の多重解像度の気体の木(gas_tree)には無い(MakeGasTree が 0 を求める)
        uint32_t gasEddyViscosityMm2PerS = 0;

        [[nodiscard]] bool operator==(const WorldPhysics&) const = default;
    };

    // --- 器が受けられる値か(T-0444)---

    // 範囲: 重力は fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2 まで、音速の基準は気体の小刻み gasSubsteps の安定の上限
    // (GasSoundSpeedLimitMmPerS)まで。パッケージの読み込み(script::ReadWorldEnvironment)と同じ決まりで、コードで作った表と
    // ホットリロードの検査(script::CheckHotReloadCompatible)が通す。気体の箱・木の検査(CheckGasEnvironment・CheckGasTreeEnvironment)も
    // 同じ理由の形でここを通す。gasFinestShift は多重解像度の木の ν がいちばん大きいレベルの k − σ(上限は 2^これ 分の 1。T-0470)。外なら理由
    [[nodiscard]] inline std::expected<void, std::string> CheckWorldPhysics(const WorldPhysics& world,
                                                                            uint32_t gasSubsteps = GasConfig{}.substeps,
                                                                            uint32_t gasFinestShift = 0) {
        if (world.gravityMicrometersPerSecond2 > fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2) {
            return std::unexpected(std::format("重力 {} µm/s² は器の上限 {} µm/s² を超える",
                                               world.gravityMicrometersPerSecond2,
                                               fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2));
        }

        const uint64_t soundSpeedLimit = GasSoundSpeedLimitMmPerS(gasSubsteps) >> gasFinestShift;
        if (world.soundSpeedMillimetersPerSecond > soundSpeedLimit) {
            const std::string tree = gasFinestShift == 0
                                         ? std::string()
                                         : std::format("(木のいちばん細かい ν のレベルで 2^{} 分の 1)", gasFinestShift);
            return std::unexpected(std::format("音速の基準 {} mm/s は気体の小刻み {}{} の安定の上限 {} mm/s を超える",
                                               world.soundSpeedMillimetersPerSecond, gasSubsteps, tree,
                                               soundSpeedLimit));
        }

        if (world.gasEddyViscosityMm2PerS > gas::GF_VISCOSITY_LIMIT_MM2_PER_S) {
            return std::unexpected(std::format("気体の乱れの粘り {} mm²/s は器の上限 {} mm²/s を超える",
                                               world.gasEddyViscosityMm2PerS, gas::GF_VISCOSITY_LIMIT_MM2_PER_S));
        }

        return {};
    }

    // --- 各サブシステムの設定へ渡す ---

    // 物理: 重力(2^-20 m/s²。PxParameters::gravity・GpuPhysics::SetGravity・PhysicsWorld::SetGravity の単位)
    [[nodiscard]] inline int64_t PhysicsGravityFor(const WorldPhysics& world) {
        return fx::PxGravityFromMicrometers(world.gravityMicrometersPerSecond2);
    }

    // 物理: 解き方の値(反復の数・硬さなど。器の既定は PxDefaultParameters)に、世界の重力を入れる
    [[nodiscard]] inline physics::PxParameters PhysicsParametersFor(
        const WorldPhysics& world, physics::PxParameters solver = physics::PxDefaultParameters()) {
        solver.gravity = PhysicsGravityFor(world);
        return solver;
    }

    // 気体: 設定の重力の強さと音速の基準を世界の値にする(重さの力積・層の重さの圧力・重さの仕事・圧力の拡散・α は
    // MakeGasCoefficients・MakeGasBox が作る)
    inline void ApplyWorldPhysics(const WorldPhysics& world, GasConfig& config) {
        config.gravityMicrometersPerSecond2 = world.gravityMicrometersPerSecond2;
        config.soundSpeedMmPerS = world.soundSpeedMillimetersPerSecond;
        config.eddyViscosityMm2PerS = world.gasEddyViscosityMm2PerS;
    }

}  // namespace bicameral::sim
