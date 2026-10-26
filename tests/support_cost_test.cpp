// support_cost_test.cpp — 支えの費用(研究 R-B3・T-1171・22 §6)の CPU の測り: 大きな岩の塊を 1 回掘った後の解き直しを、閉じない解と
// 閉じた領域(sim/support_law の closeMultiplier。閉じる長さ = 変わった所の大きさ × 倍率。C-0 の閉じた領域 T-1087)で比べる。
// 場面(石: 硬さ 50 GPa・密度 2600 kg/m³。縦の面・奥行 1 セル): 塊を下と両横で固定・上は自由。32 × 16 m・64 × 32 m(0.5 m)・32 × 16 m(0.25 m)。
//   穴: 1 × 1 m と 4 × 2 m(横の真ん中・穴の天井が上の面から 4 m 下)
// 測るもの(機械によらない数が主。ms は CPU の参考): 掘った後に解いた節の数の和(未知数 × 刻み)・作り直しの ms と刻みの ms・
//   釣り合いに着く刻み(穴の天井のたわみ)・閉じない答えとの差(たわみ・面の縁の力)・前の答えから動いたセルの届いた距離・天井のたわみの道筋
// 試験が落ちるのは: 帳簿(毎刻み)・決定性(2 回の実行のハッシュ)・倍率が世界を覆う時の閉じない解との毎刻みのビット一致・
//   固定の領域の外の値が前の答えとビット一致だけ。
//   合格の目安(T-1171)は記録としてログに出す(研究の測り)。
// 2 本目(粗さで閉じる): 遠くを値で固定せず、粗いセル(2 倍)のまま一緒に解く(違う大きさのセル T-1120・ADR-0470。1 段だけ)。
//   穴から「穴の長い辺 × 倍率」の内だけを細かく、閉じずに解き、全部細かい塊の答えと比べる(掘ったことによる変化の差)。
// 引数: [--measure](全部の場面。既定は ctest 用の 32 × 16 m・0.5 m だけ)[--coarse](2 本目の全部の場面)[--ticks n](掘った後の刻み。既定 600)
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/aliases.h"
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
    constexpr uint32_t FULL = 0;          // 閉じない
    constexpr uint32_t COVER = 100000;    // 世界を覆う倍率(閉じない解とビット一致の確かめ)

    using Clock = std::chrono::steady_clock;

    double MsSince(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    // --- 場面 ---
    struct Case {
        uint64_t widthMm = 0;
        uint64_t heightMm = 0;
        uint64_t cellMm = 0;
        uint64_t holeWidthMm = 0;
        uint64_t holeHeightMm = 0;
    };

    struct Geometry {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t holeLowX = 0;
        uint32_t holeHighX = 0;  // 含まない
        uint32_t holeLowUp = 0;
        uint32_t holeHighUp = 0;  // 含まない = 天井の段
        uint32_t centerX = 0;
    };

    Geometry GeometryOf(const Case& c) {
        Geometry g;
        g.width = (uint32_t)(c.widthMm / c.cellMm);
        g.height = (uint32_t)(c.heightMm / c.cellMm);
        const auto holeWidth = (uint32_t)(c.holeWidthMm / c.cellMm);
        const auto holeHeight = (uint32_t)(c.holeHeightMm / c.cellMm);
        g.holeLowX = (g.width - holeWidth) / 2;
        g.holeHighX = g.holeLowX + holeWidth;
        g.holeHighUp = g.height - (uint32_t)(4000 / c.cellMm);
        g.holeLowUp = g.holeHighUp - holeHeight;
        g.centerX = g.width / 2;
        return g;
    }

    // 塊(下と両横を固定)
    sim::SupportScene Block(const Case& c, const Geometry& g, bool dug) {
        sim::SupportScene scene;
        scene.width = g.width;
        scene.height = g.height;
        scene.cellMm = c.cellMm;
        scene.stiffnessKpa = test::STONE_KPA;
        scene.densityKgPerM3 = test::STONE_DENSITY;
        scene.solid.assign((size_t)g.width * g.height, 1);
        scene.clamps.assign((size_t)g.width * g.height, 0);
        for (uint32_t up = 0; up < g.height; ++up) {
            for (uint32_t x = 0; x < g.width; ++x) {
                uint8_t clamp = 0;
                if (up == 0)
                    clamp |= sim::SUPPORT_CLAMP_BOTTOM;

                if (x == 0)
                    clamp |= sim::SUPPORT_CLAMP_LEFT;

                if (x + 1 == g.width)
                    clamp |= sim::SUPPORT_CLAMP_RIGHT;

                const size_t i = x + (size_t)g.width * up;
                scene.clamps[i] = clamp;
                const bool hole = x >= g.holeLowX && x < g.holeHighX && up >= g.holeLowUp && up < g.holeHighUp;
                if (dug && hole)
                    scene.solid[i] = 0;
            }
        }

        return scene;
    }

    double CellWeightMn(const Case& c) {
        const double edgeM = (double)c.cellMm / 1000.0;
        return (double)test::STONE_DENSITY * 9.80665 * edgeM * edgeM * edgeM * 1000.0;
    }

    // --- 掘る前の答え: 閉じずに、セルに残る力が limit(重さ ÷ 10000)より小さくなってから 50 刻み ---
    uint32_t Settle(sim::SupportLaw& law, double limit) {
        uint32_t below = 0;
        for (uint32_t tick = 1; tick <= 20000; ++tick) {
            law.Step();
            below = (double)law.Ledger().worstCellMn <= limit ? below + 1 : 0;
            if (below >= 50)
                return tick;
        }

        return 0;
    }

    // --- 掘った後の 1 本 ---
    struct Snapshot {
        std::vector<int64_t> values;  // [セル × 3 + 成分](無いセルは 0)
        std::vector<int64_t> edges;   // [(セル × 2 + 軸) × 2 + 側] の面の縁の力(mN)
    };

    Snapshot Take(const sim::SupportLaw& law, const Geometry& g) {
        Snapshot shot;
        const size_t cells = (size_t)g.width * g.height;
        shot.values.resize(cells * sim::SUPPORT_COMPONENTS);
        shot.edges.resize(cells * 4);
        for (uint32_t up = 0; up < g.height; ++up) {
            for (uint32_t x = 0; x < g.width; ++x) {
                const size_t i = x + (size_t)g.width * up;
                for (uint32_t c = 0; c < sim::SUPPORT_COMPONENTS; ++c)
                    shot.values[i * sim::SUPPORT_COMPONENTS + c] = law.Value(x, up, c);

                for (uint32_t axis = 0; axis < 2; ++axis) {
                    const sim::SupportFaceForces forces = law.FaceForces(x, up, axis);
                    shot.edges[(i * 2 + axis) * 2] = forces.lowSideMn;
                    shot.edges[(i * 2 + axis) * 2 + 1] = forces.highSideMn;
                }
            }
        }

        return shot;
    }

    struct Run {
        uint32_t multiplier = 0;
        uint64_t closeLengthMm = 0;
        double buildMs = 0;
        double firstTickMs = 0;
        double totalMs = 0;
        uint64_t solvedNodes = 0;  // Σ 未知数(刻みごと)
        uint64_t nodeUpdates = 0;  // Σ 節の値を解いた回数(全部の段)
        uint32_t firstUnknowns = 0;
        uint32_t firstSeeds = 0;
        uint32_t lastSolvingTick = 0;  // 未知数が 0 でなかった最後の刻み
        bool ledgerOk = true;
        std::vector<double> sagMm;       // 刻みごとの穴の天井のたわみ(真ん中。下向きを正)
        std::vector<uint64_t> hashes;    // 刻みごと
        std::vector<uint32_t> unknowns;  // 刻みごとの未知数
        std::vector<uint64_t> worstMn;   // 刻みごとのセルに残る力の最大(mN)
        std::vector<uint64_t> updates;   // 刻みごとの節の値を解いた回数(2 本目)
        Snapshot finalShot;
        Snapshot midShot;  // 刻みの半分
    };

    Run DigRun(const Case& c, const Geometry& g, const sim::SupportLaw& before, uint32_t multiplier,
               sim::SupportCloseGrowth growth, uint32_t ticks) {
        Run run;
        run.multiplier = multiplier;
        const sim::SupportScene dugScene = Block(c, g, true);
        const Clock::time_point buildStart = Clock::now();
        sim::SupportLaw law(dugScene, {.closeMultiplier = multiplier, .closeGrowth = growth});
        law.CarryFrom(before);
        run.buildMs = MsSince(buildStart);
        run.closeLengthMm = law.CloseLengthMm();
        for (uint32_t tick = 1; tick <= ticks; ++tick) {
            const Clock::time_point start = Clock::now();
            law.Step();
            const double ms = MsSince(start);
            run.totalMs += ms;
            if (tick == 1) {
                run.firstTickMs = ms;
                run.firstUnknowns = law.LastUnknowns();
                run.firstSeeds = law.LastSeeds();
            }

            run.solvedNodes += law.LastUnknowns();
            run.nodeUpdates += law.LastNodeUpdates();
            if (law.LastUnknowns() != 0)
                run.lastSolvingTick = tick;

            const sim::SupportLedger ledger = law.Ledger();
            run.ledgerOk = run.ledgerOk && ledger.netMn == ledger.reactionMn - ledger.weightMn;
            run.unknowns.push_back(law.LastUnknowns());
            run.worstMn.push_back(ledger.worstCellMn);
            run.sagMm.push_back(-(double)law.Value(g.centerX, g.holeHighUp, sim::SUPPORT_UP) / NM_UNITS / 1.0e6);
            run.hashes.push_back(law.Hash());
            if (tick == ticks / 2)
                run.midShot = Take(law, g);
        }

        run.finalShot = Take(law, g);
        return run;
    }

    // セルに残る力の最大が limit 以下に入って出なくなる刻み(釣り合った刻み)と、そこまでに解いた節の和(掘った 1 回の費用)
    std::pair<uint32_t, uint64_t> SettledCost(const Run& run, double limit) {
        uint32_t settled = 0;
        for (uint32_t tick = (uint32_t)run.worstMn.size(); tick-- > 0;) {
            if ((double)run.worstMn[tick] > limit)
                break;

            settled = tick + 1;
        }

        uint64_t solved = 0;
        for (uint32_t tick = 0; tick < settled && tick < run.unknowns.size(); ++tick)
            solved += run.unknowns[tick];

        return {settled, solved};
    }

    // 最後の値 ± share × |最後 − 掘る前| に入って出なくなる刻み
    uint32_t TicksTo(const std::vector<double>& path, double beforeMm, double share) {
        const double finalMm = path.back();
        const double band = share * std::abs(finalMm - beforeMm);
        uint32_t ticks = 0;
        for (uint32_t tick = (uint32_t)path.size(); tick-- > 0;) {
            if (std::abs(path[tick] - finalMm) > band)
                break;

            ticks = tick + 1;
        }

        return ticks;
    }

    // 差の最大 ÷ 掘ったことで変わった量の最大(閉じない答え − 掘る前)
    double RelativeError(const std::vector<int64_t>& closed, const std::vector<int64_t>& full,
                         const std::vector<int64_t>& before) {
        double error = 0;
        double change = 0;
        for (size_t i = 0; i < full.size(); ++i) {
            error = std::max(error, std::abs((double)closed[i] - (double)full[i]));
            change = std::max(change, std::abs((double)full[i] - (double)before[i]));
        }

        return change == 0 ? 0 : error / change;
    }

    // 前の答えから値が動いたセルの、穴の外接の箱からの距離の最大(mm)
    double ReachMm(const Snapshot& shot, const Snapshot& before, const Geometry& g, uint64_t cellMm) {
        uint32_t reach = 0;
        for (uint32_t up = 0; up < g.height; ++up) {
            for (uint32_t x = 0; x < g.width; ++x) {
                const size_t i = x + (size_t)g.width * up;
                bool moved = false;
                for (uint32_t c = 0; c < sim::SUPPORT_COMPONENTS; ++c)
                    moved = moved || shot.values[i * 3 + c] != before.values[i * 3 + c];

                if (!moved)
                    continue;

                const uint32_t dx = x < g.holeLowX ? g.holeLowX - x : (x >= g.holeHighX ? x - g.holeHighX + 1 : 0);
                const uint32_t dup = up < g.holeLowUp ? g.holeLowUp - up
                                                      : (up >= g.holeHighUp ? up - g.holeHighUp + 1 : 0);
                reach = std::max(reach, std::max(dx, dup));
            }
        }

        return (double)reach * (double)cellMm;
    }

    double Ratio(uint64_t numerator, uint64_t denominator) {
        return denominator == 0 ? 0.0 : (double)numerator / (double)denominator;
    }

    std::string CaseName(const Case& c) {
        return std::format("{}×{} m・{} m・穴 {}×{} m", c.widthMm / 1000, c.heightMm / 1000, (double)c.cellMm / 1000.0,
                           (double)c.holeWidthMm / 1000.0, (double)c.holeHeightMm / 1000.0);
    }

    // --- 閉じ方(閉じない・倍率 × 広がる / 固定)---
    struct Mode {
        uint32_t multiplier = FULL;
        sim::SupportCloseGrowth growth = sim::SupportCloseGrowth::Grow;
    };

    constexpr std::array<Mode, 5> MODES = {{{FULL, sim::SupportCloseGrowth::Grow},
                                            {2, sim::SupportCloseGrowth::Grow},
                                            {4, sim::SupportCloseGrowth::Grow},
                                            {2, sim::SupportCloseGrowth::Fixed},
                                            {4, sim::SupportCloseGrowth::Fixed}}};

    std::string ModeName(const Mode& mode) {
        if (mode.multiplier == FULL)
            return "閉じない";

        return std::format("倍率 {}・{}", mode.multiplier,
                           mode.growth == sim::SupportCloseGrowth::Grow ? "広がる" : "固定");
    }

    // --- 1 つの場面: MODES の全部(と、bitCheck なら世界を覆う倍率・倍率 2 の 2 回目)---
    // 掘った 1 回の費用 = 釣り合うまでに解いた節の和(閉じない解は「セルに残る力 ≤ 重さ ÷ 1000」に入るまで・閉じた解は種が尽きるまでの全部)
    struct CaseResult {
        std::array<uint64_t, MODES.size()> eventSolved{};
    };

    CaseResult TestCase(const Case& c, uint32_t ticks, bool bitCheck) {
        const Geometry g = GeometryOf(c);
        const std::string name = CaseName(c);
        sim::SupportLaw before(Block(c, g, false), {});
        const Clock::time_point settleStart = Clock::now();
        const uint32_t settleTicks = Settle(before, CellWeightMn(c) / 10000.0);
        Log(Channel::Sim, Level::Info, "[{}] セル {}・掘る前の釣り合い {} 刻み({:.0f} ms)", name, before.CellCount(),
            settleTicks, MsSince(settleStart));
        Expect(settleTicks != 0, "掘る前の釣り合いに着く");
        const Snapshot beforeShot = Take(before, g);
        const double beforeSag = -(double)before.Value(g.centerX, g.holeHighUp, sim::SUPPORT_UP) / NM_UNITS / 1.0e6;
        const double weightMn = CellWeightMn(c);

        std::vector<Run> runs;
        for (const Mode& mode : MODES)
            runs.push_back(DigRun(c, g, before, mode.multiplier, mode.growth, ticks));

        const Run& full = runs[0];
        CaseResult result;
        for (size_t m = 0; m < runs.size(); ++m) {
            const Run& run = runs[m];
            const std::string mode = ModeName(MODES[m]);
            const double valueError = RelativeError(run.finalShot.values, full.finalShot.values, beforeShot.values);
            const double edgeError = RelativeError(run.finalShot.edges, full.finalShot.edges, beforeShot.edges);
            const bool still = run.midShot.values == run.finalShot.values;
            const auto [settled, settledSolved] = SettledCost(run, weightMn / 1000.0);
            result.eventSolved[m] = m == 0 ? settledSolved : run.solvedNodes;
            std::string path;
            for (size_t tick = 0; tick < run.unknowns.size() && tick < 12; ++tick)
                path += std::format(" {}", run.unknowns[tick]);

            Log(Channel::Sim, Level::Info,
                "[{}] {}: 掘った 1 回の費用(解いた節の和){}・閉じない ÷ これ "
                "{:.2f}・刻みごとの未知数{}・最後に解いた刻み {}・"
                "閉じる長さ {:.1f} m・1 刻み目の種 {}・セルに残る力の最大 ÷ 重さ: 最後 {:.5f}(重さ ÷ 1000 に入った刻み "
                "{})",
                name, mode, result.eventSolved[m], Ratio(result.eventSolved[0], result.eventSolved[m]), path,
                run.lastSolvingTick, (double)run.closeLengthMm / 1000.0, run.firstSeeds,
                (double)run.worstMn.back() / weightMn, settled);
            Log(Channel::Sim, Level::Info,
                "[{}] {}: 作り直し {:.2f} ms・1 刻み目 {:.2f} ms・{} 刻みの計 {:.1f} ms・節の値を解いた回数 {}・"
                "天井のたわみ {:.5f} → {:.5f} mm・着く刻み ±5 % {}・±1 % {}・閉じない答えとの差: たわみ {:.2f} "
                "%・縁の力 {:.2f} %・"
                "動いたセルの届いた距離 {:.1f} m・半分 → 最後で動かない {}",
                name, mode, run.buildMs, run.firstTickMs, ticks, run.totalMs, run.nodeUpdates, beforeSag,
                run.sagMm.back(), TicksTo(run.sagMm, beforeSag, 0.05), TicksTo(run.sagMm, beforeSag, 0.01),
                100.0 * valueError, 100.0 * edgeError, ReachMm(run.finalShot, beforeShot, g, c.cellMm) / 1000.0,
                still ? "はい" : "いいえ");
            Expect(run.ledgerOk, std::format("帳簿: {} {}", name, mode));
            // 固定の領域: 外の値は前の答えとビット一致(動いたセルは閉じる長さ + 種の 1 セルの内)
            if (MODES[m].multiplier != FULL && MODES[m].growth == sim::SupportCloseGrowth::Fixed) {
                const double reachMm = ReachMm(run.finalShot, beforeShot, g, c.cellMm);
                Expect(reachMm <= (double)(run.closeLengthMm + 2 * c.cellMm),
                       std::format("固定の領域の外が動かない: {} {}", name, mode));
            }
        }

        // 見た目の記録(S38): 穴の天井のたわみの道筋
        std::string header;
        for (const Mode& mode : MODES)
            header += " " + ModeName(mode);

        Log(Channel::Sim, Level::Info, "[{}] 天井のたわみの道筋(mm。刻み:{})", name, header);
        for (const uint32_t tick : {1u, 2u, 3u, 5u, 10u, 20u, 50u, 100u, 300u, 600u}) {
            if (tick > ticks)
                continue;

            std::string row;
            for (const Run& run : runs)
                row += std::format(" {:.5f}", run.sagMm[tick - 1]);

            Log(Channel::Sim, Level::Info, "[{}]   {:4}:{}", name, tick, row);
        }

        if (!bitCheck)
            return result;

        // 倍率が世界を覆えば閉じない解と毎刻みビット一致・2 回の実行でハッシュ一致
        // (閉じた領域は種が尽きた刻みから解かないので、比べるのは解いた刻みまで)
        const Run cover = DigRun(c, g, before, COVER, sim::SupportCloseGrowth::Grow, ticks);
        const auto solving = (ptrdiff_t)cover.lastSolvingTick;
        const bool coverSame = solving != 0 && rng::equal(cover.hashes.begin(), cover.hashes.begin() + solving,
                                                          full.hashes.begin(), full.hashes.begin() + solving);
        Expect(coverSame, "世界を覆う倍率は閉じない解と毎刻みビット一致");
        bool againSame = true;
        for (size_t m = 1; m < MODES.size(); ++m) {
            const Run again = DigRun(c, g, before, MODES[m].multiplier, MODES[m].growth, ticks);
            againSame = againSame && again.hashes == runs[m].hashes;
        }

        Expect(againSame, "閉じた解の 2 回の実行でハッシュ一致");
        Log(Channel::Sim, Level::Info, "[{}] 世界を覆う倍率と閉じない解のビット一致 {}・2 回の実行 {}", name,
            coverSame ? std::format("一致({} 刻み)", solving) : std::string("不一致"), againSame ? "一致" : "不一致");
        return result;
    }

    // --- 2 本目: 粗さで閉じる(T-1171)---
    // 混ざる場面: 粗いセル 2f(奥行も 2f)・穴の外接の箱からの距離が fineLengthMm の内の粗いセルを 2 × 2 の細かいセル f に分ける。
    //   穴は細かいセルで掘る(fineAbsent。粗いセルの 4 つとも穴なら粗いセルごと無くす)。座標は全部細かい塊(Geometry g)の細かい格子。
    //   変位は奥行によらず、面の力は奥行に比例するので、混ざる場面の面の力は ÷ 2 で全部細かい塊(奥行 f)と比べる。
    bool InHole(const Geometry& g, uint32_t x, uint32_t up) {
        return x >= g.holeLowX && x < g.holeHighX && up >= g.holeLowUp && up < g.holeHighUp;
    }

    // 細かい格子の [lowX, highX) × [lowUp, highUp) の箱から穴の箱までのチェビシェフの距離(セル。隣 = 1。ReachMm と同じ数え方)
    uint32_t BoxDistance(const Geometry& g, uint32_t lowX, uint32_t highX, uint32_t lowUp, uint32_t highUp) {
        const uint32_t dx = highX <= g.holeLowX ? g.holeLowX - (highX - 1)
                                                : (lowX >= g.holeHighX ? lowX - g.holeHighX + 1 : 0);
        const uint32_t dup = highUp <= g.holeLowUp ? g.holeLowUp - (highUp - 1)
                                                   : (lowUp >= g.holeHighUp ? lowUp - g.holeHighUp + 1 : 0);
        return std::max(dx, dup);
    }

    sim::SupportScene MixedBlock(const Case& c, const Geometry& g, uint64_t fineLengthMm, bool dug) {
        sim::SupportScene scene;
        scene.width = g.width / 2;
        scene.height = g.height / 2;
        scene.cellMm = 2 * c.cellMm;
        scene.stiffnessKpa = test::STONE_KPA;
        scene.densityKgPerM3 = test::STONE_DENSITY;
        const size_t cells = (size_t)scene.width * scene.height;
        scene.solid.assign(cells, 1);
        scene.clamps.assign(cells, 0);
        scene.fine.assign(cells, 0);
        if (dug)
            scene.fineAbsent.assign(cells * 4, 0);

        for (uint32_t up = 0; up < scene.height; ++up) {
            for (uint32_t x = 0; x < scene.width; ++x) {
                const size_t i = x + (size_t)scene.width * up;
                uint8_t clamp = 0;
                if (up == 0)
                    clamp |= sim::SUPPORT_CLAMP_BOTTOM;

                if (x == 0)
                    clamp |= sim::SUPPORT_CLAMP_LEFT;

                if (x + 1 == scene.width)
                    clamp |= sim::SUPPORT_CLAMP_RIGHT;

                scene.clamps[i] = clamp;
                const uint64_t distanceMm = (uint64_t)BoxDistance(g, 2 * x, 2 * x + 2, 2 * up, 2 * up + 2) * c.cellMm;
                if (distanceMm <= fineLengthMm)
                    scene.fine[i] = 1;

                if (!dug)
                    continue;

                uint32_t absent = 0;
                for (uint32_t part = 0; part < 4; ++part) {
                    const uint32_t fx = 2 * x + (part & 1);
                    const uint32_t fup = 2 * up + (part >> 1);
                    if (!InHole(g, fx, fup))
                        continue;

                    scene.fineAbsent[fx + (size_t)2 * scene.width * fup] = 1;
                    ++absent;
                }

                if (absent == 4)
                    scene.solid[i] = 0;
            }
        }

        return scene;
    }

    // 世界の木で段を重ねた時(2:1・距離 2 倍ごとに 1 段粗い・最も粗くて coarsestMm)のセルの数の見積もり(2 次元。形だけの数え)
    double TreeCells(const Geometry& g, uint64_t cellMm, uint64_t fineLengthMm, uint64_t coarsestMm) {
        double cells = 0;
        for (uint32_t up = 0; up < g.height; ++up) {
            for (uint32_t x = 0; x < g.width; ++x) {
                if (InHole(g, x, up))
                    continue;

                const uint64_t distanceMm = (uint64_t)BoxDistance(g, x, x + 1, up, up + 1) * cellMm;
                uint32_t level = 0;
                while ((cellMm << (level + 1)) <= coarsestMm && distanceMm > (fineLengthMm << level))
                    ++level;

                cells += 1.0 / (double)(1u << (2 * level));
            }
        }

        return cells;
    }

    // 掘った場面を ticks 刻み(閉じない)。mixed なら天井のたわみは細かい座標で読む
    Run StepDug(sim::SupportLaw& law, const Geometry& g, bool mixed, uint32_t ticks) {
        Run run;
        for (uint32_t tick = 1; tick <= ticks; ++tick) {
            const Clock::time_point start = Clock::now();
            law.Step();
            run.totalMs += MsSince(start);
            run.solvedNodes += law.LastUnknowns();
            run.nodeUpdates += law.LastNodeUpdates();
            run.unknowns.push_back(law.LastUnknowns());
            run.updates.push_back(law.LastNodeUpdates());

            const sim::SupportLedger ledger = law.Ledger();
            run.ledgerOk = run.ledgerOk && ledger.netMn == ledger.reactionMn - ledger.weightMn;
            run.worstMn.push_back(ledger.worstCellMn);
            const int64_t sag = mixed ? law.FineValue(g.centerX, g.holeHighUp, sim::SUPPORT_UP)
                                      : law.Value(g.centerX, g.holeHighUp, sim::SUPPORT_UP);
            run.sagMm.push_back(-(double)sag / NM_UNITS / 1.0e6);
            run.hashes.push_back(law.Hash());
        }

        return run;
    }

    // 掘ったことによる変化(後 − 前)の、混ざる場面と全部細かい塊の差(どれも全部細かい塊の変化の最大で割る)
    struct MixedError {
        double sag = 0;                     // 天井のたわみ
        double fineValue = 0;               // 細かいセルの変位(横・上)
        double coarseValue = 0;             // 粗いセルの変位(全部細かい塊の 4 つの平均と)
        double fineEdge = 0;                // 細かいセルどうしの面の縁の力
        double mixedEdge = 0;               // 違う大きさの面の縁の力
        double fineEdgeAbsolute = 0;        // 細かい面の掘った後の値そのものの差(掘る前からある細かさの違いも入る)
        double fineEdgeOfStress = 0;        // 同じ差 ÷ 全部細かい塊の掘った後の縁の力の最大(変化ではなく値の大きさで)
        double mixedEdgeOfStress = 0;       // 違う大きさの面の掘った後の値の差 ÷ 同じ最大
        double worstAbsoluteDistanceM = 0;  // fineEdgeAbsolute が最大の面の穴からの距離
        std::string mixedWorst;             // mixedEdgeOfStress が最大の面(軸・場所・両側の縁の力)
        double worstEdgeDistanceM = 0;      // fineEdge が最大の面の穴からの距離
    };

    double ComponentChange(const Snapshot& after, const Snapshot& before, size_t cell, uint32_t component) {
        const size_t at = cell * sim::SUPPORT_COMPONENTS + component;
        return (double)after.values[at] - (double)before.values[at];
    }

    void CompareMixedValues(const sim::SupportLaw& before, const sim::SupportLaw& after, const Snapshot& refBefore,
                            const Snapshot& refAfter, const Geometry& g, MixedError& error) {
        double change = 0;
        for (size_t cell = 0; cell < (size_t)g.width * g.height; ++cell) {
            for (uint32_t c = 0; c < 2; ++c)
                change = std::max(change, std::abs(ComponentChange(refAfter, refBefore, cell, c)));
        }

        for (uint32_t up = 0; up < g.height; ++up) {
            for (uint32_t x = 0; x < g.width; ++x) {
                const bool fine = after.IsFine(x, up);
                if (InHole(g, x, up) || (!fine && (x % 2 != 0 || up % 2 != 0)))
                    continue;

                for (uint32_t c = 0; c < 2; ++c) {
                    const double mixed = (double)after.FineValue(x, up, c) - (double)before.FineValue(x, up, c);
                    double reference = 0;
                    if (fine) {
                        reference = ComponentChange(refAfter, refBefore, x + (size_t)g.width * up, c);
                    } else {
                        for (uint32_t part = 0; part < 4; ++part) {
                            const size_t cell = x + (part & 1) + (size_t)g.width * (up + (part >> 1));
                            reference += ComponentChange(refAfter, refBefore, cell, c) / 4.0;
                        }
                    }

                    double& worst = fine ? error.fineValue : error.coarseValue;
                    worst = std::max(worst, std::abs(mixed - reference) / change);
                }
            }
        }
    }

    void CompareMixedEdges(const sim::SupportLaw& before, const sim::SupportLaw& after, const Snapshot& refBefore,
                           const Snapshot& refAfter, const Geometry& g, uint64_t cellMm, MixedError& error) {
        // 全部細かい塊の変化の最大(穴に接しない面の全部)
        const auto faceExists = [&](uint32_t x, uint32_t up, uint32_t axis) {
            const uint32_t nx = x + (axis == 0 ? 1 : 0);
            const uint32_t nup = up + (axis == 1 ? 1 : 0);
            return nx < g.width && nup < g.height && !InHole(g, x, up) && !InHole(g, nx, nup);
        };

        double change = 0;
        double stress = 0;
        for (uint32_t up = 0; up < g.height; ++up) {
            for (uint32_t x = 0; x < g.width; ++x) {
                for (uint32_t axis = 0; axis < 2; ++axis) {
                    if (!faceExists(x, up, axis))
                        continue;

                    const size_t e = ((x + (size_t)g.width * up) * 2 + axis) * 2;
                    for (size_t side = 0; side < 2; ++side)
                        stress = std::max(stress, std::abs((double)refAfter.edges[e + side]));

                    for (size_t side = 0; side < 2; ++side)
                        change = std::max(
                            change, std::abs((double)refAfter.edges[e + side] - (double)refBefore.edges[e + side]));
                }
            }
        }

        for (uint32_t up = 0; up < g.height; ++up) {
            for (uint32_t x = 0; x < g.width; ++x) {
                for (uint32_t axis = 0; axis < 2; ++axis) {
                    if (!faceExists(x, up, axis))
                        continue;

                    const bool fineSelf = after.IsFine(x, up);
                    const bool fineNext = after.IsFine(x + (axis == 0 ? 1 : 0), up + (axis == 1 ? 1 : 0));
                    if (!fineSelf && !fineNext)
                        continue;

                    const sim::SupportFaceForces mixedAfter = after.FineFaceForces(x, up, axis);
                    const sim::SupportFaceForces mixedBefore = before.FineFaceForces(x, up, axis);
                    const size_t e = ((x + (size_t)g.width * up) * 2 + axis) * 2;
                    for (size_t side = 0; side < 2; ++side) {
                        const double afterMn = (double)(side == 0 ? mixedAfter.lowSideMn : mixedAfter.highSideMn) / 2.0;
                        const double beforeMn = (double)(side == 0 ? mixedBefore.lowSideMn : mixedBefore.highSideMn) /
                                                2.0;
                        const double referenceAfter = (double)refAfter.edges[e + side];
                        const double referenceChange = referenceAfter - (double)refBefore.edges[e + side];
                        const double differ = std::abs((afterMn - beforeMn) - referenceChange) / change;
                        const double absolute = std::abs(afterMn - referenceAfter);
                        if (!fineSelf || !fineNext) {
                            error.mixedEdge = std::max(error.mixedEdge, differ);
                            if (absolute / stress > error.mixedEdgeOfStress) {
                                error.mixedEdgeOfStress = absolute / stress;
                                error.mixedWorst = std::format(
                                    "軸 {}・細かい座標 ({}, {})・高さ {:.2f} m・混ざる 低 {:.0f} 高 {:.0f}・全部細かい "
                                    "低 {} 高 {} mN",
                                    axis, x, up, (double)(up * cellMm) / 1000.0, (double)mixedAfter.lowSideMn / 2.0,
                                    (double)mixedAfter.highSideMn / 2.0, refAfter.edges[e], refAfter.edges[e + 1]);
                            }

                            continue;
                        }

                        if (absolute / change > error.fineEdgeAbsolute) {
                            error.fineEdgeAbsolute = absolute / change;
                            error.fineEdgeOfStress = absolute / stress;
                            error.worstAbsoluteDistanceM = (double)(BoxDistance(g, x, x + 1, up, up + 1) * cellMm) /
                                                           1000.0;
                        }

                        if (differ > error.fineEdge) {
                            error.fineEdge = differ;
                            error.worstEdgeDistanceM = (double)(BoxDistance(g, x, x + 1, up, up + 1) * cellMm) / 1000.0;
                        }
                    }
                }
            }
        }
    }

    struct CoarseResult {
        uint64_t fullCost = 0;
        std::array<uint64_t, 2> mixedCost{};
        std::array<double, 2> treeCells{};      // 最も粗くて 8 m
        std::array<double, 2> treeCellsWide{};  // 最も粗くて 32 m(塊の高さまで)
        uint32_t fullCells = 0;
    };

    constexpr std::array<uint32_t, 2> COARSE_MULTIPLIERS = {2, 4};

    // 1 つの場面: 全部細かい塊(閉じない)と、粗さで閉じた塊(倍率 2・4)
    CoarseResult CoarseCase(const Case& c, uint32_t ticks, bool bitCheck, size_t multiplierCount) {
        const Geometry g = GeometryOf(c);
        const std::string name = CaseName(c);
        const double weightMn = CellWeightMn(c);
        sim::SupportLaw reference(Block(c, g, false), {});
        const uint32_t referenceSettle = Settle(reference, weightMn / 10000.0);
        Expect(referenceSettle != 0, "全部細かい塊の掘る前の釣り合いに着く");
        const Snapshot refBefore = Take(reference, g);
        const double refBeforeSag = -(double)reference.Value(g.centerX, g.holeHighUp, sim::SUPPORT_UP) / NM_UNITS /
                                    1.0e6;

        sim::SupportLaw referenceDug(Block(c, g, true), {});
        referenceDug.CarryFrom(reference);
        const Run full = StepDug(referenceDug, g, false, ticks);
        const Snapshot refAfter = Take(referenceDug, g);
        const auto [fullSettled, fullCost] = SettledCost(full, weightMn / 1000.0);
        CoarseResult result{.fullCost = fullCost, .fullCells = referenceDug.CellCount()};
        Log(Channel::Sim, Level::Info,
            "[{}] 粗さで閉じる: 全部細かい塊 セル {}・掘った 1 回の費用 {}(釣り合った刻み {})・天井のたわみ {:.5f} → "
            "{:.5f} mm",
            name, referenceDug.CellCount(), fullCost, fullSettled, refBeforeSag, full.sagMm.back());

        // 混ざる場面の細かいセル 1 つの重さ(奥行 2f)= 全部細かい塊のセルの 2 倍
        const double mixedFineWeightMn = 2.0 * weightMn;
        for (size_t m = 0; m < multiplierCount; ++m) {
            const uint64_t fineLengthMm = COARSE_MULTIPLIERS[m] * std::max(c.holeWidthMm, c.holeHeightMm);
            sim::SupportLaw before(MixedBlock(c, g, fineLengthMm, false), {});
            const uint32_t settle = Settle(before, mixedFineWeightMn / 10000.0);
            Expect(settle != 0,
                   std::format("混ざる塊の掘る前の釣り合いに着く: {} 倍率 {}", name, COARSE_MULTIPLIERS[m]));
            const double beforeSag = -(double)before.FineValue(g.centerX, g.holeHighUp, sim::SUPPORT_UP) / NM_UNITS /
                                     1.0e6;

            const sim::SupportScene dugScene = MixedBlock(c, g, fineLengthMm, true);
            const Clock::time_point buildStart = Clock::now();
            sim::SupportLaw after(dugScene, {});
            after.CarryFrom(before);
            const double buildMs = MsSince(buildStart);
            const Run run = StepDug(after, g, true, ticks);
            const auto [settled, cost] = SettledCost(run, mixedFineWeightMn / 1000.0);
            uint64_t updates = 0;
            for (uint32_t tick = 0; tick < settled; ++tick)
                updates += run.updates[tick];

            MixedError error;
            CompareMixedValues(before, after, refBefore, refAfter, g, error);
            CompareMixedEdges(before, after, refBefore, refAfter, g, c.cellMm, error);
            const double referenceSag = full.sagMm.back() - refBeforeSag;
            error.sag = referenceSag == 0
                            ? 0
                            : std::abs((run.sagMm.back() - beforeSag) - referenceSag) / std::abs(referenceSag);
            result.mixedCost[m] = cost;
            result.treeCells[m] = TreeCells(g, c.cellMm, fineLengthMm, 8000);
            result.treeCellsWide[m] = TreeCells(g, c.cellMm, fineLengthMm, 32000);

            Log(Channel::Sim, Level::Info,
                "[{}] 粗さで閉じる 倍率 {}(細かい所 {:.1f} m): セル {}(全部細かい {})・木の段の見積もり "
                "{:.0f}・掘る前の釣り合い {} 刻み・"
                "掘った 1 回の費用 {}(全部細かい ÷ これ {:.2f})・節の値を解いた回数 {}・釣り合った刻み {}・作り直し "
                "{:.2f} ms・"
                "{} 刻みの計 {:.1f} ms・天井のたわみ {:.5f} → {:.5f} mm・着く刻み ±1 % {}",
                name, COARSE_MULTIPLIERS[m], (double)fineLengthMm / 1000.0, after.CellCount(), referenceDug.CellCount(),
                result.treeCells[m], settle, cost, Ratio(fullCost, cost), updates, settled, buildMs, ticks, run.totalMs,
                beforeSag, run.sagMm.back(), TicksTo(run.sagMm, beforeSag, 0.01));
            Log(Channel::Sim, Level::Info,
                "[{}] 粗さで閉じる 倍率 {}: 変化の差 ÷ 全部細かい塊の変化の最大: たわみ {:.2f} %・変位 細かい {:.2f} "
                "%・粗い {:.2f} %・"
                "縁の力 細かい面 {:.2f} %(最大の所 穴から {:.1f} m)・違う大きさの面 {:.2f} "
                "%・細かい面の掘った後の値の差 {:.2f} %"
                "(穴から {:.1f} m。÷ 縁の力の最大 {:.3f} %)・違う大きさの面の掘った後の値の差 ÷ 縁の力の最大 {:.3f} %",
                name, COARSE_MULTIPLIERS[m], 100.0 * error.sag, 100.0 * error.fineValue, 100.0 * error.coarseValue,
                100.0 * error.fineEdge, error.worstEdgeDistanceM, 100.0 * error.mixedEdge,
                100.0 * error.fineEdgeAbsolute, error.worstAbsoluteDistanceM, 100.0 * error.fineEdgeOfStress,
                100.0 * error.mixedEdgeOfStress);
            Log(Channel::Sim, Level::Info, "[{}] 粗さで閉じる 倍率 {}: 違う大きさの面の値の差が最大の面: {}", name,
                COARSE_MULTIPLIERS[m], error.mixedWorst);
            Expect(run.ledgerOk, std::format("帳簿(粗さで閉じる): {} 倍率 {}", name, COARSE_MULTIPLIERS[m]));

            if (!bitCheck)
                continue;

            sim::SupportLaw again(dugScene, {});
            again.CarryFrom(before);
            const Run second = StepDug(again, g, true, ticks);
            Expect(second.hashes == run.hashes, "粗さで閉じた解の 2 回の実行でハッシュ一致");
            Log(Channel::Sim, Level::Info, "[{}] 粗さで閉じる 倍率 {}: 2 回の実行 {}", name, COARSE_MULTIPLIERS[m],
                second.hashes == run.hashes ? "一致" : "不一致");
        }

        return result;
    }

    void RunCoarse(uint32_t ticks) {
        for (const auto& [holeWidth, holeHeight] : {std::pair{1000ull, 1000ull}, std::pair{4000ull, 2000ull}}) {
            const CoarseResult small = CoarseCase({32000, 16000, 500, holeWidth, holeHeight}, ticks, false, 2);
            const CoarseResult large = CoarseCase({64000, 32000, 500, holeWidth, holeHeight}, ticks, false, 2);
            const CoarseResult fine = CoarseCase({32000, 16000, 250, holeWidth, holeHeight}, ticks, false, 2);
            Log(Channel::Sim, Level::Info,
                "穴 {}×{} m 全部細かい: 掘った 1 回の費用 64×32 ÷ 32×16 = {:.3f}・0.25 ÷ 0.5 m = {:.3f}",
                (double)holeWidth / 1000.0, (double)holeHeight / 1000.0, Ratio(large.fullCost, small.fullCost),
                Ratio(fine.fullCost, small.fullCost));
            for (size_t m = 0; m < COARSE_MULTIPLIERS.size(); ++m) {
                Log(Channel::Sim, Level::Info,
                    "穴 {}×{} m 粗さで閉じる 倍率 {}: 掘った 1 回の費用 64×32 ÷ 32×16 = {:.3f}・0.25 ÷ 0.5 m = {:.3f}・"
                    "全部細かい ÷ これ = 32×16 {:.2f}・64×32 {:.2f}・0.25 m {:.2f}・木の段の見積もりのセル(最も粗くて "
                    "8 m / 32 m)"
                    "32×16 {:.0f} / {:.0f}・64×32 {:.0f} / {:.0f}・0.25 m {:.0f} / {:.0f}",
                    (double)holeWidth / 1000.0, (double)holeHeight / 1000.0, COARSE_MULTIPLIERS[m],
                    Ratio(large.mixedCost[m], small.mixedCost[m]), Ratio(fine.mixedCost[m], small.mixedCost[m]),
                    Ratio(small.fullCost, small.mixedCost[m]), Ratio(large.fullCost, large.mixedCost[m]),
                    Ratio(fine.fullCost, fine.mixedCost[m]), small.treeCells[m], small.treeCellsWide[m],
                    large.treeCells[m], large.treeCellsWide[m], fine.treeCells[m], fine.treeCellsWide[m]);
            }
        }
    }

    int RunAll(std::span<char*> arguments) {
        bool measure = false;
        bool coarse = false;
        uint32_t ticks = 600;
        for (size_t i = 1; i < arguments.size(); ++i) {
            const std::string_view name = arguments[i];
            if (name == "--measure")
                measure = true;
            else if (name == "--coarse")
                coarse = true;
            else if (name == "--ticks" && i + 1 < arguments.size())
                ticks = (uint32_t)std::atoi(arguments[++i]);
        }

        if (coarse) {
            RunCoarse(ticks);
        } else if (!measure) {
            (void)TestCase({32000, 16000, 500, 1000, 1000}, ticks, true);
            (void)CoarseCase({32000, 16000, 500, 1000, 1000}, ticks, true, 1);
        } else {
            for (const auto& [holeWidth, holeHeight] : {std::pair{1000ull, 1000ull}, std::pair{4000ull, 2000ull}}) {
                const CaseResult small = TestCase({32000, 16000, 500, holeWidth, holeHeight}, ticks, true);
                const CaseResult large = TestCase({64000, 32000, 500, holeWidth, holeHeight}, ticks, false);
                const CaseResult fine = TestCase({32000, 16000, 250, holeWidth, holeHeight}, ticks, false);
                for (size_t m = 0; m < MODES.size(); ++m) {
                    Log(Channel::Sim, Level::Info,
                        "穴 {}×{} m {}: 掘った 1 回の費用 64×32 ÷ 32×16 = {:.3f}・0.25 ÷ 0.5 m = {:.3f}・"
                        "閉じない ÷ これ = 32×16 {:.2f}・64×32 {:.2f}・0.25 m {:.2f}",
                        (double)holeWidth / 1000.0, (double)holeHeight / 1000.0, ModeName(MODES[m]),
                        Ratio(large.eventSolved[m], small.eventSolved[m]),
                        Ratio(fine.eventSolved[m], small.eventSolved[m]),
                        Ratio(small.eventSolved[0], small.eventSolved[m]),
                        Ratio(large.eventSolved[0], large.eventSolved[m]),
                        Ratio(fine.eventSolved[0], fine.eventSolved[m]));
                }
            }
        }

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "support_cost_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "support_cost_test: OK");
        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = RunAll(std::span(argv, (size_t)argc));
    SingletonFinalizer::Finalize();

    return exitCode;
}
