// multires_graph.hlsl — 多重解像度の木を細かくする・粗く戻す・影を引き戻すを Work Graph で(T-0017・T-0018。17 §1・§3・§5)。
// 細かくする鎖は再帰(1 レベル = 1 グループ。段の数は実行時に決まる。D-302)。
// 入口は 4 つ: RefineNode(CPU の入力 = 手で決めた影の鎖 / GPU の入力 = 許可した要求の鎖。multires_tree.hlsl の TreeAllocate が作る)・
// CoarsenRequestNode(GPU の入力 = 許可した粗くする要求)・PullBackNode・RemoveShadowNode(CPU の入力)。
// 変種は 3 つ: 既定(multires_graph)・溢れを使う世界(MR_WIDE_CELLS。multires_graph_wide)・細かくする段で寄せるセルの中身を置かれ方で配る世界
// (MR_REFINE_PLACE。multires_graph_place。T-1188・ADR-0499)。置かれ方の段は重く、同じプログラムに入れると使わない木でも RTX の HW の
// Work Graph が止まった(DEVICE_HUNG)ので、別のプログラムにして置かれ方の世界だけが持つ(gpu_multires.cpp の RecordUpload が選ぶ)。
// CPU リファレンスは engine/src/sim/multires_tree.cpp の RefineRequestLevel・ApplyCoarsen と、multires_nest.cpp の RefineShadowLevel・
// PullBackLevel・RemoveShadowChain(同じ順・同じ関数)。
// 木を変えたブロック(細かくした親と子・粗くした親)は活性の一覧(u13 = この刻みの種)に足す(つつく。T-0100)。
// 一様なブロック(T-0102): 一様な親の本物の子は一様(値 1 つ)。一様な親へ粗くして同じ値に戻らなければ、親を頁に広げる。
// 活性のブロックを刻むグラフは別の multires_activity_graph.hlsl(反応の核を含んで大きいので、使う時だけ作る)。
//
// 順番に依存しない理由: 1 刻みに 1 つの親は 1 つの要求だけが触る(取り合いは TreeSettle で決着)ので、グループどうしの書き込みは重ならない。
// 1 グループの中でスレッドは別々のセルに書き、見出し・返す枠・索引はスレッド 0 だけが書く。数える欄と帳簿は足し算。
// 次のレベルは前のレベルの書き込みを読むので、出力の前に Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC)、バッファは globallycoherent。
#include "sim/multires_bindings.hlsli"
#ifdef MR_WIDE_CELLS
#include "sim/multires_wide_access.hlsli"
#endif
#ifdef MR_REFINE_PLACE
#ifdef MR_WIDE_CELLS
#error 置かれ方の変種は溢れを使わない世界だけ(T-1188)
#endif
#include "sim/multires_refine_place.hlsli"
#endif

// 再帰の深さの上限(要求の鎖は MR_MAX_CHAIN_LEVELS 段まで)
static const uint32_t MR_MAX_RECURSION = MR_MAX_CHAIN_LEVELS;

struct MrChainRecord {
    uint32_t slot;
    uint32_t levelsLeft;
};

struct MrCoarsenRecord {
    uint32_t request;  // MR_NO_BLOCK なら何もしない(GPU の入力の空の代わり)
};

groupshared uint32_t gs_octantFraction;
groupshared uint32_t gs_restFraction;

// 手で決めた影の鎖の点(ルート定数)
MrRequest ShadowPoint() {
    return MrMakeRequest(MR_REQUEST_REFINE, g_pointLevel, (int64_t)FX_U64(g_pointXHigh, g_pointXLow),
                         (int64_t)FX_U64(g_pointYHigh, g_pointYLow), (int64_t)FX_U64(g_pointZHigh, g_pointZLow));
}

// 要求の返す端数の枠に足す(スレッド 0 だけ)
void ReleaseFraction(uint32_t request, uint32_t fractionSlot) {
    const uint32_t count = g_states[request].releaseCount;
    g_states[request].releases[count] = fractionSlot;
    g_states[request].releaseCount = count + 1;
}

#ifdef MR_WIDE_CELLS
// --- 溢れを使う変種(multires_graph_wide。T-0237。CPU は multires_tree.cpp の CopyWideToChild と「親を覆う」)---

groupshared uint32_t gs_tailStarts[MR_BLOCK_CELLS + 1];  // 子のセルの溢れの数 → 子の頁の面 0 の始まり

// 溢れの成分 1 つ(3 語)を写す
void CopyWideEntry(uint32_t fromPage, uint32_t fromSide, uint32_t fromEntry, uint32_t toPage, uint32_t toSide,
                   uint32_t toEntry) {
    const uint32_t from = MrWideEntryWord(fromEntry);
    const uint32_t to = MrWideEntryWord(toEntry);
    for (uint32_t word = 0; word < MR_WIDE_ENTRY_WORDS; ++word)
        g_treeWords[WideWord(toPage, toSide, to + word)] = g_treeWords[WideWord(fromPage, fromSide, from + word)];
}

