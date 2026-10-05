// multires_conduct.hlsli — 多重解像度の木の上の熱の伝導の GPU の段(T-0107。07 §1・17 §5「熱の伝導」)が共有する、1 ブロックの処理。
// Compute(shaders/sim/multires_conduct.hlsl)と Work Graph(multires_activity_graph.hlsl)の両方から呼ぶ。1 グループ = 1 ブロック、
// CONDUCT_THREADS スレッドで 512 セルを受け持つ。CPU リファレンスは engine/src/sim/multires_conduction.cpp と multires_nest.cpp の
// StepBlocks(同じ順・同じ式 shaders/common/multires_conduction.hlsli)。
//
// 1 刻みの順(gpu_multires.cpp の RecordConduction。CPU の StepBlocks と同じ):
//   1. 刻むブロックを決める(全部: 使っている枠 / 活性: Work Graph の ActivityStepNode が伝導の一覧に足す)
//   2. ConductMarkBlock: 一様で反応が進む・面の流れがある → 頁に広げる印(MR_PAGE_WANTED)。粗い側へ送るなら相手にも頁の印と
//      端数の印(活性なら相手も伝導の一覧へ)。どれも「端数の枠がある」とした場合の流れで決める
//   3. TreeExpand(頁を枠の順に。足りなければ凍らせる印)→ TreeFractions(端数の枠を枠の順に)(multires_tree.hlsl)
//   4. ConductPrepareBlock: 配った頁を一様の値で埋め、配った端数の枠を空にする
//   5. ConductFlowsBlock: 刻むブロックのセルの面の流れを変化に足す(刻みの初めのセルから。Jacobi 型)
//   6. ConductApplyBlock: 変化を足してから反応。活性なら忙しさの印と次の刻みの種
// 順番に依存しない理由: 印は同じ値の書き込み、一覧は集合、変化は 64bit の atomic の足し算(2 の補数の 128bit。
// 端数の桁上がりも整数なので、どの順に足しても同じ)。流れは刻みの初めのセルだけから決まる。
#ifndef BICAMERAL_MULTIRES_CONDUCT_HLSLI
#define BICAMERAL_MULTIRES_CONDUCT_HLSLI

#include "common/multires_conduction.hlsli"
#include "sim/multires_bindings.hlsli"

static const uint32_t CONDUCT_THREADS = 64;
static const uint32_t CONDUCT_CELLS_PER_THREAD = MR_BLOCK_CELLS / CONDUCT_THREADS;

groupshared uint32_t gs_conductAny;       // 流れ・変化があった(グループの OR)
groupshared uint32_t gs_conductPossible;  // 進める規則があった(グループの OR)

// --- 小さな関数 --------------------------------------------------------------------------------

bool IsListedStep() {
    return (g_stepFlags & MR_STEP_LISTED) != 0;
}

// 刻むブロックか(CPU の stepped。全部: 使っている枠〔StepNest〕/ 活性: 印のある世界の枠と観察の枠の全部〔StepActive〕)
bool IsConductStepped(uint32_t slot, MrBlock block) {
    if (IsListedStep())
        return slot >= g_worldBlocks || block.activeTick == CurrentStepMark();

    return block.kind != MR_BLOCK_UNUSED && block.kind != MR_BLOCK_MIRROR;
}

bool IsFrozenSlot(uint32_t slot) {
    return slot < g_worldBlocks && HasConductMark(slot, CONDUCT_MARK_FROZEN);
}

// ブロックの面に接するセルか(一様なブロックの中のセルどうしは同じ値・粗い側への面はブロックの面にしかない)
bool OnBlockSurface(uint32_t index) {
    const uint32_t last = MR_BLOCK_EDGE - 1;
    const uint32_t x = MrCellX(index);
    const uint32_t y = MrCellY(index);
    const uint32_t z = MrCellZ(index);

    return x == 0 || x == last || y == 0 || y == last || z == 0 || z == last;
}

MrThermal ThermalAt(uint32_t slot, uint32_t index) {
    return MrCellThermal(MakeTable(), LoadCell(slot, index));
}

// 頁のセルの変化に足す(整数部と端数を 64bit の atomic で。端数の桁上がりは整数部へ)
void AddConductDelta(uint32_t page, uint32_t index, MrEnergyDelta delta) {
    if (MrEnergyDeltaIsZero(delta))
        return;

    const uint32_t address = ConductDeltaAddress(page, index);
    uint64_t fractionBefore = 0;
    g_conduction.InterlockedAdd64(address + 8, delta.fraction, fractionBefore);
    const int64_t carry = fractionBefore + delta.fraction < delta.fraction ? 1 : 0;
    uint64_t wholeBefore = 0;
    g_conduction.InterlockedAdd64(address, (uint64_t)(delta.whole + carry), wholeBefore);
}

// --- 2. 印 --------------------------------------------------------------------------------------

