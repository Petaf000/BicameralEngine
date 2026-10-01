// avbd_solver.h — 剛体の AVBD(Augmented Vertex Block Descent)の試作(double。T-0016)。
// 物 = 6 自由度の「頂点」。1 刻みの中は刻みの初めで線形化した接触(C = C0·(1−α) + J·Δx)を拡張ラグランジュ法で解き、
// 物ごとに 6×6 の連立方程式を解いて位置を直す(彩色した Gauss-Seidel)。整数版(shaders/common/physics_*.hlsli)の手本。
//
// データの流れ: sim::PhysicsScene(整数の場面)→ double に直す → Step() を繰り返す → bodies() と StepStats を読む
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "box_collision.h"
#include "lab_math.h"
#include "sim/physics_scene.h"

namespace bicameral::lab {

    struct SolverParameters {
        // --- 反復 ---
        int iterations = 10;
        int substeps = 1;           // 1 刻みを何回に分けて解くか(分けるたびに接触を作り直す)
        bool postStabilize = true;  // 本反復は新しい誤差だけ防ぎ(α = 1)、最後の 1 回で残りの誤差を直す(速度に入れない)

        // --- 拡張ラグランジュ(AVBD の論文の記号)---
        double alpha = 0.99;  // postStabilize でないとき: 刻みの初めの誤差を 1 刻みで直す割合の残り
        double gamma = 0.99;  // 刻みをまたいだ λ と硬さの引き継ぎの割合
        double betaPerKilogram =
            1e5;  // 硬さの増え方 β(N/m を |C| m あたり。質量 1 kg あたり。組の両方に触れている物の中で一番重い質量を掛ける)
        double penaltyMin = 1;     // 硬さの下限(N/m)
        double penaltyMax = 1e12;  // 硬さの上限(N/m)
        double
            penaltyRatioMax = 1
                              << 20;  // 硬さの上限その 2: 組の軽い方(動く物)の M/h² × これ(整数の 6×6 の条件数を抑える)
        bool activeHessianOnly = false;  // 力が上下限に張り付いた行(離れた先読みの接触など)の k J Jᵀ を足さない
        double startPenaltyScale = 1;  // 組の硬さの下限 = これ × 組の質量 / h²(新しい接触もここから。0 なら penaltyMin)

        // --- 接触 ---
        double collisionMargin = 0.03;  // この距離まで離れていても接触を作る(m。これに相対速度 × h を足す)
        double stickThreshold = 0.01;   // 静止摩擦で接触点を保つ、接線のずれの上限(m)
        double gapSlop = 0.001;         // 本反復で、これ(m)以下の隙間は触れているとみなす(T-0091)
        double proximityMatch = 0.02;   // 特徴の番号が合わない点は、これ(m)以内の前の点から λ を引き継ぐ(T-0091)

        // --- 反復の途中で接触を探し直す(T-0091)---
        int recollideIteration = 5;         // この反復の前に、今の推定の姿勢で接触を探し直す(-1 = しない)
        double recollideMinMotion = 0.005;  // 組の 1 刻みの動きがこれ(m)を超える時だけ
        double gravity = 9.80665;           // m/s²(−y 向き)
        double timeStep = 1.0 / 60;
    };

    struct LabBody {
        // --- 形と質量 ---
        Vec3 halfExtent;
        double mass = 0;    // 0 = 動かない
        Vec3 inertiaLocal;  // 主慣性モーメント(局所の軸)

        // --- 状態 ---
        Vec3 position;
        Quat rotation;
        Vec3 velocity;
        Vec3 angularVelocity;
        bool active = false;

        // --- 刻みの中 ---
        Vec3 previousVelocity;
        Vec3 startPosition;
        Quat startRotation;
        Mat3 inertiaWorld;
        Vec3 inertialLinear;   // 慣性の目標(刻みの初めからの変位)
        Vec3 inertialAngular;  // 慣性の目標(刻みの初めからの回転ベクトル)
        Vec3 deltaLinear;      // 今の変位
        Vec3 deltaAngular;     // 今の回転ベクトル
        int color = -1;

        [[nodiscard]] bool IsDynamic() const { return mass > 0; }
    };

