// object_shell_flow.hlsl — 物の箱が囲む空気の層の流れ(D-667 の 2 段目・D-673・T-1143)を GPU で 1 刻み回す研究の形(T-1200)。
// 式は shaders/common/object_heat.hlsli の「層の流れ」(OhFlowCellOf・OhFlowSpeed・OhFlowBuoyancy・OhFlowMoved ほか)で、
// CPU リファレンス tests/object_flow_cpu.h(StepShellFlow)とビット単位で同じ。比べと費用は tests/gpu_object_shell_flow_test.cpp。
// 形: 物 1 つ = 1 グループ。1 刻みの流れの小刻み × 〔見立て → 面 → 運動量を運ぶ・浮力 → 残り → 圧力の赤黒 × 回数 → 圧力の力積 → 出る割合 → 運ぶ・当てる → セルへ〕を
// グループの中の同期だけで回す(Dispatch とバリアは 1 刻みに 1 回)。散らす書き方(CPU の面 → 両側)は集める書き方(ボクセル・面ごとに自分の分だけ)にした。
// 整数の足し算なので順によらず同じ値になる。圧力の値は、箱が OH_SHELL_SHARED_VOXELS 以下なら groupshared に 32bit で(溢れたら g_stats[1] に数える)、
// それより大きい箱は作業場(既定のヒープ)に 64bit で置く(g_sharedPotential)。
// 圧力の反復(T-1201。g_pressureMode): 前の小刻みの圧力の値から解き始め(作業場の potential が層の状態として刻みをまたいで残る)、
// 1 回の赤黒で値が 1 つも変わらなければ止める(上限 g_pressureSweeps)。止まるかはグループの groupshared の印で決めるので、全スレッドが同じ回数で抜ける。
// 1 つのセルを囲む物が 2 つ以上ある時はセルへの書き込みが競るので、この形は物ごとに別のセル(試験の場面)だけ。エンジンへ入れる形は T-1200 のチケット。
#include "common/object_heat.hlsli"

// --- 作業場(ボクセルごと・面ごと。真実ではない)---
// 圧力の反復が毎回読む欄(残り・逆数・6 つの面の重み)は後ろに続けて置く(1 ボクセル 1 回の続いた読み。面の並びを 6 回読まない。T-1223)
struct OhShellVoxel {
    OhFlowCell cell;    // 小刻みの初めの見立て
    int64_t potential;  // 圧力の値(groupshared に載らない時)
    uint64_t total;     // 6 つの面の重みの和
    uint64_t outShare;  // 出る割合の和(Q16)
    uint32_t changed;   // この刻みに流れで変わった
    uint32_t padding;

    // --- 圧力の反復が読む欄 ---
    int64_t residual;  // 膨らみたい体積 − 予測の出る体積(Q16)
    FxRecip64 recip;   // 重みの和の逆数(圧力の反復の割り算を掛け算に。商は OhMulDivToward0 とビット単位で同じ。04 §6)
    // 隣のボクセルとの面の重み(− x・+ x・− y・+ y・− z・+ z。箱の外に面する面は 0 = 囲むセルの圧力 0 の項。和は total に入っている)
    uint64_t weightMinusX;
    uint64_t weightPlusX;
    uint64_t weightMinusY;
    uint64_t weightPlusY;
    uint64_t weightMinusZ;
    uint64_t weightPlusZ;
};

struct OhShellFace {
    uint64_t mass;    // 面の質量(両側の平均)
    int64_t speed;    // 速さ(小刻みの初め → 圧力の後)
    int64_t next;     // 運んだ・浮力の後 → 圧力の後の運動量
    uint64_t weight;  // 圧力の重み β
    uint32_t wall;    // 骨格に面する壁(D-673)
    uint32_t padding;
};

// 物ごとの面の並びの始まり(軸 0・1・2 の順に続く。g_momentum と g_faceWork で同じ)
struct OhShellObject {
    uint32_t faceBegin[3];
    uint32_t multigridBegin;  // 圧力の多重格子の作業場 g_multigrid の始まり(全部の段を続けて。T-1222)
};

// 圧力の多重格子の段のボクセル(作業場。段 0 = 細かい格子・段 1.. = 粗い格子。T-1222)
struct OhShellMultigrid {
    // 6 つの面の重み(− x・+ x・− y・+ y・− z・+ z。箱の外に面する面も入る = 値 0 の項。和は total)
    uint64_t weightMinusX;
    uint64_t weightPlusX;
    uint64_t weightMinusY;
    uint64_t weightPlusY;
    uint64_t weightMinusZ;
    uint64_t weightPlusZ;
    uint64_t total;
    FxRecip64 recip;  // 重みの和の逆数
    int64_t rhs;      // 右辺(残り × 2^16 × 2^8 の単位。粗い段は細かい段の残りの和)
    int64_t value;    // 値(細かい単位)
    int64_t scratch;  // 残り(粗い段を作る時だけ)
};

RWStructuredBuffer<RxCell> g_voxels : register(u0);
RWStructuredBuffer<RxCell> g_cells : register(u1);
RWStructuredBuffer<OhFlow> g_carries : register(u2);
RWStructuredBuffer<int64_t> g_momentum : register(u3);  // 面の運動量(真実。層を作った時 0)
RWStructuredBuffer<OhVoxelWait> g_waits : register(u4);
RWStructuredBuffer<OhBlockState> g_blockStates : register(u5);
RWStructuredBuffer<uint32_t> g_cellChanged : register(u6);
// [0] 量が負・枠があふれた [1] 圧力の値が 32bit に入らなかった [2 + 2o] 出る上限で縮めた数(物 o)[3 + 2o] 一番速い面
RWStructuredBuffer<uint32_t> g_stats : register(u7);
RWStructuredBuffer<OhShellVoxel> g_voxelWork : register(u8);
RWStructuredBuffer<OhVoxelGas> g_gasWork : register(u9);
RWStructuredBuffer<OhShellFace> g_faceWork : register(u10);
RWStructuredBuffer<OhShellMultigrid> g_multigrid : register(u11);
StructuredBuffer<RxSpecies> g_species : register(t0);
StructuredBuffer<OhSpecies> g_objectSpecies : register(t1);
StructuredBuffer<uint32_t> g_molarMasses : register(t2);
StructuredBuffer<uint32_t> g_gasSpecies : register(t3);
StructuredBuffer<OhObject> g_objects : register(t4);
StructuredBuffer<OhVoxelInfo> g_infos : register(t5);
StructuredBuffer<OhShellObject> g_shellObjects : register(t6);
StructuredBuffer<OhBlock> g_blocks : register(t7);

cbuffer RootConstants : register(b0) {
    uint32_t g_objectCount;
    uint32_t g_tickLow;
    uint32_t g_tickHigh;
    uint32_t
        g_passBase;  // 刻みの前の段の番号(物 o の段 = g_passBase + o + 1。CPU が物の順に StepShellFlow を呼ぶのと同じ)
    uint32_t g_substeps;
    uint32_t g_pressureSweeps;
    uint32_t g_skeletonWalls;
    uint32_t g_sharedPotential;  // 1 = 圧力の値を groupshared に(32bit)
    uint32_t g_pressureMode;  // OH_PRESSURE_UNTIL_QUIET・OH_PRESSURE_WARM_START(T-1201。0 = T-1200 までの決まった回数)
    uint32_t g_stageMask;     // 回す段の印(OH_STAGE_*。既定は全部。段を外すのは費用を段ごとに分けて測る時だけ。T-1223)
};

// グループのスレッドの数(既定 1,024。費用の比べのために 256・512 の形も作る: shaders/CMakeLists.txt。T-1223)
#ifndef OH_SHELL_GROUP_THREADS
#define OH_SHELL_GROUP_THREADS 1024
#endif

FX_CONST uint32_t OH_SHELL_THREADS = OH_SHELL_GROUP_THREADS;
FX_CONST uint32_t OH_SHELL_SHARED_VOXELS = 7168;  // 28 KB(groupshared の上限 32 KB のうち)
FX_CONST uint32_t OH_SHELL_OUTSIDE = 0xFFFFFFFFu;
FX_CONST uint32_t OH_PRESSURE_UNTIL_QUIET = 1;  // 1 回の赤黒で値が 1 つも変わらなければ止める
FX_CONST uint32_t OH_PRESSURE_WARM_START = 2;   // 前の小刻みの圧力の値から解き始める
FX_CONST uint32_t OH_PRESSURE_MULTIGRID = 4;    // 解き始めを多重格子で作る(T-1222。CPU の pressureMultigrid)
// 段の印(g_stageMask。測るだけ)
FX_CONST uint32_t OH_STAGE_VOXELS = 1u << 0;    // ① ボクセルの見立て
FX_CONST uint32_t OH_STAGE_FACES = 1u << 1;     // ② 面の質量・速さ・重み
FX_CONST uint32_t OH_STAGE_ADVECT = 1u << 2;    // ③ 運動量を運ぶ・浮力
FX_CONST uint32_t OH_STAGE_RESIDUAL = 1u << 3;  // ④ 残り・重みの和
FX_CONST uint32_t OH_STAGE_PRESSURE = 1u << 4;  // ⑤ 圧力の赤黒
FX_CONST uint32_t OH_STAGE_PROJECT = 1u << 5;   // ⑥ 圧力の力積
FX_CONST uint32_t OH_STAGE_SHARES = 1u << 6;    // ⑦ 出る割合の和
FX_CONST uint32_t OH_STAGE_MOVE = 1u << 7;      // ⑧ 運ぶ・当てる
FX_CONST uint32_t OH_STAGE_EXCHANGE = 1u << 8;  // ⑨ セルへ
FX_CONST uint32_t OH_STAGE_WAKE = 1u << 9;      // ⑩ 待ちとブロックを起こす
FX_CONST uint32_t
    OH_SHELL_SWEEP_STATS = 4;  // g_stats の後ろ(2 + 2 × 物の数から): 解いた回数・赤黒の和・一番多い・上限で止まった

