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
//   5. ConductFlowsBlock: 刻むブロックのセルの面の流れを変化に足す(刻みの初めのセルから。Jacobi 型)。面の流れと当てる順は運ぶ計算の
//      共通の土台の当てる段(common/transport_apply.hlsli の TxFlowCell)と流れの種類 TxConduction(common/transport_conduction.hlsli)で、
//      CPU と同じ関数(T-0766・ADR-0334)。ここは回し方と世界の約束(GpuConductionWorld: 見立ての写し・64bit の atomic の足し算)だけ。
//      開いた世界の端(common/world_edge.hlsli。箱・外の柱は表の語)の面は外のセルと流し、帳簿の「世界の端」へ(T-0773・ADR-0337)。
//      陰解法を入れる刻み(T-0132)は、基準より細かいブロック(InImplicitConduction)を飛ばし、続けて陰解法の段(gpu_multires_implicit.cpp)が
//      解いた変化を表に足す(陰解法で解くブロックの外の端はまだ wall と同じ。CPU も同じ。T-0784)
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
#include "common/multires_table_words.hlsli"
#include "common/transport_conduction.hlsli"
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

// 自分のブロックのセルの見立てと、ブロックの外の面の先(T-0111)。印と流れの段の初めに、セルの見立て(MakeCellViewParts の種類が読む部分)を
// 1 セル 1 回、ブロックの面の外の隣(MrFindFaceNeighbor の索引引き・開いた世界の端とその見立て)を面のセル 1 つに 1 回だけ、別々のスレッドで
// 計算し、面ごとの計算はここを読む。段の間はセルも木も変わらないので、毎回計算するのと同じ値。見立ての写しは PackView(伝導はセル全体の熱)、
// 外の隣は x kind・y 枠・z セル(開いた端なら外の柱の行)・w レベルの差。外の隣の番号 = 面 × 64 + 面の上の位置(HaloEntry)
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

// 陰解法で解くブロックか(T-0132。CPU の nest_detail::IsImplicitBlock): 基準より細かく、基準 + implicitMaxGap 以下の世界の本物の
// ブロックのうち、系を作る段(implicit_build.hlsl の ScanBlocks)が上限の中で選んで印を付けたもの(T-0178。印の無いものは陽解法のまま)。
// その面は流れの段で計算せず、陰解法の段(gpu_multires_implicit.cpp)が解いて変化の表に足す
bool InImplicitConduction(uint32_t slot, MrBlock block) {
    if (!ImplicitStep())
        return false;

    const int32_t finest = SubcycleBaseLevel() + (int32_t)ImplicitMaxGap();

    return slot < g_worldBlocks && block.kind == MR_BLOCK_REAL && block.level > SubcycleBaseLevel() &&
           block.level <= finest && HasConductMark(slot, CONDUCT_MARK_IMPLICIT);
}

// ブロックの面に接するセルか(一様なブロックの中のセルどうしは同じ値・粗い側への面はブロックの面にしかない)
bool OnBlockSurface(uint32_t index) {
    const uint32_t last = MR_BLOCK_EDGE - 1;
    const uint32_t x = MrCellX(index);
    const uint32_t y = MrCellY(index);
    const uint32_t z = MrCellZ(index);

    return x == 0 || x == last || y == 0 || y == last || z == 0 || z == last;
}

// --- 表の語(t2。common/multires_table_words.hlsli の Words の約束)の相の表と世界の端(T-0766・T-0773)---

struct GpuTableWords {
    uint32_t unused;

    uint32_t Word(uint32_t at) { return g_tableWords[at]; }
};

GpuTableWords MakeGpuTableWords() {
    GpuTableWords words;
    words.unused = 0;

    return words;
}

// 見立ての表の約束(cell_view.hlsli: 物質の Species と相の Phase。CPU は sim::CellViewTable)
struct GpuViewTable {
    uint32_t unused;

    RxSpecies Species(uint32_t id) { return g_species[id]; }
    CvSpeciesPhase Phase(uint32_t id) { return TwPhase(MakeGpuTableWords(), id); }
};

GpuViewTable MakeViewTable() {
    GpuViewTable table;
    table.unused = 0;

    return table;
}

// 世界の端(world_edge.hlsli。CPU は MultiresNest::edges)
WeWorld GpuWorldEdges() {
    return TwWorldEdges(MakeGpuTableWords());
}

int64_t GpuColumnBaseUp() {
    return TwColumnBaseUp(MakeGpuTableWords());
}

uint32_t GpuColumnRows() {
    return TwColumnRows(MakeGpuTableWords());
}

