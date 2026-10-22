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
#include <string_view>

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

    // --- 気体の圧力の伝わる速さ(T-0804・D-526 ②・D-519。docs/design/20 §1.1。世界ごとに 1 つ)---

    // 環境の gas_pressure_speed = "sound" / "instant"(名前は仮。Q131)。既定は本物の音速(D-519)
    enum class GasPressureSpeed : uint8_t {
        Sound = 0,    // 本物の音速(既定。陽解法の小刻み。今までと同じ)
        Instant = 1,  // 最速(刻みの中で圧力を解く投影・音速 0。D-467)。今効くのは仮の世界の CPU(ProbeGasField)だけ
    };

    // 最速を選んだのに効かない置き場の知らせの頭(試験が使う)
    inline constexpr std::string_view GAS_INSTANT_NOT_YET_PREFIX = "最速はまだ効かない: ";

    // --- 釣り合う家族の V サイクルの回数(T-0955・ADR-0427。20 §9.2: 法則ごとの定数・判定で早く止めない)---
    // 環境の balance_cycles = { gas_pressure, liquid_pressure, support }(名前は仮)。書かない法則は既定の 2(T-0845・T-0919 の今の値)。
    // 回数は 1 刻みの費用に比例し、解けきらない所が釣り合いに着くまでの刻みを減らす(M-1: 水と空気の境で V 2 は 69 Pa・V 4 は 2.7 Pa)
    inline constexpr uint32_t BALANCE_CYCLES_DEFAULT = 2;
    inline constexpr uint32_t BALANCE_CYCLES_MAX = 16;  // 上限(書き間違いの知らせ。20 §1.4)。費用は回数に比例

    struct BalanceCycles {
        uint32_t gasPressure = BALANCE_CYCLES_DEFAULT;  // 気体の陰の圧力(GasProjectionOptions::solver.cycles)
        uint32_t
            liquidPressure = BALANCE_CYCLES_DEFAULT;  // 液体の陰の圧力(案 B の R1。読む所は C-0 の部品へ移る時。T-0868)
        uint32_t
            support = BALANCE_CYCLES_DEFAULT;  // 支えの法則(SupportOptions の blocks.cycles・equilibrium.solver.cycles)

        [[nodiscard]] bool operator==(const BalanceCycles&) const = default;
    };

    // --- 細かい所の半径と移り目(T-0954・ADR-0431。20 §11.1。物理の長さ mm)---
    // 環境の fine_region = { radius_mm, far_leaf_mm, levels_per_doubling }(名前は仮)。距離は局所の状態で細かくした所(種: 活性・前線・割れの先端)
    // からで、視点からではない(D-544)。半径の内は細かいセルのまま、その外は距離が 2 倍になるごとに levels_per_doubling 段ずつ粗く、
    // 遠くの葉の大きさで頭打ち(規則は sim/multires_grading)。既定は T-0900 M-0 の仮の幅の内(半径 128 m・遠くの葉 32 m・1 段。
    // 20 km の見込みは docs/perf.md)。どの値にするかは描画と並ぶ VRAM の線(D-209)で決まるゲームの中身
    inline constexpr uint64_t FINE_REGION_RADIUS_DEFAULT_MM = 128000;
    inline constexpr uint64_t FINE_REGION_FAR_LEAF_DEFAULT_MM = 32000;
    inline constexpr uint32_t FINE_REGION_LEVELS_PER_DOUBLING_DEFAULT = 1;
    inline constexpr uint64_t FINE_REGION_LENGTH_MAX_MM = 1000000000;  // 1000 km(書き間違いの知らせ。20 §1.4)
    inline constexpr uint32_t FINE_REGION_LEVELS_PER_DOUBLING_MAX = 8;

    struct FineRegion {
        uint64_t radiusMm = FINE_REGION_RADIUS_DEFAULT_MM;                     // 種からこの距離までは細かいセルのまま
        uint64_t farLeafMm = FINE_REGION_FAR_LEAF_DEFAULT_MM;                  // 遠くの葉の大きさ(これより粗くしない)
        uint32_t levelsPerDoubling = FINE_REGION_LEVELS_PER_DOUBLING_DEFAULT;  // 距離が 2 倍ごとに粗くする段の数

        [[nodiscard]] bool operator==(const FineRegion&) const = default;
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

        // 気体の圧力の伝わる速さ(T-0804・D-526 ②)。Instant は仮の世界の CPU の気体(ProbeGasField)を最速の投影で解き、
        // 小さい空きの層を警告する(ベイク・実行時。D-526 ①)。多重解像度と GPU の気体は陽解法だけなので本物の音速のまま流し、知らせる
        GasPressureSpeed gasPressureSpeed = GasPressureSpeed::Sound;

        // 釣り合う家族の V の回数(T-0955。既定は全部 2 = 今まで通り)
        BalanceCycles balanceCycles;

        // 細かい所の半径と移り目(T-0954。既定は FINE_REGION_*_DEFAULT)
        FineRegion fineRegion;

        [[nodiscard]] bool operator==(const WorldPhysics&) const = default;
    };

    // 最速を選んだ世界で、最速がまだ効かない置き場 where(「多重解像度の世界の気体」など)の知らせ(音速の世界は空)。
    // 値は本物の音速(陽解法)のまま流す(黙って無視しない。D-487 ②)
    [[nodiscard]] inline std::string GasInstantNotYetNotice(const WorldPhysics& world, std::string_view where) {
        if (world.gasPressureSpeed != GasPressureSpeed::Instant)
            return {};

        return std::format(
            "{}{}は陽解法だけ(投影は T-0621・T-0531)。environment.gas_pressure_speed = \"instant\" は"
            "効かず、本物の音速で流す",
            GAS_INSTANT_NOT_YET_PREFIX, where);
    }

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

        const BalanceCycles& cycles = world.balanceCycles;
        for (const uint32_t count : {cycles.gasPressure, cycles.liquidPressure, cycles.support}) {
            if (count == 0 || count > BALANCE_CYCLES_MAX) {
                return std::unexpected(std::format("釣り合う家族の V の回数 {} は 1〜{} で書く(費用は回数に比例)",
                                                   count, BALANCE_CYCLES_MAX));
            }
        }

        const FineRegion& fine = world.fineRegion;
        if (fine.radiusMm > FINE_REGION_LENGTH_MAX_MM || fine.farLeafMm == 0 ||
            fine.farLeafMm > FINE_REGION_LENGTH_MAX_MM) {
            return std::unexpected(
                std::format("細かい所の半径 {} mm・遠くの葉 {} mm は 0〜{} mm(遠くの葉は 1 以上)で書く", fine.radiusMm,
                            fine.farLeafMm, FINE_REGION_LENGTH_MAX_MM));
        }

        if (fine.levelsPerDoubling == 0 || fine.levelsPerDoubling > FINE_REGION_LEVELS_PER_DOUBLING_MAX) {
            return std::unexpected(std::format("細かい所の移り目の段の数 {} は 1〜{} で書く", fine.levelsPerDoubling,
                                               FINE_REGION_LEVELS_PER_DOUBLING_MAX));
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
