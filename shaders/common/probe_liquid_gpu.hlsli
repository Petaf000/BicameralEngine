// probe_liquid_gpu.hlsli — 仮の世界(ProbeSim)の GPU の液体の段(T-0613・ADR-0266・ADR-0276・ADR-0268)が C++ と HLSL で共有する置き場所の形。
// CPU リファレンスは engine/src/sim/probe_liquid.cpp(ProbeLiquidField::Step)。GPU は shaders/sim/probe_liquid_tick.hlsl が適用の単位の中
// (気体の段の後・起こす刻みの来たブロックを一覧へ足す前。CPU の (1c))で、区画のセルを読み書きする。
//
// バッファは 1 つ(liquidWork。RWByteAddressBuffer。ルート署名の u19): 先頭から 制御(数)・帳簿(外から入った運動量 × 3・位置のエネルギー)・
// 見出し(ProbeLiquidHeader。CPU が作る時に書く)、その後ろに見出しが場所を持つ欄:
//   - 物質ごとの値(ProbeLiquidSpecies。CPU が書く)
//   - 持ち越す状態(ProbeLiquidField の m_faces・m_kinetic・m_potential と同じ並び。作った時は 0 = 静止)
//   - 刻みの中だけの作業の値(刻みの初めのセルの写し・セルの値・圧力・面の力の後の運動量・流れ・移流・熱など。毎刻み書き直す)
// 区画の中の番号 = x + 幅 × (z + 奥行き × y)(ProbeLiquidField と同じ。y が上)。面の軸は格子の x・y・z。
// 構造化した値の並びが C++ と同じになるよう、64bit の値を先に・32bit の値を後に置く。
// 浮動小数点は使わない(shaders/common は検査の対象。04 §4)。
#ifndef BICAMERAL_PROBE_LIQUID_GPU_HLSLI
#define BICAMERAL_PROBE_LIQUID_GPU_HLSLI

#include "common/fixed.hlsli"

#ifdef __cplusplus
namespace bicameral::sim {
#endif

    // 物質ごとの液体の値(物質 ID の順。表から CPU が作る。16 バイト)
    struct ProbeLiquidSpecies {
        uint64_t molarVolume;  // mm³/mol(BakedReactionTable::speciesMolarVolume。密度を書いていなければ 0)
        uint32_t molarMass;    // mg/mol
        uint32_t flags;        // PROBE_LIQUID_SPECIES_*
    };

    FX_CONST uint32_t PROBE_LIQUID_SPECIES_LIQUID = 1;     // phase = "liquid"(流れる)
    FX_CONST uint32_t PROBE_LIQUID_SPECIES_CONDENSED = 2;  // 凝縮相(固体と液体。場所を取る)

    // 刻みの初めのセルから読んだ値(probe_liquid.cpp の LiquidCellState と同じ。56 バイト)
    struct ProbeLiquidCellState {
        uint64_t mass;        // mg(液体)
        uint64_t volume;      // pL(液体)
        uint64_t room;        // pL(液体が入れる体積 = セル − 固体)
        uint64_t equivalent;  // mg(空きが液体で満ちた時の質量をセル全部に直したもの = 質量 ÷ 空き)
        int64_t heat;         // µJ(= エネルギー × 1000 − 化学)
        uint64_t capacity;    // nJ/K(セルの全部の成分)
        uint32_t open;        // Q16(液体の空き)
        uint32_t wall;        // 1 = 固体で満ちている(空き 0)
    };

