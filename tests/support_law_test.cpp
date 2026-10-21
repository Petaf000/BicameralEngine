// support_law_test.cpp — 支えの法則の試作(sim/support_law。研究 R-B1・T-0871・22 §6)の CPU の測り。
// 場面(石: 硬さ 50 GPa・密度 2600 kg/m³。縦の面・奥行 1 セル):
//   1. 1 セルの厚さの張り出し 4 m(0.5 m と 0.25 m 〔厚さ 0.5 m = 2 セル〕)・片持ち梁 16 × 4 m(0.5 m と 2 m)・両端を固定した梁 8 × 1 m
//      → 先 / 真ん中のたわみを梁の式(Timoshenko)と比べる。2 つの形(成分ごと / 格子の梁)の両方
//   2. 帳簿: 毎刻み Σ セルの上向きの力 = 固定した面の力 − 重さ(解けきらなくても厳密)
//   3. 決定性: 2 回の実行でハッシュ一致
//   4. 釣り合いに着くまでの刻みの数(先のたわみが最後の値の ±5 %・±1 % に入る刻み)と、掘った直後の見た目(たわみの分布)の記録
//   5. (T-0944)アーチ(内径 4 m・厚さ 1 m の半円の階段。0.5 / 0.25 m)と洞窟の天井(幅 24 × 高さ 10 m・8 × 4 m の空洞。2 / 1 / 0.5 / 0.25 m)を
//      同じ階段の形を細かく刻んだ連続体(浮動小数点の捨てる試作で求めた値。下の表)と比べる。釣り合いに着く刻みと頂のたわみの道筋も記録
//   6. (T-0944)表面の引きの応力: 面の力(FaceForces)の縁の力 ÷ dx² を片持ち梁の梁の式 6M/(b h²) と比べる
//   7. (T-0919)解き方 2 つ(成分を持つ節で 1 つの系 Coupled / 結合を遅らせる Lagged)で 1〜5 を並べる: 着く刻み・答えの比(Coupled ÷ Lagged)。
//      すぐ折れるはずの棚: 1 セルの張り出しの根の面の引きの応力が「割れの値」(最後の値の 1/2・9/10 と置く)を超える刻み
//      (割れの本体 R-B2 がまだ無いので、超える刻みを 2 つの解き方で比べる)。段の斜めの棒の硬さ(ka/8・ka/12・ka/16・ka/24)の測り直し
// 合格の数値(22 §6: ±5 %)の判定は記録としてログに出し、試験が落ちるのは帳簿・決定性・符号だけ(研究の測り。T-0871 に結果)。
// 引数: [--ticks n](Lagged の刻み。既定 2000)[--coupled-ticks n](既定 300)[--cycles n](1 つの系の V の回数。既定 2)[--record](掘った場面の分布をログへ)
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

