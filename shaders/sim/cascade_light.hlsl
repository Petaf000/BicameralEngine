// cascade_light.hlsl — 世界の中の光: 距離ごとの段の GPU(Compute。T-0959・ADR-0432)。CPU リファレンスは sim/cascade_light.cpp の StepCascadeLight。
// 光線 1 本・段の受け渡し・溜めは shaders/common/cascade_light.hlsli の関数を CPU と同じに呼ぶ。ルート署名は多重解像度のもの
// (multires_bindings.hlsli)に外のバッファ u4・u5 を足して使う(太陽の影の柱と同じ形)。
//
// データの流れ(sim/gpu_cascade_light.cpp が 1 刻みを記録する。置き場は cascade_light.hlsli の RcGpuParams):
//   RcBeginTick(頭の語を 0 に)→ RcSources(熱いセルの 6 面の熱の放つ量を書き、放つ面の列に足す。組の刻みでない面は持ち越しで待つ)
//   → RcCarryIn(前の刻みの持ち越し: 組の刻みの面は放つ面へ、そうでなければ次の刻みへ)→ RcLevel0Prepare(面ごとの升目の重みの和・余り)
//   → RcLevel0Walk(段 0 の光線。残りを段 1 の袋の表へ)→ RcClearFaces(使った放つ面を 0 に)
//   → 段ごとに RcLevelWalk(袋 × raysPerBag 本。残りを次の段の袋の表へ)・RcClearBags・RcResetBags(照り返しと組を待つ光は、歩く間に次の刻みの持ち越しの表へ足してある)
// 仮の世界の負の側(sim/probe_sim。T-1229)は刻みの初めに RcPlanTick が「流す」の印を作り、空の刻みは GPU が起動の予告で全部の段を飛ばす。
// 持ち越しは (セル・面・帯) の鍵で引く密な表を刻みの偶奇で 2 つ持つ(AddCarry が足し、次の刻みの RcCarryIn が全部の鍵を掃いて読み、0 に戻す)。
// 鍵の数だけ置き場があるので溢れない(T-1164・ADR-0484。前の疎な表は溢れると取り合いの順で結果が変わった)。
// 足し算と min はすべて 64bit の atomic で可換なので、光線の処理の順によらず CPU とビット一致する(R3・R4)。
// 帳簿・本数は波の和にしてから足す(同じ語への atomic を減らす)。どのカーネルも 1 スレッドが番号を歩幅 g_external2 で回す。
#include "common/cascade_light.hlsli"
#include "sim/multires_bindings.hlsli"

RWByteAddressBuffer g_rcWork : register(u4);   // 作業(RcGpuParams・升目の表・光の値・温度・初めの持ち越し)
RWByteAddressBuffer g_rcState : register(u5);  // 状態(uint64 の語)

static const uint32_t RC_GROUP_THREADS = 64;  // sim/gpu_cascade_light.cpp の RC_GROUP_THREADS
static const uint32_t RC_NO_SLOT = 0xFFFFFFFFu;

// --- 語の読み書き ---

uint64_t LoadWord(uint32_t byteOffset) {
    return g_rcState.Load<uint64_t>(byteOffset);
}

void StoreWord(uint32_t byteOffset, uint64_t value) {
    g_rcState.Store<uint64_t>(byteOffset, value);
}

void AddWord(uint32_t byteOffset, uint64_t amount) {
    if (amount == 0)
        return;

    uint64_t before;
    g_rcState.InterlockedAdd64(byteOffset, amount, before);
}

uint32_t HeaderOffset(uint32_t word) {
    return word * 8;
}

// 波の和にしてから頭の語へ足す(word は波の中で同じであること)
void AddHeaderWave(uint32_t word, uint64_t amount) {
    const uint64_t total = WaveActiveSum(amount);
    if (WaveIsFirstLane())
        AddWord(HeaderOffset(word), total);
}

void AddLedgerWave(uint32_t band, uint32_t kind, uint64_t amount) {
    AddHeaderWave(RC_GPU_LEDGER + (band * RC_GPU_LEDGER_KINDS) + kind, amount);
}

void SetOverflow(uint64_t flag) {
    uint64_t before;
    g_rcState.InterlockedOr64(HeaderOffset(RC_GPU_OVERFLOW), flag, before);
}

