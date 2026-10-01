// integer_solver.h — 整数の AVBD(sim::PhysicsWorld)を、試作(AvbdSolver)と同じ形で読めるようにする包み(T-0016)。
// physics_lab が同じ場面・同じ判定・同じ書き出しで、double と整数を並べて比べるために使う。
#pragma once

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "avbd_solver.h"
#include "sim/physics_world.h"

namespace bicameral::lab {

    [[nodiscard]] inline physics::PxParameters ToInteger(const SolverParameters& p) {
        physics::PxParameters result = physics::PxDefaultParameters();
        result.iterations = (uint32_t)p.iterations;
        result.substeps = (uint32_t)p.substeps;
        result.gammaQ16 = (int64_t)(p.gamma * 65536 + 0.5);
        result.penaltyMin = (int64_t)(p.penaltyMin * 256);
        result.penaltyMax = (int64_t)(p.penaltyMax * 256);
        result.betaPerKilogram = (int64_t)p.betaPerKilogram;
        result.startPenaltyQ16 = (int64_t)(p.startPenaltyScale * 65536);
        result.collisionMargin = (int64_t)(p.collisionMargin * (1 << 20));
        result.stickThreshold = (int64_t)(p.stickThreshold * 4294967296.0);
        return result;
    }

    class IntegerSolver {
    public:
        IntegerSolver(const sim::PhysicsScene& scene, const SolverParameters& parameters)
            : m_world(scene, ToInteger(parameters)), m_gravity(parameters.gravity) {
            Convert();
        }

        void Step() {
            m_world.Step();
            Convert();

            const sim::PhysicsStepStats& s = m_world.Stats();
            sim::PhysicsStepStats& m = m_maxStats;
            m.solveBits = std::max(m.solveBits, s.solveBits);
            m.lhsBits = std::max(m.lhsBits, s.lhsBits);
            m.rhsBits = std::max(m.rhsBits, s.rhsBits);
            m.penaltyBits = std::max(m.penaltyBits, s.penaltyBits);
            m.lambdaBits = std::max(m.lambdaBits, s.lambdaBits);
            m.deltaBits = std::max(m.deltaBits, s.deltaBits);
            m.solveFailures += s.solveFailures;
        }

        [[nodiscard]] const std::vector<LabBody>& Bodies() const { return m_bodies; }
        [[nodiscard]] const StepStats& Stats() const { return m_stats; }
        [[nodiscard]] uint64_t Tick() const { return m_world.Tick(); }
        [[nodiscard]] const std::map<std::pair<int, int>, Manifold>& Manifolds() const { return m_manifolds; }
        [[nodiscard]] const sim::PhysicsWorld& World() const { return m_world; }
        [[nodiscard]] const sim::PhysicsStepStats& MaxStats() const { return m_maxStats; }

    private:
        void Convert() {
            constexpr double METER = 1 << 20;
            constexpr double ONE = 1 << 30;
            const auto vec = [&](const physics::PxVec3& v, double unit) {
                return Vec3{(double)v.x / unit, (double)v.y / unit, (double)v.z / unit};
            };

            m_bodies.resize(m_world.Bodies().size());
            m_stats = {};
            for (size_t i = 0; i < m_bodies.size(); ++i) {
                const sim::PhysicsBody& source = m_world.Bodies()[i];
                LabBody& body = m_bodies[i];
                body.halfExtent = vec(source.halfExtent, METER);
                body.mass = (double)source.massMilligrams / 1e6;
                const Vec3 h = body.halfExtent;
                body.inertiaLocal = Vec3{h.y * h.y + h.z * h.z, h.x * h.x + h.z * h.z, h.x * h.x + h.y * h.y} *
                                    (body.mass / 3);
                body.position = vec(source.position, METER);
                body.rotation = {(double)source.rotation.x / ONE, (double)source.rotation.y / ONE,
                                 (double)source.rotation.z / ONE, (double)source.rotation.w / ONE};
                body.velocity = vec(source.velocity, METER);
                body.angularVelocity = vec(source.angularVelocity, METER);
                body.active = source.active;
                if (!body.IsDynamic() || !body.active)
                    continue;

                m_stats.maxSpeed = std::max(m_stats.maxSpeed,
                                            Length(body.velocity) + Length(body.angularVelocity) * Length(h));
                m_stats.energy += MechanicalEnergy(body, m_gravity);
            }

            const sim::PhysicsStepStats& s = m_world.Stats();
            m_stats.maxPenetration = (double)s.maxPenetration / METER;
            m_stats.contactCount = (int)s.contactCount;
            m_stats.colorCount = (int)s.colorCount;
        }

        sim::PhysicsWorld m_world;
        double m_gravity = 0;
        std::vector<LabBody> m_bodies;
        StepStats m_stats;
        std::map<std::pair<int, int>, Manifold> m_manifolds;  // 整数版の接触は書き出さない(空)
        sim::PhysicsStepStats m_maxStats;
    };

}  // namespace bicameral::lab
