// physics_world.h — 整数の AVBD(08 §2。T-0016 研究 R-PHYS-1)の CPU の世界。式は shaders/common/physics_*.hlsli にあり、
// ここは呼ぶ順番(接触の生成 → λ と硬さの引き継ぎ → 慣性の目標 → 線形化 → 彩色 → 反復 → 速度 → 位置の仕上げ)と、
// 刻みをまたいで残す接触(組ごと・点の特徴ごと)を持つ。T-0090 で GPU に同じ手順を載せ、これをリファレンスにする。
//
// データの流れ: PhysicsScene(整数の場面)→ PhysicsWorld → Step() を繰り返す → Bodies()・Stats()・StateHash()
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "common/physics_collision.hlsli"
#include "common/physics_solver.hlsli"
#include "sim/physics_scene.h"

namespace bicameral::sim {

    struct PhysicsBody {
        // --- 形と質量(0 = 動かない)---
        physics::PxVec3 halfExtent{};  // 2^-20 m
        uint64_t massMilligrams = 0;
        int64_t massOverH2 = 0;           // M/h²(2^-8 N/m)
        physics::PxVec3 inertiaOverH2{};  // 局所の主慣性モーメント / h²(2^-8 N·m/rad)
        int64_t boundingRadius = 0;       // 外接球の半径(2^-20 m)

        // --- 状態 ---
        physics::PxVec3 position{};         // 2^-20 m
        physics::PxQuat rotation{};         // Q1.30
        physics::PxVec3 velocity{};         // 2^-20 m/s
        physics::PxVec3 angularVelocity{};  // 2^-20 rad/s
        physics::PxVec3 previousVelocity{};
        bool active = false;

        // --- 刻みの中(2^-32 m・2^-32 rad)---
        physics::PxVec3 startPosition{};
        physics::PxQuat startRotation{};
        physics::PxMat3 inertiaWorld{};  // 回転の I/h²(世界の向き)
        physics::PxVec3 inertialLinear{};
        physics::PxVec3 inertialAngular{};
        physics::PxVec3 deltaLinear{};
        physics::PxVec3 deltaAngular{};
        int32_t color = -1;

        [[nodiscard]] bool IsDynamic() const { return massMilligrams > 0; }
    };

    struct PhysicsContactPoint {
        uint32_t feature = 0;
        physics::PxVec3 localA{};  // A の局所座標での接触点(2^-20 m)
        physics::PxVec3 localB{};
        std::array<physics::PxRow, 3> rows{};  // 法線・接線 2 本
        bool stick = false;
    };

    struct PhysicsManifold {
        uint32_t bodyA = 0;
        uint32_t bodyB = 0;
        physics::PxVec3 normal{};  // A → B(Q1.30)
        int64_t frictionQ16 = 0;
        int64_t beta = 0;        // 硬さの増え方(N/m²。組の重い方の質量 × β)
        int64_t minPenalty = 0;  // 組の硬さの下限(2^-8 N/m)
        int64_t maxPenalty = 0;  // 組の硬さの上限(2^-8 N/m)
        std::array<PhysicsContactPoint, 4> points{};
        uint32_t count = 0;
    };

    // 刻みごとの量と、値の幅の記録(研究 R-PHYS-1: 各量の最大のビット数)
    struct PhysicsStepStats {
        int64_t maxPenetration = 0;  // 刻みの初めの接触の最大の食い込み(2^-20 m)
        uint32_t contactCount = 0;
        uint32_t colorCount = 0;
        uint32_t solveFailures = 0;  // 6×6 の分解で対角が 0 以下になった回数

        // --- 最大のビット数(符号を除く)---
        uint32_t solveBits = 0;    // 6×6 の途中の解(Q62)
        uint32_t lhsBits = 0;      // H の要素
        uint32_t rhsBits = 0;      // 勾配
        uint32_t penaltyBits = 0;  // 硬さ
        uint32_t lambdaBits = 0;   // λ
        uint32_t deltaBits = 0;    // 変位・回転(2^-32)
    };

    class PhysicsWorld {
    public:
        PhysicsWorld(const PhysicsScene& scene, const physics::PxParameters& parameters);

        void Step();

        [[nodiscard]] const std::vector<PhysicsBody>& Bodies() const { return m_bodies; }
        [[nodiscard]] const PhysicsStepStats& Stats() const { return m_stats; }
        [[nodiscard]] const std::map<std::pair<uint32_t, uint32_t>, PhysicsManifold>& Manifolds() const {
            return m_manifolds;
        }
        [[nodiscard]] uint64_t Tick() const { return m_tick; }

        // 全部の物の位置・向き・速度のハッシュ(2 回の実行の一致・T-0090 の GPU との一致に使う)
        [[nodiscard]] uint64_t StateHash() const;

    private:
        void Substep();
        void UpdateActivity();
        void UpdateContacts();
        void AddManifold(uint32_t a, uint32_t b, const physics::PxContactGeometry& geometry,
                         std::map<std::pair<uint32_t, uint32_t>, PhysicsManifold>& next);
        void WarmStart();
        void InitializeBodies();
        void Linearize();
        void ColorBodies();
        [[nodiscard]] std::vector<std::vector<uint32_t>> DynamicNeighbors() const;
        [[nodiscard]] std::vector<uint32_t> ChooseLocalMaxima(
            const std::vector<std::vector<uint32_t>>& neighbors) const;
        void AssignSmallestColor(uint32_t index, const std::vector<uint32_t>& neighbors);
        void SolveColor(int32_t color, int64_t alphaQ16);
        void SolveBody(uint32_t index, int64_t alphaQ16);
        [[nodiscard]] physics::PxBodySystem AddContactRows(physics::PxBodySystem system,
                                                           const PhysicsManifold& manifold, bool isA,
                                                           int64_t alphaQ16) const;
        void RecordSystemBits(const physics::PxBodySystem& system);
        void UpdateDuals(int64_t alphaQ16);
        void UpdatePointDuals(PhysicsContactPoint& point, const PhysicsManifold& manifold, int64_t alphaQ16);
        void UpdateVelocities();
        void Finish();

        [[nodiscard]] physics::PxBox ShapeOf(const PhysicsBody& body) const;
        [[nodiscard]] int64_t ConstraintValue(const PhysicsManifold& manifold, const physics::PxRow& row, bool isNormal,
                                              int64_t alphaQ16) const;

        PhysicsScene m_scene;
        physics::PxParameters m_parameters;
        int64_t m_rate = 0;  // 1 秒あたりの小刻みの数
        std::vector<PhysicsBody> m_bodies;
        std::map<std::pair<uint32_t, uint32_t>, PhysicsManifold> m_manifolds;
        std::vector<std::vector<std::pair<const PhysicsManifold*, bool>>>
            m_bodyManifolds;  // 物ごとの接触(true = 物が A)
        int32_t m_colorCount = 0;
        uint64_t m_tick = 0;
        PhysicsStepStats m_stats;
    };

}  // namespace bicameral::sim
