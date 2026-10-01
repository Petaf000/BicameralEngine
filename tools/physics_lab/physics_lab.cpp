// physics_lab.cpp — 物理の試作(double の AVBD)で T-0016 の場面を走らせ、基準を判定して軌跡を書き出す道具。
// 使い方: physics_lab [--integer] [--scene stack|mass_ratio|pile|all] [--out フォルダ] [--iterations n] [--beta x] [--start-penalty x]
//                      [--alpha x] [--gamma x] [--margin m] [--no-post-stabilize] [--seed n]
//                      [--recollide n(-1 = しない)] [--recollide-min-motion m] [--slop m] [--proximity m](T-0091)
// 書き出し(--out があるとき): <場面>_bodies.csv(0.1 s ごとの位置と向き)・<場面>_stats.csv(刻みごとの量)。
// 画像にするのは Linux 側の tools/physics_lab/plot_lab.py。
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

#include "avbd_solver.h"
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "integer_solver.h"
#include "sim/physics_scene.h"

using namespace bicameral;
using namespace bicameral::lab;

namespace {

    struct LabOptions {
        std::string scene = "all";
        std::string outputDirectory;
        uint64_t seed = 1;
        bool integer = false;           // --integer: 整数の AVBD(sim::PhysicsWorld)で走らせる
        uint64_t hashEvery = 0;         // --hash-every n: 整数のとき n 刻みごとに状態のハッシュを書き出す
        double tracePenetration = 1e9;  // この食い込み(m)を超えた刻みを書き出す
        double traceKick = 1e9;         // この速度の飛び(m/s)を超えた刻みを書き出す
        double traceLateSpeed = 1e9;    // 判定の区間でこの速さ(m/s)を超えた刻みの、一番速い物を書き出す
        int debugBodyA = -1;            // --debug-pair a b t0 t1: この組の接触を刻みごとに書き出す
        int debugBodyB = -1;
        uint64_t debugFirstTick = 0;
        uint64_t debugLastTick = 0;
        SolverParameters parameters;
    };

    // 場面ごとの基準の結果
    struct SceneReport {
        double maxPenetration = 0;
        double maxSpeed = 0;
        double maxSpeedLate = 0;        // 判定の区間の最大の速さ(stack・mass_ratio: 1 s 以降、pile: 最後の 5 s)
        double maxPenetrationLate = 0;  // 判定の区間の最大の食い込み(止まっている時の食い込み)
        double energyExcess = 0;        // 現れた物の位置エネルギーの合計を、力学的エネルギーが超えた最大の量(J)
        double energyTolerance =
            0;  // 許す超過(現れた物の位置エネルギーの 0.1% + 1 J。食い込みを直すと位置エネルギーが少し増える)
        double topDrift = 0;  // 最後の物の水平のずれ(m)
        double topTiltDegrees = 0;
        double topHeight = 0;
        int maxContacts = 0;
        int maxColors = 0;
        double seconds = 0;
    };

