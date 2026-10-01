// physics_world.cpp — 整数の AVBD の CPU の世界(physics_world.h)。式は shaders/common/physics_*.hlsli。
// 手順と分岐は試作(tools/physics_lab/avbd_solver.cpp)に合わせてある(違いは整数の単位と丸めだけ)。
#include "sim/physics_world.h"

#include <algorithm>
#include <ranges>

namespace bicameral::sim {

    namespace {

        using namespace bicameral::physics;
        using namespace bicameral::fx;

        constexpr int64_t ALPHA_ONE_Q16 = 65536;

        uint32_t BitsOf(int64_t value) {
            return value == 0 ? 0 : FxMsbU64(FxAbsU64(value)) + 1;
        }

        // 法線に直交する 2 本(法線の一番小さい成分の軸との外積から。試作と同じ選び方)
        std::array<PxVec3, 2> TangentBasis(PxVec3 normal) {
            const int64_t ax = PxAbs(normal.x);
            const int64_t ay = PxAbs(normal.y);
            const int64_t az = PxAbs(normal.z);
            PxVec3 helper = PxMakeVec3(0, 0, PX_UNIT_ONE);
            if (ax <= ay && ax <= az)
                helper = PxMakeVec3(PX_UNIT_ONE, 0, 0);
            else if (ay <= az)
                helper = PxMakeVec3(0, PX_UNIT_ONE, 0);

            const PxVec3 first = PxNormalize(PxCross(normal, helper, PX_UNIT_SHIFT));
            return {first, PxCross(normal, first, PX_UNIT_SHIFT)};
        }

        // 回転の I/h² を世界の向きに: (R diag(I) Rᵀ)_ij = Σ_k R_ik I_k R_jk
        int64_t WorldInertiaEntry(PxMat3 rotation, PxVec3 inertia, uint32_t i, uint32_t j) {
            int64_t value = 0;
            for (uint32_t k = 0; k < 3; ++k) {
                const int64_t scaled = FxMulShiftS64(PxGet(PxMatRow(rotation, i), k), PxGet(inertia, k), PX_UNIT_SHIFT);
                value += FxMulShiftS64(scaled, PxGet(PxMatRow(rotation, j), k), PX_UNIT_SHIFT);
            }

            return value;
        }

        PxMat3 WorldInertia(PxMat3 rotation, PxVec3 inertia) {
            const auto row = [&](uint32_t i) {
                return PxMakeVec3(WorldInertiaEntry(rotation, inertia, i, 0),
                                  WorldInertiaEntry(rotation, inertia, i, 1),
                                  WorldInertiaEntry(rotation, inertia, i, 2));
            };

            return {.row0 = row(0), .row1 = row(1), .row2 = row(2)};
        }

        // 前の刻みの同じ特徴の点から λ・硬さ・静止摩擦の接触点を引き継ぐ
        void InheritFromPrevious(PhysicsContactPoint& point, const PhysicsManifold& old) {
            for (uint32_t j = 0; j < old.count; ++j) {
                const PhysicsContactPoint& source = old.points[j];
                if (source.feature != point.feature)
                    continue;

                point.rows = source.rows;
                point.stick = source.stick;
                if (!point.stick)
                    continue;

                point.localA = source.localA;
                point.localB = source.localB;
            }
        }

        // 硬さを γ 倍して組の下限〜上限に収める(λ はそのまま)
        void DecayPenalties(PhysicsContactPoint& point, int64_t gammaQ16, int64_t low, int64_t high) {
            for (PxRow& row : point.rows)
                row.penalty = PxClamp(FxMulShiftS64(row.penalty, gammaQ16, 16), low, high);
        }

    }  // namespace

