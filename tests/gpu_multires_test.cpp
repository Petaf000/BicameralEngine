// gpu_multires_test.cpp — 多重解像度の木(sim/gpu_multires。Compute の段・Work Graph の再帰)を GPU で走らせ、
// CPU リファレンス(sim/multires_nest)と毎刻みビット一致することを確かめる(T-0017、17 §6 の基準 1〜2。T-0018、17 §5 の木の管理)。
// 場面は tests/multires_test_scene.h(本物の鎖・影の鎖・たくさんの要求)。状態の全部(見出し・セル・端数・空きのスタック・帳簿・数える欄)を
// 毎刻み比べ、GPU の索引で本物のブロックが全部引けることも確かめる。本物の鎖とたくさんの要求は 2 回走らせて一致を確かめる。
// 時間は別に、1 本のリストで暖機(GPU のクロックを上げる)してから測る(1 刻みの予算に収まるか)。
// debug では 2 回目の実行と時間の計測をしない(1200 秒の上限に届くため。T-0263)。場面と刻みの数は release と同じ。
// 粗くすると成分が入りきらない場面(tests/multires_limits_scene.h。T-0022)も毎刻み比べる(断った数・見出しの quietCheck も)。
// 引数は gpu_test_options.h と --part all|chains|wide(ctest の HW は chains と wide の 2 本。T-0263)・--measure-wide(計測だけ)。
#include "sim/gpu_multires.h"

#include <optional>

#include "common/multires_wide.hlsli"
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "multires_limits_scene.h"
#include "multires_test_scene.h"
#include "multires_wide_scene.h"
#include "reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace {

    // 溢れの場面で段ごとのタイムスタンプを入れるか(T-0290 の切り分け。--wide-stamps)
    bool s_wideStageTimestamps = false;

    // 計測の前に刻みを何回投げるか(短い仕事の間は GPU のクロックが上がらず、時間が 6〜8 倍に出る)
    constexpr uint32_t WARMUP_STEPS = 400;

    // debug(FX_ASSERT・GPU の検証つき)では 6 つのランナーの並走で 1 本が 1200 秒の上限に届いた(T-0263)。
    // debug は同じ場面を 1 回ずつ流して CPU と比べるだけにし、2 回目の実行(決定性)と時間の計測(暖機 400 刻み × 2)は release だけでする
#ifdef NDEBUG
    constexpr bool REPEAT_AND_MEASURE = true;
#else
    constexpr bool REPEAT_AND_MEASURE = false;
