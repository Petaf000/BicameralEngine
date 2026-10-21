// sun_column.hlsl — 太陽の影の柱の GPU(Compute。T-0910・ADR-0396)。1 スレッド = 光線 1 本で shaders/common/sun_column.hlsli の SunWalkRay を呼ぶ。
// ルート署名は多重解像度のもの(multires_bindings.hlsli。sim/gpu_multires.cpp)に、外のバッファ u4・u5 を足して使う(覗き窓と同じ形)。
//
// データの流れ(sim/gpu_sun_column.cpp が記録する):
//   u4 作業(CPU の MakeSunParams の SunParams + 物質の光の値。既定のヒープへ写したもの。読むだけ)+ 木(u0・u1・u6 の索引)
//   → SunClear(出力を 0 に)→ SunSweep(光線ごとに走らせ、セルの吸った・返した量と帳簿を 64bit の atomic の足し算で u5 へ)
// 足し算は可換なので、光線の処理の順によらず CPU リファレンス(sim/sun_column.cpp の SweepSunColumn)とビット一致する(R3・R4)。
// 木のセルは読むだけ(状態を変えない。当たる面へ熱として渡すのは使う側)。
#include "common/sun_column.hlsli"
#include "sim/multires_bindings.hlsli"

RWByteAddressBuffer g_sunWork : register(u4);    // [SunParams(128 バイト)][LtSpeciesLight × 物質]
RWByteAddressBuffer g_sunOutput : register(u5);  // uint64 の語(sun_column.hlsli の SunLedgerWord・SunCellWord)

static const uint32_t SUN_GROUP_THREADS = 64;  // sim/gpu_sun_column.cpp の SUN_GROUP_THREADS

// --- SunWalkRay に渡す道具(sun_column.hlsli の約束)---

struct GpuSunCells {
    uint32_t unused;

    RxCell Cell(uint32_t slot, uint32_t index) { return LoadCell(slot, index); }
};

struct GpuSunLights {
    uint32_t unused;

    LtSpeciesLight Light(uint32_t speciesId) {
        return g_sunWork.Load<LtSpeciesLight>(SUN_WORK_PARAMS_BYTES + (speciesId * SUN_SPECIES_LIGHT_BYTES));
    }
};

void AddOutput(uint32_t word, uint64_t amount) {
    if (amount == 0)
        return;

    uint64_t before;
    g_sunOutput.InterlockedAdd64(word * 8, amount, before);
}

struct GpuSunSink {
    uint32_t unused;

    void Deposit(uint32_t slot, uint32_t index, uint32_t band, uint64_t absorbed, uint64_t reflected) {
        AddOutput(SunCellWord(slot, index, band, SUN_KIND_ABSORBED), absorbed);
        AddOutput(SunCellWord(slot, index, band, SUN_KIND_REFLECTED), reflected);
    }
};

// --- 出力を 0 に: g_external0 = 語の数。1 スレッド = 1 語 ---
[numthreads(SUN_GROUP_THREADS, 1, 1)] void SunClear(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t word = dispatchThreadId.x;
    if (word >= g_external0)
        return;

    g_sunOutput.Store<uint64_t>(word * 8, 0);
}

    // --- 掃く: g_external0 = 光線の数(rayCountB × rayCountC)。1 スレッド = 光線 1 本 ---
    [numthreads(SUN_GROUP_THREADS, 1, 1)] void SunSweep(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t ray = dispatchThreadId.x;
    if (ray >= g_external0)
        return;

    const SunParams params = g_sunWork.Load<SunParams>(0);
    GpuSunCells cells;
    cells.unused = 0;
    GpuSunLights lights;
    lights.unused = 0;
    GpuSunSink sink;
    sink.unused = 0;

    const SunRay walked = SunWalkRay(MakeTree(), cells, lights, sink, params, ray % params.rayCountB,
                                     ray / params.rayCountB);

    // --- 帳簿(帯ごと)---
    AddOutput(SunLedgerWord(LT_BAND_SHORT, SUN_LEDGER_ENTERED), walked.enteredShort);
    AddOutput(SunLedgerWord(LT_BAND_SHORT, SUN_LEDGER_ABSORBED), walked.absorbedShort);
    AddOutput(SunLedgerWord(LT_BAND_SHORT, SUN_LEDGER_REFLECTED), walked.reflectedShort);
    AddOutput(SunLedgerWord(LT_BAND_SHORT, SUN_LEDGER_ESCAPED), walked.escapedShort);
    AddOutput(SunLedgerWord(LT_BAND_LONG, SUN_LEDGER_ENTERED), walked.enteredLong);
    AddOutput(SunLedgerWord(LT_BAND_LONG, SUN_LEDGER_ABSORBED), walked.absorbedLong);
    AddOutput(SunLedgerWord(LT_BAND_LONG, SUN_LEDGER_REFLECTED), walked.reflectedLong);
    AddOutput(SunLedgerWord(LT_BAND_LONG, SUN_LEDGER_ESCAPED), walked.escapedLong);
}
