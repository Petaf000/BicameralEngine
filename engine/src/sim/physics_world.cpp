// physics_world.cpp — 整数の AVBD の CPU の世界(physics_world.h)。物ごと・組ごとの手順は shaders/common/physics_step.hlsli(GPU と共通)、
// 式は physics_*.hlsli。手順と分岐は試作(tools/physics_lab/avbd_solver.cpp)に合わせてある(違いは整数の単位と丸めだけ)。
#include "sim/physics_world.h"

#include <algorithm>
#include <ranges>

namespace bicameral::sim {

    namespace {

        using namespace bicameral::physics;
        using namespace bicameral::fx;

        uint32_t BitsOf(int64_t value) {
            return value == 0 ? 0 : FxMsbU64(FxAbsU64(value)) + 1;
        }

    }  // namespace

    PhysicsBody MakePhysicsBody(const PhysicsSceneBody& source, int64_t rate) {
        PhysicsBody body{};
        body.halfExtent = PxMakeVec3(source.halfExtent[0], source.halfExtent[1], source.halfExtent[2]);
        body.massMilligrams = source.massMilligrams;
        body.massOverH2 = PxMassOverStepSquared(source.massMilligrams, rate);

        const PxVec3 h = body.halfExtent;
        body.inertiaOverH2 = PxMakeVec3(PxBoxInertiaOverStepSquared(body.massMilligrams, h.y, h.z, rate),
                                        PxBoxInertiaOverStepSquared(body.massMilligrams, h.x, h.z, rate),
                                        PxBoxInertiaOverStepSquared(body.massMilligrams, h.x, h.y, rate));
        body.boundingRadius = (int64_t)PxLength(h);
        body.spawnTick = source.spawnTick;
        body.removeTick = source.removeTick;
        body.position = PxMakeVec3(source.position[0], source.position[1], source.position[2]);
        body.rotation = PxQuatNormalize(
            PxMakeQuat(source.rotation[0], source.rotation[1], source.rotation[2], source.rotation[3]));
        body.color = PX_NO_COLOR;

        return body;
    }

    PhysicsWorld::PhysicsWorld(const PhysicsScene& scene, const PxParameters& parameters)
        : m_scene(scene), m_parameters(parameters), m_rate(PxStepRate(parameters)) {
        for (const PhysicsSceneBody& source : scene.bodies)
            m_bodies.push_back(MakePhysicsBody(source, m_rate));

        m_bodyManifolds.resize(m_bodies.size());
    }

    void PhysicsWorld::Step() {
        m_stats = {};
        UpdateActivity();
        for (uint32_t i = 0; i < m_parameters.substeps; ++i)
            Substep();

        ++m_tick;
    }

    void PhysicsWorld::Substep() {
        UpdateContacts();
        WarmStart();
        InitializeBodies();
        Linearize();
        ColorBodies();

        // 本反復は α = 1(新しい誤差だけ防ぐ)、最後の 1 回は α = 0(残りの誤差を直す。速度には入れない)
        const uint32_t iterations = m_parameters.iterations;
        for (uint32_t iteration = 0; iteration <= iterations; ++iteration) {
            if (iteration == m_parameters.recollideIteration)
                RecollideMidStep();

            const int64_t alphaQ16 = iteration < iterations ? PX_ALPHA_ONE_Q16 : 0;
            for (int32_t color = 0; color < m_colorCount; ++color)
                SolveColor(color, alphaQ16);

            if (iteration < iterations)
                UpdateDuals(alphaQ16);

            if (iteration + 1 == iterations)
                UpdateVelocities();
        }

        Finish();
    }

    void PhysicsWorld::UpdateActivity() {
        for (PhysicsBody& body : m_bodies)
            body.active = PxIsActiveAt(body, m_tick);
    }

    // 近い組ごとに接触を作り、前の刻みの同じ組から引き継ぐ
    void PhysicsWorld::UpdateContacts() {
        std::map<std::pair<uint32_t, uint32_t>, PhysicsManifold> next;
        const auto count = (uint32_t)m_bodies.size();
        for (uint32_t a = 0; a < count; ++a) {
            for (uint32_t b = a + 1; b < count; ++b) {
                const PhysicsBody& bodyA = m_bodies[a];
                const PhysicsBody& bodyB = m_bodies[b];
                const int64_t margin = PxPairMargin(bodyA, bodyB, m_parameters, m_rate);
                if (margin < 0)
                    continue;

                const PxContactGeometry geometry = PxCollideBoxes(PxShapeOf(bodyA), PxShapeOf(bodyB), margin);
                if (geometry.count == 0)
                    continue;

                const auto found = m_manifolds.find({a, b});
                const PxPreviousManifold previous{found != m_manifolds.end() ? found->second : PhysicsManifold{}};
                const PhysicsManifold manifold = PxBuildManifold(a, b, bodyA, bodyB, geometry, m_scene.frictionQ16,
                                                                 previous, m_parameters, m_rate);
                m_stats.maxPenetration = std::max(m_stats.maxPenetration, PxMaxPenetration(geometry));
                m_stats.contactCount += manifold.count;
                next.emplace(std::pair{a, b}, manifold);
            }
        }

        m_manifolds = std::move(next);
        for (auto& list : m_bodyManifolds)
            list.clear();

        for (const auto& [key, manifold] : m_manifolds) {
            m_bodyManifolds[manifold.bodyA].emplace_back(&manifold, true);
            m_bodyManifolds[manifold.bodyB].emplace_back(&manifold, false);
        }

        UpdateBetas();
    }

