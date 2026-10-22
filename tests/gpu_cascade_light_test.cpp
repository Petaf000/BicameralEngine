// gpu_cascade_light_test.cpp — 距離ごとの段の GPU(sim/gpu_cascade_light。shaders/sim/cascade_light.hlsl)が CPU リファレンス
// (sim/cascade_light の StepCascadeLight)と刻みごとにビット一致すること(T-0959・23 §10 R-R1)。
//   焚き火と黒い受け手・石の地面の上の熱い床(照り返しの持ち越しが刻みをまたぐ)・閉じた石の箱・袋 4 本・世界の隅が負の座標 で、
//   放った・吸った(セルごと・帯ごと)・帳簿・段ごとの光線の本数・通ったセル・持ち越しが全部同じ。
//   費用(HW のみ。docs/perf.md): 48³・64³ × 照り返す地面(石)/ 照り返さない地面(黒)× 熱いセル 1・256・2304 を、
//   今の形(n0 = 4・5 段・袋 1 本)と光線を約 8 倍にした形(袋 8 本)で。1 刻みの ms・光線の本数・VRAM。熱いセル 0 は決まった費用。
// 引数は gpu_test_options.h。
#include <array>
#include <cstdio>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "sim/cascade_light.h"
#include "sim/gpu_cascade_light.h"
#include "sim/gpu_multires.h"
#include "sun_column_scene.h"

using namespace bicameral;
namespace rx = bicameral::reaction;

namespace {

    constexpr uint32_t MEASURE_WARMUP = 30;
    constexpr uint32_t MEASURE_TICKS = 10;
    constexpr uint32_t FIRE_MK = 1200000;
    constexpr uint32_t ROOM_MK = 293000;
    constexpr uint32_t QUIET_MK = 500000;  // 室温の物は放たない(cascade_light_test と同じ)

    enum class Material : uint8_t {
        Air,
        Stone,  // 通さない・照り返し 0.5
        Black,  // 通さない・照り返し 0
    };

    struct Voxel {
        Material material = Material::Air;
        uint32_t temperatureMk = 0;
    };

    using Shape = std::function<Voxel(int64_t x, int64_t y, int64_t z)>;

    rx::RcCellLight LightOf(Material material) {
        if (material == Material::Air)
            return {};

        const uint32_t reflectance = material == Material::Stone ? 32768u : 0u;

        return {.depthShortQ32 = rx::RD_DEPTH_SATURATED_Q32,
                .depthLongQ32 = rx::RD_DEPTH_SATURATED_Q32,
                .reflectanceShortQ16 = reflectance,
                .reflectanceLongQ16 = reflectance};
    }

    sim::CascadeGrid MakeGrid(int64_t side, const Shape& shape) {
        sim::CascadeGrid grid;
        grid.size = {side, side, side};
        grid.lights.resize(static_cast<size_t>(side * side * side));
        grid.temperatureMk.resize(grid.lights.size());
        for (int64_t z = 0; z < side; ++z) {
            for (int64_t y = 0; y < side; ++y) {
                for (int64_t x = 0; x < side; ++x) {
                    const Voxel voxel = shape(x, y, z);
                    const size_t at = grid.IndexOf(x, y, z);
                    grid.lights[at] = LightOf(voxel.material);
                    grid.temperatureMk[at] = voxel.temperatureMk;
                }
            }
        }

        return grid;
    }

    // 地面(y < 4、または 8)の上に熱い床 hot × hot を真ん中に
    sim::CascadeGrid FireGround(int64_t side, int64_t groundHeight, int64_t hot, Material ground) {
        return MakeGrid(side, [=](int64_t x, int64_t y, int64_t z) {
            if (y < groundHeight)
                return Voxel{ground, ROOM_MK};

            const int64_t low = (side / 2) - (hot / 2);
            const bool fire = y == groundHeight && x >= low && x < low + hot && z >= low && z < low + hot;

            return fire ? Voxel{Material::Stone, FIRE_MK} : Voxel{};
        });
    }

    struct GpuContext {
        ID3D12Device5* device = nullptr;
        gpu::ImmediateQueue* queue = nullptr;
        gpu::DebugRing* ring = nullptr;
        sim::GpuMultires* nest = nullptr;  // ルート署名とタイムスタンプを借りるだけ
    };

    struct Case {
        std::string name;
        sim::CascadeGrid grid;
        sim::CascadeParams params;
        uint32_t ticks = 3;
    };

