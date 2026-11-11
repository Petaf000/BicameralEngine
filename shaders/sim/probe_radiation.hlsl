// probe_radiation.hlsl — 仮の世界の GPU の熱の放射の段(T-1154・D-646・D-658・D-670・D-671)。CPU リファレンスは
// engine/src/sim/probe_reference_radiation.cpp の ProbeReference::StepCascadeRadiation(ProbeReference::Advance の (1d))。
//
// 適用の単位の中(コマンド → 物理の熱 → 表の作り直し → 気体 → 液体 → **放射** → 起こす刻みの来たブロックを一覧へ)で、
// engine/src/sim/probe_sim_radiation.cpp の RecordRadiation が次の順に投げる(段の間は全部の UAV の書き込みを待つ):
//   RadiationGrid   刻みの初めのセル(成分)+ 熱のキャッシュ(温度)→ 距離ごとの段の作業のバッファの光の値と温度(CPU の CascadeGrid と同じ並び)。
//                   参照の温度の形(世界の外 = 環境の温度の物。D-671)では負の側の作業へも同じ値を書く
//                   (側ごとの放つセルの数を作業の RC_GPU_WORK_EMITTERS に持つ。T-1229・T-1235: 側が空の刻みはその側の段とハッシュを
//                   GPU が起動の予告で飛ばす。GpuCascadeLight::RecordPlan)。
//                   写すのは前に写した刻みの後で変わった印のブロックだけ(T-1228 段 1・ADR-0500 決定 4。単位の引数が 0 でない刻みは全部:
//                   作った直後・保存点へ戻した・写しの早送りの後・前の刻みに写していない・比べる試験の「いつも全部」。CPU の約束は
//                   sim::ProbeRadiationBlockStale。表の差し替えは RefreshTable が全部のブロックに今の刻みの印を付けるので印で拾う)
//   (距離ごとの段の 1 刻み: shaders/sim/cascade_light.hlsl。sim/gpu_cascade_light の自分のルート署名で。参照の温度の形では側ごとに 1 回)
//   RadiationApply  セルごとの 正(吸った − 放った)− 負(吸った − 放った)(2^−16 mJ)を最も近い mJ に丸めてエネルギーへ足し、
//                   変わったセルの熱のキャッシュを作り直し、ブロックを「刻みの直前に変わった」にして起こす刻みを今にする(気体の GasScatter と同じ)。
//                   読んだ放った・吸ったの欄は 0 に戻す(この刻みだけの値。参照の温度の形の RcSources は写しの段が持つ放つセルの一覧の
//                   セルだけ回すので、一覧に無いセルの欄はここで 0 にする。T-1228 段 1)
// ハッシュの単位(HashCells の後)では RadiationHash が次の刻みへの持ち越し(セル・面・帯ごとの量)を状態のハッシュへ足す(D-619。側ごとに 1 回。
// 負の側は面の番号 | 8。CPU の ProbeReference::StateHash と同じ)。
// 結ぶもの: ProbeSim のルート署名の u15〜u17 を、この段の間だけ借りる(気体の区画の段は済んでいる。段の後で結び直す)。段ごとの中身:
//   RadiationGrid   u15 = 正の側の作業・u16 = 負の側の作業(0 K の空の世界では正の側と同じ)・u17 = 物質の光の値
//   RadiationApply  u15 = 正の側の作業(番地を読む)・u16 = 正の側の状態・u17 = 負の側の状態(番地は正の側と同じ並び)
//   RadiationHash   u15 = その側の作業・u16 = その側の状態
// 浮動小数点は使わない(04 §4)。
#include "common/cascade_light.hlsli"
#include "common/probe_region_hash.hlsli"
#include "sim/probe_bindings.hlsli"

RWByteAddressBuffer rcWork : register(u15);    // 距離ごとの段の作業(RcGpuParams・升目の表・光の値・温度)
RWByteAddressBuffer rcSecond : register(u16);  // 段ごとに違う(上の頭の表): 負の側の作業 / その側の状態
RWByteAddressBuffer rcThird
    : register(u17);  // 段ごとに違う: 物質の光の値(LtSpeciesLight × 物質の数。表ごと)/ 負の側の状態

