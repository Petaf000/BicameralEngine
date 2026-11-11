// transport_conduction_parts.hlsli — 伝導の当てる段の、混ざったセル(層 + 気体)の面を部分ごとに流す道(ADR-0512 の 2・T-1230 段 2 ①)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU は engine/src/sim/multires_conduction.cpp の伝導の世界が呼ぶ(GPU は段 3)。
//
// 一つの芯: 面の片側を「部分つきの片側」(phase_layer.hlsli の PlPartSide)にし、両側の部分の組を重なりで流す(PlPairOverlapsOf)。
//   組ごとの片側の係数は今の伝導の種類(transport_conduction.hlsli)と同じ置き方: 自分の側 = min(G(重なり) × 4^(k − σ), 熱容量の上限(重なり))、
//   粗い側 = min(G(重なり) × 4^(kf − σf), 熱容量の上限(重なり) × 2^d ÷ 4^(σf − σc))(MrSubstepFaceLimit・MrSubstepCoarseLimit と同じ式)。
//   混ざらないセル同士の面はこの道を通らない(世界が今の TxSideFaceFlow を呼ぶ = ビット単位で同じ)。
// 当てる先(ADR-0512 の 2): 和はセルのエネルギーへ(今の変化の表)。層の部分が出した・受けた分は世界の口 AddCondensed で部分の列へ(Ec の変化)。
//   違うレベルの面: 粗い側の部分の列へは送った分の整数部だけ(0 方向の切り捨て。端数はセルのエネルギーにだけ = 気体の側)。
//   粗いセルの横の面の層の高さ h は、細かい面が覆う段 j(粗い面の高さを 2^d 段に分けた j 番目。細かいセルの y の下の d ビット)へ写す:
//   段の中の層の高さ = clamp(h × 2^d − j, 0, 1)(Q16。下の d ビットと割り算なしのシフトだけで出す = 保存は和の側・決定的)。
// 浮動小数点は使わない(04 §4)。64bit の引き算と同じ値どうしの比べを並べない(段の比べは 32bit)。
#ifndef BICAMERAL_TRANSPORT_CONDUCTION_PARTS_HLSLI
#define BICAMERAL_TRANSPORT_CONDUCTION_PARTS_HLSLI

#include "multires_conduction.hlsli"
#include "phase_layer.hlsli"
#include "transport_conduction.hlsli"

MR_NAMESPACE_BEGIN

// セルの面 face の部分つきの片側の種類(0 = 下の面・1 = 上の面・2 = 横の面)
FX_CONST uint32_t TX_PART_SIDE_BOTTOM = 0;
FX_CONST uint32_t TX_PART_SIDE_TOP = 1;
FX_CONST uint32_t TX_PART_SIDE_LATERAL = 2;

FX_FN uint32_t TxPartSideKind(uint32_t face) {
    if (face == PL_FACE_BOTTOM)
        return TX_PART_SIDE_BOTTOM;

    return face == PL_FACE_TOP ? TX_PART_SIDE_TOP : TX_PART_SIDE_LATERAL;
}

// 重なり overlapQ16 を受け持つ片側の係数(自分のレベルの単位)。conductionLevel = 細かい側のレベル − 小刻みの指数。
// 粗い側は gap(レベルの差)と repeatShift(2 × (細かい側の小刻み − 粗い側の小刻み))。自分・同じレベルは 0
FX_FN HcThermalCache TxPartCache(PlSideSlot slot, uint32_t overlapQ16, int32_t conductionLevel, uint32_t gap,
                                 uint32_t repeatShift) {
    const uint64_t byConductance = MrConductanceLimit(PlSlotConductance(slot, overlapQ16), conductionLevel);
    const uint64_t byCapacity = MrSaturatingShiftLeft(PlSlotCapacityLimit(slot, overlapQ16), gap) >> repeatShift;

    return MrFlowSide(slot.temperature, MrMin64(byConductance, byCapacity));
}

FX_FN int64_t TxPartPairFlow(PlSideSlot self, PlSideSlot other, uint32_t overlapQ16, int32_t conductionLevel,
                             uint32_t otherGap, uint32_t otherRepeatShift) {
    if (overlapQ16 == 0)
        return 0;

    return HcFaceFlow(TxPartCache(self, overlapQ16, conductionLevel, 0, 0),
                      TxPartCache(other, overlapQ16, conductionLevel, otherGap, otherRepeatShift));
}

