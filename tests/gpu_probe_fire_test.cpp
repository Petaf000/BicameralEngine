// gpu_probe_fire_test.cpp — 仮の世界(sim/probe_sim)の木箱に火をつけて長く走らせる(T-0089、02 のテストの「燃焼の場面を 10 分」)。
//
// 確かめること:
//   - 初めの --check-ticks 刻みが CPU リファレンス(ProbeReference。同じ区画・放射の段)と毎刻みビット一致(ハッシュ・エネルギー・つつき・
//     ブロックの数)し、CPU で「セル + 気体と液体の運動のぶん − 帳簿 + 放射がセルの外に持つ分」がつつきの分だけ変わる(保存。D-206)。
//     セルだけの和は見ない: 熱の放射の段(RC。T-1113)と気体の区画はエネルギーの一部をセルの外に持つ(T-1346 と同じ数え方)
//   - 最後の全部のセルを読み戻し、元素ごとの数が初めの世界と完全に同じ(区画の外の面は閉じた壁)
//   - 同じ入力でもう一度走らせ、最初の REPEAT_TICKS 刻みのハッシュ列が同じ(決定性)
//   - 木箱が燃え広がる(壁のセルの 9 割で木〔セルロース〕が 1 割以下に減る)。燃え方の経過(1 秒ごと)をログに出す
//   - 気体の段を流さなかった刻み(セルの成分の枠を超える)が 0
//   - debug layer のエラーとシェーダーの assert が 0 件
// 場面(T-1366): --flame は炎のある場面 = 試験の表に熱分解の燃える気体(D-660。tests/fuel_gas_test_table.h。CO を作らない表)を足し、
//   アプリと同じ流れる区画(表の世界の組み立て = 世界いっぱいの気体と液体。SetLayoutRegions)で走らせる。
//   木の壁(詰まった固体)どうしの面は放射の段で隠れる(伝導の仕事。T-1156)ので、壁に沿った燃え広がりは炎が運ぶ。
//   --gas は燃える気体なしで区画だけ(比べる参照)。どちらも無ければ前の場面(区画なし・燃える気体なし)。
// 走らせ方: ctest(gpu)か `job.py run -Preset release -Exe gpu_probe_fire_test`(release の方がずっと速い)。
// 引数: gpu_test_options.h(--warp は使えない。ADR-0013)、--ticks n(既定 36000)、
//   燃え方を試す値(試験の表の値を決めるため。既定は表のまま): --pokes n(火をつける回数。同じセルを刻み 0 に n 回)・
//   --solid-percent p・--gas-percent p(固体・気体の熱伝導率を試験の表の p %にする)・--flame・--gas(上の場面)・
//   --check-ticks n(CPU と突き合わせる初めの刻みの数。既定 CHECK_TICKS_DEFAULT。CPU の放射と気体の段は重い)
#include <algorithm>
#include <charconv>
#include <format>
#include <string_view>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "fuel_gas_test_table.h"
#include "gpu/com_ptr.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "reaction_test_table.h"
#include "sim/probe_gas.h"
#include "sim/probe_liquid.h"
#include "sim/probe_sim.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    constexpr uint64_t DEFAULT_TICKS = 36000;
    constexpr uint64_t REPEAT_TICKS = 3600;
    constexpr uint32_t TICKS_PER_FRAME = 60;  // 1 フレーム = 世界時間 1 秒(単位 180 個 ≤ MAX_UNITS_PER_FRAME)
    constexpr std::array<uint32_t, 3> IGNITION_CELL = {28, 32, 32};  // 木箱の壁(sim/probe_sim.cpp の初めの世界)

    // CPU と突き合わせる初めの刻み。CPU の 64³ の放射と気体の段は重い(炎の場面の爆発の刻みで 1 刻み約 4 分。T-1366)。
    // GPU の順番待ち(D-460)を長く握らないように短く
    constexpr uint64_t CHECK_TICKS_DEFAULT = 2;

    // 「燃えた」壁のセル: 木(セルロース)が初めの 1 割以下(熱分解でも燃焼でも減る。燃える気体の表は炭をほとんど残さないので炭では数えない。
    // 抽出の枠 3 をセルロースにする〔MakeTable〕)。「燃えている」: 600 K 以上
    constexpr uint32_t BURNT_REMAINING_DIVISOR = 10;
    constexpr uint32_t BURNING_MILLIKELVIN = 600000;

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition)
                return;

            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    // --- 読み戻し ---

    template <typename T>
    std::vector<T> ReadBack(ID3D12Device5* device, ID3D12Resource* source, uint64_t offsetBytes, size_t count) {
        std::vector<T> values(count);
        const uint64_t bytes = count * sizeof(T);
        auto queue = gpu::ImmediateQueue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        const ComPtr<ID3D12Resource> readback = gpu::CreateBuffer(device, bytes, gpu::BufferKind::Readback);
        if (!queue || !readback)
            return {};

        ID3D12GraphicsCommandList10* list = queue->Begin();
        if (list == nullptr)
            return {};

        list->CopyBufferRegion(readback.Get(), 0, source, offsetBytes, bytes);
        if (!queue->ExecuteAndWait() || !gpu::ReadBuffer(readback.Get(), std::as_writable_bytes(std::span(values))))
            return {};

        return values;
    }

    // --- 燃え方の経過 ---

    struct FireSnapshot {
        uint32_t burntWalls = 0;    // 木が初めの 1 割以下になった壁のセル
        uint32_t touchedWalls = 0;  // 木が減り始めた壁のセル
        uint32_t burningCells = 0;
        uint32_t maxMilliKelvin = 0;
    };

    // walls = 壁のセルの初めのセルロース(µmol。壁でなければ 0)
    FireSnapshot Summarize(std::span<const uint32_t> extraction, std::span<const uint32_t> walls) {
        FireSnapshot snapshot;
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            const uint32_t temperature = extraction[size_t{index} * PROBE_EXTRACTION_CELL_WORDS];
            const uint32_t cellulose = extraction[size_t{index} * PROBE_EXTRACTION_CELL_WORDS + 3];
            snapshot.maxMilliKelvin = std::max(snapshot.maxMilliKelvin, temperature);
            snapshot.burningCells += temperature >= BURNING_MILLIKELVIN ? 1 : 0;
            if (walls[index] == 0)
                continue;

            snapshot.burntWalls += cellulose <= walls[index] / BURNT_REMAINING_DIVISOR ? 1 : 0;
            snapshot.touchedWalls += cellulose < walls[index] ? 1 : 0;
        }

        return snapshot;
    }

    // --- 走らせる ---

    struct FireRun {
        bool ok = false;
        std::vector<ProbeTickHash> hashes;
        uint32_t debugAssertCount = 0;
        FireSnapshot last;
        uint64_t firstFullyBurntSecond = 0;  // 壁の 9 割が燃えた最初の秒(0 なら無い)
        uint64_t firstTenthBurntSecond = 0;  // 壁の 1 割が燃えた最初の秒(打ち切りの目安。0 なら無い)
        uint32_t gasStepped = 0;             // 気体の段を流した刻み(区画がある時だけ。GPU の通しの数)
        uint32_t gasSkipped = 0;             // 流さなかった刻み(欄を超えた + セルの成分の枠を超えた)
        double conductMicroseconds = 0.0;    // 伝導と反応の単位の GPU 時間の合計
        std::vector<reaction::RxCell> finalCells;
    };

    // 1 フレーム(TICKS_PER_FRAME 刻み)の読み戻しを足す。保存はここでは見ない(セルの外に持つ分がある。CheckAgainstReference)
    void AccumulateFrame(FireRun& run, const ProbeFrameReadback& readback, uint32_t unitsPerTick,
                         double usPerTimestamp) {
        for (size_t tick = 0; tick * unitsPerTick < readback.unitGpuTicks.size(); ++tick) {
            run.conductMicroseconds += static_cast<double>(
                                           readback.unitGpuTicks[tick * unitsPerTick + PROBE_UNIT_CONDUCT]) *
                                       usPerTimestamp;
        }

        run.hashes.insert(run.hashes.end(), readback.hashes.begin(), readback.hashes.end());
        run.debugAssertCount += readback.debugAssertCount;
    }

    struct FireOptions {
        uint64_t ticks = DEFAULT_TICKS;
        uint32_t pokes = 1;
        uint32_t solidPercent = 100;
        uint32_t gasPercent = 100;
        bool flame = false;  // 燃える気体 + 流れる区画(T-1366)
        bool gas = false;    // 流れる区画だけ(燃える気体なし)
        uint64_t checkTicks = CHECK_TICKS_DEFAULT;
    };

    std::vector<ProbeCommand> MakeIgnition(uint32_t pokes) {
        std::vector<ProbeCommand> ignition;
        ignition.reserve(pokes);
        for (uint32_t sequence = 0; sequence < pokes; ++sequence)
            ignition.push_back(MakePokeCommand(0, sequence, IGNITION_CELL[0], IGNITION_CELL[1], IGNITION_CELL[2]));

        return ignition;
    }

    // 区画の気体の段を流した・流さなかった刻みの数(GPU の作業場の通しの数。common/probe_gas_gpu.hlsli)
    void ReadGasCounts(ID3D12Device5* device, const ProbeSim& simulation, FireRun& run) {
        if (!simulation.HasGas())
            return;

        const uint64_t words = simulation.GasWork()->GetDesc().Width / sizeof(uint32_t);
        const std::vector<uint32_t> work = ReadBack<uint32_t>(device, simulation.GasWork(), 0, words);
        if (work.empty())
            return;

        run.gasStepped = work[PROBE_GAS_CONTROL_STEPPED / sizeof(uint32_t)];
        run.gasSkipped = work[PROBE_GAS_CONTROL_SKIPPED_TOO_MANY / sizeof(uint32_t)] +
                         work[PROBE_GAS_CONTROL_SKIPPED_OVERFLOW / sizeof(uint32_t)];
    }

    FireRun RunFire(ID3D12Device5* device, const BakedReactionTable& table, const ProbeSimOptions& simOptions,
                    uint64_t ticks, uint32_t pokes, bool logTimeline) {
        FireRun run;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"FireTestSim");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table, simOptions);
        if (!queue || !simulation) {
            Log(Channel::Sim, Level::Error, "作れない: {}{}", queue ? "" : queue.error(),
                simulation ? "" : simulation.error());
            return run;
        }

        const std::vector<reaction::RxCell> initial = MakeProbeInitialWorld(table);
        const uint32_t cellulose = table.SpeciesId("cellulose");
        std::vector<uint32_t> walls(PROBE_CELL_COUNT, 0);
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index)
            walls[index] = static_cast<uint32_t>(ProbeViewAmount(initial[index], cellulose));

        const auto wallCount = static_cast<uint32_t>(PROBE_CELL_COUNT - rng::count(walls, 0u));
        const std::vector<ProbeCommand> ignition = MakeIgnition(pokes);

        const double usPerTimestamp = 1'000'000.0 / static_cast<double>(queue->TimestampFrequency());
        const uint32_t unitsPerTick = simulation->UnitsPerTick();

        for (uint64_t tick = 0; tick < ticks; tick += TICKS_PER_FRAME) {
            const uint64_t frame = tick / TICKS_PER_FRAME;
            const auto slot = static_cast<uint32_t>(frame % ProbeSim::FRAME_SLOT_COUNT);
            const auto target = static_cast<uint32_t>(frame % PROBE_EXTRACTION_COUNT);
            const auto count = static_cast<uint32_t>(std::min<uint64_t>(TICKS_PER_FRAME, ticks - tick));
            ID3D12CommandList* list = simulation->RecordFrame(
                slot,
                {.firstTick = tick,
                 .unitCount = count * unitsPerTick,
                 .extract = true,
                 .extractionTarget = target,
                 .commands = tick == 0 ? std::span<const ProbeCommand>(ignition) : std::span<const ProbeCommand>()});

            if (list == nullptr || !queue->WaitCpu(queue->Submit(list)))
                return run;

            AccumulateFrame(run, simulation->ReadFrame(slot), unitsPerTick, usPerTimestamp);

            const std::vector<uint32_t> extraction = ReadBack<uint32_t>(device, simulation->Extraction(target), 0,
                                                                        PROBE_EXTRACTION_BLOCK_OFFSET);
            if (extraction.empty())
                return run;

            run.last = Summarize(extraction, walls);
            const uint64_t second = (tick + count) / TICKS_PER_FRAME;
            if (run.firstFullyBurntSecond == 0 && run.last.burntWalls * 10 >= wallCount * 9)
                run.firstFullyBurntSecond = second;

            if (run.firstTenthBurntSecond == 0 && run.last.burntWalls * 10 >= wallCount)
                run.firstTenthBurntSecond = second;

            const bool logThis = second <= 30 || second % 30 == 0;
            if (logTimeline && logThis) {
                Log(Channel::Sim, Level::Info,
                    "  {:>4} 秒: 燃えた壁 {:>3} / {}(減り始めた {:>3})  600 K 以上のセル {:>5}  最高 {:.0f} K  "
                    "計算したブロック {}",
                    second, run.last.burntWalls, wallCount, run.last.touchedWalls, run.last.burningCells,
                    static_cast<double>(run.last.maxMilliKelvin) / 1000.0,
                    run.hashes.empty() ? 0 : run.hashes.back().scheduledBlocks);
            }
        }

        const uint64_t generationOffset = (ticks & 1) * uint64_t{PROBE_CELL_COUNT} * sizeof(reaction::RxCell);
        run.finalCells = ReadBack<reaction::RxCell>(device, simulation->Cells(), generationOffset, PROBE_CELL_COUNT);
        ReadGasCounts(device, *simulation, run);
        run.ok = run.hashes.size() == ticks && run.finalCells.size() == PROBE_CELL_COUNT;

        return run;
    }

    std::vector<uint64_t> CountAllElements(const BakedReactionTable& table, std::span<const reaction::RxCell> cells) {
        std::vector<uint64_t> total(table.elementNames.size(), 0);
        for (const reaction::RxCell& cell : cells) {
            const std::vector<uint64_t> counts = CountElements(table, cell);
            for (size_t element = 0; element < total.size(); ++element)
                total[element] += counts[element];
        }

        return total;
    }

    // --- CPU リファレンスとの突き合わせ(T-1346 と同じ数え方。T-1366)---

    struct ReferenceCheck {
        uint64_t checkedTicks = 0;
        bool matches = false;
        bool energyConserved = false;
    };

    // 保存の単位: (セル + 気体と液体の運動〔と液体の位置〕のぶん − 帳簿)× 2^16 + 熱の放射の段がセルの外に持つ分(2^−16 mJ の mod 2^64)
    uint64_t ConservedUnits(const ProbeReference& reference, std::span<const reaction::RxCell> cells) {
        uint64_t millijoules = ProbeEnergySum(cells);
        if (const ProbeGasField* gas = reference.GasField())
            millijoules += static_cast<uint64_t>(gas->KineticEnergySum() - gas->Ledger().energy);

        if (const ProbeLiquidField* liquid = reference.LiquidField()) {
            millijoules += static_cast<uint64_t>(liquid->PotentialEnergySum() + liquid->KineticEnergySum() -
                                                 liquid->Ledger().energy);
        }

        return (millijoules << reaction::RC_ENERGY_SHIFT) + reference.CascadeRadiationUnits();
    }

    // GPU の初めの checkTicks 刻みを CPU リファレンスと比べ、CPU で保存を確かめる(セルだけの和は見ない。T-1346)
    ReferenceCheck CheckAgainstReference(const BakedReactionTable& table, const ProbeSimOptions& simOptions,
                                         const FireRun& run, uint64_t checkTicks, uint32_t pokes) {
        ReferenceCheck check;
        ProbeReference reference(table);
        if (simOptions.gas.Enabled() && !reference.EnableGas(simOptions.gas)) {
            Log(Channel::Sim, Level::Error, "CPU の気体の区画を作れない");
            return check;
        }

        if (simOptions.liquid.Enabled() && !reference.EnableLiquid(simOptions.liquid)) {
            Log(Channel::Sim, Level::Error, "CPU の液体の区画を作れない");
            return check;
        }

        const std::vector<ProbeCommand> ignition = MakeIgnition(pokes);
        check.checkedTicks = std::min<uint64_t>(checkTicks, run.hashes.size());
        check.matches = true;
        check.energyConserved = true;
        uint64_t previousUnits = ConservedUnits(reference, reference.State(0));

        for (uint64_t tick = 0; tick < check.checkedTicks; ++tick) {
            reference.Advance(tick,
                              tick == 0 ? std::span<const ProbeCommand>(ignition) : std::span<const ProbeCommand>());
            const std::span<const reaction::RxCell> state = reference.State(tick + 1);

            const ProbeTickHash& gpu = run.hashes[tick];
            const uint64_t cpuHash = reference.StateHash(tick + 1);
            const bool same = gpu.tick == tick + 1 && gpu.hash == cpuHash && gpu.energy == ProbeEnergySum(state) &&
                              gpu.sourceEnergy == reference.SourceEnergy() &&
                              gpu.scheduledBlocks == reference.ScheduledBlocks();
            if (!same && check.matches) {
                Log(Channel::Sim, Level::Error,
                    "S({}) が CPU と違う: GPU {:016x} エネルギー {} / CPU {:016x} エネルギー {}", tick + 1, gpu.hash,
                    gpu.energy, cpuHash, ProbeEnergySum(state));
            }

            check.matches = check.matches && same;

            const uint64_t nowUnits = ConservedUnits(reference, state);
            const uint64_t expectedUnits = previousUnits + (reference.SourceEnergy() << reaction::RC_ENERGY_SHIFT);
            if (nowUnits != expectedUnits && check.energyConserved) {
                Log(Channel::Sim, Level::Error, "S({}) で保存が外れた(CPU。単位の差 {})", tick + 1,
                    static_cast<int64_t>(nowUnits - expectedUnits));
            }

            check.energyConserved = check.energyConserved && nowUnits == expectedUnits;
            previousUnits = nowUnits;
        }

        return check;
    }

    // name n を取り除いて読む(無ければ value のまま。残りは gpu_test_options.h)
    template <typename T>
    void TakeNumber(std::vector<char*>& arguments, std::string_view name, T& value) {
        const auto found = rng::find_if(arguments, [name](const char* argument) { return argument == name; });
        if (found == arguments.end() || found + 1 == arguments.end())
            return;

        const std::string_view text = *(found + 1);
        std::from_chars(text.data(), text.data() + text.size(), value);
        arguments.erase(found, found + 2);
    }

    // name を取り除いて読む(あれば true)
    bool TakeFlag(std::vector<char*>& arguments, std::string_view name) {
        const auto found = rng::find_if(arguments, [name](const char* argument) { return argument == name; });
        if (found == arguments.end())
            return false;

        arguments.erase(found);

        return true;
    }

    FireOptions TakeFireOptions(std::vector<char*>& arguments) {
        FireOptions options;
        TakeNumber(arguments, "--ticks", options.ticks);
        TakeNumber(arguments, "--pokes", options.pokes);
        TakeNumber(arguments, "--solid-percent", options.solidPercent);
        TakeNumber(arguments, "--gas-percent", options.gasPercent);
        TakeNumber(arguments, "--check-ticks", options.checkTicks);
        options.flame = TakeFlag(arguments, "--flame");
        options.gas = TakeFlag(arguments, "--gas");

        return options;
    }

    // 試験の表の熱伝導率を、固体(セルロース・炭)と気体で別の % にする。flame なら熱分解の燃える気体を足す(D-660。CO を作らない表:
    // 壁のセルの成分が cellulose・炭・燃える気体・O2・N2・CO2・湯気・水の 8 = セルの成分の枠 RX_MAX_CELL_SPECIES に収まるように。T-1143 と同じ)。
    // 抽出の枠 3 はセルロース(燃えた壁を数える。描画の色分けだけで世界は変わらない)
    ReactionTableDefinition MakeTable(const FireOptions& options) {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        if (options.flame)
            test::AddFuelGasWithoutMonoxide(definition);

        definition.templates.viewQuantities[2] = ViewQuantityDefinition{
            .label = "木", .species = "cellulose", .spanMicromoles = 38600000};
        for (SpeciesDefinition& species : definition.species) {
            const bool solid = species.name == "cellulose" || species.name == "carbon";
            const uint64_t percent = solid ? options.solidPercent : options.gasPercent;
            species.thermalConductivity = static_cast<uint32_t>(uint64_t{species.thermalConductivity} * percent / 100);
        }

        return definition;
    }

}  // namespace

