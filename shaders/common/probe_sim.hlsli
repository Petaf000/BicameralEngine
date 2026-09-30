// probe_sim.hlsli — フレームループと刻みのループ(T-0004・T-0012)を確かめるための仮の小さな世界。HLSL と C++ で共通(04 §3 と同じ作り)。
//
// 本物の 1 刻み(06 §2 の 9 段)の中身は T-0005 以降。ここにあるのは「1 刻みを単位の列に分けて、フレームの予算ぶんずつ投げ、
// 刻みごとのハッシュと、コマンド → イベントの流れが CPU に戻る」形を通すための最小の中身。
//
// 世界: PROBE_GRID_SIZE × PROBE_GRID_SIZE のセルに uint32 の量(熱のようなもの)。世代同期(ADR-0003)のため 2 世代を持つ。
//   刻み t の始めの状態 S(t) は世代 (t & 1) にある。
// 1 刻み t = 単位の列(06 §4・ADR-0011。単位の間は UAV バリア。フレームの切れ目はどの単位の間にも来てよい):
//   [0] コマンドの適用: targetTick == t の「つつく」コマンドのセルを、世代 (t & 1) の中で max(値, PROBE_POKE_AMOUNT) にする
//       (max なので同じセルへの複数のコマンドの順番に依存しない。04 R2)
//   [1] 拡散: 世代 ((t + 1) & 1) のセル = (自分 × 4 + 上下左右) / 8(格子の外は 0。端から少しずつ抜ける)→ S(t + 1)
//   [2 .. 2 + k) 重さの試験(--sim-load を k 個に分けたもの。世界の結果に入らない。k = 0 なら無し)
//   [最後] ハッシュ: S(t + 1) の要約(ProbeStateHash)を、ハッシュの表の (t + 1) % PROBE_HASH_CAPACITY に書く(06 §2 段 9)
// CPU のリファレンス(sim/probe_sim.cpp の ProbeReference)と GPU(shaders/sim/probe_tick.hlsl)は同じ関数を使う。
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

// 64bit の定数(HLSL のリテラル接尾辞に頼らない。fixed.hlsli の FX_U64 と同じ)
#define PROBE_U64(high32, low32) ((((uint64_t)(high32)) << 32) | ((uint64_t)(low32)))

PROBE_NAMESPACE_BEGIN

// --- 世界の大きさ ---
PROBE_CONST uint32_t PROBE_GRID_SIZE = 128;
PROBE_CONST uint32_t PROBE_CELL_COUNT = PROBE_GRID_SIZE * PROBE_GRID_SIZE;
PROBE_CONST uint32_t PROBE_GROUP_SIZE = 8;          // 拡散は 8×8 のスレッドグループ
PROBE_CONST uint32_t PROBE_LINEAR_GROUP_SIZE = 64;  // コマンド・ハッシュ・抽出は 1 次元の 64
PROBE_CONST uint32_t PROBE_POKE_AMOUNT = 1u << 24;

// --- 描画用の抽出の組の数(06 §4)---
// 抽出は 1 フレームに 1 回まで、投げた単位の後ろで刻みの境界の状態を写す。描画は終わっている最新を読む。
// 抽出 n は組 n % 3 に書き、終わっている抽出が n − 2 以上のときだけ投げる(frame/frame_loop.cpp)→ 描画が読む組と重ならない
PROBE_CONST uint32_t PROBE_EXTRACTION_COUNT = 3;

// --- 1 刻みの単位(06 §4・ADR-0011)---
PROBE_CONST uint32_t PROBE_UNIT_APPLY = 0;
PROBE_CONST uint32_t PROBE_UNIT_DIFFUSE = 1;
PROBE_CONST uint32_t PROBE_UNIT_BUSY_FIRST = 2;       // 重さの試験の単位はここから k 個。その次がハッシュ
PROBE_CONST uint32_t PROBE_FIXED_UNITS_PER_TICK = 3;  // 適用・拡散・ハッシュ

// --- ルート定数(b0。単位を記録するときに埋め込む)---
// [0] 刻みの下位 [1] 刻みの上位 [2] 引数(抽出: 書き先の組)
PROBE_CONST uint32_t PROBE_ROOT_CONSTANT_COUNT = 3;

// --- コマンド(CPU → GPU。06 §3 の 64 バイトの形)---
// 語: [0] targetTick の下位 [1] targetTick の上位 [2] sequence [3] type(下位 16bit)| size(上位 16bit)[4..15] payload 48 バイト
PROBE_CONST uint32_t PROBE_MAX_COMMANDS = 256;  // 1 フレームに載せられるコマンドの数
PROBE_CONST uint32_t PROBE_COMMAND_WORDS = 16;
PROBE_CONST uint32_t PROBE_COMMAND_BYTES = PROBE_COMMAND_WORDS * 4;
PROBE_CONST uint32_t PROBE_COMMAND_TYPE_POKE = 1;  // payload: [0] x [1] y