// 面の流れ(自分 → 相手が正。自分のレベルの単位)と層の分(PlPartFaceFlow と同じ組み方)
FX_FN PlFaceFlow TxPartFaceFlow(PlPartSide self, PlPartSide other, int32_t conductionLevel, uint32_t otherGap,
                                uint32_t otherRepeatShift) {
    const PlPairOverlaps overlaps = PlPairOverlapsOf(self, other);
    const int64_t lowerLower = TxPartPairFlow(self.lower, other.lower, overlaps.lowerLower, conductionLevel, otherGap,
                                              otherRepeatShift);
    const int64_t upperUpper = TxPartPairFlow(self.upper, other.upper, overlaps.upperUpper, conductionLevel, otherGap,
                                              otherRepeatShift);
    const int64_t lowerUpper = TxPartPairFlow(self.lower, other.upper, overlaps.lowerUpper, conductionLevel, otherGap,
                                              otherRepeatShift);
    const int64_t upperLower = TxPartPairFlow(self.upper, other.lower, overlaps.upperLower, conductionLevel, otherGap,
                                              otherRepeatShift);

    PlFaceFlow flow;
    flow.total = lowerLower + upperUpper + lowerUpper + upperLower;
    flow.selfCondensed = (self.lower.part == PL_PART_CONDENSED ? lowerLower + lowerUpper : 0) +
                         (self.upper.part == PL_PART_CONDENSED ? upperUpper + upperLower : 0);
    flow.neighborCondensed = (other.lower.part == PL_PART_CONDENSED ? lowerLower + upperLower : 0) +
                             (other.upper.part == PL_PART_CONDENSED ? upperUpper + lowerUpper : 0);

    return flow;
}

// 粗いセルの横の片側を、細かい面が覆う段 row(0〜2^gap − 1)の上へ写す(段の中の層の高さ = clamp(h × 2^d − row, 0, 1))。
// 段が全部層なら層の部分 1 つ・全部気体なら気体の部分 1 つ。部分 1 つの片側(混ざらないセル・下/上の面)はそのまま
FX_FN PlPartSide TxPartSideOnRow(PlPartSide coarse, uint32_t gap, uint32_t row) {
    if (coarse.splitQ16 >= PL_FULL_Q16 || gap == 0)
        return coarse;

    // h × 2^d の整数部(境のある段)と端数(段の中の高さ。Q16)。gap が 16 を超える時は段を 2^(d − 16) ずつまとめて比べる(h の Q16 より細かく分けられない)
    const uint32_t shift = gap < 16u ? gap : 16u;
    const uint32_t scaled = coarse.splitQ16 << shift;
    const uint32_t boundaryRow = scaled >> 16;
    const uint32_t within = scaled & 0xFFFFu;
    const uint32_t rowOnScale = gap > 16u ? row >> (gap - 16u) : row;
    PlPartSide side = coarse;
    if (rowOnScale < boundaryRow) {
        side.splitQ16 = PL_FULL_Q16;
        return side;
    }

    if (rowOnScale > boundaryRow || within == 0) {
        side.lower = coarse.upper;
        side.splitQ16 = PL_FULL_Q16;
        return side;
    }

    side.splitQ16 = within;

    return side;
}

// 層の上の面(層 → 気体が正。自分のレベルの単位。PlInnerFlow と同じ直列の値・両側の上限は部分の熱容量の 1/8)
FX_FN int64_t TxPartInnerFlow(PlSideSlot condensed, PlSideSlot gas, uint64_t seriesConductance,
                              int32_t conductionLevel) {
    const uint64_t byConductance = MrConductanceLimit(PlClampU32(seriesConductance), conductionLevel);

    return HcFaceFlow(MrFlowSide(condensed.temperature, MrMin64(byConductance, condensed.capacityLimit)),
                      MrFlowSide(gas.temperature, MrMin64(byConductance, gas.capacityLimit)));
}

// 粗い側の部分の列へ送る分: 細かい単位の層の変化 condensed を、送った分の割合(sent ÷ flow)で縮め、粗い単位の整数部(0 方向の切り捨て)に
FX_FN int64_t TxScaleCondensed(int64_t condensed, int64_t sent, int64_t flow) {
    if (flow == 0 || sent == 0 || condensed == 0)
        return 0;

    if (sent == flow)
        return condensed;

    return FxMulDivS64(condensed, FxAbsU64(sent), FxAbsU64(flow));
}