    // 接触点 1 つの 3 行(法線・接線 2 本)
    struct ContactPoint {
        uint32_t feature = 0;
        Vec3 normal;  // A → B(点ごと。途中で探し直した点は、その姿勢の法線)
        Vec3 localA;  // A の局所座標での接触点
        Vec3 localB;
        std::array<double, 3> lambda{};
        std::array<double, 3> penalty{};
        bool stick = false;

        // --- 刻みの初めの線形化 ---
        std::array<Vec3, 3> basis{};  // 法線(A → B)・接線 2 本
        std::array<double, 3> c0{};
        std::array<Vec3, 3> angularA{};  // ∂C/∂θA
        std::array<Vec3, 3> angularB{};  // ∂C/∂θB
    };

    struct Manifold {
        int bodyA = 0;
        int bodyB = 0;
        Vec3 normal;  // A → B
        double friction = 0.5;
        double pairMass = 0;                   // 硬さの下限の尺度にする質量(組の動く物の重い方)
        double beta = 0;                       // 硬さの増え方(N/m²。組の両方に触れている物の中で一番重い質量 × βkg)
        double minPenalty = 0;                 // この組の硬さの下限(N/m)。引き継ぎで減っても、これより下げない
        double maxPenalty = 0;                 // この組の硬さの上限(N/m)
        std::array<ContactPoint, 8> points{};  // 接触の生成で 4 点まで + 途中の探し直しで 4 点まで
        int count = 0;
    };

    struct StepStats {
        double maxPenetration = 0;  // 刻みの初めの接触の最大の食い込み(m)
        double maxSpeed = 0;        // 刻みの終わりの最大の速さ(m/s、重心の速さ + 角速度 × 外接球の半径)
        double energy = 0;          // 運動 + 重力の位置エネルギー(J)
        int contactCount = 0;
        int colorCount = 0;

        // --- 一番深い接触(調べる用)---
        int deepestBodyA = -1;
        int deepestBodyB = -1;
        uint32_t deepestFeature = 0;
        bool deepestIsNew = false;   // 前の刻みにこの組の接触が無かった
        double deepestApproach = 0;  // 法線の向きの近づく速さ(m/s、正なら近づく)

        // --- 一番大きな速度の飛び(重力のぶんを除く。調べる用)---
        int kickBody = -1;
        double kick = 0;  // m/s(重心の速さ + 角速度 × 外接球の半径の変化)
    };

    class AvbdSolver {
    public:
        AvbdSolver(const sim::PhysicsScene& scene, const SolverParameters& parameters);

        void Step();

        [[nodiscard]] const std::vector<LabBody>& Bodies() const { return m_bodies; }
        [[nodiscard]] const StepStats& Stats() const { return m_stats; }
        [[nodiscard]] uint64_t Tick() const { return m_tick; }
        [[nodiscard]] const std::map<std::pair<int, int>, Manifold>& Manifolds() const { return m_manifolds; }

    private:
        void Substep();
        void UpdateActivity();
        void UpdateContacts();
        void MatchByProximity(const LabBody& bodyA, const Manifold& old, Manifold& manifold) const;
        void UpdateBetas();
        void RecollideMidStep();
        void LinearizePoint(const Manifold& manifold, ContactPoint& point) const;
        void WarmStart();
        void InitializeBodies();
        void Linearize();
        void ColorBodies();
        void SolveBody(int index, double alpha);
        void UpdateDuals(double alpha);
        void Finish();

        [[nodiscard]] double SubstepTime() const;
        [[nodiscard]] BoxShape ShapeOf(const LabBody& body) const;
        [[nodiscard]] double ConstraintValue(const Manifold& manifold, const ContactPoint& point, int row,
                                             double alpha) const;

        sim::PhysicsScene m_scene;
        SolverParameters m_parameters;
        std::vector<LabBody> m_bodies;
        std::map<std::pair<int, int>, Manifold> m_manifolds;
        std::vector<std::vector<std::pair<Manifold*, bool>>> m_bodyManifolds;  // 物ごとの接触(true = 物が A)
        int m_colorCount = 0;
        uint64_t m_tick = 0;
        StepStats m_stats;
    };

    [[nodiscard]] double MechanicalEnergy(const LabBody& body, double gravity);

}  // namespace bicameral::lab