    bool ParseOptions(int argc, char** argv, LabOptions& options) {
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            const bool hasValue = i + 1 < argc;
            SolverParameters& p = options.parameters;
            if (argument == "--no-post-stabilize")
                p.postStabilize = false;
            else if (argument == "--integer")
                options.integer = true;
            else if (argument == "--full-hessian")
                p.activeHessianOnly = false;
            else if (!hasValue)
                return false;
            else if (argument == "--scene")
                options.scene = argv[++i];
            else if (argument == "--out")
                options.outputDirectory = argv[++i];
            else if (argument == "--hash-every")
                options.hashEvery = std::stoull(argv[++i]);
            else if (argument == "--seed")
                options.seed = std::stoull(argv[++i]);
            else if (argument == "--substeps")
                p.substeps = std::stoi(argv[++i]);
            else if (argument == "--iterations")
                p.iterations = std::stoi(argv[++i]);
            else if (argument == "--beta")
                p.betaPerKilogram = std::stod(argv[++i]);
            else if (argument == "--start-penalty")
                p.startPenaltyScale = std::stod(argv[++i]);
            else if (argument == "--alpha")
                p.alpha = std::stod(argv[++i]);
            else if (argument == "--gamma")
                p.gamma = std::stod(argv[++i]);
            else if (argument == "--debug-pair" && i + 4 < argc) {
                options.debugBodyA = std::stoi(argv[++i]);
                options.debugBodyB = std::stoi(argv[++i]);
                options.debugFirstTick = std::stoull(argv[++i]);
                options.debugLastTick = std::stoull(argv[++i]);
            } else if (argument == "--trace-late-speed")
                options.traceLateSpeed = std::stod(argv[++i]);
            else if (argument == "--trace-kick")
                options.traceKick = std::stod(argv[++i]);
            else if (argument == "--trace-penetration")
                options.tracePenetration = std::stod(argv[++i]);
            else if (argument == "--margin")
                p.collisionMargin = std::stod(argv[++i]);
            else if (argument == "--recollide")
                p.recollideIteration = std::stoi(argv[++i]);
            else if (argument == "--recollide-min-motion")
                p.recollideMinMotion = std::stod(argv[++i]);
            else if (argument == "--slop")
                p.gapSlop = std::stod(argv[++i]);
            else if (argument == "--proximity")
                p.proximityMatch = std::stod(argv[++i]);
            else
                return false;
        }