static const uint32_t RC_CELL_LIGHT_BYTES = 24;  // RcCellLight
static const uint32_t RC_SPECIES_LIGHT_BYTES = 16;
static const uint32_t RADIATION_BLOCK_CELLS = PROBE_BLOCK_SIZE * PROBE_BLOCK_SIZE *
                                              PROBE_BLOCK_SIZE;  // = グループの幅(64)

struct ProbeRadiationLights {
    uint32_t unused;

    LtSpeciesLight Light(uint32_t speciesId) {
        return rcThird.Load<LtSpeciesLight>(speciesId * RC_SPECIES_LIGHT_BYTES);
    }
};

RcGpuParams LoadRcParams() {
    return rcWork.Load<RcGpuParams>(0);
}

// 参照の温度の形でセルが側ごとに放つか(T-1229・T-1235): 各側の RcSources と同じ値(RcCellEmissionRelative。温度 0 のセルは
// 放たない)が 0 でないか。正の側 = 自分の黒体 > 参照の黒体、負の側 = 参照 > 自分(帯ごと)。64bit は引かずに比べるだけ(T-1078)。
// 面の向こうの飽和は見ない(多めに流すだけで、結果は変わらない)。参照の量(reference = 参照の黒体 × このセルの放射率)と放射率は
// 呼ぶ側が 1 回ずつ求めて前後の値で使い回す(T-1228。RcCellEmission と同じ式を分けたもの)
struct RadiationEmitSides {
    bool positive;
    bool negative;
};

RadiationEmitSides EmitSidesOf(uint32_t temperatureMk, uint32_t referenceMk, RcBands emissivityQ16, RcBands reference) {
    RadiationEmitSides sides;
    sides.positive = false;
    sides.negative = false;
    if (temperatureMk == 0 || temperatureMk == referenceMk)
        return sides;

    const RcBands own = RcEmissionOf(RcBlackBands(temperatureMk), emissivityQ16);
    sides.positive = own.shortBand > reference.shortBand || own.longBand > reference.longBand;
    sides.negative = reference.shortBand > own.shortBand || reference.longBand > own.longBand;

    return sides;
}

// 温度が参照でも 0 でもない(放つかを求める要がある)
bool NeedsEmitSides(uint32_t temperatureMk, uint32_t referenceMk) {
    return temperatureMk != 0 && temperatureMk != referenceMk;
}

// ブロックが前の刻み(tick − 1)の写しの後で変わったか(sim::ProbeRadiationBlockStale と同じ: 印 ≥ tick − 1)。
// 64bit の引き算と比べを並べない(T-1078・T-1159)ので、tick − 1 も比べも 32bit の上下で(借りは手で回す)
bool BlockChangedSincePreviousGrid(uint32_t block) {
    const uint32_t word = PROBE_SCHEDULE_CHANGED_WORD + block * 2;
    const uint32_t markLow = blockSchedule[word];
    const uint32_t markHigh = blockSchedule[word + 1];
    const uint32_t previousLow = tickLow - 1u;
    const uint32_t previousHigh = tickHigh - (tickLow == 0u ? 1u : 0u);

    return markHigh > previousHigh || (markHigh == previousHigh && markLow >= previousLow);
}

// 側ごとの放つセルの数の増減(放つ = 1。mod 2^32 で足す)
uint32_t EmitDelta(bool before, bool after) {
    if (before == after)
        return 0u;

    return after ? 1u : 0xFFFFFFFFu;
}