// 粗い側へ送る相手: 一様なら頁に広げる印、端数が要るなら端数の印。活性なら伝導の一覧へ(変化を足すため)
void MarkCoarseTarget(uint32_t coarseSlot, bool wantsFraction) {
    if (g_blocks[coarseSlot].page == MR_NO_PAGE)
        g_blocks[coarseSlot].page = MR_PAGE_WANTED;

    if (wantsFraction)
        SetConductMark(coarseSlot, CONDUCT_MARK_FRACTION);

    if (IsListedStep())
        ConductAppend(coarseSlot);
}

// セル 1 つの面を調べて印を付ける。流れがあれば true(CPU の CollectCellFaces + MarkCrossSends)
bool MarkCell(uint32_t slot, MrBlock block, uint32_t index) {
    const MrThermal self = MrCellThermal(MakeTable(), LoadBlockCell(block, slot, index));
    int64_t sameLevelOutflow = 0;
    bool sends = false;
    for (uint32_t face = 0; face < MR_FACES; ++face) {
        const MrFaceNeighbor neighbor = MrFindFaceNeighbor(MakeTree(), slot, block, index, face, g_rootLevel);
        if (neighbor.kind == MR_NEIGHBOR_SAME) {
            sameLevelOutflow += MrSameLevelFlow(self, ThermalAt(neighbor.slot, neighbor.index), block.level);
            continue;
        }

        if (neighbor.kind != MR_NEIGHBOR_COARSER)
            continue;

        const int64_t flow = MrCrossLevelFlow(self, ThermalAt(neighbor.slot, neighbor.index), block.level,
                                              neighbor.gap);
        if (flow == 0)
            continue;

        const MrCrossTransfer transfer = MrSplitCrossFlow(flow, neighbor.gap, true);
        if (transfer.fineDelta == 0)
            continue;

        sends = true;
        MarkCoarseTarget(neighbor.slot, transfer.coarseDelta.fraction != 0);
    }

    return sends || sameLevelOutflow != 0;
}

// 刻むブロック 1 つ: 一様で反応が進むか流れがあれば頁に広げる印(CPU の StepBlocks の最初と MarkBlockWants)
void ConductMarkBlock(uint32_t slot, uint32_t thread) {
    const MrBlock block = g_blocks[slot];
    if (!IsConductStepped(slot, block))
        return;

    if (thread == 0)
        gs_conductAny = 0;

    GroupMemoryBarrierWithGroupSync();
    for (uint32_t k = 0; k < CONDUCT_CELLS_PER_THREAD; ++k) {
        const uint32_t index = thread + (CONDUCT_THREADS * k);
        if (!OnBlockSurface(index) || !MrIsSteppedCell(block, index))
            continue;

        if (MarkCell(slot, block, index))
            InterlockedOr(gs_conductAny, 1u);
    }

    GroupMemoryBarrierWithGroupSync();
    if (thread != 0 || !MrIsUniform(block))
        return;

    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    if (gs_conductAny != 0 || MrUniformWouldChange(MakeTable(), g_cells[slot], seed, tick, block))
        g_blocks[slot].page = MR_PAGE_WANTED;
}

// --- 4. 配った頁と端数の枠 ----------------------------------------------------------------------

void ConductPrepareBlock(uint32_t slot, uint32_t thread) {
    if (slot >= g_worldBlocks)
        return;

    const MrBlock block = g_blocks[slot];
    const bool expanded = HasConductMark(slot, CONDUCT_MARK_EXPANDED);
    const bool granted = HasConductMark(slot, CONDUCT_MARK_GRANTED);
    const RxCell value = g_cells[slot];
    for (uint32_t k = 0; k < CONDUCT_CELLS_PER_THREAD; ++k) {
        const uint32_t index = thread + (CONDUCT_THREADS * k);
        if (expanded)
            g_cells[PageCellAddress(block.page, index)] = MrUniformCell(block, value, index);

        if (granted)
            g_fractions[FractionAddress(block.fraction, index)] = MrMakeEmptyFraction();
    }
}

// --- 5. 面の流れ ---------------------------------------------------------------------------------

// セル 1 つの面の流れを変化に足す(自分と、粗い側へ送った先。CPU の AddCellFlows)。凍らせたブロックとの面は流れない
void AddCellFlows(uint32_t slot, MrBlock block, uint32_t index) {
    const MrThermal self = MrCellThermal(MakeTable(), g_cells[PageCellAddress(block.page, index)]);
    int64_t own = 0;
    for (uint32_t face = 0; face < MR_FACES; ++face) {
        const MrFaceNeighbor neighbor = MrFindFaceNeighbor(MakeTree(), slot, block, index, face, g_rootLevel);
        if (neighbor.kind != MR_NEIGHBOR_SAME && neighbor.kind != MR_NEIGHBOR_COARSER)
            continue;

        if (IsFrozenSlot(neighbor.slot))
            continue;

        const MrThermal other = ThermalAt(neighbor.slot, neighbor.index);
        if (neighbor.kind == MR_NEIGHBOR_SAME) {
            own -= MrSameLevelFlow(self, other, block.level);
            continue;
        }

        const int64_t flow = MrCrossLevelFlow(self, other, block.level, neighbor.gap);
        if (flow == 0)
            continue;

        const MrBlock coarse = g_blocks[neighbor.slot];
        const MrCrossTransfer transfer = MrSplitCrossFlow(flow, neighbor.gap, coarse.fraction != MR_NO_FRACTION);
        if (transfer.fineDelta == 0)
            continue;

        FX_ASSERT(!MrIsUniform(coarse));
        own += transfer.fineDelta;
        AddConductDelta(coarse.page, neighbor.index, transfer.coarseDelta);
    }

    MrEnergyDelta delta = MrMakeEnergyDelta();
    delta.whole = own;
    AddConductDelta(block.page, index, delta);
}

