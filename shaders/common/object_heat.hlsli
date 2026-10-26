// object_heat.hlsli — 小さい物が自分の熱を持つ(物の箱のボクセルの熱と反応・セルとの対の面の 4 つの口)の共有の式(T-1064・ADR-0434・20 §12.4)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(tests/object_heat_cpu.h)と GPU(shaders/sim/object_heat.hlsl)が
// 同じ関数を呼び、ビット一致する(tests/gpu_object_heat_test.cpp)。式は T-0991 の CPU の試作(tests/object_heat_test.cpp)と同じで、
// 試作の 1 枚の紙を「たくさんの物・物の形は箱の中の並び + ボクセルごとの外に面する数」に広げたもの(試作とのビット一致も object_heat_test が確かめる)。
//
// データの流れ(1 刻み。対の面は小刻み 4^(L−6) 回。どれも小刻みの初めの値から計算する = Jacobi):
//   ① 下ごしらえ(ボクセルごと): ボクセルの温度・熱容量・物の中の伝導率(体積の平行。D-637)→ 作業場
//   ② 流れ(ボクセルごと): 囲むセル(読むだけ)との対の面の口(伝導・気体の混ざり・放射。D-639 の膜の温度の形)と光、
//      隣のボクセルとの伝導 → 自分のボクセルに当て、セルへ出す分をボクセルの記録に書く
//   ③ 物ごとの和と対の端数(物ごと): ボクセルの記録を足し、粗い側(セル)の 1 単位に満たない分を対の端数に持ち越す → 物ごとの粗い量
//   ④ セルへ当てる(物の入っているセルごと): そのセルの物の粗い量を足してセルへ
//   ⑤ 反応(ボクセルごと): セルと同じ関数(RxStepCellWait)
// 保存: Σ セル × 8^(L−Lc) + Σ ボクセル + Σ 対の端数 − 帳簿(入った光)= 一定(エネルギーと元素。ビット単位)。
// 単位: ボクセルの量とエネルギーはそのレベルの単位(8^−L µmol・8^−L mJ)。量の数は濃さ × 0.125 m³ なので、反応と温度の式はセルと同じ。
#ifndef BICAMERAL_OBJECT_HEAT_HLSLI
#define BICAMERAL_OBJECT_HEAT_HLSLI

#include "heat_conduction.hlsli"
#include "radiation.hlsli"
#include "reaction.hlsli"

RX_NAMESPACE_BEGIN

// --- 決まった値(32bit を超える定数を式に書かない。32bit の定数の積と割り算で書く。T-1078)---

FX_CONST uint32_t OH_MAX_GASES = 8;  // 対の面で混ざる気体の数の上限(表の気体。最小の形。超える表は T-1076)
FX_CONST uint32_t OH_NO_GAS = 0xFFFFFFFFu;
FX_CONST int64_t OH_CELL_UM = 500000;  // レベル 0 の一辺(µm)
FX_CONST uint64_t OH_MEGA = 1000000;
FX_CONST uint64_t OH_BOX_MEGA_MM3 = 125000;  // 0.125 m³ = 125000 × 10^6 mm³(そのレベルの単位の量 = 濃さ × 0.125 m³)
// mW/(m·K) × µm × mK = 10^-12 W・1/60 s → mJ: ÷ (10^6 × 60000)
FX_CONST uint64_t OH_CONDUCTION_DIVISOR_MEGA = 60000;  // × 10^6
// mm²/s × µm × (単位 ÷ 0.125 m³)× 1/60 s → 単位 × 8 ÷ (60 × 10^12)
FX_CONST uint64_t OH_DIFFUSION_DIVISOR_MEGA = 60000000;  // × 10^6
FX_CONST uint32_t OH_RADIATION_SHARE_Q16 = 32768;        // 放射の 1 小刻みの上限(熱容量 × ΔT の半分)

// ボクセルの形の語(OhVoxelInfo::shape)
FX_CONST uint32_t OH_NEIGHBOUR_MASK = 0x3Fu;  // ビット 0〜5 = 隣のボクセルがある(−x・+x・−y・+y・−z・+z)
FX_CONST uint32_t OH_EXPOSED_SHIFT = 8;       // ビット 8〜11 = 囲むセルに面する面の数(0〜6)
FX_CONST uint32_t OH_EXPOSED_MASK = 0xFu;
// 物の箱が囲む空気の層(D-667・ADR-0469 3-4・T-1142)。層の無い物ではどちらも 0(今までと同じ式・同じ結果)
FX_CONST uint32_t OH_SHELL_BIT = 1u << 12;  // 層のボクセル(骨格の外の空気。中身はセルと同じ RxCell。物専用の式 0)
FX_CONST uint32_t OH_THROUGH_SHIFT =
    16;  // ビット 16〜19 = 層を透かして囲む所へ放射する面の数(骨格のボクセル。層は気体で光学的に薄い)

// --- 表(物質ごとの対の面の値。ベイクの値から作る。添字 = 物質 ID)---
struct OhSpecies {
    uint32_t gasIndex;                 // 気体なら対の面の気体の番号(0〜OH_MAX_GASES−1)、気体でなければ OH_NO_GAS
    uint32_t diffusivityMm2PerS;       // 混ざる速さ(室温の値。書いていなければ既定)
    uint32_t molarVolumeMm3PerMol;     // 固体・液体のモル体積(体積の平行の伝導率に使う)
    uint32_t absorptionLongMm2PerMol;  // 長い帯の吸い込み(放射の口)
};

// --- 物(物の見出し。並びは囲むセルの順)---
struct OhObject {
    // --- ボクセルの並び ---
    uint32_t voxelBegin;  // 全部の物のボクセルの並びの中の始まり
    uint32_t voxelCount;  // = sizeX × sizeY × sizeZ
    uint32_t sizeX;
    uint32_t sizeY;

    // --- 細かさと囲むセル ---
    uint32_t cell;      // 囲むセルの番号
    int32_t bodyLevel;  // 物の箱のレベル(7.8 mm = 6)
    int32_t cellLevel;  // 囲むセルのレベル
    uint32_t substeps;  // 対の面の小刻み(4^(L − 6)。T-1016)

