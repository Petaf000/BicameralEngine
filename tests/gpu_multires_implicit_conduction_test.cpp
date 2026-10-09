// gpu_multires_implicit_conduction_test.cpp — GPU の伝導の段から細かいレベルの熱の陰解法を呼ぶ形(sim::GpuMultires の
// EnableImplicitConduction + MultiresStepOptions::implicitConduction。sim/gpu_multires_implicit。T-0132)が、CPU リファレンス
// (sim::StepNest・StepActive の AddImplicitConduction。T-0119)と毎刻みビット一致することを確かめる。
// 選んだ形は D-434 案 a(方式① は分けない + 方式② を基準より細かい Δk 1〜8)・D-436(V の上限 64 まで解き切る)。
// 場面(gpu_multires_implicit_tree_test と同じ): 熱い点(レベル 6 の根の 1 セルだけ 1500 K)・鎖(レベル 0〜6・端数の枠 16)・
// たくさんの要求(深さ 26 段・木箱が燃える・影・相変化で熱容量が変わる)。どれも全部を刻む(Compute)と活性の刻みの両方。
// 確かめること: 毎刻み、状態の全部(HashWholeNest)・活性なら次の刻みの種・陰解法の V の回数と安全網の数が CPU と一致。
// 系の大きさの上限は CPU で先に刻んだ系(captureImplicitGrid)の 2 倍 + 16 と、全部のブロックが入る見込み(GPU は CPU の系を写さない)。
// 上限を超えた刻み(T-0178。Q22 の案 A。仮): 同じ場面を上限を小さくして(ブロックが入らない・多重格子の段が入らない)CPU と GPU に
// 同じ上限を渡し、毎刻みビット一致と保存量(ComputeConservedTotals)の一致を確かめる。
// 計測(release のハードウェアだけ。--measure-only なら計測だけ): 1 刻み全体の GPU の ms(陰解法あり / なし。なしは同じ場面を陽解法の
// 頭打ちで刻む比べる相手)。同じ場面を 2 回流して 2 回目を使う。
#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "multires_activity_scene.h"
#include "multires_conduction_scene.h"
#include "sim/gpu_multires.h"
#include "sim/gpu_multires_implicit.h"
#include "sim/multires_nest.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::multires;

namespace {

#ifdef NDEBUG
    constexpr bool RELEASE = true;
#else
    constexpr bool RELEASE = false;
#endif

    constexpr uint64_t HOT_POINT_TICKS = 12;
    constexpr uint64_t CHAIN_TICKS = 36;
    constexpr uint64_t STRESS_TICKS = 8;
    constexpr uint32_t EARLY_TICKS = 12;  // 計測の「初めの刻み」の数(鎖の熱が強い間)

    constexpr int32_t CONSERVED_LEVEL = 40;  // 保存量を数える単位のレベル(どの場面の最も細かいレベルより細かい)

    enum class SceneKind : uint8_t { HotPoint, Chain, Stress };

    // 上限の形(T-0178): 全部入る / ブロックが入らない(未知数の上限を CPU の系の 1/3)/ 多重格子の段が入らない(隣を段 0 の見込みだけ)。
    // Cheapest は全部入る上限のまま、前の刻みによらず一番安い積み方で積む(多重格子の段の回は全部 LvTail〔WARP では上限まで Dispatch〕・
    // V は 1 回目の後を全部 ImTail。前の刻みより系が深い・回が多い刻みの道。T-0179)
    enum class Squeeze : uint8_t { None, Blocks, Levels, Cheapest };

    const char* SqueezeName(Squeeze squeeze) {
        switch (squeeze) {
            case Squeeze::Blocks: return "ブロックが入らない上限";
            case Squeeze::Levels: return "段が入らない上限";
            case Squeeze::Cheapest: return "一番安い積み方";
            default: return "全部入る上限";
        }
    }

    struct SceneSpec {
        const char* name = "";
        SceneKind kind = SceneKind::HotPoint;
        uint64_t ticks = 0;
    };

