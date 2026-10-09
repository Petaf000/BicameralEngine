// multires_conduct.hlsli — 多重解像度の木の上の熱の伝導の GPU の段(T-0107。07 §1・17 §5「熱の伝導」)が共有する、1 ブロックの処理。
// Compute(shaders/sim/multires_conduct.hlsl)と Work Graph(multires_activity_graph.hlsl)の両方から呼ぶ。1 グループ = 1 ブロック、
// CONDUCT_THREADS スレッドで 512 セルを受け持つ。CPU リファレンスは engine/src/sim/multires_conduction.cpp と multires_nest.cpp の
// StepBlocks(同じ順・同じ式 shaders/common/multires_conduction.hlsli)。
//
// 1 刻みの順(gpu_multires.cpp の RecordConduction。CPU の StepBlocks と同じ):
//   1. 刻むブロックを決める(全部: 使っている枠 / 活性: Work Graph の ActivityStepNode が伝導の一覧に足す)
//   2. ConductMarkBlock: 一様で反応が進む(待ちの丸め。起こす刻みが来た時だけ評価し、変わらなければ起こす刻みを書く)・面の流れがある
//      → 頁に広げる印(MR_PAGE_WANTED)。粗い側へ送るなら相手にも頁の印と端数の印(活性なら相手も伝導の一覧へ)。
//      どれも「端数の枠がある」とした場合の流れで決める
//   3. TreeExpand(頁を枠の順に。足りなければ凍らせる印)→ TreeFractions(端数の枠を枠の順に)(multires_tree.hlsl)
//   4. ConductPrepareBlock: 配った頁を一様の値で埋め、配った端数の枠を空にする
//   5. ConductFlowsBlock: 刻むブロックのセルの面の流れを変化に足す(刻みの初めのセルから。Jacobi 型)
//   6. ConductApplyBlock: 変化を足してから反応(待ちの丸め)。見出しの busyTick(tc)・wakeTick を CPU の RecordWaitResults と同じに書く
// 反応は待ちの丸めだけ(ADR-0018。T-0125)。刻む前に起こす段(multires_step.hlsl の WakeDue)がつつかれたブロックの印を直してある。
// 次の刻みの種は書かない(変わったブロックは wakeTick = 次の刻みで、起こす段が種にする)。
// 細かいレベルの刻み(T-0109。CPU の StepConduction): 2〜5 を小刻みごとに繰り返す(小刻みの番号は stepFlags。gpu_multires.cpp の
// RecordConduction が最大回数 4^maxSubcycleGap を積み、その小刻みに始まるレベルが無ければ段は空で抜ける)。印と流れはその小刻みに
// 始まるレベルのブロックだけ。最後の小刻みの手前までは、その後に ConductEndBlock(小刻みが終わるレベルに溜めた変化を足す。
// 活性なら変わったブロックを起こす一覧へ → 活性のグラフの ActivitySeedNode が面の隣を起こす)。最後の小刻みの変化は 6 で反応と一緒に足す。
// 順番に依存しない理由: 印は同じ値の書き込み、一覧は集合、変化は 64bit の atomic の足し算(2 の補数の 128bit。
// 端数の桁上がりも整数なので、どの順に足しても同じ)。流れは刻みの初めのセルだけから決まる。
#ifndef BICAMERAL_MULTIRES_CONDUCT_HLSLI
#define BICAMERAL_MULTIRES_CONDUCT_HLSLI

#include "common/multires_conduction.hlsli"
#include "sim/multires_bindings.hlsli"
#include "sim/multires_wait_step.hlsli"

