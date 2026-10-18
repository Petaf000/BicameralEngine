// probe_gas_gpu.hlsli — 仮の世界(ProbeSim)の GPU の気体の段(T-0592・ADR-0256)が C++ と HLSL で共有する置き場所の形。
// CPU リファレンスは engine/src/sim/probe_gas.cpp(ProbeGasField)。GPU は shaders/sim/probe_gas_tick.hlsl が適用の単位の中
// (物理の熱・表の作り直しの後、起こす刻みの来たブロックを一覧へ足す前)で、区画のセル → 気体の箱(gas_step.hlsl の GpuGas)→ セルへ書き戻す。
//
// バッファ:
//   - 気体の欄(gasState。区画のセルごとの ProbeGasCellState。ProbeGasField の m_momentum・m_kinetic・m_mass と同じ並び = 区画の中の
//     x + 幅 × (z + 奥行き × y)。運動量の軸は気体の箱の x・y・z〔= 格子の x・z・y〕)
//   - 気体の作業場(gasWork。RWByteAddressBuffer): 先頭から 制御(欄の物質の数・断った印・運動量の失せ分・数)・
//     設定の型(CPU が書く。設定の頭の物質に依らない欄)・見出し(ProbeGasHeader)、その後ろに見出しが場所を持つ
//     設定(頭 + 欄ごとの物質の値。刻みごとに欄の物質で作る。gas_step の t0)・開いた境界の外(gas_step の t2。刻みごと)・その基準の大気の並びの元・
//     物質ごとの値・物質の印・欄の物質・物質 → 欄・基準の大気の物質・帳簿・凝縮相のエネルギー・セルの気体の空き(gas_step の t3。T-0617)
//   - 気体の箱のセル・外は「頭 + 物質量 × 欄の数」の件(gas_gpu.hlsli)。欄の数 = 表の気体の物質の数(区画に現れうる気体の物質の数。T-0598)
// 構造化バッファの並びが C++ と同じになるよう、64bit の値を先に・32bit の値を後に置き、32bit の数は偶数にそろえる。
// 浮動小数点は使わない(shaders/common は検査の対象。04 §4)。
#ifndef BICAMERAL_PROBE_GAS_GPU_HLSLI
#define BICAMERAL_PROBE_GAS_GPU_HLSLI

#include "common/gas_gpu.hlsli"

#ifdef __cplusplus
namespace bicameral::sim {
#endif

    // 区画の 1 セルの気体の欄(40 バイト)
    struct ProbeGasCellState {
        int64_t momentum[3];  // mg·2^-20 m/s(気体の箱の軸)
        int64_t kinetic;      // 運動エネルギーのぶん(mJ。セルのエネルギーの外)
        uint64_t mass;        // その時の気体の質量(mg。段の外で質量が変わった分を見る。T-0646・ADR-0286)
    };

    // 物質ごとの気体の値(物質 ID の順。表から CPU が作る。40 バイト)
    struct ProbeGasSpecies {
        uint64_t massPerAmount;   // mg/µmol の Q32(gas_reference.cpp の MakeGasCoefficients と同じ式)
        int64_t formationEnergy;  // J/mol(表の h0)
        // 凝縮相が占める体積(mm³/mol。BakedReactionTable::speciesMolarVolume。気体の物質・密度を書いていない物質は 0。
        // セルの気体の空き〔sim/cell_volume の GasOpenQ16〕を作る。T-0617)
        uint64_t molarVolume;
        uint32_t heatCapacity;  // mJ/(mol·K)(表の比熱)
        uint32_t isGas;         // 凝縮相でない(表の condensed = false)
        uint32_t isLiquid;      // 液体(表の liquid。空きの頭打ちを固体と液体で別々に数える)
        uint32_t
            viscosity;  // 物質の粘り(µPa·s。表の viscosity_upa_s を GF_SPECIES_VISCOSITY_LIMIT で頭打ち。T-0758・T-0775)
    };

