// support_fracture_test.cpp — 支えの法則の割れ(sim/support_fracture。研究 R-B2・T-0872・22 §6)の CPU の測り。
// 物質: 弱い岩(硬さ 5 GPa・密度 2600 kg/m³・引きの強さ 500 kPa・押しの強さ 10 MPa・破片の大きさ 0.5 m → 割れ面のエネルギー 25 J/m²・
//   割れの速さ 5 m/s)。縦の面・奥行 1 セル(力は奥行 1 m あたりに直して比べる)。60 刻み/秒。
// 場面(どれも 0.5 m と 0.25 m。3 通りの割れ方〔Strength / Energy / Window(T-1042・ℓ = 0.5 m の区間の平均)〕。Window は A・B を 0.125 m でも。
// 引数 --window-sweep で ℓ = 0.5・1・2 m の比べ〔T-1042 の続き。A・B は 0.5 / 0.25 / 0.125 m・C〔24 m の岩〕と D は 0.5 / 0.25 m〕):
//   A. 壁(幅 1 m・高さ 3 m)から出た片持ち梁(長さ 2 m・厚さ 1 m)の先の 1 m に荷を 1 kN/m ずつ足す(4 秒ごと)→ 根で折れる荷
//   B. 厚さ 0.5 m の棚(幅 6 m)の下を先から 0.5 m ずつ掘る(4 秒ごと)→ 折れる張り出しの長さ
//   C. 岩(16 × 8 m。左右と下を固定)に高さ 4 m・天井の厚さ 1 m の洞窟を真ん中から 1 m ずつ広げて掘る(4 秒ごと)→ 天井が落ちる幅
//   D. すぐ折れるはずの棚(厚さ 0.5 m・長さ 8 m。冷えた始まり)→ 割れ始め・分かれるまでの刻み(T-0919 の測りと同じ見方)
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

