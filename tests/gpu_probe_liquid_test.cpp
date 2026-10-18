// gpu_probe_liquid_test.cpp — 仮の世界の GPU の刻みに載せた液体の区画(T-0613・ADR-0266・ADR-0276・ADR-0268)が、CPU リファレンス
// (ProbeReference::EnableLiquid。sim/probe_liquid)と毎刻みビット一致すること。
//
// 確かめること(表は probe_liquid_test と同じ: 試験の表に液体の水・石〔セルを満たすと壁〕・砂〔半分を占める。孔〕・地球の環境の値):
//   1. 池に溜まる: 台の上の水が池へ流れ落ちる(区画 16 × 10 × 3)
//   2. 堤を消すと抜ける: 水で満ちた池の堤が無い世界で、水が低い土地(床の上に半分を占める砂 = 一部だけ開いた面)へ流れ出る
//   3. ダムの水が押し寄せる: 区画 24 × 8 × 1 の端の水の柱が、運動量の移流で押し寄せる
//   4. 移流なし(T-0609 の形): 1 と同じ場面を advection = false で
//   各場面で、刻みごとの状態のハッシュが CPU と同じ・最後の液体の状態(面の運動量・運動と位置のエネルギー・帳簿)が同じ・
//   流した刻みの数・成分の溢れ 0・シェーダーの assert 0・debug layer のエラー 0。
// 液体の段(適用の単位の中)の GPU 時間を出す(docs/perf.md)。
// 走らせ方: ctest(gpu_probe_liquid・gpu_probe_liquid_warp)か `job.py run -Preset release -Exe gpu_probe_liquid_test`。引数は gpu_test_options.h
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
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
#include "sim/probe_liquid.h"
#include "sim/probe_sim.h"
#include "standard_world_physics.h"

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    // --- 池の区画(probe_liquid_test と同じ地形。y が上)---
    //   x 0〜3: 台(石 y 0〜6・水 y 7〜8)/ x 4〜9: 池(石 y 0〜3)/ x 10: 堤(石 y 0〜6)/ x 11〜15: 低い土地(石 y 0)
    constexpr std::array<uint32_t, 3> POOL_MIN = {24, 0, 30};
    constexpr std::array<uint32_t, 3> POOL_SIZE = {16, 10, 3};
    constexpr uint32_t BASIN_BEGIN = 4;
    constexpr uint32_t DAM_X = 10;

    // --- ダムの区画(probe_liquid_test の 8 と同じ。水は x 0〜5・y 1〜4)---
    constexpr std::array<uint32_t, 3> DAM_MIN = {8, 0, 8};
    constexpr std::array<uint32_t, 3> DAM_SIZE = {24, 8, 1};
    constexpr uint32_t DAM_WATER_END = 6;
    constexpr uint32_t DAM_WATER_TOP = 4;

    constexpr uint64_t FULL_WATER = 6900000000;  // µmol(0.5 m 角の水 ≒ 6939 mol の少し下)
    constexpr uint64_t NITROGEN = 4000000;       // µmol(0.125 m³ の空気・300 K・1 気圧)
    constexpr uint64_t OXYGEN = 1000000;
    constexpr uint64_t STONE = 21000000000;  // µmol(密度 2000 kg/m³ でセルを満たす量の少し上 = 壁)
    constexpr uint64_t SAND = 10000000000;   // µmol(密度 2000 kg/m³ でセルの約 48% = 一部だけ開いた面)
    constexpr int32_t TEMPERATURE = 300000;

    enum class Kind : uint8_t { Air, Water, Stone, Sand };

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition)
                return;

            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    // evaporation: 水が湯気になる試験の規則(1 次・毎秒 5%・吸熱。流れている液体の質量が区画の外の段で減る場面。T-0656)
    BakedReactionTable MakeTable(bool evaporation = false) {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.environment = StandardWorldPhysics();
        definition.species.push_back({.name = "liquid_water",
                                      .composition = {{.element = "H", .count = 2}, {.element = "O", .count = 1}},
                                      .formationEnthalpy = -285830,
                                      .heatCapacity = 75300,
                                      .thermalConductivity = 600,
                                      .phase = SpeciesPhase::Liquid,
                                      .density = 1000});
        definition.species.push_back({.name = "stone",
                                      .composition = {{.element = "C", .count = 1}},
                                      .formationEnthalpy = 0,
                                      .heatCapacity = 8500,
                                      .thermalConductivity = 2000,
                                      .phase = SpeciesPhase::Solid,
                                      .density = 2000});
        definition.species.push_back({.name = "sand",
                                      .composition = {{.element = "C", .count = 1}},
                                      .formationEnthalpy = 0,
                                      .heatCapacity = 8500,
                                      .thermalConductivity = 300,
                                      .phase = SpeciesPhase::Solid,
                                      .density = 2000});
        if (evaporation)
            definition.rules.push_back(
                {.name = "test_evaporation",
                 .reactants = {{.species = "liquid_water", .coefficient = 1, .firstOrder = true}},
                 .products = {{.species = "water_vapor", .coefficient = 1}},
                 .rate = {.preExponentialMantissa = 5, .preExponentialExponent10 = -2, .activationEnergy = 0}});

        auto baked = BakeReactionTable(definition);
        FX_ASSERT(baked.has_value());

        return std::move(*baked);
    }

    reaction::RxCell MakeCell(const BakedReactionTable& table, Kind kind) {
        std::vector<SpeciesAmount> amounts = {{.species = table.SpeciesId("nitrogen"), .amount = NITROGEN},
                                              {.species = table.SpeciesId("oxygen"), .amount = OXYGEN}};
        if (kind == Kind::Water)
            amounts.push_back({.species = table.SpeciesId("liquid_water"), .amount = FULL_WATER});

        if (kind == Kind::Sand)
            amounts.push_back({.species = table.SpeciesId("sand"), .amount = SAND});

        if (kind == Kind::Stone)
            amounts = {{.species = table.SpeciesId("stone"), .amount = STONE}};

        std::ranges::sort(amounts, {}, &SpeciesAmount::species);

        return MakeReactionCell(table, amounts, TEMPERATURE);
    }

    // --- 場面(区画の中の座標 → 物)---
    enum class Scene : uint8_t { Pool, Drain, DamBreak };

    Kind SceneKind(Scene scene, uint32_t x, uint32_t y) {
        if (scene == Scene::DamBreak) {
            if (y == 0)
                return Kind::Stone;

            return y <= DAM_WATER_TOP && x < DAM_WATER_END ? Kind::Water : Kind::Air;
        }

        if (y == 0)
            return Kind::Stone;

        if (x < BASIN_BEGIN) {
            if (scene == Scene::Drain)
                return y <= 6 ? Kind::Stone : Kind::Air;

            return y <= 6 ? Kind::Stone : (y <= 8 ? Kind::Water : Kind::Air);
        }

        if (x < DAM_X) {
            if (y <= 3)
                return Kind::Stone;

            return scene == Scene::Drain && y <= 6 ? Kind::Water : Kind::Air;
        }

        if (x == DAM_X && scene == Scene::Pool)
            return y <= 6 ? Kind::Stone : Kind::Air;

        // 低い土地(堤を消した場面は床の上に砂 = 孔のある面)
        return scene == Scene::Drain && y == 1 ? Kind::Sand : Kind::Air;
    }

    ProbeLiquidOptions MakeOptions(Scene scene, bool advection) {
        const bool dam = scene == Scene::DamBreak;

        return {.minCell = dam ? DAM_MIN : POOL_MIN, .sizeCells = dam ? DAM_SIZE : POOL_SIZE, .advection = advection};
    }

    std::vector<reaction::RxCell> MakeWorld(const BakedReactionTable& table, Scene scene,
                                            const ProbeLiquidField& field) {
        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        const auto size = field.Options().sizeCells;
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const uint32_t x = region % size[0];
            const uint32_t y = region / (size[0] * size[2]);
            cells[field.GridIndex(region)] = MakeCell(table, SceneKind(scene, x, y));
        }

        return cells;
    }

    // --- GPU の液体の作業場を読み戻して、CPU の ProbeLiquidField::Hash と同じハッシュを作る ---
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
    constexpr uint64_t FNV_PRIME = 1099511628211ull;

    uint64_t Mix(uint64_t hash, uint64_t value) {
        for (uint32_t byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xffu;
            hash *= FNV_PRIME;
        }

        return hash;
    }

    struct LiquidReadout {
        std::vector<std::byte> work;

        template <typename T>
        T At(uint32_t offset) const {
            T value{};
            std::memcpy(&value, work.data() + offset, sizeof(T));
            return value;
        }
    };

    bool ReadLiquid(ID3D12Device5* device, const ProbeSim& simulation, LiquidReadout& readout) {
        auto queue = gpu::ImmediateQueue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        const uint64_t bytes = simulation.LiquidWork()->GetDesc().Width;
        const ComPtr<ID3D12Resource> readback = gpu::CreateBuffer(device, bytes, gpu::BufferKind::Readback);
        ID3D12GraphicsCommandList10* list = queue && readback ? queue->Begin() : nullptr;
        if (list == nullptr)
            return false;

        list->CopyBufferRegion(readback.Get(), 0, simulation.LiquidWork(), 0, bytes);
        readout.work.resize(bytes);

        return queue->ExecuteAndWait() && gpu::ReadBuffer(readback.Get(), std::span(readout.work));
    }

    // 食い違った刻みの、区画のセルの GPU と CPU の違いを最初の数個だけ出す(調べる時の手がかり)
    void LogCellDifferences(ID3D12Device5* device, const ProbeSim& simulation, const ProbeReference& reference,
                            const ProbeLiquidField& field, uint64_t stateTick, std::string_view name) {
        const uint64_t bytes = simulation.Cells()->GetDesc().Width;
        std::vector<reaction::RxCell> gpu(bytes / sizeof(reaction::RxCell));
        auto queue = gpu::ImmediateQueue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        const ComPtr<ID3D12Resource> readback = gpu::CreateBuffer(device, bytes, gpu::BufferKind::Readback);
        ID3D12GraphicsCommandList10* list = queue && readback ? queue->Begin() : nullptr;
        if (list == nullptr)
            return;

        list->CopyBufferRegion(readback.Get(), 0, simulation.Cells(), 0, bytes);
        if (!queue->ExecuteAndWait() || !gpu::ReadBuffer(readback.Get(), std::as_writable_bytes(std::span(gpu))))
            return;

        const std::span<const reaction::RxCell> cpu = reference.State(stateTick);
        const size_t base = (stateTick & 1) * PROBE_CELL_COUNT;
        uint32_t shown = 0;
        uint32_t differing = 0;
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            const reaction::RxCell& a = gpu[base + index];
            const reaction::RxCell& b = cpu[index];
            bool same = a.energy == b.energy && a.speciesCount == b.speciesCount;
            for (uint32_t i = 0; same && i < a.speciesCount; ++i)
                same = a.species[i] == b.species[i] && a.amounts[i] == b.amounts[i];

            if (same)
                continue;

            ++differing;
            if (shown++ >= 6)
                continue;

            std::string text;
            for (uint32_t i = 0; i < std::max(a.speciesCount, b.speciesCount) && i < reaction::RX_MAX_CELL_SPECIES; ++i)
                text += std::format(" [{}: {}/{} {}/{}]", i, a.species[i], b.species[i], a.amounts[i], b.amounts[i]);

            Log(Channel::Sim, Level::Error, "{}: セル ({}, {}, {}) エネルギー GPU {} CPU {} 成分 {}/{}{}", name,
                index % PROBE_GRID_SIZE, (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE, index / PROBE_SLICE_CELL_COUNT,
                a.energy, b.energy, a.speciesCount, b.speciesCount, text);
        }

        Log(Channel::Sim, Level::Error, "{}: 違うセル {} 個(区画 {} セル)", name, differing, field.RegionCellCount());
    }

    // 並びは ProbeLiquidField::Hash と同じ(面の運動量 → 運動のエネルギー → 流れの端数 → 位置のエネルギー → 液体の質量 → 帳簿)
    uint64_t GpuLiquidHash(const ProbeLiquidHeader& header, const LiquidReadout& readout) {
        const uint32_t count = header.regionCellCount;
        uint64_t hash = FNV_OFFSET;
        for (const uint32_t offset : {header.facesOffset, header.kineticOffset, header.carryOffset}) {
            for (uint32_t index = 0; index < 3 * count; ++index)
                hash = Mix(hash, readout.At<uint64_t>(offset + (index * 8)));
        }

        for (uint32_t region = 0; region < count; ++region)
            hash = Mix(hash, readout.At<uint64_t>(header.potentialOffset + (region * 8)));

        for (uint32_t region = 0; region < count; ++region)
            hash = Mix(hash, readout.At<uint64_t>(header.massOffset + (region * 8)));

        for (uint32_t axis = 0; axis < 3; ++axis)
            hash = Mix(hash, readout.At<uint64_t>(PROBE_LIQUID_LEDGER_EXTERNAL + (axis * 8)));

        return Mix(hash, readout.At<uint64_t>(PROBE_LIQUID_LEDGER_ENERGY));
    }

    // --- 走らせる(1 フレーム = 1 刻み。毎刻み CPU と比べる)---
    void CheckScene(ID3D12Device5* device, const BakedReactionTable& table, std::string_view name, Scene scene,
                    bool advection, uint64_t ticks, Failures& failures) {
        const ProbeLiquidOptions options = MakeOptions(scene, advection);
        const auto field = ProbeLiquidField::Create(table, options);
        if (!field) {
            failures.Check(false, std::format("{}: 区画を作れる({})", name, field.error()));
            return;
        }

        const std::vector<reaction::RxCell> world = MakeWorld(table, scene, *field);
        ProbeReference reference(table, world);
        failures.Check(reference.EnableLiquid(options).has_value(), std::format("{}: CPU の区画", name));

        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"LiquidTestSim");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table,
                                           {.initialWorld = world, .liquid = options});
        if (!queue || !simulation) {
            failures.Check(false, std::format("{}: GPU の世界を作れる({}{})", name, queue ? "" : queue.error(),
                                              simulation ? "" : simulation.error()));
            return;
        }

        const uint32_t unitsPerTick = simulation->UnitsPerTick();
        uint32_t debugAssertCount = 0;
        uint64_t matched = 0;
        uint64_t moved = 0;
        bool hashesMatch = true;
        for (uint64_t tick = 0; tick < ticks && hashesMatch; ++tick) {
            const auto slot = static_cast<uint32_t>(tick % ProbeSim::FRAME_SLOT_COUNT);
            ID3D12CommandList* list = simulation->RecordFrame(slot, {.firstTick = tick, .unitCount = unitsPerTick});
            if (list == nullptr || !queue->WaitCpu(queue->Submit(list))) {
                failures.Check(false, std::format("{}: 刻み {} を投げられる", name, tick));
                return;
            }

            const ProbeFrameReadback readback = simulation->ReadFrame(slot);
            debugAssertCount += readback.debugAssertCount;
            reference.Advance(tick, {});
            moved += reference.LastLiquidReport().movedMilligrams;
            hashesMatch = readback.hashes.size() == 1 && readback.hashes[0].hash == reference.StateHash(tick + 1);
            if (!hashesMatch)
                Log(Channel::Sim, Level::Error, "{}: 刻み {} のハッシュが食い違う(GPU {:016x} CPU {:016x})", name,
                    tick + 1, readback.hashes.empty() ? 0 : readback.hashes[0].hash, reference.StateHash(tick + 1));
            else
                ++matched;

            if (!hashesMatch)
                LogCellDifferences(device, *simulation, reference, *reference.LiquidField(), tick + 1, name);
        }

        LiquidReadout readout;
        const bool read = ReadLiquid(device, *simulation, readout);
        const ProbeLiquidField& cpu = *reference.LiquidField();
        const bool liquidSame = read && GpuLiquidHash(simulation->LiquidHeader(), readout) == cpu.Hash();
        const uint32_t stepped = read ? readout.At<uint32_t>(PROBE_LIQUID_CONTROL_STEPPED) : 0;
        const uint32_t overflow = read ? readout.At<uint32_t>(PROBE_LIQUID_CONTROL_OVERFLOW) : 1;

        Log(Channel::Sim, Level::Info,
            "{}: {} 刻みのうち {} 刻みハッシュ一致・液体の状態と帳簿 {}・流した刻み {}・溢れ {}・CPU で動いた質量 {} "
            "kg・"
            "運動のエネルギー {} mJ・x の運動量 − 帳簿 {}",
            name, ticks, matched, liquidSame ? "一致" : "NG", stepped, overflow, moved / 1000000,
            cpu.KineticEnergySum(), cpu.MomentumSum(0) - cpu.Ledger().external[0]);
        failures.Check(hashesMatch && matched == ticks, std::format("{}: 刻みごとのハッシュが CPU と同じ", name));
        failures.Check(liquidSame, std::format("{}: 液体の状態と帳簿が CPU と同じ", name));
        failures.Check(stepped == ticks && overflow == 0, std::format("{}: 毎刻み流し、成分が溢れない", name));
        failures.Check(moved > 0, std::format("{}: 水が動いた", name));
        failures.Check(debugAssertCount == 0, std::format("{}: シェーダーの assert が 0 件", name));
    }

    // --- 費用: 30 刻みずつのフレームで 120 刻み流し、適用の単位(液体の段を含む)の GPU 時間の平均(µs/刻み)---
    double MeasureApply(ID3D12Device5* device, const BakedReactionTable& table, std::span<const reaction::RxCell> world,
                        const ProbeLiquidOptions& liquid) {
        constexpr uint64_t MEASURE_TICKS = 120;
        constexpr uint32_t TICKS_PER_FRAME = 30;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"LiquidMeasureSim");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table,
                                           {.initialWorld = world, .liquid = liquid});
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

        return total / static_cast<double>(MEASURE_TICKS - TICKS_PER_FRAME);
    }

    void Measure(ID3D12Device5* device, const BakedReactionTable& table, std::string_view name, Scene scene) {
        const ProbeLiquidOptions options = MakeOptions(scene, true);
        const auto field = ProbeLiquidField::Create(table, options);
        if (!field)
            return;

        const std::vector<reaction::RxCell> world = MakeWorld(table, scene, *field);
        const double without = MeasureApply(device, table, world, {});
        const double with = MeasureApply(device, table, world, options);
        Log(Channel::Sim, Level::Info,
            "費用({}・区画 {} セル): 適用の単位 液体なし {:.1f} µs/刻み → 液体あり {:.1f} µs/刻み(液体の段 約 {:.1f} "
            "µs)",
            name, field->RegionCellCount(), without, with, with - without);
    }

}  // namespace

