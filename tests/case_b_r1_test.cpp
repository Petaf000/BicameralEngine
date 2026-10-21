// case_b_r1_test.cpp — 案 B の研究 R1(T-0868・T-0926・T-0927。docs/design/21 §5.2・ADR-0409): 硬い液体を気体と同じ陰の圧力で(case_b_flow)。CPU だけ。
// T-0926 の後: 連通管・閉じた空気は 60 秒走らせ、40〜60 秒の揺れの中心で見る(20 秒の 1 点は前の合格として並べる)。主の組は (1) 本物の水・V 4 + 物の粘り。
// 1 本目 = 本物の水(音の速さ 1483 m/s)・V サイクル 2 回。2 本目 = 液体の段 (2) 水を柔らかく(100 m/s・30 m/s)と (1) 回数を増やす(4 回)を
// 同じ試験の中で並べて測る。場面: 静かな池・連通管・閉じた空気の下の水(潜水鐘)・ダムの決壊・サイフォン(頂上 4 m・9 m・11 m)・弱い差の戻り(0.5 m と 2 m)。
// 合否の目安は 21 §5.2(仮)。この試験は**研究の記録**なので、落ちて止まるのは保存・決定性・式の壊れ(負の質量など)だけで、
// 目安の合否は「合格 / 不合格」と数値を表に出す(結果は docs/tickets/T-0868)。表示の小数は FormatFixed(整数)。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "sim/case_b/case_b_box.h"
#include "sim/case_b/case_b_flow.h"
#include "sim/case_b/case_b_gauge.h"
#include "sim/case_b/case_b_scene.h"

namespace {

    using namespace bicameral::sim::case_b;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr int64_t MICRO = 1000000;
    constexpr int64_t TOLERANCE_NL = 256;  // 陰の圧力の口の止める誤差(V サイクルの上限が先に来ることが多い)

    // 試す組(液体の硬さ × V サイクルの上限)
    // 粘り(T-0927): 物の粘り(viscosity_upa_s)と格子より小さい渦の粘り(T-0926 の仮説 2。C_s² × 10000)
    struct Trial {
        const char* name;
        uint32_t soundMmPerS;
        uint32_t cycles;
        bool materialViscosity;
        uint32_t eddyPer10000;
        MomentumLayout layout = MomentumLayout::GrainSeparate;  // 運動量の置き場(T-0963 で ① を並べる)
        bool splitSurfaceLayer = false;                         // ① で水面の層の横の面を分けるか
    };

    // 主は (1) 本物の水・V 4 + 物の粘り(T-0926・T-0927 の後の R1。司令塔のおすすめ)
    constexpr Trial TRIALS[] = {
        {"(1) 本物の水・V 4 + 物の粘り(主)", WATER_SOUND_SPEED_MM_PER_S, 4, true, 0},
        {"1 本目 本物の水・V 2(粘り無し。T-0868 の表の 1 本目)", WATER_SOUND_SPEED_MM_PER_S, 2, false, 0},
        {"(1) 本物の水・V 8 + 物の粘り(真空のセルが解き残りから出るかを見る。T-0926)", WATER_SOUND_SPEED_MM_PER_S, 8,
         true, 0},
        {"(1) 本物の水・V 4 + 物の粘り + 渦の粘り C_s 0.17(T-0926 仮説 2・記録)", WATER_SOUND_SPEED_MM_PER_S, 4, true,
         289},
        {"(2) 柔らかい水 100 m/s・V 2 + 物の粘り", 100000, 2, true, 0},
        {"(1) 本物の水・V 4 + 物の粘り・置き場 ①(T-0963 の既定。しずく・霧を別の速さで)", WATER_SOUND_SPEED_MM_PER_S, 4,
         true, 0, MomentumLayout::PerPhase},
        {"(1) 本物の水・V 4 + 物の粘り・置き場 ① + 水面の層も分ける(T-0963 の 2 本目の 1 回目・落ちた記録)",
         WATER_SOUND_SPEED_MM_PER_S, 4, true, 0, MomentumLayout::PerPhase, true},
    };

    struct Run {
        Recording recording;
        FlowStats stats;
        Box box;
    };