    // 1 刻みの CPU の結果
    struct TickCheck {
        uint64_t hash = 0;
        std::vector<uint32_t> seeds;  // 活性の時だけ
        std::vector<MrRequest> requests;
        uint32_t implicitCells = 0;
        uint32_t cycles = 0;
        uint32_t limitedCells = 0;
        uint32_t deferredBlocks = 0;  // 上限に入らず陽解法のままにしたブロック(T-0178)
        size_t gridLevels = 0;        // 多重格子の段の数
        sim::ConservedTotals conserved;
    };

    struct CpuRun {
        sim::MultiresNest initial;
        std::vector<TickCheck> ticks;
        sim::GpuMultiresImplicitLimits fitted;  // CPU が刻んだ系の大きさの最大
        sim::GpuMultiresImplicitLimits limits;  // GPU に渡す上限(上限を渡して刻んだなら同じ値)
    };

    struct Context {
        ID3D12Device5* device = nullptr;
        gpu::ImmediateQueue* queue = nullptr;
        gpu::DebugRing* ring = nullptr;
        const sim::BakedReactionTable* table = nullptr;
        uint64_t frequency = 0;
    };

    // 案 a: 方式① は分けない + 基準より細かい所は全部 ②(D-434)
    sim::MultiresStepOptions ImplicitOptions(const sim::BakedReactionTable& table, bool implicit) {
        sim::MultiresStepOptions options = test::SubcycleTestOptions(table, 0);
        options.implicitConduction = implicit;

        return options;
    }

    uint64_t SeedOf(SceneKind kind) {
        return kind == SceneKind::Stress ? test::STRESS_SEED : test::CONDUCTION_SEED;
    }

    sim::MultiresNest MakeSceneNest(const sim::BakedReactionTable& table, SceneKind kind) {
        if (kind == SceneKind::Chain)
            return test::MakeChainNest(table, 16);

        if (kind == SceneKind::Stress)
            return test::MakeActivityNest(table);

        constexpr int32_t HOT_LEVEL = 6;
        sim::MultiresCapacity capacity = test::MakeMultiresCapacity(table, 1, 0, 0);
        capacity.rootLevel = HOT_LEVEL;
        sim::MultiresNest nest = sim::MakeMultiresNest(capacity);
        std::vector<reaction::RxCell> cells(MR_BLOCK_CELLS, test::MakeConductionAir(table, 300000));
        cells[MrCellIndex(3, 4, 5)] = test::MakeConductionAir(table, 1500000);
        sim::PlaceRootBlock(nest, 0, 0, 0, cells);

        return nest;
    }

    // --- CPU ---

    void StepCpu(sim::MultiresNest& nest, const sim::BakedReactionTable& table, const SceneSpec& scene, bool active,
                 uint64_t tick, std::span<const MrRequest> requests, const sim::MultiresStepOptions& options) {
        if (scene.kind == SceneKind::Stress) {
            test::BeginActivityTick(nest, tick, requests);
            test::EndActivityTick(nest, table, tick, active, options);
            return;
        }

        if (active)
            sim::StepActive(nest, table, SeedOf(scene.kind), tick, options);
        else
            sim::StepNest(nest, table, SeedOf(scene.kind), tick, options);
    }

    // 系の上限を広げる(CPU が刻みごとに作った系の大きさ)
    void Widen(sim::GpuMultiresImplicitLimits& limits, const sim::ImplicitGrid& grid) {
        if (grid.cells.empty())
            return;

        const sim::GpuImplicitLimits fitted = sim::GpuImplicit::LimitsOf(grid);
        limits.unknowns = std::max(limits.unknowns, fitted.cells);
        limits.cells = std::max(limits.cells, fitted.cells);
        limits.nodes = std::max(limits.nodes, fitted.nodes);
        limits.links = std::max(limits.links, fitted.links);
    }