int main(int argc, char** argv) {
    std::vector<char*> arguments(argv, argv + argc);
    const FireOptions fire = TakeFireOptions(arguments);
    const uint64_t ticks = fire.ticks;
    const auto options = test::ParseGpuTestOptions(std::span(arguments));
    const auto table = BakeReactionTable(MakeTable(fire));
    if (!options || !table || ticks < REPEAT_TICKS) {
        Log(Channel::Sim, Level::Error,
            "使い方: gpu_probe_fire_test [--ticks n(≥ {})] [--flame | --gas] [--check-ticks n] [--pokes n]",
            REPEAT_TICKS);
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    // 流れる区画はアプリと同じ(表の世界の組み立て。T-0671)。放射の段は既定のまま(RC。ADR-0535 は仮 (a))
    // 陽解法の熱の列(T-0787)は GPU の区画にまだ無い(T-0813)ので、比べる CPU も切る(gpu_probe_gas_test と同じ。入れたままだと
    // 爆発の刻み 0 で 6 面縮めて S(1) から食い違う。T-1366 の測り)
    ProbeSimOptions simOptions;
    if (fire.flame || fire.gas) {
        SetLayoutRegions(table->worldLayout, simOptions);
        simOptions.gas.limitMovedHeat = false;
    }

    Failures failures;
    Log(Channel::Sim, Level::Info,
        "木箱の壁 ({}, {}, {}) に {} 回火をつけて {} 刻み(世界時間 {} 秒)。熱伝導率 固体 {} %・気体 {} %・場面 {}",
        IGNITION_CELL[0], IGNITION_CELL[1], IGNITION_CELL[2], fire.pokes, ticks, ticks / TICKS_PER_FRAME,
        fire.solidPercent, fire.gasPercent,
        fire.flame ? "炎(燃える気体 + 流れる区画)"
        : fire.gas ? "流れる区画だけ"
                   : "区画なし");
    const FireRun run = RunFire(device->Get(), *table, simOptions, ticks, fire.pokes, true);
    const FireRun repeat = RunFire(device->Get(), *table, simOptions, REPEAT_TICKS, fire.pokes, false);
    const ReferenceCheck reference = CheckAgainstReference(*table, simOptions, run, fire.checkTicks, fire.pokes);

    const bool repeatSame = repeat.ok && run.ok &&
                            rng::equal(repeat.hashes, std::span(run.hashes).first(REPEAT_TICKS), {},
                                       &ProbeTickHash::hash, &ProbeTickHash::hash);
    const bool elementsSame = run.ok && CountAllElements(*table, run.finalCells) ==
                                            CountAllElements(*table, MakeProbeInitialWorld(*table));

    Log(Channel::Sim, Level::Info,
        "{} 刻み: S({}) = {:016x}  壁の 1 割が燃えた {} 秒・9 割 {} 秒  伝導と反応 平均 {:.1f} µs/刻み  "
        "気体の段 流した {}・流さなかった {}  CPU と一致 {} / {} 刻み",
        ticks, ticks, run.hashes.empty() ? 0 : run.hashes.back().hash, run.firstTenthBurntSecond,
        run.firstFullyBurntSecond, run.conductMicroseconds / static_cast<double>(std::max<uint64_t>(ticks, 1)),
        run.gasStepped, run.gasSkipped, reference.matches ? reference.checkedTicks : 0, reference.checkedTicks);

    failures.Check(run.ok, "走らせられた");
    failures.Check(reference.checkedTicks == 0 || reference.matches,
                   std::format("初めの {} 刻みが CPU リファレンスと毎刻みビット一致", reference.checkedTicks));
    failures.Check(reference.checkedTicks == 0 || reference.energyConserved,
                   "CPU で保存(セル + 気体と液体のぶん − 帳簿 + 放射がセルの外に持つ分 = 前 + つつき)");
    failures.Check(run.gasSkipped == 0, "気体の段を流さなかった刻みが 0");
    failures.Check(elementsSame, "元素ごとの数が初めの世界と同じ");
    failures.Check(repeatSame, std::format("もう一度走らせて最初の {} 刻みのハッシュ列が同じ", REPEAT_TICKS));
    failures.Check(run.firstFullyBurntSecond > 0, "木箱が燃え広がる(壁の 9 割が炭になる)");
    failures.Check(run.debugAssertCount == 0 && repeat.debugAssertCount == 0, "シェーダーの assert が 0 件");

    const bool passed = failures.count == 0 && test::PassesValidation(*device, "gpu_probe_fire_test");
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_fire_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