    Run RunTrial(const Scene& scene, int32_t level, const Trial& trial, uint64_t ticks,
                 const std::vector<std::string_view>& regionNames) {
        auto created = BuildScene(scene, level, trial.layout);
        EXPECT(created.has_value());
        Run run{.recording = {}, .stats = {}, .box = std::move(*created)};
        std::vector<BoxRegion> regions;
        for (const std::string_view name : regionNames)
            regions.push_back(*FindRegion(scene, name, level));

        const FlowStages flow = MakeFlowStages({.liquidSoundSpeedMmPerS = trial.soundMmPerS,
                                                .materialViscosity = trial.materialViscosity,
                                                .eddyCoefficientPer10000 = trial.eddyPer10000,
                                                .splitSurfaceLayer = trial.splitSurfaceLayer});
        run.recording = RunScene(run.box, flow.stages, MakeImplicitGridSolver(trial.cycles, TOLERANCE_NL), ticks,
                                 regions);
        run.stats = *flow.stats;
        EXPECT(run.recording.AllConserved());

        return run;
    }

    std::string Meters(int64_t micrometers) {
        return FormatFixed(micrometers / 1000, 3);
    }

    std::string Pascals(int64_t microPascals) {
        return FormatFixed(microPascals / 1000, 3);
    }

    const char* Verdict(bool passed) {
        return passed ? "合格" : "不合格";
    }

    void PrintCost(const Run& run) {
        std::printf(
            "    費用 %lld ns/セル/刻み(計器込み)・V サイクル 最後 %u 最大 %u・式の誤差 最大 %lld nL・出る量を縮めた "
            "%llu・"
            "真空のセル %llu・軽い面 %llu・粘りの組 %llu\n",
            (long long)run.recording.NanosecondsPerCellTick(), run.stats.lastCycles, run.stats.maxCycles,
            (long long)run.stats.maxErrorNl, (unsigned long long)run.stats.limitedCells,
            (unsigned long long)run.stats.voidCells, (unsigned long long)run.stats.lightFaces,
            (unsigned long long)run.stats.viscousPairs);
    }

    // 列の窓(from 以降)の最小・最大・平均。揺れているのか、ずれて止まっているのかを分ける(T-0926)
    struct Window {
        int64_t low = 0;
        int64_t high = 0;
        int64_t mean = 0;
    };

    Window WindowOf(const std::vector<int64_t>& series, size_t from) {
        Window window{.low = INT64_MAX, .high = INT64_MIN, .mean = 0};
        int64_t total = 0;
        for (size_t tick = from; tick < series.size(); ++tick) {
            window.low = std::min(window.low, series[tick]);
            window.high = std::max(window.high, series[tick]);
            total += series[tick];
        }

        window.mean = series.size() > from ? total / (int64_t)(series.size() - from) : 0;

        return window;
    }

    // --- 静かな池(#13・#27・#90): 水面のセルの圧力の揺れ < 大気の 0.1 %・ほぼ止まる・水位が動かない ---
    void StillPond(const Trial& trial) {
        const Scene scene = MakeStillPondScene();
        for (const int32_t level : {0, -2}) {
            const Run run = RunTrial(scene, level, trial, 600, {"pond"});
            const auto& samples = run.recording.samples;
            // 1 秒目から後の、水面のセルの圧力の最小・最大(時間と場所の両方)
            int64_t low = INT64_MAX;
            int64_t high = INT64_MIN;
            for (size_t tick = 60; tick < samples.size(); ++tick) {
                low = std::min(low, samples[tick].regions[0].surfaceMinPressureMicroPascal);
                high = std::max(high, samples[tick].regions[0].surfaceMaxPressureMicroPascal);
            }

            const int64_t swing = high - low;
            const int64_t levelStart = samples.front().regions[0].liquidLevelUm;
            const int64_t levelEnd = samples.back().regions[0].liquidLevelUm;
            const GaugeSample& last = samples.back();
            const bool calm = swing * 1000 < ATMOSPHERE_PA * MICRO;
            const bool still = last.maxSpeed < MOVING_SPEED && last.movingFaces * 100 < last.totalFaces;
            std::printf(
                "  静かな池 %s m: 水面の圧力の幅 %s Pa(%s)・水位 %s → %s m・10 秒後 最大の速さ %lld µm/s・動く面 "
                "%u/%u(%s)\n",
                level == 0 ? "0.5" : "2", Pascals(swing).c_str(), Verdict(calm), Meters(levelStart).c_str(),
                Meters(levelEnd).c_str(), (long long)((last.maxSpeed * MICRO) >> 20), last.movingFaces, last.totalFaces,
                Verdict(still));
            PrintCost(run);
        }
    }