    // 全部のブロックが入る上限: CPU の系の 2 倍 + 16、セルは見込み(未知数 × 7)、隣は段 0 の見込み(未知数 × 12)まで
    sim::GpuMultiresImplicitLimits Generous(const sim::GpuMultiresImplicitLimits& fitted) {
        sim::GpuMultiresImplicitLimits limits;
        limits.unknowns = (2 * fitted.unknowns) + 16;
        limits.cells = std::max((2 * fitted.cells) + 16, 7 * limits.unknowns);
        limits.nodes = std::max((2 * fitted.nodes) + 16, limits.cells);
        limits.links = std::max((2 * fitted.links) + 16, 12 * limits.unknowns);

        return limits;
    }

    sim::GpuMultiresImplicitLimits Squeezed(const sim::GpuMultiresImplicitLimits& fitted, Squeeze squeeze) {
        sim::GpuMultiresImplicitLimits limits = Generous(fitted);
        if (squeeze == Squeeze::Blocks) {
            limits.unknowns = std::max(fitted.unknowns / 3, 1u);
            limits.cells = 7 * limits.unknowns;
        } else if (squeeze == Squeeze::Levels) {
            limits.unknowns = fitted.unknowns;
            limits.cells = 7 * limits.unknowns;
            limits.nodes = limits.cells;
            limits.links = 12 * limits.unknowns;
        }

        return limits;
    }

    // limits が null なら上限なしで刻み、CPU の系の大きさから GPU の上限(Generous)を決める
    CpuRun RunCpu(const sim::BakedReactionTable& table, const SceneSpec& scene, bool active,
                  const sim::GpuMultiresImplicitLimits* limits = nullptr) {
        CpuRun run{.initial = MakeSceneNest(table, scene.kind), .ticks = {}, .fitted = {}, .limits = {}};
        sim::MultiresNest nest = run.initial;
        nest.captureImplicitGrid = true;
        sim::MultiresStepOptions options = ImplicitOptions(table, true);
        if (limits != nullptr)
            options.implicitLimits = *limits;

        for (uint64_t tick = 0; tick < scene.ticks; ++tick) {
            TickCheck check;
            if (scene.kind == SceneKind::Stress)
                check.requests = test::MakeStressRequests(nest, tick);

            StepCpu(nest, table, scene, active, tick, check.requests, options);
            check.hash = sim::HashWholeNest(nest);
            check.seeds = active ? sim::SeedSlots(nest) : std::vector<uint32_t>{};
            check.implicitCells = nest.implicitCells;
            check.cycles = nest.implicitCost.cycles;
            check.limitedCells = nest.implicitCost.limitedCells;
            check.deferredBlocks = nest.implicitDeferredBlocks;
            check.gridLevels = nest.implicitGrid.levels.size();
            check.conserved = sim::ComputeConservedTotals(nest, table, CONSERVED_LEVEL);
            Widen(run.fitted, nest.implicitGrid);
            run.ticks.push_back(std::move(check));
        }

        run.limits = limits != nullptr ? *limits : Generous(run.fitted);

        return run;
    }

    // --- GPU ---

    // 1 刻みの GPU の記録(CPU の StepCpu と同じ順)
    bool RecordTick(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                    const SceneSpec& scene, bool active, uint64_t tick, std::span<const MrRequest> requests,
                    const sim::MultiresStepOptions& options) {
        const bool stress = scene.kind == SceneKind::Stress;
        const uint32_t shadowSlot = test::ACTIVITY_WORLD_BLOCKS;
        if (stress) {
            if (!gpu.RecordRequests(list, requests))
                return false;

            gpu.RecordProcessRequests(list, ring);
            if (tick == test::ACTIVITY_SHADOW_TICK) {
                gpu.RecordRefineShadow(list, ring, test::ACTIVITY_SHADOW_PARENT, shadowSlot,
                                       test::ACTIVITY_SHADOW_LEVELS, test::ActivityShadowPoint());
            } else if (tick == test::ACTIVITY_SHADOW_END_TICK) {
                gpu.RecordRemoveShadow(list, ring, shadowSlot, test::ACTIVITY_SHADOW_LEVELS);
            }
        }

        if (active) {
            if (!gpu.RecordStepActive(list, ring, SeedOf(scene.kind), tick, options))
                return false;
        } else {
            gpu.RecordStep(list, ring, SeedOf(scene.kind), tick, options);
        }

        if (stress && test::ActivityShadowExists(tick))
            gpu.RecordPullBack(list, ring, shadowSlot, test::ACTIVITY_SHADOW_LEVELS);

        return true;
    }

