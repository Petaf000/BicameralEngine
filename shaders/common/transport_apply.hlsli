// transport_apply.hlsli — 面の帳簿と当てる段(運ぶ計算の共通の土台の段 1 ④。T-0714・ADR-0334 Proposed。設計 20 §2.3〜§2.4)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU(engine/src/sim/multires_conduction.cpp)が呼ぶ。GPU の段は後のチケット。
//
// 一つの芯: 運ぶ計算のどの種類(熱の伝導・気体・液体・混ざる…)も「面を通る流れ = 坂 × 通しやすさ」で、1 刻み(小刻み)の中の順は同じ 6 つ:
//   1. 流す: 種類が面ごとに生の流れ(包み)を作る。同じレベルの面は両側が同じ式で逆向きの値を出し、違うレベルの面は細かい側だけが計算する
//   2. 上限: セルごとの「出る側の倍率 = min(1, 中身 ÷ 出る合計)」「入る側の倍率 = min(1, 器の残り ÷ 入る合計)」を先に数え
//      (Jacobi。順に依存しない)、面の流れを min(出る側の倍率, 入る側の倍率) で縮める(両側が同じ値を同じく縮めるので保存はそのまま)。
//      安定の条件で足りる種類(伝導の C/8)は使わない
//   3. 当てる: 変化を世界の変化の表へ(違うレベルの面は粗い側に端数ごと。ADR-0017。MrSplitCrossFlow)
//   4. 欄が物質に付く: 運ぶ欄(運動量など)を持つ種類だけ(段 2・3。伝導は欄を持たないので空)
//   5. 帳簿: 世界の外へ出た・入った分を世界の帳簿へ(開いた世界の端の面〔MR_NEIGHBOR_EDGE。world_edge.hlsli・T-0721〕の流れを
//      World::AddEdge で「世界の端」の列へ。外のセルは変わらない)。書く時の約束(T-0762): 列は手で数えず 0 = エネルギー・1 + 物質 ID・
//      NestFieldLedgerBase + NestConservedIndex(欄)+ 値の番号 / レベルの単位の丸ごとの数は AddWorldLedgerUnits・2^-64 の端数は
//      AddWorldLedgerBits / 符号付きの量は正 → 出・負 → 入(WlEnergyEntry)/ 保存の検査は ComputeConservedTotals の 1 つ(欄の量も含む)
//   6. 起こす: 変化の 0 でないセルのブロックを次の小刻み・刻みの種に(TxWakes。面の隣を起こすのは世界の側 = MR の WakeAround)
//
// 種類(Kind)が固有に持つのは 3 つだけ(土台 §0 の 3)。メンバー関数で渡す(HLSL 2021 と C++ のテンプレート):
//   - 坂: int64_t Slope(CvCellView view)
//   - 面の式: uint64_t SideCoefficient(CvCellView view, int32_t level, uint32_t shift)(自分の側の通しやすさ。安定の上限込み)・
//     uint64_t CoarseSideCoefficient(CvCellView coarse, int32_t fineLevel, uint32_t gap, uint32_t fineShift, uint32_t coarseShift)・
//     int64_t FaceFlow(int64_t slopeA, uint64_t coefficientA, int64_t slopeB, uint64_t coefficientB)(A → B が正)
//   - 安定の条件: uint32_t SubcycleShift(int32_t level)(小刻みの数の指数。拡散型は 4^σ)・bool LimitsOutflow()(2 を使うか)
//   ほかに読む欄の宣言: uint32_t FieldCount()(0 なら 4 を飛ばす)
//
// 世界(World)が渡すもの(土台 §3「土台が世界に求めるもの 4 つ」。CPU と GPU で回し方だけが違う):
//   - MrFaceNeighbor Neighbor(uint32_t slot, MrBlock block, uint32_t index, uint32_t face)(面の隣。流さない面は MR_NEIGHBOR_NONE)
//   - CvCellView View(uint32_t slot, uint32_t index)(刻みの初めの見立て。cell_view.hlsli の MakeCellView)
//   - int32_t Level(uint32_t slot)・bool HasFraction(uint32_t slot)(端数の置き場があるか)
//   - TxLimit Limit(uint32_t slot, uint32_t index)(2 の倍率。LimitsOutflow の種類だけ読む)
//   - void Add(uint32_t slot, uint32_t index, MrEnergyDelta delta)(3 の書き足し。順に依存しない 128bit の和。GPU は 64bit の atomic)
//   - void FollowFields(uint32_t slot, uint32_t index)(4。FieldCount が 0 でない種類だけ。中身は段 2・3)
//   - CvCellView Outside(uint32_t row, int32_t level)(開いた世界の端の外のセル = 外の柱の行 row を自分と同じレベルで見立てたもの。
//     種類は外のセルを読むだけで端の場合分けを持たない。T-0721)
//   - void AddEdge(uint32_t slot, uint32_t index, int64_t flow)(5。開いた端の面の流れ〔出ていくのが正〕を帳簿の「世界の端」へ)
// 浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_TRANSPORT_APPLY_HLSLI
#define BICAMERAL_TRANSPORT_APPLY_HLSLI

