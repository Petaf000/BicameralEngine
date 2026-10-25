// gpu_probe_gas_test.cpp — 仮の世界の GPU の刻みに載せた気体の区画(T-0592・ADR-0256)が、CPU リファレンス(ProbeReference::EnableGas。
// sim/probe_gas)と毎刻みビット一致すること。
//
// 確かめること(区画 12 × 24 × 12・閉じた壁・陽解法。表は probe_gas_test の 1〜4 と同じ〔試験の表から CO を作る規則を除き、
// 基準の大気 N2 79%・O2 21%〕):
//   1. 静かな大気: 刻みごとのハッシュが CPU と同じ・気体の欄(運動量・運動エネルギーのぶん)と帳簿も同じ
//   2. 燃える木箱: 同じく毎刻み一致し、流さなかった刻みが 0
//   3. 6 種の空気(N2・O2・Ar・CO2・H2O・CO。GPU の成分の欄に上限が無いこと。T-0598)の静かな大気と燃える木箱: 同じく毎刻み一致
//   4. 湯気の塊が露になる(T-0646): 水蒸気が凝縮相になる試験の規則で、反応が気体の質量を減らすセルの運動量の合わせ直し
//      (速さを保つ)と、出ていく割合の上限(引く側の熱を負にしない)が CPU と毎刻み一致・気体の欄(質量も)と帳簿も同じ
//   5. 密度のある固体が気体を塞ぐ(T-0617・ADR-0268): 試験の表の固体は密度を持つ(木箱のセルロースはセルの 3.3 %。1〜4 も空きを読む刻み)。
//      区画の中ほどに石(密度 2000)の床を張る(満ちた石は気体を通さず、真ん中の 2 × 2 は半分の石〔一部だけ開く〕)。
//      燃える木箱の煙が石の床に当たり、孔を通る。同じく毎刻み一致
//   6. 表の差し替え(T-0670): 燃える木箱の途中で、物質を足し(石)・物質の値(粘り)と環境の値(乱れの粘り)が変わる表へ /
//      基準の大気が変わる表(6 種の空気)へ差し替える。GPU は区画の作業場を作り直して帳簿を名前で付け替え、CPU は層の基準と帳簿を
//      付け替える。差し替えの刻みからも毎刻み一致・気体の欄と帳簿も同じ
//   7. シェーダーの assert・debug layer のエラーが 0 件
// 気体の段(適用の単位の中)の GPU 時間を出す(docs/perf.md)。
// 走らせ方: ctest(gpu_probe_gas・gpu_probe_gas_warp)か `job.py run -Preset release -Exe gpu_probe_gas_test`。引数は gpu_test_options.h
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "reaction_test_table.h"
#include "sim/probe_gas.h"
#include "sim/probe_sim.h"
#include "standard_world_physics.h"

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    // --- 区画と木箱(probe_gas_test と同じ)---
    constexpr std::array<uint32_t, 3> REGION_MIN = {26, 0, 26};
    constexpr std::array<uint32_t, 3> REGION_SIZE = {12, 24, 12};
    constexpr uint32_t WOOD_MIN = 31;
    constexpr uint32_t WOOD_BOTTOM = 1;
    constexpr uint32_t WOOD_SIZE = 2;
    constexpr uint64_t WOOD_AMOUNT = 50000000;
    constexpr int32_t WOOD_TEMPERATURE = 1000000;
    constexpr int32_t AIR_TEMPERATURE = 300000;

    // 石の床(T-0617): 高さ STONE_LAYER の全面。真ん中の 2 × 2(木箱の真上)は半分の石。石 1 mol = 6000 mm³(C・密度 2000)
    constexpr uint32_t STONE_LAYER = 8;
    constexpr uint64_t STONE_FULL_AMOUNT = 21000000000;  // 21,000 mol(0.126 m³ > セルの 0.125 m³)
    constexpr uint64_t STONE_HALF_AMOUNT = 10400000000;  // 10,400 mol(0.0624 m³。空き 50.08%)

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition)
                return;

            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    // 基準の大気: N2 79%・O2 21%、six なら 6 種(N2・O2・Ar・CO2・H2O・CO。T-0598)。dewRatePerSecond が 0 でなければ露の試験の規則(T-0646)。
    // stone なら密度のある石を足す(T-0617)。eddy が 0 でなければ気体の粘り(世界の乱れの粘り mm²/s + N2・O2 の物質の粘り。T-0775)
    BakedReactionTable MakeTable(bool six, uint64_t dewRatePerSecond = 0, bool stone = false, uint32_t eddy = 0) {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        if (dewRatePerSecond != 0)
            AddDewCondensationForTest(definition, dewRatePerSecond);

        std::erase_if(definition.rules, [](const RuleDefinition& rule) {
            return std::ranges::any_of(rule.products,
                                       [](const RuleTerm& term) { return term.species == "carbon_monoxide"; });
        });

        definition.environment = StandardWorldPhysics();
        definition.environment.gasEddyViscosityMm2PerS = eddy;
        if (eddy != 0) {
            for (SpeciesDefinition& species : definition.species) {
                if (species.name == "nitrogen")
                    species.transport.viscosityMicroPascalSeconds = 17800;
                else if (species.name == "oxygen")
                    species.transport.viscosityMicroPascalSeconds = 20600;
            }
        }

        definition.atmosphere = {
            .points = {{.heightMillimeters = 0,
                        .temperatureMilliKelvin = (uint32_t)AIR_TEMPERATURE,
                        .fractions = {{.species = "nitrogen", .partsPerMillion = 790000},
                                      {.species = "oxygen", .partsPerMillion = 210000}}}},
            .surfacePressurePascals = 101325,
        };
        if (six) {
            definition.atmosphere.points[0].fractions = {{.species = "nitrogen", .partsPerMillion = 780000},
                                                         {.species = "oxygen", .partsPerMillion = 209000},
                                                         {.species = "argon", .partsPerMillion = 9300},
                                                         {.species = "carbon_dioxide", .partsPerMillion = 400},
                                                         {.species = "water_vapor", .partsPerMillion = 1200},
                                                         {.species = "carbon_monoxide", .partsPerMillion = 100}};
        }

        if (stone) {
            definition.species.push_back({.name = "stone",
                                          .composition = {{.element = "C", .count = 1}},
                                          .formationEnthalpy = 0,
                                          .heatCapacity = 8500,
                                          .thermalConductivity = 2000,
                                          .phase = SpeciesPhase::Solid,
                                          .density = 2000});
        }

        auto baked = BakeReactionTable(definition);
        FX_ASSERT(baked.has_value());

        return std::move(*baked);
    }

    // restSpeedUnits = 静止の丸め(2^-20 m/s。0 = 切る。T-0775)。熱の列(T-0787・ADR-0355)は CPU の比べの側だけ切る:
    // GPU の区画にはまだ列が無く(T-0813)、燃える木箱(約 440 気圧の爆発)では列が縮めるので、入れたままでは刻み 2 から一致しない(仮。T-0813 で外す)
    ProbeGasOptions MakeOptions(uint32_t restSpeedUnits = 0) {
        return {.minCell = REGION_MIN,
                .sizeCells = REGION_SIZE,
                .randomSeed = 12345,
                .restSpeedUnits = restSpeedUnits,
                .limitMovedHeat = false};
    }

    bool InWood(uint32_t x, uint32_t y, uint32_t z) {
        return x >= WOOD_MIN && x < WOOD_MIN + WOOD_SIZE && z >= WOOD_MIN && z < WOOD_MIN + WOOD_SIZE &&
               y >= WOOD_BOTTOM && y < WOOD_BOTTOM + WOOD_SIZE;
    }

    bool InStoneHole(uint32_t x, uint32_t z) {
        return x >= WOOD_MIN && x < WOOD_MIN + WOOD_SIZE && z >= WOOD_MIN && z < WOOD_MIN + WOOD_SIZE;
    }

    // 石の床のセル(満ちた石は気体なし、半分の石は基準の大気の半分と)
    reaction::RxCell MakeStoneCell(const BakedReactionTable& table, const reaction::RxCell& air, bool half) {
        std::vector<SpeciesAmount> amounts = {
            {.species = table.SpeciesId("stone"), .amount = half ? STONE_HALF_AMOUNT : STONE_FULL_AMOUNT}};
        if (half) {
            for (uint32_t i = 0; i < air.speciesCount; ++i)
                amounts.push_back({.species = air.species[i], .amount = air.amounts[i] / 2});
        }

        std::ranges::sort(amounts, {}, &SpeciesAmount::species);

        return MakeReactionCell(table, amounts, AIR_TEMPERATURE);
    }

    // 区画を基準の大気で満たした世界(区画の外は空)。wood なら底に燃える木箱(steam なら木箱の代わりに同じ圧力の熱い湯気の塊。
    // 水蒸気 9 割・空気 1 割。probe_gas_test と同じ)。stone なら石の床(T-0617)
    std::vector<reaction::RxCell> MakeWorld(const BakedReactionTable& table, const ProbeGasField& field, bool wood,
                                            bool steam = false, bool stone = false) {
        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const uint32_t index = field.GridIndex(region);
            const uint32_t x = index % PROBE_GRID_SIZE;
            const uint32_t y = (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE;
            const uint32_t z = index / PROBE_SLICE_CELL_COUNT;
            const reaction::RxCell air = field.ReferenceCell(table, y);
            if (stone && y == STONE_LAYER) {
                cells[index] = MakeStoneCell(table, air, InStoneHole(x, z));
                continue;
            }

            if (!wood || !InWood(x, y, z)) {
                cells[index] = air;
                continue;
            }

            std::vector<SpeciesAmount> amounts;
            if (!steam)
                amounts.push_back({.species = table.SpeciesId("cellulose"), .amount = WOOD_AMOUNT});

            uint64_t airAmount = 0;
            for (uint32_t i = 0; i < air.speciesCount; ++i)
                airAmount += air.amounts[i];

            if (steam)
                amounts.push_back(
                    {.species = table.SpeciesId("water_vapor"),
                     .amount = airAmount * 9 / 10 * (uint64_t)AIR_TEMPERATURE / (uint64_t)WOOD_TEMPERATURE});

            for (uint32_t i = 0; i < air.speciesCount; ++i)
                amounts.push_back({.species = air.species[i],
                                   .amount = air.amounts[i] * (steam ? 1u : 10u) / 10u * (uint64_t)AIR_TEMPERATURE /
                                             (uint64_t)WOOD_TEMPERATURE});

            std::ranges::sort(amounts, {}, &SpeciesAmount::species);
            cells[index] = MakeReactionCell(table, amounts, WOOD_TEMPERATURE);
        }

        return cells;
    }

    // --- GPU の気体の欄と作業場を読み戻す ---
    struct GasReadout {
        std::vector<ProbeGasCellState> cells;
        std::vector<std::byte> work;

        template <typename T>
        T At(uint32_t offset) const {
            T value{};
            std::memcpy(&value, work.data() + offset, sizeof(T));
            return value;
        }
    };

    bool ReadGas(ID3D12Device5* device, const ProbeSim& simulation, GasReadout& readout) {
        auto queue = gpu::ImmediateQueue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        const uint64_t stateBytes = simulation.GasState()->GetDesc().Width;
        const uint64_t workBytes = simulation.GasWork()->GetDesc().Width;
        const ComPtr<ID3D12Resource> stateReadback = gpu::CreateBuffer(device, stateBytes, gpu::BufferKind::Readback);
        const ComPtr<ID3D12Resource> workReadback = gpu::CreateBuffer(device, workBytes, gpu::BufferKind::Readback);
        if (!queue || !stateReadback || !workReadback)
            return false;

        ID3D12GraphicsCommandList10* list = queue->Begin();
        if (list == nullptr)
            return false;

        list->CopyBufferRegion(stateReadback.Get(), 0, simulation.GasState(), 0, stateBytes);
        list->CopyBufferRegion(workReadback.Get(), 0, simulation.GasWork(), 0, workBytes);
        readout.cells.resize(stateBytes / sizeof(ProbeGasCellState));
        readout.work.resize(workBytes);

        return queue->ExecuteAndWait() &&
               gpu::ReadBuffer(stateReadback.Get(), std::as_writable_bytes(std::span(readout.cells))) &&
               gpu::ReadBuffer(workReadback.Get(), std::span(readout.work));
    }

    // CPU の気体の欄・帳簿と比べる
    bool SameGas(const ProbeGasField& field, const ProbeGasHeader& header, const GasReadout& readout) {
        bool same = readout.cells.size() == field.RegionCellCount();
        int64_t kinetic = 0;
        for (uint32_t region = 0; same && region < field.RegionCellCount(); ++region) {
            for (uint32_t axis = 0; axis < 3; ++axis)
                same = same && readout.cells[region].momentum[axis] == field.Momentum()[region][axis];

            same = same && readout.cells[region].mass == field.Mass()[region];

            kinetic += readout.cells[region].kinetic;
        }

        same = same && kinetic == field.KineticEnergySum();
        const ProbeGasLedger& ledger = field.Ledger();
        for (uint32_t id = 0; id < header.speciesCount; ++id)
            same = same && readout.At<int64_t>(header.ledgerOffset + (id * 8)) == ledger.amounts[id];

        const uint32_t energy = header.ledgerOffset + (header.speciesCount * 8);
        same = same && readout.At<int64_t>(energy) == ledger.energy;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            same = same && readout.At<int64_t>(energy + 8 + (axis * 8)) == ledger.momentum[axis];
            same = same &&
                   readout.At<int64_t>(energy + 32 + (axis * 8)) == ledger.restMomentum[axis];  // 静止の丸めの内訳
        }

        return same && readout.At<int64_t>(energy + 56) == ledger.restEnergy;
    }

    // --- 走らせる(1 フレーム = 1 刻み。毎刻み CPU と比べる)---
    // 表の差し替え(T-0670): 刻み tick の始めに table へ
    struct SceneSwap {
        const BakedReactionTable* table = nullptr;
        uint64_t tick = 0;
    };

    void CheckScene(ID3D12Device5* device, const BakedReactionTable& table, std::string_view name, bool wood,
                    uint64_t ticks, Failures& failures, bool steam = false, bool stone = false,
                    uint32_t restSpeedUnits = 0, SceneSwap swap = {}) {
        const ProbeGasOptions options = MakeOptions(restSpeedUnits);
        const auto field = ProbeGasField::Create(table, options);
        if (!field) {
            failures.Check(false, std::format("{}: 区画を作れる({})", name, field.error()));
            return;
        }

        const std::vector<reaction::RxCell> world = MakeWorld(table, *field, wood, steam, stone);
        ProbeReference reference(table, world);
        reference.SetCascadeRadiation({.enabled = false});  // GPU の仮の世界に放射の段が入るまで(T-1154)
        failures.Check(reference.EnableGas(options).has_value(), std::format("{}: CPU の区画", name));

        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"GasTestSim");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table,
                                           {.initialWorld = world, .gas = options});
        if (!queue || !simulation) {
            failures.Check(false, std::format("{}: GPU の世界を作れる({}{})", name, queue ? "" : queue.error(),
                                              simulation ? "" : simulation.error()));
            return;
        }

        const double usPerTimestamp = 1'000'000.0 / static_cast<double>(queue->TimestampFrequency());
        const uint32_t unitsPerTick = simulation->UnitsPerTick();
        uint32_t debugAssertCount = 0;
        uint64_t matched = 0;
        uint32_t cpuSkipped = 0;
        uint32_t blockedCells = 0;
        uint32_t partialCells = 0;
        uint32_t restedCells = 0;  // CPU が静止の丸めで止めたセルの数(全部の刻みの和)
        double applyMicroseconds = 0.0;
        bool hashesMatch = true;
        for (uint64_t tick = 0; tick < ticks && hashesMatch; ++tick) {
            const auto slot = static_cast<uint32_t>(tick % ProbeSim::FRAME_SLOT_COUNT);
            const bool swapping = swap.table != nullptr && tick == swap.tick;
            const std::array<ProbeTableSwap, 1> swaps = {ProbeTableSwap{.tick = tick, .table = swap.table}};
            ProbeFrameInput input{.firstTick = tick, .unitCount = unitsPerTick};
            if (swapping)
                input.tableSwaps = swaps;

            ID3D12CommandList* list = simulation->RecordFrame(slot, input);
            if (list == nullptr || !queue->WaitCpu(queue->Submit(list))) {
                failures.Check(false, std::format("{}: 刻み {} を投げられる", name, tick));
                return;
            }

            const ProbeFrameReadback readback = simulation->ReadFrame(slot);
            debugAssertCount += readback.debugAssertCount;
            if (!readback.unitGpuTicks.empty())
                applyMicroseconds += static_cast<double>(readback.unitGpuTicks[PROBE_UNIT_APPLY]) * usPerTimestamp;

            reference.Advance(tick, {}, swapping ? swap.table : nullptr);
            cpuSkipped += reference.LastGasReport().skippedCellOverflow ? 1 : 0;
            blockedCells = std::max(blockedCells, reference.LastGasReport().blockedCells);
            partialCells = std::max(partialCells, reference.LastGasReport().partialCells);
            restedCells += reference.LastGasReport().restedCells;
            hashesMatch = readback.hashes.size() == 1 && readback.hashes[0].hash == reference.StateHash(tick + 1);
            if (!hashesMatch)
                Log(Channel::Sim, Level::Error, "{}: 刻み {} のハッシュが食い違う(GPU {:016x} CPU {:016x})", name,
                    tick + 1, readback.hashes.empty() ? 0 : readback.hashes[0].hash, reference.StateHash(tick + 1));
            else
                ++matched;
        }

        GasReadout readout;
        const bool read = ReadGas(device, *simulation, readout);
        const ProbeGasHeader& header = simulation->GasHeader();
        const bool gasSame = read && SameGas(*reference.GasField(), header, readout);
        const uint32_t stepped = read ? readout.At<uint32_t>(PROBE_GAS_CONTROL_STEPPED) : 0;
        const uint32_t tooMany = read ? readout.At<uint32_t>(PROBE_GAS_CONTROL_SKIPPED_TOO_MANY) : 0;
        const uint32_t overflow = read ? readout.At<uint32_t>(PROBE_GAS_CONTROL_SKIPPED_OVERFLOW) : 0;
        const uint32_t lastRested = read ? readout.At<uint32_t>(PROBE_GAS_CONTROL_RESTED) : 0;

        Log(Channel::Sim, Level::Info,
            "{}: {} 刻みのうち {} 刻みハッシュ一致・気体の欄と帳簿 {}・流した刻み {}(欄を超えた {}・溢れ {}・CPU "
            "の溢れ {})・"
            "適用の単位(気体の段を含む){:.1f} µs/刻み・塞がったセル {}・一部だけ開いたセル {}",
            name, ticks, matched, gasSame ? "一致" : "NG", stepped, tooMany, overflow, cpuSkipped,
            applyMicroseconds / static_cast<double>(std::max<uint64_t>(matched, 1)), blockedCells, partialCells);
        failures.Check(hashesMatch && matched == ticks, std::format("{}: 刻みごとのハッシュが CPU と同じ", name));
        failures.Check(gasSame, std::format("{}: 気体の欄と帳簿が CPU と同じ", name));
        failures.Check(tooMany == 0 && overflow == (uint32_t)cpuSkipped && stepped + overflow == ticks,
                       std::format("{}: 流さなかった刻みが CPU と同じ", name));
        failures.Check(debugAssertCount == 0, std::format("{}: シェーダーの assert が 0 件", name));
        failures.Check(lastRested == reference.LastGasReport().restedCells,
                       std::format("{}: 最後の刻みに静止の丸めで止めたセルの数が CPU と同じ({} / {})", name, lastRested,
                                   reference.LastGasReport().restedCells));
        if (restSpeedUnits != 0) {
            Log(Channel::Sim, Level::Info, "{}: 静止の丸めで止めたセル(全部の刻みの和){}", name, restedCells);
            if (ticks >=
                30)  // WARP の短い回しでは当たる前に終わることがある(GPU の値は帳簿の内訳と最後の刻みの数で比べてある)
                failures.Check(restedCells > 0, std::format("{}: 静止の丸めが当たる", name));
        }

        if (stone) {
            failures.Check(blockedCells > 0 && partialCells > 0,
                           std::format("{}: 塞がったセルと一部だけ開いたセルがある(空きを読む刻み)", name));
        }
    }

    // --- 画像: 刻み tick の状態の z = 32 の縦の断面(y が上)。左 = 温度(300〜800 K を黒 → 赤 → 黄)・右 = CO2(0〜2 mol を黒 → 緑)。PPM ---
    template <typename T>
    std::vector<T> ReadWhole(ID3D12Device5* device, ID3D12Resource* buffer) {
        const uint64_t bytes = buffer->GetDesc().Width;
        std::vector<T> values(bytes / sizeof(T));
        auto queue = gpu::ImmediateQueue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        const ComPtr<ID3D12Resource> readback = gpu::CreateBuffer(device, bytes, gpu::BufferKind::Readback);
        ID3D12GraphicsCommandList10* list = queue && readback ? queue->Begin() : nullptr;
        if (list == nullptr)
            return {};

        list->CopyBufferRegion(readback.Get(), 0, buffer, 0, bytes);
        if (!queue->ExecuteAndWait() || !gpu::ReadBuffer(readback.Get(), std::as_writable_bytes(std::span(values))))
            return {};

        return values;
    }

    void WriteSlice(ID3D12Device5* device, const ProbeSim& simulation, const BakedReactionTable& table, uint64_t tick,
                    const std::string& path) {
        constexpr uint32_t Z = 32;
        constexpr uint32_t X_BEGIN = 20;
        constexpr uint32_t WIDTH = 24;
        constexpr uint32_t HEIGHT = 32;
        constexpr uint32_t SCALE = 10;
        const std::vector<reaction::RxCell> cells = ReadWhole<reaction::RxCell>(device, simulation.Cells());
        const std::vector<reaction::HcThermalCache> caches = ReadWhole<reaction::HcThermalCache>(device,
                                                                                                 simulation.Thermal());
        if (cells.size() < 2 * PROBE_CELL_COUNT || caches.size() < 2 * PROBE_CELL_COUNT)
            return;

        const size_t base = (tick & 1) * PROBE_CELL_COUNT;
        const uint32_t carbonDioxide = table.SpeciesId("carbon_dioxide");
        const uint32_t imageWidth = 2 * WIDTH * SCALE;
        std::vector<uint8_t> pixels((size_t)imageWidth * HEIGHT * SCALE * 3, 0);
        for (uint32_t row = 0; row < HEIGHT; ++row) {
            const uint32_t y = HEIGHT - 1 - row;
            for (uint32_t column = 0; column < WIDTH; ++column) {
                const uint32_t index = ProbeCellIndex(X_BEGIN + column, y, Z);
                const int64_t kelvin = caches[base + index].temperature / 1000;
                const auto heat = (uint32_t)std::clamp<int64_t>((kelvin - 300) * 510 / 500, 0, 510);
                uint64_t smoke = 0;
                for (uint32_t i = 0; i < cells[base + index].speciesCount; ++i) {
                    if (cells[base + index].species[i] == carbonDioxide)
                        smoke = cells[base + index].amounts[i];
                }

                const std::array<std::array<uint8_t, 3>, 2> colors = {
                    std::array<uint8_t, 3>{(uint8_t)std::min(heat, 255u), (uint8_t)(heat > 255 ? heat - 255 : 0), 0},
                    std::array<uint8_t, 3>{0, (uint8_t)std::min<uint64_t>(smoke * 255 / 2000000, 255), 0}};
                for (uint32_t panel = 0; panel < 2; ++panel) {
                    for (uint32_t dy = 0; dy < SCALE; ++dy) {
                        for (uint32_t dx = 0; dx < SCALE; ++dx) {
                            const size_t pixel = ((size_t)(row * SCALE + dy) * imageWidth) +
                                                 ((panel * WIDTH + column) * SCALE) + dx;
                            for (uint32_t channel = 0; channel < 3; ++channel)
                                pixels[pixel * 3 + channel] = colors[panel][channel];
                        }
                    }
                }
            }
        }

        std::ofstream file(path, std::ios::binary);
        if (!file)
            return;

        file << std::format("P6\n{} {}\n255\n", imageWidth, HEIGHT * SCALE);
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        Log(Channel::Sim, Level::Info, "断面の画像: {}", path);
    }

    // --- 費用: 30 刻みずつのフレームで 120 刻み流し、適用の単位(気体の段を含む)の GPU 時間の平均(µs/刻み)。gas が無ければ気体なし ---
    double MeasureApply(ID3D12Device5* device, const BakedReactionTable& table, std::span<const reaction::RxCell> world,
                        const ProbeGasOptions& gas, const std::string& imagePath) {
        constexpr uint64_t MEASURE_TICKS = 120;
        constexpr uint32_t TICKS_PER_FRAME = 30;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"GasMeasureSim");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table,
                                           {.initialWorld = world, .gas = gas});
        if (!queue || !simulation)
            return 0.0;

        const double usPerTimestamp = 1'000'000.0 / static_cast<double>(queue->TimestampFrequency());
        const uint32_t unitsPerTick = simulation->UnitsPerTick();
        double total = 0.0;
        for (uint64_t tick = 0; tick < MEASURE_TICKS; tick += TICKS_PER_FRAME) {
            const auto slot = static_cast<uint32_t>((tick / TICKS_PER_FRAME) % ProbeSim::FRAME_SLOT_COUNT);
            ID3D12CommandList* list = simulation->RecordFrame(
                slot, {.firstTick = tick, .unitCount = TICKS_PER_FRAME * unitsPerTick});
            if (list == nullptr || !queue->WaitCpu(queue->Submit(list)))
                return 0.0;

            const ProbeFrameReadback readback = simulation->ReadFrame(slot);
            // 最初のフレームは暖機(クロックが上がる前)として捨てる
            for (size_t index = 0; tick != 0 && index < readback.unitGpuTicks.size(); index += unitsPerTick)
                total += static_cast<double>(readback.unitGpuTicks[index + PROBE_UNIT_APPLY]) * usPerTimestamp;
        }

        WriteSlice(device, *simulation, table, MEASURE_TICKS, imagePath);

        return total / static_cast<double>(MEASURE_TICKS - TICKS_PER_FRAME);
    }

    // stone なら石の床(塞がったセル・一部だけ開いたセルのある刻み。T-0617)
    void Measure(ID3D12Device5* device, const BakedReactionTable& table, bool stone) {
        const auto field = ProbeGasField::Create(table, MakeOptions());
        if (!field)
            return;

        const std::vector<reaction::RxCell> world = MakeWorld(table, *field, true, false, stone);
        const std::string prefix = stone ? "gpu_probe_gas_stone" : "gpu_probe_gas";
        const double without = MeasureApply(device, table, world, {}, prefix + "_without.ppm");
        const double with = MeasureApply(device, table, world, MakeOptions(), prefix + "_with.ppm");
        Log(Channel::Sim, Level::Info,
            "費用({}・区画 {} セル・小刻み 24): 適用の単位 気体なし {:.1f} µs/刻み → 気体あり {:.1f} "
            "µs/刻み(気体の段 約 {:.1f} µs)",
            stone ? "燃える木箱と石の床" : "燃える木箱", field->RegionCellCount(), without, with, with - without);
    }

}  // namespace