    sim::CascadeParams Params(uint32_t baseTexels, uint32_t levels, uint32_t raysPerBag) {
        return {.baseTexels = baseTexels,
                .levels = levels,
                .intervalBlocks = 1,
                .seed = 7,
                .quietBelowMk = QUIET_MK,
                .raysPerBag = raysPerBag};
    }

    std::vector<Case> MakeCases() {
        std::vector<Case> cases;
        for (const uint32_t levels : {1u, 2u}) {
            cases.push_back({std::format("焚き火 1 つ・{} 段", levels),
                             MakeGrid(16,
                                      [](int64_t x, int64_t y, int64_t z) {
                                          const bool fire = x == 8 && y == 8 && z == 8;

                                          return fire ? Voxel{Material::Stone, FIRE_MK} : Voxel{};
                                      }),
                             Params(4, levels, 1), 1});
        }

        cases.push_back({"焚き火と黒い受け手 24³",
                         MakeGrid(24,
                                  [](int64_t x, int64_t y, int64_t z) {
                                      if (x == 12 && y == 12 && z == 12)
                                          return Voxel{Material::Stone, FIRE_MK};

                                      const bool receiver = y == 12 && ((x == 4 && z == 12) || (x == 20 && z == 15) ||
                                                                        (x == 12 && z == 3) || (x == 17 && z == 18));

                                      return receiver ? Voxel{Material::Black, 0} : Voxel{};
                                  }),
                         Params(4, 4, 1), 3});
        cases.push_back({"石の地面の上の熱い床 32³", FireGround(32, 4, 4, Material::Stone), Params(4, 5, 1), 3});
        cases.push_back({"閉じた石の箱 12³",
                         MakeGrid(12,
                                  [](int64_t x, int64_t y, int64_t z) {
                                      const bool shell = x < 2 || y < 2 || z < 2 || x >= 10 || y >= 10 || z >= 10;

                                      return shell ? Voxel{Material::Stone, 1000000} : Voxel{};
                                  }),
                         Params(2, 3, 1), 2});
        cases.push_back({"袋 4 本・n0 = 2", FireGround(32, 4, 4, Material::Stone), Params(2, 5, 4), 2});
        Case shifted{"世界の隅が負の座標", FireGround(24, 4, 3, Material::Stone), Params(4, 4, 1), 2};
        shifted.grid.low = {-5, 3, -9};
        cases.push_back(std::move(shifted));

        return cases;
    }

    // --- 実行 ---

    std::expected<sim::GpuCascadeLight, std::string> Prepare(const GpuContext& context, const sim::CascadeGrid& grid,
                                                             const sim::CascadeParams& params,
                                                             const sim::GpuCascadeCapacity& capacity) {
        auto light = sim::GpuCascadeLight::Create(context.device, *context.nest, grid, params, capacity);
        if (!light)
            return std::unexpected(light.error());

        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        const D3D12_GPU_VIRTUAL_ADDRESS ring = context.ring->GpuAddress();
        if (list == nullptr || !light->RecordUpload(list, *context.nest, ring, {}) || !context.queue->ExecuteAndWait())
            return std::unexpected("作業を写せない");

        return light;
    }

    std::expected<sim::GpuCascadeTickResult, std::string> TickOnGpu(const GpuContext& context,
                                                                    sim::GpuCascadeLight& light, uint64_t tick) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("リストを始められない");

        context.ring->RecordBegin(list);
        light.RecordTick(list, *context.nest, context.ring->GpuAddress(), tick);
        light.RecordReadback(list);
        context.ring->RecordReadbackAndReset(list);
        if (!context.queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = context.ring->Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        return light.Read();
    }

    // 最初の食い違い(無ければ空)
    std::string Describe(const sim::CascadeTickResult& result) {
        std::string text;
        for (const sim::CascadeBandLedger& band : result.ledger)
            text += std::format("[放 {} 持 {} 吸 {} 返 {} 外 {}]", band.emitted, band.carriedIn, band.absorbed,
                                band.reflected, band.escaped);

        return text + std::format(" 光線 {}・{}・{}・{}・{} 通った {}", result.raysPerLevel[0], result.raysPerLevel[1],
                                  result.raysPerLevel[2], result.raysPerLevel[3], result.raysPerLevel[4], result.steps);
    }