    // 硬さの増え方 β = βkg × 組の両方に触れている物の中で一番重い質量(physics_step.hlsli の PxBetaOf)
    void PhysicsWorld::UpdateBetas() {
        std::vector<uint64_t> touchingMass(m_bodies.size());
        for (uint32_t i = 0; i < m_bodies.size(); ++i)
            touchingMass[i] = PxTouchMass(m_bodies[i]);

        for (const auto& [key, manifold] : m_manifolds) {
            touchingMass[manifold.bodyA] = std::max(touchingMass[manifold.bodyA],
                                                    PxTouchMass(m_bodies[manifold.bodyB]));
            touchingMass[manifold.bodyB] = std::max(touchingMass[manifold.bodyB],
                                                    PxTouchMass(m_bodies[manifold.bodyA]));
        }

        for (auto& [key, manifold] : m_manifolds) {
            const uint64_t mass = std::max(touchingMass[manifold.bodyA], touchingMass[manifold.bodyB]);
            manifold.beta = PxBetaOf(mass, m_parameters);
        }
    }

    void PhysicsWorld::WarmStart() {
        for (auto& [key, manifold] : m_manifolds)
            manifold = PxWarmStartManifold(manifold, m_parameters.gammaQ16);
    }

    void PhysicsWorld::InitializeBodies() {
        for (PhysicsBody& body : m_bodies)
            body = PxInitializeBody(body, m_rate);
    }

    void PhysicsWorld::Linearize() {
        for (auto& [key, manifold] : m_manifolds)
            manifold = PxLinearizeManifold(manifold, m_bodies[manifold.bodyA], m_bodies[manifold.bodyB]);
    }

    // 拘束でつながる物どうしが違う色になるように塗る(Jones-Plassmann: 番号のハッシュを優先度に。GPU でも同じ色になる)
    void PhysicsWorld::ColorBodies() {
        const std::vector<std::vector<uint32_t>> neighbors = DynamicNeighbors();
        m_colorCount = 0;
        uint32_t rounds = 0;
        for (;;) {
            const std::vector<uint32_t> chosen = ChooseLocalMaxima(neighbors);
            if (chosen.empty())
                break;

            for (uint32_t i : chosen)
                AssignSmallestColor(i, neighbors[i]);

            rounds += 1;
        }

        m_stats.colorCount = std::max(m_stats.colorCount, (uint32_t)m_colorCount);
        m_stats.colorRounds = std::max(m_stats.colorRounds, rounds);
    }

    // 動く物どうしの接触でつながる隣
    std::vector<std::vector<uint32_t>> PhysicsWorld::DynamicNeighbors() const {
        std::vector<std::vector<uint32_t>> neighbors(m_bodies.size());
        for (const auto& [key, manifold] : m_manifolds) {
            if (!PxIsDynamic(m_bodies[manifold.bodyA]) || !PxIsDynamic(m_bodies[manifold.bodyB]))
                continue;

            neighbors[manifold.bodyA].push_back(manifold.bodyB);
            neighbors[manifold.bodyB].push_back(manifold.bodyA);
        }

        return neighbors;
    }

    // まだ色の無い物のうち、色の無い隣のどれよりも優先度が高い物(同じ回で選ばれた物どうしは隣り合わない)
    std::vector<uint32_t> PhysicsWorld::ChooseLocalMaxima(const std::vector<std::vector<uint32_t>>& neighbors) const {
        std::vector<uint32_t> chosen;
        for (uint32_t i = 0; i < m_bodies.size(); ++i) {
            const PhysicsBody& body = m_bodies[i];
            if (!PxIsDynamic(body) || body.active == 0 || body.color >= 0)
                continue;

            const auto isBelow = [&](uint32_t j) {
                return m_bodies[j].color >= 0 || PxColorPriorityBelow(j, i);
            };
            if (std::ranges::all_of(neighbors[i], isBelow))
                chosen.push_back(i);
        }

        return chosen;
    }