struct GpuShellTable {
    uint32_t unused;

    RxSpecies Species(uint32_t id) { return g_species[id]; }

    OhSpecies ObjectSpecies(uint32_t id) { return g_objectSpecies[id]; }

    uint32_t MolarMass(uint32_t id) { return g_molarMasses[id]; }
};

struct GpuShellGases {
    uint32_t unused;

    uint32_t GasSpecies(uint32_t gas) { return g_gasSpecies[gas]; }
};

groupshared int32_t gs_potential[OH_SHELL_SHARED_VOXELS];
groupshared uint64_t gs_cellMass;
groupshared uint64_t gs_cellPressureVolume;
groupshared uint32_t gs_cellOpen;
groupshared int32_t gs_cellTemperature;
groupshared uint64_t gs_cellGas[OH_MAX_GASES];
groupshared int64_t gs_toCell[OH_MAX_GASES + 1];  // [0] エネルギー・[1 + g] 気体
groupshared uint32_t gs_limited;
groupshared uint32_t gs_fastest;
groupshared uint32_t gs_failed;
groupshared uint32_t gs_overflow;
groupshared uint32_t
    gs_pressureMoved[3];  // 赤黒の回ごと(回 % 3)に、値が変わったボクセルがあったか(読む回・書く回・0 に戻す回を分ける)
// 収まるまでの赤黒の「汚れた」印(T-1222 2 本目): 色ごとに、その色の並び(PressureColour の k)の 1 bit。
// 前に計算してから隣の値が変わったボクセルだけを計算し直す(変わらない隣からは同じ値になるので、全部回すのとビット単位で同じ。T-1202)。
// 色 c の回は色 c の印を読んで消し、隣(もう一方の色)の印を立てるだけなので、同じ回の中で読む語と立てる語が交わらない
FX_CONST uint32_t OH_SHELL_DIRTY_WORDS = OH_SHELL_SHARED_VOXELS / 32;
groupshared uint32_t gs_dirty[2][OH_SHELL_DIRTY_WORDS];

// --- 箱の並び(軸の選びは決まった select で。動的な添字の局所の配列を作らない)---

struct ShellBox {
    uint32_t begin;  // ボクセルの始まり
    uint3 size;
    uint32_t faceBegin[3];
    uint32_t multigridBegin;  // 圧力の多重格子の作業場の始まり
};

uint32_t Axis(uint3 value, uint32_t axis) {
    return axis == 0 ? value.x : (axis == 1 ? value.y : value.z);
}

uint3 Unit(uint32_t axis) {
    return uint3(axis == 0 ? 1u : 0u, axis == 1 ? 1u : 0u, axis == 2 ? 1u : 0u);
}

uint3 FaceSize(ShellBox box, uint32_t axis) {
    return box.size + Unit(axis);
}

uint32_t FaceCount(ShellBox box, uint32_t axis) {
    const uint3 size = FaceSize(box, axis);

    return size.x * size.y * size.z;
}

uint32_t FaceBegin(ShellBox box, uint32_t axis) {
    return axis == 0 ? box.faceBegin[0] : (axis == 1 ? box.faceBegin[1] : box.faceBegin[2]);
}

// 面の通し番号(全部の物の並び)
uint32_t FaceAt(ShellBox box, uint32_t axis, uint3 c) {
    const uint3 size = FaceSize(box, axis);

    return FaceBegin(box, axis) + c.x + (size.x * (c.y + (size.y * c.z)));
}

uint32_t VoxelLocal(ShellBox box, uint3 c) {
    return c.x + (box.size.x * (c.y + (box.size.y * c.z)));
}

uint3 VoxelCoord(ShellBox box, uint32_t v) {
    return uint3(v % box.size.x, (v / box.size.x) % box.size.y, v / (box.size.x * box.size.y));
}

// 面の両側の箱の中の番号(箱の外 = OH_SHELL_OUTSIDE)
uint32_t MinusSide(ShellBox box, uint32_t axis, uint3 c) {
    return Axis(c, axis) > 0 ? VoxelLocal(box, c - Unit(axis)) : OH_SHELL_OUTSIDE;
}

uint32_t PlusSide(ShellBox box, uint32_t axis, uint3 c) {
    return Axis(c, axis) < Axis(box.size, axis) ? VoxelLocal(box, c) : OH_SHELL_OUTSIDE;
}

// 全部の軸の面を 1 つの並びとして数える(f → 軸と面の座標)
void FaceOf(ShellBox box, uint32_t f, out uint32_t axis, out uint3 c) {
    const uint32_t count0 = FaceCount(box, 0);
    const uint32_t count1 = FaceCount(box, 1);
    axis = f < count0 ? 0u : (f < count0 + count1 ? 1u : 2u);
    const uint32_t local = axis == 0 ? f : (axis == 1 ? f - count0 : f - count0 - count1);
    const uint3 size = FaceSize(box, axis);
    c = uint3(local % size.x, (local / size.x) % size.y, local / (size.x * size.y));
}

uint32_t AllFaces(ShellBox box) {
    return FaceCount(box, 0) + FaceCount(box, 1) + FaceCount(box, 2);
}

// --- 見立ての読み ---

OhFlowCell CellFlow() {
    OhFlowCell flow;
    flow.mass = gs_cellMass;
    flow.pressureVolume = gs_cellPressureVolume;
    flow.openQ16 = gs_cellOpen;
    flow.temperature = gs_cellTemperature;

    return flow;
}

OhFlowCell FlowCellAt(ShellBox box, uint32_t local) {
    if (local == OH_SHELL_OUTSIDE)
        return CellFlow();

    return g_voxelWork[box.begin + local].cell;
}

bool IsSkeleton(ShellBox box, uint32_t local) {
    return local != OH_SHELL_OUTSIDE && (g_infos[box.begin + local].shape & OH_SHELL_BIT) == 0;
}

int64_t PotentialAt(ShellBox box, uint32_t local) {
    if (local == OH_SHELL_OUTSIDE)
        return 0;

    return g_sharedPotential != 0 ? (int64_t)gs_potential[local] : g_voxelWork[box.begin + local].potential;
}

// 64bit の値が同じか(32bit の 2 つで比べる。64bit の引き算と比べを並べない: T-1078)
bool SameInt64(int64_t a, int64_t b) {
    const uint64_t ua = (uint64_t)a;
    const uint64_t ub = (uint64_t)b;

    return (uint32_t)(ua & 0xFFFFFFFFull) == (uint32_t)(ub & 0xFFFFFFFFull) &&
           (uint32_t)(ua >> 32) == (uint32_t)(ub >> 32);
}

// 32bit に入るか(上の 32bit が下の 32bit の符号の広がりと同じ。64bit の引き算と比べを並べない: T-1078)
bool FitsInt32(int64_t value) {
    const uint32_t high = (uint32_t)((uint64_t)value >> 32);
    const uint32_t low = (uint32_t)((uint64_t)value & 0xFFFFFFFFull);

    return high == ((low >> 31) != 0 ? 0xFFFFFFFFu : 0u);
}

// ① 中心 center(軸の座標 0 = 箱の外の手前・1..size = ボクセル・size + 1 = 箱の外の奥)が運ぶ運動量。forward = + 向き
int64_t CenterMoved(ShellBox box, uint32_t axis, uint3 c, uint32_t center, out bool forward) {
    const uint32_t size = Axis(box.size, axis);
    const bool outsideMinus = center == 0;
    const bool outsidePlus = center == size + 1;
    const uint3 unit = Unit(axis);
    // c は中心の座標の軸以外。軸の座標は面の座標 center − 1(手前の面)・center(奥の面)
    const uint3 base = c - (unit * Axis(c, axis));
    const int64_t qMinus = outsideMinus ? 0 : g_faceWork[FaceAt(box, axis, base + (unit * (center - 1)))].speed;
    const int64_t qPlus = outsidePlus ? 0 : g_faceWork[FaceAt(box, axis, base + (unit * center))].speed;
    // 速さは ±65536 に止めてあるので 32bit で足して 2 で割る(0 の向きに切る = CPU の int64 の / 2 と同じ)
    const int32_t sum = (int32_t)qMinus + (int32_t)qPlus;
    const int32_t average = sum / 2;
    const int32_t centerSpeed = average > 32768 ? 32768 : (average < -32768 ? -32768 : average);
    forward = centerSpeed > 0;
    if (centerSpeed == 0)
        return 0;

    uint64_t mass = gs_cellMass;
    if (!outsideMinus && !outsidePlus)
        mass = g_voxelWork[box.begin + VoxelLocal(box, base + (unit * (center - 1)))].cell.mass;

    const uint32_t magnitude = (uint32_t)(centerSpeed < 0 ? -centerSpeed : centerSpeed);
    const int64_t carried = ((int64_t)mass * (int64_t)magnitude) >> 16;
    const int64_t upwindSpeed = forward ? qMinus : qPlus;

    return OhMulDivToward0(upwindSpeed, (uint64_t)carried, 1);
}