using namespace bicameral;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    constexpr uint64_t STONE_KPA = 50000000;
    constexpr uint64_t STONE_DENSITY = 2600;
    constexpr double NM_UNITS = 65536.0;  // 2^-16 nm

    const char* SolveName(sim::SupportSolve solve) {
        return solve == sim::SupportSolve::Coupled ? "1 つの系" : "遅らせる";
    }

    // 1 つの系の V の回数(--cycles。既定は部品の既定 2)
    uint32_t coupledCycles = 2;

    sim::EquilibriumBlockOptions Blocks() {
        return {.cycles = coupledCycles};
    }

    // 解き方ごとの最後の値(答えの比 Coupled ÷ Lagged の記録)
    std::vector<double> lastFinals;

    // --- 場面 ---
    sim::SupportScene Beam(uint32_t width, uint32_t height, uint64_t cellMm, bool bothEnds) {
        sim::SupportScene scene;
        scene.width = width;
        scene.height = height;
        scene.cellMm = cellMm;
        scene.stiffnessKpa = STONE_KPA;
        scene.densityKgPerM3 = STONE_DENSITY;
        scene.solid.assign((size_t)width * height, 1);
        scene.clamps.assign((size_t)width * height, 0);
        for (uint32_t up = 0; up < height; ++up) {
            scene.clamps[(size_t)width * up] |= sim::SUPPORT_CLAMP_LEFT;
            if (bothEnds)
                scene.clamps[width - 1 + (size_t)width * up] |= sim::SUPPORT_CLAMP_RIGHT;
        }

        return scene;
    }

    // 列 x の上の変位の平均(mm。下向きを正)
    double SagMm(const sim::SupportLaw& law, uint32_t x, uint32_t height) {
        double sum = 0;
        for (uint32_t up = 0; up < height; ++up)
            sum += (double)law.Value(x, up, sim::SUPPORT_UP);

        return -sum / height / NM_UNITS / 1.0e6;
    }

    // 梁の式(Timoshenko = Euler–Bernoulli の曲げ + ずれ。奥行 = セル・この宇宙の ν = 0 なので G = E/2・κ = 5/6)。
    // 片持ち: 根から x の点。両端固定: 真ん中。深い梁(16 × 4 m)はずれの分が数 % 効く
    constexpr double SHEAR_FACTOR = 5.0 / 6.0;

    double CantileverMm(double lengthM, double heightM, double cellM, double xM) {
        const double load = STONE_DENSITY * 9.80665 * heightM * cellM;
        const double inertia = cellM * heightM * heightM * heightM / 12.0;
        const double modulus = STONE_KPA * 1000.0;
        const double bend = load * xM * xM * (6 * lengthM * lengthM - 4 * lengthM * xM + xM * xM) /
                            (24 * modulus * inertia);
        const double shear = load * (lengthM * xM - xM * xM / 2) / (SHEAR_FACTOR * modulus / 2 * heightM * cellM);

        return (bend + shear) * 1000.0;
    }

    double ClampedMidMm(double lengthM, double heightM, double cellM) {
        const double load = STONE_DENSITY * 9.80665 * heightM * cellM;
        const double inertia = cellM * heightM * heightM * heightM / 12.0;

        const double modulus = STONE_KPA * 1000.0;
        const double bend = load * std::pow(lengthM, 4) / (384 * modulus * inertia);
        const double shear = load * lengthM * lengthM / (8 * SHEAR_FACTOR * modulus / 2 * heightM * cellM);

        return (bend + shear) * 1000.0;
    }

    struct Settle {
        double finalMm = 0;
        uint32_t ticksTo5 = 0;  // 最後の値の ±5 % に入ったまま出なくなった刻み(0 = 入らない)
        uint32_t ticksTo1 = 0;
        uint64_t hash = 0;
        bool ledgerOk = true;
    };

    // ticks 刻み回して、列 x のたわみの道筋から着いた刻みを数える
    Settle Run(sim::SupportLaw& law, uint32_t x, uint32_t height, uint32_t ticks) {
        std::vector<double> path(ticks);
        Settle settle;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            law.Step();
            path[tick] = SagMm(law, x, height);
            const sim::SupportLedger ledger = law.Ledger();
            settle.ledgerOk = settle.ledgerOk && ledger.netMn == ledger.reactionMn - ledger.weightMn;
        }

        settle.finalMm = path.back();
        for (uint32_t tick = ticks; tick-- > 0;) {
            const double error = std::abs(path[tick] / settle.finalMm - 1.0);
            if (error <= 0.05)
                settle.ticksTo5 = tick + 1;

            if (error <= 0.01)
                settle.ticksTo1 = tick + 1;

            if (error > 0.05)
                break;
        }

        settle.hash = law.Hash();

        return settle;
    }

    // --- 5. アーチと洞窟の天井(T-0944)---
    // 基準(浮動小数点の捨てる試作 T-0944。E = 1・ρg = 1 の単位〔たわみ = 値 × ρg/E〕):
    //   continuumRef = 同じ階段の形を各セル 16・32 × 16・32(洞窟の 2 m は 64)の非適合モードの四辺形(Q6)で解き、刻みの細かさへ外挿した値
    //   latticeSolved = 同じ格子の梁を直接解いた値(この試験の整数の解きが着く先。解けきったかの確かめ)
    struct ShapeCase {
        const char* name;
        bool arch;
        uint64_t cellMm;
        double continuumRef;
        double latticeSolved;
        bool braces = false;  // 段の斜めの棒(2 本目)
    };

    constexpr double STONE_SAG_PER_UNIT_MM = STONE_DENSITY * 9.80665 / (STONE_KPA * 1000.0) * 1000.0;

    // アーチ: 内径 4 m・厚さ 1 m の半円。セルの中心が内径〜外径にあるセル。下の段を地面に固定
    sim::SupportScene Arch(uint64_t cellMm) {
        const auto inner = (int64_t)(4000 / cellMm);
        const auto outer = (int64_t)(5000 / cellMm);
        sim::SupportScene scene = Beam((uint32_t)(2 * outer), (uint32_t)outer, cellMm, false);
        for (int64_t up = 0; up < outer; ++up) {
            for (int64_t x = 0; x < 2 * outer; ++x) {
                // 中心の 2 倍の座標(セルの大きさ ÷ 2 の単位)
                const int64_t cx = 2 * x - 2 * outer + 1;
                const int64_t cy = 2 * up + 1;
                const int64_t radius = cx * cx + cy * cy;
                const bool inside = radius >= 4 * inner * inner && radius <= 4 * outer * outer;
                const size_t i = (size_t)x + (size_t)(2 * outer) * (size_t)up;
                scene.solid[i] = inside ? 1 : 0;
                scene.clamps[i] = inside && up == 0 ? sim::SUPPORT_CLAMP_BOTTOM : 0;
            }
        }

        return scene;
    }

    // 洞窟: 幅 24 × 高さ 10 m の岩・x 8〜16 m・高さ 4〜8 m の空洞(天井の厚さ 2 m)。下を地面に固定
    sim::SupportScene Cave(uint64_t cellMm) {
        const auto width = (uint32_t)(24000 / cellMm);
        const auto height = (uint32_t)(10000 / cellMm);
        sim::SupportScene scene = Beam(width, height, cellMm, false);
        for (uint32_t up = 0; up < height; ++up) {
            for (uint32_t x = 0; x < width; ++x) {
                const uint64_t cx = (2 * x + 1) * cellMm;
                const uint64_t cy = (2 * up + 1) * cellMm;
                const bool hollow = cx > 16000 && cx < 32000 && cy > 8000 && cy < 16000;
                scene.solid[x + (size_t)width * up] = hollow ? 0 : 1;
                scene.clamps[x + (size_t)width * up] = !hollow && up == 0 ? sim::SUPPORT_CLAMP_BOTTOM : 0;
            }
        }

        return scene;
    }

    // 見る所: アーチは頂(真ん中の 2 列の全部のセル)・洞窟は天井の下の面(x 12 m を挟む 2 セル)。下向きを正(mm)
    double ShapeSagMm(const sim::SupportLaw& law, const sim::SupportScene& scene, bool arch) {
        double sum = 0;
        int count = 0;
        const uint32_t middle = arch ? scene.width / 2 : (uint32_t)(12000 / scene.cellMm);
        const auto ceiling = (uint32_t)(8000 / scene.cellMm);
        for (const uint32_t x : {middle - 1, middle}) {
            for (uint32_t up = 0; up < scene.height; ++up) {
                if (scene.solid[x + (size_t)scene.width * up] == 0 || (!arch && up != ceiling))
                    continue;

                sum += (double)law.Value(x, up, sim::SUPPORT_UP);
                ++count;
            }
        }

        return -sum / count / NM_UNITS / 1.0e6;
    }

    void TestShape(const ShapeCase& shape, sim::SupportSolve solve, uint32_t ticks, bool record) {
        const sim::SupportScene scene = shape.arch ? Arch(shape.cellMm) : Cave(shape.cellMm);
        sim::SupportLaw law(scene, {.stairBraces = shape.braces, .solve = solve, .blocks = Blocks()});
        std::vector<double> path(ticks);
        bool ledgerOk = true;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            law.Step();
            path[tick] = ShapeSagMm(law, scene, shape.arch);
            const sim::SupportLedger ledger = law.Ledger();
            ledgerOk = ledgerOk && ledger.netMn == ledger.reactionMn - ledger.weightMn;
        }

        const double finalMm = path.back();
        uint32_t ticksTo5 = 0;
        uint32_t ticksTo1 = 0;
        for (uint32_t tick = ticks; tick-- > 0;) {
            const double error = std::abs(path[tick] / finalMm - 1.0);
            if (error <= 0.01)
                ticksTo1 = tick + 1;

            if (error > 0.05)
                break;

            ticksTo5 = tick + 1;
        }

        const double reference = shape.continuumRef * STONE_SAG_PER_UNIT_MM;
        const double solved = shape.latticeSolved * STONE_SAG_PER_UNIT_MM;
        const double ratio = finalMm / reference;
        lastFinals.push_back(finalMm);
        Log(Channel::Sim, Level::Info,
            "[{}] {}{} {} m({} セル): たわみ {:.5f} mm / 連続体 {:.5f} mm = {:.3f}({})・解けきった格子の梁の "
            "{:.4f}・着く刻み ±5 % {}・±1 % {}・{} 刻み",
            SolveName(solve), shape.name, shape.braces ? "(段の斜めの棒)" : "", (double)shape.cellMm / 1000.0,
            law.CellCount(), finalMm, reference, ratio, std::abs(ratio - 1) <= 0.05 ? "±5 % 合格" : "±5 % 不合格",
            finalMm / solved, ticksTo5, ticksTo1, ticks);
        Expect(ledgerOk, std::string("帳簿: ") + shape.name);
        Expect(finalMm > 0, std::string("重さで下がる: ") + shape.name);
        if (!record)
            return;

        std::string row;
        for (const uint32_t tick : {1u, 10u, 30u, 60u, 120u, 300u, 600u, 1200u, ticks}) {
            if (tick <= ticks)
                row += std::format(" {}:{:.3f}", tick, path[tick - 1] / finalMm);
        }

        Log(Channel::Sim, Level::Info, "[{}] {}{} {} m: 見る所のたわみの道筋(刻み:最後の値との比){}", SolveName(solve),
            shape.name, shape.braces ? "(段の斜めの棒)" : "", (double)shape.cellMm / 1000.0, row);
    }

    void TestShapes(sim::SupportSolve solve, uint32_t ticks, bool record) {
        const ShapeCase cases[] = {
            {"アーチ 内径 4 m・厚さ 1 m", true, 500, 119.00, 135.380},
            {"アーチ 内径 4 m・厚さ 1 m", true, 250, 94.29, 99.651},
            {"アーチ 内径 4 m・厚さ 1 m", true, 500, 119.00, 114.096, true},
            {"アーチ 内径 4 m・厚さ 1 m", true, 250, 94.29, 92.761, true},
            {"洞窟の天井 24 × 10 m", false, 2000, 169.94, 180.741},
            {"洞窟の天井 24 × 10 m", false, 1000, 177.45, 183.991},
            {"洞窟の天井 24 × 10 m", false, 500, 179.43, 182.856},
            {"洞窟の天井 24 × 10 m", false, 250, 179.94, 181.657},
            {"洞窟の天井 24 × 10 m", false, 2000, 169.94, 162.936, true},
            {"洞窟の天井 24 × 10 m", false, 1000, 177.45, 174.479, true},
            {"洞窟の天井 24 × 10 m", false, 500, 179.43, 178.265, true},
            {"洞窟の天井 24 × 10 m", false, 250, 179.94, 179.482, true},
        };
        for (const ShapeCase& shape : cases)
            TestShape(shape, solve, ticks, record);

        sim::SupportLaw first(Arch(500), {.stairBraces = true, .solve = solve, .blocks = Blocks()});
        sim::SupportLaw again(Arch(500), {.stairBraces = true, .solve = solve, .blocks = Blocks()});
        for (uint32_t tick = 0; tick < 200; ++tick) {
            first.Step();
            again.Step();
        }

        Expect(first.Hash() == again.Hash(), "決定性: アーチ");
    }

    // --- 6. 表面の引きの応力(T-0944): 片持ち梁 4 m の根から 1 m の切り口の上の縁 ---
    void TestSurfaceStress(uint64_t cellMm, uint32_t height, sim::SupportSolve solve, uint32_t ticks) {
        const auto width = (uint32_t)(4000 / cellMm);
        sim::SupportLaw law(Beam(width, height, cellMm, false), {.solve = solve, .blocks = Blocks()});
        for (uint32_t tick = 0; tick < ticks; ++tick)
            law.Step();

        // 切り口 x = 1 m は、セル (1 m ÷ dx − 1) と右の隣の間の面
        const uint32_t cut = (uint32_t)(1000 / cellMm) - 1;
        const sim::SupportFaceForces top = law.FaceForces(cut, height - 1, 0);
        const sim::SupportFaceForces bottom = law.FaceForces(cut, 0, 0);
        const double cellM = (double)cellMm / 1000.0;
        const double areaM2 = cellM * cellM;
        const double heightM = cellM * height;
        const double load = STONE_DENSITY * 9.80665 * heightM * cellM;
        const double moment = load * 3.0 * 3.0 / 2;
        const double beamKpa = 6 * moment / (cellM * heightM * heightM) / 1000.0;
        const double topKpa = (double)top.highSideMn / 1000.0 / areaM2 / 1000.0;
        const double bottomKpa = (double)bottom.lowSideMn / 1000.0 / areaM2 / 1000.0;
        Log(Channel::Sim, Level::Info,
            "[{}] 表面の引きの応力 片持ち梁 4 m(厚さ {} セル・{} m)根から 1 m: 上の縁 {:.2f} kPa・下の縁 {:.2f} kPa / "
            "梁の式 ±{:.2f} kPa = {:.3f} / {:.3f}",
            SolveName(solve), height, cellM, topKpa, bottomKpa, beamKpa, topKpa / beamKpa, -bottomKpa / beamKpa);
        Expect(topKpa > 0 && bottomKpa < 0, "片持ち梁の上の縁が引かれ下の縁が押される");
        Expect(std::abs(topKpa / beamKpa - 1) <= 0.05 || height > 1, "1 セルの厚さで表面の応力が梁の式と ±5 %");
    }

    const char* FormName(sim::SupportForm form) {
        return form == sim::SupportForm::BeamLattice ? "格子の梁" : "成分ごと";
    }

    // --- 1・2・4. 梁の場面 ---
    struct BeamCase {
        const char* name;
        double lengthM;
        double heightM;
        uint64_t cellMm;
        bool bothEnds;
    };

    void TestBeam(const BeamCase& beam, sim::SupportForm form, sim::SupportSolve solve, uint32_t ticks) {
        const double cellM = (double)beam.cellMm / 1000.0;
        const auto width = (uint32_t)std::lround(beam.lengthM / cellM);
        const auto height = (uint32_t)std::lround(beam.heightM / cellM);
        sim::SupportLaw law(Beam(width, height, beam.cellMm, beam.bothEnds),
                            {.form = form, .solve = solve, .blocks = Blocks()});
        const uint32_t probe = beam.bothEnds ? width / 2 : width - 1;
        Settle settle = Run(law, probe, height, ticks);
        if (beam.bothEnds)
            settle.finalMm = (settle.finalMm + SagMm(law, probe - 1, height)) / 2;

        const double theory = beam.bothEnds ? ClampedMidMm(beam.lengthM, beam.heightM, cellM)
                                            : CantileverMm(beam.lengthM, beam.heightM, cellM, beam.lengthM - cellM / 2);
        const double ratio = settle.finalMm / theory;
        lastFinals.push_back(settle.finalMm);
        Log(Channel::Sim, Level::Info,
            "[{}] {} {} {} m: たわみ {:.4f} mm / 梁の式(Timoshenko){:.4f} mm = {:.3f}({})・着く刻み ±5 % {}・±1 % "
            "{}・{} "
            "刻み・1 刻みの節の解き {}",
            SolveName(solve), beam.name, FormName(form), cellM, settle.finalMm, theory, ratio,
            std::abs(ratio - 1) <= 0.05 ? "±5 % 合格" : "±5 % 不合格", settle.ticksTo5, settle.ticksTo1, ticks,
            law.LastNodeUpdates());
        Expect(settle.ledgerOk, std::string("帳簿: ") + beam.name);
        Expect(settle.finalMm > 0, std::string("重さで下がる: ") + beam.name);

        sim::SupportLaw again(Beam(width, height, beam.cellMm, beam.bothEnds),
                              {.form = form, .solve = solve, .blocks = Blocks()});
        for (uint32_t tick = 0; tick < ticks; ++tick)
            again.Step();

        Expect(again.Hash() == settle.hash, std::string("決定性: ") + beam.name);
    }

    // --- 4. 大きな岩の塊を掘った直後(16 × 8 m・0.5 m。下を地面に固定。真ん中の下 8 × 3 m を掘る)---
    void TestDig(sim::SupportSolve solve, uint32_t ticks, bool record) {
        constexpr uint32_t WIDTH = 32;
        constexpr uint32_t HEIGHT = 16;
        sim::SupportScene scene = Beam(WIDTH, HEIGHT, 500, false);
        for (uint32_t i = 0; i < WIDTH * HEIGHT; ++i)
            scene.clamps[i] = i < WIDTH ? sim::SUPPORT_CLAMP_BOTTOM : 0;

        sim::SupportLaw block(scene, {.solve = solve, .blocks = Blocks()});
        Run(block, WIDTH / 2, HEIGHT, ticks);
        for (uint32_t up = 4; up < 10; ++up) {
            for (uint32_t x = 8; x < 24; ++x)
                scene.solid[x + WIDTH * up] = 0;
        }

        sim::SupportLaw dug(scene, {.solve = solve, .blocks = Blocks()});
        dug.CarryFrom(block);
        const Settle settle = Run(dug, WIDTH / 2, HEIGHT, ticks);
        lastFinals.push_back(settle.finalMm);
        Log(Channel::Sim, Level::Info, "[{}] 掘った直後: 真ん中の列の平均のたわみ {:.4f} mm・着く刻み ±5 % {}・±1 % {}",
            SolveName(solve), settle.finalMm, settle.ticksTo5, settle.ticksTo1);
        Expect(settle.ledgerOk, "帳簿: 掘った場面");
        if (!record)
            return;

        sim::SupportLaw view(scene, {.solve = solve, .blocks = Blocks()});
        view.CarryFrom(block);
        for (uint32_t tick = 1; tick <= ticks; ++tick) {
            view.Step();
            if (tick != 1 && tick != 10 && tick != 60 && tick != 300 && tick != 1200 && tick != ticks)
                continue;

            std::string row;
            for (uint32_t x = 8; x < 24; ++x)
                row += std::format(" {:.3f}", -(double)view.Value(x, 10, sim::SUPPORT_UP) / NM_UNITS / 1.0e6);

            Log(Channel::Sim, Level::Info, "[{}] 掘った後 {} 刻み: 天井の下の面のたわみ(mm。左から 0.5 m ごと){}",
                SolveName(solve), tick, row);
        }
    }

    // --- 7. すぐ折れるはずの棚(T-0919): 1 セルの張り出しの根の面(壁から 0.5 m)の上の縁の引きの応力の道筋 ---
    // 割れの値を最後の応力の 1/2・9/10 と置き、初めて超える刻みを返す(割れの本体 R-B2 がまだ無いので、超える刻みで比べる)
    std::vector<double> ShelfStressPath(uint64_t lengthMm, sim::SupportSolve solve, uint32_t ticks) {
        const auto width = (uint32_t)(lengthMm / 500);
        sim::SupportLaw law(Beam(width, 1, 500, false), {.solve = solve, .blocks = Blocks()});
        std::vector<double> path(ticks);
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            law.Step();
            path[tick] = (double)law.FaceForces(0, 0, 0).highSideMn / 1000.0 / 0.25 / 1000.0;
        }

        return path;
    }

    uint32_t FirstOver(const std::vector<double>& path, double limit) {
        for (uint32_t tick = 0; tick < path.size(); ++tick) {
            if (path[tick] >= limit)
                return tick + 1;
        }

        return 0;
    }

    void TestShelfCrack(uint32_t laggedTicks, uint32_t coupledTicks) {
        for (const uint64_t lengthMm : {4000ull, 8000ull}) {
            const std::vector<double> lagged = ShelfStressPath(lengthMm, sim::SupportSolve::Lagged, laggedTicks);
            const std::vector<double> coupled = ShelfStressPath(lengthMm, sim::SupportSolve::Coupled, coupledTicks);
            // 割れの値の元は 1 つの系の最後の応力(遅らせる解き方は 8 m で決まった刻みの内に着かないことがある)
            const double finalKpa = coupled.back();
            for (const double share : {0.5, 0.9}) {
                const uint32_t before = FirstOver(lagged, share * finalKpa);
                const uint32_t after = FirstOver(coupled, share * finalKpa);
                Log(Channel::Sim, Level::Info,
                    "すぐ折れるはずの棚 {} m(1 セルの張り出し・壁から 0.5 m の面の上の縁): 割れの値 = 最後の応力 "
                    "{:.1f} kPa の "
                    "{:.0f} % を超える刻み: 遅らせる {}(0 = {} 刻みの内に超えない)→ 1 つの系 "
                    "{}・遅らせるの最後の応力の比 {:.4f}",
                    (double)lengthMm / 1000.0, finalKpa, share * 100, before, laggedTicks, after,
                    lagged.back() / finalKpa);
                Expect(after != 0 && (before == 0 || after <= before), "棚が遅れて折れない(1 つの系で早いか同じ)");
            }
        }
    }

    // --- 7. 段の斜めの棒の硬さの測り直し(T-0919。1 つの系で解けきった答え)---
    void TestBraceSweep(uint32_t ticks) {
        const ShapeCase cases[] = {
            {"アーチ 0.5 m", true, 500, 119.00, 0},
            {"アーチ 0.25 m", true, 250, 94.29, 0},
            {"洞窟 2 m", false, 2000, 169.94, 0},
            {"洞窟 0.5 m", false, 500, 179.43, 0},
        };
        for (const uint64_t denominator : {8ull, 12ull, 16ull, 24ull}) {
            std::string row;
            for (const ShapeCase& shape : cases) {
                const sim::SupportScene scene = shape.arch ? Arch(shape.cellMm) : Cave(shape.cellMm);
                sim::SupportLaw law(scene, {.stairBraces = true, .braceDenominator = denominator, .blocks = Blocks()});
                for (uint32_t tick = 0; tick < ticks; ++tick)
                    law.Step();

                const double ratio = ShapeSagMm(law, scene, shape.arch) / (shape.continuumRef * STONE_SAG_PER_UNIT_MM);
                row += std::format("・{} {:.3f}", shape.name, ratio);
            }

            Log(Channel::Sim, Level::Info, "段の斜めの棒の硬さ ka/{}(1 つの系・{} 刻み)/ 連続体{}", denominator, ticks,
                row);
        }
    }

    // 答えの比(同じ場面の Coupled ÷ Lagged の最後の値。同じ式なので解けきれば 1)
    void LogFinalRatios(const std::vector<double>& coupled, const std::vector<double>& lagged) {
        std::string row;
        double worst = 0;
        for (size_t i = 0; i < coupled.size() && i < lagged.size(); ++i) {
            const double ratio = coupled[i] / lagged[i];
            worst = std::max(worst, std::abs(ratio - 1));
            row += std::format(" {:.4f}", ratio);
        }

        Log(Channel::Sim, Level::Info, "答えの比(1 つの系 ÷ 遅らせる。場面の順){}・最大のずれ {:.4f}", row, worst);
        Expect(worst <= 0.01, "1 つの系と遅らせる解き方が同じ釣り合いに着く(±1 %)");
    }

    void RunSolve(sim::SupportSolve solve, uint32_t ticks, bool record) {
        const BeamCase cases[] = {
            {"1 セルの張り出し 4 m", 4.0, 0.5, 500, false},    {"張り出し 4 m(厚さ 0.5 m)", 4.0, 0.5, 250, false},
            {"片持ち梁 16 × 4 m", 16.0, 4.0, 500, false},      {"片持ち梁 16 × 4 m", 16.0, 4.0, 2000, false},
            {"片持ち梁 16 × 4 m", 16.0, 4.0, 250, false},      {"両端を固定した梁 8 × 1 m", 8.0, 1.0, 500, true},
            {"両端を固定した梁 8 × 1 m", 8.0, 1.0, 250, true},
        };
        lastFinals.clear();
        for (const BeamCase& beam : cases) {
            if (solve == sim::SupportSolve::Lagged)
                TestBeam(beam, sim::SupportForm::PerComponent, solve, 200);

            TestBeam(beam, sim::SupportForm::BeamLattice, solve, ticks);
        }

        TestDig(solve, ticks, record);
        TestShapes(solve, ticks, record);
        TestSurfaceStress(500, 1, solve, ticks);
        TestSurfaceStress(250, 2, solve, ticks);
    }

    int RunAll(std::span<char*> arguments) {
        uint32_t ticks = 2000;
        uint32_t coupledTicks = 300;
        bool record = false;
        for (size_t i = 1; i < arguments.size(); ++i) {
            const std::string_view name = arguments[i];
            if (name == "--record")
                record = true;
            else if (name == "--ticks" && i + 1 < arguments.size())
                ticks = (uint32_t)std::atoi(arguments[++i]);
            else if (name == "--cycles" && i + 1 < arguments.size())
                coupledCycles = (uint32_t)std::atoi(arguments[++i]);
            else if (name == "--coupled-ticks" && i + 1 < arguments.size())
                coupledTicks = (uint32_t)std::atoi(arguments[++i]);
        }

        RunSolve(sim::SupportSolve::Coupled, coupledTicks, record);
        const std::vector<double> coupled = lastFinals;
        RunSolve(sim::SupportSolve::Lagged, ticks, record);
        std::vector<double> lagged;
        for (size_t i = 0; i < lastFinals.size(); ++i) {
            // Lagged は PerComponent の行も持つ(梁の場面ごとに 1 つ前)。BeamLattice の行だけ取る
            if (i < 14 && i % 2 == 0)
                continue;

            lagged.push_back(lastFinals[i]);
        }

        LogFinalRatios(coupled, lagged);
        TestShelfCrack(ticks, coupledTicks);
        TestBraceSweep(coupledTicks);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "support_law_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "support_law_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = RunAll(std::span(argv, (size_t)argc));
    SingletonFinalizer::Finalize();

    return exitCode;
}