// 64bit の min(比べて入れ替える。今の値より小さい時だけ書く。順によらず最後は全部の候補の min)
void MinWord(uint32_t byteOffset, uint64_t value) {
    uint64_t expected = RC_GPU_CHOSEN_EMPTY;
    bool done = false;
    for (uint32_t attempt = 0; attempt < 0x100000u && !done; ++attempt) {
        if (value >= expected) {
            done = true;
        } else {
            uint64_t original;
            g_rcState.InterlockedCompareExchange64(byteOffset, expected, value, original);
            done = original == expected;
            expected = original;
        }
    }
}

RcGpuParams LoadParams() {
    return g_rcWork.Load<RcGpuParams>(0);
}

RcStepConstants ConstantsOf(RcGpuParams params) {
    RcStepConstants constants;
    constants.seed = params.seed;
    constants.tick = (uint64_t)g_external0;
    constants.baseTexels = params.baseTexels;
    constants.levels = params.levels;
    constants.intervalBlocks = params.intervalBlocks;
    constants.raysPerBag = params.raysPerBag;
    constants.emitSlots = params.emitSlots;
    constants.thermalInSlots = params.thermalInSlots;

    return constants;
}

// --- RcWalkRay に渡す道具(cascade_light.hlsli の約束)---

struct GpuRcGrid {
    int64_t lowX;
    int64_t lowY;
    int64_t lowZ;
    int64_t sizeX;
    int64_t sizeY;
    int64_t sizeZ;
    uint32_t lightsOffset;

    bool Contains(int64_t x, int64_t y, int64_t z) {
        return x >= lowX && y >= lowY && z >= lowZ && x < lowX + sizeX && y < lowY + sizeY && z < lowZ + sizeZ;
    }

    uint32_t IndexOf(int64_t x, int64_t y, int64_t z) {
        return (uint32_t)((((z - lowZ) * sizeY) + (y - lowY)) * sizeX + (x - lowX));
    }

    RcCellLight Light(int64_t x, int64_t y, int64_t z) {
        return g_rcWork.Load<RcCellLight>(lightsOffset + (IndexOf(x, y, z) * 24));
    }
};

GpuRcGrid GridOf(RcGpuParams params) {
    GpuRcGrid grid;
    grid.lowX = params.lowX;
    grid.lowY = params.lowY;
    grid.lowZ = params.lowZ;
    grid.sizeX = params.sizeX;
    grid.sizeY = params.sizeY;
    grid.sizeZ = params.sizeZ;
    grid.lightsOffset = params.workLights;

    return grid;
}

uint32_t CarryTableOf(RcGpuParams params, uint32_t parity) {
    return parity == 0 ? params.stateCarry0 : params.stateCarry1;
}

// 次の刻みの持ち越しの表(outParity。鍵 = セル × 12 + 面 × 2 + 帯 で引く密な表)へ足す。表は鍵の数だけあるので溢れない
// (T-1164・ADR-0484)。0 から増えた鍵を数える(測りの目安。0 から増やす足し算はちょうど 1 つなので順によらない)
void AddCarry(RcGpuParams params, uint32_t outParity, uint64_t key, uint64_t amount) {
    if (amount == 0)
        return;

    uint64_t before;
    g_rcState.InterlockedAdd64(CarryTableOf(params, outParity) + (uint32_t)(key * 8), amount, before);
    if (before != 0)
        return;

    g_rcState.InterlockedAdd(HeaderOffset(RC_GPU_CARRY_COUNT + outParity), 1u);
}

// 放つ面を列に足す(印が 0 の面だけ。印は RcClearFaces が 0 に戻す)
void ListFace(RcGpuParams params, uint32_t face) {
    uint32_t marked;
    g_rcState.InterlockedOr(params.stateFaceFlags + (face * 4), 1u, marked);
    if (marked != 0)
        return;

    uint32_t position;
    g_rcState.InterlockedAdd(HeaderOffset(RC_GPU_FACE_COUNT), 1u, position);
    if (position < params.faceCapacity)
        g_rcState.Store(params.stateFaces + (position * 4), face);
    else
        SetOverflow(1);
}

struct GpuRcSink {
    GpuRcGrid grid;
    RcGpuParams params;
    uint32_t outParity;

    void Deposit(int64_t x, int64_t y, int64_t z, uint32_t band, uint32_t face, uint64_t absorbed, uint64_t reflected) {
        const uint32_t cell = grid.IndexOf(x, y, z);
        AddWord(params.stateAbsorbed + (((cell * 2) + band) * 8), absorbed);
        AddCarry(params, outParity, (((uint64_t)cell * RC_FACES) + face) * 2 + band, reflected);
    }
};

// --- 袋の表(開いた番地の hash。鍵 = 1 + ブロック 14bit × 3・面 3bit・升目 9bit × 2)---