    // --- 膜と層 ---
    uint32_t filmUm;       // 境の膜の厚さ(物の形と流れで決まる物理の長さ。0 は使わない)
    uint32_t shellLayers;  // 骨格の外の空気の層の数(0 = 層なし・1 = 1 段目。D-667。箱の大きさは骨格 + 2 × 層)

    // --- 物の中のブロック(64 ボクセル以下。眠らせる単位。T-1098)---
    uint32_t blockBegin;  // 全部の物のブロックの並びの中の始まり
    uint32_t blocksX;
    uint32_t blocksY;
    uint32_t blocksZ;
    uint32_t blockEdgeX;  // ブロックの一辺のボクセルの数(物の形で選ぶ。積は 64 以下)
    uint32_t blockEdgeY;
    uint32_t blockEdgeZ;
};

// --- 物の中のブロック(眠らせる単位。T-1098・ADR-0459)---
// 刻みの中の受け渡し(小刻みごと)と反応を、それぞれ通し番号の「段」(pass。1 から)で数える。
// ブロックは、最後に評価した時に静か(どのボクセルも変わらず、どの流れも 0)で、それから自分・隣のブロック・囲むセル・光のどれも
// 変わっていなければ、受け渡しを回しても厳密に何も起きない(流れは段の初めの値だけの関数なので同じ 0 になる)ので回さない。
// 反応は、ボクセルが変わった刻み(changedTick)と待ちの丸めの起こす刻み(wakeTick)で同じく決まる(ADR-0018)。
FX_CONST uint32_t OH_BLOCK_VOXELS = 64;
FX_CONST uint32_t OH_NO_BLOCK = 0xFFFFFFFFu;
FX_CONST uint32_t OH_BLOCK_EXPOSED = 1u;  // 囲むセルに面するボクセルがある
FX_CONST uint32_t OH_BLOCK_LIT = 2u;      // 光を吸うボクセルがある

struct OhBlock {
    uint32_t object;
    uint32_t x;  // ブロックの座標(物の箱の中。ボクセルは × 4)
    uint32_t y;
    uint32_t z;
};

struct OhBlockState {
    // --- 段の番号 ---
    uint32_t changedPass;   // どれかのボクセルが最後に変わった段
    uint32_t exchangePass;  // 受け渡しを最後に評価した段
    uint32_t reactPass;     // 反応を最後に評価した段
    uint32_t quiet;         // 最後の受け渡しの評価で静かだった(1)

    // --- 反応・この段 ---
    uint64_t minWakeTick;  // ボクセルの起こす刻みの最小
    uint32_t awake;        // この段で回す(OhWakeBlocks が書く)
    uint32_t flags;        // OH_BLOCK_*(変わらない)

    // --- 面ごと(方向 0〜5)に、その面に並ぶボクセルが最後に変わった段。隣のブロックはこれだけを読む ---
    uint32_t facePass[6];
    uint32_t padding0;
    uint32_t padding1;
};

// ボクセルの待ち(反応の待ちの丸め)
struct OhVoxelWait {
    uint64_t changedTick;  // 最後に変わった刻み(受け渡しで変わった刻み t は t − 1、反応で変わった刻み t は t)
    uint64_t wakeTick;     // 変わらなければ次に反応を評価する刻み
};

// ブロックの中の番号(0〜63)のボクセルの番号(箱の外なら OH_NO_BLOCK)
FX_FN uint32_t OhBlockVoxel(OhObject object, OhBlock block, uint32_t local) {
    const uint32_t edgeXY = object.blockEdgeX * object.blockEdgeY;
    const uint32_t localZ = local / edgeXY;
    const uint32_t x = (block.x * object.blockEdgeX) + (local % object.blockEdgeX);
    const uint32_t y = (block.y * object.blockEdgeY) + ((local / object.blockEdgeX) % object.blockEdgeY);
    const uint32_t z = (block.z * object.blockEdgeZ) + localZ;
    const uint32_t sizeZ = object.voxelCount / (object.sizeX * object.sizeY);
    if (localZ >= object.blockEdgeZ || x >= object.sizeX || y >= object.sizeY || z >= sizeZ)
        return OH_NO_BLOCK;

    return object.voxelBegin + x + (object.sizeX * (y + (object.sizeY * z)));
}

// ブロックの中の番号のボクセルが並ぶブロックの面(ビット 0〜5 = −x・+x・−y・+y・−z・+z)
FX_FN uint32_t OhVoxelFaces(OhObject object, uint32_t local) {
    const uint32_t x = local % object.blockEdgeX;
    const uint32_t y = (local / object.blockEdgeX) % object.blockEdgeY;
    const uint32_t z = local / (object.blockEdgeX * object.blockEdgeY);

    return (x == 0 ? 1u : 0u) | (x + 1 == object.blockEdgeX ? 2u : 0u) | (y == 0 ? 4u : 0u) |
           (y + 1 == object.blockEdgeY ? 8u : 0u) | (z == 0 ? 16u : 0u) | (z + 1 == object.blockEdgeZ ? 32u : 0u);
}

// 隣のブロックの番号(方向 0〜5 = −x・+x・−y・+y・−z・+z。物の外なら OH_NO_BLOCK)
FX_FN uint32_t OhNeighbourBlock(OhObject object, OhBlock block, uint32_t direction) {
    int32_t x = (int32_t)block.x;
    int32_t y = (int32_t)block.y;
    int32_t z = (int32_t)block.z;
    const int32_t step = (direction & 1) != 0 ? 1 : -1;
    x += direction < 2 ? step : 0;
    y += (direction == 2 || direction == 3) ? step : 0;
    z += direction >= 4 ? step : 0;
    const bool outside = x < 0 || y < 0 || z < 0 || x >= (int32_t)object.blocksX || y >= (int32_t)object.blocksY ||
                         z >= (int32_t)object.blocksZ;
    if (outside)
        return OH_NO_BLOCK;

    return object.blockBegin + (uint32_t)x + (object.blocksX * ((uint32_t)y + (object.blocksY * (uint32_t)z)));
}

