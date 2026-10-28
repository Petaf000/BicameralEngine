// gpu_phase_layer_test.cpp — 凝縮相と気体が混ざるセルの 2 つのエネルギーのセルの段(shaders/common/phase_layer.hlsli の PlStepLoneCell:
// 層の上の面 → 層の温度の反応 → 混ざり方の変わり目)を GPU(shaders/sim/phase_layer_cells.hlsl)で走らせ、CPU とビット一致を確かめる
// (T-1211・ADR-0486)。PlEvenCondensedEnergy の 64bit の差と比べ・PlGasEnergyAfter の床の割り算が RTX で壊れないことの確かめ
// (CLAUDE.md §4。T-1078・T-1159 の形の外れ)。
// 場面(tests/phase_layer_scene.h の表。セル 1024 個を 9 種の場面に分け、量と温度を番号の乱数で振る)を 50 刻みずつ 8 区間進め、
// 区間ごとにセルと Ec を読み戻して比べる。どのセルもエネルギー(和)と元素が初めとビット単位で同じことも見る(面の流れが無いので)。
// 混ざらないセル → 混ざったセル(露が付く: PlEvenCondensedEnergy)と、混ざったセル → 混ざらないセル(露が乾く: Ec を捨てる)の
// 両方の変わり目が区間の中で起きたことと、層の熱が負にならないこと(薄い層の吸熱の反応。ADR-0497 の 5)も確かめる。
// 引数は gpu_test_options.h。
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "phase_layer_scene.h"

using namespace bicameral;
using namespace bicameral::reaction;

namespace {

    constexpr uint32_t CELL_COUNT = 1024;
    constexpr uint32_t SCENE_COUNT = 9;
    constexpr uint32_t THREADS_PER_GROUP = 64;  // phase_layer_cells.hlsl の numthreads
    constexpr uint32_t TICKS_PER_SEGMENT = 50;
    constexpr uint32_t SEGMENT_COUNT = 8;
    constexpr uint64_t WORLD_SEED = 0x7a11u;
    constexpr int MAX_REPORTED_MISMATCHES = 5;

    uint64_t SegmentTickBegin(uint32_t segment) {
        return 1 + (uint64_t{segment} * TICKS_PER_SEGMENT);
    }