    // 1 本のリストを記録して投げ、読み戻す
    template <typename Record>
    std::expected<void, std::string> Execute(const Context& context, sim::GpuMultires& gpu, sim::MultiresNest& read,
                                             const Record& record) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        context.ring->RecordBegin(list);
        if (!record(list))
            return std::unexpected("記録できない");

        gpu.RecordReadback(list);
        context.ring->RecordReadbackAndReset(list);
        if (!context.queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = context.ring->Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        if (!gpu.Read(read))
            return std::unexpected("読み戻せない");

        return {};
    }

    std::expected<void, std::string> CompareTick(const sim::GpuMultires& gpu, const sim::MultiresNest& read,
                                                 const sim::BakedReactionTable& table, const TickCheck& check,
                                                 bool active) {
        if (sim::HashWholeNest(read) != check.hash)
            return std::unexpected("CPU と GPU の状態が食い違う");

        if (sim::ComputeConservedTotals(read, table, CONSERVED_LEVEL) != check.conserved)
            return std::unexpected("CPU と GPU の保存量が食い違う");

        if (active) {
            const auto seeds = gpu.ReadSeeds();
            if (!seeds || seeds->dropped != 0)
                return std::unexpected("種の一覧を読めない・落ちた");

            if (seeds->slots != check.seeds)
                return std::unexpected(std::format("次の刻みの種が CPU と違う(cpu {} 個 / gpu {} 個)",
                                                   check.seeds.size(), seeds->slots.size()));
        }

        // --- 陰解法の数(CPU に系の面が無かった刻みは CPU が解かないので比べない)---
        if (check.implicitCells == 0)
            return {};

        sim::GpuImplicitCost cost;
        if (gpu.ImplicitConduction() == nullptr || !gpu.ImplicitConduction()->ReadCost(cost))
            return std::unexpected("陰解法の数を読めない");

        if (!cost.limitFinished || cost.cycles != check.cycles || cost.limitedCells != check.limitedCells)
            return std::unexpected(
                std::format("陰解法の数が違う(V cpu {} / gpu {}・安全網 cpu {} / gpu {}・止まった {})", check.cycles,
                            cost.cycles, check.limitedCells, cost.limitedCells, cost.limitFinished));

        return {};
    }