// この受け渡しの段で回すか。newestChange = 自分の changedPass と、隣のブロックの自分に向いた面の facePass の最大。
// 隣・セルが自分を評価したのと同じ段で変わった時(自分が読んだ後に変わった)も起こすので ≥ で比べる
FX_FN bool OhExchangeAwake(OhBlockState state, uint32_t newestChange, uint32_t cellChangedPass, bool lightOn) {
    const bool cellChanged = (state.flags & OH_BLOCK_EXPOSED) != 0 && cellChangedPass >= state.exchangePass;
    const bool lit = lightOn && (state.flags & OH_BLOCK_LIT) != 0;

    return state.quiet == 0 || newestChange >= state.exchangePass || cellChanged || lit;
}

// この刻みに反応を評価するか(最後の評価から変わった・起こす刻みが来た)
FX_FN bool OhReactAwake(OhBlockState state, uint64_t tick) {
    return state.changedPass > state.reactPass || state.minWakeTick <= tick;
}

// --- 起きている物の一覧(T-1077。眠っている物を Dispatch しない。眠らせ方の印で、真実ではない)---
// 一覧は段ごとに物の印から作り、一覧の物のブロックだけを ExecuteIndirect で回す。一覧に入れるのは「どれかのブロックが起きうる」物で、
// 入れなかった物のブロックは、全部を回して OhExchangeAwake・OhReactAwake で見た時も眠っている(だから結果は一覧なしと同じ)。
//   受け渡し: 最後に回した段(lastListed)の後に、物の中のどれかが変わった・静かでなかった(busyPass ≥ lastListed)・
//             囲むセルが変わった(外に面する物で、セルの変わった段 ≥ lastListed)・光が当たる(光を吸う物)
//   反応: この刻みの受け渡しで一覧に入った・物の中の起こす刻みの最小が来た
// 物の中の変化はブロックの隣どうしだけで伝わり(OhNeighbourBlock は物の中)、セルを読む前(起こす段の前)に書かれたセルの段は
// lastListed より小さい(セルの段は書いた段の番号)ので、この条件で眠っている物のブロックは、回しても起きない。
FX_CONST uint32_t
    OH_OBJECT_AWAKE_LEFT = 0x100u;  // 前に一覧に入れた(ブロックの awake が 1 のまま残りうる。外れる時に 0 に戻す)
FX_CONST uint32_t OH_LIST_REACT = 0xFFFFFFFFu;  // 一覧を作る段の小刻みの番号: 反応の一覧
FX_CONST uint32_t OH_LIST_HEADER = 4;           // 一覧の頭の語: [0] 一覧の物の数・[1] 一覧のブロックの数
FX_CONST uint32_t OH_LIST_GROUP_OBJECTS = 256;  // 一覧を作る段の 1 グループの物の数
FX_CONST uint32_t OH_LIST_ROW_GROUPS = 1024;    // 一覧で回す段のグループの x の数(超えたら y に折る)
FX_CONST uint32_t OH_LIST_ARGS_WAKE = 0;        // 間接の引数の組: 起こす(1 スレッド = 1 ブロック)
FX_CONST uint32_t OH_LIST_ARGS_BLOCKS = 1;      // 1 グループ = 1 ブロック
FX_CONST uint32_t OH_LIST_ARGS_OBJECTS = 2;     // 1 グループ = 1 物
FX_CONST uint32_t OH_LIST_ARGS_COUNT = 3;

// 眠らせ方(段の定数 g_sleeping)
FX_CONST uint32_t OH_SLEEP_ALL = 0;     // 全部のブロックを毎段回す(眠らせ方の厳密さの比べ)
FX_CONST uint32_t OH_SLEEP_BLOCKS = 1;  // 全部のブロックを Dispatch し、眠っているブロックは 1 語読んで抜ける(T-1098)
FX_CONST uint32_t OH_SLEEP_LISTED = 2;  // 起きている物の一覧のブロックだけ ExecuteIndirect で回す(T-1077)

struct OhObjectState {
    uint32_t lastListed;   // 受け渡しを最後に回した段(一覧に入れ、小刻みを飛ばさなかった)
    uint32_t busyPass;     // 物の中のどれかのブロックが最後に変わった・静かでなかった段(InterlockedMax)
    uint32_t reducedPass;  // ③ が粗い量を最後に書いた段(④ はこの段の物だけ足す)
    uint32_t flags;        // OH_BLOCK_*(ブロックの和。変わらない)| OH_OBJECT_AWAKE_LEFT
    uint64_t minWakeTick;  // 物の中のブロックの起こす刻みの最小(反応の後に一覧の物だけ直す)
    uint32_t listedTick;   // 受け渡しの一覧に最後に入った刻み(下 32bit)
    uint32_t padding;
};

// この受け渡しの段で物を一覧に入れるか
FX_FN bool OhObjectExchangeListed(OhObjectState state, uint32_t cellChangedPass, bool lightOn) {
    const bool cellChanged = (state.flags & OH_BLOCK_EXPOSED) != 0 && cellChangedPass >= state.lastListed;
    const bool lit = lightOn && (state.flags & OH_BLOCK_LIT) != 0;

    return state.busyPass >= state.lastListed || cellChanged || lit;
}

// この刻みの反応の段で物を一覧に入れるか
FX_FN bool OhObjectReactListed(OhObjectState state, uint64_t tick) {
    return state.listedTick == (uint32_t)tick || state.minWakeTick <= tick;
}

// ボクセルの変わらない値
struct OhVoxelInfo {
    uint32_t object;  // 物の番号
    uint32_t shape;   // OH_NEIGHBOUR_MASK と外に面する面の数
};

// ① の作業場(ボクセルごと)
struct OhVoxelHeat {
    int32_t temperature;  // mK
    uint32_t padding;
    uint64_t heatCapacity;       // nJ/K
    uint64_t innerConductivity;  // mW/(m·K)(体積で重み付けた平行。D-637)
};

// ② の記録・③ の和と端数(ボクセル → セルが正。ボクセルの単位)
struct OhFlow {
    int64_t energy;
    int64_t amounts[OH_MAX_GASES];  // 対の面の気体の番号ごと
};

// セルへ当てる物の範囲(④。物の入っているセルごと)
struct OhCellRange {
    uint32_t cell;
    uint32_t objectBegin;
    uint32_t objectCount;
    uint32_t padding;
};