// 細かくした子の頁(取ったばかり)の面 0 に、親の八分の一のセルの溢れを写す(1 スレッド = 子のセル t。グループの全部のスレッドが呼ぶ)。
// 塊は TreeWideReserve が要求の順に付けてある。足りない(置き場が尽きた)時だけ写さずに MR_COUNTER_OVERFLOW に数える。
// copyTails でない(影。T-0312)なら子の頁を空にするだけ(前に使った頁の溢れを読まないため)
void RefineWideTails(uint32_t parentPage, uint32_t childPage, uint32_t octant, uint32_t t, bool copyTails) {
    const uint32_t parentSide = copyTails ? WideCurrentSide(parentPage) : 0u;
    const uint2 range = copyTails ? WideRange(parentPage, parentSide, MrParentCellOfChild(octant, t)) : uint2(0, 0);
    gs_tailStarts[t] = range.y - range.x;
    GroupMemoryBarrierWithGroupSync();
    if (t == 0) {
        uint32_t total = 0;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const uint32_t count = gs_tailStarts[index];
            gs_tailStarts[index] = total;
            total += count;
        }

        gs_tailStarts[MR_BLOCK_CELLS] = total;
    }

    GroupMemoryBarrierWithGroupSync();

    // --- 写すものが無い・塊が足りない: 今の面を面 0 にして空に ---
    const uint32_t total = gs_tailStarts[MR_BLOCK_CELLS];
    const bool fits = MrWideChunksFor(total) <= WideChunkCount(childPage, 0);
    if (total == 0 || !fits) {
        ClearWideSideWords(childPage, t, MR_BLOCK_CELLS);
        if (!fits && t == 0)
            InterlockedAdd(g_counters[MR_COUNTER_OVERFLOW], total);

        return;
    }

    // --- 始まりの表と成分(セルの番号の順に詰める)---
    if (t == 0) {
        g_treeWords[WideHeaderAt(childPage, MR_WIDE_HEADER_SIDE)] = 0;
        g_treeWords[WideWord(childPage, 0, MR_BLOCK_CELLS)] = total;
    }

    const uint32_t first = gs_tailStarts[t];
    g_treeWords[WideWord(childPage, 0, t)] = first;
    for (uint32_t entry = range.x; entry < range.y; ++entry)
        CopyWideEntry(parentPage, parentSide, entry, childPage, 0, first + (entry - range.x));
}

// 親を覆う: 覆った八分の一のセルの溢れを今の面から除いて前へ詰める(スレッド 0。写す先はいつも読む所より前なので、同じ面の中で
// 読む前に上書きしない)。細かくするのはまれなので 1 スレッドで(親の溢れの数だけ)
void CoverWideTails(uint32_t page, uint32_t octant) {
    const uint32_t side = WideCurrentSide(page);
    if (WideChunkCount(page, side) == 0)
        return;

    uint32_t written = 0;
    uint32_t start = g_treeWords[WideWord(page, side, 0)];
    for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
        const uint32_t end = g_treeWords[WideWord(page, side, index + 1)];
        g_treeWords[WideWord(page, side, index)] = written;
        if (MrOctantOfCell(index) != octant) {
            for (uint32_t entry = start; entry < end; ++entry) {
                CopyWideEntry(page, side, entry, page, side, written);
                written += 1;
            }
        }

        start = end;
    }

    g_treeWords[WideWord(page, side, MR_BLOCK_CELLS)] = written;
}
#endif

// 1 段を細かくする(1 スレッド = 子のセル t。親のセル t も読む)
void RefineLevel(MrRefineRecord record, uint32_t t) {
    const bool fromRequest = record.request != MR_NO_BLOCK;
    const bool real = record.kind == MR_BLOCK_REAL;
    const MrBlock parent = g_blocks[record.parentSlot];
    const bool uniformChild = real && MrIsUniform(parent);  // 一様な親の本物の子は一様(T-0102)
    MrRequest target = ShadowPoint();                       // HLSL の ?: は構造体を返せない
    uint32_t childSlot = record.childSlot;
    uint32_t childPage = MrObserverPage(record.childSlot, g_worldBlocks);  // 影はいつも頁を持つ
    uint32_t candidate = MR_NO_FRACTION;
    if (fromRequest) {
        target = g_requests[record.request];
        const MrRequestState state = g_states[record.request];
        childSlot = PoppedBlock(state, record.depth);
        childPage = uniformChild ? MR_NO_PAGE : PoppedPage(state, record.depth);
        candidate = PoppedFraction(state, record.depth);
    }

    const uint32_t octant = MrOctantOfPoint(parent, target.x, target.y, target.z, target.level);

    // --- 端数: 親の八分の一に 0 でない端数があるか・親の残りに残るか(本物だけ)---
    if (t == 0) {
        gs_octantFraction = 0;
        gs_restFraction = 0;
    }

    GroupMemoryBarrierWithGroupSync();
    if (real && !MrFractionIsZero(LoadFraction(parent.fraction, t))) {
        if (MrOctantOfCell(t) == octant)
            InterlockedOr(gs_octantFraction, 1u);
        else
            InterlockedOr(gs_restFraction, 1u);
    }

    GroupMemoryBarrierWithGroupSync();
    const uint32_t childFraction = gs_octantFraction != 0 ? candidate : MR_NO_FRACTION;

    // --- セルと端数: 子は親と同じ数(一様なら値 1 つ)---
    const uint32_t parentCell = MrParentCellOfChild(octant, t);
    if (!uniformChild)
        g_cells[PageCellAddress(childPage, t)] = LoadBlockCell(parent, record.parentSlot, parentCell);
    else if (t == 0)
        g_cells[childSlot] = g_cells[record.parentSlot];

    if (childFraction != MR_NO_FRACTION)
        g_fractions[FractionAddress(childFraction, t)] = LoadFraction(parent.fraction, parentCell);

#ifdef MR_REFINE_PLACE
    // --- 寄せるセルの中身は置かれ方で(T-1188・T-1176。CPU の PlaceRefineOctantCells。端数は上で写した SAME のまま。
    //     グループで同じ条件なので、中のバリアは全部のスレッドが通る)---
    if (fromRequest && real && !uniformChild)
        PlaceRefineChildren(parent, record.parentSlot, octant, childPage, t);
#endif

#ifdef MR_WIDE_CELLS
    // --- 溢れ: 子の頁へ写す(親を覆う前。一様な子は頁を持たない。一様な親は溢れを持たない)。
    //     端数の溢れも子の端数の枠へ(T-0311。childFraction はグループで同じ)---
    if (!uniformChild)
        RefineWideTails(parent.page, childPage, octant, t, fromRequest && !MrIsUniform(parent));

    if (childFraction != MR_NO_FRACTION) {
        GroupMemoryBarrierWithGroupSync();  // gs_tailStarts を使い回す
        RefineWideTails(WideFractionArea(parent.fraction), WideFractionArea(childFraction), octant, t, true);
    }
#endif

    // --- 本物なら親を覆う(全部のスレッドが親を読み終えてから。一様な親は覆われた所を空と読むので書かない)---
    GroupMemoryBarrierWithGroupSync();
    if (real && t < MR_OCTANT_CELLS) {
        const uint32_t index = MrOctantCell(octant, t);
        if (!uniformChild)
            g_cells[PageCellAddress(parent.page, index)] = RxMakeEmptyCell(0);

        if (parent.fraction != MR_NO_FRACTION)
            g_fractions[FractionAddress(parent.fraction, index)] = MrMakeEmptyFraction();
    }

#ifdef MR_WIDE_CELLS
    if (real) {
        AllMemoryBarrierWithGroupSync();  // 全部のスレッドが親の溢れを写し終えてから
        if (t == 0 && !uniformChild)
            CoverWideTails(parent.page, octant);

        if (t == 0 && parent.fraction != MR_NO_FRACTION)
            CoverWideTails(WideFractionArea(parent.fraction), octant);  // 端数の溢れ(T-0311)
    }
#endif

    if (t != 0)
        return;

    // --- 見出し・返す枠・索引(スレッド 0)---
    if (fromRequest && candidate != MR_NO_FRACTION && childFraction == MR_NO_FRACTION)
        ReleaseFraction(record.request, candidate);

    if (real && parent.fraction != MR_NO_FRACTION && gs_restFraction == 0) {
        ReleaseFraction(record.request, parent.fraction);
        g_blocks[record.parentSlot].fraction = MR_NO_FRACTION;
    }

    if (real)
        g_blocks[record.parentSlot].children[octant] = childSlot;

    g_blocks[childSlot] = MrMakeChildBlock(parent, record.parentSlot, octant, record.kind, childFraction, childPage);
    if (!real)
        return;

    // --- 本物: 索引に入れ、木を変えたブロックをつつく(活性の種。最初の段だけ親も)---
    IndexInsert(childSlot);
    if (record.depth == 0)
        PokeBlock(record.parentSlot);

    PokeBlock(childSlot);
}