    std::string Difference(const sim::CascadeTickResult& cpu, const sim::CascadeTickResult& gpu) {
        if (cpu.ledger != gpu.ledger || cpu.raysPerLevel != gpu.raysPerLevel || cpu.steps != gpu.steps)
            return std::format("CPU {} / GPU {}", Describe(cpu), Describe(gpu));

        if (cpu.ledger != gpu.ledger)
            return std::format("帳簿(長い帯の吸った CPU {}・GPU {})", cpu.ledger[1].absorbed, gpu.ledger[1].absorbed);

        if (cpu.raysPerLevel != gpu.raysPerLevel || cpu.rays != gpu.rays)
            return std::format("光線の本数(CPU {}・GPU {})", cpu.rays, gpu.rays);

        if (cpu.steps != gpu.steps)
            return std::format("通ったセル(CPU {}・GPU {})", cpu.steps, gpu.steps);

        if (cpu.emitted != gpu.emitted)
            return "放った(セルごと)";

        for (size_t at = 0; at < cpu.absorbed.size(); ++at) {
            if (cpu.absorbed[at] != gpu.absorbed[at])
                return std::format("吸った(セル {}・帯 {}: CPU {}・GPU {})", at / 2, at % 2, cpu.absorbed[at],
                                   gpu.absorbed[at]);
        }

        if (cpu.carry != gpu.carry)
            return std::format("持ち越し(CPU {} 個・GPU {} 個)", cpu.carry.entries.size(), gpu.carry.entries.size());

        return {};
    }

    std::expected<uint32_t, std::string> CompareCases(const GpuContext& context) {
        uint32_t compared = 0;
        std::string failures;
        for (const Case& item : MakeCases()) {
            auto light = Prepare(context, item.grid, item.params,
                                 {.faces = 1u << 14, .bags = 1u << 18, .carry = 1u << 16});
            if (!light)
                return std::unexpected(std::format("{}: {}", item.name, light.error()));

            sim::CascadeCarry carry;
            for (uint32_t tick = 0; tick < item.ticks; ++tick) {
                const sim::CascadeTickResult cpu = sim::StepCascadeLight(item.grid, carry, item.params, tick);
                const auto gpu = TickOnGpu(context, *light, tick);
                if (!gpu)
                    return std::unexpected(std::format("{}: {}", item.name, gpu.error()));

                if (gpu->overflow != 0)
                    return std::unexpected(std::format("{}: 置き場が溢れた({})", item.name, gpu->overflow));

                const std::string difference = Difference(cpu, gpu->result);
                if (!difference.empty()) {
                    // 全部の場面を回してから落とす(どの段から食い違うかを見るため)
                    Log(Channel::Gpu, Level::Error, "{} の刻み {}: {} が食い違う", item.name, tick, difference);
                    failures += item.name + " ";
                    break;
                }

                carry = cpu.carry;
                ++compared;
            }

            Log(Channel::Gpu, Level::Info, "{}: {} 刻み GPU と CPU がビット一致(光線 {} 本/刻み・持ち越し {} 個)",
                item.name, item.ticks, sim::StepCascadeLight(item.grid, {}, item.params, 0).rays, carry.entries.size());
        }

        if (!failures.empty())
            return std::unexpected("食い違った場面: " + failures);

        return compared;
    }

    // --- 費用 ---

    struct Form {
        const char* name;
        uint32_t baseTexels;
        uint32_t raysPerBag;
    };

