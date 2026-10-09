// implicit_build.hlsl — 細かいレベルの熱の陰解法(方式②。ADR-0019)の系のうち、未知数・境のセル・面・セルの面の一覧を GPU で作る(T-0129)。
// CPU リファレンスは engine/src/sim/multires_implicit_conduction.cpp の MakeSystem と gpu_implicit.cpp の MakeCellFaces。
// 番号の付け方も CPU と同じにする(面の順は安全網の頭打ちの足し算で効くので、番号まで揃える):
//   - 未知数 = 基準より細かい(Δk 1〜implicitMaxGap)本物のブロックの刻むセル(熱容量 0 は除く)。番号は (枠, セル) の昇順(枠ごとの数の接頭和)
//   - 面 = 未知数 u の 6 面を u・面の順に(同じレベルの面は番号の小さい側だけ)。番号は「面の候補 u × 6 + 面」ごとの有無の接頭和
//   - 境のセル = 未知数でない面の先。番号は CPU が初めて出会う順 = その先を指す最初の面の候補(atomic の最小)の接頭和 + 未知数の数
//   - セルの面の一覧 = 面の番号 × 2(+ 粗い側なら 1)を面の番号の昇順に(atomic で置いてから、セルごとに並べ直す)
// 多重格子の段と重みは implicit_levels.hlsl(T-0134)。セルの座標を作業場に残す(CellKeyWord)。
// ルート署名は多重解像度のもの(multires_bindings.hlsli。sim/gpu_multires.cpp)。外のバッファ u4 = 作業場、u5 = 系(gpu_implicit_build.cpp の並び)。
// 定数: g_external0 = 基準のレベル(符号付き 16bit)| implicitMaxGap << 16 | 凍った印を見る << 24、g_external1 = 未知数の上限、g_external2 = セルの上限。
// 段の順(どの段も前の段の書き込みを読む。段の間は UAV のバリア): Clear → CountBlocks → ScanBlocks → NumberUnknowns → FaceEntries
//   → ScanEntries(Local → Groups → Add)→ Boundary → Cells → Faces → ScanCells(Local → Groups → Add)→ FillLists → SortLists
// 伝導の段から呼ぶ時(T-0132)は、GpuImplicit が解いた後に Apply が解いた変化を伝導の変化の表(u11)へ足す。
#include "common/implicit_conduction.hlsli"
#include "common/multires_conduction.hlsli"
#include "sim/multires_bindings.hlsli"

RWByteAddressBuffer g_build : register(u4);  // 作業場(語の並びは下の番地の関数)
RWByteAddressBuffer g_system
    : register(u5);  // 系 [ImGpuCell × セルの上限][ImGpuFace × 面の上限][面の一覧 × 2 × 面の上限]

static const uint32_t BUILD_THREADS = 64;
static const uint32_t SCAN_THREADS = 1024;
static const uint32_t NONE = 0xFFFFFFFFu;

// 見出しの語(gpu_implicit_build.h の ImplicitBuildHeader と同じ並び)
static const uint32_t HEADER_UNKNOWNS = 0;
static const uint32_t HEADER_BOUNDARY = 1;
static const uint32_t HEADER_FACES = 2;
static const uint32_t HEADER_OVERFLOW = 3;
static const uint32_t HEADER_CELLS = 4;
static const uint32_t HEADER_WORDS = 16;

// 面の候補の印(EntryInfo): 下位 8bit = レベルの差、ビット 8 = 先が境のセル
static const uint32_t ENTRY_BOUNDARY = 256u;

static const uint32_t CELL_BYTES = 40;  // sizeof(ImGpuCell)
static const uint32_t FACE_BYTES = 32;  // sizeof(ImGpuFace)

// --- 定数と番地 --------------------------------------------------------------------------------

int32_t BaseLevel() {
    return ((int32_t)(g_external0 << 16)) >> 16;
}

uint32_t MaxGap() {
    return (g_external0 >> 16) & 0xFFu;
}

