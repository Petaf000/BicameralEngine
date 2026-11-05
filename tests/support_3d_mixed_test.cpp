// support_3d_mixed_test.cpp — 3 次元の格子の梁の違う大きさのセル(sim/support_law_3d・support_law_3d_mixed。T-1294)の CPU の確かめ。
//   1. 剛体の動き(並進 3・回り 3)で全部の節の力が整数で 0・一様な歪み(対称 6 つ)で内側の節の力が 0(丸めの内)・
//      形 A の項の力が全部の場(一様な曲げ 6 つ・ねじれ 3 つも)で 0。一様な曲げの内側の節の力は記録だけ(2 次の偽のずれ。2 次元と同じ)
//      分けたセルどうし・分けたセルと粗いセル・段の棒の端が分けたセル(違う大きさの段の棒)を含む塊
//   2. 奥行 1 セルの 2 次元の混ざる場面(support_mixed_test の場面)を 3 次元へ(細かいセルは奥行 2 つ)にした場面が 2 次元と同じ答え
//      (丸めの内 = 最大の値の 0.1 %)・面の外の成分が 0
//   3. 奥行の途中だけを分けた場面: 帳簿(力 3・回り 3)が毎刻み厳密・2 回の実行でハッシュ一致・全部 0.25 m の塊とのたわみの比
// 引数: [--ticks n](既定 300)
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "sim/support_law.h"
#include "sim/support_law_3d.h"
#include "support_scenes.h"

using namespace bicameral;

namespace {

    int failureCount = 0;
    uint32_t ticks = 300;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    using Values = std::vector<int64_t>;  // 節 × 6
    constexpr uint32_t K = sim::SUPPORT3_COMPONENTS;

    bool LedgerExact(const sim::SupportLedger3D& ledger) {
        for (uint32_t axis = 0; axis < sim::SUPPORT3_AXES; ++axis) {
            if (ledger.netMn[axis] != ledger.reactionMn[axis] + ledger.loadMn[axis])
                return false;

            if (ledger.netTurn[axis] != ledger.reactionTurn[axis] + ledger.loadTurn[axis])
                return false;
        }

        return true;
    }

    void Run(sim::SupportLaw3D& law, std::string_view name) {
        bool exact = true;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            law.Step();
            exact = exact && LedgerExact(law.Ledger());
        }