#endif

    struct RunResult {
        uint64_t digest = 0;
        uint32_t freeFractions = 0;
    };

    // 鎖の出来事と刻みの GPU 時間(ms)
    struct ChainTiming {
        double createMs = 0;  // 細かくする要求の処理(本物)/ 影を作る
        double stepMs = 0;    // 鎖があるときの刻み(影なら引き戻しを含む)
        double endMs = 0;     // 粗くする要求 1 段の処理(本物)/ 影を捨てる
        double emptyMs = 0;   // 要求が 0 件の処理(本物だけ)
    };

    // 1 刻みの GPU の記録(test::StepMultiresScene と同じ順)
    bool RecordTick(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                    test::MultiresScenario scenario, uint64_t tick) {
        using test::MultiresScenario;
        const sim::MultiresPoint point = test::MakeMultiresPoint(test::MULTIRES_LEVELS);
        if (!gpu.RecordRequests(list, test::MultiresRequestsAt(scenario, tick)))
            return false;

        gpu.RecordProcessRequests(list, ring);
        if (tick == test::MULTIRES_REFINE_TICK && scenario == MultiresScenario::Shadow) {
            gpu.RecordRefineShadow(list, ring, test::MULTIRES_ROOT_SLOT, test::MULTIRES_SHADOW_SLOT,
                                   test::MULTIRES_LEVELS, point);
        } else if (tick == test::MULTIRES_COARSEN_TICK && scenario == MultiresScenario::Shadow) {
            gpu.RecordRemoveShadow(list, ring, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS);
        }

        gpu.RecordStep(list, ring, test::MULTIRES_TEST_SEED, tick);
        if (test::MultiresShadowExists(scenario, tick))
            gpu.RecordPullBack(list, ring, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS);

        return true;
    }

    // 最初に食い違った所(枠・セル)をログに出す
    void ReportFirstMismatch(const sim::MultiresNest& cpu, const sim::MultiresNest& gpu, uint64_t tick) {
        for (size_t slot = 0; slot < cpu.blocks.size(); ++slot) {
            const MrBlock& a = cpu.blocks[slot];
            const MrBlock& b = gpu.blocks[slot];
            if (std::memcmp(&a, &b, sizeof(MrBlock)) != 0)
                Log(Channel::Sim, Level::Error,
                    "刻み {}: 枠 {} の見出しが違う(種類 cpu {} / gpu {}・頁 {} / {}・busyTick {} / {}・wakeTick {} / "
                    "{})",
                    tick, slot, a.kind, b.kind, a.page, b.page, a.busyTick, b.busyTick, a.wakeTick, b.wakeTick);
        }

        for (size_t i = 0; i < cpu.cells.size(); ++i) {
            if (sim::HashReactionCell(cpu.cells[i]) == sim::HashReactionCell(gpu.cells[i]))
                continue;

            Log(Channel::Sim, Level::Error, "刻み {}: 枠 {} セル {} が違う(エネルギー cpu {} / gpu {})", tick,
                i / MR_BLOCK_CELLS, i % MR_BLOCK_CELLS, cpu.cells[i].energy, gpu.cells[i].energy);
            break;
        }

        for (size_t i = 0; i < cpu.counters.size(); ++i) {
            if (cpu.counters[i] != gpu.counters[i])
                Log(Channel::Sim, Level::Error, "刻み {}: 数える欄 {} が違う(cpu {} / gpu {})", tick, i,
                    cpu.counters[i], gpu.counters[i]);
        }

        const bool sameStacks = cpu.freeBlocks == gpu.freeBlocks && cpu.freeFractions == gpu.freeFractions;
        Log(Channel::Sim, Level::Error, "刻み {}: 空きのスタック {}・帳簿 {}", tick, sameStacks ? "一致" : "不一致",
            cpu.ledger == gpu.ledger ? "一致" : "不一致");
    }

    // 1 刻みを GPU で走らせて読み戻す(record がリストに記録する)
    template <typename Record>
    std::expected<void, std::string> ExecuteTick(gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                 sim::GpuMultires& gpu, sim::MultiresNest& read, uint64_t tick,
                                                 const Record& record) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        ring.RecordBegin(list);
        if (!record(list))
            return std::unexpected(std::format("刻み {} を記録できない", tick));

        gpu.RecordReadback(list);
        ring.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected(std::format("刻み {} の GPU での実行に失敗", tick));

        const gpu::DebugRingContents debugOutput = ring.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("刻み {}: GPU の FX_ASSERT が {} 件", tick, debugOutput.assertCount));

        if (!gpu.Read(read))
            return std::unexpected("読み戻せない");

        return {};
    }

    // CPU と GPU の状態の全部が一致し、GPU の索引で本物のブロックが全部引けるか
    std::expected<void, std::string> CompareTick(const sim::MultiresNest& cpu, const sim::MultiresNest& read,
                                                 uint64_t tick) {
        if (sim::HashWholeNest(cpu) != sim::HashWholeNest(read)) {
            ReportFirstMismatch(cpu, read, tick);
            return std::unexpected(std::format("刻み {} で CPU と GPU が食い違う", tick));
        }

        if (test::CountIndexMismatches(read) != 0)
            return std::unexpected(std::format("刻み {}: GPU の索引で引けない本物のブロックがある", tick));

        return {};
    }

    double ToMilliseconds(const std::vector<uint64_t>& stamps, uint32_t from, uint32_t to, uint64_t frequency) {
        if (stamps.size() <= to || frequency == 0)
            return 0;

        return static_cast<double>(stamps[to] - stamps[from]) * 1000.0 / static_cast<double>(frequency);
    }

    std::expected<RunResult, std::string> RunScenario(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                      gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                      test::MultiresScenario scenario) {
        sim::MultiresNest cpu = test::MakeMultiresNestForTest(table);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity);
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read;
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                return RecordTick(list, *gpu, ring.GpuAddress(), scenario, tick);
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, tick, record); !executed)
                return std::unexpected(executed.error());

            test::StepMultiresScene(cpu, table, scenario, tick);
            if (auto compared = CompareTick(cpu, read, tick); !compared)
                return std::unexpected(compared.error());
        }

        return RunResult{.digest = sim::HashWholeNest(cpu), .freeFractions = cpu.counters[MR_COUNTER_FREE_FRACTIONS]};
    }

    // 粗くすると成分が入りきらない子(T-0022)。表は上限の試験の表。刻みは進めない(要求だけ)
    std::expected<RunResult, std::string> RunCoarsenFull(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                         gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = test::MakeCoarsenFullNest(table);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity);
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read;
        for (uint64_t tick = 0; tick < test::COARSEN_FULL_TICKS; ++tick) {
            const std::vector<MrRequest> requests = test::CoarsenFullRequestsAt(tick);
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                if (!gpu->RecordRequests(list, requests))
                    return false;

                gpu->RecordProcessRequests(list, ring.GpuAddress());

                return true;
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, tick, record); !executed)
                return std::unexpected(executed.error());

            sim::SubmitRequests(cpu, requests);
            sim::ProcessRequests(cpu);
            if (auto compared = CompareTick(cpu, read, tick); !compared)
                return std::unexpected(compared.error());
        }

        if (cpu.counters[MR_COUNTER_COARSEN_FULL] != 2)
            return std::unexpected("入りきらない子を粗くする要求を断っていない");

        return RunResult{.digest = sim::HashWholeNest(cpu), .freeFractions = cpu.counters[MR_COUNTER_FREE_FRACTIONS]};
    }

    // 世界の刻みで上限に当たる(T-0163)。同じ木を要求なしで刻み、上限の印の数(状態の要約に入る数える器)も毎刻み比べる
    std::expected<RunResult, std::string> RunLimitsStep(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                        gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                        bool conduction) {
        sim::MultiresNest cpu = test::MakeCoarsenFullNest(table);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity);
        if (!gpu)
            return std::unexpected(gpu.error());

        const sim::MultiresStepOptions options = test::LimitsStepOptions(conduction);
        sim::MultiresNest read;
        for (uint64_t tick = 0; tick < test::LIMITS_STEP_TICKS; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                gpu->RecordStep(list, ring.GpuAddress(), test::LIMITS_STEP_SEED, tick, options);

                return true;
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, tick, record); !executed)
                return std::unexpected(executed.error());

            sim::StepNest(cpu, table, test::LIMITS_STEP_SEED, tick, options);
            if (auto compared = CompareTick(cpu, read, tick); !compared)
                return std::unexpected(compared.error());
        }

        if (cpu.counters[MR_COUNTER_LIMIT_PRODUCTS] == 0 || cpu.counters[MR_COUNTER_LIMIT_CANDIDATES] == 0)
            return std::unexpected("世界の刻みで上限に当たっていない");

        Log(Channel::Gpu, Level::Info, "世界の刻みで上限に当たる(伝導 {}): 待たせた {}・選んだ {}(CPU と GPU で同じ)",
            conduction ? "あり" : "なし", cpu.counters[MR_COUNTER_LIMIT_PRODUCTS],
            cpu.counters[MR_COUNTER_LIMIT_CANDIDATES]);

        return RunResult{.digest = sim::HashWholeNest(cpu), .freeFractions = cpu.counters[MR_COUNTER_FREE_FRACTIONS]};
    }

    // 本物の葉のセルの成分の最大(溢れを含む)
    uint32_t MostLeafSpecies(const sim::MultiresNest& nest) {
        uint32_t most = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (MrIsSteppedCell(block, index))
                    most = std::max(most, sim::LoadWideNestCell(nest, slot, index).speciesCount);
            }
        }

        return most;
    }

    // 成分の二段(T-0176。伝導あり T-0211): 上限に当たる場面を溢れを使う世界で刻む。9 種目の生成物を待たせず、CPU の上限なしの世界と
    // 毎刻みビット一致(伝導ありは、溢れを持つセルの熱容量・伝導率と、変化を足した後の反応も比べる)
    std::expected<RunResult, std::string> RunLimitsStepWide(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                            gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                            bool conduction) {
        sim::MultiresNest cpu = test::MakeCoarsenFullNest(table);
        sim::EnableWideCells(cpu);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, {.wideCells = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        gpu->SetStageTimestamps(s_wideStageTimestamps);
        const sim::MultiresStepOptions options = test::LimitsStepOptions(conduction);
        sim::MultiresNest read;
        uint32_t most = 0;
        for (uint64_t tick = 0; tick < test::LIMITS_STEP_TICKS; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                gpu->RecordStep(list, ring.GpuAddress(), test::LIMITS_STEP_SEED, tick, options);

                return true;
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, tick, record); !executed)
                return std::unexpected(executed.error());

            sim::StepNest(cpu, table, test::LIMITS_STEP_SEED, tick, options);
            if (auto compared = CompareTick(cpu, read, tick); !compared)
                return std::unexpected(
                    std::format("溢れ(伝導 {}): {}", conduction ? "あり" : "なし", compared.error()));

            most = std::max(most, MostLeafSpecies(read));
        }

        if (cpu.counters[MR_COUNTER_LIMIT_PRODUCTS] != 0 || most <= RX_MAX_CELL_SPECIES)
            return std::unexpected("溢れ: 9 種目の生成物を待たせた・作っていない");

        Log(Channel::Gpu, Level::Info,
            "溢れを使う世界の刻み(伝導 {}): 待たせた {}・選んだ {}・最大の成分 {}(CPU と GPU で同じ)",
            conduction ? "あり" : "なし", cpu.counters[MR_COUNTER_LIMIT_PRODUCTS],
            cpu.counters[MR_COUNTER_LIMIT_CANDIDATES], most);

        return RunResult{.digest = sim::HashWholeNest(cpu), .freeFractions = cpu.counters[MR_COUNTER_FREE_FRACTIONS]};
    }

    // --- 溢れを使う世界の細かくする(T-0237): 上限の場面を溢れの世界で刻んで 9 種のセル(溢れ)を作り、刻み REFINE_WIDE_TICK に
    //     根(頁を持つ)から 2 段の鎖と、豊かな子から 1 段を細かくする。子の頁へ溢れごと写し、覆った親のセルの溢れを除く。
    //     その後も刻み、CPU の上限なしの世界と毎刻みビット一致 ---
    constexpr uint64_t REFINE_WIDE_TICK = 6;
    constexpr uint32_t REFINE_WIDE_WORLD_BLOCKS = 8;

    std::vector<MrRequest> RefineWideRequests() {
        const sim::MultiresPoint chain = {.x = 3, .y = 21, .z = 5, .level = 2};  // 根の八分の一 2(子が無い)
        const sim::MultiresPoint rich = {.x = 3, .y = 3, .z = 3, .level = 2};    // 豊かな子(レベル 1)の中

        return {test::MakeMultiresRequest(MR_REQUEST_REFINE, chain, chain.level),
                test::MakeMultiresRequest(MR_REQUEST_REFINE, rich, rich.level)};
    }

    // レベル level の本物のブロックのセルの成分の最大(溢れを含む)
    uint32_t MostSpeciesAtLevel(const sim::MultiresNest& nest, int32_t level) {
        uint32_t most = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL || block.level != level)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                most = std::max(most, sim::LoadWideNestCell(nest, slot, index).speciesCount);
        }

        return most;
    }

    std::expected<RunResult, std::string> RunRefineWide(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                        gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = test::MakeCoarsenFullNest(table, 0, REFINE_WIDE_WORLD_BLOCKS);
        sim::EnableWideCells(cpu);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, {.wideCells = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        const sim::MultiresStepOptions options = test::LimitsStepOptions(false);
        const std::vector<MrRequest> requests = RefineWideRequests();
        const uint32_t grantedBefore = cpu.counters[MR_COUNTER_GRANTED];
        sim::MultiresNest read;
        uint32_t refinedSpecies = 0;
        for (uint64_t tick = 0; tick < test::LIMITS_STEP_TICKS; ++tick) {
            const bool refine = tick == REFINE_WIDE_TICK;
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                if (refine) {
                    if (!gpu->RecordRequests(list, requests))
                        return false;

                    gpu->RecordProcessRequests(list, ring.GpuAddress());
                }

                gpu->RecordStep(list, ring.GpuAddress(), test::LIMITS_STEP_SEED, tick, options);

                return true;
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, tick, record); !executed)
                return std::unexpected(executed.error());

            if (refine) {
                sim::SubmitRequests(cpu, requests);
                sim::ProcessRequests(cpu);
                refinedSpecies = MostSpeciesAtLevel(cpu, 2);
            }

            sim::StepNest(cpu, table, test::LIMITS_STEP_SEED, tick, options);
            if (auto compared = CompareTick(cpu, read, tick); !compared)
                return std::unexpected(std::format("溢れの世界で細かくする: {}", compared.error()));
        }

        if (cpu.counters[MR_COUNTER_GRANTED] != grantedBefore + 2)
            return std::unexpected("溢れの世界で細かくする: 要求を 2 つとも適用していない");

        if (refinedSpecies <= RX_MAX_CELL_SPECIES)
            return std::unexpected("溢れの世界で細かくする: 細かくした子に溢れが無い(場面が試していない)");

        if (cpu.counters[MR_COUNTER_OVERFLOW] != 0 || cpu.counters[MR_COUNTER_LIMIT_PRODUCTS] != 0)
            return std::unexpected("溢れの世界で細かくする: 成分を捨てた・待たせた");

        Log(Channel::Gpu, Level::Info, "溢れの世界で細かくする: 子の最大の成分 {}(CPU と GPU で毎刻み同じ)",
            refinedSpecies);

        return RunResult{.digest = sim::HashWholeNest(cpu), .freeFractions = cpu.counters[MR_COUNTER_FREE_FRACTIONS]};
    }

    // --- 端数の溢れ(T-0311。TestCoarsenFullWide の場面): 溢れを使う CPU の世界で入りきらない豊かな子を粗くし(親のセルと端数は 16 種)、
    //     GPU に写して伝導ありで刻む。刻み COARSEN_WIDE_REFINE_TICK に同じ八分の一を細かくし直す(子の端数の枠へ端数の溢れを写し、
    //     親の端数の溢れを覆う)。CPU と毎刻みビット一致・保存がビット一致。GPU で粗くする和集合は T-0329 ---
    constexpr uint64_t COARSEN_WIDE_REFINE_TICK = 4;

    // 本物のブロックの端数の枠の成分の最大(溢れを含む)
    uint32_t MostFractionSpecies(const sim::MultiresNest& nest) {
        uint32_t most = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL || block.fraction == MR_NO_FRACTION)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                most = std::max(most, sim::LoadWideFraction(nest, block.fraction, index).speciesCount);
        }

        return most;
    }

    // 溢れを使う CPU の世界で豊かな子を粗くした木(TestCoarsenFullWide の前半)
    sim::MultiresNest MakeCoarsenedWideNest(const sim::BakedReactionTable& table) {
        sim::MultiresNest nest = test::MakeCoarsenFullNest(table);
        sim::EnableWideCells(nest);
        for (uint64_t tick = 0; tick < test::COARSEN_FULL_TICKS; ++tick) {
            sim::SubmitRequests(nest, test::CoarsenFullRequestsAt(tick));
            sim::ProcessRequests(nest);
        }

        return nest;
    }

    // 刻み終えた CPU の世界が場面を試したか(細かくし直した・保存・捨てた 0・待たせた 0)
    std::expected<void, std::string> CheckCoarsenFullWide(const sim::MultiresNest& cpu, uint32_t grantedBefore,
                                                          bool conserved) {
        if (cpu.counters[MR_COUNTER_GRANTED] != grantedBefore + 1)
            return std::unexpected("溢れの端数: 細かくし直す要求を適用していない");

        if (!conserved || cpu.counters[MR_COUNTER_OVERFLOW] != 0 || cpu.counters[MR_COUNTER_LIMIT_PRODUCTS] != 0)
            return std::unexpected("溢れの端数: 保存が崩れた・成分を捨てた・待たせた");

        return {};
    }

    std::expected<RunResult, std::string> RunCoarsenFullWide(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                             gpu::DebugRing& ring,
                                                             const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = MakeCoarsenedWideNest(table);
        const uint32_t coarsenedFraction = MostFractionSpecies(cpu);
        if (cpu.counters[MR_COUNTER_COARSEN_FULL] != 0 || coarsenedFraction <= RX_MAX_CELL_SPECIES)
            return std::unexpected("溢れの端数: 豊かな子を粗くしていない・端数に溢れが無い(場面が試していない)");

        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, {.wideCells = true});
        if (!gpu)
            return std::unexpected(gpu.error());

        const sim::ConservedTotals initial = sim::ComputeConservedTotals(cpu, table, 1);
        const sim::MultiresStepOptions options = test::LimitsStepOptions(true);
        const MrRequest refine = test::MakeMultiresRequest(MR_REQUEST_REFINE, test::CoarsenFullPoint(true), 1);
        const uint32_t grantedBefore = cpu.counters[MR_COUNTER_GRANTED];
        sim::MultiresNest read;
        bool conserved = true;
        uint32_t refinedFraction = 0;
        for (uint64_t tick = 0; tick < test::LIMITS_STEP_TICKS; ++tick) {
            const bool refining = tick == COARSEN_WIDE_REFINE_TICK;
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                if (refining) {
                    if (!gpu->RecordRequests(list, {&refine, 1}))
                        return false;

                    gpu->RecordProcessRequests(list, ring.GpuAddress());
                }

                gpu->RecordStep(list, ring.GpuAddress(), test::LIMITS_STEP_SEED, tick, options);

                return true;
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, tick, record); !executed)
                return std::unexpected(executed.error());

            if (refining) {
                sim::SubmitRequests(cpu, {&refine, 1});
                sim::ProcessRequests(cpu);
                refinedFraction = MostFractionSpecies(cpu);
            }

            sim::StepNest(cpu, table, test::LIMITS_STEP_SEED, tick, options);
            if (auto compared = CompareTick(cpu, read, tick); !compared)
                return std::unexpected(std::format("溢れの端数: {}", compared.error()));

            conserved = conserved && sim::ComputeConservedTotals(cpu, table, 1) == initial;
        }

        if (auto checked = CheckCoarsenFullWide(cpu, grantedBefore, conserved); !checked)
            return std::unexpected(checked.error());

        Log(Channel::Gpu, Level::Info,
            "溢れの端数: 粗くした端数の最大の成分 {}・細かくし直した後 {}(CPU と GPU で毎刻み同じ)", coarsenedFraction,
            refinedFraction);

        return RunResult{.digest = sim::HashWholeNest(cpu), .freeFractions = cpu.counters[MR_COUNTER_FREE_FRACTIONS]};
    }

    // --- GPU で粗くする和集合(T-0329。TestCoarsenFullWide の場面をそのまま): 溢れを使う世界で入りきらない豊かな子を GPU で粗くし
    //     (子の溢れまで読んで親のセル・端数は 16 種。断らない)、伝導ありで刻む。CPU と毎刻みビット一致・保存がビット一致 ---
    std::expected<RunResult, std::string> RunCoarsenUnionWide(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                              gpu::DebugRing& ring,
                                                              const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = test::MakeCoarsenFullNest(table);
        sim::EnableWideCells(cpu);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity, {.wideCells = true});
        uint64_t frequency = 0;
        if (!gpu || FAILED(queue.Native()->GetTimestampFrequency(&frequency)))
            return std::unexpected(gpu ? std::string("タイムスタンプの周波数を読めない") : gpu.error());

        gpu->SetStageTimestamps(true);  // 刻み 0(粗くする要求)の段ごとの時間を出す
        const sim::ConservedTotals initial = sim::ComputeConservedTotals(cpu, table, 1);
        sim::MultiresNest read;
        bool conserved = true;
        std::string stages;
        for (uint64_t tick = 0; tick < test::COARSEN_FULL_TICKS; ++tick) {
            const std::vector<MrRequest> requests = test::CoarsenFullRequestsAt(tick);
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                if (!gpu->RecordRequests(list, requests))
                    return false;

                gpu->RecordProcessRequests(list, ring.GpuAddress());

                return true;
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, tick, record); !executed)
                return std::unexpected(executed.error());

            sim::SubmitRequests(cpu, requests);
            sim::ProcessRequests(cpu);
            if (auto compared = CompareTick(cpu, read, tick); !compared)
                return std::unexpected(std::format("GPU で粗くする: {}", compared.error()));

            // 段の終わりのタイムスタンプ: 解決・確定・割り当て・塊・(Work Graph + 解放)・索引 × 2
            const std::vector<uint64_t> stamps = gpu->ReadTimestamps(gpu->StageTimestampCount());
            for (uint32_t i = 1; tick == 0 && i < stamps.size(); ++i)
                stages += std::format("{}{:.3f}", i == 1 ? "" : "/", ToMilliseconds(stamps, i - 1, i, frequency));

            conserved = conserved && sim::ComputeConservedTotals(cpu, table, 1) == initial;
        }

        const uint32_t coarsenedSpecies = MostLeafSpecies(cpu);
        const uint32_t coarsenedFraction = MostFractionSpecies(cpu);
        if (cpu.counters[MR_COUNTER_COARSEN_FULL] != 0 || coarsenedSpecies != 2 * RX_MAX_CELL_SPECIES ||
            coarsenedFraction <= RX_MAX_CELL_SPECIES)
            return std::unexpected("GPU で粗くする: 豊かな子を 16 種の親へ粗くしていない(場面が試していない)");

        const sim::MultiresStepOptions options = test::LimitsStepOptions(true);
        for (uint64_t tick = 0; tick < test::LIMITS_STEP_TICKS; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                gpu->RecordStep(list, ring.GpuAddress(), test::LIMITS_STEP_SEED, tick, options);

                return true;
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, tick, record); !executed)
                return std::unexpected(executed.error());

            sim::StepNest(cpu, table, test::LIMITS_STEP_SEED, tick, options);
            if (auto compared = CompareTick(cpu, read, tick); !compared)
                return std::unexpected(std::format("GPU で粗くした後: {}", compared.error()));

            conserved = conserved && sim::ComputeConservedTotals(cpu, table, 1) == initial;
        }

        if (!conserved || cpu.counters[MR_COUNTER_OVERFLOW] != 0 || cpu.counters[MR_COUNTER_LIMIT_PRODUCTS] != 0)
            return std::unexpected("GPU で粗くする: 保存が崩れた・成分を捨てた・待たせた");

        Log(Channel::Gpu, Level::Info,
            "GPU で粗くする和集合: 親のセルの最大の成分 {}・端数 {}・刻んだ後 {}(CPU と GPU で毎刻み同じ)。"
            "刻み 0 の段 {} ms(確定/割り当て/塊/Work Graph + 解放/索引/索引)",
            coarsenedSpecies, coarsenedFraction, MostLeafSpecies(cpu), stages);

        return RunResult{.digest = sim::HashWholeNest(cpu), .freeFractions = cpu.counters[MR_COUNTER_FREE_FRACTIONS]};
    }

    // 32 種のセルの溢れの塊の置き場(T-0248): 全部の頁の 2 面が 32 種のセルで埋まっても足りる数(既定の数は 1 頁の 2 面分なので、
    // 2 つの根が両方 32 種になる刻みで尽きて待たせる)
    // species = 表の物質の数(1 セルの最大)
    uint32_t WideManyChunks(const sim::MultiresNest& nest, uint32_t species = test::WIDE_MANY_SPECIES) {
        const uint32_t manyTail = species - RX_MAX_CELL_SPECIES;

        return nest.capacity.pages * 2 * MrWideChunksFor(MR_BLOCK_CELLS * manyTail);
    }

    // 試験の many の場面の表の物質の数(T-0272): GPU の 1 セルの上限まで作る(上限 64 なら 1 セル 33 種以上の場面)
    constexpr uint32_t WIDE_SCENE_MANY_SPECIES = RX_GPU_WIDE_SPECIES;

    // 頁の溢れが前の当座の枠(1 面 1024 成分。T-0176〜T-0211)より大きくなる(T-0236): 書く面の塊が足りない刻みで塊を配って同じ刻みのうちに
    // 刻み直し、待たせずに CPU の上限の無い世界と毎刻みビット一致(全部を刻む段・頁に広げて刻む段・伝導の足す段の刻み直し)。
    // many(T-0248。表は MakeWideManyTable): 1 セルが前の当座の上限(24 種)を超えても待たせずに毎刻みビット一致
    std::expected<RunResult, std::string> RunWideOverflow(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                          gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                          bool conduction, bool many = false) {
        constexpr uint32_t OLD_PAGE_ENTRIES = 1024;
        sim::MultiresNest cpu = many ? test::MakeWideOverflowNest(table, test::WideManyName)
                                     : test::MakeWideOverflowNest(table);
        sim::EnableWideCells(cpu);
        auto gpu = sim::GpuMultires::Create(
            device, table, cpu.capacity,
            {.wideCells = true, .wideChunks = many ? WideManyChunks(cpu, WIDE_SCENE_MANY_SPECIES) : 0});
        if (!gpu)
            return std::unexpected(gpu.error());

        gpu->SetStageTimestamps(s_wideStageTimestamps);

        const sim::MultiresStepOptions options = test::LimitsStepOptions(conduction);
        sim::MultiresNest read;
        uint32_t mostEntries = 0;
        uint32_t mostTail = 0;
        const uint64_t ticks = many ? test::WIDE_MANY_TICKS : test::WIDE_OVERFLOW_TICKS;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                gpu->RecordStep(list, ring.GpuAddress(), test::WIDE_OVERFLOW_SEED, tick, options);

                return true;
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, tick, record); !executed)
                return std::unexpected(executed.error());

            sim::StepNest(cpu, table, test::WIDE_OVERFLOW_SEED, tick, options);
            if (auto compared = CompareTick(cpu, read, tick); !compared)
                return std::unexpected(
                    std::format("大きい溢れ(伝導 {}): {}", conduction ? "あり" : "なし", compared.error()));

            for (const sim::MultiresOverflowArea& area : read.cellOverflow) {
                mostEntries = std::max(mostEntries, area.offsets[MR_BLOCK_CELLS]);
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                    mostTail = std::max(mostTail, area.offsets[index + 1] - area.offsets[index]);
            }
        }

        const uint32_t mostSpecies = RX_MAX_CELL_SPECIES + mostTail;
        // many: 前の当座の上限(24 種)と、上限が 32 より上なら 32 種(T-0248 の上限。T-0272)も超える
        const uint32_t manyBeyond = std::max(test::WIDE_MANY_OLD_LIMIT,
                                             std::min(WIDE_SCENE_MANY_SPECIES / 2, test::WIDE_MANY_SPECIES));
        const bool large = many ? mostSpecies > manyBeyond : mostEntries > OLD_PAGE_ENTRIES;
        if (cpu.counters[MR_COUNTER_LIMIT_PRODUCTS] != 0 || !large)
            return std::unexpected(std::format("大きい溢れ: 待たせた {}・頁の溢れの最大 {} 成分・1 セルの最大 {} 種",
                                               cpu.counters[MR_COUNTER_LIMIT_PRODUCTS], mostEntries, mostSpecies));

        // --- 一様な 8 種の根も 9 種目を作る(9 種目を作る規則だけが進む一様なブロックを頁に広げる。T-0276)---
        const uint32_t uniformSpecies = test::WideOverflowUniformRootSpecies(read);
        if (uniformSpecies <= RX_MAX_CELL_SPECIES)
            return std::unexpected(std::format("大きい溢れ: 一様な根が 9 種目を作っていない(成分 {})", uniformSpecies));

        Log(Channel::Gpu, Level::Info,
            "大きい溢れ(伝導 {}): 頁の溢れの最大 {} 成分・1 セルの最大 {} 種・一様な根の成分 {}・待たせた 0(CPU と GPU "
            "で同じ)",
            conduction ? "あり" : "なし", mostEntries, mostSpecies, uniformSpecies);

        return RunResult{.digest = sim::HashWholeNest(cpu), .freeFractions = cpu.counters[MR_COUNTER_FREE_FRACTIONS]};
    }

    // 溢れの塊の置き場が尽きた時(T-0236): 配れなかった頁の溢れるセルだけを待たせて MR_COUNTER_LIMIT_PRODUCTS に数え、FX_ASSERT も
    // 食い違う溢れも出さない(CPU の上限の無い世界とは食い違うので、刻み 0 の数える器だけ見る)
    std::expected<void, std::string> RunWideOverflowExhausted(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                              gpu::DebugRing& ring,
                                                              const sim::BakedReactionTable& table) {
        constexpr uint32_t SMALL_CHUNKS = 8;  // 1 頁の溢れが 1536 成分になると、1 頁の 2 面(6 塊ずつ)も入らない
        sim::MultiresNest cpu = test::MakeWideOverflowNest(table);
        sim::EnableWideCells(cpu);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity,
                                            {.wideCells = true, .wideChunks = SMALL_CHUNKS});
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read;
        for (uint64_t tick = 0; tick < test::WIDE_OVERFLOW_TICKS; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                gpu->RecordStep(list, ring.GpuAddress(), test::WIDE_OVERFLOW_SEED, tick,
                                test::LimitsStepOptions(false));

                return true;
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, tick, record); !executed)
                return std::unexpected(std::format("塊の置き場が尽きる: {}", executed.error()));
        }

        const uint32_t held = read.counters[MR_COUNTER_LIMIT_PRODUCTS];
        if (held == 0)
            return std::unexpected("塊の置き場が尽きる: 待たせていない");

        Log(Channel::Gpu, Level::Info, "塊の置き場が尽きる({} 塊): 待たせた {} セル", SMALL_CHUNKS, held);

        return {};
    }

    // たくさんの要求(取り合い・枠が足りない・無効・索引の作り直し・帳簿)。要求は CPU の木から作る(GPU の木と同じ)
    std::expected<RunResult, std::string> RunStress(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                    gpu::DebugRing& ring, const sim::BakedReactionTable& table) {
        sim::MultiresNest cpu = test::MakeStressNest(table);
        auto gpu = sim::GpuMultires::Create(device, table, cpu.capacity);
        if (!gpu)
            return std::unexpected(gpu.error());

        sim::MultiresNest read;
        for (uint64_t tick = 0; tick < test::STRESS_TICKS; ++tick) {
            const std::vector<MrRequest> requests = test::MakeStressRequests(cpu, tick);
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (tick == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                if (!gpu->RecordRequests(list, requests))
                    return false;

                gpu->RecordProcessRequests(list, ring.GpuAddress());
                gpu->RecordStep(list, ring.GpuAddress(), test::STRESS_SEED, tick);

                return true;
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, tick, record); !executed)
                return std::unexpected(executed.error());

            sim::SubmitRequests(cpu, requests);
            sim::ProcessRequests(cpu);
            sim::StepNest(cpu, table, test::STRESS_SEED, tick);
            if (auto compared = CompareTick(cpu, read, tick); !compared)
                return std::unexpected(compared.error());
        }

        const auto& counters = cpu.counters;
        Log(Channel::Gpu, Level::Info,
            "たくさんの要求 {} 刻み: 適用 {}・済み {}・取り合い {}・枠不足 {}・無効 {}・落ちた端数 {}",
            test::STRESS_TICKS, counters[MR_COUNTER_GRANTED], counters[MR_COUNTER_ALREADY],
            counters[MR_COUNTER_CONFLICT], counters[MR_COUNTER_NO_SPACE], counters[MR_COUNTER_INVALID],
            counters[MR_COUNTER_LOST]);

        return RunResult{.digest = sim::HashWholeNest(cpu), .freeFractions = counters[MR_COUNTER_FREE_FRACTIONS]};
    }

    // 1 本のリストで: 暖機の刻み → タイムスタンプ 0 → 鎖を作る → 1 → 刻む(影なら引き戻しも)→ 2 → 鎖を終える(本物は 1 段)→ 3
    // → 要求 0 件の処理(本物だけ)→ 4
    void RecordChainEvents(ID3D12GraphicsCommandList10* list, sim::GpuMultires& gpu, D3D12_GPU_VIRTUAL_ADDRESS ring,
                           bool real, bool& recorded) {
        const sim::MultiresPoint point = test::MakeMultiresPoint(test::MULTIRES_LEVELS);
        const auto level = static_cast<int32_t>(test::MULTIRES_LEVELS);
        const std::array<MrRequest, 1> refine = {test::MakeMultiresRequest(MR_REQUEST_REFINE, point, level)};
        const std::array<MrRequest, 1> coarsen = {test::MakeMultiresRequest(MR_REQUEST_COARSEN, point, level)};
        gpu.RecordTimestamp(list, 0);
        if (real) {
            recorded &= gpu.RecordRequests(list, refine);
            gpu.RecordProcessRequests(list, ring);
        } else {
            gpu.RecordRefineShadow(list, ring, test::MULTIRES_ROOT_SLOT, test::MULTIRES_SHADOW_SLOT,
                                   test::MULTIRES_LEVELS, point);
        }

        gpu.RecordTimestamp(list, 1);
        gpu.RecordStep(list, ring, test::MULTIRES_TEST_SEED, WARMUP_STEPS);
        if (!real)
            gpu.RecordPullBack(list, ring, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS);

        gpu.RecordTimestamp(list, 2);
        if (real) {
            recorded &= gpu.RecordRequests(list, coarsen);
            gpu.RecordProcessRequests(list, ring);
        } else {
            gpu.RecordRemoveShadow(list, ring, test::MULTIRES_SHADOW_SLOT, test::MULTIRES_LEVELS);
        }

        gpu.RecordTimestamp(list, 3);
        if (real) {
            recorded &= gpu.RecordRequests(list, {});
            gpu.RecordProcessRequests(list, ring);
        }

        gpu.RecordTimestamp(list, 4);
    }

    std::expected<ChainTiming, std::string> MeasureChain(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                         gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                         test::MultiresScenario scenario) {
        const sim::MultiresNest initial = test::MakeMultiresNestForTest(table);
        auto gpu = sim::GpuMultires::Create(device, table, initial.capacity);
        ID3D12GraphicsCommandList10* list = gpu ? queue.Begin() : nullptr;
        if (list == nullptr)
            return std::unexpected("計測の準備ができない");

        ring.RecordBegin(list);
        if (!gpu->RecordUpload(list, initial))
            return std::unexpected("初めの状態を写せない");

        const D3D12_GPU_VIRTUAL_ADDRESS ringAddress = ring.GpuAddress();
        for (uint32_t i = 0; i < WARMUP_STEPS; ++i)
            gpu->RecordStep(list, ringAddress, test::MULTIRES_TEST_SEED, i);

        bool recorded = true;
        RecordChainEvents(list, *gpu, ringAddress, scenario == test::MultiresScenario::Real, recorded);
        gpu->RecordReadback(list);
        ring.RecordReadbackAndReset(list);
        uint64_t frequency = 0;
        if (!recorded || !queue.ExecuteAndWait() || ring.Drain().assertCount > 0 ||
            FAILED(queue.Native()->GetTimestampFrequency(&frequency))) {
            return std::unexpected("計測の実行に失敗");
        }

        const std::vector<uint64_t> stamps = gpu->ReadTimestamps(5);
        return ChainTiming{.createMs = ToMilliseconds(stamps, 0, 1, frequency),
                           .stepMs = ToMilliseconds(stamps, 1, 2, frequency),
                           .endMs = ToMilliseconds(stamps, 2, 3, frequency),
                           .emptyMs = ToMilliseconds(stamps, 3, 4, frequency)};
    }

    // --- 溢れの世界の費用(T-0248。--measure-wide。release のハードウェアで): 暖機の刻みの後に MEASURE_TICKS 刻みを測り(数刻みずつの
    //     リスト。T-0273)、頁のセルの数で割る(ns/セル)。VRAM は世界のバッファの和(BufferBytes。Work Graph の作業場を除く)と溢れの領域 ---

    constexpr uint32_t MEASURE_WARMUP_TICKS = 100;
    constexpr uint32_t MEASURE_TICKS = 50;
    // 1 本のリストの GPU の時間の目安(T-0273): 32 種のセルの場面は 1 刻み 17〜100 ms(ほかのランナーの負荷で変わる)。150 刻みを 1 本の
    // リストにすると、ほかのランナーが GPU を使っている時に約 2 秒で TDR(DXGI_ERROR_DEVICE_HUNG)になる。最初の 1 刻みを 1 本で流して
    // 1 刻みの時間を見積もり、残りはこの時間に収まる刻みずつのリストで投げる(空気の場面は今までどおりほぼ 1 本)
    constexpr double MEASURE_LIST_BUDGET_MS = 250.0;

    struct WideCost {
        double nsPerCell = 0;
        uint64_t bufferBytes = 0;
        uint64_t overflowBytes = 0;
        uint32_t productsHeld = 0;
        uint32_t ticksPerList = 0;
    };

    std::expected<WideCost, std::string> MeasureWideCost(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                         gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                         bool many, bool wide, bool conduction) {
        sim::MultiresNest nest = test::MakeMeasureNest(table, many);
        if (wide)
            sim::EnableWideCells(nest);

        auto gpu = sim::GpuMultires::Create(device, table, nest.capacity,
                                            {.wideCells = wide, .wideChunks = many ? WideManyChunks(nest) : 0});
        uint64_t frequency = 0;
        if (!gpu || FAILED(queue.Native()->GetTimestampFrequency(&frequency)))
            return std::unexpected("計測の準備ができない");

        // --- 暖機 → MEASURE_TICKS 刻み。MEASURE_LIST_BUDGET_MS に収まる刻みずつのリストで、測る刻みのリストの GPU の時間を足す ---
        const sim::MultiresStepOptions options = test::LimitsStepOptions(conduction);
        const D3D12_GPU_VIRTUAL_ADDRESS ringAddress = ring.GpuAddress();
        sim::MultiresNest read;
        double milliseconds = 0;
        uint32_t ticksPerList = 1;
        for (uint32_t first = 0; first < MEASURE_WARMUP_TICKS + MEASURE_TICKS;) {
            const uint32_t last = std::min({first + ticksPerList, MEASURE_WARMUP_TICKS + MEASURE_TICKS,
                                            first < MEASURE_WARMUP_TICKS ? MEASURE_WARMUP_TICKS : UINT32_MAX});
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (first == 0 && !gpu->RecordUpload(list, nest))
                    return false;

                gpu->RecordTimestamp(list, 0);
                for (uint32_t tick = first; tick < last; ++tick)
                    gpu->RecordStep(list, ringAddress, test::WIDE_OVERFLOW_SEED, tick, options);

                gpu->RecordTimestamp(list, 1);

                return true;
            };
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, first, record); !executed)
                return std::unexpected(std::format("計測の実行に失敗({})", executed.error()));

            const double listMs = ToMilliseconds(gpu->ReadTimestamps(2), 0, 1, frequency);
            if (first >= MEASURE_WARMUP_TICKS)
                milliseconds += listMs;

            if (first == 0)
                ticksPerList = std::clamp(static_cast<uint32_t>(MEASURE_LIST_BUDGET_MS / std::max(listMs, 0.001)), 1u,
                                          MEASURE_WARMUP_TICKS + MEASURE_TICKS);

            first = last;
        }

        const double cellTicks = static_cast<double>(MEASURE_TICKS) * test::MEASURE_ROOTS * MR_BLOCK_CELLS;

        return WideCost{.nsPerCell = milliseconds * 1.0e6 / cellTicks,
                        .bufferBytes = gpu->BufferBytes(),
                        .overflowBytes = gpu->OverflowBytes(),
                        .productsHeld = read.counters[MR_COUNTER_LIMIT_PRODUCTS],
                        .ticksPerList = ticksPerList};
    }

    // 場面ごとに 2 回測って 2 回目を出す(1 回目はシェーダーの準備・クロックの立ち上がりを含む)
    int RunMeasureWide(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring) {
        const auto airTable = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto manyTable = sim::BakeReactionTable(test::MakeWideManyTable());
        if (!airTable || !manyTable)
            return 1;

        struct Case {
            const char* name;
            bool many;
            bool wide;
        };
        for (const Case& item : {Case{"空気・既定の世界", false, false}, Case{"空気・溢れの世界", false, true},
                                 Case{"上限の種のセル・溢れの世界", true, true}}) {
            for (const bool conduction : {false, true}) {
                const sim::BakedReactionTable& table = item.many ? *manyTable : *airTable;
                const auto first = MeasureWideCost(device, queue, ring, table, item.many, item.wide, conduction);
                const auto cost = MeasureWideCost(device, queue, ring, table, item.many, item.wide, conduction);
                if (!first || !cost) {
                    Log(Channel::Gpu, Level::Error, "計測 {}: {}", item.name, cost ? first.error() : cost.error());
                    return 1;
                }

                Log(Channel::Gpu, Level::Info,
                    "計測 {}(伝導 {}・根 {}・{} 刻み・1 リスト {} 刻み): {:.2f} ns/セル(1 回目 {:.2f})・バッファ {} "
                    "KiB・"
                    "溢れ {} KiB・待たせた {}",
                    item.name, conduction ? "あり" : "なし", test::MEASURE_ROOTS, MEASURE_TICKS, cost->ticksPerList,
                    cost->nsPerCell, first->nsPerCell, cost->bufferBytes / 1024, cost->overflowBytes / 1024,
                    cost->productsHeld);
            }
        }

        return 0;
    }

    // 場面 1 つを流し、かかった時間(壁時計)をログに出す(debug でどの場面が重いかを見るため。T-0263)
    template <typename Scene>
    auto Timed(std::string_view name, Scene&& scene) {
        const auto start = chr::steady_clock::now();
        auto result = scene();
        const double seconds = chr::duration<double>(chr::steady_clock::now() - start).count();
        Log(Channel::Gpu, Level::Info, "場面 {}: {:.1f} 秒", name, seconds);

        return result;
    }

    // 鎖の場面(本物の鎖・影の鎖・たくさんの要求・粗くすると入りきらない子・上限〔溢れなし〕)。通らなければ 1
    int RunChainScenes(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                       const sim::BakedReactionTable& table) {
        using test::MultiresScenario;
        const auto runReal = [&] {
            return RunScenario(device, queue, ring, table, MultiresScenario::Real);
        };
        const auto runStress = [&] {
            return RunStress(device, queue, ring, table);
        };
        const auto first = Timed("本物の鎖", runReal);
        const auto second = [&] {
            if constexpr (REPEAT_AND_MEASURE)
                return Timed("本物の鎖(2 回目)", runReal);
            else
                return first;
        }();

        const auto shadow = Timed("影の鎖",
                                  [&] { return RunScenario(device, queue, ring, table, MultiresScenario::Shadow); });

        // 計測は CPU の重いたくさんの要求の前に(GPU が長く空くとクロックが下がり、暖機しても時間が数倍に出る)
        std::expected<ChainTiming, std::string> realTiming = ChainTiming{};
        std::expected<ChainTiming, std::string> shadowTiming = ChainTiming{};
        if constexpr (REPEAT_AND_MEASURE) {
            realTiming = MeasureChain(device, queue, ring, table, MultiresScenario::Real);
            shadowTiming = MeasureChain(device, queue, ring, table, MultiresScenario::Shadow);
        }

        const auto stress = Timed("たくさんの要求", runStress);
        const auto stressAgain = [&] {
            if constexpr (REPEAT_AND_MEASURE)
                return Timed("たくさんの要求(2 回目)", runStress);
            else
                return stress;
        }();

        const auto limitsTable = sim::BakeReactionTable(test::MakeLimitsTestTable());
        if (!limitsTable) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED(上限の試験の表を作れない)");
            return 1;
        }

        const auto coarsenFull = RunCoarsenFull(device, queue, ring, *limitsTable);
        const auto limitsStep = Timed("上限", [&] { return RunLimitsStep(device, queue, ring, *limitsTable, false); });
        const auto limitsConduct = Timed("上限・伝導",
                                         [&] { return RunLimitsStep(device, queue, ring, *limitsTable, true); });
        for (const auto* result :
             {&first, &second, &shadow, &stress, &stressAgain, &coarsenFull, &limitsStep, &limitsConduct}) {
            if (*result)
                continue;

            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED ({})", result->error());
            return 1;
        }

        if (first->digest != second->digest || stress->digest != stressAgain->digest) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED(2 回の実行が食い違う)");
            return 1;
        }

        if (!realTiming || !shadowTiming) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED(計測)");
            return 1;
        }

        // 時間は release だけで測る(debug は 0 のまま)
        if constexpr (REPEAT_AND_MEASURE) {
            Log(Channel::Gpu, Level::Info,
                "GPU 時間: 細かくする要求({} 段){:.3f} ms・刻み(鎖あり){:.3f} ms・粗くする要求(1 段){:.3f} ms・"
                "要求 0 件の処理 {:.3f} ms・端数の枠の空き {}",
                test::MULTIRES_LEVELS, realTiming->createMs, realTiming->stepMs, realTiming->endMs, realTiming->emptyMs,
                second->freeFractions);
            Log(Channel::Gpu, Level::Info,
                "GPU 時間(影): 影を作る {:.3f} ms・刻み + 引き戻し {:.3f} ms・捨てる {:.3f} ms", shadowTiming->createMs,
                shadowTiming->stepMs, shadowTiming->endMs);
        }

        Log(Channel::Gpu, Level::Info,
            "鎖の場面: OK(本物の鎖・影の鎖の {} 刻み・たくさんの要求の {} 刻みで GPU と CPU がビット一致。"
            "要約 本物 {:016x} / 影 {:016x} / たくさん {:016x})",
            test::MULTIRES_END_TICK, test::STRESS_TICKS, first->digest, shadow->digest, stress->digest);

        return 0;
    }

    // 溢れを使う世界の場面(上限の場面を溢れで・大きい溢れ・1 セル上限〔RX_GPU_WIDE_SPECIES〕の種・塊の置き場が尽きる)。通らなければ 1
    int RunWideScenes(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring) {
        const auto limitsTable = sim::BakeReactionTable(test::MakeLimitsTestTable());
        const auto wideTable = sim::BakeReactionTable(test::MakeWideOverflowTable());
        const auto manyTable = sim::BakeReactionTable(test::MakeWideManyTable(WIDE_SCENE_MANY_SPECIES));
        if (!limitsTable || !wideTable || !manyTable) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED(溢れの試験の表を作れない)");
            return 1;
        }

        const auto limitsWide = Timed("上限・溢れ",
                                      [&] { return RunLimitsStepWide(device, queue, ring, *limitsTable, false); });
        const auto limitsWideConduct = Timed(
            "上限・溢れ・伝導", [&] { return RunLimitsStepWide(device, queue, ring, *limitsTable, true); });
        const auto wideOverflow = Timed("大きい溢れ",
                                        [&] { return RunWideOverflow(device, queue, ring, *wideTable, false); });
        const auto wideOverflowConduct = Timed("大きい溢れ・伝導",
                                               [&] { return RunWideOverflow(device, queue, ring, *wideTable, true); });
        const auto wideMany = Timed("上限まで(64 種の表)",
                                    [&] { return RunWideOverflow(device, queue, ring, *manyTable, false, true); });
        const auto wideManyConduct = Timed(
            "上限まで・伝導", [&] { return RunWideOverflow(device, queue, ring, *manyTable, true, true); });
        const auto refineWide = Timed("溢れの世界で細かくする",
                                      [&] { return RunRefineWide(device, queue, ring, *limitsTable); });
        const auto coarsenWide = Timed("溢れの端数",
                                       [&] { return RunCoarsenFullWide(device, queue, ring, *limitsTable); });
        const auto coarsenUnion = Timed("GPU で粗くする和集合",
                                        [&] { return RunCoarsenUnionWide(device, queue, ring, *limitsTable); });
        const auto exhausted = Timed("塊の置き場が尽きる",
                                     [&] { return RunWideOverflowExhausted(device, queue, ring, *wideTable); });
        if (!exhausted) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED ({})", exhausted.error());
            return 1;
        }

        for (const auto* result : {&limitsWide, &limitsWideConduct, &wideOverflow, &wideOverflowConduct, &wideMany,
                                   &wideManyConduct, &refineWide, &coarsenWide, &coarsenUnion}) {
            if (*result)
                continue;

            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED ({})", result->error());
            return 1;
        }

        // 伝導ありの溢れの世界で伝導が働いている(熱が流れなければ、溢れを持つセルの熱容量・伝導率を比べたことにならない。T-0211)
        if (limitsWide->digest == limitsWideConduct->digest) {
            Log(Channel::Gpu, Level::Error,
                "gpu_multires_test: FAILED(溢れの世界で伝導ありとなしが同じ = 熱が流れていない)");
            return 1;
        }

        Log(Channel::Gpu, Level::Info,
            "溢れの場面: OK(上限・大きい溢れ・上限まで〔速い形と大きい形〕・細かくする・溢れの端数で GPU と CPU "
            "が毎刻みビット一致)");

        return 0;
    }

    // 溢れの場面を 1 つ流す(T-0290 の切り分け。場面 = limits / limits-conduct / overflow / overflow-conduct / many / many-conduct)
    std::expected<RunResult, std::string> RunWideSceneByName(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                             gpu::DebugRing& ring, std::string_view scene) {
        const bool conduction = scene.ends_with("-conduct");
        if (scene.starts_with("limits")) {
            const auto table = sim::BakeReactionTable(test::MakeLimitsTestTable());
            return table ? RunLimitsStepWide(device, queue, ring, *table, conduction)
                         : std::unexpected(std::string("表を作れない"));
        }

        const bool many = scene.starts_with("many");
        if (!many && !scene.starts_with("overflow"))
            return std::unexpected(std::format("場面の名前が違う: {}", scene));

        const auto table = sim::BakeReactionTable(many ? test::MakeWideManyTable(WIDE_SCENE_MANY_SPECIES)
                                                       : test::MakeWideOverflowTable());
        return table ? RunWideOverflow(device, queue, ring, *table, conduction, many)
                     : std::unexpected(std::string("表を作れない"));
    }

    // 溢れの場面の並び(カンマ区切り)を count 回続けて流す(T-0290 の切り分け。--repeat-wide <場面,場面,...> <回数>)。
    // 前の場面の後でだけ止まるのかを、場面の組み合わせで見る
    // fresh なら場面ごとにキューとデバッグのリングを作り直す(--repeat-fresh-queue / --repeat-fresh-ring)
    struct RepeatOptions {
        D3D12_COMMAND_LIST_TYPE queueType = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        bool freshQueue = false;
        bool freshRing = false;
    };

    int RunWideRepeat(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring, std::string_view scenes,
                      uint32_t count, const RepeatOptions& repeat) {
        for (uint32_t round = 0; round < count; ++round) {
            for (const auto part : views::split(scenes, ',')) {
                const std::string_view scene(part.begin(), part.end());
                const std::string name = std::format("{} {} 回目", scene, round + 1);
                auto freshQueue = repeat.freshQueue ? gpu::ImmediateQueue::Create(device, repeat.queueType)
                                                    : std::unexpected(std::string("作り直さない"));
                auto freshRing = repeat.freshRing ? gpu::DebugRing::Create(device)
                                                  : std::unexpected(std::string("作り直さない"));
                gpu::ImmediateQueue& sceneQueue = freshQueue ? *freshQueue : queue;
                gpu::DebugRing& sceneRing = freshRing ? *freshRing : ring;
                const auto result = Timed(name,
                                          [&] { return RunWideSceneByName(device, sceneQueue, sceneRing, scene); });
                if (result)
                    continue;

                Log(Channel::Gpu, Level::Error, "繰り返し: FAILED {}: {}", name, result.error());
                return 1;
            }
        }

        Log(Channel::Gpu, Level::Info, "繰り返し: OK({} を {} 回)", scenes, count);

        return 0;
    }

    // 粗くする溢れの場面だけ(--part coarsen。T-0329 の切り分け用。溢れの場面〔wide〕にも入っている)
    int RunCoarsenScenes(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring) {
        const auto limitsTable = sim::BakeReactionTable(test::MakeLimitsTestTable());
        if (!limitsTable) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED(上限の試験の表を作れない)");
            return 1;
        }

        const auto coarsenUnion = Timed("GPU で粗くする和集合",
                                        [&] { return RunCoarsenUnionWide(device, queue, ring, *limitsTable); });
        const auto coarsenWide = Timed("溢れの端数",
                                       [&] { return RunCoarsenFullWide(device, queue, ring, *limitsTable); });
        for (const auto* result : {&coarsenUnion, &coarsenWide}) {
            if (*result)
                continue;

            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED ({})", result->error());
            return 1;
        }

        return 0;
    }

    // どの場面を流すか(--part。ctest は debug の 1200 秒の上限に収まるよう 2 本に分ける。T-0263)
    enum class Part : uint8_t { All, Chains, Wide, Coarsen };

    std::optional<Part> ParsePart(std::string_view text) {
        if (text == "all")
            return Part::All;
        if (text == "chains")
            return Part::Chains;
        if (text == "wide")
            return Part::Wide;
        if (text == "coarsen")
            return Part::Coarsen;

        return std::nullopt;
    }

    // --- 溢れの世界の切り分け(T-0273。--diagnose-wide <刻み/リスト> [--diagnose-compare] [--diagnose-conduct]
    //     [--diagnose-limits]): 計測の場面(MakeMeasureNest の many。limits なら上限の場面)を ticksPerList 刻みずつのリストで流す。
    //     リストごとに壁時計の時間と、リストの最後の刻みの段ごとの時間(SetStageTimestamps)を出す。compare ならリストの終わりに CPU と比べる ---

    constexpr uint32_t DIAGNOSE_TICKS = 150;

    struct DiagnoseOptions {
        uint32_t ticksPerList = 1;
        bool compare = false;
        bool conduction = false;
        bool limits = false;  // 上限の場面(RunLimitsStepWide と同じ。表は MakeLimitsTestTable)
    };

    // 段ごとの時間(ms)を「a/b/c」にし、合計を返す
    double FormatStages(const std::vector<uint64_t>& stamps, uint64_t frequency, std::string& text) {
        double total = 0;
        for (uint32_t i = 1; i < stamps.size(); ++i) {
            const double milliseconds = ToMilliseconds(stamps, i - 1, i, frequency);
            total += milliseconds;
            text += std::format("{}{:.2f}", i == 1 ? "" : "/", milliseconds);
        }

        return total;
    }

    std::expected<void, std::string> DiagnoseWide(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                  gpu::DebugRing& ring, const sim::BakedReactionTable& table,
                                                  const DiagnoseOptions& diagnose) {
        sim::MultiresNest cpu = diagnose.limits ? test::MakeCoarsenFullNest(table) : test::MakeMeasureNest(table, true);
        sim::EnableWideCells(cpu);
        auto gpu = sim::GpuMultires::Create(
            device, table, cpu.capacity, {.wideCells = true, .wideChunks = diagnose.limits ? 0 : WideManyChunks(cpu)});
        const uint64_t ticks = diagnose.limits ? test::LIMITS_STEP_TICKS : DIAGNOSE_TICKS;
        const uint64_t worldSeed = diagnose.limits ? test::LIMITS_STEP_SEED : test::WIDE_OVERFLOW_SEED;
        uint64_t frequency = 0;
        if (!gpu || FAILED(queue.Native()->GetTimestampFrequency(&frequency)))
            return std::unexpected("切り分けの準備ができない");

        gpu->SetStageTimestamps(true);
        const sim::MultiresStepOptions options = test::LimitsStepOptions(diagnose.conduction);
        sim::MultiresNest read;
        double worstTickMs = 0;
        for (uint64_t first = 0; first < ticks; first += diagnose.ticksPerList) {
            const uint64_t last = std::min<uint64_t>(first + diagnose.ticksPerList, ticks);
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (first == 0 && !gpu->RecordUpload(list, cpu))
                    return false;

                for (uint64_t tick = first; tick < last; ++tick)
                    gpu->RecordStep(list, ring.GpuAddress(), worldSeed, tick, options);

                return true;
            };
            const auto start = chr::steady_clock::now();
            if (auto executed = ExecuteTick(queue, ring, *gpu, read, first, record); !executed)
                return std::unexpected(std::format("刻み {}〜{}: {}", first, last - 1, executed.error()));

            // --- リストの時間と、最後の刻みの段ごとの時間 ---
            const double wallMs = chr::duration<double, std::milli>(chr::steady_clock::now() - start).count();
            std::string stages;
            const double tickMs = FormatStages(gpu->ReadTimestamps(gpu->StageTimestampCount()), frequency, stages);
            worstTickMs = std::max(worstTickMs, tickMs);
            if (first % 10 == 0 || last == ticks || tickMs > 40.0)
                Log(Channel::Gpu, Level::Info, "切り分け 刻み {}〜{}: リスト {:.1f} ms・最後の刻み {:.2f} ms(段 {})",
                    first, last - 1, wallMs, tickMs, stages);

            if (!diagnose.compare)
                continue;

            for (uint64_t tick = first; tick < last; ++tick)
                sim::StepNest(cpu, table, worldSeed, tick, options);

            if (auto compared = CompareTick(cpu, read, last - 1); !compared)
                return std::unexpected(std::format("切り分け: {}", compared.error()));
        }

        Log(Channel::Gpu, Level::Info,
            "切り分け(伝導 {}・{} 刻み/リスト・CPU と比べる {}): 通った。1 刻みの最大 {:.2f} ms・待たせた {}",
            diagnose.conduction ? "あり" : "なし", diagnose.ticksPerList, diagnose.compare ? "はい" : "いいえ",
            worstTickMs, read.counters[MR_COUNTER_LIMIT_PRODUCTS]);

        return {};
    }

    int Run(std::span<char*> arguments) {
        std::vector<char*> rest;
        bool measureWide = false;
        std::optional<Part> part = Part::All;
        std::optional<DiagnoseOptions> diagnose;
        std::string_view repeatScene;
        uint32_t repeatCount = 0;
        RepeatOptions repeat;
        for (size_t index = 0; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            if (argument == "--measure-wide")
                measureWide = true;
            else if (argument == "--wide-stamps")
                s_wideStageTimestamps = true;
            else if (argument == "--repeat-fresh-queue")
                repeat.freshQueue = true;
            else if (argument == "--repeat-fresh-ring")
                repeat.freshRing = true;
            else if (argument == "--repeat-wide" && index + 2 < arguments.size()) {
                repeatScene = arguments[++index];
                repeatCount = std::max(1, std::atoi(arguments[++index]));
            } else if (argument == "--part" && index + 1 < arguments.size())
                part = ParsePart(arguments[++index]);
            else if (argument == "--diagnose-wide" && index + 1 < arguments.size())
                diagnose = DiagnoseOptions{
                    .ticksPerList = std::max(1u, static_cast<uint32_t>(std::atoi(arguments[++index])))};
            else if (argument == "--diagnose-compare" && diagnose)
                diagnose->compare = true;
            else if (argument == "--diagnose-conduct" && diagnose)
                diagnose->conduction = true;
            else if (argument == "--diagnose-limits" && diagnose)
                diagnose->limits = true;
            else
                rest.push_back(arguments[index]);
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options || !part) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_multires_test [--warp] [--queue direct|compute] [--measure-wide] [--part "
                "all|chains|wide|coarsen] [--diagnose-wide <刻み/リスト> [--diagnose-compare] [--diagnose-conduct] "
                "[--diagnose-limits]] [--repeat-wide <場面,...> <回数> [--repeat-fresh-queue] [--repeat-fresh-ring]] "
                "[--wide-stamps]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_multires_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        if (measureWide)
            return RunMeasureWide(device->Get(), *queue, *ring);

        if (repeatCount > 0)
            return RunWideRepeat(
                device->Get(), *queue, *ring, repeatScene, repeatCount,
                {.queueType = options->queueType, .freshQueue = repeat.freshQueue, .freshRing = repeat.freshRing});

        if (diagnose) {
            const auto manyTable = sim::BakeReactionTable(diagnose->limits ? test::MakeLimitsTestTable()
                                                                           : test::MakeWideManyTable());
            const auto diagnosed = manyTable ? DiagnoseWide(device->Get(), *queue, *ring, *manyTable, *diagnose)
                                             : std::unexpected(std::string("表を作れない"));
            if (!diagnosed)
                Log(Channel::Gpu, Level::Error, "切り分け: FAILED {}", diagnosed.error());

            return diagnosed ? 0 : 1;
        }

        const bool chains = *part == Part::All || *part == Part::Chains;
        const bool wide = *part == Part::All || *part == Part::Wide;
        if (chains && RunChainScenes(device->Get(), *queue, *ring, *table) != 0)
            return 1;

        if (wide && RunWideScenes(device->Get(), *queue, *ring) != 0)
            return 1;

        if (*part == Part::Coarsen && RunCoarsenScenes(device->Get(), *queue, *ring) != 0)
            return 1;

        if (!test::PassesValidation(*device, "gpu_multires_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_multires_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