bool UseFrozenMarks() {
    return ((g_external0 >> 24) & 1u) != 0;
}

uint32_t MaxUnknowns() {
    return g_external1;
}

uint32_t MaxCells() {
    return g_external2;
}

uint32_t MapWords() {
    return g_worldBlocks * MR_BLOCK_CELLS;
}

// 語の番号(作業場)
uint32_t IdsWord(uint32_t address) {
    return HEADER_WORDS + address;
}

uint32_t KeysWord(uint32_t address) {
    return HEADER_WORDS + MapWords() + address;
}

uint32_t BlockOffsetWord(uint32_t slot) {
    return HEADER_WORDS + 2 * MapWords() + slot;
}

uint32_t EntryBase() {
    return BlockOffsetWord(g_worldBlocks + 1);
}

uint32_t EntryNeighborWord(uint32_t entry) {
    return EntryBase() + entry;
}

uint32_t EntryInfoWord(uint32_t entry) {
    return EntryBase() + 6 * MaxUnknowns() + entry;
}

uint32_t EntryFaceWord(uint32_t entry) {
    return EntryBase() + 12 * MaxUnknowns() + entry;
}

uint32_t EntryFirstWord(uint32_t entry) {
    return EntryBase() + 18 * MaxUnknowns() + entry;
}

uint32_t CellAddressWord(uint32_t cell) {
    return EntryBase() + 24 * MaxUnknowns() + cell;
}

uint32_t CellCountWord(uint32_t cell) {
    return CellAddressWord(MaxCells()) + cell;
}

uint32_t CellStartWord(uint32_t cell) {
    return CellCountWord(MaxCells()) + cell;
}

uint32_t EntryGroups() {
    return (6 * MaxUnknowns() + SCAN_THREADS - 1) / SCAN_THREADS;
}

uint32_t EntryGroupWord(uint32_t group, uint32_t kind) {
    return CellStartWord(MaxCells() + 1) + 2 * group + kind;
}

uint32_t CellGroupWord(uint32_t group) {
    return EntryGroupWord(EntryGroups() + 1, 0) + group;
}

uint32_t RawListWord(uint32_t position) {
    return CellGroupWord((MaxCells() + SCAN_THREADS - 1) / SCAN_THREADS + 1) + position;
}

uint32_t ListCellWord(uint32_t position) {
    return RawListWord(12 * MaxUnknowns()) + position;
}

// セルの座標(8 語 / セル: レベル・x・y・z〔int64 は下位 → 上位〕・空き)。多重格子の段を作る段が読む(implicit_levels.hlsl。T-0134)
uint32_t CellKeyWord(uint32_t cell) {
    return ListCellWord(12 * MaxUnknowns()) + 8 * cell;
}

uint32_t LoadWord(uint32_t word) {
    return g_build.Load(word * 4);
}

void StoreWord(uint32_t word, uint32_t value) {
    g_build.Store(word * 4, value);
}

// 系の番地(バイト)
uint32_t CellByte(uint32_t cell) {
    return cell * CELL_BYTES;
}

uint32_t FaceByte(uint32_t face) {
    return MaxCells() * CELL_BYTES + face * FACE_BYTES;
}

uint32_t ListByte(uint32_t position) {
    return MaxCells() * CELL_BYTES + 6 * MaxUnknowns() * FACE_BYTES + position * 4;
}

// --- 木の判定(CPU の CanJoin・InImplicitConduction)-----------------------------------------------

bool IsFrozen(uint32_t slot) {
    return UseFrozenMarks() && HasSubstepMark(slot, CONDUCT_MARK_FROZEN);
}

// 系に入れられるブロックか: 世界の本物のブロックで、頁があり、凍っていない
bool CanJoin(uint32_t slot) {
    if (slot >= g_worldBlocks)
        return false;

    const MrBlock block = g_blocks[slot];

    return block.kind == MR_BLOCK_REAL && !MrIsUniform(block) && !IsFrozen(slot);
}

