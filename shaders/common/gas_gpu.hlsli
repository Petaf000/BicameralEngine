// gas_gpu.hlsli — 気体の流れの GPU 版(G3 の第 1 段。T-0185・07 §2.3)が C++ と HLSL で共有する置き場所の形:
// パラメータ(設定と係数。GasConfig・GasCoefficients から作る)・セル・導く値・面・セルごとの合計・帳簿の並び。
// C++ では engine/src/sim/gpu_gas.cpp が GasBox(sim/gas_reference.h)から詰めて写し、HLSL では shaders/sim/gas_step.hlsl が読む。
// 構造化バッファの並びが C++ と同じになるよう、64bit の値を先に・32bit の値を後に置き、32bit の数は偶数にそろえる(physics_solver.hlsli と同じ約束)。
// 浮動小数点は使わない(shaders/common は検査の対象。04 §4)。
#ifndef BICAMERAL_GAS_GPU_HLSLI
#define BICAMERAL_GAS_GPU_HLSLI

#include "common/fixed.hlsli"

#ifdef __cplusplus
namespace bicameral::sim {
#endif

    FX_CONST uint32_t GAS_GPU_MAX_SPECIES = 4;  // sim::GAS_MAX_SPECIES と同じ
    FX_CONST uint32_t
        GAS_GPU_FACES_PER_CELL = 6;  // 面の番号 = (セル × 3 + 軸) × 2 + 端の境界の面(gas_reference.cpp の MakeFace)

    // --- 境界・面の片側・成分の延ばし方(gas_reference.h の enum と同じ値)---
    FX_CONST uint32_t GAS_GPU_BOUNDARY_WALL = 0;
    FX_CONST uint32_t GAS_GPU_BOUNDARY_PERIODIC = 1;
    FX_CONST uint32_t GAS_GPU_BOUNDARY_OPEN = 2;
    FX_CONST uint32_t GAS_GPU_RECONSTRUCTION_UPWIND = 0;
    FX_CONST uint32_t GAS_GPU_RECONSTRUCTION_MINMOD = 1;
    FX_CONST uint32_t GAS_GPU_RECONSTRUCTION_MC = 2;

    // 設定と係数(1 個。t0)
    struct GasGpuParameters {
        // --- 64bit ---
        uint64_t massPerAmount[GAS_GPU_MAX_SPECIES];   // mg/µmol の Q32
        int64_t formationEnergy[GAS_GPU_MAX_SPECIES];  // J/mol
        uint64_t centralFlow;
        uint64_t pressureFlow;
        uint64_t pressureImpulse;
        uint64_t gravityImpulse;
        uint64_t soundScale;
        uint64_t randomSeed;

        // --- 32bit ---
        uint32_t heatCapacity[GAS_GPU_MAX_SPECIES];  // mJ/(mol·K)
        uint32_t size[3];
        uint32_t boundary[3];
        uint32_t speciesCount;
        uint32_t soundTemperature;
        uint32_t gravity;             // 0 / 1
        uint32_t reconstruction;      // GAS_GPU_RECONSTRUCTION_*
        uint32_t stochasticRounding;  // 0 / 1
        uint32_t unused;
    };

    // セル(sim::GasCell と同じ並び。64 バイト)
    struct GasGpuCell {
        uint64_t amounts[GAS_GPU_MAX_SPECIES];  // µmol
        int64_t energy;                         // mJ
        int64_t momentum[3];                    // mg·2^-20 m/s
    };

    // 導く値(sim::GasDerived と同じ並び。48 バイト)
    struct GasGpuDerived {
        uint64_t mass;
        uint64_t inverseMass;
        uint64_t inverseAmount;
        uint32_t temperature;
        uint32_t unused;
        int64_t pressure;
        int64_t scaledPressure;
    };

    // 面(番号は GAS_GPU_FACES_PER_CELL × セル。端の境界の面のうち、無い所は使わない)
    struct GasGpuFace {
        int64_t impulse;                      // 面の圧力の力積(左のセルから引き、右のセルへ足す)
        int64_t weightLeft;                   // 縦の面の重さ(左のセルの運動量 z から引く)
        int64_t weightRight;                  // 同じく右のセル
        int64_t flow;                         // 質量の流れ(mg/小刻み。左 → 右が正)
        uint64_t fraction;                    // 風上の中身のうち移す割合(Q32。流れの段は抑える前、移す量の段で抑えた後)
        uint64_t moved[GAS_GPU_MAX_SPECIES];  // 移す成分の物質量(µmol)
        int64_t movedEnergy;                  // 移すエネルギー(mJ)
        int64_t movedMomentum[3];             // 移す運動量
    };

    // セルごとの合計(風上のセルとして出ていく割合の合計・成分ごとの移す量の合計)
    struct GasGpuCellSums {
        uint64_t outflow;                        // Q32
        uint64_t outgoing[GAS_GPU_MAX_SPECIES];  // µmol
    };

    // 帳簿(RWByteAddressBuffer。外から入った量を正。sim::GasLedger と同じ並び。64bit の原子的な足し算で足す)
    FX_CONST uint32_t GAS_GPU_LEDGER_AMOUNTS = 0;    // int64 × GAS_GPU_MAX_SPECIES
    FX_CONST uint32_t GAS_GPU_LEDGER_ENERGY = 32;    // int64
    FX_CONST uint32_t GAS_GPU_LEDGER_MOMENTUM = 40;  // int64 × 3
    FX_CONST uint32_t GAS_GPU_LEDGER_BYTES = 64;

#ifdef __cplusplus
}  // namespace bicameral::sim
#endif

#endif  // BICAMERAL_GAS_GPU_HLSLI
