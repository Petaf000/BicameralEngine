// sky_port.hlsl — 空との交換の口を GPU で 1 回の起動で掃く(Compute。T-1147・ADR-0474)。1 スレッド = 光線 1 本で、
// 太陽の影の柱と同じ SunWalkRay(shaders/common/sun_column.hlsli)を呼ぶ。並びは shaders/common/sky_port.hlsli。
// ルート署名は多重解像度のもの(multires_bindings.hlsli。sim/gpu_multires.cpp)に、外のバッファ u4・u5 を足して使う(太陽の影の柱と同じ形)。
//
// データの流れ(sim/gpu_sky_port.cpp が記録する):
//   u4 作業(CPU の MakeSkyPortWork: 向きごとの SunParams + 光線の番号の終わり + 物質の光の値。既定のヒープへ写したもの)+ 木
//   → SkyPortClear(両方の側の出力を 0 に)→ SkyPortSweep(スレッドの番号 → 向きと光線。帯ごとに決めた側へ 64bit の atomic の足し算)
// 足し算は可換なので、光線の順と向きの順によらず CPU リファレンス(sim/sky_port.cpp の SweepSkyPort)とビット一致する(R3・R4)。
// 木のセルは読むだけ。
#include "common/sky_port.hlsli"
#include "sim/multires_bindings.hlsli"

RWByteAddressBuffer g_skyWork : register(u4);    // [見出し][SunParams × SKY_PORT_SWEEP_LIMIT][LtSpeciesLight × 物質]
RWByteAddressBuffer g_skyOutput : register(u5);  // [側 0 の語][側 1 の語](uint64)

static const uint32_t SKY_GROUP_THREADS = 64;  // sim/gpu_sky_port.cpp の SKY_GROUP_THREADS

// --- SunWalkRay に渡す道具(sun_column.hlsli の約束)---

struct GpuSkyCells {
    uint32_t unused;

    RxCell Cell(uint32_t slot, uint32_t index) { return LoadCell(slot, index); }
};

struct GpuSkyLights {
    uint32_t unused;

    LtSpeciesLight Light(uint32_t speciesId) {
        return g_skyWork.Load<LtSpeciesLight>(SKY_PORT_PARAMS_BYTES + (speciesId * SUN_SPECIES_LIGHT_BYTES));
    }
};

void AddSkyOutput(uint32_t word, uint64_t amount) {
    if (amount == 0)
        return;

    uint64_t before;
    g_skyOutput.InterlockedAdd64(word * 8, amount, before);
}

// 帯ごとに足す側の先頭の語(状態を持たない: 値で渡しても同じ所へ書く)
struct GpuSkySink {
    uint32_t shortBase;
    uint32_t longBase;

    void Deposit(uint32_t slot, uint32_t index, uint32_t band, uint64_t absorbed, uint64_t reflected) {
        const uint32_t base = band == LT_BAND_SHORT ? shortBase : longBase;
        AddSkyOutput(base + SunCellWord(slot, index, band, SUN_KIND_ABSORBED), absorbed);
        AddSkyOutput(base + SunCellWord(slot, index, band, SUN_KIND_REFLECTED), reflected);
    }
};

// --- 出力を 0 に: g_external0 = 両方の側の語の数。1 スレッド = 1 語 ---
[numthreads(SKY_GROUP_THREADS, 1, 1)] void SkyPortClear(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t word = dispatchThreadId.x;
    if (word >= g_external0)
        return;

    g_skyOutput.Store<uint64_t>(word * 8, 0);
}

// スレッドの光線の通し番号 → 向きの番号(見出しの「光線の番号の終わり」を決まった数だけ見る。T-0397)
uint32_t SkySweepOf(uint32_t ray, uint32_t sweepCount) {
    uint32_t sweep = 0;
    FX_UNROLL
    for (uint32_t k = 0; k < SKY_PORT_SWEEP_LIMIT; ++k) {
        const uint32_t end = g_skyWork.Load(k * 4);
        if (k < sweepCount && ray >= end)
            sweep = k + 1;
    }

    return sweep;
}

// --- 掃く: g_external0 = 光線の数の和・g_external1 = 向きの数・g_external2 = 1 つの側の語の数・
//     g_external3 = 帯ごとの側(bit 0 = 短い帯・bit 1 = 長い帯。1 = 負の側)---
[numthreads(SKY_GROUP_THREADS, 1, 1)] void SkyPortSweep(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t ray = dispatchThreadId.x;
    if (ray >= g_external0)
        return;

    const uint32_t sweep = SkySweepOf(ray, g_external1);
    const uint32_t start = sweep == 0 ? 0u : g_skyWork.Load((sweep - 1) * 4);
    const SunParams params = g_skyWork.Load<SunParams>(SKY_PORT_HEADER_BYTES + (sweep * SUN_WORK_PARAMS_BYTES));
    const uint32_t local = ray - start;

    GpuSkyCells cells;
    cells.unused = 0;
    GpuSkyLights lights;
    lights.unused = 0;
    GpuSkySink sink;
    sink.shortBase = (g_external3 & 1u) * g_external2;
    sink.longBase = ((g_external3 >> 1) & 1u) * g_external2;

    const SunRay walked = SunWalkRay(MakeTree(), cells, lights, sink, params, local % params.rayCountB,
                                     local / params.rayCountB);

    // --- 帳簿(帯ごとに、その帯の側へ)---
    AddSkyOutput(sink.shortBase + SunLedgerWord(LT_BAND_SHORT, SUN_LEDGER_ENTERED), walked.enteredShort);
    AddSkyOutput(sink.shortBase + SunLedgerWord(LT_BAND_SHORT, SUN_LEDGER_ABSORBED), walked.absorbedShort);
    AddSkyOutput(sink.shortBase + SunLedgerWord(LT_BAND_SHORT, SUN_LEDGER_REFLECTED), walked.reflectedShort);
    AddSkyOutput(sink.shortBase + SunLedgerWord(LT_BAND_SHORT, SUN_LEDGER_ESCAPED), walked.escapedShort);
    AddSkyOutput(sink.longBase + SunLedgerWord(LT_BAND_LONG, SUN_LEDGER_ENTERED), walked.enteredLong);
    AddSkyOutput(sink.longBase + SunLedgerWord(LT_BAND_LONG, SUN_LEDGER_ABSORBED), walked.absorbedLong);
    AddSkyOutput(sink.longBase + SunLedgerWord(LT_BAND_LONG, SUN_LEDGER_REFLECTED), walked.reflectedLong);
    AddSkyOutput(sink.longBase + SunLedgerWord(LT_BAND_LONG, SUN_LEDGER_ESCAPED), walked.escapedLong);
}