#ifdef MR_WIDE_CELLS
// --- 溢れを使う変種の粗くする和集合(T-0329。CPU は multires_tree.cpp の ApplyCoarsenOf<MrWideCoarsened>)---
// 子の溢れは置き場から直接引く(上限の無い形のセルを 8 個レジスタに持つと 1 スレッドが数 KiB になり HW で止まる疑い。ADR-0052 追記 5)。
// 子の形(インライン + 溢れの範囲)と結果の形(インライン + 溢れを書く先)ごとの道具をここに置き、核の MrCoarsenCellOf
// (multires.hlsli)をそのまま使う(同じ順・同じ和なので、CPU の上限の無い形とビット単位で同じ)。
// 1 スレッド = 親の八分の一のセル。① 数える(帳簿へ足す)→ ② 親の頁・端数の枠の新しい面の並び(スレッド 0 のプレフィックス和)
// → ③ もう一度和集合を取って溢れを新しい面へ書く → 面を入れ替える。八分の一の溢れが無ければ面はそのまま(覆った八分の一は溢れが無い)

static const uint32_t COARSEN_COUNT = 0;  // 数えて、落ちた分を帳簿へ
static const uint32_t COARSEN_WRITE = 1;  // 溢れを書く
static const uint32_t COARSEN_STARTS = MR_BLOCK_CELLS + 1;
static const uint32_t COARSEN_PAGE = 0;      // 並びの欄: 親の頁
static const uint32_t COARSEN_FRACTION = 1;  // 並びの欄: 親の端数の枠

groupshared uint32_t gs_coarsenStarts[2 * COARSEN_STARTS];  // [頁・端数の枠][セル] 溢れの数 → 新しい面の始まり
groupshared uint32_t gs_coarsenOctant[2];                   // 八分の一の溢れの合計

// 子のセル 1 つ・端数 1 つ(インラインは読む位置だけ持ち、使う時にバッファから読む。今の面の溢れの範囲)。
// インラインのセルを持つと 1 スレッドが KiB 単位になり、HW の Work Graph が止まった(T-0329)
struct GpuWideChildCell {
    int64_t energy;
    uint32_t address;  // g_cells
    uint32_t count;    // インラインの成分の数
    uint32_t area;
    uint32_t side;
    uint2 tail;
};

struct GpuWideChildFraction {
    uint64_t energy;
    uint32_t address;  // g_fractions(枠が無ければ count = 0)
    uint32_t count;
    uint32_t area;
    uint32_t side;
    uint2 tail;
};

struct GpuWideChildren {
    GpuWideChildCell cells[MR_CHILDREN_PER_CELL];
    GpuWideChildFraction fractions[MR_CHILDREN_PER_CELL];
};

// 粗くした結果(インライン 8 と、溢れの数・書く先。落ちた分は数える時に帳簿へ直に足すので一覧を持たない)
struct GpuWideCoarsened {
    RxCell cell;
    MrFraction fraction;
    uint32_t lostCount;
    uint32_t overflowCount;
    uint32_t energyLostBits;

