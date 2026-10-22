// support_fracture_test.cpp — 支えの法則の割れ(sim/support_fracture。研究 R-B2・T-0872・22 §6)の CPU の測り。
// 物質: 弱い岩(硬さ 5 GPa・密度 2600 kg/m³・引きの強さ 500 kPa・押しの強さ 10 MPa・破片の大きさ 0.5 m → 割れ面のエネルギー 25 J/m²・
//   割れの速さ 5 m/s)。縦の面・奥行 1 セル(力は奥行 1 m あたりに直して比べる)。60 刻み/秒。
// 場面(どれも 0.5 m と 0.25 m。2 通りの割れ方〔Strength / Energy〕の両方):
//   A. 壁(幅 1 m・高さ 3 m)から出た片持ち梁(長さ 2 m・厚さ 1 m)の先の 1 m に荷を 1 kN/m ずつ足す(4 秒ごと)→ 根で折れる荷
//   B. 厚さ 0.5 m の棚(幅 6 m)の下を先から 0.5 m ずつ掘る(4 秒ごと)→ 折れる張り出しの長さ
//   C. 岩(16 × 8 m。左右と下を固定)に高さ 4 m・天井の厚さ 1 m の洞窟を真ん中から 1 m ずつ広げて掘る(4 秒ごと)→ 天井が落ちる幅
//   D. すぐ折れるはずの棚(1 セルの厚さ 0.5 m・長さ 4 m。冷えた始まり)→ 割れ始め・分かれるまでの刻み(T-0919 の測りと同じ見方)
// 確かめ(落ちるのは帳簿・決定性・Energy の熱の符号だけ。細かさの差・場所は研究の記録としてログへ。結果は T-0872):
//   - 力の帳簿: 毎刻み Σ セルの上向きの力 = 固定した面の力 − 重さ − 荷(割れた後も)
//   - 割れの帳簿: 解放 = 割れ面 + 熱(整数で厳密)・割れ切った面の割れ面のエネルギー = G × 面積・Energy は熱 ≥ 0(坂を下る時だけ進む)
//   - 決定性: 2 回の実行でハッシュ一致
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <numbers>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "sim/support_fracture.h"