// 写しの段のスレッドのセル(全部写す刻みはセルの番号の順。変わったブロックだけの刻みは 1 グループ = 1 ブロック〔4³ = 64 セル〕に並べ、
// 変わっていないブロックのグループは丸ごと返る)。セルの番号の順だと波 1 つが 16 ブロックにまたがり、写すブロックが少なくても
// ほとんどの波が流れた。ブロックに並べると全部写す刻みの読み書きが散らばって重くなった(2026-11-05 の測り)ので、刻みごとに選ぶ。
// セルごとの書き先は決まっているので、並べ方で結果は変わらない。PROBE_CELL_COUNT = 写さない
uint32_t GridCellOf(uint32_t thread, bool copyAll) {
    if (copyAll)
        return thread < PROBE_CELL_COUNT ? thread : PROBE_CELL_COUNT;

    const uint32_t block = thread / RADIATION_BLOCK_CELLS;
    if (block >= PROBE_BLOCK_COUNT || !BlockChangedSincePreviousGrid(block))
        return PROBE_CELL_COUNT;

    const uint32_t local = thread % RADIATION_BLOCK_CELLS;
    const uint3 corner = BlockCoordinates(block) * PROBE_BLOCK_SIZE;

    return ProbeCellIndex(corner.x + (local % PROBE_BLOCK_SIZE),
                          corner.y + ((local / PROBE_BLOCK_SIZE) % PROBE_BLOCK_SIZE),
                          corner.z + (local / (PROBE_BLOCK_SIZE * PROBE_BLOCK_SIZE)));
}

// 放つセルの一覧(作業の workEmitBits。T-1228 段 1)の bit を、放つセルの数の増減(EmitDelta。1 = 立てる・mod 2^32 の −1 = 下ろす)に合わせて直す。
// 波の中で同じ語のセルをまとめ、語ごとに立てる bit・下ろす bit を集めて 1 語に 1 回ずつ原子操作する(T-1328。全部写す刻みはセルの番号の順に
// 並ぶので、直すセルのある語は波 1 つに 1〜2 個)。1 セルの bit を書くのは 1 スレッドだけで bit は重ならないので、セルごとに当てるのと
// 結果は同じ(順によらない)。一覧はいつも「作業の今の中身で放つ」セル(数と同じ判定)。波の命令は波で揃った所(写しの段の最後)から呼ぶ
void UpdateEmitBits(RWByteAddressBuffer work, uint32_t bitsOffset, uint32_t cell, uint32_t step) {
    if (bitsOffset == 0u)
        return;

    const uint32_t at = RcEmitBitsWordOffset(bitsOffset, cell);
    const uint32_t mask = RcEmitBitMask(cell);
    const uint32_t setMask = step == 1u ? mask : 0u;
    const uint32_t clearMask = (step != 0u && step != 1u) ? mask : 0u;
    bool pending = step != 0u;

    // 残っている語のうち番地の最も小さい語から 1 つずつ(回る数 = 直すセルのある語の数。直すセルが無い波は回らない)
    while (WaveActiveAnyTrue(pending)) {
        const uint32_t wordAt = WaveActiveMin(pending ? at : 0xFFFFFFFFu);
        const bool inWord = pending && at == wordAt;
        const uint32_t setBits = WaveActiveBitOr(inWord ? setMask : 0u);
        const uint32_t clearBits = WaveActiveBitOr(inWord ? clearMask : 0u);
        uint32_t original;
        if (WaveIsFirstLane() && setBits != 0u)
            work.InterlockedOr(wordAt, setBits, original);

        if (WaveIsFirstLane() && clearBits != 0u)
            work.InterlockedAnd(wordAt, ~clearBits, original);

        pending = pending && !inWord;
    }
}

bool SameLight(RcCellLight left, RcCellLight right) {
    return left.depthShortQ32 == right.depthShortQ32 && left.depthLongQ32 == right.depthLongQ32 &&
           left.reflectanceShortQ16 == right.reflectanceShortQ16 && left.reflectanceLongQ16 == right.reflectanceLongQ16;
}

