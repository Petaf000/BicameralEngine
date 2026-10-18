// transport_apply.hlsli — 面の帳簿と当てる段(運ぶ計算の共通の土台の段 1 ④。T-0714・ADR-0334 Proposed。設計 20 §2.3〜§2.4)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU(engine/src/sim/multires_conduction.cpp)と GPU の伝導の段
// (shaders/sim/multires_conduct.hlsli の GpuConductionWorld。T-0766・ADR-0343)が呼ぶ。
//
// 一つの芯: 運ぶ計算のどの種類(熱の伝導・気体・液体・混ざる…)も「面を通る流れ = 坂 × 通しやすさ」で、1 刻み(小刻み)の中の順は同じ 6 つ:
//   1. 流す: 種類が面ごとに生の流れ(包み)を作る。同じレベルの面は両側が同じ式で逆向きの値を出し、違うレベルの面は細かい側だけが計算する
//   2. 上限: セルごとの「出る側の倍率 = min(1, 中身 ÷ 出る合計)」「入る側の倍率 = min(1, 器の残り ÷ 入る合計)」を先に数え
//      (Jacobi。順に依存しない)、面の流れを min(出る側の倍率, 入る側の倍率) で縮める(両側が同じ値を同じく縮めるので保存はそのまま)。
//      安定の条件で足りる種類(伝導の C/8)は使わない
//   3. 当てる: 変化を世界の変化の表へ(違うレベルの面は粗い側に端数ごと。ADR-0017。MrSplitCrossFlow)
//   4. 欄が物質に付く: 運ぶ欄(運動量など)を持つ種類だけ(段 2・3。伝導は欄を持たないので空)。段の外で欄の持ち主の質量が前の刻みの
//      終わり(登録の欄「前の質量」)から変わっていたら、減れば速さを保つ・増えれば運動量を保つ(ADR-0286)。式は follow_matter.hlsli の
//      FmMomentumAfterMassChange の 1 か所(気体のセル・液体の面・CPU と GPU が同じ関数。T-0718)。差の運動量は帳簿の「段の外の質量の変化」、
//      運動のエネルギーは今の質量から計算し直して差を熱へ、位置のエネルギーは今の質量から計算し直して差を同じ理由で帳簿へ
//   5. 帳簿: 世界の外へ出た・入った分を世界の帳簿へ(開いた世界の端の面〔MR_NEIGHBOR_EDGE。world_edge.hlsli・T-0721〕の流れを
//      World::AddEdge で「世界の端」の列へ。外のセルは変わらない)。書く時の約束(T-0762): 列は手で数えず 0 = エネルギー・1 + 物質 ID・
//      NestFieldLedgerBase + NestConservedIndex(欄)+ 値の番号 / レベルの単位の丸ごとの数は AddWorldLedgerUnits・2^-64 の端数は
//      AddWorldLedgerBits / 符号付きの量は正 → 出・負 → 入(WlEnergyEntry)/ 保存の検査は ComputeConservedTotals の 1 つ(欄の量も含む)
//   6. 起こす: 変化の 0 でないセルのブロックを次の小刻み・刻みの種に(TxWakes。面の隣を起こすのは世界の側 = MR の WakeAround)
//   当てる段の後の仕上げ(慣性の欄を持つ種類だけ。1 刻みに 1 回・全部の小刻みの後・書き戻しの前): 静まる性質(粘り)→ 静止の丸め
//   (rest_rounding.hlsli の RrShouldRest。刻みの初めの値と比べる。消した運動量は帳簿の「静止の丸め」)。気体 = gas_reference の
//   ApplyViscosity → gas_cells の RestGasCells(T-0765・ADR-0345)・液体 = multires_liquid の SettleEnergy の速さの上限の直後
//   (面ごとに RrRestMomentum。粘りは面の式の摩擦の中。T-0777・ADR-0349)
//
// 種類(Kind)が固有に持つのは 3 つだけ(土台 §0 の 3)。メンバー関数で渡す(HLSL 2021 と C++ のテンプレート):
//   - 坂: int64_t Slope(CvCellView view)
//   - 面の式: uint64_t SideCoefficient(CvCellView view, int32_t level, uint32_t shift)(自分の側の通しやすさ。安定の上限込み)・
//     uint64_t CoarseSideCoefficient(CvCellView coarse, int32_t fineLevel, uint32_t gap, uint32_t fineShift, uint32_t coarseShift)・
//     int64_t FaceFlow(int64_t slopeA, uint64_t coefficientA, int64_t slopeB, uint64_t coefficientB)(A → B が正)
//   - 安定の条件: uint32_t SubcycleShift(int32_t level)(小刻みの数の指数。拡散型は 4^σ)・bool LimitsOutflow()(2 を使うか)
//   当てる段は自分の側の坂・係数・小刻みをセルごとに 1 回だけ呼び(TxMakeSelfSide)、面ごとには相手の側と FaceFlow だけを呼ぶ(T-0768)。
//   ほかに読む欄の宣言: uint32_t FieldCount()(0 なら 4 を飛ばす)・uint32_t ViewParts()(見立てのどの部分を読むか。CV_VIEW_*。
//   当てる段は呼ばない。世界が見立てを作る時に読む = 読まない部分を作らない。T-0767)
//
// 世界(World)が渡すもの(土台 §3「土台が世界に求めるもの 4 つ」。CPU と GPU で回し方だけが違う):
//   - MrFaceNeighbor Neighbor(uint32_t slot, MrBlock block, uint32_t index, uint32_t face)(面の隣。流さない面は MR_NEIGHBOR_NONE)
//   - CvCellView View(uint32_t slot, uint32_t index)(刻みの初めの見立て。cell_view.hlsli の MakeCellViewParts。C++ は値でも const& でもよい
//     〔MR の CPU は種類の部分だけ詰めた置き場から値で組む。T-0768〕。作るのは種類の ViewParts() の部分だけでよい〔T-0767〕)
//   - int32_t Level(uint32_t slot)・bool HasFraction(uint32_t slot)(端数の置き場があるか)
//   - TxLimit Limit(uint32_t slot, uint32_t index)(2 の倍率。LimitsOutflow の種類だけ読む)
//   - void Add(uint32_t slot, uint32_t index, MrEnergyDelta delta)(3 の書き足し。順に依存しない 128bit の和。GPU は 64bit の atomic)
//   - void FollowFields(uint32_t slot, uint32_t index)(4。FieldCount が 0 でない種類だけ。中身は段 2・3)
//   - CvCellView Outside(uint32_t row, int32_t level)(開いた世界の端の外のセル = 外の柱の行 row を自分と同じレベルで見立てたもの。
//     種類は外のセルを読むだけで端の場合分けを持たない。T-0721)
//   - void AddEdge(uint32_t slot, uint32_t index, int64_t flow)(5。開いた端の面の流れ〔出ていくのが正〕を帳簿の「世界の端」へ)
//   - void AddPacket(uint32_t slot, uint32_t index, TxPacket packet, uint32_t incoming)(3。物質を運ぶ種類〔液体・気体〕の包み。T-0770)
//   - void AddEdgePacket(uint32_t slot, uint32_t index, TxPacket packet, uint32_t incoming)(5。物質を運ぶ種類の開いた端の面の包みを
//     帳簿の「世界の端」へ。TxApplyPacketSide が呼ぶ。T-0774)
//   上限 Limit の倍率は分数(TxScale)。器の残りで切らない種類は room = TX_ROOM_UNLIMITED。
// 浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_TRANSPORT_APPLY_HLSLI
#define BICAMERAL_TRANSPORT_APPLY_HLSLI