// 面 c(軸 axis)に入る運ぶ運動量: 手前の中心(この面が奥の面)と奥の中心(この面が手前の面)から
int64_t AdvectedInto(ShellBox box, uint32_t axis, uint3 c) {
    const uint32_t k = Axis(c, axis);
    bool forward;
    const int64_t below = CenterMoved(box, axis, c, k, forward);
    int64_t total = forward ? below : -below;
    const int64_t above = CenterMoved(box, axis, c, k + 1, forward);
    total += forward ? -above : above;

    return total;
}

// 運ぶ熱の割り算 ÷ 10^9 の速い形(T-1223): 積が 64bit に入れば逆数の掛け算(FxDivRecipU64。厳密)、入らなければ OhMulDivToward0。
// 商はどちらも OhMulDivToward0(moved, perUnit, 10^9)とビット単位で同じ(moved ≥ 0)
uint64_t CarriedEnergy(uint64_t moved, uint64_t perUnitMagnitude, FxRecip64 billion) {
    const FxU128 product = FxMulU64Full(moved, perUnitMagnitude);
    if (product.hi == 0)
        return FxDivRecipU64(product.lo, billion);

    return (uint64_t)OhMulDivToward0((int64_t)moved, perUnitMagnitude, 1000000000);
}

// 風上の気体を割合 share だけ移す流れ(共有の OhFlowMoved と同じ式・同じ値。÷ 10^9 だけ CarriedEnergy。違えば比べの試験が落ちる)
OhFlow ShellFlowMoved(GpuShellTable table, GpuShellGases gases, OhVoxelGas upwind, int32_t upwindMilliKelvin,
                      uint64_t shareQ16, FxRecip64 billion) {
    OhFlow flow = OhZeroFlow();
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
        const uint32_t species = gases.GasSpecies(g);
        if (species == 0)
            continue;

        const int64_t moved = (int64_t)((upwind.amounts[g] * shareQ16) >> 16);
        if (moved == 0)
            continue;

        const RxSpecies values = table.Species(species);
        const int64_t perUnit = (values.h0 * 1000000) + ((int64_t)values.heatCapacity * (int64_t)upwindMilliKelvin);
        const int64_t carried = (int64_t)CarriedEnergy((uint64_t)moved, FxAbsU64(perUnit), billion);
        flow.amounts[g] = moved;
        flow.energy += perUnit < 0 ? -carried : carried;
    }

    return flow;
}

// 面 c を通って箱の中のボクセル u(または箱の外)から出る流れ(q ≠ 0)
OhFlow MovedThrough(GpuShellTable table, GpuShellGases gases, ShellBox box, int64_t q, uint32_t upwind,
                    FxRecip64 billion) {
    if (upwind == OH_SHELL_OUTSIDE) {
        OhVoxelGas cellGas;
        FX_UNROLL
        for (uint32_t g = 0; g < OH_MAX_GASES; ++g)
            cellGas.amounts[g] = gs_cellGas[g];

        return ShellFlowMoved(table, gases, cellGas, gs_cellTemperature, OhFlowShareQ16(q, CellFlow()), billion);
    }

    const OhShellVoxel work = g_voxelWork[box.begin + upwind];
    uint64_t share = OhFlowShareQ16(q, work.cell);
    if (work.outShare > OH_FLOW_SHARE_LIMIT_Q16)
        share = OhDivideU64(share * OH_FLOW_SHARE_LIMIT_Q16, work.outShare);

    return ShellFlowMoved(table, gases, g_gasWork[box.begin + upwind], work.cell.temperature, share, billion);
}

OhFlow SubtractFlow(OhFlow a, OhFlow b) {
    a.energy -= b.energy;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g)
        a.amounts[g] -= b.amounts[g];

    return a;
}

// --- 段 ---

// ⓪ 囲むセルの見立て(1 スレッド)
void PrepareCell(GpuShellTable table, GpuShellGases gases, uint32_t cellIndex) {
    const RxCell air = g_cells[cellIndex];
    const OhFlowCell flow = OhFlowCellOf(table, air);
    const OhVoxelGas gas = OhPrepareGas(gases, air);
    gs_cellMass = flow.mass;
    gs_cellPressureVolume = flow.pressureVolume;
    gs_cellOpen = flow.openQ16;
    gs_cellTemperature = flow.temperature;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g)
        gs_cellGas[g] = gas.amounts[g];

    FX_UNROLL
    for (uint32_t k = 0; k < OH_MAX_GASES + 1; ++k)
        gs_toCell[k] = 0;
}

// ① ボクセルの見立て・膨らみたい体積
void PrepareVoxels(GpuShellTable table, GpuShellGases gases, ShellBox box, uint32_t thread) {
    const OhFlowCell cell = CellFlow();
    const uint32_t count = box.size.x * box.size.y * box.size.z;
    for (uint32_t v = thread; v < count; v += OH_SHELL_THREADS) {
        const RxCell voxel = g_voxels[box.begin + v];
        const OhFlowCell flow = OhFlowCellOf(table, voxel);
        g_voxelWork[box.begin + v].cell = flow;
        g_voxelWork[box.begin + v].residual = OhFlowExcessQ16(flow, cell);
        g_gasWork[box.begin + v] = OhPrepareGas(gases, voxel);
    }
}

// ② 面の質量・壁・速さ・重み
void PrepareFaces(ShellBox box, uint32_t thread) {
    const uint64_t referenceMass = gs_cellMass;
    const uint32_t faces = AllFaces(box);
    for (uint32_t f = thread; f < faces; f += OH_SHELL_THREADS) {
        uint32_t axis;
        uint3 c;
        FaceOf(box, f, axis, c);
        const uint32_t minus = MinusSide(box, axis, c);
        const uint32_t plus = PlusSide(box, axis, c);
        const uint32_t global = FaceAt(box, axis, c);
        OhShellFace face;
        face.mass = OhFlowFaceMass(FlowCellAt(box, minus), FlowCellAt(box, plus));
        face.wall = g_skeletonWalls != 0 && (IsSkeleton(box, minus) || IsSkeleton(box, plus)) ? 1u : 0u;
        if (face.wall != 0)
            g_momentum[global] = 0;

        face.speed = OhFlowSpeed(g_momentum[global], face.mass);
        face.next = 0;
        face.weight = OhFlowFaceWeight(referenceMass, face.mass);
        face.padding = 0;
        g_faceWork[global] = face;
    }
}

// ③ 運動量を運ぶ(D-663)・縦の面に浮力・壁は 0
void AdvectFaces(ShellBox box, uint32_t thread, uint64_t gravity) {
    const uint32_t faces = AllFaces(box);
    for (uint32_t f = thread; f < faces; f += OH_SHELL_THREADS) {
        uint32_t axis;
        uint3 c;
        FaceOf(box, f, axis, c);
        const uint32_t global = FaceAt(box, axis, c);
        int64_t next = g_momentum[global] + AdvectedInto(box, axis, c);
        if (axis == 2)
            next += OhFlowBuoyancy(gravity, FlowCellAt(box, MinusSide(box, axis, c)),
                                   FlowCellAt(box, PlusSide(box, axis, c)), CellFlow());

        g_faceWork[global].next = g_faceWork[global].wall != 0 ? 0 : next;
    }
}

// ④ 残り = 膨らみたい体積 − 予測の出る体積・重みの和
void PrepareResidual(ShellBox box, uint32_t thread) {
    const uint32_t count = box.size.x * box.size.y * box.size.z;
    for (uint32_t v = thread; v < count; v += OH_SHELL_THREADS) {
        const uint3 c = VoxelCoord(box, v);
        int64_t residual = g_voxelWork[box.begin + v].residual;
        uint64_t total = 0;
        uint64_t inner[6];  // 隣のボクセルとの面の重み(決まった添字だけで触る)
        FX_UNROLL
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const OhShellFace minusFace = g_faceWork[FaceAt(box, axis, c)];
            const OhShellFace plusFace = g_faceWork[FaceAt(box, axis, c + Unit(axis))];
            residual += OhFlowSpeed(minusFace.next, minusFace.mass);
            residual -= OhFlowSpeed(plusFace.next, plusFace.mass);
            total += minusFace.weight + plusFace.weight;
            inner[2 * axis] = Axis(c, axis) > 0 ? minusFace.weight : (uint64_t)0;
            inner[(2 * axis) + 1] = Axis(c, axis) + 1 < Axis(box.size, axis) ? plusFace.weight : (uint64_t)0;
        }

        g_voxelWork[box.begin + v].weightMinusX = inner[0];
        g_voxelWork[box.begin + v].weightPlusX = inner[1];
        g_voxelWork[box.begin + v].weightMinusY = inner[2];
        g_voxelWork[box.begin + v].weightPlusY = inner[3];
        g_voxelWork[box.begin + v].weightMinusZ = inner[4];
        g_voxelWork[box.begin + v].weightPlusZ = inner[5];
        g_voxelWork[box.begin + v].residual = residual;
        g_voxelWork[box.begin + v].total = total;
        g_voxelWork[box.begin + v].recip = FxMakeRecipU64(total > 0 ? total : (uint64_t)1);
        if ((g_pressureMode & OH_PRESSURE_WARM_START) != 0)
            continue;

        g_voxelWork[box.begin + v].potential = 0;
        if (g_sharedPotential != 0)
            gs_potential[v] = 0;
    }
}

