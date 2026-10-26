// placement_refine.hlsli — 細かくする時の「置かれ方で配る」を GPU で回す(T-1159。D-664・ADR-0469 の 2-1・20 §10.5・17 §3.2)。
// 1 つずつの式は shaders/common/state_field.hlsli の SfSettle*(CPU と 1 つの源)。ここは親のセル 1 つを子 2³ へ配る回しで、
// CPU リファレンスは engine/src/sim/placement_refine.cpp の PlaceRefineCell(columns = 2・depth = 2。答えはビット単位で同じ。
// tests/gpu_placement_refine_test.cpp)。1 スレッド = 子 1 つで、子の番号は MrChildCell の j(x が bit 0・重さの向き y が bit 1・z が bit 2)。
// CPU の PlacedChildren::ChildIndex(x, z, 段) とは並びが違う(写し替えは sim::PlaceIndexOfMrChild)。
//
// 回しの順は CPU と同じ: 寄せるか → x で分ける → z で分ける → 列ごとに縦へ寄せる → 気体を子の空きの体積で配り直す。
// 気体の配り直しは子 8 つの空きの体積の和が要るので、呼ぶ側が子 8 つの PrChildOpenVolume を集めて PrGasShare に渡す(グループ共有のメモリなど)。
//
// 呼ぶ側が include の前に決めるもの(物質の表と親のセルの読み方。書き換えた配列を動的な添字で読まない約束〔T-0397〕のため、
// 物質の回しはバッファから毎回読む):
//   PR_MATERIAL_COUNT      物質の数
//   PR_MATERIAL(m)         物質 m の uint3(SF_SETTLE_* の種類・密度 kg/m³・粒の詰まり %)
//   PR_PARENT_MASS(m)      親のセルの物質 m の量(mg。int64_t)
//   PR_MATERIAL_MOLAR_VOLUME(m)  (任意)物質 m のモル体積(mm³/mol)。0 でなければ量は µmol で体積はこちらから(世界の木。T-1176)。既定は 0
#ifndef BICAMERAL_PLACEMENT_REFINE_HLSLI
#define BICAMERAL_PLACEMENT_REFINE_HLSLI

#include "common/state_field.hlsli"

#ifndef PR_MATERIAL_MOLAR_VOLUME
#define PR_MATERIAL_MOLAR_VOLUME(m) 0u
#endif

// --- 親のセル 1 つの配り方(子 8 つで同じ)---
struct PrCell {
    bool settled;             // 寄せる(false なら同じ数で配る = SAME)
    uint64_t childVolumeMm3;  // 子 1 つの体積
    int64_t slopeXQ16;        // 呼ぶ側が隣の親から読んだ表面の傾き(SfSurfaceSlopeQ16)
    int64_t slopeZQ16;
    int64_t fullnessQ16;  // 親の詰まり(器 = 子 8 つ)
};

// 2 列への分け方: 列 0 の重みと和(重みの和が 0 以下なら同じ重み。CPU の SplitColumns と同じ)
struct PrSplit {
    uint64_t weight0;
    uint64_t weightSum;
};

PrSplit PrMakeSplit(int64_t fullnessQ16, int64_t slopeQ16) {
    const int64_t slope = SfClampColumnSlopeQ16(fullnessQ16, slopeQ16, 2);
    const int64_t weight0 = SfColumnWeight(fullnessQ16, slope, 2, 0);
    const int64_t weight1 = SfColumnWeight(fullnessQ16, slope, 2, 1);
    PrSplit split;
    split.weight0 = 1;
    split.weightSum = 2;
    if (weight0 + weight1 > 0) {
        split.weight0 = (uint64_t)weight0;
        split.weightSum = (uint64_t)(weight0 + weight1);
    }

    return split;
}

// 量を 2 列に分けた列 column の分(端数は最後の列へ)
int64_t PrSplitShare(int64_t massMg, PrSplit split, uint32_t column) {
    const int64_t first = FxMulDivS64(massMg, split.weight0, split.weightSum);

    return column == 0 ? first : massMg - first;
}

SfSettleSum PrAddMaterial(SfSettleSum sum, uint32_t m, int64_t massMg) {
    const uint3 material = PR_MATERIAL(m);

    return SfAddSettleOf(sum, material.x, material.y, material.z, PR_MATERIAL_MOLAR_VOLUME(m), massMg);
}

// --- 寄せるか・親の詰まり(blocked = 物を含む)---
PrCell PrMakeCell(bool blocked, int64_t slopeXQ16, int64_t slopeZQ16, uint64_t childVolumeMm3) {
    SfSettleSum sum = SfMakeSettleSum();
    for (uint32_t m = 0; m < PR_MATERIAL_COUNT; ++m)
        sum = PrAddMaterial(sum, m, PR_PARENT_MASS(m));

    const uint64_t capacity = childVolumeMm3 * 8;
    PrCell cell;
    cell.settled = !blocked && SfSettles(SfSettledVolume(sum), capacity, sum.anchored);
    cell.childVolumeMm3 = childVolumeMm3;
    cell.slopeXQ16 = slopeXQ16;
    cell.slopeZQ16 = slopeZQ16;
    cell.fullnessQ16 = cell.settled ? SfSettleFullnessQ16(SfSettledVolume(sum), capacity, false) : 0;

    return cell;
}