FX_FN int64_t TxCoarseCondensedWhole(int64_t condensed, uint32_t gap) {
    const uint32_t shift = 3 * gap;
    if (shift >= 63)
        return 0;

    const uint64_t magnitude = FxAbsU64(condensed) >> shift;

    return condensed < 0 ? -(int64_t)magnitude : (int64_t)magnitude;
}

// --- セルの部分の見立て(見立ての部分 CV_VIEW_PHASE_LAYER。世界が刻みの初めの値から作る。混ざらないセルは mixed = 0 だけ)---------------
// 部分つきの片側は面の向きで 3 つ(下の面・上の面・横の面)。構造体の配列を動的な添字で読まないので名前の付いた欄に持つ
struct TxCellParts {
    PlPartSide bottom;
    PlPartSide top;
    PlPartSide lateral;          // lower = 層・upper = 気体(層の上の面もこの 2 つの片側の温度と熱容量の上限を使う)
    uint64_t seriesConductance;  // 層の上の面の値(mW/K。PlInnerConductance)
    uint32_t mixed;
    uint32_t pad;
};

FX_FN TxCellParts TxMakeCellParts(PlParts parts, PlLayer layer) {
    TxCellParts cellParts;
    cellParts.bottom = PlMixedSide(parts, layer, PL_FACE_BOTTOM, layer.heightQ16);
    cellParts.top = PlMixedSide(parts, layer, PL_FACE_TOP, layer.heightQ16);
    cellParts.lateral = PlMixedSide(parts, layer, 0, layer.heightQ16);
    cellParts.seriesConductance = PlInnerConductance(parts, layer);
    cellParts.mixed = 1;
    cellParts.pad = 0;

    return cellParts;
}

FX_FN PlPartSide TxCellPartSide(TxCellParts parts, uint32_t face) {
    const uint32_t kind = TxPartSideKind(face);
    if (kind == TX_PART_SIDE_BOTTOM)
        return parts.bottom;

    return kind == TX_PART_SIDE_TOP ? parts.top : parts.lateral;
}

// 混ざらないセルの片側(見立ての熱から。PlWholeSlot と同じ値: 熱容量の上限 = MrThermal の capacityLimit)
FX_FN PlPartSide TxWholePartSide(MrThermal thermal) {
    PlSideSlot slot;
    slot.temperature = thermal.temperature;
    slot.conductance = thermal.conductance;
    slot.capacityLimit = thermal.capacityLimit;
    slot.capacityPerQ16 = thermal.capacityLimit >> 16;
    slot.part = PL_PART_WHOLE;
    slot.pad = 0;

    return PlSingleSide(slot);
}