// --- 刻みの初めの写し(1 スレッド = 1 セル。単位の引数が 0 なら、前の刻みの写しの後で変わったブロックのセルだけ)---
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void RadiationGrid(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t cell = GridCellOf(dispatchThreadId.x, argument != 0u);
    if (cell >= PROBE_CELL_COUNT)
        return;

    const RcGpuParams params = LoadRcParams();
    const uint32_t index = GenerationBase(CurrentTick()) + cell;
    const RxCell values = cells[index];
    ProbeRadiationLights lights;
    lights.unused = 0;
    const LtCellBand shortBand = LtCellBandOf(lights, values, LT_BAND_SHORT);
    const LtCellBand longBand = LtCellBandOf(lights, values, LT_BAND_LONG);

    RcCellLight light;
    light.depthShortQ32 = RdOpticalDepthQ32(shortBand.absorption);
    light.depthLongQ32 = RdOpticalDepthQ32(longBand.absorption);
    light.reflectanceShortQ16 = shortBand.reflectanceQ16;
    light.reflectanceLongQ16 = longBand.reflectanceQ16;
    const uint32_t temperatureMk = thermal[index].temperature;  // 負は 0(熱のキャッシュの約束)
    const uint32_t lightAt = params.workLights + (cell * RC_CELL_LIGHT_BYTES);
    const uint32_t temperatureAt = params.workTemperatures + (cell * 4);

    // 1 側の形(0 K の空。参照の語が 0)は書くだけ(放つセルの数を持たないので前の値は読まない)
    if (params.referenceWord == 0u) {
        rcWork.Store<RcCellLight>(lightAt, light);
        rcWork.Store(temperatureAt, temperatureMk);
        return;
    }

    // 参照の温度の形の放つセルの数は、前に写した値(作業の今の中身)から写す値への増減で直す。値が同じセル(全部写す刻みのほとんど)は
    // 増減 0 なので、放つかを求めず、書きもしない(作業の中身はもうその値)
    const RcCellLight previousLight = rcWork.Load<RcCellLight>(lightAt);
    const uint32_t previousMk = rcWork.Load(temperatureAt);
    const bool sameLight = SameLight(previousLight, light);
    const bool same = previousMk == temperatureMk && sameLight;
    uint32_t positiveStep = 0;
    uint32_t negativeStep = 0;
    if (!same) {
        // 負の側の作業へも(並びは同じ)
        rcWork.Store<RcCellLight>(lightAt, light);
        rcWork.Store(temperatureAt, temperatureMk);
        rcSecond.Store<RcCellLight>(lightAt, light);
        rcSecond.Store(temperatureAt, temperatureMk);

        // 放つかは前後の値の分だけ求める(参照の黒体は 1 回・光の値が同じなら放射率と参照の量も 1 回。温度だけ変わるセルがほとんど)
        const uint32_t referenceMk = params.referenceWord & ~RC_REFERENCE_NEGATIVE;
        if (NeedsEmitSides(temperatureMk, referenceMk) || NeedsEmitSides(previousMk, referenceMk)) {
            const RcBands referenceBlack = RcBlackBands(referenceMk);
            const RcBands emissivity = RcEmissivityQ16(light);
            const RcBands reference = RcEmissionOf(referenceBlack, emissivity);
            const RadiationEmitSides after = EmitSidesOf(temperatureMk, referenceMk, emissivity, reference);
            RadiationEmitSides before;
            if (sameLight) {
                before = EmitSidesOf(previousMk, referenceMk, emissivity, reference);
            } else {
                const RcBands previousEmissivity = RcEmissivityQ16(previousLight);
                before = EmitSidesOf(previousMk, referenceMk, previousEmissivity,
                                     RcEmissionOf(referenceBlack, previousEmissivity));
            }
            positiveStep = EmitDelta(before.positive, after.positive);
            negativeStep = EmitDelta(before.negative, after.negative);
        }
    }

    // 放つセルの一覧の bit(波でまとめて語ごとに 1 回。T-1328)
    UpdateEmitBits(rcWork, params.workEmitBits, cell, positiveStep);
    UpdateEmitBits(rcSecond, params.workEmitBits, cell, negativeStep);

    // 側ごとの放つセルの数(作業の RC_GPU_WORK_EMITTERS。T-1229・T-1235・T-1228)。数はいつも「作業の今の中身で放つセルの数」で、
    // 作った時の作業は温度 0(放たない)・数 0。写さないセルの値は変わらないので、写したセルの増減だけ足せば合う(波に 1 回)。
    // その側の RcPlanTick は 0 でないかだけ読む
    const uint32_t positiveDelta = WaveActiveSum(positiveStep);
    const uint32_t negativeDelta = WaveActiveSum(negativeStep);
    uint32_t original;
    if (WaveIsFirstLane() && positiveDelta != 0u)
        rcWork.InterlockedAdd(RC_GPU_WORK_EMITTERS, positiveDelta, original);

    if (WaveIsFirstLane() && negativeDelta != 0u)
        rcSecond.InterlockedAdd(RC_GPU_WORK_EMITTERS, negativeDelta, original);
}