    PhysicsWorld::PhysicsWorld(const PhysicsScene& scene, const PxParameters& parameters)
        : m_scene(scene), m_parameters(parameters), m_rate(PxStepRate(parameters)) {
        for (const PhysicsSceneBody& source : scene.bodies) {
            PhysicsBody body;
            body.halfExtent = PxMakeVec3(source.halfExtent[0], source.halfExtent[1], source.halfExtent[2]);
            body.massMilligrams = source.massMilligrams;
            body.massOverH2 = PxMassOverStepSquared(source.massMilligrams, m_rate);

            const PxVec3 h = body.halfExtent;
            body.inertiaOverH2 = PxMakeVec3(PxBoxInertiaOverStepSquared(body.massMilligrams, h.y, h.z, m_rate),
                                            PxBoxInertiaOverStepSquared(body.massMilligrams, h.x, h.z, m_rate),
                                            PxBoxInertiaOverStepSquared(body.massMilligrams, h.x, h.y, m_rate));
            body.boundingRadius = (int64_t)PxLength(h);
            body.position = PxMakeVec3(source.position[0], source.position[1], source.position[2]);
            body.rotation = PxQuatNormalize(
                PxMakeQuat(source.rotation[0], source.rotation[1], source.rotation[2], source.rotation[3]));
            m_bodies.push_back(body);
        }

        m_bodyManifolds.resize(m_bodies.size());
    }

    PxBox PhysicsWorld::ShapeOf(const PhysicsBody& body) const {
        return {.center = body.position, .rotation = PxRotationMatrix(body.rotation), .halfExtent = body.halfExtent};
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

            const int64_t alphaQ16 = iteration < iterations ? ALPHA_ONE_Q16 : 0;
            for (int32_t color = 0; color < m_colorCount; ++color)
                SolveColor(color, alphaQ16);

            if (iteration < iterations)
                UpdateDuals(alphaQ16);

            if (iteration + 1 == iterations)
                UpdateVelocities();
        }

