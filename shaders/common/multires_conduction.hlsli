// multires_conduction.hlsli — 多重解像度の木の上の熱の伝導(07 §1・17 §5「熱の伝導」。T-0019)。面の隣を木から探し、
// レベルの違う面でもエネルギーをビット単位で保存して受け渡す。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンスは engine/src/sim/multires_conduction.cpp
// (GPU は shaders/sim/multires_conduct.hlsli。同じ関数を呼ぶ。T-0107)。
//
// データの流れ:
//   刻みの初めのセル → セルの熱(MrThermal: 温度・コンダクタンス・熱容量の上限)
//   → 面ごとの流れ(同じレベル: 両側が同じ式で逆向きを出す / 違うレベル: 細かい側だけが計算して粗い側へ送る)
//   → セルのエネルギー(粗い側は端数にも)に足し引き → 反応(reaction.hlsli)
//
// レベルの単位(17 §1): レベル k のエネルギーの単位は 8^-k mJ。同じ物質・温度なら熱容量の数はレベルによらず、
// コンダクタンス(面積 ÷ 距離 ∝ 一辺)はそのレベルの単位で 4^k 倍になる。だから面の係数の上限は
//   min(G の係数 × 4^k, C の上限)(heat_conduction.hlsli の HcFaceLimit をレベルに広げたもの。C の上限 = C ÷ 8)。
// 違うレベルの面(細かい側 kf、粗い側 kc = kf − d): 面は細かい側のセルの面。細かい側の単位で、粗い側の上限は
//   min(粗い側の G の係数 × 4^kf, 粗い側の C の上限 × 2^d)(粗いセルの 1 面は細かい面 4^d 枚に分かれ、単位は 8^d 倍)。
//   粗いセルの 1 面の重みの和は今までどおり 1/8 以下(4^d 枚 × 2^d ÷ 8^d)なので、行き過ぎない。
// 細かい側が送る F(細かい単位)は、粗い側では F × 2^-3d 単位。粗い側は整数部をセルに、2^-64 単位の端数を端数のブロックの
// エネルギーの端数に足す(ADR-0015 の端数。3d ≤ 64 なら細かい単位を粗い側で正確に表せる)。
//   - 3d > 64: 2^-64 で表せない下位は細かい側に残す(送らない)。
//   - 粗いブロックに端数の枠が無い(枠が足りない): 粗い側の整数の単位の倍数だけ送り、残りは細かい側に残す。
// どの場合も「細かい側が減らした量 = 粗い側が増やした量」なので、エネルギーはビット単位で保存される(D-206)。
// 温度は整数部だけから求める(端数は反応・輸送に使わない。17 §5)。
#ifndef BICAMERAL_MULTIRES_CONDUCTION_HLSLI
#define BICAMERAL_MULTIRES_CONDUCTION_HLSLI

#include "heat_conduction.hlsli"
#include "multires_activity.hlsli"

MR_NAMESPACE_BEGIN

// --- 面の隣の種類 -------------------------------------------------------------------------------
FX_CONST uint32_t MR_NEIGHBOR_NONE = 0;     // 世界の外・影のブロックの外(断熱)
FX_CONST uint32_t MR_NEIGHBOR_SAME = 1;     // 同じレベルのセル(両側が同じ式で計算する)
FX_CONST uint32_t MR_NEIGHBOR_COARSER = 2;  // 粗いセル(自分が計算して送る)
FX_CONST uint32_t MR_NEIGHBOR_FINER = 3;    // 細かいセルに覆われた面(細かい側が計算して送ってくる)

FX_CONST uint64_t MR_UINT64_MAX = FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu);

// --- 構造体 ------------------------------------------------------------------------------------

// セルの面 1 つの先
struct MrFaceNeighbor {
    uint32_t kind;  // MR_NEIGHBOR_*
    uint32_t slot;
    uint32_t index;
    uint32_t gap;  // 粗い側とのレベルの差 d(COARSER のときだけ。1 以上)
};

// 伝導に使うセルの熱(セルから決まる値)
struct MrThermal {
    uint32_t temperature;    // mK(負なら 0)
    uint32_t conductance;    // mW/K(レベル 0 の大きさのセルとして。HcCellConductance)
    uint64_t capacityLimit;  // 熱容量の上限の係数(C ÷ 8。2^-32 単位 / mK。レベルによらない)
};

// エネルギーの変化(2 の補数の 128bit = whole · 2^64 + fraction。単位はそのセルのレベルの単位)
struct MrEnergyDelta {
    int64_t whole;
    uint64_t fraction;
};

// 違うレベルの面の受け渡し
struct MrCrossTransfer {
    int64_t fineDelta;  // 細かいセルのエネルギーの変化(細かい単位。= −送った量)
    MrEnergyDelta coarseDelta;
};