    // --- 溢れ ---
    uint32_t mode;
    int32_t ledgerLevel;
    uint32_t cellTotal;  // 成分の数(インライン + 溢れ)
    uint32_t fractionTotal;
    uint32_t pageArea;  // 書く先(書かないなら MR_NO_PAGE)・面・始まり
    uint32_t pageSide;
    uint32_t pageFirst;
    uint32_t fractionArea;
    uint32_t fractionSide;
    uint32_t fractionFirst;
};

// --- 溢れの成分を読む・書く ---

uint32_t TailSpecies(uint32_t area, uint32_t side, uint32_t entry) {
    return g_treeWords[WideWord(area, side, MrWideEntryWord(entry))];
}

uint64_t TailAmount(uint32_t area, uint32_t side, uint32_t entry) {
    const uint32_t word = MrWideEntryWord(entry);

    return FX_U64(g_treeWords[WideWord(area, side, word + 2)], g_treeWords[WideWord(area, side, word + 1)]);
}

void WriteTailEntry(uint32_t area, uint32_t side, uint32_t entry, uint32_t species, uint64_t amount) {
    const uint32_t word = MrWideEntryWord(entry);
    g_treeWords[WideWord(area, side, word)] = species;
    g_treeWords[WideWord(area, side, word + 1)] = (uint32_t)amount;
    g_treeWords[WideWord(area, side, word + 2)] = (uint32_t)(amount >> 32);
}

// インラインの成分 i(fraction なら g_fractions、でなければ g_cells の address)
uint32_t InlineSpecies(bool fraction, uint32_t address, uint32_t i) {
    return fraction ? g_fractions[address].species[i] : g_cells[address].species[i];
}

uint64_t InlineAmount(bool fraction, uint32_t address, uint32_t i) {
    return fraction ? g_fractions[address].amounts[i] : g_cells[address].amounts[i];
}

// インライン count 個と溢れの範囲 tail で、after より大きい最小の物質 ID(並びの順は仮定しない。MrNextSpecies と同じ)
uint32_t NextPartSpecies(bool fraction, uint32_t address, uint32_t count, uint32_t area, uint32_t side, uint2 tail,
                         uint32_t after) {
    uint32_t next = MR_NO_SPECIES;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t species = InlineSpecies(fraction, address, i);
        if (species > after && species < next)
            next = species;
    }

    for (uint32_t entry = tail.x; entry < tail.y; ++entry) {
        const uint32_t species = TailSpecies(area, side, entry);
        if (species > after && species < next)
            next = species;
    }

    return next;
}

// インライン count 個と溢れの範囲 tail の物質 speciesId の量(無ければ 0)
uint64_t PartAmountOf(bool fraction, uint32_t address, uint32_t count, uint32_t area, uint32_t side, uint2 tail,
                      uint32_t speciesId) {
    uint64_t amount = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (InlineSpecies(fraction, address, i) == speciesId)
            amount = InlineAmount(fraction, address, i);
    }

    for (uint32_t entry = tail.x; entry < tail.y; ++entry) {
        if (TailSpecies(area, side, entry) == speciesId)
            amount = TailAmount(area, side, entry);
    }

    return amount;
}

// --- 核(MrCoarsenCellOf)が呼ぶ形ごとの道具の版(関数テンプレートの明示的な特殊化。DXC は非テンプレートの多重定義と曖昧にする)---

template <>
uint32_t MrNextSpecies<GpuWideChildCell, GpuWideChildFraction>(GpuWideChildCell cell, GpuWideChildFraction fraction,
                                                               uint32_t after) {
    const uint32_t cellNext = NextPartSpecies(false, cell.address, cell.count, cell.area, cell.side, cell.tail, after);
    const uint32_t fractionNext = NextPartSpecies(true, fraction.address, fraction.count, fraction.area, fraction.side,
                                                  fraction.tail, after);

    return min(cellNext, fractionNext);
}

template <>
uint64_t MrAmountOf<GpuWideChildCell>(GpuWideChildCell cell, uint32_t speciesId) {
    return PartAmountOf(false, cell.address, cell.count, cell.area, cell.side, cell.tail, speciesId);
}

template <>
uint64_t MrFractionOf<GpuWideChildFraction>(GpuWideChildFraction fraction, uint32_t speciesId) {
    return PartAmountOf(true, fraction.address, fraction.count, fraction.area, fraction.side, fraction.tail, speciesId);
}

uint32_t MrUnionStepLimit(GpuWideChildren children) {
    uint32_t total = 1;
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
        total += children.cells[j].count + (children.cells[j].tail.y - children.cells[j].tail.x);
        total += children.fractions[j].count + (children.fractions[j].tail.y - children.fractions[j].tail.x);
    }

    return total;
}

// 落ちた下位 3bit は数える時に帳簿へ直に足す(CPU の RecordCoarsened と同じ欄。足し算なので順に依らない)
template <>
GpuWideCoarsened MrRecordLost<GpuWideCoarsened>(GpuWideCoarsened result, uint32_t species, uint32_t lostBits) {
    if (lostBits != 0 && result.mode == COARSEN_COUNT)
        AddToLedger(result.ledgerLevel, 1 + species, lostBits);

    return result;
}

