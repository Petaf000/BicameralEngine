// gpu_probe_sim_test.cpp — 仮の刻み(sim/probe_sim)を単位の列として GPU で走らせ、CPU リファレンスとビット一致するかを確かめる
// (T-0004・T-0012・T-0086・T-0005・T-0089)。
//
// 確かめること:
//   - (T-0005・T-0089)伝導と反応は Work Graph が活性のブロックだけを計算し、CPU は全部のセルを計算する。それでも刻みごとのハッシュ・
//     エネルギーの合計が一致し、GPU が計算したブロックの数が CPU の予想(変わった・まだ進めるブロック + つついたブロック + その 6 面の隣)と同じ。
//     エネルギーはつつき(湧き出し)の分だけ変わる。木箱の壁をつついて、反応が起きる場面を入れる
//   - (06「テスト」の 1 つ目)同じ刻みの数とコマンドなら、フレームへの単位の分け方(1 刻みずつ / 8 刻みずつ / 1 単位ずつ /
//     刻みの途中で切るばらばら / 重さの単位を足して分ける)を変えても、GPU が刻みごとに取った状態のハッシュ列が CPU リファレンスと一致する。
//     コマンドは数刻み先まで先に GPU のキューへ足す(キューの中で自分の刻みまで待つ。フレームの切れ目と関係なく適用される)
//   - 最後の抽出(描画が読むもの。温度と見る物質 3 つ)が、最後の刻みの状態から CPU で作ったものと一致する
//   - (T-0008)伝導のグラフのノードのカウンタ: ConductBlock の起動の数 = WakeBlocks の出力の数 = CPU が予想した計算するブロックの数の合計。
//     分け方を変えてもカウンタは同じ(決定的)。上限に当たったもの・近づいたものは無い
//   - イベントが (刻み, 種類, 場所) の順に並んで戻る(足した順と違う刻みも入れる)。一時置き場が溢れたら落とした数が合う
//   - (06「テスト」の 2 つ目・15 §2)窓の操作のようにフレームの途中で届くコマンドを記録し、再生ファイルに書いて読み、
//     別の分け方で再生して同じハッシュ列になる
//   - debug layer のエラーが 0 件
// 引数: gpu_test_options.h(--warp)。キューは compute だけ(シミュは compute キュー。06 §4)。
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <ranges>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "save/replay_session.h"
#include "sim/probe_sim.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    constexpr uint64_t TOTAL_TICKS = 40;
    constexpr uint32_t TEST_BUSY_ITERATIONS = 64;  // 重さの単位を入れる分け方で(結果に入らないことを確かめる)
    constexpr uint32_t TEST_BUSY_PIECES = 3;
    constexpr uint32_t OVERFLOW_POKES = 300;  // 刻みの一時置き場(PROBE_TICK_EVENT_CAPACITY)を溢れさせる数
    constexpr uint64_t OVERFLOW_TICK = 3;

    // 刻み・場所。同じ刻みの同じセル(順番に依存しないか)と、格子の端・角、ブロックの境目(3 と 4)、
    // 場所の大きい順に足す刻み(21)も入れる。木箱の壁(x = 28)をつついて燃やす(反応と、反応が隣を起こす場面)
    struct PokeSpec {
        uint64_t tick;
        uint32_t x;
        uint32_t y;
        uint32_t z;
    };

    constexpr uint32_t LAST = PROBE_GRID_SIZE - 1;
    constexpr std::array<PokeSpec, 9> POKES = {{
        {.tick = 0, .x = 28, .y = 32, .z = 32},
        {.tick = 0, .x = 10, .y = 10, .z = 10},
        {.tick = 0, .x = 10, .y = 10, .z = 10},
        {.tick = 5, .x = 0, .y = 0, .z = 0},
        {.tick = 5, .x = LAST, .y = 32, .z = LAST},
        {.tick = 13, .x = 32, .y = 32, .z = PROBE_VIEW_Z},
        {.tick = 21, .x = 33, .y = 32, .z = PROBE_VIEW_Z},
        {.tick = 21, .x = 3, .y = 4, .z = 3},
        {.tick = 39, .x = 50, .y = 3, .z = 60},
    }};

    // 記録の試験: 窓のクリックのように、フレーム frame に届いて「その時の次の適用の刻み」が付くコマンド(z = PROBE_VIEW_Z の面)
    struct ClickSpec {
        size_t frame;
        uint32_t x;
        uint32_t y;
    };

    constexpr std::array<ClickSpec, 8> CLICKS = {{
        {.frame = 0, .x = 10, .y = 10},
        {.frame = 0, .x = 10, .y = 10},
        {.frame = 3, .x = 0, .y = 0},
        {.frame = 3, .x = LAST, .y = 32},
        {.frame = 9, .x = 32, .y = 32},
        {.frame = 15, .x = 33, .y = 32},
        {.frame = 15, .x = 3, .y = 4},
        {.frame = 22, .x = 50, .y = 3},
    }};

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition)
                return;

            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    std::vector<ProbeCommand> MakeCommands() {
        std::vector<ProbeCommand> commands;
        commands.reserve(POKES.size());
        uint32_t sequence = 0;
        for (const PokeSpec& poke : POKES)
            commands.push_back(MakePokeCommand(poke.tick, sequence++, poke.x, poke.y, poke.z));

        return commands;
    }

    // 一時置き場を溢れさせる: 同じ刻みに違うセルを OVERFLOW_POKES 個
    std::vector<ProbeCommand> MakeOverflowCommands() {
        std::vector<ProbeCommand> commands;
        commands.reserve(OVERFLOW_POKES);
        for (uint32_t index = 0; index < OVERFLOW_POKES; ++index) {
            commands.push_back(
                MakePokeCommand(OVERFLOW_TICK, index, index % PROBE_GRID_SIZE, 20 + index / PROBE_GRID_SIZE, 5));
        }

        return commands;
    }

    // CPU リファレンスの S(0)〜S(TOTAL_TICKS) の要約([t] が S(t))
    struct Reference {
        std::vector<ProbeTickHash> ticks;
        uint64_t extractionHash = 0;    // S(TOTAL_TICKS) の抽出のセルの部分(ProbeExtractionHash)
        bool energyConserved = true;    // エネルギーの合計が、つつきで足した分だけ変わった(伝導と反応では変わらない)
        bool elementsConserved = true;  // 元素ごとの数が S(0) と同じ
        bool reacted = false;           // 反応が起きた(炭ができた)
    };

    // 全部のセルの元素ごとの数
    std::vector<uint64_t> CountAllElements(const BakedReactionTable& table, std::span<const reaction::RxCell> cells) {
        std::vector<uint64_t> total(table.elementNames.size(), 0);
        for (const reaction::RxCell& cell : cells) {
            const std::vector<uint64_t> counts = CountElements(table, cell);
            for (size_t element = 0; element < total.size(); ++element)
                total[element] += counts[element];
        }

        return total;
    }

    Reference RunReference(const BakedReactionTable& table, std::span<const ProbeCommand> commands) {
        ProbeReference reference(table);
        Reference result;
        result.ticks.push_back(
            {.tick = 0, .hash = ProbeStateHash(reference.State(0)), .energy = ProbeEnergySum(reference.State(0))});

        const std::vector<uint64_t> initialElements = CountAllElements(table, reference.State(0));
        for (uint64_t tick = 0; tick < TOTAL_TICKS; ++tick) {
            reference.Advance(tick, commands);
            const std::span<const reaction::RxCell> state = reference.State(tick + 1);
            result.ticks.push_back({.tick = tick + 1,
                                    .hash = ProbeStateHash(state),
                                    .energy = ProbeEnergySum(state),
                                    .sourceEnergy = reference.SourceEnergy(),
                                    .scheduledBlocks = reference.ScheduledBlocks()});

            result.energyConserved = result.energyConserved &&
                                     result.ticks.back().energy == result.ticks[tick].energy + reference.SourceEnergy();
        }

        const std::span<const reaction::RxCell> last = reference.State(TOTAL_TICKS);
        result.elementsConserved = CountAllElements(table, last) == initialElements;
        result.reacted = rng::any_of(
            last, [&](const reaction::RxCell& cell) { return ProbeViewAmount(cell, table.SpeciesId("carbon")) > 0; });
        result.extractionHash = ProbeExtractionHash(
            MakeProbeExtractionCells(last, reference.Caches(TOTAL_TICKS), ProbeViewSpecies(table)));

        return result;
    }

    // GPU が返すはずのイベント: つつきごとに 1 つを (刻み, 種類, 場所) の順に
    std::vector<ProbeEvent> ExpectedEvents(std::span<const ProbeCommand> commands) {
        std::vector<ProbeEvent> events;
        for (const ProbeCommand& command : commands) {
            events.push_back({.tick = command.targetTick,
                              .type = PROBE_EVENT_POKE_APPLIED,
                              .place = ProbePokePlace(command.payload[0], command.payload[1], command.payload[2])});
        }

        rng::sort(events, [](const ProbeEvent& a, const ProbeEvent& b) {
            if (a.tick != b.tick)
                return a.tick < b.tick;
            return ProbeEventKey(a.type, a.place) < ProbeEventKey(b.type, b.place);
        });

        return events;
    }

    // 分け方: 1 フレームに投げる単位の数の列。pattern を繰り返して合計 total にする(最後は切る)
    std::vector<uint32_t> RepeatPattern(std::span<const uint32_t> pattern, uint32_t total) {
        std::vector<uint32_t> sizes;
        uint32_t sum = 0;
        for (size_t index = 0; sum < total; ++index) {
            const uint32_t size = std::min(pattern[index % pattern.size()], total - sum);
            sizes.push_back(size);
            sum += size;
        }

        return sizes;
    }

    struct Plan {
        const char* name;
        ProbeSimOptions options;
        std::vector<uint32_t> unitsPerFrame;
    };

    // フレーム frame(まだ記録していない最初の適用の刻み applyTick、足せる数 limit)に GPU のキューへ足すコマンド
    using CommandSource = std::function<std::vector<ProbeCommand>(size_t frame, uint64_t applyTick, uint32_t limit)>;

    struct RunResult {
        bool ok = false;

        // --- 世界の結果 ---
        std::vector<ProbeTickHash> hashes;
        uint64_t extractionHash = 0;          // 抽出のセルの部分のハッシュ
        uint32_t extractionActiveBlocks = 0;  // 抽出の活性の印の数

        // --- イベントとコマンド ---
        std::vector<ProbeEvent> events;
        uint32_t droppedEventCount = 0;
        std::vector<ProbeCommand> enqueued;  // 足したコマンド(足した順)

        // --- 伝導のグラフのカウンタ ---
        gpu::GraphStatsSnapshot graphStats;  // ノードのカウンタ(全部のフレームの合計)
        uint32_t graphFindingCount = 0;
        std::string graphSummary;  // その 1 行の要約
    };

    // 抽出(COMMON。全部のセル + ブロックの活性の印)を読み戻す
    std::vector<uint32_t> ReadExtraction(ID3D12Device5* device, ID3D12Resource* extraction) {
        std::vector<uint32_t> cells(PROBE_EXTRACTION_WORDS);
        auto queue = gpu::ImmediateQueue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        const ComPtr<ID3D12Resource> readback = gpu::CreateBuffer(device, uint64_t{PROBE_EXTRACTION_WORDS} * 4,
                                                                  gpu::BufferKind::Readback);
        if (!queue || !readback)
            return {};

        ID3D12GraphicsCommandList10* list = queue->Begin();
        if (list == nullptr)
            return {};

        list->CopyBufferRegion(readback.Get(), 0, extraction, 0, uint64_t{PROBE_EXTRACTION_WORDS} * 4);
        if (!queue->ExecuteAndWait() || !gpu::ReadBuffer(readback.Get(), std::as_writable_bytes(std::span(cells))))
            return {};

        return cells;
    }

    // 単位を plan の分け方でフレームにして走らせる(フレームの枠を順に使い回す。毎フレーム抽出する)。
    // コマンドは source がフレームごとに渡す
    RunResult RunPlan(ID3D12Device5* device, const BakedReactionTable& table, const Plan& plan,
                      const CommandSource& source) {
        RunResult result;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"TestSim");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table, plan.options);

        if (!queue || !simulation) {
            Log(Channel::Sim, Level::Error, "作れない: {}{}", queue ? "" : queue.error(),
                simulation ? "" : simulation.error());
            return result;
        }

        const uint32_t unitsPerTick = simulation->UnitsPerTick();
        uint64_t unitPosition = 0;  // 通しの単位の番号(刻み × 1 刻みの単位の数 + 刻みの中の番号)
        uint32_t extractionTarget = 0;

        for (size_t frame = 0; frame < plan.unitsPerFrame.size(); ++frame) {
            const uint64_t firstTick = unitPosition / unitsPerTick;
            const auto firstUnit = static_cast<uint32_t>(unitPosition % unitsPerTick);
            const auto slot = static_cast<uint32_t>(frame % ProbeSim::FRAME_SLOT_COUNT);
            extractionTarget = static_cast<uint32_t>(frame % PROBE_EXTRACTION_COUNT);

            const std::vector<ProbeCommand> commands = source(
                frame, ProbeSim::NextApplyTick(firstTick, firstUnit),
                std::min(simulation->FreeCommandSlots(), PROBE_MAX_COMMANDS));

            ID3D12CommandList* list = simulation->RecordFrame(slot, {.firstTick = firstTick,
                                                                     .firstUnit = firstUnit,
                                                                     .unitCount = plan.unitsPerFrame[frame],
                                                                     .extract = true,
                                                                     .extractionTarget = extractionTarget,
                                                                     .commands = commands});

            if (list == nullptr)
                return result;

            // テストは 1 つずつ終わりを待つ(フレームのループは待たない。frame/frame_loop.cpp)
            if (!queue->WaitCpu(queue->Submit(list)))
                return result;
            const ProbeFrameReadback readback = simulation->ReadFrame(slot);
            result.hashes.insert(result.hashes.end(), readback.hashes.begin(), readback.hashes.end());
            result.events.insert(result.events.end(), readback.events.begin(), readback.events.end());
            result.droppedEventCount += readback.droppedEventCount;
            gpu::AccumulateGraphStats(result.graphStats, readback.graphStats);
            result.graphFindingCount += readback.graphFindingCount;
            result.enqueued.insert(result.enqueued.end(), commands.begin(), commands.end());
            unitPosition += plan.unitsPerFrame[frame];
        }

        const std::vector<uint32_t> cells = ReadExtraction(device, simulation->Extraction(extractionTarget));
        if (cells.size() != PROBE_EXTRACTION_WORDS)
            return result;

        const std::span<const uint32_t> extracted(cells);
        result.extractionHash = ProbeExtractionHash(extracted.first(PROBE_EXTRACTION_BLOCK_OFFSET));
        result.extractionActiveBlocks = static_cast<uint32_t>(
            rng::count(extracted.subspan(PROBE_EXTRACTION_BLOCK_OFFSET, PROBE_BLOCK_COUNT), 1u));
        result.graphSummary = gpu::FormatGraphStats(simulation->ConductStatsLayout(), result.graphStats);
        result.ok = unitPosition == TOTAL_TICKS * unitsPerTick;

        return result;
    }

    // 決まったコマンドの列を、数刻み先まで先に足す(再生と同じ。save::ReplayPlayer::TakeCommands)
    CommandSource ScheduledSource(save::ReplayPlayer& player) {
        return [&player](size_t, uint64_t applyTick, uint32_t limit) {
            return player.TakeCommands(applyTick, limit);
        };
    }

    save::ReplayPlayer MakePlayer(std::span<const ProbeCommand> commands) {
        save::ReplayFile replay;
        replay.commands.assign(commands.begin(), commands.end());

        return save::ReplayPlayer(std::move(replay));
    }

    // GPU の要約の列が S(1)〜S(TOTAL_TICKS) の順に並び、CPU と一致するか(ハッシュ・エネルギーの合計・つつきの分・計算したブロックの数)
    bool HashesMatch(std::span<const ProbeTickHash> hashes, const Reference& expected) {
        if (hashes.size() != TOTAL_TICKS)
            return false;

        for (size_t index = 0; index < hashes.size(); ++index) {
            const ProbeTickHash& gpu = hashes[index];
            const ProbeTickHash& cpu = expected.ticks[index + 1];
            if (gpu.tick != cpu.tick || gpu.hash != cpu.hash || gpu.energy != cpu.energy ||
                gpu.sourceEnergy != cpu.sourceEnergy || gpu.scheduledBlocks != cpu.scheduledBlocks) {
                Log(Channel::Sim, Level::Error,
                    "  S({}) = {:016x} エネルギー {} つつき {} ブロック {}(CPU S({}) = {:016x} エネルギー {} つつき {} "
                    "ブロック {})",
                    gpu.tick, gpu.hash, gpu.energy, gpu.sourceEnergy, gpu.scheduledBlocks, cpu.tick, cpu.hash,
                    cpu.energy, cpu.sourceEnergy, cpu.scheduledBlocks);

                return false;
            }
        }

        return true;
    }

    // 伝導のグラフのカウンタが CPU の予想と合うか(T-0008)。ConductBlock は 1 レコード = 1 グループ = 計算した 1 ブロック
    bool GraphStatsMatch(const RunResult& result, const Reference& expected) {
        if (result.graphStats.nodes.size() != 2 || result.graphStats.gaugePeaks.size() != 2)
            return false;

        const gpu::GraphNodeCounters& wake = result.graphStats.nodes[PROBE_STATS_NODE_WAKE];
        const gpu::GraphNodeCounters& conduct = result.graphStats.nodes[PROBE_STATS_NODE_CONDUCT];
        uint64_t scheduled = 0;
        for (size_t index = 1; index < expected.ticks.size(); ++index)
            scheduled += expected.ticks[index].scheduledBlocks;

        const uint32_t activeListPeak = result.graphStats.gaugePeaks[PROBE_STATS_GAUGE_ACTIVE_LIST];

        const bool match = conduct.launches == scheduled && conduct.inputRecords == scheduled &&
                           wake.outputRecords == scheduled && wake.launches == wake.inputRecords &&
                           wake.launches >= TOTAL_TICKS && wake.refusedOutputs == 0 &&
                           wake.peakRequestedOutputs <= PROBE_WAKE_MAX_RECORDS && conduct.outputRecords == 0 &&
                           activeListPeak > 1 && activeListPeak <= PROBE_ACTIVE_LIST_CAPACITY &&
                           result.graphFindingCount == 0;

        if (!match) {
            Log(Channel::Sim, Level::Error,
                "  WakeBlocks 起動 {} 入力 {} 出力 {} 止めた {} 最大 {} / ConductBlock 起動 {} 入力 {} 出力 {}"
                "(CPU の予想 {})/ 一覧 最大 {} / 見つかった上限 {}",
                wake.launches, wake.inputRecords, wake.outputRecords, wake.refusedOutputs, wake.peakRequestedOutputs,
                conduct.launches, conduct.inputRecords, conduct.outputRecords, scheduled, activeListPeak,
                result.graphFindingCount);
        }

        return match;
    }

    // --- 分け方 ---

    const std::array<uint32_t, 7> MIXED_PATTERN = {1, 2, 5, 4, 7, 11, 3};  // 刻みの途中で切れる
    const std::array<uint32_t, 4> MIXED_BUSY_PATTERN = {5, 7, 2, 13};
    ProbeSimOptions BusyOptions() {
        return {.busyIterations = TEST_BUSY_ITERATIONS, .busyPieces = TEST_BUSY_PIECES};
    }

    Plan MixedPlan() {
        return {.name = "ばらばら",
                .options = {},
                .unitsPerFrame = RepeatPattern(MIXED_PATTERN,
                                               static_cast<uint32_t>(TOTAL_TICKS * PROBE_FIXED_UNITS_PER_TICK))};
    }

    Plan MixedBusyPlan() {
        return {.name = "重さを分けてばらばら",
                .options = BusyOptions(),
                .unitsPerFrame = RepeatPattern(
                    MIXED_BUSY_PATTERN,
                    static_cast<uint32_t>(TOTAL_TICKS * (PROBE_FIXED_UNITS_PER_TICK + TEST_BUSY_PIECES)))};
    }

    std::vector<Plan> MakePlans() {
        const uint32_t plainUnits = PROBE_FIXED_UNITS_PER_TICK;
        const auto plainTotal = static_cast<uint32_t>(TOTAL_TICKS * plainUnits);

        return {
            {.name = "1 刻みずつ", .options = {}, .unitsPerFrame = std::vector<uint32_t>(TOTAL_TICKS, plainUnits)},
            {.name = "8 刻みずつ",
             .options = {},
             .unitsPerFrame = std::vector<uint32_t>(TOTAL_TICKS / 8, plainUnits * 8)},
            {.name = "1 単位ずつ", .options = {}, .unitsPerFrame = std::vector<uint32_t>(plainTotal, 1)},
            MixedPlan(),
            MixedBusyPlan(),
        };
    }

    // --- 試験 ---

    // 分け方を変えても、ハッシュ列・抽出・イベントの並びが同じ(CPU リファレンスと一致)
    void TestFramings(ID3D12Device5* device, const BakedReactionTable& table, Failures& failures) {
        const std::vector<ProbeCommand> commands = MakeCommands();
        const Reference expected = RunReference(table, commands);
        const std::vector<ProbeEvent> expectedEvents = ExpectedEvents(commands);
        const auto scheduled = expected.ticks | views::drop(1) |
                               views::transform([](const ProbeTickHash& tick) { return tick.scheduledBlocks; });

        Log(Channel::Sim, Level::Info,
            "CPU リファレンス: S({}) = {:016x}  エネルギー {} mJ  計算したブロック 最大 {} / {}", TOTAL_TICKS,
            expected.ticks.back().hash, static_cast<int64_t>(expected.ticks.back().energy), rng::max(scheduled),
            PROBE_BLOCK_COUNT);

        failures.Check(expected.energyConserved,
                       "CPU リファレンス: エネルギーの合計はつつきの分だけ変わる(伝導と反応で保存)");
        failures.Check(expected.elementsConserved, "CPU リファレンス: 元素ごとの数が変わらない");
        failures.Check(expected.reacted, "CPU リファレンス: 木箱の壁が反応した(炭ができた)");
        failures.Check(rng::max(scheduled) < PROBE_BLOCK_COUNT / 2 && rng::min(scheduled) > 0,
                       "CPU リファレンス: 伝導するのは一部のブロックだけ(活性が効く試験になっている)");

        std::optional<gpu::GraphStatsSnapshot> firstGraphStats;
        for (const Plan& plan : MakePlans()) {
            save::ReplayPlayer player = MakePlayer(commands);
            const RunResult result = RunPlan(device, table, plan, ScheduledSource(player));

            Log(Channel::Sim, Level::Info, "GPU({}、{} フレーム): S({}) = {:016x}  抽出 {:016x}  イベント {}",
                plan.name, plan.unitsPerFrame.size(), result.hashes.empty() ? 0 : result.hashes.back().tick,
                result.hashes.empty() ? 0 : result.hashes.back().hash, result.extractionHash, result.events.size());

            failures.Check(result.ok, std::format("{}: 走らせられた", plan.name));
            failures.Check(player.LateCommands() == 0 && result.enqueued.size() == commands.size(),
                           std::format("{}: コマンドを全部、刻みに間に合うように足した", plan.name));
            failures.Check(HashesMatch(result.hashes, expected),
                           std::format("{}: 刻みごとのハッシュが CPU と一致", plan.name));
            failures.Check(result.extractionHash == expected.extractionHash,
                           std::format("{}: 最後の抽出(全部のセル)が CPU と一致", plan.name));
            failures.Check(result.extractionActiveBlocks == expected.ticks.back().scheduledBlocks,
                           std::format("{}: 抽出の活性の印の数が、最後の刻みで計算したブロックの数と一致", plan.name));
            failures.Check(result.events == expectedEvents && result.droppedEventCount == 0,
                           std::format("{}: イベントが (刻み, 種類, 場所) の順に全部戻る", plan.name));
            failures.Check(GraphStatsMatch(result, expected),
                           std::format("{}: 伝導のグラフのカウンタが CPU の予想と合う", plan.name));

            if (!firstGraphStats) {
                firstGraphStats = result.graphStats;
                Log(Channel::Sim, Level::Info, "  {}", result.graphSummary);
            }

            // ノードのカウンタは分け方によらない(計器のコマンドキューは、先に足す数が分け方で変わるので比べない)
            failures.Check(result.graphStats.nodes == firstGraphStats->nodes,
                           std::format("{}: 伝導のノードのカウンタが分け方によらず同じ", plan.name));
        }
    }

    // 刻みの一時置き場が溢れたら、容量ぶんを並べて返し、残りを落とした数に数える(世界の結果は変わらない)
    void TestEventOverflow(ID3D12Device5* device, const BakedReactionTable& table, Failures& failures) {
        const std::vector<ProbeCommand> commands = MakeOverflowCommands();
        const Reference expected = RunReference(table, commands);
        save::ReplayPlayer player = MakePlayer(commands);

        const Plan plan{.name = "溢れ",
                        .options = {},
                        .unitsPerFrame = std::vector<uint32_t>(TOTAL_TICKS, PROBE_FIXED_UNITS_PER_TICK)};

        const RunResult result = RunPlan(device, table, plan, ScheduledSource(player));
        const bool sorted = rng::is_sorted(
            result.events, {}, [](const ProbeEvent& event) { return ProbeEventKey(event.type, event.place); });
        const bool allInTick = rng::all_of(result.events, [](const ProbeEvent& event) {
            return event.tick == OVERFLOW_TICK && event.type == PROBE_EVENT_POKE_APPLIED;
        });

        Log(Channel::Sim, Level::Info, "GPU(溢れ): イベント {} 落とした {}", result.events.size(),
            result.droppedEventCount);
        failures.Check(result.ok && player.LateCommands() == 0, "溢れ: 走らせられた");
        failures.Check(HashesMatch(result.hashes, expected), "溢れ: 刻みごとのハッシュが CPU と一致");
        failures.Check(result.events.size() == PROBE_TICK_EVENT_CAPACITY && sorted && allInTick,
                       "溢れ: 一時置き場の容量ぶんが並んで戻る");
        failures.Check(result.droppedEventCount == OVERFLOW_POKES - PROBE_TICK_EVENT_CAPACITY, "溢れ: 落とした数");
    }

    // 窓のクリックのように途中で届くコマンドを記録 → 再生ファイルに書いて読む → 別の分け方で再生して同じハッシュ列になる
    CommandSource ClickSource(uint32_t& sequence) {
        return [&sequence](size_t frame, uint64_t applyTick, uint32_t) {
            std::vector<ProbeCommand> commands;
            for (const ClickSpec& click : CLICKS | views::filter([&](const ClickSpec& c) { return c.frame == frame; }))
                commands.push_back(MakePokeCommand(applyTick, sequence++, click.x, click.y, PROBE_VIEW_Z));

            return commands;
        };
    }

    void TestRecordAndReplay(ID3D12Device5* device, const BakedReactionTable& table, Failures& failures) {
        uint32_t sequence = 0;
        const RunResult recorded = RunPlan(device, table, MixedPlan(), ClickSource(sequence));
        save::ReplayRecorder recorder;
        recorder.AddCommands(recorded.enqueued);
        for (const ProbeTickHash& tickHash : recorded.hashes)
            recorder.AddHash(tickHash.tick, tickHash.hash);

        const fs::path path = fs::temp_directory_path() / "bicameral_gpu_probe_sim.bcreplay";
        const auto written = recorder.Write(path);
        auto player = save::ReplayPlayer::Load(path);
        std::error_code ignored;
        fs::remove(path, ignored);
        failures.Check(recorded.ok && written.has_value() && player.has_value(),
                       "記録: 走らせて再生ファイルに書いて読めた");
        if (!player)
            return;

        const RunResult replayed = RunPlan(device, table, MixedBusyPlan(), ScheduledSource(*player));
        for (const ProbeTickHash& tickHash : replayed.hashes)
            player->CheckHash(tickHash.tick, tickHash.hash);

        Log(Channel::Sim, Level::Info, "再生(重さを分けてばらばら): 一致 {} / 不一致 {} / 全部 {}  間に合わなかった {}",
            player->Matches(), player->Mismatches(), player->HashCount(), player->LateCommands());
        failures.Check(replayed.ok && player->Passed() && player->HashCount() == TOTAL_TICKS,
                       "再生: 別の分け方で刻みごとのハッシュが記録と全部一致");
        failures.Check(HashesMatch(replayed.hashes, RunReference(table, recorder.Build().commands)),
                       "再生: 記録したコマンドの CPU リファレンスとも一致");
        failures.Check(replayed.events == recorded.events, "再生: イベントの列も記録と同じ");
    }

}  // namespace

int main(int argc, char** argv) {
    const auto options = test::ParseGpuTestOptions(std::span(argv, static_cast<size_t>(argc)));
    if (!options) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_probe_sim_test [--warp]");
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    const auto table = BakeReactionTable(MakeCombustionTestTable());
    if (!table) {
        Log(Channel::Sim, Level::Error, "試験の反応の表をベイクできない: {}", table.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    Failures failures;
    TestFramings(device->Get(), *table, failures);
    TestEventOverflow(device->Get(), *table, failures);
    TestRecordAndReplay(device->Get(), *table, failures);

    const bool passesValidation = test::PassesValidation(*device, "gpu_probe_sim_test");
    const bool passed = failures.count == 0 && passesValidation;
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_sim_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