// --- 整数の道具 ---

// value × multiplier ÷ divisor(符号つき。128bit の積で桁あふれしない。0 の向きに切る = 向きを逆にすると値も逆)
FX_FN int64_t OhMulDivToward0(int64_t value, uint64_t multiplier, uint64_t divisor) {
    const uint64_t magnitude = FxAbsU64(value);
    const int64_t result = (int64_t)FxDivU128By64(FxMulU64Full(magnitude, multiplier), divisor).quotient;

    return value < 0 ? -result : result;
}

// 64bit ÷ 64bit(FxDivU128By64 で。GPU で定数の 64bit の割り算〔÷ 0.125 m³ など〕が CPU と食い違ったので、この式では整数の / を使わない。T-1064)
FX_FN uint64_t OhDivideU64(uint64_t numerator, uint64_t divisor) {
    FxU128 wide;
    wide.hi = 0;
    wide.lo = numerator;

    return FxDivU128By64(wide, divisor).quotient;
}

FX_FN OhFlow OhZeroFlow() {
    OhFlow flow;
    flow.energy = 0;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g)
        flow.amounts[g] = 0;

    return flow;
}

FX_FN bool OhFlowIsZero(OhFlow flow) {
    bool zero = flow.energy == 0;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g)
        zero = zero && flow.amounts[g] == 0;

    return zero;
}

template <typename Cell>
FX_FN uint64_t OhAmountOf(Cell cell, uint32_t species) {
    const uint32_t slot = RxFindSlot(cell, species);

    return slot == RX_NO_SLOT ? (uint64_t)0 : RX_AMOUNT_AT(cell, slot);
}

// 物質の量を足し引きした結果(ok = 0 なら負になる・成分の枠があふれる。cell は変えない)
struct OhAmountChange {
    RxCell cell;
    uint32_t ok;
};

FX_FN OhAmountChange OhAddAmount(RxCell cell, uint32_t species, int64_t delta) {
    OhAmountChange change;
    change.cell = cell;
    change.ok = 1;
    if (delta == 0)
        return change;

    const uint32_t slot = RxFindSlot(cell, species);
    if (slot == RX_NO_SLOT) {
        if (delta < 0) {
            change.ok = 0;
            return change;
        }

        const RxAdded added = RxTryAddSpecies(cell, species, (uint64_t)delta);
        change.cell = added.cell;
        change.ok = added.overflowed == 0 ? 1u : 0u;

        return change;
    }

    const int64_t now = (int64_t)RX_AMOUNT_AT(cell, slot) + delta;
    if (now < 0) {
        change.ok = 0;
        return change;
    }

    // now ≥ 0 を確かめたので、符号なしの足し算(負の delta は 2^64 の剰余)で同じ値になる
    RxCell changed = cell;
    RX_ADD_AMOUNT(changed, slot, (uint64_t)delta);
    change.cell = RxCompactCell(changed);

    return change;
}

// --- 膜の気体の値(D-639: 膜の温度〔両側の平均〕で、剛体球の気体の運動論の形。混ざる速さ ∝ T^1.5・伝導率 ∝ T^0.5)---

struct OhFilmScale {
    uint64_t diffusionQ16;
    uint64_t conductionQ16;
};

// 膜の温度の気体の値はセルと同じ式(HcGasScaleAt。室温 HC_ROOM_TEMPERATURE_MK。T-1066)
FX_FN OhFilmScale OhFilmScaleOf(int32_t voxelMilliKelvin, int32_t airMilliKelvin) {
    const uint64_t film = ((uint64_t)voxelMilliKelvin + (uint64_t)airMilliKelvin) / 2;
    const HcGasScale gas = HcGasScaleAt((uint32_t)film);
    OhFilmScale scale;
    scale.diffusionQ16 = (uint64_t)gas.diffusionQ16;
    scale.conductionQ16 = (uint64_t)gas.conductionQ16;

    return scale;
}

// 外に面する面の「広さ ÷ 膜の厚さ」(µm)= 面の数 × s² ÷ 膜
FX_FN int64_t OhPairLengthUm(OhObject object, uint32_t exposedFaces) {
    const int64_t facesUm2 = ((int64_t)exposedFaces * OH_CELL_UM * OH_CELL_UM) >> (2 * object.bodyLevel);

    return facesUm2 / (int64_t)object.filmUm;
}

// --- ① 下ごしらえ: 箱の伝導率はセルと同じ式(HcCellConductivity。体積で重み付けた平行の道・気体は ∝ √T。D-637・D-639・T-1066)---

template <typename Table>
FX_FN OhVoxelHeat OhPrepareVoxel(Table table, RxCell cell) {
    const RxThermal thermal = RxComputeThermal(table, cell);
    OhVoxelHeat heat;
    heat.temperature = thermal.temperature;
    heat.padding = 0;
    heat.heatCapacity = thermal.heatCapacity;
    heat.innerConductivity = HcCellConductivity(table, cell, HcCacheTemperature(thermal));

    return heat;
}

// --- ② 対の面の口(ボクセル → セルが正。ボクセルの単位)---

// 伝導: 囲むセルの気体の伝導率 × (面の広さ ÷ 膜)× ΔT × Δt
FX_FN int64_t OhPairConduction(OhObject object, uint64_t airConductivity, int64_t pairLengthUm, OhVoxelHeat voxel,
                               int32_t airMilliKelvin, OhFilmScale film) {
    const int64_t difference = (int64_t)voxel.temperature - (int64_t)airMilliKelvin;
    const uint64_t multiplier = (airConductivity * (uint64_t)pairLengthUm * film.conductionQ16) >> 16;

    return OhMulDivToward0(difference * ((int64_t)1 << (3 * object.bodyLevel)), multiplier,
                           (uint64_t)object.substeps * OH_CONDUCTION_DIVISOR_MEGA * OH_MEGA);
}