    // 見出し(CPU が作る時に書く。場所は liquidWork の先頭からのバイト)
    struct ProbeLiquidHeader {
        uint32_t regionMin[3];   // 区画(格子の座標。y が上)
        uint32_t regionSize[3];  // 幅・高さ・奥行き(格子の x・y・z)
        uint32_t regionCellCount;
        uint32_t speciesCount;  // 表の物質の数
        uint32_t gravityMicrometersPerSecond2;
        uint32_t frictionShift;
        uint32_t overfillStiffness;
        uint32_t advection;  // 1 = 運動量を液体と一緒に運ぶ(ProbeLiquidOptions::advection)
        uint32_t maxSpeedMillimetersPerSecond;
        // --- 欄の場所(バイト)---
        uint32_t speciesOffset;           // ProbeLiquidSpecies × speciesCount
        uint32_t facesOffset;             // int64 × 3 × 区画のセル(面の運動量。持ち越す)
        uint32_t kineticOffset;           // int64 × 3 × 区画のセル(面の運動のエネルギー mJ。持ち越す)
        uint32_t potentialOffset;         // int64 × 区画のセル(位置のエネルギー mJ。持ち越す)
        uint32_t snapshotOffset;          // RxCell × 区画のセル(刻みの初めのセル)
        uint32_t stateOffset;             // ProbeLiquidCellState × 区画のセル
        uint32_t pressureOffset;          // int64 × 区画のセル
        uint32_t forcedOffset;            // int64 × 3 × 区画のセル(力の後の面の運動量)
        uint32_t desiredOffset;           // uint64 × 3 × 区画のセル(面の望む動く質量)
        uint32_t outgoingOffset;          // uint64 × 区画のセル(セルから出る望む質量の合計)
        uint32_t movedOffset;             // int64 × 3 × 区画のセル(面を実際に動いた質量。+ 向きが正)
        uint32_t leavingOffset;           // uint64 × 3 × 区画のセル(面から出る 2 倍の流れの合計)
        uint32_t carriedOffset;           // int64 × 3 × 区画のセル(運動量と一緒に運んだ後の運動のエネルギー)
        uint32_t releasedOffset;          // int64 × 3 × 区画のセル(運動のエネルギーから熱になる分)
        uint32_t massOffset;              // uint64 × 区画のセル(動かした後の液体の質量 mg)
        uint32_t potentialCarriedOffset;  // int64 × 区画のセル(液体と一緒に運んだ後の位置のエネルギー)
        uint32_t changedOffset;           // uint32 × 区画のセル(液体を動かして値が変わった)
        uint32_t listOffset;              // 区画のセルごとに PROBE_LIQUID_LIST_ENTRIES 件(成分の増減の作業場)
        uint32_t unused;
    };

    // --- 制御と帳簿(liquidWork の先頭)---
    FX_CONST uint32_t PROBE_LIQUID_CONTROL_STEPPED = 0;   // 流した刻みの数(通し)
    FX_CONST uint32_t PROBE_LIQUID_CONTROL_CHANGED = 4;   // この刻みに値が変わったセルの数
    FX_CONST uint32_t PROBE_LIQUID_CONTROL_OVERFLOW = 8;  // 成分がインラインの数を超えたセルの数(通し。CPU は止まる)
    FX_CONST uint32_t PROBE_LIQUID_LEDGER_EXTERNAL = 64;  // int64 × 3(外から入った運動量。ProbeLiquidLedger::external)
    FX_CONST uint32_t PROBE_LIQUID_LEDGER_ENERGY = 88;  // int64(位置のエネルギーの出入り mJ。ProbeLiquidLedger::energy)
    FX_CONST uint32_t PROBE_LIQUID_HEADER_OFFSET = 256;   // ProbeLiquidHeader
    FX_CONST uint32_t PROBE_LIQUID_DYNAMIC_OFFSET = 512;  // ここから見出しが場所を持つ欄
    FX_CONST uint32_t PROBE_LIQUID_ALIGNMENT = 256;
    FX_CONST uint32_t PROBE_LIQUID_SPECIES_BYTES = 16;
    FX_CONST uint32_t PROBE_LIQUID_CELL_STATE_BYTES = 56;

    // 成分の増減の作業場: 1 件 = 物質 ID(uint32)+ 空き(uint32)+ 物質量(int64)。1 セルの成分 + 流れ込む新しい液体の物質の数まで
    FX_CONST uint32_t PROBE_LIQUID_LIST_ENTRIES = 16;
    FX_CONST uint32_t PROBE_LIQUID_LIST_ENTRY_BYTES = 16;

#ifdef __cplusplus
}  // namespace bicameral::sim
#endif

#endif  // BICAMERAL_PROBE_LIQUID_GPU_HLSLI
