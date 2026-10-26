// support_fracture_test.cpp — 支えの法則の割れ(sim/support_fracture。研究 R-B2・T-0872・22 §6)の CPU の測り。
// 物質: 弱い岩(硬さ 5 GPa・密度 2600 kg/m³・引きの強さ 500 kPa・押しの強さ 10 MPa・破片の大きさ 0.5 m → 割れ面のエネルギー 25 J/m²・
//   割れの速さ 5 m/s)。縦の面・奥行 1 セル(力は奥行 1 m あたりに直して比べる)。60 刻み/秒。
// 場面(どれも 0.5 m と 0.25 m。3 通りの割れ方〔Strength / Energy / Window(T-1042・ℓ = 0.5 m の区間の平均)〕。Window は A・B を 0.125 m でも。
// 引数 --refine で割れかけの所を 0.25 m に下りる道(T-1067。A を 0.5 m・0.25 m・根だけ 0.25 m・下りる で比べる)。
// 引数 --refine-scenes でほかの場面(B・C・D・厚い梁)でも下りる道が 0.25 m の答えに合うか(T-1134)。
// 引数 --root-compare [荷 kN/m] [刻み] で、全部 0.25 m と根だけ 0.25 m の釣り合いの根の面の縁の力を比べる(調べ用)。
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
#include <cstdlib>
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
            "{:.3f} J)・分かれたセル {}・熱で下げた歩 {}・r {:.2f}・{} 刻みに {:.1f} 秒",
            label, measure, outcome.startStage, outcome.startTick, outcome.brokenStage, outcome.brokenTick,
            outcome.where, ledger.surfacePj * 1.0e-12, ledger.heatPj * 1.0e-12, ledger.worstStepHeatPj * 1.0e-12,
            ledger.separatedCells, ledger.heatLimitedSteps, fracture.SofteningRatio() / ONE, outcome.ticks,
            outcome.seconds);
    }

    void CheckFineLedger(const sim::SupportFracture& fracture, const sim::SupportFractureLedger& ledger,
                         std::string_view label);
    std::string WhereFine(const sim::SupportFracture& fracture);

    // 割れの形(refine なら割れかけの所を細かくする道 = T-1067・T-1134)
    sim::SupportFractureOptions FractureOptions(sim::FractureRule rule, bool refine) {
        sim::SupportFractureOptions options{.rule = rule};
        options.refine = refine;

        return options;
    }

    void CheckAnyLedger(const sim::SupportFracture& fracture, const Outcome& outcome, sim::FractureRule rule,
                        std::string_view label) {
        if (fracture.FineMode())
            CheckFineLedger(fracture, outcome.ledger, label);
        else
            CheckLedger(fracture, outcome, rule, label);
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
    double TestLedge(uint64_t cellMm, sim::FractureRule rule, uint64_t windowMm = 0, bool refine = false) {
        const sim::SupportScene scene = Ledge(cellMm);
        sim::SupportFracture fracture(scene, {}, Strength(windowMm), FractureOptions(rule, refine));
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
        const std::string label = std::format("B 棚の下を掘る {} m {}{}", cellMm / 1000.0, RuleName(rule, windowMm),
                                              refine ? "・下りる" : "");
        const Outcome outcome = RunStages(fracture, 12, prepare, label);
        CheckAnyLedger(fracture, outcome, rule, label);
        LogOutcome(label, fracture, outcome,
                   std::format("割れ始めの張り出し {:.1f} m・折れた張り出し {:.1f} m", outcome.startStage * 0.5,
                               outcome.brokenStage * 0.5));

        return outcome.brokenStage < 0 ? -1.0 : outcome.brokenStage * 0.5;
    }

    // --- C. 洞窟を広げる(高さ 3〜7 m・天井の厚さ 1 m・真ん中〔16 m の岩なら 8 m〕から両側へ 0.5 m ずつ)---
    double TestCave(uint64_t cellMm, sim::FractureRule rule, uint64_t windowMm = 0, uint64_t widthMm = 16000,
                    bool refine = false) {
        const sim::SupportScene scene = Rock(cellMm, widthMm);
        sim::SupportFracture fracture(scene, {}, Strength(windowMm), FractureOptions(rule, refine));
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
        const std::string label = std::format("C 洞窟 {} m {}(岩の幅 {} m){}", cellMm / 1000.0,
                                              RuleName(rule, windowMm), widthMm / 1000, refine ? "・下りる" : "");
        const Outcome outcome = RunStages(fracture, (uint32_t)(widthMm / 1000), prepare, label);
        CheckAnyLedger(fracture, outcome, rule, label);
        LogOutcome(label, fracture, outcome,
                   std::format("割れ始めの幅 {} m・落ちた幅 {} m", outcome.startStage, outcome.brokenStage));

        return outcome.brokenStage < 0 ? -1.0 : (double)outcome.brokenStage;
    }

    // --- D. すぐ折れるはずの棚(冷えた始まり)---
    uint32_t TestShelf(uint64_t cellMm, sim::FractureRule rule, uint64_t windowMm = 0, bool refine = false) {
        const sim::SupportScene scene = Shelf(cellMm);
        sim::SupportFracture fracture(scene, {}, Strength(windowMm), FractureOptions(rule, refine));
        const std::string label = std::format("D すぐ折れる棚 {} m {}{}", cellMm / 1000.0, RuleName(rule, windowMm),
                                              refine ? "・下りる" : "");
        const Outcome outcome = RunStages(fracture, 10, [](uint32_t) {}, label);
        CheckAnyLedger(fracture, outcome, rule, label);
        // 下りた時は割れが細かい面にあるので細かい格子の場所も(粗い面の場所は 2 つの細かい面が割れ切った所だけ)
        const std::string fineWhere = refine ? "・細かい格子 " + WhereFine(fracture) : std::string();
        LogOutcome(label, fracture, outcome,
                   std::format("割れ始め {} 刻み({:.3f} 秒)・分かれた {} 刻み({:.3f} 秒){}", outcome.startTick,
                               outcome.startTick / 60.0, outcome.brokenTick, outcome.brokenTick / 60.0, fineWhere));

        return outcome.brokenTick;
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
        // 比べの元: 前の前の既定(V 2 回・共役勾配なし)。今の既定(V 2 回 + 共役勾配・刻みをまたぐ。D-649・T-1086)は「既定」の行、
        // 前の既定(V 4 回 + 刻みの中の共役勾配。T-1085)も並べる
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
                       std::format("T-1041 {} m 壁の棚 0.5 m(既定 = V 2 回 + 共役勾配・刻みをまたぐ)", cell),
                       STUDY_TICKS);
            sim::SupportOptions previous;
            previous.blocks = {.cycles = 4, .lineSearch = true};
            SettleOnce(wallShelf, previous, wallRootX, topUp(2000),
                       std::format("T-1041 {} m 壁の棚 0.5 m(V 4 回 + 刻みの中の共役勾配 = 前の既定)", cell),
                       STUDY_TICKS);

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

            // T-1086: 共役勾配の向きを刻みをまたいで持つ
            for (const uint32_t cycles : {2u, 4u}) {
                sim::SupportOptions options;
                options.blocks.cycles = cycles;
                options.blocks.conjugateAcrossTicks = true;
                SettleOnce(wallShelf, options, wallRootX, topUp(2000),
                           std::format("T-1086 {} m 壁の棚 0.5 m(V {} 回 + 共役勾配・刻みをまたぐ)", cell, cycles),
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

    // --- 割れかけの所を細かくする(T-1067): 細かい格子の面で持つ時の帳簿・場所 ---
    // 割れ切った細かい面の場所(縦の面の x・横の面の高さ。m)
    std::string WhereFine(const sim::SupportFracture& fracture) {
        const sim::SupportScene& scene = fracture.Scene();
        const uint64_t side = scene.cellMm / 2;
        std::set<uint64_t> vertical;
        std::set<uint64_t> horizontal;
        for (uint32_t fup = 0; fup < 2 * scene.height; ++fup) {
            for (uint32_t fx = 0; fx < 2 * scene.width; ++fx) {
                if (fracture.FineProgress(fx, fup, 0) >= sim::SUPPORT_INTACT_FULL)
                    vertical.insert((fx + 1) * side);

                if (fracture.FineProgress(fx, fup, 1) >= sim::SUPPORT_INTACT_FULL)
                    horizontal.insert((fup + 1) * side);
            }
        }

        std::string text = "縦の割れ x =";
        for (const uint64_t mm : vertical)
            text += std::format(" {:.2f}", mm / 1000.0);

        text += " m・横の割れ 高さ =";
        for (const uint64_t mm : horizontal)
            text += std::format(" {:.2f}", mm / 1000.0);

        return text + " m";
    }

    // 細かい格子の帳簿: 解放 = 割れ面 + 熱・割れ面のエネルギー = G × 割れた面積(面の幅で数える。ADR-0470)・熱 ≥ 0
    void CheckFineLedger(const sim::SupportFracture& fracture, const sim::SupportFractureLedger& ledger,
                         std::string_view label) {
        Expect(ledger.releasedPj == ledger.surfacePj + ledger.heatPj, std::format("{}: 解放 = 割れ面 + 熱", label));
        const auto energy = (int64_t)fracture.FractureEnergyMjPerM2() * 1000;  // pJ/mm²
        Expect(ledger.surfacePj >= energy * (int64_t)ledger.brokenAreaMm2 &&
                   ledger.surfacePj <= energy * (int64_t)(ledger.brokenAreaMm2 + ledger.crackingAreaMm2),
               std::format("{}: 割れ面のエネルギー = G × 割れた面積", label));
        const uint64_t side = fracture.Scene().cellMm / 2;
        const auto fineFacePj = energy * (int64_t)(side * fracture.Scene().cellMm);
        Expect(ledger.worstStepHeatPj >= -fineFacePj / 16384, std::format("{}: 熱 ≥ 0(坂を下る時だけ進む)", label));
    }

    struct RefineRun {
        double load = -1.0;  // 折れた荷(kN/m)
        uint64_t hash = 0;
        std::string where;
        uint32_t refineSteps = 0;  // 分けた・戻した刻み
        uint32_t ticks = 0;
        double msPerTick = 0.0;
    };

    // A の片持ち梁を割れの形 options で(荷は TestCantilever と同じ)。fine = 最初から分けておくセル(x, 上の範囲 mm。空 = なし)
    RefineRun TestCantileverFine(uint64_t cellMm, const sim::SupportFractureOptions& options, uint32_t startKn,
                                 std::string_view name, uint64_t fineFromMm = 0, uint64_t fineToMm = 0,
                                 const BeamShape& shape = {}) {
        sim::SupportScene scene = Cantilever(cellMm, shape);
        if (fineToMm > fineFromMm) {
            scene.fine.assign(scene.solid.size(), 0);
            for (uint32_t up = 0; up < scene.height; ++up) {
                for (uint32_t x = 0; x < scene.width; ++x) {
                    const uint64_t cx = Center(x, cellMm);
                    scene.fine[x + (size_t)scene.width * up] = cx > fineFromMm && cx < fineToMm ? 1 : 0;
                }
            }
        }

        sim::SupportFracture fracture(scene, {}, Strength(), options);
        std::vector<uint32_t> loaded;
        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                const size_t at = x + (size_t)scene.width * up;
                if (scene.solid[at] != 0 && Center(x, cellMm) > shape.lengthMm)
                    loaded.push_back((uint32_t)at);
            }
        }

        uint32_t worstFine = 0;
        const auto prepare = [&](uint32_t stage) {
            worstFine = std::max(worstFine, fracture.Ledger().fineCells);
            std::vector<int64_t> loads(scene.solid.size(), 0);
            const auto perCell = (int64_t)((uint64_t)(startKn + stage) * 1000 * cellMm / loaded.size());
            for (const uint32_t at : loaded)
                loads[at] = perCell;

            fracture.SetLoads(loads);
        };
        const std::string label = std::format("A 片持ち梁 {} m {}(厚さ {:.1f} m)", cellMm / 1000.0, name,
                                              shape.thicknessMm / 1000.0);
        const Outcome outcome = RunStages(fracture, 400, prepare, label);
        if (fracture.FineMode())
            CheckFineLedger(fracture, outcome.ledger, label);
        else
            CheckLedger(fracture, outcome, sim::FractureRule::Window, label);

        const sim::SupportFractureLedger& ledger = outcome.ledger;
        RefineRun result{.load = outcome.brokenStage < 0 ? -1.0 : (double)(startKn + outcome.brokenStage),
                         .hash = outcome.hash,
                         .where = fracture.FineMode() ? WhereFine(fracture) : outcome.where,
                         .refineSteps = ledger.refineSteps,
                         .ticks = outcome.ticks,
                         .msPerTick = outcome.seconds * 1000.0 / std::max<uint32_t>(outcome.ticks, 1)};
        Log(Channel::Sim, Level::Info,
            "{}: 割れ始めの荷 {} kN/m・折れた荷 {:.0f} kN/m{} / {} / 細かいセル 最大 {}・分けた刻み {} / 割れ面 {:.1f} "
            "J・熱 "
            "{:.1f} J(区間 1 刻みの最小 {:.4f} J)・熱で下げた歩 {} / {} 刻みに {:.1f} 秒({:.3f} ms/刻み)",
            label, KnOf(startKn, outcome.startStage), result.load,
            startKn != 0 && outcome.startStage == 0 ? "(最初の段で割れ始めた)" : "", result.where,
            std::max(worstFine, ledger.fineCells), ledger.refineSteps, ledger.surfacePj * 1.0e-12,
            ledger.heatPj * 1.0e-12, ledger.worstStepHeatPj * 1.0e-12, ledger.heatLimitedSteps, outcome.ticks,
            outcome.seconds, outcome.seconds * 1000.0 / std::max<uint32_t>(outcome.ticks, 1));

        return result;
    }

    // 0.5 m の場面で割れかけの所を 0.25 m に下りて判定した荷が、最初から 0.25 m で置いた荷と ±10 % で同じか(T-1067 の完了条件の 2 つ目を
    // 2:1 で。ℓ = 0.5 m・岩の ℓ は下限ちょうど = 0.5 m のままでは 1 割強く折れる)
    void RunRefine() {
        constexpr uint32_t START_KN = 46;  // ℓ 0.5 m の見込み 77 kN/m の 6 割
        const sim::SupportFractureOptions window{.rule = sim::FractureRule::Window};
        const double coarse = TestCantileverFine(500, window, START_KN, "ℓ 0.5 m の平均(細かくしない)").load;
        const RefineRun fineRun = TestCantileverFine(250, window, START_KN, "ℓ 0.5 m の平均(細かくしない)");
        const double fine = fineRun.load;

        // 細かい格子で持つが分けない(粗い面だけ = 面の数で数える形と同じ区間)
        sim::SupportFractureOptions never = window;
        never.refine = true;
        never.refineIndex = UINT32_MAX;
        never.coarsenIndex = 0;
        const double same = TestCantileverFine(500, never, START_KN, "細かい格子で持つ・分けない").load;
        Expect(same == coarse,
               std::format("細かい格子で持っても分けなければ同じ荷: {:.0f} / {:.0f} kN/m", same, coarse));

        // 根の 1 m(壁の 0.5 m と梁の 0.5 m)を最初から 0.25 m に
        const sim::SupportFractureOptions fixedFine = window;
        const RefineRun root = TestCantileverFine(500, fixedFine, START_KN, "根の 1 m を最初から 0.25 m", 500, 1500);

        // 割れかけの所を下りる(強さの比 1/2 で分け・1/4 未満で戻す)
        sim::SupportFractureOptions refine = window;
        refine.refine = true;
        const RefineRun first = TestCantileverFine(500, refine, START_KN, "割れかけの所を 0.25 m に下りる");
        const RefineRun second = TestCantileverFine(500, refine, START_KN, "割れかけの所を 0.25 m に下りる(2 回目)");
        Expect(first.hash == second.hash, "細かくする道の決定性: 2 回の実行でハッシュ一致");
        sim::SupportFractureOptions tight = refine;
        tight.refineRing = 0;
        const RefineRun closest = TestCantileverFine(500, tight, START_KN, "割れかけの所を 0.25 m に下りる(周りなし)");
        const auto near = [&](double load) {
            return fine > 0 && load > 0 && std::abs(load - fine) <= 0.1 * fine;
        };

        // 戻す所を広げない形(T-1067: 戻すのは比が 1/4 未満の自分のセルだけを見る。周りの縁で行き来した)と比べる
        sim::SupportFractureOptions loose = refine;
        loose.coarsenRing = 0;
        const RefineRun looseRun = TestCantileverFine(500, loose, START_KN, "下りる(戻す所を広げない = T-1067 の形)");
        Expect(near(looseRun.load), std::format("戻す所を広げない形も ±10 %: {:.0f} kN/m", looseRun.load));
        Log(Channel::Sim, Level::Info,
            "縁の行き来(T-1134): 分けた・戻した刻み 戻す所を広げる {} / 広げない {}(刻み {} / {})・ms/刻み {:.3f} / "
            "{:.3f}",
            first.refineSteps, looseRun.refineSteps, first.ticks, looseRun.ticks, first.msPerTick, looseRun.msPerTick);
        Expect(near(first.load),
               std::format("下りた荷が最初から 0.25 m の荷の ±10 %: {:.0f} / {:.0f} kN/m", first.load, fine));
        Log(Channel::Sim, Level::Info,
            "細かくする道(A・ℓ 0.5 m): 0.5 m {:.0f} / 0.25 m {:.0f} / 細かい格子で持つ・分けない {:.0f} / 根の 1 m を "
            "0.25 m "
            "{:.0f}({:+.1f} %) / 下りる {:.0f}({:+.1f} %)/ 下りる・周りなし {:.0f}({:+.1f} %)kN/m\n"
            "  場所: 0.25 m {} / 根だけ {} / 下りる {} / 周りなし {}",
            coarse, fine, same, root.load, 100.0 * (root.load - fine) / fine, first.load,
            100.0 * (first.load - fine) / fine, closest.load, 100.0 * (closest.load - fine) / fine, fineRun.where,
            root.where, first.where, closest.where);
    }

    // 下りる道の 1 刻みの費用の内訳(T-1134 の残り・ADR-0469 の 5-3 の材料。CPU・release): 刻みを、法則を作り直した刻み
    // (割れが進んだ・分けた・戻した)と作り直さない刻みに分け、さらに細かいセルがいちばん多い時の場面で、法則を作る(+ 温めた始まりの移し替え)
    // 時間と、釣り合いを解く(SupportLaw::Step)時間を別に測る。作り直さない刻み − 解く = 割れの読み(面の力・区間・細かくする判定)
    void MeasureRefineCost(std::string_view name, const BeamShape& shape, uint32_t startKn) {
        sim::SupportFractureOptions options{.rule = sim::FractureRule::Window};
        options.refine = true;
        const sim::SupportScene scene = Cantilever(500, shape);
        sim::SupportFracture fracture(scene, {}, Strength(), options);
        std::vector<uint32_t> loaded;
        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                const size_t at = x + (size_t)scene.width * up;
                if (scene.solid[at] != 0 && Center(x, 500) > shape.lengthMm)
                    loaded.push_back((uint32_t)at);
            }
        }

        // --- 刻みごとの時間を、作り直した刻みと作り直さない刻みに分ける ---
        const auto millisecondsSince = [](chr::steady_clock::time_point begin) {
            return chr::duration<double, std::milli>(chr::steady_clock::now() - begin).count();
        };
        double loadMs = 0.0;
        double rebuildMs = 0.0;
        double plainMs = 0.0;
        uint32_t rebuildTicks = 0;
        uint32_t plainTicks = 0;
        uint32_t busiestFine = 0;
        sim::SupportScene busiest = fracture.Scene();
        for (uint32_t stage = 0; stage < 400 && fracture.Ledger().separatedCells == 0; ++stage) {
            std::vector<int64_t> loads(scene.solid.size(), 0);
            const auto perCell = (int64_t)((uint64_t)(startKn + stage) * 1000 * 500 / loaded.size());
            for (const uint32_t at : loaded)
                loads[at] = perCell;

            const auto loadBegin = chr::steady_clock::now();
            fracture.SetLoads(loads);
            loadMs += millisecondsSince(loadBegin);
            for (uint32_t i = 0; i < SETTLE_TICKS && fracture.Ledger().separatedCells == 0; ++i) {
                const uint32_t before = fracture.Ledger().rebuilds;
                const auto begin = chr::steady_clock::now();
                fracture.Step();
                const double ms = millisecondsSince(begin);
                const bool rebuilt = fracture.Ledger().rebuilds != before;
                (rebuilt ? rebuildMs : plainMs) += ms;
                ++(rebuilt ? rebuildTicks : plainTicks);
                if (fracture.Ledger().fineCells > busiestFine) {
                    busiestFine = fracture.Ledger().fineCells;
                    busiest = fracture.Scene();
                }
            }
        }

        // --- 細かいセルがいちばん多い時の場面で、作る時間と解く時間 ---
        constexpr uint32_t BUILDS = 20;
        constexpr uint32_t SOLVES = 200;
        sim::SupportLaw law(busiest, {});
        const auto buildBegin = chr::steady_clock::now();
        for (uint32_t i = 0; i < BUILDS; ++i) {
            sim::SupportLaw next(busiest, {});
            next.CarryFrom(law);
        }

        const double buildMs = millisecondsSince(buildBegin) / BUILDS;
        const auto solveBegin = chr::steady_clock::now();
        for (uint32_t i = 0; i < SOLVES; ++i)
            law.Step();

        const double solveMs = millisecondsSince(solveBegin) / SOLVES;
        const uint32_t ticks = rebuildTicks + plainTicks;
        const double totalMs = loadMs + rebuildMs + plainMs;
        const double plainMean = plainMs / std::max<uint32_t>(plainTicks, 1);
        const double rebuildMean = rebuildMs / std::max<uint32_t>(rebuildTicks, 1);
        Expect(ticks > 0 && fracture.Ledger().separatedCells > 0, std::format("費用の測り({}): 折れた", name));
        Log(Channel::Sim, Level::Info,
            "下りる道の費用({}・0.5 m・ℓ 0.5 m): {} 刻みに {:.0f} ms({:.3f} ms/刻み)/ 作り直した刻み {}({:.3f} "
            "ms/刻み・計 {:.1f} %)・"
            "作り直さない刻み {}({:.3f} ms/刻み・計 {:.1f} %)・荷を置いた {} 回(計 {:.1f} %)/ 細かいセル 最大 {}・セル "
            "{} の場面で "
            "作る + 移す {:.3f} ms・解く {:.3f} ms/刻み・割れの読み(作り直さない刻み − 解く){:.3f} ms/刻み",
            name, ticks, totalMs, totalMs / std::max<uint32_t>(ticks, 1), rebuildTicks, rebuildMean,
            100.0 * rebuildMs / totalMs, plainTicks, plainMean, 100.0 * plainMs / totalMs,
            fracture.Ledger().rebuilds - rebuildTicks, 100.0 * loadMs / totalMs, busiestFine, law.CellCount(), buildMs,
            solveMs, plainMean - solveMs);
    }

    void RunRefineCost() {
        MeasureRefineCost("A 片持ち梁(厚さ 1 m)", {}, 46);
        MeasureRefineCost("厚い梁(厚さ 2 m)", {.bottomMm = 2000, .thicknessMm = 2000, .lengthMm = 3000}, 55);
    }

    // ほかの場面でも下りる道が 0.25 m の答えに合うか(T-1134): B・C・D・厚い梁を 0.5 m / 0.25 m / 0.5 m + 下りる で(ℓ 0.5 m の平均)
    void RunRefineScenes() {
        constexpr auto WINDOW = sim::FractureRule::Window;
        constexpr uint64_t CAVE_WIDTH_MM = 24000;
        const auto within = [](double value, double reference) {
            return reference > 0 && value > 0 && std::abs(value - reference) <= 0.1 * reference;
        };
        std::string summary;

        // --- B 棚を掘る(折れた張り出し)---
        const double ledgeCoarse = TestLedge(500, WINDOW);
        const double ledgeFine = TestLedge(250, WINDOW);
        const double ledgeRefine = TestLedge(500, WINDOW, 0, true);
        Expect(within(ledgeRefine, ledgeFine),
               std::format("B: 下りた張り出しが 0.25 m の ±10 %: {:.1f} / {:.1f} m", ledgeRefine, ledgeFine));
        summary += std::format("B 棚を掘る(m): 0.5 m {:.1f} / 0.25 m {:.1f} / 下りる {:.1f}\n", ledgeCoarse, ledgeFine,
                               ledgeRefine);

        // --- C 洞窟(落ちた幅)---
        const double caveCoarse = TestCave(500, WINDOW, 0, CAVE_WIDTH_MM);
        const double caveFine = TestCave(250, WINDOW, 0, CAVE_WIDTH_MM);
        const double caveRefine = TestCave(500, WINDOW, 0, CAVE_WIDTH_MM, true);
        Expect(within(caveRefine, caveFine),
               std::format("C: 下りた落ちた幅が 0.25 m の ±10 %: {:.0f} / {:.0f} m", caveRefine, caveFine));
        summary += std::format("C 洞窟(m): 0.5 m {:.0f} / 0.25 m {:.0f} / 下りる {:.0f}\n", caveCoarse, caveFine,
                               caveRefine);

        // --- D すぐ折れる棚(分かれた刻み。冷えた始まりなので割れ切るまでの刻みは割れの速さで決まる)---
        const uint32_t shelfCoarse = TestShelf(500, WINDOW);
        const uint32_t shelfFine = TestShelf(250, WINDOW);
        const uint32_t shelfRefine = TestShelf(500, WINDOW, 0, true);
        Expect(shelfRefine > 0, std::format("D: 下りても折れる(分かれた刻み {})", shelfRefine));
        // 割れの先が区間を渡る速さの上限が細かさによらない(T-1135: 速さ × 刻み ÷ 区間の幅。面の幅で割った時は 11 / 12 / 5 刻み)。
        // どの細かさでも、割れ始め(1 刻み目より後)から区間 ℓ 0.5 m を割れの速さ 5 m/s で渡る 6 刻みより早く分かれない。
        // 0.5 m と下りる道(同じ 0.5 m の解き)は ±2 刻み。0.25 m は冷えた始まりの釣り合いの遅れで割れの先がエネルギーを待つ
        // (速さの上限ではなく T-1041・T-1068 の側。T-1206 で固定した面を面の点の形にしたら 0.5 m が 11 → 7 刻み = 速さの上限に着き、
        // 0.25 m の 12 → 13 刻みと ±2 に入らなくなった。T-1206 のチケット)
        constexpr uint32_t CROSS_TICKS = (500 * 60 + 5000 - 1) / 5000;
        const auto closeTicks = [](uint32_t a, uint32_t b) {
            return a + 2 >= b && b + 2 >= a;
        };
        Expect(shelfCoarse > CROSS_TICKS && shelfFine > CROSS_TICKS && shelfRefine > CROSS_TICKS,
               std::format("D: 割れの速さで区間を渡るより早く分かれない(> {} 刻み): {} / {} / {}", CROSS_TICKS,
                           shelfCoarse, shelfFine, shelfRefine));
        Expect(closeTicks(shelfCoarse, shelfRefine),
               std::format("D: 0.5 m と下りる道の分かれた刻みが ±2 刻み: {} / {}", shelfCoarse, shelfRefine));
        summary += std::format("D すぐ折れる棚(分かれた刻み): 0.5 m {} / 0.25 m {} / 下りる {}\n", shelfCoarse,
                               shelfFine, shelfRefine);

        // --- 厚い梁(厚さ 2 m・長さ 3 m・高さ 2〜4 m。折れた荷)---
        constexpr uint32_t THICK_START_KN = 55;  // ℓ 0.5 m の 0.5 m で 100・0.25 m で 91 kN/m(T-1067)の 6 割
        const BeamShape thick{.bottomMm = 2000, .thicknessMm = 2000, .lengthMm = 3000};
        const sim::SupportFractureOptions window{.rule = WINDOW};
        sim::SupportFractureOptions refine = window;
        refine.refine = true;
        const RefineRun thickCoarse = TestCantileverFine(500, window, THICK_START_KN, "細かくしない", 0, 0, thick);
        const RefineRun thickFine = TestCantileverFine(250, window, THICK_START_KN, "細かくしない", 0, 0, thick);
        const RefineRun thickRefine = TestCantileverFine(500, refine, THICK_START_KN, "下りる", 0, 0, thick);
        Expect(
            within(thickRefine.load, thickFine.load),
            std::format("厚い梁: 下りた荷が 0.25 m の ±10 %: {:.0f} / {:.0f} kN/m", thickRefine.load, thickFine.load));
        summary += std::format(
            "厚い梁(kN/m): 0.5 m {:.0f} / 0.25 m {:.0f} / 下りる {:.0f}(分けた刻み {}・{:.3f} ms/刻み)\n  場所: 0.25 m "
            "{} / 下りる {}\n",
            thickCoarse.load, thickFine.load, thickRefine.load, thickRefine.refineSteps, thickRefine.msPerTick,
            thickFine.where, thickRefine.where);

        Log(Channel::Sim, Level::Info, "ほかの場面の下りる道(T-1134・ℓ 0.5 m):\n{}", summary);
    }

    // 根の面の力の比べ(調べ用): 全部 0.25 m と、0.5 m の根の 1 m だけ 0.25 m で、同じ荷の釣り合いの根の 2 つの線(x = 1.0・1.25 m)の縁の力
    void RunRootCompare(uint32_t loadKn, uint32_t ticks) {
        for (const bool mixed : {false, true}) {
            const uint64_t cellMm = mixed ? 500 : 250;
            sim::SupportScene scene = Cantilever(cellMm);
            if (mixed) {
                scene.fine.assign(scene.solid.size(), 0);
                for (uint32_t at = 0; at < scene.solid.size(); ++at) {
                    const uint64_t cx = Center(at % scene.width, cellMm);
                    scene.fine[at] = cx > 500 && cx < 1500 ? 1 : 0;
                }
            }

            std::vector<uint32_t> loaded;
            for (uint32_t at = 0; at < scene.solid.size(); ++at) {
                if (scene.solid[at] != 0 && Center(at % scene.width, cellMm) > 2000)
                    loaded.push_back(at);
            }

            scene.loadMn.assign(scene.solid.size(), 0);
            for (const uint32_t at : loaded)
                scene.loadMn[at] = (int64_t)((uint64_t)loadKn * 1000 * cellMm / loaded.size());

            sim::SupportLaw law(scene, {});
            for (uint32_t tick = 0; tick < ticks; ++tick)
                law.Step();

            const sim::SupportLedger ledger = law.Ledger();
            std::string text = std::format("{}: 残りの力の最大 {} mN(重さ {} mN)",
                                           mixed ? "根だけ 0.25 m" : "全部 0.25 m", ledger.worstCellMn,
                                           ledger.weightMn);
            for (const uint32_t fx : {3u, 4u}) {
                text += std::format(" / x = {:.2f} m:", (fx + 1) * 0.25);
                for (uint32_t fup = 4; fup < 8; ++fup) {
                    const sim::SupportFaceForces forces = mixed ? law.FineFaceForces(fx, fup, 0)
                                                                : law.FaceForces(fx, fup, 0);
                    const double scale = 1.0e-3 / (double)cellMm;  // 奥行 1 m あたりの kN
                    text += std::format(" [{:.1f} {:.1f} ず {:.1f}]", forces.lowSideMn * scale,
                                        forces.highSideMn * scale, forces.shearMn * scale);
                }
            }

            Log(Channel::Sim, Level::Info, "{}", text);
        }
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
    } else if (mode == "--root-compare") {
        RunRootCompare(argc > 2 ? (uint32_t)std::atoi(argv[2]) : 60, argc > 3 ? (uint32_t)std::atoi(argv[3]) : 2000);
    } else if (mode == "--refine") {
        RunRefine();
        exitCode = failureCount == 0 ? 0 : 1;
        Log(Channel::Sim, Level::Info, "support_fracture_test --refine: {}", exitCode == 0 ? "OK" : "FAILED");
    } else if (mode == "--refine-cost") {
        RunRefineCost();
        exitCode = failureCount == 0 ? 0 : 1;
        Log(Channel::Sim, Level::Info, "support_fracture_test --refine-cost: {}", exitCode == 0 ? "OK" : "FAILED");
    } else if (mode == "--refine-scenes") {
        RunRefineScenes();
        exitCode = failureCount == 0 ? 0 : 1;
        Log(Channel::Sim, Level::Info, "support_fracture_test --refine-scenes: {}", exitCode == 0 ? "OK" : "FAILED");
    } else if (mode == "--window-sweep") {
        RunWindowSweep();
        exitCode = failureCount == 0 ? 0 : 1;
        Log(Channel::Sim, Level::Info, "support_fracture_test --window-sweep: {}", exitCode == 0 ? "OK" : "FAILED");
    } else
        exitCode = RunAll();

    SingletonFinalizer::Finalize();

    return exitCode;
}