// 面の広さ s² の吸い込み(Q16): τ = Σ a·n ÷ (0.25 m² × 2^L)(n はボクセルの単位)。和を 2^L で割ってから τ にする
// (τ にしてから割ると τ の飽和 32 が 32 ÷ 2^L に下がり、7.8 mm の箱では厚い紙・炭・草でも吸い込みが 1 − e^−0.5 = 0.39 で頭打ちだった。T-1097)
FX_FN uint32_t OhAbsorptanceQ16(FxU128 sum, int32_t bodyLevel) {
    const uint32_t shift = (uint32_t)bodyLevel;
    FxU128 face;
    face.hi = sum.hi >> shift;
    face.lo = shift == 0 ? sum.lo : ((sum.lo >> shift) | (sum.hi << ((64 - shift) & 63)));

    return RdAbsorptanceQ16(RdOpticalDepthQ32(face));
}

// 放射: ボクセルの吸い込み(長い帯)× 囲む所(吸い込み 1)。RdPairCoefficient は 0.25 m² の面の値なので、
// 外に面する面の広さ(面の数 × s²)とボクセルの単位(× 8^L)にする: × 面の数 × 2^L
template <typename Table>
FX_FN int64_t OhPairRadiation(Table table, OhObject object, uint32_t exposedFaces, RxCell cell, OhVoxelHeat voxel,
                              int32_t airMilliKelvin) {
    FxU128 sum;
    sum.hi = 0;
    sum.lo = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        const uint32_t absorption = table.ObjectSpecies(RX_SPECIES_AT(cell, i)).absorptionLongMm2PerMol;
        sum = RdAddAbsorption(sum, absorption, RX_AMOUNT_AT(cell, i));
    }

    const uint32_t absorptance = OhAbsorptanceQ16(sum, object.bodyLevel);
    const uint64_t weight = RdPairWeightQ32(absorptance, RD_ONE_Q16, RD_ONE_Q16);
    const uint64_t coefficient = (RdPairCoefficient((uint32_t)voxel.temperature, (uint32_t)airMilliKelvin, weight) *
                                  (uint64_t)exposedFaces)
                                 << object.bodyLevel;
    const uint64_t limit = RdPairLimit(voxel.heatCapacity, voxel.heatCapacity, OH_RADIATION_SHARE_Q16);
    const uint64_t bounded = coefficient < limit ? coefficient : limit;

    return RdPairFlow((uint32_t)voxel.temperature, (uint32_t)airMilliKelvin, bounded / (uint64_t)object.substeps);
}

// 気体の混ざり: 気体ごとに 濃さの差 × 混ざる速さ × (面の広さ ÷ 膜)× Δt。運ぶ気体は出ていく側の温度の熱と化学のエネルギーを持っていく。
// flow に足して返す
template <typename Table, typename GasList>
FX_FN OhFlow OhPairGas(Table table, GasList gases, OhObject object, int64_t pairLengthUm, RxCell cell, RxCell air,
                       OhVoxelHeat voxel, int32_t airMilliKelvin, OhFilmScale film, OhFlow flow) {
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
        const uint32_t species = gases.GasSpecies(g);
        if (species == 0)
            continue;

        // 濃さ × 0.125 m³ は、どのレベルでも単位の数そのもの(ボクセルの単位とセルの単位で比べられる)
        const uint64_t inVoxel = OhAmountOf(cell, species);
        const int64_t difference = (int64_t)inVoxel - (int64_t)OhAmountOf(air, species);
        const uint64_t diffusivity = (uint64_t)table.ObjectSpecies(species).diffusivityMm2PerS;
        const uint64_t multiplier = (diffusivity * (uint64_t)pairLengthUm * film.diffusionQ16) >> 16;
        int64_t moved = OhMulDivToward0(difference * ((int64_t)8 << (3 * object.bodyLevel)), multiplier,
                                        (uint64_t)object.substeps * OH_DIFFUSION_DIVISOR_MEGA * OH_MEGA);
        if (moved > (int64_t)inVoxel)
            moved = (int64_t)inVoxel;

        if (moved == 0)
            continue;

        // 運ぶエネルギー = 量 × (h0 + 比熱 × 出ていく側の温度)。µJ/µmol と nJ/(µmol·K) × mK → mJ は ÷ 10^9(積は 128bit。T-1016)
        const RxSpecies values = table.Species(species);
        const int64_t temperature = moved > 0 ? (int64_t)voxel.temperature : (int64_t)airMilliKelvin;
        const int64_t perUnit = (values.h0 * 1000000) + ((int64_t)values.heatCapacity * temperature);
        const int64_t carried = OhMulDivToward0(moved, FxAbsU64(perUnit), 1000000000);
        flow.amounts[g] += moved;
        flow.energy += perUnit < 0 ? -carried : carried;
    }

    return flow;
}

// 物の中の隣のボクセルどうしの伝導(面 s²・距離 s。伝導率は両側の小さい方)。自分 → 隣が正
FX_FN int64_t OhInnerConduction(OhObject object, OhVoxelHeat self, OhVoxelHeat other) {
    const uint64_t conductivity = self.innerConductivity < other.innerConductivity ? self.innerConductivity
                                                                                   : other.innerConductivity;
    const int64_t difference = (int64_t)self.temperature - (int64_t)other.temperature;
    const uint64_t multiplier = conductivity * (uint64_t)(OH_CELL_UM >> object.bodyLevel);

    return OhMulDivToward0(difference * ((int64_t)1 << (3 * object.bodyLevel)), multiplier,
                           (uint64_t)object.substeps * OH_CONDUCTION_DIVISOR_MEGA * OH_MEGA);
}

// --- 層に面する面の気体の混ざり(T-1142。層 ⇄ 骨格・層 ⇄ 層。骨格どうしは今まで通り伝導だけ)---

// ① の作業場の気体(小刻みの初めの量。隣のボクセルが同じ小刻みに書き換えても、両側が同じ値から計算する = Jacobi)
struct OhVoxelGas {
    uint64_t amounts[OH_MAX_GASES];  // 対の面の気体の番号ごと
};

template <typename GasList>
FX_FN OhVoxelGas OhPrepareGas(GasList gases, RxCell cell) {
    OhVoxelGas gas;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
        const uint32_t species = gases.GasSpecies(g);
        gas.amounts[g] = species == 0 ? (uint64_t)0 : OhAmountOf(cell, species);
    }

    return gas;
}

