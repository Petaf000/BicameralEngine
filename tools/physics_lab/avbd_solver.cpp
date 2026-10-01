// avbd_solver.cpp — 剛体の AVBD の試作(avbd_solver.h)。手順は Giles ら(SIGGRAPH 2025)の AVBD と、その公開の 2D の実装の形に沿う:
// 接触の生成 → λ と硬さの引き継ぎ → 物の慣性の目標 → [物ごとの 6×6 の解(色の順)→ λ と硬さの更新] × 反復 → 速度 → 位置の仕上げ。
#include "avbd_solver.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bicameral::lab {

    namespace {

        constexpr double METER = 1 << 20;
        constexpr double ROTATION_ONE = 1 << 30;
        constexpr double INFINITE_FORCE = std::numeric_limits<double>::infinity();

        Vec3 ToMeters(const std::array<int64_t, 3>& value) {
            return {(double)value[0] / METER, (double)value[1] / METER, (double)value[2] / METER};
        }

        uint64_t Mix(uint64_t value) {
            value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
            value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
            return value ^ (value >> 31);
        }

        // 法線に直交する 2 本(法線の一番小さい成分の軸との外積から。決まった選び方)
        std::array<Vec3, 2> TangentBasis(Vec3 normal) {
            const Vec3 helper = std::abs(normal.x) <= std::abs(normal.y) && std::abs(normal.x) <= std::abs(normal.z)
                                    ? Vec3{1, 0, 0}
                                    : (std::abs(normal.y) <= std::abs(normal.z) ? Vec3{0, 1, 0} : Vec3{0, 0, 1});
            Vec3 first = Cross(normal, helper);
            first = first * (1 / Length(first));

            return {first, Cross(normal, first)};
        }

        double BoundingRadius(const LabBody& body) {
            return Length(body.halfExtent);
        }

    }  // namespace

    double MechanicalEnergy(const LabBody& body, double gravity) {
        if (!body.IsDynamic() || !body.active)
            return 0;

        const Mat3 rotation = RotationMatrix(body.rotation);
        const Vec3 localSpin = Transpose(rotation) * body.angularVelocity;
        const Vec3 inertia = body.inertiaLocal;
        const double rotational = 0.5 * (inertia.x * localSpin.x * localSpin.x + inertia.y * localSpin.y * localSpin.y +
                                         inertia.z * localSpin.z * localSpin.z);
        const double linear = 0.5 * body.mass * Dot(body.velocity, body.velocity);

        return linear + rotational + body.mass * gravity * body.position.y;
    }

    AvbdSolver::AvbdSolver(const sim::PhysicsScene& scene, const SolverParameters& parameters)
        : m_scene(scene), m_parameters(parameters) {
        for (const sim::PhysicsSceneBody& source : scene.bodies) {
            LabBody body;
            body.halfExtent = ToMeters(source.halfExtent);
            body.mass = (double)source.massMilligrams / 1e6;

            const Vec3 h = body.halfExtent;
            body.inertiaLocal = Vec3{h.y * h.y + h.z * h.z, h.x * h.x + h.z * h.z, h.x * h.x + h.y * h.y} *
                                (body.mass / 3);
            body.position = ToMeters(source.position);
            body.rotation = Normalize({source.rotation[0] / ROTATION_ONE, source.rotation[1] / ROTATION_ONE,
                                       source.rotation[2] / ROTATION_ONE, source.rotation[3] / ROTATION_ONE});
            m_bodies.push_back(body);
        }

        m_bodyManifolds.resize(m_bodies.size());
    }

    double AvbdSolver::SubstepTime() const {
        return m_parameters.timeStep / m_parameters.substeps;
    }

    BoxShape AvbdSolver::ShapeOf(const LabBody& body) const {
        return {body.position, RotationMatrix(body.rotation), body.halfExtent};
    }

    void AvbdSolver::Step() {
        m_stats = {};
        UpdateActivity();
        for (int i = 0; i < m_parameters.substeps; ++i)
            Substep();

        ++m_tick;
    }

    void AvbdSolver::Substep() {
        UpdateContacts();
        WarmStart();
        InitializeBodies();
        Linearize();
        ColorBodies();

        const int iterations = m_parameters.iterations;
        const int total = iterations + (m_parameters.postStabilize ? 1 : 0);
        for (int iteration = 0; iteration < total; ++iteration) {
            double alpha = m_parameters.alpha;
            if (m_parameters.postStabilize)
                alpha = iteration < iterations ? 1.0 : 0.0;

            for (int color = 0; color < m_colorCount; ++color) {
                for (size_t i = 0; i < m_bodies.size(); ++i) {
                    if (m_bodies[i].color == color)
                        SolveBody((int)i, alpha);
                }
            }

            if (iteration < iterations)
                UpdateDuals(alpha);

            if (iteration != iterations - 1)
                continue;

            for (LabBody& body : m_bodies) {
                if (!body.IsDynamic() || !body.active)
                    continue;

                const Vec3 oldAngular = body.angularVelocity;
                body.previousVelocity = body.velocity;
                body.velocity = body.deltaLinear * (1 / SubstepTime());
                body.angularVelocity = body.deltaAngular * (1 / SubstepTime());

                const Vec3 gravityStep{0, -m_parameters.gravity * SubstepTime(), 0};
                const double kick = Length(body.velocity - body.previousVelocity - gravityStep) +
                                    Length(body.angularVelocity - oldAngular) * BoundingRadius(body);
                if (kick > m_stats.kick) {
                    m_stats.kick = kick;
                    m_stats.kickBody = (int)(&body - m_bodies.data());
                }
            }
        }

        Finish();
    }

    void AvbdSolver::UpdateActivity() {
        for (size_t i = 0; i < m_bodies.size(); ++i) {
            const sim::PhysicsSceneBody& source = m_scene.bodies[i];
            m_bodies[i].active = m_tick >= source.spawnTick && m_tick < source.removeTick;
        }
    }

    void AvbdSolver::UpdateContacts() {
        std::map<std::pair<int, int>, Manifold> next;
        const double h = SubstepTime();
        const int count = (int)m_bodies.size();

        for (int a = 0; a < count; ++a) {
            for (int b = a + 1; b < count; ++b) {
                const LabBody& bodyA = m_bodies[a];
                const LabBody& bodyB = m_bodies[b];
                if (!bodyA.active || !bodyB.active || (!bodyA.IsDynamic() && !bodyB.IsDynamic()))
                    continue;

                // 速さぶん広げた余裕(先読みの接触。速い物が 1 刻みで食い込まないように)
                const double radiusA = BoundingRadius(bodyA);
                const double radiusB = BoundingRadius(bodyB);
                const double relativeSpeed = Length(bodyA.velocity - bodyB.velocity) +
                                             Length(bodyA.angularVelocity) * radiusA +
                                             Length(bodyB.angularVelocity) * radiusB;
                const double margin = m_parameters.collisionMargin + relativeSpeed * h;
                if (Length(bodyA.position - bodyB.position) > radiusA + radiusB + margin)
                    continue;

                ContactGeometry geometry;
                if (!CollideBoxes(ShapeOf(bodyA), ShapeOf(bodyB), margin, geometry))
                    continue;

                const double massA = bodyA.IsDynamic() ? bodyA.mass : 0;
                const double massB = bodyB.IsDynamic() ? bodyB.mass : 0;
                Manifold manifold{.bodyA = a,
                                  .bodyB = b,
                                  .normal = geometry.normal,
                                  .friction = m_scene.frictionQ16 / 65536.0,
                                  .pairMass = std::max(massA, massB)};

                const auto previous = m_manifolds.find({a, b});
                const Mat3 inverseA = Transpose(RotationMatrix(bodyA.rotation));
                const Mat3 inverseB = Transpose(RotationMatrix(bodyB.rotation));
                manifold.minPenalty = std::max(m_parameters.penaltyMin,
                                               m_parameters.startPenaltyScale * manifold.pairMass / (h * h));
                const double startPenalty = manifold.minPenalty;
                const double lighterMass = !bodyA.IsDynamic() ? massB
                                                              : (!bodyB.IsDynamic() ? massA : std::min(massA, massB));
                manifold.maxPenalty = std::max(
                    manifold.minPenalty,
                    std::min(m_parameters.penaltyMax, m_parameters.penaltyRatioMax * lighterMass / (h * h)));

                for (int k = 0; k < geometry.count; ++k) {
                    const ContactPointGeometry& source = geometry.points[k];
                    ContactPoint point{.feature = source.feature,
                                       .localA = inverseA * (source.pointA - bodyA.position),
                                       .localB = inverseB * (source.pointB - bodyB.position)};
                    point.penalty = {startPenalty, startPenalty, startPenalty};
                    if (-source.separation > m_stats.maxPenetration) {
                        m_stats.maxPenetration = -source.separation;
                        m_stats.deepestBodyA = a;
                        m_stats.deepestBodyB = b;
                        m_stats.deepestFeature = source.feature;
                        m_stats.deepestIsNew = previous == m_manifolds.end();
                        m_stats.deepestApproach = -Dot(geometry.normal, bodyB.velocity - bodyA.velocity);
                    }

                    if (previous != m_manifolds.end()) {
                        const Manifold& old = previous->second;
                        for (int j = 0; j < old.count; ++j) {
                            if (old.points[j].feature != point.feature)
                                continue;

                            point.lambda = old.points[j].lambda;
                            point.penalty = old.points[j].penalty;
                            point.stick = old.points[j].stick;
                            if (point.stick) {
                                point.localA = old.points[j].localA;
                                point.localB = old.points[j].localB;
                            }
                        }
                    }

                    manifold.points[manifold.count++] = point;
                }

                m_stats.contactCount += manifold.count;
                next.emplace(std::pair{a, b}, manifold);
            }
        }

        m_manifolds = std::move(next);
        for (auto& list : m_bodyManifolds)
            list.clear();

        for (auto& [key, manifold] : m_manifolds) {
            m_bodyManifolds[manifold.bodyA].emplace_back(&manifold, true);
            m_bodyManifolds[manifold.bodyB].emplace_back(&manifold, false);
        }
    }

    void AvbdSolver::WarmStart() {
        const SolverParameters& p = m_parameters;
        for (auto& [key, manifold] : m_manifolds) {
            for (int k = 0; k < manifold.count; ++k) {
                ContactPoint& point = manifold.points[k];
                for (int row = 0; row < 3; ++row) {
                    if (!p.postStabilize)
                        point.lambda[row] *= p.alpha * p.gamma;

                    point.penalty[row] = std::clamp(point.penalty[row] * p.gamma, manifold.minPenalty,
                                                    manifold.maxPenalty);
                }
            }
        }
    }

    void AvbdSolver::InitializeBodies() {
        const double h = SubstepTime();
        const Vec3 gravity{0, -m_parameters.gravity, 0};

        for (LabBody& body : m_bodies) {
            body.startPosition = body.position;
            body.startRotation = body.rotation;
            body.deltaLinear = {};
            body.deltaAngular = {};
            body.color = -1;
            if (!body.IsDynamic() || !body.active)
                continue;

            const Mat3 rotation = RotationMatrix(body.rotation);
            const Mat3 inertia{
                {Vec3{body.inertiaLocal.x, 0, 0}, Vec3{0, body.inertiaLocal.y, 0}, Vec3{0, 0, body.inertiaLocal.z}}};
            body.inertiaWorld = rotation * inertia * Transpose(rotation);
            body.inertialLinear = body.velocity * h + gravity * (h * h);
            body.inertialAngular = body.angularVelocity * h;

            // 初めの推定: 前の刻みで重力の向きに加速していた割合だけ重力を入れる(AVBD の適応的なウォームスタート)
            const Vec3 acceleration = (body.velocity - body.previousVelocity) * (1 / h);
            const double weight = std::clamp(-acceleration.y / m_parameters.gravity, 0.0, 1.0);
            body.deltaLinear = body.velocity * h + gravity * (h * h * weight);
            body.deltaAngular = body.inertialAngular;
        }
    }

    void AvbdSolver::Linearize() {
        for (auto& [key, manifold] : m_manifolds) {
            const LabBody& a = m_bodies[manifold.bodyA];
            const LabBody& b = m_bodies[manifold.bodyB];
            const Mat3 rotationA = RotationMatrix(a.startRotation);
            const Mat3 rotationB = RotationMatrix(b.startRotation);
            const auto tangents = TangentBasis(manifold.normal);

            for (int k = 0; k < manifold.count; ++k) {
                ContactPoint& point = manifold.points[k];
                const Vec3 armA = rotationA * point.localA;
                const Vec3 armB = rotationB * point.localB;
                const Vec3 gap = (b.startPosition + armB) - (a.startPosition + armA);
                point.basis = {manifold.normal, tangents[0], tangents[1]};

                for (int row = 0; row < 3; ++row) {
                    const Vec3 direction = point.basis[row];
                    point.c0[row] = Dot(direction, gap);
                    point.angularA[row] = -Cross(armA, direction);
                    point.angularB[row] = Cross(armB, direction);
                }
            }
        }
    }

    double AvbdSolver::ConstraintValue(const Manifold& manifold, const ContactPoint& point, int row,
                                       double alpha) const {
        const LabBody& a = m_bodies[manifold.bodyA];
        const LabBody& b = m_bodies[manifold.bodyB];
        const Vec3 direction = point.basis[row];

        // 法線の行で離れている(c0 > 0)ときは、隙間をそのまま使う(先読みの接触。α で消すと、離れた物が触れているように押し合う)。
        // α で弱めるのは食い込みと接線のずれ(刻みの初めにある誤差)だけ
        const bool isGap = row == 0 && point.c0[row] > 0;
        const double initial = isGap ? point.c0[row] : point.c0[row] * (1 - alpha);

        return initial - Dot(direction, a.deltaLinear) + Dot(point.angularA[row], a.deltaAngular) +
               Dot(direction, b.deltaLinear) + Dot(point.angularB[row], b.deltaAngular);
    }

    // 拘束でつながる物どうしが違う色になるように塗る(Jones-Plassmann: 番号のハッシュを優先度に。GPU でも同じ色になる)
    void AvbdSolver::ColorBodies() {
        const int count = (int)m_bodies.size();
        std::vector<std::vector<int>> neighbors(count);
        for (auto& [key, manifold] : m_manifolds) {
            if (!m_bodies[manifold.bodyA].IsDynamic() || !m_bodies[manifold.bodyB].IsDynamic())
                continue;

            neighbors[manifold.bodyA].push_back(manifold.bodyB);
            neighbors[manifold.bodyB].push_back(manifold.bodyA);
        }

        const auto priority = [](int index) {
            return std::pair{Mix((uint64_t)index + 1), index};
        };
        m_colorCount = 0;
        bool remaining = true;
        while (remaining) {
            remaining = false;
            std::vector<int> chosen;
            for (int i = 0; i < count; ++i) {
                const LabBody& body = m_bodies[i];
                if (!body.IsDynamic() || !body.active || body.color >= 0)
                    continue;

                remaining = true;
                const bool isLocalMaximum = std::ranges::all_of(
                    neighbors[i], [&](int j) { return m_bodies[j].color >= 0 || priority(j) < priority(i); });
                if (isLocalMaximum)
                    chosen.push_back(i);
            }

            for (int i : chosen) {
                int color = 0;
                while (std::ranges::any_of(neighbors[i], [&](int j) { return m_bodies[j].color == color; }))
                    ++color;

                m_bodies[i].color = color;
                m_colorCount = std::max(m_colorCount, color + 1);
            }
        }

        m_stats.colorCount = m_colorCount;
    }

    void AvbdSolver::SolveBody(int index, double alpha) {
        LabBody& body = m_bodies[index];
        const double h2 = SubstepTime() * SubstepTime();

        // 慣性の項: M/h² と M/h² (Δ − 慣性の目標)
        Mat6 lhs{};
        Vec6 rhs{};
        const Vec3 linearError = (body.deltaLinear - body.inertialLinear) * (body.mass / h2);
        const Vec3 angularError = body.inertiaWorld * (body.deltaAngular - body.inertialAngular);
        for (int i = 0; i < 3; ++i) {
            lhs[i][i] = body.mass / h2;
            rhs[i] = linearError[i];
            rhs[i + 3] = angularError[i] / h2;
            for (int j = 0; j < 3; ++j)
                lhs[i + 3][j + 3] = body.inertiaWorld.row[i][j] / h2;
        }

        // 拘束の項: J f と k J Jᵀ
        for (const auto& [manifold, isA] : m_bodyManifolds[index]) {
            for (int k = 0; k < manifold->count; ++k) {
                const ContactPoint& point = manifold->points[k];
                const double frictionBound = manifold->friction * std::abs(point.lambda[0]);

                for (int row = 0; row < 3; ++row) {
                    const double lower = row == 0 ? -INFINITE_FORCE : -frictionBound;
                    const double upper = row == 0 ? 0 : frictionBound;
                    const double c = ConstraintValue(*manifold, point, row, alpha);
                    const double unclamped = point.penalty[row] * c + point.lambda[row];
                    const double force = std::clamp(unclamped, lower, upper);
                    const bool isActive = unclamped > lower && unclamped < upper;
                    if (m_parameters.activeHessianOnly && !isActive && force == 0)
                        continue;

                    const Vec3 linear = isA ? -point.basis[row] : point.basis[row];
                    const Vec3 angular = isA ? point.angularA[row] : point.angularB[row];
                    const Vec6 jacobian{linear.x, linear.y, linear.z, angular.x, angular.y, angular.z};

                    for (int i = 0; i < 6; ++i) {
                        rhs[i] += jacobian[i] * force;
                        for (int j = 0; j < 6; ++j)
                            lhs[i][j] += point.penalty[row] * jacobian[i] * jacobian[j];
                    }
                }
            }
        }

        const Vec6 step = SolveSymmetric6(lhs, rhs);
        body.deltaLinear -= Vec3{step[0], step[1], step[2]};
        body.deltaAngular -= Vec3{step[3], step[4], step[5]};
    }

    void AvbdSolver::UpdateDuals(double alpha) {
        const SolverParameters& p = m_parameters;
        for (auto& [key, manifold] : m_manifolds) {
            const double beta = p.betaPerKilogram * manifold.pairMass;
            for (int k = 0; k < manifold.count; ++k) {
                ContactPoint& point = manifold.points[k];
                const double frictionBound = manifold.friction * std::abs(point.lambda[0]);

                for (int row = 0; row < 3; ++row) {
                    const double lower = row == 0 ? -INFINITE_FORCE : -frictionBound;
                    const double upper = row == 0 ? 0 : frictionBound;
                    const double c = ConstraintValue(manifold, point, row, alpha);
                    point.lambda[row] = std::clamp(point.penalty[row] * c + point.lambda[row], lower, upper);
                    if (point.lambda[row] > lower && point.lambda[row] < upper)
                        point.penalty[row] = std::min(point.penalty[row] + beta * std::abs(c), manifold.maxPenalty);
                }

                const double tangentialDrift = std::sqrt(point.c0[1] * point.c0[1] + point.c0[2] * point.c0[2]);
                point.stick = std::abs(point.lambda[1]) < frictionBound && std::abs(point.lambda[2]) < frictionBound &&
                              tangentialDrift < p.stickThreshold;
            }
        }
    }

    void AvbdSolver::Finish() {
        m_stats.energy = 0;
        for (LabBody& body : m_bodies) {
            if (!body.IsDynamic() || !body.active)
                continue;

            body.position = body.startPosition + body.deltaLinear;
            body.rotation = Integrate(body.startRotation, body.deltaAngular);

            const double speed = Length(body.velocity) + Length(body.angularVelocity) * BoundingRadius(body);
            m_stats.maxSpeed = std::max(m_stats.maxSpeed, speed);
            m_stats.energy += MechanicalEnergy(body, m_parameters.gravity);
        }
    }

}  // namespace bicameral::lab