    // 見出し(CPU が作る時に書く。場所は gasWork の先頭からのバイト)
    struct ProbeGasHeader {
        uint32_t regionMin[3];   // 区画(格子の座標。y が上)
        uint32_t regionSize[3];  // 幅・高さ・奥行き(格子の x・y・z)
        uint32_t regionCellCount;
        uint32_t layerCount;    // = 高さ(気体の箱の z)
        uint32_t speciesCount;  // 表の物質の数
        uint32_t maskWords;     // 物質の印の語の数
        uint32_t atmosphereCount;
        uint32_t slotCapacity;   // 欄の数(気体の箱の件の欄。表の気体の物質の数。T-0598)
        uint32_t speciesOffset;  // ProbeGasSpecies × speciesCount
        uint32_t maskOffset;     // uint32 × maskWords
        // int64 × speciesCount(物質 ID ごと)+ エネルギー + 運動量 × 3(格子の軸)+ 内訳: 静止の丸めの運動量 × 3(格子の軸。運動量に含む)
        // + 静止の丸めで熱へ移した運動エネルギー(mJ。セルの中で移すのでエネルギーには入れない。T-0775)
        uint32_t ledgerOffset;
        uint32_t condensedOffset;        // int64 × regionCellCount(凝縮相のぶんのエネルギー。刻みの中だけ)
        uint32_t ghostOffset;            // セルの件 × layerCount(欄の物質の並び。刻みごと。gas_step の t2)
        uint32_t ghostAtmosphereOffset;  // セルの件 × layerCount(基準の大気の並び。CPU が書く)
        uint32_t atmosphereOffset;       // uint32 × atmosphereCount(基準の大気の物質。ID の順)
        uint32_t parametersOffset;       // 設定の頭 + GasGpuSpecies × 欄の数(刻みごと。gas_step の t0)
        uint32_t slotsOffset;            // uint32 × 欄の数(この刻みの欄の物質。ID の順)
        uint32_t slotOfOffset;  // uint32 × speciesCount(物質 ID → 欄。この刻みに欄が無ければ PROBE_GAS_NO_SLOT)
        uint32_t openOffset;    // uint32 × regionCellCount(セルの気体の空き Q16。刻みごと。gas_step の t3。T-0617)
        // 静止の丸め(ProbeGasOptions::restSpeedUnits。2^-20 m/s。0 = 切る。T-0723・T-0758・T-0775)
        uint32_t restSpeedUnits;
        uint32_t
            restBeforeOffset;  // uint64 × regionCellCount(刻みの初めの運動量のいちばん大きい軸の大きさ。静止の丸めが比べる)
        uint32_t unused;
    };

    FX_CONST uint32_t PROBE_GAS_NO_SLOT = 0xFFFFFFFFu;

    // --- 制御(gasWork の先頭)---
    FX_CONST uint32_t PROBE_GAS_CONTROL_SLOT_COUNT = 0;  // この刻みの欄の物質の数(欄の数まで)
    // 区画の気体の物質が欄の数を超えた(この刻みは流さない)。欄の数は表の気体の物質の数なので起きない(守りとして残す。T-0598)
    FX_CONST uint32_t PROBE_GAS_CONTROL_TOO_MANY = 4;
    FX_CONST uint32_t PROBE_GAS_CONTROL_OVERFLOW = 8;  // 書き戻すと 1 セルの成分がインラインの数を超える(流さない)
    FX_CONST uint32_t PROBE_GAS_CONTROL_CHANGED = 12;  // この刻みに値が変わったセルの数
    // 16〜31 は使わない(欄の物質は見出しの slotsOffset。T-0598)
    FX_CONST uint32_t PROBE_GAS_CONTROL_LOST = 32;              // int64 × 3(気体が無くなったセルの運動量。格子の軸)
    FX_CONST uint32_t PROBE_GAS_CONTROL_SKIPPED_TOO_MANY = 56;  // 数(通し)
    FX_CONST uint32_t PROBE_GAS_CONTROL_SKIPPED_OVERFLOW = 60;  // 数(通し)
    FX_CONST uint32_t PROBE_GAS_CONTROL_STEPPED = 64;           // 流した刻みの数(通し)
    FX_CONST uint32_t PROBE_GAS_CONTROL_SPECIES_SEEN = 68;      // この刻みの区画の気体の物質の数(欄に入らなかった分も)
    // 静止の丸め(この刻み。T-0775): 消した運動量 int64 × 3(格子の軸)・熱へ移した運動エネルギー int64(mJ)・止めたセルの数
    FX_CONST uint32_t PROBE_GAS_CONTROL_REST_MOMENTUM = 72;
    FX_CONST uint32_t PROBE_GAS_CONTROL_REST_ENERGY = 96;
    FX_CONST uint32_t PROBE_GAS_CONTROL_RESTED = 104;
    FX_CONST uint32_t PROBE_GAS_LEDGER_WORDS_AFTER_SPECIES =
        8;  // 帳簿の物質 ID ごとの後ろの語(エネルギー・運動量 × 3・丸めの内訳 × 4)

    FX_CONST uint32_t PROBE_GAS_TEMPLATE_OFFSET = 512;  // GasGpuParameters の型(設定の頭。物質に依らない欄)
    FX_CONST uint32_t PROBE_GAS_HEADER_OFFSET = 768;    // ProbeGasHeader
    FX_CONST uint32_t PROBE_GAS_DYNAMIC_OFFSET = 1024;  // ここから見出しが場所を持つ欄
    FX_CONST uint32_t PROBE_GAS_ALIGNMENT = 256;
    FX_CONST uint32_t PROBE_GAS_SPECIES_BYTES = 40;
    FX_CONST uint32_t PROBE_GAS_CELL_STATE_BYTES = 40;

    // 気体の箱の軸(x・y・z = 上)→ 格子の軸(x・z・y)
    FX_FN uint32_t ProbeGasGridAxis(uint32_t gasAxis) {
        return gasAxis == 0 ? 0u : (gasAxis == 1 ? 2u : 1u);
    }

#ifdef __cplusplus
}  // namespace bicameral::sim
#endif

#endif  // BICAMERAL_PROBE_GAS_GPU_HLSLI