#include "cell_view.hlsli"
#include "world_edge.hlsli"

MR_NAMESPACE_BEGIN

// --- 2 上限(順に依存しない倍率。Q32)-------------------------------------------------------------

FX_CONST uint32_t TX_SCALE_SHIFT = 32;
FX_CONST uint64_t TX_SCALE_ONE = FX_U64(1u, 0u);  // 倍率の 1(2^32)

// セル 1 つの倍率(Q32。1 以下)
struct TxLimit {
    uint64_t outScale;  // 出る側 = min(1, 中身 ÷ 出る合計)
    uint64_t inScale;   // 入る側 = min(1, 器の残り ÷ 入る合計)
};

// min(1, available ÷ total)(切り捨て = 縮めすぎる側。total = 0 なら 1)
FX_FN uint64_t TxSideScale(uint64_t available, uint64_t total) {
    if (total <= available)
        return TX_SCALE_ONE;

    // available < total なので商 < 2^32
    return FxDivU128By64(FxMulU64Full(available, TX_SCALE_ONE), total).quotient;
}

FX_FN TxLimit TxNoLimit() {
    TxLimit limit;
    limit.outScale = TX_SCALE_ONE;
    limit.inScale = TX_SCALE_ONE;

    return limit;
}

// 列(量・エネルギー…)1 つの倍率。列が複数あるセルは TxMinLimit で列ごとの最小をとる
FX_FN TxLimit TxMakeLimit(uint64_t content, uint64_t outTotal, uint64_t room, uint64_t inTotal) {
    TxLimit limit;
    limit.outScale = TxSideScale(content, outTotal);
    limit.inScale = TxSideScale(room, inTotal);

    return limit;
}

FX_FN TxLimit TxMinLimit(TxLimit a, TxLimit b) {
    TxLimit limit;
    limit.outScale = MrMin64(a.outScale, b.outScale);
    limit.inScale = MrMin64(a.inScale, b.inScale);

    return limit;
}

// 面の倍率 = min(出ていく側の出る倍率, 入る側の入る倍率)。両側が同じ 2 つの値から同じ倍率を出す
FX_FN uint64_t TxFaceScale(TxLimit source, TxLimit target) {
    return MrMin64(source.outScale, target.inScale);
}

// 流れを倍率で縮める(大きさを切り捨て。符号は保つ = 両側で同じ大きさ)。包みの列はどれも同じ倍率で縮める
FX_FN int64_t TxScaleFlow(int64_t flow, uint64_t scale) {
    if (scale >= TX_SCALE_ONE)
        return flow;

    const bool negative = flow < 0;
    const uint64_t magnitude = negative ? (uint64_t)0 - (uint64_t)flow : (uint64_t)flow;
    const FxU128 product = FxMulU64Full(magnitude, scale);
    const uint64_t scaled = (product.hi << TX_SCALE_SHIFT) | (product.lo >> TX_SCALE_SHIFT);

    return negative ? -(int64_t)scaled : (int64_t)scaled;
}

// --- 6 起こす ----------------------------------------------------------------------------------

// 変化のあったセル(そのブロックと面の隣を次の種に)
FX_FN bool TxWakes(MrEnergyDelta delta) {
    return !MrEnergyDeltaIsZero(delta);
}

// --- 1 流す(面 1 つ)----------------------------------------------------------------------------

