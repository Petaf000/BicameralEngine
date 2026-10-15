// gas_gpu.hlsli — 気体の流れの GPU 版(G3 の第 1 段。T-0185・07 §2.3)が C++ と HLSL で共有する置き場所の形:
// パラメータ(設定と係数。GasConfig・GasCoefficients から作る)・セル・導く値・面・セルごとの合計・帳簿の並び。
// C++ では engine/src/sim/gpu_gas.cpp が GasBox(sim/gas_reference.h)から詰めて写し、HLSL では shaders/sim/gas_step.hlsl が読む。
//
// 成分の数に上限は無い(T-0598・ADR-0284。CPU の GasPerSpecies〔T-0591〕と同じ)。成分ごとの欄を持つもの(パラメータ・セル・面・
// セルごとの合計・帳簿)は「決まった形の頭 + 成分ごとの欄 × 欄の数」の 1 件にし、ByteAddressBuffer に件ごとに並べる。
// 欄の数(speciesStride)は箱を作る時に決まり、ルート定数で渡す(GetDimensions は使わない。T-0024)。回す成分の数は設定の speciesCount
// (≤ 欄の数。仮の世界では刻みごとに GPU が決める)。
// 構造化バッファの並びが C++ と同じになるよう、64bit の値を先に・32bit の値を後に置き、32bit の数は偶数にそろえる(physics_solver.hlsli と同じ約束)。
// 浮動小数点は使わない(shaders/common は検査の対象。04 §4)。
#ifndef BICAMERAL_GAS_GPU_HLSLI
#define BICAMERAL_GAS_GPU_HLSLI

#include "common/fixed.hlsli"

#ifdef __cplusplus
namespace bicameral::sim {
#endif

    FX_CONST uint32_t
        GAS_GPU_FACES_PER_CELL = 6;  // 面の番号 = (セル × 3 + 軸) × 2 + 端の境界の面(gas_reference.cpp の MakeFace)

    // --- 境界・面の片側・成分の延ばし方(gas_reference.h の enum と同じ値)---
    FX_CONST uint32_t GAS_GPU_BOUNDARY_WALL = 0;
    FX_CONST uint32_t GAS_GPU_BOUNDARY_PERIODIC = 1;
    FX_CONST uint32_t GAS_GPU_BOUNDARY_OPEN = 2;
    FX_CONST uint32_t GAS_GPU_RECONSTRUCTION_UPWIND = 0;
    FX_CONST uint32_t GAS_GPU_RECONSTRUCTION_MINMOD = 1;
    FX_CONST uint32_t GAS_GPU_RECONSTRUCTION_MC = 2;
    FX_CONST uint32_t GAS_GPU_THERMODYNAMICS_ISOTHERMAL = 0;  // sim::GasThermodynamics と同じ値(T-0234)
    FX_CONST uint32_t GAS_GPU_THERMODYNAMICS_ADIABATIC = 1;

    // 設定と係数の頭(1 件。t0 の先頭。後ろに GasGpuSpecies × 欄の数)
    struct GasGpuParameters {
        // --- 64bit ---
        uint64_t centralFlow;
        uint64_t pressureFlow;
        uint64_t pressureImpulse;
        uint64_t gravityImpulse;
        uint64_t hydrostatic;  // 質量(mg)× これ ÷ 10^4 = 1 セルの高さの層の重さの圧力(µPa。世界の重力から。ADR-0171)
        uint64_t gravityWork;  // 質量(mg)× これ ÷ 10^6 = 1 セル上がる重さの仕事(µJ)
        uint64_t soundScale;
        uint64_t randomSeed;

        // --- 32bit ---
        uint32_t size[3];
        uint32_t boundary[3];
        uint32_t speciesCount;  // 回す成分の数(≤ 欄の数)
        uint32_t soundTemperature;
        uint32_t gravity;             // 0 / 1
        uint32_t reconstruction;      // GAS_GPU_RECONSTRUCTION_*
        uint32_t stochasticRounding;  // 0 / 1
        uint32_t thermodynamics;      // GAS_GPU_THERMODYNAMICS_*(T-0234)
    };