int main(int argc, char** argv) {
    const auto options = test::ParseGpuTestOptions(std::span(argv, static_cast<size_t>(argc)));
    if (!options) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_probe_liquid_test [gpu_test_options の引数]");
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
    const BakedReactionTable table = MakeTable();
    Failures failures;
    CheckScene(device->Get(), table, "池に溜まる", Scene::Pool, true, warp ? 4 : 120, failures);
    CheckScene(device->Get(), table, "堤を消すと抜ける(砂の床)", Scene::Drain, true, warp ? 4 : 120, failures);
    CheckScene(device->Get(), table, "ダムの水が押し寄せる", Scene::DamBreak, true, warp ? 4 : 180, failures);
    CheckScene(device->Get(), table, "池に溜まる・移流なし", Scene::Pool, false, warp ? 3 : 60, failures);

    // 流れているダムの水が湯気になって減る(区画の外の段で液体の質量が変わる面の運動量。T-0656)
    const BakedReactionTable evaporating = MakeTable(true);
    CheckScene(device->Get(), evaporating, "ダムの水が押し寄せながら湯気になる", Scene::DamBreak, true, warp ? 4 : 180,
               failures);

    if (!warp) {
        Measure(device->Get(), table, "池に溜まる", Scene::Pool);
        Measure(device->Get(), table, "ダムの水が押し寄せる", Scene::DamBreak);
    }

    const bool passed = failures.count == 0 && test::PassesValidation(*device, "gpu_probe_liquid_test");
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_liquid_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