#include "cell_view.hlsli"
#include "follow_matter.hlsli"
#include "world_edge.hlsli"

MR_NAMESPACE_BEGIN

// --- 2 上限(順に依存しない倍率。分数のまま持つ = 丸めは流れに掛ける 1 回だけ。T-0770)-------------------------
// 倍率は「分子 ÷ 分母」(1 以下。1 = 1/1)。仮の世界の液体(LqLimitOutflow)・気体(GfLimitMovedAmount)の
// 「流れ × 中身 ÷ 出る合計」と同じ 1 回の切り捨てになる(Q32 の倍率に丸めてから掛けると 2 回切り捨てて値が変わる)。

// 倍率 1 つ(numerator ≤ denominator。denominator は 0 でない)
struct TxScale {
    uint64_t numerator;
    uint64_t denominator;
};

// セル 1 つの倍率(1 以下)
struct TxLimit {
    TxScale outScale;  // 出る側 = min(1, 中身 ÷ 出る合計)
    TxScale inScale;   // 入る側 = min(1, 器の残り ÷ 入る合計)
};

// 器の残りが無限(入る側を縮めない種類。液体は溢れの圧力が入る側の安定の条件なので、器で切らない。T-0770)
FX_CONST uint64_t TX_ROOM_UNLIMITED = FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu);