uint64_t BagKeyOf(RcBagShare share, RcGpuParams params) {
    const uint64_t relativeX = (uint64_t)(share.homeX - (params.lowX >> share.level));
    const uint64_t relativeY = (uint64_t)(share.homeY - (params.lowY >> share.level));
    const uint64_t relativeZ = (uint64_t)(share.homeZ - (params.lowZ >> share.level));
    uint64_t key = (relativeX << RC_GPU_BLOCK_BITS) | relativeY;
    key = (key << RC_GPU_BLOCK_BITS) | relativeZ;
    key = (key << 3) | (uint64_t)share.face;
    key = (key << 9) | (uint64_t)share.i;
    key = (key << 9) | (uint64_t)share.j;

    return key + 1;
}

struct GpuRcBags {
    RcGpuParams params;
    uint32_t tableOffset;
    uint32_t listOffset;
    uint32_t countWord;

    uint32_t FindSlot(uint64_t key) {
        const uint32_t mask = params.bagCapacity - 1;
        uint32_t slot = (uint32_t)(FxMix64(key) & (uint64_t)mask);
        uint32_t found = RC_NO_SLOT;
        for (uint32_t probe = 0; probe < params.bagCapacity && found == RC_NO_SLOT; ++probe) {
            uint64_t original;
            g_rcState.InterlockedCompareExchange64(tableOffset + (slot * RC_GPU_BAG_WORDS * 8), (uint64_t)0, key,
                                                   original);
            if (original == 0) {
                uint32_t position;
                g_rcState.InterlockedAdd(HeaderOffset(countWord), 1u, position);  // 下位 32bit(袋の数 < 2^32)
                g_rcState.Store(listOffset + (position * 4), slot);
                found = slot;
            } else if (original == key) {
                found = slot;
            } else {
                slot = (slot + 1) & mask;
            }
        }

        return found;
    }

    void Add(RcBagShare share) {
        const uint32_t slot = FindSlot(BagKeyOf(share, params));
        if (slot == RC_NO_SLOT) {
            SetOverflow(2);
            return;
        }

        const uint32_t base = tableOffset + (slot * RC_GPU_BAG_WORDS * 8);
        AddWord(base + 8, share.energyShort);
        AddWord(base + 16, share.energyLong);
        AddWord(base + 24, share.lobeShortX);
        AddWord(base + 32, share.lobeShortY);
        AddWord(base + 40, share.lobeShortZ);
        AddWord(base + 48, share.lobeLongX);
        AddWord(base + 56, share.lobeLongY);
        AddWord(base + 64, share.lobeLongZ);
        MinWord(base + 72, share.chosen);
    }
};

GpuRcBags BagsOf(RcGpuParams params, uint32_t table) {
    GpuRcBags bags;
    bags.params = params;
    bags.tableOffset = table == 0 ? params.stateBags0 : params.stateBags1;
    bags.listOffset = table == 0 ? params.stateBagList0 : params.stateBagList1;
    bags.countWord = RC_GPU_BAG_COUNT + table;

    return bags;
}

RcBagShare LoadBag(RcGpuParams params, uint32_t table, uint32_t slot, uint32_t level) {
    const uint32_t base = (table == 0 ? params.stateBags0 : params.stateBags1) + (slot * RC_GPU_BAG_WORDS * 8);
    const uint64_t key = LoadWord(base) - 1;
    RcBagShare share;
    share.j = (uint32_t)(key & 511);
    share.i = (uint32_t)((key >> 9) & 511);
    share.face = (uint32_t)((key >> 18) & 7);
    const uint64_t blockMask = ((uint64_t)1 << RC_GPU_BLOCK_BITS) - 1;
    share.homeZ = (int64_t)((key >> 21) & blockMask) + (params.lowZ >> level);
    share.homeY = (int64_t)((key >> (21 + RC_GPU_BLOCK_BITS)) & blockMask) + (params.lowY >> level);
    share.homeX = (int64_t)((key >> (21 + (2 * RC_GPU_BLOCK_BITS))) & blockMask) + (params.lowX >> level);
    share.energyShort = LoadWord(base + 8);
    share.energyLong = LoadWord(base + 16);
    share.lobeShortX = LoadWord(base + 24);
    share.lobeShortY = LoadWord(base + 32);
    share.lobeShortZ = LoadWord(base + 40);
    share.lobeLongX = LoadWord(base + 48);
    share.lobeLongY = LoadWord(base + 56);
    share.lobeLongZ = LoadWord(base + 64);
    share.chosen = LoadWord(base + 72);
    share.level = level;

    return share;
}

