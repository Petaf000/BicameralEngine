// gpu_lab_box_test.cpp — 実験室(sim/lab_session。T-0142)を GPU で走らせる: 箱の真ん中に木を置き(刻み 0)、1 つに火を付けて
// (刻み 3)200 刻み、毎刻み GPU と CPU リファレンスの状態の全部がビット一致すること。燃えたこと(GPU の箱でセルロースが減った)。
// 記録(コマンドの列 + ハッシュの列)を読み書きして初めの箱から流し直すと、同じハッシュの列になること(記録・再生)。
// 引数は gpu_test_options.h。
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu_test_options.h"
#include "sim/lab_session.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;

namespace {

    constexpr uint32_t IGNITE_TICK = 3;
    constexpr uint32_t RUN_TICKS = 200;
    constexpr uint32_t IGNITE_MILLIKELVIN = 1500000;

    uint64_t TotalOf(const sim::MultiresNest& nest, uint32_t species) {
        uint64_t total = 0;
        for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
            const reaction::RxCell cell = sim::LabBoxCell(nest, index);
            for (uint32_t i = 0; i < cell.speciesCount; ++i)
                total += cell.species[i] == species ? cell.amounts[i] : 0;
        }

        return total;
    }

    std::expected<void, std::string> CheckNoMismatch(const sim::LabSession& session) {
        if (const auto& mismatch = session.Mismatch(); mismatch)
            return std::unexpected(std::format("CPU と GPU が食い違う: {}", mismatch->what));

        return {};
    }

    // 木を置いて火を付ける実験
    std::expected<void, std::string> RunFire(sim::LabSession& session) {
        const std::vector<sim::LabMaterial> materials = sim::MakeLabMaterials(session.Table());
        const auto wood = std::ranges::find_if(materials, [](const sim::LabMaterial& m) { return m.name == "木"; });
        if (wood == materials.end())
            return std::unexpected("木の材料が無い");

        for (uint32_t i = 0; i < 8; ++i) {
            const sim::LabCellPosition cell{.x = 3 + (i & 1u), .y = 3 + ((i >> 1) & 1u), .z = 3 + (i >> 2)};
            if (!session.Place(cell, wood->contents, 300000))
                return std::unexpected("置けない");
        }

        if (auto stepped = session.Step(IGNITE_TICK); !stepped)
            return stepped;

        if (!session.SetTemperature({.x = 3, .y = 3, .z = 3}, IGNITE_MILLIKELVIN))
            return std::unexpected("温度を決められない");

        if (auto stepped = session.Step(RUN_TICKS - IGNITE_TICK); !stepped)
            return stepped;

        if (auto checked = CheckNoMismatch(session); !checked)
            return checked;

        const uint32_t cellulose = session.Table().SpeciesId("cellulose");
        const uint64_t placed = 8 * wood->contents[0].amount;
        const uint64_t left = TotalOf(session.Gpu(), cellulose);
        Log(Channel::Gpu, Level::Info, "gpu_lab_box_test: {} 刻みでセルロース {} → {} µmol(GPU)", RUN_TICKS, placed,
            left);
        if (left >= placed)
            return std::unexpected("燃えていない");

        return {};
    }

    std::expected<void, std::string> RunReplay(sim::LabSession& session) {
        const sim::LabRecording recorded = session.Recording();
        const auto parsed = sim::ParseLabRecording(sim::SerializeLabRecording(recorded));
        if (!parsed)
            return std::unexpected(parsed.error());

        if (auto replayed = session.Replay(*parsed); !replayed)
            return replayed;

        if (auto checked = CheckNoMismatch(session); !checked)
            return checked;

        if (session.ReplayDivergence())
            return std::unexpected(std::format("再生が記録と刻み {} で違う", *session.ReplayDivergence()));

        const sim::LabRecording again = session.Recording();
        if (again.tickCount != recorded.tickCount || again.hashes != recorded.hashes ||
            again.commands != recorded.commands)
            return std::unexpected("再生した記録が元と違う");

        return {};
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_lab_box_test [--warp] [--queue direct|compute]");
            return 2;
        }

        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto session = sim::LabSession::Create(device->Get(), options->queueType, *table);
        if (!session) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED({})", session.error());
            return 1;
        }

        if (auto result = RunFire(*session); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(実験: {})", result.error());
            return 1;
        }

        if (auto result = RunReplay(*session); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_lab_box_test: FAILED(再生: {})", result.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_lab_box_test"))
            return 1;

        Log(Channel::Gpu, Level::Info,
            "gpu_lab_box_test: OK(実験室の箱の GPU と CPU が {} 刻みビット一致・記録から流し直しても同じ)", RUN_TICKS);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