// --- フレームの入力(アップロードのバッファ。CPU がフレームの枠ごとに書く)のレイアウト ---
// [0]    見出し: コマンドの数、重さの試験の 1 個あたりの繰り返し回数
// [256]  コマンド × PROBE_MAX_COMMANDS(このフレームに適用の単位がある刻みの分。シェーダーが targetTick で選ぶ)
PROBE_CONST uint32_t PROBE_INPUT_HEADER_OFFSET = 0;
PROBE_CONST uint32_t PROBE_INPUT_COMMANDS_OFFSET = 256;
PROBE_CONST uint32_t PROBE_INPUT_BYTES = PROBE_INPUT_COMMANDS_OFFSET + PROBE_MAX_COMMANDS * PROBE_COMMAND_BYTES;
PROBE_CONST uint32_t PROBE_HEADER_COMMAND_COUNT = 0;  // 見出しの語の位置(× 4 バイト)
PROBE_CONST uint32_t PROBE_HEADER_BUSY_ITERATIONS = 1;

// 重さの試験(--sim-load)の繰り返しの上限(1 刻みの合計。--sim-split で分けたときは 1 個あたりがこれを分けた数で割ったもの)。
// これ以上の値は CPU が送らないので、シェーダーの「使わない分岐」は決して通らない
PROBE_CONST uint32_t PROBE_BUSY_ITERATIONS_LIMIT = 1u << 24;
PROBE_CONST uint32_t PROBE_MAX_BUSY_PIECES = 64;  // 1 刻みの重さを分ける数の上限(--sim-split)

// --- イベント(GPU → CPU。readback_ring の追記バッファ)---
// 見出し 16 バイト([0] 書こうとした数)+ 16 バイトのレコード × PROBE_EVENT_CAPACITY
// レコード: [0] 刻みの下位 [1] 刻みの上位 [2] 種類 [3] 引数(つつき: x | y << 16)
PROBE_CONST uint32_t PROBE_EVENT_CAPACITY = 1024;
PROBE_CONST uint32_t PROBE_EVENT_HEADER_BYTES = 16;
PROBE_CONST uint32_t PROBE_EVENT_WORDS = 4;
PROBE_CONST uint32_t PROBE_EVENT_BYTES = PROBE_EVENT_HEADER_BYTES + PROBE_EVENT_CAPACITY * PROBE_EVENT_WORDS * 4;
PROBE_CONST uint32_t PROBE_EVENT_POKE_APPLIED = 1;

// --- 刻みごとの状態のハッシュ(GPU → CPU。06 §2 段 9)---
// 表: PROBE_HASH_CAPACITY 個 × 16 バイト([0] 刻みの下位 [1] 刻みの上位 [2] ハッシュの下位 [3] ハッシュの上位)。
// S(t) のハッシュは (t % PROBE_HASH_CAPACITY) 番目。フレームの終わりに表を丸ごと読み戻し、CPU はそのフレームで終えた刻みの分だけ読む。
// 1 フレームに積める単位は 256 まで(3 単位/刻みでも 86 刻み)なので、同じフレームの中で番号が重なることはない
PROBE_CONST uint32_t PROBE_HASH_CAPACITY = 256;  // 2 の冪(番号を下位ビットで取る)
PROBE_CONST uint32_t PROBE_HASH_ENTRY_BYTES = 16;
PROBE_CONST uint32_t PROBE_HASH_BYTES = PROBE_HASH_CAPACITY * PROBE_HASH_ENTRY_BYTES;

// --- 規則(CPU と GPU で同じ)---

PROBE_FN uint32_t ProbeCellIndex(uint32_t x, uint32_t y) {
    return y * PROBE_GRID_SIZE + x;
}

// 拡散の 1 セル: (自分 × 4 + 上下左右) / 8(格子の外の隣は 0 を渡す)。
// 値は PROBE_POKE_AMOUNT 以下(平均なので増えない)なので、8 倍しても 32bit に収まる
PROBE_FN uint32_t ProbeDiffuseValue(uint32_t self, uint32_t left, uint32_t right, uint32_t up, uint32_t down) {
    return (self * 4 + left + right + up + down) >> 3;
}

// 64bit を混ぜる(splitmix64 の仕上げ。入力の 1 ビットの違いが全体に広がる)
PROBE_FN uint64_t ProbeMix64(uint64_t value) {
    value ^= value >> 30;
    value *= PROBE_U64(0xBF58476Du, 0x1CE4E5B9u);
    value ^= value >> 27;
    value *= PROBE_U64(0x94D049BBu, 0x133111EBu);
    value ^= value >> 31;
    return value;
}

// 1 セルの寄与。状態のハッシュ = 全セルの寄与の和(mod 2^64)。和は足す順番に依存しないので、
// GPU が並列に(wave の和 + 64bit の atomic)足しても CPU が順に足しても同じ値になる(04 R2)
PROBE_FN uint64_t ProbeCellHash(uint32_t cellIndex, uint32_t value) {
    return ProbeMix64(PROBE_U64(cellIndex, value));
}

PROBE_NAMESPACE_END

#endif  // BICAMERAL_PROBE_SIM_HLSLI