// --- 段の光線 1 本を歩かせ、帳簿へ足し、残りを次の段へ ---

void WalkAndPass(RcGpuParams params, RcStepConstants constants, RcLevelRay levelRay, uint32_t nextTable,
                 uint32_t outParity) {
    const GpuRcGrid grid = GridOf(params);
    GpuRcSink sink;
    sink.grid = grid;
    sink.params = params;
    sink.outParity = outParity;
    const RcWalk walk = RcWalkLevelRay(constants, levelRay, grid, sink);

    AddHeaderWave(RC_GPU_RAYS + levelRay.level, 1);
    AddHeaderWave(RC_GPU_STEPS, (uint64_t)walk.steps);
    AddLedgerWave(LT_BAND_SHORT, 2, walk.absorbedShort);
    AddLedgerWave(LT_BAND_SHORT, 3, walk.reflectedShort);
    AddLedgerWave(LT_BAND_LONG, 2, walk.absorbedLong);
    AddLedgerWave(LT_BAND_LONG, 3, walk.reflectedLong);

    const bool last = levelRay.level + 1 >= params.levels;
    // 最後の段は世界の端まで歩くので残らない(歩みの上限で止まった時の保険: 外へ出たとして数える)
    AddLedgerWave(LT_BAND_SHORT, 4, walk.escapedShort + (last ? walk.remainingShort : 0));
    AddLedgerWave(LT_BAND_LONG, 4, walk.escapedLong + (last ? walk.remainingLong : 0));
    if (!last)
        RcPassToNextLevel(constants, levelRay, walk, params.lowX, params.lowY, params.lowZ, BagsOf(params, nextTable));
}

// --- 1 刻みの始まり: g_external1 = 入る持ち越しの表(0・1)---
[numthreads(RC_GROUP_THREADS, 1, 1)] void RcBeginTick(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t word = dispatchThreadId.x;
    const bool keep = word == RC_GPU_CARRY_COUNT + g_external1 || word == RC_GPU_OVERFLOW || word == RC_GPU_BAG_COUNT ||
                      word == RC_GPU_BAG_COUNT + 1 || word == RC_GPU_BAG_PEAK || word == RC_GPU_SKIPPED;
    if (word < RC_GPU_HEADER_WORDS && !keep)
        StoreWord(HeaderOffset(word), 0);
}

    // --- この刻みを流すか(T-1229。起動の予告の元。g_external1 = 入る持ち越しの表。1 スレッド)---
    // 飛ばしてよい = 放つセルが無い(作業の RC_GPU_WORK_EMITTERS が 0)・入る持ち越しの表が空(0 でない鍵の数が 0。数は多めにしかずれない)・
    // 前に流した刻みが何も放たず何も受けなかった(帳簿の放った・持ち越しを放った・そのまま送ったが 0 = 放った・吸ったの欄と出る表が 0)。
    // その時に流した刻みの結果は「頭の帳簿が 0・欄が 0・表が 0」で今の置き場と同じなので、飛ばしても結果は変わらない(D-428)
    [numthreads(RC_GROUP_THREADS, 1, 1)] void RcPlanTick(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (dispatchThreadId.x != 0)
        return;

    uint64_t pending = LoadWord(HeaderOffset(RC_GPU_CARRY_COUNT + g_external1));
    FX_UNROLL
    for (uint32_t band = 0; band < LT_BANDS; ++band) {
        pending |= LoadWord(HeaderOffset(RC_GPU_LEDGER + (band * RC_GPU_LEDGER_KINDS)));
        pending |= LoadWord(HeaderOffset(RC_GPU_LEDGER + (band * RC_GPU_LEDGER_KINDS) + 1));
        pending |= LoadWord(HeaderOffset(RC_GPU_KEPT + band));
    }

    const bool emitters = g_rcWork.Load(RC_GPU_WORK_EMITTERS) != 0u;
    const bool run = emitters || pending != 0;
    StoreWord(HeaderOffset(RC_GPU_RUN), run ? 1 : 0);
    if (!run)
        StoreWord(HeaderOffset(RC_GPU_SKIPPED), LoadWord(HeaderOffset(RC_GPU_SKIPPED)) + 1);

    g_rcWork.Store(RC_GPU_WORK_EMITTERS, 0u);
}