// 物の中の隣どうしの気体の混ざり: 膜の口と同じ式で「広さ ÷ 膜」を s² ÷ s = s にしたもの。自分 → 隣が正。
// 両側が符号違いの同じ値を出す(膜の温度 = 両側の平均・運ぶ熱は出ていく側の温度・上限は出ていく側の量の 1/8〔面 6 つと膜で負にならない〕。
// 丸めは 0 へ向かうので f(−x) = −f(x))
template <typename Table, typename GasList>
FX_FN OhFlow OhInnerGas(Table table, GasList gases, OhObject object, OhVoxelGas self, OhVoxelGas other,
                        OhVoxelHeat selfHeat, OhVoxelHeat otherHeat) {
    OhFlow flow = OhZeroFlow();
    const OhFilmScale film = OhFilmScaleOf(selfHeat.temperature, otherHeat.temperature);
    const uint64_t lengthUm = (uint64_t)(OH_CELL_UM >> object.bodyLevel);
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
        const uint32_t species = gases.GasSpecies(g);
        if (species == 0)
            continue;

        const int64_t difference = (int64_t)self.amounts[g] - (int64_t)other.amounts[g];
        const uint64_t diffusivity = (uint64_t)table.ObjectSpecies(species).diffusivityMm2PerS;
        const uint64_t multiplier = (diffusivity * lengthUm * film.diffusionQ16) >> 16;
        int64_t moved = OhMulDivToward0(difference * ((int64_t)8 << (3 * object.bodyLevel)), multiplier,
                                        (uint64_t)object.substeps * OH_DIFFUSION_DIVISOR_MEGA * OH_MEGA);
        const int64_t limit = (int64_t)((moved > 0 ? self.amounts[g] : other.amounts[g]) >> 3);
        moved = moved > limit ? limit : (moved < -limit ? -limit : moved);
        if (moved == 0)
            continue;

        // 運ぶエネルギー(OhPairGas と同じ式)
        const RxSpecies values = table.Species(species);
        const int64_t temperature = moved > 0 ? (int64_t)selfHeat.temperature : (int64_t)otherHeat.temperature;
        const int64_t perUnit = (values.h0 * 1000000) + ((int64_t)values.heatCapacity * temperature);
        const int64_t carried = OhMulDivToward0(moved, FxAbsU64(perUnit), 1000000000);
        flow.amounts[g] = moved;
        flow.energy += perUnit < 0 ? -carried : carried;
    }

    return flow;
}

// 隣のボクセルの番号の差(方向 0〜5 = −x・+x・−y・+y・−z・+z)
FX_FN int32_t OhNeighbourOffset(OhObject object, uint32_t direction) {
    const int32_t stride = direction < 2
                               ? 1
                               : (direction < 4 ? (int32_t)object.sizeX : (int32_t)(object.sizeX * object.sizeY));

    return (direction & 1) != 0 ? stride : -stride;
}

// 光を小刻みに等しく分ける(端数は後ろの小刻みへ。全部の小刻みを足すと 1 刻みの量に戻る)
FX_FN int64_t OhLightShare(int64_t lightPerTick, uint32_t substep, uint32_t substeps) {
    const int64_t steps = (int64_t)substeps;

    return ((lightPerTick * ((int64_t)substep + 1)) / steps) - ((lightPerTick * (int64_t)substep) / steps);
}

// ② の結果: 当てた後のボクセルと、セルへ出す分
struct OhVoxelStep {
    RxCell cell;
    OhFlow toCell;
    uint32_t ok;         // 0 = 量が負になる・成分の枠があふれる(試作の誤り。R8)
    uint32_t flowsZero;  // 1 = 光もセルとの流れも 0(静かなブロックの見分け。T-1098)
};

// 外に面する口(伝導・放射・気体)をまとめて。inner = 物の中の隣から入る熱と気体(伝導 + 層に面する面の混ざり。隣 → 自分が正)、
// sunlight = この小刻みに吸う光。放射は外に面する面と、層を透かす面(OH_THROUGH_SHIFT)から囲む所へ
template <typename Table, typename GasList>
FX_FN OhVoxelStep OhApplyVoxelInner(Table table, GasList gases, OhObject object, uint32_t shape, RxCell cell,
                                    OhVoxelHeat heat, RxCell air, RxThermal airThermal, uint64_t airConductivity,
                                    OhFlow inner, int64_t sunlight) {
    OhVoxelStep step;
    step.toCell = OhZeroFlow();
    const uint32_t exposed = (shape >> OH_EXPOSED_SHIFT) & OH_EXPOSED_MASK;
    const uint32_t radiating = exposed + ((shape >> OH_THROUGH_SHIFT) & OH_EXPOSED_MASK);
    if (exposed != 0) {
        const int64_t pairLength = OhPairLengthUm(object, exposed);
        const OhFilmScale film = OhFilmScaleOf(heat.temperature, airThermal.temperature);
        step.toCell.energy = OhPairConduction(object, airConductivity, pairLength, heat, airThermal.temperature, film);
        step.toCell = OhPairGas(table, gases, object, pairLength, cell, air, heat, airThermal.temperature, film,
                                step.toCell);
    }

    if (radiating != 0)
        step.toCell.energy += OhPairRadiation(table, object, radiating, cell, heat, airThermal.temperature);

    // 当てる(気体は表の気体の番号の順。足し引きの順は結果を変えない)
    step.ok = 1;
    cell.energy += sunlight + inner.energy - step.toCell.energy;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
        const OhAmountChange change = OhAddAmount(cell, gases.GasSpecies(g), inner.amounts[g] - step.toCell.amounts[g]);
        cell = change.cell;
        step.ok = step.ok & change.ok;
    }

    step.cell = cell;
    step.flowsZero = (sunlight == 0 && OhFlowIsZero(step.toCell)) ? 1u : 0u;

    return step;
}

// 層の無い物の形(物の中は伝導だけ)。inner = 物の中の伝導で入る熱
template <typename Table, typename GasList>
FX_FN OhVoxelStep OhApplyVoxel(Table table, GasList gases, OhObject object, uint32_t shape, RxCell cell,
                               OhVoxelHeat heat, RxCell air, RxThermal airThermal, uint64_t airConductivity,
                               int64_t inner, int64_t sunlight) {
    OhFlow innerFlow = OhZeroFlow();
    innerFlow.energy = inner;

    return OhApplyVoxelInner(table, gases, object, shape, cell, heat, air, airThermal, airConductivity, innerFlow,
                             sunlight);
}

