// gpu_time_step_choose_test.cpp — 時間を飛ばす試作(sim/time_step_tree)の刻みを選ぶ段(D-669 の窓・隣の差 1 段)を GPU で回し、
// CPU リファレンス(WindowCaps・ChooseExponents)とビット一致で確かめ、1 出来事の GPU の時間を測る(T-1112)。
// 入力は time_step_tree_test の choose-dump が火事の出来事から書いたファイル(tests/time_step_choose_file.h): 木の形の表と、
//   出来事ごとの選ぶ前の詰めた歩の記録(2 B/単位)・CPU の窓の上限・選んだ e・出来事の後の詰めた記録。
// 1. ビット一致: 出来事ごとに GPU の窓の上限・選んだ e・後の詰めた記録が CPU と同じか。掃く回が不動点に着いたか(区画の中の回の
//    上限・仕上げの区画の数の上限の内)。
// 2. 費用(--measure): 出来事ごとに 1 本のリストへ続けて積んだ時間(GPU の時計が高いまま = 早送りの形)の平均・最大と、
//    1 出来事を 4 回続けて積んだ最小。--events-total n で火事全体(n 出来事)の待ちへ伸ばす。
// 3. 内訳(--breakdown。T-1213): 段のまとまりの間に時刻印を打った 1 出来事の固定費の内訳(GpuTimeTreeChoose::PhaseNames)。
// 4. --expect-crossing(T-1287): 区画が 2 つ以上で、区画をまたいで縮みが伝わった出来事があることも求める(区画をまたぐ道の確かめ)。
// 引数: --choices <ファイル> [--measure] [--breakdown] [--events-total n] [--expect-crossing] と GPU の試験の共通の引数(--warp・--queue・--no-gbv)。
#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "sim/gpu_time_tree_choose.h"
#include "time_step_choose_file.h"

using namespace bicameral;

namespace {

    constexpr uint32_t REPEATS = 4;

    struct Context {
        ID3D12Device5* device = nullptr;
        gpu::ImmediateQueue* queue = nullptr;
        gpu::DebugRing* ring = nullptr;
        uint64_t frequency = 0;
    };

    std::expected<void, std::string> Execute(Context& context, ID3D12GraphicsCommandList* list) {
        context.ring->RecordReadbackAndReset(list);
        if (!context.queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debug = context.ring->Drain();
        if (debug.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debug.assertCount));

        return {};
    }

    double ToMs(const Context& context, uint64_t begin, uint64_t end) {
        return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(context.frequency);
    }

    // 最初に違う単位(無ければ空)
    std::string FirstDifference(const test::TimeStepChooseEvent& cpu, const sim::GpuTimeTreeChoice& gpu) {
        for (size_t u = 0; u < cpu.before.size(); ++u) {
            if (cpu.windowCaps[u] != gpu.windowCaps[u] || cpu.next[u] != gpu.next[u] || cpu.after[u] != gpu.after[u])
                return std::format(
                    "単位 {}: 窓の上限 CPU {} / GPU {}・e CPU {} / GPU {}・後の記録 CPU {:04x} / GPU {:04x}"
                    "(前 {:04x})",
                    u, cpu.windowCaps[u], gpu.windowCaps[u], cpu.next[u], gpu.next[u], cpu.after[u], gpu.after[u],
                    cpu.before[u]);
        }

        return {};
    }