// 色の並びの数(行ごとに x を 2 つおき。色 0 と色 1 で同じ数え方)
uint32_t PressureSlots(ShellBox box) {
    return ((box.size.x + 1) >> 1) * box.size.y * box.size.z;
}

// ボクセル (x, y, z) の色の並びの番号(どちらの色でも x ÷ 2 + 行 × 半分の幅)
uint32_t PressureSlotOf(ShellBox box, uint32_t x, uint32_t y, uint32_t z) {
    return (x >> 1) + (((box.size.x + 1) >> 1) * (y + (box.size.y * z)));
}

void MarkDirty(uint32_t colour, uint32_t slot) {
    InterlockedOr(gs_dirty[colour][slot >> 5], 1u << (slot & 31u));
}

// 値が変わったボクセルの隣(もう一方の色)に印を立てる
void MarkNeighboursDirty(ShellBox box, uint32_t x, uint32_t y, uint32_t z, uint32_t other) {
    if (x > 0)
        MarkDirty(other, PressureSlotOf(box, x - 1, y, z));
    if (x + 1 < box.size.x)
        MarkDirty(other, PressureSlotOf(box, x + 1, y, z));
    if (y > 0)
        MarkDirty(other, PressureSlotOf(box, x, y - 1, z));
    if (y + 1 < box.size.y)
        MarkDirty(other, PressureSlotOf(box, x, y + 1, z));
    if (z > 0)
        MarkDirty(other, PressureSlotOf(box, x, y, z - 1));
    if (z + 1 < box.size.z)
        MarkDirty(other, PressureSlotOf(box, x, y, z + 1));
}

// ⑤ 圧力の赤黒の半分(colour の色のボクセル)。値が変わったら gs_pressureMoved[slot] に印。
// その色のボクセルだけを回す(行ごとに x を 2 つおき。T-1223)。重みが 0 の項(箱の外に面する面)は足しても同じ値。
// dirtyOnly なら印の立ったボクセルだけ(印を消して計算し、値が変わったら隣の印を立てる。T-1222 2 本目)
void PressureColour(ShellBox box, uint32_t thread, uint32_t colour, uint32_t slot, bool dirtyOnly) {
    const uint32_t halfX = (box.size.x + 1) >> 1;
    const uint32_t slots = halfX * box.size.y * box.size.z;
    const uint32_t strideY = box.size.x;
    const uint32_t strideZ = box.size.x * box.size.y;
    for (uint32_t k = thread; k < slots; k += OH_SHELL_THREADS) {
        const uint32_t row = k / halfX;
        const uint32_t y = row % box.size.y;
        const uint32_t z = row / box.size.y;
        const uint32_t x = ((k - (row * halfX)) << 1) + ((y + z + colour) & 1u);
        if (x >= box.size.x)
            continue;

        if (dirtyOnly) {
            const uint32_t bit = 1u << (k & 31u);
            if ((gs_dirty[colour][k >> 5] & bit) == 0)
                continue;

            InterlockedAnd(gs_dirty[colour][k >> 5], ~bit);
        }

        const uint32_t v = x + (strideY * y) + (strideZ * z);
        const OhShellVoxel work = g_voxelWork[box.begin + v];
        int64_t sum = work.residual * 65536;
        sum += (int64_t)work.weightMinusX * PotentialAt(box, x > 0 ? v - 1 : OH_SHELL_OUTSIDE);
        sum += (int64_t)work.weightPlusX * PotentialAt(box, x + 1 < box.size.x ? v + 1 : OH_SHELL_OUTSIDE);
        sum += (int64_t)work.weightMinusY * PotentialAt(box, y > 0 ? v - strideY : OH_SHELL_OUTSIDE);
        sum += (int64_t)work.weightPlusY * PotentialAt(box, y + 1 < box.size.y ? v + strideY : OH_SHELL_OUTSIDE);
        sum += (int64_t)work.weightMinusZ * PotentialAt(box, z > 0 ? v - strideZ : OH_SHELL_OUTSIDE);
        sum += (int64_t)work.weightPlusZ * PotentialAt(box, z + 1 < box.size.z ? v + strideZ : OH_SHELL_OUTSIDE);

        // = OhMulDivToward0(sum, 1, 重みの和)(0 の向きに切る。逆数の掛け算は厳密)
        const int64_t quotient = (int64_t)FxDivRecipU64(FxAbsU64(sum), work.recip);
        const int64_t potential = sum < 0 ? -quotient : quotient;
        bool changed = false;
        if (g_sharedPotential == 0) {
            changed = !SameInt64(potential, work.potential);
            g_voxelWork[box.begin + v].potential = potential;
        } else {
            if (!FitsInt32(potential))
                gs_overflow = 1;

            const int32_t value = (int32_t)potential;
            changed = value != gs_potential[v];
            gs_potential[v] = value;
        }

        if (!changed)
            continue;

        gs_pressureMoved[slot] = 1;
        if (dirtyOnly)
            MarkNeighboursDirty(box, x, y, z, 1 - colour);
    }
}

// --- 圧力の多重格子の解き始め(T-1222。CPU リファレンス tests/object_flow_cpu.h の PressureMultigrid と同じ規則・ビット単位で同じ)---
// ① 細かい単位(Ψ × 2^8)で V 字 1 回 → ② 動かない点の残りの幅の真ん中へ寄せて V 字をもう 1 回 → ③ Ψ = 四捨五入(値 ÷ 2^8)。
// その後は今までと同じ赤黒を収まるまで(止める判定・上限も同じ)。段はすべて作業場 g_multigrid(物ごとに multigridBegin から、段 0 = 細かい格子・段 1.. = 粗い格子を続けて)。
// 粗い段: 2 × 2 × 2 をまとめる(奇数の端は 1 つ)。一番粗い段はどの向きも 2 以下(赤黒 16 回)。段の大きさは箱の大きさから毎回数える(GetDimensions を使わない)。
// 赤黒・残り・まとめる・戻すはグループの同期だけで回す(再帰を使わず、下りの回し → 一番粗い段 → 上りの回し)。
// 2 本目: 段 0 の値は小さい箱なら groupshared に借りる(MgFineShared)・一番粗い段は 1 波で(MgCoarsestWave)・粗い段の重みは V 字 1 回目だけ作る。
// T-1285: V 字の中の同期はグループの範囲(MgSync)。小さい段を全部 1 波で回す形・段 0 の丸写しをやめる形は測って採らなかった(runner/t1285/)。

FX_CONST uint32_t OH_MG_FINE_BITS = 8;           // ① ② の細かい単位(Ψ × 2^8)
FX_CONST int64_t OH_MG_FINE_SCALE = 16777216;    // 残り(Q16)を細かい単位の右辺に: 65536 × 2^8
FX_CONST uint32_t OH_MG_SMOOTH_SWEEPS = 2;       // 段ごとの前と後の赤黒
FX_CONST uint32_t OH_MG_COARSEST_SWEEPS = 16;    // 一番粗い段の赤黒
FX_CONST uint64_t OH_MG_PROLONG_DIVISOR = 1728;  // 三線形の重み 12 × 12 × 12
FX_CONST uint32_t OH_MG_NONE = 0xFFFFFFFFu;      // 戻しの相手が壁の向こう(値 0)

bool MgCoarsest(uint3 size) {
    return size.x <= 2 && size.y <= 2 && size.z <= 2;
}

// 段の数(一番粗い段まで含む)
uint32_t MgLevelCount(uint3 size) {
    uint32_t count = 1;
    while (!MgCoarsest(size)) {
        size = (size + 1) >> 1;
        ++count;
    }

    return count;
}

// 段 level の大きさと、物の作業場の中の始まり
void MgLevelOf(ShellBox box, uint32_t level, out uint3 size, out uint32_t begin) {
    size = box.size;
    begin = box.multigridBegin;
    for (uint32_t l = 0; l < level; ++l) {
        begin += size.x * size.y * size.z;
        size = (size + 1) >> 1;
    }
}

uint32_t MgIndex(uint3 size, uint3 c) {
    return c.x + (size.x * (c.y + (size.y * c.z)));
}

uint3 MgCoord(uint3 size, uint32_t v) {
    return uint3(v % size.x, (v / size.x) % size.y, v / (size.x * size.y));
}

// 64bit の足し算を 32bit の 2 つで(繰り上がりを手で回す。64bit の引き算と比べを並べない: T-1078・T-1159)
int64_t MgAddSplit(int64_t a, int64_t b) {
    const uint64_t ua = (uint64_t)a;
    const uint64_t ub = (uint64_t)b;
    const uint32_t aLow = (uint32_t)(ua & 0xFFFFFFFFull);
    const uint32_t bLow = (uint32_t)(ub & 0xFFFFFFFFull);
    const uint32_t low = aLow + bLow;
    const uint32_t carry = low < aLow ? 1u : 0u;
    const uint32_t high = (uint32_t)(ua >> 32) + (uint32_t)(ub >> 32) + carry;

    return (int64_t)FX_U64(high, low);
}

// 符号(64bit の上の 32bit の最上位。0 との比べを 64bit で書かない)
bool MgNegative(int64_t value) {
    return ((uint32_t)((uint64_t)value >> 32) >> 31) != 0;
}

