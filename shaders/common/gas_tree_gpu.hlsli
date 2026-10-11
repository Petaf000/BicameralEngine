// gas_tree_gpu.hlsli — 気体の多重解像度の木の GPU 版(T-0209。07 §2.5・ADR-0117)が C++ と HLSL で共有する置き場所の形:
// 面の形(どのセル・外・壁が面の両側か)・レベルごとの係数と小刻みの指数・セルの場所(レベル・層・自分の面の並びの範囲)・セルの端数・粗い小刻みの間に出した量(T-0356)・帳簿の並び。
// C++ では engine/src/sim/gpu_gas_tree.cpp が GasTree(sim/gas_tree.h)から詰めて写し、HLSL では shaders/sim/gas_tree_step.hlsl が読む。
// セル・導く値・面の動く値・セルごとの合計・設定は 1 レベルの形(common/gas_gpu.hlsli)をそのまま使う。
// 64bit の値を先に・32bit の値を後に置き、32bit の数は偶数にそろえる(gas_gpu.hlsli と同じ約束)。浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_GAS_TREE_GPU_HLSLI
#define BICAMERAL_GAS_TREE_GPU_HLSLI

#include "common/gas_gpu.hlsli"

#ifdef __cplusplus
namespace bicameral::sim {
#endif

    // --- 面の片側の種類(sim::GasTreeSideKind と同じ値)---
    FX_CONST uint32_t GAS_TREE_GPU_SIDE_CELL = 0;
    FX_CONST uint32_t GAS_TREE_GPU_SIDE_GHOST = 1;
    FX_CONST uint32_t GAS_TREE_GPU_SIDE_WALL = 2;
    FX_CONST uint32_t GAS_TREE_GPU_SIDE_MIXED = 3;  // MUSCL の外側の隣がレベルの違うセル(勾配を使わない)

    // 面の形(木を作った時に一度だけ。sim::GasTreeFaceLayout と同じ中身。64 バイト)。
    // 側の番号: セルはセルの番号、外は層の表の番号(レベルの層の始め + z)
    struct GasTreeGpuFace {
        uint32_t leftKind;
        uint32_t leftIndex;
        uint32_t leftLevel;
        uint32_t rightKind;
        uint32_t rightIndex;
        uint32_t rightLevel;
        uint32_t leftOuterKind;
        uint32_t leftOuterIndex;
        uint32_t rightOuterKind;
        uint32_t rightOuterIndex;
        uint32_t axis;
        uint32_t level;  // 面を計算する単位(細かい側のレベル)
        uint32_t id;     // 面の番号(乱数の丸め。そのレベルの座標から)
        uint32_t unused0;
        uint32_t unused1;
        uint32_t unused2;
    };

    // レベルごとの係数(sim::GasTreeLevel の coefficients のうちレベルで変わるもの。40 バイト)
    struct GasTreeGpuLevel {
        uint64_t centralFlow;
        uint64_t pressureFlow;
        uint64_t pressureImpulse;
        uint64_t gravityImpulse;
        uint32_t layerBegin;        // 層の表(基準の導く値・開いた境界の外)の始め
        uint32_t subcycleExponent;  // σ: 1 刻みに substeps × 2^σ 回の小刻み(T-0356)
    };

    // セルの場所(16 バイト)。自分の面は面の並び(u32: 面の番号 × 2 + 自分が右側なら 1)の [faceBegin, faceEnd)
    struct GasTreeGpuPlace {
        uint32_t level;
        uint32_t layer;  // 層の表の番号(レベルの層の始め + z)
        uint32_t faceBegin;
        uint32_t faceEnd;
    };

    // セルの端数(sim::GasTreeFraction と同じ並び。2^-64 × そのセルのレベルの単位。64 バイト)
    struct GasTreeGpuFraction {
        uint64_t amounts[GAS_GPU_MAX_SPECIES];
        uint64_t energy;
        uint64_t momentum[3];
    };

    // 粗い小刻み 1 回の間に出した量(セルごと。gas_tree.cpp の TreeWork の committedOutflow・taken。T-0356)。
    // セルの小刻みの初めに 0 にし、細かい面が何度も引く時の抑えに使う。40 バイト
    struct GasTreeGpuCarry {
        uint64_t committedOutflow;            // 同じ小刻みの面が出す割合の和(Q32。抑えた後)
        uint64_t taken[GAS_GPU_MAX_SPECIES];  // 出した物質量(風上のレベル + 1 の単位)
    };

    // 帳簿(RWByteAddressBuffer。sim::GasTreeTotals と同じ並び = whole(int64)+ fraction(uint64)× 8。レベル 0 の単位)。
    // 足し込みは下位(fraction)へ 64bit の原子的な足し算 → 繰り上がりと上位を whole へ(どちらも足し算なので順に依存しない)
    FX_CONST uint32_t GAS_TREE_GPU_LEDGER_AMOUNTS = 0;    // GasQ64 × GAS_GPU_MAX_SPECIES
    FX_CONST uint32_t GAS_TREE_GPU_LEDGER_ENERGY = 64;    // GasQ64
    FX_CONST uint32_t GAS_TREE_GPU_LEDGER_MOMENTUM = 80;  // GasQ64 × 3
    FX_CONST uint32_t GAS_TREE_GPU_LEDGER_STRIDE = 16;    // GasQ64 の大きさ
    FX_CONST uint32_t GAS_TREE_GPU_LEDGER_BYTES = 128;

#ifdef __cplusplus
}  // namespace bicameral::sim
#endif

#endif  // BICAMERAL_GAS_TREE_GPU_HLSLI