    // u0 セル・u1 Ec / b0 定数 7 個 / デバッグのリング / t0〜t5 表と初めの値
    constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
        .uavCount = 2, .rootConstantCount = 7, .debugRing = true, .srvCount = 6};

    // --- 場面 ---------------------------------------------------------------------------------------
    struct LoneScene {
        std::vector<RxCell> cells;
        std::vector<int64_t> condensedEnergies;
    };

    // 番号 index の乱数の 0〜span − 1
    uint64_t Pick(uint32_t index, uint32_t purpose, uint64_t span) {
        return fx::FxHash64(WORLD_SEED, 0, index, purpose) % span;
    }

    // 気体の体積の割合だけ空気を減らす(凝縮相の体積 ÷ セル)
    std::vector<test::PlAmount> AirBeside(const sim::BakedReactionTable& table,
                                          const std::vector<test::PlAmount>& condensed) {
        uint64_t volume = 0;
        for (const test::PlAmount& entry : condensed)
            volume += entry.amount * table.View().Species(table.SpeciesId(entry.name)).molarVolume;

        const uint64_t cellVolume = 125000000000000ull;
        const uint64_t open = volume < cellVolume ? cellVolume - volume : cellVolume / 100;
        std::vector<test::PlAmount> air = {{"nitrogen", test::PL_AIR_NITROGEN_MICROMOL},
                                           {"oxygen", test::PL_AIR_OXYGEN_MICROMOL}};
        for (test::PlAmount& entry : air)
            entry.amount = (uint64_t)fx::FxMulDivS64((int64_t)entry.amount, open, cellVolume);

        return air;
    }

    // 凝縮相と気体をそれぞれの温度で置いたセル(混ざったセルは Ec = 凝縮相の部分のエネルギー。ほかは 0)
    void AddCell(const sim::BakedReactionTable& table, LoneScene& scene, const std::vector<test::PlAmount>& condensed,
                 uint32_t condensedMk, std::vector<test::PlAmount> gas, uint32_t gasMk) {
        const int64_t condensedMj = test::PlPartEnergyMicrojoules(table, condensed, condensedMk) / 1000;
        const int64_t gasMj = test::PlPartEnergyMicrojoules(table, gas, gasMk) / 1000;
        std::vector<test::PlAmount> all = condensed;
        all.insert(all.end(), gas.begin(), gas.end());
        const RxCell cell = test::MakePhaseLayerCell(table, all, condensedMj + gasMj);
        const bool mixed = PlLayerOf(table.View(), cell).mixed != 0;
        scene.cells.push_back(cell);
        scene.condensedEnergies.push_back(mixed ? condensedMj : 0);
    }

    // 9 種: 氷・水たまり・露(乾く)・湿った空気(露が付く)・炭の燃焼・木・満ちた石(孔の気体)・冷たい層のそばの燃える気体・空気だけ
    LoneScene MakeScene(const sim::BakedReactionTable& table) {
        LoneScene scene;
        for (uint32_t index = 0; index < CELL_COUNT; ++index) {
            const uint32_t gasMk = 260000 + (uint32_t)Pick(index, 1, 80000);
            const uint64_t amount = 1 + Pick(index, 2, 400);  // mol
            const auto air = [&](const std::vector<test::PlAmount>& condensed) {
                return AirBeside(table, condensed);
            };
            switch (index % SCENE_COUNT) {
                case 0: {
                    const std::vector<test::PlAmount> ice = {{"ice", amount * 1000000}};
                    AddCell(table, scene, ice, 250000 + (uint32_t)Pick(index, 3, 30000), air(ice), gasMk);
                    break;
                }
                case 1: {
                    const std::vector<test::PlAmount> water = {{"water", amount * 1000000}};
                    AddCell(table, scene, water, 290000 + (uint32_t)Pick(index, 3, 80000), air(water), gasMk);
                    break;
                }
                case 2: {
                    const std::vector<test::PlAmount> dew = {{"water", 100 + Pick(index, 3, 20000)}};
                    AddCell(table, scene, dew, 290000 + (uint32_t)Pick(index, 4, 60000), air(dew), 320000);
                    break;
                }
                case 3: {
                    std::vector<test::PlAmount> humid = air({});
                    humid.push_back({"water_vapor", 300000 + Pick(index, 3, 700000)});
                    AddCell(table, scene, {}, 0, humid, 270000 + (uint32_t)Pick(index, 4, 25000));
                    break;
                }
                case 4: {
                    const std::vector<test::PlAmount> carbon = {{"carbon", amount * 100000}};
                    AddCell(table, scene, carbon, 700000 + (uint32_t)Pick(index, 3, 500000), air(carbon), gasMk);
                    break;
                }
                case 5: {
                    const std::vector<test::PlAmount> wood = {{"cellulose", amount * 10000}};
                    AddCell(table, scene, wood, 500000 + (uint32_t)Pick(index, 3, 300000), air(wood), gasMk + 200000);
                    break;
                }
                case 6: {
                    const std::vector<test::PlAmount> stone = {{"stone_real", test::PL_STONE_MICROMOL}};
                    AddCell(table, scene, stone, 300000 + (uint32_t)Pick(index, 3, 100000), air(stone), gasMk);
                    break;
                }
                case 7: {
                    // 冷たい石の薄い層のそばの熱い燃える気体(気体だけの規則は気体の温度で進む = 2 回目の反応。T-1211)
                    const std::vector<test::PlAmount> stone = {{"stone_real", amount * 100000}};
                    std::vector<test::PlAmount> fuel = air(stone);
                    fuel.push_back({"test_fuel_gas", 200000 + Pick(index, 3, 800000)});
                    AddCell(table, scene, stone, 290000 + (uint32_t)Pick(index, 4, 20000), fuel,
                            900000 + (uint32_t)Pick(index, 5, 300000));
                    break;
                }
                default: AddCell(table, scene, {}, 0, air({}), gasMk); break;
            }
        }

        return scene;
    }

    // --- GPU ---------------------------------------------------------------------------------------
    struct PhasePipeline {
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12PipelineState> pipeline;
        std::array<ComPtr<ID3D12Resource>, 6>
            tables;  // 物質・規則・索引・速度・初めのセル・初めの Ec(アップロードのヒープのまま読む)
        ComPtr<ID3D12Resource> cells;
        ComPtr<ID3D12Resource> energies;
        ComPtr<ID3D12Resource> cellReadback;
        ComPtr<ID3D12Resource> energyReadback;
    };

    template <typename T>
    ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, std::span<const T> data) {
        const auto bytes = std::as_bytes(data);
        ComPtr<ID3D12Resource> buffer = gpu::CreateBuffer(device, bytes.size(), gpu::BufferKind::Upload);
        void* mapped = nullptr;
        const D3D12_RANGE noRead{.Begin = 0, .End = 0};
        if (!buffer || FAILED(buffer->Map(0, &noRead, &mapped)))
            return nullptr;

        std::memcpy(mapped, bytes.data(), bytes.size());
        buffer->Unmap(0, nullptr);

        return buffer;
    }

    std::expected<PhasePipeline, std::string> CreatePipeline(ID3D12Device5* device,
                                                             const sim::BakedReactionTable& table,
                                                             const LoneScene& scene) {
        const auto bytecode = gpu::LoadShader("sim/phase_layer_cells.cso");
        if (!bytecode)
            return std::unexpected(bytecode.error());

        PhasePipeline result;
        result.rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!result.rootSignature)
            return std::unexpected("ルート署名を作れない");

        result.pipeline = gpu::CreateComputePipeline(device, result.rootSignature.Get(), *bytecode);
        result.tables = {CreateUploadBuffer(device, std::span(table.species)),
                         CreateUploadBuffer(device, std::span(table.rules)),
                         CreateUploadBuffer(device, std::span(table.ruleIndex)),
                         CreateUploadBuffer(device, std::span(table.rates)),
                         CreateUploadBuffer(device, std::span(scene.cells)),
                         CreateUploadBuffer(device, std::span(scene.condensedEnergies))};
        const uint64_t cellBytes = std::span(scene.cells).size_bytes();
        const uint64_t energyBytes = std::span(scene.condensedEnergies).size_bytes();
        result.cells = gpu::CreateBuffer(device, cellBytes, gpu::BufferKind::UnorderedAccess);
        result.energies = gpu::CreateBuffer(device, energyBytes, gpu::BufferKind::UnorderedAccess);
        result.cellReadback = gpu::CreateBuffer(device, cellBytes, gpu::BufferKind::Readback);
        result.energyReadback = gpu::CreateBuffer(device, energyBytes, gpu::BufferKind::Readback);
        const bool tablesCreated = std::ranges::all_of(result.tables,
                                                       [](const auto& buffer) { return buffer != nullptr; });
        if (!result.pipeline || !tablesCreated || !result.cells || !result.energies || !result.cellReadback ||
            !result.energyReadback)
            return std::unexpected("パイプラインかバッファを作れない");

        return result;
    }

    // 1 区間を GPU で進め、セルと Ec を読み戻す
    std::expected<LoneScene, std::string> RunSegment(gpu::ImmediateQueue& queue, const PhasePipeline& pipeline,
                                                     gpu::DebugRing& debugRing, uint32_t segment) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        const uint64_t tickBegin = SegmentTickBegin(segment);
        const std::array<uint32_t, 7> constants = {CELL_COUNT,
                                                   TICKS_PER_SEGMENT,
                                                   static_cast<uint32_t>(tickBegin),
                                                   static_cast<uint32_t>(tickBegin >> 32),
                                                   static_cast<uint32_t>(WORLD_SEED),
                                                   static_cast<uint32_t>(WORLD_SEED >> 32),
                                                   segment == 0 ? 1u : 0u};
        debugRing.RecordBegin(list);
        list->SetComputeRootSignature(pipeline.rootSignature.Get());
        list->SetPipelineState(pipeline.pipeline.Get());
        list->SetComputeRootUnorderedAccessView(0, pipeline.cells->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1, pipeline.energies->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), 7, constants.data(), 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing.GpuAddress());
        for (uint32_t i = 0; i < pipeline.tables.size(); ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i), pipeline.tables[i]->GetGPUVirtualAddress());

        list->Dispatch(CELL_COUNT / THREADS_PER_GROUP, 1, 1);
        gpu::RecordCopyToReadback(list, pipeline.cells.Get(), pipeline.cellReadback.Get());
        gpu::RecordCopyToReadback(list, pipeline.energies.Get(), pipeline.energyReadback.Get());
        debugRing.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = debugRing.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        LoneScene result;
        result.cells.resize(CELL_COUNT);
        result.condensedEnergies.resize(CELL_COUNT);
        if (!gpu::ReadBuffer(pipeline.cellReadback.Get(), std::as_writable_bytes(std::span(result.cells))) ||
            !gpu::ReadBuffer(pipeline.energyReadback.Get(),
                             std::as_writable_bytes(std::span(result.condensedEnergies))))
            return std::unexpected("読み戻せない");

        return result;
    }

    // --- CPU と比べる ---------------------------------------------------------------------------------
    struct Coverage {
        uint32_t becameMixed = 0;    // 区間の初めに混ざらず、終わりに混ざったセル
        uint32_t becameUnmixed = 0;  // 区間の初めに混ざり、終わりに混ざらないセル
        uint32_t mixedCells = 0;     // 区間の終わりに混ざったセル(累計)
        uint32_t gasRuleCells = 0;   // 混ざったまま気体だけの規則が進んだセル(燃える気体が減った。累計)
        // 区間の終わりに層の熱が負のセル(場面ごとの累計。0 でなければ失敗)・気体の部分の熱が負のセル(表示だけ。ADR-0497 の 5)
        std::array<uint32_t, SCENE_COUNT> negativeCondensed{};
        std::array<uint32_t, SCENE_COUNT> negativeGas{};
    };

    bool IsMixed(const sim::BakedReactionTable& table, const RxCell& cell) {
        return PlLayerOf(table.View(), cell).mixed != 0;
    }

    uint64_t AmountOf(const RxCell& cell, uint32_t speciesId) {
        const uint32_t slot = RxFindSlot(cell, speciesId);

        return slot < cell.speciesCount ? cell.amounts[slot] : 0;
    }

    // CPU のセルを 1 区間進めて GPU と比べる。食い違ったセル(保存が破れたセルも)の数を返す
    int CompareSegment(const sim::BakedReactionTable& table, LoneScene& cpu, const LoneScene& gpuResult,
                       std::span<const std::vector<uint64_t>> elements, std::span<const int64_t> energies,
                       uint32_t segment, Coverage& coverage) {
        int mismatchCount = 0;
        const uint64_t tickBegin = SegmentTickBegin(segment);
        const uint32_t fuelId = table.SpeciesId("test_fuel_gas");
        for (uint32_t index = 0; index < CELL_COUNT; ++index) {
            const bool wasMixed = IsMixed(table, cpu.cells[index]);
            const uint64_t fuelBefore = AmountOf(cpu.cells[index], fuelId);
            const PlLoneCell lone = PlAdvanceLoneCell(
                table.View(), PlMakeLoneCell(cpu.cells[index], cpu.condensedEnergies[index], tickBegin - 1), WORLD_SEED,
                tickBegin, TICKS_PER_SEGMENT, index);
            cpu.cells[index] = lone.cell;
            cpu.condensedEnergies[index] = lone.condensedEnergy;

            const bool mixed = IsMixed(table, lone.cell);
            coverage.becameMixed += !wasMixed && mixed ? 1 : 0;
            coverage.becameUnmixed += wasMixed && !mixed ? 1 : 0;
            coverage.mixedCells += mixed ? 1 : 0;
            coverage.gasRuleCells += wasMixed && mixed && AmountOf(lone.cell, fuelId) < fuelBefore ? 1 : 0;
            if (mixed) {
                const PlParts parts = PlPartsOf(table.View(), lone.cell, lone.condensedEnergy);
                coverage.negativeCondensed[index % SCENE_COUNT] += parts.condensed.heat < 0 ? 1 : 0;
                coverage.negativeGas[index % SCENE_COUNT] += parts.gas.heat < 0 ? 1 : 0;
            }

            const bool conserved = sim::CountElements(table, lone.cell) == elements[index] &&
                                   lone.cell.energy == energies[index] && (mixed || lone.condensedEnergy == 0);
            if (!conserved) {
                Log(Channel::Reaction, Level::Error, "保存か疎の約束が破れた: 区間 {} セル {}", segment, index);
                ++mismatchCount;
            }

            const bool same = RxSameCell(lone.cell, gpuResult.cells[index]) &&
                              lone.condensedEnergy == gpuResult.condensedEnergies[index];
            if (same)
                continue;

            if (mismatchCount++ < MAX_REPORTED_MISMATCHES) {
                Log(Channel::Reaction, Level::Error,
                    "食い違い: 区間 {} セル {}(場面 {}): cpu E {}・Ec {}・成分 {} / gpu E {}・Ec {}・成分 {}", segment,
                    index, index % SCENE_COUNT, lone.cell.energy, lone.condensedEnergy, lone.cell.speciesCount,
                    gpuResult.cells[index].energy, gpuResult.condensedEnergies[index],
                    gpuResult.cells[index].speciesCount);
            }
        }

        return mismatchCount;
    }

    std::expected<uint64_t, std::string> RunAndCompare(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType,
                                                       const sim::BakedReactionTable& table) {
        LoneScene cpu = MakeScene(table);
        std::vector<std::vector<uint64_t>> elements;
        std::vector<int64_t> energies;
        for (const RxCell& cell : cpu.cells) {
            elements.push_back(sim::CountElements(table, cell));
            energies.push_back(cell.energy);
        }

        auto queue = gpu::ImmediateQueue::Create(device, queueType);
        if (!queue)
            return std::unexpected(queue.error());

        const auto pipeline = CreatePipeline(device, table, cpu);
        if (!pipeline)
            return std::unexpected(pipeline.error());

        auto debugRing = gpu::DebugRing::Create(device);
        if (!debugRing)
            return std::unexpected(debugRing.error());

        Coverage coverage;
        for (uint32_t segment = 0; segment < SEGMENT_COUNT; ++segment) {
            const auto started = chr::steady_clock::now();
            const auto gpuResult = RunSegment(*queue, *pipeline, *debugRing, segment);
            if (!gpuResult)
                return std::unexpected(gpuResult.error());

            const auto elapsed = chr::duration_cast<chr::milliseconds>(chr::steady_clock::now() - started);
            Log(Channel::Gpu, Level::Info, "  区間 {}: GPU {} ms", segment, elapsed.count());

            const int mismatches = CompareSegment(table, cpu, *gpuResult, elements, energies, segment, coverage);
            if (mismatches != 0)
                return std::unexpected(std::format("区間 {}(刻み {}〜)で {} セルが食い違う", segment,
                                                   SegmentTickBegin(segment), mismatches));
        }

        Log(Channel::Gpu, Level::Info,
            "  混ざり方の変わり目: 混ざった {}・混ざらなくなった "
            "{}・区間の終わりの混ざったセル(累計){}・気体だけの規則が進んだ {}",
            coverage.becameMixed, coverage.becameUnmixed, coverage.mixedCells, coverage.gasRuleCells);
        Log(Channel::Gpu, Level::Info, "  部分の熱が負のセル(場面ごと): 層 {}・気体 {}", coverage.negativeCondensed,
            coverage.negativeGas);
        if (std::ranges::any_of(coverage.negativeCondensed, [](uint32_t count) { return count != 0; }))
            return std::unexpected("層の熱が負になった(PlEndReact の下限が効いていない)");
        if (coverage.gasRuleCells == 0)
            return std::unexpected("混ざったセルで気体だけの規則が進んでいない(2 回目の反応を通っていない)");
        if (coverage.becameMixed == 0 || coverage.becameUnmixed == 0)
            return std::unexpected("混ざり方の変わり目の両方が起きていない(場面が核の道を通っていない)");

        uint64_t digest = 0;
        for (uint32_t index = 0; index < CELL_COUNT; ++index) {
            digest = fx::FxHashCombine(digest, sim::HashReactionCell(cpu.cells[index]));
            digest = fx::FxHashCombine(digest, static_cast<uint64_t>(cpu.condensedEnergies[index]));
        }

        return digest;
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_phase_layer_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_phase_layer_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const sim::BakedReactionTable table = test::MakePhaseLayerTable();
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_phase_layer_test: FAILED(デバイスを作れない)");
            return 1;
        }

        const auto digest = RunAndCompare(device->Get(), options->queueType, table);
        if (!digest) {
            Log(Channel::Gpu, Level::Error, "gpu_phase_layer_test: FAILED ({})", digest.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_phase_layer_test"))
            return 1;

        Log(Channel::Gpu, Level::Info,
            "gpu_phase_layer_test: OK(セル {} × {} 刻みで GPU と CPU がビット一致。要約 {:016x})", CELL_COUNT,
            TICKS_PER_SEGMENT * SEGMENT_COUNT, *digest);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