        Finish();
    }

    // 同じ色の物は拘束を共有しないので、どの順に解いても結果は同じ(GPU では並列に解く)
    void PhysicsWorld::SolveColor(int32_t color, int64_t alphaQ16) {
        for (uint32_t i = 0; i < m_bodies.size(); ++i) {
            if (m_bodies[i].color == color)
                SolveBody(i, alphaQ16);
        }
    }

    void PhysicsWorld::UpdateActivity() {
        for (size_t i = 0; i < m_bodies.size(); ++i) {
            const PhysicsSceneBody& source = m_scene.bodies[i];
            m_bodies[i].active = m_tick >= source.spawnTick && m_tick < source.removeTick;
        }
    }

    void PhysicsWorld::UpdateContacts() {
        std::map<std::pair<uint32_t, uint32_t>, PhysicsManifold> next;
        const auto count = (uint32_t)m_bodies.size();

        for (uint32_t a = 0; a < count; ++a) {
            for (uint32_t b = a + 1; b < count; ++b) {
                const PhysicsBody& bodyA = m_bodies[a];
                const PhysicsBody& bodyB = m_bodies[b];
                if (!bodyA.active || !bodyB.active || (!bodyA.IsDynamic() && !bodyB.IsDynamic()))
                    continue;

                // 速さぶん広げた余裕(先読みの接触)。角速度(2^-20 rad/s)× 半径(2^-20 m)/ 2^20 = 2^-20 m/s
                const int64_t spinA = FxMulShiftS64((int64_t)PxLength(bodyA.angularVelocity), bodyA.boundingRadius, 20);
                const int64_t spinB = FxMulShiftS64((int64_t)PxLength(bodyB.angularVelocity), bodyB.boundingRadius, 20);
                const int64_t relativeSpeed = (int64_t)PxLength(PxSub(bodyA.velocity, bodyB.velocity)) + spinA + spinB;
                const int64_t margin = m_parameters.collisionMargin + relativeSpeed / m_rate;
                if ((int64_t)PxLength(PxSub(bodyA.position, bodyB.position)) >
                    bodyA.boundingRadius + bodyB.boundingRadius + margin)
                    continue;

                const PxContactGeometry geometry = PxCollideBoxes(ShapeOf(bodyA), ShapeOf(bodyB), margin);
                if (geometry.count > 0)
                    AddManifold(a, b, geometry, next);
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

    // 接触を作る(前の刻みの同じ組・同じ特徴の点から引き継ぐ)
    void PhysicsWorld::AddManifold(uint32_t a, uint32_t b, const PxContactGeometry& geometry,
                                   std::map<std::pair<uint32_t, uint32_t>, PhysicsManifold>& next) {
        const PhysicsBody& bodyA = m_bodies[a];
        const PhysicsBody& bodyB = m_bodies[b];
        const uint64_t pairMass = std::max(bodyA.massMilligrams, bodyB.massMilligrams);
        const int64_t pairMassOverH2 = PxMassOverStepSquared(pairMass, m_rate);

        PhysicsManifold manifold{.bodyA = a, .bodyB = b, .normal = geometry.normal, .frictionQ16 = m_scene.frictionQ16};
        manifold.minPenalty = std::max(m_parameters.penaltyMin,
                                       FxMulShiftS64(pairMassOverH2, m_parameters.startPenaltyQ16, 16));

        // 上限: 組の軽い方(動く物)の M/h² × 2^penaltyRatioShift(その物の 6×6 の条件数がこれで抑えられる)
        const uint64_t lighterMass = !bodyA.IsDynamic()   ? bodyB.massMilligrams
                                     : !bodyB.IsDynamic() ? bodyA.massMilligrams
                                                          : std::min(bodyA.massMilligrams, bodyB.massMilligrams);
        const int64_t ratioCap = PxMassOverStepSquared(lighterMass, m_rate) << m_parameters.penaltyRatioShift;
        manifold.maxPenalty = std::max(manifold.minPenalty, std::min(m_parameters.penaltyMax, ratioCap));

        const auto previous = m_manifolds.find({a, b});
        const PxMat3 rotationA = PxRotationMatrix(bodyA.rotation);
        const PxMat3 rotationB = PxRotationMatrix(bodyB.rotation);

        for (uint32_t k = 0; k < geometry.count; ++k) {
            const PxContactPointGeometry& source = geometry.points[k];
            PhysicsContactPoint point{
                .feature = source.feature,
                .normal = geometry.normal,
                .localA = PxMulMatTransposed(rotationA, PxSub(source.pointA, bodyA.position), PX_UNIT_SHIFT),
                .localB = PxMulMatTransposed(rotationB, PxSub(source.pointB, bodyB.position), PX_UNIT_SHIFT)};
            for (PxRow& row : point.rows)
                row.penalty = manifold.minPenalty;

            m_stats.maxPenetration = std::max(m_stats.maxPenetration, -source.separation);

            if (previous != m_manifolds.end())
                InheritFromPrevious(point, previous->second);

            manifold.points[manifold.count++] = point;
        }

        if (previous != m_manifolds.end())
            MatchByProximity(bodyA, previous->second, manifold);

        m_stats.contactCount += manifold.count;
        next.emplace(std::pair{a, b}, manifold);
    }

    // 特徴の番号が合わなかった点(切り抜きの境界にかかった頂点・4 点の選び方が変わった)は、
    // 特徴で使われなかった前の点のうち一番近いもの(proximityMatch 以内・法線が近い)から λ と硬さを引き継ぐ(1 対 1)。
    // 静止している山で、荷重のかかった点の λ が刻みごとに 0 に戻って揺れ続けるのを防ぐ(T-0091)
    void PhysicsWorld::MatchByProximity(const PhysicsBody& bodyA, const PhysicsManifold& old,
                                        PhysicsManifold& manifold) const {
        constexpr int64_t NORMAL_SIMILARITY = 1020054733;  // 0.95(Q1.30)
        std::array<bool, 8> usedOld{};
        std::array<bool, 8> matchedNew{};
        for (uint32_t k = 0; k < manifold.count; ++k) {
            for (uint32_t j = 0; j < old.count; ++j) {
                if (old.points[j].feature != manifold.points[k].feature)
                    continue;

                usedOld[j] = true;
                matchedNew[k] = true;
            }
        }

        const PxMat3 rotationA = PxRotationMatrix(bodyA.rotation);
        for (uint32_t k = 0; k < manifold.count; ++k) {
            if (matchedNew[k])
                continue;

            PhysicsContactPoint& point = manifold.points[k];
            const PxVec3 here = PxMulMat(rotationA, point.localA, PX_UNIT_SHIFT);
            int32_t nearestIndex = -1;
            int64_t nearest = m_parameters.proximityMatch;
            for (uint32_t j = 0; j < old.count; ++j) {
                const PhysicsContactPoint& source = old.points[j];
                const auto distance = (int64_t)PxLength(PxSub(here, PxMulMat(rotationA, source.localA, PX_UNIT_SHIFT)));
                const bool similar = PxDot(source.normal, point.normal, PX_UNIT_SHIFT) >= NORMAL_SIMILARITY;
                if (usedOld[j] || distance >= nearest || !similar)
                    continue;

                nearest = distance;
                nearestIndex = (int32_t)j;
            }

            if (nearestIndex < 0)
                continue;

            usedOld[nearestIndex] = true;
            for (uint32_t r = 0; r < 3; ++r) {
                point.rows[r].lambda = old.points[nearestIndex].rows[r].lambda;
                point.rows[r].penalty = old.points[nearestIndex].rows[r].penalty;
            }
        }
    }

    // 硬さの増え方 β = βkg × 組の両方に触れている物の中で一番重い質量。
    // 軽い物どうし・軽い物と地面の組も、上に重い物が載れば速く硬くなる(重い物に押されて潰れるのを少ない反復で止める。T-0091)
    void PhysicsWorld::UpdateBetas() {
        const auto massOf = [&](uint32_t index) {
            return m_bodies[index].IsDynamic() ? m_bodies[index].massMilligrams : (uint64_t)0;
        };
        std::vector<uint64_t> touchingMass(m_bodies.size());
        for (uint32_t i = 0; i < m_bodies.size(); ++i)
            touchingMass[i] = massOf(i);

        for (const auto& [key, manifold] : m_manifolds) {
            touchingMass[manifold.bodyA] = std::max(touchingMass[manifold.bodyA], massOf(manifold.bodyB));
            touchingMass[manifold.bodyB] = std::max(touchingMass[manifold.bodyB], massOf(manifold.bodyA));
        }

        for (auto& [key, manifold] : m_manifolds) {
            const uint64_t mass = std::max(touchingMass[manifold.bodyA], touchingMass[manifold.bodyB]);
            manifold.beta = (int64_t)(mass / 1000000) * m_parameters.betaPerKilogram +
                            (int64_t)(mass % 1000000) * m_parameters.betaPerKilogram / 1000000;
        }
    }

    // 1 刻みの今の動き(2^-32 m): |Δ| + |Δθ| × 外接球の半径
    int64_t PhysicsWorld::StepMotion(const PhysicsBody& body) const {
        if (!body.IsDynamic())
            return 0;

        const auto turn = FxMulShiftS64((int64_t)PxLength(body.deltaAngular), body.boundingRadius, 20);
        return (int64_t)PxLength(body.deltaLinear) + turn;
    }

    // 反復の途中で、今の推定の姿勢(刻みの初め + Δ)で接触を探し直し、まだ無い特徴の点を足す(回って当たる角・跳ね返りで当たる面)。
    // 組は増やさない(彩色が変わるため)。止まっている組(1 刻みの動きが recollideMinMotion 以下)は探さない(静止の釣り合いを崩さない)
    void PhysicsWorld::RecollideMidStep() {
        for (auto& [key, manifold] : m_manifolds) {
            const int64_t motion = StepMotion(m_bodies[manifold.bodyA]) + StepMotion(m_bodies[manifold.bodyB]);
            if (motion > m_parameters.recollideMinMotion)
                AddRecollidedPoints(manifold);
        }
    }

    // 今の推定の姿勢で接触を作り、まだ無い特徴の点を足す(刻みの初めの姿勢で線形化する。ほかの点と同じ式)
    void PhysicsWorld::AddRecollidedPoints(PhysicsManifold& manifold) {
        const int64_t divisor = (int64_t)1 << PX_DELTA_EXTRA_BITS;
        const auto pose = [&](const PhysicsBody& body) {
            if (!body.IsDynamic())
                return ShapeOf(body);

            const PxVec3 step = PxMakeVec3(body.deltaLinear.x / divisor, body.deltaLinear.y / divisor,
                                           body.deltaLinear.z / divisor);
            const PxQuat rotation = PxIntegrateRotation(body.startRotation, body.deltaAngular);
            return PxBox{.center = PxAdd(body.startPosition, step),
                         .rotation = PxRotationMatrix(rotation),
                         .halfExtent = body.halfExtent};
        };
        const PxBox shapeA = pose(m_bodies[manifold.bodyA]);
        const PxBox shapeB = pose(m_bodies[manifold.bodyB]);
        const PxContactGeometry geometry = PxCollideBoxes(shapeA, shapeB, m_parameters.collisionMargin);

        for (uint32_t k = 0; k < geometry.count && manifold.count < manifold.points.size(); ++k) {
            const PxContactPointGeometry& source = geometry.points[k];
            const auto first = manifold.points.begin();
            const bool known = std::any_of(first, first + manifold.count, [&](const PhysicsContactPoint& point) {
                return point.feature == source.feature;
            });
            if (known)
                continue;

            PhysicsContactPoint point{
                .feature = source.feature,
                .normal = geometry.normal,
                .localA = PxMulMatTransposed(shapeA.rotation, PxSub(source.pointA, shapeA.center), PX_UNIT_SHIFT),
                .localB = PxMulMatTransposed(shapeB.rotation, PxSub(source.pointB, shapeB.center), PX_UNIT_SHIFT)};
            for (PxRow& row : point.rows)
                row.penalty = manifold.minPenalty;

            LinearizePoint(manifold, point);
            manifold.points[manifold.count++] = point;
            m_stats.contactCount += 1;
        }
    }

    // λ はそのまま、硬さは γ 倍(組の下限〜上限に収める)
    void PhysicsWorld::WarmStart() {
        for (auto& [key, manifold] : m_manifolds) {
            for (uint32_t k = 0; k < manifold.count; ++k)
                DecayPenalties(manifold.points[k], m_parameters.gammaQ16, manifold.minPenalty, manifold.maxPenalty);
        }
    }

    void PhysicsWorld::InitializeBodies() {
        const int64_t rate = m_rate;
        // 重力の 1 小刻みの変位 g h²(2^-32 m)= g(2^-20 m/s²)× 2^12 / rate²
        const int64_t gravityStep = (PX_GRAVITY << PX_DELTA_EXTRA_BITS) / (rate * rate);

        for (PhysicsBody& body : m_bodies) {
            body.startPosition = body.position;
            body.startRotation = body.rotation;
            body.deltaLinear = PxMakeVec3(0, 0, 0);
            body.deltaAngular = PxMakeVec3(0, 0, 0);
            body.color = -1;
            if (!body.IsDynamic() || !body.active)
                continue;

            body.inertiaWorld = WorldInertia(PxRotationMatrix(body.rotation), body.inertiaOverH2);

            // v h(2^-32 m)= v(2^-20 m/s)× 2^12 / rate
            const PxVec3 velocityStep = PxMakeVec3((body.velocity.x << PX_DELTA_EXTRA_BITS) / rate,
                                                   (body.velocity.y << PX_DELTA_EXTRA_BITS) / rate,
                                                   (body.velocity.z << PX_DELTA_EXTRA_BITS) / rate);
            body.inertialLinear = PxMakeVec3(velocityStep.x, velocityStep.y - gravityStep, velocityStep.z);
            body.inertialAngular = PxMakeVec3((body.angularVelocity.x << PX_DELTA_EXTRA_BITS) / rate,
                                              (body.angularVelocity.y << PX_DELTA_EXTRA_BITS) / rate,
                                              (body.angularVelocity.z << PX_DELTA_EXTRA_BITS) / rate);

            // 初めの推定: 前の刻みで重力の向きに加速していた割合(Q16)だけ重力を入れる(適応的なウォームスタート)
            const int64_t accelerationY = (body.velocity.y - body.previousVelocity.y) * rate;  // 2^-20 m/s²
            const int64_t weightQ16 = PxClamp(FxDivShiftS64(-accelerationY, PX_GRAVITY, 16), 0, 65536);
            body.deltaLinear = PxMakeVec3(velocityStep.x, velocityStep.y - FxMulShiftS64(gravityStep, weightQ16, 16),
                                          velocityStep.z);
            body.deltaAngular = body.inertialAngular;
        }
    }

    void PhysicsWorld::Linearize() {
        for (auto& [key, manifold] : m_manifolds) {
            for (uint32_t k = 0; k < manifold.count; ++k)
                LinearizePoint(manifold, manifold.points[k]);
        }
    }

    // 刻みの初めの姿勢で 1 点を線形化する(法線は点ごと)
    void PhysicsWorld::LinearizePoint(const PhysicsManifold& manifold, PhysicsContactPoint& point) const {
        const PhysicsBody& a = m_bodies[manifold.bodyA];
        const PhysicsBody& b = m_bodies[manifold.bodyB];
        const auto tangents = TangentBasis(point.normal);
        const std::array<PxVec3, 3> basis{point.normal, tangents[0], tangents[1]};
        const PxVec3 armA = PxMulMat(PxRotationMatrix(a.startRotation), point.localA, PX_UNIT_SHIFT);
        const PxVec3 armB = PxMulMat(PxRotationMatrix(b.startRotation), point.localB, PX_UNIT_SHIFT);
        const PxVec3 gap = PxSub(PxAdd(b.startPosition, armB), PxAdd(a.startPosition, armA));
        for (uint32_t row = 0; row < 3; ++row)
            point.rows[row] = PxLinearizeRow(point.rows[row], basis[row], armA, armB, gap);
    }

    int64_t PhysicsWorld::ConstraintValue(const PhysicsManifold& manifold, const PxRow& row, bool isNormal,
                                          int64_t alphaQ16) const {
        const PhysicsBody& a = m_bodies[manifold.bodyA];
        const PhysicsBody& b = m_bodies[manifold.bodyB];
        return PxConstraintValue(row, isNormal, alphaQ16, m_parameters.gapSlop, a.deltaLinear, a.deltaAngular,
                                 b.deltaLinear, b.deltaAngular);
    }

    // 拘束でつながる物どうしが違う色になるように塗る(Jones-Plassmann: 番号のハッシュを優先度に。GPU でも同じ色になる)
    void PhysicsWorld::ColorBodies() {
        const std::vector<std::vector<uint32_t>> neighbors = DynamicNeighbors();
        m_colorCount = 0;
        for (;;) {
            const std::vector<uint32_t> chosen = ChooseLocalMaxima(neighbors);
            if (chosen.empty())
                break;

            for (uint32_t i : chosen)
                AssignSmallestColor(i, neighbors[i]);
        }

        m_stats.colorCount = std::max(m_stats.colorCount, (uint32_t)m_colorCount);
    }

    // 動く物どうしの接触でつながる隣
    std::vector<std::vector<uint32_t>> PhysicsWorld::DynamicNeighbors() const {
        std::vector<std::vector<uint32_t>> neighbors(m_bodies.size());
        for (const auto& [key, manifold] : m_manifolds) {
            if (!m_bodies[manifold.bodyA].IsDynamic() || !m_bodies[manifold.bodyB].IsDynamic())
                continue;

            neighbors[manifold.bodyA].push_back(manifold.bodyB);
            neighbors[manifold.bodyB].push_back(manifold.bodyA);
        }

        return neighbors;
    }

    // まだ色の無い物のうち、色の無い隣のどれよりも優先度が高い物(同じ回で選ばれた物どうしは隣り合わない)
    std::vector<uint32_t> PhysicsWorld::ChooseLocalMaxima(const std::vector<std::vector<uint32_t>>& neighbors) const {
        const auto priority = [](uint32_t index) {
            return std::pair{FxMix64((uint64_t)index + 1), index};
        };
        std::vector<uint32_t> chosen;
        for (uint32_t i = 0; i < m_bodies.size(); ++i) {
            const PhysicsBody& body = m_bodies[i];
            if (!body.IsDynamic() || !body.active || body.color >= 0)
                continue;

            const auto isBelow = [&](uint32_t j) {
                return m_bodies[j].color >= 0 || priority(j) < priority(i);
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

    void PhysicsWorld::SolveBody(uint32_t index, int64_t alphaQ16) {
        PhysicsBody& body = m_bodies[index];
        PxBodySystem system = PxBeginBodySystem(body.massOverH2, body.inertiaWorld,
                                                PxSub(body.deltaLinear, body.inertialLinear),
                                                PxSub(body.deltaAngular, body.inertialAngular));
        for (const auto& [manifold, isA] : m_bodyManifolds[index])
            system = AddContactRows(system, *manifold, isA, alphaQ16);

        RecordSystemBits(system);
        const PxSolveResult solved = PxSolveSymmetric6(system.lhs, system.rhs, PX_SOLVE_GAIN_SHIFT);
        m_stats.solveBits = std::max(m_stats.solveBits, solved.maxBits);
        m_stats.solveFailures += solved.ok ? 0 : 1;

        body.deltaLinear = PxSub(body.deltaLinear, PxMakeVec3(solved.x.v[0], solved.x.v[1], solved.x.v[2]));
        body.deltaAngular = PxSub(body.deltaAngular, PxMakeVec3(solved.x.v[3], solved.x.v[4], solved.x.v[5]));
        for (uint32_t i = 0; i < 3; ++i) {
            m_stats.deltaBits = std::max(m_stats.deltaBits, BitsOf(PxGet(body.deltaLinear, i)));
            m_stats.deltaBits = std::max(m_stats.deltaBits, BitsOf(PxGet(body.deltaAngular, i)));
        }
    }

    // 1 つの接触の全部の行を、物の 6×6 に足す(物が A なら法線の向きを逆に)
    PxBodySystem PhysicsWorld::AddContactRows(PxBodySystem system, const PhysicsManifold& manifold, bool isA,
                                              int64_t alphaQ16) const {
        for (uint32_t k = 0; k < manifold.count; ++k) {
            const PhysicsContactPoint& point = manifold.points[k];
            const int64_t frictionBound = FxMulShiftS64(manifold.frictionQ16, PxAbs(point.rows[0].lambda), 16);

            for (uint32_t r = 0; r < 3; ++r) {
                const PxRow& row = point.rows[r];
                const int64_t lower = r == 0 ? PX_MOST_NEGATIVE : -frictionBound;
                const int64_t upper = r == 0 ? 0 : frictionBound;
                const int64_t force = PxRowForce(row, ConstraintValue(manifold, row, r == 0, alphaQ16), lower, upper);
                const PxVec3 linear = isA ? PxNegate(row.direction) : row.direction;
                system = PxAddRow(system, linear, isA ? row.angularA : row.angularB, row.penalty, force);
            }
        }

        return system;
    }

    void PhysicsWorld::RecordSystemBits(const PxBodySystem& system) {
        for (int64_t value : system.lhs.m)
            m_stats.lhsBits = std::max(m_stats.lhsBits, BitsOf(value));

        for (int64_t value : system.rhs.v)
            m_stats.rhsBits = std::max(m_stats.rhsBits, BitsOf(value));
    }

    void PhysicsWorld::UpdateDuals(int64_t alphaQ16) {
        for (auto& [key, manifold] : m_manifolds) {
            for (uint32_t k = 0; k < manifold.count; ++k)
                UpdatePointDuals(manifold.points[k], manifold, alphaQ16);
        }
    }

    // λ = clamp(k C + λ)。上下限に張り付いていなければ硬さを β|C| だけ上げる。静止摩擦の判定も
    void PhysicsWorld::UpdatePointDuals(PhysicsContactPoint& point, const PhysicsManifold& manifold, int64_t alphaQ16) {
        const int64_t frictionBound = FxMulShiftS64(manifold.frictionQ16, PxAbs(point.rows[0].lambda), 16);
        for (uint32_t r = 0; r < 3; ++r) {
            PxRow& row = point.rows[r];
            const int64_t lower = r == 0 ? PX_MOST_NEGATIVE : -frictionBound;
            const int64_t upper = r == 0 ? 0 : frictionBound;
            const int64_t c = ConstraintValue(manifold, row, r == 0, alphaQ16);
            row.lambda = PxRowForce(row, c, lower, upper);
            if (row.lambda > lower && row.lambda < upper) {
                const int64_t grown = row.penalty + FxMulShiftS64(manifold.beta, PxAbs(c), PX_FORCE_FROM_PENALTY_SHIFT);
                row.penalty = PxMin(grown, manifold.maxPenalty);
            }

            m_stats.penaltyBits = std::max(m_stats.penaltyBits, BitsOf(row.penalty));
            m_stats.lambdaBits = std::max(m_stats.lambdaBits, BitsOf(row.lambda));
        }

        const uint64_t drift = PxLength(PxMakeVec3(point.rows[1].c0, point.rows[2].c0, 0));
        point.stick = PxAbs(point.rows[1].lambda) < frictionBound && PxAbs(point.rows[2].lambda) < frictionBound &&
                      (int64_t)drift < m_parameters.stickThreshold;
    }

    // 速度 = Δ / h(2^-20 m/s = 2^-32 m × rate / 2^12)
    void PhysicsWorld::UpdateVelocities() {
        const int64_t divisor = (int64_t)1 << PX_DELTA_EXTRA_BITS;
        for (PhysicsBody& body : m_bodies) {
            if (!body.IsDynamic() || !body.active)
                continue;

            body.previousVelocity = body.velocity;
            body.velocity = PxMakeVec3(body.deltaLinear.x * m_rate / divisor, body.deltaLinear.y * m_rate / divisor,
                                       body.deltaLinear.z * m_rate / divisor);
            body.angularVelocity = PxMakeVec3(body.deltaAngular.x * m_rate / divisor,
                                              body.deltaAngular.y * m_rate / divisor,
                                              body.deltaAngular.z * m_rate / divisor);
        }
    }

    void PhysicsWorld::Finish() {
        const int64_t divisor = (int64_t)1 << PX_DELTA_EXTRA_BITS;
        for (PhysicsBody& body : m_bodies) {
            if (!body.IsDynamic() || !body.active)
                continue;

            const PxVec3 step = PxMakeVec3(body.deltaLinear.x / divisor, body.deltaLinear.y / divisor,
                                           body.deltaLinear.z / divisor);
            body.position = PxAdd(body.startPosition, step);
            body.rotation = PxIntegrateRotation(body.startRotation, body.deltaAngular);
        }
    }

    uint64_t PhysicsWorld::StateHash() const {
        uint64_t hash = FxMix64(m_tick);
        const auto add = [&](int64_t value) {
            hash = FxHashCombine(hash, (uint64_t)value);
        };
        for (const PhysicsBody& body : m_bodies) {
            add(body.position.x);
            add(body.position.y);
            add(body.position.z);
            add(body.rotation.x);
            add(body.rotation.y);
            add(body.rotation.z);
            add(body.rotation.w);
            add(body.velocity.x);
            add(body.velocity.y);
            add(body.velocity.z);
            add(body.angularVelocity.x);
            add(body.angularVelocity.y);
            add(body.angularVelocity.z);
        }

        return hash;
    }

}  // namespace bicameral::sim