    std::expected<void, std::string> Compare(Context& context, const sim::GpuTimeTreeChoose& gpu,
                                             const test::TimeStepChooseFile& file, bool expectCrossing) {
        uint32_t widestSweeps = 0;
        uint32_t widestInner = 0;
        uint32_t widestFixup = 0;
        uint64_t started = 0;
        uint64_t truncated = 0;
        for (uint32_t event = 0; event < file.events.size(); ++event) {
            const test::TimeStepChooseEvent& cpu = file.events[event];
            ID3D12GraphicsCommandList10* list = context.queue->Begin();
            if (list == nullptr)
                return std::unexpected("コマンドリストを始められない");

            context.ring->RecordBegin(list);
            if (event == 0)
                gpu.RecordUpload(list);

            gpu.RecordChoose(list, context.ring->GpuAddress(), event, cpu.cap);
            gpu.RecordReadback(list);
            const auto executed = Execute(context, list);
            if (!executed)
                return executed;

            sim::GpuTimeTreeChoice choice;
            if (!gpu.Read(choice))
                return std::unexpected("読み戻せない");

            if (!choice.settled)
                return std::unexpected(
                    std::format("出来事 {}(刻み {}): 掃く回が {} 回(区画の中 {} 回)で不動点に着かない", event, cpu.tick,
                                sim::GpuTimeTreeChoose::PASSES, choice.innerSweeps));

            const std::string difference = FirstDifference(cpu, choice);
            if (!difference.empty())
                return std::unexpected(
                    std::format("出来事 {}(刻み {}・cap {}): {}", event, cpu.tick, cpu.cap, difference));

            widestSweeps = std::max(widestSweeps, choice.sweeps);
            widestInner = std::max(widestInner, choice.innerSweeps);
            widestFixup = std::max(widestFixup, choice.fixupTiles);
            for (size_t u = 0; u < cpu.next.size(); ++u) {
                started += cpu.next[u] >= 0 ? 1 : 0;
                truncated += cpu.next[u] >= 0 && (cpu.before[u] & sim::TIME_TREE_RECORD_OPEN) != 0 ? 1 : 0;
            }
        }

        Log(Channel::Gpu, Level::Info,
            "刻みを選ぶ段: {} 出来事・単位 {} で CPU とビット一致(窓の上限・選んだ e・後の詰めた記録)。始めた単位 {}・"
            "打ち切り {}・区画 {}・変わった掃く回の最大 {}・区画の中の回の最大 {}・仕上げの区画の最大 {}",
            file.events.size(), file.tables.units, started, truncated,
            (file.tables.units + sim::GpuTimeTreeChoose::TILE_UNITS - 1) / sim::GpuTimeTreeChoose::TILE_UNITS,
            widestSweeps, widestInner, widestFixup);

        // --expect-crossing(T-1287): 区画が 2 つ以上で、区画をまたいで縮みが伝わった出来事がある(1 回目の後の掃く回か仕上げで
        // 区画が変わった = 隣の区画の結果を読んで変わった)。無ければ区画をまたぐ道を確かめていない
        const uint32_t tiles = (file.tables.units + sim::GpuTimeTreeChoose::TILE_UNITS - 1) /
                               sim::GpuTimeTreeChoose::TILE_UNITS;
        if (expectCrossing && (tiles < 2 || (widestSweeps < 2 && widestFixup == 0)))
            return std::unexpected(
                std::format("区画をまたぐ道を通っていない(区画 {}・変わった掃く回の最大 {}・仕上げの区画の最大 {})",
                            tiles, widestSweeps, widestFixup));

        return {};
    }

    // 出来事を続けて積んで時刻印を挟む(first から count 出来事・repeats 回ずつ)
    std::expected<std::vector<double>, std::string> TimeEvents(Context& context, const sim::GpuTimeTreeChoose& gpu,
                                                               const test::TimeStepChooseFile& file, uint32_t first,
                                                               uint32_t count, uint32_t repeats) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        context.ring->RecordBegin(list);
        uint32_t stamp = 0;
        gpu.RecordTimestamp(list, stamp++);
        for (uint32_t i = 0; i < count; ++i) {
            for (uint32_t r = 0; r < repeats; ++r) {
                gpu.RecordChoose(list, context.ring->GpuAddress(), first + i, file.events[first + i].cap);
                gpu.RecordTimestamp(list, stamp++);
            }
        }

        gpu.RecordTimestampReadback(list, stamp);
        const auto executed = Execute(context, list);
        if (!executed)
            return std::unexpected(executed.error());

        const std::vector<uint64_t> ticks = gpu.ReadTimestamps(stamp);
        if (ticks.size() != stamp)
            return std::unexpected("時刻印を読めない");

        std::vector<double> ms;
        for (uint32_t i = 0; i + 1 < stamp; ++i)
            ms.push_back(ToMs(context, ticks[i], ticks[i + 1]));