// 自分(見立て self・レベル level)から面の隣 neighbor(SAME・COARSER・EDGE)への生の流れ(自分 → 隣が正。自分のレベルの単位)。
// 同じレベル: 両側が同じ式で逆向きの値。粗い隣: 細かい側(自分)だけが計算する(粗い側の係数は種類の CoarseSideCoefficient)
template <typename Kind, typename World>
FX_FN int64_t TxFaceFlow(Kind kind, World world, CvCellView self, int32_t level, MrFaceNeighbor neighbor) {
    const uint32_t shift = kind.SubcycleShift(level);
    const uint64_t selfCoefficient = kind.SideCoefficient(self, level, shift);

    // 開いた世界の端: 相手は外のセル(自分と同じレベルの見立て)で、同じレベルの面と同じ式(T-0721)
    CvCellView other;
    if (neighbor.kind == MR_NEIGHBOR_EDGE)
        other = world.Outside(neighbor.index, level);
    else
        other = world.View(neighbor.slot, neighbor.index);

    if (neighbor.kind == MR_NEIGHBOR_SAME || neighbor.kind == MR_NEIGHBOR_EDGE)
        return kind.FaceFlow(kind.Slope(self), selfCoefficient, kind.Slope(other),
                             kind.SideCoefficient(other, level, shift));

    const uint32_t coarseShift = kind.SubcycleShift(world.Level(neighbor.slot));

    return kind.FaceFlow(kind.Slope(self), selfCoefficient, kind.Slope(other),
                         kind.CoarseSideCoefficient(other, level, neighbor.gap, shift, coarseShift));
}

// 2 上限: 流れの向きで出ていく側と入る側を決めて縮める(上限を使う種類だけ)
template <typename Kind, typename World>
FX_FN int64_t TxLimitFaceFlow(Kind kind, World world, uint32_t slot, uint32_t index, MrFaceNeighbor neighbor,
                              int64_t flow) {
    if (!kind.LimitsOutflow() || flow == 0)
        return flow;

    const TxLimit selfLimit = world.Limit(slot, index);
    TxLimit otherLimit = TxNoLimit();  // 開いた世界の端の外は無限の溜め(縮めるのは自分の側の倍率だけ。T-0721)
    if (neighbor.kind != MR_NEIGHBOR_EDGE)
        otherLimit = world.Limit(neighbor.slot, neighbor.index);

    return TxScaleFlow(flow, flow > 0 ? TxFaceScale(selfLimit, otherLimit) : TxFaceScale(otherLimit, selfLimit));
}

// --- 当てる段(セル 1 つの 6 面。1 流す → 2 上限 → 3 当てる → 4 欄 → 5 帳簿 → 6 起こす)-----------------

// 枠 slot(見出し block)のセル index の 6 面を流し、自分と粗い側の変化を世界の変化の表に書き足す。変化があれば true(6 起こす)。
// 流れはどれも刻み(小刻み)の初めの見立てから(Jacobi)。足す順に依らない(128bit の整数の和)
template <typename Kind, typename World>
FX_FN bool TxFlowCell(Kind kind, World world, uint32_t slot, MrBlock block, uint32_t index) {
    const CvCellView self = world.View(slot, index);
    MrEnergyDelta own = MrMakeEnergyDelta();
    bool crossMoved = false;
    for (uint32_t face = 0; face < MR_FACES; ++face) {
        const MrFaceNeighbor neighbor = world.Neighbor(slot, block, index, face);
        if (neighbor.kind != MR_NEIGHBOR_SAME && neighbor.kind != MR_NEIGHBOR_COARSER &&
            neighbor.kind != MR_NEIGHBOR_EDGE)
            continue;

        // --- 1 流す・2 上限 ---
        const int64_t raw = TxFaceFlow(kind, world, self, block.level, neighbor);
        const int64_t flow = TxLimitFaceFlow(kind, world, slot, index, neighbor, raw);

        // --- 3 当てる・5 帳簿(開いた世界の端: 外のセルは変わらず、流れた分は帳簿の「世界の端」へ。T-0721)---
        if (neighbor.kind == MR_NEIGHBOR_EDGE) {
            own.whole -= flow;
            world.AddEdge(slot, index, flow);
            continue;
        }

        // --- 3 当てる(同じレベルは自分の分だけ。相手は相手が同じ値を逆向きに足す)---
        if (neighbor.kind == MR_NEIGHBOR_SAME) {
            own.whole -= flow;
            continue;
        }

        // 粗い側: 表せる分だけ送り、粗い側は整数部 + 端数(ADR-0017)
        const MrCrossTransfer transfer = MrSplitCrossFlow(flow, neighbor.gap, world.HasFraction(neighbor.slot));
        if (transfer.fineDelta == 0)
            continue;

        own.whole += transfer.fineDelta;
        world.Add(neighbor.slot, neighbor.index, transfer.coarseDelta);
        crossMoved = true;
    }

    // --- 4 欄が物質に付く(使う種類だけ。5 帳簿は開いた端の面ごとに上で)---
    if (kind.FieldCount() != 0)
        world.FollowFields(slot, index);

    world.Add(slot, index, own);

    // --- 6 起こす ---
    return crossMoved || TxWakes(own);
}

MR_NAMESPACE_END

#endif  // BICAMERAL_TRANSPORT_APPLY_HLSLI