// 未知数のブロックか(基準より細かく、基準 + implicitMaxGap 以下)
bool InImplicit(MrBlock block) {
    const int32_t finest = BaseLevel() + (int32_t)MaxGap();

    return block.kind == MR_BLOCK_REAL && block.level > BaseLevel() && block.level <= finest;
}

MrThermal ThermalAt(uint32_t address) {
    return MrCellThermal(MakeTable(), LoadCell(address / MR_BLOCK_CELLS, address % MR_BLOCK_CELLS));
}

bool IsUnknownCell(uint32_t slot, MrBlock block, uint32_t index) {
    return MrIsSteppedCell(block, index) && ThermalAt(slot * MR_BLOCK_CELLS + index).capacityLimit != 0;
}

bool IsUnknownBlock(uint32_t slot, MrBlock block) {
    return CanJoin(slot) && InImplicit(block);
}

void MarkOverflow() {
    g_build.Store(HEADER_OVERFLOW * 4, 1u);
}

// --- 1 グループの排他的な接頭和(SCAN_THREADS スレッド。count 個を連続の塊に分ける)------------------

groupshared uint32_t gs_sums[SCAN_THREADS];
groupshared uint32_t gs_sumsSecond[SCAN_THREADS];

// 塊の和の排他的な接頭和(2 本同時。Hillis–Steele)
void ScanGroup(uint32_t thread) {
    for (uint32_t step = 1; step < SCAN_THREADS; step <<= 1) {
        GroupMemoryBarrierWithGroupSync();
        const uint32_t first = thread >= step ? gs_sums[thread - step] : 0;
        const uint32_t second = thread >= step ? gs_sumsSecond[thread - step] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_sums[thread] += first;
        gs_sumsSecond[thread] += second;
    }

    GroupMemoryBarrierWithGroupSync();
}

uint32_t ChunkSize(uint32_t count) {
    return (count + SCAN_THREADS - 1) / SCAN_THREADS;
}

// --- Clear: 見出しを 0、番号の表と最初の面の候補を「無し」に(1 スレッド = 1 語)---
[numthreads(BUILD_THREADS, 1, 1)] void BuildClear(uint3 id : SV_DispatchThreadID) {
    const uint32_t word = id.x;
    if (word < HEADER_WORDS)
        StoreWord(word, 0);
    else if (word < HEADER_WORDS + 2 * MapWords())
        StoreWord(word, NONE);
}

// --- CountBlocks: 世界の枠ごとの未知数の数(1 グループ = 1 枠。64 スレッドで 8 セルずつ)---
groupshared uint32_t gs_blockCount;
groupshared uint32_t gs_threadCounts[BUILD_THREADS];

[numthreads(BUILD_THREADS, 1, 1)] void BuildCountBlocks(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    const uint32_t slot = group.x;
    const MrBlock block = g_blocks[slot];
    const bool joins = IsUnknownBlock(slot, block);
    uint32_t count = 0;
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / BUILD_THREADS && joins; ++k) {
        if (IsUnknownCell(slot, block, thread.x * (MR_BLOCK_CELLS / BUILD_THREADS) + k))
            ++count;
    }

    if (thread.x == 0)
        gs_blockCount = 0;

    GroupMemoryBarrierWithGroupSync();
    InterlockedAdd(gs_blockCount, count);
    GroupMemoryBarrierWithGroupSync();
    if (thread.x == 0)
        StoreWord(BlockOffsetWord(slot), gs_blockCount);
}

    // --- ScanBlocks: 枠ごとの数の接頭和 → 未知数の始まり(1 グループ)---
    [numthreads(SCAN_THREADS, 1, 1)] void BuildScanBlocks(uint3 thread : SV_GroupThreadID) {
    const uint32_t count = g_worldBlocks;
    const uint32_t chunk = ChunkSize(count);
    const uint32_t begin = min(thread.x * chunk, count);
    const uint32_t end = min(begin + chunk, count);
    uint32_t sum = 0;
    for (uint32_t i = begin; i < end; ++i)
        sum += LoadWord(BlockOffsetWord(i));

    gs_sums[thread.x] = sum;
    gs_sumsSecond[thread.x] = 0;
    ScanGroup(thread.x);

    uint32_t running = gs_sums[thread.x] - sum;
    for (uint32_t j = begin; j < end; ++j) {
        const uint32_t value = LoadWord(BlockOffsetWord(j));
        StoreWord(BlockOffsetWord(j), running);
        running += value;
    }

    if (thread.x == SCAN_THREADS - 1) {
        StoreWord(BlockOffsetWord(count), gs_sums[thread.x]);
        StoreWord(HEADER_UNKNOWNS, min(gs_sums[thread.x], MaxUnknowns()));
        if (gs_sums[thread.x] > MaxUnknowns())
            MarkOverflow();
    }
}