// 段 0(細かい格子)の値を groupshared に置く形(T-1222 2 本目): 解き始めの間は gs_potential が使われない
// (多重格子の規則では、解き始めの前の圧力の値は読まれず、MgFinishToPotential が全部書く)ので、64bit の値を下の 32bit は [v]・上の 32bit は
// [OH_MG_SHARED_HIGH + v] に借りる。段 0 のボクセルが OH_MG_SHARED_HIGH 以下で、段が 2 つ以上(一番粗い段は作業場)の時だけ
FX_CONST uint32_t OH_MG_SHARED_HIGH = OH_SHELL_SHARED_VOXELS / 2;

bool MgFineShared(ShellBox box) {
    return box.size.x * box.size.y * box.size.z <= OH_MG_SHARED_HIGH && !MgCoarsest(box.size);
}

int64_t MgValueAt(uint32_t begin, uint32_t local, bool onShared) {
    if (!onShared)
        return g_multigrid[begin + local].value;

    return (int64_t)FX_U64((uint32_t)gs_potential[OH_MG_SHARED_HIGH + local], (uint32_t)gs_potential[local]);
}

void MgStoreValue(uint32_t begin, uint32_t local, bool onShared, int64_t value) {
    if (!onShared) {
        g_multigrid[begin + local].value = value;
        return;
    }

    gs_potential[local] = (int32_t)(uint32_t)((uint64_t)value & 0xFFFFFFFFull);
    gs_potential[OH_MG_SHARED_HIGH + local] = (int32_t)(uint32_t)((uint64_t)value >> 32);
}

// 隣の値の重みつきの和(段の外は 0)
int64_t MgNeighbourSum(uint32_t begin, uint3 size, uint3 c, OhShellMultigrid voxel, bool onShared) {
    const uint32_t v = MgIndex(size, c);
    const uint32_t strideY = size.x;
    const uint32_t strideZ = size.x * size.y;
    int64_t sum = 0;
    if (c.x > 0)
        sum += (int64_t)voxel.weightMinusX * MgValueAt(begin, v - 1, onShared);
    if (c.x + 1 < size.x)
        sum += (int64_t)voxel.weightPlusX * MgValueAt(begin, v + 1, onShared);
    if (c.y > 0)
        sum += (int64_t)voxel.weightMinusY * MgValueAt(begin, v - strideY, onShared);
    if (c.y + 1 < size.y)
        sum += (int64_t)voxel.weightPlusY * MgValueAt(begin, v + strideY, onShared);
    if (c.z > 0)
        sum += (int64_t)voxel.weightMinusZ * MgValueAt(begin, v - strideZ, onShared);
    if (c.z + 1 < size.z)
        sum += (int64_t)voxel.weightPlusZ * MgValueAt(begin, v + strideZ, onShared);

    return sum;
}

// 多重格子の中の同期(T-1285): 作業場 g_multigrid・g_voxelWork の物の領域は同じグループだけが読み書きするので、
// 書き込みの見える化はグループの範囲で足りる(装置の範囲の AllMemoryBarrierWithGroupSync より待ちが短い)
void MgSync() {
    Barrier(UAV_MEMORY | GROUP_SHARED_MEMORY, GROUP_SCOPE | GROUP_SYNC);
}

// 赤黒 1 回(段が作業場にある時。色ごとに同期)。値 = (右辺 + 隣の和)÷ 重みの和(0 の向きに切る。逆数の掛け算は厳密)
// その色のボクセルだけを回す(行ごとに x を 2 つおき。PressureColour と同じ並べ方。色ごとの答えは回す順によらない)
void MgSweep(uint32_t begin, uint3 size, uint32_t thread, bool onShared) {
    const uint32_t halfX = (size.x + 1) >> 1;
    const uint32_t slots = halfX * size.y * size.z;
    for (uint32_t colour = 0; colour < 2; ++colour) {
        for (uint32_t k = thread; k < slots; k += OH_SHELL_THREADS) {
            const uint32_t row = k / halfX;
            const uint32_t y = row % size.y;
            const uint32_t z = row / size.y;
            const uint32_t x = ((k - (row * halfX)) << 1) + ((y + z + colour) & 1u);
            if (x >= size.x)
                continue;

            const uint3 c = uint3(x, y, z);
            const uint32_t v = MgIndex(size, c);
            const OhShellMultigrid voxel = g_multigrid[begin + v];
            const int64_t sum = voxel.rhs + MgNeighbourSum(begin, size, c, voxel, onShared);
            const int64_t quotient = (int64_t)FxDivRecipU64(FxAbsU64(sum), voxel.recip);
            MgStoreValue(begin, v, onShared, sum < 0 ? -quotient : quotient);
        }

        MgSync();
    }
}

void MgSweeps(uint32_t begin, uint3 size, uint32_t sweeps, uint32_t thread, bool onShared) {
    for (uint32_t s = 0; s < sweeps; ++s)
        MgSweep(begin, size, thread, onShared);
}

// 軸の粗い番号 coarse の中心(細かいセル i の中心を 2i + 1 とする単位)
uint32_t MgCenter2(uint32_t fineCount, uint32_t coarse) {
    const uint32_t first = 2 * coarse;
    const uint32_t last = min(first + 1, fineCount - 1);

    return first + last + 1;
}

// 粗い面の重み: まとめた面の重みの和 × 細かい中心の距離(内 2・壁まで 1)÷ 粗い中心の距離
uint64_t MgCoarseFaceWeight(uint64_t sum, uint32_t fineCount, uint32_t coarseCount, uint32_t coarse, uint32_t side) {
    const uint32_t me = MgCenter2(fineCount, coarse);
    if (side == 0) {
        if (coarse == 0)
            return OhDivideU64(sum, me);

        return OhDivideU64(sum * 2, me - MgCenter2(fineCount, coarse - 1));
    }

    if (coarse + 1 == coarseCount)
        return OhDivideU64(sum, (2 * fineCount) - me);

    return OhDivideU64(sum * 2, MgCenter2(fineCount, coarse + 1) - me);
}

// 細かいボクセル f の残り = 右辺 − (重みの和 × 値 − 隣の和)
int64_t MgResidualAt(uint32_t begin, uint3 size, uint3 f, bool onShared) {
    const uint32_t v = MgIndex(size, f);
    const OhShellMultigrid voxel = g_multigrid[begin + v];

    return voxel.rhs + MgNeighbourSum(begin, size, f, voxel, onShared) -
           ((int64_t)voxel.total * MgValueAt(begin, v, onShared));
}

// 粗い段のボクセル 1 つを、子(最大 8)から作る(集める書き方。値は 0)。右辺 = 子の残りの和(残りはここで数える。
// 残りを一度書いて同期する段を省いた。T-1222 2 本目)。面の重みは値によらないので、withWeights の時(解き始めの V 字 1 回目)だけ作る
void MgCoarsen(uint32_t fineBegin, uint3 fineSize, uint32_t coarseBegin, uint3 coarseSize, uint32_t thread,
               bool withWeights, bool fineShared) {
    const uint32_t count = coarseSize.x * coarseSize.y * coarseSize.z;
    for (uint32_t v = thread; v < count; v += OH_SHELL_THREADS) {
        const uint3 c = MgCoord(coarseSize, v);
        int64_t rhs = 0;
        FX_UNROLL
        for (uint32_t k = 0; k < 8; ++k) {
            const uint3 f = (c * 2) + uint3(k & 1u, (k >> 1) & 1u, (k >> 2) & 1u);
            if (f.x >= fineSize.x || f.y >= fineSize.y || f.z >= fineSize.z)
                continue;

            rhs += MgResidualAt(fineBegin, fineSize, f, fineShared);
        }

        if (!withWeights) {
            g_multigrid[coarseBegin + v].rhs = rhs;
            g_multigrid[coarseBegin + v].value = 0;
            continue;
        }

        uint64_t sumMinusX = 0;
        uint64_t sumPlusX = 0;
        uint64_t sumMinusY = 0;
        uint64_t sumPlusY = 0;
        uint64_t sumMinusZ = 0;
        uint64_t sumPlusZ = 0;
        FX_UNROLL
        for (uint32_t k = 0; k < 8; ++k) {
            const uint3 f = (c * 2) + uint3(k & 1u, (k >> 1) & 1u, (k >> 2) & 1u);
            if (f.x >= fineSize.x || f.y >= fineSize.y || f.z >= fineSize.z)
                continue;

            const OhShellMultigrid child = g_multigrid[fineBegin + MgIndex(fineSize, f)];
            // まとまりの外へ向く面だけ(− 側は偶数の座標・+ 側は奇数の座標か箱の端)
            sumMinusX += (f.x & 1u) == 0 ? child.weightMinusX : (uint64_t)0;
            sumPlusX += (f.x & 1u) == 1 || f.x + 1 == fineSize.x ? child.weightPlusX : (uint64_t)0;
            sumMinusY += (f.y & 1u) == 0 ? child.weightMinusY : (uint64_t)0;
            sumPlusY += (f.y & 1u) == 1 || f.y + 1 == fineSize.y ? child.weightPlusY : (uint64_t)0;
            sumMinusZ += (f.z & 1u) == 0 ? child.weightMinusZ : (uint64_t)0;
            sumPlusZ += (f.z & 1u) == 1 || f.z + 1 == fineSize.z ? child.weightPlusZ : (uint64_t)0;
        }

        OhShellMultigrid coarse;
        coarse.weightMinusX = MgCoarseFaceWeight(sumMinusX, fineSize.x, coarseSize.x, c.x, 0);
        coarse.weightPlusX = MgCoarseFaceWeight(sumPlusX, fineSize.x, coarseSize.x, c.x, 1);
        coarse.weightMinusY = MgCoarseFaceWeight(sumMinusY, fineSize.y, coarseSize.y, c.y, 0);
        coarse.weightPlusY = MgCoarseFaceWeight(sumPlusY, fineSize.y, coarseSize.y, c.y, 1);
        coarse.weightMinusZ = MgCoarseFaceWeight(sumMinusZ, fineSize.z, coarseSize.z, c.z, 0);
        coarse.weightPlusZ = MgCoarseFaceWeight(sumPlusZ, fineSize.z, coarseSize.z, c.z, 1);
        coarse.total = coarse.weightMinusX + coarse.weightPlusX + coarse.weightMinusY + coarse.weightPlusY +
                       coarse.weightMinusZ + coarse.weightPlusZ;
        coarse.recip = FxMakeRecipU64(coarse.total > 0 ? coarse.total : (uint64_t)1);
        coarse.rhs = rhs;
        coarse.value = 0;
        coarse.scratch = 0;
        g_multigrid[coarseBegin + v] = coarse;
    }
}