    // 揺れの中心と幅(T-0926): 窓 [from, to) の平均(窓はほぼ揺れの周期の整数倍に取る)と、窓の中の最大 − 最小の半分
    struct Swing {
        int64_t center = 0;
        int64_t amplitude = 0;
    };

    Swing SwingOf(const std::vector<int64_t>& series, size_t from, size_t to) {
        const Window window = WindowOf(std::vector<int64_t>(series.begin(), series.begin() + (ptrdiff_t)to), from);
        return {.center = window.mean, .amplitude = (window.high - window.low) / 2};
    }

    // 10 秒ごとの揺れの幅(減り方。本物の水の形の損失の見積もりと比べる)
    std::string Envelope(const std::vector<int64_t>& series, uint32_t ticksPerSecond) {
        std::string text;
        for (size_t begin = 0; begin + (10 * ticksPerSecond) < series.size(); begin += 10 * ticksPerSecond) {
            const Swing swing = SwingOf(series, begin, begin + (10 * ticksPerSecond));
            text += "±" + FormatFixed(swing.amplitude / 1000, 3) + " ";
        }

        return text;
    }

    // --- 連通管(#13・#14): 揺れの中心が 2.5 m(±2 %)・揺れが減る(T-0926 で合格を取り直した。チケット T-0926)---
    // 本物の水でも、この形(幅 5 m の池・高さ 1 m の管)は 20 秒では止まらない(形の損失 K ≈ 1〜3 で ±0.1〜0.2 m。T-0926)。
    // だから 20 秒の 1 点ではなく、40〜60 秒(周期 約 6.5 秒の約 3 回)の平均を揺れの中心として見る。
    void ConnectedVessels(const Trial& trial) {
        const Scene scene = MakeConnectedVesselsScene();
        for (const int32_t level : {0, 1}) {
            const Run run = RunTrial(scene, level, trial, 3600, {"left", "right"});
            const auto left = RegionSeries(run.recording, 0, &RegionSample::liquidLevelUm);
            const auto right = RegionSeries(run.recording, 1, &RegionSample::liquidLevelUm);
            constexpr int64_t TARGET = 2500000;
            const Swing swing = SwingOf(right, 2400, 3601);
            const bool level20 = DifferencePermille(left[1200], TARGET) <= 20 &&
                                 DifferencePermille(right[1200], TARGET) <= 20;
            const bool centered = DifferencePermille(swing.center, TARGET) <= 20;
            std::printf(
                "  連通管 %s m: 20 秒の 1 点 左 %s・右 %s m(前の合格 %s)・右の揺れの中心(40〜60 秒)%s m(%s・差 %lld "
                "‰)・60 秒 %s m\n",
                level == 0 ? "0.5" : "0.25", Meters(left[1200]).c_str(), Meters(right[1200]).c_str(), Verdict(level20),
                Meters(swing.center).c_str(), Verdict(centered), (long long)DifferencePermille(swing.center, TARGET),
                Meters(right.back()).c_str());
            std::printf("    右の揺れの幅 10 秒ごと %s\n", Envelope(right, 60).c_str());
            PrintCost(run);
        }
    }

    // --- 閉じた空気の下の水(#9): 鐘の中の水位(揺れの中心)が釣り合いの計算と ±2 % ---
    // 釣り合い(等温・外の閉じた空気 144 セルも膨らむ): 鐘の空気 36 セル・口は水面の 4 段下・外の水面 16 列。
    // P0 × 6 ÷ (6 − d) = P0 × 144 ÷ (144 + 6d) + ρ g × 0.5 m × (4 − d − 6d/16) を解いて d = 0.6578 段 = 0.3289 m(T-0868 の計算)
    void TrappedAir(const Trial& trial) {
        const Scene scene = MakeTrappedAirScene();
        const Run run = RunTrial(scene, 0, trial, 3600, {"bell", "outside"});
        const auto bell = RegionSeries(run.recording, 0, &RegionSample::liquidLevelUm);
        constexpr int64_t EXPECTED = 328882;
        const Swing swing = SwingOf(bell, 2400, 3601);
        const bool point = DifferencePermille(bell[900], EXPECTED) <= 20;
        const bool centered = DifferencePermille(swing.center, EXPECTED) <= 20;
        std::printf(
            "  閉じた空気の下の水: 15 秒の 1 点 %s m(前の合格 %s)・揺れの中心(40〜60 秒)%s m(計算 %s。%s・差 %lld "
            "‰)\n",
            Meters(bell[900]).c_str(), Verdict(point), Meters(swing.center).c_str(), Meters(EXPECTED).c_str(),
            Verdict(centered), (long long)DifferencePermille(swing.center, EXPECTED));
        std::printf("    鐘の揺れの幅 10 秒ごと %s\n", Envelope(bell, 60).c_str());
        PrintCost(run);
    }