// --- セルごとに 6 面の熱の放つ量(RcFaceEmission。CPU の CollectSources の前半)---
[numthreads(RC_GROUP_THREADS, 1, 1)] void RcSources(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const RcGpuParams params = LoadParams();
    const GpuRcGrid grid = GridOf(params);
    for (uint32_t cell = dispatchThreadId.x; cell < params.cellCount; cell += g_external2) {
        const int64_t flat = (int64_t)cell;
        const int64_t x = params.lowX + (flat % params.sizeX);
        const int64_t y = params.lowY + ((flat / params.sizeX) % params.sizeY);
        const int64_t z = params.lowZ + (flat / (params.sizeX * params.sizeY));
        const uint32_t temperature = g_rcWork.Load(params.workTemperatures + (cell * 4));
        // 参照の温度との差を運ぶ時(T-1102)は、冷たい物も負の側で放つので quietBelowMk は使わない
        const bool hot = temperature != 0 && (params.referenceWord != 0 || temperature >= params.quietBelowMk);
        if (!hot) {
            // 冷たいセルは読むだけ(放つ面の量はいつも 0。放った・吸ったは前の刻みに 0 でなければ 0 に。T-1010)
            const uint32_t emitted = params.stateEmitted + (cell * 16);
            const uint32_t absorbed = params.stateAbsorbed + (cell * 16);
            if ((LoadWord(emitted) | LoadWord(emitted + 8)) != 0) {
                StoreWord(emitted, 0);
                StoreWord(emitted + 8, 0);
            }

            if ((LoadWord(absorbed) | LoadWord(absorbed + 8)) != 0) {
                StoreWord(absorbed, 0);
                StoreWord(absorbed + 8, 0);
            }

            continue;
        }

        // 組の刻みでなければ、熱で放つ光は持ち越しの密な受けで待つ(T-1010。次の刻みの表は前の刻みの RcCarryIn で 0)
        const bool active = params.thermalInSlots == 0 || RcEmitSlotActive(ConstantsOf(params), x, y, z);
        uint64_t heldShort = 0;
        uint64_t heldLong = 0;
        const RcCellLight light = g_rcWork.Load<RcCellLight>(params.workLights + (cell * 24));
        // 面の向こうに依らない分はセルごとに 1 回(6 面で同じ。面の量 = RcFaceEmissionRelative。T-1161)。
        // 0 なら(参照と同じ温度・負の側で参照より暖かい物など)向こうのセルも読まない(どの面も 0)
        const RcBands cellBands = RcCellEmissionRelative(temperature, params.referenceWord, light);
        const bool emits = (cellBands.shortBand | cellBands.longBand) != 0;
        uint64_t emittedShort = 0;
        uint64_t emittedLong = 0;
        for (uint32_t face = 0; face < RC_FACES && emits; ++face) {
            const int64_t step = (face & 1u) == 0 ? 1 : -1;
            const uint32_t axis = face >> 1;
            const int64_t acrossX = x + (axis == 0 ? step : 0);
            const int64_t acrossY = y + (axis == 1 ? step : 0);
            const int64_t acrossZ = z + (axis == 2 ? step : 0);
            RcCellLight across;
            across.depthShortQ32 = 0;
            across.depthLongQ32 = 0;
            across.reflectanceShortQ16 = 0;
            across.reflectanceLongQ16 = 0;
            if (grid.Contains(acrossX, acrossY, acrossZ))
                across = grid.Light(acrossX, acrossY, acrossZ);

            const RcBands bands = RcMaskAcross(cellBands, across);

            // 放つ面の量は使った後に RcClearFaces が 0 に戻すので、量のある面だけ書いて列に足す
            const uint32_t sourceFace = (cell * RC_FACES) + face;
            const bool some = (bands.shortBand | bands.longBand) != 0;
            if (active && some) {
                StoreWord(params.stateSources + (sourceFace * 16), bands.shortBand);
                StoreWord(params.stateSources + (sourceFace * 16) + 8, bands.longBand);
                ListFace(params, sourceFace);
            } else if (some) {
                AddCarry(params, 1 - g_external1, ((uint64_t)sourceFace * 2) + LT_BAND_SHORT, bands.shortBand);
                AddCarry(params, 1 - g_external1, ((uint64_t)sourceFace * 2) + LT_BAND_LONG, bands.longBand);
                heldShort += bands.shortBand;
                heldLong += bands.longBand;
            }

            emittedShort += bands.shortBand;
            emittedLong += bands.longBand;
        }

        StoreWord(params.stateEmitted + (cell * 16), emittedShort);
        StoreWord(params.stateEmitted + (cell * 16) + 8, emittedLong);
        StoreWord(params.stateAbsorbed + (cell * 16), 0);
        StoreWord(params.stateAbsorbed + (cell * 16) + 8, 0);
        AddLedgerWave(LT_BAND_SHORT, 0, emittedShort);
        AddLedgerWave(LT_BAND_LONG, 0, emittedLong);
        AddHeaderWave(RC_GPU_HELD + LT_BAND_SHORT, heldShort);
        AddHeaderWave(RC_GPU_HELD + LT_BAND_LONG, heldLong);
    }
}

    // --- 前の刻みの持ち越し(g_external1 の表)を放つ面へ足し、表を 0 に戻す(鍵ごとに 1 スレッド。表の全部の鍵を掃く)---
    [numthreads(RC_GROUP_THREADS, 1, 1)] void RcCarryIn(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const RcGpuParams params = LoadParams();
    const uint32_t table = CarryTableOf(params, g_external1);
    for (uint32_t keyIndex = dispatchThreadId.x; keyIndex < params.carryKeys; keyIndex += g_external2) {
        const uint64_t key = (uint64_t)keyIndex;
        const uint64_t amount = LoadWord(table + (keyIndex * 8));
        if (amount == 0)
            continue;

        StoreWord(table + (keyIndex * 8), 0);
        const uint32_t band = (uint32_t)(key % 2);
        const uint32_t cell = (uint32_t)(key / (RC_FACES * 2));
        const int64_t flat = (int64_t)cell;
        const int64_t x = params.lowX + (flat % params.sizeX);
        const int64_t y = params.lowY + ((flat / params.sizeX) % params.sizeY);
        const int64_t z = params.lowZ + (flat / (params.sizeX * params.sizeY));
        if (!RcEmitSlotActive(ConstantsOf(params), x, y, z)) {
            // 組の刻みでない: 持ち越しの密な受けへ戻し、次の刻みへそのまま送る(T-1010)
            AddCarry(params, 1 - g_external1, key, amount);
            AddHeaderWave(RC_GPU_KEPT + LT_BAND_SHORT, band == LT_BAND_SHORT ? amount : 0);
            AddHeaderWave(RC_GPU_KEPT + LT_BAND_LONG, band == LT_BAND_LONG ? amount : 0);
            continue;
        }

        const uint32_t source = params.stateSources + (uint32_t)((key / 2) * 16) + (band * 8);
        StoreWord(source, LoadWord(source) + amount);
        ListFace(params, (uint32_t)(key / 2));
        // 波の和は語ごとに(帯がレーンごとに違うので、両方の帯を決まった語で呼ぶ)
        AddLedgerWave(LT_BAND_SHORT, 1, band == LT_BAND_SHORT ? amount : 0);
        AddLedgerWave(LT_BAND_LONG, 1, band == LT_BAND_LONG ? amount : 0);
    }
}

