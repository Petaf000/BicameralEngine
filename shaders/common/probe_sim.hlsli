// probe_sim.hlsli — フレームループ(T-0004)を確かめるための仮の小さな世界。HLSL と C++ で共通(04 §3 と同じ作り)。
//
// 本物の 1 刻み(06 §2 の 9 段)は T-0012 から。ここにあるのは「CPU のコマンドが GPU の刻みで適用され、
// 結果がイベントで CPU に戻り、描画が抽出を読む」流れを通すための最小の中身。
//
// 世界: PROBE_GRID_SIZE × PROBE_GRID_SIZE のセルに uint32 の量(熱のようなもの)。世代同期(ADR-0003)のため 2 世代を持つ。
//   刻み t の始めの状態は世代 (t & 1) にある。
// 1 刻み:
//   (1) コマンドの適用: targetTick == t の「つつく」コマンドのセルを、世代 (t & 1) の中で max(値, PROBE_POKE_AMOUNT) にする
//       (max なので同じセルへの複数のコマンドの順番に依存しない。04 R2)
//   (2) 拡散: 世代 ((t + 1) & 1) のセル = (自分 × 4 + 上下左右) / 8(格子の外は 0。端から少しずつ抜ける)
// CPU のリファレンス(ProbeApplyCommandCpu・ProbeDiffuseCell)と GPU(shaders/sim/probe_tick.hlsl)は同じ関数を使う。
#ifndef BICAMERAL_PROBE_SIM_HLSLI
#define BICAMERAL_PROBE_SIM_HLSLI

#ifdef __cplusplus
#include <cstdint>
#define PROBE_FN constexpr inline
#define PROBE_CONST inline constexpr
#define PROBE_NAMESPACE_BEGIN namespace bicameral::sim {
#define PROBE_NAMESPACE_END }
#else
#define PROBE_FN
#define PROBE_CONST static const
#define PROBE_NAMESPACE_BEGIN
#define PROBE_NAMESPACE_END
#endif

PROBE_NAMESPACE_BEGIN

// --- 世界の大きさ ---
PROBE_CONST uint32_t PROBE_GRID_SIZE = 128;
PROBE_CONST uint32_t PROBE_CELL_COUNT = PROBE_GRID_SIZE * PROBE_GRID_SIZE;
PROBE_CONST uint32_t PROBE_GROUP_SIZE = 8;  // 拡散は 8×8 のスレッドグループ
PROBE_CONST uint32_t PROBE_POKE_AMOUNT = 1u << 24;

// --- 描画用の抽出の組の数(06 §4)---
// シミュのバッチは最大 2 つ重ねて投げる(frame/frame_loop.cpp)。描画は終わっている最新を読むので、読んでいる組・
// 書いている 2 組で 3 組あれば、描画はシミュを待たない(バッチ b は組 b % 3 に書く)
PROBE_CONST uint32_t PROBE_EXTRACTION_COUNT = 3;

// --- 1 バッチ(1 回の投入で進める刻みのまとまり)---
PROBE_CONST uint32_t PROBE_MAX_TICKS_PER_BATCH = 8;  // 記録済みのリストが持つ刻みの枠の数
PROBE_CONST uint32_t PROBE_MAX_COMMANDS = 256;       // 1 バッチに載せられるコマンドの数
PROBE_CONST uint32_t PROBE_COMMAND_GROUP_SIZE = 64;

// --- コマンド(CPU → GPU。06 §3 の 64 バイトの形)---
// 語: [0] targetTick の下位 [1] targetTick の上位 [2] sequence [3] type(下位 16bit)| size(上位 16bit)[4..15] payload 48 バイト
PROBE_CONST uint32_t PROBE_COMMAND_WORDS = 16;
PROBE_CONST uint32_t PROBE_COMMAND_BYTES = PROBE_COMMAND_WORDS * 4;
PROBE_CONST uint32_t PROBE_COMMAND_TYPE_POKE = 1;  // payload: [0] x [1] y

// --- バッチの入力(アップロードのバッファ。CPU がバッチごとに書く)のレイアウト ---
// [0]    見出し: firstTick の下位・上位、刻みの数、コマンドの数、抽出の書き先(0〜2)、重さの試験の繰り返し回数
// [256]  コマンド × PROBE_MAX_COMMANDS
// 刻みの数はリストの選び方で決まる(刻みの数ごとに記録したリスト。sim/probe_sim.cpp)
PROBE_CONST uint32_t PROBE_BATCH_HEADER_OFFSET = 0;
PROBE_CONST uint32_t PROBE_BATCH_COMMANDS_OFFSET = 256;
PROBE_CONST uint32_t PROBE_BATCH_BYTES = PROBE_BATCH_COMMANDS_OFFSET + PROBE_MAX_COMMANDS * PROBE_COMMAND_BYTES;

PROBE_CONST uint32_t PROBE_HEADER_FIRST_TICK_LOW = 0;  // 見出しの語の位置(× 4 バイト)
PROBE_CONST uint32_t PROBE_HEADER_FIRST_TICK_HIGH = 1;
PROBE_CONST uint32_t PROBE_HEADER_TICK_COUNT = 2;
PROBE_CONST uint32_t PROBE_HEADER_COMMAND_COUNT = 3;
PROBE_CONST uint32_t PROBE_HEADER_EXTRACTION_TARGET = 4;
PROBE_CONST uint32_t PROBE_HEADER_BUSY_ITERATIONS = 5;
// 重さの試験(--sim-load)の繰り返しの上限(1 刻みの合計。--sim-split で分けたときは 1 個あたりがこれを分けた数で割ったもの)。これ以上の値は CPU が送らないので、シェーダーの「使わない分岐」は決して通らない
PROBE_CONST uint32_t PROBE_BUSY_ITERATIONS_LIMIT = 1u << 24;
PROBE_CONST uint32_t PROBE_MAX_BUSY_PIECES = 64;  // 1 刻みの重さを分けて投げる数の上限(--sim-split)

// --- イベント(GPU → CPU。readback_ring の追記バッファ)---
// 見出し 16 バイト([0] 書こうとした数)+ 16 バイトのレコード × PROBE_EVENT_CAPACITY
// レコード: [0] 刻みの下位 [1] 刻みの上位 [2] 種類 [3] 引数(つつき: x | y << 16)
PROBE_CONST uint32_t PROBE_EVENT_CAPACITY = 1024;
PROBE_CONST uint32_t PROBE_EVENT_HEADER_BYTES = 16;
PROBE_CONST uint32_t PROBE_EVENT_WORDS = 4;
PROBE_CONST uint32_t PROBE_EVENT_BYTES = PROBE_EVENT_HEADER_BYTES + PROBE_EVENT_CAPACITY * PROBE_EVENT_WORDS * 4;
PROBE_CONST uint32_t PROBE_EVENT_POKE_APPLIED = 1;

// --- 規則(CPU と GPU で同じ)---

PROBE_FN uint32_t ProbeCellIndex(uint32_t x, uint32_t y) {
    return y * PROBE_GRID_SIZE + x;
}

// 拡散の 1 セル: (自分 × 4 + 上下左右) / 8(格子の外の隣は 0 を渡す)。
// 値は PROBE_POKE_AMOUNT 以下(平均なので増えない)なので、8 倍しても 32bit に収まる
PROBE_FN uint32_t ProbeDiffuseValue(uint32_t self, uint32_t left, uint32_t right, uint32_t up, uint32_t down) {
    return (self * 4 + left + right + up + down) >> 3;
}

PROBE_NAMESPACE_END

#endif  // BICAMERAL_PROBE_SIM_HLSLI