    // --- ダムの決壊(#16): 向こうの壁に届く時・壁際の最高(T-0927 で基準を取り直した)---
    // 前の基準 2.13 秒・1.82 セルは液体の模型 T-0610 の面の摩擦 1/128 の値。司令塔の一旦の決定(T-0927): 基準は物の値から出る値。
    // 水の粘り 890 µPa·s では Re ≈ 10^7 で粘りは効かないので、物の値から出る値 = 粘りの無い水の解。
    // 浅い水の理論(Ritter の形・壁で跳ね返る。有限体積で 0.005 m まで細かくして収束: 届く 1.31 秒・壁の列の最高 1.60 m = 3.2 セル。T-0927)。
    // 届く時は ±5 %。最高は浅い水の理論が壁を駆け上がる水の舌を持たないので**記録だけ**(0.5 m と 0.25 m の差を見る)。
    void DamBreak(const Trial& trial) {
        const Scene scene = MakeDamBreakScene();
        std::vector<int64_t> peaks;
        for (const int32_t level : {0, 1}) {
            const Run run = RunTrial(scene, level, trial, 480, {"wall_column", "reservoir"});
            const auto wall = RegionSeries(run.recording, 0, &RegionSample::liquidLevelUm);
            // 届いた = 壁の列(0.5 m)の水の深さが 0.12 m(元の深さの 6 %)を超えた(probe_liquid_test と同じ)
            const auto arrival = FirstTickAtLeast(wall, 120000);
            const Peak peak = PeakOf(wall);
            peaks.push_back(peak.value);
            const int64_t arrivalMs = arrival.has_value() ? (int64_t)*arrival * 1000 / 60 : -1;
            const bool arrivalOk = arrival.has_value() && DifferencePermille(arrivalMs, 1310) <= 50;
            const bool oldArrival = arrival.has_value() && DifferencePermille(arrivalMs, 2130) <= 50;
            std::printf(
                "  ダムの決壊 %s m: 壁に届く %s 秒(基準 1.31。%s・前の基準 2.13 は %s)・壁際の最高 %s セル(%s "
                "秒。浅い水の理論 "
                "3.2・記録)・8 秒後の最大の速さ %lld mm/s\n",
                level == 0 ? "0.5" : "0.25", arrival.has_value() ? FormatFixed(arrivalMs, 3).c_str() : "届かない",
                Verdict(arrivalOk), Verdict(oldArrival), FormatFixed(peak.value / 500, 3).c_str(),
                FormatFixed((int64_t)peak.tick * 1000 / 60, 3).c_str(),
                (long long)((run.recording.samples.back().maxSpeed * 1000) >> 20));
            PrintCost(run);
        }

        std::printf("  ダムの決壊 壁際の最高 0.5 m と 0.25 m の差 %lld ‰(記録)\n",
                    (long long)DifferencePermille(peaks[0], peaks[1]));
    }

