// sky_port.hlsli — 空との交換の口を 1 回の起動で掃く時の作業の並び(T-1147・ADR-0474。CPU は engine/src/sim/sky_port.cpp の MakeSkyPortWork、
// GPU は shaders/sim/sky_port.hlsl と engine/src/sim/gpu_sky_port.cpp)。HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。
//
// なぜまとめるか: 口の掃き(向き 4 つ × 正と負)を太陽の影の柱の起動 1 本ずつで走らせると、起動ごとに出力を 0 にし直し、
// 1 本の光線の数(64³ で約 1.7 万本)では GPU が埋まらない(T-1145 で 64³ に 4.42 ms)。そこで
//   - 帯ごとの正と負を 1 本の光線にまとめる(帯どうしは光線の中で混ざらない: SunPassSegment は帯ごとに別に減らし、量 0 の帯は何も足さない。
//     だから帯を分けた掃きを足したのと同じ語。足す先の側だけを帯ごとに選ぶ)
//   - 向きの掃きを 1 回の起動に並べる(スレッドの番号 → 向きと光線)
// 作業のバッファ(u4): [見出し SKY_PORT_HEADER_BYTES][SunParams × SKY_PORT_SWEEP_LIMIT][LtSpeciesLight × 物質]
//   見出し = 向きごとの光線の番号の終わり(uint32 × SKY_PORT_SWEEP_LIMIT。累積)。
// 出力(u5): [側 0(正)の語][側 1(負)の語]。それぞれ太陽の影の柱と同じ並び(SunLedgerWord・SunCellWord)。
// 浮動小数点は使わない。
#ifndef BICAMERAL_SKY_PORT_HLSLI
#define BICAMERAL_SKY_PORT_HLSLI

#include "sun_column.hlsli"

MR_NAMESPACE_BEGIN

FX_CONST uint32_t SKY_PORT_SWEEP_LIMIT = 8;  // 1 回の起動に並べる向きの数の上限
FX_CONST uint32_t
    SKY_PORT_HEADER_BYTES = 64;  // 向きごとの光線の終わり(4 × 8 = 32 バイト)+ 空き。SunParams を 8 の倍数に置く
FX_CONST uint32_t SKY_PORT_PARAMS_BYTES = SKY_PORT_HEADER_BYTES + (SKY_PORT_SWEEP_LIMIT * SUN_WORK_PARAMS_BYTES);
FX_CONST uint32_t SKY_PORT_SIDES = 2;  // 0 = 正の側(吸った分を足す)・1 = 負の側(吸った分を引く)

// 語 word(太陽の影の柱の並び)の帯
FX_FN uint32_t SkyPortWordBand(uint32_t word) {
    if (word < SUN_LEDGER_WORDS)
        return word / SUN_LEDGER_ENTRIES;

    return (word - SUN_LEDGER_WORDS) % LT_BANDS;
}

MR_NAMESPACE_END

#endif  // BICAMERAL_SKY_PORT_HLSLI
