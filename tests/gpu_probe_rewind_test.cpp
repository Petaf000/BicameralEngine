// gpu_probe_rewind_test.cpp — 巻き戻し(保存点 + 再生。T-0143・ADR-0036)の決定性。
//
// 確かめること(仮の世界 + 積み木。最初のフレームで、つつき・押すコマンドを全部 GPU のキューへ足す):
//   - 基準: 別の ProbeSim で最初から TICKS 刻み流した、刻みごとの世界のハッシュ(セル + 物)
//   - 巻き戻す側: 刻み 20・60・100 の境界で保存点へ写しながら 120 刻みまで進み、
//       1. 保存点(刻み 20)へ戻し、刻み 20 以降のコマンドを足し直して TICKS まで → S(21)〜S(TICKS) が基準と全部一致
//          (戻すときにキューで待っていたコマンドを捨てられること。戻した後も 60・100 で写し直す)
//       2. 保存点(刻み 100)へ戻して TICKS まで → 一致(巻き戻した後にもう一度巻き戻す)
//       3. 保存点(刻み 60)へ戻し、コマンドを足し直さずに進める → 刻み 70 のつつきの後で基準と違う(試験が効いていることの確かめ)
//   - 反応表の差し替え(T-0299・T-0548・ADR-0246): 同じコマンドで刻み 40 に木の燃焼の速さを 100 倍にした表へ替える。
//       基準(差し替えあり)を別の ProbeSim で流し、写しながら 120 刻み進む側は、保存点を全部残す(保存点はその境界の表も持つ)。
//       差し替えの後の保存点(刻み 60)へ戻して進めると S(61)〜S(TICKS) が基準と全部一致し(戻した後の世界 = その時の表で刻んだ世界)、
//       差し替えより前の保存点(刻み 20)へ戻すと表も前の表に戻り、刻み 40 で差し替え直して S(21)〜S(TICKS) が基準と全部一致する。
//       差し替えなしの基準とは違う(差し替えが効いている)
//   - 刻み 0 の保存点(T-0411・D-474): 単位の無いリストで世界を初期化だけし、次の単位の無いリストで S(0) を写して 120 刻み進む。
//       刻み 0 へ戻してコマンドを全部足し直すと S(1)〜S(TICKS) が基準と全部一致し、足し直さないと最後の刻みで違う
//       (刻み 0 のつつきも戻すとキューから捨てられ、足し直した分だけが効く。起動直後の筆の運びを元に戻す道の土台)
//   - フレームの単位の数はばらばら(刻みの途中で切れる)。保存点の刻みでだけ、境界で切ってから写す
//   - シェーダーの assert と debug layer のエラーが 0 件
//   - --regions(T-0620): 同じことを、気体の区画(木箱を囲む 16 × 14 × 16。基準の大気で満たす)と液体の区画(ダムの水が押し寄せる
//       12 × 8 × 3)のある世界で。区画の持ち越す欄(気体の運動量・液体の面の運動量・位置と運動のエネルギー・帳簿)も保存点が写し、
//       状態のハッシュに入る(入っていなければ、戻した後に区画の欄だけ違っても気づけない)。差し替える表は反応の速さだけが違う
//       (区画のある世界で通る差し替え。区画の欄の作りが変わる表は断る = CheckRegionTableSwap も確かめる)。
//       写しの早送り(sim/world_copy。T-0412): 刻み COPY_TICK の写しに同じコマンドを当てて TICKS まで → 基準と全部一致
//       (区画の欄も写る)
// 保存点 1 つの大きさと、写す・戻すフレームの GPU の時間をログに出す(docs/perf.md)。
// 引数: gpu_test_options.h(--warp)・--regions
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu/queue.h"
#include "gpu_test_options.h"
#include "reaction_test_table.h"
#include "sim/probe_gas.h"
#include "sim/probe_sim.h"
#include "sim/world_copy.h"
#include "sim/world_physics.h"
#include "standard_world_physics.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    constexpr uint64_t TICKS = 140;
    constexpr uint64_t FIRST_LEG_TICKS = 120;                      // 巻き戻す前に進む刻み
    constexpr std::array<uint64_t, 3> SAVE_TICKS = {20, 60, 100};  // 保存点 i に写す刻み
    constexpr uint64_t DIVERGING_POKE_TICK = 70;  // 足し直さないと違いが出る最初のコマンド(保存点 1 の後)
    constexpr std::array<uint32_t, 5> FRAME_UNITS = {3, 7, 5, 11, 4};
    constexpr uint64_t FAR_TICK = uint64_t{1} << 40;  // 保存点の無い時の「次の境界」
    constexpr uint64_t SWAP_TICK = 40;                // 反応表を差し替える刻み(保存点 0 と 1 の間。T-0299)

    constexpr int64_t METER = int64_t{1} << 20;
    constexpr int32_t UNIT = 1 << 30;                 // Q1.30 の 1
    constexpr uint32_t PUSH_IMPULSE_MNS = 5'000'000;  // 5000 N·s

    // 刻みの順・番号の順(GPU のキューの約束)
    std::vector<ProbeCommand> MakeCommands() {
        return {
            MakePokeCommand(0, 0, 28, 32, 32),  // 木箱の壁に火をつける
            MakePushCommand(30, 1, {10 * METER, 11 * METER / 2, 6 * METER}, {0, 0, UNIT}, PUSH_IMPULSE_MNS),
            MakePokeCommand(45, 2, 36, 32, 32),
            MakePokeCommand(DIVERGING_POKE_TICK, 3, 20, 40, 24),
            MakePushCommand(90, 4, {10 * METER + 3 * METER / 10, 44 * METER / 5, 26 * METER}, {0, 0, -UNIT},
                            PUSH_IMPULSE_MNS),
            MakePokeCommand(110, 5, 32, 20, 40),
        };
    }

    std::vector<ProbeCommand> CommandsFrom(uint64_t tick) {
        std::vector<ProbeCommand> commands = MakeCommands();
        std::erase_if(commands, [&](const ProbeCommand& command) { return command.targetTick < tick; });

        return commands;
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

    // 1 つの ProbeSim を 1 フレームずつ投げて待つ(並べない。巻き戻しの正しさだけを見る)
    class Driver {
    public:
        Driver(gpu::Queue& queue, ProbeSim& simulation) : m_queue(queue), m_simulation(simulation) {}

        // 今の境界(m_tick)から刻み to の境界まで。first の最初のフレームで restoreFrom から戻し、commands を足す。
        // 保存点の刻みに来たら、そのフレームを境界で切ってから次のフレームの先頭で写す(save = true のとき)
        bool Advance(uint64_t to, std::span<const ProbeCommand> commands, uint32_t restoreFrom, bool save) {
            const uint32_t unitsPerTick = m_simulation.UnitsPerTick();
            const uint64_t end = to * unitsPerTick;
            bool first = true;

            while (m_position < end) {
                const uint64_t tick = m_position / unitsPerTick;
                const auto unit = static_cast<uint32_t>(m_position % unitsPerTick);
                const uint32_t saveIndex = save && unit == 0 && !first ? SaveIndex(tick) : NO_SAVE_POINT;
                uint64_t units = std::min<uint64_t>(FRAME_UNITS[m_frame % FRAME_UNITS.size()], end - m_position);
                units = std::min(units, NextSaveBoundary(tick, save) * unitsPerTick - m_position);

                const uint64_t swapUnit = m_swap.tick * unitsPerTick;
                const bool swapHere = m_swap.table != nullptr && swapUnit >= m_position &&
                                      swapUnit < m_position + units;

                const ProbeFrameInput input{.firstTick = tick,
                                            .firstUnit = unit,
                                            .unitCount = static_cast<uint32_t>(units),
                                            .commands = first ? commands : std::span<const ProbeCommand>(),
                                            .saveTo = saveIndex,
                                            .restoreFrom = first ? restoreFrom : NO_SAVE_POINT,
                                            .tableSwaps = swapHere ? std::span<const ProbeTableSwap>(&m_swap, 1)
                                                                   : std::span<const ProbeTableSwap>()};
                if (!Submit(input, saveIndex != NO_SAVE_POINT || input.restoreFrom != NO_SAVE_POINT))
                    return false;

                m_position += units;
                first = false;
            }

            return true;
        }

        // 保存点へ戻す前に、境界を保存点の刻みに合わせる
        void JumpTo(uint64_t tick) { m_position = tick * m_simulation.UnitsPerTick(); }

        // 単位の無いリスト。最初の 1 つは世界を初期化するだけ。saveTo を渡せば、今の境界の状態を写すだけ(刻み 0 の保存点。T-0411)
        bool SubmitEmpty(uint32_t saveTo) {
            const uint32_t unitsPerTick = m_simulation.UnitsPerTick();
            const ProbeFrameInput input{.firstTick = m_position / unitsPerTick,
                                        .firstUnit = static_cast<uint32_t>(m_position % unitsPerTick),
                                        .unitCount = 0,
                                        .saveTo = saveTo};

            return Submit(input, false);
        }

        // 刻み swap.tick の適用の単位を投げるフレームで、表を差し替える(T-0299)
        void SetTableSwap(const ProbeTableSwap& swap) { m_swap = swap; }

        // 集めたハッシュを取り出して空にする(刻み → 世界のハッシュ)
        std::map<uint64_t, uint64_t> TakeHashes() { return std::exchange(m_hashes, {}); }

        [[nodiscard]] uint32_t DebugAssertCount() const { return m_debugAssertCount; }
        [[nodiscard]] double SaveRestoreMicroseconds() const { return m_saveRestoreMicroseconds; }
        [[nodiscard]] uint32_t SaveRestoreFrames() const { return m_saveRestoreFrames; }

    private:
        static uint32_t SaveIndex(uint64_t tick) {
            const auto found = rng::find(SAVE_TICKS, tick);
            return found == SAVE_TICKS.end() ? NO_SAVE_POINT : static_cast<uint32_t>(found - SAVE_TICKS.begin());
        }

        // 刻み tick の途中(か始め)より後の、最初の保存点の刻み(無ければ十分先)。始めにいる保存点はこのフレームの先頭で写す
        static uint64_t NextSaveBoundary(uint64_t tick, bool save) {
            if (save) {
                for (const uint64_t saveTick : SAVE_TICKS) {
                    if (saveTick > tick)
                        return saveTick;
                }
            }

            return FAR_TICK;
        }

        bool Submit(const ProbeFrameInput& input, bool measure) {
            ID3D12CommandList* list = m_simulation.RecordFrame(0, input);
            if (list == nullptr)
                return false;

            const uint64_t fence = m_queue.Submit(list);
            if (!m_queue.WaitCpu(fence))
                return false;

            const ProbeFrameReadback readback = m_simulation.ReadFrame(0);
            for (const ProbeTickHash& hash : readback.hashes)
                m_hashes[hash.tick] = hash.WorldHash();

            m_debugAssertCount += readback.debugAssertCount;
            ++m_frame;
            if (measure) {
                // 写す・戻すフレーム全体(コピー + 単位)の時間。単位の時間を引いて写す・戻すだけの目安にする
                uint64_t unitTicks = 0;
                for (const uint64_t ticks : readback.unitGpuTicks)
                    unitTicks += ticks;

                const uint64_t total = readback.gpuEndTimestamp - readback.gpuBeginTimestamp;
                m_saveRestoreMicroseconds += static_cast<double>(total - std::min(total, unitTicks)) * 1'000'000.0 /
                                             static_cast<double>(m_queue.TimestampFrequency());
                ++m_saveRestoreFrames;
            }

            return true;
        }

        gpu::Queue& m_queue;
        ProbeSim& m_simulation;
        uint64_t m_position = 0;  // 投げた単位の数(刻み × 単位の数 + 単位)
        uint64_t m_frame = 0;
        ProbeTableSwap m_swap;  // table が nullptr なら差し替えない
        std::map<uint64_t, uint64_t> m_hashes;
        uint32_t m_debugAssertCount = 0;
        double m_saveRestoreMicroseconds = 0.0;
        uint32_t m_saveRestoreFrames = 0;
    };

    // segment の刻みが全部 baseline と一致し、from + 1 〜 to が揃っているか
    bool MatchesBaseline(const std::map<uint64_t, uint64_t>& segment, const std::map<uint64_t, uint64_t>& baseline,
                         uint64_t from, uint64_t to, std::string_view label) {
        uint32_t mismatches = 0;
        uint64_t firstMismatch = UINT64_MAX;
        for (uint64_t tick = from + 1; tick <= to; ++tick) {
            const auto got = segment.find(tick);
            const auto expected = baseline.find(tick);
            if (got != segment.end() && expected != baseline.end() && got->second == expected->second)
                continue;

            ++mismatches;
            firstMismatch = std::min(firstMismatch, tick);
        }

        Log(Channel::Sim, Level::Info, "{}: S({})〜S({}) のうち一致しない刻み {}(最初 {})", label, from + 1, to,
            mismatches, firstMismatch == UINT64_MAX ? 0 : firstMismatch);

        return mismatches == 0;
    }

    struct Created {
        std::unique_ptr<gpu::Queue> queue;
        std::unique_ptr<ProbeSim> simulation;
    };

    // 試す世界(区画なし = 仮の世界 + 積み木。--regions なら気体と液体の区画を足す)
    struct Scenario {
        BakedReactionTable table;
        BakedReactionTable swapped;           // 刻み SWAP_TICK に差し替える表(木の燃焼の速さ 100 倍)
        std::vector<reaction::RxCell> world;  // 空なら表の世界の組み立て
        PhysicsScene scene;
        ProbeGasOptions gas;
        ProbeLiquidOptions liquid;
    };

    std::optional<Created> Create(ID3D12Device5* device, const Scenario& scenario, uint32_t savePoints) {
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"RewindTestSim");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, scenario.table,
                                           {.physicsScene = &scenario.scene,
                                            .physicsParameters = PhysicsParametersFor(StandardWorldPhysics()),
                                            .initialWorld = scenario.world,
                                            .gas = scenario.gas,
                                            .liquid = scenario.liquid});
        if (!queue || !simulation) {
            Log(Channel::Sim, Level::Error, "作れない: {}{}", queue ? "" : queue.error(),
                simulation ? "" : simulation.error());
            return std::nullopt;
        }

        if (savePoints > 0 && !simulation->CreateSavePoints(device, savePoints))
            return std::nullopt;

        return Created{.queue = std::make_unique<gpu::Queue>(std::move(*queue)),
                       .simulation = std::make_unique<ProbeSim>(std::move(*simulation))};
    }

    // 木の燃焼の速さを 100 倍にした定義(物質の一覧は同じ。gpu_probe_sim_test の差し替えと同じ作り方)
    ReactionTableDefinition SpeedUpCombustion(ReactionTableDefinition definition) {
        for (RuleDefinition& rule : definition.rules) {
            if (rule.name == "cellulose_combustion")
                rule.rate.preExponentialMantissa *= 100;
        }

        return definition;
    }

    // --- 区画のある世界(--regions。T-0620)---
    constexpr std::array<uint32_t, 3> GAS_MIN = {24, 26, 24};  // 木箱(つつく 28・36, 32, 32)を囲む
    constexpr std::array<uint32_t, 3> GAS_SIZE = {16, 14, 16};
    constexpr std::array<uint32_t, 3> LIQUID_MIN = {44, 0, 44};  // 積み木(x 20 前後)から離す
    constexpr std::array<uint32_t, 3> LIQUID_SIZE = {12, 8, 3};
    constexpr uint32_t DAM_WATER_END = 5;  // 水は区画の x 0〜4・y 1〜4(y 0 は石の床)
    constexpr uint32_t DAM_WATER_TOP = 4;
    constexpr uint64_t FULL_WATER = 6900000000;  // µmol(gpu_probe_liquid_test と同じ)
    constexpr uint64_t NITROGEN = 4000000;
    constexpr uint64_t OXYGEN = 1000000;
    constexpr uint64_t STONE = 21000000000;
    constexpr int32_t TEMPERATURE = 300000;

    // 燃焼の表 + 地球の環境の値 + 基準の大気(N2 79%・O2 21%)+ 水と石(gpu_probe_gas_test・gpu_probe_liquid_test と同じ)
    ReactionTableDefinition MakeRegionDefinition() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.environment = StandardWorldPhysics();
        definition.atmosphere = {
            .points = {{.heightMillimeters = 0,
                        .temperatureMilliKelvin = (uint32_t)TEMPERATURE,
                        .fractions = {{.species = "nitrogen", .partsPerMillion = 790000},
                                      {.species = "oxygen", .partsPerMillion = 210000}}}},
            .surfacePressurePascals = 101325,
        };
        definition.species.push_back({.name = "liquid_water",
                                      .composition = {{.element = "H", .count = 2}, {.element = "O", .count = 1}},
                                      .formationEnthalpy = -285830,
                                      .heatCapacity = 75300,
                                      .thermalConductivity = 600,
                                      .liquid = true,
                                      .density = 1000});
        definition.species.push_back({.name = "stone",
                                      .composition = {{.element = "C", .count = 1}},
                                      .formationEnthalpy = 0,
                                      .heatCapacity = 8500,
                                      .thermalConductivity = 2000,
                                      .condensed = true,
                                      .density = 2000});

        return definition;
    }

    bool HasCondensed(const BakedReactionTable& table, const reaction::RxCell& cell) {
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            if (table.speciesCondensed[cell.species[i]] != 0)
                return true;
        }

        return false;
    }

    // 気体の区画の凝縮相の無いセルを基準の大気に(木箱はそのまま)、液体の区画をダムの場面に
    std::vector<reaction::RxCell> MakeRegionWorld(const BakedReactionTable& table, const ProbeGasField& gas,
                                                  const ProbeLiquidOptions& liquid) {
        std::vector<reaction::RxCell> cells = MakeProbeInitialWorld(table);
        for (uint32_t region = 0; region < gas.RegionCellCount(); ++region) {
            const uint32_t index = gas.GridIndex(region);
            if (!HasCondensed(table, cells[index]))
                cells[index] = gas.ReferenceCell(table, (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE);
        }

        for (uint32_t y = 0; y < liquid.sizeCells[1]; ++y) {
            for (uint32_t z = 0; z < liquid.sizeCells[2]; ++z) {
                for (uint32_t x = 0; x < liquid.sizeCells[0]; ++x) {
                    std::vector<SpeciesAmount> amounts = {{.species = table.SpeciesId("nitrogen"), .amount = NITROGEN},
                                                          {.species = table.SpeciesId("oxygen"), .amount = OXYGEN}};
                    if (y == 0)
                        amounts = {{.species = table.SpeciesId("stone"), .amount = STONE}};
                    else if (x < DAM_WATER_END && y <= DAM_WATER_TOP)
                        amounts.push_back({.species = table.SpeciesId("liquid_water"), .amount = FULL_WATER});

                    std::ranges::sort(amounts, {}, &SpeciesAmount::species);
                    const uint32_t index = ProbeCellIndex(liquid.minCell[0] + x, liquid.minCell[1] + y,
                                                          liquid.minCell[2] + z);
                    cells[index] = MakeReactionCell(table, amounts, TEMPERATURE);
                }
            }
        }

        return cells;
    }

    std::optional<Scenario> MakeScenario(bool regions) {
        const ReactionTableDefinition definition = regions ? MakeRegionDefinition() : MakeCombustionTestTable();
        auto table = BakeReactionTable(definition);
        auto swapped = BakeReactionTable(SpeedUpCombustion(definition));
        if (!table || !swapped)
            return std::nullopt;

        Scenario scenario{.table = std::move(*table), .swapped = std::move(*swapped)};
        scenario.scene = MakeLayoutPhysicsScene(scenario.table.worldLayout);
        if (!regions)
            return scenario;

        scenario.gas = {.minCell = GAS_MIN, .sizeCells = GAS_SIZE, .randomSeed = 12345};
        scenario.liquid = {.minCell = LIQUID_MIN, .sizeCells = LIQUID_SIZE, .advection = true};
        const auto gas = ProbeGasField::Create(scenario.table, scenario.gas);
        if (!gas) {
            Log(Channel::Sim, Level::Error, "気体の区画を作れない: {}", gas.error());
            return std::nullopt;
        }

        scenario.world = MakeRegionWorld(scenario.table, *gas, scenario.liquid);

        return scenario;
    }

    // 反応表の差し替えと巻き戻し(T-0299)。unswapped は差し替えなしの基準
    void RunTableSwapTest(ID3D12Device5* device, const Scenario& scenario,
                          const std::map<uint64_t, uint64_t>& unswapped, Failures& failures) {
        std::optional<Created> base = Create(device, scenario, 0);
        std::optional<Created> rewind = Create(device, scenario, static_cast<uint32_t>(SAVE_TICKS.size()));
        failures.Check(base && rewind, "差し替え: 作れた");
        if (!base || !rewind)
            return;

        const std::vector<ProbeCommand> commands = MakeCommands();
        const ProbeTableSwap swap{.tick = SWAP_TICK, .table = &scenario.swapped};

        // --- 基準(差し替えあり)---
        Driver baseDriver(*base->queue, *base->simulation);
        baseDriver.SetTableSwap(swap);
        failures.Check(baseDriver.Advance(TICKS, commands, NO_SAVE_POINT, false), "差し替え: 基準を流せた");
        const std::map<uint64_t, uint64_t> baseline = baseDriver.TakeHashes();
        failures.Check(MatchesBaseline(baseline, unswapped, 0, SWAP_TICK - 1, "差し替え: 差し替えの前"),
                       "差し替え: 差し替えの刻みの前までは、差し替えなしと同じ");
        failures.Check(
            baseline.contains(TICKS) && unswapped.contains(TICKS) && baseline.at(TICKS) != unswapped.at(TICKS),
            "差し替え: 差し替えが効いている(差し替えなしと最後の刻みで違う)");

        // --- 写しながら進む(差し替えても保存点は残る)---
        Driver driver(*rewind->queue, *rewind->simulation);
        driver.SetTableSwap(swap);
        ProbeSim& simulation = *rewind->simulation;
        failures.Check(driver.Advance(FIRST_LEG_TICKS, commands, NO_SAVE_POINT, true), "差し替え: 写しながら進めた");
        failures.Check(MatchesBaseline(driver.TakeHashes(), baseline, 0, FIRST_LEG_TICKS, "差し替え: 写しながら"),
                       "差し替え: 保存点を写しても基準と同じ");
        Log(Channel::Sim, Level::Info, "差し替え(刻み {})の後の保存点の刻み: {} {} {}(空 = {})", SWAP_TICK,
            simulation.SavePointTick(0), simulation.SavePointTick(1), simulation.SavePointTick(2), UINT64_MAX);
        failures.Check(simulation.SavePointTick(0) == SAVE_TICKS[0] && simulation.SavePointTick(1) == SAVE_TICKS[1] &&
                           simulation.SavePointTick(2) == SAVE_TICKS[2],
                       "差し替え: 差し替えより前(刻み 20)も後(刻み 60・100)も保存点は残る");
        failures.Check(simulation.SavePointChangesTable(0) && !simulation.SavePointChangesTable(1),
                       "差し替え: 刻み 20 の保存点は前の表・刻み 60 の保存点は今の表");

        // --- 差し替えの後の保存点へ戻す → その時の表で刻んだ世界と同じ ---
        driver.JumpTo(SAVE_TICKS[1]);
        failures.Check(driver.Advance(TICKS, CommandsFrom(SAVE_TICKS[1]), 1, false),
                       "差し替え: 刻み 60 へ戻して進めた");
        failures.Check(MatchesBaseline(driver.TakeHashes(), baseline, SAVE_TICKS[1], TICKS, "差し替え: 刻み 60 から"),
                       "差し替え: 差し替えの後の保存点へ戻した世界が、差し替えた表で最初から流した世界と同じ");

        // --- 差し替えより前の保存点へ戻す → 表も前の表に戻り、刻み 40 で差し替え直して同じ(T-0548)---
        driver.JumpTo(SAVE_TICKS[0]);
        failures.Check(driver.Advance(TICKS, CommandsFrom(SAVE_TICKS[0]), 0, false),
                       "差し替え: 刻み 20 へ戻して進めた");
        failures.Check(MatchesBaseline(driver.TakeHashes(), baseline, SAVE_TICKS[0], TICKS, "差し替え: 刻み 20 から"),
                       "差し替え: 差し替えより前の保存点へ戻して差し替え直した世界が、最初から流した世界と同じ");

        failures.Check(baseDriver.DebugAssertCount() == 0 && driver.DebugAssertCount() == 0,
                       "差し替え: シェーダーの assert が 0 件");
    }

    // 刻み 0 の保存点(T-0411・D-474)。baseline は差し替えなしの基準
    void RunTickZeroTest(ID3D12Device5* device, const Scenario& scenario, const std::map<uint64_t, uint64_t>& baseline,
                         Failures& failures) {
        std::optional<Created> rewind = Create(device, scenario, 1);
        failures.Check(rewind.has_value(), "刻み 0: 作れた");
        if (!rewind)
            return;

        // --- 初期化だけのリスト → 刻み 0 の境界で写す → 写さずに進む ---
        const std::vector<ProbeCommand> commands = MakeCommands();
        Driver driver(*rewind->queue, *rewind->simulation);
        failures.Check(driver.SubmitEmpty(NO_SAVE_POINT), "刻み 0: 初期化だけのリストを投げた");
        failures.Check(driver.SubmitEmpty(0), "刻み 0: 刻み 0 の境界で写した");
        failures.Check(rewind->simulation->SavePointTick(0) == 0, "刻み 0: 保存点が刻み 0 を持つ");
        failures.Check(driver.Advance(FIRST_LEG_TICKS, commands, NO_SAVE_POINT, false), "刻み 0: 写した後に進めた");
        failures.Check(MatchesBaseline(driver.TakeHashes(), baseline, 0, FIRST_LEG_TICKS, "刻み 0: 写した後"),
                       "刻み 0: 初期化だけのリストと写すリストを挟んでも世界のハッシュ列が変わらない");

        // --- 刻み 0 へ戻して全部足し直す → 基準と同じ ---
        driver.JumpTo(0);
        failures.Check(driver.Advance(TICKS, commands, 0, false), "刻み 0: 刻み 0 へ戻して進めた");
        failures.Check(MatchesBaseline(driver.TakeHashes(), baseline, 0, TICKS, "刻み 0 から"),
                       "刻み 0: 刻み 0 へ巻き戻した後のハッシュ列が、最初から流した時と同じ");

        // --- 刻み 0 へ戻して足し直さない → 違う(刻み 0 のつつきもキューから捨てられた)---
        driver.JumpTo(0);
        failures.Check(driver.Advance(TICKS, {}, 0, false), "刻み 0: 刻み 0 へ戻して(コマンドなしで)進めた");
        const std::map<uint64_t, uint64_t> withoutCommands = driver.TakeHashes();
        failures.Check(withoutCommands.contains(TICKS) && withoutCommands.at(TICKS) != baseline.at(TICKS),
                       "刻み 0: 足し直さないと最後の刻みで違う");
        failures.Check(driver.DebugAssertCount() == 0, "刻み 0: シェーダーの assert が 0 件");
    }

    // 写しの早送り(T-0412・T-0620): 本物を COPY_TICK まで流し、その境界の写しに同じコマンドを当てて TICKS まで早送りする
    void RunCopyTest(ID3D12Device5* device, const Scenario& scenario, const std::map<uint64_t, uint64_t>& baseline,
                     Failures& failures) {
        constexpr uint64_t COPY_TICK = 60;
        std::optional<Created> real = Create(device, scenario, 0);
        auto copy = real ? WorldCopy::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, real->simulation->Table(),
                                             {.physicsScene = &scenario.scene,
                                              .physicsParameters = PhysicsParametersFor(StandardWorldPhysics()),
                                              .gas = scenario.gas,
                                              .liquid = scenario.liquid})
                         : std::unexpected(std::string("本物を作れない"));
        failures.Check(real && copy, "写し: 作れた");
        if (!real || !copy)
            return;

        gpu::Queue& queue = *real->queue;
        const auto run = [&](const std::vector<ID3D12CommandList*>& lists) {
            const bool ran = lists.empty() || queue.WaitCpu(queue.Submit(lists));
            copy->Complete();

            return ran;
        };

        Driver driver(queue, *real->simulation);
        failures.Check(run(copy->RecordInitialization()), "写し: 写しの世界を初期化した");
        failures.Check(driver.Advance(COPY_TICK, MakeCommands(), NO_SAVE_POINT, false), "写し: 本物を流した");
        failures.Check(run(copy->RecordStart(*real->simulation, COPY_TICK,
                                             {.maxTicks = TICKS - COPY_TICK, .commands = CommandsFrom(COPY_TICK)})),
                       "写し: 写しを取った");
        for (uint32_t frame = 0; frame < 100 && !copy->Finished(); ++frame)
            failures.Check(run(copy->RecordNext(TICKS)), "写し: 早送りした");

        std::map<uint64_t, uint64_t> hashes;
        for (const ProbeTickHash& hash : copy->Result().hashes)
            hashes[hash.tick] = hash.WorldHash();

        failures.Check(copy->Finished() && !copy->Result().failed &&
                           MatchesBaseline(hashes, baseline, COPY_TICK, TICKS, "写し: 早送り"),
                       "写し: 写しに同じコマンドを当てた早送りが、本物を最初から流した世界と同じ");
        failures.Check(driver.DebugAssertCount() == 0 && copy->Result().debugAssertCount == 0,
                       "写し: シェーダーの assert が 0 件");
    }

    // 区画のある世界の表の差し替えの約束(T-0620・ADR-0300): 速さだけ違う表は通り、区画が読む物質の値が違う表は断る
    void CheckRegionTableSwaps(const Scenario& scenario, const ProbeSim& simulation, Failures& failures) {
        failures.Check(simulation.CheckRegionTableSwap(scenario.swapped).has_value(),
                       "区画: 反応の速さだけが違う表へは差し替えられる");

        ReactionTableDefinition denser = MakeRegionDefinition();
        for (SpeciesDefinition& species : denser.species) {
            if (species.name == "liquid_water")
                species.density = 1100;
        }

        auto table = BakeReactionTable(denser);
        const auto refused = table ? simulation.CheckRegionTableSwap(*table) : std::expected<void, std::string>();
        failures.Check(table && !refused.has_value(), "区画: 液体の値(密度)が違う表は断る");
        if (table && !refused)
            Log(Channel::Sim, Level::Info, "断った理由: {}", refused.error());
    }

    int RunTest(ID3D12Device5* device, const Scenario& scenario) {
        const std::vector<ProbeCommand> commands = MakeCommands();
        std::optional<Created> base = Create(device, scenario, 0);
        std::optional<Created> rewind = Create(device, scenario, static_cast<uint32_t>(SAVE_TICKS.size()));
        Failures failures;
        failures.Check(base && rewind, "作れた");
        if (!base || !rewind)
            return failures.count;

        Log(Channel::Sim, Level::Info, "保存点 1 つ: {:.1f} MiB(物 {}・気体の区画 {}・液体の区画 {})",
            static_cast<double>(rewind->simulation->SavePointBytes()) / (1024.0 * 1024.0), scenario.scene.bodies.size(),
            rewind->simulation->HasGas(), rewind->simulation->HasLiquid());
        if (rewind->simulation->HasGas())
            CheckRegionTableSwaps(scenario, *rewind->simulation, failures);

        // --- 基準 ---
        Driver baseDriver(*base->queue, *base->simulation);
        failures.Check(baseDriver.Advance(TICKS, commands, NO_SAVE_POINT, false), "基準を流せた");
        const std::map<uint64_t, uint64_t> baseline = baseDriver.TakeHashes();

        // --- 写しながら進む → 戻す ---
        Driver driver(*rewind->queue, *rewind->simulation);
        ProbeSim& simulation = *rewind->simulation;
        failures.Check(driver.Advance(FIRST_LEG_TICKS, commands, NO_SAVE_POINT, true), "保存点を写しながら進めた");
        failures.Check(MatchesBaseline(driver.TakeHashes(), baseline, 0, FIRST_LEG_TICKS, "写しながら"),
                       "保存点を写しても世界のハッシュ列が変わらない");
        for (uint32_t index = 0; index < SAVE_TICKS.size(); ++index)
            failures.Check(simulation.SavePointTick(index) == SAVE_TICKS[index], "保存点が決まった刻みを持つ");

        // 1. 刻み 20 へ(キューには 30 以降のコマンドが待っている)
        driver.JumpTo(SAVE_TICKS[0]);
        const std::vector<ProbeCommand> from20 = CommandsFrom(SAVE_TICKS[0]);
        failures.Check(driver.Advance(TICKS, from20, 0, true), "刻み 20 へ戻して進めた");
        failures.Check(MatchesBaseline(driver.TakeHashes(), baseline, SAVE_TICKS[0], TICKS, "刻み 20 から"),
                       "刻み 20 へ巻き戻した後のハッシュ列が、最初から流した時と同じ");

        // 2. 刻み 100 へ(戻した後に写し直した保存点)
        driver.JumpTo(SAVE_TICKS[2]);
        failures.Check(driver.Advance(TICKS, CommandsFrom(SAVE_TICKS[2]), 2, false), "刻み 100 へ戻して進めた");
        failures.Check(MatchesBaseline(driver.TakeHashes(), baseline, SAVE_TICKS[2], TICKS, "刻み 100 から"),
                       "もう一度巻き戻した後のハッシュ列も同じ");

        // 3. 刻み 60 へ戻し、コマンドを足し直さない → つつきの刻みの後で違う
        driver.JumpTo(SAVE_TICKS[1]);
        failures.Check(driver.Advance(TICKS, {}, 1, false), "刻み 60 へ戻して(コマンドなしで)進めた");
        const std::map<uint64_t, uint64_t> withoutCommands = driver.TakeHashes();
        failures.Check(MatchesBaseline(withoutCommands, baseline, SAVE_TICKS[1], DIVERGING_POKE_TICK, "足し直さず(前)"),
                       "足し直さなくても、つつきの刻みまでは同じ");
        failures.Check(withoutCommands.contains(DIVERGING_POKE_TICK + 1) &&
                           withoutCommands.at(DIVERGING_POKE_TICK + 1) != baseline.at(DIVERGING_POKE_TICK + 1),
                       "足し直さないと、つつきの後で違う(戻すとキューのコマンドが捨てられている)");

        failures.Check(baseDriver.DebugAssertCount() == 0 && driver.DebugAssertCount() == 0,
                       "シェーダーの assert が 0 件");
        RunTableSwapTest(device, scenario, baseline, failures);
        RunTickZeroTest(device, scenario, baseline, failures);
        RunCopyTest(device, scenario, baseline, failures);
        Log(Channel::Sim, Level::Info, "写す・戻すフレームの単位以外の GPU 時間: 平均 {:.0f} µs({} フレーム。暖機なし)",
            driver.SaveRestoreMicroseconds() / std::max(1u, driver.SaveRestoreFrames()), driver.SaveRestoreFrames());

        return failures.count;
    }

}  // namespace