    std::expected<void, std::string> MeasureOne(const GpuContext& context, const char* scene,
                                                const sim::CascadeGrid& grid, const Form& form) {
        const sim::CascadeParams params = Params(form.baseTexels, 5, form.raysPerBag);
        auto light = Prepare(context, grid, params, {.faces = 1u << 14, .bags = 1u << 21, .carry = 1u << 18});
        if (!light)
            return std::unexpected(light.error());

        // --- 暖機(照り返しの持ち越しを作り、GPU のクロックを上げる)---
        const D3D12_GPU_VIRTUAL_ADDRESS ring = context.ring->GpuAddress();
        ID3D12GraphicsCommandList10* warm = context.queue->Begin();
        if (warm == nullptr)
            return std::unexpected("暖機のリストを始められない");

        for (uint32_t tick = 0; tick < MEASURE_WARMUP; ++tick)
            light->RecordTick(warm, *context.nest, ring, tick);

        if (!context.queue->ExecuteAndWait())
            return std::unexpected("暖機で回せない");

        // --- 測る(タイムスタンプは最初と最後)---
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("リストを始められない");

        context.nest->RecordTimestamp(list, 0);
        for (uint32_t tick = 0; tick < MEASURE_TICKS; ++tick)
            light->RecordTick(list, *context.nest, ring, MEASURE_WARMUP + tick);

        context.nest->RecordTimestamp(list, 1);
        context.nest->RecordReadback(list);
        light->RecordReadback(list);
        if (!context.queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        uint64_t frequency = 0;
        const std::vector<uint64_t> stamps = context.nest->ReadTimestamps(2);
        if (FAILED(context.queue->Native()->GetTimestampFrequency(&frequency)) || stamps.size() != 2 || frequency == 0)
            return std::unexpected("タイムスタンプを読めない");

        const auto read = light->Read();
        if (!read)
            return std::unexpected(read.error());

        if (read->overflow != 0)
            return std::unexpected(std::format("置き場が溢れた({})", read->overflow));

        const double ms = static_cast<double>(stamps[1] - stamps[0]) * 1000.0 /
                          (static_cast<double>(frequency) * MEASURE_TICKS);
        const std::array<uint64_t, 8>& levels = read->result.raysPerLevel;
        Log(Channel::Gpu, Level::Info,
            "費用 [{}] {}: {:.3f} ms/刻み・光線 {} 本/刻み(段ごと {}・{}・{}・{}・{})・通ったセル {}・放つ面 "
            "{}・袋の最大 {}・"
            "持ち越し {} 個・VRAM {:.1f} MB(袋の表 2 × 2^21 を含む)",
            form.name, scene, ms, read->result.rays, levels[0], levels[1], levels[2], levels[3], levels[4],
            read->result.steps, read->activeFaces, read->bagPeak, read->result.carry.entries.size(),
            static_cast<double>(light->VramBytes()) / (1024.0 * 1024.0));

        return {};
    }

    std::expected<void, std::string> Measure(const GpuContext& context) {
        constexpr std::array<Form, 2> FORMS = {Form{"今の形 n0 = 4・袋 1 本", 4, 1},
                                               Form{"光線 約 8 倍 n0 = 4・袋 8 本", 4, 8}};
        for (const Form& form : FORMS) {
            for (const int64_t side : {48, 64}) {
                for (const Material ground : {Material::Stone, Material::Black}) {
                    for (const int64_t hot : {0, 1, 16, 48}) {  // 0 = 熱いものが無い(1 刻みの決まった費用)
                        const std::string scene = std::format("{}³・{}・熱いセル {}", side,
                                                              ground == Material::Stone ? "照り返す地面" : "黒い地面",
                                                              hot * hot);
                        const sim::CascadeGrid grid = FireGround(side, 8, hot, ground);
                        if (auto measured = MeasureOne(context, scene.c_str(), grid, form); !measured)
                            return std::unexpected(std::format("{} {}: {}", form.name, scene, measured.error()));
                    }
                }
            }
        }

        return {};
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_cascade_light_test [--warp] [--queue direct|compute] [--no-gbv]");
            return 2;
        }

        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_cascade_light_test: FAILED ({})", device.error());
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_cascade_light_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        // ルート署名とタイムスタンプを借りるだけの小さな木
        const sim::BakedReactionTable table = test::MakeSunTable();
        const sim::MultiresNest tree = test::MakeSunWorld(table, 0, 1, 1, 1, test::FlatGround);
        auto nest = sim::GpuMultires::Create(device->Get(), table, tree.capacity);
        if (!nest) {
            Log(Channel::Gpu, Level::Error, "gpu_cascade_light_test: FAILED ({})", nest.error());
            return 1;
        }

        const GpuContext context{.device = device->Get(), .queue = &*queue, .ring = &*ring, .nest = &*nest};
        const auto compared = CompareCases(context);
        if (!compared) {
            Log(Channel::Gpu, Level::Error, "gpu_cascade_light_test: FAILED ({})", compared.error());
            return 1;
        }

        // 費用は WARP では省く(ビット一致は上で見た)
        if (options->adapter != gpu::AdapterKind::Warp) {
            if (auto measured = Measure(context); !measured) {
                Log(Channel::Gpu, Level::Error, "gpu_cascade_light_test: FAILED(費用: {})", measured.error());
                return 1;
            }
        }

        if (!test::PassesValidation(*device, "gpu_cascade_light_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_cascade_light_test: OK({} 刻み分の場面で GPU と CPU がビット一致)",
            *compared);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