// --- NumberUnknowns: 未知数に番号(枠の始まり + 枠の中のセルの順)。番号の表とセルの番地へ(1 グループ = 1 枠)---
[numthreads(BUILD_THREADS, 1, 1)] void BuildNumberUnknowns(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    const uint32_t slot = group.x;
    const MrBlock block = g_blocks[slot];
    const bool joins = IsUnknownBlock(slot, block);
    const uint32_t perThread = MR_BLOCK_CELLS / BUILD_THREADS;
    uint32_t flags = 0;
    for (uint32_t k = 0; k < perThread && joins; ++k) {
        if (IsUnknownCell(slot, block, thread.x * perThread + k))
            flags |= 1u << k;
    }

    gs_threadCounts[thread.x] = countbits(flags);
    GroupMemoryBarrierWithGroupSync();
    uint32_t id = LoadWord(BlockOffsetWord(slot));
    for (uint32_t t = 0; t < thread.x; ++t)
        id += gs_threadCounts[t];

    for (uint32_t c = 0; c < perThread; ++c) {
        if ((flags & (1u << c)) == 0)
            continue;

        if (id < MaxUnknowns()) {
            const uint32_t address = slot * MR_BLOCK_CELLS + thread.x * perThread + c;
            StoreWord(IdsWord(address), id);
            StoreWord(CellAddressWord(id), address);
        }

        ++id;
    }
}

    // --- FaceEntries: 未知数 u の 6 面の候補(1 スレッド = 1 未知数)。先が境のセルなら最初の候補を atomic の最小で ---
    [numthreads(BUILD_THREADS, 1, 1)] void BuildFaceEntries(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t unknown = dispatch.x;
    if (unknown >= LoadWord(HEADER_UNKNOWNS))
        return;

    const uint32_t address = LoadWord(CellAddressWord(unknown));
    const uint32_t slot = address / MR_BLOCK_CELLS;
    const uint32_t index = address % MR_BLOCK_CELLS;
    const MrBlock block = g_blocks[slot];
    for (uint32_t face = 0; face < MR_FACES; ++face) {
        const uint32_t entry = unknown * MR_FACES + face;
        const MrFaceNeighbor neighbor = MrFindFaceNeighbor(MakeTree(), slot, block, index, face, g_rootLevel);
        uint32_t target = NONE;
        uint32_t info = 0;
        const bool same = neighbor.kind == MR_NEIGHBOR_SAME;
        if ((same || neighbor.kind == MR_NEIGHBOR_COARSER) && CanJoin(neighbor.slot)) {
            const uint32_t other = neighbor.slot * MR_BLOCK_CELLS + neighbor.index;
            const uint32_t otherId = LoadWord(IdsWord(other));
            info = same ? 0 : neighbor.gap;
            if (otherId != NONE) {
                if (!(same && otherId < unknown))
                    target = other;
            } else if (ThermalAt(other).capacityLimit != 0) {
                target = other;
                info |= ENTRY_BOUNDARY;
                g_build.InterlockedMin(KeysWord(other) * 4, entry);
            }
        }

        StoreWord(EntryNeighborWord(entry), target);
        StoreWord(EntryInfoWord(entry), info);
    }
}