    std::expected<sim::GpuMultires, std::string> MakeGpu(const Context& context, const CpuRun& run) {
        auto gpu = sim::GpuMultires::Create(context.device, *context.table, run.initial.capacity, {.activity = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        if (auto enabled = gpu->EnableImplicitConduction(context.device, run.limits); !enabled)
            return std::unexpected(enabled.error());

        return gpu;
    }

    // 上限を小さくした刻みが本当に上限に当たったか(ブロックが入らない形は 1 刻みでも陽解法のブロックがあること。段の形は数だけ記録)
    std::expected<void, std::string> CheckSqueezed(const CpuRun& full, const CpuRun& run, Squeeze squeeze,
                                                   uint32_t& deferredTicks, uint32_t& shallowerTicks) {
        deferredTicks = 0;
        shallowerTicks = 0;
        for (size_t tick = 0; tick < run.ticks.size(); ++tick) {
            deferredTicks += run.ticks[tick].deferredBlocks != 0 ? 1 : 0;
            shallowerTicks += run.ticks[tick].gridLevels < full.ticks[tick].gridLevels ? 1 : 0;
        }

        if (squeeze == Squeeze::Blocks && deferredTicks == 0)
            return std::unexpected("ブロックが入らない上限なのに、陽解法に回したブロックが無い");

        return {};
    }

    // 保存: 伝導だけの場面(熱い点・鎖)は刻みの初めから毎刻み同じ(上限で陽解法に回しても保存は守る)
    std::expected<void, std::string> CheckConservation(const sim::BakedReactionTable& table, const SceneSpec& scene,
                                                       const CpuRun& run) {
        if (scene.kind == SceneKind::Stress)
            return {};

        const sim::ConservedTotals initial = sim::ComputeConservedTotals(run.initial, table, CONSERVED_LEVEL);
        for (size_t tick = 0; tick < run.ticks.size(); ++tick) {
            if (run.ticks[tick].conserved != initial)
                return std::unexpected(std::format("刻み {} で保存量が刻みの初めと違う", tick));
        }

        return {};
    }

    std::expected<void, std::string> CheckScene(const Context& context, const SceneSpec& scene, Squeeze squeeze) {
        for (const bool active : {false, true}) {
            CpuRun run = RunCpu(*context.table, scene, active);
            const CpuRun full = run;
            uint32_t deferredTicks = 0;
            uint32_t shallowerTicks = 0;
            if (squeeze == Squeeze::Blocks || squeeze == Squeeze::Levels) {
                const sim::GpuMultiresImplicitLimits limits = Squeezed(full.fitted, squeeze);
                run = RunCpu(*context.table, scene, active, &limits);
                if (auto squeezed = CheckSqueezed(full, run, squeeze, deferredTicks, shallowerTicks); !squeezed)
                    return std::unexpected(std::format("{}({}・{}): {}", scene.name, active ? "活性" : "全部",
                                                       SqueezeName(squeeze), squeezed.error()));
            }

            if (auto conserved = CheckConservation(*context.table, scene, run); !conserved)
                return std::unexpected(std::format("{}({}・{}): {}", scene.name, active ? "活性" : "全部",
                                                   SqueezeName(squeeze), conserved.error()));

            auto gpu = MakeGpu(context, run);
            if (!gpu)
                return std::unexpected(gpu.error());

            if (squeeze == Squeeze::Cheapest) {
                gpu->ForceImplicitCheapest(true);
                Log(Channel::Gpu, Level::Info, "  {}({}・{}): 多重格子の段は {}", scene.name, active ? "活性" : "全部",
                    SqueezeName(squeeze),
                    gpu->ImplicitConduction()->LevelRoundsSelectable() ? "全部 LvTail" : "上限まで Dispatch(WARP)");
            }

            sim::MultiresStepOptions options = ImplicitOptions(*context.table, true);
            options.implicitLimits = run.limits;
            sim::MultiresNest read;
            uint32_t solved = 0;
            for (uint64_t tick = 0; tick < scene.ticks; ++tick) {
                const TickCheck& check = run.ticks[tick];
                const auto record = [&](ID3D12GraphicsCommandList10* list) {
                    if (tick == 0 && !gpu->RecordUpload(list, run.initial))
                        return false;

                    return RecordTick(list, *gpu, context.ring->GpuAddress(), scene, active, tick, check.requests,
                                      options);
                };
                if (auto executed = Execute(context, *gpu, read, record); !executed)
                    return std::unexpected(std::format("{}({}) 刻み {}: {}", scene.name, active ? "活性" : "全部", tick,
                                                       executed.error()));

                if (auto same = CompareTick(*gpu, read, *context.table, check, active); !same)
                    return std::unexpected(std::format("{}({}・{}) 刻み {}: {}", scene.name, active ? "活性" : "全部",
                                                       SqueezeName(squeeze), tick, same.error()));

                solved += check.implicitCells != 0 ? 1 : 0;
            }

            Log(Channel::Gpu, Level::Info,
                "  {}({}・{}): {} 刻み一致(陰解法を解いた刻み {}・最後の系 {} セル・V {} 回・上限 未知数 {}・セル "
                "{}・節 "
                "{}・隣 {}・陽解法に回したブロックのある刻み {}・段が浅くなった刻み {})",
                scene.name, active ? "活性" : "全部", SqueezeName(squeeze), scene.ticks, solved,
                run.ticks.back().implicitCells, run.ticks.back().cycles, run.limits.unknowns, run.limits.cells,
                run.limits.nodes, run.limits.links, deferredTicks, shallowerTicks);
        }

        return {};
    }

    // --- 計測 ---

    double Mean(std::span<const double> values) {
        if (values.empty())
            return 0.0;

        double sum = 0.0;
        for (const double value : values)
            sum += value;

        return sum / static_cast<double>(values.size());
    }

    // 1 刻みの ms: 全体と、陰解法の段の内訳(系を作る・多重格子の段・写して解く・変化を足す)
    struct TickTime {
        double total = 0.0;
        std::array<double, sim::GpuMultiresImplicit::PHASE_STAMP_COUNT - 1> phases{};
    };

    // 1 回流して刻みごとの ms(1 刻み = 1 本のリスト。読み戻しはタイムスタンプの外)
    std::expected<std::vector<TickTime>, std::string> TimeScene(const Context& context, sim::GpuMultires& gpu,
                                                                const CpuRun& run, const SceneSpec& scene, bool active,
                                                                const sim::MultiresStepOptions& options) {
        std::vector<TickTime> times;
        sim::MultiresNest read = run.initial;
        constexpr uint32_t STAMPS = sim::GpuMultiresImplicit::PHASE_STAMP_FIRST +
                                    sim::GpuMultiresImplicit::PHASE_STAMP_COUNT;
        for (uint64_t tick = 0; tick < scene.ticks; ++tick) {
            // 要求は刻みの前の GPU の木から(陰解法なしの比べでは CPU と違う木になるので)
            const std::vector<MrRequest> requests = scene.kind == SceneKind::Stress
                                                        ? test::MakeStressRequests(read, tick)
                                                        : std::vector<MrRequest>{};
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu.RecordUpload(list, run.initial))
                    return false;

                gpu.RecordTimestamp(list, 0);
                const bool recorded = RecordTick(list, gpu, context.ring->GpuAddress(), scene, active, tick, requests,
                                                 options);
                gpu.RecordTimestamp(list, 1);
                return recorded;
            };
            if (auto executed = Execute(context, gpu, read, record); !executed)
                return std::unexpected(executed.error());

            const std::vector<uint64_t> stamps = gpu.ReadTimestamps(STAMPS);
            if (stamps.size() < STAMPS || context.frequency == 0)
                return std::unexpected("タイムスタンプを読めない");

            const auto ms = [&](uint32_t from, uint32_t to) {
                return static_cast<double>(stamps[to] - stamps[from]) * 1000.0 / static_cast<double>(context.frequency);
            };
            TickTime time{.total = ms(0, 1), .phases = {}};
            if (options.implicitConduction) {
                for (uint32_t phase = 0; phase + 1 < sim::GpuMultiresImplicit::PHASE_STAMP_COUNT; ++phase) {
                    const uint32_t first = sim::GpuMultiresImplicit::PHASE_STAMP_FIRST + phase;
                    time.phases[phase] = ms(first, first + 1);
                }
            }

            times.push_back(time);
        }

        return times;
    }