FX_FN TxScale TxScaleOne() {
    TxScale scale;
    scale.numerator = 1;
    scale.denominator = 1;

    return scale;
}

// a < b(どちらも分数。128bit の掛け算で比べる)
FX_FN bool TxScaleLess(TxScale a, TxScale b) {
    const FxU128 left = FxMulU64Full(a.numerator, b.denominator);
    const FxU128 right = FxMulU64Full(b.numerator, a.denominator);

    return left.hi < right.hi || (left.hi == right.hi && left.lo < right.lo);
}

FX_FN bool TxScaleIsOne(TxScale scale) {
    return scale.numerator >= scale.denominator;
}

// min(a, b)。どちらかが 1 なら掛け算をせずに他方(倍率は 1 以下なので同じ値。面の多くは縮めないので速い。T-0765)
FX_FN TxScale TxMinScale(TxScale a, TxScale b) {
    if (TxScaleIsOne(a))
        return b;

    if (TxScaleIsOne(b) || !TxScaleLess(b, a))
        return a;

    return b;
}

// min(1, available ÷ total)(total = 0 なら 1)
FX_FN TxScale TxSideScale(uint64_t available, uint64_t total) {
    if (total <= available)
        return TxScaleOne();

    TxScale scale;
    scale.numerator = available;
    scale.denominator = total;

    return scale;
}

FX_FN TxLimit TxNoLimit() {
    TxLimit limit;
    limit.outScale = TxScaleOne();
    limit.inScale = TxScaleOne();

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
    limit.outScale = TxMinScale(a.outScale, b.outScale);
    limit.inScale = TxMinScale(a.inScale, b.inScale);

    return limit;
}

// 面の倍率 = min(出ていく側の出る倍率, 入る側の入る倍率)。両側が同じ 2 つの値から同じ倍率を出す
FX_FN TxScale TxFaceScale(TxLimit source, TxLimit target) {
    return TxMinScale(source.outScale, target.inScale);
}

// 大きさを倍率で縮める(切り捨て 1 回)
FX_FN uint64_t TxScaleAmount(uint64_t amount, TxScale scale) {
    if (TxScaleIsOne(scale) || amount == 0)
        return amount;

    // numerator < denominator なので商 < amount
    return FxDivU128By64(FxMulU64Full(amount, scale.numerator), scale.denominator).quotient;
}

// 流れを倍率で縮める(大きさを切り捨て。符号は保つ = 両側で同じ大きさ)。包みの列はどれも同じ倍率で縮める
FX_FN int64_t TxScaleFlow(int64_t flow, TxScale scale) {
    const bool negative = flow < 0;
    const uint64_t magnitude = negative ? (uint64_t)0 - (uint64_t)flow : (uint64_t)flow;
    const uint64_t scaled = TxScaleAmount(magnitude, scale);

    return negative ? -(int64_t)scaled : (int64_t)scaled;
}