// 面の候補の有無と、境のセルに初めて出会う候補か
uint32_t EntryExists(uint32_t entry) {
    return LoadWord(EntryNeighborWord(entry)) != NONE ? 1u : 0u;
}

uint32_t EntryFirst(uint32_t entry) {
    const uint32_t target = LoadWord(EntryNeighborWord(entry));
    if (target == NONE || (LoadWord(EntryInfoWord(entry)) & ENTRY_BOUNDARY) == 0)
        return 0;

    return LoadWord(KeysWord(target)) == entry ? 1u : 0u;
}

// --- 面の候補の接頭和(3 段: グループの中 → グループの和 → 足す。T-0129 の計測で 1 グループの塊の接頭和は 12 万候補で 0.48 ms)---

uint32_t EntryCount() {
    return LoadWord(HEADER_UNKNOWNS) * MR_FACES;
}

// ScanEntriesLocal: グループ(SCAN_THREADS 候補)の中の排他的な接頭和を面の番号・境のセルの番号の欄へ、グループの和を EntryGroupWord へ
[numthreads(SCAN_THREADS, 1, 1)] void BuildScanEntriesLocal(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    const uint32_t entry = group.x * SCAN_THREADS + thread.x;
    const bool inside = entry < EntryCount();
    const uint32_t face = inside ? EntryExists(entry) : 0;
    const uint32_t first = inside ? EntryFirst(entry) : 0;
    gs_sums[thread.x] = face;
    gs_sumsSecond[thread.x] = first;
    ScanGroup(thread.x);

    if (inside) {
        StoreWord(EntryFaceWord(entry), gs_sums[thread.x] - face);
        StoreWord(EntryFirstWord(entry), gs_sumsSecond[thread.x] - first);
    }

    if (thread.x == SCAN_THREADS - 1) {
        StoreWord(EntryGroupWord(group.x, 0), gs_sums[thread.x]);
        StoreWord(EntryGroupWord(group.x, 1), gs_sumsSecond[thread.x]);
    }
}

    // ScanEntriesGroups: グループの和の排他的な接頭和(1 グループ)と、面・境のセル・セルの数を見出しへ
    [numthreads(SCAN_THREADS, 1, 1)] void BuildScanEntriesGroups(uint3 thread : SV_GroupThreadID) {
    const uint32_t unknowns = LoadWord(HEADER_UNKNOWNS);
    const uint32_t count = (unknowns * MR_FACES + SCAN_THREADS - 1) / SCAN_THREADS;
    const uint32_t chunk = ChunkSize(count);
    const uint32_t begin = min(thread.x * chunk, count);
    const uint32_t end = min(begin + chunk, count);
    uint32_t faces = 0;
    uint32_t firsts = 0;
    for (uint32_t i = begin; i < end; ++i) {
        faces += LoadWord(EntryGroupWord(i, 0));
        firsts += LoadWord(EntryGroupWord(i, 1));
    }

    gs_sums[thread.x] = faces;
    gs_sumsSecond[thread.x] = firsts;
    ScanGroup(thread.x);

    uint32_t face = gs_sums[thread.x] - faces;
    uint32_t first = gs_sumsSecond[thread.x] - firsts;
    for (uint32_t j = begin; j < end; ++j) {
        const uint32_t groupFaces = LoadWord(EntryGroupWord(j, 0));
        const uint32_t groupFirsts = LoadWord(EntryGroupWord(j, 1));
        StoreWord(EntryGroupWord(j, 0), face);
        StoreWord(EntryGroupWord(j, 1), first);
        face += groupFaces;
        first += groupFirsts;
    }

    if (thread.x != SCAN_THREADS - 1)
        return;

    const uint32_t boundary = gs_sumsSecond[thread.x];
    StoreWord(HEADER_FACES, gs_sums[thread.x]);
    StoreWord(HEADER_BOUNDARY, boundary);
    StoreWord(HEADER_CELLS, min(unknowns + boundary, MaxCells()));
    if (unknowns + boundary > MaxCells())
        MarkOverflow();
}