    // 1 刻み目(写しと初めの記録の形)・2〜EARLY_TICKS 刻みの平均・後の刻みの平均
    void LogTimes(const SceneSpec& scene, bool active, bool implicit, std::span<const TickTime> times) {
        const size_t early = std::min<size_t>(EARLY_TICKS, times.size());
        const auto mean = [&](size_t first, size_t last, const auto& value) {
            std::vector<double> values;
            for (size_t i = first; i < last; ++i)
                values.push_back(value(times[i]));

            return Mean(values);
        };
        const auto total = [](const TickTime& time) {
            return time.total;
        };
        std::string phases;
        for (size_t phase = 0; phase < TickTime{}.phases.size(); ++phase) {
            const auto part = [phase](const TickTime& time) {
                return time.phases[phase];
            };
            phases += std::format(" {:.3f}/{:.3f}", mean(1, early, part), mean(early, times.size(), part));
        }

        Log(Channel::Gpu, Level::Info,
            "  計測 {}({}・陰解法 {}): 1 刻み目 {:.3f} ms・2〜{} 刻みの平均 {:.3f} ms・後の平均 {:.3f} ms"
            "(内訳 初め/後: 系を作る・多重格子の段・写して解く・変化を足す{})",
            scene.name, active ? "活性" : "全部", implicit ? "あり" : "なし", times.front().total, early,
            mean(1, early, total), mean(early, times.size(), total), phases);
    }

