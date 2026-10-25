// probe_radiation.hlsl — 仮の世界の GPU の熱の放射の段(T-1154・D-646・D-658)。CPU リファレンスは
// engine/src/sim/probe_reference_radiation.cpp の ProbeReference::StepCascadeRadiation(ProbeReference::Advance の (1d))。
//
// 適用の単位の中(コマンド → 物理の熱 → 表の作り直し → 気体 → 液体 → **放射** → 起こす刻みの来たブロックを一覧へ)で、
// engine/src/sim/probe_sim_radiation.cpp の RecordRadiation が次の順に投げる(段の間は全部の UAV の書き込みを待つ):
//   RadiationGrid   刻みの初めのセル(成分)+ 熱のキャッシュ(温度)→ 距離ごとの段の作業のバッファの光の値と温度(CPU の CascadeGrid と同じ並び)
//   (距離ごとの段の 1 刻み: shaders/sim/cascade_light.hlsl。sim/gpu_cascade_light の自分のルート署名で)
//   RadiationApply  セルごとの 吸った − 放った(2^−16 mJ)を最も近い mJ に丸めてエネルギーへ足し、変わったセルの熱のキャッシュを作り直し、
//                   ブロックを「刻みの直前に変わった」にして起こす刻みを今にする(気体の GasScatter と同じ)
// ハッシュの単位(HashCells の後)では RadiationHash が次の刻みへの持ち越し(セル・面・帯ごとの量)を状態のハッシュへ足す(D-619)。
// 結ぶもの: ProbeSim のルート署名の u15〜u17 を、この段の間だけ借りる(気体の区画の段は済んでいる。段の後で結び直す)。
// 浮動小数点は使わない(04 §4)。
#include "common/cascade_light.hlsli"
#include "common/probe_region_hash.hlsli"
#include "sim/probe_bindings.hlsli"

RWByteAddressBuffer rcWork : register(u15);    // 距離ごとの段の作業(RcGpuParams・升目の表・光の値・温度)
RWByteAddressBuffer rcState : register(u16);   // 距離ごとの段の状態(uint64 の語。放った・吸った・持ち越しの表)
RWByteAddressBuffer rcLights : register(u17);  // 物質の光の値(LtSpeciesLight × 物質の数。表ごと)

static const uint32_t RC_CELL_LIGHT_BYTES = 24;  // RcCellLight
static const uint32_t RC_SPECIES_LIGHT_BYTES = 16;

struct ProbeRadiationLights {
    uint32_t unused;

    LtSpeciesLight Light(uint32_t speciesId) {
        return rcLights.Load<LtSpeciesLight>(speciesId * RC_SPECIES_LIGHT_BYTES);
    }
};

RcGpuParams LoadRcParams() {
    return rcWork.Load<RcGpuParams>(0);
}

// --- 刻みの初めの写し(1 スレッド = 1 セル)---
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void RadiationGrid(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t cell = dispatchThreadId.x;
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
    rcWork.Store<RcCellLight>(params.workLights + (cell * RC_CELL_LIGHT_BYTES), light);
    rcWork.Store(params.workTemperatures + (cell * 4), thermal[index].temperature);  // 負は 0(熱のキャッシュの約束)
}

    // --- セルへ当てる(1 スレッド = 1 セル。丸めは CPU の RoundToMilliJoules と同じ: 半分は +∞ 側・算術シフト)---
    [numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void RadiationApply(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t cell = dispatchThreadId.x;
    if (cell >= PROBE_CELL_COUNT)
        return;

    const RcGpuParams params = LoadRcParams();
    const uint32_t emittedAt = params.stateEmitted + (cell * 16);
    const uint32_t absorbedAt = params.stateAbsorbed + (cell * 16);
    const uint64_t absorbed = rcState.Load<uint64_t>(absorbedAt) + rcState.Load<uint64_t>(absorbedAt + 8);
    const uint64_t emitted = rcState.Load<uint64_t>(emittedAt) + rcState.Load<uint64_t>(emittedAt + 8);
    const int64_t net = (int64_t)absorbed - (int64_t)emitted;
    const int64_t milliJoules = (net + ((int64_t)1 << (RC_ENERGY_SHIFT - 1))) >> RC_ENERGY_SHIFT;
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

// --- 次の刻みへの持ち越しを状態 S(tick + 1) のハッシュへ(持ち越しの表 1 − (tick & 1)。歩幅は起動したスレッドの数)---
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void RadiationHash(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const RcGpuParams params = LoadRcParams();
    const uint64_t tick = CurrentTick();
    const uint32_t outTable = 1u - (uint32_t)(tick & 1);
    const uint32_t carryList = outTable == 0 ? params.stateCarryList0 : params.stateCarryList1;
    const uint32_t listed = (uint32_t)rcState.Load<uint64_t>((RC_GPU_CARRY_COUNT + outTable) * 8);
    const uint32_t count = min(listed, params.carryCapacity);
    const uint32_t stride = argument;  // 起動したスレッドの数(RecordRadiationHash が単位の定数に入れる)

    if (dispatchThreadId.x == 0) {
        const uint32_t overflow = (uint32_t)rcState.Load<uint64_t>(RC_GPU_OVERFLOW * 8);
        DEBUG_ASSERT(overflow == 0, DebugFormat::ProbeRadiationOverflow, overflow);
    }

    uint64_t hash = 0;
    for (uint32_t entry = dispatchThreadId.x; entry < count; entry += stride) {
        const uint32_t key = (uint32_t)rcState.Load<uint64_t>(carryList + (entry * 16));
        const uint64_t amount = rcState.Load<uint64_t>(carryList + (entry * 16) + 8);
        if (amount == 0)
            continue;

        const uint32_t cell = key / (RC_FACES * LT_BANDS);
        const uint32_t face = (key / LT_BANDS) % RC_FACES;
        const uint32_t band = key % LT_BANDS;
        hash += ProbeCascadeCarryHashTerm(cell, face, band, amount);
    }

    const uint64_t sum = WaveActiveSum(hash);
    if (WaveIsFirstLane() && sum != 0) {
        uint64_t original;
        hashes.InterlockedAdd64(HashEntryAddress(tick + 1) + PROBE_HASH_OFFSET_HASH, sum, original);
    }
}
