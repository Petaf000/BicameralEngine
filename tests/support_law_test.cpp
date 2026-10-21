// support_law_test.cpp — 支えの法則の試作(sim/support_law。研究 R-B1・T-0871・22 §6)の CPU の測り。
// 場面(石: 硬さ 50 GPa・密度 2600 kg/m³。縦の面・奥行 1 セル):
//   1. 1 セルの厚さの張り出し 4 m(0.5 m と 0.25 m 〔厚さ 0.5 m = 2 セル〕)・片持ち梁 16 × 4 m(0.5 m と 2 m)・両端を固定した梁 8 × 1 m
//      → 先 / 真ん中のたわみを梁の式(Timoshenko)と比べる。2 つの形(成分ごと / 格子の梁)の両方
//   2. 帳簿: 毎刻み Σ セルの上向きの力 = 固定した面の力 − 重さ(解けきらなくても厳密)
//   3. 決定性: 2 回の実行でハッシュ一致
//   4. 釣り合いに着くまでの刻みの数(先のたわみが最後の値の ±5 %・±1 % に入る刻み)と、掘った直後の見た目(たわみの分布)の記録
// 合格の数値(22 §6: ±5 %)の判定は記録としてログに出し、試験が落ちるのは帳簿・決定性・符号だけ(研究の測り。T-0871 に結果)。
// 引数: [--ticks n](既定 2000)[--record](掘った場面の分布をログへ)
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

    void TestBeam(const BeamCase& beam, sim::SupportForm form, uint32_t ticks) {
        const double cellM = (double)beam.cellMm / 1000.0;
        const auto width = (uint32_t)std::lround(beam.lengthM / cellM);
        const auto height = (uint32_t)std::lround(beam.heightM / cellM);
        sim::SupportLaw law(Beam(width, height, beam.cellMm, beam.bothEnds), {.form = form});
        const uint32_t probe = beam.bothEnds ? width / 2 : width - 1;
        Settle settle = Run(law, probe, height, ticks);
        if (beam.bothEnds)
            settle.finalMm = (settle.finalMm + SagMm(law, probe - 1, height)) / 2;

        const double theory = beam.bothEnds ? ClampedMidMm(beam.lengthM, beam.heightM, cellM)
                                            : CantileverMm(beam.lengthM, beam.heightM, cellM, beam.lengthM - cellM / 2);
        const double ratio = settle.finalMm / theory;
        Log(Channel::Sim, Level::Info,
            "{} {} {} m: たわみ {:.4f} mm / 梁の式(Timoshenko){:.4f} mm = {:.3f}({})・着く刻み ±5 % {}・±1 % {}・{} "
            "刻み",
            beam.name, FormName(form), cellM, settle.finalMm, theory, ratio,
            std::abs(ratio - 1) <= 0.05 ? "±5 % 合格" : "±5 % 不合格", settle.ticksTo5, settle.ticksTo1, ticks);
        Expect(settle.ledgerOk, std::string("帳簿: ") + beam.name);
        Expect(settle.finalMm > 0, std::string("重さで下がる: ") + beam.name);

        sim::SupportLaw again(Beam(width, height, beam.cellMm, beam.bothEnds), {.form = form});
        for (uint32_t tick = 0; tick < ticks; ++tick)
            again.Step();

        Expect(again.Hash() == settle.hash, std::string("決定性: ") + beam.name);
    }

    // --- 4. 大きな岩の塊を掘った直後(16 × 8 m・0.5 m。下を地面に固定。真ん中の下 8 × 3 m を掘る)---
    void TestDig(uint32_t ticks, bool record) {
        constexpr uint32_t WIDTH = 32;
        constexpr uint32_t HEIGHT = 16;
        sim::SupportScene scene = Beam(WIDTH, HEIGHT, 500, false);
        for (uint32_t i = 0; i < WIDTH * HEIGHT; ++i)
            scene.clamps[i] = i < WIDTH ? sim::SUPPORT_CLAMP_BOTTOM : 0;

        sim::SupportLaw block(scene, {});
        Run(block, WIDTH / 2, HEIGHT, ticks);
        for (uint32_t up = 4; up < 10; ++up) {
            for (uint32_t x = 8; x < 24; ++x)
                scene.solid[x + WIDTH * up] = 0;
        }

        sim::SupportLaw dug(scene, {});
        dug.CarryFrom(block);
        const Settle settle = Run(dug, WIDTH / 2, HEIGHT, ticks);
        Log(Channel::Sim, Level::Info, "掘った直後: 真ん中の列の平均のたわみ {:.4f} mm・着く刻み ±5 % {}・±1 % {}",
            settle.finalMm, settle.ticksTo5, settle.ticksTo1);
        Expect(settle.ledgerOk, "帳簿: 掘った場面");
        if (!record)
            return;

        sim::SupportLaw view(scene, {});
        view.CarryFrom(block);
        for (uint32_t tick = 1; tick <= ticks; ++tick) {
            view.Step();
            if (tick != 1 && tick != 10 && tick != 60 && tick != 300 && tick != 1200 && tick != ticks)
                continue;

            std::string row;
            for (uint32_t x = 8; x < 24; ++x)
                row += std::format(" {:.3f}", -(double)view.Value(x, 10, sim::SUPPORT_UP) / NM_UNITS / 1.0e6);

            Log(Channel::Sim, Level::Info, "掘った後 {} 刻み: 天井の下の面のたわみ(mm。左から 0.5 m ごと){}", tick,
                row);
        }
    }

    int RunAll(std::span<char*> arguments) {
        uint32_t ticks = 2000;
        bool record = false;
        for (size_t i = 1; i < arguments.size(); ++i) {
            const std::string_view name = arguments[i];
            if (name == "--record")
                record = true;
            else if (name == "--ticks" && i + 1 < arguments.size())
                ticks = (uint32_t)std::atoi(arguments[++i]);
        }

        const BeamCase cases[] = {
            {"1 セルの張り出し 4 m", 4.0, 0.5, 500, false},    {"張り出し 4 m(厚さ 0.5 m)", 4.0, 0.5, 250, false},
            {"片持ち梁 16 × 4 m", 16.0, 4.0, 500, false},      {"片持ち梁 16 × 4 m", 16.0, 4.0, 2000, false},
            {"片持ち梁 16 × 4 m", 16.0, 4.0, 250, false},      {"両端を固定した梁 8 × 1 m", 8.0, 1.0, 500, true},
            {"両端を固定した梁 8 × 1 m", 8.0, 1.0, 250, true},
        };
        for (const BeamCase& beam : cases) {
            TestBeam(beam, sim::SupportForm::PerComponent, 200);
            TestBeam(beam, sim::SupportForm::BeamLattice, ticks);
        }

        TestDig(ticks, record);
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
