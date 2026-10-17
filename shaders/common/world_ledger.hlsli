// world_ledger.hlsli — 世界の帳簿の 1 つの型(運ぶ計算の共通の土台の段 1 ②。T-0712。runner の設計 substrate-decided §2.4 ⑤)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU(engine/src/sim/multires_nest.cpp の AddWorldLedgerBits・
// ComputeConservedTotals)と GPU(shaders/sim/multires_bindings.hlsli の AddWorldLedgerCarrying)が同じ番地・同じ繰り上げを使う。
//
// 帳簿は「世界のセル(登録した保存量)へ外から入った量と、外へ出た量」を、理由 × 向き × レベル × 量 の欄に数える。
// 保存の検査は 1 つ: Σ 登録した保存量 − (入 − 出) = 一定(engine/src/sim/multires_nest.h の ComputeConservedTotals)。
//   量(列): 0 = エネルギー、1 + 物質 ID(元素は物質の組成から数える)。この後ろに、状態の登録(T-0711)が「保存量」とした欄
//           (運動量 ×3・位置/運動のエネルギー)の列を足す(今の MR は 0 本。列の数は MultiresCapacity::ledgerColumns)。
//   理由: 下の WL_REASON_*。向き: 出(セルから減った)・入(セルに増えた)。どちらも足すだけの符号なしの数で、差は検査の時に取る
//         (GPU は 64bit の atomic の足し算だけ。順によらない)。
//   レベル: 値はそのレベルの単位 × 2^-64 の数(MR の端数と同じ)。桁あふれ(= そのレベルの 1 単位)は 1 段粗い段の 2^61 へ繰り上げる
//         (レベルが 1 段粗いと単位は 8 倍)。丸ごとの量 units(そのレベルの単位)は 21 段粗い段の 2 × units と 22 段粗い段へ(WlSplitWholeUnits)。
// 番地: [欄 = 理由 × 2 + 向き][レベル − MR_LEDGER_LEVEL_MIN][列]。欄 0(粗くした端数・出)の番地は前の帳簿(MrLedgerAddress)と同じ。
// 浮動小数点は使わない。
#ifndef BICAMERAL_WORLD_LEDGER_HLSLI
#define BICAMERAL_WORLD_LEDGER_HLSLI

#include "multires_tree.hlsli"

MR_NAMESPACE_BEGIN

// --- 理由(帳簿の行の見出し。数を変えたら WL_REASON_COUNT も)---------------------------------------------
FX_CONST uint32_t WL_REASON_COARSEN_REMAINDER = 0;  // 粗くした端数(畳んだ平均の余り・返した端数の枠。前の MR の帳簿)
FX_CONST uint32_t WL_REASON_EDIT = 1;               // 改造(置く・消す・温度を決める。出 = 消した中身、入 = 置いた中身)
FX_CONST uint32_t WL_REASON_PHYSICS_HEAT = 2;       // 物理の熱(物の運動のエネルギー → セルの熱。MR にはまだ無い)
FX_CONST uint32_t WL_REASON_WORLD_EDGE = 3;         // 世界の端(開いた端からの出入り。段 3)
FX_CONST uint32_t WL_REASON_WALL_IMPULSE = 4;       // 壁の力積(閉じた端・固体が受けた運動量。段 2〜3)
FX_CONST uint32_t WL_REASON_GRAVITY_WORK = 5;       // 重さの仕事(位置のエネルギーとの出入り。段 2〜3)
FX_CONST uint32_t WL_REASON_SPEED_LIMIT = 6;        // 速さの上限(頭打ちで消した運動量・運動のエネルギー。段 2〜3)
FX_CONST uint32_t WL_REASON_REST_ROUNDING = 7;      // 静止の丸め(1 単位未満の速さを 0 にした分。段 4)
FX_CONST uint32_t WL_REASON_COUNT = 8;

// --- 向きと欄 ---
FX_CONST uint32_t WL_DIRECTION_OUT = 0;  // セルから減った(世界の外へ出た)
FX_CONST uint32_t WL_DIRECTION_IN = 1;   // セルに増えた(世界の外から入った)
FX_CONST uint32_t WL_SLOT_COUNT = WL_REASON_COUNT * 2;

// --- 量(列)---
FX_CONST uint32_t WL_COLUMN_ENERGY = 0;
FX_CONST uint32_t WL_COLUMN_SPECIES_BASE = 1;  // 1 + 物質 ID

// --- 丸ごとの量の置き場(レベル L の単位 = L − 21 の段の 2 × 2^-64 単位。8^21 = 2^63)---
FX_CONST int32_t WL_WHOLE_LOW_LEVELS = 21;   // 下位 64bit(2 × units)を置く段の粗さ
FX_CONST int32_t WL_WHOLE_HIGH_LEVELS = 22;  // 最上位の bit(× 2^61)を置く段の粗さ

FX_FN uint32_t WlSlot(uint32_t reason, uint32_t direction) {
    return (reason * 2) + direction;
}

FX_FN bool WlSlotIsIn(uint32_t slot) {
    return (slot & 1u) == WL_DIRECTION_IN;
}

// 帳簿の語の数(列 columns)
FX_FN uint32_t WlLedgerWords(uint32_t columns) {
    return WL_SLOT_COUNT * MR_LEDGER_LEVELS * columns;
}

// 帳簿の番地(欄・レベル・列が帳簿の外なら MR_NO_BLOCK)。欄 0 は MrLedgerAddress と同じ
FX_FN uint32_t WlAddress(uint32_t slot, int32_t level, uint32_t column, uint32_t columns) {
    const uint32_t row = MrLedgerAddress(level, column, columns);
    if (row == MR_NO_BLOCK || slot >= WL_SLOT_COUNT)
        return MR_NO_BLOCK;

    return (slot * MR_LEDGER_LEVELS * columns) + row;
}

// 段の桁あふれ(その段の 1 単位)を 1 段粗い段に繰り上げる値(2^61)
FX_FN uint64_t WlCarryBits() {
    return FX_U64(1u << 29, 0u);
}

// 丸ごとの量 units(レベル L の単位)の置き方: L − 21 の段に low、L − 22 の段に high(どちらもその段の単位 × 2^-64)
struct WlWholeSplit {
    uint64_t low;   // 2 × units の下位 64bit
    uint64_t high;  // units の最上位の bit × 2^61
};

FX_FN WlWholeSplit WlSplitWholeUnits(uint64_t units) {
    WlWholeSplit split;
    split.low = units << 1;
    split.high = (units >> 63) << 61;

    return split;
}

// セルのエネルギー(符号つき)を向き direction で帳簿に載せる時の向きと大きさ(負のエネルギーを出す = 入れる)
struct WlSignedEntry {
    uint32_t direction;
    uint64_t magnitude;
};

FX_FN WlSignedEntry WlEnergyEntry(int64_t energy, uint32_t direction) {
    WlSignedEntry entry;
    entry.direction = direction;
    entry.magnitude = (uint64_t)energy;
    if (energy < 0) {
        entry.direction = 1u - direction;
        entry.magnitude = ~((uint64_t)energy) + 1u;
    }

    return entry;
}

MR_NAMESPACE_END

#endif  // BICAMERAL_WORLD_LEDGER_HLSLI