    // --- サイフォン(#18): 頂上 4 m は流れ続け、約 10 m を超えると切れる ---
    void Siphon(const Trial& trial) {
        for (const uint32_t crest : {16u, 26u, 30u}) {
            const Scene scene = MakeSiphonScene(crest);
            const Run run = RunTrial(scene, 0, trial, 600, {"source", "outlet", "crest"});
            const auto source = RegionSeries(run.recording, 0, &RegionSample::liquidLevelUm);
            const auto outlet = RegionSeries(run.recording, 1, &RegionSample::liquidLevelUm);
            const auto crestWater = RegionSeries(run.recording, 2, &RegionSample::liquidLevelUm);
            // 5 秒目から 10 秒目に池の水位が下がった量(流れ続けているか)
            const int64_t lateDrop = source[300] - source.back();
            std::printf(
                "  サイフォン 頂上 %u m: 池の水位 %s → %s m(5〜10 秒で %s m 下がる)・出口 %s m・頂上の管の水 %s → %s "
                "m\n",
                (crest - 8) / 2, Meters(source.front()).c_str(), Meters(source.back()).c_str(),
                Meters(lateDrop).c_str(), Meters(outlet.back()).c_str(), Meters(crestWater.front()).c_str(),
                Meters(crestWater.back()).c_str());
            PrintCost(run);
        }
    }

    // --- 弱い差の戻り(B-c): 閉じた空気の左半分 +2 kPa。差が 10 % に入る時間が 0.5 m と 2 m で ±10 %・揺れずに戻る ---
    void PressureRelax(const Trial& trial) {
        const Scene scene = MakePressureRelaxScene();
        std::vector<int64_t> settleTicks;
        for (const int32_t level : {0, -2}) {
            const Run run = RunTrial(scene, level, trial, 240, {"high", "low"});
            const auto high = RegionSeries(run.recording, 0, &RegionSample::meanPressureMicroPascal);
            const auto low = RegionSeries(run.recording, 1, &RegionSample::meanPressureMicroPascal);
            std::vector<int64_t> difference;
            for (size_t tick = 1; tick < high.size(); ++tick)
                difference.push_back(high[tick] - low[tick]);

            // 初めの差 2 kPa の 10 %(200 Pa)に入って出ない刻み(1 刻み目から数える)
            const auto settled = SettleTick(difference, 0, 200 * MICRO);
            settleTicks.push_back(settled.has_value() ? (int64_t)*settled + 1 : -1);
            std::printf("  弱い差の戻り %s m: 1 刻み目の差 %s Pa・10 %% に入る %s 刻み・揺り戻し %u 回(50 Pa 以上)\n",
                        level == 0 ? "0.5" : "2", Pascals(difference.front()).c_str(),
                        settled.has_value() ? std::to_string(*settled + 1).c_str() : "入らない",
                        CountReversals(difference, 50 * MICRO));
            PrintCost(run);
        }

        const bool same = settleTicks[0] > 0 && settleTicks[1] > 0 &&
                          DifferencePermille(settleTicks[0], settleTicks[1]) <= 100;
        std::printf("  弱い差の戻る時間 0.5 m と 2 m: %lld と %lld 刻み(%s)\n", (long long)settleTicks[0],
                    (long long)settleTicks[1], Verdict(same));
    }

    // 決定性: 同じ場面を 2 回走らせてハッシュの列が一致
    void Determinism() {
        const Trial& trial = TRIALS[0];
        const Run first = RunTrial(MakeDamBreakScene(), 0, trial, 120, {"wall_column"});
        const Run second = RunTrial(MakeDamBreakScene(), 0, trial, 120, {"wall_column"});
        EXPECT(first.recording.HashChain() == second.recording.HashChain());
        EXPECT(first.recording.samples.back().hash != first.recording.samples.front().hash);
        std::printf("決定性: ダムの決壊 120 刻みを 2 回 → ハッシュ %016llx(%s)\n",
                    (unsigned long long)first.recording.HashChain(),
                    first.recording.HashChain() == second.recording.HashChain() ? "一致" : "違う");
    }

}  // namespace

int main(int argc, char** argv) {
    // 引数で組を 1 つに絞れる(0〜6。5 = 置き場 ①・6 = ① + 水面の層も分ける)。無ければ全部
    const int only = argc > 1 ? std::stoi(argv[1]) : -1;
    Determinism();
    for (int index = 0; index < (int)std::size(TRIALS); ++index) {
        if (only >= 0 && index != only)
            continue;

        const Trial& trial = TRIALS[index];
        std::printf("=== %s ===\n", trial.name);
        StillPond(trial);
        ConnectedVessels(trial);
        TrappedAir(trial);
        DamBreak(trial);
        Siphon(trial);
        PressureRelax(trial);
    }

    std::printf("case_b r1: %d failure(s)\n", failureCount);

    return failureCount == 0 ? 0 : 1;
}