        const sim::SupportLedger3D ledger = law.Ledger();
        Log(Channel::Sim, Level::Info,
            "  {}: セル {}・違う大きさの面 {}・段の棒 {}・帳簿 {}・残る力 最大 {} mN・重さ {} mN・支える {} mN", name,
            law.CellCount(), law.MixedFaceCount(), law.BraceCount(), exact ? "厳密" : "ずれ", ledger.worstCellMn,
            -ledger.loadMn[sim::SUPPORT3_UP], ledger.reactionMn[sim::SUPPORT3_UP]);
        Expect(exact, std::format("{}: 帳簿(力 3・回り 3)が毎刻み厳密", name));
    }

    // 系の A x(丸めない整数)
    std::vector<int64_t> Apply(const sim::EquilibriumBlockSystem& system, const Values& x) {
        std::vector<int64_t> result(x.size(), 0);
        const auto add = [&](uint32_t row, uint32_t column, const sim::EquilibriumBlock& block, bool transpose) {
            for (uint32_t i = 0; i < K; ++i) {
                for (uint32_t j = 0; j < K; ++j) {
                    const int64_t a = transpose ? block[j * sim::EQ_MAX_COMPONENTS + i]
                                                : block[i * sim::EQ_MAX_COMPONENTS + j];
                    result[(size_t)row * K + i] += a * x[(size_t)column * K + j];
                }
            }
        };

        for (const sim::EquilibriumBlockFace& face : system.faces) {
            add(face.low, face.low, face.lowLow, false);
            add(face.low, face.high, face.lowHigh, false);
            add(face.high, face.low, face.lowHigh, true);
            add(face.high, face.high, face.highHigh, false);
        }

        for (const sim::EquilibriumBlockFixed& fixed : system.fixedFaces)
            add(fixed.node, fixed.node, fixed.block, false);

        return result;
    }

    // --- 1. 剛体の動きと一様な歪み: 4 × 4 × 4 の塊 + 3 つの座標の面の段。分けたセルを中と段に ---
    sim::SupportScene3D MixedBlock() {
        sim::SupportScene3D scene;
        scene.width = 6;
        scene.height = 5;
        scene.depth = 4;
        scene.cellMm = 500;
        scene.stiffnessKpa = test::STONE_KPA;
        scene.densityKgPerM3 = test::STONE_DENSITY;
        scene.solid.assign((size_t)6 * 5 * 4, 0);
        scene.clamps.assign(scene.solid.size(), 0);
        scene.fine.assign(scene.solid.size(), 0);
        const auto at = [](uint32_t x, uint32_t up, uint32_t z) {
            return x + 6 * (up + 5 * (size_t)z);
        };
        for (uint32_t z = 0; z < 4; ++z) {
            for (uint32_t up = 0; up < 4; ++up) {
                for (uint32_t x = 0; x < 4; ++x)
                    scene.solid[at(x, up, z)] = 1;
            }
        }

        for (const auto& [x, up, z] : std::array<std::array<uint32_t, 3>, 7>{
                 {{4, 3, 1}, {4, 4, 1}, {5, 4, 1}, {4, 0, 0}, {4, 0, 1}, {5, 0, 1}, {1, 4, 0}}})
            scene.solid[at(x, up, z)] = 1;

        // 中の 2 つ(隣り合う)・中の斜め・段の角の端(粗いセルとの段)・段の間のセル(粗いセルどうしの段の間)
        for (const auto& [x, up, z] :
             std::array<std::array<uint32_t, 3>, 5>{{{1, 1, 1}, {2, 1, 1}, {2, 2, 2}, {4, 4, 1}, {4, 0, 1}}})
            scene.fine[at(x, up, z)] = 1;

        return scene;
    }

    // 剛体の動き・一様な歪み・一様な曲げ・ねじれ(None と形 A)。位置は細かいセルの半分の単位・回りの腕は粗いセル 2・細かいセル 1
    void TestPatch() {
        const sim::SupportScene3D scene = MixedBlock();
        sim::SupportOptions formA;
        formA.mixedPairMaterial = sim::MixedPairMaterial::NeighborStrain;
        const sim::SupportLaw3D law(scene, {});
        const sim::SupportLaw3D lawA(scene, formA);
        const sim::EquilibriumBlockSystem& system = law.BlockSystem();
        const uint32_t count = law.CellCount();
        Log(Channel::Sim, Level::Info, "  セル {}・違う大きさの面 {}・辺の段の棒 {}・角の段の棒 {}", count,
            law.MixedFaceCount(), law.BraceCount(), law.CornerBraceCount());
        Expect(law.Mixed() && law.MixedFaceCount() > 0, "違う大きさの面がある");

        std::vector<std::array<int64_t, 3>> positions(count);
        std::vector<int64_t> arms(count);
        std::vector<bool> inside(count, false);
        for (uint32_t cell = 0; cell < count; ++cell) {
            const sim::EquilibriumNode& node = system.nodes[cell];
            const bool fine = node.level == 1;
            const int64_t scale = fine ? 2 : 4;
            const int64_t offset = fine ? 1 : 2;
            positions[cell] = {scale * node.x + offset, scale * node.y + offset, scale * node.z + offset};
            arms[cell] = fine ? 1 : 2;
            const std::array<int64_t, 3> parent = fine ? std::array<int64_t, 3>{node.x / 2, node.y / 2, node.z / 2}
                                                       : std::array<int64_t, 3>{node.x, node.y, node.z};
            inside[cell] = rng::all_of(parent, [](int64_t c) { return c >= 1 && c <= 2; });
        }

        // exact: 整数で 0。そうでなければ係数の丸め(b ÷ 288 など)の内(外側の節の力の最大の 1e-9)。interior: 内側の節だけ見る。
        // 形 A は None との差(形 A の項の力)を全部の節で見る
        const auto check = [&](const Values& x, bool exact, bool interior, std::string_view name,
                               bool expectZero = true) {
            const std::vector<int64_t> force = Apply(system, x);
            const std::vector<int64_t> forceA = Apply(lawA.BlockSystem(), x);
            int64_t largest = 1;
            int64_t worst = 0;
            int64_t worstA = 0;
            for (size_t i = 0; i < force.size(); ++i) {
                largest = std::max(largest, std::abs(force[i]));
                worstA = std::max(worstA, std::abs(forceA[i] - force[i]));
                if (!interior || inside[i / K])
                    worst = std::max(worst, std::abs(force[i]));
            }

            const double limit = exact ? 0.0 : 1.0e-9 * (double)largest;
            Log(Channel::Sim, Level::Info, "  {}: 残る力 {}・形 A の項の力 {}(節の力の最大 {})", name, worst, worstA,
                largest);
            Expect(!expectZero || (double)worst <= limit, name);
            Expect((double)worstA <= limit, std::format("{}: 形 A の項の力が 0", name));
        };

        constexpr int64_t SCALE = 1000;
        const auto field = [&](const auto& displacement, const auto& turn) {
            Values x((size_t)count * K, 0);
            for (uint32_t cell = 0; cell < count; ++cell) {
                const std::array<int64_t, 3> u = displacement(positions[cell]);
                const std::array<int64_t, 3> theta = turn(positions[cell]);
                for (uint32_t axis = 0; axis < 3; ++axis) {
                    x[(size_t)cell * K + axis] = u[axis];
                    x[(size_t)cell * K + sim::SUPPORT3_TURN + axis] = theta[axis] * arms[cell];
                }
            }

            return x;
        };
        const auto cross = [](const std::array<int64_t, 3>& a, const std::array<int64_t, 3>& b) {
            return std::array<int64_t, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                                          a[0] * b[1] - a[1] * b[0]};
        };
        const auto unit = [](uint32_t axis) {
            std::array<int64_t, 3> e{};
            e[axis] = 1;
            return e;
        };
        const auto none = [](const std::array<int64_t, 3>&) {
            return std::array<int64_t, 3>{};
        };

        for (uint32_t axis = 0; axis < 3; ++axis) {
            const auto shift = [&](const std::array<int64_t, 3>&) {
                std::array<int64_t, 3> u{};
                u[axis] = SCALE;
                return u;
            };
            const std::array<int64_t, 3> omega = [&] {
                std::array<int64_t, 3> w{};
                w[axis] = SCALE;
                return w;
            }();
            check(field(shift, none), true, false, std::format("剛体の並進(軸 {})", axis));
            check(field([&](const std::array<int64_t, 3>& p) { return cross(omega, p); },
                        [&](const std::array<int64_t, 3>&) { return omega; }),
                  true, false, std::format("剛体の回り(軸 {})", axis));
        }

        // 一様な歪み(対称): u_i = Σ_j ε_ij p_j・φ = 0
        for (uint32_t i = 0; i < 3; ++i) {
            for (uint32_t j = i; j < 3; ++j) {
                const auto strain = [&](const std::array<int64_t, 3>& p) {
                    std::array<int64_t, 3> u{};
                    u[i] += SCALE * p[j];
                    if (i != j)
                        u[j] += SCALE * p[i];

                    return u;
                };
                check(field(strain, none), false, true, std::format("一様な歪み ({}, {}) の内側", i, j));
            }
        }

        constexpr int64_t BEND = 10;
        // 一様な曲げ(軸 a の回りの角が軸 b に沿って一様に変わる。κ = 2 BEND。値が int64 に収まる大きさ): θ = κ p_b e_a・
        // u = κ p_b (e_a × p) − ½ κ p_b² (e_a × e_b)。a = b はねじれ(反りなし。格子の釣り合いではないので形 A の項だけ見る)
        for (uint32_t a = 0; a < 3; ++a) {
            for (uint32_t b = 0; b < 3; ++b) {
                const auto bendU = [&](const std::array<int64_t, 3>& p) {
                    const std::array<int64_t, 3> around = cross(unit(a), p);
                    const std::array<int64_t, 3> side = cross(unit(a), unit(b));
                    std::array<int64_t, 3> u{};
                    for (uint32_t c = 0; c < 3; ++c)
                        u[c] = 2 * BEND * p[b] * around[c] - BEND * p[b] * p[b] * side[c];

                    return u;
                };
                const auto bendTurn = [&](const std::array<int64_t, 3>& p) {
                    std::array<int64_t, 3> theta{};
                    theta[a] = 2 * BEND * p[b];
                    return theta;
                };
                if (a == b) {
                    const Values x = field(bendU, bendTurn);
                    const std::vector<int64_t> force = Apply(system, x);
                    const std::vector<int64_t> forceA = Apply(lawA.BlockSystem(), x);
                    int64_t largest = 1;
                    int64_t worstA = 0;
                    for (size_t i = 0; i < force.size(); ++i) {
                        largest = std::max(largest, std::abs(force[i]));
                        worstA = std::max(worstA, std::abs(forceA[i] - force[i]));
                    }

                    Log(Channel::Sim, Level::Info, "  ねじれ(軸 {}): 形 A の項の力 {}(節の力の最大 {})", a, worstA,
                        largest);
                    Expect((double)worstA <= 1.0e-9 * (double)largest,
                           std::format("ねじれ(軸 {}): 形 A の項の力が 0", a));
                    continue;
                }

                // 内側の節の力は 0 にならない(記録だけ): 粗いセルの剛体の動きで面の点へ延ばす値は 2 次の場で −½ κ s²、細かいセルは
                // −⅛ κ s² ずれ、違う大きさの面に 2 次の偽のずれ ⅜ κ s² が残る(2 次元の面の点のばね〔ADR-0485〕と同じ。T-1294 の作業ログ)
                check(field(bendU, bendTurn), false, true,
                      std::format("一様な曲げ(軸 {} の回り・軸 {} に沿って)の内側", a, b), false);
            }
        }
    }

    // --- 2. 奥行 1 セルの 2 次元の混ざる場面 ---
    void TestDepthOne(std::string_view name, const sim::SupportScene& scene, const sim::SupportOptions& options) {
        sim::SupportLaw flat(scene, options);
        sim::SupportLaw3D solid(test::Extruded(scene), options);
        for (uint32_t tick = 0; tick < ticks; ++tick)
            flat.Step();

        Run(solid, name);

        constexpr std::array<std::array<uint32_t, 2>, 3> PLANAR = {
            {{sim::SUPPORT_X, sim::SUPPORT3_X},
             {sim::SUPPORT_UP, sim::SUPPORT3_UP},
             {sim::SUPPORT_TURN, sim::SUPPORT3_TURN + sim::SUPPORT3_DEPTH}}};
        int64_t largest = 0;
        int64_t difference = 0;
        int64_t outside = 0;
        int64_t depthDifference = 0;
        for (uint32_t fup = 0; fup < 2 * scene.height; ++fup) {
            for (uint32_t fx = 0; fx < 2 * scene.width; ++fx) {
                for (const auto& [flatComponent, solidComponent] : PLANAR) {
                    const int64_t value = flat.FineValue(fx, fup, flatComponent);
                    largest = std::max(largest, std::abs(value));
                    difference = std::max(difference, std::abs(value - solid.FineValue(fx, fup, 0, solidComponent)));
                    depthDifference = std::max(depthDifference, std::abs(solid.FineValue(fx, fup, 0, solidComponent) -
                                                                         solid.FineValue(fx, fup, 1, solidComponent)));
                }

                for (const uint32_t component : {sim::SUPPORT3_DEPTH, sim::SUPPORT3_TURN + sim::SUPPORT3_X,
                                                 sim::SUPPORT3_TURN + sim::SUPPORT3_UP}) {
                    for (const uint32_t fz : {0u, 1u})
                        outside = std::max(outside, std::abs(solid.FineValue(fx, fup, fz, component)));
                }
            }
        }

        const auto ratioOf = [&](int64_t value) {
            return (double)value / (double)std::max<int64_t>(largest, 1);
        };
        Log(Channel::Sim, Level::Info,
            "  {}: 最大の値 {:.4f} mm・2 次元との差 {:.2e}・奥行の 2 つの差 {:.2e}・面の外 {:.2e}", name,
            (double)largest / 65536.0 / 1.0e6, ratioOf(difference), ratioOf(depthDifference), ratioOf(outside));
        Expect(ratioOf(difference) <= 1.0e-3, std::format("{}: 奥行 1 セルが 2 次元と同じ(0.1 %)", name));
        Expect(ratioOf(outside) <= 1.0e-3, std::format("{}: 面の外の成分が 0(0.1 %)", name));
    }

    // --- 3. 奥行の途中だけを分けた場面(3 次元の形): 帳簿・決定性・全部 0.25 m の塊とのたわみ ---
    sim::SupportScene3D AllFine(const sim::SupportScene3D& scene) {
        sim::SupportScene3D fine;
        fine.width = 2 * scene.width;
        fine.height = 2 * scene.height;
        fine.depth = 2 * scene.depth;
        fine.cellMm = scene.cellMm / 2;
        fine.stiffnessKpa = scene.stiffnessKpa;
        fine.stiffnessUpKpa = scene.stiffnessUpKpa;
        fine.densityKgPerM3 = scene.densityKgPerM3;
        fine.gravityUmPerS2 = scene.gravityUmPerS2;
        const size_t count = (size_t)fine.width * fine.height * fine.depth;
        fine.solid.assign(count, 0);
        fine.clamps.assign(count, 0);
        for (uint32_t z = 0; z < fine.depth; ++z) {
            for (uint32_t up = 0; up < fine.height; ++up) {
                for (uint32_t x = 0; x < fine.width; ++x) {
                    const size_t from = x / 2 + (size_t)scene.width * (up / 2 + (size_t)scene.height * (z / 2));
                    const size_t to = x + (size_t)fine.width * (up + (size_t)fine.height * z);
                    fine.solid[to] = scene.solid[from];
                    // 固定した面は向きの側の細かいセルへ
                    const std::array<uint32_t, 3> p = {x, up, z};
                    for (uint32_t axis = 0; axis < 3; ++axis) {
                        for (const int32_t side : {-1, 1}) {
                            const uint8_t bit = sim::Support3Clamp(axis, side);
                            if ((scene.clamps[from] & bit) != 0 && (p[axis] & 1) == (side > 0 ? 1u : 0u))
                                fine.clamps[to] |= bit;
                        }
                    }
                }
            }
        }

        return fine;
    }

    void TestPartialDepth() {
        // 片持ち梁 8 × 1 m(0.5 m・奥行 2 セル)の根の 2 m の奥行の手前だけを 0.25 m
        sim::SupportScene3D scene = test::Extruded(test::Beam(16, 2, 500, false), 2);
        scene.fine.assign(scene.solid.size(), 0);
        for (uint32_t up = 0; up < 2; ++up) {
            for (uint32_t x = 0; x < 4; ++x)
                scene.fine[x + (size_t)16 * up] = 1;
        }

        sim::SupportLaw3D law(scene, {});
        Run(law, "片持ち梁の根の 2 m の奥行の手前だけ 0.25 m");
        sim::SupportLaw3D again(scene, {});
        for (uint32_t tick = 0; tick < ticks; ++tick)
            again.Step();

        Expect(law.Hash() == again.Hash(), "2 回の実行でハッシュ一致");

        sim::SupportLaw3D fine(AllFine(scene), {});
        Run(fine, "全部 0.25 m");
        sim::SupportLaw3D coarse(test::Extruded(test::Beam(16, 2, 500, false), 2), {});
        Run(coarse, "全部 0.5 m");
        const auto tip = [&](const sim::SupportLaw3D& each) {
            double sum = 0;
            for (uint32_t fz = 0; fz < 4; ++fz) {
                for (uint32_t fup = 0; fup < 4; ++fup)
                    sum += (double)each.FineValue(31, fup, fz, sim::SUPPORT3_UP);
            }

            return sum;
        };
        const auto tipFine = [&]() {
            double sum = 0;
            for (uint32_t z = 0; z < 4; ++z) {
                for (uint32_t up = 0; up < 4; ++up)
                    sum += (double)fine.Value(31, up, z, sim::SUPPORT3_UP);
            }

            return sum;
        };
        const double reference = tipFine();
        const double mixedRatio = tip(law) / reference;
        const double coarseRatio = tip(coarse) / reference;
        Log(Channel::Sim, Level::Info, "  先のたわみ ÷ 全部 0.25 m: 混ざる {:.4f}・全部 0.5 m {:.4f}", mixedRatio,
            coarseRatio);
        Expect(std::abs(mixedRatio - 1.0) <= 0.05, "奥行の手前だけ分けた片持ち梁の先のたわみが全部 0.25 m と ±5 %");
    }

    sim::SupportScene WallBeam() {
        return test::WallBeam();
    }

    int RunAll(std::span<char*> arguments) {
        for (size_t i = 1; i < arguments.size(); ++i) {
            const std::string_view name = arguments[i];
            if (name == "--ticks" && i + 1 < arguments.size())
                ticks = (uint32_t)std::atoi(arguments[++i]);
        }

        Log(Channel::Sim, Level::Info, "1. 剛体の動き・一様な歪み・一様な曲げ・ねじれ(None と形 A)");
        TestPatch();

        sim::SupportOptions formA;
        formA.mixedPairMaterial = sim::MixedPairMaterial::NeighborStrain;
        const std::pair<const char*, sim::SupportOptions> forms[] = {{"None(既定)", {}}, {"形 A", formA}};
        for (const auto& [formName, options] : forms) {
            Log(Channel::Sim, Level::Info, "2. 奥行 1 セルの 2 次元の混ざる場面({} 刻み)・{}", ticks, formName);
            const sim::SupportScene cantilever = test::Beam(16, 2, 500, false);
            TestDepthOne("片持ち梁 8 × 1 m の根の半分を 0.25 m", test::MarkedFine(cantilever, 0, 8, 0, 2), options);
            TestDepthOne("片持ち梁 8 × 1 m の上の段だけ 0.25 m", test::MarkedFine(cantilever, 0, 16, 1, 2), options);
            TestDepthOne("両端を固定した梁の真ん中 3 m を 0.25 m",
                         test::MarkedFine(test::Beam(16, 2, 500, true), 5, 11, 0, 2), options);
            TestDepthOne("壁と梁の上の角の壁の側だけ 0.25 m", test::MarkedFine(WallBeam(), 1, 2, 4, 5), options);
            TestDepthOne("壁と梁の上の角の梁の側だけ 0.25 m", test::MarkedFine(WallBeam(), 2, 3, 3, 4), options);
            TestDepthOne("壁と梁の上の角の間のセルだけ 0.25 m", test::MarkedFine(WallBeam(), 1, 2, 3, 4), options);
            TestDepthOne("アーチの右の足を 0.25 m", test::MarkedFine(test::Arch(500), 14, 20, 0, 6), options);
        }

        Log(Channel::Sim, Level::Info, "3. 奥行の途中だけを分けた場面");
        TestPartialDepth();

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "support_3d_mixed_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "support_3d_mixed_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = RunAll(std::span(argv, (size_t)argc));
    SingletonFinalizer::Finalize();

    return exitCode;
}
