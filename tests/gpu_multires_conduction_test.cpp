// gpu_multires_conduction_test.cpp — 多重解像度の木の上の熱の伝導(sim::GpuMultires の RecordStep・RecordStepActive に
// MultiresStepOptions{.conduction = true}。Compute の段 shaders/sim/multires_conduct.hlsl)を GPU で走らせ、CPU リファレンス
// (sim::StepNest・StepActive)と毎刻みビット一致することを確かめる(T-0107)。
// 場面: 鎖(tests/multires_conduction_scene.h。熱が 6 段をまたいで根へ流れる)を全部刻む・活性だけ刻む、
// たくさんの要求の場面(tests/multires_activity_scene.h。深さ 26 段・木箱が燃える・影・端数の枠が足りなくなる)を活性だけ・全部。
// 状態の全部(見出し・セル・端数・空きのスタック・帳簿・数える欄)と次の刻みの種(活性)を毎刻み比べる。活性の場面は、伝導の段を
// Compute で投げる版と Work Graph で投げる版(GpuMultiresOptions::conductionGraph)の両方で走らせ、結果が同じことも確かめる。
// 活性のグラフは debug の GPU-based validation で作るのに数分かかるので、全部の場面で同じ GpuMultires を使い回す(大きさを揃える)。
// 計測(release のハードウェアだけ。D-302): 根 8³ の世界で、活性の刻み(伝導の段が Compute / Work Graph)と全部を刻む刻みの GPU 時間。
// --subcycle: 細かいレベルの熱の刻み(T-0109。MultiresStepOptions の subcycleBaseLevel・maxSubcycleGap = 3 で小刻み 64 回)で同じ場面を
// 刻む(刻みの数は CPU が重いので減らす)。Work Graph 版は、Compute 版の時に刻んだ CPU の結果(ハッシュと種)と比べて CPU を 2 回刻まない。
// 計測は、根 8³ の世界(全部が基準以下のレベル = 小刻み 63 回が空。空の段の費用)を分けない・64 回で、鎖(レベル 0〜6)を Δkmax 0〜3 で比べる。
// --near-fold: 鎖を、許容差つきで頁を畳む(端数の枠を返す)・静かな葉を粗くするも入れて、頁が全部畳まれるまで毎刻み比べる(T-0112。
// CPU は multires_conduction_test の --residue と同じ順。熱が止まって静かになるまで約 2000 刻みかかる)。
// 引数は gpu_test_options.h と --subcycle・--measure-only(計測だけで抜ける。T-0111)・--near-fold。
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "multires_activity_scene.h"
#include "multires_conduction_scene.h"
#include "sim/gpu_multires.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace {

    constexpr sim::MultiresStepOptions CONDUCTION = {.conduction = true, .cutoffRounding = true};
    constexpr uint64_t CHAIN_TICKS = 80;
    constexpr uint64_t STRESS_ACTIVE_TICKS = 30;
    constexpr uint64_t STRESS_FULL_TICKS = 12;
    constexpr uint32_t WARMUP_STEPS = 400;     // 計測の前に刻みを何回投げるか(GPU のクロックを上げる)
    constexpr uint32_t MEASURE_ROOT_EDGE = 8;  // 計測の世界は根 8³ = 512 個(32m 角)
    constexpr uint32_t MEASURE_TICKS = 8;      // 最初の刻み(全部の根が種)の後に測る刻みの数

    // --- 細かいレベルの刻み(T-0109。--subcycle)---
    constexpr uint64_t SUBCYCLE_CHAIN_TICKS = 40;
    constexpr uint64_t SUBCYCLE_STRESS_ACTIVE_TICKS = 8;
    constexpr uint64_t SUBCYCLE_STRESS_FULL_TICKS = 6;
    constexpr uint32_t SUBCYCLE_CHAIN_FRACTIONS = 16;  // 計測の鎖の端数の枠の数(CPU の multires_subcycle_test と同じ)

    // 場面 1 つの進め方
    struct SceneRun {
        const char* name = "";
        bool stress = false;  // たくさんの要求の場面(要求・影)か、鎖か
        bool active = false;  // 活性だけ刻むか、全部か
        bool graph = false;   // 活性の伝導の段を Work Graph で投げるか
        bool replay = false;  // CPU を刻まず、前の版の時に刻んだ CPU の結果(TickCheck)と比べる
        uint64_t ticks = 0;
        sim::MultiresStepOptions step = CONDUCTION;
    };

    // 1 刻みの CPU の結果(Work Graph 版を、Compute 版の時に刻んだ CPU と比べるため)
    struct TickCheck {
        uint64_t hash = 0;
        std::vector<uint32_t> seeds;  // 活性の時だけ
    };

    // 1 刻みの GPU の記録(CPU の StepTick と同じ順)
    bool RecordTick(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                    const SceneRun& run, uint64_t tick, std::span<const MrRequest> requests) {
        if (!run.stress) {
            if (run.active)
                return gpu.RecordStepActive(list, ring, test::CONDUCTION_SEED, tick, run.step);

            gpu.RecordStep(list, ring, test::CONDUCTION_SEED, tick, run.step);
            return true;
        }

        if (!gpu.RecordRequests(list, requests))
            return false;

        const uint32_t shadowSlot = test::ACTIVITY_WORLD_BLOCKS;
        gpu.RecordProcessRequests(list, ring);
        if (tick == test::ACTIVITY_SHADOW_TICK) {
            gpu.RecordRefineShadow(list, ring, test::ACTIVITY_SHADOW_PARENT, shadowSlot, test::ACTIVITY_SHADOW_LEVELS,
                                   test::ActivityShadowPoint());
        } else if (tick == test::ACTIVITY_SHADOW_END_TICK) {
            gpu.RecordRemoveShadow(list, ring, shadowSlot, test::ACTIVITY_SHADOW_LEVELS);
        }

        if (run.active) {
            if (!gpu.RecordStepActive(list, ring, test::STRESS_SEED, tick, run.step))
                return false;
        } else {
            gpu.RecordStep(list, ring, test::STRESS_SEED, tick, run.step);
        }

        if (test::ActivityShadowExists(tick))
            gpu.RecordPullBack(list, ring, shadowSlot, test::ACTIVITY_SHADOW_LEVELS);

        return true;
    }

    // 1 刻みの CPU(RecordTick と同じ順)
    void StepTick(sim::MultiresNest& nest, const sim::BakedReactionTable& table, const SceneRun& run, uint64_t tick,
                  std::span<const MrRequest> requests) {
        if (run.stress) {
            test::BeginActivityTick(nest, tick, requests);
            test::EndActivityTick(nest, table, tick, run.active, run.step);
            return;
        }

        if (run.active)
            sim::StepActive(nest, table, test::CONDUCTION_SEED, tick, run.step);
        else
            sim::StepNest(nest, table, test::CONDUCTION_SEED, tick, run.step);
    }

    // 最初に食い違った所をログに出す
    void ReportFirstMismatch(const sim::MultiresNest& cpu, const sim::MultiresNest& gpu, uint64_t tick) {
        for (size_t slot = 0; slot < cpu.blocks.size(); ++slot) {
            const MrBlock& a = cpu.blocks[slot];
            const MrBlock& b = gpu.blocks[slot];
            if (std::memcmp(&a, &b, sizeof(MrBlock)) != 0) {
                Log(Channel::Sim, Level::Error,
                    "刻み {}: 枠 {} の見出しが違う(頁 cpu {} / gpu {}・端数 cpu {} / gpu {}・印 cpu {} / gpu "
                    "{}・忙しさ cpu {} / "
                    "gpu {})",
                    tick, slot, a.page, b.page, a.fraction, b.fraction, a.activeTick, b.activeTick, a.busyTick,
                    b.busyTick);
            }
        }

        for (size_t i = 0; i < cpu.cells.size(); ++i) {
            if (sim::HashReactionCell(cpu.cells[i]) == sim::HashReactionCell(gpu.cells[i]))
                continue;

            Log(Channel::Sim, Level::Error, "刻み {}: セル {} が違う(エネルギー cpu {} / gpu {})", tick, i,
                cpu.cells[i].energy, gpu.cells[i].energy);
            break;
        }

        for (size_t i = 0; i < cpu.fractions.size(); ++i) {
            if (cpu.fractions[i].energy == gpu.fractions[i].energy)
                continue;

            Log(Channel::Sim, Level::Error, "刻み {}: 端数 {} のエネルギーが違う(cpu {} / gpu {})", tick, i,
                cpu.fractions[i].energy, gpu.fractions[i].energy);
            break;
        }

        for (size_t i = 0; i < cpu.counters.size(); ++i) {
            if (cpu.counters[i] != gpu.counters[i])
                Log(Channel::Sim, Level::Error, "刻み {}: 数える欄 {} が違う(cpu {} / gpu {})", tick, i,
                    cpu.counters[i], gpu.counters[i]);
        }
    }

    // 1 本のリストを記録して投げ、読み戻す
    template <typename Record>
    std::expected<void, std::string> Execute(gpu::ImmediateQueue& queue, gpu::DebugRing& ring, sim::GpuMultires& gpu,
                                             sim::MultiresNest& read, const Record& record) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        ring.RecordBegin(list);
        if (!record(list))
            return std::unexpected("記録できない");

        gpu.RecordReadback(list);
        ring.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = ring.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        if (!gpu.Read(read))
            return std::unexpected("読み戻せない");

        return {};
    }

    // 状態の全部と、活性なら次の刻みの種
    std::expected<void, std::string> Compare(const sim::MultiresNest& cpu, const sim::MultiresNest& read,
                                             const sim::GpuMultires& gpu, const SceneRun& run, uint64_t tick) {
        if (sim::HashWholeNest(cpu) != sim::HashWholeNest(read)) {
            ReportFirstMismatch(cpu, read, tick);
            return std::unexpected("CPU と GPU が食い違う");
        }

        if (!run.active)
            return {};

        const auto seeds = gpu.ReadSeeds();
        if (!seeds || seeds->dropped != 0)
            return std::unexpected("種の一覧を読めない・落ちた");

        if (seeds->slots != sim::SeedSlots(cpu))
            return std::unexpected(std::format("次の刻みの種が CPU と違う(cpu {} 個 / gpu {} 個)",
                                               sim::SeedSlots(cpu).size(), seeds->slots.size()));

        return {};
    }

    // 前の版の時に刻んだ CPU の結果と比べる(食い違った所は出せない。出すなら Compute 版の方で)
    std::expected<void, std::string> CompareReplay(const sim::MultiresNest& read, const sim::GpuMultires& gpu,
                                                   const SceneRun& run, const TickCheck& check) {
        if (sim::HashWholeNest(read) != check.hash)
            return std::unexpected("CPU(前の版の時に刻んだもの)と GPU が食い違う");

        if (!run.active)
            return {};

        const auto seeds = gpu.ReadSeeds();
        if (!seeds || seeds->dropped != 0)
            return std::unexpected("種の一覧を読めない・落ちた");

        if (seeds->slots != check.seeds)
            return std::unexpected(std::format("次の刻みの種が CPU と違う(cpu {} 個 / gpu {} 個)", check.seeds.size(),
                                               seeds->slots.size()));

        return {};
    }

    sim::MultiresNest MakeSceneNest(const sim::BakedReactionTable& table, const SceneRun& run) {
        if (run.stress)
            return test::MakeActivityNest(table);

        // 鎖も、たくさんの要求の場面と同じ大きさの木に置く(GpuMultires を使い回すため)
        return test::MakeChainNest(table, test::MakeActivityNest(table).capacity);
    }

    // 1 刻みを GPU で投げて読み戻し、CPU と比べる(replay なら CPU を刻まず checks と比べる。そうでなければ checks に書く)
    std::expected<void, std::string> RunTick(gpu::ImmediateQueue& queue, gpu::DebugRing& ring, sim::GpuMultires& gpu,
                                             const sim::BakedReactionTable& table, const SceneRun& run, uint64_t tick,
                                             sim::MultiresNest& cpu, sim::MultiresNest& read,
                                             std::vector<TickCheck>& checks) {
        // --- 要求は刻みの前の木から(replay では読み戻した GPU の木。前の刻みまでは CPU と一致している)---
        const sim::MultiresNest& before = run.replay && tick > 0 ? read : cpu;
        const std::vector<MrRequest> requests = run.stress ? test::MakeStressRequests(before, tick)
                                                           : std::vector<MrRequest>{};
        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            if (tick == 0 && !gpu.RecordUpload(list, cpu))
                return false;

            return RecordTick(list, gpu, ring.GpuAddress(), run, tick, requests);
        };
        if (auto executed = Execute(queue, ring, gpu, read, record); !executed)
            return executed;

        if (run.replay) {
            if (tick >= checks.size())
                return std::unexpected("前の版の CPU の結果が足りない");

            return CompareReplay(read, gpu, run, checks[tick]);
        }

        StepTick(cpu, table, run, tick, requests);
        if (auto compared = Compare(cpu, read, gpu, run, tick); !compared)
            return compared;

        checks.push_back(
            {.hash = sim::HashWholeNest(cpu), .seeds = run.active ? sim::SeedSlots(cpu) : std::vector<uint32_t>{}});

        return {};
    }

    std::expected<uint64_t, std::string> RunScene(gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                  sim::GpuMultires& gpu, const sim::BakedReactionTable& table,
                                                  const SceneRun& run, std::vector<TickCheck>& checks) {
        sim::MultiresNest cpu = MakeSceneNest(table, run);
        sim::MultiresNest read;
        if (!gpu.UseConductionGraph(run.graph))
            return std::unexpected("伝導の段の Work Graph が無い");

        if (!run.replay)
            checks.clear();

        for (uint64_t tick = 0; tick < run.ticks; ++tick) {
            if (auto ran = RunTick(queue, ring, gpu, table, run, tick, cpu, read, checks); !ran)
                return std::unexpected(std::format("{} 刻み {}: {}", run.name, tick, ran.error()));
        }

        const sim::MultiresNest& last = run.replay ? read : cpu;
        Log(Channel::Gpu, Level::Info,
            "{}: {} 刻み一致(刻んだブロック {}・頁に広げた {}・端数の枠の残り {}・端数の不足 {}・頁の不足 {})",
            run.name, run.ticks, last.counters[MR_COUNTER_SCHEDULED], last.counters[MR_COUNTER_EXPANDED],
            last.counters[MR_COUNTER_FREE_FRACTIONS], last.counters[MR_COUNTER_FRACTION_SHORTAGE],
            last.counters[MR_COUNTER_PAGE_SHORTAGE]);

        return sim::HashWholeNest(last);
    }

    // --- 許容差つきで畳む鎖(T-0112。--near-fold)---
    constexpr uint64_t NEAR_FOLD_MAX_TICKS = 4000;
    constexpr uint64_t NEAR_FOLD_EXTRA_TICKS = 16;  // 頁が全部畳まれた後も刻む数(畳んだ後に何も起きないこと)
    constexpr MrFoldTolerance NEAR_FOLD_TOLERANCE = {.temperatureMk = 100, .amountShift = 20};

    // 1 刻みの GPU(CPU の NearFoldTick と同じ順)
    bool RecordNearFoldTick(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                            uint64_t tick) {
        if (!gpu.RecordRequests(list, {}))
            return false;

        gpu.RecordFoldPages(list, ring, tick, NEAR_FOLD_TOLERANCE);
        gpu.RecordQuietRequests(list, ring, tick);
        gpu.RecordProcessRequests(list, ring);

        return gpu.RecordStepActive(list, ring, test::CONDUCTION_SEED, tick, CONDUCTION);
    }

    // 1 刻みの CPU。畳む段で返した端数の枠の数を返す
    uint32_t NearFoldTick(sim::MultiresNest& nest, const sim::BakedReactionTable& table, uint64_t tick) {
        const uint32_t fractionBlocks = test::CountFractionBlocks(nest);
        sim::FoldQuietPages(nest, table, tick, NEAR_FOLD_TOLERANCE);
        const uint32_t returned = fractionBlocks - test::CountFractionBlocks(nest);
        sim::SubmitQuietCoarsenRequests(nest, tick);
        sim::ProcessRequests(nest);
        sim::StepActive(nest, table, test::CONDUCTION_SEED, tick, CONDUCTION);

        return returned;
    }

    std::expected<void, std::string> RunNearFoldChain(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                      gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = test::MakeChainNest(table, 16);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, {.activity = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        const SceneRun run{.name = "許容差つきで畳む鎖", .active = true};
        sim::MultiresNest read;
        uint32_t returned = 0;
        uint64_t foldedTick = NEAR_FOLD_MAX_TICKS;  // 頁が全部畳まれた刻み
        uint64_t tick = 0;
        for (; tick < NEAR_FOLD_MAX_TICKS && tick < foldedTick + NEAR_FOLD_EXTRA_TICKS; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordNearFoldTick(list, *gpu, ring.GpuAddress(), tick);
            };
            if (auto executed = Execute(queue, ring, *gpu, read, record); !executed)
                return std::unexpected(std::format("{} 刻み {}: {}", run.name, tick, executed.error()));

            returned += NearFoldTick(cpu, table, tick);
            if (auto compared = Compare(cpu, read, *gpu, run, tick); !compared)
                return std::unexpected(std::format("{} 刻み {}: {}", run.name, tick, compared.error()));

            if (foldedTick == NEAR_FOLD_MAX_TICKS && cpu.counters[MR_COUNTER_FOLDED] > 0 &&
                sim::UsedWorldPages(cpu) == 0)
                foldedTick = tick;
        }

        Log(Channel::Gpu, Level::Info,
            "{}: {} 刻み一致(許容差 {} mK・量 >> {}。畳んだ頁 {}・頁が全部畳まれた刻み {}・返した端数の枠 {}・"
            "端数の枠の残り {}・端数の枠を持つブロック {})",
            run.name, tick, NEAR_FOLD_TOLERANCE.temperatureMk, NEAR_FOLD_TOLERANCE.amountShift,
            cpu.counters[MR_COUNTER_FOLDED], static_cast<int64_t>(foldedTick), returned,
            cpu.counters[MR_COUNTER_FREE_FRACTIONS], test::CountFractionBlocks(cpu));
        if (foldedTick == NEAR_FOLD_MAX_TICKS || returned == 0)
            return std::unexpected("場面の確認: 頁が全部畳まれる・端数の枠を返す、になっていない");

        return {};
    }

    // 版の並び(活性の場面は伝導の段が Compute の版〔偶数番〕と Work Graph の版〔奇数番〕を並べる。subcycle なら Work Graph 版は
    // Compute 版の CPU の結果と比べる)
    std::array<SceneRun, 6> MakeRuns(const sim::BakedReactionTable& table, bool subcycle) {
        if (!subcycle) {
            return {SceneRun{.name = "鎖(活性・Compute)", .active = true, .ticks = CHAIN_TICKS},
                    SceneRun{.name = "鎖(活性・Work Graph)", .active = true, .graph = true, .ticks = CHAIN_TICKS},
                    SceneRun{.name = "たくさんの要求(活性・Compute)",
                             .stress = true,
                             .active = true,
                             .ticks = STRESS_ACTIVE_TICKS},
                    SceneRun{.name = "たくさんの要求(活性・Work Graph)",
                             .stress = true,
                             .active = true,
                             .graph = true,
                             .ticks = STRESS_ACTIVE_TICKS},
                    SceneRun{.name = "鎖(全部)", .ticks = CHAIN_TICKS},
                    SceneRun{.name = "たくさんの要求(全部)", .stress = true, .ticks = STRESS_FULL_TICKS}};
        }

        sim::MultiresStepOptions split = test::SubcycleTestOptions(table, sim::MULTIRES_MAX_SUBCYCLE_GAP);
        split.cutoffRounding = true;  // GPU はまだ今までの丸め(T-0121)
        return {
            SceneRun{.name = "小刻みの鎖(活性・Compute)", .active = true, .ticks = SUBCYCLE_CHAIN_TICKS, .step = split},
            SceneRun{.name = "小刻みの鎖(活性・Work Graph)",
                     .active = true,
                     .graph = true,
                     .replay = true,
                     .ticks = SUBCYCLE_CHAIN_TICKS,
                     .step = split},
            SceneRun{.name = "小刻みのたくさんの要求(活性・Compute)",
                     .stress = true,
                     .active = true,
                     .ticks = SUBCYCLE_STRESS_ACTIVE_TICKS,
                     .step = split},
            SceneRun{.name = "小刻みのたくさんの要求(活性・Work Graph)",
                     .stress = true,
                     .active = true,
                     .graph = true,
                     .replay = true,
                     .ticks = SUBCYCLE_STRESS_ACTIVE_TICKS,
                     .step = split},
            SceneRun{.name = "小刻みの鎖(全部)", .ticks = SUBCYCLE_CHAIN_TICKS, .step = split},
            SceneRun{.name = "小刻みのたくさんの要求(全部)",
                     .stress = true,
                     .ticks = SUBCYCLE_STRESS_FULL_TICKS,
                     .step = split}};
    }

    // withGraph = 伝導の段の Work Graph 版も走らせる(release の WARP では最初の DispatchGraph でデバイスが失われるので外す。
    // debug の WARP とハードウェアでは一致する。WARP の JIT の不具合と推定・未確認。ADR-0013 と同じ種類)
    std::expected<void, std::string> RunAll(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                            const sim::BakedReactionTable& table, bool withGraph, bool subcycle) {
        const sim::MultiresCapacity capacity = test::MakeActivityNest(table).capacity;
        auto gpu = sim::GpuMultires::Create(device, table, capacity, {.activity = true, .conductionGraph = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        const std::array<SceneRun, 6> runs = MakeRuns(table, subcycle);
        std::array<uint64_t, runs.size()> hashes{};
        std::vector<TickCheck> checks;
        for (size_t i = 0; i < runs.size(); ++i) {
            if (runs[i].graph && !withGraph) {
                Log(Channel::Gpu, Level::Warning, "{}: 走らせない(release の WARP)", runs[i].name);
                hashes[i] = hashes[i - 1];
                continue;
            }

            auto hash = RunScene(queue, ring, *gpu, table, runs[i], checks);
            if (!hash)
                return std::unexpected(hash.error());

            hashes[i] = *hash;
        }

        if (hashes[0] != hashes[1] || hashes[2] != hashes[3])
            return std::unexpected("伝導の段の Compute 版と Work Graph 版が食い違う");

        return {};
    }

    // --- 計測(D-302)---

    struct ConductionTiming {
        std::array<double, 3> firstMs{};  // 最初の刻み(全部の根が種): 活性・Compute / 活性・Work Graph / 全部
        std::array<double, 3> laterMs{};  // その後の MEASURE_TICKS 刻みの平均
        uint32_t blocks = 0;              // 本物のブロックの数
    };

    double ToMilliseconds(const std::vector<uint64_t>& stamps, uint32_t from, uint32_t to, uint64_t frequency) {
        if (stamps.size() <= to || frequency == 0)
            return 0;

        return static_cast<double>(stamps[to] - stamps[from]) * 1000.0 / static_cast<double>(frequency);
    }

    // 計測の版 variant(0 活性・Compute / 1 活性・Work Graph / 2 全部)の 1 本のリスト(タイムスタンプ 0〜2)
    bool RecordMeasureVariant(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                              const sim::MultiresNest& initial, const sim::MultiresStepOptions& step,
                              uint32_t variant) {
        constexpr uint64_t FIRST_TICK = 10000;
        bool recorded = gpu.RecordUpload(list, initial) && gpu.UseConductionGraph(variant == 1);
        for (uint32_t i = 0; i < WARMUP_STEPS; ++i)
            gpu.RecordStep(list, ring, test::STRESS_SEED, i);

        for (uint32_t i = 0; i <= MEASURE_TICKS; ++i) {
            if (i <= 1)
                gpu.RecordTimestamp(list, i);

            const uint64_t tick = FIRST_TICK + i;
            if (variant == 2)
                gpu.RecordStep(list, ring, test::STRESS_SEED, tick, step);
            else
                recorded = recorded && gpu.RecordStepActive(list, ring, test::STRESS_SEED, tick, step);
        }

        gpu.RecordTimestamp(list, 2);

        return recorded;
    }

    // 版(0 活性・Compute / 1 活性・Work Graph / 2 全部)ごとに 1 本のリストで: 写す → 全部を刻む暖機(伝導なし)→ 印 → 最初の刻み → 印
    // → MEASURE_TICKS 刻み → 印。版の順の影響(最初に使うパイプラインの準備)を除くため、版を 2 周して 2 周目を採る
    std::expected<ConductionTiming, std::string> Measure(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                         gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                         const sim::MultiresNest& initial,
                                                         const sim::MultiresStepOptions& step) {
        auto gpu = sim::GpuMultires::Create(device, table, initial.capacity,
                                            {.activity = true, .conductionGraph = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        uint64_t frequency = 0;
        if (FAILED(queue.Native()->GetTimestampFrequency(&frequency)))
            return std::unexpected("タイムスタンプの周波数を読めない");

        constexpr uint32_t VARIANTS = 3;
        ConductionTiming timing;
        sim::MultiresNest read;
        for (uint32_t run = 0; run < 2 * VARIANTS; ++run) {
            const uint32_t variant = run % VARIANTS;
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                return RecordMeasureVariant(list, *gpu, ring.GpuAddress(), initial, step, variant);
            };
            if (auto executed = Execute(queue, ring, *gpu, read, record); !executed)
                return std::unexpected(executed.error());

            const std::vector<uint64_t> stamps = gpu->ReadTimestamps(3);
            timing.firstMs[variant] = ToMilliseconds(stamps, 0, 1, frequency);
            timing.laterMs[variant] = ToMilliseconds(stamps, 1, 2, frequency) / MEASURE_TICKS;
        }

        for (uint32_t slot = 0; slot < read.capacity.worldBlocks; ++slot)
            timing.blocks += read.blocks[slot].kind == MR_BLOCK_REAL ? 1 : 0;

        return timing;
    }

    void LogTiming(std::string_view name, const ConductionTiming& timing) {
        Log(Channel::Gpu, Level::Info,
            "GPU 時間({}・本物のブロック {} 個): 最初の刻み(全部が種)活性 Compute {:.3f}・活性 Work Graph {:.3f}・全部 "
            "{:.3f} ms / その後の平均 活性 Compute {:.3f}・活性 Work Graph {:.3f}・全部 {:.3f} ms",
            name, timing.blocks, timing.firstMs[0], timing.firstMs[1], timing.firstMs[2], timing.laterMs[0],
            timing.laterMs[1], timing.laterMs[2]);
    }

    // 細かいレベルの刻みの計測(T-0109): 根 8³(全部が基準以下 = 小刻み 63 回が空)を分けない・分ける(64 回)で、鎖(レベル 0〜6。いちばん細かい
    // レベルが毎回の小刻みで流れる)を分けない・Δkmax 1〜3 で
    std::expected<void, std::string> MeasureSubcycle(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                     gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        const sim::MultiresNest roots = test::MakeActivityNest(table, MEASURE_ROOT_EDGE);
        const sim::MultiresNest chain = test::MakeChainNest(table, SUBCYCLE_CHAIN_FRACTIONS);
        struct Case {
            const sim::MultiresNest* world;
            const char* name;
            uint32_t maxGap;
        };
        constexpr uint32_t MAX_GAP = sim::MULTIRES_MAX_SUBCYCLE_GAP;
        const std::array<Case, 6> cases = {Case{.world = &roots, .name = "根 8³", .maxGap = 0},
                                           Case{.world = &roots, .name = "根 8³", .maxGap = MAX_GAP},
                                           Case{.world = &chain, .name = "鎖", .maxGap = 0},
                                           Case{.world = &chain, .name = "鎖", .maxGap = 1},
                                           Case{.world = &chain, .name = "鎖", .maxGap = 2},
                                           Case{.world = &chain, .name = "鎖", .maxGap = MAX_GAP}};
        std::array<ConductionTiming, cases.size()> timings{};
        for (size_t i = 0; i < cases.size(); ++i) {
            const sim::MultiresStepOptions step = cases[i].maxGap == 0
                                                      ? CONDUCTION
                                                      : test::SubcycleTestOptions(table, cases[i].maxGap);
            auto timing = Measure(device, queue, ring, table, *cases[i].world, step);
            if (!timing)
                return std::unexpected(timing.error());

            timings[i] = *timing;
            LogTiming(std::format("{}・小刻み {} 回", cases[i].name, 1u << (2 * cases[i].maxGap)), *timing);
        }

        // --- 空の小刻み 1 回の費用(根 8³ は全部が基準以下なので、分けた刻みの増えた分 = 空の小刻み 63 回)---
        const auto emptySubstepUs = [&](uint32_t variant) {
            return (timings[1].laterMs[variant] - timings[0].laterMs[variant]) * 1000.0 / 63.0;
        };
        Log(Channel::Gpu, Level::Info,
            "空の小刻み 1 回(Compute 6 段 + 活性なら起こすグラフ 1 回): 活性 Compute {:.2f}・活性 Work Graph "
            "{:.2f}・全部 {:.2f} "
            "µs",
            emptySubstepUs(0), emptySubstepUs(1), emptySubstepUs(2));

        return {};
    }

    struct RunOptions {
        test::GpuTestOptions gpu;
        bool subcycle = false;
        bool measureOnly = false;  // 計測だけ(比べる実行を省く。T-0111 で段を詰める時の繰り返し用)
        bool nearFold = false;     // 許容差つきで畳む鎖だけ(T-0112)
    };

    // gpu_test_options.h の引数と --subcycle・--measure-only
    std::optional<RunOptions> ParseRunOptions(std::span<char*> arguments) {
        std::vector<char*> rest;
        RunOptions options;
        for (char* argument : arguments) {
            if (std::string_view(argument) == "--subcycle")
                options.subcycle = true;
            else if (std::string_view(argument) == "--measure-only")
                options.measureOnly = true;
            else if (std::string_view(argument) == "--near-fold")
                options.nearFold = true;
            else
                rest.push_back(argument);
        }

        const auto gpuOptions = test::ParseGpuTestOptions(rest);
        if (!gpuOptions)
            return std::nullopt;

        options.gpu = *gpuOptions;

        return options;
    }

    // 計測(release のハードウェアだけ。debug は検証の計装で意味がない)。CPU の重い比べる実行の前に(GPU が長く空くとクロックが下がる)
    std::expected<void, std::string> MeasureAll(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                const sim::BakedReactionTable& table, bool subcycle) {
        if (subcycle)
            return MeasureSubcycle(device, queue, ring, table);

        const auto timing = Measure(device, queue, ring, table, test::MakeActivityNest(table, MEASURE_ROOT_EDGE),
                                    CONDUCTION);
        if (!timing)
            return std::unexpected(timing.error());

        LogTiming("伝導あり", *timing);

        return {};
    }

    int Run(std::span<char*> arguments) {
        const auto options = ParseRunOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_multires_conduction_test [--warp] [--queue direct|compute] [--subcycle] [--measure-only] "
                "[--near-fold]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_multires_conduction_test: adapter {}, queue {}{}",
            gpu::AdapterKindName(options->gpu.adapter), test::QueueTypeName(options->gpu.queueType),
            options->subcycle ? "、小刻み" : "");
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->gpu.adapter, test::TestDeviceOptions(options->gpu));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_conduction_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->gpu.queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_conduction_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        if (options->nearFold) {
            if (auto result = RunNearFoldChain(device->Get(), *queue, *ring, *table); !result) {
                Log(Channel::Gpu, Level::Error, "gpu_multires_conduction_test: FAILED ({})", result.error());
                return 1;
            }

            if (!test::PassesValidation(*device, "gpu_multires_conduction_test"))
                return 1;

            Log(Channel::Gpu, Level::Info,
                "gpu_multires_conduction_test: OK(許容差つきで畳む鎖の GPU と CPU が毎刻みビット一致)");

            return 0;
        }

#ifdef NDEBUG
        constexpr bool RELEASE = true;
#else
        constexpr bool RELEASE = false;
#endif
        const bool warp = options->gpu.adapter == gpu::AdapterKind::Warp;
        if (RELEASE && !warp) {
            if (auto measured = MeasureAll(device->Get(), *queue, *ring, *table, options->subcycle); !measured) {
                Log(Channel::Gpu, Level::Error, "gpu_multires_conduction_test: FAILED(計測: {})", measured.error());
                return 1;
            }
        }

        if (options->measureOnly)
            return 0;

        if (auto result = RunAll(device->Get(), *queue, *ring, *table, !(RELEASE && warp), options->subcycle);
            !result) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_conduction_test: FAILED ({})", result.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_multires_conduction_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_multires_conduction_test: OK(熱の伝導の GPU と CPU が毎刻みビット一致)");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