RxCell GpuColumnCell(uint32_t row) {
    return TwColumnCell(MakeGpuTableWords(), row);
}

// --- セルの見立て(cell_view.hlsli の MakeCellViewParts。CPU の CellViews と同じ関数・同じ部分。T-0766)---

// 伝導の流れの種類(当てる段に渡す 3 つ。安定の条件の値は stepFlags の小刻み)
TxConduction MakeConductionKind() {
    return TxMakeConduction(SubcycleBaseLevel(), SubcycleGap());
}

// 見立てを作る部分 = 種類の ViewParts(定数。作らない部分〔相ごとの和・割り算・相の表の読み〕はインラインの後で消える。T-0767)
uint32_t ConductViewParts() {
    return MakeConductionKind().ViewParts();
}

#ifdef MR_WIDE_CELLS
// 溢れを持つセルの見立て: インライン → 溢れ(今の面 side)の順に 1 成分ずつ読んで足す(T-0272)。上限の無い形のセル(RxGpuWideCell)を
// 作らないので、1 スレッドのスクラッチが上限(RX_GPU_WIDE_SPECIES)に依らない(512 スレッドの段で上限 56 以上が HW で止まった。T-0290)。
// 和は整数なので足す順によらず、上限の無い形のセルを MakeCellViewParts で見立てたものと同じ値(CPU の CellViews の LoadWideNestCell)
CvCellView WideTailView(RxCell inlineCell, MrBlock block, uint32_t side, uint32_t index, uint32_t parts) {
    const GpuViewTable table = MakeViewTable();
    CvViewSums sums = CvMakeViewSums();
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i)
        sums = CvAddSpeciesParts(table, sums, inlineCell.species[i], inlineCell.amounts[i], parts);

    const uint2 range = WideRange(block.page, side, index);
    for (uint32_t e = range.x; e < range.y; ++e) {
        const uint32_t word = MrWideEntryWord(e);
        const uint64_t amount = FX_U64(g_treeWords[WideWord(block.page, side, word + 2)],
                                       g_treeWords[WideWord(block.page, side, word + 1)]);
        sums = CvAddSpeciesParts(table, sums, g_treeWords[WideWord(block.page, side, word)], amount, parts);
    }

    return CvViewFromSumsParts(inlineCell.energy, sums, block.level, 0, parts);
}
#endif

// 枠 slot(見出し block)のセル index の見立て(伝導は圧力を読まないので基準の圧力は 0。CPU の CellViews::View)。
// 溢れを使う変種(MR_WIDE_CELLS。T-0211)は、溢れを持つセルだけ溢れまで足す(8 種以下のセルは同じ成分の並びなので同じ値)
CvCellView BlockCellView(MrBlock block, uint32_t slot, uint32_t index, uint32_t parts) {
#ifdef MR_WIDE_CELLS
    if (!MrIsUniform(block)) {
        const RxCell inlineCell = g_cells[PageCellAddress(block.page, index)];
        const uint32_t side = WideCurrentSide(block.page);
        if (WideHasTail(inlineCell, block.page, side, index))
            return WideTailView(inlineCell, block, side, index, parts);
    }
#endif

    return MakeCellViewParts(MakeViewTable(), LoadBlockCell(block, slot, index), block.level, 0, parts);
}

// 開いた世界の端の外のセル(外の柱の行 row)を自分と同じレベル level で見立てたもの(CPU の CellViews::Outside。T-0773)
CvCellView OutsideView(uint32_t row, int32_t level, uint32_t parts) {
    return MakeCellViewParts(MakeViewTable(), GpuColumnCell(row), level, 0, parts);
}

// --- 見立ての写し(T-0111 の形のまま。groupshared に入るのは種類が読む部分だけ)---
// 伝導の種類が読むのはセル全体の熱(view.thermal。CV_VIEW_THERMAL)だけなので、写しは MrThermal の 4 語
// (見立て全部は 1 セル約 200 バイトで 512 + 384 セル分は groupshared に入らない)。x 温度・y コンダクタンス・zw 熱容量の上限の下位・上位

uint4 PackView(CvCellView view) {
    return uint4(view.thermal.temperature, view.thermal.conductance, (uint32_t)view.thermal.capacityLimit,
                 (uint32_t)(view.thermal.capacityLimit >> 32));
}