// 三線形の戻しの軸ごとの重み(12 分): 細かいセル i が使う粗い番号 2 つ(OH_MG_NONE = 壁の向こう = 0)
void MgProlongTap(uint32_t fineCount, uint32_t coarseCount, uint32_t i, out uint32_t close, out uint32_t other,
                  out int64_t closeWeight, out int64_t otherWeight) {
    const uint32_t position = (2 * i) + 1;
    close = i / 2;
    const uint32_t closeCenter = MgCenter2(fineCount, close);
    other = OH_MG_NONE;
    closeWeight = 12;
    otherWeight = 0;
    if (position == closeCenter)
        return;

    // 反対側の中心(壁なら壁の位置)
    uint32_t otherCenter = 0;
    if (position < closeCenter) {
        if (close > 0) {
            other = close - 1;
            otherCenter = MgCenter2(fineCount, other);
        }
    } else if (close + 1 < coarseCount) {
        other = close + 1;
        otherCenter = MgCenter2(fineCount, other);
    } else {
        otherCenter = 2 * fineCount;
    }

    const uint32_t toClose = position > closeCenter ? position - closeCenter : closeCenter - position;
    const uint32_t toOther = position > otherCenter ? position - otherCenter : otherCenter - position;
    const uint32_t weight = (12 * toOther) / (toClose + toOther);
    closeWeight = (int64_t)weight;
    otherWeight = (int64_t)(12 - weight);
}

// 細かい段の値に、粗い段の直しの三線形の戻しを足す
void MgProlongAdd(uint32_t fineBegin, uint3 fineSize, uint32_t coarseBegin, uint3 coarseSize, uint32_t thread,
                  bool fineShared) {
    const uint32_t count = fineSize.x * fineSize.y * fineSize.z;
    for (uint32_t v = thread; v < count; v += OH_SHELL_THREADS) {
        const uint3 c = MgCoord(fineSize, v);
        uint32_t x0, x1, y0, y1, z0, z1;
        int64_t wx0, wx1, wy0, wy1, wz0, wz1;
        MgProlongTap(fineSize.x, coarseSize.x, c.x, x0, x1, wx0, wx1);
        MgProlongTap(fineSize.y, coarseSize.y, c.y, y0, y1, wy0, wy1);
        MgProlongTap(fineSize.z, coarseSize.z, c.z, z0, z1, wz0, wz1);
        int64_t sum = 0;
        FX_UNROLL
        for (uint32_t k = 0; k < 8; ++k) {
            const uint32_t cx = (k & 1u) == 0 ? x0 : x1;
            const uint32_t cy = ((k >> 1) & 1u) == 0 ? y0 : y1;
            const uint32_t cz = ((k >> 2) & 1u) == 0 ? z0 : z1;
            if (cx == OH_MG_NONE || cy == OH_MG_NONE || cz == OH_MG_NONE)
                continue;

            const int64_t weight = ((k & 1u) == 0 ? wx0 : wx1) * (((k >> 1) & 1u) == 0 ? wy0 : wy1) *
                                   (((k >> 2) & 1u) == 0 ? wz0 : wz1);
            sum += weight * g_multigrid[coarseBegin + MgIndex(coarseSize, uint3(cx, cy, cz))].value;
        }

        const int64_t correction = OhMulDivToward0(sum, 1, OH_MG_PROLONG_DIVISOR);
        MgStoreValue(fineBegin, v, fineShared, MgAddSplit(MgValueAt(fineBegin, v, fineShared), correction));
    }
}

// 一番粗い段(どの向きも 2 以下 = 8 ボクセル以下)の赤黒を 1 波の中で回す(レーン = ボクセル。値はレジスタ・隣の値は WaveReadLaneAt。
// グループの同期 2 × 16 回の代わり。T-1222 2 本目)。波の全レーンで呼ぶ。色ごとの答えは回す順によらないので、全スレッドの赤黒とビット単位で同じ
void MgCoarsestWave(uint32_t begin, uint3 size) {
    const uint32_t count = size.x * size.y * size.z;
    const uint32_t lane = WaveGetLaneIndex();
    const bool mine = lane < count;
    const uint32_t own = mine ? lane : 0u;
    const uint3 c = MgCoord(size, own);
    const uint32_t strideY = size.x;
    const uint32_t strideZ = size.x * size.y;
    const OhShellMultigrid voxel = g_multigrid[begin + own];
    const uint32_t ownColour = (c.x + c.y + c.z) & 1u;
    // 隣のレーン(段の外なら自分。重み 0 の項にする)
    const uint32_t minusX = c.x > 0 ? own - 1 : own;
    const uint32_t plusX = c.x + 1 < size.x ? own + 1 : own;
    const uint32_t minusY = c.y > 0 ? own - strideY : own;
    const uint32_t plusY = c.y + 1 < size.y ? own + strideY : own;
    const uint32_t minusZ = c.z > 0 ? own - strideZ : own;
    const uint32_t plusZ = c.z + 1 < size.z ? own + strideZ : own;
    const int64_t weightMinusX = c.x > 0 ? (int64_t)voxel.weightMinusX : (int64_t)0;
    const int64_t weightPlusX = c.x + 1 < size.x ? (int64_t)voxel.weightPlusX : (int64_t)0;
    const int64_t weightMinusY = c.y > 0 ? (int64_t)voxel.weightMinusY : (int64_t)0;
    const int64_t weightPlusY = c.y + 1 < size.y ? (int64_t)voxel.weightPlusY : (int64_t)0;
    const int64_t weightMinusZ = c.z > 0 ? (int64_t)voxel.weightMinusZ : (int64_t)0;
    const int64_t weightPlusZ = c.z + 1 < size.z ? (int64_t)voxel.weightPlusZ : (int64_t)0;
    int64_t value = voxel.value;
    for (uint32_t s = 0; s < OH_MG_COARSEST_SWEEPS; ++s) {
        for (uint32_t colour = 0; colour < 2; ++colour) {
            int64_t sum = voxel.rhs;
            sum += weightMinusX * WaveReadLaneAt(value, minusX);
            sum += weightPlusX * WaveReadLaneAt(value, plusX);
            sum += weightMinusY * WaveReadLaneAt(value, minusY);
            sum += weightPlusY * WaveReadLaneAt(value, plusY);
            sum += weightMinusZ * WaveReadLaneAt(value, minusZ);
            sum += weightPlusZ * WaveReadLaneAt(value, plusZ);
            const int64_t quotient = (int64_t)FxDivRecipU64(FxAbsU64(sum), voxel.recip);
            if (ownColour == colour)
                value = sum < 0 ? -quotient : quotient;
        }
    }

    if (mine)
        g_multigrid[begin + lane].value = value;
}

// V 字 1 回(段 0 の右辺と値から。再帰の代わりに下りと上りの回し)。withWeights = 粗い段の面の重みも作る(解き始めの 1 回目)
void MgCycle(ShellBox box, uint32_t thread, bool withWeights) {
    const uint32_t levels = MgLevelCount(box.size);
    const bool fineShared = MgFineShared(box);
    for (uint32_t level = 0; level + 1 < levels; ++level) {
        uint3 fineSize;
        uint32_t fineBegin;
        uint3 coarseSize;
        uint32_t coarseBegin;
        MgLevelOf(box, level, fineSize, fineBegin);
        MgLevelOf(box, level + 1, coarseSize, coarseBegin);
        MgSweeps(fineBegin, fineSize, OH_MG_SMOOTH_SWEEPS, thread, fineShared && level == 0);
        MgCoarsen(fineBegin, fineSize, coarseBegin, coarseSize, thread, withWeights, fineShared && level == 0);
        MgSync();
    }

    uint3 coarsestSize;
    uint32_t coarsestBegin;
    MgLevelOf(box, levels - 1, coarsestSize, coarsestBegin);
    // 波の幅(装置で決まる)が 8 以上なら 1 波で、でなければ全スレッドの赤黒で
    const bool inWave = WaveGetLaneCount() >= 8;
    if (inWave && WaveActiveAnyTrue(thread == 0))
        MgCoarsestWave(coarsestBegin, coarsestSize);
    if (!inWave)
        MgSweeps(coarsestBegin, coarsestSize, OH_MG_COARSEST_SWEEPS, thread, false);

    MgSync();
    for (uint32_t up = levels - 1; up > 0; --up) {
        uint3 fineSize;
        uint32_t fineBegin;
        uint3 coarseSize;
        uint32_t coarseBegin;
        MgLevelOf(box, up - 1, fineSize, fineBegin);
        MgLevelOf(box, up, coarseSize, coarseBegin);
        MgProlongAdd(fineBegin, fineSize, coarseBegin, coarseSize, thread, fineShared && up == 1);
        MgSync();
        MgSweeps(fineBegin, fineSize, OH_MG_SMOOTH_SWEEPS, thread, fineShared && up == 1);
    }
}