using namespace bicameral;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    // --- 物質(弱い岩)---
    constexpr uint64_t ROCK_KPA = 5000000;
    constexpr uint64_t ROCK_DENSITY = 2600;
    constexpr uint64_t ROCK_TENSILE_KPA = 500;
    constexpr uint64_t ROCK_COMPRESSIVE_KPA = 10000;
    constexpr uint32_t
        SETTLE_TICKS = 240;  // 段ごとに回す刻み(4 秒。壁につながる棚は釣り合いの 9/10 に 140〜260 刻み = D')
    constexpr double GRAVITY = 9.80665;
    constexpr double ONE = sim::SUPPORT_INTACT_FULL;

    sim::SupportStrength Strength() {
        return {.compressiveKpa = ROCK_COMPRESSIVE_KPA, .tensileKpa = ROCK_TENSILE_KPA, .fragmentMm = 500};
    }

    const char* RuleName(sim::FractureRule rule) {
        return rule == sim::FractureRule::Strength ? "強さ" : "エネルギー";
    }

    // 何もない width × height(mm)の箱。solid(x, up) で埋める
    sim::SupportScene Box(uint64_t widthMm, uint64_t heightMm, uint64_t cellMm) {
        sim::SupportScene scene;
        scene.width = (uint32_t)(widthMm / cellMm);
        scene.height = (uint32_t)(heightMm / cellMm);
        scene.cellMm = cellMm;
        scene.stiffnessKpa = ROCK_KPA;
        scene.densityKgPerM3 = ROCK_DENSITY;
        scene.solid.assign((size_t)scene.width * scene.height, 0);
        scene.clamps.assign((size_t)scene.width * scene.height, 0);

        return scene;
    }

    // セルの中心(mm)
    uint64_t Center(uint32_t index, uint64_t cellMm) {
        return index * cellMm + cellMm / 2;
    }

    // --- 場面 ---
    // A: 壁(x < 1 m・全部の高さ。左と下を固定)+ 片持ち梁(x 1〜3 m・高さ 1〜2 m)
    sim::SupportScene Cantilever(uint64_t cellMm) {
        sim::SupportScene scene = Box(3000, 3000, cellMm);
        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                const uint64_t cx = Center(x, cellMm);
                const uint64_t cy = Center(up, cellMm);
                const bool wall = cx < 1000;
                const bool beam = cy > 1000 && cy < 2000;
                const size_t at = x + (size_t)scene.width * up;
                scene.solid[at] = wall || beam ? 1 : 0;
                if (wall && x == 0)
                    scene.clamps[at] |= sim::SUPPORT_CLAMP_LEFT;

                if (wall && up == 0)
                    scene.clamps[at] |= sim::SUPPORT_CLAMP_BOTTOM;
            }
        }

        return scene;
    }

    // B: 幅 6 m・高さ 2 m の岩(左と下を固定)。上の 0.5 m が棚
    sim::SupportScene Ledge(uint64_t cellMm) {
        sim::SupportScene scene = Box(6000, 2000, cellMm);
        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                const size_t at = x + (size_t)scene.width * up;
                scene.solid[at] = 1;
                scene.clamps[at] = (x == 0 ? sim::SUPPORT_CLAMP_LEFT : 0) | (up == 0 ? sim::SUPPORT_CLAMP_BOTTOM : 0);
            }
        }

        return scene;
    }

    // C: 16 × 8 m の岩(左右と下を固定)
    sim::SupportScene Rock(uint64_t cellMm) {
        sim::SupportScene scene = Box(16000, 8000, cellMm);
        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                const size_t at = x + (size_t)scene.width * up;
                scene.solid[at] = 1;
                scene.clamps[at] = (x == 0 ? sim::SUPPORT_CLAMP_LEFT : 0) |
                                   (x + 1 == scene.width ? sim::SUPPORT_CLAMP_RIGHT : 0) |
                                   (up == 0 ? sim::SUPPORT_CLAMP_BOTTOM : 0);
            }
        }

        return scene;
    }

    // D: 壁(x < 1 m・高さ 3 m)+ 厚さ 0.5 m・長さ 4 m の棚(高さ 1.5〜2 m)
    sim::SupportScene Shelf(uint64_t cellMm) {
        sim::SupportScene scene = Box(5000, 3000, cellMm);
        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                const uint64_t cx = Center(x, cellMm);
                const uint64_t cy = Center(up, cellMm);
                const bool wall = cx < 1000;
                const size_t at = x + (size_t)scene.width * up;
                scene.solid[at] = wall || (cy > 1500 && cy < 2000) ? 1 : 0;
                if (wall && x == 0)
                    scene.clamps[at] |= sim::SUPPORT_CLAMP_LEFT;

                if (wall && up == 0)
                    scene.clamps[at] |= sim::SUPPORT_CLAMP_BOTTOM;
            }
        }

        return scene;
    }

    // --- 回す ---
    struct Outcome {
        int startStage = -1;     // 割れ始めた段
        int brokenStage = -1;    // 分かれた段(-1 = 分かれない)
        uint32_t startTick = 0;  // 割れ始めた刻み(通し)
        uint32_t brokenTick = 0;
        std::string where;  // 割れ切った面の場所
        sim::SupportFractureLedger ledger;
        uint64_t hash = 0;
    };

    // 割れ切った面の場所: 縦の面(x の向きの面)の x と、横の面(上の向きの面)の高さ(m)
    std::string Where(const sim::SupportFracture& fracture) {
        const sim::SupportScene& scene = fracture.Scene();
        std::set<uint64_t> vertical;
        std::set<uint64_t> horizontal;
        uint32_t verticalCount = 0;
        uint32_t horizontalCount = 0;
        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                if (fracture.Progress(x, up, 0) >= sim::SUPPORT_INTACT_FULL) {
                    vertical.insert((x + 1) * scene.cellMm);
                    ++verticalCount;
                }

                if (fracture.Progress(x, up, 1) >= sim::SUPPORT_INTACT_FULL) {
                    horizontal.insert((up + 1) * scene.cellMm);
                    ++horizontalCount;
                }
            }
        }

        std::string text = std::format("縦の割れ {} 面 x =", verticalCount);
        for (const uint64_t mm : vertical)
            text += std::format(" {:.2f}", mm / 1000.0);

        text += std::format(" m・横の割れ {} 面 高さ =", horizontalCount);
        for (const uint64_t mm : horizontal)
            text += std::format(" {:.2f}", mm / 1000.0);

        return text + " m";
    }

    // 段ごとに prepare(段) を呼んで SETTLE_TICKS 刻み回す。分かれたら止める。帳簿を毎刻み確かめる
    Outcome RunStages(sim::SupportFracture& fracture, uint32_t stages, const std::function<void(uint32_t)>& prepare,
                      std::string_view label) {
        Outcome outcome;
        uint32_t tick = 0;
        bool forceOk = true;
        for (uint32_t stage = 0; stage < stages && outcome.brokenStage < 0; ++stage) {
            prepare(stage);
            for (uint32_t i = 0; i < SETTLE_TICKS; ++i) {
                fracture.Step();
                ++tick;
                const sim::SupportLedger force = fracture.Law().Ledger();
                forceOk = forceOk && force.netMn == force.reactionMn - force.weightMn;
                const sim::SupportFractureLedger& ledger = fracture.Ledger();
                if (outcome.startStage < 0 && ledger.crackingFaces + ledger.brokenFaces > 0) {
                    outcome.startStage = (int)stage;
                    outcome.startTick = tick;
                }

                if (ledger.separatedCells > 0) {
                    outcome.brokenStage = (int)stage;
                    outcome.brokenTick = tick;
                    break;
                }
            }
        }

        outcome.where = Where(fracture);
        outcome.ledger = fracture.Ledger();
        outcome.hash = fracture.Hash();
        Expect(forceOk, std::format("{}: 力の帳簿(Σ = 固定した面 − 重さ − 荷)", label));
        return outcome;
    }

    // 割れの帳簿の確かめ(Energy は熱 ≥ 0。割れ切った面の割れ面のエネルギー = G × 面積)
    void CheckLedger(const sim::SupportFracture& fracture, const Outcome& outcome, sim::FractureRule rule,
                     std::string_view label) {
        const sim::SupportFractureLedger& ledger = outcome.ledger;
        Expect(ledger.releasedPj == ledger.surfacePj + ledger.heatPj, std::format("{}: 解放 = 割れ面 + 熱", label));
        const uint64_t cellMm = fracture.Scene().cellMm;
        const auto faceSurfacePj = (int64_t)(fracture.FractureEnergyMjPerM2() * cellMm * cellMm * 1000);
        Expect(ledger.surfacePj >= faceSurfacePj * ledger.brokenFaces &&
                   ledger.surfacePj <= faceSurfacePj * (ledger.brokenFaces + ledger.crackingFaces),
               std::format("{}: 割れ面のエネルギー = G × 割れた面積", label));
        // 丸め: 割れ面のエネルギーの表(Q16)の 1 目盛り = G × 面積 ÷ 65536 の数個分まで
        if (rule == sim::FractureRule::Energy)
            Expect(ledger.worstStepHeatPj >= -faceSurfacePj / 16384,
                   std::format("{}: 熱 ≥ 0(坂を下る時だけ進む)", label));
    }

    void LogOutcome(std::string_view label, const sim::SupportFracture& fracture, const Outcome& outcome,
                    std::string_view measure) {
        const sim::SupportFractureLedger& ledger = outcome.ledger;
        Log(Channel::Sim, Level::Info,
            "{}: {} / 割れ始め 段 {}(刻み {})・分かれた 段 {}(刻み {})/ {} / 割れ面 {:.1f} J・熱 {:.1f} J(1 面 1 "
            "刻みの最小 "
            "{:.3f} J)・分かれたセル {}・r {:.2f}",
            label, measure, outcome.startStage, outcome.startTick, outcome.brokenStage, outcome.brokenTick,
            outcome.where, ledger.surfacePj * 1.0e-12, ledger.heatPj * 1.0e-12, ledger.worstStepHeatPj * 1.0e-12,
            ledger.separatedCells, fracture.SofteningRatio() / ONE);
    }

    // --- A. 片持ち梁に荷を足す ---
    // 荷は先の 1 m(x 2〜3 m)の梁のセルに等しく。P(N/m)× 奥行(セル)= P × dx(mN)
    double TestCantilever(uint64_t cellMm, sim::FractureRule rule, uint64_t* hash) {
        const sim::SupportScene scene = Cantilever(cellMm);
        sim::SupportFracture fracture(scene, {}, Strength(), {.rule = rule});
        std::vector<uint32_t> loaded;
        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                const size_t at = x + (size_t)scene.width * up;
                if (scene.solid[at] != 0 && Center(x, cellMm) > 2000)
                    loaded.push_back((uint32_t)at);
            }
        }

        const auto prepare = [&](uint32_t stage) {
            std::vector<int64_t> loads(scene.solid.size(), 0);
            const auto perCell = (int64_t)((uint64_t)stage * 1000 * cellMm / loaded.size());
            for (const uint32_t at : loaded)
                loads[at] = perCell;

            fracture.SetLoads(loads);
        };
        const std::string label = std::format("A 片持ち梁 {} m {}", cellMm / 1000.0, RuleName(rule));
        const Outcome outcome = RunStages(fracture, 150, prepare, label);
        CheckLedger(fracture, outcome, rule, label);
        LogOutcome(label, fracture, outcome,
                   std::format("割れ始めの荷 {} kN/m・折れた荷 {} kN/m", outcome.startStage, outcome.brokenStage));
        if (hash != nullptr)
            *hash = outcome.hash;

        return outcome.brokenStage;
    }

    // --- B. 棚の下を掘る ---
    double TestLedge(uint64_t cellMm, sim::FractureRule rule) {
        const sim::SupportScene scene = Ledge(cellMm);
        sim::SupportFracture fracture(scene, {}, Strength(), {.rule = rule});
        const auto prepare = [&](uint32_t stage) {
            std::vector<std::array<uint32_t, 2>> dig;
            for (uint32_t up = 0; up < scene.height; ++up) {
                for (uint32_t x = 0; x < scene.width; ++x) {
                    if (Center(up, cellMm) < 1500 && Center(x, cellMm) > 6000 - 500 * (uint64_t)stage &&
                        fracture.Scene().solid[x + (size_t)scene.width * up] != 0)
                        dig.push_back({x, up});
                }
            }

            if (!dig.empty())
                fracture.Dig(dig);
        };
        const std::string label = std::format("B 棚の下を掘る {} m {}", cellMm / 1000.0, RuleName(rule));
        const Outcome outcome = RunStages(fracture, 12, prepare, label);
        CheckLedger(fracture, outcome, rule, label);
        LogOutcome(label, fracture, outcome,
                   std::format("割れ始めの張り出し {:.1f} m・折れた張り出し {:.1f} m", outcome.startStage * 0.5,
                               outcome.brokenStage * 0.5));

        return outcome.brokenStage * 0.5;
    }

    // --- C. 洞窟を広げる(高さ 3〜7 m・天井の厚さ 1 m・真ん中 8 m から両側へ 0.5 m ずつ)---
    double TestCave(uint64_t cellMm, sim::FractureRule rule) {
        const sim::SupportScene scene = Rock(cellMm);
        sim::SupportFracture fracture(scene, {}, Strength(), {.rule = rule});
        const auto prepare = [&](uint32_t stage) {
            std::vector<std::array<uint32_t, 2>> dig;
            const uint64_t reach = 500 * (uint64_t)stage;
            for (uint32_t up = 0; up < scene.height; ++up) {
                for (uint32_t x = 0; x < scene.width; ++x) {
                    const uint64_t cx = Center(x, cellMm);
                    const uint64_t cy = Center(up, cellMm);
                    const bool inside = cy > 3000 && cy < 7000 && cx + reach > 8000 && cx < 8000 + reach;
                    if (inside && fracture.Scene().solid[x + (size_t)scene.width * up] != 0)
                        dig.push_back({x, up});
                }
            }

            if (!dig.empty())
                fracture.Dig(dig);
        };
        const std::string label = std::format("C 洞窟 {} m {}", cellMm / 1000.0, RuleName(rule));
        const Outcome outcome = RunStages(fracture, 16, prepare, label);
        CheckLedger(fracture, outcome, rule, label);
        LogOutcome(label, fracture, outcome,
                   std::format("割れ始めの幅 {} m・落ちた幅 {} m", outcome.startStage, outcome.brokenStage));

        return outcome.brokenStage;
    }

    // --- D. すぐ折れるはずの棚(冷えた始まり)---
    void TestShelf(uint64_t cellMm, sim::FractureRule rule) {
        const sim::SupportScene scene = Shelf(cellMm);
        sim::SupportFracture fracture(scene, {}, Strength(), {.rule = rule});
        const std::string label = std::format("D すぐ折れる棚 {} m {}", cellMm / 1000.0, RuleName(rule));
        const Outcome outcome = RunStages(fracture, 10, [](uint32_t) {}, label);
        CheckLedger(fracture, outcome, rule, label);
        LogOutcome(label, fracture, outcome,
                   std::format("割れ始め {} 刻み({:.3f} 秒)・分かれた {} 刻み({:.3f} 秒)", outcome.startTick,
                               outcome.startTick / 60.0, outcome.brokenTick, outcome.brokenTick / 60.0));
    }

    // 釣り合いに着く速さ(割れない強さで): 棚の根の上の縁の強さの比が最後の値の 1/2・9/10 を超える刻み(T-0919 の測りと同じ見方)
    void TestShelfSettle(uint64_t cellMm) {
        const sim::SupportScene scene = Shelf(cellMm);
        sim::SupportStrength strong = Strength();
        strong.tensileKpa *= 1000;
        strong.compressiveKpa *= 1000;
        sim::SupportFracture fracture(scene, {}, strong, {.rule = sim::FractureRule::Strength});
        const auto rootX = (uint32_t)(1000 / cellMm) - 1;
        const auto rootUp = (uint32_t)(2000 / cellMm) - 1;
        std::vector<double> path;
        for (uint32_t tick = 0; tick < 600; ++tick) {
            fracture.Step();
            path.push_back(fracture.FailureIndex(rootX, rootUp, 0) / ONE);
        }

        const double last = path.back();
        const auto reach = [&](double part) {
            for (size_t tick = 0; tick < path.size(); ++tick) {
                if (path[tick] >= part * last)
                    return tick + 1;
            }

            return path.size();
        };
        Log(Channel::Sim, Level::Info,
            "D' 棚の根の縁(壁につながる棚・割れない強さ){} m: 最後の比 {:.0f} 倍の引きの強さ(×1000 の強さで "
            "{:.3f})・1/2 に {} 刻み・9/10 に {} 刻み・"
            "1 刻み {:.3f}・5 刻み {:.3f}・10 刻み {:.3f}・30 刻み {:.3f}(最後の値に対して)",
            cellMm / 1000.0, last * 1000, last, reach(0.5), reach(0.9), path[0] / last, path[4] / last, path[9] / last,
            path[29] / last);
    }

    // 梁の式・強さの形の見込み(記録)
    void LogExpectations() {
        const double weight = ROCK_DENSITY * GRAVITY / 1000.0;  // kN/m³
        const double tensile = ROCK_TENSILE_KPA;
        const double cantilever = (tensile / 6.0 - weight * 1.0 * 4.0 / 2.0) / 1.5;
        const double ledge = std::sqrt(tensile * 0.5 / (3.0 * weight));
        const double caveEnds = std::sqrt(2.0 * 1.0 * tensile / weight);
        const double caveMiddle = std::sqrt(4.0 * 1.0 * tensile / weight);
        const double compressive = ROCK_COMPRESSIVE_KPA;
        const double cohesion = std::sqrt(compressive * tensile) / 2.0;
        const double angle = std::asin((compressive - tensile) / (compressive + tensile)) * 180.0 / std::numbers::pi;
        const double stoneAngle = std::asin((100000.0 - 5000.0) / 105000.0) * 180.0 / std::numbers::pi;
        Log(Channel::Sim, Level::Info,
            "見込み(梁の式・引きの強さで): A 折れる荷 {:.1f} kN/m / B 張り出し {:.2f} m / C 両端固定の梁の端 {:.2f} "
            "m・真ん中 {:.2f} m",
            cantilever, ledge, caveEnds, caveMiddle);
        Log(Channel::Sim, Level::Info,
            "強さの形(22 §3.3): 弱い岩 c = {:.0f} kPa・φ = {:.1f}° / 石(100 MPa・5 MPa)φ = "
            "{:.1f}°(瓦礫の崩れる角度の既定にすると急すぎる)",
            cohesion, angle, stoneAngle);
    }

    int RunAll() {
        LogExpectations();
        for (const sim::FractureRule rule : {sim::FractureRule::Strength, sim::FractureRule::Energy}) {
            for (const uint64_t cellMm : {500ull, 250ull}) {
                TestShelf(cellMm, rule);
                if (rule == sim::FractureRule::Strength)
                    TestShelfSettle(cellMm);

                TestCantilever(cellMm, rule, nullptr);
                TestLedge(cellMm, rule);
                TestCave(cellMm, rule);
            }
        }

        // 決定性
        uint64_t first = 0;
        uint64_t second = 0;
        TestCantilever(500, sim::FractureRule::Energy, &first);
        TestCantilever(500, sim::FractureRule::Energy, &second);
        Expect(first == second, "決定性: 2 回の実行でハッシュ一致");

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "support_fracture_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "support_fracture_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = RunAll();
    SingletonFinalizer::Finalize();

    return exitCode;
}