// 粗くした物質 1 つ(ID の昇順): インライン 8 の後は溢れ(書く時だけ書く先へ)
template <>
GpuWideCoarsened MrAppendCoarsened<GpuWideCoarsened>(GpuWideCoarsened result, uint32_t species, MrWideShifted shifted) {
    if (shifted.whole != 0) {
        if (result.cellTotal < RX_MAX_CELL_SPECIES) {
            result.cell.species[result.cellTotal] = species;
            result.cell.amounts[result.cellTotal] = shifted.whole;
            result.cell.speciesCount += 1;
        } else if (result.mode == COARSEN_WRITE && result.pageArea != MR_NO_PAGE) {
            WriteTailEntry(result.pageArea, result.pageSide,
                           result.pageFirst + (result.cellTotal - RX_MAX_CELL_SPECIES), species, shifted.whole);
        }

        result.cellTotal += 1;
    }

    if (shifted.fraction != 0) {
        if (result.fractionTotal < RX_MAX_CELL_SPECIES) {
            result.fraction.species[result.fractionTotal] = species;
            result.fraction.amounts[result.fractionTotal] = shifted.fraction;
            result.fraction.speciesCount += 1;
        } else if (result.mode == COARSEN_WRITE && result.fractionArea != MR_NO_PAGE) {
            WriteTailEntry(result.fractionArea, result.fractionSide,
                           result.fractionFirst + (result.fractionTotal - RX_MAX_CELL_SPECIES), species,
                           shifted.fraction);
        }

        result.fractionTotal += 1;
    }

    return result;
}

// --- 粗くする 1 セル ---

// 親の八分の一のセル t の子 2³(粗くする子は本物の子を持たないので、一様なら値のセルをそのまま読む)
GpuWideChildren GatherWideChildren(MrBlock child, uint32_t childSlot, uint32_t t) {
    const bool paged = !MrIsUniform(child);
    const bool fractions = child.fraction != MR_NO_FRACTION;
    const uint32_t fractionArea = fractions ? WideFractionArea(child.fraction) : 0u;
    const uint32_t pageSide = paged ? WideCurrentSide(child.page) : 0u;
    const uint32_t fractionSide = fractions ? WideCurrentSide(fractionArea) : 0u;
    GpuWideChildren children;
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
        const uint32_t index = MrChildCell(t, j);
        const uint32_t cellAddress = paged ? PageCellAddress(child.page, index) : childSlot;
        children.cells[j].energy = g_cells[cellAddress].energy;
        children.cells[j].address = cellAddress;
        children.cells[j].count = g_cells[cellAddress].speciesCount;
        children.cells[j].area = child.page;
        children.cells[j].side = pageSide;
        children.cells[j].tail = paged ? WideRange(child.page, pageSide, index) : uint2(0, 0);

        const uint32_t fractionAddress = fractions ? FractionAddress(child.fraction, index) : 0u;
        children.fractions[j].energy = fractions ? g_fractions[fractionAddress].energy : 0u;
        children.fractions[j].address = fractionAddress;
        children.fractions[j].count = fractions ? g_fractions[fractionAddress].speciesCount : 0u;
        children.fractions[j].area = fractionArea;
        children.fractions[j].side = fractionSide;
        children.fractions[j].tail = fractions ? WideRange(fractionArea, fractionSide, index) : uint2(0, 0);
    }

    return children;
}

// 子 2³ を 1 つに(mode。書くなら書く先は「領域・面・始まり」、書かない領域は MR_NO_PAGE)
GpuWideCoarsened CoarsenWideCell(MrBlock child, uint32_t childSlot, uint32_t t, uint32_t mode, uint3 page,
                                 uint3 fraction) {
    GpuWideCoarsened result;
    result.cell = RxMakeEmptyCell(0);
    result.fraction = MrMakeEmptyFraction();
    result.lostCount = 0;
    result.overflowCount = 0;
    result.energyLostBits = 0;
    result.mode = mode;
    result.ledgerLevel = child.level;
    result.cellTotal = 0;
    result.fractionTotal = 0;
    result.pageArea = page.x;
    result.pageSide = page.y;
    result.pageFirst = page.z;
    result.fractionArea = fraction.x;
    result.fractionSide = fraction.y;
    result.fractionFirst = fraction.z;

    return MrCoarsenCellOf(GatherWideChildren(child, childSlot, t), result);
}

uint32_t TailCount(uint32_t total) {
    return total > RX_MAX_CELL_SPECIES ? total - RX_MAX_CELL_SPECIES : 0u;
}

// --- 親の新しい面 ---

// 領域 area の欄 which に、新しい面の溢れの数を置く(八分の一のセルは粗くした数 own、ほかは今の面の溢れ。keepRest でなければ 0)
void CountCoarsenArea(uint32_t which, uint32_t area, bool keepRest, uint32_t octant, uint32_t t, uint32_t own) {
    const uint32_t base = which * COARSEN_STARTS;
    const uint32_t side = keepRest ? WideCurrentSide(area) : 0u;
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / MR_OCTANT_CELLS; ++k) {
        const uint32_t index = t + (k * MR_OCTANT_CELLS);
        if (MrOctantOfCell(index) == octant)
            continue;

        const uint2 range = keepRest ? WideRange(area, side, index) : uint2(0, 0);
        gs_coarsenStarts[base + index] = range.y - range.x;
    }

    gs_coarsenStarts[base + MrOctantCell(octant, t)] = own;
}

// 欄 which の数を始まりにする(スレッド 0)。八分の一の合計を gs_coarsenOctant に
void PrefixCoarsenArea(uint32_t which, uint32_t octant) {
    const uint32_t base = which * COARSEN_STARTS;
    uint32_t total = 0;
    uint32_t octantTotal = 0;
    for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
        const uint32_t count = gs_coarsenStarts[base + index];
        gs_coarsenStarts[base + index] = total;
        total += count;
        octantTotal += MrOctantOfCell(index) == octant ? count : 0u;
    }

    gs_coarsenStarts[base + MR_BLOCK_CELLS] = total;
    gs_coarsenOctant[which] = octantTotal;
}