// 印・流れ・足して反応の段は 1 スレッド = 1 セル(T-0111)。64 スレッドで 8 セルずつ順に計算すると、小刻み 1 回が 1 ブロックの直列の遅延
// (約 0.54 ms)で決まっていた。中身の軽い段(埋める・小刻みの終わり)は 64 スレッドのまま(空で抜けるグループを投げる費用がスレッドの数に比例する)
static const uint32_t CONDUCT_THREADS = MR_BLOCK_CELLS;
static const uint32_t CONDUCT_CELLS_PER_THREAD = MR_BLOCK_CELLS / CONDUCT_THREADS;
static const uint32_t CONDUCT_LIGHT_THREADS = 64;
static const uint32_t CONDUCT_LIGHT_CELLS_PER_THREAD = MR_BLOCK_CELLS / CONDUCT_LIGHT_THREADS;

groupshared uint32_t gs_conductAny;       // 流れ・変化があった(グループの OR)
groupshared uint32_t gs_conductReacts;    // 一様なブロックの反応でセルが変わる(グループの OR)
groupshared uint32_t gs_conductWakeHigh;  // 起こす刻みのグループの最小(上位・下位 32bit。GroupMinTick)
groupshared uint32_t gs_conductWakeLow;

// 自分のブロックのセルの熱と、ブロックの外の面の先(T-0111)。印と流れの段の初めに、セルの熱(MrCellThermal)を 1 セル 1 回、
// ブロックの面の外の隣(MrFindFaceNeighbor の索引引きとその熱)を面のセル 1 つに 1 回だけ、別々のスレッドで計算し、面ごとの計算はここを読む。
// 段の間はセルも木も変わらないので、毎回計算するのと同じ値。熱は x 温度・y コンダクタンス・zw 熱容量の上限の下位・上位、
// 外の隣は x kind・y 枠・z セル・w レベルの差。外の隣の番号 = 面 × 64 + 面の上の位置(HaloEntry)
static const uint32_t CONDUCT_FACE_CELLS = MR_BLOCK_EDGE * MR_BLOCK_EDGE;
static const uint32_t CONDUCT_HALO_CELLS = MR_FACES * CONDUCT_FACE_CELLS;

groupshared uint4 gs_thermals[MR_BLOCK_CELLS];
groupshared uint4 gs_haloNeighbors[CONDUCT_HALO_CELLS];
groupshared uint4 gs_haloThermals[CONDUCT_HALO_CELLS];

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

// グループの OR に足す。ウェーブで 1 回にまとめる(512 スレッドが同じ groupshared の語に atomic を打つと直列になる。T-0111)
void OrGroupAny(bool value) {
    if (WaveActiveAnyTrue(value) && WaveIsFirstLane())
        InterlockedOr(gs_conductAny, 1u);
}

void OrGroupReacts(bool value) {
    if (WaveActiveAnyTrue(value) && WaveIsFirstLane())
        InterlockedOr(gs_conductReacts, 1u);
}

// グループの 64bit の最小(全部のスレッドが呼ぶ。グループで一様な所で)。上位 32bit の最小を取り、その上位を持つスレッドの下位の最小を取る
// (どちらも順によらない。64bit の atomic とウェーブの演算を避ける。HW の 64bit の不具合は T-0124)
uint64_t GroupMinTick(uint32_t thread, uint64_t value) {
    if (thread == 0) {
        gs_conductWakeHigh = 0xFFFFFFFFu;
        gs_conductWakeLow = 0xFFFFFFFFu;
    }

    GroupMemoryBarrierWithGroupSync();
    const uint32_t high = (uint32_t)(value >> 32);
    const uint32_t waveHigh = WaveActiveMin(high);
    if (WaveIsFirstLane())
        InterlockedMin(gs_conductWakeHigh, waveHigh);

    GroupMemoryBarrierWithGroupSync();
    const uint32_t low = high == gs_conductWakeHigh ? (uint32_t)value : 0xFFFFFFFFu;
    const uint32_t waveLow = WaveActiveMin(low);
    if (WaveIsFirstLane())
        InterlockedMin(gs_conductWakeLow, waveLow);

    GroupMemoryBarrierWithGroupSync();

    return FX_U64(gs_conductWakeHigh, gs_conductWakeLow);
}