uint32_t FaceCountOf(RcGpuParams params) {
    const uint32_t count = (uint32_t)LoadWord(HeaderOffset(RC_GPU_FACE_COUNT));

    return count < params.faceCapacity ? count : params.faceCapacity;
}

// --- 段 0 の後: 列にした放つ面の量と印を 0 に戻す(次の刻みは冷たいセルの面を書かずに済む。T-1010)---
[numthreads(RC_GROUP_THREADS, 1, 1)] void RcClearFaces(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const RcGpuParams params = LoadParams();
    const uint32_t count = FaceCountOf(params);
    for (uint32_t item = dispatchThreadId.x; item < count; item += g_external2) {
        const uint32_t face = g_rcState.Load(params.stateFaces + (item * 4));
        StoreWord(params.stateSources + (face * 16), 0);
        StoreWord(params.stateSources + (face * 16) + 8, 0);
        g_rcState.Store(params.stateFaceFlags + (face * 4), 0u);
    }
}

RcLevelRay Level0RayOf(RcGpuParams params, RcStepConstants constants, uint32_t sourceKey, uint32_t texelIndex) {
    const uint32_t cell = sourceKey / RC_FACES;
    const uint32_t sourceFace = sourceKey % RC_FACES;
    const int64_t flat = (int64_t)cell;
    const int64_t x = params.lowX + (flat % params.sizeX);
    const int64_t y = params.lowY + ((flat / params.sizeX) % params.sizeY);
    const int64_t z = params.lowZ + (flat / (params.sizeX * params.sizeY));
    const uint32_t texel = g_rcWork.Load(params.workTexels + (((sourceFace * params.texelsPerFace) + texelIndex) * 4));

    return RcLevel0Ray(constants, (uint64_t)sourceKey, x, y, z, sourceFace, texelIndex, texel & 255u,
                       (texel >> 8) & 255u, texel >> 16);
}

