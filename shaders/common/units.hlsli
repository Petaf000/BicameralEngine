// units.hlsli — シミュの量の単位と桁(docs/design/04-numerics-determinism.md §2 の表と同じ値)。
// HLSL と C++ の両方から読む(fixed.hlsli の約束に従う)。量を足すときは、先に 04 §2 の表に行を足してからここに書く。
// ここにあるのはレベル 0(0.5 m のセル)の単位。レベルごとの質量・エネルギーの単位の換算は T-0017(多重解像度)で決める。
#ifndef BICAMERAL_UNITS_HLSLI
#define BICAMERAL_UNITS_HLSLI

#include "fixed.hlsli"

FX_NAMESPACE_BEGIN

// --- 時間 -------------------------------------------------------------------------------------
FX_CONST uint32_t TICKS_PER_SECOND = 60;           // 1 刻み = 世界時間 1/60 s(D-409)
FX_CONST uint32_t MAX_TICK_SUBDIVISION_SHIFT = 6;  // 局所の細分は 1/2^k(k ≤ 6)

// --- 長さ・位置・速度 --------------------------------------------------------------------------
FX_CONST uint32_t POSITION_FRACTION_BITS = 20;         // 位置の 1 = 2^-20 m
FX_CONST int64_t CELL_SIZE_LEVEL0 = (int64_t)1 << 19;  // レベル 0 のセルの一辺 0.5 m(位置の単位で)
FX_CONST uint32_t VELOCITY_FRACTION_BITS = 20;         // 速度の 1 = 2^-20 m/s(int32 で ±2048 m/s)

// --- 物質量・質量・エネルギー・温度(レベル 0)------------------------------------------------
// 物質量(uint64): 1 = 1 µmol(ADR-0012。反応の進行度も同じ)/ 質量(uint64、導出値): 1 = 1 mg
// エネルギー(int64): 1 = 1 mJ。内部エネルギー = 熱 + 化学(Σ 物質量 × H0)
FX_CONST uint64_t MICROMOLES_PER_MOLE = 1000000;
FX_CONST uint64_t MILLIGRAMS_PER_KILOGRAM = 1000000;
FX_CONST int64_t MILLIJOULES_PER_JOULE = 1000;
FX_CONST int64_t MICROJOULES_PER_MILLIJOULE = 1000;
// 物質の H0(int64): 1 = 1 J/mol = 1 µJ/µmol / 比熱(uint32): 1 = 1 mJ/(mol·K) / セルの熱容量(uint64): 1 = 1 nJ/K
// 温度(int32、導出値): 1 = 1 mK。0〜2.1e6 K
FX_CONST int32_t MILLIKELVIN_PER_KELVIN = 1000;
FX_CONST int32_t TEMPERATURE_MAX_MILLIKELVIN = 2100000000;

// --- 角度・回転・比率 --------------------------------------------------------------------------
// 角度(uint32): 1 周 = 2^32(FxSinCosTurn32)。回転の四元数と sin・cos の結果は Q1.30
FX_CONST uint32_t ROTATION_FRACTION_BITS = 30;
FX_CONST int32_t ROTATION_ONE = (int32_t)1 << 30;
// 比率・係数: 量ごとに Q16.16 か Q2.30 を選んで 04 §2 の表に書く
FX_CONST uint32_t RATIO_Q16_FRACTION_BITS = 16;
FX_CONST uint32_t RATIO_Q30_FRACTION_BITS = 30;
// log・exp の入出力(int64): Q32(1 = 2^-32)
FX_CONST uint32_t LOG_FRACTION_BITS = 32;

FX_NAMESPACE_END

#endif  // BICAMERAL_UNITS_HLSLI