// --- ③ 物ごとの和を粗い側の単位にし、1 単位に満たない分を対の端数に持ち越す ---

struct OhCarryStep {
    OhFlow carry;   // 新しい対の端数(ボクセルの単位)
    OhFlow coarse;  // セルへ渡す量(セルの単位)
};

FX_FN OhCarryStep OhSplitCarry(OhObject object, OhFlow total, OhFlow carry) {
    // 粗い側の 1 単位 = 細かい側の 8^(L − Lc) 単位。算術の右シフト = 負の側へ切る(floor)
    const uint32_t shift = (uint32_t)(3 * (object.bodyLevel - object.cellLevel));
    OhCarryStep step;
    const int64_t energy = total.energy + carry.energy;
    step.coarse.energy = energy >> shift;
    step.carry.energy = energy - (step.coarse.energy << shift);
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
        const int64_t amount = total.amounts[g] + carry.amounts[g];
        step.coarse.amounts[g] = amount >> shift;
        step.carry.amounts[g] = amount - (step.coarse.amounts[g] << shift);
    }

    return step;
}

FX_FN OhFlow OhAddFlow(OhFlow a, OhFlow b) {
    a.energy += b.energy;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g)
        a.amounts[g] += b.amounts[g];

    return a;
}

// --- ④ セルへ当てる(そのセルの物の粗い量の和)---

template <typename GasList>
FX_FN OhAmountChange OhApplyToCell(GasList gases, RxCell cell, OhFlow coarse) {
    OhAmountChange result;
    result.ok = 1;
    cell.energy += coarse.energy;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
        const OhAmountChange change = OhAddAmount(cell, gases.GasSpecies(g), coarse.amounts[g]);
        cell = change.cell;
        result.ok = result.ok & change.ok;
    }

    result.cell = cell;

    return result;
}

// --- 層を作る・返す(D-667・T-1142。セル ⇄ 層は流れとして ③ の対の端数 OhSplitCarry と ④ OhApplyToCell で受ける = 帳簿「物との受け渡し」)---

// 気体の量とエネルギーを流れの形に(層を返す時。ボクセル → セルが正)
template <typename GasList>
FX_FN OhFlow OhGasFlowOf(GasList gases, RxCell cell) {
    const OhVoxelGas gas = OhPrepareGas(gases, cell);
    OhFlow flow = OhZeroFlow();
    flow.energy = cell.energy;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g)
        flow.amounts[g] = (int64_t)gas.amounts[g];

    return flow;
}

// 層のボクセルの初めの中身 = 囲むセルの気体(同じ濃さ = 同じ数。量の数は濃さ × 0.125 m³)と、その気体の熱と化学のエネルギー
// (運ぶ気体と同じ式: 量 × (h0 + 比熱 × セルの温度))。セルの気体でない物(混ぜた草・煤・水)は移さない(層は空気の箱)
struct OhShellFill {
    RxCell cell;
    uint32_t ok;
};

template <typename Table, typename GasList>
FX_FN OhShellFill OhShellVoxelOf(Table table, GasList gases, RxCell air, int32_t airMilliKelvin) {
    OhShellFill fill;
    fill.ok = 1;
    RxCell voxel = RxMakeEmptyCell(0);
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
        const uint32_t species = gases.GasSpecies(g);
        if (species == 0)
            continue;

        const uint64_t amount = OhAmountOf(air, species);
        const RxSpecies values = table.Species(species);
        const int64_t perUnit = (values.h0 * 1000000) + ((int64_t)values.heatCapacity * (int64_t)airMilliKelvin);
        const int64_t carried = OhMulDivToward0((int64_t)amount, FxAbsU64(perUnit), 1000000000);
        voxel.energy += perUnit < 0 ? -carried : carried;
        const OhAmountChange change = OhAddAmount(voxel, species, (int64_t)amount);
        voxel = change.cell;
        fill.ok = fill.ok & change.ok;
    }

    fill.cell = voxel;

    return fill;
}

// 層のボクセルを返せるか: 中身が全部、対の面の気体(流れの形で運べる物)
template <typename Table>
FX_FN bool OhShellReturnable(Table table, RxCell cell) {
    bool returnable = true;
    for (uint32_t i = 0; i < cell.speciesCount; ++i)
        returnable = returnable && table.ObjectSpecies(RX_SPECIES_AT(cell, i)).gasIndex != OH_NO_GAS;

    return returnable;
}

// --- 層の流れ(D-667 の 2 段目・T-1143。物の箱の中の気体を、面の運動量と圧力で動かす)---
// 形は案 B の R1(case_b_flow.h)の気体だけ・音より遅い流れの極限: 面の運動量 M(真実)・面の質量 m_f = 両側の平均・速さ q = M ÷ m_f。
// 1 小刻み: ① 運動量を移した質量に比例して同じ軸の隣の面へ運ぶ(D-663)② 縦の面に浮力 ③ 圧力の射影(膨らみたい体積 e を
// その小刻みで面から出す。外の面の向こう = 囲むセルの圧力)④ 風上のボクセルの気体を体積の割合で移す(運ぶ熱は運ぶ気体と同じ式)。
// 単位(物の箱のレベル L・流れの小刻み N): 速さ q = 1 小刻みに動く体積(ボクセルの体積の Q16)・質量 = 量 × モル質量 >> 10
// (量の数は濃さ × 0.125 m³ なので、ボクセルとセルで同じ尺度)・運動量 = 質量 × q。運動のエネルギーと浮力の仕事は数えない(音より遅い極限。T-1143)。
FX_CONST uint32_t OH_FLOW_SHARE_LIMIT_Q16 = 32768;  // 1 小刻みにボクセルから出る気体の上限(半分。負にならない)
FX_CONST int64_t OH_FLOW_SPEED_LIMIT_Q16 = 65536;   // 速さの上限(1 小刻みに 1 ボクセル。桁あふれの止め)