bool IsFrozenSlot(uint32_t slot) {
    return slot < g_worldBlocks && HasSubstepMark(slot, CONDUCT_MARK_FROZEN);
}

// --- 細かいレベルの刻み(T-0109。式は multires_conduction.hlsli の MrSubcycleShift・MrSubstepBegins / Ends)---

uint32_t SubcycleGap() {
    return (g_stepFlags >> MR_STEP_GAP_SHIFT) & 3u;
}

int32_t SubcycleBaseLevel() {
    return ((int32_t)g_stepFlags) >> MR_STEP_BASE_SHIFT;  // 符号付きの右シフト(上位 16bit が符号付きの基準)
}

// レベル level の 1 刻みの小刻みの数の指数(4^σ 回)
uint32_t LevelSubcycleShift(int32_t level) {
    return MrSubcycleShift(level, SubcycleBaseLevel(), SubcycleGap());
}

bool SubstepBegins(int32_t level) {
    return MrSubstepBegins(CurrentSubstep(), LevelSubcycleShift(level), SubcycleGap());
}

bool SubstepEnds(int32_t level) {
    return MrSubstepEnds(CurrentSubstep(), LevelSubcycleShift(level), SubcycleGap());
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

// --- 自分のブロックのセルの熱(T-0111)---

uint4 PackThermal(MrThermal thermal) {
    return uint4(thermal.temperature, thermal.conductance, (uint32_t)thermal.capacityLimit,
                 (uint32_t)(thermal.capacityLimit >> 32));
}

MrThermal UnpackThermal(uint4 packed) {
    MrThermal thermal;
    thermal.temperature = packed.x;
    thermal.conductance = packed.y;
    thermal.capacityLimit = FX_U64(packed.w, packed.z);

    return thermal;
}

// セル index の面 face がブロックの外を向くか
bool FaceLeavesBlock(uint32_t index, uint32_t face) {
    const uint32_t axis = face >> 1;
    const uint32_t along = axis == 0 ? MrCellX(index) : (axis == 1 ? MrCellY(index) : MrCellZ(index));

    return (face & 1u) != 0 ? along == MR_BLOCK_EDGE - 1 : along == 0;
}

// ブロックの外を向く面(セル index・面 face)の外の隣の番号
uint32_t HaloEntry(uint32_t index, uint32_t face) {
    const uint32_t axis = face >> 1;
    const uint32_t u = axis == 0 ? MrCellY(index) : MrCellX(index);
    const uint32_t v = axis == 2 ? MrCellY(index) : MrCellZ(index);

    return face * CONDUCT_FACE_CELLS + v * MR_BLOCK_EDGE + u;
}

// 外の隣の番号 entry の、面に接するセル(HaloEntry の逆)
uint32_t HaloCell(uint32_t entry) {
    const uint32_t face = entry / CONDUCT_FACE_CELLS;
    const uint32_t u = entry % MR_BLOCK_EDGE;
    const uint32_t v = (entry % CONDUCT_FACE_CELLS) / MR_BLOCK_EDGE;
    const uint32_t along = (face & 1u) != 0 ? MR_BLOCK_EDGE - 1 : 0;
    const uint32_t axis = face >> 1;
    if (axis == 0)
        return MrCellIndex(along, u, v);

    if (axis == 1)
        return MrCellIndex(u, along, v);

    return MrCellIndex(u, v, along);
}

// 刻むセルの熱を gs_thermals に、刻むセルの外を向く面の隣とその熱を gs_halo* に書く(グループの全部のスレッドが呼び、
// 後で GroupMemoryBarrierWithGroupSync)。外の隣の熱は流れのある隣(同じレベル・粗い)だけ
void CacheBlockFaces(uint32_t slot, MrBlock block, uint32_t thread) {
    for (uint32_t k = 0; k < CONDUCT_CELLS_PER_THREAD; ++k) {
        const uint32_t index = thread + (CONDUCT_THREADS * k);
        if (MrIsSteppedCell(block, index))
            gs_thermals[index] = PackThermal(MrCellThermal(MakeTable(), LoadBlockCell(block, slot, index)));
    }

    for (uint32_t entry = thread; entry < CONDUCT_HALO_CELLS; entry += CONDUCT_THREADS) {
        const uint32_t index = HaloCell(entry);
        if (!MrIsSteppedCell(block, index))
            continue;

        const uint32_t face = entry / CONDUCT_FACE_CELLS;
        const MrFaceNeighbor neighbor = MrFindFaceNeighbor(MakeTree(), slot, block, index, face, g_rootLevel);
        gs_haloNeighbors[entry] = uint4(neighbor.kind, neighbor.slot, neighbor.index, neighbor.gap);
        if (neighbor.kind == MR_NEIGHBOR_SAME || neighbor.kind == MR_NEIGHBOR_COARSER)
            gs_haloThermals[entry] = PackThermal(ThermalAt(neighbor.slot, neighbor.index));
    }
}

MrThermal CachedThermal(uint32_t index) {
    return UnpackThermal(gs_thermals[index]);
}

// 刻むセル index の面 face の先(CacheBlockFaces の後。外を向く面は gs_haloNeighbors、中は MrFindFaceNeighbor の中の枝)
MrFaceNeighbor ConductFaceNeighbor(uint32_t slot, MrBlock block, uint32_t index, uint32_t face) {
    if (!FaceLeavesBlock(index, face))
        return MrFindFaceNeighbor(MakeTree(), slot, block, index, face, g_rootLevel);

    const uint4 packed = gs_haloNeighbors[HaloEntry(index, face)];

    return MrMakeFaceNeighbor(packed.x, packed.y, packed.z, packed.w);
}

// 面の先のセルの熱(流れのある隣だけ。同じブロックなら gs_thermals〔同じブロックの隣は刻むセル。覆われたセルは子の側を指す〕、
// 外なら gs_haloThermals)
MrThermal NeighborThermal(uint32_t index, uint32_t face, MrFaceNeighbor neighbor) {
    if (FaceLeavesBlock(index, face))
        return UnpackThermal(gs_haloThermals[HaloEntry(index, face)]);

    return CachedThermal(neighbor.index);
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
        SetSubstepMark(coarseSlot, CONDUCT_MARK_FRACTION);

    if (IsListedStep())
        ConductAppend(coarseSlot);
}

// セル 1 つの面を調べて印を付ける。流れがあれば true(CPU の CollectCellFaces + MarkCrossSends)
bool MarkCell(uint32_t slot, MrBlock block, uint32_t index) {
    const MrThermal self = CachedThermal(index);
    const uint32_t shift = LevelSubcycleShift(block.level);
    int64_t sameLevelOutflow = 0;
    bool sends = false;
    for (uint32_t face = 0; face < MR_FACES; ++face) {
        const MrFaceNeighbor neighbor = ConductFaceNeighbor(slot, block, index, face);
        if (neighbor.kind == MR_NEIGHBOR_SAME) {
            sameLevelOutflow += MrSubstepSameLevelFlow(self, NeighborThermal(index, face, neighbor), block.level,
                                                       shift);
            continue;
        }

        if (neighbor.kind != MR_NEIGHBOR_COARSER)
            continue;

        const uint32_t coarseShift = LevelSubcycleShift(g_blocks[neighbor.slot].level);
        const int64_t flow = MrSubstepCrossLevelFlow(self, NeighborThermal(index, face, neighbor), block.level,
                                                     neighbor.gap, shift, coarseShift);
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

// 一様なブロックの刻むセルを待ちの丸めで評価する(CPU の StepBlocks の初めの MrUniformWaitOf。1 スレッド = 1 セル)。
// 変わるなら gs_conductReacts に OR し、スレッドの受け持つセルの起こす刻みの最小を返す
uint64_t EvaluateUniformCells(uint32_t slot, MrBlock block, uint32_t thread) {
    const RxCell value = g_cells[slot];
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    uint64_t wakeTick = RX_WAIT_NEVER;
    bool changes = false;
    for (uint32_t k = 0; k < CONDUCT_CELLS_PER_THREAD; ++k) {
        const uint32_t index = thread + (CONDUCT_THREADS * k);
        if (!MrIsSteppedCell(block, index))
            continue;

        const RxWaitStep step = MrStepCellWait(MakeTable(), value, seed, tick, block, index);
        wakeTick = MinTick(wakeTick, step.wakeTick);
        changes = changes || !MrSameCell(step.cell, value);
    }

    OrGroupReacts(changes);

    return wakeTick;
}

// 一様なブロックの反応を評価するか: 最初の小刻みで、起こす刻みが来た(つつかれた = busyTick がこの刻みの印、も)
// (CPU は小刻みの前に 1 回。起こす段 WakeDue がつつかれたブロックの wakeTick を印 + 1 にしてあるので busyTick も見る)
bool EvaluatesUniform(MrBlock block) {
    const uint64_t mark = CurrentChangeMark();

    return MrIsUniform(block) && CurrentSubstep() == 0 && (block.wakeTick <= mark || block.busyTick == mark);
}

// 刻むブロック 1 つ: 一様で反応が進むか流れがあれば頁に広げる印(CPU の StepBlocks の最初と MarkBlockWants)。
// この小刻みに小刻みが始まるレベルのブロックだけ。反応を評価するのは最初の小刻みだけ(T-0109)。評価して変わらない一様なブロックは
// 起こす刻みを書く(CPU の RecordWaitResults。後で頁に広げたら ConductApplyBlock が書き直す)
void ConductMarkBlock(uint32_t slot, uint32_t thread) {
    const MrBlock block = g_blocks[slot];
    if (!IsConductStepped(slot, block) || !SubstepBegins(block.level))
        return;

    if (thread == 0) {
        gs_conductAny = 0;
        gs_conductReacts = 0;
    }

    CacheBlockFaces(slot, block, thread);
    GroupMemoryBarrierWithGroupSync();
    bool flows = false;
    for (uint32_t k = 0; k < CONDUCT_CELLS_PER_THREAD; ++k) {
        const uint32_t index = thread + (CONDUCT_THREADS * k);
        if (OnBlockSurface(index) && MrIsSteppedCell(block, index) && MarkCell(slot, block, index))
            flows = true;
    }

    OrGroupAny(flows);

    // --- 一様なブロックの反応(グループで一様な分岐)---
    const bool evaluates = EvaluatesUniform(block);
    uint64_t wakeTick = RX_WAIT_NEVER;
    if (evaluates)
        wakeTick = GroupMinTick(thread, EvaluateUniformCells(slot, block, thread));
    else
        GroupMemoryBarrierWithGroupSync();

    if (thread != 0 || !MrIsUniform(block))
        return;

    const bool reacts = evaluates && gs_conductReacts != 0;
    if (evaluates && !reacts)
        FinishWaitBlock(slot, 0, wakeTick);

    if (gs_conductAny != 0 || reacts)
        g_blocks[slot].page = MR_PAGE_WANTED;
}

// --- 4. 配った頁と端数の枠 ----------------------------------------------------------------------

void ConductPrepareBlock(uint32_t slot, uint32_t thread) {
    if (slot >= g_worldBlocks)
        return;

    const MrBlock block = g_blocks[slot];
    const bool expanded = HasSubstepMark(slot, CONDUCT_MARK_EXPANDED);
    const bool granted = HasSubstepMark(slot, CONDUCT_MARK_GRANTED);
    const RxCell value = g_cells[slot];
    for (uint32_t k = 0; k < CONDUCT_LIGHT_CELLS_PER_THREAD; ++k) {
        const uint32_t index = thread + (CONDUCT_LIGHT_THREADS * k);
        if (expanded)
            g_cells[PageCellAddress(block.page, index)] = MrUniformCell(block, value, index);

        if (granted)
            g_fractions[FractionAddress(block.fraction, index)] = MrMakeEmptyFraction();
    }
}

// --- 5. 面の流れ ---------------------------------------------------------------------------------

// セル 1 つの面の流れを変化に足す(自分と、粗い側へ送った先。CPU の AddCellFlows)。凍らせたブロックとの面は流れない
void AddCellFlows(uint32_t slot, MrBlock block, uint32_t index) {
    const MrThermal self = CachedThermal(index);
    const uint32_t shift = LevelSubcycleShift(block.level);
    int64_t own = 0;
    for (uint32_t face = 0; face < MR_FACES; ++face) {
        const MrFaceNeighbor neighbor = ConductFaceNeighbor(slot, block, index, face);
        if (neighbor.kind != MR_NEIGHBOR_SAME && neighbor.kind != MR_NEIGHBOR_COARSER)
            continue;

        if (IsFrozenSlot(neighbor.slot))
            continue;

        const MrThermal other = NeighborThermal(index, face, neighbor);
        if (neighbor.kind == MR_NEIGHBOR_SAME) {
            own -= MrSubstepSameLevelFlow(self, other, block.level, shift);
            continue;
        }

        const MrBlock coarse = g_blocks[neighbor.slot];
        const int64_t flow = MrSubstepCrossLevelFlow(self, other, block.level, neighbor.gap, shift,
                                                     LevelSubcycleShift(coarse.level));
        if (flow == 0)
            continue;

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
    if (!IsConductStepped(slot, block) || !SubstepBegins(block.level) || MrIsUniform(block) || IsFrozenSlot(slot))
        return;

    CacheBlockFaces(slot, block, thread);
    GroupMemoryBarrierWithGroupSync();
    for (uint32_t k = 0; k < CONDUCT_CELLS_PER_THREAD; ++k) {
        const uint32_t index = thread + (CONDUCT_THREADS * k);
        if (MrIsSteppedCell(block, index))
            AddCellFlows(slot, block, index);
    }
}

// --- 6. 変化を足して反応 -------------------------------------------------------------------------

// セル 1 つに溜めた伝導の変化を足して 0 に戻す(CPU の ApplyEnergyDelta。端数は g_fractions にも書く)。足したら true
bool ApplyCellDelta(MrBlock block, uint32_t index, inout RxCell cell, inout MrFraction fraction) {
    const uint32_t deltaAddress = ConductDeltaAddress(block.page, index);
    MrEnergyDelta delta;
    delta.whole = (int64_t)g_conduction.Load<uint64_t>(deltaAddress);
    delta.fraction = g_conduction.Load<uint64_t>(deltaAddress + 8);
    if (MrEnergyDeltaIsZero(delta))
        return false;

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

    return true;
}

// セル 1 つ: 変化を足し(読んだら 0 に戻す)、react なら待ちの丸めで反応を進め、起こす刻みを wakeTick の最小に入れる
// (上限に当たった印は tally に足す。T-0163)。変わったら true
bool ApplyCell(MrBlock block, uint32_t index, bool react, inout uint64_t wakeTick, inout MrLimitTally tally) {
    const uint32_t address = PageCellAddress(block.page, index);
    RxCell cell = g_cells[address];
    const RxCell before = cell;
    MrFraction fraction = LoadFraction(block.fraction, index);
    const uint64_t fractionBefore = fraction.energy;

    // --- 伝導の変化(最後の小刻みの分)---
    ApplyCellDelta(block, index, cell, fraction);

    // --- 反応(tc は刻みの初めの見出しの busyTick。CPU の StepPagedBlock)---
    if (react) {
        const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
        const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
        const RxWaitStep step = MrStepCellWait(MakeTable(), cell, seed, tick, block, index);
        wakeTick = MinTick(wakeTick, step.wakeTick);
        cell = step.cell;
        tally = MrAddLimits(tally, step.limits);
    }

    g_cells[address] = cell;

    return MrCellChanged(before, cell) || fraction.energy != fractionBefore;
}

// 頁を持つブロック 1 つ(CPU の StepPagedBlock)と、見出しの busyTick・wakeTick(CPU の RecordWaitResults の 1 ブロック分。
// 観察の枠も)。変わった・どこかの小刻みで変わった・頁を配った(刻みの印だけ見る。T-0109)なら busyTick = この刻みの印・
// wakeTick = 次の刻み、刻んで変わらなければ wakeTick = 刻むセルの最小
void ConductApplyBlock(uint32_t slot, uint32_t thread) {
    const MrBlock block = g_blocks[slot];
    if (MrIsUniform(block))
        return;

    if (thread == 0)
        gs_conductAny = 0;

    GroupMemoryBarrierWithGroupSync();
    const bool react = IsConductStepped(slot, block);
    bool changed = false;
    uint64_t wakeTick = RX_WAIT_NEVER;
    MrLimitTally tally = MrMakeLimitTally();
    for (uint32_t k = 0; k < CONDUCT_CELLS_PER_THREAD; ++k) {
        const uint32_t index = thread + (CONDUCT_THREADS * k);
        if (MrIsSteppedCell(block, index))
            changed = ApplyCell(block, index, react, wakeTick, tally) || changed;
    }

    CountLimits(tally);
    OrGroupAny(changed);
    const uint64_t blockWakeTick = GroupMinTick(thread, wakeTick);
    if (thread != 0)
        return;

    const bool blockChanged = gs_conductAny != 0 || HasConductMark(slot, CONDUCT_MARK_CHANGED) ||
                              HasConductMark(slot, CONDUCT_MARK_EXPANDED);
    if (react || blockChanged || block.busyTick == CurrentChangeMark())
        FinishWaitBlock(slot, blockChanged ? 1u : 0u, blockWakeTick);
}

// --- 6a. 小刻みの終わり(最後の小刻みの手前まで。T-0109。CPU の ApplyEndingDeltas と WakeChangedBlocks)--------------------

// 小刻みが終わるレベルの頁のブロックに、溜めた変化を足して 0 に戻す(反応は進めない)。足したら CHANGED の印。活性なら、変わった世界の
// 本物のブロックを起こす一覧へ(u13 に結んだこの刻みの種の一覧。次に活性のグラフの ActivitySeedNode が面の隣を起こし、
// 初めて印を付けたブロックを伝導の一覧に足す。gpu_multires.cpp の RecordSubstepEnd)
void ConductEndBlock(uint32_t slot, uint32_t thread) {
    const MrBlock block = g_blocks[slot];
    if (MrIsUniform(block) || !SubstepEnds(block.level))
        return;

    if (thread == 0)
        gs_conductAny = 0;

    GroupMemoryBarrierWithGroupSync();
    bool changed = false;
    for (uint32_t k = 0; k < CONDUCT_LIGHT_CELLS_PER_THREAD; ++k) {
        const uint32_t index = thread + (CONDUCT_LIGHT_THREADS * k);
        if (!MrIsSteppedCell(block, index))
            continue;

        const uint32_t address = PageCellAddress(block.page, index);
        RxCell cell = g_cells[address];
        MrFraction fraction = LoadFraction(block.fraction, index);
        if (!ApplyCellDelta(block, index, cell, fraction))
            continue;

        g_cells[address] = cell;
        changed = true;
    }

    OrGroupAny(changed);

    GroupMemoryBarrierWithGroupSync();
    if (thread != 0 || gs_conductAny == 0)
        return;

    SetConductMark(slot, CONDUCT_MARK_CHANGED);
    if (IsListedStep() && slot < g_worldBlocks && block.kind == MR_BLOCK_REAL)
        AppendActivity(slot);
}

#endif  // BICAMERAL_MULTIRES_CONDUCT_HLSLI
