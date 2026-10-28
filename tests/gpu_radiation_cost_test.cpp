// gpu_radiation_cost_test.cpp — 仮の世界(ProbeSim)の熱の放射の段の費用を段ごとに測る(T-1161。docs/perf.md)。
//
// 段ごとの GPU 時間は ProbeSimOptions::radiationStageTiming(gpu/stage_stamps のタイムスタンプ。起動の後ろの印の差)で取る。
// 場面は試験の表(MakeCombustionTestTable)の既定の世界(空気の中の木箱・300 K)で、外の温度と火の有無を変えて、
// 光の当たる / 放つセルの数(放つ面・光線の本数)を変えた点を並べる(固定の費用と、数に比例する分を分ける)。
// 確かめること(ctest の gpu_radiation_cost): 印を付けても世界の結果が同じ(刻みごとのハッシュが印なしと全部一致。D-428)・
//   印を捨てていない・段の時間が取れた・放射の段の置き場が溢れない・シェーダーの assert が 0 件。
//   側が空の刻みを GPU が飛ばしても(負の側 T-1229・正の側 T-1235)、飛ばさない時と刻みごとのハッシュが全部同じ
//   (側ごとにずっと空・時々要る・ずっと要るの場面。飛ばした刻みの数も見込みどおり)。段 1 からの間接の起動(T-1235)は
//   どの場面でも入っていて、CPU リファレンスとの一致は gpu_play_metric・gpu_probe_sim が見る。
// 引数: gpu_test_options.h(--warp・--no-gbv)と --ticks n(既定 48)・--measure(全部の場面を測ってログに。既定は火の場面だけ)
#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu/queue.h"
#include "gpu_test_options.h"
#include "reaction_test_table.h"
#include "sim/probe_sim.h"

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    constexpr uint32_t TICKS_PER_FRAME = 8;       // 1 フレームの刻み(放つ面の数はフレームの最後の刻みで見る)
    constexpr uint32_t REFERENCE_300_K = 300000;  // mK
    constexpr uint32_t REFERENCE_ISA = 288150;    // mK(ISA の海面。T-1164 の測りの環境)

    // 側を飛ばす刻みの見込み(T-1229・T-1235)
    enum class SkipExpect : uint8_t { All, Some, None };

    struct Scene {
        std::string_view name;
        ProbeCascadeOptions cascade;
        bool fire = false;
        // 刻み COLD_FROM_TICK に芯の温度が coreMk(0 = 置かない)の木の球を置き、COLD_UNTIL_TICK に 300 K の球へ戻す
        uint32_t coreMk = 0;
        SkipExpect skipPositive = SkipExpect::All;
        SkipExpect skipNegative = SkipExpect::All;
    };

    constexpr uint64_t COLD_FROM_TICK = 8;
    constexpr uint64_t COLD_UNTIL_TICK = 24;
    constexpr uint32_t COLD_BLOCK_MK = 250000;
    constexpr uint32_t HOT_BLOCK_MK = 330000;  // 木が燃え始めない温度

    ProbeCascadeOptions FixedReference(uint32_t referenceMk) {
        ProbeCascadeOptions options{.environmentReference = false};
        options.params.referenceMk = referenceMk;

        return options;
    }

    std::vector<Scene> Scenes(bool measureAll) {
        std::vector<Scene> scenes = {{.name = "火・外は表の環境(既定)", .cascade = {}, .fire = true}};
        if (!measureAll)
            return scenes;

        scenes.push_back(
            {.name = "何もしない・外 300 K(2 側・放つ面ほぼ 0)", .cascade = FixedReference(REFERENCE_300_K)});
        scenes.push_back({.name = "火・外 300 K(2 側)", .cascade = FixedReference(REFERENCE_300_K), .fire = true});
        scenes.push_back(
            {.name = "何もしない・外 288.15 K(2 側・正の側が全部の面)", .cascade = FixedReference(REFERENCE_ISA)});
        scenes.push_back({.name = "火・外 288.15 K(2 側)", .cascade = FixedReference(REFERENCE_ISA), .fire = true});
        scenes.push_back({.name = "何もしない・0 K の空(1 側・全部の面)", .cascade = FixedReference(0)});
        scenes.push_back({.name = "火・0 K の空(1 側)", .cascade = FixedReference(0), .fire = true});
        scenes.push_back({.name = "火・放射なし", .cascade = {.enabled = false}, .fire = true});

        return scenes;
    }

    // 飛ばす段の出入りを見る場面(T-1229・T-1235): 側ごとにずっと空(外 = 世界の温度)・時々要る・ずっと要る
    // (火・外 300 K は火の周りも参照より冷たくならないので、負の側はずっと空。芯の冷たい / 熱い球を置いて戻す場面が「時々」。
    //  開けた所に冷たい塊を置くと、光が見える所を全部わずかに冷やすので、置き直しても負の側は空に戻らない)
    std::vector<Scene> SkipScenes() {
        return {
            {.name = "何もしない・外 300 K(両側がずっと空)", .cascade = FixedReference(REFERENCE_300_K)},
            {.name = "火・外 300 K(負の側がずっと空・正の側がずっと要る)",
             .cascade = FixedReference(REFERENCE_300_K),
             .fire = true,
             .skipPositive = SkipExpect::None},
            {.name = "芯の冷たい木の球を置いて戻す・外 300 K(負の側が時々)",
             .cascade = FixedReference(REFERENCE_300_K),
             .coreMk = COLD_BLOCK_MK,
             .skipNegative = SkipExpect::Some},
            {.name = "芯の熱い木の球を置いて戻す・外 300 K(正の側が時々)",
             .cascade = FixedReference(REFERENCE_300_K),
             .coreMk = HOT_BLOCK_MK,
             .skipPositive = SkipExpect::Some},
            {.name = "火・外 301 K(両側がずっと要る)",
             .cascade = FixedReference(REFERENCE_300_K + 1000),
             .fire = true,
             .skipPositive = SkipExpect::None,
             .skipNegative = SkipExpect::None},
            {.name = "何もしない・外 299 K(正の側がずっと要る)",
             .cascade = FixedReference(REFERENCE_300_K - 1000),
             .skipPositive = SkipExpect::None},
        };
    }

    // 場面のコマンド(刻みの順)
    std::vector<ProbeCommand> SceneCommands(const BakedReactionTable& table, const Scene& scene) {
        std::vector<ProbeCommand> commands;
        if (scene.fire)
            commands.push_back(MakePokeCommand(0, 0, 35, 32, 32));  // 木箱の壁に火

        if (scene.coreMk == 0)
            return commands;

        // 木の壁と同じ中身の球(半径 4。300 K)の芯(半径 1)だけを冷たく(熱く)して置き、後で球ごと 300 K で置き直す。
        // 芯は木に囲まれて光が出ない(面の向こうが光を通さない)が、写しの段は向こうを見ないのでその側を流す。
        // 置き直した後は全部が参照の温度なので、その側は空に戻って飛ばす(流す → 飛ばすの出入りを両方通る)
        const std::array<SpeciesAmount, 3> wood = {
            SpeciesAmount{.species = table.SpeciesId("cellulose"), .amount = 38600000},
            SpeciesAmount{.species = table.SpeciesId("oxygen"), .amount = 983000},
            SpeciesAmount{.species = table.SpeciesId("nitrogen"), .amount = 3697000}};
        const ProbePlaceShape shell{.x = 12, .y = 32, .z = 32, .radius = 4, .temperatureMilliKelvin = REFERENCE_300_K};
        ProbePlaceShape core = shell;
        core.radius = 1;
        core.temperatureMilliKelvin = scene.coreMk;
        commands.push_back(MakePlaceCommand(COLD_FROM_TICK, 1, shell, wood));
        commands.push_back(MakePlaceCommand(COLD_FROM_TICK, 2, core, wood));
        commands.push_back(MakePlaceCommand(COLD_UNTIL_TICK, 3, shell, wood));

        return commands;
    }

    // 飛ばした刻みの数が見込みどおりか(時々: 冷たい芯の前の刻みに加えて、戻した後にまた飛ばす)
    bool SkipsAsExpected(SkipExpect expect, uint64_t skipped, uint64_t ticks) {
        switch (expect) {
            case SkipExpect::All: return skipped == ticks;
            case SkipExpect::None: return skipped == 0;
            case SkipExpect::Some: return skipped > COLD_FROM_TICK && skipped < ticks;
        }

        return false;
    }

    struct Run {
        std::vector<uint64_t> hashes;
        std::vector<uint64_t> stageTicks;
        std::vector<uint64_t> stageCounts;
        uint64_t unitTicks = 0;  // 全部の単位の GPU 時間の和
        uint64_t faceSum = 0;    // フレームの最後の刻みの放つ面の数の和
        uint64_t raySum = 0;
        uint64_t samples = 0;
        uint64_t dropped = 0;
        uint64_t skippedPositive = 0;  // 側が空で飛ばした刻みの数(T-1229・T-1235)
        uint64_t skippedNegative = 0;
        uint32_t overflow = 0;
        uint32_t asserts = 0;
        bool ok = false;
    };

    std::optional<Run> RunScene(ID3D12Device5* device, gpu::Queue& queue, const BakedReactionTable& table,
                                const Scene& scene, uint64_t ticks, bool timing, bool skipIdle = true,
                                bool indirect = false) {
        auto created = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table,
                                        {.cascadeRadiation = scene.cascade,
                                         .radiationStageTiming = timing,
                                         .radiationSkipIdle = skipIdle,
                                         .radiationIndirectLevels = indirect});
        if (!created) {
            Log(Channel::Sim, Level::Error, "{}", created.error());
            return std::nullopt;
        }

        ProbeSim& simulation = *created;
        const std::vector<ProbeCommand> commands = SceneCommands(table, scene);
        const uint32_t unitsPerTick = simulation.UnitsPerTick();
        const uint64_t end = ticks * unitsPerTick;
        Run run;
        for (uint64_t position = 0, frame = 0; position < end; ++frame) {
            const uint64_t units = std::min<uint64_t>(uint64_t{TICKS_PER_FRAME} * unitsPerTick, end - position);
            // このフレームの刻みに当てるコマンド
            const uint64_t firstTick = position / unitsPerTick;
            const uint64_t endTick = (position + units + unitsPerTick - 1) / unitsPerTick;
            std::vector<ProbeCommand> frameCommands;
            for (const ProbeCommand& command : commands) {
                if (command.targetTick >= firstTick && command.targetTick < endTick)
                    frameCommands.push_back(command);
            }
            const ProbeFrameInput input{.firstTick = position / unitsPerTick,
                                        .firstUnit = static_cast<uint32_t>(position % unitsPerTick),
                                        .unitCount = static_cast<uint32_t>(units),
                                        .commands = frameCommands};
            ID3D12CommandList* list = simulation.RecordFrame(0, input);
            if (list == nullptr || !queue.WaitCpu(queue.Submit(list)))
                return std::nullopt;

            position += units;
            const ProbeFrameReadback readback = simulation.ReadFrame(0);
            for (const ProbeTickHash& hash : readback.hashes)
                run.hashes.push_back(hash.WorldHash());

            for (const uint64_t unitTicks : readback.unitGpuTicks)
                run.unitTicks += unitTicks;

            run.faceSum += readback.radiationFaces;
            run.raySum += readback.radiationRays;
            run.skippedPositive = readback.radiationSkippedPositive;
            run.skippedNegative = readback.radiationSkippedNegative;
            ++run.samples;
            run.overflow |= readback.radiationOverflow;
            run.asserts += readback.debugAssertCount;
        }

        if (const gpu::StageStamps* stamps = simulation.RadiationStageStamps()) {
            run.stageTicks.assign(stamps->Ticks().begin(), stamps->Ticks().end());
            run.stageCounts.assign(stamps->Counts().begin(), stamps->Counts().end());
            run.dropped = stamps->Dropped();
        }

        run.ok = run.hashes.size() == ticks;

        return run;
    }

    std::string StageName(size_t stage) {
        constexpr std::array<std::string_view, 5> HEAD = {"Start", "Grid", "Apply", "Hash+", "Hash-"};
        const auto first = static_cast<size_t>(ProbeRadiationStage::PositiveFirst);
        const auto negative = static_cast<size_t>(ProbeRadiationStage::NegativeFirst);
        if (stage < first)
            return std::string(HEAD[stage]);

        if (stage < negative)
            return std::format("+{}", GpuCascadeLight::StageName(static_cast<uint32_t>(stage - first)));

        return std::format("-{}", GpuCascadeLight::StageName(static_cast<uint32_t>(stage - negative)));
    }

    // 1 刻みあたりの µs で段ごとの内訳をログへ
    void LogRun(const Scene& scene, const Run& run, uint64_t ticks, double frequency) {
        const auto perTick = [&](uint64_t value) {
            return static_cast<double>(value) * 1'000'000.0 / frequency / static_cast<double>(ticks);
        };
        const uint64_t samples = std::max<uint64_t>(1, run.samples);
        uint64_t radiation = 0;
        std::string stages;
        for (size_t stage = 1; stage < run.stageTicks.size(); ++stage) {
            if (run.stageCounts[stage] == 0)
                continue;

            radiation += run.stageTicks[stage];
            stages += std::format(" {}={:.1f}", StageName(stage), perTick(run.stageTicks[stage]));
        }

        Log(Channel::Sim, Level::Info,
            "放射の費用 [{}]: 1 刻み {:.1f} µs(放射の段の印の和 {:.1f} µs)・放つ面 {}・光線 "
            "{}(フレームの最後の刻みの平均)・飛ばした刻み 正 {}/{}・負 {}/{}",
            scene.name, perTick(run.unitTicks), perTick(radiation), run.faceSum / samples, run.raySum / samples,
            run.skippedPositive, ticks, run.skippedNegative, ticks);
        if (!stages.empty())
            Log(Channel::Sim, Level::Info, "  段ごとの µs/刻み:{}", stages);
    }

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition)
                return;

            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    int RunTest(ID3D12Device5* device, const BakedReactionTable& table, uint64_t ticks, bool measureAll) {
        Failures failures;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"RadiationCostTest");
        failures.Check(queue.has_value(), "キューを作れた");
        if (!queue)
            return failures.count;

        const double frequency = static_cast<double>(queue->TimestampFrequency());
        Log(Channel::Sim, Level::Info, "表の参照の温度 {} mK・{} 刻み", ProbeCascadeReferenceMk({}, table), ticks);

        // --- 印を付けても結果が同じ(既定の火の場面)---
        const std::vector<Scene> scenes = Scenes(measureAll);
        const std::optional<Run> plain = RunScene(device, *queue, table, scenes.front(), ticks, false);
        failures.Check(plain && plain->ok, "印なしで流せた");

        // --- 側が空の刻みを飛ばしても結果が同じ(T-1229・T-1235。飛ばさない時と刻みごとのハッシュが全部一致)---
        for (const Scene& scene : SkipScenes()) {
            const std::optional<Run> skipping = RunScene(device, *queue, table, scene, ticks, false, true);
            const std::optional<Run> running = RunScene(device, *queue, table, scene, ticks, false, false);
            const std::optional<Run> indirect = RunScene(device, *queue, table, scene, ticks, false, true, true);
            failures.Check(skipping && skipping->ok && running && running->ok && indirect && indirect->ok,
                           std::format("流せた: {}", scene.name));
            if (!skipping || !running || !indirect)
                continue;

            // 段 1 からの間接の起動(T-1235。既定は切る・ADR-0495)を入れても同じ
            failures.Check(skipping->hashes == indirect->hashes,
                           std::format("間接の起動でも直の起動と刻みごとの世界のハッシュが全部同じ: {}", scene.name));

            Log(Channel::Sim, Level::Info, "飛ばした刻み [{}]: 正 {}/{}・負 {}/{}", scene.name,
                skipping->skippedPositive, ticks, skipping->skippedNegative, ticks);
            failures.Check(skipping->hashes == running->hashes,
                           std::format("飛ばしても刻みごとの世界のハッシュが全部同じ: {}", scene.name));
            failures.Check(running->skippedPositive == 0 && running->skippedNegative == 0,
                           std::format("飛ばさない時は 0 刻み: {}", scene.name));
            const bool expected = SkipsAsExpected(scene.skipPositive, skipping->skippedPositive, ticks) &&
                                  SkipsAsExpected(scene.skipNegative, skipping->skippedNegative, ticks);
            failures.Check(expected, std::format("飛ばした刻みの数が見込みどおり: {}", scene.name));
            failures.Check(skipping->asserts == 0 && skipping->overflow == 0,
                           std::format("飛ばした時も assert・溢れが 0: {}", scene.name));
        }

        for (const Scene& scene : scenes) {
            const std::optional<Run> run = RunScene(device, *queue, table, scene, ticks, true);
            failures.Check(run && run->ok, std::format("流せた: {}", scene.name));
            if (!run)
                continue;

            LogRun(scene, *run, ticks, frequency);
            // 測りの時は同じ場面を別の形でも(段 1 からを間接の起動・側を飛ばさない。T-1235 の前後を同じ回の中で比べる)
            for (const auto& [skipIdle, indirect, label] :
                 {std::tuple{true, true, "間接の起動"}, std::tuple{false, false, "飛ばさない・直の起動"}}) {
                if (!measureAll || !scene.cascade.enabled)
                    break;

                const std::optional<Run> before = RunScene(device, *queue, table, scene, ticks, true, skipIdle,
                                                           indirect);
                const std::string name = std::format("{}・{}", scene.name, label);
                Scene labeled = scene;
                labeled.name = name;
                if (before)
                    LogRun(labeled, *before, ticks, frequency);
            }
            failures.Check(run->overflow == 0, std::format("放射の段の置き場が溢れない: {}", scene.name));
            failures.Check(run->asserts == 0, std::format("シェーダーの assert が 0 件: {}", scene.name));
            failures.Check(run->dropped == 0, std::format("印を捨てていない: {}", scene.name));
            if (&scene != &scenes.front())
                continue;

            failures.Check(plain && run->hashes == plain->hashes, "印を付けても刻みごとの世界のハッシュが全部同じ");
            failures.Check(
                !run->stageCounts.empty() && run->stageCounts[static_cast<size_t>(ProbeRadiationStage::Apply)] == ticks,
                "当てる段の時間が刻みの数だけ取れた");
        }

        return failures.count;
    }

    uint64_t TakeTicks(std::vector<char*>& arguments, uint64_t fallback) {
        for (size_t index = 1; index + 1 < arguments.size(); ++index) {
            if (std::string_view(arguments[index]) != "--ticks")
                continue;

            const std::string_view text = arguments[index + 1];
            uint64_t value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index),
                            arguments.begin() + static_cast<std::ptrdiff_t>(index) + 2);
            return parsed.ec == std::errc{} && value > 0 ? value : 0;
        }

        return fallback;
    }

    bool TakeFlag(std::vector<char*>& arguments, std::string_view flag) {
        const auto found = rng::find_if(arguments, [&](const char* argument) { return argument == flag; });
        if (found == arguments.end())
            return false;

        arguments.erase(found);

        return true;
    }

}  // namespace

int main(int argc, char** argv) {
    std::vector<char*> arguments(argv, argv + argc);
    const uint64_t ticks = TakeTicks(arguments, 48);
    const bool measureAll = TakeFlag(arguments, "--measure");
    const std::optional<test::GpuTestOptions> options = test::ParseGpuTestOptions(std::span(arguments));
    const auto table = BakeReactionTable(MakeCombustionTestTable());
    if (!options || !table || ticks == 0) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_radiation_cost_test [--warp] [--no-gbv] [--ticks n] [--measure]");
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    const int failures = RunTest(device->Get(), *table, ticks, measureAll);
    const bool passed = failures == 0 && test::PassesValidation(*device, "gpu_radiation_cost_test");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