// ScanEntriesAdd: グループの始まりを足す(1 スレッド = 1 候補)
[numthreads(BUILD_THREADS, 1, 1)] void BuildScanEntriesAdd(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t entry = dispatch.x;
    if (entry >= EntryCount())
        return;

    const uint32_t group = entry / SCAN_THREADS;
    StoreWord(EntryFaceWord(entry), LoadWord(EntryFaceWord(entry)) + LoadWord(EntryGroupWord(group, 0)));
    StoreWord(EntryFirstWord(entry), LoadWord(EntryFirstWord(entry)) + LoadWord(EntryGroupWord(group, 1)));
}

    // --- Boundary: 境のセルに番号(未知数の数 + 初めて出会う候補の順。1 スレッド = 1 候補)---
    [numthreads(BUILD_THREADS, 1, 1)] void BuildBoundary(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t entry = dispatch.x;
    if (entry >= EntryCount() || EntryFirst(entry) == 0)
        return;

    const uint32_t id = LoadWord(HEADER_UNKNOWNS) + LoadWord(EntryFirstWord(entry));
    if (id >= MaxCells())
        return;

    const uint32_t target = LoadWord(EntryNeighborWord(entry));
    StoreWord(IdsWord(target), id);
    StoreWord(CellAddressWord(id), target);
}

// --- Cells: セル(刻みの初めの温度と熱容量。CPU の AddCell)。面の数を 0 に(1 スレッド = 1 セル)---
[numthreads(BUILD_THREADS, 1, 1)] void BuildCells(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t cellId = dispatch.x;
    if (cellId >= LoadWord(HEADER_CELLS))
        return;

    const MrThermal thermal = ThermalAt(LoadWord(CellAddressWord(cellId)));
    ImGpuCell cell;
    cell.heatCapacity = thermal.capacityLimit << IM_ENERGY_BITS_PER_LEVEL;
    cell.energy = ImEnergyFor(cell.heatCapacity, (int64_t)thermal.temperature);
    cell.fraction = 0;
    cell.startTemperature = ((int64_t)thermal.temperature) << IM_TEMPERATURE_SHIFT;
    cell.faceStart = 0;
    cell.faceEnd = 0;
    g_system.Store<ImGpuCell>(CellByte(cellId), cell);
    StoreWord(CellCountWord(cellId), 0);

    // --- 座標(CPU の AddCell と同じ: ブロックの原点 + セルの位置)---
    const uint32_t address = LoadWord(CellAddressWord(cellId));
    const MrBlock block = g_blocks[address / MR_BLOCK_CELLS];
    const uint32_t index = address % MR_BLOCK_CELLS;
    const int64_t x = block.originX + (int64_t)MrCellX(index);
    const int64_t y = block.originY + (int64_t)MrCellY(index);
    const int64_t z = block.originZ + (int64_t)MrCellZ(index);
    g_build.Store4(CellKeyWord(cellId) * 4,
                   uint4((uint32_t)block.level, (uint32_t)x, (uint32_t)((uint64_t)x >> 32), (uint32_t)y));
    g_build.Store4((CellKeyWord(cellId) + 4) * 4,
                   uint4((uint32_t)((uint64_t)y >> 32), (uint32_t)z, (uint32_t)((uint64_t)z >> 32), 0u));
}

    // --- Faces: 面(係数 = min(G) × G の係数 × 4^kf。CPU の FaceCoefficient)。セルごとの面の数を数える(1 スレッド = 1 候補)---
    [numthreads(BUILD_THREADS, 1, 1)] void BuildFaces(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t entry = dispatch.x;
    if (entry >= EntryCount() || EntryExists(entry) == 0)
        return;

    const uint32_t fine = entry / MR_FACES;
    const uint32_t target = LoadWord(EntryNeighborWord(entry));
    const uint32_t coarse = LoadWord(IdsWord(target));
    if (coarse >= MaxCells())
        return;

    const uint32_t fineAddress = LoadWord(CellAddressWord(fine));
    const MrThermal fineThermal = ThermalAt(fineAddress);
    const MrThermal coarseThermal = ThermalAt(target);
    const uint32_t conductance = min(fineThermal.conductance, coarseThermal.conductance);
    const MrBlock fineBlock = g_blocks[fineAddress / MR_BLOCK_CELLS];
    const MrBlock coarseBlock = g_blocks[target / MR_BLOCK_CELLS];
    const FxU128 coefficient = ImWideShiftLeft(ImWide((uint64_t)conductance * HC_LIMIT_PER_CONDUCTANCE),
                                               2 * (uint32_t)fineBlock.level);

    ImGpuFace face;
    face.coefficientHigh = coefficient.hi;
    face.coefficientLow = coefficient.lo;
    face.fine = fine;
    face.coarse = coarse;
    face.gap = LoadWord(EntryInfoWord(entry)) & 0xFFu;
    face.coarseFraction = coarseBlock.fraction != MR_NO_FRACTION ? 1u : 0u;
    g_system.Store<ImGpuFace>(FaceByte(LoadWord(EntryFaceWord(entry))), face);

    g_build.InterlockedAdd(CellCountWord(fine) * 4, 1u);
    g_build.InterlockedAdd(CellCountWord(coarse) * 4, 1u);
}