    std::expected<void, std::string> MeasureScene(const Context& context, const SceneSpec& scene) {
        for (const bool active : {false, true}) {
            const CpuRun run = RunCpu(*context.table, scene, active);
            auto gpu = MakeGpu(context, run);
            if (!gpu)
                return std::unexpected(gpu.error());

            gpu->StampImplicitPhases(true);
            for (const bool implicit : {true, false}) {
                const sim::MultiresStepOptions options = ImplicitOptions(*context.table, implicit);
                std::vector<TickTime> times;
                for (uint32_t pass = 0; pass < 2; ++pass) {  // 2 回目を使う(GPU の時計を上げる)
                    auto measured = TimeScene(context, *gpu, run, scene, active, options);
                    if (!measured)
                        return std::unexpected(measured.error());

                    times = std::move(*measured);
                }

                LogTimes(scene, active, implicit, times);
            }
        }

        return {};
    }

    std::vector<SceneSpec> Scenes() {
        return {{.name = "熱い点", .kind = SceneKind::HotPoint, .ticks = HOT_POINT_TICKS},
                {.name = "鎖", .kind = SceneKind::Chain, .ticks = CHAIN_TICKS},
                {.name = "たくさんの要求", .kind = SceneKind::Stress, .ticks = STRESS_TICKS}};
    }

    int Run(std::span<char*> arguments) {
        std::vector<char*> rest;
        bool measureOnly = false;
        for (char* argument : arguments) {
            if (std::string_view(argument) == "--measure-only")
                measureOnly = true;
            else
                rest.push_back(argument);
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_multires_implicit_conduction_test [--warp] [--queue direct|compute] [--measure-only]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_multires_implicit_conduction_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_implicit_conduction_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error,
                "gpu_multires_implicit_conduction_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        Context context{.device = device->Get(), .queue = &*queue, .ring = &*ring, .table = &*table, .frequency = 0};
        if (RELEASE && options->adapter != gpu::AdapterKind::Warp) {
            if (FAILED(queue->Native()->GetTimestampFrequency(&context.frequency)))
                context.frequency = 0;

            for (const SceneSpec& scene : Scenes()) {
                if (auto measured = MeasureScene(context, scene); !measured) {
                    Log(Channel::Gpu, Level::Error, "gpu_multires_implicit_conduction_test: FAILED(計測 {}: {})",
                        scene.name, measured.error());
                    return 1;
                }
            }
        }

        if (measureOnly)
            return 0;

        for (const Squeeze squeeze : {Squeeze::None, Squeeze::Blocks, Squeeze::Levels, Squeeze::Cheapest}) {
            for (const SceneSpec& scene : Scenes()) {
                if (auto checked = CheckScene(context, scene, squeeze); !checked) {
                    Log(Channel::Gpu, Level::Error, "gpu_multires_implicit_conduction_test: FAILED ({})",
                        checked.error());
                    return 1;
                }
            }
        }

        if (!test::PassesValidation(*device, "gpu_multires_implicit_conduction_test"))
            return 1;

        Log(Channel::Gpu, Level::Info,
            "gpu_multires_implicit_conduction_test: OK(伝導の段から呼ぶ陰解法が全部・活性の刻みで CPU "
            "と毎刻みビット一致。上限を超えた刻みも)");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