int main(int argc, char** argv) {
    const auto options = test::ParseGpuTestOptions(std::span(argv, static_cast<size_t>(argc)));
    if (!options) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_probe_gas_test [gpu_test_options の引数]");
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    // WARP は遅いので刻みを減らす
    const bool warp = options->adapter == gpu::AdapterKind::Warp;
    const BakedReactionTable table = MakeTable(false);
    const BakedReactionTable sixTable = MakeTable(true);
    Failures failures;
    CheckScene(device->Get(), table, "静かな大気", false, warp ? 3 : 10, failures);
    CheckScene(device->Get(), table, "燃える木箱", true, warp ? 4 : 60, failures);
    CheckScene(device->Get(), sixTable, "6 種の空気・静か", false, warp ? 2 : 10, failures);
    CheckScene(device->Get(), sixTable, "6 種の空気・燃える木箱", true, warp ? 3 : 60, failures);
    CheckScene(device->Get(), MakeTable(false, 100), "湯気の塊が露になる", true, warp ? 4 : 60, failures, true);
    const BakedReactionTable stoneTable = MakeTable(false, 0, true);
    CheckScene(device->Get(), stoneTable, "燃える木箱と石の床", true, warp ? 3 : 60, failures, false, true);

    // 気体の粘り(0.2 m²/s + N2・O2 の物質の粘り)と静止の丸め 1 m/s(火の柱の縁の遅くなるセルに当たる大きさ。T-0775。GPU の粘りの段・丸めが CPU と毎刻みビット一致)
    constexpr uint32_t EDDY_MM2_PER_S = 200000;
    constexpr uint32_t REST_SPEED_UNITS = 1u << 20;  // 1 m/s
    const BakedReactionTable viscousTable = MakeTable(false, 0, false, EDDY_MM2_PER_S);
    CheckScene(device->Get(), viscousTable, "燃える木箱・粘りと静止の丸め", true, warp ? 4 : 120, failures, false,
               false, REST_SPEED_UNITS);
    CheckScene(device->Get(), MakeTable(false, 0, true, EDDY_MM2_PER_S), "燃える木箱と石の床・粘り", true,
               warp ? 3 : 60, failures, false, true);

    // 表の差し替え(T-0670): 物質を足す・物質と環境の値が変わる表へ / 基準の大気が変わる表へ(区画の作業場を作り直す)
    const BakedReactionTable stoneViscousTable = MakeTable(false, 0, true, EDDY_MM2_PER_S);
    const uint64_t swapTick = warp ? 2 : 20;
    CheckScene(device->Get(), table, "燃える木箱 → 石と粘りの表へ差し替え", true, warp ? 4 : 60, failures, false, false,
               0, {.table = &stoneViscousTable, .tick = swapTick});
    CheckScene(device->Get(), table, "燃える木箱 → 6 種の空気の表へ差し替え", true, warp ? 4 : 60, failures, false,
               false, 0, {.table = &sixTable, .tick = swapTick});

    if (!warp) {
        Measure(device->Get(), table, false);
        Measure(device->Get(), stoneTable, true);
    }

    const bool passed = failures.count == 0 && test::PassesValidation(*device, "gpu_probe_gas_test");
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_gas_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