// --- 包み(面を通って動くもの。熱だけの種類は MrEnergyDelta、物質を運ぶ種類〔液体・気体〕は TxPacket。T-0770)-----------
// 物質を運ぶ種類は「主の量」(液体 = 質量・気体 = 物質量など、種類が決める 1 つの数)を面ごとに流し、上限は主の量に掛ける。
// 包みは上限の後の主の量から種類が作る(Pack: 上流のセルの成分の比で物質量・一緒に動くエネルギー・運ぶ欄の値)。
// 列ごとに別の上限が要る種類(気体の成分ごとの中身)は、包みを作ってから TxScalePacket で同じ倍率を全部の列に掛けてもよい。
// 包みはいつも上流 → 下流の向きの大きさ(物質量は符号なし。エネルギーと運ぶ欄は符号つき)。当てる時に上流は引き、下流は足す。

FX_CONST uint32_t TX_PACKET_SPECIES = RX_MAX_CELL_SPECIES;  // 包みの物質の数(上流のセルのインラインの成分の数まで)
FX_CONST uint32_t TX_PACKET_CARRIED = 4;  // 運ぶ欄の値の数(液体 = 位置のエネルギー 1。気体 = 面の運動量など)

struct TxPacket {
    uint32_t speciesCount;
    uint32_t species[TX_PACKET_SPECIES];
    uint64_t amounts[TX_PACKET_SPECIES];  // 動く物質量(セルの単位)
    int64_t energy;                       // 一緒に動くエネルギー(セルの単位)
    int64_t carried[TX_PACKET_CARRIED];   // 運ぶ欄の値(種類が宣言した順。使わない値は 0)
};

FX_FN TxPacket TxMakePacket() {
    TxPacket packet;
    packet.speciesCount = 0;
    packet.energy = 0;
    FX_UNROLL
    for (uint32_t i = 0; i < TX_PACKET_SPECIES; ++i) {
        packet.species[i] = 0;
        packet.amounts[i] = 0;
    }

    FX_UNROLL
    for (uint32_t c = 0; c < TX_PACKET_CARRIED; ++c)
        packet.carried[c] = 0;

    return packet;
}

// 物質 species の量 amount を包みの後ろに足す(0 は足さない。同じ物質を 2 回足さない = 上流のセルの成分を 1 回ずつ回す)
FX_FN TxPacket TxPacketWithSpecies(TxPacket packet, uint32_t species, uint64_t amount) {
    if (amount == 0 || packet.speciesCount >= TX_PACKET_SPECIES)
        return packet;

    FX_UNROLL
    for (uint32_t i = 0; i < TX_PACKET_SPECIES; ++i) {
        if (i == packet.speciesCount) {
            packet.species[i] = species;
            packet.amounts[i] = amount;
        }
    }

    packet.speciesCount += 1;

    return packet;
}

FX_FN bool TxPacketIsEmpty(TxPacket packet) {
    bool empty = packet.speciesCount == 0 && packet.energy == 0;
    FX_UNROLL
    for (uint32_t c = 0; c < TX_PACKET_CARRIED; ++c)
        empty = empty && packet.carried[c] == 0;

    return empty;
}

// 包みの全部の列に同じ倍率を掛ける(列ごとの上限を包みの後に掛ける種類)
FX_FN TxPacket TxScalePacket(TxPacket packet, TxScale scale) {
    FX_UNROLL
    for (uint32_t i = 0; i < TX_PACKET_SPECIES; ++i)
        packet.amounts[i] = TxScaleAmount(packet.amounts[i], scale);

    packet.energy = TxScaleFlow(packet.energy, scale);
    FX_UNROLL
    for (uint32_t c = 0; c < TX_PACKET_CARRIED; ++c)
        packet.carried[c] = TxScaleFlow(packet.carried[c], scale);

    return packet;
}

// --- 6 起こす ----------------------------------------------------------------------------------

// 変化のあったセル(そのブロックと面の隣を次の種に)
FX_FN bool TxWakes(MrEnergyDelta delta) {
    return !MrEnergyDeltaIsZero(delta);
}

