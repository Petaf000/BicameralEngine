// multires_tree.cpp — 多重解像度の世界の木の管理の CPU リファレンス(multires_nest.h。17 §5「木の管理」。T-0018。ADR-0016)。
// 要求の処理の段(Resolve・Settle・Allocate・Apply・Release・索引の作り直し)を、GPU(shaders/sim/multires_tree.hlsl の段と
// multires_graph.hlsl の鎖)と同じ順・同じ関数で行う。GPU は段の中を並列に走らせるが、1 刻みに 1 つの親は 1 つの要求だけが触るので
// 書き込みは重ならず、結果は要求の順に 1 つずつ行うこの実装と同じになる。
//
// 枠の番号(ADR-0016): 要求 i は空きのスタックの上から「i より前の許可した要求の数の和」だけ下から順に取る。返すのも要求の順に積む。
// 索引は開番地法(線形探査)。入れる = 空の所(墓石は使い回さない)、消す = 墓石。表の中の並びは GPU と違ってよい。
#include <algorithm>

#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        using nest_detail::CellAt;
        using nest_detail::FractionAt;
        using nest_detail::SetFraction;

        // --- 索引 ---

        uint32_t IndexMask(const MultiresNest& nest) {
            return nest.capacity.indexEntries - 1;
        }

        void IndexInsert(MultiresNest& nest, uint32_t slot) {
            const MrBlock& block = nest.blocks[slot];
            const uint32_t home = MrIndexHome(block.level, block.originX, block.originY, block.originZ,
                                              nest.capacity.indexEntries);
            for (uint32_t probe = 0; probe < nest.capacity.indexEntries; ++probe) {
                uint32_t& entry = nest.index[(home + probe) & IndexMask(nest)];
                if (entry != MR_INDEX_EMPTY)
                    continue;

                entry = slot;
                return;
            }

            nest.counters[MR_COUNTER_INDEX_FULL] += 1;
        }

        void IndexRemove(MultiresNest& nest, uint32_t slot) {
            const MrBlock& block = nest.blocks[slot];
            const uint32_t home = MrIndexHome(block.level, block.originX, block.originY, block.originZ,
                                              nest.capacity.indexEntries);
            for (uint32_t probe = 0; probe < nest.capacity.indexEntries; ++probe) {
                uint32_t& entry = nest.index[(home + probe) & IndexMask(nest)];
                if (entry == MR_INDEX_EMPTY)
                    return;

                if (entry != slot)
                    continue;

                entry = MR_INDEX_TOMBSTONE;
                nest.counters[MR_COUNTER_TOMBSTONES] += 1;
                return;
            }
        }

        // --- 端数の枠 ---

        void Release(MrRequestState& state, uint32_t fractionSlot) {
            FX_ASSERT(state.releaseCount < MR_MAX_RELEASES);
            state.releases[state.releaseCount] = fractionSlot;
            state.releaseCount += 1;
        }

        uint32_t PoppedBlock(const MultiresNest& nest, const MrRequestState& state, uint32_t i) {
            return nest.freeBlocks[state.blockBase - 1 - i];
        }

        uint32_t PoppedFraction(const MultiresNest& nest, const MrRequestState& state, uint32_t i) {
            if (i >= state.fractionNeed)
                return MR_NO_FRACTION;

            return nest.freeFractions[state.fractionBase - 1 - i];
        }

        // --- 1. 解決 ---

        void ResolveRefine(MultiresNest& nest, uint32_t i) {
            const MrRequest& request = nest.requests[i];
            MrRequestState& state = nest.states[i];
            const int32_t rootLevel = nest.capacity.rootLevel;
            const uint32_t root = LookupBlock(
                nest, rootLevel, MrBlockOriginOf(MrCoarserCoordinate(request.x, request.level, rootLevel)),
                MrBlockOriginOf(MrCoarserCoordinate(request.y, request.level, rootLevel)),
                MrBlockOriginOf(MrCoarserCoordinate(request.z, request.level, rootLevel)));
            if (root == MR_NO_BLOCK) {
                state.status = MR_STATUS_INVALID;
                return;
            }

            // --- 子の番号をたどって、点を含む最も深い本物のブロックへ ---
            uint32_t deepest = root;
            while (nest.blocks[deepest].level < request.level) {
                const MrBlock& block = nest.blocks[deepest];
                const uint32_t
                    child = block.children[MrOctantOfPoint(block, request.x, request.y, request.z, request.level)];
                if (child == MR_NO_BLOCK)
                    break;

                deepest = child;
            }

            const int32_t deepestLevel = nest.blocks[deepest].level;
            if (deepestLevel == request.level) {
                state.status = MR_STATUS_ALREADY;
                return;
            }

            state.target = deepest;
            state.claimSlot = deepest;
            state.levels = std::min(static_cast<uint32_t>(request.level - deepestLevel), MR_MAX_CHAIN_LEVELS);
            nest.claims[deepest] = std::min(nest.claims[deepest], i);
        }

        void ResolveCoarsen(MultiresNest& nest, uint32_t i) {
            const MrRequest& request = nest.requests[i];
            MrRequestState& state = nest.states[i];
            const uint32_t slot = LookupBlock(nest, request.level, MrBlockOriginOf(request.x),
                                              MrBlockOriginOf(request.y), MrBlockOriginOf(request.z));
            if (slot == MR_NO_BLOCK || nest.blocks[slot].parent == MR_NO_BLOCK || MrHasRealChild(nest.blocks[slot])) {
                state.status = MR_STATUS_INVALID;
                return;
            }

            const uint32_t parent = nest.blocks[slot].parent;
            state.target = slot;
            state.claimSlot = parent;
            nest.claims[parent] = std::min(nest.claims[parent], i);
        }

        void Resolve(MultiresNest& nest, uint32_t i) {
            const MrRequest& request = nest.requests[i];
            nest.states[i] = MrMakeRequestState();
            const int32_t span = request.level - nest.capacity.rootLevel;
            if (span < 0 || span > static_cast<int32_t>(MR_MAX_LEVEL_SPAN)) {
                nest.states[i].status = MR_STATUS_INVALID;
                return;
            }

            if (request.op == MR_REQUEST_REFINE)
                ResolveRefine(nest, i);
            else if (request.op == MR_REQUEST_COARSEN)
                ResolveCoarsen(nest, i);
            else
                nest.states[i].status = MR_STATUS_INVALID;
        }

        // --- 2. 確定: 取り合いに負けたら後回し。要る枠の数(端数は控えめに多く取り、適用の後で 0 なら返す)---

        void Settle(MultiresNest& nest, uint32_t i) {
            MrRequestState& state = nest.states[i];
            if (state.status != MR_STATUS_PENDING)
                return;

            const bool coarsen = nest.requests[i].op == MR_REQUEST_COARSEN;
            const bool lost = nest.claims[state.claimSlot] != i;
            const bool targetTaken = coarsen && nest.claims[state.target] != MR_NO_CLAIM;
            if (lost || targetTaken) {
                state.status = MR_STATUS_CONFLICT;
                return;
            }

            if (coarsen) {
                const uint32_t parent = nest.blocks[state.target].parent;
                state.fractionNeed = nest.blocks[parent].fraction == MR_NO_FRACTION ? 1 : 0;
                return;
            }

            state.blockNeed = state.levels;
            state.fractionNeed = nest.blocks[state.target].fraction != MR_NO_FRACTION ? state.levels : 0;
        }

        // --- 3. 割り当て: 要求の順の累積和が空きの数以下なら許可(許可は一覧の前から続く)---

        void Allocate(MultiresNest& nest, uint32_t count) {
            const uint32_t freeBlocks = nest.counters[MR_COUNTER_FREE_BLOCKS];
            const uint32_t freeFractions = nest.counters[MR_COUNTER_FREE_FRACTIONS];
            uint32_t blockSum = 0;
            uint32_t fractionSum = 0;
            uint32_t grantedBlocks = 0;
            uint32_t grantedFractions = 0;
            for (uint32_t i = 0; i < count; ++i) {
                MrRequestState& state = nest.states[i];
                if (state.status != MR_STATUS_PENDING)
                    continue;

                const uint32_t blockBase = freeBlocks - blockSum;
                const uint32_t fractionBase = freeFractions - fractionSum;
                blockSum += state.blockNeed;
                fractionSum += state.fractionNeed;
                if (blockSum > freeBlocks || fractionSum > freeFractions) {
                    state.status = MR_STATUS_NO_SPACE;
                    continue;
                }

                state.status = MR_STATUS_GRANTED;
                state.blockBase = blockBase;
                state.fractionBase = fractionBase;
                grantedBlocks = blockSum;
                grantedFractions = fractionSum;
            }

            nest.counters[MR_COUNTER_FREE_BLOCKS] = freeBlocks - grantedBlocks;
            nest.counters[MR_COUNTER_FREE_FRACTIONS] = freeFractions - grantedFractions;
        }

        // --- 4. 適用: 細かくする(1 段)---

        void RefineRequestLevel(MultiresNest& nest, uint32_t i, uint32_t depth, uint32_t parentSlot) {
            const MrRequest& request = nest.requests[i];
            MrRequestState& state = nest.states[i];
            const uint32_t childSlot = PoppedBlock(nest, state, depth);
            const uint32_t candidate = PoppedFraction(nest, state, depth);
            MrBlock& parent = nest.blocks[parentSlot];
            const uint32_t octant = MrOctantOfPoint(parent, request.x, request.y, request.z, request.level);

            // --- 端数: 親の八分の一に 0 でない端数があれば、取っておいた枠に写す。親の残り(覆わない所)に 0 でない端数が残るか ---
            bool octantFraction = false;
            bool restFraction = false;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const bool nonzero = !MrFractionIsZero(FractionAt(nest, parent.fraction, index));
                const bool covered = MrOctantOfCell(index) == octant;
                octantFraction |= nonzero && covered;
                restFraction |= nonzero && !covered;
            }

            FX_ASSERT(!octantFraction || candidate != MR_NO_FRACTION);
            const uint32_t childFraction = octantFraction ? candidate : MR_NO_FRACTION;

            // --- セルと端数: 子は親と同じ数 ---
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const uint32_t parentCell = MrParentCellOfChild(octant, index);
                CellAt(nest, childSlot, index) = CellAt(nest, parentSlot, parentCell);
                if (childFraction != MR_NO_FRACTION)
                    SetFraction(nest, childFraction, index, FractionAt(nest, parent.fraction, parentCell));
            }

            // --- 親を覆う(覆われた親のセルと端数は空)---
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                const uint32_t index = MrOctantCell(octant, local);
                CellAt(nest, parentSlot, index) = RxMakeEmptyCell(0);
                if (parent.fraction != MR_NO_FRACTION)
                    SetFraction(nest, parent.fraction, index, MrMakeEmptyFraction());
            }

            // --- 見出し・返す枠・索引 ---
            if (candidate != MR_NO_FRACTION && childFraction == MR_NO_FRACTION)
                Release(state, candidate);

            if (parent.fraction != MR_NO_FRACTION && !restFraction) {
                Release(state, parent.fraction);
                parent.fraction = MR_NO_FRACTION;
            }

            parent.children[octant] = childSlot;
            nest.blocks[childSlot] = MrMakeChildBlock(parent, parentSlot, octant, MR_BLOCK_REAL, childFraction);
            IndexInsert(nest, childSlot);
        }

        void ApplyRefine(MultiresNest& nest, uint32_t i) {
            uint32_t parent = nest.states[i].target;
            for (uint32_t depth = 0; depth < nest.states[i].levels; ++depth) {
                RefineRequestLevel(nest, i, depth, parent);
                parent = PoppedBlock(nest, nest.states[i], depth);
            }
        }

        // --- 4. 適用: 粗くする(1 段)---

        MrChildren GatherChildren(const MultiresNest& nest, uint32_t childSlot, uint32_t fractionSlot, uint32_t local) {
            MrChildren children{};
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                const uint32_t index = MrChildCell(local, j);
                children.cells[j] = CellAt(nest, childSlot, index);
                children.fractions[j] = FractionAt(nest, fractionSlot, index);
            }

            return children;
        }

        void AddToLedger(MultiresNest& nest, int32_t level, uint32_t column, uint32_t lostBits) {
            const uint32_t address = MrLedgerAddress(level, column, nest.capacity.ledgerColumns);
            if (address == MR_NO_BLOCK) {
                nest.counters[MR_COUNTER_LEDGER_OUTSIDE] += 1;
                return;
            }

            nest.ledger[address] += lostBits;
        }

        void RecordCoarsened(MultiresNest& nest, const MrCoarsened& result, int32_t childLevel) {
            nest.counters[MR_COUNTER_LOST] += result.lostCount;
            nest.counters[MR_COUNTER_OVERFLOW] += result.overflowCount;
            if (result.energyLostBits != 0)
                AddToLedger(nest, childLevel, 0, result.energyLostBits);

            for (uint32_t k = 0; k < result.lostSpeciesCount; ++k)
                AddToLedger(nest, childLevel, 1 + result.lostSpecies[k], result.lostBits[k]);
        }

        void ApplyCoarsen(MultiresNest& nest, uint32_t i) {
            MrRequestState& state = nest.states[i];
            const uint32_t childSlot = state.target;
            const MrBlock child = nest.blocks[childSlot];
            MrBlock& parent = nest.blocks[child.parent];

            // --- 子 2³ を 1 つに(親の八分の一の 64 セル)---
            std::array<MrCoarsened, MR_OCTANT_CELLS> results{};
            bool octantFraction = false;
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                results[local] = MrCoarsenCell(GatherChildren(nest, childSlot, child.fraction, local));
                RecordCoarsened(nest, results[local], child.level);
                octantFraction |= !MrFractionIsZero(results[local].fraction);
            }

            // --- 親の端数: 枠が無ければ取っておいた枠を全部 0 で始める。残り(八分の一の外)に 0 でない端数があるか ---
            bool restFraction = false;
            const uint32_t newFraction = PoppedFraction(nest, state, 0);
            if (parent.fraction == MR_NO_FRACTION) {
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                    SetFraction(nest, newFraction, index, MrMakeEmptyFraction());
                parent.fraction = newFraction;
            }

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (MrOctantOfCell(index) != child.parentOctant)
                    restFraction |= !MrFractionIsZero(FractionAt(nest, parent.fraction, index));
            }

            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                const uint32_t index = MrOctantCell(child.parentOctant, local);
                CellAt(nest, child.parent, index) = results[local].cell;
                SetFraction(nest, parent.fraction, index, results[local].fraction);
            }

            // --- 見出し・返す枠・索引 ---
            if (!octantFraction && !restFraction) {
                Release(state, parent.fraction);
                parent.fraction = MR_NO_FRACTION;
            }

            if (child.fraction != MR_NO_FRACTION)
                Release(state, child.fraction);

            state.releaseBlock = childSlot;
            parent.children[child.parentOctant] = MR_NO_BLOCK;
            IndexRemove(nest, childSlot);
            nest.blocks[childSlot] = MrMakeUnusedBlock();
        }

        // --- 5. 解放: 返す枠を要求の順に積む・取り合いの印を消す・数える ---

        void ReleaseAll(MultiresNest& nest, uint32_t count) {
            for (uint32_t i = 0; i < count; ++i) {
                const MrRequestState& state = nest.states[i];
                if (state.claimSlot != MR_NO_CLAIM)
                    nest.claims[state.claimSlot] = MR_NO_CLAIM;

                nest.counters[MrStatusCounter(state.status)] += 1;
                if (state.status != MR_STATUS_GRANTED)
                    continue;

                for (uint32_t k = 0; k < state.releaseCount; ++k)
                    nest.freeFractions[nest.counters[MR_COUNTER_FREE_FRACTIONS]++] = state.releases[k];

                if (state.releaseBlock != MR_NO_BLOCK)
                    nest.freeBlocks[nest.counters[MR_COUNTER_FREE_BLOCKS]++] = state.releaseBlock;
            }

            // --- 墓石が表の 1/4 を超えたら作り直す ---
            const bool rebuild = nest.counters[MR_COUNTER_TOMBSTONES] * 4 > nest.capacity.indexEntries;
            nest.counters[MR_COUNTER_REBUILD_INDEX] = rebuild ? 1 : 0;
            if (rebuild)
                nest.counters[MR_COUNTER_TOMBSTONES] = 0;

            nest.counters[MR_COUNTER_REQUESTS] = 0;
        }

    }  // namespace

    uint32_t PlaceRootBlock(MultiresNest& nest, int64_t originX, int64_t originY, int64_t originZ,
                            std::span<const RxCell> cells) {
        FX_ASSERT(cells.size() == MR_BLOCK_CELLS);
        FX_ASSERT(nest.counters[MR_COUNTER_FREE_BLOCKS] > 0);
        const uint32_t slot = nest.freeBlocks[--nest.counters[MR_COUNTER_FREE_BLOCKS]];

        MrBlock block = MrMakeUnusedBlock();
        block.originX = originX;
        block.originY = originY;
        block.originZ = originZ;
        block.level = nest.capacity.rootLevel;
        block.kind = MR_BLOCK_REAL;
        nest.blocks[slot] = block;
        std::ranges::copy(cells, nest.cells.begin() + static_cast<ptrdiff_t>(size_t{slot} * MR_BLOCK_CELLS));
        IndexInsert(nest, slot);

        return slot;
    }

    void SubmitRequests(MultiresNest& nest, std::span<const MrRequest> requests) {
        uint32_t& count = nest.counters[MR_COUNTER_REQUESTS];
        FX_ASSERT(count + requests.size() <= MR_MAX_REQUESTS);
        for (const MrRequest& request : requests)
            nest.requests[count++] = request;
    }

    void ProcessRequests(MultiresNest& nest) {
        const uint32_t count = nest.counters[MR_COUNTER_REQUESTS];
        for (uint32_t i = 0; i < count; ++i)
            Resolve(nest, i);

        for (uint32_t i = 0; i < count; ++i)
            Settle(nest, i);

        Allocate(nest, count);
        for (uint32_t i = 0; i < count; ++i) {
            if (nest.states[i].status != MR_STATUS_GRANTED)
                continue;

            if (nest.requests[i].op == MR_REQUEST_REFINE)
                ApplyRefine(nest, i);
            else
                ApplyCoarsen(nest, i);
        }

        ReleaseAll(nest, count);
        if (nest.counters[MR_COUNTER_REBUILD_INDEX] != 0)
            RebuildIndex(nest);
    }

    uint32_t LookupBlock(const MultiresNest& nest, int32_t level, int64_t originX, int64_t originY, int64_t originZ) {
        const uint32_t home = MrIndexHome(level, originX, originY, originZ, nest.capacity.indexEntries);
        for (uint32_t probe = 0; probe < nest.capacity.indexEntries; ++probe) {
            const uint32_t entry = nest.index[(home + probe) & IndexMask(nest)];
            if (entry == MR_INDEX_EMPTY)
                return MR_NO_BLOCK;

            if (entry != MR_INDEX_TOMBSTONE && MrBlockHasKey(nest.blocks[entry], level, originX, originY, originZ))
                return entry;
        }

        return MR_NO_BLOCK;
    }

    void RebuildIndex(MultiresNest& nest) {
        std::ranges::fill(nest.index, MR_INDEX_EMPTY);
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            if (nest.blocks[slot].kind == MR_BLOCK_REAL)
                IndexInsert(nest, slot);
        }
    }

}  // namespace bicameral::sim