    // 成分ごとの値(パラメータの頭の後ろに欄の数だけ。24 バイト)
    struct GasGpuSpecies {
        uint64_t massPerAmount;   // mg/µmol の Q32
        int64_t formationEnergy;  // J/mol
        uint32_t heatCapacity;    // mJ/(mol·K)
        uint32_t roundingKey;     // 丸めの乱数の鍵(既定は欄の番号。世界のセルに載せる時は物質 ID。T-0591・T-0592)
    };

    // セルの頭(32 バイト。後ろに物質量 uint64 × 欄の数〔µmol〕)
    struct GasGpuCell {
        int64_t energy;       // mJ
        int64_t momentum[3];  // mg·2^-20 m/s
    };

    // 導く値(sim::GasDerived と同じ並び。48 バイト。成分に依らない)
    struct GasGpuDerived {
        uint64_t mass;
        uint64_t inverseMass;
        uint64_t inverseAmount;
        uint32_t temperature;
        uint32_t unused;
        int64_t pressure;
        int64_t scaledPressure;
    };

    // 面の頭(80 バイト。後ろに移す成分の物質量 uint64 × 欄の数〔µmol〕。番号は GAS_GPU_FACES_PER_CELL × セル。端の境界の面のうち、
    // 無い所は使わない)
    struct GasGpuFace {
        int64_t impulse;           // 面の圧力の力積(左のセルから引き、右のセルへ足す)
        int64_t weightLeft;        // 縦の面の重さ(左のセルの運動量 z から引く)
        int64_t weightRight;       // 同じく右のセル
        int64_t flow;              // 質量の流れ(mg/小刻み。左 → 右が正)
        uint64_t fraction;         // 風上の中身のうち移す割合(Q32。流れの段は抑える前、移す量の段で抑えた後)
        int64_t movedEnergy;       // 風上から引くエネルギー(mJ)
        int64_t receivedEnergy;    // 風下へ足すエネルギー(mJ。断熱では重さの仕事を引いた値。T-0234)
        int64_t movedMomentum[3];  // 移す運動量
    };

    // セルごとの合計の頭(8 バイト。風上のセルとして出ていく割合の合計。後ろに成分ごとの移す量の合計 uint64 × 欄の数)
    struct GasGpuCellSums {
        uint64_t outflow;  // Q32
    };

    FX_CONST uint32_t GAS_GPU_PARAMETERS_BYTES = 112;
    FX_CONST uint32_t GAS_GPU_SPECIES_BYTES = 24;
    FX_CONST uint32_t GAS_GPU_CELL_BYTES = 32;
    FX_CONST uint32_t GAS_GPU_FACE_BYTES = 80;
    FX_CONST uint32_t GAS_GPU_SUMS_BYTES = 8;

    // 帳簿(RWByteAddressBuffer。外から入った量を正。64bit の原子的な足し算で足す)
    FX_CONST uint32_t GAS_GPU_LEDGER_ENERGY = 0;    // int64
    FX_CONST uint32_t GAS_GPU_LEDGER_MOMENTUM = 8;  // int64 × 3
    FX_CONST uint32_t GAS_GPU_LEDGER_AMOUNTS = 32;  // int64 × 欄の数

    // --- 1 件の大きさ(バイト。欄の数から)---
    FX_FN uint32_t GasGpuParametersBytes(uint32_t speciesStride) {
        return GAS_GPU_PARAMETERS_BYTES + (speciesStride * GAS_GPU_SPECIES_BYTES);
    }

    FX_FN uint32_t GasGpuCellStride(uint32_t speciesStride) {
        return GAS_GPU_CELL_BYTES + (speciesStride * 8);
    }

    FX_FN uint32_t GasGpuFaceStride(uint32_t speciesStride) {
        return GAS_GPU_FACE_BYTES + (speciesStride * 8);
    }

    FX_FN uint32_t GasGpuSumsStride(uint32_t speciesStride) {
        return GAS_GPU_SUMS_BYTES + (speciesStride * 8);
    }

    FX_FN uint32_t GasGpuLedgerBytes(uint32_t speciesStride) {
        return GAS_GPU_LEDGER_AMOUNTS + (speciesStride * 8);
    }

#ifdef __cplusplus
}  // namespace bicameral::sim
#endif

#endif  // BICAMERAL_GAS_GPU_HLSLI