// --- 1 流す(面 1 つ)----------------------------------------------------------------------------
// 自分の側(坂・自分の側の係数・小刻みの指数)はセルごとに 1 回だけ作り、面ごとには相手の側と面の式だけを計算する
// (TxMakeSelfSide → TxSideFaceFlow。T-0768。値は面ごとに作り直した時と同じ = 同じ関数を同じ引数で呼ぶだけ)。

// 自分の側(見立て self・レベル level のセルの、面によらない値)
struct TxSelfSide {
    int64_t slope;         // 坂(kind.Slope)
    uint64_t coefficient;  // 自分の側の通しやすさ(kind.SideCoefficient。安定の上限込み)
    uint32_t shift;        // 小刻みの数の指数(kind.SubcycleShift)
    int32_t level;
};

template <typename Kind>
FX_FN TxSelfSide TxMakeSelfSide(Kind kind, FX_IN(CvCellView) self, int32_t level) {
    TxSelfSide side;
    side.shift = kind.SubcycleShift(level);
    side.slope = kind.Slope(self);
    side.coefficient = kind.SideCoefficient(self, level, side.shift);
    side.level = level;

    return side;
}

// 自分の側 side から開いた世界の端の外のセル outside(自分と同じレベルの見立て)への生の流れ(外へ出るのが正。同じレベルの面と同じ式。
// T-0721。陰の伝導が安全網で陽に戻す端の流れもこれ。T-0790)
template <typename Kind>
FX_FN int64_t TxEdgeFaceFlow(Kind kind, TxSelfSide side, FX_IN(CvCellView) outside) {
    return kind.FaceFlow(side.slope, side.coefficient, kind.Slope(outside),
                         kind.SideCoefficient(outside, side.level, side.shift));
}

// 自分の側 side から面の隣 neighbor(SAME・COARSER・EDGE)への生の流れ(自分 → 隣が正。自分のレベルの単位)。
// 同じレベル: 両側が同じ式で逆向きの値。粗い隣: 細かい側(自分)だけが計算する(粗い側の係数は種類の CoarseSideCoefficient)
template <typename Kind, typename World>
FX_FN int64_t TxSideFaceFlow(Kind kind, World world, TxSelfSide side, MrFaceNeighbor neighbor) {
    // 開いた世界の端: 相手は外のセル(自分と同じレベルの見立て)で、同じレベルの面と同じ式(T-0721)
    if (neighbor.kind == MR_NEIGHBOR_EDGE)
        return TxEdgeFaceFlow(kind, side, world.Outside(neighbor.index, side.level));

    // 見立ては世界の置き場から(C++ は種類の部分だけ詰めた置き場から組む。T-0767・T-0768)
    FX_IN(CvCellView) other = world.View(neighbor.slot, neighbor.index);
    if (neighbor.kind == MR_NEIGHBOR_SAME)
        return kind.FaceFlow(side.slope, side.coefficient, kind.Slope(other),
                             kind.SideCoefficient(other, side.level, side.shift));

    const uint32_t coarseShift = kind.SubcycleShift(world.Level(neighbor.slot));

    return kind.FaceFlow(side.slope, side.coefficient, kind.Slope(other),
                         kind.CoarseSideCoefficient(other, side.level, neighbor.gap, side.shift, coarseShift));
}

// 自分(見立て self・レベル level)から面の隣 neighbor への生の流れ(面 1 つだけ流す時の形。セルの 6 面は TxMakeSelfSide を 1 回)
template <typename Kind, typename World>
FX_FN int64_t TxFaceFlow(Kind kind, World world, FX_IN(CvCellView) self, int32_t level, MrFaceNeighbor neighbor) {
    return TxSideFaceFlow(kind, world, TxMakeSelfSide(kind, self, level), neighbor);
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

    if (flow > 0)
        return TxScaleFlow(flow, TxFaceScale(selfLimit, otherLimit));

    return TxScaleFlow(flow, TxFaceScale(otherLimit, selfLimit));
}