// 欄 which の新しい面へ書けるか(八分の一に溢れがあり、書く面の塊に入る)。入らなければ書く先は MR_NO_PAGE
uint32_t CoarsenWriteArea(uint32_t which, uint32_t area) {
    if (area == MR_NO_PAGE || gs_coarsenOctant[which] == 0)
        return MR_NO_PAGE;

    const uint32_t need = MrWideChunksFor(gs_coarsenStarts[(which * COARSEN_STARTS) + MR_BLOCK_CELLS]);

    return need <= WideChunkCount(area, 1 - WideCurrentSide(area)) ? area : MR_NO_PAGE;
}

// 新しい面に始まりの表とほかのセルの溢れを写す(全部のスレッド。八分の一のセルは ③ が書く)
void CopyCoarsenRest(uint32_t which, uint32_t area, bool keepRest, uint32_t octant, uint32_t t) {
    const uint32_t base = which * COARSEN_STARTS;
    const uint32_t side = WideCurrentSide(area);
    for (uint32_t index = t; index <= MR_BLOCK_CELLS; index += MR_OCTANT_CELLS)
        g_treeWords[WideWord(area, 1 - side, index)] = gs_coarsenStarts[base + index];

    for (uint32_t k = 0; keepRest && k < MR_BLOCK_CELLS / MR_OCTANT_CELLS; ++k) {
        const uint32_t index = t + (k * MR_OCTANT_CELLS);
        if (MrOctantOfCell(index) == octant)
            continue;

        const uint2 range = WideRange(area, side, index);
        const uint32_t first = gs_coarsenStarts[base + index];
        for (uint32_t entry = range.x; entry < range.y; ++entry)
            CopyWideEntry(area, side, entry, area, 1 - side, first + (entry - range.x));
    }
}

// 書かなかった領域: 新しく使う領域(広げた頁・取った端数の枠)は今の面を空に、前からの領域はそのまま(覆った八分の一は溢れが無い)。
// 八分の一に溢れがあったのに入らなければ捨てて MR_COUNTER_OVERFLOW に数える(置き場が尽きた時だけ。CPU と食い違う)
void KeepCoarsenArea(uint32_t which, uint32_t area, bool keepRest, uint32_t t, uint32_t own) {
    if (area == MR_NO_PAGE)
        return;

    if (!keepRest)
        ClearWideSideWords(area, t, MR_OCTANT_CELLS);

    if (gs_coarsenOctant[which] != 0 && own != 0)
        InterlockedAdd(g_counters[MR_COUNTER_OVERFLOW], own);
}

// 書いた領域の面を入れ替える(スレッド 0。③ の後)。頁の書いた面の塊が余れば WideAllocate に返させる(PlanWideSide と同じ要求の語)
void SwapCoarsenArea(uint32_t which, uint32_t area, bool page) {
    const uint32_t side = 1 - WideCurrentSide(area);
    g_treeWords[WideHeaderAt(area, MR_WIDE_HEADER_SIDE)] = side;
    const uint32_t need = MrWideChunksFor(gs_coarsenStarts[(which * COARSEN_STARTS) + MR_BLOCK_CELLS]);
    if (page && need < WideChunkCount(area, side))
        g_treeWords[WideHeaderAt(area, MR_WIDE_HEADER_REQUEST)] = MrWideMakeRequest(side, need, false);
}

// 粗くした八分の一の溢れを親の頁(pageArea。頁を持たない一様な親は MR_NO_PAGE)と端数の枠へ書く(全部のスレッドが呼ぶ。終わりに同期)。
// ownPage・ownFraction = ① で数えた八分の一のセル t の溢れの数(セル・端数)
void WriteCoarsenTails(MrBlock child, uint32_t childSlot, uint32_t ownPage, uint32_t ownFraction, uint32_t pageArea,
                       bool keepPage, uint32_t fractionArea, bool keepFraction, uint32_t octant, uint32_t t) {
    if (pageArea != MR_NO_PAGE)
        CountCoarsenArea(COARSEN_PAGE, pageArea, keepPage, octant, t, ownPage);

    CountCoarsenArea(COARSEN_FRACTION, fractionArea, keepFraction, octant, t, ownFraction);
    GroupMemoryBarrierWithGroupSync();
    if (t == 0) {
        gs_coarsenOctant[COARSEN_PAGE] = 0;
        if (pageArea != MR_NO_PAGE)
            PrefixCoarsenArea(COARSEN_PAGE, octant);

        PrefixCoarsenArea(COARSEN_FRACTION, octant);
    }

    GroupMemoryBarrierWithGroupSync();
    const uint32_t pageWrite = CoarsenWriteArea(COARSEN_PAGE, pageArea);
    const uint32_t fractionWrite = CoarsenWriteArea(COARSEN_FRACTION, fractionArea);
    if (pageWrite == MR_NO_PAGE)
        KeepCoarsenArea(COARSEN_PAGE, pageArea, keepPage, t, ownPage);
    else
        CopyCoarsenRest(COARSEN_PAGE, pageArea, keepPage, octant, t);

    if (fractionWrite == MR_NO_PAGE)
        KeepCoarsenArea(COARSEN_FRACTION, fractionArea, keepFraction, t, ownFraction);
    else
        CopyCoarsenRest(COARSEN_FRACTION, fractionArea, keepFraction, octant, t);

    // --- ③ もう一度和集合を取って、八分の一のセルの溢れを新しい面へ ---
    if (pageWrite != MR_NO_PAGE || fractionWrite != MR_NO_PAGE) {
        const uint32_t index = MrOctantCell(octant, t);
        const uint3 page = pageWrite == MR_NO_PAGE
                               ? uint3(MR_NO_PAGE, 0, 0)
                               : uint3(pageWrite, 1 - WideCurrentSide(pageWrite), gs_coarsenStarts[index]);
        const uint3 fraction = fractionWrite == MR_NO_PAGE ? uint3(MR_NO_PAGE, 0, 0)
                                                           : uint3(fractionWrite, 1 - WideCurrentSide(fractionWrite),
                                                                   gs_coarsenStarts[COARSEN_STARTS + index]);
        CoarsenWideCell(child, childSlot, t, COARSEN_WRITE, page, fraction);
    }

    AllMemoryBarrierWithGroupSync();  // 全部のスレッドが書き終えてから面を入れ替える
    if (t == 0 && pageWrite != MR_NO_PAGE)
        SwapCoarsenArea(COARSEN_PAGE, pageWrite, true);

    if (t == 0 && fractionWrite != MR_NO_PAGE)
        SwapCoarsenArea(COARSEN_FRACTION, fractionWrite, false);

    AllMemoryBarrierWithGroupSync();
}
#endif