// --- セルの面の一覧の始まり(面の数の接頭和。3 段は面の候補と同じ形)。数は置く位置の数え直しのため 0 に ---

// ScanCellsLocal: グループの中の排他的な接頭和を CellStartWord へ、グループの和を CellGroupWord へ
[numthreads(SCAN_THREADS, 1, 1)] void BuildScanCellsLocal(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    const uint32_t cellId = group.x * SCAN_THREADS + thread.x;
    const bool inside = cellId < LoadWord(HEADER_CELLS);
    const uint32_t count = inside ? LoadWord(CellCountWord(cellId)) : 0;
    gs_sums[thread.x] = count;
    gs_sumsSecond[thread.x] = 0;
    ScanGroup(thread.x);

    if (inside)
        StoreWord(CellStartWord(cellId), gs_sums[thread.x] - count);

    if (thread.x == SCAN_THREADS - 1)
        StoreWord(CellGroupWord(group.x), gs_sums[thread.x]);
}

    // ScanCellsGroups: グループの和の排他的な接頭和(1 グループ)
    [numthreads(SCAN_THREADS, 1, 1)] void BuildScanCellsGroups(uint3 thread : SV_GroupThreadID) {
    const uint32_t count = (LoadWord(HEADER_CELLS) + SCAN_THREADS - 1) / SCAN_THREADS;
    const uint32_t chunk = ChunkSize(count);
    const uint32_t begin = min(thread.x * chunk, count);
    const uint32_t end = min(begin + chunk, count);
    uint32_t sum = 0;
    for (uint32_t i = begin; i < end; ++i)
        sum += LoadWord(CellGroupWord(i));

    gs_sums[thread.x] = sum;
    gs_sumsSecond[thread.x] = 0;
    ScanGroup(thread.x);

    uint32_t running = gs_sums[thread.x] - sum;
    for (uint32_t j = begin; j < end; ++j) {
        const uint32_t value = LoadWord(CellGroupWord(j));
        StoreWord(CellGroupWord(j), running);
        running += value;
    }
}

// ScanCellsAdd: グループの始まりを足し、セルの faceStart・faceEnd を書く(1 スレッド = 1 セル)
[numthreads(BUILD_THREADS, 1, 1)] void BuildScanCellsAdd(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t cellId = dispatch.x;
    if (cellId >= LoadWord(HEADER_CELLS))
        return;

    const uint32_t start = LoadWord(CellStartWord(cellId)) + LoadWord(CellGroupWord(cellId / SCAN_THREADS));
    const uint32_t count = LoadWord(CellCountWord(cellId));
    StoreWord(CellStartWord(cellId), start);
    StoreWord(CellCountWord(cellId), 0);
    g_system.Store2(CellByte(cellId) + 32, uint2(start, start + count));  // ImGpuCell の faceStart・faceEnd
}