        return true;
    }

    // 判定の区間か(stack・mass_ratio は 1 s 以降、pile は最後の 5 s)
    bool InLateWindow(const sim::PhysicsScene& scene, uint64_t tick) {
        if (scene.name == "pile")
            return tick + 5 * 60 >= scene.tickCount;

        return tick >= 60;
    }

    template <class Solver>
    void WriteBodies(std::ofstream& file, const Solver& solver) {
        const auto& bodies = solver.Bodies();
        for (size_t i = 0; i < bodies.size(); ++i) {
            const LabBody& b = bodies[i];
            file << std::format("{},{},{},{:.6f},{:.6f},{:.6f},{:.7f},{:.7f},{:.7f},{:.7f},{:.4f},{:.4f},{:.4f}\n",
                                solver.Tick(), i, b.active ? 1 : 0, b.position.x, b.position.y, b.position.z,
                                b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w, b.halfExtent.x, b.halfExtent.y,
                                b.halfExtent.z);
        }
    }

    template <class Solver>
    void DebugPair(const Solver& solver, const LabOptions& options) {
        const LabBody& a = solver.Bodies()[options.debugBodyA];
        const LabBody& b = solver.Bodies()[options.debugBodyB];
        const auto shape = [](const LabBody& body) {
            return BoxShape{body.position, RotationMatrix(body.rotation), body.halfExtent};
        };
        ContactGeometry contact;
        const bool touching = CollideBoxes(shape(a), shape(b), 0.3, contact);
        std::string points;
        for (int k = 0; k < contact.count; ++k)
            points += std::format(" {:.1f}mm({:08x})", contact.points[k].separation * 1000, contact.points[k].feature);

        Log(Channel::Physics, Level::Info,
            "  刻み {} A p=({:.3f},{:.3f},{:.3f}) v=({:.2f},{:.2f},{:.2f}) w={:.2f} / B p=({:.3f},{:.3f},{:.3f}) "
            "v=({:.2f},{:.2f},{:.2f}) "
            "w={:.2f} / 接触 {} n=({:.2f},{:.2f},{:.2f}){}",
            solver.Tick(), a.position.x, a.position.y, a.position.z, a.velocity.x, a.velocity.y, a.velocity.z,
            Length(a.angularVelocity), b.position.x, b.position.y, b.position.z, b.velocity.x, b.velocity.y,
            b.velocity.z, Length(b.angularVelocity), touching, contact.normal.x, contact.normal.y, contact.normal.z,
            points);
    }

    // 物の接触(相手・点の数・法線の λ・隙間・硬さ)を文字にする
    template <class Solver>
    std::string DescribeContacts(const Solver& solver, int index) {
        std::string partners;
        for (const auto& [key, manifold] : solver.Manifolds()) {
            if (manifold.bodyA != index && manifold.bodyB != index)
                continue;

            const int other = manifold.bodyA == index ? manifold.bodyB : manifold.bodyA;
            partners += std::format(" #{}({:.0f}kg n=({:.2f},{:.2f},{:.2f}) {}点 λ", other, solver.Bodies()[other].mass,
                                    manifold.normal.x, manifold.normal.y, manifold.normal.z, manifold.count);
            for (int k = 0; k < manifold.count; ++k) {
                partners += std::format(" {:.0f}/c0={:.1f}mm/k={:.1e}", manifold.points[k].lambda[0],
                                        manifold.points[k].c0[0] * 1000, manifold.points[k].penalty[0]);
            }

            partners += ")";
        }

        return partners;
    }

    std::string DescribeContacts(const IntegerSolver& solver, int index) {
        std::string partners;
        for (const auto& [key, manifold] : solver.World().Manifolds()) {
            if ((int)manifold.bodyA != index && (int)manifold.bodyB != index)
                continue;

            const int other = (int)manifold.bodyA == index ? (int)manifold.bodyB : (int)manifold.bodyA;
            partners += std::format(" #{}({:.0f}kg n=({:.2f},{:.2f},{:.2f}) {}点", other, solver.Bodies()[other].mass,
                                    manifold.normal.x / 1073741824.0, manifold.normal.y / 1073741824.0,
                                    manifold.normal.z / 1073741824.0, manifold.count);
            for (uint32_t k = 0; k < manifold.count; ++k) {
                const auto& rows = manifold.points[k].rows;
                partners += std::format(" λ{:.0f}/{:.0f}/{:.0f} c0={:.2f}mm k={:.1e}{}", rows[0].lambda / 65536.0,
                                        rows[1].lambda / 65536.0, rows[2].lambda / 65536.0,
                                        rows[0].c0 / 4294967296.0 * 1000, rows[0].penalty / 256.0,
                                        manifold.points[k].stick ? "s" : "");
            }

            partners += ")";
        }

        return partners;
    }

    // 速度が飛んだ物と、その物の接触を書き出す
    template <class Solver>
    void TraceKick(const Solver& solver) {
        const StepStats& stats = solver.Stats();
        const LabBody& body = solver.Bodies()[stats.kickBody];
        Log(Channel::Physics, Level::Info,
            "  刻み {} 飛び {:.2f} m/s: #{}({:.1f} kg) v=({:.2f},{:.2f},{:.2f}) w={:.2f}:{}", solver.Tick(), stats.kick,
            stats.kickBody, body.mass, body.velocity.x, body.velocity.y, body.velocity.z, Length(body.angularVelocity),
            DescribeContacts(solver, stats.kickBody));
    }

    // 最後に一番速く動いている物(止まりきらない物)を書き出す
    template <class Solver>
    void TraceFastestAtEnd(const Solver& solver) {
        int fastest = -1;
        double fastestSpeed = 0;
        for (size_t i = 0; i < solver.Bodies().size(); ++i) {
            const LabBody& body = solver.Bodies()[i];
            const double speed = Length(body.velocity) + Length(body.angularVelocity) * Length(body.halfExtent);
            if (body.IsDynamic() && body.active && speed > fastestSpeed) {
                fastestSpeed = speed;
                fastest = (int)i;
            }
        }

        if (fastest < 0)
            return;

        const LabBody& body = solver.Bodies()[fastest];
        Log(Channel::Physics, Level::Info,
            "  刻み {} 一番速い物 #{}({:.1f} kg、{:.2f}×{:.2f}×{:.2f}) v=({:.4f},{:.4f},{:.4f}) "
            "w=({:.4f},{:.4f},{:.4f}):{}",
            solver.Tick(), fastest, body.mass, body.halfExtent.x * 2, body.halfExtent.y * 2, body.halfExtent.z * 2,
            body.velocity.x, body.velocity.y, body.velocity.z, body.angularVelocity.x, body.angularVelocity.y,
            body.angularVelocity.z, DescribeContacts(solver, fastest));
    }

    template <class Solver>
    SceneReport RunScene(const sim::PhysicsScene& scene, const LabOptions& options) {
        Solver solver(scene, options.parameters);
        SceneReport report;
        std::ofstream bodiesFile;
        std::ofstream statsFile;
        if (!options.outputDirectory.empty()) {
            fs::create_directories(options.outputDirectory);
            bodiesFile.open(fs::path(options.outputDirectory) / (scene.name + "_bodies.csv"));
            statsFile.open(fs::path(options.outputDirectory) / (scene.name + "_stats.csv"));
            bodiesFile << "tick,body,active,px,py,pz,qx,qy,qz,qw,hx,hy,hz\n";
            statsFile << "tick,max_penetration,max_speed,energy,spawned_energy,contacts,colors\n";
            WriteBodies(bodiesFile, solver);
        }

        const auto start = chr::steady_clock::now();
        double spawnedEnergy = 0;
        for (uint64_t tick = 0; tick < scene.tickCount; ++tick) {
            // 現れた物の位置エネルギーを足す(エネルギーの増加の基準)
            for (size_t i = 0; i < scene.bodies.size(); ++i) {
                const auto& source = scene.bodies[i];
                if (source.spawnTick == tick && source.massMilligrams > 0) {
                    spawnedEnergy += (double)source.massMilligrams / 1e6 * options.parameters.gravity *
                                     (double)source.position[1] / (1 << 20);
                }
            }

            if (options.debugBodyA >= 0 && tick >= options.debugFirstTick && tick <= options.debugLastTick)
                DebugPair(solver, options);

            solver.Step();
            const StepStats& stats = solver.Stats();
            report.maxPenetration = std::max(report.maxPenetration, stats.maxPenetration);
            report.maxSpeed = std::max(report.maxSpeed, stats.maxSpeed);
            report.energyExcess = std::max(report.energyExcess, stats.energy - spawnedEnergy);
            report.maxContacts = std::max(report.maxContacts, stats.contactCount);
            report.maxColors = std::max(report.maxColors, stats.colorCount);
            if (InLateWindow(scene, solver.Tick())) {
                report.maxSpeedLate = std::max(report.maxSpeedLate, stats.maxSpeed);
                report.maxPenetrationLate = std::max(report.maxPenetrationLate, stats.maxPenetration);
            }

            if (stats.maxPenetration > options.tracePenetration) {
                const auto& bodies = solver.Bodies();
                const auto describe = [&](int i) {
                    const LabBody& b = bodies[i];
                    return std::format("#{}({:.1f} kg, {:.2f}×{:.2f}×{:.2f})", i, b.mass, b.halfExtent.x * 2,
                                       b.halfExtent.y * 2, b.halfExtent.z * 2);
                };
                Log(Channel::Physics, Level::Info,
                    "  刻み {} 食い込み {:.1f} mm: {} と {} 特徴 {:08x}{} 近づく速さ {:.2f} m/s", tick,
                    stats.maxPenetration * 1000, describe(stats.deepestBodyA), describe(stats.deepestBodyB),
                    stats.deepestFeature, stats.deepestIsNew ? "(新しい組)" : "", stats.deepestApproach);
            }

            if (stats.kick > options.traceKick)
                TraceKick(solver);

            if (InLateWindow(scene, solver.Tick()) && stats.maxSpeed > options.traceLateSpeed)
                TraceFastestAtEnd(solver);

            if constexpr (std::is_same_v<Solver, IntegerSolver>) {
                if (options.hashEvery > 0 && solver.Tick() % options.hashEvery == 0) {
                    Log(Channel::Physics, Level::Info, "  刻み {} ハッシュ {:016x}", solver.Tick(),
                        solver.World().StateHash());
                }
            }

            if (statsFile.is_open()) {
                statsFile << std::format("{},{:.6f},{:.6f},{:.3f},{:.3f},{},{}\n", solver.Tick(), stats.maxPenetration,
                                         stats.maxSpeed, stats.energy, spawnedEnergy, stats.contactCount,
                                         stats.colorCount);
            }

            if (bodiesFile.is_open() && solver.Tick() % 6 == 0)
                WriteBodies(bodiesFile, solver);
        }

        if (options.tracePenetration < 1e9 || options.traceKick < 1e9)
            TraceFastestAtEnd(solver);

        if constexpr (std::is_same_v<Solver, IntegerSolver>) {
            const sim::PhysicsStepStats& m = solver.MaxStats();
            Log(Channel::Physics, Level::Info,
                "  整数の値の幅(ビット数): 6×6 の途中の解 {} / H {} / 勾配 {} / 硬さ {} / λ {} / 変位 {} / 分解の失敗 "
                "{} 回 / 最後のハッシュ {:016x}",
                m.solveBits, m.lhsBits, m.rhsBits, m.penaltyBits, m.lambdaBits, m.deltaBits, m.solveFailures,
                solver.World().StateHash());
        }

        report.energyTolerance = std::abs(spawnedEnergy) * 1e-3 + 1;
        report.seconds = chr::duration<double>(chr::steady_clock::now() - start).count();

        // 最後の物(stack・mass_ratio では一番上の箱)のずれと傾き
        const LabBody& top = solver.Bodies().back();
        const Vec3 initial{(double)scene.bodies.back().position[0] / (1 << 20), 0,
                           (double)scene.bodies.back().position[2] / (1 << 20)};
        report.topDrift = std::hypot(top.position.x - initial.x, top.position.z - initial.z);
        report.topHeight = top.position.y;
        const Vec3 up = RotationMatrix(top.rotation) * Vec3{0, 1, 0};
        report.topTiltDegrees = std::acos(std::clamp(up.y, -1.0, 1.0)) * 180 / 3.14159265358979;

        return report;
    }

    // 基準(docs/tickets/T-0016)。満たせば true
    bool Judge(const sim::PhysicsScene& scene, const SceneReport& r) {
        if (scene.name == "stack") {
            return r.topDrift < 0.01 && r.topTiltDegrees < 0.5 && r.maxPenetration < 0.005 && r.maxSpeedLate < 0.01 &&
                   r.energyExcess < r.energyTolerance;
        }

        if (scene.name == "mass_ratio") {
            return r.maxPenetration < 0.005 && r.topHeight > 1.4 && r.maxSpeedLate < 0.01 &&
                   r.energyExcess < r.energyTolerance;
        }

        return r.maxSpeed < 20 && r.maxPenetration < 0.02 && r.energyExcess < r.energyTolerance &&
               r.maxSpeedLate < 0.01;
    }

}  // namespace

