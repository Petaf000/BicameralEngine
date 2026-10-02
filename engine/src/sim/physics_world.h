// physics_world.h — 整数の AVBD(08 §2。T-0016 研究 R-PHYS-1)の CPU の世界。GPU(T-0090、shaders/sim/physics_step.hlsl)のリファレンス。
// 物ごと・組ごと・接触点ごとの手順と構造体は shaders/common/physics_step.hlsli(GPU と共通)にあり、ここは呼ぶ順番
// (活性 → 接触の生成と引き継ぎ → β → 硬さの引き継ぎ → 物の初期化 → 線形化 → 彩色 → 反復(途中で接触の探し直し)→ 速度 → 位置の仕上げ)と、
// 刻みをまたいで残す接触(組ごと)を持つ。組は std::map で (小さい番号, 大きい番号) の順に持つ(GPU は持ち主の物の枠。順番に依存しない理由は physics_step.hlsli)。
//
// データの流れ: PhysicsScene(整数の場面)→ PhysicsWorld → Step() を繰り返す → Bodies()・Stats()・StateHash()
#pragma once

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "common/physics_step.hlsli"
#include "sim/physics_scene.h"

namespace bicameral::sim {

    using PhysicsBody = physics::PxBody;
    using PhysicsContactPoint = physics::PxContactPoint;
    using PhysicsManifold = physics::PxManifold;

    // HLSL の構造化バッファと同じ並び(physics_step.hlsli の約束。変えたら GPU のテストも確かめる)
    static_assert(sizeof(PhysicsBody) == 448);
    static_assert(sizeof(PhysicsContactPoint) == 368);
    static_assert(sizeof(PhysicsManifold) == 3016);
    static_assert(sizeof(physics::PxParameters) == 96);

    // 場面の 1 つの物から、刻みを始める前の物を作る
    [[nodiscard]] PhysicsBody MakePhysicsBody(const PhysicsSceneBody& source, int64_t rate);

    // 刻みごとの量と、値の幅の記録(研究 R-PHYS-1: 各量の最大のビット数)
    struct PhysicsStepStats {
        int64_t maxPenetration = 0;  // 刻みの初めの接触の最大の食い込み(2^-20 m)
        uint32_t contactCount = 0;
        uint32_t colorCount = 0;
        uint32_t colorRounds = 0;    // 彩色の回数(GPU の回数の上限を決める材料)
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

        // 全部の物の位置・向き・速度のハッシュ(2 回の実行の一致・GPU との一致に使う)
        [[nodiscard]] uint64_t StateHash() const;

    private:
        void Substep();
        void UpdateActivity();
        void UpdateContacts();
        void UpdateBetas();
        void WarmStart();
        void InitializeBodies();
        void Linearize();
        void ColorBodies();
        [[nodiscard]] std::vector<std::vector<uint32_t>> DynamicNeighbors() const;
        [[nodiscard]] std::vector<uint32_t> ChooseLocalMaxima(
            const std::vector<std::vector<uint32_t>>& neighbors) const;
        void AssignSmallestColor(uint32_t index, const std::vector<uint32_t>& neighbors);
        void RecollideMidStep();
        void SolveColor(int32_t color, int64_t alphaQ16);
        void SolveBody(uint32_t index, int64_t alphaQ16);
        void RecordSystemBits(const physics::PxBodySystem& system);
        void UpdateDuals(int64_t alphaQ16);
        void RecordPointBits(const PhysicsContactPoint& point);
        void UpdateVelocities();
        void Finish();

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