// 段 0 を面の重み(箱の外の面も)と残りから作る(値 0)
void MgPrepareFine(ShellBox box, uint32_t thread) {
    const uint32_t count = box.size.x * box.size.y * box.size.z;
    for (uint32_t v = thread; v < count; v += OH_SHELL_THREADS) {
        const uint3 c = VoxelCoord(box, v);
        OhShellMultigrid fine;
        fine.weightMinusX = g_faceWork[FaceAt(box, 0, c)].weight;
        fine.weightPlusX = g_faceWork[FaceAt(box, 0, c + Unit(0))].weight;
        fine.weightMinusY = g_faceWork[FaceAt(box, 1, c)].weight;
        fine.weightPlusY = g_faceWork[FaceAt(box, 1, c + Unit(1))].weight;
        fine.weightMinusZ = g_faceWork[FaceAt(box, 2, c)].weight;
        fine.weightPlusZ = g_faceWork[FaceAt(box, 2, c + Unit(2))].weight;
        fine.total = g_voxelWork[box.begin + v].total;
        fine.recip = g_voxelWork[box.begin + v].recip;
        fine.rhs = g_voxelWork[box.begin + v].residual * OH_MG_FINE_SCALE;
        fine.value = 0;
        fine.scratch = 0;
        g_multigrid[box.multigridBegin + v] = fine;
        if (MgFineShared(box))
            MgStoreValue(box.multigridBegin, v, true, 0);
    }
}

// ② 動かない点の残りの幅の真ん中へ寄せる: 右辺から 符号(値)×(重みの和 − 1)÷ 2(細かい単位)を引く
void MgCenterRhs(ShellBox box, uint32_t thread) {
    const uint32_t count = box.size.x * box.size.y * box.size.z;
    for (uint32_t v = thread; v < count; v += OH_SHELL_THREADS) {
        const OhShellMultigrid fine = g_multigrid[box.multigridBegin + v];
        const uint64_t total = fine.total > 0 ? fine.total : (uint64_t)1;
        const uint64_t middle = ((total - 1) >> 1) << OH_MG_FINE_BITS;
        // 値 ≥ 0 なら − middle、負なら + middle(符号を付けて 32bit の 2 つで足す)
        const int64_t shift = FxApplySign(middle, !MgNegative(MgValueAt(box.multigridBegin, v, MgFineShared(box))));
        g_multigrid[box.multigridBegin + v].rhs = MgAddSplit(fine.rhs, shift);
    }
}

// ③ Ψ = 四捨五入(値 ÷ 2^8。0 から遠い方へ)を圧力の値に
void MgFinishToPotential(ShellBox box, uint32_t thread) {
    const uint32_t count = box.size.x * box.size.y * box.size.z;
    const uint64_t carryUnit = (uint64_t)1 << (OH_MG_FINE_BITS - 1);
    for (uint32_t v = thread; v < count; v += OH_SHELL_THREADS) {
        const int64_t value = MgValueAt(box.multigridBegin, v, MgFineShared(box));
        const uint64_t rounded = (FxAbsU64(value) + carryUnit) >> OH_MG_FINE_BITS;
        const int64_t potential = MgNegative(value) ? -(int64_t)rounded : (int64_t)rounded;
        g_voxelWork[box.begin + v].potential = potential;
        if (g_sharedPotential == 0)
            continue;

        if (!FitsInt32(potential))
            gs_overflow = 1;

        gs_potential[v] = (int32_t)potential;
    }
}

// ① ② ③ の解き始め(グループの全スレッドで呼ぶ。前後に同期)
void MgSolveStart(ShellBox box, uint32_t thread) {
    MgPrepareFine(box, thread);
    MgSync();
    MgCycle(box, thread, true);
    MgCenterRhs(box, thread);
    MgSync();
    MgCycle(box, thread, false);
    MgFinishToPotential(box, thread);
}

// ⑥ 圧力の力積(ΔM = (Ψ_i − Ψ_j)× 基準の質量)→ 速さ。運動量の真実は壁なら 0
void ProjectFaces(ShellBox box, uint32_t thread) {
    const int64_t referenceMass = (int64_t)gs_cellMass;
    const uint32_t faces = AllFaces(box);
    for (uint32_t f = thread; f < faces; f += OH_SHELL_THREADS) {
        uint32_t axis;
        uint3 c;
        FaceOf(box, f, axis, c);
        const uint32_t global = FaceAt(box, axis, c);
        OhShellFace face = g_faceWork[global];
        const int64_t minus = PotentialAt(box, MinusSide(box, axis, c));
        const int64_t plus = PotentialAt(box, PlusSide(box, axis, c));
        face.next += (minus - plus) * referenceMass;
        face.speed = OhFlowSpeed(face.next, face.mass);
        const int32_t speed32 = (int32_t)face.speed;  // ±65536 に止めてある
        const uint32_t magnitude = (uint32_t)(speed32 < 0 ? -speed32 : speed32);
        InterlockedMax(gs_fastest, magnitude);
        g_faceWork[global].next = face.next;
        g_faceWork[global].speed = face.speed;
        g_momentum[global] = face.wall != 0 ? 0 : face.next;
    }
}

// ⑦ 風上のボクセルから出る割合の和
void SumOutShares(ShellBox box, uint32_t thread) {
    const uint32_t count = box.size.x * box.size.y * box.size.z;
    for (uint32_t v = thread; v < count; v += OH_SHELL_THREADS) {
        const uint3 c = VoxelCoord(box, v);
        const OhFlowCell cell = g_voxelWork[box.begin + v].cell;
        uint64_t outTotal = 0;
        FX_UNROLL
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const int64_t minusSpeed = g_faceWork[FaceAt(box, axis, c)].speed;
            const int64_t plusSpeed = g_faceWork[FaceAt(box, axis, c + Unit(axis))].speed;
            if (minusSpeed < 0)
                outTotal += OhFlowShareQ16(minusSpeed, cell);

            if (plusSpeed > 0)
                outTotal += OhFlowShareQ16(plusSpeed, cell);
        }

        g_voxelWork[box.begin + v].outShare = outTotal;
        if (outTotal > OH_FLOW_SHARE_LIMIT_Q16)
            InterlockedAdd(gs_limited, 1u);
    }
}

// ⑧ 運ぶ: ボクセルごとに自分の 6 つの面の出入りを集めて当てる。箱の外に面する面はセルへ(グループの和)
void MoveGas(GpuShellTable table, GpuShellGases gases, ShellBox box, uint32_t thread) {
    const uint32_t count = box.size.x * box.size.y * box.size.z;
    const FxRecip64 billion = FxMakeRecipU64(1000000000);
    OhFlow toCell = OhZeroFlow();
    for (uint32_t v = thread; v < count; v += OH_SHELL_THREADS) {
        const uint3 c = VoxelCoord(box, v);
        OhFlow delta = OhZeroFlow();
        FX_UNROLL
        for (uint32_t side = 0; side < 6; ++side) {
            const uint32_t axis = side >> 1;
            const bool plusFace = (side & 1u) != 0;  // 自分の + 側の面(自分は面の − 側)
            const uint3 faceCoord = plusFace ? c + Unit(axis) : c;
            const int64_t q = g_faceWork[FaceAt(box, axis, faceCoord)].speed;
            if (q == 0)
                continue;

            const uint32_t other = plusFace ? PlusSide(box, axis, faceCoord) : MinusSide(box, axis, faceCoord);
            const bool selfUpwind = plusFace ? q > 0 : q < 0;
            const uint32_t upwind = selfUpwind ? v : other;
            const OhFlow moved = MovedThrough(table, gases, box, q, upwind, billion);
            // 風上なら出る・風下なら入る。箱の外に面する面は、その逆をセルへ
            if (selfUpwind) {
                delta = SubtractFlow(delta, moved);
                if (other == OH_SHELL_OUTSIDE)
                    toCell = OhAddFlow(toCell, moved);
            } else {
                delta = OhAddFlow(delta, moved);
                if (other == OH_SHELL_OUTSIDE)
                    toCell = SubtractFlow(toCell, moved);
            }
        }

        if (OhFlowIsZero(delta))
            continue;

        RxCell voxel = g_voxels[box.begin + v];
        voxel.energy += delta.energy;
        uint32_t ok = 1;
        FX_UNROLL
        for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
            const OhAmountChange change = OhAddAmount(voxel, gases.GasSpecies(g), delta.amounts[g]);
            voxel = change.cell;
            ok = ok & change.ok;
        }

        g_voxels[box.begin + v] = voxel;
        g_voxelWork[box.begin + v].changed = 1;
        if (ok == 0)
            gs_failed = 1;
    }

    // セルへの和(整数の足し算なので順によらない)
    const int64_t energy = WaveActiveSum(toCell.energy);
    if (WaveIsFirstLane() && energy != 0)
        InterlockedAdd(gs_toCell[0], energy);

    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
        const int64_t amount = WaveActiveSum(toCell.amounts[g]);
        if (WaveIsFirstLane() && amount != 0)
            InterlockedAdd(gs_toCell[1 + g], amount);
    }
}