uint64_t Level0WeightOf(RcGpuParams params, RcStepConstants constants, uint32_t sourceKey, uint32_t texelIndex) {
    return RcLevel0Weight(constants, Level0RayOf(params, constants, sourceKey, texelIndex), sourceKey % RC_FACES);
}

// --- 段 0: 放つ面ごとに升目の重みの和・余りの升目・余り(CPU の EmitFace の前半)---
[numthreads(RC_GROUP_THREADS, 1, 1)] void RcLevel0Prepare(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const RcGpuParams params = LoadParams();
    const RcStepConstants constants = ConstantsOf(params);
    const uint32_t count = FaceCountOf(params);
    for (uint32_t face = dispatchThreadId.x; face < count; face += g_external2) {
        const uint32_t sourceKey = g_rcState.Load(params.stateFaces + (face * 4));
        const uint32_t source = params.stateSources + (sourceKey * 16);
        const uint64_t energyShort = LoadWord(source);
        const uint64_t energyLong = LoadWord(source + 8);
        uint64_t weightSum = 0;
        for (uint32_t t = 0; t < params.texelsPerFace; ++t)
            weightSum += Level0WeightOf(params, constants, sourceKey, t);

        uint64_t givenShort = 0;
        uint64_t givenLong = 0;
        if (weightSum != 0) {
            for (uint32_t t = 0; t < params.texelsPerFace; ++t) {
                const uint64_t weight = Level0WeightOf(params, constants, sourceKey, t);
                givenShort += RcShare(energyShort, weight, weightSum);
                givenLong += RcShare(energyLong, weight, weightSum);
            }
        }

        const uint64_t hash = FxHash64(constants.seed, constants.tick, (uint64_t)sourceKey, RC_PURPOSE_REMAINDER);
        uint32_t lucky = FxRandomBelow(hash, params.texelsPerFace);
        for (uint32_t guard = 0;
             guard < params.texelsPerFace && weightSum != 0 && Level0WeightOf(params, constants, sourceKey, lucky) == 0;
             ++guard)
            lucky = (lucky + 1) % params.texelsPerFace;

        const uint32_t info = params.stateFaceInfo + (face * RC_GPU_FACE_INFO_WORDS * 8);
        StoreWord(info, weightSum);
        StoreWord(info + 8, (uint64_t)lucky);
        StoreWord(info + 16, energyShort - givenShort);
        StoreWord(info + 24, energyLong - givenLong);
    }
}

    // --- 段 0 の光線: 1 スレッド = (放つ面・升目)(CPU の EmitFace の後半)---
    [numthreads(RC_GROUP_THREADS, 1, 1)] void RcLevel0Walk(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const RcGpuParams params = LoadParams();
    const RcStepConstants constants = ConstantsOf(params);
    const uint32_t count = FaceCountOf(params) * params.texelsPerFace;
    for (uint32_t item = dispatchThreadId.x; item < count; item += g_external2) {
        const uint32_t face = item / params.texelsPerFace;
        const uint32_t texelIndex = item % params.texelsPerFace;
        const uint32_t info = params.stateFaceInfo + (face * RC_GPU_FACE_INFO_WORDS * 8);
        const uint64_t weightSum = LoadWord(info);
        if (weightSum == 0)
            continue;

        const uint32_t sourceKey = g_rcState.Load(params.stateFaces + (face * 4));
        const uint32_t source = params.stateSources + (sourceKey * 16);
        const bool lucky = LoadWord(info + 8) == (uint64_t)texelIndex;
        const uint64_t weight = Level0WeightOf(params, constants, sourceKey, texelIndex);
        const uint64_t energyShort = RcShare(LoadWord(source), weight, weightSum) + (lucky ? LoadWord(info + 16) : 0);
        const uint64_t energyLong = RcShare(LoadWord(source + 8), weight, weightSum) +
                                    (lucky ? LoadWord(info + 24) : 0);
        if (energyShort == 0 && energyLong == 0)
            continue;

        const RcLevelRay levelRay = RcLevel0WithEnergy(Level0RayOf(params, constants, sourceKey, texelIndex),
                                                       sourceKey % RC_FACES, energyShort, energyLong);
        WalkAndPass(params, constants, levelRay, 0, g_external3 >> 1);
    }
}

