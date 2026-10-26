// support_mixed_test.cpp — 支えの法則で 0.5 m と 0.25 m のセルが混ざる場面(T-1120・ADR-0470。22 §3.2・§3.5)の CPU の測り。
// 場面(石。縦の面・板の厚さ 0.5 m): 張り出し 4 m(厚さ 0.5 m)の根・片持ち梁 8 × 1 m の根 / 先 / 真ん中・両端を固定した梁 8 × 1 m の
// 真ん中・洞窟の天井(幅 24 × 高さ 10 m・8 × 4 m の空洞)を 0.25 m に分け、全部 0.25 m の答え(同じ場面を 0.25 m のセルで)と比べる。
// 比べ方: 0.5 m の格子のセルごとに、混ざった答え(分けていないセルはその値・分けたセルは細かい 4 つの平均)と全部 0.25 m の答えの
// 細かい 4 つの平均。探る列(片持ちの先・真ん中・天井の真ん中)のたわみの比(±5 % が T-1120 の合格)と、全部のセルの最大の差 ÷ 最大の変位。
// 違う大きさの面の形 5 つ(④ 対の面 = 既定・③ 面の点のばね・③' 面の点で梁のずれの硬さ・② 細かい側の面ごとの梁・① 中心どうし)と、全部 0.5 m の答えも並べる。
// 面の縁の力(T-1191): 細かいセルが絡む面(違う大きさの面・細かい面)の縁の力と、全部 0.25 m の答えの同じ面の差 ÷ 全部 0.25 m の縁の力の最大
// (混ざる場面の板の厚さは 0.5 m なので ÷ 2 で比べる)。
// 試験が落ちるのは帳簿(毎刻み Σ セルの上の力 = 固定した面の力 − 重さ)・決定性(2 回の実行のハッシュ)・符号・④ の ±5 %・④ の縁の力の差 ≤ 5 %(固定した面から 0.5 m のセル 1 つ以内の面は除いて別に出す。T-1206)。
// 引数: [--ticks n](既定 240)
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "sim/support_law.h"
#include "support_scenes.h"