    // 隣が使っていない一番小さい色
    void PhysicsWorld::AssignSmallestColor(uint32_t index, const std::vector<uint32_t>& neighbors) {
        int32_t color = 0;
        while (std::ranges::any_of(neighbors, [&](uint32_t j) { return m_bodies[j].color == color; }))
            ++color;

        m_bodies[index].color = color;
        m_colorCount = std::max(m_colorCount, color + 1);
    }

    // 反復の途中で、今の推定の姿勢で接触を探し直す(physics_step.hlsli の PxRecollideManifold)
    void PhysicsWorld::RecollideMidStep() {
        for (auto& [key, manifold] : m_manifolds) {
            const uint32_t before = manifold.count;
            manifold = PxRecollideManifold(manifold, m_bodies[manifold.bodyA], m_bodies[manifold.bodyB], m_parameters);
            m_stats.contactCount += manifold.count - before;
        }
    }

    // 同じ色の物は拘束を共有しないので、どの順に解いても結果は同じ(GPU では並列に解く)
    void PhysicsWorld::SolveColor(int32_t color, int64_t alphaQ16) {
        for (uint32_t i = 0; i < m_bodies.size(); ++i) {
            if (m_bodies[i].color == color)
                SolveBody(i, alphaQ16);
        }
    }

    void PhysicsWorld::SolveBody(uint32_t index, int64_t alphaQ16) {
        PhysicsBody& body = m_bodies[index];
        PxBodySystem system = PxBeginSystemOf(body);
        for (const auto& [manifold, isA] : m_bodyManifolds[index]) {
            const PhysicsBody& a = m_bodies[manifold->bodyA];
            const PhysicsBody& b = m_bodies[manifold->bodyB];
            for (uint32_t k = 0; k < manifold->count; ++k) {
                system = PxAddPointRows(system, manifold->points[k], manifold->frictionQ16, isA, alphaQ16,
                                        m_parameters.gapSlop, a.deltaLinear, a.deltaAngular, b.deltaLinear,
                                        b.deltaAngular);
            }
        }

        RecordSystemBits(system);
        const PxSolveResult solved = PxSolveSymmetric6(system.lhs, system.rhs, PX_SOLVE_GAIN_SHIFT);
        m_stats.solveBits = std::max(m_stats.solveBits, solved.maxBits);
        m_stats.solveFailures += solved.ok ? 0 : 1;

        body = PxApplySolution(body, solved.x);
        for (uint32_t i = 0; i < 3; ++i) {
            m_stats.deltaBits = std::max(m_stats.deltaBits, BitsOf(PxGet(body.deltaLinear, i)));
            m_stats.deltaBits = std::max(m_stats.deltaBits, BitsOf(PxGet(body.deltaAngular, i)));
        }
    }

    void PhysicsWorld::RecordSystemBits(const PxBodySystem& system) {
        for (int64_t value : system.lhs.m)
            m_stats.lhsBits = std::max(m_stats.lhsBits, BitsOf(value));

        for (int64_t value : system.rhs.v)
            m_stats.rhsBits = std::max(m_stats.rhsBits, BitsOf(value));
    }

    void PhysicsWorld::UpdateDuals(int64_t alphaQ16) {
        for (auto& [key, manifold] : m_manifolds) {
            const PhysicsBody& a = m_bodies[manifold.bodyA];
            const PhysicsBody& b = m_bodies[manifold.bodyB];
            for (uint32_t k = 0; k < manifold.count; ++k) {
                PhysicsContactPoint& contactPoint = manifold.points[k];
                contactPoint = PxUpdatePointDuals(contactPoint, manifold.frictionQ16, manifold.beta,
                                                  manifold.maxPenalty, alphaQ16, m_parameters, a.deltaLinear,
                                                  a.deltaAngular, b.deltaLinear, b.deltaAngular);
                RecordPointBits(contactPoint);
            }
        }
    }

    void PhysicsWorld::RecordPointBits(const PhysicsContactPoint& contactPoint) {
        for (const PxRow& row : contactPoint.rows) {
            m_stats.penaltyBits = std::max(m_stats.penaltyBits, BitsOf(row.penalty));
            m_stats.lambdaBits = std::max(m_stats.lambdaBits, BitsOf(row.lambda));
        }
    }

    void PhysicsWorld::UpdateVelocities() {
        for (PhysicsBody& body : m_bodies)
            body = PxUpdateVelocity(body, m_rate);
    }

    void PhysicsWorld::Finish() {
        for (PhysicsBody& body : m_bodies)
            body = PxFinishBody(body);
    }

    uint64_t PhysicsWorld::StateHash() const {
        uint64_t hash = FxMix64(m_tick);
        for (const PhysicsBody& body : m_bodies)
            hash = PxHashBody(hash, body);

        return hash;
    }

}  // namespace bicameral::sim