// --- 小さな関数 --------------------------------------------------------------------------------

// 左へ shift ビット(はみ出すなら最大で止める)
FX_FN uint64_t MrSaturatingShiftLeft(uint64_t value, uint32_t shift) {
    if (value == 0)
        return 0;

    if (shift >= 64 || value > (MR_UINT64_MAX >> shift))
        return MR_UINT64_MAX;

    return value << shift;
}

// × 4^level(level < 0 なら ÷ 4^-level の切り捨て)
FX_FN uint64_t MrScaleByLevel(uint64_t value, int32_t level) {
    if (level >= 0)
        return MrSaturatingShiftLeft(value, 2 * (uint32_t)level);

    const uint32_t shift = 2 * (uint32_t)(-level);

    return shift >= 64 ? 0 : value >> shift;
}

FX_FN uint64_t MrMin64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

FX_FN MrEnergyDelta MrMakeEnergyDelta() {
    MrEnergyDelta delta;
    delta.whole = 0;
    delta.fraction = 0;

    return delta;
}

FX_FN MrEnergyDelta MrAddEnergyDelta(MrEnergyDelta sum, MrEnergyDelta value) {
    MrEnergyDelta result;
    result.fraction = sum.fraction + value.fraction;
    const int64_t carry = result.fraction < value.fraction ? 1 : 0;
    result.whole = sum.whole + value.whole + carry;

    return result;
}

FX_FN bool MrEnergyDeltaIsZero(MrEnergyDelta delta) {
    return delta.whole == 0 && delta.fraction == 0;
}

// --- セルの熱 ----------------------------------------------------------------------------------

template <typename Table>
FX_FN MrThermal MrCellThermal(Table table, RxCell cell) {
    const RxThermal thermal = RxComputeThermal(table, cell);

    MrThermal result;
    result.temperature = thermal.temperature > 0 ? (uint32_t)thermal.temperature : 0;
    result.conductance = HcCellConductance(table, cell);
    result.capacityLimit = (thermal.heatCapacity >> HC_CAPACITY_SHIFT) * HC_LIMIT_PER_CAPACITY;

    return result;
}

// コンダクタンスの係数を、面がレベル level のセルの面の時の、そのレベルの単位で
FX_FN uint64_t MrConductanceLimit(uint32_t conductance, int32_t level) {
    return MrScaleByLevel((uint64_t)conductance * HC_LIMIT_PER_CONDUCTANCE, level);
}

// 自分のレベルの面の係数の上限
FX_FN uint64_t MrFaceLimit(MrThermal thermal, int32_t level) {
    return MrMin64(MrConductanceLimit(thermal.conductance, level), thermal.capacityLimit);
}

FX_FN HcThermalCache MrMakeFlowSide(MrThermal thermal, uint64_t faceLimit) {
    HcThermalCache side;
    side.temperature = thermal.temperature;
    side.conductance = thermal.conductance;
    side.faceLimit = faceLimit;

    return side;
}

// 同じレベル level の面の流れ(自分 → 隣が正、そのレベルの単位)。両側が同じ値を逆向きに出す
FX_FN int64_t MrSameLevelFlow(MrThermal self, MrThermal neighbor, int32_t level) {
    return HcFaceFlow(MrMakeFlowSide(self, MrFaceLimit(self, level)),
                      MrMakeFlowSide(neighbor, MrFaceLimit(neighbor, level)));
}

// 細かいセル(レベル fineLevel)→ 粗いセル(fineLevel − gap)の面の流れ(細かい単位。細かい → 粗いが正)
FX_FN int64_t MrCrossLevelFlow(MrThermal fine, MrThermal coarse, int32_t fineLevel, uint32_t gap) {
    const uint64_t coarseLimit = MrMin64(MrConductanceLimit(coarse.conductance, fineLevel),
                                         MrSaturatingShiftLeft(coarse.capacityLimit, gap));

    return HcFaceFlow(MrMakeFlowSide(fine, MrFaceLimit(fine, fineLevel)), MrMakeFlowSide(coarse, coarseLimit));
}