        return ms;
    }

    std::expected<void, std::string> Measure(Context& context, const sim::GpuTimeTreeChoose& gpu,
                                             const test::TimeStepChooseFile& file, uint64_t eventsTotal) {
        const auto events = static_cast<uint32_t>(file.events.size());
        const uint32_t perList = (sim::GpuTimeTreeChoose::MAX_TIMESTAMPS - 1) / REPEATS;
        (void)TimeEvents(context, gpu, file, 0, std::min(events, perList), 1);  // 暖機

        // --- 続けて積む(1 回ずつ): 平均・最大 ---
        std::vector<double> continuous;
        for (uint32_t first = 0; first < events; first += perList) {
            const auto ms = TimeEvents(context, gpu, file, first, std::min(perList, events - first), 1);
            if (!ms)
                return std::unexpected(ms.error());

            continuous.insert(continuous.end(), ms->begin(), ms->end());
        }

        // --- 1 出来事を REPEATS 回続けて積んだ最小(同じ出来事の揺れを除く)---
        std::vector<double> best;
        for (uint32_t first = 0; first < events; first += perList) {
            const auto ms = TimeEvents(context, gpu, file, first, std::min(perList, events - first), REPEATS);
            if (!ms)
                return std::unexpected(ms.error());

            for (size_t i = 0; i < ms->size(); i += REPEATS)
                best.push_back(*std::min_element(ms->begin() + static_cast<std::ptrdiff_t>(i),
                                                 ms->begin() + static_cast<std::ptrdiff_t>(i + REPEATS)));
        }

        double sum = 0.0;
        for (const double ms : continuous)
            sum += ms;

        double bestSum = 0.0;
        for (const double ms : best)
            bestSum += ms;

        const double mean = sum / static_cast<double>(continuous.size());
        const double bestMean = bestSum / static_cast<double>(best.size());
        Log(Channel::Gpu, Level::Info,
            "刻みを選ぶ段の時間(単位 {}・窓の格子 {}×{}×{}・半径 {}・VRAM {:.2f} MB・{} 出来事): 続けて積んだ 1 出来事 "
            "平均 "
            "{:.4f} ms・最大 {:.4f} ms・最小 {:.4f} ms / 同じ出来事 {} 回の最小の平均 {:.4f} ms",
            file.tables.units, file.tables.gridSize[0], file.tables.gridSize[1], file.tables.gridSize[2],
            file.tables.radius, static_cast<double>(gpu.DeviceBytes()) / (1024.0 * 1024.0), events, mean,
            *std::ranges::max_element(continuous), *std::ranges::min_element(continuous), REPEATS, bestMean);
        if (eventsTotal != 0)
            Log(Channel::Gpu, Level::Info, "刻みを選ぶ段の待ち: {} 出来事で {:.2f} 秒(平均)/ {:.2f} 秒(最小の平均)",
                eventsTotal, mean * static_cast<double>(eventsTotal) / 1000.0,
                bestMean * static_cast<double>(eventsTotal) / 1000.0);

        return {};
    }

    // 固定費の内訳(--breakdown。T-1213): 出来事ごとに段のまとまりの間へ時刻印を打ち、まとまりごとの平均を出す
    std::expected<void, std::string> Breakdown(Context& context, const sim::GpuTimeTreeChoose& gpu,
                                               const test::TimeStepChooseFile& file) {
        const std::span<const char* const> names = sim::GpuTimeTreeChoose::PhaseNames();
        const auto phases = static_cast<uint32_t>(names.size());
        const uint32_t perList = sim::GpuTimeTreeChoose::MAX_TIMESTAMPS / (phases + 1);
        const auto events = static_cast<uint32_t>(file.events.size());
        std::vector<double> sums(phases, 0.0);
        std::vector<double> peaks(phases, 0.0);
        for (uint32_t pass = 0; pass < 2; ++pass) {  // 1 回目は暖機
            std::ranges::fill(sums, 0.0);
            for (uint32_t first = 0; first < events; first += perList) {
                const uint32_t count = std::min(perList, events - first);
                ID3D12GraphicsCommandList10* list = context.queue->Begin();
                if (list == nullptr)
                    return std::unexpected("コマンドリストを始められない");

                context.ring->RecordBegin(list);
                uint32_t stamp = 0;
                for (uint32_t i = 0; i < count; ++i)
                    gpu.RecordChoose(list, context.ring->GpuAddress(), first + i, file.events[first + i].cap, &stamp);

                gpu.RecordTimestampReadback(list, stamp);
                const auto executed = Execute(context, list);
                if (!executed)
                    return executed;

                const std::vector<uint64_t> ticks = gpu.ReadTimestamps(stamp);
                if (ticks.size() != stamp || stamp != count * (phases + 1))
                    return std::unexpected("時刻印を読めない");

                for (uint32_t i = 0; i < count; ++i) {
                    for (uint32_t phase = 0; phase < phases; ++phase) {
                        const uint32_t at = (i * (phases + 1)) + phase;
                        const double ms = ToMs(context, ticks[at], ticks[at + 1]);
                        sums[phase] += ms;
                        peaks[phase] = std::max(peaks[phase], ms);
                    }
                }
            }
        }

        double total = 0.0;
        for (const double sum : sums)
            total += sum;

        for (uint32_t phase = 0; phase < phases; ++phase)
            Log(Channel::Gpu, Level::Info, "内訳 {}: 平均 {:.4f} ms・最大 {:.4f} ms({:.0f} %)", names[phase],
                sums[phase] / events, peaks[phase], 100.0 * sums[phase] / total);

        Log(Channel::Gpu, Level::Info, "内訳の和: 1 出来事 平均 {:.4f} ms(単位 {}・{} 出来事)", total / events,
            file.tables.units, events);

        return {};
    }

    int Run(std::span<char*> arguments) {
        std::string path;
        bool measure = false;
        bool breakdown = false;
        bool expectCrossing = false;
        uint64_t eventsTotal = 0;
        std::vector<char*> rest = {arguments[0]};
        for (size_t index = 1; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            if (argument == "--choices" && index + 1 < arguments.size())
                path = arguments[++index];
            else if (argument == "--measure")
                measure = true;
            else if (argument == "--breakdown")
                breakdown = true;
            else if (argument == "--expect-crossing")
                expectCrossing = true;
            else if (argument == "--events-total" && index + 1 < arguments.size())
                eventsTotal = std::stoull(arguments[++index]);
            else
                rest.push_back(arguments[index]);
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options || path.empty()) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_time_step_choose_test --choices <ファイル> [--warp] [--queue direct|compute] [--no-gbv] "
                "[--measure] [--breakdown] [--events-total n] [--expect-crossing]");
            return 2;
        }

        const auto file = test::ReadTimeStepChoose(path);
        if (!file || file->events.empty()) {
            Log(Channel::Gpu, Level::Error, "gpu_time_step_choose_test: FAILED(ファイルを読めない: {})", path);
            return 1;
        }

        std::vector<uint16_t> befores;
        for (const test::TimeStepChooseEvent& event : file->events)
            befores.insert(befores.end(), event.before.begin(), event.before.end());

        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_time_step_choose_test: FAILED(デバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        uint64_t frequency = 0;
        if (!queue || !ring || FAILED(queue->Native()->GetTimestampFrequency(&frequency))) {
            Log(Channel::Gpu, Level::Error, "gpu_time_step_choose_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        auto gpu = sim::GpuTimeTreeChoose::Create(device->Get(), file->tables, befores);
        if (!gpu) {
            Log(Channel::Gpu, Level::Error, "gpu_time_step_choose_test: FAILED({})", gpu.error());
            return 1;
        }

        Context context{.device = device->Get(), .queue = &*queue, .ring = &*ring, .frequency = frequency};
        auto ran = Compare(context, *gpu, *file, expectCrossing);
        if (ran && measure)
            ran = Measure(context, *gpu, *file, eventsTotal);

        if (ran && breakdown)
            ran = Breakdown(context, *gpu, *file);

        if (!ran) {
            Log(Channel::Gpu, Level::Error, "gpu_time_step_choose_test: FAILED({})", ran.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_time_step_choose_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_time_step_choose_test: OK({} 出来事)", file->events.size());

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