// --- FillLists: 面を両側のセルの一覧に仮に置く(置く順は決まらない。作業場の仮の一覧へ。1 スレッド = 1 候補)---
void PlaceInList(uint32_t cellId, uint32_t value) {
    uint32_t position;
    g_build.InterlockedAdd(CellCountWord(cellId) * 4, 1u, position);
    const uint32_t at = LoadWord(CellStartWord(cellId)) + position;
    StoreWord(RawListWord(at), value);
    StoreWord(ListCellWord(at), cellId);
}

[numthreads(BUILD_THREADS, 1, 1)] void BuildFillLists(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t entry = dispatch.x;
    if (entry >= EntryCount() || EntryExists(entry) == 0)
        return;

    const uint32_t coarse = LoadWord(IdsWord(LoadWord(EntryNeighborWord(entry))));
    if (coarse >= MaxCells())
        return;

    const uint32_t face = LoadWord(EntryFaceWord(entry));
    PlaceInList(entry / MR_FACES, face * 2);
    PlaceInList(coarse, (face * 2) + 1);
}

    // --- SortLists: 仮の一覧の 1 つの値を、同じセルの一覧で自分より小さい値の数の位置へ(面の番号の昇順。1 スレッド = 一覧の 1 つ)---
    // 1 セルの一覧が長い(境のセルの粗い面。鎖で 208)時に 1 スレッドで並べ直すと 0.8 ms かかったので、値ごとに順位を数える
    [numthreads(BUILD_THREADS, 1, 1)] void BuildSortLists(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t position = dispatch.x;
    if (position >= 2 * LoadWord(HEADER_FACES))
        return;

    const uint32_t cellId = LoadWord(ListCellWord(position));
    const uint32_t start = LoadWord(CellStartWord(cellId));
    const uint32_t end = start + LoadWord(CellCountWord(cellId));
    const uint32_t value = LoadWord(RawListWord(position));
    uint32_t rank = start;
    for (uint32_t i = start; i < end; ++i) {
        if (LoadWord(RawListWord(i)) < value)
            ++rank;
    }

    g_system.Store(ListByte(rank), value);
}

// --- Apply(T-0132。系を作る段の後、GpuImplicit が解いた後に 1 回): 解いたセルの「足した後 − 刻みの初め」を伝導の変化の表へ足す
//     (CPU の AddImplicitConduction の後半)。この段だけ u5 = GpuImplicit のセルのバッファ(並びは系のセルと同じ ImGpuCell)。
//     刻みの初めのエネルギーは BuildCells と同じ式(熱容量と刻みの初めの温度から)で作り直す。活性の刻みでは、変わったセルのブロックを
//     伝導の一覧に足す(眠っているブロックにも足す。CPU は頁のブロックを全部 StepPagedBlock に通す。足すのは ConductApply)---
[numthreads(BUILD_THREADS, 1, 1)] void BuildApply(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t cellId = dispatch.x;
    if (cellId >= LoadWord(HEADER_CELLS))
        return;

    const ImGpuCell cell = g_system.Load<ImGpuCell>(CellByte(cellId));
    const int64_t startEnergy = ImEnergyFor(cell.heatCapacity, cell.startTemperature >> IM_TEMPERATURE_SHIFT);
    MrEnergyDelta change = MrMakeEnergyDelta();
    change.whole = cell.energy - startEnergy;
    change.fraction = cell.fraction;
    if (MrEnergyDeltaIsZero(change))
        return;

    const uint32_t address = LoadWord(CellAddressWord(cellId));
    const uint32_t slot = address / MR_BLOCK_CELLS;
    AddConductDelta(g_blocks[slot].page, address % MR_BLOCK_CELLS, change);
    if ((g_stepFlags & MR_STEP_LISTED) != 0)
        ConductAppend(slot);
}