// ⑨ 箱の外へ出た・入った分を対の端数を通してセルへ(1 スレッド。帳簿「物との受け渡し」)
void ExchangeWithCell(GpuShellGases gases, OhObject object, uint32_t objectIndex, uint32_t pass) {
    OhFlow toCell;
    toCell.energy = gs_toCell[0];
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g)
        toCell.amounts[g] = gs_toCell[1 + g];

    if (OhFlowIsZero(toCell))
        return;

    const OhCarryStep step = OhSplitCarry(object, toCell, g_carries[objectIndex]);
    g_carries[objectIndex] = step.carry;
    const OhAmountChange change = OhApplyToCell(gases, g_cells[object.cell], step.coarse);
    g_cells[object.cell] = change.cell;
    g_cellChanged[object.cell] = pass;
    if (change.ok == 0)
        gs_failed = 1;
}

// ⑩ 変わったボクセルの待ちとブロックを起こす(CPU の StepShellFlow の終わりと同じ)
void WakeChanged(OhObject object, ShellBox box, uint32_t thread, uint64_t tick, uint32_t pass) {
    const uint32_t count = box.size.x * box.size.y * box.size.z;
    for (uint32_t v = thread; v < count; v += OH_SHELL_THREADS) {
        if (g_voxelWork[box.begin + v].changed != 0)
            g_waits[box.begin + v].changedTick = tick;
    }

    const uint32_t blocks = object.blocksX * object.blocksY * object.blocksZ;
    for (uint32_t b = thread; b < blocks; b += OH_SHELL_THREADS) {
        const OhBlock block = g_blocks[object.blockBegin + b];
        bool any = false;
        for (uint32_t local = 0; local < OH_BLOCK_VOXELS; ++local) {
            const uint32_t voxel = OhBlockVoxel(object, block, local);
            if (voxel != OH_NO_BLOCK && g_voxelWork[voxel].changed != 0)
                any = true;
        }

        if (!any)
            continue;

        OhBlockState state = g_blockStates[object.blockBegin + b];
        state.changedPass = pass;
        state.quiet = 0;
        FX_UNROLL
        for (uint32_t face = 0; face < 6; ++face)
            state.facePass[face] = pass;

        g_blockStates[object.blockBegin + b] = state;
    }
}

// 1 グループ = 1 物の 1 刻みの流れ
[numthreads(OH_SHELL_THREADS, 1, 1)] void OhShellFlowTick(uint3 groupId : SV_GroupID, uint32_t thread : SV_GroupIndex) {
    const uint32_t objectIndex = groupId.x;
    if (objectIndex >= g_objectCount)
        return;

    const OhObject object = g_objects[objectIndex];
    if (object.shellLayers == 0)
        return;

    GpuShellTable table;
    table.unused = 0;
    GpuShellGases gases;
    gases.unused = 0;
    const OhShellObject shell = g_shellObjects[objectIndex];
    ShellBox box;
    box.begin = object.voxelBegin;
    box.size = uint3(object.sizeX, object.sizeY, object.voxelCount / (object.sizeX * object.sizeY));
    box.faceBegin[0] = shell.faceBegin[0];
    box.faceBegin[1] = shell.faceBegin[1];
    box.faceBegin[2] = shell.faceBegin[2];
    box.multigridBegin = shell.multigridBegin;
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    const uint32_t pass = g_passBase + objectIndex + 1;
    const uint64_t gravity = OhFlowGravityQ16(object.bodyLevel, g_substeps);
    if (thread == 0) {
        gs_limited = 0;
        gs_fastest = 0;
        gs_failed = 0;
        gs_overflow = 0;
    }

    // 温めた始まり: 圧力の値を作業場(層の状態)から groupshared へ
    const bool warmStart = (g_pressureMode & OH_PRESSURE_WARM_START) != 0;
    for (uint32_t v = thread; v < object.voxelCount; v += OH_SHELL_THREADS) {
        g_voxelWork[box.begin + v].changed = 0;
        if (warmStart && g_sharedPotential != 0)
            gs_potential[v] = (int32_t)g_voxelWork[box.begin + v].potential;
    }

    const bool untilQuiet = (g_pressureMode & OH_PRESSURE_UNTIL_QUIET) != 0;
    uint32_t sweepsUsed = 0;
    uint32_t mostSweeps = 0;
    uint32_t capped = 0;
    for (uint32_t substep = 0; substep < g_substeps; ++substep) {
        if (thread == 0) {
            PrepareCell(table, gases, object.cell);
            gs_pressureMoved[0] = 0;
        }

        for (uint32_t w = thread; w < OH_SHELL_DIRTY_WORDS; w += OH_SHELL_THREADS) {
            gs_dirty[0][w] = 0xFFFFFFFFu;
            gs_dirty[1][w] = 0xFFFFFFFFu;
        }

        AllMemoryBarrierWithGroupSync();
        if ((g_stageMask & OH_STAGE_VOXELS) != 0)
            PrepareVoxels(table, gases, box, thread);

        AllMemoryBarrierWithGroupSync();
        if ((g_stageMask & OH_STAGE_FACES) != 0)
            PrepareFaces(box, thread);

        AllMemoryBarrierWithGroupSync();
        if ((g_stageMask & OH_STAGE_ADVECT) != 0)
            AdvectFaces(box, thread, gravity);

        AllMemoryBarrierWithGroupSync();
        if ((g_stageMask & OH_STAGE_RESIDUAL) != 0)
            PrepareResidual(box, thread);

        AllMemoryBarrierWithGroupSync();
        // 解き始めを多重格子で(T-1222。g_pressureMode はルート定数なのでグループの全スレッドが同じ道を通る)
        if ((g_pressureMode & OH_PRESSURE_MULTIGRID) != 0 && (g_stageMask & OH_STAGE_PRESSURE) != 0) {
            MgSolveStart(box, thread);
            MgSync();
        }

        // 圧力の値が groupshared なら、反復の間は groupshared の同期だけ(作業場の読みは反復の前に書いた値だけ)
        // 回 sweep は印 sweep % 3 に書き、終わりの同期の後に全スレッドが読む。次の回の印は今 0 に戻す(最後に読まれたのは 2 回前)
        uint32_t used = 0;
        bool quiet = false;
        const bool dirtyOnly = PressureSlots(box) <= OH_SHELL_DIRTY_WORDS * 32;
        const uint32_t sweepLimit = (g_stageMask & OH_STAGE_PRESSURE) != 0 ? g_pressureSweeps : 0u;
        for (uint32_t sweep = 0; sweep < sweepLimit; ++sweep) {
            const uint32_t slot = sweep % 3;
            if (thread == 0)
                gs_pressureMoved[(sweep + 1) % 3] = 0;

            for (uint32_t colour = 0; colour < 2; ++colour) {
                PressureColour(box, thread, colour, slot, dirtyOnly);
                if (g_sharedPotential != 0)
                    GroupMemoryBarrierWithGroupSync();
                else
                    AllMemoryBarrierWithGroupSync();
            }

            ++used;
            if (untilQuiet && gs_pressureMoved[slot] == 0) {
                quiet = true;
                break;
            }
        }

        sweepsUsed += used;
        mostSweeps = max(mostSweeps, used);
        capped += untilQuiet && !quiet ? 1u : 0u;

        AllMemoryBarrierWithGroupSync();

        if ((g_stageMask & OH_STAGE_PROJECT) != 0)
            ProjectFaces(box, thread);

        AllMemoryBarrierWithGroupSync();
        if ((g_stageMask & OH_STAGE_SHARES) != 0)
            SumOutShares(box, thread);

        AllMemoryBarrierWithGroupSync();
        if ((g_stageMask & OH_STAGE_MOVE) != 0)
            MoveGas(table, gases, box, thread);

        AllMemoryBarrierWithGroupSync();
        if (thread == 0 && (g_stageMask & OH_STAGE_EXCHANGE) != 0)
            ExchangeWithCell(gases, object, objectIndex, pass);

        AllMemoryBarrierWithGroupSync();
    }

    // 温めた始まり: 圧力の値を作業場(層の状態)へ戻す
    if (warmStart && g_sharedPotential != 0) {
        for (uint32_t v = thread; v < object.voxelCount; v += OH_SHELL_THREADS)
            g_voxelWork[box.begin + v].potential = gs_potential[v];
    }

    if ((g_stageMask & OH_STAGE_WAKE) != 0)
        WakeChanged(object, box, thread, tick, pass);

    if (thread != 0)
        return;

    const uint32_t sweepStats = 2 + (2 * g_objectCount);
    InterlockedAdd(g_stats[sweepStats + 0], g_substeps);
    InterlockedAdd(g_stats[sweepStats + 1], sweepsUsed);
    InterlockedMax(g_stats[sweepStats + 2], mostSweeps);
    InterlockedAdd(g_stats[sweepStats + 3], capped);

    InterlockedAdd(g_stats[2 + (2 * objectIndex)], gs_limited);
    InterlockedMax(g_stats[3 + (2 * objectIndex)], gs_fastest);
    if (gs_failed != 0)
        InterlockedAdd(g_stats[0], 1u);

    if (gs_overflow != 0)
        InterlockedAdd(g_stats[1], 1u);
}