// --- 段 g_external1(≥ 1)の袋(表 g_external3)× raysPerBag 本。残りは他方の表へ ---
[numthreads(RC_GROUP_THREADS, 1, 1)] void RcLevelWalk(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const RcGpuParams params = LoadParams();
    const RcStepConstants constants = ConstantsOf(params);
    const uint32_t table = g_external3 & 1u;
    const uint32_t listOffset = table == 0 ? params.stateBagList0 : params.stateBagList1;
    const uint32_t bagCount = (uint32_t)LoadWord(HeaderOffset(RC_GPU_BAG_COUNT + table));
    const uint32_t count = (bagCount < params.bagCapacity ? bagCount : params.bagCapacity) * params.raysPerBag;
    for (uint32_t item = dispatchThreadId.x; item < count; item += g_external2) {
        const uint32_t slot = g_rcState.Load(listOffset + ((item / params.raysPerBag) * 4));
        const RcBagShare share = LoadBag(params, table, slot, g_external1);
        const RcLevelRay levelRay = RcBagRay(share, item % params.raysPerBag, params.raysPerBag, params.lowX,
                                             params.lowY, params.lowZ);
        if (levelRay.ray.energyShort == 0 && levelRay.ray.energyLong == 0)
            continue;

        WalkAndPass(params, constants, levelRay, 1 - table, g_external3 >> 1);
    }
}

    // --- 使い終わった袋の表(g_external3)の番地を空に ---
    [numthreads(RC_GROUP_THREADS, 1, 1)] void RcClearBags(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const RcGpuParams params = LoadParams();
    const uint32_t table = g_external3;
    const uint32_t listOffset = table == 0 ? params.stateBagList0 : params.stateBagList1;
    const uint32_t tableOffset = table == 0 ? params.stateBags0 : params.stateBags1;
    const uint32_t bagCount = (uint32_t)LoadWord(HeaderOffset(RC_GPU_BAG_COUNT + table));
    const uint32_t count = bagCount < params.bagCapacity ? bagCount : params.bagCapacity;
    for (uint32_t item = dispatchThreadId.x; item < count; item += g_external2) {
        const uint32_t base = tableOffset + (g_rcState.Load(listOffset + (item * 4)) * RC_GPU_BAG_WORDS * 8);
        for (uint32_t word = 0; word + 1 < RC_GPU_BAG_WORDS; ++word)
            StoreWord(base + (word * 8), 0);

        StoreWord(base + ((RC_GPU_BAG_WORDS - 1) * 8), RC_GPU_CHOSEN_EMPTY);
    }
}

// --- 袋の数を 0 に(RcClearBags の後。g_external3 = 表)---
[numthreads(RC_GROUP_THREADS, 1, 1)] void RcResetBags(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (dispatchThreadId.x != 0)
        return;

    uint64_t before;
    g_rcState.InterlockedMax64(HeaderOffset(RC_GPU_BAG_PEAK), LoadWord(HeaderOffset(RC_GPU_BAG_COUNT + g_external3)),
                               before);
    StoreWord(HeaderOffset(RC_GPU_BAG_COUNT + g_external3), 0);
}

    // --- 作るときに 1 回: 状態を 0 に(語ごと)---
    [numthreads(RC_GROUP_THREADS, 1, 1)] void RcInitState(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const RcGpuParams params = LoadParams();
    for (uint32_t word = dispatchThreadId.x; word < params.stateWords; word += g_external2)
        StoreWord(word * 8, 0);
}

// --- 作るときに 1 回: 袋の溜めを空に・初めの持ち越しを表 0 へ ---
[numthreads(RC_GROUP_THREADS, 1, 1)] void RcInitBags(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const RcGpuParams params = LoadParams();
    for (uint32_t slot = dispatchThreadId.x; slot < params.bagCapacity * 2; slot += g_external2) {
        const uint32_t table = slot < params.bagCapacity ? params.stateBags0 : params.stateBags1;
        const uint32_t base = table + ((slot % params.bagCapacity) * RC_GPU_BAG_WORDS * 8);
        StoreWord(base + ((RC_GPU_BAG_WORDS - 1) * 8), RC_GPU_CHOSEN_EMPTY);
    }

    // 初めの持ち越しは表 0 の鍵の所へ(鍵は (セル・面・帯) ごとに 1 つ)
    for (uint32_t entry = dispatchThreadId.x; entry < params.initialCarryCount; entry += g_external2) {
        const uint64_t key = g_rcWork.Load<uint64_t>(params.workCarry + (entry * 16));
        StoreWord(params.stateCarry0 + (uint32_t)(key * 8),
                  g_rcWork.Load<uint64_t>(params.workCarry + (entry * 16) + 8));
    }

    if (dispatchThreadId.x == 0)
        StoreWord(HeaderOffset(RC_GPU_CARRY_COUNT), (uint64_t)params.initialCarryCount);
}