// --- 当てる段(セル 1 つの 6 面。1 流す → 2 上限 → 3 当てる → 4 欄 → 5 帳簿 → 6 起こす)-----------------

// 枠 slot(見出し block)のセル index の 6 面を流し、自分と粗い側の変化を世界の変化の表に書き足す。変化があれば true(6 起こす)。
// 流れはどれも刻み(小刻み)の初めの見立てから(Jacobi)。足す順に依らない(128bit の整数の和)
template <typename Kind, typename World>
FX_FN bool TxFlowCell(Kind kind, World world, uint32_t slot, MrBlock block, uint32_t index) {
    const TxSelfSide side = TxMakeSelfSide(kind, world.View(slot, index), block.level);
    MrEnergyDelta own = MrMakeEnergyDelta();
    bool crossMoved = false;
    for (uint32_t face = 0; face < MR_FACES; ++face) {
        const MrFaceNeighbor neighbor = world.Neighbor(slot, block, index, face);
        if (neighbor.kind != MR_NEIGHBOR_SAME && neighbor.kind != MR_NEIGHBOR_COARSER &&
            neighbor.kind != MR_NEIGHBOR_EDGE)
            continue;

        // --- 1 流す・2 上限 ---
        const int64_t raw = TxSideFaceFlow(kind, world, side, neighbor);
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

// --- 当てる段(流れを面の並びで解く種類〔陰の伝導〕の面 1 つ。3 当てる → 6 起こす。T-0715)-------------------------
// 面の流れは種類が面の並びごとに 1 回解いて渡す(陰の伝導は多重格子で解いた温度と安全網から。sim/implicit_conduction の
// SolveImplicitFlows)。面の持ち主 (fineSlot, fineIndex)(違うレベルなら細かい側)から相手 (otherSlot, otherIndex) への流れ flow
// (持ち主のレベルの単位。持ち主 → 相手が正)を両側に当てる: 同じレベル(gap = 0)は逆向きに同じ値、違うレベルは粗い側に
// 整数部 + 端数(ADR-0017。MrSplitCrossFlow。TxFlowCell の粗い面と同じ)。世界の約束は TxFlowCell の Add・HasFraction だけ。
// 変化があれば true(6 起こす)
template <typename World>
FX_FN bool TxApplyFaceFlow(World world, uint32_t fineSlot, uint32_t fineIndex, uint32_t otherSlot, uint32_t otherIndex,
                           uint32_t gap, int64_t flow) {
    MrEnergyDelta fine = MrMakeEnergyDelta();
    MrEnergyDelta other = MrMakeEnergyDelta();
    if (gap == 0) {
        fine.whole = -flow;
        other.whole = flow;
    } else {
        const MrCrossTransfer transfer = MrSplitCrossFlow(flow, gap, world.HasFraction(otherSlot));
        fine.whole = transfer.fineDelta;
        other = transfer.coarseDelta;
    }

    if (MrEnergyDeltaIsZero(fine) && MrEnergyDeltaIsZero(other))
        return false;

    world.Add(fineSlot, fineIndex, fine);
    world.Add(otherSlot, otherIndex, other);

    return true;
}

// 開いた世界の端の面 1 つの流れ flow(外へ出るのが正。セルのレベルの単位)を当てる(3 当てる・5 帳簿: 外のセルは変わらず、流れた分は
// 帳簿の「世界の端」へ。TxFlowCell の端の面と同じ。流れを面の並びで解く種類〔陰の伝導〕が使う。T-0790)。世界の約束は Add・AddEdge。
// 変化があれば true(6 起こす)
template <typename World>
FX_FN bool TxApplyEdgeFlow(World world, uint32_t slot, uint32_t index, int64_t flow) {
    if (flow == 0)
        return false;

    MrEnergyDelta delta = MrMakeEnergyDelta();
    delta.whole = -flow;
    world.Add(slot, index, delta);
    world.AddEdge(slot, index, flow);

    return true;
}

// --- 当てる段(面に状態を持つ種類〔液体〕の面 1 つ。2 上限 → 包み → 3 当てる。T-0770)----------------------------
// 面の生の流れ(主の量)は種類が刻みの初めの値から面ごとに作り(液体は面の運動量から)、世界は出る合計・入る合計から
// セルごとの Limit を先に数えておく(Jacobi)。面ごとに: TxLimitAmount で縮める → 種類が上流のセルから包みを作る(Pack)→ TxApplyPacket。
// 世界が渡すもの(TxFlowCell の世界に足す 1 つ): void AddPacket(uint32_t slot, uint32_t index, TxPacket packet, uint32_t incoming)
// (incoming = 0 なら上流 = 引く、1 なら下流 = 足す。順に依存しない整数の和)。

// 列 1 つの量 raw を、上流の出る側と下流の入る側の倍率の min で縮める(切り捨て 1 回)。主の量は TxLimitAmount、
// 列ごとに別の上限が要る種類(気体の成分ごとの中身。T-0765)は列ごとの TxLimit を渡す
FX_FN uint64_t TxLimitColumn(TxLimit source, TxLimit target, uint64_t raw) {
    if (raw == 0)
        return 0;

    return TxScaleAmount(raw, TxFaceScale(source, target));
}

// 上流 (fromSlot, fromIndex) から下流 (toSlot, toIndex) への主の量 raw を、両側の倍率の min で縮める
template <typename World>
FX_FN uint64_t TxLimitAmount(World world, uint32_t fromSlot, uint32_t fromIndex, uint32_t toSlot, uint32_t toIndex,
                             uint64_t raw) {
    if (raw == 0)
        return 0;

    return TxLimitColumn(world.Limit(fromSlot, fromIndex), world.Limit(toSlot, toIndex), raw);
}

// 面の欄(state_field の SF_PLACE_FACE = セルの + 側 3 面)の持ち主: セルの面 face が + の面なら自分の欄(軸 face / 2)、
// − の面なら − 側の隣の同じ欄(開いた世界の端の − の面は持ち主のセルが無い。種類が刻みの中だけの値で持つ。T-0771・T-0774)
FX_FN bool TxFaceFieldIsOwn(uint32_t face) {
    return (face & 1u) != 0;
}

// 面に状態を持つ種類の面 1 つを、セル (slot, index) の側だけ当てる(TxFlowCell の 6 面の回し方。T-0771)。incoming = 1 なら
// 自分が下流(足す)・0 なら上流(引く)。相手のセルは相手の回で同じ包みを逆向きに当てる(包みは両側が同じ上流の刻みの初めの値から
// 同じに作る)。開いた世界の端の面(edge = 1)は外のセルが変わらないので、外の側の分を世界の帳簿へ
// (World::AddEdgePacket(slot, index, packet, incoming)。自分が上流 = 外へ出た)
template <typename World>
FX_FN bool TxApplyPacketSide(World world, uint32_t slot, uint32_t index, uint32_t incoming, uint32_t edge,
                             TxPacket packet) {
    if (TxPacketIsEmpty(packet))
        return false;

    world.AddPacket(slot, index, packet, incoming);
    if (edge != 0)
        world.AddEdgePacket(slot, index, packet, incoming);

    return true;
}

// 包みを上流から引き、下流に足す(3 当てる)。変化があれば true(6 起こす)
template <typename World>
FX_FN bool TxApplyPacket(World world, uint32_t fromSlot, uint32_t fromIndex, uint32_t toSlot, uint32_t toIndex,
                         TxPacket packet) {
    if (TxPacketIsEmpty(packet))
        return false;

    world.AddPacket(fromSlot, fromIndex, packet, 0u);
    world.AddPacket(toSlot, toIndex, packet, 1u);

    return true;
}

MR_NAMESPACE_END

#endif  // BICAMERAL_TRANSPORT_APPLY_HLSLI