int main(int argc, char** argv) {
    std::vector<char*> arguments(argv, argv + argc);
    const bool regions = std::erase_if(arguments, [](const char* argument) {
                             return std::string_view(argument) == "--regions";
                         }) != 0;
    const std::optional<test::GpuTestOptions> options = test::ParseGpuTestOptions(std::span(arguments));
    const std::optional<Scenario> scenario = MakeScenario(regions);
    if (!options || !scenario) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_probe_rewind_test [--warp] [--no-gbv] [--regions]");
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    // 物理のシェーダーは GPU-based validation の計装に数分かかるので切る(gpu_probe_physics_test と同じ。debug layer は有効)
    gpu::DeviceOptions deviceOptions = test::TestDeviceOptions(*options);
    deviceOptions.gpuBasedValidation = false;
    auto device = gpu::Device::Create(options->adapter, deviceOptions);
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    Log(Channel::Sim, Level::Info, "仮の世界 + 積み木{}を {} 刻み。保存点 {} {} {}",
        regions ? " + 気体と液体の区画" : "", TICKS, SAVE_TICKS[0], SAVE_TICKS[1], SAVE_TICKS[2]);
    const int failures = RunTest(device->Get(), *scenario);
    const bool passed = failures == 0 && test::PassesValidation(*device, "gpu_probe_rewind_test");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
