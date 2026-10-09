// probe_peek.cpp — 覗き窓(probe_peek.h)。GPU は多重解像度の入れ子(sim/gpu_multires)に世界の写しと抽出の Compute
// (shaders/sim/multires_peek.hlsl)を足して記録する。CPU リファレンスは同じ順で sim/multires_nest の関数を呼ぶ。
#include "sim/probe_peek.h"

#include <algorithm>

#include "common/probe_world.hlsli"
#include "core/aliases.h"
#include "core/log.h"
#include "gpu/resources.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        constexpr uint32_t PEEK_GROUP_THREADS = 64;  // multires_peek.hlsl の numthreads
        constexpr int32_t PEEK_POINT_LEVEL = static_cast<int32_t>(PEEK_LEVEL_COUNT);

        // 覗き窓の入れ子は観察の枠だけ(世界の木を持たない。写しと影の鎖。17 §5)
        constexpr MultiresCapacity PEEK_CAPACITY = {.worldBlocks = 0,
                                                    .observerBlocks = PEEK_BLOCK_CAPACITY,
                                                    .fractions = 1,
                                                    .indexEntries = 1,
                                                    .ledgerColumns = 1};

        // 点を含む世界の 8³ のブロックの原点(1 軸ぶん。レベル 0 のセル)
        uint32_t BlockOrigin(uint32_t cell) {
            return cell & ~(MR_BLOCK_EDGE - 1);
        }

        uint32_t Generation(uint64_t tick) {
            return static_cast<uint32_t>(tick & 1u);
        }

    }  // namespace

    MultiresPoint PeekPointOfCell(PeekCell cell) {
        const int64_t halfCell = int64_t{1} << (PEEK_POINT_LEVEL - 1);
        const auto center = [halfCell](uint32_t value) {
            return (int64_t{value} << PEEK_POINT_LEVEL) + halfCell;
        };

        return {.x = center(cell.x), .y = center(cell.y), .z = center(cell.z), .level = PEEK_POINT_LEVEL};
    }

    // --- 覗く場所の状態 ---

    PeekState::Plan PeekState::NextPlan(uint64_t tick) {
        Plan plan;

        // --- やめた・移った・表が変わった: 前の影の鎖を捨てる ---
        if (m_hasActive && (!m_hasRequest || m_active != m_requested || m_tableChanged)) {
            plan.removeShadow = true;
            m_hasActive = false;
        }

        m_tableChanged = false;

        if (!m_hasRequest)
            return plan;

        plan.mirror = true;
        plan.cell = m_requested;

        // --- 初めて: 今の世界の写しから細かくする(影は世界と同じ数で始まる)---
        if (!m_hasActive) {
            plan.refine = true;
            m_active = m_requested;
            m_hasActive = true;
            m_lastTick = tick;

            return plan;
        }

        // --- 続き: 世界の刻みが進んでいれば影も 1 刻み。どちらでも親(今の世界)へ引き戻す ---
        plan.step = tick > m_lastTick;
        plan.pullBack = true;
        m_lastTick = tick;

        return plan;
    }

    // --- GPU ---

    std::expected<ProbePeek, std::string> ProbePeek::Create(ID3D12Device5* device, const BakedReactionTable& table) {
        auto nest = GpuMultires::Create(device, table, PEEK_CAPACITY);
        if (!nest)
            return std::unexpected(nest.error());

        ProbePeek result(std::move(*nest), ProbeViewSpecies(table));
        ID3D12RootSignature* rootSignature = result.m_nest.RootSignature();

        const auto mirror = gpu::LoadShader("sim/multires_peek_mirror.cso");
        const auto extract = gpu::LoadShader("sim/multires_peek_extract.cso");
        if (!mirror)
            return std::unexpected(mirror.error());

        if (!extract)
            return std::unexpected(extract.error());

        result.m_mirrorPipeline = gpu::CreateComputePipeline(device, rootSignature, *mirror);
        result.m_extractPipeline = gpu::CreateComputePipeline(device, rootSignature, *extract);
        if (!result.m_mirrorPipeline || !result.m_extractPipeline)
            return std::unexpected("覗き窓のパイプラインを作れない");

        return result;
    }

    void ProbePeek::RecordAfterExtract(ID3D12GraphicsCommandList10* list, const ProbeExtractContext& context) {
        // --- 最初だけ: 空の入れ子(全部の枠が空き)を写す ---
        if (!m_uploaded) {
            if (!m_nest.RecordUpload(list, MakeMultiresNest(PEEK_CAPACITY)))
                return;

            m_uploaded = true;
        }

        m_nest.SetExternalViews(context.cells->GetGPUVirtualAddress(), context.extraction->GetGPUVirtualAddress());
        ApplyDueTables(context);

        const PeekState::Plan plan = m_state.NextPlan(context.tick);
        if (plan.removeShadow)
            m_nest.RecordRemoveShadow(list, context.debugRing, PEEK_FIRST_SHADOW_SLOT, PEEK_LEVEL_COUNT);

        // --- 世界の写し → 細かくする、または刻んで引き戻す ---
        if (plan.mirror) {
            const std::array<uint32_t, 4> mirrorConstants = {Generation(context.tick), BlockOrigin(plan.cell.x),
                                                             BlockOrigin(plan.cell.y), BlockOrigin(plan.cell.z)};
            m_nest.RecordExternalDispatch(list, context.debugRing, m_mirrorPipeline.Get(),
                                          MR_BLOCK_CELLS / PEEK_GROUP_THREADS, mirrorConstants);
        }

        if (plan.refine) {
            m_nest.RecordRefineShadow(list, context.debugRing, PEEK_MIRROR_SLOT, PEEK_FIRST_SHADOW_SLOT,
                                      PEEK_LEVEL_COUNT, PeekPointOfCell(plan.cell));
        }

        if (plan.step)
            m_nest.RecordStep(list, context.debugRing, ProbeWorldSeed(), context.tick);

        if (plan.pullBack)
            m_nest.RecordPullBack(list, context.debugRing, PEEK_FIRST_SHADOW_SLOT, PEEK_LEVEL_COUNT);

        // --- 抽出の覗きの欄(覗いていなければ段の数 0)---
        const std::array<uint32_t, 4> extractConstants = {m_viewSpecies[0], m_viewSpecies[1], m_viewSpecies[2],
                                                          PEEK_FIRST_SHADOW_SLOT};
        m_nest.RecordExternalDispatch(list, context.debugRing, m_extractPipeline.Get(),
                                      PEEK_LEVEL_COUNT * MR_BLOCK_CELLS / PEEK_GROUP_THREADS, extractConstants);
    }

    // 世界が抽出した境界より前に替えた表を当てる(いちばん新しいものだけが残る)。前の表のバッファは、それを読んだ前のリストが
    // 終わるまで持つ(このリストの keepAlive。キューは順に終わる)
    void ProbePeek::ApplyDueTables(const ProbeExtractContext& context) {
        rng::stable_sort(m_pendingTables, {}, &PendingTable::tick);
        while (!m_pendingTables.empty() && m_pendingTables.front().tick < context.tick) {
            const PendingTable& pending = m_pendingTables.front();
            auto retired = m_nest.ReplaceTable(pending.table);
            if (retired && context.keepAlive != nullptr)
                context.keepAlive->insert(context.keepAlive->end(), retired->begin(), retired->end());

            if (retired) {
                m_viewSpecies = ProbeViewSpecies(pending.table);
                m_state.TableChanged();
            } else {
                Log(Channel::Sim, Level::Error, "覗き窓の反応表を替えられない: {}", retired.error());
            }

            m_pendingTables.erase(m_pendingTables.begin());
        }
    }

    // --- CPU リファレンス ---

    ProbePeekReference::ProbePeekReference(const BakedReactionTable& table)
        : m_table(&table), m_viewSpecies(ProbeViewSpecies(table)), m_nest(MakeMultiresNest(PEEK_CAPACITY)) {}

    void ProbePeekReference::Advance(std::span<const RxCell> world, uint64_t tick) {
        FX_ASSERT(world.size() == PROBE_CELL_COUNT);

        // --- ProbePeek::ApplyDueTables と同じ ---
        rng::stable_sort(m_pendingTables, {}, &PendingTable::tick);
        while (!m_pendingTables.empty() && m_pendingTables.front().tick < tick) {
            m_table = m_pendingTables.front().table;
            m_viewSpecies = ProbeViewSpecies(*m_table);
            m_state.TableChanged();
            m_pendingTables.erase(m_pendingTables.begin());
        }

        const PeekState::Plan plan = m_state.NextPlan(tick);
        if (plan.removeShadow)
            RemoveShadowChain(m_nest, PEEK_FIRST_SHADOW_SLOT, PEEK_LEVEL_COUNT);

        // --- 世界の写し(multires_peek.hlsl の MirrorWorld と同じ)---
        if (plan.mirror) {
            const uint32_t originX = BlockOrigin(plan.cell.x);
            const uint32_t originY = BlockOrigin(plan.cell.y);
            const uint32_t originZ = BlockOrigin(plan.cell.z);
            std::vector<RxCell> cells(MR_BLOCK_CELLS);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                cells[index] = world[ProbeCellIndex(originX + MrCellX(index), originY + MrCellY(index),
                                                    originZ + MrCellZ(index))];
            }

            PlaceMirrorBlock(m_nest, PEEK_MIRROR_SLOT, 0, originX, originY, originZ, cells);
        }

        if (plan.refine) {
            RefineShadowChain(m_nest, PEEK_MIRROR_SLOT, PEEK_FIRST_SHADOW_SLOT, PEEK_LEVEL_COUNT,
                              PeekPointOfCell(plan.cell));
        }

        // 反応は待ちの丸め(ADR-0018。仮の世界と同じ。T-0122)
        if (plan.step)
            StepNest(m_nest, *m_table, ProbeWorldSeed(), tick);

        if (plan.pullBack)
            PullBackShadowChain(m_nest, *m_table, PEEK_FIRST_SHADOW_SLOT, PEEK_LEVEL_COUNT);
    }

    std::vector<uint32_t> ProbePeekReference::Extraction() const {
        std::vector<uint32_t> words(PROBE_EXTRACTION_PEEK_WORDS, 0);
        const ReactionTableView table = m_table->View();

        uint32_t levelCount = 0;
        while (levelCount < PEEK_LEVEL_COUNT &&
               m_nest.blocks[PEEK_FIRST_SHADOW_SLOT + levelCount].kind == MR_BLOCK_SHADOW)
            ++levelCount;

        words[0] = levelCount;
        for (uint32_t level = 0; level < levelCount; ++level) {
            const uint32_t slot = PEEK_FIRST_SHADOW_SLOT + level;
            const MrBlock& block = m_nest.blocks[slot];
            const uint32_t header = PROBE_PEEK_LEVEL_WORDS + (level * PROBE_PEEK_LEVEL_WORDS);
            words[header] = static_cast<uint32_t>(block.level);
            words[header + 1] = static_cast<uint32_t>(static_cast<int32_t>(block.originX));
            words[header + 2] = static_cast<uint32_t>(static_cast<int32_t>(block.originY));
            words[header + 3] = static_cast<uint32_t>(static_cast<int32_t>(block.originZ));

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxCell cell = LoadNestCell(m_nest, slot, index);
                const uint32_t base = PROBE_PEEK_HEADER_WORDS +
                                      (((level * PROBE_PEEK_BLOCK_CELLS) + index) * PROBE_EXTRACTION_CELL_WORDS);
                words[base] = ProbeMakeCache(table, cell).temperature;
                for (uint32_t view = 0; view < PROBE_VIEW_SPECIES_COUNT; ++view)
                    words[base + 1 + view] = ProbeViewAmount(cell, m_viewSpecies[view]);
            }
        }

        return words;
    }

}  // namespace bicameral::sim
