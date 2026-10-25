// gpu_probe_blank_test.cpp — ブランクの世界(D-455・T-0371)の GPU の刻みが CPU リファレンスとハッシュ一致し、何も起きないこと。
//
// 確かめること(空の表〔物質 0・規則 0〕と、試験の表〔法則だけで物が無い〕の 2 通り):
//   - 空の表でも ProbeSim を作れて刻みが回る(大きさ 0 の表のバッファを作らない。T-0371)
//   - 刻みごとのハッシュが CPU リファレンス(ProbeReference)と同じで、刻み 0 のまま変わらない・エネルギー 0 のまま
//   - シェーダーの assert・debug layer のエラーが 0 件
// 走らせ方: ctest(gpu・warp)か `job.py run -Exe gpu_probe_blank_test`。引数は gpu_test_options.h
#include <algorithm>
#include <format>
#include <span>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu/queue.h"
#include "gpu_test_options.h"
#include "reaction_test_table.h"
#include "script/reaction_table_loader.h"
#include "sim/probe_sim.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    constexpr uint64_t TICKS = 120;
    constexpr uint32_t TICKS_PER_FRAME = 30;
    // CPU リファレンスと突き合わせる刻み(全部のセルを毎刻み計算するので debug では遅い。残りの刻みは刻み 0 のハッシュと比べる)
    constexpr uint64_t REFERENCE_TICKS = 4;

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition)
                return;

            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    // GPU で TICKS 刻み流し、刻みごとの状態のハッシュを返す(作れない・投げられないなら空)
    std::vector<ProbeTickHash> RunGpu(ID3D12Device5* device, const BakedReactionTable& table,
                                      std::span<const reaction::RxCell> world, uint32_t& debugAssertCount) {
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"BlankTestSim");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table, {.initialWorld = world});
        if (!queue || !simulation) {
            Log(Channel::Sim, Level::Error, "作れない: {}{}", queue ? "" : queue.error(),
                simulation ? "" : simulation.error());
            return {};
        }

        std::vector<ProbeTickHash> hashes;
        const uint32_t unitsPerTick = simulation->UnitsPerTick();
        for (uint64_t tick = 0; tick < TICKS; tick += TICKS_PER_FRAME) {
            const uint64_t frame = tick / TICKS_PER_FRAME;
            const auto slot = static_cast<uint32_t>(frame % ProbeSim::FRAME_SLOT_COUNT);
            ID3D12CommandList* list = simulation->RecordFrame(
                slot, {.firstTick = tick, .unitCount = TICKS_PER_FRAME * unitsPerTick});
            if (list == nullptr || !queue->WaitCpu(queue->Submit(list)))
                return {};

            const ProbeFrameReadback readback = simulation->ReadFrame(slot);
            hashes.insert(hashes.end(), readback.hashes.begin(), readback.hashes.end());
            debugAssertCount += readback.debugAssertCount;
        }

        return hashes;
    }

    void CheckStill(ID3D12Device5* device, const BakedReactionTable& table, std::string_view name, Failures& failures) {
        const std::vector<reaction::RxCell> world = MakeBlankProbeWorld();
        ProbeReference reference(table, world);
        reference.SetCascadeRadiation({.enabled = false});  // GPU の仮の世界に放射の段が入るまで(T-1154)
        const uint64_t initialHash = ProbeStateHash(reference.State(0));

        uint32_t debugAssertCount = 0;
        const std::vector<ProbeTickHash> hashes = RunGpu(device, table, world, debugAssertCount);

        bool matches = hashes.size() == TICKS;
        bool still = matches;
        for (size_t index = 0; matches && index < hashes.size(); ++index) {
            const ProbeTickHash& gpuHash = hashes[index];
            if (gpuHash.tick <= REFERENCE_TICKS) {
                reference.Advance(gpuHash.tick - 1, {});
                matches = gpuHash.hash == reference.StateHash(gpuHash.tick);
            }

            still = still && gpuHash.hash == initialHash && gpuHash.energy == 0 && gpuHash.sourceEnergy == 0;
        }

        Log(Channel::Sim, Level::Info,
            "{}: GPU {} 刻み、CPU と一致(最初の {} 刻み){}・刻み 0 のまま {}(ハッシュ {:016x})", name, hashes.size(),
            REFERENCE_TICKS, matches ? "OK" : "NG", still ? "OK" : "NG", initialHash);
        failures.Check(matches, std::format("{}: 刻みごとのハッシュが CPU リファレンスと同じ", name));
        failures.Check(still, std::format("{}: ハッシュとエネルギーが刻み 0 のまま", name));
        failures.Check(debugAssertCount == 0, std::format("{}: シェーダーの assert が 0 件", name));
    }

}  // namespace

int main(int argc, char** argv) {
    const auto options = test::ParseGpuTestOptions(std::span(argv, static_cast<size_t>(argc)));
    const auto blank = script::LoadBlankReactionTable();
    const auto combustion = BakeReactionTable(MakeCombustionTestTable());
    if (!options || !blank || !combustion) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_probe_blank_test [gpu_test_options の引数]");
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    Failures failures;
    CheckStill(device->Get(), blank->table, "空の表", failures);
    CheckStill(device->Get(), *combustion, "試験の表(物が無い)", failures);

    const bool passed = failures.count == 0 && test::PassesValidation(*device, "gpu_probe_blank_test");
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_blank_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