void ConductFlowsBlock(uint32_t slot, uint32_t thread) {
    const MrBlock block = g_blocks[slot];
    if (!IsConductStepped(slot, block) || MrIsUniform(block) || IsFrozenSlot(slot))
        return;

    for (uint32_t k = 0; k < CONDUCT_CELLS_PER_THREAD; ++k) {
        const uint32_t index = thread + (CONDUCT_THREADS * k);
        if (MrIsSteppedCell(block, index))
            AddCellFlows(slot, block, index);
    }
}

// --- 6. 変化を足して反応 -------------------------------------------------------------------------

// セル 1 つ: 変化を足し(読んだら 0 に戻す)、react なら反応を進める。変わったら STEP_CHANGED・進める規則があれば STEP_POSSIBLE
static const uint32_t CONDUCT_STEP_POSSIBLE = 1;
static const uint32_t CONDUCT_STEP_CHANGED = 2;

uint32_t ApplyCell(uint32_t slot, MrBlock block, uint32_t index, bool react) {
    const uint32_t address = PageCellAddress(block.page, index);
    RxCell cell = g_cells[address];
    const RxCell before = cell;
    MrFraction fraction = LoadFraction(block.fraction, index);
    const uint64_t fractionBefore = fraction.energy;

    // --- 伝導の変化(CPU の ApplyEnergyDelta)---
    const uint32_t deltaAddress = ConductDeltaAddress(block.page, index);
    MrEnergyDelta delta;
    delta.whole = (int64_t)g_conduction.Load<uint64_t>(deltaAddress);
    delta.fraction = g_conduction.Load<uint64_t>(deltaAddress + 8);
    if (!MrEnergyDeltaIsZero(delta)) {
        g_conduction.Store<uint64_t>(deltaAddress, 0);
        g_conduction.Store<uint64_t>(deltaAddress + 8, 0);
        MrEnergyDelta value;
        value.whole = cell.energy;
        value.fraction = fraction.energy;
        value = MrAddEnergyDelta(value, delta);
        cell.energy = value.whole;
        FX_ASSERT(block.fraction != MR_NO_FRACTION || value.fraction == 0);
        if (block.fraction != MR_NO_FRACTION) {
            fraction.energy = value.fraction;
            g_fractions[FractionAddress(block.fraction, index)] = fraction;
        }
    }

    // --- 反応 ---
    uint32_t result = 0;
    if (react) {
        const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
        const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
        const RxCellStep step = MrStepCellDetailed(MakeTable(), cell, seed, tick, block, index);
        result |= step.possible != 0 ? CONDUCT_STEP_POSSIBLE : 0u;
        cell = step.cell;
    }

    g_cells[address] = cell;
    if (MrCellChanged(before, cell) || fraction.energy != fractionBefore)
        result |= CONDUCT_STEP_CHANGED;

    return result;
}

// 頁を持つブロック 1 つ(CPU の StepPagedBlock)。活性なら、世界の枠の忙しさの印と次の刻みの種(CPU の StepActive の後ろ)
void ConductApplyBlock(uint32_t slot, uint32_t thread) {
    const MrBlock block = g_blocks[slot];
    if (MrIsUniform(block))
        return;

    if (thread == 0) {
        gs_conductAny = 0;
        gs_conductPossible = 0;
    }

    GroupMemoryBarrierWithGroupSync();
    const bool react = IsConductStepped(slot, block);
    for (uint32_t k = 0; k < CONDUCT_CELLS_PER_THREAD; ++k) {
        const uint32_t index = thread + (CONDUCT_THREADS * k);
        if (!MrIsSteppedCell(block, index))
            continue;

        const uint32_t result = ApplyCell(slot, block, index, react);
        if ((result & CONDUCT_STEP_CHANGED) != 0)
            InterlockedOr(gs_conductAny, 1u);

        if ((result & CONDUCT_STEP_POSSIBLE) != 0)
            InterlockedOr(gs_conductPossible, 1u);
    }

    GroupMemoryBarrierWithGroupSync();
    if (thread != 0 || !IsListedStep() || slot >= g_worldBlocks)
        return;

    // --- 変わった・頁に広げたら忙しい(T-0101・T-0103)。進める規則があった・変わったら次の刻みの種(T-0019)---
    const bool changed = gs_conductAny != 0;
    if (changed || HasConductMark(slot, CONDUCT_MARK_EXPANDED))
        g_blocks[slot].busyTick = CurrentStepMark();

    if (changed || gs_conductPossible != 0)
        AppendActivity(slot);
}

#endif  // BICAMERAL_MULTIRES_CONDUCT_HLSLI
