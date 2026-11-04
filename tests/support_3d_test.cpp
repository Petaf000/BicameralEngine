// support_3d_test.cpp — 支えの法則の 3 次元の格子の梁(sim/support_law_3d。T-0960)の CPU の確かめ。
//   1. 剛体の動き(並進 3・回り 3)で全部の節の力が整数で 0・一様な歪み(対称 6 つ)で内側の節の力が 0(段の斜めの棒の 3 つの面を含む形)
//   2. 奥行 1 セルの場面(張り出し・梁・両端固定・アーチ・洞窟)が 2 次元(sim/support_law)と同じ答え(丸めの内 = 最大の値の 0.1 %)・
//      奥行の向きの変位と面の外の回りが 0(同じ線)
//   3. 向きを変えても同じ: 奥行の向きに伸びた張り出し・奥行 2 セルの張り出しが 2 次元の張り出しと同じたわみ
//   4. ねじれ: 先に腕のある棒(腕の先に荷)の途中の回りの差が T L ÷ (G J)(J = 7/48 dx⁴)と ±1 %・Saint-Venant と ±5 %・固定した面の回りの力が荷の回りの力と釣り合う
//   5. 帳簿(力 3 成分・回りの力 3 成分)が毎刻み厳密・2 回の実行でハッシュ一致
//   6. (T-1293)段の形: 斜めの梁 3 向き・回したアーチ(0°・45°)の先 / 頂のたわみを同じ階段の形の連続体と ±5 %(0.5 m と 0.25 m)
// 引数: [--ticks n](既定 300)
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <span>
#include <string_view>
#include <vector>

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

    bool LedgerExact(const sim::SupportLedger3D& ledger) {
        for (uint32_t axis = 0; axis < sim::SUPPORT3_AXES; ++axis) {
            if (ledger.netMn[axis] != ledger.reactionMn[axis] + ledger.loadMn[axis])
                return false;

            if (ledger.netTurn[axis] != ledger.reactionTurn[axis] + ledger.loadTurn[axis])
                return false;
        }

        return true;
    }

    // ticks 刻み回す(毎刻み帳簿を確かめる)
    void Run(sim::SupportLaw3D& law, std::string_view name) {
        bool exact = true;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            law.Step();
            exact = exact && LedgerExact(law.Ledger());
        }

        const sim::SupportLedger3D ledger = law.Ledger();
        Log(Channel::Sim, Level::Info, "  {}: 帳簿 {}・残る力 最大 {} mN・回り {} mN・重さ {} mN・支える {} mN", name,
            exact ? "厳密" : "ずれ", ledger.worstCellMn, ledger.worstTurnMn, -ledger.loadMn[sim::SUPPORT3_UP],
            ledger.reactionMn[sim::SUPPORT3_UP]);
        Expect(exact, "帳簿(力 3・回り 3)が毎刻み厳密");
    }

    // --- 1. 剛体の動きと一様な歪み ---
    // 系の A x(右辺の単位 × 2^coefficientShift の整数。丸めない)
    std::vector<int64_t> Apply(const sim::EquilibriumBlockSystem& system, const Values& x) {
        constexpr uint32_t k = sim::SUPPORT3_COMPONENTS;
        std::vector<int64_t> result(x.size(), 0);
        const auto add = [&](uint32_t row, uint32_t column, const sim::EquilibriumBlock& block, bool transpose) {
            for (uint32_t i = 0; i < k; ++i) {
                for (uint32_t j = 0; j < k; ++j) {
                    const int64_t a = transpose ? block[j * sim::EQ_MAX_COMPONENTS + i]
                                                : block[i * sim::EQ_MAX_COMPONENTS + j];
                    result[(size_t)row * k + i] += a * x[(size_t)column * k + j];
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

    // 4 × 4 × 4 の塊 + 3 つの座標の面の段(段の斜めの棒が立つ)。固定した面なし
    sim::SupportScene3D BlockWithStairs() {
        sim::SupportScene3D scene;
        scene.width = 6;
        scene.height = 5;
        scene.depth = 4;
        scene.cellMm = 500;
        scene.stiffnessKpa = test::STONE_KPA;
        scene.densityKgPerM3 = test::STONE_DENSITY;
        scene.solid.assign((size_t)6 * 5 * 4, 0);
        scene.clamps.assign(scene.solid.size(), 0);
        const auto set = [&](uint32_t x, uint32_t up, uint32_t z) {
            scene.solid[x + 6 * (up + 5 * (size_t)z)] = 1;
        };
        for (uint32_t z = 0; z < 4; ++z) {
            for (uint32_t up = 0; up < 4; ++up) {
                for (uint32_t x = 0; x < 4; ++x)
                    set(x, up, z);
            }
        }

        set(4, 3, 1);  // (x, 上) の面の段: (4, 3, 1) → (5, 4, 1)
        set(4, 4, 1);
        set(5, 4, 1);
        set(4, 0, 0);  // (x, 奥行) の面の段: (4, 0, 0) → (5, 0, 1)
        set(4, 0, 1);
        set(5, 0, 1);
        set(1, 4, 0);  // (上, 奥行) の面の段: (1, 4, 0) → (1, 3, 1)

        return scene;
    }

    void TestRigidAndStrain() {
        const sim::SupportScene3D scene = BlockWithStairs();
        const sim::SupportLaw3D law(scene, {});
        const sim::EquilibriumBlockSystem& system = law.BlockSystem();
        const uint32_t count = law.CellCount();
        Expect(law.BraceCount() >= 3, "段の斜めの棒が 3 つの座標の面に立つ");
        Expect(law.CornerBraceCount() >= 1, "角の斜めの棒が 3 歩の階段に立つ");

        std::vector<std::array<int64_t, 3>> positions(count);
        std::vector<bool> inside(count, false);
        for (uint32_t cell = 0; cell < count; ++cell) {
            const sim::EquilibriumNode& node = system.nodes[cell];
            positions[cell] = {2 * node.x + 1, 2 * node.y + 1, 2 * node.z + 1};
            inside[cell] = node.x >= 1 && node.x <= 2 && node.y >= 1 && node.y <= 2 && node.z >= 1 && node.z <= 2;
        }

        const auto check = [&](const Values& x, bool all, std::string_view name) {
            const std::vector<int64_t> force = Apply(system, x);
            bool zero = true;
            for (uint32_t cell = 0; cell < count; ++cell) {
                if (!all && !inside[cell])
                    continue;

                for (uint32_t c = 0; c < sim::SUPPORT3_COMPONENTS; ++c)
                    zero = zero && force[(size_t)cell * sim::SUPPORT3_COMPONENTS + c] == 0;
            }

            Log(Channel::Sim, Level::Info, "  {}: {}", name, zero ? "力 0" : "力が残る");
            Expect(zero, name);
        };

        constexpr int64_t SCALE = 1000;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            Values translation((size_t)count * sim::SUPPORT3_COMPONENTS, 0);
            Values rotation = translation;
            for (uint32_t cell = 0; cell < count; ++cell) {
                translation[(size_t)cell * sim::SUPPORT3_COMPONENTS + axis] = SCALE;
                // u = ω' × p・φ = ω'(ω' = SCALE e_axis)
                const std::array<int64_t, 3>& p = positions[cell];
                std::array<int64_t, 3> omega{};
                omega[axis] = SCALE;
                rotation[(size_t)cell * 6 + 0] = omega[1] * p[2] - omega[2] * p[1];
                rotation[(size_t)cell * 6 + 1] = omega[2] * p[0] - omega[0] * p[2];
                rotation[(size_t)cell * 6 + 2] = omega[0] * p[1] - omega[1] * p[0];
                rotation[(size_t)cell * 6 + sim::SUPPORT3_TURN + axis] = SCALE;
            }

            check(translation, true, std::format("剛体の並進(軸 {})", axis));
            check(rotation, true, std::format("剛体の回り(軸 {})", axis));
        }

        // 一様な歪み(対称): u_i = Σ_j ε_ij p_j・φ = 0
        for (uint32_t i = 0; i < 3; ++i) {
            for (uint32_t j = i; j < 3; ++j) {
                Values strain((size_t)count * sim::SUPPORT3_COMPONENTS, 0);
                for (uint32_t cell = 0; cell < count; ++cell) {
                    strain[(size_t)cell * 6 + i] += SCALE * positions[cell][j];
                    if (i != j)
                        strain[(size_t)cell * 6 + j] += SCALE * positions[cell][i];
                }

                check(strain, false, std::format("一様な歪み ({}, {}) の内側", i, j));
            }
        }
    }

    // --- 2. 奥行 1 セルの場面が 2 次元と同じ ---
    void TestDepthOne(std::string_view name, const sim::SupportScene& scene) {
        sim::SupportLaw flat(scene, {});
        sim::SupportLaw3D solid(test::Extruded(scene), {});
        for (uint32_t tick = 0; tick < ticks; ++tick)
            flat.Step();

        Run(solid, name);

        // 平面の成分(x・上・奥行の回り = 2 次元の φ)の差と、面の外の成分(奥行・x と上の回り)の大きさ
        constexpr std::array<std::array<uint32_t, 2>, 3> PLANAR = {
            {{sim::SUPPORT_X, sim::SUPPORT3_X},
             {sim::SUPPORT_UP, sim::SUPPORT3_UP},
             {sim::SUPPORT_TURN, sim::SUPPORT3_TURN + sim::SUPPORT3_DEPTH}}};
        int64_t largest = 0;
        int64_t difference = 0;
        int64_t outside = 0;
        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                for (const auto& [flatComponent, solidComponent] : PLANAR) {
                    const int64_t value = flat.Value(x, up, flatComponent);
                    largest = std::max(largest, std::abs(value));
                    difference = std::max(difference, std::abs(value - solid.Value(x, up, 0, solidComponent)));
                }

                for (const uint32_t component :
                     {sim::SUPPORT3_DEPTH, sim::SUPPORT3_TURN + sim::SUPPORT3_X, sim::SUPPORT3_TURN + sim::SUPPORT3_UP})
                    outside = std::max(outside, std::abs(solid.Value(x, up, 0, component)));
            }
        }

        const double ratio = (double)difference / (double)std::max<int64_t>(largest, 1);
        const double outsideRatio = (double)outside / (double)std::max<int64_t>(largest, 1);
        Log(Channel::Sim, Level::Info, "  {}: 最大の値 {:.4f} mm・2 次元との差 {:.2e}・面の外 {:.2e}", name,
            (double)largest / 65536.0 / 1.0e6, ratio, outsideRatio);
        Expect(ratio <= 1.0e-3, std::format("{}: 奥行 1 セルが 2 次元と同じ(0.1 %)", name));
        Expect(outsideRatio <= 1.0e-3, std::format("{}: 面の外の成分が 0(0.1 %)", name));
    }

    // --- 3. 向きを変えても同じ ---
    double TipSag(const sim::SupportLaw3D& law, uint32_t x, uint32_t z) {
        return -(double)law.Value(x, 0, z, sim::SUPPORT3_UP);
    }

    void TestOrientation() {
        const sim::SupportScene shelf = test::Beam(8, 1, 500, false);
        sim::SupportLaw flat(shelf, {});
        for (uint32_t tick = 0; tick < ticks; ++tick)
            flat.Step();

        const double reference = -(double)flat.Value(7, 0, sim::SUPPORT_UP);

        // 奥行の向きに伸びた張り出し(奥行 0 の面を壁に固定)
        sim::SupportScene3D along;
        along.width = 1;
        along.height = 1;
        along.depth = 8;
        along.cellMm = 500;
        along.stiffnessKpa = test::STONE_KPA;
        along.densityKgPerM3 = test::STONE_DENSITY;
        along.solid.assign(8, 1);
        along.clamps.assign(8, 0);
        along.clamps[0] = sim::Support3Clamp(sim::SUPPORT3_DEPTH, -1);
        sim::SupportLaw3D turned(along, {});
        Run(turned, "奥行の向きの張り出し");

        sim::SupportLaw3D wide(test::Extruded(shelf, 2), {});
        Run(wide, "奥行 2 セルの張り出し");

        const double turnedRatio = TipSag(turned, 0, 7) / reference;
        const double wideRatio = TipSag(wide, 7, 0) / reference;
        const double wideOther = TipSag(wide, 7, 1) / reference;
        Log(Channel::Sim, Level::Info, "  先のたわみ ÷ 2 次元: 奥行の向き {:.6f}・奥行 2 セル {:.6f} / {:.6f}",
            turnedRatio, wideRatio, wideOther);
        Expect(std::abs(turnedRatio - 1.0) <= 1.0e-3, "奥行の向きの張り出しが 2 次元と同じ(0.1 %)");
        Expect(std::abs(wideRatio - 1.0) <= 1.0e-3 && std::abs(wideOther - 1.0) <= 1.0e-3,
               "奥行 2 セルの張り出しが 2 次元と同じ(0.1 %)");
    }

    // --- 4. ねじれ: x の向きの棒(8 セル・断面 1 セル・根を壁に固定)の先に奥行の向きの腕(2 セル)・腕の先に荷 P(重さ 0)---
    void TestTwist() {
        constexpr uint32_t LENGTH = 8;
        constexpr int64_t LOAD_MN = 1000000;  // 1 kN
        const sim::SupportScene3D scene = test::TwistArm(LENGTH, LOAD_MN);

        sim::SupportLaw3D law(scene, {});
        Run(law, "ねじれ(腕の先に 1 kN)");
        sim::SupportLaw3D again(scene, {});
        for (uint32_t tick = 0; tick < ticks; ++tick)
            again.Step();

        Expect(law.Hash() == again.Hash(), "2 回の実行でハッシュ一致");

        // 棒の途中(x = 1〜5 の 4 セル。先の腕の付け根は段の斜めの棒が立つので外す)の回りの差 Δθ = 2 Δφ_x ÷ dx と、
        // 格子の式 T × 4 dx ÷ (G J)(T = P × 2 dx・G = E/2・J = 7/48 dx⁴〔T-1293〕)。Saint-Venant の 1 × 1 の断面(0.1406 dx⁴)との比も
        // (+3.7 % の見込み。太い棒は T-1293 の捨てる試作)
        const double cellM = 0.5;
        const auto turnX = [&](uint32_t x) {
            return (double)law.Value(x, 0, 0, sim::SUPPORT3_TURN + sim::SUPPORT3_X);
        };
        const double angle = 2.0 * (turnX(5) - turnX(1)) / 65536.0 / 1.0e9 / cellM;
        const double torque = (double)LOAD_MN / 1000.0 * 2.0 * cellM;
        const double modulus = test::STONE_KPA * 1000.0;
        const double expected = torque * 4.0 * cellM / (modulus / 2.0 * 7.0 / 48.0 * std::pow(cellM, 4));
        const double saintVenant = torque * 4.0 * cellM / (modulus / 2.0 * 0.140577 * std::pow(cellM, 4));
        const sim::SupportLedger3D ledger = law.Ledger();
        // 固定した面の x の回りの力(原点の回り。mN × dx/2)が重さと荷の回りの力の逆(着いた答えの回りの釣り合い)
        const double reaction = (double)ledger.reactionTurn[sim::SUPPORT3_X] / (double)ledger.loadTurn[sim::SUPPORT3_X];
        Log(Channel::Sim, Level::Info,
            "  途中 4 セルの回りの差 {:.6e} rad・格子の式 {:.6e}(比 {:.5f})・Saint-Venant との硬さの比 "
            "{:.4f}・壁の回りの力 ÷ "
            "荷の回りの力 {:.6f}",
            angle, expected, angle / expected, saintVenant / angle, reaction);
        Expect(std::abs(angle / expected - 1.0) <= 0.01, "ねじれの角が格子の式と ±1 %");
        Expect(std::abs(saintVenant / angle - 1.0) <= 0.05, "ねじれの硬さが Saint-Venant と ±5 %");
        Expect(std::abs(reaction + 1.0) <= 1.0e-3, "壁の回りの力が荷の回りの力と釣り合う");
    }

    // --- 5. 段の形(T-1293): 斜めの梁・回したアーチを、同じ階段の形の連続体(H11 を m 分割・外挿。浮動小数点の捨てる試作)と比べる ---
    // 基準の値は E = 1・ρg = 1 の単位の上の変位(m)。石では × ρg ÷ E。cells は捨てる試作の形のセルの数(形の作りが同じかの確かめ)
    struct StairCase {
        const char* name;
        bool arch;
        std::array<double, 3> direction;  // 梁の向き(アーチは回す角 = direction[0] 度)
        uint64_t cellMm;
        uint32_t cells;
        double continuumRef;
    };

    constexpr StairCase STAIR_CASES[] = {
        {"梁 (1,1,0) 0.5 m", false, {1, 1, 0}, 500, 52, -1830.9},
        {"梁 (1,1,0) 0.25 m", false, {1, 1, 0}, 250, 340, -1867.9},
        {"梁 (1,0,1) 0.5 m", false, {1, 0, 1}, 500, 52, -2445.7},
        {"梁 (1,0,1) 0.25 m", false, {1, 0, 1}, 250, 340, -2230.0},
        {"梁 (1,1,1) 0.5 m", false, {1, 1, 1}, 500, 49, -3109.7},
        {"梁 (1,1,1) 0.25 m", false, {1, 1, 1}, 250, 322, -2622.9},
        {"アーチ 0° 0.5 m", true, {0, 0, 0}, 500, 108, -118.9},
        {"アーチ 0° 0.25 m", true, {0, 0, 0}, 250, 904, -94.36},
        {"アーチ 45° 0.5 m", true, {45, 0, 0}, 500, 116, -145.6},
        {"アーチ 45° 0.25 m", true, {45, 0, 0}, 250, 794, -96.62},
    };

    void TestStairs() {
        const double stoneSagPerUnit = (double)test::STONE_DENSITY * 9.80665 / (test::STONE_KPA * 1000.0);
        for (const StairCase& stair : STAIR_CASES) {
            const test::StairScene built = stair.arch ? test::RotatedArch(stair.direction[0], stair.cellMm)
                                                      : test::TiltedBeam(stair.direction, stair.cellMm);
            sim::SupportLaw3D law(built.scene, {});
            Expect(law.CellCount() == stair.cells, std::format("{}: 形のセルの数が捨てる試作と同じ", stair.name));
            Run(law, stair.name);
            double sum = 0;
            for (const auto& c : built.probe)
                sum += (double)law.Value(c[0], c[1], c[2], sim::SUPPORT3_UP);

            const double sagM = sum / (double)built.probe.size() / 65536.0 / 1.0e9;
            const double ratio = sagM / (stair.continuumRef * stoneSagPerUnit);
            Log(Channel::Sim, Level::Info, "  {}: セル {}・辺の棒 {}・角の棒 {}・格子 ÷ 連続体 {:.4f}", stair.name,
                law.CellCount(), law.BraceCount(), law.CornerBraceCount(), ratio);
            Expect(std::abs(ratio - 1.0) <= 0.05, std::format("{}: 連続体と ±5 %", stair.name));
        }
    }

    int RunAll(std::span<char*> arguments) {
        for (size_t i = 1; i < arguments.size(); ++i) {
            const std::string_view name = arguments[i];
            if (name == "--ticks" && i + 1 < arguments.size())
                ticks = (uint32_t)std::atoi(arguments[++i]);
        }

        Log(Channel::Sim, Level::Info, "1. 剛体の動きと一様な歪み");
        TestRigidAndStrain();
        Log(Channel::Sim, Level::Info, "2. 奥行 1 セルの場面と 2 次元({} 刻み)", ticks);
        TestDepthOne("1 セルの張り出し 4 m(0.5 m)", test::Beam(8, 1, 500, false));
        TestDepthOne("張り出し 4 m(厚さ 0.5 m・0.25 m)", test::Beam(16, 2, 250, false));
        TestDepthOne("両端を固定した梁 8 × 1 m(0.5 m)", test::Beam(16, 2, 500, true));
        TestDepthOne("アーチ(0.5 m)", test::Arch(500));
        TestDepthOne("洞窟(1 m)", test::Cave(1000));
        TestDepthOne("壁と梁(0.5 m)", test::WallBeam());
        Log(Channel::Sim, Level::Info, "3. 向き");
        TestOrientation();
        Log(Channel::Sim, Level::Info, "4. ねじれ");
        TestTwist();
        Log(Channel::Sim, Level::Info, "5. 段の形と連続体(T-1293)");
        TestStairs();

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "support_3d_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "support_3d_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = RunAll(std::span(argv, (size_t)argc));
    SingletonFinalizer::Finalize();

    return exitCode;
}