// 側 1 つのセルの 吸った(短い帯 + 長い帯)と 放った を 32bit の上下に分けて返す(足し算だけ)。
// written = 4 語のどれかが 0 でない(語の OR で。足した和を 0 と比べない)
struct RadiationSideSums {
    uint64_t absorbed;
    uint64_t emitted;
    bool written;
};

// uint64 の語(下・上の 32bit)
uint64_t RadiationWordPair(uint32_t low, uint32_t high) {
    return ((uint64_t)high << 32) | (uint64_t)low;
}

RadiationSideSums LoadRadiationSide(RWByteAddressBuffer state, RcGpuParams params, uint32_t cell) {
    const uint32_t emittedAt = params.stateEmitted + (cell * 16);
    const uint32_t absorbedAt = params.stateAbsorbed + (cell * 16);
    const uint4 absorbedWords = state.Load4(absorbedAt);
    const uint4 emittedWords = state.Load4(emittedAt);
    RadiationSideSums sums;
    sums.absorbed = RadiationWordPair(absorbedWords.x, absorbedWords.y) +
                    RadiationWordPair(absorbedWords.z, absorbedWords.w);
    sums.emitted = RadiationWordPair(emittedWords.x, emittedWords.y) +
                   RadiationWordPair(emittedWords.z, emittedWords.w);
    const uint4 ored = absorbedWords | emittedWords;
    sums.written = (ored.x | ored.y | ored.z | ored.w) != 0u;

    return sums;
}

// 側 1 つのセルの放った・吸ったの欄を、読んだ後に 0 に戻す(この刻みだけの値。ADR-0500。0 でない時だけ書く)。
// 次の刻みの RcSources は放つセルの一覧のセルだけ回すので、一覧に無いセルの欄はここで 0 にしておく(T-1228 段 1)
void ClearRadiationSide(RWByteAddressBuffer state, RcGpuParams params, uint32_t cell, RadiationSideSums sums) {
    if (!sums.written)
        return;

    state.Store4(params.stateEmitted + (cell * 16), uint4(0, 0, 0, 0));
    state.Store4(params.stateAbsorbed + (cell * 16), uint4(0, 0, 0, 0));
}

// 増える側 − 減る側(mod 2^64)を最も近い mJ に丸める(CPU の RoundToMilliJoules と同じ: 半分は +∞ 側・算術シフト)。
// 64bit の引き算と 0 との比べを並べると RTX の GPU で差の上の 32bit が壊れる(T-1078・T-1159)ので、32bit の 2 つに分けて借りと繰り上がりを手で回す
int64_t RadiationRoundedNet(uint64_t gain, uint64_t loss) {
    const uint32_t gainLow = (uint32_t)(gain & 0xFFFFFFFFu);
    const uint32_t gainHigh = (uint32_t)(gain >> 32);
    const uint32_t lossLow = (uint32_t)(loss & 0xFFFFFFFFu);
    const uint32_t lossHigh = (uint32_t)(loss >> 32);
    const uint32_t borrow = gainLow < lossLow ? 1u : 0u;
    const uint32_t netLow = gainLow - lossLow;
    const uint32_t netHigh = gainHigh - lossHigh - borrow;

    const uint32_t halfUnit = 1u << (RC_ENERGY_SHIFT - 1);
    const uint32_t roundedLow = netLow + halfUnit;
    const uint32_t carry = roundedLow < netLow ? 1u : 0u;
    const uint32_t roundedHigh = netHigh + carry;

    // 算術シフト(RC_ENERGY_SHIFT ビット。上の語は符号を残して下げる)
    const uint32_t resultLow = (roundedLow >> RC_ENERGY_SHIFT) | (roundedHigh << (32 - RC_ENERGY_SHIFT));
    const uint32_t resultHigh = (uint32_t)((int32_t)roundedHigh >> RC_ENERGY_SHIFT);

    return (int64_t)(((uint64_t)resultHigh << 32) | (uint64_t)resultLow);
}