// 許可した粗くする要求の 1 段(1 スレッド = 親の八分の一のセル t。multires_tree.cpp の ApplyCoarsen)
void CoarsenRequest(uint32_t request, uint32_t t) {
    const MrRequestState state = g_states[request];
    const uint32_t childSlot = state.target;
    const MrBlock child = g_blocks[childSlot];
    const uint32_t parentSlot = child.parent;
    const MrBlock parent = g_blocks[parentSlot];
    const uint32_t octant = child.parentOctant;

    // --- 子 2³ を 1 つに(溢れを使う変種は子の溢れまで読む。落ちた成分は数える時に帳簿へ。T-0329)---
#ifdef MR_WIDE_CELLS
    const GpuWideCoarsened result = CoarsenWideCell(child, childSlot, t, COARSEN_COUNT, uint3(MR_NO_PAGE, 0, 0),
                                                    uint3(MR_NO_PAGE, 0, 0));
#else
    MrChildren children;
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
        const uint32_t index = MrChildCell(t, j);
        children.cells[j] = LoadBlockCell(child, childSlot, index);
        children.fractions[j] = LoadFraction(child.fraction, index);
    }

    const MrCoarsened result = MrCoarsenCell(children);
#endif
    if (result.lostCount != 0)
        InterlockedAdd(g_counters[MR_COUNTER_LOST], result.lostCount);

    if (result.overflowCount != 0)
        InterlockedAdd(g_counters[MR_COUNTER_OVERFLOW], result.overflowCount);

    if (result.energyLostBits != 0)
        AddToLedger(child.level, 0, result.energyLostBits);

#ifndef MR_WIDE_CELLS
    for (uint32_t k = 0; k < result.lostSpeciesCount; ++k)
        AddToLedger(child.level, 1 + result.lostSpecies[k], result.lostBits[k]);
#endif

    // --- 親の端数: 枠が無ければ取っておいた枠(八分の一の外を 0 で始める)。残りに 0 でない端数があるか。
    //     一様な親が同じ値に戻らなければ頁に広げる(八分の一の外を一様の値で埋める。T-0102)---
    if (t == 0) {
        gs_octantFraction = 0;
        gs_restFraction = 0;
    }

    GroupMemoryBarrierWithGroupSync();
    const bool expandParent = state.pageNeed != 0;
    const uint32_t parentPage = expandParent ? PoppedPage(state, 0) : parent.page;
    const uint32_t parentFraction = parent.fraction != MR_NO_FRACTION ? parent.fraction : PoppedFraction(state, 0);
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / MR_OCTANT_CELLS; ++k) {
        const uint32_t index = t + (k * MR_OCTANT_CELLS);
        if (MrOctantOfCell(index) == octant)
            continue;

        if (expandParent)
            g_cells[PageCellAddress(parentPage, index)] = MrUniformCell(parent, g_cells[parentSlot], index);

        if (parent.fraction == MR_NO_FRACTION)
            g_fractions[FractionAddress(parentFraction, index)] = MrMakeEmptyFraction();
        else if (!MrFractionIsZero(g_fractions[FractionAddress(parentFraction, index)]))
            InterlockedOr(gs_restFraction, 1u);
    }

    if (!MrFractionIsZero(result.fraction))
        InterlockedOr(gs_octantFraction, 1u);

    const uint32_t index = MrOctantCell(octant, t);
    if (expandParent || !MrIsUniform(parent))
        g_cells[PageCellAddress(parentPage, index)] = result.cell;

    g_fractions[FractionAddress(parentFraction, index)] = result.fraction;

    GroupMemoryBarrierWithGroupSync();

#ifdef MR_WIDE_CELLS
    // --- 溢れ: 粗くした八分の一の溢れを親の頁と端数の枠の新しい面へ(広げた頁・取った端数の枠は残りの溢れが無い。T-0329)。
    //     その後で、返す子の頁と端数の枠の今の面を空にする(CPU の ClearPageOverflow・ClearFractionOverflow。T-0311)---
    const bool parentPaged = expandParent || !MrIsUniform(parent);
    WriteCoarsenTails(child, childSlot, TailCount(result.cellTotal), TailCount(result.fractionTotal),
                      parentPaged ? parentPage : MR_NO_PAGE, !expandParent, WideFractionArea(parentFraction),
                      parent.fraction != MR_NO_FRACTION, octant, t);
    if (!MrIsUniform(child))
        ClearWideSideWords(child.page, t, MR_OCTANT_CELLS);

    if (child.fraction != MR_NO_FRACTION)
        ClearWideSideWords(WideFractionArea(child.fraction), t, MR_OCTANT_CELLS);