#include "core/aliases.h"
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

    // windowMm = 割れの判定の長さ ℓ(0 = 破片の大きさ 0.5 m)。ℓ = 硬さ × 割れ面のエネルギー ÷ 引きの強さ²(D-644)なので、
    // ℓ を変える = 割れ面のエネルギー G = ℓ × σt² ÷ 硬さ を書く(ℓ 0.5 / 1 / 2 m = 25 / 50 / 100 J/m²。G が変わると軟化の傾きも変わる)
    sim::SupportStrength Strength(uint64_t windowMm = 0) {
        const uint64_t energyMj = windowMm * ROCK_TENSILE_KPA * ROCK_TENSILE_KPA * 1000 / ROCK_KPA;

        return {.compressiveKpa = ROCK_COMPRESSIVE_KPA,
                .tensileKpa = ROCK_TENSILE_KPA,
                .fractureEnergyMjPerM2 = energyMj,
                .fragmentMm = 500};
    }

    std::string RuleName(sim::FractureRule rule, uint64_t windowMm = 0) {
        if (rule == sim::FractureRule::Strength)
            return "強さ";

        if (rule == sim::FractureRule::Energy)
            return "エネルギー";

        return std::format("ℓ {:.1f} m の平均", (windowMm != 0 ? windowMm : 500) / 1000.0);
    }

    // 3 本目の見込みの倍率: 厚さ h の面の直線の分布(縁 σ・真ん中 0)の引きの側を、縁からそろえた ℓ の区間で平均した値が σ ÷ 倍率。
    // 区間は面のある所だけで数える(ℓ ≥ h なら区間 = 面全体 = σ/4)
    double WindowFactor(double thicknessM, double windowM) {
        if (windowM >= thicknessM)
            return 4.0;

        if (windowM <= thicknessM / 2.0)
            return 1.0 / (1.0 - windowM / thicknessM);

        return 4.0 * windowM / thicknessM;
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
    // 梁の形(既定は A: 高さ 1〜2 m・厚さ 1 m・長さ 2 m。壁は幅 1 m で、梁の上下に 1 m ずつ)
    struct BeamShape {
        uint64_t bottomMm = 1000;
        uint64_t thicknessMm = 1000;
        uint64_t lengthMm = 2000;
        uint64_t liftMm =
            0;  // 場面全体を上へずらす(下は空。壁の下の面は壁の一番下のセルで固定。置く場所によらないかの確かめ T-1069)
    };

    sim::SupportScene Cantilever(uint64_t cellMm, const BeamShape& shape = {}) {
        const uint64_t heightMm = shape.bottomMm + shape.thicknessMm + 1000;
        sim::SupportScene scene = Box(1000 + shape.lengthMm, shape.liftMm + heightMm, cellMm);
        const auto groundRow = (uint32_t)(shape.liftMm / cellMm);
        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                const uint64_t cx = Center(x, cellMm);
                const uint64_t cy = Center(up, cellMm);
                if (cy < shape.liftMm)
                    continue;

                const uint64_t above = cy - shape.liftMm;
                const bool wall = cx < 1000;
                const bool beam = above > shape.bottomMm && above < shape.bottomMm + shape.thicknessMm;
                const size_t at = x + (size_t)scene.width * up;
                scene.solid[at] = wall || beam ? 1 : 0;
                if (wall && x == 0)
                    scene.clamps[at] |= sim::SUPPORT_CLAMP_LEFT;

                if (wall && up == groundRow)
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

    // C: 16(または widthMm)× 8 m の岩(左右と下を固定)
    sim::SupportScene Rock(uint64_t cellMm, uint64_t widthMm = 16000) {
        sim::SupportScene scene = Box(widthMm, 8000, cellMm);
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

    // D: 壁(x < 1 m・高さ 3 m)+ 厚さ 0.5 m・長さ 8 m の棚(高さ 1.5〜2 m。3 本目の見込み 3.6 m の 2 倍強)
    sim::SupportScene Shelf(uint64_t cellMm) {
        sim::SupportScene scene = Box(9000, 3000, cellMm);
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
        uint32_t ticks = 0;    // 回した刻み
        double seconds = 0.0;  // 回すのにかかった時間(CPU・release)
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
        const auto begin = chr::steady_clock::now();
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

        outcome.ticks = tick;
        outcome.seconds = chr::duration<double>(chr::steady_clock::now() - begin).count();
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
        if (rule != sim::FractureRule::Strength)
            Expect(ledger.worstStepHeatPj >= -faceSurfacePj / 16384,
                   std::format("{}: 熱 ≥ 0(坂を下る時だけ進む)", label));
    }

    void LogOutcome(std::string_view label, const sim::SupportFracture& fracture, const Outcome& outcome,
                    std::string_view measure) {
        const sim::SupportFractureLedger& ledger = outcome.ledger;
        Log(Channel::Sim, Level::Info,
            "{}: {} / 割れ始め 段 {}(刻み {})・分かれた 段 {}(刻み {})/ {} / 割れ面 {:.1f} J・熱 {:.1f} J(1 面 1 "
            "刻みの最小 "
            "{:.3f} J)・分かれたセル {}・r {:.2f}・{} 刻みに {:.1f} 秒",
            label, measure, outcome.startStage, outcome.startTick, outcome.brokenStage, outcome.brokenTick,
            outcome.where, ledger.surfacePj * 1.0e-12, ledger.heatPj * 1.0e-12, ledger.worstStepHeatPj * 1.0e-12,
            ledger.separatedCells, fracture.SofteningRatio() / ONE, outcome.ticks, outcome.seconds);
    }

    // --- A. 片持ち梁に荷を足す ---
    // 段の番号から荷(kN/m。-1 = 割れなかった)
    int KnOf(uint32_t startKn, int stage) {
        return stage < 0 ? -1 : (int)startKn + stage;
    }

    // 荷は先の 1 m(x 2〜3 m)の梁のセルに等しく。P(N/m)× 奥行(セル)= P × dx(mN)。
    // startKn: 最初の段の荷(kN/m。見込みの 6 割ほどから始めて刻みを省く。最初の段で割れ始めたら下げて測り直す印をログに出す)
    double TestCantilever(uint64_t cellMm, sim::FractureRule rule, uint64_t* hash, uint64_t windowMm = 0,
                          uint32_t startKn = 0, const BeamShape& shape = {}) {
        const sim::SupportScene scene = Cantilever(cellMm, shape);
        sim::SupportFracture fracture(scene, {}, Strength(windowMm), {.rule = rule});
        std::vector<uint32_t> loaded;
        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                const size_t at = x + (size_t)scene.width * up;
                if (scene.solid[at] != 0 && Center(x, cellMm) > shape.lengthMm)
                    loaded.push_back((uint32_t)at);
            }
        }

        const auto prepare = [&](uint32_t stage) {
            std::vector<int64_t> loads(scene.solid.size(), 0);
            const auto perCell = (int64_t)((uint64_t)(startKn + stage) * 1000 * cellMm / loaded.size());
            for (const uint32_t at : loaded)
                loads[at] = perCell;

            fracture.SetLoads(loads);
        };
        const std::string label = std::format("A 片持ち梁 {} m {}(厚さ {:.1f} m・高さ {:.1f} m から・長さ {:.1f} m)",
                                              cellMm / 1000.0, RuleName(rule, windowMm), shape.thicknessMm / 1000.0,
                                              shape.bottomMm / 1000.0, shape.lengthMm / 1000.0);
        const Outcome outcome = RunStages(fracture, 400, prepare, label);
        CheckLedger(fracture, outcome, rule, label);
        LogOutcome(
            label, fracture, outcome,
            std::format(
                "割れ始めの荷 {} kN/m・折れた荷 {} kN/m{}", KnOf(startKn, outcome.startStage),
                KnOf(startKn, outcome.brokenStage),
                startKn != 0 && outcome.startStage == 0 ? "(最初の段で割れ始めた: 始めの荷を下げて測り直す)" : ""));
        if (hash != nullptr)
            *hash = outcome.hash;

        return outcome.brokenStage < 0 ? -1.0 : (double)(startKn + outcome.brokenStage);
    }

    // --- B. 棚の下を掘る ---
    double TestLedge(uint64_t cellMm, sim::FractureRule rule, uint64_t windowMm = 0) {
        const sim::SupportScene scene = Ledge(cellMm);
        sim::SupportFracture fracture(scene, {}, Strength(windowMm), {.rule = rule});
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
        const std::string label = std::format("B 棚の下を掘る {} m {}", cellMm / 1000.0, RuleName(rule, windowMm));
        const Outcome outcome = RunStages(fracture, 12, prepare, label);
        CheckLedger(fracture, outcome, rule, label);
        LogOutcome(label, fracture, outcome,
                   std::format("割れ始めの張り出し {:.1f} m・折れた張り出し {:.1f} m", outcome.startStage * 0.5,
                               outcome.brokenStage * 0.5));

        return outcome.brokenStage < 0 ? -1.0 : outcome.brokenStage * 0.5;
    }

    // --- C. 洞窟を広げる(高さ 3〜7 m・天井の厚さ 1 m・真ん中〔16 m の岩なら 8 m〕から両側へ 0.5 m ずつ)---
    double TestCave(uint64_t cellMm, sim::FractureRule rule, uint64_t windowMm = 0, uint64_t widthMm = 16000) {
        const sim::SupportScene scene = Rock(cellMm, widthMm);
        sim::SupportFracture fracture(scene, {}, Strength(windowMm), {.rule = rule});
        const uint64_t middle = widthMm / 2;
        const auto prepare = [&](uint32_t stage) {
            std::vector<std::array<uint32_t, 2>> dig;
            const uint64_t reach = 500 * (uint64_t)stage;
            for (uint32_t up = 0; up < scene.height; ++up) {
                for (uint32_t x = 0; x < scene.width; ++x) {
                    const uint64_t cx = Center(x, cellMm);
                    const uint64_t cy = Center(up, cellMm);
                    const bool inside = cy > 3000 && cy < 7000 && cx + reach > middle && cx < middle + reach;
                    if (inside && fracture.Scene().solid[x + (size_t)scene.width * up] != 0)
                        dig.push_back({x, up});
                }
            }

            if (!dig.empty())
                fracture.Dig(dig);
        };
        const std::string label = std::format("C 洞窟 {} m {}(岩の幅 {} m)", cellMm / 1000.0, RuleName(rule, windowMm),
                                              widthMm / 1000);
        const Outcome outcome = RunStages(fracture, (uint32_t)(widthMm / 1000), prepare, label);
        CheckLedger(fracture, outcome, rule, label);
        LogOutcome(label, fracture, outcome,
                   std::format("割れ始めの幅 {} m・落ちた幅 {} m", outcome.startStage, outcome.brokenStage));

        return outcome.brokenStage < 0 ? -1.0 : (double)outcome.brokenStage;
    }

    // --- D. すぐ折れるはずの棚(冷えた始まり)---
    void TestShelf(uint64_t cellMm, sim::FractureRule rule, uint64_t windowMm = 0) {
        const sim::SupportScene scene = Shelf(cellMm);
        sim::SupportFracture fracture(scene, {}, Strength(windowMm), {.rule = rule});
        const std::string label = std::format("D すぐ折れる棚 {} m {}", cellMm / 1000.0, RuleName(rule, windowMm));
        const Outcome outcome = RunStages(fracture, 10, [](uint32_t) {}, label);
        CheckLedger(fracture, outcome, rule, label);
        LogOutcome(label, fracture, outcome,
                   std::format("割れ始め {} 刻み({:.3f} 秒)・分かれた {} 刻み({:.3f} 秒)", outcome.startTick,
                               outcome.startTick / 60.0, outcome.brokenTick, outcome.brokenTick / 60.0));
    }

    // T-1041 の場面: 長さ 8 m の棚(高さ bottomMm〜topMm)。wall なら壁(x < 1 m・高さ 3 m。左と下を固定)につながり、
    // そうでなければ棚の根のセル(x 1〜1 m + セル)の左の面を固定する(T-0919 の棚と同じ「根が固定した面」)
    // shiftMm: 場面全体を右へずらす(左は空。壁の左の面は壁の一番左のセルで固定。粗い段の箱と継ぎ目の位置だけを変える)
    sim::SupportScene SettleShelf(uint64_t cellMm, uint64_t bottomMm, uint64_t topMm, bool wall, uint64_t wallMm = 1000,
                                  uint64_t shiftMm = 0) {
        sim::SupportScene scene = Box(shiftMm + wallMm + 8000, 3000, cellMm);
        const auto rootColumn = (uint32_t)((shiftMm + wallMm) / cellMm);
        const auto wallColumn = (uint32_t)(shiftMm / cellMm);
        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                const uint64_t cx = Center(x, cellMm);
                const uint64_t cy = Center(up, cellMm);
                const bool inWall = wall && cx > shiftMm && cx < shiftMm + wallMm;
                const bool shelf = cx > shiftMm + wallMm && cy > bottomMm && cy < topMm;
                const size_t at = x + (size_t)scene.width * up;
                scene.solid[at] = inWall || shelf ? 1 : 0;
                if (inWall && x == wallColumn)
                    scene.clamps[at] |= sim::SUPPORT_CLAMP_LEFT;

                if (inWall && up == 0)
                    scene.clamps[at] |= sim::SUPPORT_CLAMP_BOTTOM;

                if (!wall && shelf && x == rootColumn)
                    scene.clamps[at] |= sim::SUPPORT_CLAMP_LEFT;
            }
        }

        return scene;
    }

    // 釣り合いに着く速さ(割れない強さで): 棚の根の上の縁の強さの比が最後の値の 1/2・9/10 を超える刻み(T-0919 の測りと同じ見方)。
    // 根の面 = セル (rootX, rootUp) と +x の隣の間
    void SettleOnce(const sim::SupportScene& scene, const sim::SupportOptions& options, uint32_t rootX, uint32_t rootUp,
                    std::string_view label, uint32_t ticks = 600) {
        sim::SupportStrength strong = Strength();
        strong.tensileKpa *= 1000;
        strong.compressiveKpa *= 1000;
        sim::SupportFracture fracture(scene, options, strong, {.rule = sim::FractureRule::Strength});
        std::vector<double> path;
        const auto begin = chr::steady_clock::now();
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            fracture.Step();
            path.push_back(fracture.FailureIndex(rootX, rootUp, 0) / ONE);
        }

        const double seconds = chr::duration<double>(chr::steady_clock::now() - begin).count();
        const double last = path.back();
        const auto reach = [&](double part) {
            for (size_t tick = 0; tick < path.size(); ++tick) {
                if (path[tick] >= part * last)
                    return tick + 1;
            }

            return path.size();
        };
        Log(Channel::Sim, Level::Info,
            "{}: 最後の比 {:.0f} 倍の引きの強さ(×1000 の強さで {:.3f})・1/2 に {} 刻み・9/10 に {} 刻み・99/100 に {} "
            "刻み・"
            "1 刻み {:.3f}・5 刻み {:.3f}・10 刻み {:.3f}・30 刻み {:.3f}・{} 刻み {:.3f}(最後の値に対して)・{} 刻みに "
            "{:.2f} 秒",
            label, last * 1000, last, reach(0.5), reach(0.9), reach(0.99), path[0] / last, path[4] / last,
            path[9] / last, path[29] / last, ticks / 4, path[ticks / 4 - 1] / last, ticks, seconds);
    }

    void TestShelfSettle(uint64_t cellMm) {
        const auto rootX = (uint32_t)(1000 / cellMm) - 1;
        const auto rootUp = (uint32_t)(2000 / cellMm) - 1;
        SettleOnce(Shelf(cellMm), {}, rootX, rootUp,
                   std::format("D' 棚の根の縁(壁につながる棚・割れない強さ){} m", cellMm / 1000.0));
    }

    // --- T-1041: 壁につながる棚の釣り合いが遅い理由の見立て(V の回数・掃き出し・粗い段の箱のそろい方・根が固定した面)---
    void RunSettleStudy() {
        // 長さ 8 m の棚は 600 刻みでも着ききらない(D' の 9/10 が 500 刻みほど)ので、40 秒回して最後の値を取る
        constexpr uint32_t STUDY_TICKS = 2400;
        // 比べの元: 前の既定(V 2 回・共役勾配なし)。今の既定(V 4 回 + 共役勾配。T-1085)は「既定」の行
        sim::SupportOptions plain;
        plain.blocks = sim::EquilibriumBlockOptions{};
        for (const uint64_t cellMm : {500ull, 250ull}) {
            const auto wallRootX = (uint32_t)(1000 / cellMm) - 1;
            const auto clampRootX = (uint32_t)(1000 / cellMm);
            const auto topUp = [&](uint64_t topMm) {
                return (uint32_t)(topMm / cellMm) - 1;
            };
            const double cell = cellMm / 1000.0;
            const sim::SupportScene wallShelf = SettleShelf(cellMm, 1500, 2000, true);
            SettleOnce(wallShelf, {}, wallRootX, topUp(2000),
                       std::format("T-1041 {} m 壁の棚 0.5 m(既定 = V 4 回 + 共役勾配)", cell), STUDY_TICKS);

            SettleOnce(wallShelf, plain, wallRootX, topUp(2000),
                       std::format("T-1041 {} m 壁の棚 0.5 m(V 2 回・前の既定)", cell), STUDY_TICKS);
            for (const uint32_t cycles : {4u, 8u}) {
                sim::SupportOptions options = plain;
                options.blocks.cycles = cycles;
                SettleOnce(wallShelf, options, wallRootX, topUp(2000),
                           std::format("T-1041 {} m 壁の棚 0.5 m(V {} 回)", cell, cycles), STUDY_TICKS);
            }

            // T-1068: 2 回目の V の直しの向きにエネルギー最小の歩幅へ伸ばす(lineSearch)
            for (const uint32_t cycles : {2u, 4u}) {
                sim::SupportOptions options = plain;
                options.blocks.cycles = cycles;
                options.blocks.lineSearch = true;
                SettleOnce(wallShelf, options, wallRootX, topUp(2000),
                           std::format("T-1041 {} m 壁の棚 0.5 m(V {} 回 + 歩幅)", cell, cycles), STUDY_TICKS);
                SettleOnce(SettleShelf(cellMm, 1500, 2000, false), options, clampRootX, topUp(2000),
                           std::format("T-1041 {} m 根が固定した面の棚 0.5 m(V {} 回 + 歩幅)", cell, cycles),
                           STUDY_TICKS);
            }

            sim::SupportOptions smooth = plain;
            smooth.blocks.preSmooth = 4;
            smooth.blocks.postSmooth = 4;
            SettleOnce(wallShelf, smooth, wallRootX, topUp(2000),
                       std::format("T-1041 {} m 壁の棚 0.5 m(掃き出し 4 / 4)", cell), STUDY_TICKS);

            sim::SupportOptions coarsest = plain;
            coarsest.blocks.coarsestSweeps = 32;
            SettleOnce(wallShelf, coarsest, wallRootX, topUp(2000),
                       std::format("T-1041 {} m 壁の棚 0.5 m(最も粗い段 32 回)", cell), STUDY_TICKS);

            SettleOnce(SettleShelf(cellMm, 1500, 2000, false), plain, clampRootX, topUp(2000),
                       std::format("T-1041 {} m 根が固定した面の棚 0.5 m", cell), STUDY_TICKS);
            // 粗い段の箱のそろい方: 厚さ 1 m の棚を 1〜2 m(粗い段の箱にそろう)と 1.5〜2.5 m(半分ずれる)に置く
            SettleOnce(SettleShelf(cellMm, 1000, 2000, true), plain, wallRootX, topUp(2000),
                       std::format("T-1041 {} m 壁の棚 1 m・高さ 1〜2 m(箱にそろう)", cell), STUDY_TICKS);
            SettleOnce(SettleShelf(cellMm, 1500, 2500, true), plain, wallRootX, topUp(2500),
                       std::format("T-1041 {} m 壁の棚 1 m・高さ 1.5〜2.5 m(箱から半分ずれる)", cell), STUDY_TICKS);
            // 壁と棚の継ぎ目を粗い段の箱の境にそろえる(壁の幅 2 m・4 m。0.5 m なら継ぎ目はセル 4・8 = 2・3 段上までの箱の境)
            for (const uint64_t wallMm : {2000ull, 4000ull}) {
                SettleOnce(SettleShelf(cellMm, 1500, 2000, true, wallMm), plain, (uint32_t)(wallMm / cellMm) - 1,
                           topUp(2000), std::format("T-1041 {} m 壁の棚 0.5 m・壁の幅 {} m", cell, wallMm / 1000),
                           STUDY_TICKS);
            }

            // 壁の幅は 1 m のまま、場面を右へ 1 m・3 m ずらす(継ぎ目が壁の幅 2 m・4 m と同じセルに来る。広さの効きと箱の境の効きを分ける)
            for (const uint64_t shiftMm : {1000ull, 3000ull}) {
                SettleOnce(SettleShelf(cellMm, 1500, 2000, true, 1000, shiftMm), {},
                           (uint32_t)((shiftMm + 1000) / cellMm) - 1, topUp(2000),
                           std::format("T-1041 {} m 壁の棚 0.5 m・右へ {} m ずらす", cell, shiftMm / 1000),
                           STUDY_TICKS);
            }

            SettleOnce(SettleShelf(cellMm, 1000, 2000, false), plain, clampRootX, topUp(2000),
                       std::format("T-1041 {} m 根が固定した面の棚 1 m・高さ 1〜2 m", cell), STUDY_TICKS);
        }
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
        // 3 本目(ℓ = 0.5 m の区間の引きの側の平均 = 強さ): 直線の分布の縁 σ から、厚さ h の梁の外の ℓ の平均は σ (1 − ℓ/h)(ℓ ≤ h/2)・
        // 厚さ ℓ の棚(全部が 1 区間)は σ/4
        Log(Channel::Sim, Level::Info,
            "見込み(3 本目・ℓ = 0.5 m の平均): A 折れる荷 {:.1f} kN/m / B 張り出し {:.2f} m / C 端 {:.2f} m・真ん中 "
            "{:.2f} m",
            (2.0 * tensile / 6.0 - weight * 4.0 / 2.0) / 1.5, std::sqrt(4.0 * tensile * 0.5 / (3.0 * weight)),
            std::sqrt(2.0 * 2.0 * tensile / weight), std::sqrt(4.0 * 2.0 * tensile / weight));
        Log(Channel::Sim, Level::Info,
            "強さの形(22 §3.3): 弱い岩 c = {:.0f} kPa・φ = {:.1f}° / 石(100 MPa・5 MPa)φ = "
            "{:.1f}°(瓦礫の崩れる角度の既定にすると急すぎる)",
            cohesion, angle, stoneAngle);
    }

    // --- ℓ の比べ(T-1042 の続き): ℓ = 0.5・1・2 m で A・B・C・D。見込みは梁の式に WindowFactor を掛けた引きの強さで ---
    void RunWindowSweep() {
        const double weight = ROCK_DENSITY * GRAVITY / 1000.0;  // kN/m³
        const double tensile = ROCK_TENSILE_KPA;
        // C の岩の幅: ℓ ≥ 1 m の見込み(端 12.5 m・真ん中 17.7 m)が入るように 16 m から広げた
        constexpr uint64_t CAVE_WIDTH_MM = 24000;
        std::string summary;
        for (const uint64_t windowMm : {500ull, 1000ull, 2000ull}) {
            const double windowM = windowMm / 1000.0;
            const double beamFactor = WindowFactor(1.0, windowM);   // A の梁・C の天井(厚さ 1 m)
            const double shelfFactor = WindowFactor(0.5, windowM);  // B・D の棚(厚さ 0.5 m)
            const double expectA = (beamFactor * tensile / 6.0 - weight * 4.0 / 2.0) / 1.5;
            const double expectB = std::sqrt(shelfFactor * tensile * 0.5 / (3.0 * weight));
            const double expectEnds = std::sqrt(2.0 * beamFactor * tensile / weight);
            const double expectMiddle = std::sqrt(4.0 * beamFactor * tensile / weight);
            Log(Channel::Sim, Level::Info,
                "見込み(ℓ {:.1f} m・梁 ×{:.2f}・棚 ×{:.2f}): A {:.1f} kN/m / B {:.2f} m / "
                "C 端 {:.2f} m・真ん中 {:.2f} m / D の根の応力 ÷ ℓ の強さ {:.1f} 倍",
                windowM, beamFactor, shelfFactor, expectA, expectB, expectEnds, expectMiddle,
                std::pow(8.0 / expectB, 2.0));

            const auto startKn = (uint32_t)(expectA * 0.6);
            std::string line = std::format("ℓ {:.1f} m: A", windowM);
            for (const uint64_t cellMm : {500ull, 250ull, 125ull}) {
                const double load = TestCantilever(cellMm, sim::FractureRule::Window, nullptr, windowMm, startKn);
                line += std::format(" {:.0f}", load);
            }

            line += std::format("(見込み {:.1f})・B", expectA);
            for (const uint64_t cellMm : {500ull, 250ull, 125ull})
                line += std::format(" {:.1f}", TestLedge(cellMm, sim::FractureRule::Window, windowMm));

            line += std::format("(見込み {:.2f})・C", expectB);
            for (const uint64_t cellMm : {500ull, 250ull})
                line += std::format(" {:.0f}", TestCave(cellMm, sim::FractureRule::Window, windowMm, CAVE_WIDTH_MM));

            line += std::format("(見込み 端 {:.1f}・真ん中 {:.1f})", expectEnds, expectMiddle);
            for (const uint64_t cellMm : {500ull, 250ull})
                TestShelf(cellMm, sim::FractureRule::Window, windowMm);

            summary += line + "\n";
        }

        Log(Channel::Sim, Level::Info, "ℓ の比べ(A 0.5 / 0.25 / 0.125 m・B 同・C 0.5 / 0.25 m):\n{}", summary);
    }

    // --- 厚い梁(T-1042 の続き): ℓ が梁の厚さより短い場面で ℓ = 0.5・1・2 m の差を見る ---
    // 厚さ 2 m・長さ 3 m の片持ち梁(先の 1 m に荷。腕 2.5 m)。高さ 2〜4 m(2 m の区間にそろう)と 1〜3 m(2 m の区間の境が梁の真ん中)
    void RunThickBeam() {
        const double weight = ROCK_DENSITY * GRAVITY / 1000.0;  // kN/m³
        const double tensile = ROCK_TENSILE_KPA;
        std::string summary;
        const auto expect = [&](double windowM) {
            const double factor = WindowFactor(2.0, windowM);
            return (factor * tensile * 4.0 / 6.0 - weight * 2.0 * 9.0 / 2.0) / 2.5;
        };
        for (const uint64_t windowMm : {500ull, 1000ull, 2000ull}) {
            const double expected = expect(windowMm / 1000.0);
            std::string line = std::format("ℓ {:.1f} m(そろう): A2", windowMm / 1000.0);
            for (const uint64_t cellMm : {500ull, 250ull}) {
                const double load = TestCantilever(cellMm, sim::FractureRule::Window, nullptr, windowMm,
                                                   (uint32_t)(expected * 0.6),
                                                   {.bottomMm = 2000, .thicknessMm = 2000, .lengthMm = 3000});
                line += std::format(" {:.0f}", load);
            }

            summary += line + std::format("(見込み {:.1f})\n", expected);
        }

        // ずれる置き方(ℓ 2 m の区間 [0, 2)・[2, 4) が梁を 1 m ずつに分ける = ℓ 1 m と同じ見込み)
        std::string line = "ℓ 2.0 m(区間の境が梁の真ん中): A2";
        for (const uint64_t cellMm : {500ull, 250ull}) {
            const double load = TestCantilever(cellMm, sim::FractureRule::Window, nullptr, 2000,
                                               (uint32_t)(expect(1.0) * 0.6),
                                               {.bottomMm = 1000, .thicknessMm = 2000, .lengthMm = 3000});
            line += std::format(" {:.0f}", load);
        }

        summary += line + std::format("(見込み ℓ 1 m と同じ {:.1f}・そろえば {:.1f})\n", expect(1.0), expect(2.0));
        Log(Channel::Sim, Level::Info, "厚い梁(厚さ 2 m・0.5 / 0.25 m):\n{}", summary);
    }

    int RunAll() {
        LogExpectations();
        for (const sim::FractureRule rule :
             {sim::FractureRule::Strength, sim::FractureRule::Energy, sim::FractureRule::Window}) {
            for (const uint64_t cellMm : {500ull, 250ull}) {
                TestShelf(cellMm, rule);
                if (rule == sim::FractureRule::Strength)
                    TestShelfSettle(cellMm);

                TestCantilever(cellMm, rule, nullptr);
                TestLedge(cellMm, rule);
                TestCave(cellMm, rule);
            }
        }

        // 3 本目は 0.125 m でも(A・B。C は大きいので 0.5 / 0.25 m だけ)
        TestCantilever(125, sim::FractureRule::Window, nullptr);
        TestLedge(125, sim::FractureRule::Window);

        // 置く場所によらない(T-1069): ℓ 1 m の片持ち梁を 0.5 m・0.25 m 上へずらしても同じ荷で折れる
        // (世界の原点からそろえた区間だと、ずらすと区間の境が梁の中に入って荷が変わった)
        for (const uint64_t cellMm : {500ull, 250ull}) {
            const double base = TestCantilever(cellMm, sim::FractureRule::Window, nullptr, 1000, 150);
            const double lifted = TestCantilever(cellMm, sim::FractureRule::Window, nullptr, 1000, 150,
                                                 {.liftMm = cellMm == 500 ? 500ull : 250ull});
            Expect(base > 0 && base == lifted,
                   std::format("置く場所によらない(ℓ 1 m・{} m): {:.0f} / {:.0f} kN/m", cellMm / 1000.0, base, lifted));
        }

        // 決定性
        uint64_t first = 0;
        uint64_t second = 0;
        TestCantilever(250, sim::FractureRule::Window, &first);
        TestCantilever(250, sim::FractureRule::Window, &second);
        Expect(first == second, "決定性: 2 回の実行でハッシュ一致");

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "support_fracture_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "support_fracture_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? std::string_view(argv[1]) : std::string_view();
    int exitCode = 0;
    if (mode == "--settle") {
        RunSettleStudy();
        Log(Channel::Sim, Level::Info, "support_fracture_test --settle: OK");
    } else if (mode == "--thick-beam") {
        RunThickBeam();
        exitCode = failureCount == 0 ? 0 : 1;
        Log(Channel::Sim, Level::Info, "support_fracture_test --thick-beam: {}", exitCode == 0 ? "OK" : "FAILED");
    } else if (mode == "--window-sweep") {
        RunWindowSweep();
        exitCode = failureCount == 0 ? 0 : 1;
        Log(Channel::Sim, Level::Info, "support_fracture_test --window-sweep: {}", exitCode == 0 ? "OK" : "FAILED");
    } else
        exitCode = RunAll();

    SingletonFinalizer::Finalize();

    return exitCode;
}