// ボクセル(またはセル)の流れの見立て(小刻みの初めの値。両側が同じ値を読む = Jacobi)
struct OhFlowCell {
    uint64_t mass;            // 気体の質量(量 × mg/mol >> 10)
    uint64_t pressureVolume;  // Σ 気体の量 × 温度(mK)>> 8(= pV/R の尺度)
    uint32_t openQ16;         // 気体が占める体積の割合(凝縮相の体積を除く)
    int32_t temperature;      // mK
};

template <typename Table>
FX_FN OhFlowCell OhFlowCellOf(Table table, RxCell cell) {
    OhFlowCell flowCell;
    flowCell.mass = 0;
    flowCell.pressureVolume = 0;
    flowCell.temperature = RxComputeThermal(table, cell).temperature;
    uint64_t moles = 0;
    uint64_t solidVolume = 0;  // pL(量 × モル体積)
    FX_UNROLL
    for (uint32_t s = 0; s < RX_MAX_CELL_SPECIES; ++s) {
        if (s >= cell.speciesCount)
            continue;

        const uint32_t species = RX_SPECIES_AT(cell, s);
        const uint64_t amount = RX_AMOUNT_AT(cell, s);
        if (table.ObjectSpecies(species).gasIndex == OH_NO_GAS) {
            solidVolume += amount * (uint64_t)table.ObjectSpecies(species).molarVolumeMm3PerMol;
            continue;
        }

        moles += amount;
        flowCell.mass += (amount * (uint64_t)table.MolarMass(species)) >> 10;
    }

    flowCell.pressureVolume = (moles * (uint64_t)(flowCell.temperature > 0 ? flowCell.temperature : 0)) >> 8;
    // 凝縮相の割合 = 量 × モル体積(pL)÷ 0.125 m³(= 1.25 × 10^14 pL = 125000 × 10^6 × 1000。32bit の定数の積で割る。T-1078)
    const uint64_t solidShareQ16 = (uint64_t)OhMulDivToward0((int64_t)solidVolume, 65536,
                                                             OH_BOX_MEGA_MM3 * OH_MEGA * (uint64_t)1000);
    flowCell.openQ16 = solidShareQ16 >= 65536 ? 1u : (uint32_t)(65536 - solidShareQ16);

    return flowCell;
}

// 膨らみたい体積(ボクセルの体積の Q16。正 = 出ていきたい): pV/R ÷ セルの pV/R − 気体の割合
FX_FN int64_t OhFlowExcessQ16(OhFlowCell voxel, OhFlowCell cell) {
    const uint64_t divisor = cell.pressureVolume > 0 ? cell.pressureVolume : 1;

    return OhMulDivToward0((int64_t)voxel.pressureVolume, 65536, divisor) - (int64_t)voxel.openQ16;
}

// 面の質量(両側の平均。0 にしない)
FX_FN uint64_t OhFlowFaceMass(OhFlowCell a, OhFlowCell b) {
    const uint64_t mass = (a.mass + b.mass) >> 1;

    return mass > 0 ? mass : 1;
}

// 面の速さ q = M ÷ m_f(上限で止める)
FX_FN int64_t OhFlowSpeed(int64_t momentum, uint64_t faceMass) {
    const int64_t speed = OhMulDivToward0(momentum, 1, faceMass);

    return speed > OH_FLOW_SPEED_LIMIT_Q16 ? OH_FLOW_SPEED_LIMIT_Q16
                                           : (speed < -OH_FLOW_SPEED_LIMIT_Q16 ? -OH_FLOW_SPEED_LIMIT_Q16 : speed);
}

// 重さの加速(1 小刻みの速さの変わり ÷ 1 小刻み。Q16): g Δt² ÷ s = 9806650 µm/s² × 2^L ÷ (500000 µm × 3600 × N²)
FX_FN uint64_t OhFlowGravityQ16(int32_t bodyLevel, uint32_t flowSubsteps) {
    const uint64_t numerator = ((uint64_t)9806650 << bodyLevel) * 65536;
    const uint64_t steps = (uint64_t)flowSubsteps * (uint64_t)flowSubsteps;

    return OhDivideU64(numerator, (uint64_t)500000 * 3600 * steps);
}

// 縦の面の浮力の力積(運動量。上向きが正): g × (同じ体積の囲むセルの気体の質量 − 面の質量)
FX_FN int64_t OhFlowBuoyancy(uint64_t gravityQ16, OhFlowCell below, OhFlowCell above, OhFlowCell cell) {
    const uint64_t openQ16 = ((uint64_t)below.openQ16 + (uint64_t)above.openQ16) >> 1;
    const int64_t displaced = (int64_t)((cell.mass * openQ16) >> 16);
    const int64_t difference = displaced - (int64_t)OhFlowFaceMass(below, above);

    return OhMulDivToward0(difference, gravityQ16, 65536);
}

// 圧力の射影の面の重み β = (基準の質量 << 16)÷ m_f(Δq = (Ψ_i − Ψ_j)× β >> 16・ΔM = (Ψ_i − Ψ_j)× 基準の質量)
FX_FN uint64_t OhFlowFaceWeight(uint64_t referenceMass, uint64_t faceMass) {
    return OhDivideU64(referenceMass << 16, faceMass);
}

// 風上のボクセルから出る気体の割合(Q16。体積 ÷ 気体の体積)
FX_FN uint64_t OhFlowShareQ16(int64_t speedQ16, OhFlowCell upwind) {
    const uint64_t speed = FxAbsU64(speedQ16);
    const uint64_t open = upwind.openQ16 > 0 ? (uint64_t)upwind.openQ16 : 1;

    return OhDivideU64(speed << 16, open);
}

// 風上の気体を割合 share だけ移す流れ(風上 → 風下が正。運ぶ熱は運ぶ気体と同じ式 = OhInnerGas・OhShellVoxelOf)
template <typename Table, typename GasList>
FX_FN OhFlow OhFlowMoved(Table table, GasList gases, OhVoxelGas upwind, int32_t upwindMilliKelvin, uint64_t shareQ16) {
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
        const int64_t carried = OhMulDivToward0(moved, FxAbsU64(perUnit), 1000000000);
        flow.amounts[g] = moved;
        flow.energy += perUnit < 0 ? -carried : carried;
    }

    return flow;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_OBJECT_HEAT_HLSLI