CvCellView UnpackView(uint4 packed) {
    CvCellView view = (CvCellView)0;
    view.thermal.temperature = packed.x;
    view.thermal.conductance = packed.y;
    view.thermal.capacityLimit = FX_U64(packed.w, packed.z);

    return view;
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

// 面の先(CPU の ConductionWorld::Neighbor の前半): 木の隣か、世界の外で開いた端なら MR_NEIGHBOR_EDGE(index = 外の柱の行。T-0773)
MrFaceNeighbor FindConductNeighbor(uint32_t slot, MrBlock block, uint32_t index, uint32_t face) {
    const MrFaceNeighbor neighbor = MrFindFaceNeighbor(MakeTree(), slot, block, index, face, g_rootLevel);
    if (neighbor.kind != MR_NEIGHBOR_NONE)
        return neighbor;

    return WeEdgeNeighbor(GpuWorldEdges(), GpuColumnBaseUp(), GpuColumnRows(), block, index, face);
}

// 刻むセルの見立てを gs_thermals に、刻むセルの外を向く面の隣とその見立てを gs_halo* に書く(グループの全部のスレッドが呼び、
// 後で GroupMemoryBarrierWithGroupSync)。外の隣の見立ては流れのある隣(同じレベル・粗い・開いた端の外のセル)だけ。
// 段の間はセルも木も変わらないので、面ごとに作るのと同じ値
void CacheBlockFaces(uint32_t slot, MrBlock block, uint32_t thread) {
    const uint32_t parts = ConductViewParts();
    for (uint32_t k = 0; k < CONDUCT_CELLS_PER_THREAD; ++k) {
        const uint32_t index = thread + (CONDUCT_THREADS * k);
        if (MrIsSteppedCell(block, index))
            gs_thermals[index] = PackView(BlockCellView(block, slot, index, parts));
    }

    for (uint32_t entry = thread; entry < CONDUCT_HALO_CELLS; entry += CONDUCT_THREADS) {
        const uint32_t index = HaloCell(entry);
        if (!MrIsSteppedCell(block, index))
            continue;

        const uint32_t face = entry / CONDUCT_FACE_CELLS;
        const MrFaceNeighbor neighbor = FindConductNeighbor(slot, block, index, face);
        gs_haloNeighbors[entry] = uint4(neighbor.kind, neighbor.slot, neighbor.index, neighbor.gap);
        if (neighbor.kind == MR_NEIGHBOR_SAME || neighbor.kind == MR_NEIGHBOR_COARSER)
            gs_haloThermals[entry] = PackView(
                BlockCellView(g_blocks[neighbor.slot], neighbor.slot, neighbor.index, parts));
        else if (neighbor.kind == MR_NEIGHBOR_EDGE)
            gs_haloThermals[entry] = PackView(OutsideView(neighbor.index, block.level, parts));
    }
}

// --- 伝導の段の世界(transport_apply.hlsli の World の約束。CPU は multires_conduction.cpp の ConductionWorld)----------------
// 刻むセル 1 つ(self)ごとに作る。見立ては CacheBlockFaces の写しから読む: 自分の枠のセルは gs_thermals、ブロックの外は自分のセルの
// 外を向く面の写し(当てる段は面の番号を渡さないので、面の先のセルが合う写しを探す。同じセルなら同じ値)。
// filters: 流れの回は、凍らせたブロックとの面と、陰解法で解くブロックとの同じレベルの面(こちらのセルが境のセルとして陰解法の系で受ける。
// T-0178)を流さない。印の回は流す(CPU の印の回の世界は凍らせ・陰解法の印を持たない)。変化は 64bit の atomic の足し算(順によらない)
struct GpuConductionWorld {
    uint32_t selfSlot;
    uint32_t selfIndex;
    bool filters;

    MrFaceNeighbor Neighbor(uint32_t slot, MrBlock block, uint32_t index, uint32_t face) {
        MrFaceNeighbor neighbor;
        if (FaceLeavesBlock(index, face)) {
            const uint4 packed = gs_haloNeighbors[HaloEntry(index, face)];
            neighbor = MrMakeFaceNeighbor(packed.x, packed.y, packed.z, packed.w);
        } else {
            neighbor = MrFindFaceNeighbor(MakeTree(), slot, block, index, face, g_rootLevel);
        }

        if (!filters || (neighbor.kind != MR_NEIGHBOR_SAME && neighbor.kind != MR_NEIGHBOR_COARSER))
            return neighbor;

        const MrFaceNeighbor none = MrMakeFaceNeighbor(MR_NEIGHBOR_NONE, MR_NO_BLOCK, 0, 0);
        if (IsFrozenSlot(neighbor.slot))
            return none;

        const bool joinsImplicit = slot < g_worldBlocks && block.kind == MR_BLOCK_REAL;
        if (neighbor.kind == MR_NEIGHBOR_SAME && joinsImplicit &&
            InImplicitConduction(neighbor.slot, g_blocks[neighbor.slot]))
            return none;

        return neighbor;
    }

    CvCellView View(uint32_t slot, uint32_t index) {
        if (slot == selfSlot)
            return UnpackView(gs_thermals[index]);

        uint4 packed = uint4(0, 0, 0, 0);
        for (uint32_t face = 0; face < MR_FACES; ++face) {
            const uint32_t entry = HaloEntry(selfIndex, face);
            const uint4 neighbor = gs_haloNeighbors[entry];
            if (FaceLeavesBlock(selfIndex, face) && neighbor.x != MR_NEIGHBOR_EDGE && neighbor.y == slot &&
                neighbor.z == index)
                packed = gs_haloThermals[entry];
        }

        return UnpackView(packed);
    }

    // 開いた世界の端の外のセル(外の柱の行 row。level は自分のレベル = 写しを作った時と同じ)
    CvCellView Outside(uint32_t row, int32_t level) {
        uint4 packed = uint4(0, 0, 0, 0);
        for (uint32_t face = 0; face < MR_FACES; ++face) {
            const uint32_t entry = HaloEntry(selfIndex, face);
            const uint4 neighbor = gs_haloNeighbors[entry];
            if (FaceLeavesBlock(selfIndex, face) && neighbor.x == MR_NEIGHBOR_EDGE && neighbor.z == row)
                packed = gs_haloThermals[entry];
        }

        return UnpackView(packed);
    }

    int32_t Level(uint32_t slot) { return g_blocks[slot].level; }
    bool HasFraction(uint32_t slot) { return g_blocks[slot].fraction != MR_NO_FRACTION; }

    // 伝導は上限の段を使わない(C/8 の安定の条件で行き過ぎない)・欄を持たない
    TxLimit Limit(uint32_t slot, uint32_t index) { return TxNoLimit(); }
    void FollowFields(uint32_t slot, uint32_t index) {}

    // 当てる: 頁のセルの変化に足す(粗い側も印の段で頁に広げてある)
    void Add(uint32_t slot, uint32_t index, MrEnergyDelta delta) {
        const MrBlock block = g_blocks[slot];
        FX_ASSERT(!MrIsUniform(block));
        AddConductDelta(block.page, index, delta);
    }

    // 5 帳簿: 開いた端の面の流れ outflow(出ていくのが正。枠 slot のレベルの単位)を世界の帳簿の「世界の端」のエネルギーの列へ
    // (CPU の ConductionWorld::AddEdge。T-0762 の帳簿の書き方の約束・繰り上げは AddWorldLedgerCarrying)
    void AddEdge(uint32_t slot, uint32_t index, int64_t outflow) {
        if (outflow == 0)
            return;

        const WlSignedEntry entry = WlEnergyEntry(outflow, WL_DIRECTION_OUT);
        AddWorldLedgerUnits(WlSlot(WL_REASON_WORLD_EDGE, entry.direction), g_blocks[slot].level, WL_COLUMN_ENERGY,
                            entry.magnitude);
    }
};

GpuConductionWorld MakeConductWorld(uint32_t slot, uint32_t index, bool filters) {
    GpuConductionWorld world;
    world.selfSlot = slot;
    world.selfIndex = index;
    world.filters = filters;

    return world;
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

// セル 1 つの面を調べて印を付ける。流れがあれば true(CPU の MarkCellWants。面の流れは当てる段の TxFaceFlow。
// 開いた世界の端の面も自分だけが変わる = 同じレベルの面と同じ数え方。T-0773)
bool MarkCell(uint32_t slot, MrBlock block, uint32_t index) {
    const TxConduction kind = MakeConductionKind();
    GpuConductionWorld world = MakeConductWorld(slot, index, false);
    const CvCellView self = world.View(slot, index);
    int64_t sameLevelOutflow = 0;
    bool sends = false;
    for (uint32_t face = 0; face < MR_FACES; ++face) {
        const MrFaceNeighbor neighbor = world.Neighbor(slot, block, index, face);
        if (neighbor.kind != MR_NEIGHBOR_SAME && neighbor.kind != MR_NEIGHBOR_COARSER &&
            neighbor.kind != MR_NEIGHBOR_EDGE)
            continue;

        const int64_t flow = TxFaceFlow(kind, world, self, block.level, neighbor);
        if (neighbor.kind == MR_NEIGHBOR_SAME || neighbor.kind == MR_NEIGHBOR_EDGE) {
            sameLevelOutflow += flow;
            continue;
        }

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
// (溢れを使う変種は 9 種目の生成物を待たせたセルも「変わる」。UNIFORM_EXPANDS_HELD。T-0276)
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
        changes = changes || MrUniformStepExpands(step, value, UNIFORM_EXPANDS_HELD);
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

#ifdef MR_WIDE_CELLS
    // 前に使った頁の溢れを読まない(一様の値は溢れを持たない。T-0211)
    if (expanded)
        ClearWideSideWords(block.page, thread, CONDUCT_LIGHT_THREADS);
#endif
}

// --- 5. 面の流れ ---------------------------------------------------------------------------------

// 刻むブロックのセルの面の流れを、運ぶ計算の共通の土台の当てる段(common/transport_apply.hlsli の TxFlowCell。T-0766・ADR-0334)で
// 変化に足す(自分と、粗い側へ送った先。開いた世界の端の面は帳簿の「世界の端」へ。T-0773)。CPU の ComputeConduction と同じ関数
void ConductFlowsBlock(uint32_t slot, uint32_t thread) {
    const MrBlock block = g_blocks[slot];
    if (!IsConductStepped(slot, block) || !SubstepBegins(block.level) || MrIsUniform(block) || IsFrozenSlot(slot) ||
        InImplicitConduction(slot, block))
        return;

    CacheBlockFaces(slot, block, thread);
    GroupMemoryBarrierWithGroupSync();
    const TxConduction kind = MakeConductionKind();
    for (uint32_t k = 0; k < CONDUCT_CELLS_PER_THREAD; ++k) {
        const uint32_t index = thread + (CONDUCT_THREADS * k);
        if (MrIsSteppedCell(block, index))
            TxFlowCell(kind, MakeConductWorld(slot, index, true), slot, block, index);
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

#ifdef MR_WIDE_CELLS
// --- 溢れを使う変種(T-0211。T-0272 で反応を後の段へ回した)---
// 溢れを持つセル・9 種目を待たせたセルは、この段では伝導の変化を足したインラインだけ書いて印を付け、反応(StepPagedWaitWide と同じ ①②③)は
// 足す段の後の WideDeferredPass(1 グループ 64 スレッド = 1 枠。multires_wide_step.hlsli の WideDeferredBlock)が上限の無い形で刻む。
// 512 スレッドのこの段に上限の無い形のセルを持たせない(上限 56 以上で HW の compute キューが止まった。T-0290・ADR-0052 追記 5)

// 上限の無い形に回すセル: 伝導の変化を足したインラインを書き(反応はまだ)、印を付ける。変わったら true
bool DeferWideCell(MrBlock block, uint32_t index, RxCell cell, RxCell before, bool fractionChanged) {
    g_cells[PageCellAddress(block.page, index)] = cell;
    InterlockedOr(gs_wideMarks[index / 32], 1u << (index % 32));
    gs_wideAny = 1;

    return MrCellChanged(before, cell) || fractionChanged;
}

// 印を付けたセルがあれば、印と刻みの初めの busyTick を見出しに残して WideDeferredPass に回す(グループの全部のスレッドが呼ぶ)。
// 見出しの busyTick・wakeTick は印のセルを除いてこの段が書き、WideDeferredPass が印のセルの分を合わせる(WideRetryBlock と同じ)
void DeferConductWideCells(MrBlock block, uint32_t thread) {
    GroupMemoryBarrierWithGroupSync();
    if (gs_wideAny == 0)
        return;

    RequestWideDeferred(block.page, block.busyTick, thread, CONDUCT_THREADS);
}
#endif

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
#ifdef MR_WIDE_CELLS
        // 溢れを持つセルは変化だけ書き、反応は上限の無い形で後の段が刻む(DeferConductWideCells。T-0211・T-0272)
        if (WideHasTail(cell, block.page, WideCurrentSide(block.page), index))
            return DeferWideCell(block, index, cell, before, fraction.energy != fractionBefore);
#endif
        const RxWaitStep step = MrStepCellWait(MakeTable(), cell, seed, tick, block, index);
#ifdef MR_WIDE_CELLS
        // 9 種目の生成物を待たせたセルも(8 種以下の間はインラインの核と上限の無い形がビット単位で同じ。T-0175)
        if ((step.limits & RX_LIMIT_PRODUCTS) != 0)
            return DeferWideCell(block, index, cell, before, fraction.energy != fractionBefore);
#endif
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

#ifdef MR_WIDE_CELLS
    BeginWideCells(thread, CONDUCT_THREADS);
#endif

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

#ifdef MR_WIDE_CELLS
    DeferConductWideCells(block, thread);
#endif

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