// 細かい側の流れ flow を、粗い側で表せる分だけ送る(先頭の「データの流れ」)。coarseHasFraction = 粗いブロックに端数の枠がある
FX_FN MrCrossTransfer MrSplitCrossFlow(int64_t flow, uint32_t gap, bool coarseHasFraction) {
    const bool negative = flow < 0;
    const uint64_t magnitude = negative ? (uint64_t)(-flow) : (uint64_t)flow;
    const uint32_t shift = 3 * gap;

    // --- 送る大きさ(細かい単位)と、粗い側の整数部・端数(2^-64 単位)---
    uint64_t sent = 0;
    uint64_t whole = 0;
    uint64_t fraction = 0;
    if (!coarseHasFraction) {
        whole = shift >= 64 ? 0 : magnitude >> shift;
        sent = shift >= 64 ? 0 : whole << shift;
    } else if (shift < 64) {
        sent = magnitude;
        whole = magnitude >> shift;
        fraction = magnitude << (64 - shift);
    } else if (shift == 64) {
        sent = magnitude;
        fraction = magnitude;
    } else if (shift - 64 < 64) {
        fraction = magnitude >> (shift - 64);
        sent = fraction << (shift - 64);
    }

    MrCrossTransfer transfer;
    if (!negative) {
        transfer.fineDelta = -(int64_t)sent;
        transfer.coarseDelta.whole = (int64_t)whole;
        transfer.coarseDelta.fraction = fraction;

        return transfer;
    }

    // --- 粗い → 細かい: 粗い側は (整数部, 端数) の 2 の補数を引く ---
    transfer.fineDelta = (int64_t)sent;
    transfer.coarseDelta.fraction = 0 - fraction;
    transfer.coarseDelta.whole = -(int64_t)whole - (fraction != 0 ? 1 : 0);

    return transfer;
}

// --- 面の隣 ------------------------------------------------------------------------------------

FX_FN MrFaceNeighbor MrMakeFaceNeighbor(uint32_t kind, uint32_t slot, uint32_t index, uint32_t gap) {
    MrFaceNeighbor neighbor;
    neighbor.kind = kind;
    neighbor.slot = slot;
    neighbor.index = index;
    neighbor.gap = gap;

    return neighbor;
}

// 枠 slot(見出し block)のセル index の面 face(軸 = face / 2、正の向き = face % 2)の先のセル。
// 同じブロックの中なら、覆われていれば細かい側。外なら、先のセルを含む最も細かい本物のブロックを索引で上へ引く
// (multires_activity.hlsli の MrFindContaining)。見つかったブロックが粗ければ、その覆われていないセル
// (覆われていれば、覆う子のほうが先に見つかる)。影のブロックは自分の中だけ(外は断熱。親との受け渡しは引き戻し)
template <typename Tree>
FX_FN MrFaceNeighbor MrFindFaceNeighbor(Tree tree, uint32_t slot, MrBlock block, uint32_t index, uint32_t face,
                                        int32_t rootLevel) {
    const uint32_t axis = face >> 1;
    const int64_t offset = (face & 1u) != 0 ? 1 : -1;
    int64_t x = (int64_t)MrCellX(index) + (axis == 0 ? offset : 0);
    int64_t y = (int64_t)MrCellY(index) + (axis == 1 ? offset : 0);
    int64_t z = (int64_t)MrCellZ(index) + (axis == 2 ? offset : 0);
    const int64_t edge = (int64_t)MR_BLOCK_EDGE;

    // --- 同じブロックの中 ---
    if (x >= 0 && x < edge && y >= 0 && y < edge && z >= 0 && z < edge) {
        const uint32_t inside = MrCellIndex((uint32_t)x, (uint32_t)y, (uint32_t)z);
        const uint32_t kind = MrIsCoveredCell(block, inside) ? MR_NEIGHBOR_FINER : MR_NEIGHBOR_SAME;

        return MrMakeFaceNeighbor(kind, slot, inside, 0);
    }

    if (block.kind != MR_BLOCK_REAL)
        return MrMakeFaceNeighbor(MR_NEIGHBOR_NONE, MR_NO_BLOCK, 0, 0);

    // --- 外: 先のセルを含む最も細かい本物のブロック ---
    x += block.originX;
    y += block.originY;
    z += block.originZ;
    const uint32_t found = MrFindContaining(tree, block.level, x, y, z, rootLevel);
    if (found == MR_NO_BLOCK)
        return MrMakeFaceNeighbor(MR_NEIGHBOR_NONE, MR_NO_BLOCK, 0, 0);

    const MrBlock other = tree.Block(found);
    const uint32_t gap = (uint32_t)(block.level - other.level);
    const uint32_t neighborIndex = MrCellIndex((uint32_t)((x >> gap) - other.originX),
                                               (uint32_t)((y >> gap) - other.originY),
                                               (uint32_t)((z >> gap) - other.originZ));
    if (gap == 0) {
        const uint32_t kind = MrIsCoveredCell(other, neighborIndex) ? MR_NEIGHBOR_FINER : MR_NEIGHBOR_SAME;

        return MrMakeFaceNeighbor(kind, found, neighborIndex, 0);
    }

    FX_ASSERT(!MrIsCoveredCell(other, neighborIndex));

    return MrMakeFaceNeighbor(MR_NEIGHBOR_COARSER, found, neighborIndex, gap);
}

MR_NAMESPACE_END

#endif  // BICAMERAL_MULTIRES_CONDUCTION_HLSLI