// --- 子 (x, z) の列: x で分けた板の詰まりから z の分け方を作る ---
struct PrColumn {
    PrSplit splitX;
    PrSplit splitZ;
    uint64_t settledVolume;  // 列(子 2 つ)の沈み固まった体積
    bool settles;            // 列を縦へ寄せる
    uint32_t topRow;         // 一番上の詰まった子(下から)
};

int64_t PrColumnMass(PrColumn column, uint32_t m, uint32_t x, uint32_t z) {
    return PrSplitShare(PrSplitShare(PR_PARENT_MASS(m), column.splitX, x), column.splitZ, z);
}

PrColumn PrMakeColumn(PrCell cell, uint32_t x, uint32_t z) {
    PrColumn column;
    column.splitX = PrMakeSplit(cell.fullnessQ16, cell.slopeXQ16);

    // --- 板 x の詰まり(器 = 子 4 つ)で z を分ける ---
    SfSettleSum slab = SfMakeSettleSum();
    for (uint32_t m = 0; m < PR_MATERIAL_COUNT; ++m)
        slab = PrAddMaterial(slab, m, PrSplitShare(PR_PARENT_MASS(m), column.splitX, x));

    const int64_t slabFullness = SfSettleFullnessQ16(SfSettledVolume(slab), cell.childVolumeMm3 * 4, slab.anchored);
    column.splitZ = PrMakeSplit(slabFullness, cell.slopeZQ16);

    // --- 列(子 2 つ)を縦へ寄せるか ---
    SfSettleSum columnSum = SfMakeSettleSum();
    for (uint32_t m = 0; m < PR_MATERIAL_COUNT; ++m)
        columnSum = PrAddMaterial(columnSum, m, PrColumnMass(column, m, x, z));

    column.settledVolume = SfSettledVolume(columnSum);
    column.settles = SfSettles(column.settledVolume, cell.childVolumeMm3 * 2, columnSum.anchored);
    column.topRow = column.settles ? SfSettleTopChild(column.settledVolume, cell.childVolumeMm3) : 0;

    return column;
}

// --- 子 j の物質 m の量(気体は列の中で同じ数。空きで配り直すのは PrGasShare)---
int64_t PrSameShare(int64_t massMg, uint32_t j) {
    return (massMg / 8) + (j == 0 ? massMg % 8 : 0);
}

int64_t PrChildMass(PrCell cell, PrColumn column, uint32_t m, uint32_t j) {
    if (!cell.settled)
        return PrSameShare(PR_PARENT_MASS(m), j);

    const uint32_t x = j & 1u;
    const uint32_t row = (j >> 1) & 1u;
    const uint32_t z = j >> 2;
    const uint32_t kind = PR_MATERIAL(m).x;
    const int64_t columnMass = PrColumnMass(column, m, x, z);
    if (!column.settles || kind == SF_SETTLE_GAS || kind == SF_SETTLE_ANCHORED)
        return (columnMass / 2) + (row == 0 ? columnMass % 2 : 0);

    const int64_t below = SfSettleBelowShare(columnMass, cell.childVolumeMm3, column.settledVolume);
    if (row < column.topRow)
        return below;

    return row == column.topRow ? columnMass - (below * (int64_t)column.topRow) : 0;
}

// --- 子 j の重み: 凝縮相の量(mg)と空きの体積(mm³。CPU の PlaceChildWeights)---
struct PrChildWeights {
    uint64_t condensedMg;
    uint64_t openMm3;
};

PrChildWeights PrChildWeightsOf(PrCell cell, PrColumn column, uint32_t j) {
    uint64_t condensedMass = 0;
    uint64_t condensedVolume = 0;
    for (uint32_t m = 0; m < PR_MATERIAL_COUNT; ++m) {
        const uint3 material = PR_MATERIAL(m);
        const int64_t mass = PrChildMass(cell, column, m, j);
        if (material.x == SF_SETTLE_GAS || mass <= 0)
            continue;

        condensedMass += (uint64_t)mass;
        condensedVolume += SfMaterialVolume(mass, material.y, PR_MATERIAL_MOLAR_VOLUME(m));
    }

    PrChildWeights weights;
    weights.condensedMg = condensedMass;
    weights.openMm3 = SfOpenVolume(cell.childVolumeMm3, condensedVolume);

    return weights;
}

// --- 気体を子の空きの体積で配り直す(寄せた時だけ。CPU の SpreadGasToOpen)---
// openMm3[k] は子 k の PrChildWeightsOf(...).openMm3(呼ぶ側が集める)。和が 0 なら列の中の同じ数のまま。残りは子 7 へ
int64_t PrGasShare(int64_t columnShareMg, int64_t parentMassMg, uint64_t ownOpenMm3, uint64_t openSumMm3,
                   int64_t othersGivenMg, uint32_t j) {
    if (openSumMm3 == 0)
        return columnShareMg;

    if (j != 7)
        return FxMulDivS64(parentMassMg, ownOpenMm3, openSumMm3);

    return parentMassMg - othersGivenMg;
}

#endif  // BICAMERAL_PLACEMENT_REFINE_HLSLI