int main(int argc, char** argv) {
    LabOptions options;
    if (!ParseOptions(argc, argv, options)) {
        Log(Channel::Physics, Level::Error, "引数が読めない(先頭のコメントを見る)");
        return 2;
    }

    std::vector<sim::PhysicsScene> scenes{sim::MakeStackScene(), sim::MakeMassRatioScene(),
                                          sim::MakePileScene(options.seed)};
    int failures = 0;
    for (const sim::PhysicsScene& scene : scenes) {
        if (options.scene != "all" && options.scene != scene.name)
            continue;

        const SceneReport r = options.integer ? RunScene<IntegerSolver>(scene, options)
                                              : RunScene<AvbdSolver>(scene, options);
        const bool passed = Judge(scene, r);
        failures += passed ? 0 : 1;
        Log(Channel::Physics, Level::Info,
            "{} {}: 食い込み {:.2f} mm(終盤 {:.2f} mm)/ 速さ {:.3f} m/s / 終盤の速さ {:.4f} m/s / エネルギーの超過 "
            "{:.2f} J / "
            "最後の物 ずれ {:.2f} mm・傾き {:.3f}°・高さ {:.3f} m / 接触 {} 点・{} 色 / {:.2f} s",
            passed ? "OK  " : "FAIL", scene.name, r.maxPenetration * 1000, r.maxPenetrationLate * 1000, r.maxSpeed,
            r.maxSpeedLate, r.energyExcess, r.topDrift * 1000, r.topTiltDegrees, r.topHeight, r.maxContacts,
            r.maxColors, r.seconds);
    }

    SingletonFinalizer::Finalize();
    return failures == 0 ? 0 : 1;
}
