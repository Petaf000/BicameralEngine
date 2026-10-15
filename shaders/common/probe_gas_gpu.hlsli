// probe_gas_gpu.hlsli — 仮の世界(ProbeSim)の GPU の気体の段(T-0592・ADR-0256)が C++ と HLSL で共有する置き場所の形。
// CPU リファレンスは engine/src/sim/probe_gas.cpp(ProbeGasField)。GPU は shaders/sim/probe_gas_tick.hlsl が適用の単位の中
// (物理の熱・表の作り直しの後、起こす刻みの来たブロックを一覧へ足す前)で、区画のセル → 気体の箱(gas_step.hlsl の GpuGas)→ セルへ書き戻す。
//
// バッファ:
//   - 気体の欄(gasState。区画のセルごとの ProbeGasCellState。ProbeGasField の m_momentum・m_kinetic と同じ並び = 区画の中の
//     x + 幅 × (z + 奥行き × y)。運動量の軸は気体の箱の x・y・z〔= 格子の x・z・y〕)
//   - 気体の作業場(gasWork。RWByteAddressBuffer): 先頭から 制御(刻みごとの欄の物質・断った印・運動量の失せ分・数)・
//     設定(GasGpuParameters。刻みごとに欄の物質で作る。gas_step の t0)・設定の型(CPU が書く。物質に依らない欄)・見出し(ProbeGasHeader)、
//     その後ろに見出しが場所を持つ 開いた境界の外(gas_step の t2。刻みごと)・その基準の大気の並びの元・物質ごとの値・物質の印・帳簿・凝縮相のエネルギー
// 構造化バッファの並びが C++ と同じになるよう、64bit の値を先に・32bit の値を後に置き、32bit の数は偶数にそろえる。
// 浮動小数点は使わない(shaders/common は検査の対象。04 §4)。
#ifndef BICAMERAL_PROBE_GAS_GPU_HLSLI
#define BICAMERAL_PROBE_GAS_GPU_HLSLI

#include "common/gas_gpu.hlsli"

#ifdef __cplusplus
namespace bicameral::sim {
#endif

    // 区画の 1 セルの気体の欄(32 バイト)
    struct ProbeGasCellState {
        int64_t momentum[3];  // mg·2^-20 m/s(気体の箱の軸)
        int64_t kinetic;      // 運動エネルギーのぶん(mJ。セルのエネルギーの外)
    };

    // 物質ごとの気体の値(物質 ID の順。表から CPU が作る。32 バイト)
    struct ProbeGasSpecies {
        uint64_t massPerAmount;   // mg/µmol の Q32(gas_reference.cpp の MakeGasCoefficients と同じ式)
        int64_t formationEnergy;  // J/mol(表の h0)
        uint32_t heatCapacity;    // mJ/(mol·K)(表の比熱)
        uint32_t isGas;           // 凝縮相でない(表の condensed = false)
        uint32_t unused[2];
    };

    FX_CONST uint32_t PROBE_GAS_MAX_ATMOSPHERE = GAS_GPU_MAX_SPECIES;  // 基準の大気の物質(GPU の欄の数まで。T-0598)

    // 見出し(CPU が作る時に書く。場所は gasWork の先頭からのバイト)
    struct ProbeGasHeader {
        uint32_t regionMin[3];   // 区画(格子の座標。y が上)
        uint32_t regionSize[3];  // 幅・高さ・奥行き(格子の x・y・z)
        uint32_t regionCellCount;
        uint32_t layerCount;    // = 高さ(気体の箱の z)
        uint32_t speciesCount;  // 表の物質の数
        uint32_t maskWords;     // 物質の印の語の数
        uint32_t atmosphereCount;
        uint32_t atmosphereSpecies[PROBE_GAS_MAX_ATMOSPHERE];  // ID の順
        uint32_t speciesOffset;                                // ProbeGasSpecies × speciesCount
        uint32_t maskOffset;                                   // uint32 × maskWords
        uint32_t ledgerOffset;           // int64 × speciesCount(物質 ID ごと)+ エネルギー + 運動量 × 3(格子の軸)
        uint32_t condensedOffset;        // int64 × regionCellCount(凝縮相のぶんのエネルギー。刻みの中だけ)
        uint32_t ghostOffset;            // GasGpuCell × layerCount(欄の物質の並び。刻みごと。gas_step の t2)
        uint32_t ghostAtmosphereOffset;  // GasGpuCell × layerCount(基準の大気の並び。CPU が書く)
        uint32_t unused[3];
    };

    // --- 制御(gasWork の先頭)---
    FX_CONST uint32_t PROBE_GAS_CONTROL_SLOT_COUNT = 0;  // この刻みの欄の物質の数(GPU の欄の数まで)
    FX_CONST uint32_t PROBE_GAS_CONTROL_TOO_MANY = 4;    // 区画の気体の物質が欄の数を超えた(この刻みは流さない。T-0598)
    FX_CONST uint32_t PROBE_GAS_CONTROL_OVERFLOW = 8;    // 書き戻すと 1 セルの成分がインラインの数を超える(流さない)
    FX_CONST uint32_t PROBE_GAS_CONTROL_CHANGED = 12;    // この刻みに値が変わったセルの数
    FX_CONST uint32_t PROBE_GAS_CONTROL_SLOTS = 16;      // uint32 × 4(欄の物質。ID の順)
    FX_CONST uint32_t PROBE_GAS_CONTROL_LOST = 32;       // int64 × 3(気体が無くなったセルの運動量。格子の軸)
    FX_CONST uint32_t PROBE_GAS_CONTROL_SKIPPED_TOO_MANY = 56;  // 数(通し)
    FX_CONST uint32_t PROBE_GAS_CONTROL_SKIPPED_OVERFLOW = 60;  // 数(通し)
    FX_CONST uint32_t PROBE_GAS_CONTROL_STEPPED = 64;           // 流した刻みの数(通し)
    FX_CONST uint32_t PROBE_GAS_CONTROL_SPECIES_SEEN = 68;      // この刻みの区画の気体の物質の数(欄に入らなかった分も)

    FX_CONST uint32_t PROBE_GAS_PARAMETERS_OFFSET = 256;  // GasGpuParameters(刻みごと。t0 は 256 バイトにそろえる)
    FX_CONST uint32_t PROBE_GAS_TEMPLATE_OFFSET = 512;    // GasGpuParameters の型(物質に依らない欄)
    FX_CONST uint32_t PROBE_GAS_HEADER_OFFSET = 768;      // ProbeGasHeader
    FX_CONST uint32_t PROBE_GAS_DYNAMIC_OFFSET = 1024;    // ここから見出しが場所を持つ欄
    FX_CONST uint32_t PROBE_GAS_ALIGNMENT = 256;
    FX_CONST uint32_t PROBE_GAS_SPECIES_BYTES = 32;
    FX_CONST uint32_t PROBE_GAS_CELL_STATE_BYTES = 32;

    // 気体の箱の軸(x・y・z = 上)→ 格子の軸(x・z・y)
    FX_FN uint32_t ProbeGasGridAxis(uint32_t gasAxis) {
        return gasAxis == 0 ? 0u : (gasAxis == 1 ? 2u : 1u);
    }

#ifdef __cplusplus
}  // namespace bicameral::sim
#endif

#endif  // BICAMERAL_PROBE_GAS_GPU_HLSLI