// --- セルへ当てる(1 スレッド = 1 セル。丸めは CPU の RoundToMilliJoules と同じ: 半分は +∞ 側・算術シフト)---
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void RadiationApply(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t cell = dispatchThreadId.x;
    if (cell >= PROBE_CELL_COUNT)
        return;

    const RcGpuParams params = LoadRcParams();
    const RadiationSideSums positive = LoadRadiationSide(rcSecond, params, cell);
    ClearRadiationSide(rcSecond, params, cell, positive);
    uint64_t gain = positive.absorbed;
    uint64_t loss = positive.emitted;
    if (params.referenceWord != 0u) {
        // 負の側は符号が逆(吸った = セルから引く・放った = セルへ足す)
        const RadiationSideSums negative = LoadRadiationSide(rcThird, params, cell);
        ClearRadiationSide(rcThird, params, cell, negative);
        gain += negative.emitted;
        loss += negative.absorbed;
    }

    const int64_t milliJoules = RadiationRoundedNet(gain, loss);
    if (milliJoules == 0)
        return;

    const uint64_t tick = CurrentTick();
    const uint32_t index = GenerationBase(tick) + cell;
    RxCell values = cells[index];
    values.energy += milliJoules;
    cells[index] = values;
    thermal[index] = ProbeMakeCache(ReactionTable(), values);

    const uint32_t block = ProbeBlockOfCell(cell % PROBE_GRID_SIZE, (cell / PROBE_GRID_SIZE) % PROBE_GRID_SIZE,
                                            cell / PROBE_SLICE_CELL_COUNT);
    StoreBlockMark(PROBE_SCHEDULE_CHANGED_WORD, block, ProbeChangeMark(tick) - 1);
    StoreBlockMark(PROBE_SCHEDULE_WAKE_WORD, block, ProbeChangeMark(tick));
}

    // --- 次の刻みへの持ち越しを状態 S(tick + 1) のハッシュへ(持ち越しの表 1 − (tick & 1) の 0 でない鍵。歩幅は起動したスレッドの数)---
    [numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void RadiationHash(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const RcGpuParams params = LoadRcParams();
    const uint32_t faceTag = (params.referenceWord & RC_REFERENCE_NEGATIVE) != 0u ? 8u : 0u;  // 負の側は面の番号 | 8
    const uint64_t tick = CurrentTick();
    const uint32_t outTable = 1u - (uint32_t)(tick & 1);
    const uint32_t carryTable = outTable == 0 ? params.stateCarry0 : params.stateCarry1;
    const uint32_t stride = argument;  // 起動したスレッドの数(RecordRadiationHash が単位の定数に入れる)

    if (dispatchThreadId.x == 0) {
        const uint32_t overflow = (uint32_t)rcSecond.Load<uint64_t>(RC_GPU_OVERFLOW * 8);
        DEBUG_ASSERT(overflow == 0, DebugFormat::ProbeRadiationOverflow, overflow);
    }

    // 印の無い頁(cascade_light.hlsl の AddCarry が立てる。T-1227 段 1)の鍵はいつも 0 なので読まない
    const uint32_t marks = outTable == 0 ? params.stateCarryMarks0 : params.stateCarryMarks1;
    uint64_t hash = 0;
    for (uint32_t key = dispatchThreadId.x; key < params.carryKeys; key += stride) {
        if (rcSecond.Load(marks + ((key / RC_GPU_CARRY_PAGE_KEYS) * 4)) == 0u)
            continue;

        const uint64_t amount = rcSecond.Load<uint64_t>(carryTable + (key * 8));
        if (amount == 0)
            continue;

        const uint32_t cell = key / (RC_FACES * LT_BANDS);
        const uint32_t face = (key / LT_BANDS) % RC_FACES;
        const uint32_t band = key % LT_BANDS;
        hash += ProbeCascadeCarryHashTerm(cell, face | faceTag, band, amount);
    }

    const uint64_t sum = WaveActiveSum(hash);
    if (WaveIsFirstLane() && sum != 0) {
        uint64_t original;
        hashes.InterlockedAdd64(HashEntryAddress(tick + 1) + PROBE_HASH_OFFSET_HASH, sum, original);
    }
}