using namespace bicameral;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    constexpr double NM_UNITS = 65536.0;  // 2^-16 nm
    constexpr uint64_t COARSE_MM = 500;
    constexpr uint64_t FINE_MM = 250;

    // 解き方: 早く着く形(V 4 回 + 刻みをまたぐ共役勾配。T-1086)で釣り合いまで回す(答えの比べ。着く速さは T-1086)
    sim::SupportOptions Options(sim::MixedBeam beam) {
        sim::SupportOptions options;
        options.blocks = {.cycles = 4, .lineSearch = true, .conjugateAcrossTicks = true};
        options.mixedBeam = beam;

        return options;
    }

    // --- 0.5 m の格子のセルの値(分けたセルは細かい 4 つの平均・全部 0.25 m の答えも細かい 4 つの平均)---
    double CoarseCellValue(const sim::SupportLaw& law, uint32_t x, uint32_t up, uint32_t component) {
        if (!law.Mixed() || !law.IsFine(2 * x, 2 * up))
            return (double)law.Value(x, up, component);

        double sum = 0;
        for (uint32_t part = 0; part < 4; ++part)
            sum += (double)law.FineValue(2 * x + (part & 1), 2 * up + (part >> 1), component);

        return sum / 4;
    }

    double ReferenceCellValue(const sim::SupportLaw& reference, uint32_t x, uint32_t up, uint32_t component) {
        double sum = 0;
        for (uint32_t part = 0; part < 4; ++part)
            sum += (double)reference.Value(2 * x + (part & 1), 2 * up + (part >> 1), component);

        return sum / 4;
    }

    // 列 x の上の変位の平均(mm。下向きを正。ある 0.5 m のセルだけ)
    template <typename Read>
    double ColumnSagMm(const sim::SupportScene& scene, uint32_t x, Read&& read) {
        double sum = 0;
        uint32_t count = 0;
        for (uint32_t up = 0; up < scene.height; ++up) {
            if (scene.solid[x + (size_t)scene.width * up] == 0)
                continue;

            sum += read(x, up);
            ++count;
        }

        return count == 0 ? 0 : -sum / count / NM_UNITS / 1.0e6;
    }

    // 0.5 m の場面を 0.25 m のセルの同じ場面に(固定した面は向きの側の細かいセルへ)
    sim::SupportScene Refined(const sim::SupportScene& coarse) {
        sim::SupportScene fine = coarse;
        fine.width = 2 * coarse.width;
        fine.height = 2 * coarse.height;
        fine.cellMm = coarse.cellMm / 2;
        fine.solid.assign((size_t)fine.width * fine.height, 0);
        fine.clamps.assign(fine.solid.size(), 0);
        fine.fine.clear();
        for (uint32_t fup = 0; fup < fine.height; ++fup) {
            for (uint32_t fx = 0; fx < fine.width; ++fx) {
                const size_t from = fx / 2 + (size_t)coarse.width * (fup / 2);
                const size_t to = fx + (size_t)fine.width * fup;
                const uint8_t clamps = coarse.clamps[from];
                fine.solid[to] = coarse.solid[from];
                fine.clamps[to] = (uint8_t)(((fx & 1) == 0 ? clamps & sim::SUPPORT_CLAMP_LEFT : 0) |
                                            ((fx & 1) != 0 ? clamps & sim::SUPPORT_CLAMP_RIGHT : 0) |
                                            ((fup & 1) == 0 ? clamps & sim::SUPPORT_CLAMP_BOTTOM : 0));
            }
        }

        return fine;
    }

    struct Run {
        std::vector<double> path;  // 探る列のたわみ(mm)
        uint64_t hash = 0;
        bool ledgerOk = true;
    };

    template <typename Read>
    Run Settle(sim::SupportLaw& law, const sim::SupportScene& scene, uint32_t probe, uint32_t ticks, Read&& read) {
        Run run;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            law.Step();
            run.path.push_back(ColumnSagMm(scene, probe, [&](uint32_t x, uint32_t up) { return read(law, x, up); }));
            const sim::SupportLedger ledger = law.Ledger();
            run.ledgerOk = run.ledgerOk && ledger.netMn == ledger.reactionMn - ledger.weightMn;
        }

        run.hash = law.Hash();

        return run;
    }

    // 最後の 1/8 の刻みでの動き(釣り合いに着いたか)
    double Drift(const std::vector<double>& path) {
        const double last = path.back();
        const double earlier = path[path.size() - 1 - path.size() / 8];

        return std::abs(last - earlier) / std::max(std::abs(last), 1e-12);
    }

    // 全部の 0.5 m のセルの最大の差 ÷ 最大の変位(成分 x と上)
    template <typename Read>
    double WorstDifference(const sim::SupportScene& scene, const sim::SupportLaw& reference, Read&& read) {
        double worst = 0;
        double largest = 0;
        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                if (scene.solid[x + (size_t)scene.width * up] == 0)
                    continue;

                for (const uint32_t c : {sim::SUPPORT_X, sim::SUPPORT_UP}) {
                    const double expected = ReferenceCellValue(reference, x, up, c);
                    largest = std::max(largest, std::abs(expected));
                    worst = std::max(worst, std::abs(read(x, up, c) - expected));
                }
            }
        }

        return worst / std::max(largest, 1.0);
    }

    struct MixedCase {
        std::string name;
        sim::SupportScene scene;  // 0.5 m(fine の印つき)
        uint32_t probe = 0;       // 探る列(0.5 m の格子)
        bool edgeCheck = true;    // 縁の力の差 ≤ 5 % を確かめる(上の段だけの移り目は T-1207 まで記録だけ)
    };

    const char* BeamName(sim::MixedBeam beam) {
        switch (beam) {
            case sim::MixedBeam::FacePair: return "④ 対の面";
            case sim::MixedBeam::FaceSpring: return "③ 面の点のばね";
            case sim::MixedBeam::FaceLever: return "③' 面の点・梁のずれの硬さ";
            case sim::MixedBeam::FaceOffset: return "② 面ごとの梁";
            default: return "① 中心どうし";
        }
    }

    // 細かいセルが絡む面の縁の力の差 ÷ 全部 0.25 m の縁の力の最大(T-1191)
    struct EdgeError {
        double mixedFace = 0;    // 違う大きさの面
        double fineFace = 0;     // 細かいセルどうしの面
        double clampedFace = 0;  // 固定した面の近くの面(T-1206)
        std::string worst;       // 違う大きさの面の差が最大の面
    };

    EdgeError CompareEdges(const sim::SupportScene& fineScene, const sim::SupportLaw& reference,
                           const sim::SupportLaw& mixed) {
        const auto solid = [&](uint32_t x, uint32_t up) {
            return x < fineScene.width && up < fineScene.height &&
                   fineScene.solid[x + (size_t)fineScene.width * up] != 0;
        };
        // 固定した面の近く: 0.25 m のセルの元の 0.5 m のセルから 0.5 m のセル 1 つ以内に固定した面がある(固定した面の式が大きさで揃っていない分。T-1206)
        const auto clamped = [&](uint32_t x, uint32_t up) {
            const int64_t lowX = std::max<int64_t>(0, (int64_t)(x & ~1u) - 2);
            const int64_t lowUp = std::max<int64_t>(0, (int64_t)(up & ~1u) - 2);
            for (int64_t py = lowUp; py < std::min<int64_t>(fineScene.height, (int64_t)(up & ~1u) + 4); ++py) {
                for (int64_t px = lowX; px < std::min<int64_t>(fineScene.width, (int64_t)(x & ~1u) + 4); ++px) {
                    if (fineScene.clamps[(size_t)px + (size_t)fineScene.width * (size_t)py] != 0)
                        return true;
                }
            }

            return false;
        };

        double largest = 0;
        for (uint32_t up = 0; up < fineScene.height; ++up) {
            for (uint32_t x = 0; x < fineScene.width; ++x) {
                for (uint32_t axis = 0; axis < 2; ++axis) {
                    const sim::SupportFaceForces forces = reference.FaceForces(x, up, axis);
                    largest = std::max(
                        {largest, std::abs((double)forces.lowSideMn), std::abs((double)forces.highSideMn)});
                }
            }
        }

        EdgeError error;
        for (uint32_t up = 0; up < fineScene.height; ++up) {
            for (uint32_t x = 0; x < fineScene.width; ++x) {
                for (uint32_t axis = 0; axis < 2; ++axis) {
                    const uint32_t nextX = x + (axis == 0 ? 1 : 0);
                    const uint32_t nextUp = up + (axis == 1 ? 1 : 0);
                    const bool fineSelf = mixed.IsFine(x, up);
                    const bool fineNext = solid(nextX, nextUp) && mixed.IsFine(nextX, nextUp);
                    if (!solid(x, up) || !solid(nextX, nextUp) || (!fineSelf && !fineNext))
                        continue;

                    const sim::SupportFaceForces expected = reference.FaceForces(x, up, axis);
                    const sim::SupportFaceForces got = mixed.FineFaceForces(x, up, axis);
                    const double differ = std::max(std::abs((double)got.lowSideMn / 2 - (double)expected.lowSideMn),
                                                   std::abs((double)got.highSideMn / 2 - (double)expected.highSideMn)) /
                                          std::max(largest, 1.0);
                    if (clamped(x, up) || clamped(nextX, nextUp)) {
                        error.clampedFace = std::max(error.clampedFace, differ);
                        continue;
                    }

                    if (fineSelf && fineNext) {
                        error.fineFace = std::max(error.fineFace, differ);
                        continue;
                    }

                    if (differ > error.mixedFace) {
                        error.mixedFace = differ;
                        error.worst = std::format(
                            "軸 {}・細かい座標 ({}, {})・混ざる 低 {:.0f} 高 {:.0f}・全部 0.25 m 低 {} 高 {} mN", axis,
                            x, up, (double)got.lowSideMn / 2, (double)got.highSideMn / 2, expected.lowSideMn,
                            expected.highSideMn);
                    }
                }
            }
        }

        return error;
    }

    void TestCase(const MixedCase& each, uint32_t ticks) {
        const sim::SupportScene fineScene = Refined(each.scene);
        sim::SupportLaw reference(fineScene, Options(sim::MixedBeam::FacePair));
        const Run referenceRun = Settle(reference, fineScene, 2 * each.probe, ticks,
                                        [](const sim::SupportLaw& law, uint32_t x, uint32_t up) {
                                            return (double)law.Value(x, up, sim::SUPPORT_UP);
                                        });
        // 0.25 m の探る列は 2 列の平均
        const double referenceSag = (referenceRun.path.back() + ColumnSagMm(fineScene, 2 * each.probe + 1,
                                                                            [&](uint32_t x, uint32_t up) {
                                                                                return (double)reference.Value(
                                                                                    x, up, sim::SUPPORT_UP);
                                                                            })) /
                                    2;
        Expect(referenceRun.ledgerOk, each.name + ": 帳簿(全部 0.25 m)");

        // 全部 0.5 m(分けない)
        sim::SupportScene plain = each.scene;
        plain.fine.clear();
        sim::SupportLaw coarse(plain, Options(sim::MixedBeam::FacePair));
        const Run coarseRun = Settle(coarse, plain, each.probe, ticks,
                                     [](const sim::SupportLaw& law, uint32_t x, uint32_t up) {
                                         return (double)law.Value(x, up, sim::SUPPORT_UP);
                                     });
        Log(Channel::Sim, Level::Info,
            "[T-1120] {}: 全部 0.25 m {:.4f} mm(動き {:.1e})・全部 0.5 m {:.4f} mm = {:.3f}・セルの最大の差 {:.3f}",
            each.name, referenceSag, Drift(referenceRun.path), coarseRun.path.back(),
            coarseRun.path.back() / referenceSag,
            WorstDifference(plain, reference,
                            [&](uint32_t x, uint32_t up, uint32_t c) { return CoarseCellValue(coarse, x, up, c); }));

        for (const sim::MixedBeam beam :
             {sim::MixedBeam::FacePair, sim::MixedBeam::FaceSpring, sim::MixedBeam::FaceLever,
              sim::MixedBeam::FaceOffset, sim::MixedBeam::CenterLine}) {
            const auto read = [](const sim::SupportLaw& law, uint32_t x, uint32_t up) {
                return CoarseCellValue(law, x, up, sim::SUPPORT_UP);
            };
            sim::SupportLaw mixed(each.scene, Options(beam));
            const Run run = Settle(mixed, each.scene, each.probe, ticks, read);
            const double ratio = run.path.back() / referenceSag;
            const double worst = WorstDifference(each.scene, reference, [&](uint32_t x, uint32_t up, uint32_t c) {
                return CoarseCellValue(mixed, x, up, c);
            });
            const bool pass = std::abs(ratio - 1) <= 0.05;
            const EdgeError edges = CompareEdges(fineScene, reference, mixed);
            Log(Channel::Sim, Level::Info,
                "[T-1120] {} {}: {:.4f} mm = 全部 0.25 m の {:.3f}({})・セルの最大の差 {:.3f}・動き {:.1e}・セル {}・"
                "縁の力の差 ÷ 最大 違う大きさの面 {:.2f} %・細かい面 {:.2f} %・固定した面の近く {:.2f} "
                "%(違う大きさの面の最大 {})",
                each.name, BeamName(beam), run.path.back(), ratio, pass ? "±5 % 合格" : "±5 % 不合格", worst,
                Drift(run.path), mixed.CellCount(), 100.0 * edges.mixedFace, 100.0 * edges.fineFace,
                100.0 * edges.clampedFace, edges.worst);
            Expect(run.ledgerOk, std::format("{} {}: 帳簿", each.name, BeamName(beam)));
            Expect(run.path.back() > 0, std::format("{} {}: 重さで下がる", each.name, BeamName(beam)));
            if (beam == sim::MixedBeam::FacePair) {
                Expect(pass, std::format("{} {}: 全部 0.25 m の ±5 %", each.name, BeamName(beam)));
                // 細い梁の曲げ(0.5 m のセル 1〜2 つの厚さ)では粗いセルの精度そのもの(全部 0.5 m と全部 0.25 m の差)が入るので 5 %(T-1191)
                Expect(!each.edgeCheck || (edges.mixedFace <= 0.05 && edges.fineFace <= 0.05),
                       std::format("{} {}: 縁の力の差 ≤ 5 %", each.name, BeamName(beam)));
            }

            sim::SupportLaw again(each.scene, Options(beam));
            for (uint32_t tick = 0; tick < ticks; ++tick)
                again.Step();

            Expect(again.Hash() == run.hash, std::format("{} {}: 決定性", each.name, BeamName(beam)));
        }
    }

    // --- 場面 ---
    using test::Marked;

    std::vector<MixedCase> Cases() {
        const sim::SupportScene shelf = test::Beam(8, 1, COARSE_MM, false);
        const sim::SupportScene cantilever = test::Beam(16, 2, COARSE_MM, false);
        const sim::SupportScene clamped = test::Beam(16, 2, COARSE_MM, true);
        const sim::SupportScene cave = test::Cave(COARSE_MM);

        return {
            {"張り出し 4 m(厚さ 0.5 m)の根 1 m を 0.25 m", Marked(shelf, 0, 2, 0, 1), 7},
            {"片持ち梁 8 × 1 m の根の半分を 0.25 m", Marked(cantilever, 0, 8, 0, 2), 15},
            {"片持ち梁 8 × 1 m の先の半分を 0.25 m", Marked(cantilever, 8, 16, 0, 2), 15},
            {"片持ち梁 8 × 1 m の真ん中 4 m を 0.25 m", Marked(cantilever, 4, 12, 0, 2), 15},
            {"片持ち梁 8 × 1 m の上の段だけ 0.25 m", Marked(cantilever, 0, 16, 1, 2), 15, false},
            {"両端を固定した梁 8 × 1 m の真ん中 3 m を 0.25 m", Marked(clamped, 5, 11, 0, 2), 8},
            {"洞窟の天井(と空洞の縁)を 0.25 m", Marked(cave, 14, 34, 6, 20), 24},
        };
    }

    int RunAll(std::span<char*> arguments) {
        uint32_t ticks = 240;
        for (size_t i = 1; i < arguments.size(); ++i) {
            const std::string_view name = arguments[i];
            if (name == "--ticks" && i + 1 < arguments.size())
                ticks = (uint32_t)std::atoi(arguments[++i]);
        }

        static_assert(FINE_MM * 2 == COARSE_MM);
        for (const MixedCase& each : Cases())
            TestCase(each, ticks);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "support_mixed_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "support_mixed_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = RunAll(std::span(argv, (size_t)argc));
    SingletonFinalizer::Finalize();

    return exitCode;
}