// --- 当てる段(伝導・混ざったセルのある世界のセル 1 つの 6 面。TxFlowCell と同じ 1 流す → 3 当てる → 5 帳簿 → 6 起こす)-------------
// 世界の口は TxFlowCell の口に 2 つ足す(ADR-0512 の 2・司令塔の一旦の決定):
//   - TxCellParts Parts(uint32_t slot, uint32_t index)(見立ての部分 CV_VIEW_PHASE_LAYER。刻みの初めの値)
//   - void AddCondensed(uint32_t slot, uint32_t index, int64_t delta)(層の部分の変化 = Ec の変化。そのセルのレベルの単位)
// どちらの側も混ざらない面は今の TxSideFaceFlow(ビット単位で同じ)。混ざったセルの面は部分の道(TxPartFaceFlow)。
// 同じレベルの面は両側が同じ式で逆向きの値(層の分も PlPairOverlapsOf の対称で逆向き)。違うレベルの面は細かい側だけが計算する
template <typename World>
FX_FN bool TxFlowConductionCell(TxConduction kind, World world, uint32_t slot, MrBlock block, uint32_t index) {
    const TxSelfSide side = TxMakeSelfSide(kind, world.View(slot, index), block.level);
    const TxCellParts selfParts = world.Parts(slot, index);
    const int32_t conductionLevel = block.level - (int32_t)side.shift;
    MrEnergyDelta own = MrMakeEnergyDelta();
    int64_t ownCondensed = 0;
    bool crossMoved = false;
    for (uint32_t face = 0; face < MR_FACES; ++face) {
        const MrFaceNeighbor neighbor = world.Neighbor(slot, block, index, face);
        if (neighbor.kind != MR_NEIGHBOR_SAME && neighbor.kind != MR_NEIGHBOR_COARSER &&
            neighbor.kind != MR_NEIGHBOR_EDGE)
            continue;

        // --- 1 流す(相手の片側。開いた世界の端の外のセルは混ざらない)---
        PlPartSide otherSide;
        uint32_t otherMixed = 0;
        uint32_t gap = 0;
        uint32_t repeatShift = 0;
        if (neighbor.kind == MR_NEIGHBOR_EDGE) {
            otherSide = TxWholePartSide(world.Outside(neighbor.index, block.level).thermal);
        } else {
            const TxCellParts otherParts = world.Parts(neighbor.slot, neighbor.index);
            otherMixed = otherParts.mixed;
            otherSide = otherMixed != 0 ? TxCellPartSide(otherParts, face ^ 1u)
                                        : TxWholePartSide(world.View(neighbor.slot, neighbor.index).thermal);
            if (neighbor.kind == MR_NEIGHBOR_COARSER) {
                gap = neighbor.gap;
                repeatShift = 2 * (side.shift - kind.SubcycleShift(world.Level(neighbor.slot)));
                const uint32_t mask = gap < 32u ? (1u << gap) - 1u : 0xFFFFFFFFu;
                const uint32_t row = (uint32_t)(block.originY + (int64_t)MrCellY(index)) & mask;
                otherSide = TxPartSideOnRow(otherSide, gap, row);
            }
        }

        PlFaceFlow flow;
        flow.selfCondensed = 0;
        flow.neighborCondensed = 0;
        if (selfParts.mixed == 0 && otherMixed == 0)
            flow.total = TxSideFaceFlow(kind, world, side, neighbor);
        else
            flow = TxPartFaceFlow(selfParts.mixed != 0 ? TxCellPartSide(selfParts, face)
                                                       : TxWholePartSide(world.View(slot, index).thermal),
                                  otherSide, conductionLevel, gap, repeatShift);

        // --- 3 当てる・5 帳簿(同じレベル・世界の端は自分の分だけ)---
        if (neighbor.kind == MR_NEIGHBOR_EDGE) {
            own.whole -= flow.total;
            ownCondensed -= flow.selfCondensed;
            world.AddEdge(slot, index, flow.total);
            continue;
        }

        if (neighbor.kind == MR_NEIGHBOR_SAME) {
            own.whole -= flow.total;
            ownCondensed -= flow.selfCondensed;
            continue;
        }

        // 粗い側: 表せる分だけ送り、粗い側は整数部 + 端数(ADR-0017)。層の分は送った割合で・粗い側は整数部だけ(ADR-0512 の 2)
        const MrCrossTransfer transfer = MrSplitCrossFlow(flow.total, gap, world.HasFraction(neighbor.slot));
        if (transfer.fineDelta == 0)
            continue;

        const int64_t sent = -transfer.fineDelta;
        own.whole += transfer.fineDelta;
        ownCondensed -= TxScaleCondensed(flow.selfCondensed, sent, flow.total);
        world.Add(neighbor.slot, neighbor.index, transfer.coarseDelta);
        const int64_t coarseCondensed = TxCoarseCondensedWhole(
            TxScaleCondensed(flow.neighborCondensed, sent, flow.total), gap);
        if (coarseCondensed != 0)
            world.AddCondensed(neighbor.slot, neighbor.index, coarseCondensed);

        crossMoved = true;
    }

    // --- 層の上の面(セルの中の対の面。セルのエネルギーは変えず、層の分だけ)---
    if (selfParts.mixed != 0)
        ownCondensed -= TxPartInnerFlow(selfParts.lateral.lower, selfParts.lateral.upper, selfParts.seriesConductance,
                                        conductionLevel);

    world.Add(slot, index, own);
    if (ownCondensed != 0)
        world.AddCondensed(slot, index, ownCondensed);

    // --- 6 起こす ---
    return crossMoved || TxWakes(own) || ownCondensed != 0;
}

MR_NAMESPACE_END

#endif  // BICAMERAL_TRANSPORT_CONDUCTION_PARTS_HLSLI