#endif
    if (t != 0)
        return;

    // --- 見出し・返す枠・索引(スレッド 0。CPU と同じ順に返す)---
    uint32_t kept = parentFraction;
    if (gs_octantFraction == 0 && gs_restFraction == 0) {
        ReleaseFraction(request, parentFraction);
        kept = MR_NO_FRACTION;
    }

    if (child.fraction != MR_NO_FRACTION)
        ReleaseFraction(request, child.fraction);

    g_states[request].releaseBlock = childSlot;
    if (!MrIsUniform(child))
        g_states[request].releasePage = child.page;

    if (expandParent)
        g_blocks[parentSlot].page = parentPage;

    g_blocks[parentSlot].fraction = kept;
    g_blocks[parentSlot].children[octant] = MR_NO_BLOCK;
    IndexRemove(childSlot);
    g_blocks[childSlot] = MrMakeUnusedBlock();
    PokeBlock(parentSlot);
}

// clang-format は HLSL のノードの属性を並べ崩すので、属性つきの宣言だけ整形を止める
// clang-format off

// 親の八分の一を子ブロックに細かくし、次の段を自分に投げる(1 スレッド = 子のセル 1 つ)
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(512, 1, 1)]
[NodeMaxRecursionDepth(MR_MAX_RECURSION)]
[NodeIsProgramEntry]
void RefineNode(DispatchNodeInputRecord<MrRefineRecord> input, uint32_t thread : SV_GroupIndex,
                [MaxRecords(1)] [NodeId("RefineNode")] NodeOutput<MrRefineRecord> next) {
    const MrRefineRecord record = input.Get();
    if (record.levelsLeft > 0)
        RefineLevel(record, thread);

    // --- 次のレベル ---
    Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC);
    const bool recurse = record.levelsLeft > 1;
    GroupNodeOutputRecords<MrRefineRecord> output = next.GetGroupNodeOutputRecords(recurse ? 1 : 0);
    if (recurse && thread == 0) {
        const bool fromRequest = record.request != MR_NO_BLOCK;
        const uint32_t childSlot = fromRequest ? PoppedBlock(g_states[record.request], record.depth) : record.childSlot;
        output.Get().request = record.request;
        output.Get().parentSlot = childSlot;
        output.Get().childSlot = childSlot + 1;
        output.Get().levelsLeft = record.levelsLeft - 1;
        output.Get().kind = record.kind;
        output.Get().depth = record.depth + 1;
    }

    output.OutputComplete();
}

// 許可した粗くする要求: 子ブロックを親の八分の一へ戻す(1 スレッド = 親のセル 1 つ)
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(64, 1, 1)]
[NodeIsProgramEntry]
void CoarsenRequestNode(DispatchNodeInputRecord<MrCoarsenRecord> input, uint32_t thread : SV_GroupIndex) {
    const uint32_t request = input.Get().request;
    if (request == MR_NO_BLOCK)
        return;

    CoarsenRequest(request, thread);
}

// clang-format on

// clang-format off

// 影の子 2³ を親 × 8 に引き戻し、次はその下の影を(1 スレッド = 親のセル 1 つ。影の鎖の枠は連続)
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(64, 1, 1)]
[NodeMaxRecursionDepth(MR_MAX_RECURSION)]
[NodeIsProgramEntry]
void PullBackNode(DispatchNodeInputRecord<MrChainRecord> input, uint32_t thread : SV_GroupIndex,
                  [MaxRecords(1)] [NodeId("PullBackNode")] NodeOutput<MrChainRecord> next) {
    const MrChainRecord record = input.Get();
    const MrBlock shadow = g_blocks[record.slot];
    const RxCell parent = LoadCell(shadow.parent, MrOctantCell(shadow.parentOctant, thread));

    MrShadowFamily family;
    family.clamped = 0;
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
        family.cells[j] = g_cells[PageCellAddress(shadow.page, MrChildCell(thread, j))];

    const MrShadowFamily pulled = MrPullBackShadow(MakeTable(), parent, family);
    if (pulled.clamped != 0)
        InterlockedAdd(g_counters[MR_COUNTER_SHADOW_CLAMPED], pulled.clamped);

    for (uint32_t k = 0; k < MR_CHILDREN_PER_CELL; ++k)
        g_cells[PageCellAddress(shadow.page, MrChildCell(thread, k))] = pulled.cells[k];

    // --- セルの値が変わりうるので、つついた印にして次の刻みに tc を書き直す(CPU の PullBackLevel。ADR-0018 追記 T-0125。
    //     観察の枠は種の一覧に入れない)---
    if (thread == 0)
        g_blocks[record.slot].busyTick = MR_BUSY_POKED;

    // --- 次のレベル ---
    Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC);
    const bool recurse = record.levelsLeft > 1;
    GroupNodeOutputRecords<MrChainRecord> output = next.GetGroupNodeOutputRecords(recurse ? 1 : 0);
    if (recurse && thread == 0) {
        output.Get().slot = record.slot + 1;
        output.Get().levelsLeft = record.levelsLeft - 1;
    }

    output.OutputComplete();
}

// 影の鎖を捨てる(世界に返さない。見出しを空にするだけ)
[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
void RemoveShadowNode(ThreadNodeInputRecord<MrChainRecord> input) {
    const MrChainRecord record = input.Get();
    for (uint32_t i = 0; i < record.levelsLeft; ++i)
        g_blocks[record.slot + i] = MrMakeUnusedBlock();
}

// clang-format on
